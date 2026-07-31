# ClassiCube webOS Port Notes

## Build / toolchain
- Cross toolchain: `Tools/native-toolchain-compiled/arm-webos-linux-gnueabi_sdk-buildroot`
  (prefix `arm-webos-linux-gnueabi-`, sysroot has SDL2 2.30.12 headers incl. `SDL_webOS.h`).
- Bundled SDL: `Tools/precompiled-SDL/lib/libSDL2-2.0.so.0` (webosbrew SDL 2.30.12, ARM EABI5,
  needs glibc up to 2.9; ClassiCube itself only needs GLIBC_2.4).
- Build: `make webos` (uses `misc/makefiles/webos.mk`, `-DCC_BUILD_WEBOS`, neon softfp).
- Package/install/launch: `Makefile.webos` + `install.sh`; ares CLI in
  `Tools/ares-cli-rs-v0.2.0-linux-x86_64` (`ARES_DEVICE` default `tv-sala`).
- Binary needs RPATH `$ORIGIN/lib` (patchelf) so the bundled SDL is found.

## Devices
- `tv-sala` (default): root@192.168.1.133:22 (plain root SSH rejected; ares works).
- `tv`: root@192.168.1.205:22.
- App installs to `/media/developer/apps/usr/palm/applications/com.classicube.webos/`
  (over SSH). Inside the app's jail, paths are `/var/palm/jail/com.classicube.webos/...`.
- Logs: `<appdir>/client.log` (webOS bootstrap redirects stdout/stderr there BEFORE any
  SDL/EGL init; `CC_LOG_FILE` env overrides name). Debug probe: touch `cc_probe_joy`
  in app dir to dump input devices + SDL joysticks then exit.

## webOS runtime facts (learned the hard way)
1. **The webOS app manager passes the appInfo JSON as a command-line argument:**
   `ClassiCube {"@system_native_app":true,"nid":"com.classicube.webos"}`.
   ClassiCube's `ProcessProgramArgs` treats a single non-flag arg as a *username* and
   runs the GAME directly (ARG_RESULT_RUN_GAME), skipping the launcher entirely.
   Consequences: no CheckResources/FetchResources UI, no default.zip download, and the
   game logs skin fetches for the JSON username and can render black.
   FIX: in `main()` force `argc = 1` on webOS so the launcher always runs.
2. App starts with cwd == $HOME; `Platform_SetDefaultCurrentDirectory` then chdirs to the
   exe dir (the app folder). All game-created files land in the app folder.
3. `stdout/stderr` are block-buffered when redirected to a file, so webOS bootstrap
   messages (handshake, "logging to...") can appear late/out-of-order vs the game's own
   Logger output (Logger uses its own fd). Keep `setvbuf(_IOLBF)` after freopen.
4. Networking works in the app sandbox (skin fetch got a real HTTP 403 from
   cdn.classicube.net; default.zip from static.classicube.net returns HTTP 200 on desktop).
5. SDL `SDL_HINT_JOYSTICK_HIDAPI=0`: jailer blocks /dev/hidraw for hidapi, legacy evdev
   works. 8BitDo Switch Pro (057e:2009) needs the USB handshake `{0x80,0x02}` first
   (`WebOS_SwitchProHandshake`).
6. Audio is NULL backend (webOS MFE crashes with std::bad_function_call otherwise, same
   as sm64 `SM64_NOSOUND`).

## Data directory (.config) - psvita-style
- ClassiCube's psvita port relocates all game data by prepending a fixed root path in
  `Platform_EncodePath` (`ux0:data/ClassiCube/`). RetroArch uses `$HOME/.config/retroarch/`.
- webOS applies the same: `Platform_EncodePath` prepends `<exedir>/.config/` (computed from
  `/proc/self/exe`, mkdir'd at bootstrap). All relative data (options.txt, texpacks/,
  texturecache/, audio/, maps/, plugins/, client.log, session) then lives in
  `<appdir>/.config/`. Bootstrap logfile + `cc_probe_joy` marker resolved into that dir too.

## Key code locations
- `src/Platform_Posix.c`: `WebOS_Bootstrap()`, `WebOS_SwitchProHandshake()`,
  `WebOS_ProbeJoysticks()`, `WebOS_DumpJoysticks()`, `Platform_EncodePath()` (data dir
  prefix), `main()` (argc=1 fix).
- `src/Window_SDL2.c`: webOS hints, fullscreen native res, `Gamepads_ReopenPads()` hotplug.
- `src/Core.h`: `CC_BUILD_WEBOS` platform section (net=BUILTIN, ssl=BEARSSL,
  crt=OPENSSL, aud=NULL, win=SDL2, GLES, gfx=GL2).
- `src/Launcher.c` + `src/LScreens.c` + `src/Resources.c`: launcher resource flow -
  `Resources_CheckExistence()` (checks `texpacks/classicube.zip`), `CheckResourcesScreen`
  ("download required resources?"), `FetchResourcesScreen` (calls `Fetcher_Run()` which
  GETs `http://static.classicube.net/default.zip` -> `texpacks/classicube.zip`).
- `misc/makefiles/webos.mk`, `Makefile.webos`, `install.sh`, `webos/appinfo.json`.

## Reference ports
- `../sm64ex-lgtv`: exact webOS porting commits by luizfern12 (bundled SDL2, patchelf
  RPATH, ares packaging, TARGET_WEBOS).
- `../RetroArch`: downloads work on webOS; data in `$HOME/.config/retroarch/` (see
  `file_path_special.c`).
- ClassiCube `src/psvita/Platform_PSVita.c`: `Platform_EncodePath` root-path prefix pattern.

## Gamepad / SDL controller mappings (confirmed on tv-sala)
- The webOS SDL reports device GUIDs that embed the device-name CRC (`03008fe5...` PS4,
  `03007755...` Switch Pro) instead of the stock SDL GUID scheme, so standard
  gamecontrollerdb.txt GUID entries never match; entries for these devices MUST use the
  webOS GUID.
- Baked-in webOS mappings are **wrong for the PS4** (off-by-one on axes): it maps
  `lefttrigger:a4, rightx:a2, righty:a3`. `/proc/bus/input/devices` shows the PS4 exposes
  `ABS=X,Y,Z,RX,RY,RZ` (L2=raw2, right-stick X=raw3, right-stick Y=raw4, R2=raw5), so the
  baked-in map points the right stick at L2/right-stick-X. Raw axes 4/5 also rest at
  -32768 with no input, so triggers (mapped to 3/4 by the baked-in map) never fire and the
  camera drifts/spins. Root cause of "swapped analogs / drift / slow sensitivity".
- `webos/gamecontrollerdb.txt` carries corrected mappings and **does override** the baked-in
  maps (confirmed: after `SDL_GameControllerAddMappingsFromFile` the reported mapping for
  both controllers matches the file entries). The call returns `0` even when it applies, so
  don't rely on the return value.
- PS4 corrected map: `leftx:a0,lefty:a1,rightx:a3,righty:a4,lefttrigger:a2,righttrigger:a5`,
  buttons `a:b0,b:b1,x:b2,y:b3`, dpad `b11-14`, `l1:b9,r1:b10,l3:b7,r3:b8,start:b6,back:b4`,
  `guide:b5,touchpad:b15`. Switch Pro map (from sm64ex): dpad via hat `h0.1/2/4/8`.
- `WebOS_InitDataDir` now chdir()s to the app dir so `Gamepads_Init`'s relative
  `gamecontrollerdb.txt` load (`src/Window_SDL2.c`) always finds the file regardless of the
  webOS launch cwd.
- Debug: touch `cc_probe_joy` in the app dir. Probe now: dumps devices/HIDAPI=0 mapping,
  loads the db file, re-dumps ("after file load") to prove override, then `WebOS_ProbeLiveRead`
  waits up to 120s for a button press and records 30s of raw+mapped state.
