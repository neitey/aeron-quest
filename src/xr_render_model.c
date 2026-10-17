/* Controller models from the OpenXR runtime (see Aeron_XrGetRenderModel):
 *
 * - XR_FB_render_model (Meta Quest): the glTF of "/model_fb/controller/left"
 *   and ".../right", drawn at the grip pose. Fetched after the session runs
 *   (the runtime may report no model until the controllers are seen; we
 *   try again every couple of seconds for a while).
 * - XR_EXT_render_model + XR_EXT_interaction_render_model (SteamVR, Pico 4
 *   Ultra): the models of the bound interaction profile, each with its own
 *   space and animatable nodes (buttons, trigger, stick), and the hand it
 *   belongs to (its "top level user path"). Fetched after the first action
 *   sync and again whenever the runtime says they changed.
 *
 * All calls happen on the thread that syncs the actions. */
#include "aeron/aeron.h"

#include <string.h>

#if !defined(AERON_HAS_OPENXR)

int Aeron_XrGetRenderModel(int hand, AeronXrRenderModel* out) {
	(void)hand;
	if (out) {
		memset(out, 0, sizeof *out);
	}
	return 0;
}

int Aeron_XrLocateRenderModel(int hand, float position[3], float orientation[4], AeronXrRenderModelNode* nodes,
							  uint32_t node_capacity) {
	(void)hand;
	(void)position;
	(void)orientation;
	(void)nodes;
	(void)node_capacity;
	return 0;
}

#else

#include <openxr/openxr.h>

#include "aeron/log.h"
#include "xr_private.h"

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

enum { RM_HANDS = 2, RM_MAX_EXT_MODELS = 8, RM_FB_TRIES = 20 };

typedef struct RmExtModel {
	XrRenderModelIdEXT id;
	XrRenderModelEXT   model;
	XrSpace            space;
	int                hand; /* -1 not (yet) on a hand */
	uint8_t*           glb;
	size_t             glb_size;
	uint32_t           node_count;
	char (*node_names)[64];
	XrRenderModelNodeStateEXT* states;
	uint32_t                   generation;
} RmExtModel;

static struct {
	int      loaded_fns;
	uint32_t generation;
	/* FB */
	PFN_xrEnumerateRenderModelPathsFB xrEnumerateRenderModelPathsFB;
	PFN_xrGetRenderModelPropertiesFB  xrGetRenderModelPropertiesFB;
	PFN_xrLoadRenderModelFB           xrLoadRenderModelFB;
	int                               fb_paths_done;
	int                               fb_tries[RM_HANDS];
	uint64_t                          fb_next_ns[RM_HANDS];
	uint8_t*                          fb_glb[RM_HANDS];
	size_t                            fb_size[RM_HANDS];
	char                              fb_name[RM_HANDS][64];
	uint32_t                          fb_generation[RM_HANDS];
	/* EXT */
	PFN_xrCreateRenderModelEXT                  xrCreateRenderModelEXT;
	PFN_xrDestroyRenderModelEXT                 xrDestroyRenderModelEXT;
	PFN_xrGetRenderModelPropertiesEXT           xrGetRenderModelPropertiesEXT;
	PFN_xrCreateRenderModelSpaceEXT             xrCreateRenderModelSpaceEXT;
	PFN_xrCreateRenderModelAssetEXT             xrCreateRenderModelAssetEXT;
	PFN_xrDestroyRenderModelAssetEXT            xrDestroyRenderModelAssetEXT;
	PFN_xrGetRenderModelAssetDataEXT            xrGetRenderModelAssetDataEXT;
	PFN_xrGetRenderModelAssetPropertiesEXT      xrGetRenderModelAssetPropertiesEXT;
	PFN_xrGetRenderModelStateEXT                xrGetRenderModelStateEXT;
	PFN_xrEnumerateInteractionRenderModelIdsEXT xrEnumerateInteractionRenderModelIdsEXT;
	PFN_xrGetRenderModelPoseTopLevelUserPathEXT xrGetRenderModelPoseTopLevelUserPathEXT;
	int                                         ext_ok;
	int                                         ext_dirty;
	uint64_t                                    ext_retry_ns; /* none yet: ask again now and then */
	int                                         ext_retries;
	RmExtModel                                  ext[RM_MAX_EXT_MODELS];
	int                                         ext_count;
	XrPath                                      hand_path[RM_HANDS];
} g_rm;

static int rm_enabled(void) {
	const char* env = SDL_getenv("AERON_XR_RENDER_MODELS");
	return !(env && env[0] == '0');
}

#define RM_LOAD(fn)                                                                                          \
	(XR_SUCCEEDED(g_xr.xrGetInstanceProcAddr(g_xr.instance, #fn, (PFN_xrVoidFunction*)&g_rm.fn)) && g_rm.fn)

static void rm_load_functions(void) {
	if (g_rm.loaded_fns || !g_xr.xrGetInstanceProcAddr || !g_xr.instance) {
		return;
	}
	g_rm.loaded_fns = 1;
	if (g_xr.has_fb_render_model) {
		if (!(RM_LOAD(xrEnumerateRenderModelPathsFB) && RM_LOAD(xrGetRenderModelPropertiesFB) &&
			  RM_LOAD(xrLoadRenderModelFB))) {
			Aeron_LogWarn("aeron.xr", "XR_FB_render_model functions missing; no controller models");
			g_xr.has_fb_render_model = 0;
		}
	}
	if (g_xr.has_ext_render_model && g_xr.has_ext_interaction_render_model) {
		g_rm.ext_ok = RM_LOAD(xrCreateRenderModelEXT) && RM_LOAD(xrDestroyRenderModelEXT) &&
					  RM_LOAD(xrGetRenderModelPropertiesEXT) && RM_LOAD(xrCreateRenderModelSpaceEXT) &&
					  RM_LOAD(xrCreateRenderModelAssetEXT) && RM_LOAD(xrDestroyRenderModelAssetEXT) &&
					  RM_LOAD(xrGetRenderModelAssetDataEXT) && RM_LOAD(xrGetRenderModelAssetPropertiesEXT) &&
					  RM_LOAD(xrGetRenderModelStateEXT) && RM_LOAD(xrEnumerateInteractionRenderModelIdsEXT) &&
					  RM_LOAD(xrGetRenderModelPoseTopLevelUserPathEXT);
		if (!g_rm.ext_ok) {
			Aeron_LogWarn("aeron.xr", "XR_EXT_render_model functions missing; no controller models");
		}
		g_rm.ext_dirty = 1;
	}
	g_rm.hand_path[0] = Aeron_XrPath("/user/hand/left");
	g_rm.hand_path[1] = Aeron_XrPath("/user/hand/right");
}

/* ---- XR_FB_render_model --------------------------------------------------- */

static void rm_fb_update(void) {
	static const char* const paths[RM_HANDS] = { "/model_fb/controller/left", "/model_fb/controller/right" };
	const uint64_t now = SDL_GetTicksNS();
	if (!g_xr.has_fb_render_model) {
		return;
	}
	for (int h = 0; h < RM_HANDS; ++h) {
		if (g_rm.fb_glb[h] || g_rm.fb_tries[h] >= RM_FB_TRIES || now < g_rm.fb_next_ns[h]) {
			continue;
		}
		g_rm.fb_tries[h]++;
		g_rm.fb_next_ns[h] = now + 2000000000ull;
		if (!g_rm.fb_paths_done) {
			/* The paths must be enumerated before a model is asked for. */
			uint32_t count = 0;
			XrResult result = g_rm.xrEnumerateRenderModelPathsFB(g_xr.session, 0, &count, NULL);
			if (XR_FAILED(result)) {
				Aeron_LogWarn("aeron.xr", "xrEnumerateRenderModelPathsFB: %s", Aeron_XrResultName(result));
				return;
			}
			XrRenderModelPathInfoFB* infos = (XrRenderModelPathInfoFB*)SDL_calloc(count ? count : 1, sizeof *infos);
			if (!infos) {
				return;
			}
			for (uint32_t i = 0; i < count; ++i) {
				infos[i].type = XR_TYPE_RENDER_MODEL_PATH_INFO_FB;
			}
			result = g_rm.xrEnumerateRenderModelPathsFB(g_xr.session, count, &count, infos);
			if (XR_SUCCEEDED(result)) {
				for (uint32_t i = 0; i < count && g_xr.xrPathToString; ++i) {
					char     name[256];
					uint32_t len = 0;
					if (XR_SUCCEEDED(g_xr.xrPathToString(g_xr.instance, infos[i].path, sizeof name, &len, name))) {
						Aeron_LogInfo("aeron.xr", "render model path: %s", name);
					}
				}
				g_rm.fb_paths_done = 1;
			}
			SDL_free(infos);
			if (!g_rm.fb_paths_done) {
				return;
			}
		}
		/* Subset 1 first (plain textures); subset 2 for controllers only
		 * offered that way. */
		XrRenderModelPropertiesFB props = { XR_TYPE_RENDER_MODEL_PROPERTIES_FB, NULL };
		for (int subset = 1; subset <= 2; ++subset) {
			XrRenderModelCapabilitiesRequestFB caps = { XR_TYPE_RENDER_MODEL_CAPABILITIES_REQUEST_FB, NULL };
			caps.flags = subset == 1 ? XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_1_BIT_FB
									 : XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_2_BIT_FB;
			memset(&props, 0, sizeof props);
			props.type     = XR_TYPE_RENDER_MODEL_PROPERTIES_FB;
			props.next     = &caps;
			XrResult result = g_rm.xrGetRenderModelPropertiesFB(g_xr.session, Aeron_XrPath(paths[h]), &props);
			if (XR_SUCCEEDED(result) && result != XR_RENDER_MODEL_UNAVAILABLE_FB &&
				props.modelKey != XR_NULL_RENDER_MODEL_KEY_FB) {
				Aeron_LogInfo("aeron.xr", "%s controller model: \"%s\" v%u (vendor %u, glTF subset %d)",
							  h ? "right" : "left", props.modelName, props.modelVersion, props.vendorId, subset);
				break;
			}
			if (XR_FAILED(result)) {
				Aeron_LogInfo("aeron.xr", "%s controller model (subset %d): %s", h ? "right" : "left", subset,
							  Aeron_XrResultName(result));
			}
			props.modelKey = XR_NULL_RENDER_MODEL_KEY_FB;
		}
		if (props.modelKey == XR_NULL_RENDER_MODEL_KEY_FB) {
			if (g_rm.fb_tries[h] == RM_FB_TRIES) {
				Aeron_LogInfo("aeron.xr", "%s controller: the runtime has no model", h ? "right" : "left");
			}
			continue;
		}
		XrRenderModelLoadInfoFB load = { XR_TYPE_RENDER_MODEL_LOAD_INFO_FB, NULL };
		XrRenderModelBufferFB   buf  = { XR_TYPE_RENDER_MODEL_BUFFER_FB, NULL };
		load.modelKey                = props.modelKey;
		XrResult result              = g_rm.xrLoadRenderModelFB(g_xr.session, &load, &buf);
		if (XR_FAILED(result) || buf.bufferCountOutput == 0) {
			Aeron_LogWarn("aeron.xr", "xrLoadRenderModelFB (size): %s", Aeron_XrResultName(result));
			continue;
		}
		uint8_t* data = (uint8_t*)SDL_malloc(buf.bufferCountOutput);
		if (!data) {
			continue;
		}
		buf.bufferCapacityInput = buf.bufferCountOutput;
		buf.buffer              = data;
		result                  = g_rm.xrLoadRenderModelFB(g_xr.session, &load, &buf);
		if (XR_FAILED(result)) {
			Aeron_LogWarn("aeron.xr", "xrLoadRenderModelFB: %s", Aeron_XrResultName(result));
			SDL_free(data);
			continue;
		}
		g_rm.fb_glb[h]  = data;
		g_rm.fb_size[h] = buf.bufferCountOutput;
		SDL_strlcpy(g_rm.fb_name[h], props.modelName, sizeof g_rm.fb_name[h]);
		g_rm.fb_generation[h] = ++g_rm.generation;
		Aeron_LogInfo("aeron.xr", "%s controller model loaded (%u bytes)", h ? "right" : "left",
					  buf.bufferCountOutput);
	}
}

/* ---- XR_EXT_render_model ---------------------------------------------------- */

static void rm_ext_free(RmExtModel* m) {
	if (m->space && g_xr.xrDestroySpace) {
		g_xr.xrDestroySpace(m->space);
	}
	if (m->model && g_rm.xrDestroyRenderModelEXT) {
		g_rm.xrDestroyRenderModelEXT(m->model);
	}
	SDL_free(m->glb);
	SDL_free(m->node_names);
	SDL_free(m->states);
	memset(m, 0, sizeof *m);
}

static int rm_ext_create(RmExtModel* m, XrRenderModelIdEXT id) {
	XrResult result;
	memset(m, 0, sizeof *m);
	m->id   = id;
	m->hand = -1;
	{
		/* No glTF extensions asked for: plain textures. */
		XrRenderModelCreateInfoEXT info = { XR_TYPE_RENDER_MODEL_CREATE_INFO_EXT, NULL };
		info.renderModelId              = id;
		result                          = g_rm.xrCreateRenderModelEXT(g_xr.session, &info, &m->model);
		if (XR_FAILED(result)) {
			Aeron_LogWarn("aeron.xr", "xrCreateRenderModelEXT: %s", Aeron_XrResultName(result));
			return 0;
		}
	}
	XrRenderModelPropertiesGetInfoEXT get   = { XR_TYPE_RENDER_MODEL_PROPERTIES_GET_INFO_EXT, NULL };
	XrRenderModelPropertiesEXT        props = { XR_TYPE_RENDER_MODEL_PROPERTIES_EXT, NULL };
	result = g_rm.xrGetRenderModelPropertiesEXT(m->model, &get, &props);
	if (XR_FAILED(result)) {
		Aeron_LogWarn("aeron.xr", "xrGetRenderModelPropertiesEXT: %s", Aeron_XrResultName(result));
		return 0;
	}
	XrRenderModelAssetEXT asset = XR_NULL_HANDLE;
	{
		XrRenderModelAssetCreateInfoEXT info = { XR_TYPE_RENDER_MODEL_ASSET_CREATE_INFO_EXT, NULL };
		info.cacheId                          = props.cacheId;
		result = g_rm.xrCreateRenderModelAssetEXT(g_xr.session, &info, &asset);
		if (XR_FAILED(result)) {
			Aeron_LogWarn("aeron.xr", "xrCreateRenderModelAssetEXT: %s", Aeron_XrResultName(result));
			return 0;
		}
	}
	int ok = 0;
	{
		XrRenderModelAssetDataGetInfoEXT info = { XR_TYPE_RENDER_MODEL_ASSET_DATA_GET_INFO_EXT, NULL };
		XrRenderModelAssetDataEXT        data = { XR_TYPE_RENDER_MODEL_ASSET_DATA_EXT, NULL };
		result = g_rm.xrGetRenderModelAssetDataEXT(asset, &info, &data);
		if (XR_SUCCEEDED(result) && data.bufferCountOutput > 0 &&
			(m->glb = (uint8_t*)SDL_malloc(data.bufferCountOutput)) != NULL) {
			data.bufferCapacityInput = data.bufferCountOutput;
			data.buffer              = m->glb;
			result                   = g_rm.xrGetRenderModelAssetDataEXT(asset, &info, &data);
			if (XR_SUCCEEDED(result)) {
				m->glb_size = data.bufferCountOutput;
				ok          = 1;
			}
		}
		if (!ok) {
			Aeron_LogWarn("aeron.xr", "xrGetRenderModelAssetDataEXT: %s", Aeron_XrResultName(result));
		}
	}
	if (ok && props.animatableNodeCount > 0) {
		m->node_count = props.animatableNodeCount;
		m->node_names = (char(*)[64])SDL_calloc(m->node_count, 64);
		m->states     = (XrRenderModelNodeStateEXT*)SDL_calloc(m->node_count, sizeof *m->states);
		XrRenderModelAssetNodePropertiesEXT* nodes =
			(XrRenderModelAssetNodePropertiesEXT*)SDL_calloc(m->node_count, sizeof *nodes);
		XrRenderModelAssetPropertiesGetInfoEXT info  = { XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_GET_INFO_EXT, NULL };
		XrRenderModelAssetPropertiesEXT        aprops = { XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_EXT, NULL };
		aprops.nodePropertyCount                     = m->node_count;
		aprops.nodeProperties                        = nodes;
		if (!m->node_names || !m->states || !nodes ||
			XR_FAILED(result = g_rm.xrGetRenderModelAssetPropertiesEXT(asset, &info, &aprops))) {
			Aeron_LogWarn("aeron.xr", "xrGetRenderModelAssetPropertiesEXT: %s", Aeron_XrResultName(result));
			m->node_count = 0;
		} else {
			for (uint32_t i = 0; i < m->node_count; ++i) {
				SDL_strlcpy(m->node_names[i], nodes[i].uniqueName, 64);
			}
		}
		SDL_free(nodes);
	}
	g_rm.xrDestroyRenderModelAssetEXT(asset);
	if (!ok) {
		return 0;
	}
	{
		XrRenderModelSpaceCreateInfoEXT info = { XR_TYPE_RENDER_MODEL_SPACE_CREATE_INFO_EXT, NULL };
		info.renderModel                     = m->model;
		result = g_rm.xrCreateRenderModelSpaceEXT(g_xr.session, &info, &m->space);
		if (XR_FAILED(result)) {
			Aeron_LogWarn("aeron.xr", "xrCreateRenderModelSpaceEXT: %s", Aeron_XrResultName(result));
			return 0;
		}
	}
	m->generation = ++g_rm.generation;
	Aeron_LogInfo("aeron.xr", "controller model %llu: %u bytes, %u animatable nodes", (unsigned long long)id,
				  (unsigned)m->glb_size, m->node_count);
	for (uint32_t i = 0; i < m->node_count; ++i) {
		Aeron_LogInfo("aeron.xr", "  node %u: %s", i, m->node_names[i]);
	}
	return 1;
}

static void rm_ext_update(void) {
	if (!g_rm.ext_ok) {
		return;
	}
	if (g_rm.ext_count == 0 && g_rm.ext_retries < 20 && SDL_GetTicksNS() >= g_rm.ext_retry_ns) {
		g_rm.ext_retries++;
		g_rm.ext_retry_ns = SDL_GetTicksNS() + 3000000000ull;
		g_rm.ext_dirty    = 1;
	}
	if (g_rm.ext_dirty) {
		g_rm.ext_dirty = 0;
		XrInteractionRenderModelIdsEnumerateInfoEXT info = { XR_TYPE_INTERACTION_RENDER_MODEL_IDS_ENUMERATE_INFO_EXT,
															NULL };
		XrRenderModelIdEXT ids[RM_MAX_EXT_MODELS];
		uint32_t           count  = 0;
		XrResult           result = g_rm.xrEnumerateInteractionRenderModelIdsEXT(g_xr.session, &info, 0, &count, NULL);
		if (XR_SUCCEEDED(result) && count > 0) {
			if (count > RM_MAX_EXT_MODELS) {
				count = RM_MAX_EXT_MODELS;
			}
			result = g_rm.xrEnumerateInteractionRenderModelIdsEXT(g_xr.session, &info, count, &count, ids);
		}
		if (XR_FAILED(result)) {
			Aeron_LogWarn("aeron.xr", "xrEnumerateInteractionRenderModelIdsEXT: %s", Aeron_XrResultName(result));
			count = 0;
		}
		/* Drop the models that went, create the new ones. */
		for (int i = 0; i < g_rm.ext_count;) {
			int kept = 0;
			for (uint32_t k = 0; k < count; ++k) {
				kept |= ids[k] == g_rm.ext[i].id;
			}
			if (kept) {
				++i;
				continue;
			}
			rm_ext_free(&g_rm.ext[i]);
			g_rm.ext[i]               = g_rm.ext[g_rm.ext_count - 1];
			memset(&g_rm.ext[g_rm.ext_count - 1], 0, sizeof g_rm.ext[0]);
			g_rm.ext_count--;
		}
		for (uint32_t k = 0; k < count && g_rm.ext_count < RM_MAX_EXT_MODELS; ++k) {
			int known = 0;
			for (int i = 0; i < g_rm.ext_count; ++i) {
				known |= g_rm.ext[i].id == ids[k];
			}
			if (known) {
				continue;
			}
			RmExtModel* m = &g_rm.ext[g_rm.ext_count];
			if (rm_ext_create(m, ids[k])) {
				g_rm.ext_count++;
			} else {
				rm_ext_free(m);
			}
		}
	}
	/* Which hand each model is on (it can change). */
	for (int i = 0; i < g_rm.ext_count; ++i) {
		RmExtModel*                                        m    = &g_rm.ext[i];
		XrInteractionRenderModelTopLevelUserPathGetInfoEXT info = {
			XR_TYPE_INTERACTION_RENDER_MODEL_TOP_LEVEL_USER_PATH_GET_INFO_EXT, NULL };
		XrPath path                = XR_NULL_PATH;
		info.topLevelUserPathCount = RM_HANDS;
		info.topLevelUserPaths     = g_rm.hand_path;
		int hand                   = -1;
		if (XR_SUCCEEDED(g_rm.xrGetRenderModelPoseTopLevelUserPathEXT(m->model, &info, &path))) {
			hand = path == g_rm.hand_path[0] ? 0 : path == g_rm.hand_path[1] ? 1 : -1;
		}
		if (hand != m->hand) {
			m->hand       = hand;
			m->generation = ++g_rm.generation;
			Aeron_LogInfo("aeron.xr", "controller model %llu is on the %s", (unsigned long long)m->id,
						  hand == 0 ? "left hand" : hand == 1 ? "right hand" : "no hand");
		}
	}
}

static const RmExtModel* rm_ext_for_hand(int hand) {
	for (int i = 0; i < g_rm.ext_count; ++i) {
		if (g_rm.ext[i].hand == hand && g_rm.ext[i].glb) {
			return &g_rm.ext[i];
		}
	}
	return NULL;
}

/* ---- API ---------------------------------------------------------------- */

void Aeron_XrRenderModelsUpdate(void) {
	if (!g_xr.session || !rm_enabled()) {
		return;
	}
	rm_load_functions();
	rm_ext_update();
	if (g_rm.ext_count == 0) {
		rm_fb_update(); /* Quest (no EXT models there) */
	}
}

void Aeron_XrRenderModelsDirty(void) { g_rm.ext_dirty = 1; }

void Aeron_XrRenderModelsShutdown(void) {
	for (int i = 0; i < g_rm.ext_count; ++i) {
		rm_ext_free(&g_rm.ext[i]);
	}
	for (int h = 0; h < RM_HANDS; ++h) {
		SDL_free(g_rm.fb_glb[h]);
	}
	memset(&g_rm, 0, sizeof g_rm);
}

int Aeron_XrGetRenderModel(int hand, AeronXrRenderModel* out) {
	if (!out) {
		return 0;
	}
	memset(out, 0, sizeof *out);
	if (hand < 0 || hand >= RM_HANDS) {
		return 0;
	}
	const RmExtModel* m = rm_ext_for_hand(hand);
	if (m) {
		out->kind       = AERON_XR_RENDER_MODEL_SPACE;
		out->generation = m->generation;
		out->glb        = m->glb;
		out->glb_size   = m->glb_size;
		SDL_snprintf(out->name, sizeof out->name, "model %llu", (unsigned long long)m->id);
		out->node_count = m->node_count;
		out->node_names = (const char(*)[64])m->node_names;
		return 1;
	}
	if (g_rm.fb_glb[hand]) {
		out->kind       = AERON_XR_RENDER_MODEL_GRIP;
		out->generation = g_rm.fb_generation[hand];
		out->glb        = g_rm.fb_glb[hand];
		out->glb_size   = g_rm.fb_size[hand];
		SDL_strlcpy(out->name, g_rm.fb_name[hand], sizeof out->name);
		return 1;
	}
	return 0;
}

int Aeron_XrLocateRenderModel(int hand, float position[3], float orientation[4], AeronXrRenderModelNode* nodes,
							  uint32_t node_capacity) {
	XrPosef pose;
	if (hand < 0 || hand >= RM_HANDS || !position || !orientation || !g_xr.session ||
		!g_xr.predicted_display_time) {
		return 0;
	}
	const RmExtModel* m = rm_ext_for_hand(hand);
	if (m) {
		XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION, NULL };
		if (XR_FAILED(g_xr.xrLocateSpace(m->space, g_xr.local_space, g_xr.predicted_display_time, &location)) ||
			!(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
			!(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
			return 0;
		}
		pose = location.pose;
		if (nodes && node_capacity && m->node_count) {
			XrRenderModelStateGetInfoEXT info  = { XR_TYPE_RENDER_MODEL_STATE_GET_INFO_EXT, NULL };
			XrRenderModelStateEXT        state = { XR_TYPE_RENDER_MODEL_STATE_EXT, NULL };
			info.displayTime                   = g_xr.predicted_display_time;
			state.nodeStateCount               = m->node_count;
			state.nodeStates                   = m->states;
			const int ok = XR_SUCCEEDED(g_rm.xrGetRenderModelStateEXT(m->model, &info, &state));
			for (uint32_t i = 0; i < node_capacity && i < m->node_count; ++i) {
				const XrRenderModelNodeStateEXT* s = &m->states[i];
				nodes[i].position[0]               = s->nodePose.position.x;
				nodes[i].position[1]               = s->nodePose.position.y;
				nodes[i].position[2]               = s->nodePose.position.z;
				nodes[i].orientation[0]            = s->nodePose.orientation.x;
				nodes[i].orientation[1]            = s->nodePose.orientation.y;
				nodes[i].orientation[2]            = s->nodePose.orientation.z;
				nodes[i].orientation[3]            = ok ? s->nodePose.orientation.w : 1.0f;
				nodes[i].visible                   = ok ? s->isVisible != XR_FALSE : 1;
			}
		}
	} else if (g_rm.fb_glb[hand]) {
		if (!Aeron_XrInputGripPose(hand, &pose)) {
			return 0;
		}
	} else {
		return 0;
	}
	position[0]    = pose.position.x;
	position[1]    = pose.position.y;
	position[2]    = pose.position.z;
	orientation[0] = pose.orientation.x;
	orientation[1] = pose.orientation.y;
	orientation[2] = pose.orientation.z;
	orientation[3] = pose.orientation.w;
	return 1;
}

#endif
