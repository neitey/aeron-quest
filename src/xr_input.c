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
 * - Menu-keys mode (Aeron_XrSetMenuKeys) turns sticks and face buttons into
 *   arrow/Enter/Space/Escape keys for keyboard-only menus.
 * - Flight-buttons mode (Aeron_XrSetFlightButtons) queues face-button,
 *   left-trigger, stick-click and grip presses (left grip = shift layer,
 *   right-stick flicks while shifted) for the game to bind to its actions.
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
static int g_xr_menu_keys       = 0;
static int g_xr_flight_buttons  = 0;
/* Triggers owned by a game UI (Aeron_XrCaptureTrigger), and pulls that
 * started while owned (hidden until released). */
static int g_xr_trigger_captured[2];
static int g_xr_trigger_latched[2];

void Aeron_XrCaptureTrigger(int hand, int captured) {
	if (hand >= 0 && hand < 2) {
		g_xr_trigger_captured[hand] = captured != 0;
	}
}

/* Grips owned by a game UI, and squeezes that started while owned. */
static int g_xr_grip_captured[2];
static int g_xr_grip_latched[2];

void Aeron_XrCaptureGrip(int hand, int captured) {
	if (hand >= 0 && hand < 2) {
		g_xr_grip_captured[hand] = captured != 0;
	}
}

/* A game-drawn flight stick (see Aeron_XrSetVirtualStick). */
static int   g_xr_vstick_active;
static float g_xr_vstick[3];

void Aeron_XrSetVirtualStick(int active, float x, float y, float roll) {
	g_xr_vstick_active = active != 0;
	g_xr_vstick[0]     = active ? x : 0.0f;
	g_xr_vstick[1]     = active ? y : 0.0f;
	g_xr_vstick[2]     = active ? roll : 0.0f;
}

#define AERON_XR_BUTTON_QUEUE 32

typedef struct AeronXrButtonPress {
	uint8_t button;
	uint8_t shifted;
} AeronXrButtonPress;

static AeronXrButtonPress g_xr_button_queue[AERON_XR_BUTTON_QUEUE];
static int                g_xr_button_head;
static int                g_xr_button_count;

void Aeron_XrSetPointerEnabled(int enabled) { g_xr_pointer_enabled = enabled != 0; }

void Aeron_XrSetMenuKeys(int enabled) { g_xr_menu_keys = enabled != 0; }

/* A game's title gate: the controllers reach nothing but the A latch. */
static int g_xr_input_blocked;
static int g_xr_blocked_a_previous = 1; /* a held A must be let go first */
static int g_xr_blocked_a_pressed;
/* After a block: the pointer clicks nothing until trigger and grip are let
 * go (the press that lifted the block must not click the menu). */
static int g_xr_pointer_holdoff;

void Aeron_XrSetInputBlocked(int blocked) {
	if (blocked && !g_xr_input_blocked) {
		g_xr_blocked_a_previous = 1;
		g_xr_blocked_a_pressed  = 0;
	}
	if (!blocked && g_xr_input_blocked) {
		g_xr_pointer_holdoff = 1;
	}
	g_xr_input_blocked = blocked != 0;
}

int Aeron_XrTakeAPressed(void) {
	const int pressed = g_xr_blocked_a_pressed;
	g_xr_blocked_a_pressed = 0;
	return pressed;
}

/* B and X pressed in the menus (pointer on, not keyboard menus, input not
 * blocked). */
static int g_xr_menu_b_previous = 1;
static int g_xr_menu_b_pressed;
static int g_xr_menu_x_previous = 1;
static int g_xr_menu_x_pressed;

int Aeron_XrTakeMenuBPressed(void) {
	const int pressed = g_xr_menu_b_pressed;
	g_xr_menu_b_pressed = 0;
	return pressed;
}

int Aeron_XrTakeMenuXPressed(void) {
	const int pressed = g_xr_menu_x_pressed;
	g_xr_menu_x_pressed = 0;
	return pressed;
}

int Aeron_XrLeftHanded(void) {
	const char* v = SDL_getenv("AERON_XR_LEFT_HANDED");
	return v && v[0] == '1';
}

/* Left-handed: the controllers trade their game roles (see aeron.h). */
static void Aeron_XrMirrorTouch(AeronXrTouchState* t) {
	float f[2];
	float v;
	int   i;
	memcpy(f, t->left_stick, sizeof f);
	memcpy(t->left_stick, t->right_stick, sizeof f);
	memcpy(t->right_stick, f, sizeof f);
	v = t->left_trigger; t->left_trigger = t->right_trigger; t->right_trigger = v;
	v = t->left_grip; t->left_grip = t->right_grip; t->right_grip = v;
	i = t->active_left; t->active_left = t->active_right; t->active_right = i;
	i = t->left_stick_click; t->left_stick_click = t->right_stick_click; t->right_stick_click = i;
	i = t->a; t->a = t->x; t->x = i;
	i = t->b; t->b = t->y; t->y = i;
}

/* The physical hand behind a game-side (logical) hand. */
static int Aeron_XrPhysicalHand(int logical) {
	return Aeron_XrLeftHanded() ? 1 - logical : logical; /* two hands: 0 left, 1 right */
}

void Aeron_XrSetFlightButtons(int enabled) {
	g_xr_flight_buttons = enabled != 0;
	if (!g_xr_flight_buttons) {
		g_xr_button_head  = 0;
		g_xr_button_count = 0;
	}
}

int Aeron_XrPollButton(AeronXrButton* button, int* shifted) {
	const AeronXrButtonPress* press;
	if (g_xr_button_count <= 0) {
		return 0;
	}
	press            = &g_xr_button_queue[g_xr_button_head];
	g_xr_button_head = (g_xr_button_head + 1) % AERON_XR_BUTTON_QUEUE;
	--g_xr_button_count;
	if (button) {
		*button = (AeronXrButton)press->button;
	}
	if (shifted) {
		*shifted = press->shifted;
	}
	return 1;
}

#if defined(AERON_HAS_OPENXR)
static void Aeron_XrQueueButton(AeronXrButton button, int shifted) {
	AeronXrButtonPress* press;
	if (g_xr_button_count >= AERON_XR_BUTTON_QUEUE) {
		return; /* the game is not draining; drop rather than replay late */
	}
	press          = &g_xr_button_queue[(g_xr_button_head + g_xr_button_count) % AERON_XR_BUTTON_QUEUE];
	press->button  = (uint8_t)button;
	press->shifted = (uint8_t)(shifted != 0);
	++g_xr_button_count;
}
#endif

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

int Aeron_XrGetHand(int hand, AeronXrHand* out) {
	(void)hand;
	if (out) {
		memset(out, 0, sizeof *out);
	}
	return 0;
}

int Aeron_XrThumbrestTouch(int hand) {
	(void)hand;
	return -1;
}

void Aeron_XrHapticPulse(int hand, float amplitude, int duration_ms) {
	(void)hand;
	(void)amplitude;
	(void)duration_ms;
}

int Aeron_XrPointerHit(float out[3]) {
	(void)out;
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
	/* Capacitive touch (Touch controllers): index finger on the trigger,
	 * thumb on any thumb surface. For the VR hands' poses. */
	XrAction    trigger_touch, thumb_touch, thumbrest_touch;
	int         touch_bound;
	XrSpace     aim_space[AERON_XR_HAND_COUNT];

	AeronXrTouchMapper mapper;
	int                slot; /* controller slot used for the virtual gamepad */
	uint32_t           pointer_buttons;
	int                pointer_valid;
	int                menu_previous;
	float              pointer_logical_x;
	float              pointer_logical_y;
	int                logged_active;

	/* Keys this module is holding down in the input snapshot. */
	uint8_t key_held[AERON_KEY_COUNT];
	/* Menu keys: current stick direction key and its repeat timer. */
	AeronKey menu_dir_key;
	float    menu_repeat_timer;
	int      menu_enter_prev, menu_space_prev, menu_escape_prev;
	/* Flight buttons: previous digital state per AeronXrButton. */
	uint8_t button_prev[AERON_XR_BUTTON_COUNT];
	/* Last synced analog values, for Aeron_XrGetHand. */
	float hand_trigger[AERON_XR_HAND_COUNT];
	float hand_grip[AERON_XR_HAND_COUNT];
	int   hand_index_touch[AERON_XR_HAND_COUNT]; /* -1 unknown */
	int   hand_thumb_touch[AERON_XR_HAND_COUNT];
	int   hand_thumbrest_touch[AERON_XR_HAND_COUNT];
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
		const char* zone = SDL_getenv("AERON_XR_THROTTLE_ZONE");
		if (zone && zone[0] && SDL_atof(zone) > 0.0 && SDL_atof(zone) < 90.0) {
			g_in.mapper.throttle_zone_deg = (float)SDL_atof(zone);
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
	g_in.trigger_touch = Aeron_XrCreateAction("trigger_touch", "Finger on trigger", XR_ACTION_TYPE_BOOLEAN_INPUT, 1);
	g_in.thumb_touch   = Aeron_XrCreateAction("thumb_touch", "Thumb resting", XR_ACTION_TYPE_BOOLEAN_INPUT, 1);
	g_in.thumbrest_touch =
		Aeron_XrCreateAction("thumbrest_touch", "Thumb on the thumb rest", XR_ACTION_TYPE_BOOLEAN_INPUT, 1);
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
		/* The same bindings plus capacitive touch. A suggestion replaces any
		 * earlier one for the profile, so fall back to the plain set if the
		 * runtime rejects the touch paths. */
		XrActionSuggestedBinding touch_ext[SDL_arraysize(touch) + 12];
		uint32_t                 touch_ext_count = 0;
		int                      touch_ok        = 0;
		for (uint32_t k = 0; k < (uint32_t)SDL_arraysize(touch); ++k) {
			touch_ext[touch_ext_count++] = touch[k];
		}
		if (g_in.trigger_touch && g_in.thumb_touch) {
			static const char* const thumb_paths[] = {
				"/user/hand/left/input/thumbrest/touch",  "/user/hand/right/input/thumbrest/touch",
				"/user/hand/left/input/thumbstick/touch", "/user/hand/right/input/thumbstick/touch",
				"/user/hand/left/input/x/touch",          "/user/hand/left/input/y/touch",
				"/user/hand/right/input/a/touch",         "/user/hand/right/input/b/touch",
			};
			touch_ext[touch_ext_count++] =
				(XrActionSuggestedBinding) { g_in.trigger_touch, Aeron_XrPath("/user/hand/left/input/trigger/touch") };
			touch_ext[touch_ext_count++] =
				(XrActionSuggestedBinding) { g_in.trigger_touch, Aeron_XrPath("/user/hand/right/input/trigger/touch") };
			for (uint32_t k = 0; k < (uint32_t)SDL_arraysize(thumb_paths); ++k) {
				touch_ext[touch_ext_count++] =
					(XrActionSuggestedBinding) { g_in.thumb_touch, Aeron_XrPath(thumb_paths[k]) };
			}
			if (g_in.thumbrest_touch) {
				touch_ext[touch_ext_count++] = (XrActionSuggestedBinding) {
					g_in.thumbrest_touch, Aeron_XrPath("/user/hand/left/input/thumbrest/touch") };
				touch_ext[touch_ext_count++] = (XrActionSuggestedBinding) {
					g_in.thumbrest_touch, Aeron_XrPath("/user/hand/right/input/thumbrest/touch") };
			}
			touch_ok = Aeron_XrSuggest("/interaction_profiles/oculus/touch_controller", touch_ext, touch_ext_count);
			g_in.touch_bound = touch_ok;
		}
		if (!touch_ok) {
			touch_ok = Aeron_XrSuggest("/interaction_profiles/oculus/touch_controller", touch,
									   (uint32_t)SDL_arraysize(touch));
		}
		Aeron_XrSuggest("/interaction_profiles/valve/index_controller", index,
						(uint32_t)SDL_arraysize(index));
		{
			/* Pico 4 / Neo 3 / 4 Ultra and HTC Focus 3 controllers: Touch's
			 * layout (A/B right, X/Y left, menu left), with touch sensors on
			 * trigger, stick and face buttons but no thumb rest (HTC has one).
			 * Their runtimes don't take Touch bindings for them, so each needs
			 * its own suggestion. */
			static const char* const other_thumb_paths[] = {
				"/user/hand/left/input/thumbstick/touch", "/user/hand/right/input/thumbstick/touch",
				"/user/hand/left/input/x/touch",          "/user/hand/left/input/y/touch",
				"/user/hand/right/input/a/touch",         "/user/hand/right/input/b/touch",
			};
			XrActionSuggestedBinding other[SDL_arraysize(touch) + 12];
			uint32_t                 base_count = 0;
			for (uint32_t k = 0; k < (uint32_t)SDL_arraysize(touch); ++k) {
				other[base_count++] = touch[k];
			}
			struct {
				int         offered;
				const char* profile;
				int         face_touch; /* x/y/a/b touch sensors */
				int         thumbrest;
			} const others[] = {
				{ g_xr.has_bd_controller, "/interaction_profiles/bytedance/pico4_controller", 1, 0 },
				{ g_xr.has_bd_controller, "/interaction_profiles/bytedance/pico_neo3_controller", 1, 0 },
				{ g_xr.has_bd_ultra, "/interaction_profiles/bytedance/pico_ultra_controller_bd", 1, 0 },
				{ g_xr.has_htc_focus3, "/interaction_profiles/htc/vive_focus3_controller", 0, 1 },
			};
			for (size_t o = 0; o < SDL_arraysize(others); ++o) {
				uint32_t count = base_count;
				if (!others[o].offered) {
					continue;
				}
				if (g_in.trigger_touch && g_in.thumb_touch) {
					other[count++] = (XrActionSuggestedBinding) {
						g_in.trigger_touch, Aeron_XrPath("/user/hand/left/input/trigger/touch") };
					other[count++] = (XrActionSuggestedBinding) {
						g_in.trigger_touch, Aeron_XrPath("/user/hand/right/input/trigger/touch") };
					for (uint32_t k = 0; k < (uint32_t)SDL_arraysize(other_thumb_paths); ++k) {
						if (k >= 2 && !others[o].face_touch) {
							break;
						}
						other[count++] =
							(XrActionSuggestedBinding) { g_in.thumb_touch, Aeron_XrPath(other_thumb_paths[k]) };
					}
					if (others[o].thumbrest && g_in.thumbrest_touch) {
						other[count++] = (XrActionSuggestedBinding) {
							g_in.thumbrest_touch, Aeron_XrPath("/user/hand/left/input/thumbrest/touch") };
						other[count++] = (XrActionSuggestedBinding) {
							g_in.thumbrest_touch, Aeron_XrPath("/user/hand/right/input/thumbrest/touch") };
					}
				}
				if (Aeron_XrSuggest(others[o].profile, other, count)) {
					g_in.touch_bound = g_in.touch_bound || count > base_count;
				} else if (count > base_count && Aeron_XrSuggest(others[o].profile, other, base_count)) {
					/* fine without the touch sensors */
				} else {
					continue;
				}
				touch_ok = 1;
				Aeron_LogInfo("aeron.xr", "bindings suggested for %s", others[o].profile);
			}
		}
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
	Aeron_LogInfo("aeron.xr", "controller input ready (virtual gamepad + menu pointer)");
	return 1;
}

void Aeron_XrInputLogProfiles(void) {
	static const char* const hand_name[AERON_XR_HAND_COUNT] = { "left", "right" };
	if (!g_in.ready || !g_xr.xrGetCurrentInteractionProfile || !g_xr.xrPathToString) {
		return;
	}
	for (int h = 0; h < AERON_XR_HAND_COUNT; ++h) {
		XrInteractionProfileState state = { XR_TYPE_INTERACTION_PROFILE_STATE, NULL };
		char                      name[XR_MAX_PATH_LENGTH];
		uint32_t                  length = 0;
		XrResult                  result = g_xr.xrGetCurrentInteractionProfile(g_xr.session, g_in.hand[h], &state);
		if (XR_FAILED(result)) {
			Aeron_LogWarn("aeron.xr", "%s controller profile unknown: %s", hand_name[h], Aeron_XrResultName(result));
			continue;
		}
		if (state.interactionProfile == XR_NULL_PATH) {
			Aeron_LogInfo("aeron.xr", "%s controller: none (off, or no binding matches it)", hand_name[h]);
			continue;
		}
		if (XR_FAILED(g_xr.xrPathToString(g_xr.instance, state.interactionProfile, sizeof name, &length, name))) {
			SDL_strlcpy(name, "?", sizeof name);
		}
		Aeron_LogInfo("aeron.xr", "%s controller: %s", hand_name[h], name);
	}
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

/* Presses (down = 1), repeats (down = 2) or releases (down = 0) a key in the
 * snapshot the way a physical keyboard event would. */
static void Aeron_XrKey(AeronInputSnapshot* input, AeronKey key, int down) {
	AeronKeyEvent* event;
	if ((int)key <= 0 || (int)key >= AERON_KEY_COUNT) {
		return;
	}
	if (down) {
		if (!input->key_down[key] && down == 1) {
			input->key_pressed[key] = 1;
		}
		if (input->key_typed[key] < 255) {
			input->key_typed[key]++;
		}
		input->key_down[key] = 1;
		g_in.key_held[key]   = 1;
	} else {
		if (!g_in.key_held[key]) {
			return;
		}
		if (input->key_down[key]) {
			input->key_released[key] = 1;
		}
		input->key_down[key] = 0;
		g_in.key_held[key]   = 0;
	}
	if (input->key_event_count < AERON_KEY_EVENT_CAPACITY) {
		event = &input->key_events[input->key_event_count++];
		memset(event, 0, sizeof *event);
		event->chord.key = (uint16_t)key;
		event->down      = down != 0;
		event->repeat    = down == 2;
	} else {
		input->key_events_overflow = 1;
	}
}

/* Edge-triggered key that follows a digital source. */
static void Aeron_XrKeyFollow(AeronInputSnapshot* input, AeronKey key, int now, int* previous) {
	if (now && !*previous) {
		Aeron_XrKey(input, key, 1);
	} else if (!now && *previous) {
		Aeron_XrKey(input, key, 0);
	}
	*previous = now;
}

static void Aeron_XrReleaseMenuKeys(AeronInputSnapshot* input) {
	if (g_in.menu_dir_key) {
		Aeron_XrKey(input, g_in.menu_dir_key, 0);
		g_in.menu_dir_key = (AeronKey)0;
	}
	Aeron_XrKeyFollow(input, AERON_KEY_RETURN, 0, &g_in.menu_enter_prev);
	Aeron_XrKeyFollow(input, AERON_KEY_SPACE, 0, &g_in.menu_space_prev);
	if (g_in.menu_escape_prev) {
		Aeron_XrKeyFollow(input, AERON_KEY_ESCAPE, 0, &g_in.menu_escape_prev);
	}
}

/* Thumbsticks -> arrow keys with typematic repeat; A / right trigger ->
 * Enter, X -> Space, B -> Escape. */
static void Aeron_XrApplyMenuKeys(AeronInputSnapshot* input, const AeronXrTouchState* touch, float dt) {
	const float press_threshold   = 0.6f;
	const float release_threshold = 0.35f;
	float       sx                = 0.0f;
	float       sy                = 0.0f;
	float       mag;
	AeronKey    dir = (AeronKey)0;

	/* The stick deflected furthest wins. */
	if (touch->active_right) {
		sx = touch->right_stick[0];
		sy = touch->right_stick[1];
	}
	if (touch->active_left &&
		fmaxf(fabsf(touch->left_stick[0]), fabsf(touch->left_stick[1])) > fmaxf(fabsf(sx), fabsf(sy))) {
		sx = touch->left_stick[0];
		sy = touch->left_stick[1];
	}
	mag = fmaxf(fabsf(sx), fabsf(sy));
	if (g_in.menu_dir_key && mag > release_threshold) {
		dir = g_in.menu_dir_key; /* hysteresis: keep the held direction */
		if (mag >= press_threshold) {
			const AeronKey wanted = fabsf(sx) > fabsf(sy) ? (sx > 0.0f ? AERON_KEY_RIGHT : AERON_KEY_LEFT)
														  : (sy > 0.0f ? AERON_KEY_UP : AERON_KEY_DOWN);
			dir                   = wanted;
		}
	} else if (mag >= press_threshold) {
		dir = fabsf(sx) > fabsf(sy) ? (sx > 0.0f ? AERON_KEY_RIGHT : AERON_KEY_LEFT)
									: (sy > 0.0f ? AERON_KEY_UP : AERON_KEY_DOWN);
	}
	if (dir != g_in.menu_dir_key) {
		if (g_in.menu_dir_key) {
			Aeron_XrKey(input, g_in.menu_dir_key, 0);
		}
		g_in.menu_dir_key = dir;
		if (dir) {
			Aeron_XrKey(input, dir, 1);
			g_in.menu_repeat_timer = 0.45f;
		}
	} else if (dir) {
		g_in.menu_repeat_timer -= dt;
		if (g_in.menu_repeat_timer <= 0.0f) {
			Aeron_XrKey(input, dir, 2);
			g_in.menu_repeat_timer += 0.15f;
			if (g_in.menu_repeat_timer <= 0.0f) {
				g_in.menu_repeat_timer = 0.15f;
			}
		}
	}

	Aeron_XrKeyFollow(input, AERON_KEY_RETURN, touch->a || (touch->active_right && touch->right_trigger >= 0.5f),
					  &g_in.menu_enter_prev);
	Aeron_XrKeyFollow(input, AERON_KEY_SPACE, touch->x, &g_in.menu_space_prev);
	Aeron_XrKeyFollow(input, AERON_KEY_ESCAPE, touch->b, &g_in.menu_escape_prev);
}

/* Records flight-button edges. Always tracks state so that turning the mode
 * on mid-press does not produce a phantom press. */
static void Aeron_XrTrackFlightButtons(const AeronXrTouchState* touch) {
	const int shifted = touch->active_left && touch->left_grip >= 0.5f;
	uint8_t   now[AERON_XR_BUTTON_COUNT];
	int       b;

	memset(now, 0, sizeof now);
	now[AERON_XR_BUTTON_A]            = (uint8_t)touch->a;
	now[AERON_XR_BUTTON_B]            = (uint8_t)touch->b;
	now[AERON_XR_BUTTON_X]            = (uint8_t)touch->x;
	now[AERON_XR_BUTTON_Y]            = (uint8_t)touch->y;
	now[AERON_XR_BUTTON_LEFT_STICK]   = (uint8_t)touch->left_stick_click;
	now[AERON_XR_BUTTON_RIGHT_STICK]  = (uint8_t)touch->right_stick_click;
	/* Hysteresis on the analog ones so a resting finger doesn't chatter. */
	now[AERON_XR_BUTTON_LEFT_TRIGGER] = (uint8_t)(touch->active_left &&
		touch->left_trigger >= (g_in.button_prev[AERON_XR_BUTTON_LEFT_TRIGGER] ? 0.35f : 0.5f));
	now[AERON_XR_BUTTON_RIGHT_GRIP]   = (uint8_t)(touch->active_right &&
		touch->right_grip >= (g_in.button_prev[AERON_XR_BUTTON_RIGHT_GRIP] ? 0.35f : 0.5f));
	if (shifted && touch->active_right) {
		const float x       = touch->right_stick[0];
		const float y       = touch->right_stick[1];
		const float mag     = fmaxf(fabsf(x), fabsf(y));
		int         was_any = 0;
		for (b = AERON_XR_BUTTON_FLICK_UP; b <= AERON_XR_BUTTON_FLICK_RIGHT; ++b) {
			was_any |= g_in.button_prev[b];
		}
		if (was_any && mag > 0.35f) {
			/* Hold the flick until the stick comes back to centre. */
			for (b = AERON_XR_BUTTON_FLICK_UP; b <= AERON_XR_BUTTON_FLICK_RIGHT; ++b) {
				now[b] = g_in.button_prev[b];
			}
		} else if (mag >= 0.7f) {
			if (fabsf(x) > fabsf(y)) {
				now[x > 0.0f ? AERON_XR_BUTTON_FLICK_RIGHT : AERON_XR_BUTTON_FLICK_LEFT] = 1;
			} else {
				now[y > 0.0f ? AERON_XR_BUTTON_FLICK_UP : AERON_XR_BUTTON_FLICK_DOWN] = 1;
			}
		}
	}
	for (b = 0; b < AERON_XR_BUTTON_COUNT; ++b) {
		if (b == AERON_XR_BUTTON_LEFT_TRIGGER &&
			(g_xr_trigger_captured[Aeron_XrPhysicalHand(AERON_XR_HAND_LEFT)] ||
			 g_xr_trigger_latched[Aeron_XrPhysicalHand(AERON_XR_HAND_LEFT)])) {
			g_in.button_prev[b] = now[b]; /* owned by a game UI */
			continue;
		}
		if (g_xr_flight_buttons && now[b] && !g_in.button_prev[b]) {
			Aeron_XrQueueButton((AeronXrButton)b, shifted);
		}
		g_in.button_prev[b] = now[b];
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
static int Aeron_XrPointerUvHit(float* u, float* v, float hit_local[3]) {
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
	if (hit_local) {
		hit_local[0] = hit_x;
		hit_local[1] = location.pose.position.y + dir[1] * t;
		hit_local[2] = -g_xr.screen_distance_m;
	}
	half_w = g_xr.screen_width_m * 0.5f;
	half_h = half_w * (float)g_xr.screen_height / (float)g_xr.screen_width;
	*u     = (hit_x + half_w) / (2.0f * half_w);
	*v     = (half_h - hit_y) / (2.0f * half_h);
	return *u >= 0.0f && *u <= 1.0f && *v >= 0.0f && *v <= 1.0f;
}

static int Aeron_XrPointerUv(float* u, float* v) { return Aeron_XrPointerUvHit(u, v, NULL); }

int Aeron_XrPointerHit(float out[3]) {
	float u = 0.0f;
	float v = 0.0f;
	if (!g_xr_pointer_enabled || !g_in.ready || !out) {
		return 0;
	}
	return Aeron_XrPointerUvHit(&u, &v, out);
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
	if (g_xr_pointer_holdoff) {
		if (touch->right_trigger < 0.2f && touch->right_grip < 0.2f) {
			g_xr_pointer_holdoff = 0;
		}
		buttons = 0;
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

	g_in.hand_trigger[AERON_XR_HAND_LEFT]  = touch.left_trigger;
	g_in.hand_trigger[AERON_XR_HAND_RIGHT] = touch.right_trigger;
	g_in.hand_grip[AERON_XR_HAND_LEFT]     = touch.left_grip;
	g_in.hand_grip[AERON_XR_HAND_RIGHT]    = touch.right_grip;
	for (int h = 0; h < AERON_XR_HAND_COUNT; ++h) {
		g_in.hand_index_touch[h] = -1;
		g_in.hand_thumb_touch[h] = -1;
		g_in.hand_thumbrest_touch[h] = -1;
		if (g_in.touch_bound) {
			XrActionStateGetInfo info  = { XR_TYPE_ACTION_STATE_GET_INFO, NULL };
			XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN, NULL };
			info.subactionPath         = g_in.hand[h];
			info.action                = g_in.trigger_touch;
			if (XR_SUCCEEDED(g_xr.xrGetActionStateBoolean(g_xr.session, &info, &state)) && state.isActive) {
				g_in.hand_index_touch[h] = state.currentState ? 1 : 0;
			}
			info.action = g_in.thumb_touch;
			state.type  = XR_TYPE_ACTION_STATE_BOOLEAN;
			state.next  = NULL;
			if (XR_SUCCEEDED(g_xr.xrGetActionStateBoolean(g_xr.session, &info, &state)) && state.isActive) {
				g_in.hand_thumb_touch[h] = state.currentState ? 1 : 0;
			}
			if (g_in.thumbrest_touch) {
				info.action = g_in.thumbrest_touch;
				state.type  = XR_TYPE_ACTION_STATE_BOOLEAN;
				state.next  = NULL;
				if (XR_SUCCEEDED(g_xr.xrGetActionStateBoolean(g_xr.session, &info, &state)) && state.isActive) {
					g_in.hand_thumbrest_touch[h] = state.currentState ? 1 : 0;
				}
			}
		}
	}
	for (int h = 0; h < AERON_XR_HAND_COUNT; ++h) {
		if (g_in.hand_trigger[h] < 0.2f) {
			g_xr_trigger_latched[h] = 0;
		} else if (g_xr_trigger_captured[h] && g_in.hand_trigger[h] >= 0.35f) {
			g_xr_trigger_latched[h] = 1;
		}
		if (g_in.hand_grip[h] < 0.2f) {
			g_xr_grip_latched[h] = 0;
		} else if (g_xr_grip_captured[h] && g_in.hand_grip[h] >= 0.35f) {
			g_xr_grip_latched[h] = 1;
		}
	}
	/* A grip a game UI owns (e.g. grabbing a panel) is not a game input. */
	if (g_xr_grip_captured[AERON_XR_HAND_LEFT] || g_xr_grip_latched[AERON_XR_HAND_LEFT]) {
		touch.left_grip = 0.0f;
	}
	if (g_xr_grip_captured[AERON_XR_HAND_RIGHT] || g_xr_grip_latched[AERON_XR_HAND_RIGHT]) {
		touch.right_grip = 0.0f;
	}

	/* The menu pointer stays on the right controller: it gets the physical
	 * state; everything else the game-side one. */
	const AeronXrTouchState physical = touch;
	{
		const int menus = g_xr_pointer_enabled && !g_xr_menu_keys && !g_xr_input_blocked;
		if (menus && physical.b && !g_xr_menu_b_previous) {
			g_xr_menu_b_pressed = 1;
		}
		if (menus && physical.x && !g_xr_menu_x_previous) {
			g_xr_menu_x_pressed = 1;
		}
		g_xr_menu_b_previous = physical.b;
		g_xr_menu_x_previous = physical.x;
	}
	if (g_xr_input_blocked) {
		/* Nothing reaches the game but the A (or right trigger) press it
		 * waits for. */
		const int a = physical.a || physical.right_trigger >= 0.6f;
		if (a && !g_xr_blocked_a_previous) {
			g_xr_blocked_a_pressed = 1;
		}
		g_xr_blocked_a_previous = physical.a || physical.right_trigger >= 0.3f;
		Aeron_XrReleaseMenuKeys(input);
		slot = Aeron_XrFindVirtualSlot(input);
		if (slot >= 0 && slot == g_in.slot) {
			AeronControllerSnapshot* idle = &input->controllers[slot];
			memset(idle->gamepad_axes, 0, sizeof idle->gamepad_axes);
			idle->gamepad_buttons = idle->gamepad_pressed_buttons = idle->gamepad_released_buttons = 0;
		}
		return;
	}
	if (Aeron_XrLeftHanded()) {
		Aeron_XrMirrorTouch(&touch);
	}

	if ((touch.active_left || touch.active_right) && !g_in.logged_active) {
		Aeron_LogInfo("aeron.xr", "controllers active (left %d, right %d)", touch.active_left,
					  touch.active_right);
		g_in.logged_active = 1;
	}

	/* Menu button -> Escape (pause / back), edge-triggered. */
	Aeron_XrKeyFollow(input, AERON_KEY_ESCAPE, touch.menu, &g_in.menu_previous);

	/* Keyboard-only menus and bindable flight buttons. */
	Aeron_XrTrackFlightButtons(&touch);
	if (g_xr_menu_keys) {
		Aeron_XrApplyMenuKeys(input, &touch, dt_seconds);
	} else {
		Aeron_XrReleaseMenuKeys(input);
	}

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
		{
			/* What the game's gamepad bindings get to see. */
			AeronXrTouchState mapped = touch;
			if (g_xr_menu_keys) {
				/* Everything is a key in these menus; keep the throttle lever
				 * where it is. */
				memset(mapped.left_stick, 0, sizeof mapped.left_stick);
				memset(mapped.right_stick, 0, sizeof mapped.right_stick);
				mapped.left_trigger = mapped.right_trigger = 0.0f;
				mapped.left_grip = mapped.right_grip = 0.0f;
				mapped.a = mapped.b = mapped.x = mapped.y = 0;
				mapped.left_stick_click = mapped.right_stick_click = 0;
			} else if (g_xr_flight_buttons) {
				const int shifted = touch.active_left && touch.left_grip >= 0.5f;
				mapped.a = mapped.b = mapped.x = mapped.y = 0;
				mapped.left_trigger      = 0.0f;
				mapped.left_grip         = 0.0f;
				mapped.left_stick_click  = 0;
				mapped.right_stick_click = 0;
				if (shifted) {
					mapped.right_grip = 0.0f;
					memset(mapped.right_stick, 0, sizeof mapped.right_stick); /* flicks */
				}
			}
			if (!g_xr_menu_keys) {
				/* AERON_XR_INVERT_PITCH=1: thumbstick back = nose down (the
				 * flight stick keeps its real-stick sense). Read every frame
				 * so the VR options apply at once. */
				const char* invert = SDL_getenv("AERON_XR_INVERT_PITCH");
				if (invert && invert[0] == '1') {
					mapped.right_stick[1] = -mapped.right_stick[1];
				}
			}
			if (g_xr_vstick_active && !g_xr_menu_keys) {
				/* The game's virtual flight stick: per axis, whichever of it
				 * and the thumbstick is pushed further. */
				for (int a = 0; a < 2; ++a) {
					if (fabsf(g_xr_vstick[a]) > fabsf(mapped.right_stick[a])) {
						mapped.right_stick[a] = g_xr_vstick[a];
					}
				}
				mapped.active_right = 1;
				mapped.extra_roll   = g_xr_vstick[2];
			}
			{
				const int fire = Aeron_XrPhysicalHand(AERON_XR_HAND_RIGHT);
				const int other = Aeron_XrPhysicalHand(AERON_XR_HAND_LEFT);
				if (g_xr_trigger_captured[fire] || g_xr_trigger_latched[fire]) {
					mapped.right_trigger = 0.0f; /* owned by a game UI: no fire */
				}
				if (g_xr_trigger_captured[other] || g_xr_trigger_latched[other]) {
					mapped.left_trigger = 0.0f;
				}
			}
			AeronXrTouchMapper_Map(&g_in.mapper, &mapped, dt_seconds, pad);
		}
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

	Aeron_XrApplyPointer(input, &physical);
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

int Aeron_XrGetHand(int hand, AeronXrHand* out) {
	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION, NULL };
	if (!out) {
		return 0;
	}
	memset(out, 0, sizeof *out);
	if (hand < 0 || hand >= AERON_XR_HAND_COUNT || !g_in.ready || !g_in.aim_space[hand] ||
		!g_xr.predicted_display_time || !Aeron_XrSessionRunning()) {
		return 0;
	}
	if (XR_FAILED(g_xr.xrLocateSpace(g_in.aim_space[hand], g_xr.local_space, g_xr.predicted_display_time,
									 &location)) ||
		!(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
		!(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
		return 0;
	}
	out->position[0]    = location.pose.position.x;
	out->position[1]    = location.pose.position.y;
	out->position[2]    = location.pose.position.z;
	out->orientation[0] = location.pose.orientation.x;
	out->orientation[1] = location.pose.orientation.y;
	out->orientation[2] = location.pose.orientation.z;
	out->orientation[3] = location.pose.orientation.w;
	out->trigger        = g_in.hand_trigger[hand];
	out->grip           = g_in.hand_grip[hand];
	out->index_touch    = g_in.hand_index_touch[hand];
	out->thumb_touch    = g_in.hand_thumb_touch[hand];
	out->valid          = 1;
	return 1;
}

int Aeron_XrThumbrestTouch(int hand) {
	if (hand < 0 || hand >= AERON_XR_HAND_COUNT || !g_in.set) {
		return -1;
	}
	return g_in.hand_thumbrest_touch[hand];
}

void Aeron_XrHapticPulse(int hand, float amplitude, int duration_ms) {
	XrHapticVibration  vibration = { XR_TYPE_HAPTIC_VIBRATION, NULL };
	XrHapticActionInfo info      = { XR_TYPE_HAPTIC_ACTION_INFO, NULL };
	if (hand < 0 || hand >= AERON_XR_HAND_COUNT || !g_in.ready || !Aeron_XrSessionRunning() ||
		amplitude <= 0.0f || duration_ms <= 0) {
		return;
	}
	info.action         = g_in.haptic;
	info.subactionPath  = g_in.hand[hand];
	vibration.amplitude = amplitude > 1.0f ? 1.0f : amplitude;
	vibration.duration  = (XrDuration)duration_ms * 1000000;
	vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
	g_xr.xrApplyHapticFeedback(g_xr.session, &info, (const XrHapticBaseHeader*)&vibration);
}

#endif /* AERON_HAS_OPENXR */
