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
 *   left thumbstick Y (raw)       -> RIGHTY                  -> (unbound)
 *   left trigger                  -> LEFT_TRIGGER            -> (unbound)
 *   right trigger                 -> SOUTH                   -> fire
 *   right grip                    -> EAST                    -> target in sights
 *   A / B                         -> WEST / NORTH            -> button 3 / 4
 *   X / Y                         -> LEFT / RIGHT SHOULDER   -> button 5 / 6
 *   left grip                     -> BACK                    -> button 7
 *   left thumbstick click         -> START                   -> button 8
 *   right thumbstick click        -> RIGHT_STICK             -> (unbound)
 *   left menu button              -> GUIDE (consumed as Esc by xr.c)
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
} AeronXrTouchState;

/* Persistent mapper state (throttle lever position, previous buttons). */
typedef struct AeronXrTouchMapper {
	float    throttle;          /* 0..1, persists across frames */
	float    throttle_rate;     /* full travel per second at full stick */
	float    stick_deadzone;    /* applied before the lever integrates */
	float    digital_threshold; /* trigger/grip press threshold */
	uint32_t previous_buttons;
} AeronXrTouchMapper;

void AeronXrTouchMapper_Init(AeronXrTouchMapper* mapper);

/* Fills `out` as a connected standardized gamepad. `dt_seconds` drives the
 * throttle lever. Identity fields (name, guid, instance_id) are set by the
 * caller. */
void AeronXrTouchMapper_Map(AeronXrTouchMapper* mapper, const AeronXrTouchState* touch, float dt_seconds,
							AeronControllerSnapshot* out);

#ifdef __cplusplus
}
#endif

#endif
