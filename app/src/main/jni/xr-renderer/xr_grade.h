// Picture grade: brightness, contrast, gamma and saturation. No GL; the
// shaders apply the same maths from the four PictureGrade values.

#ifndef XR_GRADE_H
#define XR_GRADE_H

#include "xr_shared.h"

typedef struct {
    // Added after contrast, full scale
    float offset;
    // Gain about mid grey
    float contrast;
    // 1 / gamma, so gamma > 1 lifts the mid tones
    float exponent;
    // 0 grey, 1 unchanged, 2 double
    float saturation;
} PictureGrade;

// Range and default per row, in the preferences' integer units, PICTURE_
// order. Out-of-range rows read as brightness.
int pictureMin(int row);
int pictureMax(int row);
int pictureDefault(int row);

int pictureClamp(int row, int units);

// 1 when every row is at its default (no grade)
int pictureNeutral(const int units[PICTURE_VALUES]);

// Grade for four values in integer units, each clamped first
PictureGrade pictureGradeFor(const int units[PICTURE_VALUES]);

// Clamp to 0..1, contrast about mid grey plus brightness, clamp, gamma,
// saturation against Rec. 709 luma, clamp
void pictureGradeApply(const PictureGrade* grade, const float in[3], float out[3]);

#endif
