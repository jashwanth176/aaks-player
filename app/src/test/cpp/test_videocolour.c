// HDR to SDR: PQ curve, BT.2390 tone map, peak selection and the rewritten video shaders.
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "xr_shaders.h"
#include "xr_videocolour.h"

static void testPq(void) {
    // Published ST 2084 values.
    CHECK_NEAR(videoNitsToPq(0.0f), 0.0, 1e-6);
    CHECK_NEAR(videoNitsToPq(100.0f), 0.5081, 0.0005);
    CHECK_NEAR(videoNitsToPq(1000.0f), 0.7518, 0.0005);
    CHECK_NEAR(videoNitsToPq(10000.0f), 1.0, 1e-5);
    const float nits[] = { 0.05f, 1.0f, 100.0f, 203.0f, 1000.0f, 4000.0f, 10000.0f };
    for (size_t i = 0; i < sizeof(nits) / sizeof(nits[0]); i++) {
        CHECK_NEAR(videoPqToNits(videoNitsToPq(nits[i])), nits[i], nits[i] * 2e-3);
    }
    // Clamped to range.
    CHECK_NEAR(videoPqToNits(-1.0f), 0.0, 1e-6);
    CHECK_NEAR(videoPqToNits(2.0f), 10000.0, 1.0);
}

static void testEetf(void) {
    // Identity below the knee.
    float dark = videoNitsToPq(40.0f);
    CHECK_NEAR(videoEetf(dark, 1000.0f, VIDEO_REFERENCE_NITS), dark, 1e-5);
    // Content peak maps to the display peak.
    CHECK_NEAR(videoPqToNits(videoEetf(videoNitsToPq(1000.0f), 1000.0f, VIDEO_REFERENCE_NITS)),
               VIDEO_REFERENCE_NITS, 1.0);
    CHECK_NEAR(videoPqToNits(videoEetf(videoNitsToPq(4000.0f), 4000.0f, VIDEO_REFERENCE_NITS)),
               VIDEO_REFERENCE_NITS, 1.0);
    // Above the content peak clamps to the display peak.
    CHECK_NEAR(videoEetf(videoNitsToPq(9000.0f), 1000.0f, VIDEO_REFERENCE_NITS),
               videoNitsToPq(VIDEO_REFERENCE_NITS), 1e-4);
    // Monotonic and bounded by the display peak.
    float last = -1.0f;
    for (int i = 0; i <= 200; i++) {
        float pq = i / 200.0f;
        float out = videoEetf(pq, 1000.0f, VIDEO_REFERENCE_NITS);
        CHECK(out >= last - 1e-6f);
        CHECK(out <= videoNitsToPq(VIDEO_REFERENCE_NITS) + 1e-5f);
        last = out;
    }
}

static void grey(float nits, float coded[3]) {
    float e = videoNitsToPq(nits);
    coded[0] = coded[1] = coded[2] = e;
}

static void testToSdr(void) {
    VideoColour pq = videoColourFor(VIDEO_TRANSFER_PQ, 1000.0f, 1);
    float coded[3], out[3];

    grey(0.0f, coded);
    videoColourToSdr(&pq, coded, out);
    CHECK_NEAR(out[0], 0.0, 1e-4);
    CHECK_NEAR(out[1], 0.0, 1e-4);
    CHECK_NEAR(out[2], 0.0, 1e-4);

    // Under the knee: 50 of 203 nits reference white, encoded with gamma 2.4.
    grey(50.0f, coded);
    videoColourToSdr(&pq, coded, out);
    CHECK_NEAR(out[0], powf(50.0f / 203.0f, 1.0f / 2.4f), 2e-3);
    CHECK_NEAR(out[0], out[1], 1e-4);
    CHECK_NEAR(out[1], out[2], 1e-4);

    // Content peak becomes SDR white.
    grey(1000.0f, coded);
    videoColourToSdr(&pq, coded, out);
    CHECK_NEAR(out[0], 1.0, 2e-3);
    CHECK_NEAR(out[2], 1.0, 2e-3);

    // Greys stay neutral and monotonic.
    float last = -1.0f;
    for (int i = 1; i <= 100; i++) {
        grey(i * 10.0f, coded);
        videoColourToSdr(&pq, coded, out);
        CHECK(out[1] >= last);
        CHECK_NEAR(out[0], out[1], 2e-3);
        CHECK_NEAR(out[2], out[1], 2e-3);
        last = out[1];
    }

    // BT.2020 red is outside BT.709: must stay red and in range.
    coded[0] = videoNitsToPq(400.0f);
    coded[1] = 0.0f;
    coded[2] = 0.0f;
    videoColourToSdr(&pq, coded, out);
    for (int i = 0; i < 3; i++) {
        CHECK(out[i] >= 0.0f && out[i] <= 1.0f);
    }
    CHECK(out[0] > out[1] && out[0] > out[2]);

    // Driver-sampled path: same maths, only the sampling differs.
    VideoColour driver = videoColourFor(VIDEO_TRANSFER_PQ, 1000.0f, 0);
    float again[3];
    videoColourToSdr(&driver, coded, again);
    CHECK_NEAR(again[0], out[0], 1e-6);

    // HLG full signal is 1000 nits on the reference display, so SDR white.
    VideoColour hlg = videoColourFor(VIDEO_TRANSFER_HLG, 0.0f, 1);
    coded[0] = coded[1] = coded[2] = 1.0f;
    videoColourToSdr(&hlg, coded, out);
    CHECK_NEAR(out[1], 1.0, 2e-3);
    // Half signal is about 51 nits, under the knee.
    coded[0] = coded[1] = coded[2] = 0.5f;
    videoColourToSdr(&hlg, coded, out);
    float halfNits = 1000.0f * powf(1.0f / 12.0f, 0.2f) / 12.0f;
    CHECK_NEAR(out[1], powf(halfNits / 203.0f, 1.0f / 2.4f), 3e-3);

    // SDR passes through.
    VideoColour sdr = videoColourFor(VIDEO_TRANSFER_SDR, 0.0f, 1);
    coded[0] = 0.2f;
    coded[1] = 0.5f;
    coded[2] = 0.9f;
    videoColourToSdr(&sdr, coded, out);
    CHECK_NEAR(out[0], 0.2, 1e-7);
    CHECK_NEAR(out[2], 0.9, 1e-7);
}

static void testPeak(void) {
    CHECK_NEAR(videoPeakNits(0.0f, 0.0f), VIDEO_PEAK_DEFAULT_NITS, 1e-3);
    CHECK_NEAR(videoPeakNits(1500.0f, 4000.0f), 1500.0, 1e-3);
    // An implausibly low MaxCLL falls back to the mastering display peak.
    CHECK_NEAR(videoPeakNits(50.0f, 1000.0f), 1000.0, 1e-3);
    CHECK_NEAR(videoPeakNits(0.0f, 4000.0f), 4000.0, 1e-3);
    CHECK_NEAR(videoPeakNits(0.0f, 20000.0f), VIDEO_PEAK_MAX_NITS, 1e-3);
    CHECK_NEAR(videoPeakNits(20000.0f, 0.0f), VIDEO_PEAK_DEFAULT_NITS, 1e-3);

    VideoColour sdr = videoColourFor(VIDEO_TRANSFER_SDR, 500.0f, 1);
    CHECK(sdr.yuvTarget == 0 && sdr.peakNits == 0.0f);
    VideoColour hlg = videoColourFor(VIDEO_TRANSFER_HLG, 4000.0f, 1);
    CHECK_NEAR(hlg.peakNits, VIDEO_HLG_PEAK_NITS, 1e-3);
    VideoColour pq = videoColourFor(VIDEO_TRANSFER_PQ, 100.0f, 1);
    CHECK_NEAR(pq.peakNits, VIDEO_PEAK_MIN_NITS, 1e-3);
    VideoColour pq2 = videoColourFor(VIDEO_TRANSFER_PQ, 0.0f, 0);
    CHECK_NEAR(pq2.peakNits, VIDEO_PEAK_DEFAULT_NITS, 1e-3);
    CHECK(!videoColourSame(&pq, &pq2));
    VideoColour sdr2 = videoColourFor(VIDEO_TRANSFER_SDR, 0.0f, 0);
    CHECK(videoColourSame(&sdr, &sdr2));
}

static size_t count(const char* s, const char* what) {
    size_t n = 0;
    for (const char* p = strstr(s, what); p != NULL; p = strstr(p + 1, what)) {
        n++;
    }
    return n;
}

static void checkShader(const char* name, const char* src) {
    size_t reads = count(src, "texture(u_texture,");
    CHECK(reads > 0);
    if (reads == 0) {
        fprintf(stderr, "%s reads no video\n", name);
    }

    // SDR leaves the source unchanged.
    VideoColour sdr = videoColourFor(VIDEO_TRANSFER_SDR, 0.0f, 1);
    char* same = videoShaderSource(src, &sdr);
    CHECK(same != NULL && strcmp(same, src) == 0);
    free(same);

    // Raw HDR: YUV_target extension after #version, raw sampler, all reads via videoTexel.
    VideoColour raw = videoColourFor(VIDEO_TRANSFER_PQ, 1000.0f, 1);
    char* out = videoShaderSource(src, &raw);
    CHECK(out != NULL);
    if (out != NULL) {
        CHECK(strncmp(out, "#version 300 es\n#extension GL_EXT_YUV_target : require\n", 55) == 0);
        CHECK(count(out, "uniform highp __samplerExternal2DY2YEXT u_texture;") == 1);
        CHECK(count(out, "samplerExternalOES") == 0);
        // Only videoTexel's own read remains.
        CHECK(count(out, "texture(u_texture,") == 1);
        CHECK(count(out, "videoTexel(") == reads + 1);
        CHECK(count(out, "hdrPqToNits(coded)") == 1);
        free(out);
    }

    // Driver-sampled HDR: external sampler kept, no extension.
    VideoColour driver = videoColourFor(VIDEO_TRANSFER_HLG, 0.0f, 0);
    out = videoShaderSource(src, &driver);
    CHECK(out != NULL);
    if (out != NULL) {
        CHECK(strstr(out, "GL_EXT_YUV_target") == NULL);
        CHECK(count(out, "uniform samplerExternalOES u_texture;") == 1);
        CHECK(count(out, "texture(u_texture,") == 1);
        CHECK(count(out, "videoTexel(") == reads + 1);
        CHECK(count(out, "hdrHlgToNits(coded)") == 1);
        free(out);
    }
}

static void testShaders(void) {
    checkShader("picture", FRAGMENT_SRC);
    checkShader("depth upsample", UPSAMPLE_FRAGMENT_SRC);
    checkShader("depth model input", DOWNSCALE_FRAGMENT_SRC);
    checkShader("frame colour sample", AMBI_FRAGMENT_SRC);

    // Shaders that don't sample video are unchanged.
    VideoColour raw = videoColourFor(VIDEO_TRANSFER_PQ, 1000.0f, 1);
    char* glow = videoShaderSource(GLOW_FRAGMENT_SRC, &raw);
    CHECK(glow != NULL && strcmp(glow, GLOW_FRAGMENT_SRC) == 0);
    free(glow);
    CHECK(videoShaderSource(NULL, &raw) == NULL);
}

int main(void) {
    testPq();
    testEetf();
    testToSdr();
    testPeak();
    testShaders();
    return checksDone("videocolour");
}
