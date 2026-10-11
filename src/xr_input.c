/*
 * OpenXR controller input for Aeron.
 *
 * - Touch controllers become a virtual standardized gamepad in a free
 *   controller slot (see xr_input_map.h for the button layout), so games use
 *   their normal gamepad bindings and rebinding UI.
 * - While the pointer is enabled (menus), the right controller's aim ray is
 *   intersected with the virtual screen and drives the mouse: trigger = left
 *   click, grip = right click.
 * - The left menu button is delivered as Escape.
 * - Rumble requests for the virtual gamepad become controller haptics.
 */

#if defined(AERON_HAS_OPENXR)
#include <openxr/openxr.h>
#endif

#include "xr.h"
#include "xr_input_map.h"

#include "aeron/log.h"
#include "internal.h"

#include <math.h>
#include <string.h>

static int g_xr_pointer_enabled = 1;

void Aeron_XrSetPointerEnabled(int enabled) { g_xr_pointer_enabled = enabled != 0; }

int Aeron_XrIsControllerId(uint32_t instance_id) { return instance_id == AERON_XR_CONTROLLER_INSTANCE_ID; }

#if !defined(AERON_HAS_OPENXR)

void Aeron_XrUpdateInput(AeronInputSnapshot* input, float dt_seconds) {
	(void)input;
	(void)dt_seconds;
}

int Aeron_XrRumble(uint16_t low, uint16_t high, uint32_t duration_ms) {
	(void)low;
	(void)high;
	(void)duration_ms;
	return 0;
}

#else

#include "xr_private.h"

enum { AERON_XR_HAND_LEFT = 0, AERON_XR_HAND_RIGHT = 1, AERON_XR_HAND_COUNT = 2 };

typedef struct AeronXrInput {
	int         ready;
	XrActionSet set;
	XrPath      hand[AERON_XR_HAND_COUNT];
	XrAction    stick, trigger, grip, aim, haptic;
	XrAction    a, b, x, y, stick_click, menu;
	XrSpace     aim_space[AERON_XR_HAND_COUNT];

	AeronXrTouchMapper mapper;
	int                slot; /* controller slot used for the virtual gamepad */
	uint32_t           pointer_buttons;
	int                pointer_valid;
	int                menu_previous;
	float              pointer_logical_x;
	float              pointer_logical_y;
	int                logged_active;
} AeronXrInput;

static AeronXrInput g_in;

static XrAction Aeron_XrCreateAction(const char* name, const char* localized, XrActionType type,
									 int per_hand) {
	XrActionCreateInfo info   = { XR_TYPE_ACTION_CREATE_INFO, NULL };
	XrAction           action = XR_NULL_HANDLE;
	XrResult           result;
	SDL_strlcpy(info.actionName, name, sizeof info.actionName);
	SDL_strlcpy(info.localizedActionName, localized, sizeof info.localizedActionName);
	info.actionType = type;
	if (per_hand) {
		info.countSubactionPaths = AERON_XR_HAND_COUNT;
		info.subactionPaths      = g_in.hand;
	}
	result = g_xr.xrCreateAction(g_in.set, &info, &action);
	if (XR_FAILED(result)) {
		Aeron_LogError("aeron.xr", "xrCreateAction(%s) failed: %s", name, Aeron_XrResultName(result));
		return XR_NULL_HANDLE;
	}
	return action;
}

static int Aeron_XrSuggest(const char* profile, const XrActionSuggestedBinding* bindings, uint32_t count) {
	XrInteractionProfileSuggestedBinding suggested = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, NULL };
	XrResult                             result;
	suggested.interactionProfile     = Aeron_XrPath(profile);
	suggested.suggestedBindings      = bindings;
	suggested.countSuggestedBindings = count;
	result                           = g_xr.xrSuggestInteractionProfileBindings(g_xr.instance, &suggested);
	if (XR_FAILED(result)) {
		Aeron_LogWarn("aeron.xr", "bindings for %s rejected: %s", profile, Aeron_XrResultName(result));
		return 0;
	}
	return 1;
}

int Aeron_XrInputInit(void) {
	XrActionSetCreateInfo         set_info = { XR_TYPE_ACTION_SET_CREATE_INFO, NULL };
	XrSessionActionSetsAttachInfo attach   = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, NULL };
	XrResult                      result;
	int                           h;

	memset(&g_in, 0, sizeof g_in);
	g_in.slot = -1;
	AeronXrTouchMapper_Init(&g_in.mapper);
	{
		const char* rate = SDL_getenv("AERON_XR_THROTTLE_RATE");
		if (rate && rate[0] && SDL_atof(rate) > 0.0) {
			g_in.mapper.throttle_rate = (float)SDL_atof(rate);
		}
	}

	SDL_strlcpy(set_info.actionSetName, "flight", sizeof set_info.actionSetName);
	SDL_strlcpy(set_info.localizedActionSetName, "Flight", sizeof set_info.localizedActionSetName);
	result = g_xr.xrCreateActionSet(g_xr.instance, &set_info, &g_in.set);
	if (XR_FAILED(result)) {
		Aeron_LogError("aeron.xr", "xrCreateActionSet failed: %s", Aeron_XrResultName(result));
		return 0;
	}
	g_in.hand[AERON_XR_HAND_LEFT]  = Aeron_XrPath("/user/hand/left");
	g_in.hand[AERON_XR_HAND_RIGHT] = Aeron_XrPath("/user/hand/right");

	g_in.stick   = Aeron_XrCreateAction("stick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, 1);
	g_in.trigger = Aeron_XrCreateAction("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT, 1);
	g_in.grip    = Aeron_XrCreateAction("grip", "Grip", XR_ACTION_TYPE_FLOAT_INPUT, 1);
	g_in.stick_click =
		Aeron_XrCreateAction("stick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT, 1);
	g_in.aim    = Aeron_XrCreateAction("aim", "Aim pose", XR_ACTION_TYPE_POSE_INPUT, 1);
	g_in.haptic = Aeron_XrCreateAction("haptic", "Rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT, 1);
	g_in.a      = Aeron_XrCreateAction("button_a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
	g_in.b      = Aeron_XrCreateAction("button_b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
	g_in.x      = Aeron_XrCreateAction("button_x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
	g_in.y      = Aeron_XrCreateAction("button_y", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
	g_in.menu   = Aeron_XrCreateAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
	if (!g_in.stick || !g_in.trigger || !g_in.grip || !g_in.stick_click || !g_in.aim || !g_in.haptic ||
		!g_in.a || !g_in.b || !g_in.x || !g_in.y || !g_in.menu) {
		return 0;
	}

	{
		/* Meta Quest Touch (Quest 2/3/Pro via Link, Air Link, Virtual Desktop
		 * or SteamVR). Quest 3 Touch Plus controllers are presented to apps as
		 * this profile by the runtime. */
		const XrActionSuggestedBinding touch[] = {
			{ g_in.stick, Aeron_XrPath("/user/hand/left/input/thumbstick") },
			{ g_in.stick, Aeron_XrPath("/user/hand/right/input/thumbstick") },
			{ g_in.trigger, Aeron_XrPath("/user/hand/left/input/trigger/value") },
			{ g_in.trigger, Aeron_XrPath("/user/hand/right/input/trigger/value") },
			{ g_in.grip, Aeron_XrPath("/user/hand/left/input/squeeze/value") },
			{ g_in.grip, Aeron_XrPath("/user/hand/right/input/squeeze/value") },
			{ g_in.stick_click, Aeron_XrPath("/user/hand/left/input/thumbstick/click") },
			{ g_in.stick_click, Aeron_XrPath("/user/hand/right/input/thumbstick/click") },
			{ g_in.aim, Aeron_XrPath("/user/hand/left/input/aim/pose") },
			{ g_in.aim, Aeron_XrPath("/user/hand/right/input/aim/pose") },
			{ g_in.haptic, Aeron_XrPath("/user/hand/left/output/haptic") },
			{ g_in.haptic, Aeron_XrPath("/user/hand/right/output/haptic") },
			{ g_in.a, Aeron_XrPath("/user/hand/right/input/a/click") },
			{ g_in.b, Aeron_XrPath("/user/hand/right/input/b/click") },
			{ g_in.x, Aeron_XrPath("/user/hand/left/input/x/click") },
			{ g_in.y, Aeron_XrPath("/user/hand/left/input/y/click") },
			{ g_in.menu, Aeron_XrPath("/user/hand/left/input/menu/click") },
		};
		/* Valve Index as a courtesy for SteamVR users. */
		const XrActionSuggestedBinding index[] = {
			{ g_in.stick, Aeron_XrPath("/user/hand/left/input/thumbstick") },
			{ g_in.stick, Aeron_XrPath("/user/hand/right/input/thumbstick") },
			{ g_in.trigger, Aeron_XrPath("/user/hand/left/input/trigger/value") },
			{ g_in.trigger, Aeron_XrPath("/user/hand/right/input/trigger/value") },
			{ g_in.grip, Aeron_XrPath("/user/hand/left/input/squeeze/value") },
			{ g_in.grip, Aeron_XrPath("/user/hand/right/input/squeeze/value") },
			{ g_in.stick_click, Aeron_XrPath("/user/hand/left/input/thumbstick/click") },
			{ g_in.stick_click, Aeron_XrPath("/user/hand/right/input/thumbstick/click") },
			{ g_in.aim, Aeron_XrPath("/user/hand/left/input/aim/pose") },
			{ g_in.aim, Aeron_XrPath("/user/hand/right/input/aim/pose") },
			{ g_in.haptic, Aeron_XrPath("/user/hand/left/output/haptic") },
			{ g_in.haptic, Aeron_XrPath("/user/hand/right/output/haptic") },
			{ g_in.a, Aeron_XrPath("/user/hand/right/input/a/click") },
			{ g_in.b, Aeron_XrPath("/user/hand/right/input/b/click") },
			{ g_in.x, Aeron_XrPath("/user/hand/left/input/a/click") },
			{ g_in.y, Aeron_XrPath("/user/hand/left/input/b/click") },
			{ g_in.menu, Aeron_XrPath("/user/hand/left/input/system/click") },
		};
		const int touch_ok = Aeron_XrSuggest("/interaction_profiles/oculus/touch_controller", touch,
											 (uint32_t)SDL_arraysize(touch));
		Aeron_XrSuggest("/interaction_profiles/valve/index_controller", index,
						(uint32_t)SDL_arraysize(index));
		if (!touch_ok) {
			return 0;
		}
	}

	for (h = 0; h < AERON_XR_HAND_COUNT; ++h) {
		XrActionSpaceCreateInfo space         = { XR_TYPE_ACTION_SPACE_CREATE_INFO, NULL };
		space.action                          = g_in.aim;
		space.subactionPath                   = g_in.hand[h];
		space.poseInActionSpace.orientation.w = 1.0f;
		result = g_xr.xrCreateActionSpace(g_xr.session, &space, &g_in.aim_space[h]);
		if (XR_FAILED(result)) {
			Aeron_LogWarn("aeron.xr", "aim space creation failed: %s", Aeron_XrResultName(result));
		}
	}

	attach.countActionSets = 1;
	attach.actionSets      = &g_in.set;
	result                 = g_xr.xrAttachSessionActionSets(g_xr.session, &attach);
	if (XR_FAILED(result)) {
		Aeron_LogError("aeron.xr", "xrAttachSessionActionSets failed: %s", Aeron_XrResultName(result));
		return 0;
	}
	g_in.ready = 1;
	Aeron_LogInfo("aeron.xr", "controller input ready (Touch profile; virtual gamepad + menu pointer)");
	return 1;
}

void Aeron_XrInputShutdown(void) {
	int h;
	for (h = 0; h < AERON_XR_HAND_COUNT; ++h) {
		if (g_in.aim_space[h] && g_xr.xrDestroySpace) {
			g_xr.xrDestroySpace(g_in.aim_space[h]);
		}
	}
	if (g_in.set && g_xr.xrDestroyActionSet) {
		g_xr.xrDestroyActionSet(g_in.set); /* destroys its actions too */
	}
	if (g_in.slot >= 0) {
		memset(&g_aeron.input.controllers[g_in.slot], 0, sizeof g_aeron.input.controllers[g_in.slot]);
	}
	memset(&g_in, 0, sizeof g_in);
	g_in.slot = -1;
}

static float Aeron_XrFloat(XrAction action, int hand, int* active) {
	XrActionStateGetInfo info  = { XR_TYPE_ACTION_STATE_GET_INFO, NULL };
	XrActionStateFloat   state = { XR_TYPE_ACTION_STATE_FLOAT, NULL };
	info.action                = action;
	info.subactionPath         = hand >= 0 ? g_in.hand[hand] : XR_NULL_PATH;
	if (XR_FAILED(g_xr.xrGetActionStateFloat(g_xr.session, &info, &state)) || !state.isActive) {
		return 0.0f;
	}
	if (active) {
		*active = 1;
	}
	return state.currentState;
}

static int Aeron_XrBool(XrAction action, int hand) {
	XrActionStateGetInfo info  = { XR_TYPE_ACTION_STATE_GET_INFO, NULL };
	XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN, NULL };
	info.action                = action;
	info.subactionPath         = hand >= 0 ? g_in.hand[hand] : XR_NULL_PATH;
	if (XR_FAILED(g_xr.xrGetActionStateBoolean(g_xr.session, &info, &state)) || !state.isActive) {
		return 0;
	}
	return state.currentState ? 1 : 0;
}

static void Aeron_XrStick(int hand, float out[2], int* active) {
	XrActionStateGetInfo  info  = { XR_TYPE_ACTION_STATE_GET_INFO, NULL };
	XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F, NULL };
	out[0] = out[1]    = 0.0f;
	info.action        = g_in.stick;
	info.subactionPath = g_in.hand[hand];
	if (XR_FAILED(g_xr.xrGetActionStateVector2f(g_xr.session, &info, &state)) || !state.isActive) {
		return;
	}
	out[0] = state.currentState.x;
	out[1] = state.currentState.y;
	if (active) {
		*active = 1;
	}
}

static int Aeron_XrFindVirtualSlot(const AeronInputSnapshot* input) {
	int slot;
	if (g_in.slot >= 0 && g_aeron.controllers[g_in.slot].joystick == NULL) {
		return g_in.slot;
	}
	/* Use the highest free slot so a physical HOTAS/gamepad keeps priority
	 * for the game's "first connected controller" default. */
	for (slot = AERON_CONTROLLER_MAX - 1; slot >= 0; --slot) {
		if (g_aeron.controllers[slot].joystick == NULL && !input->controllers[slot].connected) {
			return slot;
		}
	}
	return -1;
}

/* Quaternion rotate v by q (x, y, z, w). */
static void Aeron_XrRotate(const XrQuaternionf* q, const float v[3], float out[3]) {
	const float tx = 2.0f * (q->y * v[2] - q->z * v[1]);
	const float ty = 2.0f * (q->z * v[0] - q->x * v[2]);
	const float tz = 2.0f * (q->x * v[1] - q->y * v[0]);
	out[0]         = v[0] + q->w * tx + (q->y * tz - q->z * ty);
	out[1]         = v[1] + q->w * ty + (q->z * tx - q->x * tz);
	out[2]         = v[2] + q->w * tz + (q->x * ty - q->y * tx);
}

/* Intersects the right aim ray with the virtual screen quad (LOCAL space,
 * facing +Z at z = -distance). Returns 1 with uv in [0,1] when it hits. */
static int Aeron_XrPointerUv(float* u, float* v) {
	XrSpaceLocation location   = { XR_TYPE_SPACE_LOCATION, NULL };
	const float     forward[3] = { 0.0f, 0.0f, -1.0f };
	float           dir[3];
	float           t;
	float           hit_x;
	float           hit_y;
	float           half_w;
	float           half_h;
	const XrSpace   space = g_in.aim_space[AERON_XR_HAND_RIGHT];

	if (!space || !g_xr.screen_width || !g_xr.predicted_display_time) {
		return 0;
	}
	if (XR_FAILED(g_xr.xrLocateSpace(space, g_xr.local_space, g_xr.predicted_display_time, &location)) ||
		!(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
		!(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
		return 0;
	}
	Aeron_XrRotate(&location.pose.orientation, forward, dir);
	if (dir[2] > -1.0e-4f) {
		return 0; /* parallel to or pointing away from the screen */
	}
	t = (-g_xr.screen_distance_m - location.pose.position.z) / dir[2];
	if (t <= 0.0f) {
		return 0;
	}
	hit_x  = location.pose.position.x + dir[0] * t;
	hit_y  = location.pose.position.y + dir[1] * t - g_xr.screen_height_offset_m;
	half_w = g_xr.screen_width_m * 0.5f;
	half_h = half_w * (float)g_xr.screen_height / (float)g_xr.screen_width;
	*u     = (hit_x + half_w) / (2.0f * half_w);
	*v     = (half_h - hit_y) / (2.0f * half_h);
	return *u >= 0.0f && *u <= 1.0f && *v >= 0.0f && *v <= 1.0f;
}

static void Aeron_XrApplyPointer(AeronInputSnapshot* input, const AeronXrTouchState* touch) {
	SDL_Rect content;
	float    u       = 0.0f;
	float    v       = 0.0f;
	uint32_t buttons = 0;
	uint32_t previous;
	int      hit;

	previous = g_in.pointer_buttons;
	hit      = g_xr_pointer_enabled && touch->active_right && Aeron_XrPointerUv(&u, &v);
	if (hit) {
		Aeron_ComputePresentationRect(g_xr.screen_width, g_xr.screen_height, &content);
		if (content.w > 0 && content.h > 0 && g_aeron.logical_width > 0 && g_aeron.logical_height > 0) {
			const float px = u * (float)g_xr.screen_width - (float)content.x;
			const float py = v * (float)g_xr.screen_height - (float)content.y;
			const float lx = px * (float)g_aeron.logical_width / (float)content.w;
			const float ly = py * (float)g_aeron.logical_height / (float)content.h;
			if (lx >= 0.0f && ly >= 0.0f && lx < (float)g_aeron.logical_width &&
				ly < (float)g_aeron.logical_height) {
				if (g_in.pointer_valid) {
					input->mouse.relative_x += lx - g_in.pointer_logical_x;
					input->mouse.relative_y += ly - g_in.pointer_logical_y;
				}
				g_in.pointer_logical_x      = lx;
				g_in.pointer_logical_y      = ly;
				g_in.pointer_valid          = 1;
				input->mouse.x              = (int)lx;
				input->mouse.y              = (int)ly;
				input->mouse.inside_content = 1;
				if (touch->right_trigger >= 0.5f) {
					buttons |= AERON_MOUSE_BUTTON_LEFT;
				}
				if (touch->right_grip >= 0.5f) {
					buttons |= AERON_MOUSE_BUTTON_RIGHT;
				}
			}
		}
	} else {
		g_in.pointer_valid = 0;
	}
	/* Keep a held click held until release even if the ray slips off. */
	if (!hit && previous) {
		if (touch->right_trigger >= 0.5f) {
			buttons |= previous & AERON_MOUSE_BUTTON_LEFT;
		}
		if (touch->right_grip >= 0.5f) {
			buttons |= previous & AERON_MOUSE_BUTTON_RIGHT;
		}
	}
	input->mouse.buttons |= buttons;
	input->mouse.pressed_buttons |= buttons & ~previous;
	input->mouse.released_buttons |= previous & ~buttons;
	g_in.pointer_buttons = buttons;
}

void Aeron_XrUpdateInput(AeronInputSnapshot* input, float dt_seconds) {
	XrActiveActionSet        active = { XR_NULL_HANDLE, XR_NULL_PATH };
	XrActionsSyncInfo        sync   = { XR_TYPE_ACTIONS_SYNC_INFO, NULL };
	AeronXrTouchState        touch;
	AeronControllerSnapshot* pad;
	XrResult                 result;
	int                      slot;

	if (!input || !g_in.ready || !Aeron_XrSessionRunning()) {
		return;
	}
	/* While the headset has focus the desktop window usually does not; the
	 * game must not pause because of that. */
	if (g_xr.state == XR_SESSION_STATE_FOCUSED) {
		input->has_focus = 1;
	} else {
		return; /* actions only report while focused */
	}

	active.actionSet           = g_in.set;
	sync.countActiveActionSets = 1;
	sync.activeActionSets      = &active;
	result                     = g_xr.xrSyncActions(g_xr.session, &sync);
	if (XR_FAILED(result) || result == XR_SESSION_NOT_FOCUSED) {
		return;
	}

	memset(&touch, 0, sizeof touch);
	Aeron_XrStick(AERON_XR_HAND_LEFT, touch.left_stick, &touch.active_left);
	Aeron_XrStick(AERON_XR_HAND_RIGHT, touch.right_stick, &touch.active_right);
	touch.left_trigger      = Aeron_XrFloat(g_in.trigger, AERON_XR_HAND_LEFT, &touch.active_left);
	touch.right_trigger     = Aeron_XrFloat(g_in.trigger, AERON_XR_HAND_RIGHT, &touch.active_right);
	touch.left_grip         = Aeron_XrFloat(g_in.grip, AERON_XR_HAND_LEFT, &touch.active_left);
	touch.right_grip        = Aeron_XrFloat(g_in.grip, AERON_XR_HAND_RIGHT, &touch.active_right);
	touch.left_stick_click  = Aeron_XrBool(g_in.stick_click, AERON_XR_HAND_LEFT);
	touch.right_stick_click = Aeron_XrBool(g_in.stick_click, AERON_XR_HAND_RIGHT);
	touch.a                 = Aeron_XrBool(g_in.a, -1);
	touch.b                 = Aeron_XrBool(g_in.b, -1);
	touch.x                 = Aeron_XrBool(g_in.x, -1);
	touch.y                 = Aeron_XrBool(g_in.y, -1);
	touch.menu              = Aeron_XrBool(g_in.menu, -1);

	if ((touch.active_left || touch.active_right) && !g_in.logged_active) {
		Aeron_LogInfo("aeron.xr", "controllers active (left %d, right %d)", touch.active_left,
					  touch.active_right);
		g_in.logged_active = 1;
	}

	/* Menu button -> Escape (pause / back), edge-triggered. */
	if (touch.menu && !g_in.menu_previous) {
		input->key_down[AERON_KEY_ESCAPE]    = 1;
		input->key_pressed[AERON_KEY_ESCAPE] = 1;
		input->key_typed[AERON_KEY_ESCAPE]++;
		if (input->key_event_count < AERON_KEY_EVENT_CAPACITY) {
			AeronKeyEvent* event = &input->key_events[input->key_event_count++];
			memset(event, 0, sizeof *event);
			event->chord.key = AERON_KEY_ESCAPE;
			event->down      = 1;
		}
	} else if (!touch.menu && g_in.menu_previous) {
		input->key_released[AERON_KEY_ESCAPE] = 1;
		if (input->key_event_count < AERON_KEY_EVENT_CAPACITY) {
			AeronKeyEvent* event = &input->key_events[input->key_event_count++];
			memset(event, 0, sizeof *event);
			event->chord.key = AERON_KEY_ESCAPE;
			event->down      = 0;
		}
	}
	g_in.menu_previous = touch.menu;

	/* Virtual gamepad. */
	slot = Aeron_XrFindVirtualSlot(input);
	if (slot >= 0) {
		const int fresh = slot != g_in.slot;
		g_in.slot       = slot;
		pad             = &input->controllers[slot];
		if (fresh) {
			memset(pad, 0, sizeof *pad);
			Aeron_LogInfo("aeron.xr", "virtual gamepad 'Meta Quest Touch (OpenXR)' in controller slot %d",
						  slot);
		}
		AeronXrTouchMapper_Map(&g_in.mapper, &touch, dt_seconds, pad);
		pad->instance_id = AERON_XR_CONTROLLER_INSTANCE_ID;
		pad->has_rumble  = 1;
		SDL_strlcpy(pad->name, "Meta Quest Touch (OpenXR)", sizeof pad->name);
		SDL_strlcpy(pad->path, "openxr:touch", sizeof pad->path);
		SDL_strlcpy(pad->guid, "4f70656e5852546f7563680000000001", sizeof pad->guid);
		/* The menu button is Escape, not a gamepad button. */
		pad->gamepad_buttons &= ~(1u << AERON_GAMEPAD_BUTTON_GUIDE);
		pad->gamepad_pressed_buttons &= ~(1u << AERON_GAMEPAD_BUTTON_GUIDE);
		pad->gamepad_released_buttons &= ~(1u << AERON_GAMEPAD_BUTTON_GUIDE);
		if (g_xr_pointer_enabled) {
			/* In menus the right trigger/grip are the mouse; don't also fire. */
			pad->gamepad_buttons &= ~((1u << AERON_GAMEPAD_BUTTON_SOUTH) | (1u << AERON_GAMEPAD_BUTTON_EAST));
			pad->gamepad_pressed_buttons &=
				~((1u << AERON_GAMEPAD_BUTTON_SOUTH) | (1u << AERON_GAMEPAD_BUTTON_EAST));
		}
	}

	Aeron_XrApplyPointer(input, &touch);
}

int Aeron_XrRumble(uint16_t low, uint16_t high, uint32_t duration_ms) {
	XrHapticVibration  vibration = { XR_TYPE_HAPTIC_VIBRATION, NULL };
	XrHapticActionInfo info      = { XR_TYPE_HAPTIC_ACTION_INFO, NULL };
	const float        amplitude = (float)(low > high ? low : high) / 65535.0f;
	int                h;

	if (!g_in.ready || !Aeron_XrSessionRunning()) {
		return 0;
	}
	info.action = g_in.haptic;
	for (h = 0; h < AERON_XR_HAND_COUNT; ++h) {
		info.subactionPath = g_in.hand[h];
		if (amplitude <= 0.0f || duration_ms == 0) {
			g_xr.xrStopHapticFeedback(g_xr.session, &info);
			continue;
		}
		vibration.amplitude = amplitude;
		vibration.duration  = (XrDuration)duration_ms * 1000000;
		vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
		g_xr.xrApplyHapticFeedback(g_xr.session, &info, (const XrHapticBaseHeader*)&vibration);
	}
	return 1;
}

#endif /* AERON_HAS_OPENXR */
