#include "xr_shaders.h"

const char* const VERTEX_SRC =
    "#version 300 es\n"
    "in vec4 a_position;\n"
    "in vec4 a_texcoord;\n"
    "out vec2 v_plain;\n"
    "void main() {\n"
    "    gl_Position = a_position;\n"
    "    v_plain = a_texcoord.xy;\n"
    "}\n";

// Picture grade, mirrors pictureGradeApply in xr_grade.c. u_grade: offset,
// contrast, exponent (1 / gamma), saturation.
#define GRADE_GLSL \
    "uniform float u_gradeOn;\n" \
    "uniform vec4 u_grade;\n" \
    "vec3 grade(vec3 c) {\n" \
    "    c = clamp(c, 0.0, 1.0);\n" \
    "    c = clamp((c - 0.5) * u_grade.y + 0.5 + u_grade.x, 0.0, 1.0);\n" \
    "    c = pow(c, vec3(u_grade.z));\n" \
    "    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));\n" \
    "    return clamp(mix(vec3(l), c, u_grade.w), 0.0, 1.0);\n" \
    "}\n"

// Gather warp: samples the frame shifted by a per-eye disparity from the depth
// map. The texture matrix is applied after the shift (frame space).
// Player frame: the picture edge feathers into a glow, in screen heights from
// the centre. Shared by the eye pass and the glow so both split light alike.
//   u_frame      x enabled, y width / height, z edge feather, w corner radius
//   u_frameLook  x stereo crop fraction, y ambient light 0..1, z strength 0..1,
//                w this eye's shift in eye widths (eye pass only)
#define FRAME_GLSL \
    "uniform vec4 u_frame;\n" \
    "uniform vec4 u_frameLook;\n" \
    "float frameDist(vec2 p) {\n" \
    "    vec2 halfSize = vec2(0.5 * (1.0 - u_frameLook.x) * u_frame.y, 0.5);\n" \
    "    float r = u_frame.w;\n" \
    "    vec2 q = abs(p) - halfSize + r;\n" \
    "    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n" \
    "}\n" \
    /* x picture weight at that distance, y glow weight */ \
    "vec2 frameWeights(float dist) {\n" \
    "    float imm = u_frameLook.y;\n" \
    "    float s = u_frameLook.z;\n" \
    "    float fw = u_frame.z;\n" \
    "    float K = mix(16.0, 1.5 + 3.5 * (1.0 - s), imm);\n" \
    "    float Q = mix(0.4, 0.3, imm);\n" \
    "    float inner = (1.0 - 0.6 * (1.0 - s)) * imm;\n" \
    "    float f = mix(fw, fw * 0.2 / 0.084, imm);\n" \
    "    float g = 1.0 - clamp(-dist / f, 0.0, 1.0);\n" \
    "    float g3 = g * g * g;\n" \
    "    float sf3 = 1.0 - (4.0 * g3 - 3.0 * g3 * g);\n" \
    "    float k = K * 0.084 / fw;\n" \
    "    float sep = -2.0 * Q / (k * (1.0 - Q));\n" \
    "    float x = dist + sep * inner;\n" \
    "    float I;\n" \
    "    if (x > 0.0) {\n" \
    "        I = (1.0 - Q) / (1.0 + k * x);\n" \
    "    }\n" \
    "    else if (x < sep) {\n" \
    "        I = 1.0;\n" \
    "    }\n" \
    "    else {\n" \
    "        float t = k * x * (1.0 - Q) + 2.0 * Q;\n" \
    "        I = 1.0 - t * t / (4.0 * Q);\n" \
    "    }\n" \
    "    return vec2(sf3 * I, (1.0 - sf3) * I);\n" \
    "}\n"

const char* const FRAGMENT_SRC =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform samplerExternalOES u_texture;\n"
    "uniform sampler2D u_depth;\n"
    "uniform sampler2D u_offsets;\n"
    "uniform mat4 u_texmatrix;\n"
    "uniform float u_disparity;\n"
    "uniform float u_showDepth;\n"
    "uniform float u_barTest;\n"
    "uniform vec3 u_tint;\n"
    "uniform float u_occlusion;\n"
    "uniform float u_eyeIndex;\n"
    "uniform float u_convergence;\n"
    "uniform float u_dispTexels;\n"
    "uniform float u_lowResWidth;\n"
    "uniform float u_frameWidth;\n"
    // Keeps the shifted sample inside the frame, uv; -1 disables
    "uniform float u_srcInset;\n"
    // Left/right edge band over which the shift fades to 0, frame uv; 0 for none
    "uniform float u_edgeFade;\n"
    "uniform float u_depthCubic;\n"
    // This eye's source rect (offset, size) in frame uv
    "uniform vec4 u_srcRect;\n"
    GRADE_GLSL
    FRAME_GLSL
    "out vec4 fragColor;\n"
    // Optional cubic B-spline depth read (bilinear stair-steps silhouettes).
    // The vertical half is computed once per pixel into depthRow.
    "float depthY;\n"
    "vec2 depthSize;\n"
    "float depthTexelX;\n"
    "vec3 depthRow;\n"
    // One spline axis: two bilinear fetch positions and the first's weight
    "vec3 splineAxis(float t) {\n"
    "    float base = floor(t);\n"
    "    float f = t - base;\n"
    "    float f2 = f * f;\n"
    "    float g0 = 5.0 + f * (-3.0 + f * (-3.0 + 2.0 * f));\n"
    "    float w1 = 4.0 + f2 * (-6.0 + 3.0 * f);\n"
    "    return vec3(base - 0.5 + w1 / g0, base + 1.5 + f2 * f / (6.0 - g0), g0 / 6.0);\n"
    "}\n"
    "float depthAt(float x) {\n"
    "    if (u_depthCubic < 0.5) {\n"
    "        return texture(u_depth, vec2(x, depthY)).a;\n"
    "    }\n"
    "    vec3 sx = splineAxis(x * depthSize.x - 0.5);\n"
    "    vec2 hx = sx.xy * depthTexelX;\n"
    "    float a00 = texture(u_depth, vec2(hx.x, depthRow.x)).a;\n"
    "    float a10 = texture(u_depth, vec2(hx.y, depthRow.x)).a;\n"
    "    float a01 = texture(u_depth, vec2(hx.x, depthRow.y)).a;\n"
    "    float a11 = texture(u_depth, vec2(hx.y, depthRow.y)).a;\n"
    "    return mix(mix(a11, a01, sx.z), mix(a10, a00, sx.z), depthRow.z);\n"
    "}\n"
    "void main() {\n"
    "    depthY = u_srcRect.y + v_plain.y * u_srcRect.w;\n"
    "    if (u_depthCubic > 0.5) {\n"
    "        depthSize = vec2(textureSize(u_depth, 0));\n"
    "        depthTexelX = 1.0 / depthSize.x;\n"
    "        vec3 sy = splineAxis(depthY * depthSize.y - 0.5);\n"
    "        depthRow = vec3(sy.xy / depthSize.y, sy.z);\n"
    "    }\n"
    "    if (u_showDepth > 0.5) {\n"
    "        float dd = depthAt(v_plain.x);\n"
    "        fragColor = vec4(dd, dd, dd, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    vec2 tc = v_plain;\n"
    // Fade the shift near the side edges so the edge column is not smeared
    "    float edgeW = smoothstep(0.0, max(u_edgeFade, 1e-6),\n"
    "                             min(v_plain.x, 1.0 - v_plain.x));\n"
    "    float disp = edgeW * u_disparity;\n"
    "    if (u_occlusion > 0.5) {\n"
    // Two Newton steps refine the offset map's texel-accurate hit
    "        int reach = int(ceil(abs(u_dispTexels)\n"
    "                        * max(u_convergence, 1.0 - u_convergence))) + 2;\n"
    "        vec2 enc = texture(u_offsets, vec2(v_plain.x, depthY)).rg;\n"
    "        float off = (u_eyeIndex < 0.5 ? enc.r : enc.g) - 0.5;\n"
    "        tc.x = v_plain.x + edgeW * off * 2.0 * float(reach) / u_lowResWidth;\n"
    "        float h = 1.0 / u_frameWidth;\n"
    "        for (int i = 0; i < 2; i++) {\n"
    // Only d0 sets the position, so only it uses the cubic read
    "            float d0 = depthAt(tc.x);\n"
    "            float dm = texture(u_depth, vec2(tc.x - h, depthY)).a;\n"
    "            float dp = texture(u_depth, vec2(tc.x + h, depthY)).a;\n"
    "            float e = (tc.x - v_plain.x) + disp * (d0 - u_convergence);\n"
    "            float slope = 1.0 + disp * (dp - dm) / (2.0 * h);\n"
    "            if (abs(slope) < 0.25) {\n"
    "                slope = 0.25;\n"
    "            }\n"
    "            tc.x -= clamp(e / slope, -4.0 * h, 4.0 * h);\n"
    "        }\n"
    "    }\n"
    "    else {\n"
    "        tc.x -= disp * (depthAt(v_plain.x) - u_convergence);\n"
    "    }\n"
    "    if (u_barTest > 0.5) {\n"
    "        float b = 1.0 - step(0.004, abs(tc.x - 0.5));\n"
    "        fragColor = vec4(b, b, b, 1.0);\n"
    "        return;\n"
    "    }\n"
    // Keep the sample inside the frame (external texture edges are driver-
    // defined). The stereo crop shifts each eye outward.
    "    if (u_frame.x > 0.5) {\n"
    "        tc.x = clamp(tc.x + u_frameLook.w, 0.0, 1.0);\n"
    "    }\n"
    "    tc.x = clamp(tc.x, u_srcInset, 1.0 - u_srcInset);\n"
    "    tc = u_srcRect.xy + tc * u_srcRect.zw;\n"
    "    fragColor = texture(u_texture, (u_texmatrix * vec4(tc, 0.0, 1.0)).xy);\n"
    // Grade after the shift, colour only
    "    if (u_gradeOn > 0.5) {\n"
    "        fragColor.rgb = grade(fragColor.rgb);\n"
    "    }\n"
    "    fragColor.rgb *= u_tint;\n"
    // Picture weight, premultiplied, under the glow
    "    if (u_frame.x > 0.5) {\n"
    "        vec2 w = frameWeights(frameDist(vec2((v_plain.x - 0.5) * u_frame.y, v_plain.y - 0.5)));\n"
    "        float a = clamp(w.x / max(1.0 - w.y, 0.001), 0.0, 1.0);\n"
    "        fragColor = vec4(fragColor.rgb * a, a);\n"
    "    }\n"
    "}\n";

// Joint bilateral depth upsample: weights the 5x5 depth neighbourhood by guide
// colour similarity so depth edges snap to colour edges, reducing halos. The
// guide is the depth texture's rgb. Too small a u_sigmaR makes texture speckle.
const char* const UPSAMPLE_FRAGMENT_SRC =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform samplerExternalOES u_texture;\n"
    "uniform sampler2D u_depth;\n"
    "uniform mat4 u_texmatrix;\n"
    "uniform float u_sigmaR;\n"
    "uniform float u_sharp;\n"
    "uniform vec2 u_depthSize;\n"
    "out vec4 fragColor;\n"
    "const float SIGMA_S = 1.5;\n"
    "const float FLAT = 0.05;\n"
    "void main() {\n"
    "    vec3 hi = texture(u_texture, (u_texmatrix * vec4(v_plain, 0.0, 1.0)).xy).rgb;\n"
    "    vec2 lp = v_plain * u_depthSize - 0.5;\n"
    "    ivec2 base = ivec2(floor(lp));\n"
    "    float num = 0.0;\n"
    "    float den = 0.0;\n"
    "    float dlo = 1.0;\n"
    "    float dhi = 0.0;\n"
    "    for (int dy = -2; dy <= 2; dy++) {\n"
    "        for (int dx = -2; dx <= 2; dx++) {\n"
    "            ivec2 q = clamp(base + ivec2(dx, dy), ivec2(0), ivec2(u_depthSize) - 1);\n"
    "            vec4 s = texelFetch(u_depth, q, 0);\n"
    "            vec2 off = vec2(q) - lp;\n"
    "            float ws = exp(-dot(off, off) / (2.0 * SIGMA_S * SIGMA_S));\n"
    "            vec3 cd = hi - s.rgb;\n"
    "            float wr = exp(-dot(cd, cd) / (2.0 * u_sigmaR * u_sigmaR));\n"
    "            float w = ws * wr;\n"
    "            num += w * s.a;\n"
    "            den += w;\n"
    "            dlo = min(dlo, s.a);\n"
    "            dhi = max(dhi, s.a);\n"
    "        }\n"
    "    }\n"
    "    float d = num / max(den, 1e-6);\n"
    // Push toward the nearer local extreme, turning ramps into steps
    "    float span = dhi - dlo;\n"
    "    if (u_sharp > 0.0 && span >= FLAT) {\n"
    "        float u = clamp((d - dlo) / max(span, 1e-6), 0.0, 1.0);\n"
    "        float snapped = dlo + span / (1.0 + exp(-24.0 * (u - 0.5)));\n"
    "        d = mix(d, snapped, u_sharp);\n"
    "    }\n"
    "    fragColor = vec4(d);\n"
    "}\n";

// Offset map: inverts the warp for both eyes at depth map resolution. A source
// at offset t lands here with error
//     e(t) = t + disp * (d(here + t) - convergence)
// so each zero crossing is a candidate; the nearest surface wins. The span is
// about nine texels, so the search is exhaustive.
const char* const OFFSET_FRAGMENT_SRC =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform sampler2D u_depth;\n"
    "uniform float u_dispTexels;\n"
    "uniform float u_convergence;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    ivec2 sz = textureSize(u_depth, 0);\n"
    "    int x = int(gl_FragCoord.x);\n"
    "    int y = int(gl_FragCoord.y);\n"
    "    int reach = int(ceil(abs(u_dispTexels)\n"
    "                    * max(u_convergence, 1.0 - u_convergence))) + 2;\n"
    "    vec2 result = vec2(0.0);\n"
    "    for (int eye = 0; eye < 2; eye++) {\n"
    "        float disp = (eye == 0) ? u_dispTexels : -u_dispTexels;\n"
    "        float here = texelFetch(u_depth, ivec2(x, y), 0).a;\n"
    "        float bestD = -1.0;\n"
    "        float bestOff = -disp * (here - u_convergence);\n"
    "        float pd = texelFetch(u_depth,\n"
    "                ivec2(clamp(x - reach, 0, sz.x - 1), y), 0).a;\n"
    "        float pe = float(-reach) + disp * (pd - u_convergence);\n"
    "        for (int t = -reach + 1; t <= reach; t++) {\n"
    "            float cd = texelFetch(u_depth,\n"
    "                    ivec2(clamp(x + t, 0, sz.x - 1), y), 0).a;\n"
    "            float ce = float(t) + disp * (cd - u_convergence);\n"
    "            float span = ce - pe;\n"
    "            if (pe * ce <= 0.0 && abs(span) > 1e-6) {\n"
    "                float f = clamp(-pe / span, 0.0, 1.0);\n"
    "                float rd = pd + f * (cd - pd);\n"
    "                if (rd > bestD) {\n"
    "                    bestD = rd;\n"
    "                    bestOff = float(t - 1) + f;\n"
    "                }\n"
    "            }\n"
    "            pd = cd;\n"
    "            pe = ce;\n"
    "        }\n"
    "        result[eye] = bestOff;\n"
    "    }\n"
    "    fragColor = vec4(result / (2.0 * float(reach)) + 0.5, 0.0, 1.0);\n"
    "}\n";

// Depth model input downscale, 4x4 box per texel to avoid aliasing
const char* const DOWNSCALE_FRAGMENT_SRC =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform samplerExternalOES u_texture;\n"
    "uniform mat4 u_texmatrix;\n"
    "uniform vec2 u_texel;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec3 sum = vec3(0.0);\n"
    "    for (int y = 0; y < 4; y++) {\n"
    "        for (int x = 0; x < 4; x++) {\n"
    "            vec2 off = (vec2(float(x), float(y)) - 1.5) * (0.25 * u_texel);\n"
    "            vec2 tc = v_plain + off;\n"
    "            sum += texture(u_texture, (u_texmatrix * vec4(tc, 0.0, 1.0)).xy).rgb;\n"
    "        }\n"
    "    }\n"
    "    fragColor = vec4(sum * (1.0 / 16.0), 1.0);\n"
    "}\n";

// Ambilight colour sample, 32x32, 4x4 box per texel. u_crop is the sampled
// region (x0 y0 w h). Graded per tap.
const char* const AMBI_FRAGMENT_SRC =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform samplerExternalOES u_texture;\n"
    "uniform mat4 u_texmatrix;\n"
    "uniform vec4 u_crop;\n"
    GRADE_GLSL
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec2 base = u_crop.xy + v_plain * u_crop.zw;\n"
    "    vec3 sum = vec3(0.0);\n"
    "    for (int y = 0; y < 4; y++) {\n"
    "        for (int x = 0; x < 4; x++) {\n"
    // Box scales with the crop so it stays inside the content
    "            vec2 off = (vec2(float(x), float(y)) - 1.5) * (0.25 / 32.0) * u_crop.zw;\n"
    "            vec2 tc = base + off;\n"
    "            vec3 c = texture(u_texture, (u_texmatrix * vec4(tc, 0.0, 1.0)).xy).rgb;\n"
    "            if (u_gradeOn > 0.5) {\n"
    "                c = grade(c);\n"
    "            }\n"
    "            sum += c;\n"
    "        }\n"
    "    }\n"
    "    fragColor = vec4(sum * (1.0 / 16.0), 1.0);\n"
    "}\n";

// Glow edge pass: luma normalisation and dark-edge roll-off along the ring.
// Mirrors xr_glow.c; constants come in as uniforms.
const char* const GLOW_EDGE_FRAGMENT_SRC =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D u_texture;\n"
    // Luma target/floor/full, lit range, ring reach in texels, inward steps
    "uniform vec3 u_luma;\n"
    "uniform vec2 u_lit;\n"
    "uniform float u_reach;\n"
    "uniform int u_steps;\n"
    "out vec4 fragColor;\n"
    // 32 must match AMBI_SAMPLE_TEX. Ring runs anticlockwise from bottom left,
    // 31 texels a side.
    "const int N = 32;\n"
    "const int RING = 4 * (N - 1);\n"
    "vec3 lifted(ivec2 p) {\n"
    "    vec3 c = texelFetch(u_texture, p, 0).rgb;\n"
    "    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
    "    float s = smoothstep(u_luma.y, u_luma.z, l);\n"
    "    return clamp(c * (mix(l, u_luma.x, s) / max(l, 1e-5)), 0.0, 1.0);\n"
    "}\n"
    "float lit(vec3 c) {\n"
    "    return smoothstep(u_lit.x, u_lit.y, max(c.r, max(c.g, c.b)));\n"
    "}\n"
    "ivec2 ringTexel(int i) {\n"
    "    if (i < N - 1) return ivec2(i, 0);\n"
    "    if (i < 2 * (N - 1)) return ivec2(N - 1, i - (N - 1));\n"
    "    if (i < 3 * (N - 1)) return ivec2(3 * (N - 1) - i, N - 1);\n"
    "    return ivec2(0, RING - i);\n"
    "}\n"
    "int ringIndex(ivec2 p) {\n"
    "    if (p.y == 0) return p.x;\n"
    "    if (p.x == N - 1) return (N - 1) + p.y;\n"
    "    if (p.y == N - 1) return 3 * (N - 1) - p.x;\n"
    "    return RING - p.y;\n"
    "}\n"
    // Texel colour, or if dark the first lit texel inward
    "vec3 edgeColour(int i) {\n"
    "    ivec2 p = ringTexel(i);\n"
    "    ivec2 inward = ivec2(p.x == 0 ? 1 : (p.x == N - 1 ? -1 : 0),\n"
    "                         p.y == 0 ? 1 : (p.y == N - 1 ? -1 : 0));\n"
    "    vec3 c = lifted(p);\n"
    "    for (int s = 1; s <= u_steps; s++) {\n"
    "        vec3 deeper = lifted(p + inward * s);\n"
    "        c = mix(deeper, c, lit(c));\n"
    "    }\n"
    "    return c;\n"
    "}\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "    if (p.x > 0 && p.y > 0 && p.x < N - 1 && p.y < N - 1) {\n"
    "        fragColor = vec4(lifted(p), 1.0);\n"
    "        return;\n"
    "    }\n"
    "    int j = ringIndex(p);\n"
    "    int r = min(int(u_reach), RING / 2 - 1);\n"
    "    vec3 own = vec3(0.0);\n"
    "    vec3 sum = vec3(0.0);\n"
    "    float weight = 0.0;\n"
    "    float carried = 0.0;\n"
    "    for (int k = -r; k <= r; k++) {\n"
    "        vec3 c = edgeColour((j + k + RING) % RING);\n"
    "        float t = float(k) / u_reach;\n"
    "        float w = (1.0 - t * t) * lit(c);\n"
    "        sum += c * w;\n"
    "        weight += w;\n"
    "        carried = max(carried, w);\n"
    "        if (k == 0) {\n"
    "            own = c;\n"
    "        }\n"
    "    }\n"
    "    vec3 nearby = sum / max(weight, 1e-4);\n"
    "    vec3 c = own + nearby * (carried * (1.0 - lit(own)));\n"
    "    fragColor = vec4(clamp(c, 0.0, 1.0), 1.0);\n"
    "}\n";

// Glow. The quad is larger than the screen and only its border is visible;
// clamped sampling carries the edge colours outward.
const char* const GLOW_FRAGMENT_SRC =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform sampler2D u_texture;\n"
    "uniform float u_intensity;\n"
    // 1: spline widened by three texels; 0: spline only
    "uniform float u_blur;\n"
    "out vec4 fragColor;\n"
    // Glow scale; 32.0 must match AMBI_SAMPLE_TEX
    "uniform float u_scale;\n"
    FRAME_GLSL
    "const float size = 32.0;\n"
    // Cubic B-spline as four bilinear fetches (plain bilinear shows creases)
    "vec3 spline(vec2 fuv) {\n"
    "    vec2 tc = fuv * size - 0.5;\n"
    "    vec2 base = floor(tc);\n"
    "    vec2 f = tc - base;\n"
    "    vec2 f2 = f * f;\n"
    "    vec2 f3 = f2 * f;\n"
    "    vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;\n"
    "    vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;\n"
    "    vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;\n"
    "    vec2 w3 = f3 / 6.0;\n"
    // Each tap pair folds into one bilinear fetch: 16 reads from 4 fetches
    "    vec2 g0 = w0 + w1;\n"
    "    vec2 g1 = w2 + w3;\n"
    "    vec2 h0 = (base - 0.5 + w1 / g0) / size;\n"
    "    vec2 h1 = (base + 1.5 + w3 / g1) / size;\n"
    "    vec3 c00 = texture(u_texture, vec2(h0.x, h0.y)).rgb;\n"
    "    vec3 c10 = texture(u_texture, vec2(h1.x, h0.y)).rgb;\n"
    "    vec3 c01 = texture(u_texture, vec2(h0.x, h1.y)).rgb;\n"
    "    vec3 c11 = texture(u_texture, vec2(h1.x, h1.y)).rgb;\n"
    "    return mix(mix(c11, c01, g0.x), mix(c10, c00, g0.x), g0.y);\n"
    "}\n"
    // Spline averaged over three texels per axis, as 9 bilinear fetches
    "vec3 blurredSpline(vec2 fuv) {\n"
    "    vec2 tc = fuv * size - 0.5;\n"
    "    vec2 base = floor(tc);\n"
    "    vec2 f = tc - base;\n"
    "    vec2 f2 = f * f;\n"
    "    vec2 f3 = f2 * f;\n"
    // Spline weights on base-1..base+2, widened kernel on base-2..base+3
    "    vec2 b0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;\n"
    "    vec2 b1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;\n"
    "    vec2 b2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;\n"
    "    vec2 b3 = f3 / 6.0;\n"
    "    vec2 wb = (b0 + b1) / 3.0;\n"
    "    vec2 wd = (b1 + b2 + b3) / 3.0;\n"
    "    vec2 wf = b3 / 3.0;\n"
    "    vec2 g0 = (2.0 * b0 + b1) / 3.0;\n"
    "    vec2 g1 = (b0 + 2.0 * b1 + 2.0 * b2 + b3) / 3.0;\n"
    "    vec2 g2 = (b2 + 2.0 * b3) / 3.0;\n"
    "    vec2 h0 = (base - 1.5 + wb / g0) / size;\n"
    "    vec2 h1 = (base + 0.5 + wd / g1) / size;\n"
    "    vec2 h2 = (base + 2.5 + wf / g2) / size;\n"
    "    vec3 r0 = texture(u_texture, vec2(h0.x, h0.y)).rgb * g0.x\n"
    "            + texture(u_texture, vec2(h1.x, h0.y)).rgb * g1.x\n"
    "            + texture(u_texture, vec2(h2.x, h0.y)).rgb * g2.x;\n"
    "    vec3 r1 = texture(u_texture, vec2(h0.x, h1.y)).rgb * g0.x\n"
    "            + texture(u_texture, vec2(h1.x, h1.y)).rgb * g1.x\n"
    "            + texture(u_texture, vec2(h2.x, h1.y)).rgb * g2.x;\n"
    "    vec3 r2 = texture(u_texture, vec2(h0.x, h2.y)).rgb * g0.x\n"
    "            + texture(u_texture, vec2(h1.x, h2.y)).rgb * g1.x\n"
    "            + texture(u_texture, vec2(h2.x, h2.y)).rgb * g2.x;\n"
    "    return r0 * g0.y + r1 * g1.y + r2 * g2.y;\n"
    "}\n"
    "void main() {\n"
    "    vec2 uv = v_plain;\n"
    "    float scale = u_scale;\n"
    "    vec2 fuv = (uv - 0.5) * scale + 0.5;\n"
    "    vec3 color;\n"
    "    if (u_blur > 0.5) {\n"
    "        color = blurredSpline(fuv);\n"
    "    }\n"
    "    else {\n"
    "        color = spline(fuv);\n"
    "    }\n"
    // 0 at the screen edge, 1 at the rim
    "    vec2 d = max(abs(uv - 0.5) - 0.5 / scale, 0.0) / (0.5 - 0.5 / scale);\n"
    "    float t = min(length(d), 1.0);\n"
    // Zero slope at both ends to avoid visible lines; squared for strength
    "    float s = t * t * (3.0 - 2.0 * t);\n"
    "    float fall = (1.0 - s) * (1.0 - s);\n"
    "    float a = fall * u_intensity;\n"
    // Player mode: glow weight at this distance, faded out near the rim
    "    if (u_frame.x > 0.5) {\n"
    "        vec2 p = (uv - 0.5) * scale * vec2(u_frame.y, 1.0);\n"
    "        vec2 rim = 1.0 - smoothstep(vec2(0.38), vec2(0.5), abs(uv - 0.5));\n"
    "        a = frameWeights(frameDist(p)).y * rim.x * rim.y;\n"
    "    }\n"
    // Premultiplied, as the compositor expects
    "    fragColor = vec4(color * a, a);\n"
    "}\n";

// 3D room. Screen light is per vertex to keep the fragment shader cheap.
const char* const ROOM_VERTEX_SRC =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec3 a_position;\n"
    "in vec3 a_color;\n"
    "in float a_spill;\n"
    "in vec2 a_uv;\n"
    "uniform mat4 u_viewproj;\n"
    "uniform sampler2D u_ambi;\n"
    "uniform float u_spillGain;\n"
    "out vec3 v_color;\n"
    "out vec3 v_wash;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    // Five taps: centre and each edge midpoint
    "    vec3 lit = texture(u_ambi, vec2(0.5, 0.5)).rgb;\n"
    "    lit += texture(u_ambi, vec2(0.15, 0.5)).rgb;\n"
    "    lit += texture(u_ambi, vec2(0.85, 0.5)).rgb;\n"
    "    lit += texture(u_ambi, vec2(0.5, 0.15)).rgb;\n"
    "    lit += texture(u_ambi, vec2(0.5, 0.85)).rgb;\n"
    "    vec3 wash = lit * 0.2;\n"
    "    v_color = a_color;\n"
    "    v_wash = wash * (a_spill * u_spillGain);\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = u_viewproj * vec4(a_position, 1.0);\n"
    "}\n";

const char* const ROOM_FRAGMENT_SRC =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec3 v_color;\n"
    "in vec3 v_wash;\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_room;\n"
    "uniform float u_texMix;\n"
    "uniform float u_dim;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    // mix 0 vertex colour, 1 atlas (untextured parts bind a white 1x1)
    "    vec3 base = mix(v_color, texture(u_room, v_uv).rgb, u_texMix) * u_dim;\n"
    "    fragColor = vec4(base + v_wash, 1.0);\n"
    "}\n";

// Controller model: vertex colours, per-vertex lighting from one fixed light
// above and behind the seat, plus ambient
const char* const MODEL_VERTEX_SRC =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec3 a_position;\n"
    "in vec3 a_normal;\n"
    "in vec3 a_color;\n"
    "uniform mat4 u_viewproj;\n"
    "uniform mat4 u_model;\n"
    "out vec3 v_color;\n"
    "void main() {\n"
    // Rigid (+ mirror) model matrix, so its 3x3 transforms normals
    "    vec3 n = normalize(mat3(u_model) * a_normal);\n"
    "    float lit = 0.45 + 0.55 * max(dot(n, vec3(-0.303, 0.808, 0.505)), 0.0);\n"
    "    v_color = a_color * lit;\n"
    "    gl_Position = u_viewproj * (u_model * vec4(a_position, 1.0));\n"
    "}\n";

const char* const MODEL_FRAGMENT_SRC =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec3 v_color;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    fragColor = vec4(v_color, 1.0);\n"
    "}\n";
