#ifndef AERON_XR_INTERNAL_H
#define AERON_XR_INTERNAL_H

/*
 * OpenXR presentation (internal).
 *
 * Milestone 1 of the VR work: when XR is requested, Aeron keeps composing its
 * ordered layer stack exactly as it does for the desktop window, but into an
 * offscreen "virtual screen" texture. That texture is copied into an OpenXR
 * swapchain and submitted as a world-locked quad layer, and mirrored to the
 * desktop window. The headset compositor handles stereo for the quad, so no
 * renderer changes are needed for menus, cutscenes or the 2D frame of flight.
 *
 * Frame protocol (one OpenXR frame per Aeron host frame):
 *   Aeron_BeginFrame -> Aeron_XrWaitAndBeginFrame   (xrWaitFrame + xrBeginFrame,
 *                       replacing the swapchain throttle so input is sampled
 *                       after the headset wait, as on the desktop path)
 *   Aeron_Present    -> compose into Aeron_XrScreenTexture(), then
 *                       Aeron_XrCopyScreenToSwapchain() + Aeron_XrEndFrame()
 * A frame begun but never presented is ended with the previous screen image.
 *
 * Built only with AERON_HAS_OPENXR; every entry point is a safe no-op when XR
 * was not requested or failed to start (Aeron then behaves exactly as before).
 */

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decides whether XR was requested (config flag or AERON_XR=1). When it was,
 * adds the XR properties to a device-creation property set. Returns 1 when the
 * properties were added. */
int  Aeron_XrConfigureDeviceProperties(SDL_PropertiesID properties);
int  Aeron_XrRequested(void);
void Aeron_XrSetRequested(int requested);

/* Creates the session, reference space, the virtual-screen swapchain and its
 * intermediate texture. Returns 0 (and leaves XR inactive) on failure. */
int  Aeron_XrInit(SDL_GPUDevice* device, int screen_width, int screen_height,
				  SDL_GPUTextureFormat preferred_format);
void Aeron_XrShutdown(void);

/* Nonzero once a session exists (it may still be waiting for READY). */
/* Session exists and is FOCUSED (headset worn, no system overlay). */
int Aeron_XrFocused(void);
int Aeron_XrActive(void);
/* Nonzero while the session is running and frames are being submitted. */
int Aeron_XrSessionRunning(void);

/* Pumps XR events; returns 0 when the runtime asked the app to exit. */
int Aeron_XrPollEvents(void);

/* Returns 1 when an XR frame was begun (the caller must later present or the
 * next call ends it), 0 when the session is not running (caller should use
 * normal window pacing), -1 on a fatal XR error. */
int Aeron_XrWaitAndBeginFrame(void);
int Aeron_XrFrameBegun(void);
/* Whether the runtime wants content for the current frame. */
int Aeron_XrShouldRender(void);

SDL_GPUTexture*      Aeron_XrScreenTexture(void);
SDL_GPUTextureFormat Aeron_XrScreenFormat(void);
void                 Aeron_XrScreenSize(int* width, int* height);

/* Records the screen -> swapchain image copy (acquiring the image). */
int Aeron_XrCopyScreenToSwapchain(SDL_GPUCommandBuffer* command_buffer);
/* Optional capture of the virtual screen for headless verification. Call
 * after recording the composition and before submission. Returns 1 when a
 * capture was recorded; the caller must then submit with a fence and pass it
 * to Aeron_XrEndFrame. */
int Aeron_XrRecordCapture(SDL_GPUCommandBuffer* command_buffer);
/* After the command buffer was submitted: releases the image and calls
 * xrEndFrame with the quad layer. */
int Aeron_XrEndFrame(SDL_GPUFence* submitted_fence);

/* Stereo (xr_stereo.c): set by the game through aeron/xr.h. */
int             Aeron_XrStereoCommitted(void);
/* Committed with Aeron_XrStereoCommitBackdrop: the virtual screen is
 * composed as usual and shown over the eyes. */
int             Aeron_XrStereoScreenOverlay(void);
int             Aeron_XrStereoCopyToSwapchains(SDL_GPUCommandBuffer* command_buffer);
SDL_GPUTexture* Aeron_XrStereoTexture(int slot); /* 0 left, 1 right, 2 HUD */
int             Aeron_XrStereoHudVisible(void);
void            Aeron_XrStereoSize(int* width, int* height);

/* Controller input (xr_input.c). The virtual Touch gamepad uses this id. */
#define AERON_XR_CONTROLLER_INSTANCE_ID 0x58520001u
struct AeronInputSnapshot;
void Aeron_XrUpdateInput(struct AeronInputSnapshot* input, float dt_seconds);
int  Aeron_XrIsControllerId(uint32_t instance_id);
int  Aeron_XrRumble(uint16_t low, uint16_t high, uint32_t duration_ms);

/* Predicted display period of the headset in Hz (0 when unknown). */
double Aeron_XrDisplayRefreshRate(void);

#ifdef __cplusplus
}
#endif

#endif
