#ifndef AERON_XR_INPUT_MAP_H
#define AERON_XR_INPUT_MAP_H

/*
 * Pure mapping from Meta Quest Touch controller state to Aeron's standardized
 * gamepad snapshot. Kept free of OpenXR and SDL so it can be unit tested.
 *
 * The physical-to-gamepad assignment is chosen so OpenXWA's DEFAULT gamepad
 * profile (yaw/pitch = left stick, roll = right stick X, throttle = right
 * trigger, fire = south, target-in-sights = east, ...) gives natural VR
 * controls without any rebinding:
 *
 *   Touch                         -> virtual gamepad         -> default XWA use
 *   right thumbstick X/Y          -> LEFTX / LEFTY           -> yaw / pitch
 *   left thumbstick X             -> RIGHTX                  -> roll
 *   left thumbstick Y (integrated)-> RIGHT_TRIGGER           -> throttle lever
 *   (the left stick is split into a wide roll zone and a narrow throttle
 *   zone, see throttle_zone_deg)
 *   left thumbstick Y (raw)       -> RIGHTY                  -> (unbound)
 *   left trigger                  -> LEFT_TRIGGER            -> (unbound)
 *   right trigger                 -> SOUTH                   -> fire
 *   right grip                    -> EAST                    -> target in sights
 *   (in flight the game takes the face buttons, the left trigger and grip,
 *   the stick clicks and the right grip as keys of its own instead:
 *   Aeron_XrSetFlightButtons)
 *   A / B                         -> WEST / NORTH            -> button 3 / 4
 *   X / Y                         -> LEFT / RIGHT SHOULDER   -> button 5 / 6
 *   left grip                     -> BACK                    -> button 7
 *   left thumbstick click         -> START                   -> button 8
 *   right thumbstick click        -> RIGHT_STICK             -> (unbound)
 *   left menu button              -> GUIDE (consumed as Esc by xr.c)
 *
 * Left-handed controls (AERON_XR_LEFT_HANDED=1) swap the two controllers
 * before this mapping (xr_input.c), so the table reads mirrored. Swapped
 * thumbsticks (Aeron_XrSetSwapSticks) then trade only the two sticks'
 * axes (AeronXrTouchSwapSticks): the thumbstick X/Y rows above come from
 * the other stick, every button row stays.
 *
 * The throttle is a "virtual lever": pushing the left stick forward/back
 * moves it and it stays where it was left, like a HOTAS throttle.
 */

#include "aeron/input.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronXrTouchState {
	int   active_left;
	int   active_right;
	float left_stick[2];  /* x right, y forward/up, -1..1 */
	float right_stick[2]; /* x right, y forward/up, -1..1 */
	float left_trigger;   /* 0..1 */
	float right_trigger;  /* 0..1 */
	float left_grip;      /* 0..1 */
	float right_grip;     /* 0..1 */
	int   a, b, x, y;
	int   left_stick_click;
	int   right_stick_click;
	int   menu;
	float extra_roll;     /* roll from elsewhere (virtual stick twist), -1..1 */
} AeronXrTouchState;

/* Persistent mapper state (throttle lever position, previous buttons). */
typedef struct AeronXrTouchMapper {
	float    throttle;          /* 0..1, persists across frames */
	float    throttle_rate;     /* full travel per second at full stick */
	float    stick_deadzone;    /* applied before the lever integrates */
	float    digital_threshold; /* trigger/grip press threshold */
	/* Left stick zones: within this angle of straight forward/back the
	 * stick moves the throttle only; everywhere else it rolls only, so a
	 * sloppy sideways push never changes speed (default 30 degrees: roll
	 * gets 2 x 120 degrees, throttle 2 x 60). */
	float    throttle_zone_deg;
	uint32_t previous_buttons;
} AeronXrTouchMapper;

void AeronXrTouchMapper_Init(AeronXrTouchMapper* mapper);

/* Swapped thumbsticks: the two sticks' axes trade places; the stick clicks
 * and everything else stay. (An idle controller reports a centred stick, so
 * the per-hand `active` flags are left alone.) */
void AeronXrTouchSwapSticks(AeronXrTouchState* touch);

/* Fills `out` as a connected standardized gamepad. `dt_seconds` drives the
 * throttle lever. Identity fields (name, guid, instance_id) are set by the
 * caller. */
void AeronXrTouchMapper_Map(AeronXrTouchMapper* mapper, const AeronXrTouchState* touch, float dt_seconds,
							AeronControllerSnapshot* out);

#ifdef __cplusplus
}
#endif

#endif
