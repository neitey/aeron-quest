/*
 * OpenXR virtual-screen presentation for Aeron. See xr.h for the frame
 * protocol. Everything here is gated on AERON_HAS_OPENXR; without it the
 * functions compile to inert stubs so callers need no #ifdefs.
 */

#if defined(AERON_HAS_OPENXR)
/* Must precede SDL.h (via xr.h): SDL_openxr.h then uses the real OpenXR
 * types instead of its own placeholder typedefs. */
#include <openxr/openxr.h>
#endif

#include "xr.h"

#include "aeron/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -1 = undecided (read AERON_XR), 0 = off, 1 = on. Once XR setup fails it
 * is switched off for the rest of the run so the desktop path is used. */
static int g_xr_requested = -1;

void Aeron_XrSetRequested(int requested) { g_xr_requested = requested ? 1 : 0; }

int Aeron_XrRequested(void) {
	if (g_xr_requested < 0) {
		const char* env = SDL_getenv("AERON_XR");
		g_xr_requested  = env && env[0] && strcmp(env, "0") != 0;
	}
	return g_xr_requested > 0;
}

#if !defined(AERON_HAS_OPENXR)

int Aeron_XrConfigureDeviceProperties(SDL_PropertiesID properties) {
	(void)properties;
	if (Aeron_XrRequested()) {
		Aeron_LogWarn("aeron.xr", "XR requested, but this build has no OpenXR support");
	}
	return 0;
}

int Aeron_XrInit(SDL_GPUDevice* device, int w, int h, SDL_GPUTextureFormat f) {
	(void)device;
	(void)w;
	(void)h;
	(void)f;
	return 0;
}

void Aeron_XrShutdown(void) {}

int Aeron_XrActive(void) { return 0; }

int Aeron_XrFocused(void) { return 0; }

int Aeron_XrSessionRunning(void) { return 0; }

int Aeron_XrPollEvents(void) { return 1; }

int Aeron_XrWaitAndBeginFrame(void) { return 0; }

int Aeron_XrFrameBegun(void) { return 0; }

int Aeron_XrShouldRender(void) { return 0; }

SDL_GPUTexture* Aeron_XrScreenTexture(void) { return NULL; }

SDL_GPUTextureFormat Aeron_XrScreenFormat(void) { return SDL_GPU_TEXTUREFORMAT_INVALID; }

void Aeron_XrScreenSize(int* w, int* h) {
	if (w)
		*w = 0;
	if (h)
		*h = 0;
}

int Aeron_XrCopyScreenToSwapchain(SDL_GPUCommandBuffer* c) {
	(void)c;
	return 0;
}

int Aeron_XrRecordCapture(SDL_GPUCommandBuffer* c) {
	(void)c;
	return 0;
}

int Aeron_XrEndFrame(SDL_GPUFence* f) {
	(void)f;
	return 1;
}

double Aeron_XrDisplayRefreshRate(void) { return 0.0; }

#else /* AERON_HAS_OPENXR */

#include "xr_private.h"

AeronXr g_xr;

/* Up to three captures per frame: the virtual screen, or (stereo) both eyes
 * plus the HUD. Only 8-bit formats are written. */
#define AERON_XR_CAPTURE_SLOTS 3
static SDL_GPUTransferBuffer* g_capture_buffers[AERON_XR_CAPTURE_SLOTS];
static const char*            g_capture_names[AERON_XR_CAPTURE_SLOTS];
static int                    g_capture_sizes[AERON_XR_CAPTURE_SLOTS][2];
static int                    g_capture_count;

const char* Aeron_XrResultName(XrResult result) {
	static char buffer[XR_MAX_RESULT_STRING_SIZE];
	if (g_xr.xrResultToString && g_xr.instance &&
		XR_SUCCEEDED(g_xr.xrResultToString(g_xr.instance, result, buffer))) {
		return buffer;
	}
	SDL_snprintf(buffer, sizeof buffer, "XrResult(%d)", (int)result);
	return buffer;
}

static float Aeron_XrEnvFloat(const char* name, float fallback) {
	const char* value = SDL_getenv(name);
	if (value && value[0]) {
		const float parsed = (float)SDL_atof(value);
		if (parsed > 0.0f) {
			return parsed;
		}
	}
	return fallback;
}

int Aeron_XrConfigureDeviceProperties(SDL_PropertiesID properties) {
	if (!Aeron_XrRequested()) {
		return 0;
	}
	SDL_zero(g_xr);
	return SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_XR_ENABLE_BOOLEAN, true) &&
		   SDL_SetPointerProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_XR_INSTANCE_POINTER,
								  &g_xr.instance) &&
		   SDL_SetPointerProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_XR_SYSTEM_ID_POINTER,
								  &g_xr.system_id) &&
		   SDL_SetStringProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_XR_APPLICATION_NAME_STRING,
								 "OpenXWA") &&
		   SDL_SetNumberProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_XR_APPLICATION_VERSION_NUMBER, 1) &&
		   SDL_SetStringProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_XR_ENGINE_NAME_STRING, "Aeron") &&
		   SDL_SetNumberProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_XR_ENGINE_VERSION_NUMBER, 1);
}

static int Aeron_XrLoadFunctions(void) {
	g_xr.xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)SDL_OpenXR_GetXrGetInstanceProcAddr();
	if (!g_xr.xrGetInstanceProcAddr) {
		Aeron_LogError("aeron.xr", "SDL_OpenXR_GetXrGetInstanceProcAddr failed: %s", SDL_GetError());
		return 0;
	}
#define AERON_XR_LOAD(fn)                                                                                    \
	if (XR_FAILED(g_xr.xrGetInstanceProcAddr(g_xr.instance, #fn, (PFN_xrVoidFunction*)&g_xr.fn))) {          \
		Aeron_LogError("aeron.xr", "missing OpenXR entry point %s", #fn);                                    \
		return 0;                                                                                            \
	}
	AERON_XR_LOAD(xrPollEvent);
	AERON_XR_LOAD(xrBeginSession);
	AERON_XR_LOAD(xrEndSession);
	AERON_XR_LOAD(xrDestroySession);
	AERON_XR_LOAD(xrDestroyInstance);
	AERON_XR_LOAD(xrCreateReferenceSpace);
	AERON_XR_LOAD(xrDestroySpace);
	AERON_XR_LOAD(xrWaitFrame);
	AERON_XR_LOAD(xrBeginFrame);
	AERON_XR_LOAD(xrEndFrame);
	AERON_XR_LOAD(xrAcquireSwapchainImage);
	AERON_XR_LOAD(xrEnumerateSwapchainImages);
	AERON_XR_LOAD(xrWaitSwapchainImage);
	AERON_XR_LOAD(xrReleaseSwapchainImage);
	AERON_XR_LOAD(xrGetInstanceProperties);
	AERON_XR_LOAD(xrGetSystemProperties);
	AERON_XR_LOAD(xrResultToString);
	AERON_XR_LOAD(xrStringToPath);
	AERON_XR_LOAD(xrCreateActionSet);
	AERON_XR_LOAD(xrDestroyActionSet);
	AERON_XR_LOAD(xrCreateAction);
	AERON_XR_LOAD(xrSuggestInteractionProfileBindings);
	AERON_XR_LOAD(xrAttachSessionActionSets);
	AERON_XR_LOAD(xrSyncActions);
	AERON_XR_LOAD(xrGetActionStateFloat);
	AERON_XR_LOAD(xrGetActionStateBoolean);
	AERON_XR_LOAD(xrGetActionStateVector2f);
	AERON_XR_LOAD(xrGetActionStatePose);
	AERON_XR_LOAD(xrCreateActionSpace);
	AERON_XR_LOAD(xrLocateSpace);
	AERON_XR_LOAD(xrApplyHapticFeedback);
	AERON_XR_LOAD(xrStopHapticFeedback);
	AERON_XR_LOAD(xrEnumerateViewConfigurationViews);
	AERON_XR_LOAD(xrLocateViews);
#undef AERON_XR_LOAD
	return 1;
}

static SDL_GPUTextureFormat Aeron_XrPickFormat(SDL_GPUTextureFormat preferred) {
	/* The composition shaders output LINEAR light, so the virtual screen must
	 * be an _SRGB format (hardware encodes on write, which is also what XR
	 * compositors expect) or a float format. Plain UNORM would look too dark. */
	static const SDL_GPUTextureFormat ranked[] = {
		SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB,
		SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB,
		SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
	};
	int                   count   = 0;
	SDL_GPUTextureFormat* formats = SDL_GetGPUXRSwapchainFormats(g_xr.device, g_xr.session, &count);
	SDL_GPUTextureFormat  chosen  = SDL_GPU_TEXTUREFORMAT_INVALID;
	size_t                r;
	int                   i;

	(void)preferred;
	if (!formats) {
		Aeron_LogError("aeron.xr", "SDL_GetGPUXRSwapchainFormats failed: %s", SDL_GetError());
		return SDL_GPU_TEXTUREFORMAT_INVALID;
	}
	for (r = 0; r < SDL_arraysize(ranked) && chosen == SDL_GPU_TEXTUREFORMAT_INVALID; ++r) {
		for (i = 0; i < count && formats[i] != SDL_GPU_TEXTUREFORMAT_INVALID; ++i) {
			if (formats[i] == ranked[r]) {
				chosen = ranked[r];
				break;
			}
		}
	}
	if (chosen == SDL_GPU_TEXTUREFORMAT_INVALID && count > 0) {
		chosen = formats[0];
		Aeron_LogWarn("aeron.xr", "no sRGB/float XR swapchain format; using %d (colors may be off)",
					  (int)chosen);
	}
	SDL_free(formats);
	return chosen;
}

static void Aeron_XrParseCapture(void) {
	const char* frames       = SDL_getenv("AERON_XR_CAPTURE");
	const char* dir          = SDL_getenv("AERON_XR_CAPTURE_DIR");
	g_xr.capture_frame_count = 0;
	if (!frames || !frames[0]) {
		return;
	}
	SDL_snprintf(g_xr.capture_dir, sizeof g_xr.capture_dir, "%s", dir && dir[0] ? dir : ".");
	while (*frames && g_xr.capture_frame_count < (int)SDL_arraysize(g_xr.capture_frames)) {
		char*                    end   = NULL;
		const unsigned long long value = strtoull(frames, &end, 10);
		if (end == frames) {
			break;
		}
		g_xr.capture_frames[g_xr.capture_frame_count++] = (uint64_t)value;
		frames                                          = *end == ',' ? end + 1 : end;
	}
}

int Aeron_XrInit(SDL_GPUDevice* device, int screen_width, int screen_height,
				 SDL_GPUTextureFormat preferred_format) {
	XrInstanceProperties       instance_properties = { XR_TYPE_INSTANCE_PROPERTIES, NULL };
	XrSystemProperties         system_properties   = { XR_TYPE_SYSTEM_PROPERTIES, NULL };
	XrSessionCreateInfo        session_info        = { XR_TYPE_SESSION_CREATE_INFO, NULL };
	XrReferenceSpaceCreateInfo space_info          = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO, NULL };
	XrSwapchainCreateInfo      swapchain_info      = { XR_TYPE_SWAPCHAIN_CREATE_INFO, NULL };
	SDL_GPUTextureCreateInfo   texture_info;
	XrResult                   result;

	if (!Aeron_XrRequested() || !device) {
		return 0;
	}
	if (g_xr.instance == XR_NULL_HANDLE) {
		Aeron_LogError("aeron.xr", "XR was requested, but the GPU device has no OpenXR instance");
		return 0;
	}
	g_xr.device = device;
	if (!Aeron_XrLoadFunctions()) {
		Aeron_XrShutdown();
		return 0;
	}
	if (XR_SUCCEEDED(g_xr.xrGetInstanceProperties(g_xr.instance, &instance_properties))) {
		Aeron_LogInfo("aeron.xr", "OpenXR runtime: %s %u.%u.%u", instance_properties.runtimeName,
					  XR_VERSION_MAJOR(instance_properties.runtimeVersion),
					  XR_VERSION_MINOR(instance_properties.runtimeVersion),
					  XR_VERSION_PATCH(instance_properties.runtimeVersion));
	}
	g_xr.max_layers = XR_MIN_COMPOSITION_LAYERS_SUPPORTED;
	if (XR_SUCCEEDED(g_xr.xrGetSystemProperties(g_xr.instance, g_xr.system_id, &system_properties))) {
		Aeron_LogInfo("aeron.xr", "OpenXR system: %s (max swapchain %ux%u, %u layers)", system_properties.systemName,
					  system_properties.graphicsProperties.maxSwapchainImageWidth,
					  system_properties.graphicsProperties.maxSwapchainImageHeight,
					  system_properties.graphicsProperties.maxLayerCount);
		if (system_properties.graphicsProperties.maxLayerCount > g_xr.max_layers) {
			g_xr.max_layers = system_properties.graphicsProperties.maxLayerCount;
		}
	}

	result = SDL_CreateGPUXRSession(device, &session_info, &g_xr.session);
	if (XR_FAILED(result)) {
		Aeron_LogError("aeron.xr", "SDL_CreateGPUXRSession failed: %s (%s)", Aeron_XrResultName(result),
					   SDL_GetError());
		Aeron_XrShutdown();
		return 0;
	}

	space_info.referenceSpaceType                 = XR_REFERENCE_SPACE_TYPE_LOCAL;
	space_info.poseInReferenceSpace.orientation.w = 1.0f;
	result = g_xr.xrCreateReferenceSpace(g_xr.session, &space_info, &g_xr.local_space);
	if (XR_FAILED(result)) {
		Aeron_LogError("aeron.xr", "xrCreateReferenceSpace(LOCAL) failed: %s", Aeron_XrResultName(result));
		Aeron_XrShutdown();
		return 0;
	}

	g_xr.screen_width  = screen_width > 0 ? screen_width : 1920;
	g_xr.screen_height = screen_height > 0 ? screen_height : 1080;
	g_xr.screen_format = Aeron_XrPickFormat(preferred_format);
	if (g_xr.screen_format == SDL_GPU_TEXTUREFORMAT_INVALID) {
		Aeron_XrShutdown();
		return 0;
	}

	swapchain_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	swapchain_info.sampleCount = 1;
	swapchain_info.width       = (uint32_t)g_xr.screen_width;
	swapchain_info.height      = (uint32_t)g_xr.screen_height;
	swapchain_info.faceCount   = 1;
	swapchain_info.arraySize   = 1;
	swapchain_info.mipCount    = 1;
	result = SDL_CreateGPUXRSwapchain(device, g_xr.session, &swapchain_info, g_xr.screen_format,
									  &g_xr.screen_swapchain, &g_xr.screen_images);
	if (XR_FAILED(result)) {
		Aeron_LogError("aeron.xr", "SDL_CreateGPUXRSwapchain failed: %s (%s)", Aeron_XrResultName(result),
					   SDL_GetError());
		Aeron_XrShutdown();
		return 0;
	}

	SDL_zero(texture_info);
	texture_info.type                 = SDL_GPU_TEXTURETYPE_2D;
	texture_info.format               = g_xr.screen_format;
	texture_info.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
	texture_info.width                = (Uint32)g_xr.screen_width;
	texture_info.height               = (Uint32)g_xr.screen_height;
	texture_info.layer_count_or_depth = 1;
	texture_info.num_levels           = 1;
	texture_info.sample_count         = SDL_GPU_SAMPLECOUNT_1;
	g_xr.screen_texture               = SDL_CreateGPUTexture(device, &texture_info);
	if (!g_xr.screen_texture) {
		Aeron_LogError("aeron.xr", "virtual screen texture creation failed: %s", SDL_GetError());
		Aeron_XrShutdown();
		return 0;
	}
	SDL_SetGPUTextureName(device, g_xr.screen_texture, "aeron.xr.screen");

	/* World-locked screen in the LOCAL space (origin = head at session start).
	 * Defaults give a comfortable cinema-like screen; tunable per user. */
	g_xr.screen_distance_m      = Aeron_XrEnvFloat("AERON_XR_SCREEN_DISTANCE", 1.6f);
	g_xr.screen_width_m         = Aeron_XrEnvFloat("AERON_XR_SCREEN_WIDTH", 2.0f);
	g_xr.screen_height_offset_m = 0.0f;
	{
		const char* offset = SDL_getenv("AERON_XR_SCREEN_HEIGHT_OFFSET");
		if (offset && offset[0]) {
			g_xr.screen_height_offset_m = (float)SDL_atof(offset);
		}
	}

	Aeron_XrParseCapture(); /* before stereo init: capture changes swapchain usage */
	Aeron_XrStereoInit();
	if (!Aeron_XrInputInit()) {
		Aeron_LogWarn("aeron.xr", "controller input unavailable; keyboard/mouse/joystick still work");
	}
	g_xr.state = XR_SESSION_STATE_UNKNOWN;
	Aeron_LogInfo("aeron.xr", "virtual screen %dx%d format %d at %.2f m, %.2f m wide", g_xr.screen_width,
				  g_xr.screen_height, (int)g_xr.screen_format, g_xr.screen_distance_m, g_xr.screen_width_m);
	return 1;
}

void Aeron_XrShutdown(void) {
	Aeron_XrInputShutdown();
	Aeron_XrStereoShutdown();
	if (g_xr.device) {
		for (int i = 0; i < AERON_XR_CAPTURE_SLOTS; ++i) {
			if (g_capture_buffers[i]) {
				SDL_ReleaseGPUTransferBuffer(g_xr.device, g_capture_buffers[i]);
				g_capture_buffers[i] = NULL;
			}
		}
		if (g_xr.screen_texture) {
			SDL_ReleaseGPUTexture(g_xr.device, g_xr.screen_texture);
		}
		if (g_xr.screen_swapchain) {
			SDL_DestroyGPUXRSwapchain(g_xr.device, g_xr.screen_swapchain, g_xr.screen_images);
		}
	}
	if (g_xr.local_space && g_xr.xrDestroySpace) {
		g_xr.xrDestroySpace(g_xr.local_space);
	}
	/* xrDestroySession is valid in any session state; xrEndSession is only
	 * valid after the runtime moved the session to STOPPING. */
	if (g_xr.session && g_xr.xrDestroySession) {
		g_xr.xrDestroySession(g_xr.session);
	}
	/* The instance belongs to the GPU device; SDL destroys it with the device. */
	SDL_zero(g_xr);
}

int Aeron_XrActive(void) { return g_xr.session != XR_NULL_HANDLE; }

int Aeron_XrFocused(void) { return g_xr.session != XR_NULL_HANDLE && g_xr.state == XR_SESSION_STATE_FOCUSED; }

int Aeron_XrSessionRunning(void) { return g_xr.session != XR_NULL_HANDLE && g_xr.session_running; }

int Aeron_XrPollEvents(void) {
	XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER, NULL };

	if (!Aeron_XrActive()) {
		return 1;
	}
	while (g_xr.xrPollEvent(g_xr.instance, &event) == XR_SUCCESS) {
		if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			const XrEventDataSessionStateChanged* changed = (const XrEventDataSessionStateChanged*)&event;
			g_xr.state                                    = changed->state;
			Aeron_LogInfo("aeron.xr", "session state -> %d", (int)changed->state);
			if (changed->state == XR_SESSION_STATE_READY) {
				XrSessionBeginInfo begin = { XR_TYPE_SESSION_BEGIN_INFO, NULL };
				XrResult           result;
				begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				result                             = g_xr.xrBeginSession(g_xr.session, &begin);
				if (XR_SUCCEEDED(result)) {
					g_xr.session_running = 1;
					Aeron_LogInfo("aeron.xr", "session running");
				} else {
					Aeron_LogError("aeron.xr", "xrBeginSession failed: %s", Aeron_XrResultName(result));
				}
			} else if (changed->state == XR_SESSION_STATE_STOPPING) {
				g_xr.xrEndSession(g_xr.session);
				g_xr.session_running = 0;
			} else if (changed->state == XR_SESSION_STATE_EXITING ||
					   changed->state == XR_SESSION_STATE_LOSS_PENDING) {
				g_xr.session_running = 0;
				g_xr.exit_requested  = 1;
			}
		} else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
			g_xr.session_running = 0;
			g_xr.exit_requested  = 1;
		}
		event.type = XR_TYPE_EVENT_DATA_BUFFER;
		event.next = NULL;
	}
	return !g_xr.exit_requested;
}

int Aeron_XrFrameBegun(void) { return g_xr.frame_begun; }

int Aeron_XrShouldRender(void) { return g_xr.frame_begun && g_xr.should_render; }

int Aeron_XrWaitAndBeginFrame(void) {
	XrFrameWaitInfo  wait_info   = { XR_TYPE_FRAME_WAIT_INFO, NULL };
	XrFrameState     frame_state = { XR_TYPE_FRAME_STATE, NULL };
	XrFrameBeginInfo begin_info  = { XR_TYPE_FRAME_BEGIN_INFO, NULL };
	XrResult         result;

	if (!Aeron_XrSessionRunning()) {
		return 0;
	}
	if (g_xr.frame_begun) {
		/* The previous host frame never presented (no layers submitted):
		 * end it so the runtime keeps showing the last screen image. */
		Aeron_XrEndFrame(NULL);
	}
	result = g_xr.xrWaitFrame(g_xr.session, &wait_info, &frame_state);
	if (XR_FAILED(result)) {
		Aeron_LogError("aeron.xr", "xrWaitFrame failed: %s", Aeron_XrResultName(result));
		return -1;
	}
	result = g_xr.xrBeginFrame(g_xr.session, &begin_info);
	if (XR_FAILED(result)) {
		Aeron_LogError("aeron.xr", "xrBeginFrame failed: %s", Aeron_XrResultName(result));
		return -1;
	}
	g_xr.perf_frame_start_ns = SDL_GetTicksNS();
	if (g_xr.perf_last_display_time && frame_state.predictedDisplayPeriod > 0 &&
		frame_state.predictedDisplayTime - g_xr.perf_last_display_time >
			frame_state.predictedDisplayPeriod + frame_state.predictedDisplayPeriod / 2) {
		g_xr.perf_late++; /* the runtime skipped at least one display slot */
	}
	g_xr.perf_last_display_time   = frame_state.predictedDisplayTime;
	g_xr.frame_begun              = 1;
	g_xr.image_acquired           = 0;
	g_xr.predicted_display_time   = frame_state.predictedDisplayTime;
	g_xr.predicted_display_period = frame_state.predictedDisplayPeriod;
	g_xr.should_render            = frame_state.shouldRender;
	return 1;
}

SDL_GPUTexture* Aeron_XrScreenTexture(void) { return g_xr.screen_texture; }

SDL_GPUTextureFormat Aeron_XrScreenFormat(void) { return g_xr.screen_format; }

void Aeron_XrScreenSize(int* width, int* height) {
	if (width)
		*width = g_xr.screen_width;
	if (height)
		*height = g_xr.screen_height;
}

int Aeron_XrCopyScreenToSwapchain(SDL_GPUCommandBuffer* command_buffer) {
	XrSwapchainImageAcquireInfo acquire_info = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, NULL };
	XrSwapchainImageWaitInfo    wait_info    = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, NULL };
	SDL_GPUBlitInfo             blit;
	uint32_t                    image_index = 0;
	XrResult                    result;

	if (!Aeron_XrShouldRender() || !command_buffer) {
		return 0;
	}
	result = g_xr.xrAcquireSwapchainImage(g_xr.screen_swapchain, &acquire_info, &image_index);
	if (XR_FAILED(result)) {
		Aeron_LogError("aeron.xr", "xrAcquireSwapchainImage failed: %s", Aeron_XrResultName(result));
		return 0;
	}
	g_xr.image_acquired = 1;
	wait_info.timeout   = XR_INFINITE_DURATION;
	result              = g_xr.xrWaitSwapchainImage(g_xr.screen_swapchain, &wait_info);
	if (XR_FAILED(result)) {
		Aeron_LogError("aeron.xr", "xrWaitSwapchainImage failed: %s", Aeron_XrResultName(result));
		return 0;
	}
	SDL_zero(blit);
	blit.source.texture      = g_xr.screen_texture;
	blit.source.w            = (Uint32)g_xr.screen_width;
	blit.source.h            = (Uint32)g_xr.screen_height;
	blit.destination.texture = g_xr.screen_images[image_index];
	blit.destination.w       = (Uint32)g_xr.screen_width;
	blit.destination.h       = (Uint32)g_xr.screen_height;
	blit.load_op             = SDL_GPU_LOADOP_DONT_CARE;
	blit.filter              = SDL_GPU_FILTER_NEAREST;
	SDL_BlitGPUTexture(command_buffer, &blit);
	return 1;
}

static int Aeron_XrCaptureDue(void) {
	int i;
	for (i = 0; i < g_xr.capture_frame_count; ++i) {
		if (g_xr.capture_frames[i] == g_xr.frame_index) {
			return 1;
		}
	}
	return 0;
}

static int Aeron_XrCaptureTexture(SDL_GPUCopyPass* copy_pass, SDL_GPUTexture* texture, int width, int height,
								  const char* name) {
	SDL_GPUTextureRegion       region;
	SDL_GPUTextureTransferInfo transfer;
	const int                  slot = g_capture_count;
	const Uint32               size = (Uint32)width * (Uint32)height * 4u;

	if (!texture || slot >= AERON_XR_CAPTURE_SLOTS || width <= 0 || height <= 0) {
		return 0;
	}
	if (g_capture_buffers[slot] &&
		(g_capture_sizes[slot][0] != width || g_capture_sizes[slot][1] != height)) {
		SDL_ReleaseGPUTransferBuffer(g_xr.device, g_capture_buffers[slot]);
		g_capture_buffers[slot] = NULL;
	}
	if (!g_capture_buffers[slot]) {
		SDL_GPUTransferBufferCreateInfo info;
		SDL_zero(info);
		info.usage              = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		info.size               = size;
		g_capture_buffers[slot] = SDL_CreateGPUTransferBuffer(g_xr.device, &info);
		if (!g_capture_buffers[slot]) {
			return 0;
		}
	}
	SDL_zero(region);
	region.texture = texture;
	region.w       = (Uint32)width;
	region.h       = (Uint32)height;
	region.d       = 1;
	SDL_zero(transfer);
	transfer.transfer_buffer = g_capture_buffers[slot];
	SDL_DownloadFromGPUTexture(copy_pass, &region, &transfer);
	g_capture_sizes[slot][0] = width;
	g_capture_sizes[slot][1] = height;
	g_capture_names[slot]    = name;
	g_capture_count++;
	return 1;
}

int Aeron_XrRecordCapture(SDL_GPUCommandBuffer* command_buffer) {
	SDL_GPUCopyPass* copy_pass;

	g_capture_count = 0;
	if (!command_buffer || !Aeron_XrCaptureDue() ||
		g_xr.screen_format == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT) {
		return 0;
	}
	copy_pass = SDL_BeginGPUCopyPass(command_buffer);
	if (!copy_pass) {
		return 0;
	}
	if (Aeron_XrStereoCommitted()) {
		int w = 0;
		int h = 0;
		Aeron_XrStereoSize(&w, &h);
		Aeron_XrCaptureTexture(copy_pass, Aeron_XrStereoTexture(0), w, h, "eye_left");
		Aeron_XrCaptureTexture(copy_pass, Aeron_XrStereoTexture(1), w, h, "eye_right");
		if (Aeron_XrStereoHudVisible()) {
			Aeron_XrCaptureTexture(copy_pass, Aeron_XrStereoTexture(2), w, h, "hud");
		}
	} else {
		Aeron_XrCaptureTexture(copy_pass, g_xr.screen_texture, g_xr.screen_width, g_xr.screen_height,
							   "screen");
	}
	SDL_EndGPUCopyPass(copy_pass);
	g_xr.capture_pending = g_capture_count > 0;
	return g_xr.capture_pending;
}

static void Aeron_XrFinishCapture(SDL_GPUFence* fence) {
	int slot;

	g_xr.capture_pending = 0;
	if (!fence || !SDL_WaitForGPUFences(g_xr.device, true, &fence, 1)) {
		return;
	}
	for (slot = 0; slot < g_capture_count; ++slot) {
		const int    width  = g_capture_sizes[slot][0];
		const int    height = g_capture_sizes[slot][1];
		void*        pixels = SDL_MapGPUTransferBuffer(g_xr.device, g_capture_buffers[slot], false);
		SDL_Surface* surface;
		char         path[640];
		if (!pixels) {
			continue;
		}
		surface = SDL_CreateSurfaceFrom(width, height,
										g_xr.screen_format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB ||
												g_xr.screen_format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM
											? SDL_PIXELFORMAT_ARGB8888
											: SDL_PIXELFORMAT_ABGR8888,
										pixels, width * 4);
		if (surface) {
			SDL_snprintf(path, sizeof path, "%s/xr_%s_%06llu.bmp", g_xr.capture_dir, g_capture_names[slot],
						 (unsigned long long)g_xr.frame_index);
			if (SDL_SaveBMP(surface, path)) {
				Aeron_LogInfo("aeron.xr", "captured %s to %s", g_capture_names[slot], path);
			}
			SDL_DestroySurface(surface);
		}
		SDL_UnmapGPUTransferBuffer(g_xr.device, g_capture_buffers[slot]);
	}
	g_capture_count = 0;
}

/* The virtual screen as a world-locked quad in LOCAL space. */
static void Aeron_XrFillScreenQuad(XrCompositionLayerQuad* quad) {
	const float aspect                     = (float)g_xr.screen_height / (float)g_xr.screen_width;
	quad->space                            = g_xr.local_space;
	quad->eyeVisibility                    = XR_EYE_VISIBILITY_BOTH;
	quad->subImage.swapchain               = g_xr.screen_swapchain;
	quad->subImage.imageRect.extent.width  = g_xr.screen_width;
	quad->subImage.imageRect.extent.height = g_xr.screen_height;
	quad->pose.orientation.w               = 1.0f;
	quad->pose.position.y                  = g_xr.screen_height_offset_m;
	quad->pose.position.z                  = -g_xr.screen_distance_m;
	quad->size.width                       = g_xr.screen_width_m;
	quad->size.height                      = g_xr.screen_width_m * aspect;
}

int Aeron_XrEndFrame(SDL_GPUFence* submitted_fence) {
	XrSwapchainImageReleaseInfo         release_info = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, NULL };
	XrCompositionLayerQuad              quad         = { XR_TYPE_COMPOSITION_LAYER_QUAD, NULL };
	const XrCompositionLayerBaseHeader* layers[AERON_XR_MAX_LAYERS];
	XrFrameEndInfo                      end_info = { XR_TYPE_FRAME_END_INFO, NULL };
	XrResult                            result;
	uint32_t                            stereo_layers;
	int                                 screen_overlay;

	if (g_xr.capture_pending) {
		Aeron_XrFinishCapture(submitted_fence);
	}
	if (submitted_fence) {
		SDL_ReleaseGPUFence(g_xr.device, submitted_fence);
	}
	if (!g_xr.frame_begun) {
		return 1;
	}
	if (g_xr.image_acquired) {
		result = g_xr.xrReleaseSwapchainImage(g_xr.screen_swapchain, &release_info);
		if (XR_SUCCEEDED(result)) {
			g_xr.screen_has_released_image = 1;
		} else {
			Aeron_LogError("aeron.xr", "xrReleaseSwapchainImage failed: %s", Aeron_XrResultName(result));
		}
		g_xr.image_acquired = 0;
	}

	end_info.displayTime          = g_xr.predicted_display_time;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end_info.layerCount           = 0;
	end_info.layers               = layers;
	screen_overlay                = g_xr.should_render && Aeron_XrStereoScreenOverlay() &&
					 g_xr.screen_has_released_image;
	if (screen_overlay) {
		/* Backdrop frame: the virtual screen over the environment. */
		Aeron_XrFillScreenQuad(&quad);
	}
	stereo_layers = g_xr.should_render
						? Aeron_XrStereoEndFrameLayers(
							  layers, g_xr.max_layers < AERON_XR_MAX_LAYERS ? g_xr.max_layers : AERON_XR_MAX_LAYERS,
							  screen_overlay ? (const XrCompositionLayerBaseHeader*)&quad : NULL)
						: 0;
	if (!g_xr.should_render) {
		Aeron_XrStereoEndFrameLayers(layers, 0, NULL); /* release anything acquired */
	}
	if (stereo_layers > 0) {
		end_info.layerCount = stereo_layers;
	} else if (g_xr.should_render && g_xr.screen_has_released_image) {
		Aeron_XrFillScreenQuad(&quad);
		layers[0]                             = (const XrCompositionLayerBaseHeader*)&quad;
		end_info.layerCount                   = 1;
	}
	result           = g_xr.xrEndFrame(g_xr.session, &end_info);
	g_xr.frame_begun = 0;
	{
		const uint64_t now    = SDL_GetTicksNS();
		const double   cpu_ms = g_xr.perf_frame_start_ns ? (double)(now - g_xr.perf_frame_start_ns) / 1.0e6 : 0.0;
		if (!g_xr.perf_window_start_ns) {
			g_xr.perf_window_start_ns = now;
		}
		g_xr.perf_frames++;
		g_xr.perf_stereo_frames += stereo_layers ? 1u : 0u;
		g_xr.perf_cpu_ms_sum += cpu_ms;
		if (cpu_ms > g_xr.perf_cpu_ms_max) {
			g_xr.perf_cpu_ms_max = cpu_ms;
		}
		if (now - g_xr.perf_window_start_ns >= 5000000000ull) {
			const double secs = (double)(now - g_xr.perf_window_start_ns) / 1.0e9;
			const double hz   = g_xr.predicted_display_period > 0 ? 1.0e9 / (double)g_xr.predicted_display_period : 0.0;
			Aeron_LogInfo("aeron.xr",
						  "perf: %.1f fps (display %.0f Hz), frame work avg %.2f ms max %.2f ms, skipped slots %u, "
						  "%u/%u frames stereo",
						  (double)g_xr.perf_frames / secs, hz, g_xr.perf_cpu_ms_sum / (double)g_xr.perf_frames,
						  g_xr.perf_cpu_ms_max, g_xr.perf_late, g_xr.perf_stereo_frames, g_xr.perf_frames);
			g_xr.perf_window_start_ns = now;
			g_xr.perf_frames          = 0;
			g_xr.perf_stereo_frames   = 0;
			g_xr.perf_late            = 0;
			g_xr.perf_cpu_ms_sum      = 0.0;
			g_xr.perf_cpu_ms_max      = 0.0;
		}
	}
	if ((g_xr.frame_index % 300u) == 0u) {
		Aeron_LogInfo("aeron.xr", "frame %llu: display period %.2f ms, layers %u (%s)",
					  (unsigned long long)g_xr.frame_index, (double)g_xr.predicted_display_period / 1.0e6,
					  end_info.layerCount, stereo_layers ? "stereo" : "screen");
	}
	g_xr.frame_index++;
	if (XR_FAILED(result)) {
		Aeron_LogError("aeron.xr", "xrEndFrame failed: %s", Aeron_XrResultName(result));
		return 0;
	}
	return 1;
}

XrPath Aeron_XrPath(const char* string) {
	XrPath path = XR_NULL_PATH;
	if (g_xr.xrStringToPath && XR_FAILED(g_xr.xrStringToPath(g_xr.instance, string, &path))) {
		Aeron_LogWarn("aeron.xr", "invalid OpenXR path %s", string);
		path = XR_NULL_PATH;
	}
	return path;
}

double Aeron_XrDisplayRefreshRate(void) {
	return g_xr.predicted_display_period > 0 ? 1.0e9 / (double)g_xr.predicted_display_period : 0.0;
}

#endif /* AERON_HAS_OPENXR */
