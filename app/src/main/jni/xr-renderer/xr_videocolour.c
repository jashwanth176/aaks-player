// HDR to SDR video colour, see xr_videocolour.h. The GLSL below mirrors the
// C path per pixel.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xr_videocolour.h"

// SMPTE ST 2084
#define PQ_M1 0.1593017578125f
#define PQ_M2 78.84375f
#define PQ_C1 0.8359375f
#define PQ_C2 18.8515625f
#define PQ_C3 18.6875f

// ARIB STD-B67 (HLG)
#define HLG_A 0.17883277f
#define HLG_B 0.28466892f
#define HLG_C 0.55991073f

static const float LUMA_2020[3] = { 0.2627f, 0.6780f, 0.0593f };
static const float LUMA_709[3] = { 0.2126f, 0.7152f, 0.0722f };
// Linear BT.2020 to linear BT.709, row major
static const float M_2020_TO_709[3][3] = {
    {  1.6605f, -0.5876f, -0.0728f },
    { -0.1246f,  1.1329f, -0.0083f },
    { -0.0182f, -0.1006f,  1.1187f },
};

static float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

float videoPeakNits(float maxCll, float masteringNits) {
    // Below the floor is untrustworthy; above the ceiling is beyond PQ
    if (maxCll >= VIDEO_PEAK_MIN_NITS && maxCll <= VIDEO_PEAK_MAX_NITS) {
        return maxCll;
    }
    if (masteringNits > 0.0f) {
        return clampf(masteringNits, VIDEO_PEAK_MIN_NITS, VIDEO_PEAK_MAX_NITS);
    }
    return VIDEO_PEAK_DEFAULT_NITS;
}

VideoColour videoColourFor(int transfer, float peakNits, int yuvTarget) {
    VideoColour vc;
    vc.yuvTarget = yuvTarget ? 1 : 0;
    if (transfer == VIDEO_TRANSFER_PQ) {
        vc.transfer = VIDEO_TRANSFER_PQ;
        vc.peakNits = peakNits > 0.0f ? clampf(peakNits, VIDEO_PEAK_MIN_NITS, VIDEO_PEAK_MAX_NITS)
                                      : VIDEO_PEAK_DEFAULT_NITS;
    }
    else if (transfer == VIDEO_TRANSFER_HLG) {
        vc.transfer = VIDEO_TRANSFER_HLG;
        vc.peakNits = VIDEO_HLG_PEAK_NITS;
    }
    else {
        vc.transfer = VIDEO_TRANSFER_SDR;
        vc.peakNits = 0.0f;
        // Raw YUV is never used for SDR
        vc.yuvTarget = 0;
    }
    return vc;
}

int videoColourSame(const VideoColour* a, const VideoColour* b) {
    return a->transfer == b->transfer && fabsf(a->peakNits - b->peakNits) < 0.5f
            && a->yuvTarget == b->yuvTarget;
}

float videoPqToNits(float e) {
    float p = powf(clampf(e, 0.0f, 1.0f), 1.0f / PQ_M2);
    float num = p - PQ_C1 > 0.0f ? p - PQ_C1 : 0.0f;
    return 10000.0f * powf(num / (PQ_C2 - PQ_C3 * p), 1.0f / PQ_M1);
}

float videoNitsToPq(float nits) {
    float y = powf(clampf(nits / 10000.0f, 0.0f, 1.0f), PQ_M1);
    return powf((PQ_C1 + PQ_C2 * y) / (1.0f + PQ_C3 * y), PQ_M2);
}

// ITU-R BT.2390 EETF without black level lift: identity below the knee, a
// Hermite spline above it. In PQ code, normalised to the source peak.
static float eetfNormalised(float pq, float srcPq, float dstPq) {
    float x = clampf(pq / srcPq, 0.0f, 1.0f);
    float maxL = dstPq / srcPq;
    float ks = 1.5f * maxL - 0.5f;
    if (x > ks && ks < 1.0f) {
        float t = (x - ks) / (1.0f - ks);
        float t2 = t * t;
        float t3 = t2 * t;
        x = (2.0f * t3 - 3.0f * t2 + 1.0f) * ks + (t3 - 2.0f * t2 + t) * (1.0f - ks)
                + (3.0f * t2 - 2.0f * t3) * maxL;
    }
    return x * srcPq;
}

float videoEetf(float pq, float srcPeakNits, float dstPeakNits) {
    return eetfNormalised(pq, videoNitsToPq(srcPeakNits), videoNitsToPq(dstPeakNits));
}

static void hlgToNits(const float e[3], float out[3]) {
    float s[3];
    for (int i = 0; i < 3; i++) {
        float v = clampf(e[i], 0.0f, 1.0f);
        s[i] = v <= 0.5f ? v * v / 3.0f : (expf((v - HLG_C) / HLG_A) + HLG_B) / 12.0f;
    }
    // Reference display system gamma, 1.2 at 1000 nits
    float ys = LUMA_2020[0] * s[0] + LUMA_2020[1] * s[1] + LUMA_2020[2] * s[2];
    float gain = VIDEO_HLG_PEAK_NITS * powf(ys > 1e-6f ? ys : 1e-6f, 0.2f);
    for (int i = 0; i < 3; i++) {
        out[i] = gain * s[i];
    }
}

void videoColourToSdr(const VideoColour* vc, const float coded[3], float out[3]) {
    if (vc->transfer == VIDEO_TRANSFER_SDR) {
        for (int i = 0; i < 3; i++) {
            out[i] = coded[i];
        }
        return;
    }
    float nits[3];
    if (vc->transfer == VIDEO_TRANSFER_HLG) {
        hlgToNits(coded, nits);
    }
    else {
        for (int i = 0; i < 3; i++) {
            nits[i] = videoPqToNits(coded[i]);
        }
    }
    float y = LUMA_2020[0] * nits[0] + LUMA_2020[1] * nits[1] + LUMA_2020[2] * nits[2];
    if (y > 1e-4f) {
        float mapped = videoPqToNits(videoEetf(videoNitsToPq(y), vc->peakNits, VIDEO_REFERENCE_NITS));
        for (int i = 0; i < 3; i++) {
            nits[i] *= mapped / y;
        }
    }
    float c[3];
    for (int r = 0; r < 3; r++) {
        c[r] = (M_2020_TO_709[r][0] * nits[0] + M_2020_TO_709[r][1] * nits[1]
                + M_2020_TO_709[r][2] * nits[2]) / VIDEO_REFERENCE_NITS;
    }
    // Gamut map: desaturate toward luminance just enough to fit, preserving
    // hue (a per-channel clip would shift it)
    float l = LUMA_709[0] * c[0] + LUMA_709[1] * c[1] + LUMA_709[2] * c[2];
    float lo = fminf(fminf(c[0], c[1]), c[2]);
    float hi = fmaxf(fmaxf(c[0], c[1]), c[2]);
    float k = 1.0f;
    if (lo < 0.0f) {
        k = fminf(k, l / fmaxf(l - lo, 1e-6f));
    }
    if (hi > 1.0f) {
        k = fminf(k, fmaxf(1.0f - l, 0.0f) / fmaxf(hi - l, 1e-6f));
    }
    k = clampf(k, 0.0f, 1.0f);
    for (int i = 0; i < 3; i++) {
        float v = clampf(l + (c[i] - l) * k, 0.0f, 1.0f);
        out[i] = powf(v, 1.0f / 2.4f);
    }
}

const char* videoTransferName(int transfer) {
    switch (transfer) {
        case VIDEO_TRANSFER_PQ: return "HDR10 (PQ)";
        case VIDEO_TRANSFER_HLG: return "HLG";
        default: return "SDR";
    }
}

// ---- shaders ---------------------------------------------------------------

static const char VERSION_LINE[] = "#version 300 es\n";
static const char YUV_TARGET_LINE[] = "#extension GL_EXT_YUV_target : require\n";
static const char SAMPLER_LINE[] = "uniform samplerExternalOES u_texture;\n";
static const char READ_CALL[] = "texture(u_texture,";
static const char READ_NEW[] = "videoTexel(";

// Raw read of limited-range BT.2020 YUV. Scaled for 10 bits in the top of 16
// (P010), the decoder's HDR output format.
static const char SAMPLER_RAW[] = "uniform highp __samplerExternal2DY2YEXT u_texture;\n";
static const char READ_RAW[] =
    "    vec3 coded = clamp(HDR_YUV_TO_RGB * (texture(u_texture, uv).rgb - vec3(0.0625, 0.5, 0.5)),"
    " 0.0, 1.0);\n";
// Driver read: its RGB taken as the coded BT.2020 signal
static const char READ_DRIVER[] = "    vec3 coded = texture(u_texture, uv).rgb;\n";

static const char HDR_GLSL[] =
    "%s"
    "const float HDR_SRC_PQ = %.7f;\n"
    "const float HDR_DST_PQ = %.7f;\n"
    "const float HDR_REF_NITS = %.1f;\n"
    "const mat3 HDR_YUV_TO_RGB = mat3(1.16895, 1.16895, 1.16895, 0.0, -0.18806, 2.15017,"
    " 1.68526, -0.65297, 0.0);\n"
    "const mat3 HDR_2020_TO_709 = mat3(1.6605, -0.1246, -0.0182, -0.5876, 1.1329, -0.1006,"
    " -0.0728, -0.0083, 1.1187);\n"
    "const vec3 HDR_LUMA_2020 = vec3(0.2627, 0.6780, 0.0593);\n"
    "const vec3 HDR_LUMA_709 = vec3(0.2126, 0.7152, 0.0722);\n"
    "vec3 hdrPqToNits(vec3 e) {\n"
    "    vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0 / 78.84375));\n"
    "    return 10000.0 * pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p),"
    " vec3(1.0 / 0.1593017578125));\n"
    "}\n"
    "float hdrNitsToPq(float n) {\n"
    "    float y = pow(clamp(n / 10000.0, 0.0, 1.0), 0.1593017578125);\n"
    "    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);\n"
    "}\n"
    "vec3 hdrHlgToNits(vec3 e) {\n"
    "    e = clamp(e, 0.0, 1.0);\n"
    "    vec3 s = mix(e * e / 3.0, (exp((e - 0.55991073) / 0.17883277) + 0.28466892) / 12.0,"
    " step(0.5000001, e));\n"
    "    return 1000.0 * pow(max(dot(s, HDR_LUMA_2020), 1e-6), 0.2) * s;\n"
    "}\n"
    "float hdrEetf(float e) {\n"
    "    float x = clamp(e / HDR_SRC_PQ, 0.0, 1.0);\n"
    "    float maxL = HDR_DST_PQ / HDR_SRC_PQ;\n"
    "    float ks = 1.5 * maxL - 0.5;\n"
    "    if (x > ks && ks < 1.0) {\n"
    "        float t = (x - ks) / (1.0 - ks);\n"
    "        float t2 = t * t;\n"
    "        float t3 = t2 * t;\n"
    "        x = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks)\n"
    "            + (3.0 * t2 - 2.0 * t3) * maxL;\n"
    "    }\n"
    "    return x * HDR_SRC_PQ;\n"
    "}\n"
    "vec4 videoTexel(vec2 uv) {\n"
    "%s"
    "    vec3 nits = %s(coded);\n"
    "    float y = dot(nits, HDR_LUMA_2020);\n"
    "    if (y > 1e-4) {\n"
    "        nits *= hdrPqToNits(vec3(hdrEetf(hdrNitsToPq(y)))).r / y;\n"
    "    }\n"
    "    vec3 c = HDR_2020_TO_709 * nits / HDR_REF_NITS;\n"
    "    float l = dot(c, HDR_LUMA_709);\n"
    "    float lo = min(min(c.r, c.g), c.b);\n"
    "    float hi = max(max(c.r, c.g), c.b);\n"
    "    float k = 1.0;\n"
    "    if (lo < 0.0) {\n"
    "        k = min(k, l / max(l - lo, 1e-6));\n"
    "    }\n"
    "    if (hi > 1.0) {\n"
    "        k = min(k, max(1.0 - l, 0.0) / max(hi - l, 1e-6));\n"
    "    }\n"
    "    c = clamp(mix(vec3(l), c, clamp(k, 0.0, 1.0)), 0.0, 1.0);\n"
    "    return vec4(pow(c, vec3(1.0 / 2.4)), 1.0);\n"
    "}\n";

static size_t countOf(const char* s, const char* what) {
    size_t n = 0;
    size_t len = strlen(what);
    for (const char* p = strstr(s, what); p != NULL; p = strstr(p + len, what)) {
        n++;
    }
    return n;
}

// Returns a new string with every from replaced by to
static char* replaceAll(const char* s, const char* from, const char* to) {
    size_t fromLen = strlen(from);
    size_t toLen = strlen(to);
    size_t n = countOf(s, from);
    size_t size = strlen(s) + n * (toLen > fromLen ? toLen - fromLen : 0) + 1;
    char* out = malloc(size);
    if (out == NULL) {
        return NULL;
    }
    char* w = out;
    const char* r = s;
    for (const char* p = strstr(r, from); p != NULL; p = strstr(r, from)) {
        memcpy(w, r, (size_t)(p - r));
        w += p - r;
        memcpy(w, to, toLen);
        w += toLen;
        r = p + fromLen;
    }
    strcpy(w, r);
    return out;
}

static char* copyOf(const char* s) {
    size_t len = strlen(s) + 1;
    char* out = malloc(len);
    if (out != NULL) {
        memcpy(out, s, len);
    }
    return out;
}

char* videoShaderSource(const char* src, const VideoColour* vc) {
    if (src == NULL) {
        return NULL;
    }
    if (vc == NULL || vc->transfer == VIDEO_TRANSFER_SDR || strstr(src, SAMPLER_LINE) == NULL) {
        return copyOf(src);
    }

    char block[4096];
    int blockLen = snprintf(block, sizeof(block), HDR_GLSL,
                            vc->yuvTarget ? SAMPLER_RAW : SAMPLER_LINE,
                            videoNitsToPq(vc->peakNits), videoNitsToPq(VIDEO_REFERENCE_NITS),
                            VIDEO_REFERENCE_NITS,
                            vc->yuvTarget ? READ_RAW : READ_DRIVER,
                            vc->transfer == VIDEO_TRANSFER_HLG ? "hdrHlgToNits" : "hdrPqToNits");
    if (blockLen <= 0 || blockLen >= (int)sizeof(block)) {
        return NULL;
    }

    // Replace reads first so the block's own u_texture read is untouched
    char* reads = replaceAll(src, READ_CALL, READ_NEW);
    if (reads == NULL) {
        return NULL;
    }
    char* withBlock = replaceAll(reads, SAMPLER_LINE, block);
    free(reads);
    if (withBlock == NULL || !vc->yuvTarget) {
        return withBlock;
    }
    // The raw read needs its extension, right after #version
    if (strncmp(withBlock, VERSION_LINE, strlen(VERSION_LINE)) != 0) {
        free(withBlock);
        return NULL;
    }
    size_t len = strlen(withBlock);
    char* out = malloc(len + sizeof(YUV_TARGET_LINE));
    if (out == NULL) {
        free(withBlock);
        return NULL;
    }
    size_t head = strlen(VERSION_LINE);
    memcpy(out, withBlock, head);
    memcpy(out + head, YUV_TARGET_LINE, strlen(YUV_TARGET_LINE));
    memcpy(out + head + strlen(YUV_TARGET_LINE), withBlock + head, len - head + 1);
    free(withBlock);
    return out;
}
