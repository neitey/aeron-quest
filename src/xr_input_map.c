#include "xr_input_map.h"

#include <math.h>
#include <string.h>

void AeronXrTouchMapper_Init(AeronXrTouchMapper* mapper) {
	if (!mapper) {
		return;
	}
	memset(mapper, 0, sizeof *mapper);
	mapper->throttle          = 0.0f;
	mapper->throttle_rate     = 0.8f;
	mapper->stick_deadzone    = 0.15f;
	mapper->digital_threshold = 0.5f;
}

static float AeronXrClamp(float value, float lo, float hi) {
	return value < lo ? lo : (value > hi ? hi : value);
}

static int16_t AeronXrStickAxis(float value) {
	return (int16_t)lrintf(AeronXrClamp(value, -1.0f, 1.0f) * 32767.0f);
}

static int16_t AeronXrTriggerAxis(float value) {
	return (int16_t)lrintf(AeronXrClamp(value, 0.0f, 1.0f) * 32767.0f);
}

void AeronXrTouchMapper_Map(AeronXrTouchMapper* mapper, const AeronXrTouchState* touch, float dt_seconds,
							AeronControllerSnapshot* out) {
	uint32_t buttons = 0;
	float    lever_input;

	if (!mapper || !touch || !out) {
		return;
	}
	if (dt_seconds < 0.0f) {
		dt_seconds = 0.0f;
	} else if (dt_seconds > 0.1f) {
		dt_seconds = 0.1f; /* a hitch must not slam the throttle */
	}

	/* Throttle lever: integrate the left stick's forward/back deflection
	 * outside the deadzone, rescaled so the lever starts moving smoothly. */
	lever_input = touch->active_left ? touch->left_stick[1] : 0.0f;
	if (fabsf(lever_input) <= mapper->stick_deadzone) {
		lever_input = 0.0f;
	} else {
		const float sign = lever_input > 0.0f ? 1.0f : -1.0f;
		lever_input = sign * (fabsf(lever_input) - mapper->stick_deadzone) / (1.0f - mapper->stick_deadzone);
	}
	mapper->throttle =
		AeronXrClamp(mapper->throttle + lever_input * mapper->throttle_rate * dt_seconds, 0.0f, 1.0f);

	out->connected = 1;
	out->kind      = AERON_CONTROLLER_KIND_GAMEPAD;
	/* Gamepad stick Y is +down (SDL convention); Touch +y is forward/up,
	 * i.e. stick pushed away = nose down, matching a real flight stick
	 * once the default profile's pitch inversion is applied. */
	out->gamepad_axes[AERON_GAMEPAD_AXIS_LEFTX] =
		touch->active_right ? AeronXrStickAxis(touch->right_stick[0]) : 0;
	out->gamepad_axes[AERON_GAMEPAD_AXIS_LEFTY] =
		touch->active_right ? AeronXrStickAxis(-touch->right_stick[1]) : 0;
	out->gamepad_axes[AERON_GAMEPAD_AXIS_RIGHTX] =
		touch->active_left ? AeronXrStickAxis(touch->left_stick[0]) : 0;
	out->gamepad_axes[AERON_GAMEPAD_AXIS_RIGHTY] =
		touch->active_left ? AeronXrStickAxis(-touch->left_stick[1]) : 0;
	out->gamepad_axes[AERON_GAMEPAD_AXIS_LEFT_TRIGGER] =
		touch->active_left ? AeronXrTriggerAxis(touch->left_trigger) : 0;
	out->gamepad_axes[AERON_GAMEPAD_AXIS_RIGHT_TRIGGER] = AeronXrTriggerAxis(mapper->throttle);

	if (touch->active_right && touch->right_trigger >= mapper->digital_threshold) {
		buttons |= 1u << AERON_GAMEPAD_BUTTON_SOUTH;
	}
	if (touch->active_right && touch->right_grip >= mapper->digital_threshold) {
		buttons |= 1u << AERON_GAMEPAD_BUTTON_EAST;
	}
	if (touch->a) {
		buttons |= 1u << AERON_GAMEPAD_BUTTON_WEST;
	}
	if (touch->b) {
		buttons |= 1u << AERON_GAMEPAD_BUTTON_NORTH;
	}
	if (touch->x) {
		buttons |= 1u << AERON_GAMEPAD_BUTTON_LEFT_SHOULDER;
	}
	if (touch->y) {
		buttons |= 1u << AERON_GAMEPAD_BUTTON_RIGHT_SHOULDER;
	}
	if (touch->active_left && touch->left_grip >= mapper->digital_threshold) {
		buttons |= 1u << AERON_GAMEPAD_BUTTON_BACK;
	}
	if (touch->left_stick_click) {
		buttons |= 1u << AERON_GAMEPAD_BUTTON_START;
	}
	if (touch->right_stick_click) {
		buttons |= 1u << AERON_GAMEPAD_BUTTON_RIGHT_STICK;
	}
	if (touch->menu) {
		buttons |= 1u << AERON_GAMEPAD_BUTTON_GUIDE;
	}

	out->gamepad_available_axes = (1u << AERON_GAMEPAD_AXIS_COUNT) - 1u;
	out->gamepad_available_buttons =
		(1u << AERON_GAMEPAD_BUTTON_SOUTH) | (1u << AERON_GAMEPAD_BUTTON_EAST) |
		(1u << AERON_GAMEPAD_BUTTON_WEST) | (1u << AERON_GAMEPAD_BUTTON_NORTH) |
		(1u << AERON_GAMEPAD_BUTTON_BACK) | (1u << AERON_GAMEPAD_BUTTON_GUIDE) |
		(1u << AERON_GAMEPAD_BUTTON_START) | (1u << AERON_GAMEPAD_BUTTON_RIGHT_STICK) |
		(1u << AERON_GAMEPAD_BUTTON_LEFT_SHOULDER) | (1u << AERON_GAMEPAD_BUTTON_RIGHT_SHOULDER);
	out->gamepad_buttons          = buttons;
	out->gamepad_pressed_buttons  = buttons & ~mapper->previous_buttons;
	out->gamepad_released_buttons = mapper->previous_buttons & ~buttons;
	mapper->previous_buttons      = buttons;
}
