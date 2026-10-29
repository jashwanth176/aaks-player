#!/usr/bin/env python3
#
# Bakes a glTF binary (.glb) model into the flat .room mesh file the renderer
# loads from assets (the renderer has no glTF loader).
#
#   python3 tools/bake_room.py --mesh Home-Theater-HighRes_Baked \
#       --texture atlas.png model.glb app/src/main/assets/rooms/home_theater.room
#
# --mesh names a mesh or its node; repeat it to append several meshes. Each
# primitive becomes a part. --texture writes each atlas in first-use order, as
# PNG, or as mipmapped ASTC (via tools/atlas_astc.py) for a .atlas path;
# --atlas-size N limits the longer side.
#
# A part is painted from one atlas or from vertex colours, never both. Without
# an atlas, vertex colours or the material base colour are used. A primitive
# with both is split: white vertices take the atlas.
#
# Animations are not baked (a warning is printed); channel count is always 0.
#
# Output layout, little endian:
#   char[4]  magic 'MXR3'
#   uint32   vertex count
#   uint32   index count
#   uint32   part count
#   uint32   atlas count
#   per part: uint32 first index, uint32 index count, int32 atlas, -1 for one
#            painted from its vertex colours
#   uint32   channel count, always 0 here
#   float[11] per vertex: position xyz, normal xyz, uv, rgb
#   uint32   indices
#
# Triangles only. Node transforms are applied, except non-uniform scale (flatten
# such models in a 3d tool first).

import argparse
import json
import math
import os
import struct
import sys
import tempfile

COMPONENTS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
# Component type -> (struct code, size, normalisation divisor).
COMPONENT_TYPES = {
    5121: ("B", 1, 255.0),
    5123: ("H", 2, 65535.0),
    5125: ("I", 4, 0.0),
    5126: ("f", 4, 0.0),
}
# Renderer's ROOM_PARTS_MAX and ROOM_ATLASES_MAX.
MAX_PARTS = 16
MAX_ATLASES = 4


def accessor(gltf, blob, index):
    a = gltf["accessors"][index]
    if "bufferView" not in a or "sparse" in a:
        sys.exit(f"accessor {index} is sparse or has no data, flatten the model first")
    view = gltf["bufferViews"][a["bufferView"]]
    start = view.get("byteOffset", 0) + a.get("byteOffset", 0)
    comps = COMPONENTS[a["type"]]
    if a["componentType"] not in COMPONENT_TYPES:
        sys.exit(f"accessor {index} is component type {a['componentType']}")
    fmt, size, _ = COMPONENT_TYPES[a["componentType"]]
    stride = view.get("byteStride", comps * size)
    out = []
    for i in range(a["count"]):
        base = start + i * stride
        out.append(struct.unpack_from("<" + fmt * comps, blob, base))
    return out


# Accessor with normalised integers read as 0..1 floats.
def accessor_unit(gltf, blob, index):
    a = gltf["accessors"][index]
    _, _, full = COMPONENT_TYPES[a["componentType"]]
    values = accessor(gltf, blob, index)
    if full == 0.0:
        return values
    return [tuple(v / full for v in value) for value in values]


# With no name the file must hold exactly one mesh.
def pick_mesh(gltf, name):
    meshes = gltf.get("meshes", [])
    if name is None:
        if len(meshes) != 1:
            sys.exit(f"{len(meshes)} meshes in the file, name one with --mesh")
        return 0

    found = [i for i, m in enumerate(meshes) if m.get("name") == name]
    for node in gltf.get("nodes", []):
        if node.get("name") == name and "mesh" in node and node["mesh"] not in found:
            found.append(node["mesh"])
    if len(found) != 1:
        sys.exit(f"{name} names {len(found)} meshes, expected one")
    return found[0]


# The single node carrying a mesh; instanced meshes are refused.
def mesh_node(gltf, mesh):
    carriers = [i for i, n in enumerate(gltf.get("nodes", [])) if n.get("mesh") == mesh]
    if len(carriers) != 1:
        sys.exit(f"mesh {mesh} hangs off {len(carriers)} nodes, expected one")
    check_parents(gltf, carriers[0])
    return carriers[0]


# Only the mesh node's own transform is baked, so moving parents are refused.
def check_parents(gltf, node):
    nodes = gltf.get("nodes", [])
    parent = {c: i for i, n in enumerate(nodes) for c in n.get("children", [])}
    at = parent.get(node)
    while at is not None:
        if moves(nodes[at]):
            sys.exit(f"node {nodes[node].get('name', node)} sits under "
                     f"{nodes[at].get('name', at)}, which moves it; apply the "
                     "transforms in a 3d tool first")
        at = parent.get(at)


def moves(node):
    if "matrix" in node:
        # Identity diagonal is at 0, 5, 10, 15.
        return any(abs(v - (1.0 if i % 5 == 0 else 0.0)) > 1e-9
                   for i, v in enumerate(node["matrix"]))
    return (any(abs(v) > 1e-9 for v in node.get("translation", [0.0, 0.0, 0.0]))
            or any(abs(v - w) > 1e-9 for v, w in
                   zip(node.get("rotation", [0.0, 0.0, 0.0, 1.0]), (0.0, 0.0, 0.0, 1.0)))
            or any(abs(v - 1.0) > 1e-9 for v in node.get("scale", [1.0, 1.0, 1.0])))


# Node transform as (3x3, translation).
def mesh_transform(node):
    if "matrix" in node:
        m = node["matrix"]
        # Column major.
        linear = [[m[0], m[4], m[8]], [m[1], m[5], m[9]], [m[2], m[6], m[10]]]
        offset = (m[12], m[13], m[14])
        lengths = [math.sqrt(sum(linear[r][c] ** 2 for r in range(3))) for c in range(3)]
        if max(lengths) - min(lengths) > 1e-5:
            sys.exit("node matrix has a non uniform scale, flatten the model first")
        return linear, offset

    scale = node.get("scale", [1.0, 1.0, 1.0])
    if max(scale) - min(scale) > 1e-6:
        sys.exit("node has a non uniform scale, flatten the model first")
    s = scale[0]
    x, y, z, w = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
    linear = [
        [(1.0 - 2.0 * (y * y + z * z)) * s, (2.0 * (x * y - z * w)) * s,
         (2.0 * (x * z + y * w)) * s],
        [(2.0 * (x * y + z * w)) * s, (1.0 - 2.0 * (x * x + z * z)) * s,
         (2.0 * (y * z - x * w)) * s],
        [(2.0 * (x * z - y * w)) * s, (2.0 * (y * z + x * w)) * s,
         (1.0 - 2.0 * (x * x + y * y)) * s],
    ]
    return linear, tuple(node.get("translation", [0.0, 0.0, 0.0]))


def apply_point(linear, offset, p):
    return tuple(linear[r][0] * p[0] + linear[r][1] * p[1] + linear[r][2] * p[2] + offset[r]
                 for r in range(3))


# Scale is uniform, so renormalising is enough.
def apply_normal(linear, n):
    out = [linear[r][0] * n[0] + linear[r][1] * n[1] + linear[r][2] * n[2] for r in range(3)]
    length = math.sqrt(out[0] ** 2 + out[1] ** 2 + out[2] ** 2)
    if length < 1e-8:
        return (0.0, 1.0, 0.0)
    return (out[0] / length, out[1] / length, out[2] / length)


def is_identity(linear, offset):
    for r in range(3):
        if abs(offset[r]) > 1e-9:
            return False
        for c in range(3):
            if abs(linear[r][c] - (1.0 if r == c else 0.0)) > 1e-9:
                return False
    return True


def material_of(gltf, prim):
    if "material" not in prim:
        return {}
    return gltf.get("materials", [])[prim["material"]]


# Embedded image index of the base colour texture, or None.
def primitive_image(gltf, prim):
    texture = material_of(gltf, prim).get("pbrMetallicRoughness", {}).get("baseColorTexture")
    if texture is None:
        return None
    return gltf["textures"][texture["index"]]["source"]


# Per-vertex colours, or the material base colour. glTF colours are linear but
# the renderer treats colours like the sRGB atlases, so convert to sRGB.
def primitive_colors(gltf, blob, prim, count):
    if "COLOR_0" in prim["attributes"]:
        colors = accessor_unit(gltf, blob, prim["attributes"]["COLOR_0"])
        return [tuple(srgb(min(max(c, 0.0), 1.0)) for c in color[:3]) for color in colors]
    factor = material_of(gltf, prim).get("pbrMetallicRoughness", {}).get("baseColorFactor")
    if factor is None:
        factor = [1.0, 1.0, 1.0, 1.0]
    flat = tuple(srgb(min(max(c, 0.0), 1.0)) for c in factor[:3])
    return [flat] * count


def srgb(linear):
    if linear <= 0.0031308:
        return linear * 12.92
    return 1.055 * linear ** (1.0 / 2.4) - 0.055


# Raw bytes of an embedded PNG.
def image_bytes(gltf, blob, index):
    image = gltf.get("images", [])[index]
    if image.get("mimeType") != "image/png":
        sys.exit(f"embedded image is {image.get('mimeType')}, expected image/png")
    if "bufferView" not in image:
        sys.exit("embedded image is a file reference, not bytes")
    view = gltf["bufferViews"][image["bufferView"]]
    start = view.get("byteOffset", 0)
    return blob[start:start + view["byteLength"]]


# PNG dimensions; must be 8-bit RGB.
def png_size(png):
    if png[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit("embedded image is not a PNG")
    width, height, depth, colour = struct.unpack_from(">IIBB", png, 16)
    if depth != 8 or colour != 2:
        sys.exit(f"atlas is depth {depth} colour type {colour}, expected 8 bit RGB")
    return width, height


# image is the atlas, or None for a part painted from colours.
class Part:
    def __init__(self, pos, normal, uv, colors, indices, image):
        self.pos = pos
        self.normal = normal
        self.uv = uv
        self.colors = colors
        self.indices = indices
        self.image = image


def read_parts(gltf, blob, mesh):
    parts = []
    for prim in gltf["meshes"][mesh]["primitives"]:
        parts.extend(read_primitive(gltf, blob, mesh, prim))
    return parts


def read_primitive(gltf, blob, mesh, prim):
    if prim.get("mode", 4) != 4:
        sys.exit("primitive is not triangles")

    for need in ("POSITION", "NORMAL"):
        if need not in prim["attributes"]:
            sys.exit(f"primitive has no {need}")
    pos = accessor(gltf, blob, prim["attributes"]["POSITION"])
    normal = accessor(gltf, blob, prim["attributes"]["NORMAL"])
    if "indices" in prim:
        indices = [i[0] for i in accessor(gltf, blob, prim["indices"])]
    else:
        indices = list(range(len(pos)))
    if not indices or len(indices) % 3:
        sys.exit(f"primitive has {len(indices)} indices, not whole triangles")
    if max(indices) >= len(pos):
        sys.exit("index out of range")

    linear, offset = mesh_transform(gltf["nodes"][mesh_node(gltf, mesh)])
    if not is_identity(linear, offset):
        pos = [apply_point(linear, offset, p) for p in pos]
        normal = [apply_normal(linear, n) for n in normal]

    image = primitive_image(gltf, prim)
    # No atlas: texture coordinates are dropped.
    if image is None:
        return [Part(pos, normal, None, primitive_colors(gltf, blob, prim, len(pos)),
                     indices, None)]
    if "TEXCOORD_0" not in prim["attributes"]:
        sys.exit("primitive has an atlas but no texture coordinates")
    uv = accessor(gltf, blob, prim["attributes"]["TEXCOORD_0"])
    if "COLOR_0" in prim["attributes"]:
        return split_painted(pos, normal, uv, indices, image,
                             primitive_colors(gltf, blob, prim, len(pos)),
                             accessor_unit(gltf, blob, prim["attributes"]["COLOR_0"]))
    return [Part(pos, normal, uv, None, indices, image)]


# Splits a primitive with an atlas and vertex colours into an atlas part (white
# vertices) and a coloured part. Mixed triangles are refused.
def split_painted(pos, normal, uv, indices, image, colors, unit):
    white = [all(c > 0.999 for c in value[:3]) for value in unit]
    textured = []
    painted = []
    for t in range(0, len(indices), 3):
        tri = indices[t:t + 3]
        if all(white[i] for i in tri):
            textured.extend(tri)
        elif not any(white[i] for i in tri):
            painted.extend(tri)
        else:
            sys.exit("a triangle is part atlas and part vertex colour, split the "
                     "mesh on its materials in a 3d tool first")
    if not painted:
        return [Part(pos, normal, uv, None, indices, image)]
    if not textured:
        return [Part(pos, normal, None, colors, indices, None)]
    return [take(pos, normal, uv, None, textured, image),
            take(pos, normal, None, colors, painted, None)]


# A part from a subset of triangles, keeping only the vertices they use.
def take(pos, normal, uv, colors, indices, image):
    order = {}
    for i in indices:
        if i not in order:
            order[i] = len(order)
    keep = list(order)
    return Part([pos[i] for i in keep], [normal[i] for i in keep],
                [uv[i] for i in keep] if uv else None,
                [colors[i] for i in keep] if colors else None,
                [order[i] for i in indices], image)


def warn_animations(gltf, meshes):
    baked = {mesh_node(gltf, mesh) for mesh in meshes}
    for anim in gltf.get("animations", []):
        for channel in anim.get("channels", []):
            node = channel.get("target", {}).get("node")
            if node in baked:
                name = gltf["nodes"][node].get("name", node)
                print(f"warning: {anim.get('name', 'an animation')} drives {name}, "
                      "which is baked where it rests", file=sys.stderr)


# Atlases in first-use order, which is the renderer's slot order.
def atlas_order(parts):
    order = []
    for part in parts:
        if part.image is not None and part.image not in order:
            order.append(part.image)
    return order


def main():
    parser = argparse.ArgumentParser(description="bake a glb room into a .room mesh file")
    parser.add_argument("--mesh", action="append",
                        help="the mesh, or the node carrying it, repeated for a room of several")
    parser.add_argument("--texture", action="append",
                        help="write one atlas here as a PNG, or as ASTC for a .atlas, "
                             "repeated in atlas order")
    parser.add_argument("--atlas-size", type=int,
                        help="resize each .atlas so the longer side is this many pixels")
    parser.add_argument("model")
    parser.add_argument("out")
    args = parser.parse_args()

    with open(args.model, "rb") as f:
        data = f.read()
    if len(data) < 20:
        sys.exit("not a glb v2 file")
    magic, version, _ = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67 or version != 2:
        sys.exit("not a glb v2 file")

    json_len, _ = struct.unpack_from("<II", data, 12)
    gltf = json.loads(data[20:20 + json_len])
    bin_off = 20 + json_len
    if bin_off + 8 > len(data):
        sys.exit("the glb has no binary chunk")
    bin_len, _ = struct.unpack_from("<II", data, bin_off)
    blob = data[bin_off + 8:bin_off + 8 + bin_len]

    names = args.mesh if args.mesh else [None]
    meshes = [pick_mesh(gltf, name) for name in names]
    warn_animations(gltf, meshes)
    parts = []
    for mesh in meshes:
        parts.extend(read_parts(gltf, blob, mesh))
    if len(parts) > MAX_PARTS:
        sys.exit(f"{len(parts)} parts here, the renderer takes {MAX_PARTS}")
    atlases = atlas_order(parts)
    if len(atlases) > MAX_ATLASES:
        sys.exit(f"{len(atlases)} atlases here, the renderer takes {MAX_ATLASES}")
    textures = args.texture if args.texture else []
    if textures and len(textures) != len(atlases):
        sys.exit(f"{len(atlases)} atlases here, {len(textures)} --texture given")

    total = sum(len(part.pos) for part in parts)
    index_total = sum(len(part.indices) for part in parts)

    with open(args.out, "wb") as out:
        out.write(b"MXR3")
        out.write(struct.pack("<IIII", total, index_total, len(parts), len(atlases)))
        first = 0
        for part in parts:
            atlas = atlases.index(part.image) if part.image is not None else -1
            out.write(struct.pack("<IIi", first, len(part.indices), atlas))
            first += len(part.indices)
        # Channel count.
        out.write(struct.pack("<I", 0))
        black = (0.0, 0.0, 0.0)
        for part in parts:
            for i, (p, n) in enumerate(zip(part.pos, part.normal)):
                uv = part.uv[i] if part.uv else (0.0, 0.0)
                rgb = part.colors[i] if part.colors else black
                out.write(struct.pack("<11f", *p, *n, *uv, *rgb))
        base = 0
        for part in parts:
            for i in part.indices:
                out.write(struct.pack("<I", i + base))
            base += len(part.pos)

    size = 20 + len(parts) * 12 + 4 + total * 44 + index_total * 4
    part_count = f"{len(parts)} parts" if len(parts) > 1 else "1 part"
    print(f"{args.out}: {total} vertices, {index_total // 3} triangles, {part_count}, "
          f"{len(atlases)} atlases, {size} bytes")
    for i, part in enumerate(parts):
        paint = (f"atlas {atlases.index(part.image)}" if part.image is not None
                 else "vertex colours")
        print(f"  part {i}: {len(part.pos)} vertices, {len(part.indices) // 3} triangles, {paint}")

    for i, path in enumerate(textures):
        png = image_bytes(gltf, blob, atlases[i])
        width, height = png_size(png)
        if path.endswith(".atlas"):
            write_atlas(png, path, args.atlas_size)
            continue
        with open(path, "wb") as out:
            out.write(png)
        print(f"{path}: {len(png)} bytes, {width}x{height}")


# Converts the embedded PNG to ASTC via tools/atlas_astc.py.
def write_atlas(png, path, size):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import atlas_astc
    with tempfile.TemporaryDirectory() as work:
        source = os.path.join(work, "atlas.png")
        with open(source, "wb") as out:
            out.write(png)
        width, height, count, total = atlas_astc.convert(source, path, size=size)
    print(f"{path}: {total} bytes, {width}x{height}, 6x6, {count} levels")


if __name__ == "__main__":
    main()
