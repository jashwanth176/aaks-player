// Ambilight glow colour maths and cylinder geometry. The glow edge shader
// takes these constants as uniforms.

#ifndef XR_GLOW_H
#define XR_GLOW_H

// Frame sample size per side, and glow image size
#define AMBI_SAMPLE_TEX 32
#define GLOW_TEX 512
// Glow size relative to the screen per axis, and its offset toward the viewer
// in metres. Layer submit order keeps the picture on top.
#define GLOW_SCALE 1.7f
#define GLOW_PROUD_M 0.05f

// Each sample texel is scaled to this luma, so hue comes from the picture and
// brightness from the glow level
#define GLOW_LUMA_TARGET 0.5f
// Below the floor nothing is lifted (black stays dark); above FULL everything
// reaches the target. A soft knee avoids one-frame jumps on smoothed samples.
#define GLOW_LUMA_FLOOR 0.03f
#define GLOW_LUMA_FULL 0.08f
// Lit ramp on the brightest channel
#define GLOW_LIT_LO 0.08f
#define GLOW_LIT_HI 0.35f
// How far a lit colour carries into a dark run along an edge, as a fraction
// of the edge length. Weights fall as 1 - t^2.
#define GLOW_ROLLOFF_REACH (1.0f / 3.0f)
// How far inward a dark edge texel looks, fraction of picture size; skips
// thin dark borders (0.09 = two texels)
#define GLOW_INWARD_DEPTH 0.09f

// Ring of edge texels, anticlockwise from bottom left, AMBI_SAMPLE_TEX - 1 per side
#define GLOW_RING (4 * (AMBI_SAMPLE_TEX - 1))

// Max glow cylinder angle in radians; a cylinder layer cannot reach 2*pi
#define GLOW_MAX_ANGLE 6.0f

// Glow for a curved screen: a coaxial cylinder, since a flat quad would be
// hidden at the sides
typedef struct {
    // Slightly less than the screen radius, so it sits in front
    float radius;
    float centralAngle;
    // Arc length over height, as XrCompositionLayerCylinderKHR takes it
    float aspectRatio;
    // Glow image columns shown. All of them unless the glow would close the
    // circle; then the middle ones, so the picture stays centred.
    int rectX;
    int rectWidth;
} GlowCylinder;

// Returns 0 and writes nothing if no cylinder applies
int glowCylinderFor(float screenWidth, float screenHeight, float screenRadius,
                    GlowCylinder* out);

// Rec. 709 luma
float glowLuma(const float rgb[3]);

// Scales the colour toward GLOW_LUMA_TARGET through the knee, clamped to 0..1
void glowNormalise(const float in[3], float out[3]);

// 0 dark, 1 lit, smooth between
float glowLit(const float rgb[3]);

// GLOW_ROLLOFF_REACH in sample texels, and the inward look-up depth in texels
float glowReachTexels(void);
int glowInwardSteps(void);

// Weight of a colour k texels along the ring: 1 at k = 0, 0 at reach or beyond
float glowRolloffWeight(int k, float reach);

// Ring index to texel and back; -1 for a texel off the ring
void glowRingTexel(int i, int* x, int* y);
int glowRingIndex(int x, int y);

#endif
