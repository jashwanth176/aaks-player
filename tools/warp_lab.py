#!/usr/bin/env python3
#
# Offline bench for the stereo warp: reproduces the warp shader on a frame
# capture dumped by the renderer.
#
# Capturing on device, during playback:
#   adb shell setprop debug.moonlight.capture 1
#   adb shell ls /sdcard/Android/data/com.aaks.player.debug/files/
#   adb pull /sdcard/Android/data/com.aaks.player.debug/files/ captures/
#
# Then:
#   ./venv/bin/python tools/warp_lab.py captures/ --tag 1
#
# Files in a capture set, all headerless:
#   cap_<tag>_source.raw      W*H*4 uint8 RGBA, the unwarped frame
#   cap_<tag>_left.raw        W*H*4 uint8 RGBA, what the left eye was shown
#   cap_<tag>_right.raw       W*H*4 uint8 RGBA, likewise
#   cap_<tag>_depthtex.raw    MW*MH uint8, the depth texture the warp read
#   cap_<tag>_guidetex.raw    MW*MH*4 uint8, guide colour in rgb, depth in a
#   cap_<tag>_depthrect.raw   6 float32, MW and MH, then the x, y, w, h of
#                             the frame the map covers
#   cap_<tag>_upsampled.raw   (W/4)*(H/4) uint8, the upsampled depth
#   cap_<tag>_depthraw.raw    MW*MH float32, raw model output before
#                             normalization
#   cap_<tag>_modelinput.raw  MW*MH*3 float32, the RGB the model was given
#
# MW x MH comes from the depthrect file; without it, a square map over the
# whole frame is assumed, sized from the guide texture.
#
# Orientation: the GL readbacks (source, left, right, depthtex, upsampled) are
# bottom row first; depthraw and modelinput are top row first. PNGs are written
# top row first.
#
# Pipeline: joint bilateral upsample of depth guided by colour, then an
# occlusion-aware gather inverting the forward map, with the side edge fade and
# optional cubic B-spline depth read. Match --edge-fade and --depth-cubic to
# debug.moonlight.edgefade / depthcubic at capture time. Expected agreement
# with a device capture: upsample mean error ~0.001 (8-bit floor), warped eyes
# ~0.4/255.
#
# The large max difference is expected: at depth folds a tiny numeric
# difference picks a different crossing. Judge changes on the stretch
# statistics and images.

import argparse
import os
import sys

import numpy as np
from PIL import Image

# Depth map size and its rect (x, y, w, h in frame uv); overridden by the capture.
DEPTH_W = 512
DEPTH_H = 288
DEPTH_RECT = (0.0, 0.0, 1.0, 1.0)
SIGMA_S = 1.5
CHUNK_ROWS = 108


def load_raw(directory, tag, what, dtype, count):
    path = os.path.join(directory, "cap_%s_%s.raw" % (tag, what))
    if not os.path.exists(path):
        return None
    data = np.fromfile(path, dtype=dtype)
    if count is not None and data.size != count:
        raise SystemExit("%s has %d values, expected %d" % (path, data.size, count))
    return data


def save_png(path, array, flip=True):
    a = np.clip(array, 0.0, 255.0).astype(np.uint8)
    if flip:
        a = a[::-1]
    Image.fromarray(a).save(path)
    print("wrote %s" % path)


def jbu_upsample(source, guide_rgb, depth_tex, uw, uh, sigma_r, sharp=0.0):
    """Joint bilateral upsample; sharp > 0 pushes depth toward the nearer local extreme."""
    h, w = source.shape[:2]
    out = np.zeros((uh, uw), np.float32)
    vx = (np.arange(uw) + 0.5) / uw
    vy = (np.arange(uh) + 0.5) / uh
    sx = np.clip((vx * w).astype(int), 0, w - 1)
    sy = np.clip((vy * h).astype(int), 0, h - 1)
    # Outside DEPTH_RECT is far (0).
    mx = (vx - DEPTH_RECT[0]) / DEPTH_RECT[2]
    my = (vy - DEPTH_RECT[1]) / DEPTH_RECT[3]
    inside_x = (mx >= 0.0) & (mx <= 1.0)
    lp_x = mx * DEPTH_W - 0.5
    lp_y = my * DEPTH_H - 0.5
    bx = np.floor(lp_x).astype(int)
    by = np.floor(lp_y).astype(int)

    for y in range(uh):
        hi = source[sy[y], sx] / 255.0
        num = np.zeros(uw, np.float32)
        den = np.zeros(uw, np.float32)
        lo_d = np.full(uw, 1.0, np.float32)
        hi_d = np.zeros(uw, np.float32)
        for dy in range(-2, 3):
            qy = np.clip(by[y] + dy, 0, DEPTH_H - 1)
            for dx in range(-2, 3):
                qx = np.clip(bx + dx, 0, DEPTH_W - 1)
                sa = depth_tex[qy, qx]
                srgb = guide_rgb[qy, qx]
                offx = qx - lp_x
                offy = float(qy) - lp_y[y]
                ws = np.exp(-(offx * offx + offy * offy) / (2 * SIGMA_S ** 2))
                cd = hi - srgb
                wr = np.exp(-(cd * cd).sum(axis=1) / (2 * sigma_r ** 2))
                wgt = ws * wr
                num += wgt * sa
                den += wgt
                lo_d = np.minimum(lo_d, sa)
                hi_d = np.maximum(hi_d, sa)
        d = num / np.maximum(den, 1e-6)
        if sharp > 0.0:
            span = hi_d - lo_d
            u = np.clip((d - lo_d) / np.maximum(span, 1e-6), 0.0, 1.0)
            snapped = lo_d + span / (1.0 + np.exp(-24.0 * (u - 0.5)))
            d = np.where(span < 0.05, d, d + sharp * (snapped - d))
        inside = inside_x & (0.0 <= my[y] <= 1.0)
        out[y] = np.where(inside, d, 0.0)
    return out


def expand(depth_small, w, h):
    """Bilinear expand to full resolution, matching the sampler."""
    uh, uw = depth_small.shape
    ys = (np.arange(h) + 0.5) / h * uh - 0.5
    xs = (np.arange(w) + 0.5) / w * uw - 0.5
    y0 = np.clip(np.floor(ys).astype(int), 0, uh - 1)
    y1 = np.clip(y0 + 1, 0, uh - 1)
    x0 = np.clip(np.floor(xs).astype(int), 0, uw - 1)
    x1 = np.clip(x0 + 1, 0, uw - 1)
    ty = (ys - np.floor(ys))[:, None]
    tx = (xs - np.floor(xs))[None, :]
    top = depth_small[np.ix_(y0, x0)] * (1 - tx) + depth_small[np.ix_(y0, x1)] * tx
    bot = depth_small[np.ix_(y1, x0)] * (1 - tx) + depth_small[np.ix_(y1, x1)] * tx
    return top * (1 - ty) + bot * ty


def linear_axis(src, coords, axis):
    """Clamped linear fetch along one axis; coords in texels."""
    n = src.shape[axis]
    p = coords - 0.5
    i0 = np.floor(p).astype(int)
    shape = [1, 1]
    shape[axis] = -1
    f = (p - i0).astype(np.float32).reshape(shape)
    a = np.take(src, np.clip(i0, 0, n - 1), axis=axis)
    b = np.take(src, np.clip(i0 + 1, 0, n - 1), axis=axis)
    return a * (1 - f) + b * f


def cubic_axis(src, out_n, axis):
    """Cubic B-spline along one axis as two linear fetches, as in the shader."""
    n = src.shape[axis]
    tc = (np.arange(out_n) + 0.5) / out_n * n - 0.5
    base = np.floor(tc)
    f = tc - base
    f2 = f * f
    f3 = f2 * f
    w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0
    w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0
    w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0
    w3 = f3 / 6.0
    g0 = w0 + w1
    a0 = linear_axis(src, base - 0.5 + w1 / g0, axis)
    a1 = linear_axis(src, base + 1.5 + w3 / (w2 + w3), axis)
    shape = [1, 1]
    shape[axis] = -1
    g0 = g0.astype(np.float32).reshape(shape)
    return a1 + (a0 - a1) * g0


def expand_cubic(depth_small, w, h):
    """Separable cubic expand to full resolution."""
    return cubic_axis(cubic_axis(depth_small, w, 1), h, 0)


def edge_weight(w, fade_px):
    """Per-column shift weight: smoothstep from 0 at the side edges to 1 at fade_px."""
    if fade_px <= 0:
        return np.ones(w, np.float32)
    u = (np.arange(w) + 0.5) / w
    t = np.clip(np.minimum(u, 1.0 - u) * w / fade_px, 0.0, 1.0)
    return (t * t * (3.0 - 2.0 * t)).astype(np.float32)


def warp_rows(source, depth, y0, y1, sign, disp, convergence, span=20):
    """Occlusion-aware gather: each zero crossing of
    e(t) = t + disp * (d(x + t) - convergence) is a source landing here; the
    nearest surface wins. disp is per column (edge fade)."""
    h, w = source.shape[:2]
    xs = np.arange(w)
    best_t = np.zeros((y1 - y0, w), np.float32)
    best_d = np.full((y1 - y0, w), -1.0, np.float32)
    prev_e = prev_t = None
    for t in np.arange(-span, span + 1, dtype=np.float32):
        sx = np.clip(xs + t, 0, w - 1).astype(int)
        d = depth[y0:y1][:, sx]
        e = t + sign * disp[None, :] * (d - convergence)
        if prev_e is not None:
            crossed = (np.sign(e) != np.sign(prev_e)) & (np.abs(e - prev_e) > 1e-6)
            frac = np.where(crossed, prev_e / (prev_e - e + 1e-9), 0.0)
            take = crossed & (d > best_d)
            best_t = np.where(take, prev_t + frac, best_t)
            best_d = np.where(take, d, best_d)
        prev_e, prev_t = e, t

    gx = np.clip(xs[None, :] + best_t, 0, w - 1)
    g0 = np.floor(gx).astype(int)
    g1 = np.clip(g0 + 1, 0, w - 1)
    fx = (gx - g0)[..., None]
    rows = np.arange(y0, y1)[:, None]
    return source[rows, g0] * (1 - fx) + source[rows, g1] * fx, best_t


def warp_frame(source, depth, sign, disp, convergence):
    h, w = source.shape[:2]
    out = np.zeros((h, w, 3), np.float32)
    s15 = s50 = total = 0
    for y0 in range(0, h, CHUNK_ROWS):
        y1 = min(h, y0 + CHUNK_ROWS)
        rows, best_t = warp_rows(source, depth, y0, y1, sign, disp, convergence)
        out[y0:y1] = rows
        # Local stretch shows disocclusion; peak stretch matters more than area.
        st = 1.0 + np.diff(best_t, axis=1)
        s15 += int((st > 1.15).sum())
        s50 += int((st > 1.5).sum())
        total += st.size
    return out, 100.0 * s15 / total, 100.0 * s50 / total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("directory")
    ap.add_argument("--tag", default="1")
    ap.add_argument("--width", type=int, default=3840)
    ap.add_argument("--height", type=int, default=2160)
    ap.add_argument("--separation", type=float, default=0.005,
                    help="frame widths, the seekbar value over 1000")
    ap.add_argument("--convergence", type=float, default=0.5)
    ap.add_argument("--sigma", type=float, default=0.25, help="upsample range sigma")
    ap.add_argument("--sharp", type=float, default=0.0,
                    help="depth boundary sharpening, 0 to 1")
    ap.add_argument("--edge-fade", type=float, default=8.0,
                    help="pixels the shift fades out over at each side edge, 0 off")
    ap.add_argument("--depth-cubic", type=int, default=0,
                    help="1 reads the depth through the cubic B spline, 0 bilinear")
    ap.add_argument("--out", default=None, help="where to write PNGs")
    args = ap.parse_args()

    w, h = args.width, args.height
    uw, uh = w // 4, h // 4
    out = args.out or os.path.join(args.directory, "out")
    os.makedirs(out, exist_ok=True)

    source = load_raw(args.directory, args.tag, "source", np.uint8, w * h * 4)
    if source is None:
        raise SystemExit("no capture tagged %s in %s" % (args.tag, args.directory))
    source = source.reshape(h, w, 4)[:, :, :3].astype(np.float32)

    guide = load_raw(args.directory, args.tag, "guidetex", np.uint8, None)
    if guide is None:
        raise SystemExit("capture has no guidetex, it predates the upsample pass")
    global DEPTH_W, DEPTH_H, DEPTH_RECT
    shape = load_raw(args.directory, args.tag, "depthrect", np.float32, 6)
    if shape is not None:
        DEPTH_W, DEPTH_H = int(shape[0]), int(shape[1])
        DEPTH_RECT = tuple(float(v) for v in shape[2:])
    else:
        DEPTH_W = DEPTH_H = int(round((guide.size / 4) ** 0.5))
        DEPTH_RECT = (0.0, 0.0, 1.0, 1.0)
    if DEPTH_W * DEPTH_H * 4 != guide.size:
        raise SystemExit("guidetex is %d bytes, which is not %dx%d"
                         % (guide.size, DEPTH_W, DEPTH_H))
    guide = guide.reshape(DEPTH_H, DEPTH_W, 4).astype(np.float32) / 255.0
    depth_tex = guide[:, :, 3]

    print("source %dx%d, depth %dx%d over x %.3f y %.3f w %.3f h %.3f, upsampled %dx%d"
          % ((w, h, DEPTH_W, DEPTH_H) + DEPTH_RECT + (uw, uh)))
    print("depth percentiles: 2%% %.3f  25%% %.3f  50%% %.3f  75%% %.3f  98%% %.3f"
          % tuple(np.percentile(depth_tex, [2, 25, 50, 75, 98])))
    print("interquartile span %.3f of the 0..1 range"
          % (np.percentile(depth_tex, 75) - np.percentile(depth_tex, 25)))

    upsampled = jbu_upsample(source, guide[:, :, :3], depth_tex, uw, uh,
                             args.sigma, args.sharp)

    device_ups = load_raw(args.directory, args.tag, "upsampled", np.uint8, uw * uh)
    if device_ups is not None and args.sharp == 0.0:
        device_ups = device_ups.reshape(uh, uw).astype(np.float32) / 255.0
        err = np.abs(upsampled - device_ups)
        print("upsample vs device: mean %.4f, 99th %.4f (quantisation floor %.4f)"
              % (err.mean(), np.percentile(err, 99), 1 / 255))

    # The upsample target is RGBA8, so the device warp reads quantised depth.
    quantised = np.round(upsampled * 255.0) / 255.0
    if args.depth_cubic:
        depth_full = expand_cubic(quantised, w, h)
    else:
        depth_full = expand(quantised, w, h)
    disp = args.separation * w * edge_weight(w, args.edge_fade)

    for name, sign in (("left", 1.0), ("right", -1.0)):
        mine, s15, s50 = warp_frame(source, depth_full, sign, disp, args.convergence)
        save_png(os.path.join(out, "%s_%s_mine.png" % (args.tag, name)), mine)
        print("%-5s stretched >15%% %.3f%% of pixels, >50%% %.3f%%" % (name, s15, s50))
        device = load_raw(args.directory, args.tag, name, np.uint8, w * h * 4)
        if device is None or args.sharp != 0.0:
            continue
        device = device.reshape(h, w, 4)[:, :, :3].astype(np.float32)
        diff = np.abs(device - mine)
        print("%-5s vs device: mean |diff| %.2f, 99th pct %.0f, max %.0f (of 255)"
              % (name, diff.mean(), np.percentile(diff, 99), diff.max()))

    save_png(os.path.join(out, "%s_source.png" % args.tag), source)
    save_png(os.path.join(out, "%s_depth.png" % args.tag), depth_full * 255.0)
    print("one depth texel covers %.1f x %.1f output pixels"
          % (w * DEPTH_RECT[2] / DEPTH_W, h * DEPTH_RECT[3] / DEPTH_H))


if __name__ == "__main__":
    sys.exit(main())
