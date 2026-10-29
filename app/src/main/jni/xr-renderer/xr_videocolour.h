// HDR to SDR conversion for video sampling. SDR uses the driver's conversion
// as is. For HDR (PQ or HLG, BT.2020) the driver returns the coded signal,
// which looks flat and dim, so every shader that reads the video is rewritten
// to sample through videoTexel:
//
//   BT.2020 limited-range YUV to RGB (raw via GL_EXT_YUV_target if available)
//   PQ (or HLG) to nits
//   BT.2390 EETF on luminance in PQ, with HDR reference white (203 nits) as
//   SDR white
//   BT.2020 to BT.709; out-of-gamut colours desaturated toward grey at
//   constant luminance rather than clipped per channel
//   gamma 2.4 encode
//
// Everything downstream (grade, glow, ambient light, depth input) sees SDR.
// videoColourToSdr is the CPU equivalent of the GLSL.

#ifndef XR_VIDEOCOLOUR_H
#define XR_VIDEOCOLOUR_H

#define VIDEO_TRANSFER_SDR 0
#define VIDEO_TRANSFER_PQ 1
#define VIDEO_TRANSFER_HLG 2

// HDR reference white, mapped to SDR white (ITU-R BT.2408)
#define VIDEO_REFERENCE_NITS 203.0f
// Peak assumed when metadata is missing or implausible
#define VIDEO_PEAK_DEFAULT_NITS 1000.0f
#define VIDEO_PEAK_MIN_NITS 400.0f
#define VIDEO_PEAK_MAX_NITS 10000.0f
// HLG reference display peak
#define VIDEO_HLG_PEAK_NITS 1000.0f

typedef struct {
    // VIDEO_TRANSFER_
    int transfer;
    // Mastering peak, nits
    float peakNits;
    // 1: sample raw YUV and convert in the shader; 0: treat the driver's RGB
    // as the coded BT.2020 signal
    int yuvTarget;
} VideoColour;

// Tone map source peak: MaxCLL if plausible, else the mastering display
// peak, else the default. 0 means not present.
float videoPeakNits(float maxCll, float masteringNits);

// Normalised settings: SDR has no peak, HLG uses the standard's, PQ is clamped
VideoColour videoColourFor(int transfer, float peakNits, int yuvTarget);

int videoColourSame(const VideoColour* a, const VideoColour* b);

// SMPTE ST 2084: code value 0..1 <-> nits
float videoPqToNits(float e);
float videoNitsToPq(float nits);

// BT.2390 EETF on a PQ-coded luminance, srcPeakNits to dstPeakNits
float videoEetf(float pq, float srcPeakNits, float dstPeakNits);

// Coded BT.2020 RGB (PQ or HLG, 0..1) to gamma-coded BT.709. SDR passes through.
void videoColourToSdr(const VideoColour* vc, const float coded[3], float out[3]);

// Returns a copy of src with u_texture reads routed through videoTexel
// (unchanged for SDR). Caller frees; NULL on allocation failure or an
// unexpected source layout.
char* videoShaderSource(const char* src, const VideoColour* vc);

const char* videoTransferName(int transfer);

#endif
