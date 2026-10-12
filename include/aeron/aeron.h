#ifndef AERON_H
#define AERON_H

#include <stddef.h>
#include <stdint.h>

#include "aeron/audio.h"
#include "aeron/config_file.h"
#include "aeron/debug.h"
#include "aeron/dialog.h"
#include "aeron/input.h"
#include "aeron/log.h"
#include "aeron/numeric.h"
#include "aeron/paths.h"
#include "aeron/render.h"
#include "aeron/surface.h"
#include "aeron/sync.h"
#include "aeron/temporal.h"
#include "aeron/time.h"
#include "aeron/vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Controls how Aeron maps the application's logical render area to the window. */
typedef enum AeronPresentationMode {
	AERON_PRESENTATION_STRETCH    = 0,
	AERON_PRESENTATION_ASPECT_FIT = 1
} AeronPresentationMode;

/* Startup configuration for the Aeron runtime. */
typedef struct AeronConfig {
	const char* org_name;
	const char* app_name;
	const char* asset_root;
	/* Optional explicit resource root. When unset, resource_path is resolved
	 * relative to the application directory. */
	const char* resource_root;
	const char* resource_path;
	/* Compiled-shader directory, relative to the application executable. */
	const char* shader_path;
	const char* window_title;
	/* Optional window/taskbar icon as an in-memory BMP passed to
	 * SDL_SetWindowIcon on Windows and Linux. Not applied on macOS, where
	 * the bundle icon is authoritative and SDL would instead replace the
	 * Dock icon with this lower-resolution image. */
	const void* window_icon_bmp;
	size_t      window_icon_bmp_size;
	/* Window size in logical points. <= 0 auto-sizes to ~90% of the
	 * primary display's usable area, locked to the logical aspect. */
	int                   window_width;
	int                   window_height;
	int                   logical_width;
	int                   logical_height;
	AeronPresentationMode presentation_mode;
	/* Optional swapchain background shown outside submitted layers. When
	 * disabled, Aeron's default background is retained. */
	int   clear_color_enabled;
	float clear_color_rgba[4];
	/* Request OpenXR presentation (also enabled by AERON_XR=1). Falls back to
	 * the desktop window when no runtime/headset is available. */
	int xr_enabled;
} AeronConfig;

/* Initializes SDL, the Aeron window, render backend, text input, and VFS. */
int Aeron_Init(const AeronConfig* config);
/* Nonzero while an OpenXR session is presenting frames. */
int Aeron_XrIsPresenting(void);
/* Nonzero while an OpenXR session exists but does not have input focus:
 * headset taken off, system menu or guardian open. Games should pause. */
int Aeron_XrIsUnfocused(void);
/* Menus: the right VR controller points at the virtual screen as a mouse
 * (trigger = left click, grip = right click). Disable during flight so the
 * trigger fires instead. Enabled by default. */
void Aeron_XrSetPointerEnabled(int enabled);
/* Menus that are driven by the keyboard only (e.g. a game's in-cockpit
 * launch menu): while enabled, either thumbstick sends the arrow keys (with
 * key repeat), A or the right trigger sends Enter, X sends Space and B sends
 * Escape, and the virtual gamepad reports neutral sticks and no buttons.
 * Disabled by default. */
void Aeron_XrSetMenuKeys(int enabled);

/* Controller buttons a game can bind to its own actions in flight. */
typedef enum AeronXrButton {
	AERON_XR_BUTTON_A = 0,
	AERON_XR_BUTTON_B,
	AERON_XR_BUTTON_X,
	AERON_XR_BUTTON_Y,
	AERON_XR_BUTTON_LEFT_TRIGGER,
	AERON_XR_BUTTON_LEFT_STICK,  /* left thumbstick click */
	AERON_XR_BUTTON_RIGHT_STICK, /* right thumbstick click */
	AERON_XR_BUTTON_RIGHT_GRIP,
	AERON_XR_BUTTON_FLICK_UP, /* right thumbstick flicks, only while shifted */
	AERON_XR_BUTTON_FLICK_DOWN,
	AERON_XR_BUTTON_FLICK_LEFT,
	AERON_XR_BUTTON_FLICK_RIGHT,
	AERON_XR_BUTTON_COUNT
} AeronXrButton;

/* Flight buttons: while enabled, presses of the buttons above are queued
 * for Aeron_XrPollButton instead of being reported on the virtual gamepad
 * (the right trigger, right stick, left stick and throttle lever stay on
 * the gamepad, and so does the right grip unless shifted). Holding the left
 * grip is the shift layer: presses made while it is held report
 * `shifted`, and the right thumbstick turns into four flick buttons instead
 * of steering. Disabled by default. */
void Aeron_XrSetFlightButtons(int enabled);
/* Pops the oldest queued flight-button press. Returns 0 when none is
 * queued. */
int Aeron_XrPollButton(AeronXrButton* button, int* shifted);
/* A tracked VR controller for touch panels the game draws itself. */
typedef struct AeronXrHand {
	int   valid;          /* pose tracked this frame */
	float position[3];    /* aim pose in OpenXR LOCAL space, metres */
	float orientation[4]; /* quaternion x, y, z, w; the aim ray is -Z */
	float trigger;        /* 0..1 */
	float grip;           /* 0..1 */
	/* Capacitive touch where the controller has it: 1 touching, 0 not,
	 * -1 unknown. index_touch = finger on the trigger, thumb_touch = thumb
	 * on a button, the stick or the thumb rest. */
	int index_touch;
	int thumb_touch;
} AeronXrHand;
/* Fills `out` for hand 0 (left) or 1 (right) at this frame's predicted
 * display time. Returns out->valid. */
int Aeron_XrGetHand(int hand, AeronXrHand* out);
/* While captured, the hand's trigger is not reported to the game (no
 * gamepad button, no flight-button press): a game UI owns it. A pull that
 * started while captured stays hidden until it is released. */
void Aeron_XrCaptureTrigger(int hand, int captured);
/* Short vibration on one controller (amplitude 0..1). */
void Aeron_XrHapticPulse(int hand, float amplitude, int duration_ms);
/* Nonzero when frames are also shown in a desktop window (not headless). */
int Aeron_HasDesktopMirror(void);
/* Shuts down Aeron and releases all runtime-owned resources. */
void Aeron_Shutdown(void);

/* Starts a new frame, pumps platform events, updates input, and returns capped elapsed microseconds. */
int32_t Aeron_BeginFrame(void);
/* Services queued platform events without starting a frame. Intended for
 * synchronous startup or other work performed outside the normal frame loop.
 * Persistent input state and lifecycle requests are updated immediately;
 * transient input edges are cleared by the next Aeron_BeginFrame. */
void Aeron_PumpEvents(void);
/* Presents all render submissions queued for the current frame. Returns zero
 * after an unexpected renderer failure. A successful no-op frame returns one. */
int Aeron_Present(void);

/* Requests that the application exit at the next convenient point. */
void Aeron_RequestQuit(void);
/* Returns nonzero after the user or application has requested exit. */
int Aeron_QuitRequested(void);
/* Reports one fatal application error, requests orderly shutdown, and latches a
 * nonzero process result. Later reports do not replace the first diagnostic. */
void Aeron_RequestFatalError(const char* title, const char* message);
int  Aeron_FatalErrorRequested(void);

/* Changes the application logical render size without touching the host window. */
int Aeron_SetLogicalSize(int width, int height);
/* Returns the application logical render size configured at init or by
 * Aeron_SetLogicalSize. Returns nonzero when a logical size is set. */
int Aeron_GetLogicalSize(int* width, int* height);
/* Returns the current window client size in logical points. */
int Aeron_GetWindowSize(int* width, int* height);
/* Constrains ordinary window resizing to one width:height ratio. Fixed-size
 * states such as fullscreen defer the constraint until they are left. */
int Aeron_SetWindowAspectRatio(int aspect_width, int aspect_height);
/* Adjusts an ordinary window to the requested aspect while keeping it inside
 * the current display's usable bounds. Fixed-size states are left unchanged. */
int Aeron_ResizeWindowToAspect(int aspect_width, int aspect_height);
/* Switches the host window between windowed and fullscreen modes. */
int Aeron_SetFullscreen(int fullscreen);
/* Returns nonzero while the host window is fullscreen. */
int Aeron_Fullscreen(void);
/* Asks the OS to raise the host window and give it input focus, e.g. after a
 * native dialog held focus. The request is asynchronous and the platform may
 * deny it under its focus-stealing policy. */
void Aeron_RaiseWindow(void);

/* Physical pixel dimensions of the aspect-fitted presentation area for the
 * current host frame. This is distinct from logical coordinates and window
 * points, particularly on high-density displays. */
int Aeron_GetPresentationPixelSize(int* width, int* height);

/* Returns the current immutable input snapshot for this frame. */
const AeronInputSnapshot* Aeron_InputSnapshot(void);
/* Clears this frame's mouse button edges (press, release, double click)
 * and wheel in the snapshot, for an overlay that consumed them; later
 * readers see no click. The held-button state is left alone. */
void Aeron_InputSuppressMouseButtons(void);
/* Returns the runtime VFS used by the application. */
AeronVfs* Aeron_GetVfs(void);
/* Shows or hides the host OS cursor; returns nonzero on success. */
int Aeron_SetHostCursorVisible(int visible);
/* Warps the host mouse to an application logical coordinate. */
int Aeron_WarpMouseLogical(int x, int y);
/* Enables or disables relative mouse mode for flight-style delta input. */
int Aeron_SetRelativeMouseMode(int enabled);
/* Nonzero while relative mouse mode is active, i.e. while the pointer is
 * captured and its deltas belong to the application rather than the OS. */
int Aeron_RelativeMouseMode(void);

#ifdef __cplusplus
}
#endif

#endif
