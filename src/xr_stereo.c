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

#include <math.h>
#include <string.h>

#if !defined(AERON_HAS_OPENXR)

int Aeron_XrStereoBegin(AeronXrStereoFrame* out) {
	(void)out;
	return 0;
}

void Aeron_XrStereoCommit(const AeronXrHudQuad* hud) { (void)hud; }

void Aeron_XrStereoCommitHuds(const AeronXrHudQuad* hud, const AeronXrHudQuad* hud_world) {
	(void)hud;
	(void)hud_world;
}

void Aeron_XrStereoCommitPanels(const AeronXrHudQuad* hud_world, const AeronXrPanel* panels, int count) {
	(void)hud_world;
	(void)panels;
	(void)count;
}

void Aeron_XrStereoCommitConsole(const AeronXrPanel* console) { (void)console; }

int Aeron_XrStereoCommitted(void) { return 0; }

void Aeron_XrStereoCommitBackdrop(void) {}

int Aeron_XrStereoScreenOverlay(void) { return 0; }

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

enum {
	AERON_XR_EYE_COUNT    = 2,
	AERON_XR_SLOT_HUD     = 2,
	AERON_XR_SLOT_HUD_WORLD = 3,
	AERON_XR_SLOT_CONSOLE = 4,
	AERON_XR_SLOT_HUD_AUX = 5,
	AERON_XR_STEREO_SLOTS = 6 /* 2 eyes + head HUD + world HUD + console + second head HUD */
};

/* The console layer (a touch panel near the player's lap) has its own
 * fixed-size swapchain, independent of the eye size. */
#define AERON_XR_CONSOLE_WIDTH 1536
#define AERON_XR_CONSOLE_HEIGHT 512

typedef struct AeronXrStereoSwapchain {
	XrSwapchain        swapchain;
	int                width;
	int                height;
	SDL_GPUTexture**   images;
	AeronRenderTarget* target;
	int                acquired;
	int                released_once;
	/* Direct mode: the game renders straight into the acquired swapchain
	 * image (no intermediate target, no per-frame blit). */
	uint32_t           image_count;
	AeronRenderTarget* image_targets; /* one non-owning wrapper per image */
	uint32_t           index;         /* acquired image */
	uint64_t           direct_frame;  /* frame index the image was acquired for, 0 = none */
	int                waited;        /* the acquired image passed xrWaitSwapchainImage */
} AeronXrStereoSwapchain;

typedef struct AeronXrStereo {
	int                    ready;
	int                    direct;
	int                    width;
	int                    height;
	AeronTextureFormat     format;
	AeronXrStereoSwapchain chains[AERON_XR_STEREO_SLOTS];
	/* Per-frame */
	uint64_t       located_frame;
	int            located;
	XrView         views[AERON_XR_EYE_COUNT];
	int            committed;
	int            screen_overlay; /* committed as a backdrop to the virtual screen */
	AeronXrHudQuad hud;
	AeronXrHudQuad hud_world;
	/* Smoothed head-locked HUD pose (LOCAL space). */
	int        follow_valid;
	XrPosef    follow_pose;
	XrTime     follow_time;
	/* Layer storage for xrEndFrame (must outlive the call) */
	XrCompositionLayerProjectionView proj_views[AERON_XR_EYE_COUNT];
	XrCompositionLayerProjection     projection;
	XrCompositionLayerQuad           hud_quad;
	XrCompositionLayerQuad           hud_world_quad;
	/* Cockpit panels: parts of the HUD target as separate quads. */
	AeronXrPanel           panels[AERON_XR_MAX_PANELS];
	int                    panel_count;
	XrCompositionLayerQuad panel_quads[AERON_XR_MAX_PANELS];
	/* Console panel: a part of the console target as one more quad. */
	int                    console_visible;
	int                    hud_aux_visible; /* a panel shows part of the second head HUD */
	AeronXrPanel           console;
	XrCompositionLayerQuad console_quad;
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
															   "aeron.xr.hud", "aeron.xr.hud_world",
															   "aeron.xr.console", "aeron.xr.hud_aux" };
		XrSwapchainCreateInfo info                         = { XR_TYPE_SWAPCHAIN_CREATE_INFO, NULL };
		const int             optional = i == AERON_XR_SLOT_CONSOLE || i == AERON_XR_SLOT_HUD_AUX;
		g_st.chains[i].width  = i == AERON_XR_SLOT_CONSOLE ? AERON_XR_CONSOLE_WIDTH : g_st.width;
		g_st.chains[i].height = i == AERON_XR_SLOT_CONSOLE ? AERON_XR_CONSOLE_HEIGHT : g_st.height;
		info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
		if (g_xr.capture_frame_count > 0) {
			/* Lets AERON_XR_CAPTURE read images rendered in place. */
			info.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
		}
		info.sampleCount = 1;
		info.width       = (uint32_t)g_st.chains[i].width;
		info.height      = (uint32_t)g_st.chains[i].height;
		info.faceCount   = 1;
		info.arraySize   = 1;
		info.mipCount    = 1;
		result           = SDL_CreateGPUXRSwapchain(g_xr.device, g_xr.session, &info, g_xr.screen_format,
													&g_st.chains[i].swapchain, &g_st.chains[i].images);
		if (XR_FAILED(result)) {
			Aeron_LogWarn("aeron.xr", "stereo swapchain %d failed: %s (%s)", i, Aeron_XrResultName(result),
						  SDL_GetError());
			if (optional) {
				memset(&g_st.chains[i], 0, sizeof g_st.chains[i]);
				continue; /* no console layer */
			}
			Aeron_XrStereoShutdown();
			return 0;
		}
		{
			uint32_t count = 0;
			if (XR_SUCCEEDED(g_xr.xrEnumerateSwapchainImages(g_st.chains[i].swapchain, 0, &count, NULL)) &&
				count > 0) {
				g_st.chains[i].image_targets = (AeronRenderTarget*)SDL_calloc(count, sizeof(AeronRenderTarget));
				if (g_st.chains[i].image_targets) {
					g_st.chains[i].image_count = count;
					for (uint32_t k = 0; k < count; ++k) {
						AeronTexture* t = &g_st.chains[i].image_targets[k].color;
						t->texture      = g_st.chains[i].images[k];
						t->width        = g_st.chains[i].width;
						t->height       = g_st.chains[i].height;
						t->mip_count    = 1;
						t->layer_count  = 1;
						t->format       = g_st.format;
						t->usage        = AERON_TEXTURE_USAGE_COLOR_TARGET;
						t->sample_count = AERON_SAMPLE_COUNT_1;
						t->owned        = 0;
					}
				}
			}
		}
		g_st.chains[i].target = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
			.width      = g_st.chains[i].width,
			.height     = g_st.chains[i].height,
			.format     = g_st.format,
			.debug_name = names[i],
		});
		if (!g_st.chains[i].target) {
			Aeron_LogWarn("aeron.xr", "stereo render target %d failed", i);
			Aeron_XrStereoShutdown();
			return 0;
		}
	}
	g_st.ready  = 1;
	g_st.direct = 1;
	for (i = 0; i < AERON_XR_STEREO_SLOTS; ++i) {
		if (g_st.chains[i].swapchain && !g_st.chains[i].image_count) {
			g_st.direct = 0;
		}
	}
	{
		const char* direct = SDL_getenv("AERON_XR_DIRECT");
		if (direct && direct[0] == '0') {
			g_st.direct = 0;
		}
	}
	Aeron_LogInfo("aeron.xr", "stereo ready: %dx%d per eye (recommended %ux%u, scale %.2f), %s", g_st.width,
				  g_st.height, configs[0].recommendedImageRectWidth, configs[0].recommendedImageRectHeight,
				  (double)scale, g_st.direct ? "rendering into swapchain images" : "copying to swapchain images");
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
		SDL_free(g_st.chains[i].image_targets);
	}
	memset(&g_st, 0, sizeof g_st);
}

/* The target the game renders slot `slot` into this frame. */
static AeronRenderTarget* Aeron_XrStereoSlotTarget(int slot) {
	AeronXrStereoSwapchain* chain = &g_st.chains[slot];
	if (chain->acquired && chain->direct_frame == g_xr.frame_index && chain->direct_frame != 0) {
		return &chain->image_targets[chain->index];
	}
	return chain->target;
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
	if (g_st.direct) {
		/* Acquire every image up front; a slot that cannot be acquired falls
		 * back to its intermediate target and the blit for this frame. */
		for (int slot = 0; slot < AERON_XR_STEREO_SLOTS; ++slot) {
			AeronXrStereoSwapchain* chain = &g_st.chains[slot];
			if (chain->acquired || !chain->swapchain) {
				continue;
			}
			XrSwapchainImageAcquireInfo acquire_info = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, NULL };
			XrSwapchainImageWaitInfo    wait_info    = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, NULL };
			uint32_t                    index        = 0;
			chain->direct_frame                      = 0;
			chain->waited                            = 0;
			if (XR_FAILED(g_xr.xrAcquireSwapchainImage(chain->swapchain, &acquire_info, &index))) {
				continue;
			}
			chain->acquired   = 1;
			wait_info.timeout = XR_INFINITE_DURATION;
			/* Always wait: releasing an acquired image that was never waited
			 * on is a call-order error that would wedge the swapchain. */
			if (XR_SUCCEEDED(g_xr.xrWaitSwapchainImage(chain->swapchain, &wait_info))) {
				chain->index  = index;
				chain->waited = 1;
				if (index < chain->image_count) {
					chain->direct_frame = g_xr.frame_index;
				}
			}
		}
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
		out->eye_targets[i]          = Aeron_XrStereoSlotTarget(i);
	}
	out->hud_target       = Aeron_XrStereoSlotTarget(AERON_XR_SLOT_HUD);
	out->hud_world_target = Aeron_XrStereoSlotTarget(AERON_XR_SLOT_HUD_WORLD);
	if (g_st.chains[AERON_XR_SLOT_HUD_AUX].swapchain) {
		out->hud_aux_target = Aeron_XrStereoSlotTarget(AERON_XR_SLOT_HUD_AUX);
	}
	if (g_st.chains[AERON_XR_SLOT_CONSOLE].swapchain) {
		out->console_target = Aeron_XrStereoSlotTarget(AERON_XR_SLOT_CONSOLE);
		out->console_width  = g_st.chains[AERON_XR_SLOT_CONSOLE].width;
		out->console_height = g_st.chains[AERON_XR_SLOT_CONSOLE].height;
	}
	out->width      = g_st.width;
	out->height     = g_st.height;
	out->format     = g_st.format;
	return 1;
}

void Aeron_XrStereoCommitHuds(const AeronXrHudQuad* hud, const AeronXrHudQuad* hud_world) {
	if (!g_st.ready || !g_st.located || g_st.located_frame != g_xr.frame_index) {
		return;
	}
	g_st.committed       = 1;
	g_st.screen_overlay  = 0;
	g_st.panel_count     = 0;
	g_st.console_visible = 0;
	g_st.hud_aux_visible = 0;
	memset(&g_st.hud, 0, sizeof g_st.hud);
	memset(&g_st.hud_world, 0, sizeof g_st.hud_world);
	if (hud) {
		g_st.hud = *hud;
	}
	if (hud_world) {
		g_st.hud_world = *hud_world;
	}
}

void Aeron_XrStereoCommit(const AeronXrHudQuad* hud) { Aeron_XrStereoCommitHuds(hud, NULL); }

void Aeron_XrStereoCommitPanels(const AeronXrHudQuad* hud_world, const AeronXrPanel* panels, int count) {
	Aeron_XrStereoCommitHuds(NULL, hud_world);
	if (!g_st.committed || !panels || count <= 0) {
		return;
	}
	if (count > AERON_XR_MAX_PANELS) {
		count = AERON_XR_MAX_PANELS;
	}
	for (int i = 0; i < count; ++i) {
		const AeronXrPanel* p = &panels[i];
		/* Clamp to the target; drop empty rectangles. */
		int x = p->rect[0] < 0 ? 0 : p->rect[0];
		int y = p->rect[1] < 0 ? 0 : p->rect[1];
		int r = p->rect[0] + p->rect[2];
		int b = p->rect[1] + p->rect[3];
		if (r > g_st.width) {
			r = g_st.width;
		}
		if (b > g_st.height) {
			b = g_st.height;
		}
		if (r - x < 2 || b - y < 2 || p->size[0] <= 0.0f || p->size[1] <= 0.0f ||
			(p->source == 1 && !g_st.chains[AERON_XR_SLOT_HUD_AUX].swapchain)) {
			continue;
		}
		if (p->source == 1) {
			g_st.hud_aux_visible = 1;
		}
		g_st.panels[g_st.panel_count]         = *p;
		g_st.panels[g_st.panel_count].rect[0] = x;
		g_st.panels[g_st.panel_count].rect[1] = y;
		g_st.panels[g_st.panel_count].rect[2] = r - x;
		g_st.panels[g_st.panel_count].rect[3] = b - y;
		g_st.panel_count++;
	}
	/* The panels sample the head HUD targets, so they have to be submitted. */
	g_st.hud.visible = 0;
	for (int i = 0; i < g_st.panel_count; ++i) {
		if (g_st.panels[i].source != 1) {
			g_st.hud.visible = 1;
		}
	}
	g_st.hud.size[0] = 0.0f; /* no full-size head quad */
}

void Aeron_XrStereoCommitConsole(const AeronXrPanel* console) {
	const AeronXrStereoSwapchain* chain = &g_st.chains[AERON_XR_SLOT_CONSOLE];
	g_st.console_visible                = 0;
	if (!g_st.committed || !console || !chain->swapchain || console->size[0] <= 0.0f || console->size[1] <= 0.0f) {
		return;
	}
	g_st.console = *console;
	if (g_st.console.rect[2] <= 0 || g_st.console.rect[3] <= 0) {
		g_st.console.rect[0] = 0;
		g_st.console.rect[1] = 0;
		g_st.console.rect[2] = chain->width;
		g_st.console.rect[3] = chain->height;
	}
	g_st.console_visible = 1;
}

int Aeron_XrStereoCommitted(void) {
	return g_st.ready && g_st.committed && g_st.located_frame == g_xr.frame_index && Aeron_XrShouldRender();
}

void Aeron_XrStereoCommitBackdrop(void) {
	Aeron_XrStereoCommitHuds(NULL, NULL);
	g_st.screen_overlay = g_st.committed;
}

int Aeron_XrStereoScreenOverlay(void) { return Aeron_XrStereoCommitted() && g_st.screen_overlay; }

SDL_GPUTexture* Aeron_XrStereoTexture(int slot) {
	if (slot < 0 || slot >= AERON_XR_STEREO_SLOTS || !g_st.chains[slot].target) {
		return NULL;
	}
	return Aeron_RenderTargetGetTexture(Aeron_XrStereoSlotTarget(slot))->texture;
}

int Aeron_XrStereoHudVisible(void) { return g_st.hud.visible; }

static int Aeron_XrStereoSlotVisible(int slot) {
	if (slot == AERON_XR_SLOT_HUD) {
		return g_st.hud.visible;
	}
	if (slot == AERON_XR_SLOT_HUD_WORLD) {
		return g_st.hud_world.visible;
	}
	if (slot == AERON_XR_SLOT_CONSOLE) {
		return g_st.console_visible && g_st.chains[slot].swapchain;
	}
	if (slot == AERON_XR_SLOT_HUD_AUX) {
		return g_st.hud_aux_visible && g_st.chains[slot].swapchain;
	}
	return 1;
}

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
		if (!Aeron_XrStereoSlotVisible(slot)) {
			continue;
		}
		if (chain->acquired) {
			if (!chain->waited) {
				ok = 0; /* acquired for direct rendering but the wait failed */
				continue;
			}
			if (chain->direct_frame == g_xr.frame_index && chain->direct_frame != 0) {
				continue; /* rendered in place */
			}
			/* Acquired in an earlier frame index: the game drew into the
			 * intermediate target, so copy it into the waiting image. */
			SDL_zero(blit);
			blit.source.texture      = Aeron_RenderTargetGetTexture(chain->target)->texture;
			blit.source.w            = (Uint32)chain->width;
			blit.source.h            = (Uint32)chain->height;
			blit.destination.texture = chain->images[chain->index];
			blit.destination.w       = (Uint32)chain->width;
			blit.destination.h       = (Uint32)chain->height;
			blit.load_op             = SDL_GPU_LOADOP_DONT_CARE;
			blit.filter              = SDL_GPU_FILTER_NEAREST;
			SDL_BlitGPUTexture(command_buffer, &blit);
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
		blit.source.w            = (Uint32)chain->width;
		blit.source.h            = (Uint32)chain->height;
		blit.destination.texture = chain->images[index];
		blit.destination.w       = (Uint32)chain->width;
		blit.destination.h       = (Uint32)chain->height;
		blit.load_op             = SDL_GPU_LOADOP_DONT_CARE;
		blit.filter              = SDL_GPU_FILTER_NEAREST;
		SDL_BlitGPUTexture(command_buffer, &blit);
	}
	return ok;
}

static XrVector3f Aeron_XrQuatRotate(XrQuaternionf q, XrVector3f v) {
	const float tx = 2.0f * (q.y * v.z - q.z * v.y);
	const float ty = 2.0f * (q.z * v.x - q.x * v.z);
	const float tz = 2.0f * (q.x * v.y - q.y * v.x);
	XrVector3f  r;
	r.x = v.x + q.w * tx + (q.y * tz - q.z * ty);
	r.y = v.y + q.w * ty + (q.z * tx - q.x * tz);
	r.z = v.z + q.w * tz + (q.x * ty - q.y * tx);
	return r;
}

static XrQuaternionf Aeron_XrQuatNormalize(XrQuaternionf q) {
	const float len = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	if (len < 1.0e-6f) {
		return (XrQuaternionf) { 0.0f, 0.0f, 0.0f, 1.0f };
	}
	q.x /= len;
	q.y /= len;
	q.z /= len;
	q.w /= len;
	return q;
}

/* Pose of a head-locked quad this frame: head pose (between the eyes) times
 * the requested offset, eased towards that target when follow_s > 0. */
static XrPosef Aeron_XrHudFollowPose(const AeronXrHudQuad* hud) {
	const XrPosef* l = &g_st.views[0].pose;
	const XrPosef* r = &g_st.views[1].pose;
	XrPosef        head;
	XrPosef        target;
	XrVector3f     offset;

	head.position.x  = 0.5f * (l->position.x + r->position.x);
	head.position.y  = 0.5f * (l->position.y + r->position.y);
	head.position.z  = 0.5f * (l->position.z + r->position.z);
	head.orientation = Aeron_XrQuatNormalize(l->orientation);

	offset             = (XrVector3f) { hud->position[0], hud->position[1], hud->position[2] };
	target.orientation = head.orientation;
	target.position    = Aeron_XrQuatRotate(head.orientation, offset);
	target.position.x += head.position.x;
	target.position.y += head.position.y;
	target.position.z += head.position.z;

	if (hud->follow_s <= 0.0f || !g_st.follow_valid || g_xr.predicted_display_time <= g_st.follow_time) {
		g_st.follow_pose = target;
	} else {
		float dt = (float)((double)(g_xr.predicted_display_time - g_st.follow_time) * 1.0e-9);
		float a;
		float dot;
		XrQuaternionf q = target.orientation;
		if (dt > 0.25f) {
			dt = 0.25f; /* after a hitch, catch up rather than crawl */
		}
		a = 1.0f - expf(-dt / hud->follow_s);
		g_st.follow_pose.position.x += (target.position.x - g_st.follow_pose.position.x) * a;
		g_st.follow_pose.position.y += (target.position.y - g_st.follow_pose.position.y) * a;
		g_st.follow_pose.position.z += (target.position.z - g_st.follow_pose.position.z) * a;
		dot = g_st.follow_pose.orientation.x * q.x + g_st.follow_pose.orientation.y * q.y +
			  g_st.follow_pose.orientation.z * q.z + g_st.follow_pose.orientation.w * q.w;
		if (dot < 0.0f) {
			q.x = -q.x;
			q.y = -q.y;
			q.z = -q.z;
			q.w = -q.w;
		}
		g_st.follow_pose.orientation.x += (q.x - g_st.follow_pose.orientation.x) * a;
		g_st.follow_pose.orientation.y += (q.y - g_st.follow_pose.orientation.y) * a;
		g_st.follow_pose.orientation.z += (q.z - g_st.follow_pose.orientation.z) * a;
		g_st.follow_pose.orientation.w += (q.w - g_st.follow_pose.orientation.w) * a;
		g_st.follow_pose.orientation = Aeron_XrQuatNormalize(g_st.follow_pose.orientation);
	}
	g_st.follow_valid = 1;
	g_st.follow_time  = g_xr.predicted_display_time;
	return g_st.follow_pose;
}

static void Aeron_XrFillQuad(XrCompositionLayerQuad* quad, int slot, const AeronXrHudQuad* hud) {
	memset(quad, 0, sizeof *quad);
	quad->type                             = XR_TYPE_COMPOSITION_LAYER_QUAD;
	quad->layerFlags                       = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	quad->space                            = g_xr.local_space;
	quad->eyeVisibility                    = XR_EYE_VISIBILITY_BOTH;
	quad->subImage.swapchain               = g_st.chains[slot].swapchain;
	quad->subImage.imageRect.extent.width  = g_st.width;
	quad->subImage.imageRect.extent.height = g_st.height;
	if (hud->head_locked) {
		quad->pose = Aeron_XrHudFollowPose(hud);
	} else {
		quad->pose.orientation.w = 1.0f;
		quad->pose.position.x    = hud->position[0];
		quad->pose.position.y    = hud->position[1];
		quad->pose.position.z    = hud->position[2];
	}
	quad->size.width  = hud->size[0];
	quad->size.height = hud->size[1];
}

/* Releases acquired images and appends this frame's layers. Returns the
 * number of layers written (0 when the frame was not stereo). */
uint32_t Aeron_XrStereoEndFrameLayers(const XrCompositionLayerBaseHeader** layers, uint32_t capacity,
									  const XrCompositionLayerBaseHeader* screen) {
	uint32_t count = 0;
	int      slot;
	int      eyes_ok = 1;

	for (slot = 0; slot < AERON_XR_STEREO_SLOTS; ++slot) {
		AeronXrStereoSwapchain* chain = &g_st.chains[slot];
		if (chain->acquired && chain->swapchain) {
			XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, NULL };
			if (XR_SUCCEEDED(g_xr.xrReleaseSwapchainImage(chain->swapchain, &release))) {
				chain->released_once = 1;
			}
			chain->acquired     = 0;
			chain->waited       = 0;
			chain->direct_frame = 0;
		}
	}
	if (!Aeron_XrStereoCommitted()) {
		g_st.committed      = 0;
		g_st.screen_overlay = 0;
		return 0;
	}
	g_st.committed      = 0;
	g_st.screen_overlay = 0;
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
	if (screen && count < capacity) {
		layers[count++] = screen;
	}

	/* World HUD first: the head HUD is composited on top of it. */
	if (g_st.hud_world.visible && g_st.chains[AERON_XR_SLOT_HUD_WORLD].released_once && count < capacity) {
		Aeron_XrFillQuad(&g_st.hud_world_quad, AERON_XR_SLOT_HUD_WORLD, &g_st.hud_world);
		layers[count++] = (const XrCompositionLayerBaseHeader*)&g_st.hud_world_quad;
	}
	if (g_st.hud.visible && g_st.hud.size[0] > 0.0f && g_st.chains[AERON_XR_SLOT_HUD].released_once &&
		count < capacity) {
		Aeron_XrFillQuad(&g_st.hud_quad, AERON_XR_SLOT_HUD, &g_st.hud);
		layers[count++] = (const XrCompositionLayerBaseHeader*)&g_st.hud_quad;
	}
	{
		for (int i = 0; i < g_st.panel_count && count < capacity; ++i) {
			const AeronXrPanel*     p    = &g_st.panels[i];
			XrCompositionLayerQuad* quad = &g_st.panel_quads[i];
			const int slot = p->source == 1 ? AERON_XR_SLOT_HUD_AUX : AERON_XR_SLOT_HUD;
			if (!g_st.chains[slot].released_once) {
				continue;
			}
			memset(quad, 0, sizeof *quad);
			quad->type                              = XR_TYPE_COMPOSITION_LAYER_QUAD;
			quad->layerFlags                        = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			quad->space                             = g_xr.local_space;
			quad->eyeVisibility                     = XR_EYE_VISIBILITY_BOTH;
			quad->subImage.swapchain                = g_st.chains[slot].swapchain;
			quad->subImage.imageRect.offset.x       = p->rect[0];
			quad->subImage.imageRect.offset.y       = p->rect[1];
			quad->subImage.imageRect.extent.width   = p->rect[2];
			quad->subImage.imageRect.extent.height  = p->rect[3];
			quad->pose.orientation = Aeron_XrQuatNormalize((XrQuaternionf) { p->orientation[0], p->orientation[1],
																			 p->orientation[2], p->orientation[3] });
			quad->pose.position.x = p->position[0];
			quad->pose.position.y = p->position[1];
			quad->pose.position.z = p->position[2];
			quad->size.width      = p->size[0];
			quad->size.height     = p->size[1];
			layers[count++]       = (const XrCompositionLayerBaseHeader*)quad;
		}
	}
	/* The console last: it is the panel the hands reach into. */
	if (g_st.console_visible && g_st.chains[AERON_XR_SLOT_CONSOLE].released_once && count < capacity) {
		const AeronXrPanel*     p    = &g_st.console;
		XrCompositionLayerQuad* quad = &g_st.console_quad;
		memset(quad, 0, sizeof *quad);
		quad->type                             = XR_TYPE_COMPOSITION_LAYER_QUAD;
		quad->layerFlags                       = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		quad->space                            = g_xr.local_space;
		quad->eyeVisibility                    = XR_EYE_VISIBILITY_BOTH;
		quad->subImage.swapchain               = g_st.chains[AERON_XR_SLOT_CONSOLE].swapchain;
		quad->subImage.imageRect.offset.x      = p->rect[0];
		quad->subImage.imageRect.offset.y      = p->rect[1];
		quad->subImage.imageRect.extent.width  = p->rect[2];
		quad->subImage.imageRect.extent.height = p->rect[3];
		quad->pose.orientation = Aeron_XrQuatNormalize(
			(XrQuaternionf) { p->orientation[0], p->orientation[1], p->orientation[2], p->orientation[3] });
		quad->pose.position.x = p->position[0];
		quad->pose.position.y = p->position[1];
		quad->pose.position.z = p->position[2];
		quad->size.width      = p->size[0];
		quad->size.height     = p->size[1];
		layers[count++]       = (const XrCompositionLayerBaseHeader*)quad;
	}
	g_st.console_visible = 0;
	if (!g_st.hud.visible || !g_st.hud.head_locked) {
		g_st.follow_valid = 0; /* next appearance starts in place, no swoop */
	}
	return count;
}

#endif /* AERON_HAS_OPENXR */
