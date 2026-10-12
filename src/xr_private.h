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

typedef struct AeronXr {
	SDL_GPUDevice* device;
	XrInstance     instance;
	XrSystemId     system_id;
	XrSession      session;
	XrSpace        local_space;
	XrSessionState state;
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
} AeronXr;

extern AeronXr g_xr;

const char* Aeron_XrResultName(XrResult result);
XrPath      Aeron_XrPath(const char* string);

/* xr_stereo.c */
int      Aeron_XrStereoInit(void);
void     Aeron_XrStereoShutdown(void);
uint32_t Aeron_XrStereoEndFrameLayers(const XrCompositionLayerBaseHeader** layers, uint32_t capacity);

/* xr_input.c */
int  Aeron_XrInputInit(void);
void Aeron_XrInputShutdown(void);

#endif
