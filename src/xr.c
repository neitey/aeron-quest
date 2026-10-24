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

#if defined(AERON_HAS_OPENXR) && defined(SDL_PLATFORM_ANDROID)
#include <jni.h>
#endif

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

int Aeron_XrRecoverInstanceFailure(void) { return 0; }

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

void Aeron_XrRequestRecenter(void) {}

int Aeron_XrVirtualScreenGeometry(float* d, float* w, float* h, float* o) {
	(void)d;
	(void)w;
	(void)h;
	(void)o;
	return 0;
}

int Aeron_XrCopyScreenToSwapchain(SDL_GPUCommandBuffer* c) {
	(void)c;
	return 0;
}

SDL_GPUTexture* Aeron_XrScreenRightTexture(void) { return NULL; }

int Aeron_XrCopyScreenRightToSwapchain(SDL_GPUCommandBuffer* c) {
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

static int Aeron_XrSetLocalPose(const XrPosef* pose, XrTime time);

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

/* After a failed instance creation (Aeron_XrRecoverInstanceFailure): the
 * optional extensions the runtime turned down, whether XR_EXT_uuid goes
 * with XR_EXT_render_model, and the API version to ask for (0 = SDL's
 * default, 1.0). */
static char      g_xr_dropped[16][XR_MAX_EXTENSION_NAME_SIZE];
static int       g_xr_dropped_count;
static int       g_xr_add_uuid;
static XrVersion g_xr_api_version;

static int Aeron_XrDropped(const char* name) {
	for (int i = 0; i < g_xr_dropped_count; ++i) {
		if (strcmp(g_xr_dropped[i], name) == 0) {
			return 1;
		}
	}
	return 0;
}

/* The optional instance extensions this runtime offers. Enabling one it
 * lacks would fail instance creation, so ask the loader first. */
static void Aeron_XrPickExtensions(void) {
	static const char* const wanted[] = {
		XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME,
		XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME,
		/* Controller profiles: without them the runtimes of these headsets
		 * reject the bindings for their own controllers. */
		XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME,
		XR_BD_ULTRA_CONTROLLER_INTERACTION_EXTENSION_NAME,
		XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME,
		"XR_VALVE_frame_controller_interaction",
		/* Controller models the runtime draws for us (xr_render_model.c). */
		XR_FB_RENDER_MODEL_EXTENSION_NAME,
		XR_EXT_RENDER_MODEL_EXTENSION_NAME,
		XR_EXT_INTERACTION_RENDER_MODEL_EXTENSION_NAME,
	};
	const char* models_env = SDL_getenv("AERON_XR_RENDER_MODELS");
	const int   want_models = !(models_env && models_env[0] == '0');
	PFN_xrEnumerateInstanceExtensionProperties enumerate = NULL;
	PFN_xrGetInstanceProcAddr                  gipa;
	XrExtensionProperties*                     props;
	uint32_t                                   count = 0;
	size_t                                     w;

	if (!SDL_OpenXR_LoadLibrary()) {
		return;
	}
	/* The Khronos loader exports the global functions; the runtime-negotiated
	 * xrGetInstanceProcAddr may not serve them before an instance exists. */
	{
		static const char* const names[] = { "libopenxr_loader.so", "libopenxr_loader.so.1", "openxr_loader.dll" };
		size_t                   n;
		for (n = 0; n < SDL_arraysize(names) && !enumerate; ++n) {
			SDL_SharedObject* lib = SDL_LoadObject(names[n]);
			if (lib) {
				enumerate = (PFN_xrEnumerateInstanceExtensionProperties)SDL_LoadFunction(
					lib, "xrEnumerateInstanceExtensionProperties");
			}
		}
	}
	gipa = (PFN_xrGetInstanceProcAddr)SDL_OpenXR_GetXrGetInstanceProcAddr();
	if (!enumerate && gipa) {
		gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&enumerate);
	}
	if (!enumerate || XR_FAILED(enumerate(NULL, 0, &count, NULL)) || count == 0) {
		Aeron_LogWarn("aeron.xr", "cannot list OpenXR extensions; optional ones stay off");
		return;
	}
	props = (XrExtensionProperties*)SDL_calloc(count, sizeof *props);
	if (!props) {
		return;
	}
	for (uint32_t i = 0; i < count; ++i) {
		props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
	}
	if (XR_SUCCEEDED(enumerate(NULL, count, &count, props))) {
		/* Everything the runtime offers, for the log (a few per line). */
		{
			char     line[900];
			size_t   len = 0;
			uint32_t first = 0;
			line[0]        = 0;
			for (uint32_t i = 0; i <= count; ++i) {
				const size_t n = i < count ? strlen(props[i].extensionName) : 0;
				if (i == count || (len && len + n + 1 >= sizeof line)) {
					Aeron_LogInfo("aeron.xr", "runtime offers (%u-%u of %u): %s", first + 1, i, count, line);
					len     = 0;
					line[0] = 0;
					first   = i;
					if (i == count) {
						break;
					}
				}
				len += (size_t)SDL_snprintf(line + len, sizeof line - len, "%s%s", len ? " " : "",
											props[i].extensionName);
			}
		}
		int offered_ext_render_model = 0;
		for (uint32_t i = 0; i < count; ++i) {
			offered_ext_render_model |= strcmp(props[i].extensionName, XR_EXT_RENDER_MODEL_EXTENSION_NAME) == 0;
		}
		for (w = 0; w < SDL_arraysize(wanted); ++w) {
			const int model_ext = strcmp(wanted[w], XR_FB_RENDER_MODEL_EXTENSION_NAME) == 0 ||
								  strcmp(wanted[w], XR_EXT_RENDER_MODEL_EXTENSION_NAME) == 0 ||
								  strcmp(wanted[w], XR_EXT_INTERACTION_RENDER_MODEL_EXTENSION_NAME) == 0;
			if (model_ext && !want_models) {
				continue;
			}
			/* The interaction models need the render model extension too. */
			if (strcmp(wanted[w], XR_EXT_INTERACTION_RENDER_MODEL_EXTENSION_NAME) == 0 && !offered_ext_render_model) {
				continue;
			}
			if (Aeron_XrDropped(wanted[w])) {
				continue;
			}
			for (uint32_t i = 0; i < count; ++i) {
				if (strcmp(props[i].extensionName, wanted[w]) == 0 &&
					g_xr.extension_count < SDL_arraysize(g_xr.extensions)) {
					g_xr.extensions[g_xr.extension_count++] = wanted[w];
					break;
				}
			}
		}
	}
	/* XR_EXT_render_model needs XR_EXT_uuid on OpenXR 1.0 (the Steam Frame's
	 * runtime turns the instance down without it). */
	{
		int has_model = 0, has_uuid = 0, offered_uuid = g_xr_add_uuid;
		for (uint32_t i = 0; i < g_xr.extension_count; ++i) {
			has_model |= strcmp(g_xr.extensions[i], XR_EXT_RENDER_MODEL_EXTENSION_NAME) == 0;
			has_uuid |= strcmp(g_xr.extensions[i], XR_EXT_UUID_EXTENSION_NAME) == 0;
		}
		for (uint32_t i = 0; i < count && !offered_uuid; ++i) {
			offered_uuid = strcmp(props[i].extensionName, XR_EXT_UUID_EXTENSION_NAME) == 0;
		}
		if (has_model && !has_uuid && offered_uuid && !Aeron_XrDropped(XR_EXT_UUID_EXTENSION_NAME) &&
			g_xr.extension_count < SDL_arraysize(g_xr.extensions)) {
			g_xr.extensions[g_xr.extension_count++] = XR_EXT_UUID_EXTENSION_NAME;
		}
	}
	SDL_free(props);
	for (uint32_t i = 0; i < g_xr.extension_count; ++i) {
		if (strcmp(g_xr.extensions[i], XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME) == 0) {
			g_xr.has_perf_settings = 1;
		} else if (strcmp(g_xr.extensions[i], XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0) {
			g_xr.has_refresh_rate = 1;
		} else if (strcmp(g_xr.extensions[i], XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME) == 0) {
			g_xr.has_bd_controller = 1;
		} else if (strcmp(g_xr.extensions[i], XR_BD_ULTRA_CONTROLLER_INTERACTION_EXTENSION_NAME) == 0) {
			g_xr.has_bd_ultra = 1;
		} else if (strcmp(g_xr.extensions[i], XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME) == 0) {
			g_xr.has_htc_focus3 = 1;
		} else if (strcmp(g_xr.extensions[i], "XR_VALVE_frame_controller_interaction") == 0) {
			g_xr.has_valve_frame = 1;
		} else if (strcmp(g_xr.extensions[i], XR_FB_RENDER_MODEL_EXTENSION_NAME) == 0) {
			g_xr.has_fb_render_model = 1;
		} else if (strcmp(g_xr.extensions[i], XR_EXT_RENDER_MODEL_EXTENSION_NAME) == 0) {
			g_xr.has_ext_render_model = 1;
		} else if (strcmp(g_xr.extensions[i], XR_EXT_INTERACTION_RENDER_MODEL_EXTENSION_NAME) == 0) {
			g_xr.has_ext_interaction_render_model = 1;
		}
		Aeron_LogInfo("aeron.xr", "enabling %s", g_xr.extensions[i]);
	}
}

/* AERON_XR_PERF_LEVEL: 0 (leave it to the runtime), low, high (default),
 * boost. Sets the floor of the headset's CPU/GPU clock levels. */
static void Aeron_XrApplyPerfLevel(void) {
	const char*              env   = SDL_getenv("AERON_XR_PERF_LEVEL");
	XrPerfSettingsLevelEXT   level = XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
	if (!g_xr.has_perf_settings || !g_xr.xrPerfSettingsSetPerformanceLevelEXT) {
		return;
	}
	if (env && env[0]) {
		if (strcmp(env, "0") == 0 || SDL_strcasecmp(env, "off") == 0) {
			return;
		} else if (SDL_strcasecmp(env, "low") == 0) {
			level = XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT;
		} else if (SDL_strcasecmp(env, "boost") == 0) {
			level = XR_PERF_SETTINGS_LEVEL_BOOST_EXT;
		}
	}
	{
		const XrResult gpu = g_xr.xrPerfSettingsSetPerformanceLevelEXT(g_xr.session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, level);
		const XrResult cpu = g_xr.xrPerfSettingsSetPerformanceLevelEXT(g_xr.session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, level);
		Aeron_LogInfo("aeron.xr", "performance level %d: GPU %s, CPU %s", (int)level, Aeron_XrResultName(gpu),
					  Aeron_XrResultName(cpu));
	}
}

/* AERON_XR_REFRESH_RATE: display rate in Hz (0 = the headset's setting).
 * The nearest offered rate is used. */
static void Aeron_XrApplyRefreshRate(void) {
	const char* env = SDL_getenv("AERON_XR_REFRESH_RATE");
	float       want = env && env[0] ? (float)SDL_atof(env) : 0.0f;
	float       rates[16];
	uint32_t    count = 0;
	float       best  = 0.0f;
	if (want <= 0.0f || !g_xr.has_refresh_rate || !g_xr.xrEnumerateDisplayRefreshRatesFB ||
		!g_xr.xrRequestDisplayRefreshRateFB) {
		return;
	}
	if (XR_FAILED(g_xr.xrEnumerateDisplayRefreshRatesFB(g_xr.session, 16, &count, rates)) || count == 0) {
		return;
	}
	for (uint32_t i = 0; i < count; ++i) {
		if (best == 0.0f || SDL_fabsf(rates[i] - want) < SDL_fabsf(best - want)) {
			best = rates[i];
		}
	}
	Aeron_LogInfo("aeron.xr", "display refresh %.0f Hz requested: %s", best,
				  Aeron_XrResultName(g_xr.xrRequestDisplayRefreshRateFB(g_xr.session, best)));
}

/* Re-applies the two settings above when their variables change (the VR
 * options write them while the session runs). */
static char g_perf_applied[32];
static char g_refresh_applied[32];
static int  g_settings_dirty = 1;

static void Aeron_XrSyncSettings(void) {
	const char* perf    = SDL_getenv("AERON_XR_PERF_LEVEL");
	const char* refresh = SDL_getenv("AERON_XR_REFRESH_RATE");
	perf                = perf ? perf : "";
	refresh             = refresh ? refresh : "";
	if (g_settings_dirty || strcmp(perf, g_perf_applied) != 0) {
		SDL_strlcpy(g_perf_applied, perf, sizeof g_perf_applied);
		Aeron_XrApplyPerfLevel();
	}
	if (g_settings_dirty || strcmp(refresh, g_refresh_applied) != 0) {
		SDL_strlcpy(g_refresh_applied, refresh, sizeof g_refresh_applied);
		Aeron_XrApplyRefreshRate();
	}
	g_settings_dirty = 0;
}

int Aeron_XrConfigureDeviceProperties(SDL_PropertiesID properties) {
	if (!Aeron_XrRequested()) {
		return 0;
	}
	SDL_zero(g_xr);
	Aeron_XrPickExtensions();
	if (g_xr_api_version) {
		SDL_SetNumberProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_XR_VERSION_NUMBER, (Sint64)g_xr_api_version);
	}
	if (g_xr.extension_count) {
		SDL_SetNumberProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_XR_EXTENSION_COUNT_NUMBER, g_xr.extension_count);
		SDL_SetPointerProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_XR_EXTENSION_NAMES_POINTER, (void*)g_xr.extensions);
	}
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

/* SDL creates the instance and only says that it failed (the Steam Frame's
 * runtime turned it down without a word). Create test instances the same
 * way to log the runtime's answer and find what it rejects: each optional
 * extension on its own, then the API version. */
typedef struct AeronXrAndroidCreateInfo {
	XrStructureType type;
	const void*     next;
	void*           applicationVM;
	void*           applicationActivity;
} AeronXrAndroidCreateInfo;

static XrResult Aeron_XrTryInstance(PFN_xrGetInstanceProcAddr gipa, PFN_xrCreateInstance create, XrVersion api,
									const char* const* optional, uint32_t optional_count) {
	const char*          names[24];
	uint32_t             n = 0;
	XrInstanceCreateInfo info;
	XrInstance           instance = XR_NULL_HANDLE;
	XrResult             result;
	SDL_zero(info);
	/* What SDL adds itself: its graphics binding (Vulkan here) and, on
	 * Android, the create-instance extension. */
	names[n++] = "XR_KHR_vulkan_enable2";
#if defined(SDL_PLATFORM_ANDROID)
	names[n++] = "XR_KHR_android_create_instance";
	AeronXrAndroidCreateInfo android;
	JNIEnv*                  env = (JNIEnv*)SDL_GetAndroidJNIEnv();
	JavaVM*                  vm  = NULL;
	jobject                  activity = (jobject)SDL_GetAndroidActivity();
	if (env) {
		(*env)->GetJavaVM(env, &vm);
	}
	SDL_zero(android);
	android.type                = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
	android.applicationVM       = vm;
	android.applicationActivity = activity;
	info.next                   = &android;
#endif
	for (uint32_t i = 0; i < optional_count && n < SDL_arraysize(names); ++i) {
		names[n++] = optional[i];
	}
	info.type                       = XR_TYPE_INSTANCE_CREATE_INFO;
	info.applicationInfo.apiVersion = api;
	SDL_strlcpy(info.applicationInfo.applicationName, "OpenXWA", XR_MAX_APPLICATION_NAME_SIZE);
	info.applicationInfo.applicationVersion = 1;
	SDL_strlcpy(info.applicationInfo.engineName, "Aeron", XR_MAX_ENGINE_NAME_SIZE);
	info.applicationInfo.engineVersion = 1;
	info.enabledExtensionCount         = n;
	info.enabledExtensionNames         = names;
	result                             = create(&info, &instance);
	if (XR_SUCCEEDED(result) && instance != XR_NULL_HANDLE) {
		PFN_xrDestroyInstance destroy = NULL;
		if (XR_SUCCEEDED(gipa(instance, "xrDestroyInstance", (PFN_xrVoidFunction*)&destroy)) && destroy) {
			destroy(instance);
		}
	}
#if defined(SDL_PLATFORM_ANDROID)
	if (env && activity) {
		(*env)->DeleteLocalRef(env, activity);
	}
#endif
	return result;
}

int Aeron_XrRecoverInstanceFailure(void) {
	static int                 tried;
	PFN_xrGetInstanceProcAddr  gipa;
	PFN_xrCreateInstance       create = NULL;
	const char*                opt[16];
	const char*                keep[16];
	uint32_t                   count = g_xr.extension_count;
	uint32_t                   kept  = 0;
	XrResult                   r;
	const XrVersion            v10 = XR_MAKE_VERSION(1, 0, 0);
	const XrVersion            v11 = XR_MAKE_VERSION(1, 1, 0);
	XrVersion                  api = v10;

	if (tried) {
		return 0;
	}
	tried = 1;
	gipa  = (PFN_xrGetInstanceProcAddr)SDL_OpenXR_GetXrGetInstanceProcAddr();
	if (!gipa || XR_FAILED(gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&create)) || !create) {
		Aeron_LogWarn("aeron.xr", "instance check: no xrCreateInstance");
		return 0;
	}
	for (uint32_t i = 0; i < count; ++i) {
		opt[i] = g_xr.extensions[i];
	}
	r = Aeron_XrTryInstance(gipa, create, v10, opt, count);
	Aeron_LogWarn("aeron.xr", "instance check: OpenXR 1.0 with the %u optional extensions: %s", count,
				  Aeron_XrResultName(r));
	if (XR_SUCCEEDED(r)) {
		return 0; /* The instance is fine; SDL failed after it. */
	}
	r = Aeron_XrTryInstance(gipa, create, v10, NULL, 0);
	Aeron_LogWarn("aeron.xr", "instance check: OpenXR 1.0 without them: %s", Aeron_XrResultName(r));
	if (XR_FAILED(r)) {
		r = Aeron_XrTryInstance(gipa, create, v11, NULL, 0);
		Aeron_LogWarn("aeron.xr", "instance check: OpenXR 1.1 without them: %s", Aeron_XrResultName(r));
		if (XR_FAILED(r)) {
			return 0;
		}
		api = v11;
	}
	for (uint32_t i = 0; i < count; ++i) {
		if (strcmp(opt[i], XR_EXT_UUID_EXTENSION_NAME) == 0) {
			continue; /* goes with XR_EXT_render_model */
		}
		if (strcmp(opt[i], XR_EXT_INTERACTION_RENDER_MODEL_EXTENSION_NAME) == 0) {
			/* Needs XR_EXT_render_model (and that XR_EXT_uuid). */
			const char* deps[3] = { XR_EXT_RENDER_MODEL_EXTENSION_NAME, XR_EXT_UUID_EXTENSION_NAME, opt[i] };
			r = Aeron_XrTryInstance(gipa, create, api, deps, 3);
			if (XR_FAILED(r)) {
				const char* no_uuid[2] = { XR_EXT_RENDER_MODEL_EXTENSION_NAME, opt[i] };
				r = Aeron_XrTryInstance(gipa, create, api, no_uuid, 2);
			} else {
				g_xr_add_uuid = 1;
			}
			Aeron_LogWarn("aeron.xr", "instance check: %s (with XR_EXT_render_model): %s", opt[i],
						  Aeron_XrResultName(r));
			if (XR_SUCCEEDED(r)) {
				keep[kept++] = opt[i];
			} else if (g_xr_dropped_count < (int)SDL_arraysize(g_xr_dropped)) {
				SDL_strlcpy(g_xr_dropped[g_xr_dropped_count++], opt[i], XR_MAX_EXTENSION_NAME_SIZE);
			}
			continue;
		}
		r = Aeron_XrTryInstance(gipa, create, api, &opt[i], 1);
		if (XR_FAILED(r) && strcmp(opt[i], XR_EXT_RENDER_MODEL_EXTENSION_NAME) == 0) {
			const char* pair[2] = { opt[i], "XR_EXT_uuid" };
			const XrResult with_uuid = Aeron_XrTryInstance(gipa, create, api, pair, 2);
			Aeron_LogWarn("aeron.xr", "instance check: %s with XR_EXT_uuid: %s", opt[i],
						  Aeron_XrResultName(with_uuid));
			if (XR_SUCCEEDED(with_uuid)) {
				g_xr_add_uuid = 1;
				r             = with_uuid;
			}
		}
		Aeron_LogWarn("aeron.xr", "instance check: %s: %s", opt[i], Aeron_XrResultName(r));
		if (XR_SUCCEEDED(r)) {
			keep[kept++] = opt[i];
		} else if (g_xr_dropped_count < (int)SDL_arraysize(g_xr_dropped)) {
			SDL_strlcpy(g_xr_dropped[g_xr_dropped_count++], opt[i], XR_MAX_EXTENSION_NAME_SIZE);
		}
	}
	{
		const char* both[18];
		uint32_t    n = 0;
		for (uint32_t i = 0; i < kept; ++i) {
			both[n++] = keep[i];
		}
		if (g_xr_add_uuid) {
			both[n++] = "XR_EXT_uuid";
		}
		r = Aeron_XrTryInstance(gipa, create, api, both, n);
	}
	Aeron_LogWarn("aeron.xr", "instance check: OpenXR 1.%d with the %u accepted extensions: %s",
				  api == v11 ? 1 : 0, kept, Aeron_XrResultName(r));
	if (XR_FAILED(r)) {
		/* Each works alone but not together: go without any. */
		for (uint32_t i = 0; i < kept && g_xr_dropped_count < (int)SDL_arraysize(g_xr_dropped); ++i) {
			SDL_strlcpy(g_xr_dropped[g_xr_dropped_count++], keep[i], XR_MAX_EXTENSION_NAME_SIZE);
		}
		g_xr_add_uuid = 0;
	}
	g_xr_api_version = api == v11 ? v11 : 0;
	return 1;
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
#define AERON_XR_LOAD_OPTIONAL(fn)                                                                           \
	if (XR_FAILED(g_xr.xrGetInstanceProcAddr(g_xr.instance, #fn, (PFN_xrVoidFunction*)&g_xr.fn))) {          \
		g_xr.fn = NULL;                                                                                      \
	}
	if (g_xr.has_perf_settings) {
		AERON_XR_LOAD_OPTIONAL(xrPerfSettingsSetPerformanceLevelEXT);
	}
	if (g_xr.has_refresh_rate) {
		AERON_XR_LOAD_OPTIONAL(xrEnumerateDisplayRefreshRatesFB);
		AERON_XR_LOAD_OPTIONAL(xrRequestDisplayRefreshRateFB);
	}
	AERON_XR_LOAD_OPTIONAL(xrGetCurrentInteractionProfile);
	AERON_XR_LOAD_OPTIONAL(xrPathToString);
#undef AERON_XR_LOAD_OPTIONAL
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
#if defined(SDL_PLATFORM_ANDROID)
		g_xr.quads_over_projection = strstr(instance_properties.runtimeName, "SteamVR") != NULL;
#endif
	}
	{
		const char* v = SDL_getenv("AERON_XR_EYE_OVERLAY");
		if (v && v[0]) {
			g_xr.quads_over_projection = v[0] == '0';
		}
		if (g_xr.quads_over_projection) {
			Aeron_LogInfo("aeron.xr", "panels are composited over the 3D view here: no hand overlay layer");
		}
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
	{
		/* Meta's runtime reports 32 but fails xrEndFrame with
		 * XR_ERROR_LAYER_LIMIT_EXCEEDED past 16 layers; AERON_XR_MAX_LAYERS
		 * overrides the budget (the runtime's figure stays the ceiling). */
		const char* env    = SDL_getenv("AERON_XR_MAX_LAYERS");
		uint32_t    budget = env && env[0] ? (uint32_t)SDL_atoi(env) : 16u;
		if (budget < XR_MIN_COMPOSITION_LAYERS_SUPPORTED) {
			budget = XR_MIN_COMPOSITION_LAYERS_SUPPORTED;
		}
		if (budget < g_xr.max_layers) {
			g_xr.max_layers = budget;
		}
		Aeron_LogInfo("aeron.xr", "composition layer budget: %u", g_xr.max_layers);
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
	g_xr.local_pose.orientation.w = 1.0f;
	if (XR_FAILED(g_xr.xrCreateReferenceSpace(g_xr.session, &space_info, &g_xr.base_space))) {
		g_xr.base_space = XR_NULL_HANDLE;
	}
	{
		XrReferenceSpaceCreateInfo view_info = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO, NULL };
		view_info.referenceSpaceType         = XR_REFERENCE_SPACE_TYPE_VIEW;
		view_info.poseInReferenceSpace.orientation.w = 1.0f;
		if (XR_FAILED(g_xr.xrCreateReferenceSpace(g_xr.session, &view_info, &g_xr.view_space))) {
			g_xr.view_space = XR_NULL_HANDLE;
		}
	}
	/* Face the screen at start: whatever way the headset was recentred
	 * last, the view starts looking at it (AERON_XR_RECENTER_ON_START=0
	 * keeps the system's LOCAL space). */
	{
		const char* v = SDL_getenv("AERON_XR_RECENTER_ON_START");
		g_xr.recenter_pending = !(v && v[0] == '0');
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
	g_xr.screen_distance_m      = Aeron_XrEnvFloat("AERON_XR_SCREEN_DISTANCE", 2.1f);
	g_xr.screen_width_m         = Aeron_XrEnvFloat("AERON_XR_SCREEN_WIDTH", 2.1f);
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
	Aeron_XrRenderModelsShutdown();
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
		if (g_xr.screen_right_texture) {
			SDL_ReleaseGPUTexture(g_xr.device, g_xr.screen_right_texture);
		}
		if (g_xr.screen_right_swapchain) {
			SDL_DestroyGPUXRSwapchain(g_xr.device, g_xr.screen_right_swapchain, g_xr.screen_right_images);
		}
	}
	if (g_xr.view_space && g_xr.xrDestroySpace) {
		g_xr.xrDestroySpace(g_xr.view_space);
		g_xr.view_space = XR_NULL_HANDLE;
	}
	if (g_xr.local_space && g_xr.xrDestroySpace) {
		g_xr.xrDestroySpace(g_xr.local_space);
	}
	if (g_xr.base_space && g_xr.xrDestroySpace) {
		g_xr.xrDestroySpace(g_xr.base_space);
		g_xr.base_space = XR_NULL_HANDLE;
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
					g_settings_dirty = 1;
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
		} else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
			/* The system recentred LOCAL on the head itself: drop our offset
			 * so the two don't add up. */
			const XrEventDataReferenceSpaceChangePending* change =
				(const XrEventDataReferenceSpaceChangePending*)&event;
			if (change->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL && g_xr.session) {
				XrPosef identity;
				SDL_zero(identity);
				identity.orientation.w = 1.0f;
				Aeron_XrSetLocalPose(&identity, g_xr.predicted_display_time);
				g_xr.recenter_pending = 0;
				Aeron_LogInfo("aeron.xr", "system recentre: view offset cleared");
			}
		} else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
			g_xr.session_running = 0;
			g_xr.exit_requested  = 1;
		} else if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
			Aeron_XrInputLogProfiles();
			Aeron_XrRenderModelsDirty();
		} else if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_RENDER_MODELS_CHANGED_EXT) {
			Aeron_XrRenderModelsDirty();
		}
		event.type = XR_TYPE_EVENT_DATA_BUFFER;
		event.next = NULL;
	}
	if (g_xr.session_running) {
		Aeron_XrSyncSettings();
	}
	return !g_xr.exit_requested;
}

int Aeron_XrFrameBegun(void) { return g_xr.frame_begun; }

int Aeron_XrShouldRender(void) { return g_xr.frame_begun && g_xr.should_render; }

static XrVector3f Aeron_XrRotate(XrQuaternionf q, XrVector3f v) {
	const float tx = 2.0f * (q.y * v.z - q.z * v.y), ty = 2.0f * (q.z * v.x - q.x * v.z),
				tz = 2.0f * (q.x * v.y - q.y * v.x);
	XrVector3f r;
	r.x = v.x + q.w * tx + (q.y * tz - q.z * ty);
	r.y = v.y + q.w * ty + (q.z * tx - q.x * tz);
	r.z = v.z + q.w * tz + (q.x * ty - q.y * tx);
	return r;
}

/* a * b: b's pose expressed in the space a is in. */
static XrPosef Aeron_XrPoseCompose(XrPosef a, XrPosef b) {
	const XrQuaternionf p = a.orientation, q = b.orientation;
	const XrVector3f    t = Aeron_XrRotate(p, b.position);
	XrPosef             r;
	r.position.x    = a.position.x + t.x;
	r.position.y    = a.position.y + t.y;
	r.position.z    = a.position.z + t.z;
	r.orientation.w = p.w * q.w - p.x * q.x - p.y * q.y - p.z * q.z;
	r.orientation.x = p.w * q.x + p.x * q.w + p.y * q.z - p.z * q.y;
	r.orientation.y = p.w * q.y - p.x * q.z + p.y * q.w + p.z * q.x;
	r.orientation.z = p.w * q.z + p.x * q.y - p.y * q.x + p.z * q.w;
	return r;
}

static XrPosef Aeron_XrPoseInverse(XrPosef a) {
	XrPosef    r;
	XrVector3f t;
	r.orientation.w = a.orientation.w;
	r.orientation.x = -a.orientation.x;
	r.orientation.y = -a.orientation.y;
	r.orientation.z = -a.orientation.z;
	t               = Aeron_XrRotate(r.orientation, a.position);
	r.position.x    = -t.x;
	r.position.y    = -t.y;
	r.position.z    = -t.z;
	return r;
}

static XrSpace Aeron_XrCreateLocal(const XrPosef* pose) {
	XrReferenceSpaceCreateInfo info = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO, NULL };
	XrSpace                    space = XR_NULL_HANDLE;
	info.referenceSpaceType          = XR_REFERENCE_SPACE_TYPE_LOCAL;
	info.poseInReferenceSpace        = *pose;
	if (XR_FAILED(g_xr.xrCreateReferenceSpace(g_xr.session, &info, &space))) {
		return XR_NULL_HANDLE;
	}
	return space;
}

/* Replace local_space by LOCAL offset by `pose`, LOCAL being where it was at
 * the start. A runtime may put each new LOCAL space somewhere else (the
 * Steam Frame at the head's current heading): where a fresh one lands is
 * checked against the start's, and the offset is corrected for it. */
static int Aeron_XrSetLocalPose(const XrPosef* pose, XrTime time) {
	XrPosef identity;
	XrPosef want = *pose;
	XrSpace space;
	SDL_zero(identity);
	identity.orientation.w = 1.0f;
	if (g_xr.base_space && time > 0) {
		XrSpace fresh = Aeron_XrCreateLocal(&identity);
		if (fresh) {
			XrSpaceLocation where = { XR_TYPE_SPACE_LOCATION, NULL };
			if (XR_SUCCEEDED(g_xr.xrLocateSpace(fresh, g_xr.base_space, time, &where)) &&
				(where.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
				(where.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
				const XrPosef h = where.pose;
				const float   moved = h.position.x * h.position.x + h.position.y * h.position.y +
									h.position.z * h.position.z;
				if (moved > 1.0e-4f || SDL_fabsf(h.orientation.w) < 0.99996f) {
					static int logged;
					if (!logged) {
						logged = 1;
						Aeron_LogInfo("aeron.xr",
									  "a new LOCAL space lands %.2f %.2f %.2f m / %.0f deg from the first; corrected",
									  (double)h.position.x, (double)h.position.y, (double)h.position.z,
									  (double)(2.0f * SDL_acosf(SDL_min(1.0f, SDL_fabsf(h.orientation.w))) *
											   57.29578f));
					}
					want = Aeron_XrPoseCompose(Aeron_XrPoseInverse(h), *pose);
				}
			}
			g_xr.xrDestroySpace(fresh);
		}
	}
	space = Aeron_XrCreateLocal(&want);
	if (!space) {
		return 0;
	}
	if (g_xr.local_space) {
		g_xr.xrDestroySpace(g_xr.local_space);
	}
	g_xr.local_space = space;
	g_xr.local_pose  = *pose;
	return 1;
}

/* Recentre: the head's current position and heading (yaw only, the
 * horizon stays level) become the origin and the forward direction. */
static int Aeron_XrRecenterNow(XrTime time) {
	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION, NULL };
	if (!g_xr.view_space || XR_FAILED(g_xr.xrLocateSpace(g_xr.view_space, g_xr.local_space, time, &location)) ||
		!(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
		!(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
		return 0;
	}
	const XrQuaternionf q = location.pose.orientation;
	/* The head's forward (-Z) in the current local space, flattened. */
	const float fx = -(2.0f * (q.x * q.z + q.w * q.y));
	const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
	if (fx * fx + fz * fz < 1.0e-6f) {
		return 0; /* looking straight up or down */
	}
	const float yaw = SDL_atan2f(-fx, -fz); /* rotation about +Y from -Z to forward */
	XrPosef delta;
	delta.orientation.x = 0.0f;
	delta.orientation.y = SDL_sinf(0.5f * yaw);
	delta.orientation.z = 0.0f;
	delta.orientation.w = SDL_cosf(0.5f * yaw);
	delta.position      = location.pose.position;
	/* Compose with the current offset: new = local_pose * delta. */
	const XrQuaternionf a = g_xr.local_pose.orientation, b = delta.orientation;
	float rp[3];
	{
		const float vx = delta.position.x, vy = delta.position.y, vz = delta.position.z;
		const float tx = 2.0f * (a.y * vz - a.z * vy), ty = 2.0f * (a.z * vx - a.x * vz),
					tz = 2.0f * (a.x * vy - a.y * vx);
		rp[0] = vx + a.w * tx + (a.y * tz - a.z * ty);
		rp[1] = vy + a.w * ty + (a.z * tx - a.x * tz);
		rp[2] = vz + a.w * tz + (a.x * ty - a.y * tx);
	}
	XrPosef pose;
	pose.position.x    = g_xr.local_pose.position.x + rp[0];
	pose.position.y    = g_xr.local_pose.position.y + rp[1];
	pose.position.z    = g_xr.local_pose.position.z + rp[2];
	pose.orientation.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
	pose.orientation.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
	pose.orientation.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
	pose.orientation.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
	if (!Aeron_XrSetLocalPose(&pose, time)) {
		return 0;
	}
	Aeron_LogInfo("aeron.xr", "view recentred (turned %.0f deg, moved %.2f %.2f %.2f m)",
				  (double)(yaw * 57.29578f), (double)delta.position.x, (double)delta.position.y,
				  (double)delta.position.z);
	return 1;
}

void Aeron_XrRequestRecenter(void) { g_xr.recenter_pending = 1; }

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
	g_xr.screen_right_acquired    = 0;
	g_xr.screen_right_this_frame  = 0;
	g_xr.predicted_display_time   = frame_state.predictedDisplayTime;
	g_xr.predicted_display_period = frame_state.predictedDisplayPeriod;
	g_xr.should_render            = frame_state.shouldRender;
	/* Recentre once tracking has settled (a few focused frames). */
	if (g_xr.recenter_pending && g_xr.state == XR_SESSION_STATE_FOCUSED && ++g_xr.recenter_frames > 10 &&
		Aeron_XrRecenterNow(frame_state.predictedDisplayTime)) {
		g_xr.recenter_pending = 0;
		g_xr.recenter_frames  = 0;
	}
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

int Aeron_XrVirtualScreenGeometry(float* distance_m, float* width_m, float* height_m, float* height_offset_m) {
	if (!g_xr.screen_width || !g_xr.screen_height) {
		return 0;
	}
	if (distance_m)
		*distance_m = g_xr.screen_distance_m;
	if (width_m)
		*width_m = g_xr.screen_width_m;
	if (height_m)
		*height_m = g_xr.screen_width_m * (float)g_xr.screen_height / (float)g_xr.screen_width;
	if (height_offset_m)
		*height_offset_m = g_xr.screen_height_offset_m;
	return 1;
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

/* The right eye's virtual screen: the same size and format as the shared
 * one, created the first time a frame's 2D layers differ per eye. */
SDL_GPUTexture* Aeron_XrScreenRightTexture(void) {
	XrSwapchainCreateInfo swapchain_info = { XR_TYPE_SWAPCHAIN_CREATE_INFO, NULL };
	SDL_GPUTextureCreateInfo texture_info;
	XrResult result;

	if (g_xr.screen_right_texture || g_xr.screen_right_failed) {
		return g_xr.screen_right_texture;
	}
	if (!g_xr.device || !g_xr.session || !g_xr.screen_texture) {
		return NULL;
	}
	swapchain_info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	swapchain_info.sampleCount = 1;
	swapchain_info.width       = (uint32_t)g_xr.screen_width;
	swapchain_info.height      = (uint32_t)g_xr.screen_height;
	swapchain_info.faceCount   = 1;
	swapchain_info.arraySize   = 1;
	swapchain_info.mipCount    = 1;
	result = SDL_CreateGPUXRSwapchain(g_xr.device, g_xr.session, &swapchain_info, g_xr.screen_format,
									  &g_xr.screen_right_swapchain, &g_xr.screen_right_images);
	if (XR_FAILED(result)) {
		Aeron_LogError("aeron.xr", "right-eye screen swapchain: %s (%s)", Aeron_XrResultName(result),
					   SDL_GetError());
		g_xr.screen_right_swapchain = XR_NULL_HANDLE;
		g_xr.screen_right_failed    = 1;
		return NULL;
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
	g_xr.screen_right_texture         = SDL_CreateGPUTexture(g_xr.device, &texture_info);
	if (!g_xr.screen_right_texture) {
		Aeron_LogError("aeron.xr", "right-eye screen texture: %s", SDL_GetError());
		SDL_DestroyGPUXRSwapchain(g_xr.device, g_xr.screen_right_swapchain, g_xr.screen_right_images);
		g_xr.screen_right_swapchain = XR_NULL_HANDLE;
		g_xr.screen_right_images    = NULL;
		g_xr.screen_right_failed    = 1;
		return NULL;
	}
	SDL_SetGPUTextureName(g_xr.device, g_xr.screen_right_texture, "aeron.xr.screen.right");
	Aeron_LogInfo("aeron.xr", "right-eye virtual screen created (%dx%d)", g_xr.screen_width, g_xr.screen_height);
	return g_xr.screen_right_texture;
}

int Aeron_XrCopyScreenRightToSwapchain(SDL_GPUCommandBuffer* command_buffer) {
	XrSwapchainImageAcquireInfo acquire_info = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, NULL };
	XrSwapchainImageWaitInfo    wait_info    = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, NULL };
	SDL_GPUBlitInfo             blit;
	uint32_t                    image_index = 0;
	XrResult                    result;

	if (!Aeron_XrShouldRender() || !command_buffer || !g_xr.screen_right_texture || g_xr.screen_right_acquired) {
		return 0;
	}
	result = g_xr.xrAcquireSwapchainImage(g_xr.screen_right_swapchain, &acquire_info, &image_index);
	if (XR_FAILED(result)) {
		Aeron_LogError("aeron.xr", "xrAcquireSwapchainImage (right screen) failed: %s", Aeron_XrResultName(result));
		return 0;
	}
	g_xr.screen_right_acquired = 1;
	wait_info.timeout          = XR_INFINITE_DURATION;
	result                     = g_xr.xrWaitSwapchainImage(g_xr.screen_right_swapchain, &wait_info);
	if (XR_FAILED(result)) {
		Aeron_LogError("aeron.xr", "xrWaitSwapchainImage (right screen) failed: %s", Aeron_XrResultName(result));
		return 0;
	}
	SDL_zero(blit);
	blit.source.texture      = g_xr.screen_right_texture;
	blit.source.w            = (Uint32)g_xr.screen_width;
	blit.source.h            = (Uint32)g_xr.screen_height;
	blit.destination.texture = g_xr.screen_right_images[image_index];
	blit.destination.w       = (Uint32)g_xr.screen_width;
	blit.destination.h       = (Uint32)g_xr.screen_height;
	blit.load_op             = SDL_GPU_LOADOP_DONT_CARE;
	blit.filter              = SDL_GPU_FILTER_NEAREST;
	SDL_BlitGPUTexture(command_buffer, &blit);
	g_xr.screen_right_this_frame = 1;
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
static void Aeron_XrFillScreenQuad(XrCompositionLayerQuad* quad, XrEyeVisibility eye) {
	const float aspect                     = (float)g_xr.screen_height / (float)g_xr.screen_width;
	quad->space                            = g_xr.local_space;
	quad->eyeVisibility                    = eye;
	quad->subImage.swapchain = eye == XR_EYE_VISIBILITY_RIGHT ? g_xr.screen_right_swapchain : g_xr.screen_swapchain;
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
	XrCompositionLayerQuad              quad_right   = { XR_TYPE_COMPOSITION_LAYER_QUAD, NULL };
	const XrCompositionLayerBaseHeader* screens[2]   = { (const XrCompositionLayerBaseHeader*)&quad,
														 (const XrCompositionLayerBaseHeader*)&quad_right };
	int                                 stereo_screen;
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
	const int screen_composed = g_xr.image_acquired;
	if (g_xr.image_acquired) {
		result = g_xr.xrReleaseSwapchainImage(g_xr.screen_swapchain, &release_info);
		if (XR_SUCCEEDED(result)) {
			g_xr.screen_has_released_image = 1;
		} else {
			Aeron_LogError("aeron.xr", "xrReleaseSwapchainImage failed: %s", Aeron_XrResultName(result));
		}
		g_xr.image_acquired = 0;
	}
	if (g_xr.screen_right_acquired) {
		result = g_xr.xrReleaseSwapchainImage(g_xr.screen_right_swapchain, &release_info);
		if (XR_SUCCEEDED(result)) {
			g_xr.screen_right_has_released_image = 1;
		} else {
			Aeron_LogError("aeron.xr", "xrReleaseSwapchainImage (right screen) failed: %s",
						   Aeron_XrResultName(result));
			g_xr.screen_right_this_frame = 0;
		}
		g_xr.screen_right_acquired = 0;
	}
	/* Each eye sees its own screen image when this frame composed one; a
	 * frame that composed nothing keeps showing the last frame's images. */
	if (screen_composed) {
		g_xr.screen_stereo_last = g_xr.screen_right_this_frame && g_xr.screen_right_has_released_image;
	}
	stereo_screen                = g_xr.screen_stereo_last && g_xr.screen_right_has_released_image;
	g_xr.screen_right_this_frame = 0;

	end_info.displayTime          = g_xr.predicted_display_time;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end_info.layerCount           = 0;
	end_info.layers               = layers;
	screen_overlay                = g_xr.should_render && Aeron_XrStereoScreenOverlay() &&
					 g_xr.screen_has_released_image;
	if (screen_overlay) {
		/* Backdrop frame: the virtual screen over the environment. */
		Aeron_XrFillScreenQuad(&quad, stereo_screen ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_BOTH);
		if (stereo_screen) {
			Aeron_XrFillScreenQuad(&quad_right, XR_EYE_VISIBILITY_RIGHT);
		}
	}
	stereo_layers = g_xr.should_render
						? Aeron_XrStereoEndFrameLayers(
							  layers, g_xr.max_layers < AERON_XR_MAX_LAYERS ? g_xr.max_layers : AERON_XR_MAX_LAYERS,
							  screens, screen_overlay ? (stereo_screen ? 2 : 1) : 0)
						: 0;
	if (!g_xr.should_render) {
		Aeron_XrStereoEndFrameLayers(layers, 0, NULL, 0); /* release anything acquired */
	}
	if (stereo_layers > 0) {
		end_info.layerCount = stereo_layers;
	} else if (g_xr.should_render && g_xr.screen_has_released_image) {
		Aeron_XrFillScreenQuad(&quad, stereo_screen ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_BOTH);
		layers[0]           = (const XrCompositionLayerBaseHeader*)&quad;
		end_info.layerCount = 1;
		if (stereo_screen && g_xr.max_layers >= 2) {
			Aeron_XrFillScreenQuad(&quad_right, XR_EYE_VISIBILITY_RIGHT);
			layers[1]           = (const XrCompositionLayerBaseHeader*)&quad_right;
			end_info.layerCount = 2;
		} else if (stereo_screen) {
			quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		}
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
