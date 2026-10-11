/*
 * OpenXR stereo presentation for Aeron: per-eye swapchains + a HUD quad.
 * See include/aeron/xr.h for the game-facing protocol.
 */

#if defined(AERON_HAS_OPENXR)
#include <openxr/openxr.h>
#endif

#include "aeron/xr.h"
#include "xr.h"

#include "aeron/log.h"
#include "internal.h"

#include <string.h>

#if !defined(AERON_HAS_OPENXR)

int Aeron_XrStereoBegin(AeronXrStereoFrame* out) {
	(void)out;
	return 0;
}

void Aeron_XrStereoCommit(const AeronXrHudQuad* hud) { (void)hud; }

int Aeron_XrStereoCommitted(void) { return 0; }

int Aeron_XrStereoCopyToSwapchains(SDL_GPUCommandBuffer* c) {
	(void)c;
	return 0;
}

SDL_GPUTexture* Aeron_XrStereoTexture(int slot) {
	(void)slot;
	return NULL;
}

int Aeron_XrStereoHudVisible(void) { return 0; }

void Aeron_XrStereoSize(int* w, int* h) {
	if (w)
		*w = 0;
	if (h)
		*h = 0;
}

#else

#include "xr_private.h"

enum { AERON_XR_EYE_COUNT = 2, AERON_XR_STEREO_SLOTS = 3 /* 2 eyes + HUD */ };

typedef struct AeronXrStereoSwapchain {
	XrSwapchain        swapchain;
	SDL_GPUTexture**   images;
	AeronRenderTarget* target;
	int                acquired;
	int                released_once;
} AeronXrStereoSwapchain;

typedef struct AeronXrStereo {
	int                    ready;
	int                    width;
	int                    height;
	AeronTextureFormat     format;
	AeronXrStereoSwapchain chains[AERON_XR_STEREO_SLOTS];
	/* Per-frame */
	uint64_t       located_frame;
	int            located;
	XrView         views[AERON_XR_EYE_COUNT];
	int            committed;
	AeronXrHudQuad hud;
	/* Layer storage for xrEndFrame (must outlive the call) */
	XrCompositionLayerProjectionView proj_views[AERON_XR_EYE_COUNT];
	XrCompositionLayerProjection     projection;
	XrCompositionLayerQuad           hud_quad;
} AeronXrStereo;

static AeronXrStereo g_st;

int Aeron_XrStereoInit(void) {
	XrViewConfigurationView configs[AERON_XR_EYE_COUNT];
	uint32_t                count = 0;
	XrResult                result;
	float                   scale = 1.0f;
	int                     i;
	const char*             env = SDL_getenv("AERON_XR_STEREO");

	memset(&g_st, 0, sizeof g_st);
	g_st.located_frame = UINT64_MAX;
	if (env && env[0] == '0') {
		Aeron_LogInfo("aeron.xr", "stereo disabled by AERON_XR_STEREO=0 (virtual screen only)");
		return 0;
	}
	for (i = 0; i < AERON_XR_EYE_COUNT; ++i) {
		configs[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
		configs[i].next = NULL;
	}
	result = g_xr.xrEnumerateViewConfigurationViews(g_xr.instance, g_xr.system_id,
													XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
													AERON_XR_EYE_COUNT, &count, configs);
	if (XR_FAILED(result) || count != AERON_XR_EYE_COUNT) {
		Aeron_LogWarn("aeron.xr", "stereo view configuration unavailable: %s", Aeron_XrResultName(result));
		return 0;
	}
	{
		const char* s = SDL_getenv("AERON_XR_RESOLUTION_SCALE");
		if (s && s[0] && SDL_atof(s) > 0.1) {
			scale = (float)SDL_atof(s);
		}
	}
	g_st.width  = (int)((float)configs[0].recommendedImageRectWidth * scale + 0.5f);
	g_st.height = (int)((float)configs[0].recommendedImageRectHeight * scale + 0.5f);
	if (g_st.width > (int)configs[0].maxImageRectWidth) {
		g_st.width = (int)configs[0].maxImageRectWidth;
	}
	if (g_st.height > (int)configs[0].maxImageRectHeight) {
		g_st.height = (int)configs[0].maxImageRectHeight;
	}
	g_st.format = Aeron_FromSdlTextureFormat(g_xr.screen_format);
	if (g_st.width <= 0 || g_st.height <= 0 || g_st.format == AERON_TEXTURE_FORMAT_UNKNOWN) {
		Aeron_LogWarn("aeron.xr", "stereo disabled: unusable eye size/format");
		return 0;
	}

	for (i = 0; i < AERON_XR_STEREO_SLOTS; ++i) {
		static const char*    names[AERON_XR_STEREO_SLOTS] = { "aeron.xr.eye_left", "aeron.xr.eye_right",
															   "aeron.xr.hud" };
		XrSwapchainCreateInfo info                         = { XR_TYPE_SWAPCHAIN_CREATE_INFO, NULL };
		info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
		info.sampleCount = 1;
		info.width       = (uint32_t)g_st.width;
		info.height      = (uint32_t)g_st.height;
		info.faceCount   = 1;
		info.arraySize   = 1;
		info.mipCount    = 1;
		result           = SDL_CreateGPUXRSwapchain(g_xr.device, g_xr.session, &info, g_xr.screen_format,
													&g_st.chains[i].swapchain, &g_st.chains[i].images);
		if (XR_FAILED(result)) {
			Aeron_LogWarn("aeron.xr", "stereo swapchain %d failed: %s (%s)", i, Aeron_XrResultName(result),
						  SDL_GetError());
			Aeron_XrStereoShutdown();
			return 0;
		}
		g_st.chains[i].target = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
			.width      = g_st.width,
			.height     = g_st.height,
			.format     = g_st.format,
			.debug_name = names[i],
		});
		if (!g_st.chains[i].target) {
			Aeron_LogWarn("aeron.xr", "stereo render target %d failed", i);
			Aeron_XrStereoShutdown();
			return 0;
		}
	}
	g_st.ready = 1;
	Aeron_LogInfo("aeron.xr", "stereo ready: %dx%d per eye (recommended %ux%u, scale %.2f)", g_st.width,
				  g_st.height, configs[0].recommendedImageRectWidth, configs[0].recommendedImageRectHeight,
				  (double)scale);
	return 1;
}

void Aeron_XrStereoShutdown(void) {
	int i;
	for (i = 0; i < AERON_XR_STEREO_SLOTS; ++i) {
		if (g_st.chains[i].target) {
			Aeron_DestroyRenderTarget(g_st.chains[i].target);
		}
		if (g_st.chains[i].swapchain && g_xr.device) {
			SDL_DestroyGPUXRSwapchain(g_xr.device, g_st.chains[i].swapchain, g_st.chains[i].images);
		}
	}
	memset(&g_st, 0, sizeof g_st);
}

int Aeron_XrStereoBegin(AeronXrStereoFrame* out) {
	if (!out || !g_st.ready || !Aeron_XrShouldRender() ||
		(g_xr.state != XR_SESSION_STATE_VISIBLE && g_xr.state != XR_SESSION_STATE_FOCUSED)) {
		return 0;
	}
	if (g_st.located_frame != g_xr.frame_index) {
		XrViewLocateInfo locate = { XR_TYPE_VIEW_LOCATE_INFO, NULL };
		XrViewState      state  = { XR_TYPE_VIEW_STATE, NULL };
		uint32_t         count  = 0;
		int              i;
		g_st.located_frame           = g_xr.frame_index;
		g_st.located                 = 0;
		locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locate.displayTime           = g_xr.predicted_display_time;
		locate.space                 = g_xr.local_space;
		for (i = 0; i < AERON_XR_EYE_COUNT; ++i) {
			g_st.views[i].type = XR_TYPE_VIEW;
			g_st.views[i].next = NULL;
		}
		if (XR_SUCCEEDED(
				g_xr.xrLocateViews(g_xr.session, &locate, &state, AERON_XR_EYE_COUNT, &count, g_st.views)) &&
			count == AERON_XR_EYE_COUNT && (state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
			if (!(state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
				/* Orientation-only tracking: keep a fixed IPD around the origin. */
				g_st.views[0].pose.position = (XrVector3f) { -0.032f, 0.0f, 0.0f };
				g_st.views[1].pose.position = (XrVector3f) { 0.032f, 0.0f, 0.0f };
			}
			g_st.located = 1;
		}
	}
	if (!g_st.located) {
		return 0;
	}
	memset(out, 0, sizeof *out);
	for (int i = 0; i < AERON_XR_EYE_COUNT; ++i) {
		const XrView* v              = &g_st.views[i];
		out->views[i].orientation[0] = v->pose.orientation.x;
		out->views[i].orientation[1] = v->pose.orientation.y;
		out->views[i].orientation[2] = v->pose.orientation.z;
		out->views[i].orientation[3] = v->pose.orientation.w;
		out->views[i].position[0]    = v->pose.position.x;
		out->views[i].position[1]    = v->pose.position.y;
		out->views[i].position[2]    = v->pose.position.z;
		out->views[i].fov[0]         = v->fov.angleLeft;
		out->views[i].fov[1]         = v->fov.angleRight;
		out->views[i].fov[2]         = v->fov.angleUp;
		out->views[i].fov[3]         = v->fov.angleDown;
		out->eye_targets[i]          = g_st.chains[i].target;
	}
	out->hud_target = g_st.chains[2].target;
	out->width      = g_st.width;
	out->height     = g_st.height;
	out->format     = g_st.format;
	return 1;
}

void Aeron_XrStereoCommit(const AeronXrHudQuad* hud) {
	if (!g_st.ready || !g_st.located || g_st.located_frame != g_xr.frame_index) {
		return;
	}
	g_st.committed = 1;
	memset(&g_st.hud, 0, sizeof g_st.hud);
	if (hud) {
		g_st.hud = *hud;
	}
}

int Aeron_XrStereoCommitted(void) {
	return g_st.ready && g_st.committed && g_st.located_frame == g_xr.frame_index && Aeron_XrShouldRender();
}

SDL_GPUTexture* Aeron_XrStereoTexture(int slot) {
	if (slot < 0 || slot >= AERON_XR_STEREO_SLOTS || !g_st.chains[slot].target) {
		return NULL;
	}
	return Aeron_RenderTargetGetTexture(g_st.chains[slot].target)->texture;
}

int Aeron_XrStereoHudVisible(void) { return g_st.hud.visible; }

void Aeron_XrStereoSize(int* width, int* height) {
	if (width)
		*width = g_st.width;
	if (height)
		*height = g_st.height;
}

int Aeron_XrStereoCopyToSwapchains(SDL_GPUCommandBuffer* command_buffer) {
	int slot;
	int ok = 1;
	if (!Aeron_XrStereoCommitted() || !command_buffer) {
		return 0;
	}
	for (slot = 0; slot < AERON_XR_STEREO_SLOTS; ++slot) {
		AeronXrStereoSwapchain*     chain        = &g_st.chains[slot];
		XrSwapchainImageAcquireInfo acquire_info = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, NULL };
		XrSwapchainImageWaitInfo    wait_info    = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, NULL };
		SDL_GPUBlitInfo             blit;
		uint32_t                    index = 0;
		XrResult                    result;
		if (slot == 2 && !g_st.hud.visible) {
			continue;
		}
		result = g_xr.xrAcquireSwapchainImage(chain->swapchain, &acquire_info, &index);
		if (XR_FAILED(result)) {
			Aeron_LogError("aeron.xr", "stereo acquire %d failed: %s", slot, Aeron_XrResultName(result));
			ok = 0;
			continue;
		}
		chain->acquired   = 1;
		wait_info.timeout = XR_INFINITE_DURATION;
		result            = g_xr.xrWaitSwapchainImage(chain->swapchain, &wait_info);
		if (XR_FAILED(result)) {
			Aeron_LogError("aeron.xr", "stereo wait %d failed: %s", slot, Aeron_XrResultName(result));
			ok = 0;
			continue;
		}
		SDL_zero(blit);
		blit.source.texture      = Aeron_XrStereoTexture(slot);
		blit.source.w            = (Uint32)g_st.width;
		blit.source.h            = (Uint32)g_st.height;
		blit.destination.texture = chain->images[index];
		blit.destination.w       = (Uint32)g_st.width;
		blit.destination.h       = (Uint32)g_st.height;
		blit.load_op             = SDL_GPU_LOADOP_DONT_CARE;
		blit.filter              = SDL_GPU_FILTER_NEAREST;
		SDL_BlitGPUTexture(command_buffer, &blit);
	}
	return ok;
}

/* Releases acquired images and appends this frame's layers. Returns the
 * number of layers written (0 when the frame was not stereo). */
uint32_t Aeron_XrStereoEndFrameLayers(const XrCompositionLayerBaseHeader** layers, uint32_t capacity) {
	uint32_t count = 0;
	int      slot;
	int      eyes_ok = 1;

	for (slot = 0; slot < AERON_XR_STEREO_SLOTS; ++slot) {
		AeronXrStereoSwapchain* chain = &g_st.chains[slot];
		if (chain->acquired) {
			XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, NULL };
			if (XR_SUCCEEDED(g_xr.xrReleaseSwapchainImage(chain->swapchain, &release))) {
				chain->released_once = 1;
			}
			chain->acquired = 0;
		}
	}
	if (!Aeron_XrStereoCommitted()) {
		g_st.committed = 0;
		return 0;
	}
	g_st.committed = 0;
	eyes_ok        = g_st.chains[0].released_once && g_st.chains[1].released_once;
	if (!eyes_ok || capacity < 2) {
		return 0;
	}
	for (int eye = 0; eye < AERON_XR_EYE_COUNT; ++eye) {
		XrCompositionLayerProjectionView* view = &g_st.proj_views[eye];
		memset(view, 0, sizeof *view);
		view->type                             = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		view->pose                             = g_st.views[eye].pose;
		view->fov                              = g_st.views[eye].fov;
		view->subImage.swapchain               = g_st.chains[eye].swapchain;
		view->subImage.imageRect.extent.width  = g_st.width;
		view->subImage.imageRect.extent.height = g_st.height;
	}
	memset(&g_st.projection, 0, sizeof g_st.projection);
	g_st.projection.type      = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
	g_st.projection.space     = g_xr.local_space;
	g_st.projection.viewCount = AERON_XR_EYE_COUNT;
	g_st.projection.views     = g_st.proj_views;
	layers[count++]           = (const XrCompositionLayerBaseHeader*)&g_st.projection;

	if (g_st.hud.visible && g_st.chains[2].released_once) {
		memset(&g_st.hud_quad, 0, sizeof g_st.hud_quad);
		g_st.hud_quad.type                             = XR_TYPE_COMPOSITION_LAYER_QUAD;
		g_st.hud_quad.layerFlags                       = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		g_st.hud_quad.space                            = g_xr.local_space;
		g_st.hud_quad.eyeVisibility                    = XR_EYE_VISIBILITY_BOTH;
		g_st.hud_quad.subImage.swapchain               = g_st.chains[2].swapchain;
		g_st.hud_quad.subImage.imageRect.extent.width  = g_st.width;
		g_st.hud_quad.subImage.imageRect.extent.height = g_st.height;
		g_st.hud_quad.pose.orientation.w               = 1.0f;
		g_st.hud_quad.pose.position.x                  = g_st.hud.position[0];
		g_st.hud_quad.pose.position.y                  = g_st.hud.position[1];
		g_st.hud_quad.pose.position.z                  = g_st.hud.position[2];
		g_st.hud_quad.size.width                       = g_st.hud.size[0];
		g_st.hud_quad.size.height                      = g_st.hud.size[1];
		if (count < capacity) {
			layers[count++] = (const XrCompositionLayerBaseHeader*)&g_st.hud_quad;
		}
	}
	return count;
}

#endif /* AERON_HAS_OPENXR */
