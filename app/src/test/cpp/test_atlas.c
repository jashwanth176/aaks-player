// Tests for the .atlas parser.
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "xr_atlas.h"


static void putU32(unsigned char* p, uint32_t v) {
    memcpy(p, &v, sizeof(v));
}

// Builds an atlas in the tools/atlas_astc.py layout with filler block data.
// Returns its size; the caller frees *out.
static size_t makeAtlas(uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, uint32_t levels,
                        unsigned char** out) {
    size_t size = ATLAS_HEADER_BYTES;
    for (uint32_t i = 0; i < levels; i++) {
        size += 4 + atlasLevelBytes(atlasLevelSize(w, (int)i), atlasLevelSize(h, (int)i),
                                    bw, bh);
    }
    unsigned char* data = malloc(size);
    memcpy(data, "MXA1", 4);
    putU32(data + 4, w);
    putU32(data + 8, h);
    putU32(data + 12, bw);
    putU32(data + 16, bh);
    putU32(data + 20, levels);
    size_t at = ATLAS_HEADER_BYTES;
    for (uint32_t i = 0; i < levels; i++) {
        uint32_t length = atlasLevelBytes(atlasLevelSize(w, (int)i),
                                          atlasLevelSize(h, (int)i), bw, bh);
        putU32(data + at, length);
        memset(data + at + 4, (int)(0x40 + i), length);
        at += 4 + length;
    }
    *out = data;
    return size;
}

static void testLevelMaths(void) {
    CHECK(atlasLevelSize(4096, 0) == 4096);
    CHECK(atlasLevelSize(4096, 12) == 1);
    CHECK(atlasLevelSize(64, 3) == 8);
    // Clamps at 1 past the end of the chain.
    CHECK(atlasLevelSize(32, 9) == 1);
    CHECK(atlasLevelSize(37, 1) == 18);
    // Partial blocks round up.
    CHECK(atlasLevelBytes(4096, 4096, 6, 6) == 683u * 683u * 16u);
    CHECK(atlasLevelBytes(1, 1, 6, 6) == 16);
    CHECK(atlasLevelBytes(37, 19, 4, 4) == 10u * 5u * 16u);
}

static void testWellFormed(void) {
    unsigned char* data = NULL;
    AtlasInfo info;

    // Full 6x6 chain down to 1x1.
    size_t size = makeAtlas(64, 64, 6, 6, 7, &data);
    CHECK(atlasParse(data, size, &info) == 7);
    CHECK(info.width == 64 && info.height == 64);
    CHECK(info.blockWidth == 6 && info.blockHeight == 6);
    CHECK(info.levels == 7);
    CHECK(info.offsets[0] == ATLAS_HEADER_BYTES + 4);
    CHECK(info.lengths[0] == 11u * 11u * 16u);
    CHECK(info.lengths[6] == 16);
    // Each level follows the previous one plus its 4-byte length.
    int contiguous = 1;
    for (int i = 1; i < info.levels; i++) {
        contiguous &= info.offsets[i] == info.offsets[i - 1] + info.lengths[i - 1] + 4;
    }
    CHECK(contiguous);
    // Filler byte is 0x40 + level.
    CHECK(data[info.offsets[0]] == 0x40);
    CHECK(data[info.offsets[3]] == 0x43);
    CHECK(data[info.offsets[6] + 15] == 0x46);
    free(data);

    // Non-square: level count follows the longer side.
    size = makeAtlas(128, 32, 8, 8, 8, &data);
    CHECK(atlasParse(data, size, &info) == 8);
    CHECK(info.lengths[7] == 16);
    free(data);

    // A truncated chain is still valid for GL.
    size = makeAtlas(256, 256, 4, 4, 3, &data);
    CHECK(atlasParse(data, size, &info) == 3);
    free(data);
}

static void testRefused(void) {
    unsigned char* data = NULL;
    AtlasInfo info;
    size_t size = makeAtlas(64, 64, 6, 6, 7, &data);

    CHECK(atlasParse(NULL, size, &info) == 0);
    CHECK(atlasParse(data, ATLAS_HEADER_BYTES - 1, &info) == 0);

    // Bad magic, including the room model's.
    memcpy(data, "MXA2", 4);
    CHECK(atlasParse(data, size, &info) == 0);
    memcpy(data, "MXR1", 4);
    CHECK(atlasParse(data, size, &info) == 0);
    CHECK(info.levels == 0);
    memcpy(data, "MXA1", 4);
    CHECK(atlasParse(data, size, &info) == 7);

    // Truncated in a level, truncated in a length, trailing bytes.
    CHECK(atlasParse(data, size - 1, &info) == 0);
    CHECK(atlasParse(data, ATLAS_HEADER_BYTES + 2, &info) == 0);
    unsigned char* longer = malloc(size + 1);
    memcpy(longer, data, size);
    longer[size] = 0;
    CHECK(atlasParse(longer, size + 1, &info) == 0);
    free(longer);

    // Level length disagrees with its size.
    putU32(data + ATLAS_HEADER_BYTES, 11u * 11u * 16u + 16u);
    CHECK(atlasParse(data, size, &info) == 0);
    putU32(data + ATLAS_HEADER_BYTES, 11u * 11u * 16u);
    CHECK(atlasParse(data, size, &info) == 7);

    // Level count: too many for the chain, zero, over the limit.
    putU32(data + 20, 8);
    CHECK(atlasParse(data, size, &info) == 0);
    putU32(data + 20, 0);
    CHECK(atlasParse(data, size, &info) == 0);
    putU32(data + 20, ATLAS_LEVELS_MAX + 1);
    CHECK(atlasParse(data, size, &info) == 0);
    putU32(data + 20, 7);

    // Unsupported block sizes.
    putU32(data + 12, 5);
    putU32(data + 16, 5);
    CHECK(atlasParse(data, size, &info) == 0);
    putU32(data + 12, 6);
    putU32(data + 16, 8);
    CHECK(atlasParse(data, size, &info) == 0);
    putU32(data + 12, 0);
    putU32(data + 16, 0);
    CHECK(atlasParse(data, size, &info) == 0);
    putU32(data + 12, 6);
    putU32(data + 16, 6);

    // Width zero or over ATLAS_SIZE_MAX.
    putU32(data + 4, 0);
    CHECK(atlasParse(data, size, &info) == 0);
    putU32(data + 4, ATLAS_SIZE_MAX + 1);
    CHECK(atlasParse(data, size, &info) == 0);
    putU32(data + 4, 0xFFFFFFFFu);
    CHECK(atlasParse(data, size, &info) == 0);
    putU32(data + 4, 64);
    CHECK(atlasParse(data, size, &info) == 7);
    free(data);
}

int main(void) {
    testLevelMaths();
    testWellFormed();
    testRefused();
    return checksDone("xr_atlas");
}
