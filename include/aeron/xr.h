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
	AeronRenderTarget* hud_target;     /* hud_width x hud_height; clear to transparent, draw HUD */
	int                hud_width;      /* HUD targets' size: the eye's aspect, at most the eye size */
	int                hud_height;
	/* Second HUD layer for elements that belong to the world (reticle,
	 * target brackets), shown behind hud_target. Same size and use. */
	AeronRenderTarget* hud_world_target;
	/* Second head-HUD target (same size), for panels that must not share
	 * pixels with the first, e.g. HUD parts that overlap on the flat
	 * screen. NULL when unavailable. */
	AeronRenderTarget* hud_aux_target;
	/* Console layer (a touch panel the game places near the player, e.g. on
	 * the lap): its own target of console_width x console_height, NULL when
	 * unavailable. Clear to transparent and draw; shown only when committed
	 * with Aeron_XrStereoCommitConsole. */
	AeronRenderTarget* console_target; /* NULL: use Aeron_XrStereoPanelTarget */
	int                console_width;
	int                console_height;
	/* Card layer: a second panel of the same kind (e.g. a reference card
	 * the player can pick up), card_width x card_height, NULL when
	 * unavailable. Shown when committed with Aeron_XrStereoCommitCard. */
	AeronRenderTarget* card_target; /* NULL: use Aeron_XrStereoPanelTarget */
	int                card_width;
	int                card_height;
	/* Beam layer: a small target (beam_width x beam_height) for pointer
	 * beams; parts of it are shown as up to two quads on top of all other
	 * layers with Aeron_XrStereoCommitBeams. NULL when unavailable. */
	AeronRenderTarget* beam_target;
	int                beam_width;
	int                beam_height;
} AeronXrStereoFrame;

/* Quad that shows a HUD target over the 3D view.
 *
 * World-locked (head_locked = 0): `position` is the centre in LOCAL space,
 * the quad faces +Z.
 *
 * Head-locked (head_locked = 1): `position` is relative to the head (e.g.
 * {0, 0, -3} = 3 m straight ahead) and the quad turns with the head, like
 * a helmet visor display. With follow_s > 0 it trails the head smoothly
 * (exponential ease with that time constant, in seconds) instead of being
 * rigidly attached. */
typedef struct AeronXrHudQuad {
	int   visible;
	float position[3]; /* centre, metres */
	float size[2];     /* width, height in metres */
	int   head_locked;
	float follow_s;
} AeronXrHudQuad;

/* A rectangle of hud_target shown as its own quad in LOCAL space, e.g. one
 * HUD instrument placed in the cockpit. The quad faces +Z of its
 * orientation. */
typedef struct AeronXrPanel {
	int   rect[4];        /* x, y, width, height in hud_target pixels */
	float position[3];    /* centre, metres, LOCAL space */
	float orientation[4]; /* quaternion x, y, z, w */
	float size[2];        /* width, height in metres */
	int   source;         /* 0 = hud_target, 1 = hud_aux_target */
	int   eye;            /* 0 both eyes, 1 left only, 2 right only */
} AeronXrPanel;

/* Panel layers with their own swapchains (console, card, second card).
 * They keep showing the last image drawn into them: the game only draws
 * when something changed (Aeron_XrStereoPanelTarget acquires a fresh image
 * for this frame), and commits the quads every frame it wants them shown.
 * Each target is two panels wide, so a panel can be drawn once per eye
 * (left half, right half) when it differs between the eyes. */
/* AERON_XR_PANEL_FRAME: a static picture composited right over the
 * virtual screen (e.g. a bezel around it), AERON_XR_FRAME_WIDTH x
 * AERON_XR_FRAME_HEIGHT, drawn once and kept. */
enum {
	AERON_XR_PANEL_CONSOLE = 0,
	AERON_XR_PANEL_CARD = 1,
	AERON_XR_PANEL_CARD2 = 2,
	AERON_XR_PANEL_FRAME = 3,
	AERON_XR_PANEL_COUNT = 4
};
#define AERON_XR_FRAME_WIDTH 1280
#define AERON_XR_FRAME_HEIGHT 1104

#define AERON_XR_MAX_PANELS 12

/* Recentre the view on the head's current position and heading at the
 * next frame (done automatically once at start). */
void Aeron_XrRequestRecenter(void);

/* The world-locked virtual screen in the LOCAL space: distance ahead,
 * width and height (the quad, its pixel aspect) and the height offset, in
 * metres. Returns 0 while there is no XR screen. */
int Aeron_XrVirtualScreenGeometry(float* distance_m, float* width_m, float* height_m, float* height_offset_m);

/* Returns 1 and fills `out` when this host frame can be rendered in stereo
 * (session focused/visible, frame begun, runtime wants content, views
 * tracked). Safe to call more than once per frame. */
int Aeron_XrStereoBegin(AeronXrStereoFrame* out);

/* Marks this frame as stereo: Aeron_Present submits the eye targets (and the
 * HUD quad when `hud` is non-NULL and visible). Call after recording the eye
 * renders into a command buffer that is submitted before Aeron_Present. */
void Aeron_XrStereoCommit(const AeronXrHudQuad* hud);
/* As Aeron_XrStereoCommit, with an optional second quad for
 * hud_world_target (drawn behind `hud`). Either may be NULL. */
void Aeron_XrStereoCommitHuds(const AeronXrHudQuad* hud, const AeronXrHudQuad* hud_world);
/* As Aeron_XrStereoCommitHuds with hud = NULL: instead of one HUD quad,
 * `count` panels each show a part of hud_target. Panels beyond what the
 * runtime's layer limit allows are dropped (last first). */
void Aeron_XrStereoCommitPanels(const AeronXrHudQuad* hud_world, const AeronXrPanel* panels, int count);
/* After one of the commits above: also show `console` (rect in
 * console_target pixels; all-zero rect = the whole target), on top of the
 * other layers. */
void Aeron_XrStereoCommitConsole(const AeronXrPanel* console);
/* The same for the card target (all-zero rect = the whole target); drawn
 * under the console. */
void Aeron_XrStereoCommitCard(const AeronXrPanel* card);
/* This frame's image of panel layer `which` to draw into (all of it: the
 * image's earlier content is undefined), and its size. NULL when the layer
 * is unavailable. Call only when the panel changed; without a call the
 * layer keeps its last image. */
AeronRenderTarget* Aeron_XrStereoPanelTarget(int which, int* width, int* height);
/* After one of the HUD commits: show up to two quads of panel layer
 * `which` (rects in its target's pixels; panel.eye picks the eye). Cards
 * are drawn under the console, the console under the beams. */
void Aeron_XrStereoCommitPanelLayer(int which, const AeronXrPanel* quads, int count);
/* After the HUD commit (or the backdrop): show the eye images a second
 * time right over the frame panel layer, blended by their alpha
 * (unpremultiplied). The game sets alpha to 1 where something must appear
 * in front of the screen and its frame (the hands) and 0 elsewhere. */
void Aeron_XrStereoCommitEyeOverlay(void);
/* Up to two pointer beams (rects in beam_target pixels), drawn last. */
void Aeron_XrStereoCommitBeams(const AeronXrPanel* beams, int count);
/* The eye targets are a 3D environment around the flat virtual screen:
 * Aeron_Present submits the eyes as the projection layer and the composed
 * 2D layers on the virtual screen quad on top (menus in a VR room). The
 * mouse pointer keeps working on the screen. */
void Aeron_XrStereoCommitBackdrop(void);

#ifdef __cplusplus
}
#endif

#endif
