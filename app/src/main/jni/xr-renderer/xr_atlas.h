// Parser for the .atlas files baked room textures ship in: pre-encoded ASTC
// blocks for every mip level, written by tools/atlas_astc.py. No GL here.
//
// Layout, little endian:
//   char[4]  magic 'MXA1'
//   uint32   width of level 0
//   uint32   height of level 0
//   uint32   block width
//   uint32   block height
//   uint32   level count
//   per level, largest first: uint32 byte length, then that many bytes of
//            ASTC blocks, 16 bytes a block, rows from the top of the picture
//
// Level i is max(1, width >> i) by max(1, height >> i) and its length is
// ceil(w / block width) * ceil(h / block height) * 16.

#ifndef XR_ATLAS_H
#define XR_ATLAS_H

#include <stddef.h>
#include <stdint.h>

#define ATLAS_HEADER_BYTES 24
#define ATLAS_BLOCK_BYTES 16
// Small enough that none of the 32-bit size arithmetic can overflow
#define ATLAS_SIZE_MAX 16384
// A full chain for ATLAS_SIZE_MAX, 16384 down to 1
#define ATLAS_LEVELS_MAX 15

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t blockWidth;
    uint32_t blockHeight;
    int levels;
    // Byte offset and length of each level's blocks in the file
    size_t offsets[ATLAS_LEVELS_MAX];
    uint32_t lengths[ATLAS_LEVELS_MAX];
} AtlasInfo;

// Validates the header and every level against the file size. Returns the
// level count, or 0 for a bad magic, an unsupported block size (4x4, 6x6 and
// 8x8 only), an out-of-range size, too many levels, a level length mismatch,
// or trailing bytes.
int atlasParse(const unsigned char* data, size_t size, AtlasInfo* out);

// Width or height of a mip level
uint32_t atlasLevelSize(uint32_t size, int level);

// Byte size of a w by h level in bw by bh blocks
uint32_t atlasLevelBytes(uint32_t w, uint32_t h, uint32_t bw, uint32_t bh);

#endif
