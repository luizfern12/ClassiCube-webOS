# How RetroArch handles input on webOS

Reference tree: `/run/media/1TB/Dev-Projects/Porting/RetroArch` (webOS build).
Reference SDL fork: `/run/media/1TB/Dev-Projects/Porting/Tools/SDL-webOS`.

This documents the input stack of the RetroArch webOS build currently running on
`tv-sala`, as a reference for ClassiCube's gamepad handling.

## 1. Overview

The webOS RetroArch build is a Linux build with udev compiled OUT and SDL2 in.

Two input+joypad driver pairs are compiled:

- `linuxraw` (`input/drivers/linuxraw_input.c`,
  `input/drivers_joypad/linuxraw_joypad.c`) — the **default** on Linux.
- `sdl2` (`input/drivers/sdl_input.c`,
  `input/drivers_joypad/sdl_joypad.c`) — what is actually **enabled on tv-sala**.

Live config on the TV
(`com.retroarch.webos/.config/retroarch/retroarch.cfg`):

```
input_driver          = "sdl2"
input_joypad_driver   = "sdl2"
input_axis_threshold      = 0.500000
input_analog_deadzone     = 0.000000
input_analog_sensitivity  = 1.000000
input_max_users           = 8
```

## 2. Device discovery (webOS SDL fork)

`Tools/SDL-webOS/src/joystick/webos/dev_presence.c`:

- webOS pre-creates every `/dev/input/eventN`, `/dev/input/jsN`, `/dev/hidrawN`
  node at boot regardless of hardware.
- `SDL_webOSGetDevicePresenceFlags()` walks those nodes and inspects
  `/sys/dev/char/<major>:<minor>` to decide which ones are backed by real
  hardware, returning a bitmask of present devices.
- Because webOS runs apps in a container, `SDL_DetectSandbox()` returns a value
  other than `SDL_SANDBOX_NONE`, so
  `src/joystick/linux/SDL_sysjoystick.c:1102` switches enumeration to
  `ENUMERATION_POLLING` (a 3-second poll loop,
  `LINUX_FallbackJoystickDetect`) instead of udev/inotify. On non-webOS
  builds the same branch uses `ENUMERATION_FALLBACK`; webOS forces POLLING.

Current TV hardware (per `/proc/bus/input/devices`):

- PS4 pad "Sony Interactive Entertainment Wireless Controller"
  `054c:09cc`, exposed as **both** `/dev/input/event3` (evdev) and `/dev/input/js0`
  (Linux joystick API).
- LG virtual devices report vendor/product `0x9999,0x9999`
  (`MAKE_VIDPID(0x9999,0x9999)` in SDL's joystick code) — "Smart Remote RCU
  Input", "LGE Network Input". These are the TV remote / Magic Remote.

Key point: the same physical pad is available through both the evdev and the
joystick-API paths, so a driver that does NOT use SDL's gamecontroller layer
(linuxraw) works even when SDL's mapping tables are wrong.

## 3. Joypad drivers

### 3.1 linuxraw (default Linux driver)

`input/drivers_joypad/linuxraw_joypad.c`:

- Opens `/dev/input/jsN`, reads `struct js_event` (Linux joystick API).
  Raw axes are int16 symmetric (−32767..+32767), buttons 0/1.
- Device name from `JSIOCGNAME`; **autoconfig matches by exact device name
  string**, never by GUID/VID/PID.
- Digital threshold in `input_driver.c`:
  `(abs(raw_axis) / 0x8000) > input_axis_threshold`.
- Axis direction is controlled purely by the bind file: a bind entry uses
  `+N` / `-N` where N is the raw axis index (e.g. `input_l_y_plus_axis = "+1"`,
  `input_l_y_minus_axis = "-1"`). Inverting an axis = flipping the sign in the
  autoconfig bind. There is no other normalization.

### 3.2 sdl2 (enabled on tv-sala)

`input/drivers_joypad/sdl_joypad.c`:

- Polls `SDL_GameControllerOpen()` when `SDL_IsGameController(i)`.
- Reads state via `SDL_GameControllerGetButton` / `SDL_GameControllerGetAxis`.
- **The axis/button identifiers returned are SDL GameController enum values,
  and the SDL gamecontroller mapping (gamecontrollerdb) decides what physical
  input each enum maps to.** This is the exact same layer ClassiCube uses — so
  the webOS SDL fork's baked-in (wrong) mapping is shared by both apps.

Per-pad identification (for autoconfig):

- `SDL_JoystickGetGUID`; on Linux the GUID decodes to
  `vendor = guid.u16[2]`, `product = guid.u16[4]`.
- webOS GUIDs embed a CRC of the device name, so decoded VID/PID are garbage
  and VID/PID-based autoconfig matching fails.
- Special case in code (webOS patch, `sdl_joypad.c:145`): pads with
  `vendor == 0x9999 && product == 0x9999` are skipped (filters LG virtual
  input devices).

Then `input_autoconfigure_connect(name, NULL, NULL, "sdl2", port, vid, pid)`.

The driver's `ident` string is `"sdl2"`.

## 4. Autoconfig resolution

`tasks/task_autodetect.c`:

1. `input_autoconfigure_connect()` (line 833) records the connected device
   (name, driver, vid/pid) and queues a lookup task.
2. Autoconfig sources, in order:
   - driver-specific directory `<autoconfig_dir>/sdl2/`
   - generic `<autoconfig_dir>/`
   - built-in profiles (`input/input_autodetect_builtin.c`,
     `input_builtin_autoconfs[]`)
3. Match affinity (`input_autoconfigure_get_config_file_affinity`, line 114),
   for up to 9 alternatives `_alt1.._alt9` in one file:
   - **VID+PID equal** → +30 (`input_vendor_id`, `input_product_id`)
   - **exact name** equal → +20 (`input_device`)
   - physical port match +10 / mismatch −10 (`input_phys`)
   - any `affinity > 0` against a **built-in** is accepted outright
4. Built-in for sdl2 = `"Standard Gamepad"` (`SDL2_DEFAULT_BINDS`):
   A=1, B=0, X=3, Y=2, start=6, select=4, dpad up/down/left/right = 11/12/13/14,
   l=9, r=10, l2=+4, r2=+5, l3=7, r3=8; axes
   `l_x_plus=+0, l_x_minus=-0, l_y_plus=+1, l_y_minus=-1, r_x_plus=+2,
   r_x_minus=-2, r_y_plus=-3, r_y_minus=+3`.
   It only matches pads whose name is literally "Standard Gamepad" — real pads
   (e.g. "Sony Interactive Entertainment Wireless Controller") do **not** match.
5. If a matching autoconfig is found its binds are merged with any
   `input_playerN_*` overrides already saved in `retroarch.cfg` (the TV's config
   has `input_playerN_*` at their "nul" defaults, lines 230-416 of the live
   config).

The TV's autoconfig directory is empty, so in practice sdl2 relies on the SDL
gamecontrollerdb (via SDL) plus whatever binds are baked into the config.

## 5. Analog / deadzone / sensitivity pipeline

`input/input_driver.c`:

- `sdl_pad_get_axis` returns the raw SDL axis int16 unchanged.
- Digital (button-like) use: `(abs(raw) / 0x8000) > axis_threshold`.
- Analog stick use (`input_joypad_axis`, line 863), per core-requested stick:
  1. radial `normal_mag = sqrt(x²+y²) / 0x8000`
  2. deadzone (only if `input_analog_deadzone > 0`):
     `val *= (normal_mag - deadzone) / (1 - deadzone)` (radial-scaled)
  3. sensitivity (only if != 1.0):
     `normalized = val / 0x7fff; new_val = 0x7fff * normalized * sens`
     clamped to ±0x7fff.

With the TV config (deadzone 0, sensitivity 1, threshold 0.5) the raw SDL value
passes through untouched: **no deadzone, no scaling**; digital edges at 50%.

## 6. Takeaways for ClassiCube

- ClassiCube uses the SDL_GameController layer (same as RetroArch's sdl2
  driver), so it inherits the webOS SDL fork's baked-in mapping bug. That was
  already worked around in `b2c9a7609` via a corrected `gamecontrollerdb.txt`
  line for the PS4 pad (`leftx:a0,lefty:a1,rightx:a3,righty:a4,
  lefttrigger:a2,righttrigger:a5`).
- RetroArch escapes that entire problem when using `linuxraw`, because it reads
  raw joystick axes and lets autoconfig files name the sign. ClassiCube has no
  such per-axis sign config; axis inversion is done in code
  (`-y` / `PAD_AXIS_SCALE` in its input pipeline).
- webOS SDL GUIDs are name-CRC based → never use them for VID/PID matching.
  Filter LG virtual devices by checking `vendor==0x9999 && product==0x9999`
  (or by name).
- Deadzone/sensitivity in RetroArch are global floats; ClassiCube applies a
  fixed `PAD_AXIS_SCALE` deadzone in code. To mirror RetroArch behavior for
  fixing the "low right-analog sensitivity" issue, either drop the scale on the
  right stick or expose per-axis config.
- The physical pad appears on both `/dev/input/js0` and `/dev/input/event3`;
  ClassiCube's evdev path (used with `SDL_HINT_JOYSTICK_HIDAPI=0`) is the
  correct one for webOS since jailer blocks `/dev/hidraw`.

## 7. ClassiCube webOS gamepad pipeline & fixes

ClassiCube reads pads through the same SDL_GameController layer as RetroArch's
`sdl2` driver. Per-frame (`src/Window_SDL2.c::ProcessJoystick`):

1. `SDL_GameControllerGetAxis` for LEFT/RIGHT X+Y (semantic axes, values are
   the raw evdev axis values — sign not normalized by SDL or the mapping).
2. Linear deadzone: `abs(v) <= 1024` → 0 (≈3% of full scale).
3. Normalize: `x / 32768`, `y = -y / 32768` (stock upstream convention:
   up = +1).
4. webOS hook `WebOS_NormalizeGamepadAxis` adjusts sign/sensitivity
   (`src/webos/Window_WebOS.c`).
5. `Gamepad_SetAxis` stores the values, then scales by
   `delta * 60 * axis_sensiFactor[Gamepad_AxisSensitivity[axis]]` (default
   factor 0.5) before the camera/movement consumers see them.

### Hardcoded fixes (verified by ear, not yet on-device confirmed)

- **Stick Y inverted**: on the webOS SDL fork both sticks' Y axes are reported
  with opposite polarity to desktop (up = positive). The stock `-y`
  therefore maps "push up" → "walk/aim backwards". Fix: flip the sign for
  both `PAD_AXIS_LEFT` and `PAD_AXIS_RIGHT`
  (`WEBOS_LEFT_Y_FLIP`/`WEBOS_RIGHT_Y_FLIP = -1.0`).
- **Right stick look too slow**: `Camera.c::PerspectiveCamera_GetMouseDelta`
  multiplies accumulated stick deltas by `0.0002/3 * RAD2DEG * Sensitivity`;
  a stick value maxing out at 1.0 (then × 0.5 axis factor) yields only
  ~5 deg/s at full deflection — far below mouse deltas. Fix: boost the right
  stick X/Y by `WEBOS_RIGHT_*_SCALE = 10.0`. The in-game Sensitivity option
  still scales on top.
- **Shoulder/trigger swap**: `ProcessGamepadButtons` in
  `src/Window_SDL2.c` re-routes (webOS only) the physical L1/R1 bumpers to
  `CCPAD_ZL/ZR` (hotbar) and the L2/R2 triggers to `CCPAD_L/R`
  (delete/place), matching the original pad mapping.

### TODO

- Expose the per-stick Y sign and sensitivity as Options entries instead of
  hardcoded `#define`s (the values live at the top of
  `WebOS_NormalizeGamepadAxis`).

