#ifndef AERON_XR_H
#define AERON_XR_H

/*
 * Aeron OpenXR stereo API (experimental).
 *
 * A game that can render its 3D scene per eye asks for this frame's stereo
 * views, renders each eye into the provided targets, optionally draws a 2D
 * HUD into the HUD target, and commits. Aeron_Present then submits a
 * projection layer (+ HUD quad layer) instead of the flat virtual screen,
 * while the desktop window keeps showing the ordinary composed layers.
 * Frames that do not commit fall back to the virtual screen automatically.
 *
 * Coordinates: views are poses in the OpenXR LOCAL space (metres; +X right,
 * +Y up, -Z forward; origin = the seated head position at session start or
 * the last recenter).
 */

#include "aeron/render.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronXrView {
	float orientation[4]; /* quaternion x, y, z, w */
	float position[3];    /* metres */
	float fov[4];         /* angle left, right, up, down in radians (left/down negative) */
} AeronXrView;

typedef struct AeronXrStereoFrame {
	AeronXrView        views[2]; /* 0 = left eye, 1 = right eye */
	int                width;    /* per-eye render size */
	int                height;
	AeronTextureFormat format;         /* eye/HUD target format (sRGB or float) */
	AeronRenderTarget* eye_targets[2]; /* write final display-ready colour here */
	AeronRenderTarget* hud_target;     /* same size; clear to transparent, draw HUD */
} AeronXrStereoFrame;

/* World-locked quad (LOCAL space) that shows hud_target over the 3D view. */
typedef struct AeronXrHudQuad {
	int   visible;
	float position[3]; /* centre, metres */
	float size[2];     /* width, height in metres */
} AeronXrHudQuad;

/* Returns 1 and fills `out` when this host frame can be rendered in stereo
 * (session focused/visible, frame begun, runtime wants content, views
 * tracked). Safe to call more than once per frame. */
int Aeron_XrStereoBegin(AeronXrStereoFrame* out);

/* Marks this frame as stereo: Aeron_Present submits the eye targets (and the
 * HUD quad when `hud` is non-NULL and visible). Call after recording the eye
 * renders into a command buffer that is submitted before Aeron_Present. */
void Aeron_XrStereoCommit(const AeronXrHudQuad* hud);

#ifdef __cplusplus
}
#endif

#endif
