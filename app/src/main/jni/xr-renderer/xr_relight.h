// Passthrough relight: the real room is dimmed and tinted by the picture's
// average colour, scaled by one brightness setting. The ambient light switch
// fades the glow around the screen. No GL.

#ifndef XR_RELIGHT_H
#define XR_RELIGHT_H

// Default brightness, 0..1
#define RELIGHT_DEFAULT_BRIGHTNESS 0.4f
// Per-frame smoothing: fraction of the previous room colour kept
#define RELIGHT_KEEP 0.9f
// Per-frame return to neutral when there is no picture
#define RELIGHT_NEUTRAL_RETURN 0.01f
// Ambient light fade rate, per second
#define RELIGHT_AMBIENT_RATE 3.0f
// Glow size relative to the screen, per axis
#define RELIGHT_GLOW_SCALE 5.0f
// Edge feather width and corner radius, in screen heights (0.084 m and
// 0.08 m on a 1.08 m high screen)
#define RELIGHT_FEATHER (0.084f / 1.08f)
#define RELIGHT_CORNER (0.08f / 1.08f)
// Default stereo crop: fraction of width each eye's picture is shifted
// outward, placing the scene behind the screen plane
#define RELIGHT_CROP_DEFAULT 0.015f

// Room colour multiplier from the picture's average colour and brightness, 0..1
void relightScaleFor(const float avg[3], float brightness, float out[3]);

// Glow strength: 0 at brightness 0, about 0.8 at the default, 1 at 1
float relightGlowStrength(float brightness);

// One frame of the room colour following its target
void relightFollow(float current[3], const float target[3], float keep);

// Eases the ambient light toward its target over dt seconds
float relightEaseAmbient(float current, float target, float dt);

#endif
