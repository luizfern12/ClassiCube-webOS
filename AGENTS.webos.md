# AGENTS.md — Porting a native project to LG webOS (Smart TV)

Everything an AI agent needs to take an existing C/C++ project (SDL2, GLES,
native binary) and make it run on a webOS LG Smart TV. Distilled from porting
`sm64ex` (see `sm64ex-lgtv`) and from the RetroArch / Moonlight webOS ports.

Copy this file into the target project as `AGENTS.md` when starting a webOS port.

---

## 1. Mental model of webOS

- LG Smart TVs run **webOS** on top of Linux (kernel 4.4.x, **ARMv7** or
  **ARM64**). It's a real Linux system: `/proc`, `/dev`, `/sys`, sshd, etc.
- Native ("native" type) apps are **untrusted processes run under a jailer**:
  `jailer -t native_devmode -i <appId> -p <appDir> <binary>`. The jail
  restricts some device access but still exposes `/dev/input`, most of `/dev`,
  Wayland, and a Luna service bus.
- **Developer mode** must be enabled on the TV (Homebrew channel / devmode
  app). Apps are installed by a webOS packaging toolchain (`ares-cli`), not dpkg.
- The system **SDL2 is old (2.0.5)**. Bundle a modern SDL (webosbrew's
  SDL 2.30.12) with your app instead of relying on the system one.
- `/tmp` is a **RAM fs and is wiped between sessions**. Persist state under the
  app folder instead.

## 2. Toolchain (cross-compilation)

- Use the **buildroot SDK** that RetroArch/webosbrew use:
  `arm-webos-linux-gnueabi_sdk-buildroot` (GCC 14.2.0, glibc 2.12.2 sysroot).
  Host must be Linux **x86_64**. If you extracted the wrong arch build,
  re-download the `x86_64` tarball.
  - Download: `https://github.com/openlgtv/buildroot-nc4/releases/latest/download/arm-webos-linux-gnueabi_sdk-buildroot-x86_64.tar.gz`
  - `tar -xvf ... && ./relocate-sdk.sh`
- `native-toolchain` (webosbrew) is a CMake wrapper that builds/installs this
  same SDK layout — good reference.
- RetroArch's `Makefile.webos` is the canonical example of a port makefile.
  Key flags:
  - `-mcpu=cortex-a9 -mtune=cortex-a53 -mfloat-abi=softfp -mfpu=neon`
  - `RENDER_API=GL WINDOW_API=SDL2 AUDIO_API=SDL2 CONTROLLER_API=SDL2`
  - `-DUSE_GLES` / GLES2 headers
  - Link: `-lSDL2 -lGLESv2 -lm -ldl -lpthread -lrt`
  - rpath: `-Wl,-rpath,\$$ORIGIN/lib` (but see §5 — patchelf is safer)

## 3. Packaging (IPK)

The IPK is a Debian `.ar` archive: `debian-binary`, `control.tar.gz`,
`data.tar.gz`. Use `ares-cli` to build it correctly.

- `appinfo.json` (in the dist root):
  ```json
  {
    "id": "com.yourorg.yourgame",
    "version": "1.0.0",
    "vendor": "you",
    "title": "Your Game",
    "icon": "icon160.png",
    "main": "yourbinary",
    "type": "native",
    "useAllMouseButtons": true,
    "useAllKeyboardKeys": true
  }
  ```
- Toolchain (ares-cli-rs): `ares-package webos/dist -o webos/`,
  `ares-install <app>.ipk`, `ares-launch <appId>`, `ares-launch -c <appId>`
  (close; returns an error if the app isn't running — that's normal).
  Set the target device once: `ares-setup-device` or `export ARES_DEVICE=<name>`.
- Install target on the TV: `/media/developer/apps/usr/palm/applications/<appId>/`.
- Bundle dynamic libs in `lib/` next to the binary and set the rpath (see §5).

## 4. App runtime essentials

- **Launch via `ares-launch`**, not plain SSH. SDL on webOS wants Luna app
  registration (old system SDL aborts with `registerScreenSaverRequest(1)
  failed`; newer webosbrew SDL logs a warning and continues).
- **Wayland/EGL**: set `EGL_PLATFORM=wayland` and `XDG_RUNTIME_DIR=/tmp/xdg`
  (mkdir it). The Wayland socket lives at `/run/wayland-0`; symlink it into
  `$XDG_RUNTIME_DIR/wayland-0` (try `/var/run/wayland-0`,
  `/run/user/0/wayland-0`, `/tmp/wayland-0` as fallbacks).
- **No terminal output** — `freopen` `stdout`/`stderr` into `app.log` in the
  app folder at startup (allow an env override of the filename).
- The jailer **does not hide** `/dev/input`, and direct `/dev/hidraw` open+
  write works (see §6). SDL's hidapi still fails in the jail — see §6.
- Debug loop: `make → ares-package → ares-install → ares-launch`, then
  `ssh root@<tv> "tail -f .../<appId>/app.log"`. Enable SSH key auth on the
  TV (`ssh-copy-id root@<tv>`).

## 5. Bundling a modern SDL2 (critical)

- System SDL is **2.0.5**. It lacks modern gamecontroller mappings, rumble
  APIs, and hidapi improvements.
- Bundle **webosbrew SDL 2.30.12**: download
  `SDL2-2.30.12-webos-abi.tar.gz` from
  `https://github.com/webosbrew/SDL-webOS/releases/tag/release-2.30.12-webos.1`
  and copy `lib/libSDL2-2.0.so.0` into `dist/lib/`.
- Make the loader use the bundled lib: the binary's `DT_RPATH` must point at
  `$ORIGIN/lib`. **Use `patchelf --force-rpath --set-rpath '$ORIGIN/lib'`**:
  - `--force-rpath` writes a real **DT_RPATH**. glibc searches RPATH *before*
    `LD_LIBRARY_PATH`; the jailer may set `LD_LIBRARY_PATH`, which would shadow
    a plain DT_RUNPATH (glibc searches RUNPATH *after* LD_LIBRARY_PATH).
  - Write the patchelf command in the makefile *as a separate shell recipe*
    with a literal `'$$ORIGIN/lib'`; a `-Wl,-rpath,\$$ORIGIN/lib` in LDFLAGS
    gets mangled to `RIGIN/lib`.
- Verify with `readelf -d <binary> | grep -i rpath` (expect `Library rpath:
  [$ORIGIN/lib]`). Note the TV may lack `readelf` — check on the host.

## 6. Input / gamepads

- **Disable SDL hidapi**: `SDL_SetHint("SDL_HINT_JOYSTICK_HIDAPI", "0")`.
  The jailer blocks the `/dev/hidraw` access SDL's hidapi needs, so with it
  enabled SDL enumerates *zero* joysticks; the legacy evdev driver sees every
  pad and (with the bundled SDL's DB) yields full gamecontroller mappings.
- **8BitDo pads in Switch mode** emulate the Switch Pro Controller
  (VID `0x057e`, PID `0x2009`) and sit in a "charging" HID state (yellow LED)
  emitting nothing until a host sends the Switch Pro USB handshake
  (`0x80 0x02`). RetroArch triggers it via hidapi. Since we disable hidapi,
  do it directly: scan `/dev/hidraw*`, match VID/PID with the `HIDIOCGRAWINFO`
  ioctl, `write(fd, "\x80\x02", 2)`. Blue LED = active. Opening/writing
  `/dev/hidraw` **is allowed** in the jail (it's SDL's libusb/udev path that
  fails, not raw open).
- **Hotplug**: webosbrew SDL polls evdev device presence every ~3 s. Don't
  cache a single gamecontroller — re-open any pad not already open, tracked by
  `SDL_JoystickGetDeviceInstanceID(i)` vs
  `SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc))`.
- LG Magic Remote / RCUs show up as keyboard/joystick devices
  (`LGE M-RCU`, `LGE Simple Premium` in `/proc/bus/input/devices`).
- If you need rumble symbols the system SDL might lack, resolve them via
  `dlsym(RTLD_DEFAULT, ...)` (and `#define _GNU_SOURCE` + `<dlfcn.h>`).

## 7. Graphics

- Render via **OpenGL ES 2.0 / EGL** on Wayland. RetroArch's webOS backend is
  the reference. GL may not work from a bare SSH run — launch through
  `ares-launch`.
- Query the TV's native resolution with `SDL_GetCurrentDisplayMode(0)` and
  open fullscreen to match.

## 8. Audio

- The webOS "MFE" audio stack can **crash native apps**
  (`std::bad_function_call`). Provide an env-var escape hatch to skip audio
  init for headless/SSH debugging. SDL2 audio usually works under
  `ares-launch`, but keep the fallback.

## 9. Pitfalls checklist (all hit in practice)

- `relocation error: symbol GLIBC_2.4 not defined in libc.so.6` at load —
  a version-skew between the binary's glibc needs and the TV libc; **not**
  caused by `strip` or `patchelf`. Diagnose on the TV with
  `LD_DEBUG=files,libs,versions ./binary 2>&1 | tail -60`.
- RUNPATH vs RPATH vs LD_LIBRARY_PATH ordering (§5).
- `/tmp` is RAM and wiped; the app dir is persistent.
- `ares-launch -c` errors if the app isn't running — ignore it.
- `readelf` may be missing on the TV; do ELF inspection on the host.
- Old system SDL may lack symbols newer game code references → dlsym or bundle.
- Bundled-lib libc needs should stay within the same version family as the TV
  (webosbrew SDL needs only `GLIBC_2.4/2.7/2.9` and runs fine on TV).

## 10. Reference projects

- **RetroArch webOS**: `Makefile.webos`, `webos/README.md` — SDL bundling,
  jailer launch line, appinfo. The closest thing to a port template.
- **Moonlight webOS** (`moonlight-tv`): `deploy/webos`, `scripts/webos`,
  CMake toolchain.
- **webosbrew/SDL-webOS**: SDL fork with webOS video/audio/input + the
  prebuilt releases.
- **webosbrew/native-toolchain**: CMake SDK wrapper.
- **openlgtv/buildroot-nc4**: the toolchain releases.
- **Homebrew Channel / ares-cli**: install/launch tooling (`ares-cli-rs`).

## 11. Suggested porting order

1. Get the toolchain + ares-cli working; verify with a trivial binary.
2. Cross-compile the project for ARM with SDL2/GLES (host build first if
   possible to isolate port bugs).
3. Add `appinfo.json` + packaging; install and launch on the TV.
4. Wire Wayland/EGL + log redirect (you'll need the log to debug anything).
5. Bundle modern SDL2 + `--force-rpath` (verify which SDL loads via a
   version-print in the app log).
6. Input: `HIDAPI=0`, gamecontroller open/reopen, 8BitDo handshake if needed.
7. Audio fallback flag; native resolution/fullscreen; lifecycle (pause on
   background if the engine needs it).
