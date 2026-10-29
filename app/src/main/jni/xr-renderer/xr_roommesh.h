// Parser for .room baked room meshes, written by tools/bake_room.py. No GL.
//
// Layout, little endian:
//   char[4]  magic 'MXR3'
//   uint32   vertex count
//   uint32   index count
//   uint32   part count
//   uint32   atlas count
//   per part: uint32 first index, uint32 index count, int32 atlas, -1 for a
//            part painted from its vertex colours
//   uint32   channel count
//   float[11] per vertex: position xyz, normal xyz, uv, rgb
//   uint32   indices
//
// Parts cover the index buffer contiguously and in order. A part uses either
// its atlas or its vertex colours.
//
// Files with animation channels are refused: animated parts are baked in node
// space and would be misplaced without playback.

#ifndef XR_ROOMMESH_H
#define XR_ROOMMESH_H

#include <stddef.h>
#include <stdint.h>

// Header, one part record, and the channel count field
#define ROOM_MESH_HEADER_BYTES 20
#define ROOM_MESH_PART_BYTES 12
#define ROOM_MESH_CHANNEL_COUNT_BYTES 4
// Position, normal, uv, colour
#define ROOM_MESH_VERTEX_FLOATS 11
// Small enough that the 32-bit size arithmetic cannot overflow
#define ROOM_MESH_VERTS_MAX (1 << 20)
#define ROOM_MESH_PARTS_MAX 16
#define ROOM_MESH_ATLASES_MAX 4

typedef struct {
    uint32_t firstIndex;
    uint32_t indexCount;
    // Atlas slot, or -1 for vertex colours
    int32_t atlas;
} RoomMeshPart;

typedef struct {
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t partCount;
    uint32_t atlasCount;
    RoomMeshPart parts[ROOM_MESH_PARTS_MAX];
    // Byte offsets of the vertices and indices in the file
    size_t vertexOffset;
    size_t indexOffset;
} RoomMeshInfo;

// Validates the header, part table, sizes and every index. Returns 1 for a
// drawable mesh, 0 otherwise (layout rules above, limits, no channels).
int roomMeshParse(const unsigned char* data, size_t size, RoomMeshInfo* out);

// Number of parts drawn with vertex colours
int roomMeshPaintedParts(const RoomMeshInfo* info);

#endif
