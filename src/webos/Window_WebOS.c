#include "../Core.h"
#ifdef CC_BUILD_WEBOS
#include <SDL2/SDL.h>
#include <SDL2/SDL_webOS.h>
#include "../Window.h"
#include "../Input.h"
#include "../webos/webos.h"

/* SDL window/input quirks for webOS. Runs between WebOS_Bootstrap (sets up the
   app dir, cwd, env) and the game's own Window_PreInit/Window_Init. */

/* gamepad slots owned by the shared SDL2 window backend */
extern SDL_GameController* controllers[INPUT_MAX_GAMEPADS];

void WebOS_PreSDLInit(void) {
	SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_BACK, "true");
	SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_EXIT, "true");
	SDL_SetHint(SDL_HINT_WEBOS_CURSOR_SLEEP_TIME, "5000");
	/* The webOS jailer blocks /dev/hidraw*, so SDL's hidapi joystick driver
	enumerates nothing by default. Force the legacy (evdev) driver instead,
	which works in the app sandbox and still yields gamecontroller mappings. */
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
}

/* TV apps run fullscreen at the panel's native resolution */
void WebOS_ApplyWindowSize(int* width, int* height, int* flags) {
	*width  = DisplayInfo.Width;
	*height = DisplayInfo.Height;
	*flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
}

/* Extra mappings for pads not in the bundled SDL's built-in database.
   Runs after the cwd has been switched to the app dir by WebOS_InitDataDir. */
void WebOS_LoadGamepadMappings(void) {
	SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt");
}

/* Closes detached pads and opens any pad not already open (tracked by SDL
   joystick instance ID). Picks up controllers hotplugged after launch. */
void WebOS_ReopenGamepads(void) {
	int i, slot;

	for (slot = 0; slot < INPUT_MAX_GAMEPADS; slot++) {
		if (controllers[slot] && !SDL_GameControllerGetAttached(controllers[slot])) {
			SDL_GameControllerClose(controllers[slot]);
			controllers[slot] = NULL;
		}
	}

	for (i = 0; i < SDL_NumJoysticks(); i++) {
		SDL_JoystickID instance;
		cc_bool alreadyOpen;
		if (!SDL_IsGameController(i)) continue;

		instance = SDL_JoystickGetDeviceInstanceID(i);
		alreadyOpen = false;
		for (slot = 0; slot < INPUT_MAX_GAMEPADS; slot++) {
			if (controllers[slot] &&
				SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controllers[slot])) == instance) {
				alreadyOpen = true; break;
			}
		}
		if (alreadyOpen) continue;

		for (slot = 0; slot < INPUT_MAX_GAMEPADS; slot++) {
			if (controllers[slot]) continue;
			controllers[slot] = SDL_GameControllerOpen(i);
			if (controllers[slot]) break;
		}
	}
}

/* SDL does NOT normalize axis polarity: SDL_CONTROLLER_AXIS_* carries the raw
   evdev value, sign and all, and gamecontrollerdb entries only pick *which*
   raw axis feeds each semantic axis. On the webOS SDL fork (with the evdev
   joystick driver forced via SDL_HINT_JOYSTICK_HIDAPI=0) both sticks' Y axes
   come out with opposite polarity to desktop SDL (up = positive), so the
   stock -y / PAD_AXIS_SCALE in ProcessJoystick maps "push up" to "walk/aim
   backwards". Flip the signs back.

   The right stick drives the camera. Its look pipeline (Camera.c) is tuned for
   mouse pixel deltas, so a stick value that maxes out at 1.0 turns the view
   far too slowly (~5 deg/s at full deflection) — the sensitivity boost below
   puts it in the same ballpark as mouse movement. The in-game Sensitivity
   option still scales it on top.

   TODO(settings): expose these per-stick sign/sensitivity values as Options so
   they can be tuned from the menu instead of requiring a rebuild. */
#define WEBOS_LEFT_Y_FLIP   (-1.0f)
#define WEBOS_LEFT_X_SCALE  ( 1.0f)
#define WEBOS_RIGHT_Y_FLIP  (-1.0f)
#define WEBOS_RIGHT_X_SCALE (10.0f)
#define WEBOS_RIGHT_Y_SCALE (10.0f)

void WebOS_NormalizeGamepadAxis(int axis, float* ax, float* ay) {
	if (axis == PAD_AXIS_LEFT) {
		*ax *= WEBOS_LEFT_X_SCALE;
		*ay *= WEBOS_LEFT_Y_FLIP;
	} else {
		*ax *= WEBOS_RIGHT_X_SCALE;
		*ay *= WEBOS_RIGHT_Y_FLIP * WEBOS_RIGHT_Y_SCALE;
	}
}
#endif
