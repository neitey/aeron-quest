#ifndef AERON_XR_INTERNAL_PRIVATE_H
#define AERON_XR_INTERNAL_PRIVATE_H

/* Shared state of the OpenXR modules (xr.c, xr_input.c, xr_stereo.c).
 * Only included when AERON_HAS_OPENXR is defined, after <openxr/openxr.h>. */

#include <openxr/openxr.h>

#include "xr.h"

#include <SDL3/SDL_openxr.h>

/* OpenXR structs are conventionally initialized as { XR_TYPE_..., NULL } with
 * the remaining fields zeroed; that idiom is intentional here. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#define AERON_XR_MAX_IMAGES 8

/* Layers submitted per frame at most (projection, HUD quads, panels). */
#define AERON_XR_MAX_LAYERS 32 /* used up to the runtime's maxLayerCount */

typedef struct AeronXr {
	SDL_GPUDevice* device;
	XrInstance     instance;
	XrSystemId     system_id;
	XrSession      session;
	XrSpace        local_space;
	/* LOCAL recentred on the head: local_space is LOCAL offset by
	 * local_pose (yaw + position of the head when recentred). */
	XrSpace        view_space;
	/* LOCAL as it was at the start, kept to check where the runtime puts a
	 * new LOCAL space (the Steam Frame puts it at the head's heading). */
	XrSpace        base_space;
	XrPosef        local_pose;
	int            recenter_pending;
	int            recenter_frames;
	XrSessionState state;
	uint32_t       max_layers; /* composition layers the runtime accepts per frame */
	int            session_running;
	int            exit_requested;

	/* Virtual screen: intermediate composition target + XR swapchain. */
	SDL_GPUTexture*      screen_texture;
	SDL_GPUTextureFormat screen_format;
	int                  screen_width;
	int                  screen_height;
	XrSwapchain          screen_swapchain;
	SDL_GPUTexture**     screen_images;
	int                  screen_has_released_image;
	/* Right-eye virtual screen, created on first use: when a frame's 2D
	 * layers differ per eye (Aeron texture layers with a right_eye_texture),
	 * the screen is composed twice and shown as one quad per eye. */
	SDL_GPUTexture*  screen_right_texture;
	XrSwapchain      screen_right_swapchain;
	SDL_GPUTexture** screen_right_images;
	int              screen_right_failed;
	int              screen_right_acquired;
	int              screen_right_has_released_image;
	int              screen_right_this_frame;
	int              screen_stereo_last; /* the last composed frame showed each eye its own image */
	float                screen_distance_m;
	float                screen_width_m;
	float                screen_height_offset_m;

	/* Current frame. */
	int        frame_begun;
	int        image_acquired;
	XrTime     predicted_display_time;
	XrDuration predicted_display_period;
	XrBool32   should_render;
	uint64_t   frame_index;
	/* Performance stats (logged every ~5 s): CPU time from xrWaitFrame return
	 * to xrEndFrame, frame pacing from predicted display times. */
	uint64_t perf_frame_start_ns;
	uint64_t perf_window_start_ns;
	uint32_t perf_frames;
	uint32_t perf_stereo_frames;
	uint32_t perf_late;
	double   perf_cpu_ms_sum;
	double   perf_cpu_ms_max;
	XrTime   perf_last_display_time;

	/* Optional headless capture (AERON_XR_CAPTURE="60,300" + _DIR). */
	SDL_GPUTransferBuffer* capture_buffer;
	int                    capture_pending;
	uint64_t               capture_frames[16];
	int                    capture_frame_count;
	char                   capture_dir[512];

	PFN_xrGetInstanceProcAddr   xrGetInstanceProcAddr;
	PFN_xrPollEvent             xrPollEvent;
	PFN_xrBeginSession          xrBeginSession;
	PFN_xrEndSession            xrEndSession;
	PFN_xrDestroySession        xrDestroySession;
	PFN_xrDestroyInstance       xrDestroyInstance;
	PFN_xrCreateReferenceSpace  xrCreateReferenceSpace;
	PFN_xrDestroySpace          xrDestroySpace;
	PFN_xrWaitFrame             xrWaitFrame;
	PFN_xrBeginFrame            xrBeginFrame;
	PFN_xrEndFrame              xrEndFrame;
	PFN_xrAcquireSwapchainImage xrAcquireSwapchainImage;
	PFN_xrEnumerateSwapchainImages xrEnumerateSwapchainImages;
	PFN_xrWaitSwapchainImage    xrWaitSwapchainImage;
	PFN_xrReleaseSwapchainImage xrReleaseSwapchainImage;
	PFN_xrGetInstanceProperties xrGetInstanceProperties;
	PFN_xrGetSystemProperties   xrGetSystemProperties;
	PFN_xrResultToString        xrResultToString;
	/* Input (xr_input.c) */
	PFN_xrStringToPath                      xrStringToPath;
	PFN_xrCreateActionSet                   xrCreateActionSet;
	PFN_xrDestroyActionSet                  xrDestroyActionSet;
	PFN_xrCreateAction                      xrCreateAction;
	PFN_xrSuggestInteractionProfileBindings xrSuggestInteractionProfileBindings;
	PFN_xrAttachSessionActionSets           xrAttachSessionActionSets;
	PFN_xrSyncActions                       xrSyncActions;
	PFN_xrGetActionStateFloat               xrGetActionStateFloat;
	PFN_xrGetActionStateBoolean             xrGetActionStateBoolean;
	PFN_xrGetActionStateVector2f            xrGetActionStateVector2f;
	PFN_xrGetActionStatePose                xrGetActionStatePose;
	PFN_xrCreateActionSpace                 xrCreateActionSpace;
	PFN_xrLocateSpace                       xrLocateSpace;
	PFN_xrApplyHapticFeedback               xrApplyHapticFeedback;
	PFN_xrStopHapticFeedback                xrStopHapticFeedback;
	/* Stereo (xr_stereo.c) */
	PFN_xrEnumerateViewConfigurationViews xrEnumerateViewConfigurationViews;
	PFN_xrLocateViews                     xrLocateViews;

	/* Optional extensions, enabled only when the runtime offers them. */
	const char* extensions[16];
	uint32_t    extension_count;
	int         has_perf_settings;   /* XR_EXT_performance_settings */
	int         has_refresh_rate;    /* XR_FB_display_refresh_rate */
	/* Controller profiles beyond Touch / Index (Pico, HTC). */
	int         has_bd_controller;   /* XR_BD_controller_interaction: Pico Neo 3, Pico 4 */
	int         has_bd_ultra;        /* XR_BD_ultra_controller_interaction: Pico 4 Ultra */
	int         has_htc_focus3;      /* XR_HTC_vive_focus3_controller_interaction */
	/* Controller models from the runtime (xr_render_model.c). */
	int         has_fb_render_model;           /* XR_FB_render_model (Quest) */
	int         has_ext_render_model;          /* XR_EXT_render_model */
	int         has_ext_interaction_render_model; /* XR_EXT_interaction_render_model */
	PFN_xrPerfSettingsSetPerformanceLevelEXT xrPerfSettingsSetPerformanceLevelEXT;
	PFN_xrEnumerateDisplayRefreshRatesFB     xrEnumerateDisplayRefreshRatesFB;
	PFN_xrRequestDisplayRefreshRateFB        xrRequestDisplayRefreshRateFB;
	/* Core, only used for the log (may be missing). */
	PFN_xrGetCurrentInteractionProfile xrGetCurrentInteractionProfile;
	PFN_xrPathToString                 xrPathToString;
} AeronXr;

extern AeronXr g_xr;

const char* Aeron_XrResultName(XrResult result);
XrPath      Aeron_XrPath(const char* string);

/* xr_stereo.c */
int      Aeron_XrStereoInit(void);
void     Aeron_XrStereoShutdown(void);
/* `screens` (screen_count may be 0): the virtual screen quad of a backdrop
 * frame (one per eye when the screen is stereo), placed right above the
 * projection layer (under any panels). */
uint32_t Aeron_XrStereoEndFrameLayers(const XrCompositionLayerBaseHeader** layers, uint32_t capacity,
									  const XrCompositionLayerBaseHeader* const* screens, int screen_count);

/* xr_render_model.c: controller models from the runtime. Update after
 * each xrSyncActions (fetches what is missing or changed), Dirty on an
 * interaction profile / render model change event, Shutdown before the
 * session goes. */
void Aeron_XrRenderModelsUpdate(void);
void Aeron_XrRenderModelsDirty(void);
void Aeron_XrRenderModelsShutdown(void);

/* xr_input.c */
int  Aeron_XrInputInit(void);
/* Hand 0/1's grip pose this frame (LOCAL). Returns 0 when not tracked. */
int  Aeron_XrInputGripPose(int hand, XrPosef* out);
void Aeron_XrInputShutdown(void);
/* Logs the controller profile the runtime picked for each hand (after an
 * interaction profile change event). */
void Aeron_XrInputLogProfiles(void);

#endif
