#include "../Core.h"
#ifdef CC_BUILD_WEBOS
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <SDL2/SDL.h>
#include "../Platform.h"
#include "../Constants.h"
#include "../Funcs.h"
#include "../String_.h"
#include "../Errors.h"
#include "../ExtMath.h"
#include "../webos/webos.h"

/* webOS bootstrap. The jailer blocks /dev/hidraw for SDL's hidapi driver, so the
   joystick layer falls back to evdev; 8BitDo controllers in Switch mode sit in a
   "charging" HID state over USB until the Switch Pro handshake is sent. */

struct hidraw_devinfo {
	unsigned int bustype;
	short vendor;
	short product;
};
#define HIDIOCGRAWINFO _IOR('H', 0x03, struct hidraw_devinfo)

static void WebOS_SwitchProHandshake(void) {
	struct stat st;
	struct hidraw_devinfo info;
	int fd, i, gone = 0;
	const unsigned char cmd[] = { 0x80, 0x02 };

	printf("--- Switch Pro handshake ---" _NL);
	for (i = 0; i < 32; i++) {
		char path[64];
		snprintf(path, sizeof(path), "/dev/hidraw%d", i);
		/* webOS pre-creates all /dev/hidrawN nodes, so skip the rest once
		   several in a row fail to open with ENODEV */
		if (stat(path, &st) != 0 || (fd = open(path, O_RDWR | O_NONBLOCK)) < 0) {
			if (errno == ENOENT || ++gone >= 2) break;
			continue;
		}
		gone = 0;

		if (ioctl(fd, HIDIOCGRAWINFO, &info) != 0) {
			printf("%s: HIDIOCGRAWINFO failed: %s" _NL, path, strerror(errno));
			close(fd); continue;
		}
		printf("%s: bus=%u vid=%04x pid=%04x" _NL, path, info.bustype, info.vendor, info.product);
		if (info.vendor != 0x057e || info.product != 0x2009) { close(fd); continue; }

		ssize_t n = write(fd, cmd, sizeof(cmd));
		printf("%s: wrote handshake: %zd (%s)" _NL, path, n, n >= 0 ? "ok" : strerror(errno));
		close(fd);
	}
}

static void WebOS_DumpJoysticks(const char* label) {
	SDL_version sv;
	SDL_GameController* gc;
	SDL_Joystick* js;
	SDL_JoystickGUID g;
	char guid[33];
	char* map;
	int n, i;

	SDL_GetVersion(&sv);
	printf("%s: SDL %u.%u.%u, NumJoysticks: %d" _NL, label, sv.major, sv.minor, sv.patch, (n = SDL_NumJoysticks()));
	for (i = 0; i < n; i++) {
		const char* name = SDL_JoystickNameForIndex(i);
		printf("  [%d] name='%s' isGameController=%d" _NL, i, name ? name : "(null)", SDL_IsGameController(i));
		js = SDL_JoystickOpen(i);
		if (js) {
			g = SDL_JoystickGetDeviceGUID(i);
			SDL_JoystickGetGUIDString(g, guid, sizeof(guid));
			printf("      guid=%s axes=%d buttons=%d balls=%d hats=%d" _NL,
				guid, SDL_JoystickNumAxes(js), SDL_JoystickNumButtons(js),
				SDL_JoystickNumBalls(js), SDL_JoystickNumHats(js));
			SDL_JoystickClose(js);
		}
		if (SDL_IsGameController(i)) {
			map = SDL_GameControllerMappingForGUID(SDL_JoystickGetDeviceGUID(i));
			printf("      mapping: %s" _NL, map ? map : "(none)");
			if (map) SDL_free(map);
		}
	}
}

/* Interactive probe step: open the first game controller, wait for a button
   press (up to 120s), then record 30s of raw + mapped axis/button changes. */
static void WebOS_ProbeLiveRead(void) {
	SDL_GameController* gc = NULL;
	int i;
	for (i = 0; i < SDL_NumJoysticks(); i++) {
		if (!SDL_IsGameController(i)) continue;
		gc = SDL_GameControllerOpen(i);
		if (gc) break;
	}
	if (gc) {
		SDL_Joystick* js = SDL_GameControllerGetJoystick(gc);
		int16_t rawA[16] = {0}, mapA[SDL_CONTROLLER_AXIS_MAX] = {0};
		cc_uint8 rawB[32] = {0}, mapB[SDL_CONTROLLER_BUTTON_MAX] = {0};
		int nAxes = SDL_JoystickNumAxes(js), nBtns = SDL_JoystickNumButtons(js);
		if (nAxes > 16) nAxes = 16;
		if (nBtns > 32) nBtns = 32;
		printf("--- live read controller [%d] (%s), press any button to start recording ---" _NL,
			i, SDL_GameControllerName(gc) ? SDL_GameControllerName(gc) : "?");
		printf("    RAW axes/buttons = physical device, MAPPED = via gamecontroller mapping" _NL);
		{
			int b;
			cc_uint32 tw0 = SDL_GetTicks();
			while (SDL_GetTicks() - tw0 < 120000) {
				int pressed = 0;
				SDL_GameControllerUpdate();
				for (b = 0; b < nBtns; b++)
					if (SDL_JoystickGetButton(js, b)) { pressed = 1; break; }
				if (pressed) break;
				SDL_Delay(16);
			}
		}
		printf("    recording for 30s now..." _NL);
		cc_uint32 t0 = SDL_GetTicks();
		while (SDL_GetTicks() - t0 < 30000) {
			int a, b;
			SDL_GameControllerUpdate();
			for (a = 0; a < nAxes; a++) {
				int16_t v = SDL_JoystickGetAxis(js, a);
				if (Math_AbsI(v) <= 2000 && rawA[a] == 0) continue;
				if (v == rawA[a]) continue;
				printf("  RAW axis %d = %d%s" _NL, a, v, (Math_AbsI(v) > 2000) ? "" : " (centered)");
				rawA[a] = v;
			}
			for (a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++) {
				int16_t v = SDL_GameControllerGetAxis(gc, a);
				if (v == mapA[a]) continue;
				printf("  MAP axis %d = %d" _NL, a, v);
				mapA[a] = v;
			}
			for (b = 0; b < nBtns; b++) {
				int v = SDL_JoystickGetButton(js, b);
				if (v == rawB[b]) continue;
				printf("  RAW button %d %s" _NL, b, v ? "pressed" : "released");
				rawB[b] = v;
			}
			for (b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
				int v = SDL_GameControllerGetButton(gc, b);
				if (v == mapB[b]) continue;
				printf("  MAP button %d %s" _NL, b, v ? "pressed" : "released");
				mapB[b] = v;
			}
			SDL_Delay(16);
		}
		SDL_GameControllerClose(gc);
	}
	printf("=== probe done ===" _NL);
}

/* Marker-triggered debug tool: prints input devices and SDL joystick info */
static void WebOS_ProbeJoysticks(void) {
	struct dirent* ent;
	DIR* dir;
	FILE* f;
	char buf[512];

	printf("=== ClassiCube joystick probe ===" _NL);
	printf("--- /dev/input listing ---" _NL);
	dir = opendir("/dev/input");
	if (dir) {
		while ((ent = readdir(dir)) != NULL) {
			if (ent->d_name[0] == '.') continue;
			printf("/dev/input/%s" _NL, ent->d_name);
		}
		closedir(dir);
	} else { printf("opendir(/dev/input) failed: %s" _NL, strerror(errno)); }

	printf("--- /proc/bus/input/devices ---" _NL);
	f = fopen("/proc/bus/input/devices", "r");
	if (f) {
		while (fgets(buf, sizeof(buf), f)) printf("%s", buf);
		fclose(f);
	} else { printf("open /proc/bus/input/devices failed: %s" _NL, strerror(errno)); }

	/* The real app sets HIDAPI=0 in WebOS_PreSDLInit before SDL_Init; mirror that */
	SDL_SetHint("SDL_HINT_JOYSTICK_HIDAPI", "0");
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
		printf("SDL init failed: %s" _NL, SDL_GetError());
	WebOS_DumpJoysticks("HIDAPI=0");

	/* Show whether our gamecontrollerdb.txt can override the baked-in mappings */
	{
		char dbPath[256], exe[NATIVE_STR_LEN];
		int l = readlink("/proc/self/exe", exe, NATIVE_STR_LEN - 1), i;
		if (l <= 0) { printf("readlink /proc/self/exe failed" _NL); }
		else {
			exe[l] = '\0';
			printf("probe exe raw: '%s' (len %d)" _NL, exe, l);
			for (i = l - 1; i >= 0; i--) if (exe[i] == '/') break;
			exe[i] = '\0';
			snprintf(dbPath, sizeof(dbPath), "%s/gamecontrollerdb.txt", exe);
			printf("probe dbPath: '%s'" _NL, dbPath);
			int loaded = SDL_GameControllerAddMappingsFromFile(dbPath);
			printf("loaded %d mappings from '%s'" _NL, loaded, dbPath);
		}
	}
	WebOS_DumpJoysticks("after file load");

	WebOS_ProbeLiveRead();

	exit(0);
}

/* psvita-style data dir: all game data (options.txt, texpacks/, texturecache/,
   audio/, maps/, plugins/, client.log) is redirected into <exedir>/.config/ by
   Platform_EncodePath, keeping the app folder itself clean. */
static char webosDataDir[NATIVE_STR_LEN];
static int  webosDataDirLen;

static void WebOS_InitDataDir(void) {
	char path[NATIVE_STR_LEN];
	int i, len;
	if (webosDataDirLen) return;

	len = readlink("/proc/self/exe", path, NATIVE_STR_LEN - 1);
	if (len <= 0) return;
	path[len] = '\0';
	/* trim filename at the end of the path */
	for (i = len - 1; i >= 0; i--, len--) {
		if (path[i] == '/') break;
	}
	if (len <= 1) return;
	path[len] = '\0';
	/* make the app dir the cwd so relative paths (gamecontrollerdb.txt) work */
	chdir(path);
	Mem_Copy(path + len, ".config/", sizeof(".config/"));
	len += (int)sizeof(".config/") - 1;
	path[len] = '\0';

	mkdir(path, 0700);
	Mem_Copy(webosDataDir, path, len + 1);
	webosDataDirLen = len;
}

void WebOS_Bootstrap(int argc, char** argv) {
	struct stat st;
	char sockpath[256], logpath[NATIVE_STR_LEN];
	const char* runtime_dir;
	const char* logfile;
	int i;

	WebOS_InitDataDir();

	if (!getenv("EGL_PLATFORM")) setenv("EGL_PLATFORM", "wayland", 0);
	if (!getenv("XDG_RUNTIME_DIR")) setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 0);
	mkdir("/tmp/xdg", 0700);

	logfile = getenv("CC_LOG_FILE");
	if (!logfile) logfile = "client.log";
	if (webosDataDirLen) {
		snprintf(logpath, sizeof(logpath), "%s%s", webosDataDir, logfile);
		freopen(logpath, "a", stdout);
		freopen(logpath, "a", stderr);
	} else {
		freopen(logfile, "a", stdout);
		freopen(logfile, "a", stderr);
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("logging to '%s'" _NL, webosDataDirLen ? logpath : logfile);
	for (i = 0; i < argc; i++) printf("argv[%d]='%s'" _NL, i, argv[i]);

	runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (runtime_dir && runtime_dir[0]) {
		snprintf(sockpath, sizeof(sockpath), "%s/wayland-0", runtime_dir);
		if (stat(sockpath, &st) != 0) {
			/* Try to find the wayland socket and symlink it into XDG_RUNTIME_DIR */
			static const char* candidates[] = {
				"/run/wayland-0", "/var/run/wayland-0",
				"/run/user/0/wayland-0", "/tmp/wayland-0", NULL
			};
			int i;
			for (i = 0; candidates[i]; i++) {
				if (stat(candidates[i], &st) != 0 || !S_ISSOCK(st.st_mode)) continue;
				if (symlink(candidates[i], sockpath) == 0)
					printf("wayland socket: linked %s -> %s" _NL, candidates[i], sockpath);
				break;
			}
		}
	}

	WebOS_SwitchProHandshake();

	if (webosDataDirLen) {
		snprintf(sockpath, sizeof(sockpath), "%scc_probe_joy", webosDataDir);
		if (access(sockpath, F_OK) == 0) WebOS_ProbeJoysticks();
	}
	if (access("cc_probe_joy", F_OK) == 0) WebOS_ProbeJoysticks();
}

/*########################################################################################################################*
*-----------------------------------------------------Directory/File------------------------------------------------------*
*#########################################################################################################################*/
/* psvita-style: prefix relative paths with the data dir so all game data
   (options.txt, texpacks/, texturecache/, audio/, maps/, plugins/, client.log)
   is kept in <exedir>/.config/ instead of the app folder itself. */
void Platform_EncodePath(cc_filepath* dst, const cc_string* path) {
	char* str = dst->buffer;
	if (webosDataDirLen && path->length > 0 && path->buffer[0] != '/') {
		Mem_Copy(str, webosDataDir, webosDataDirLen);
		str += webosDataDirLen;
	}
	String_EncodeUtf8(str, path);
}

/*########################################################################################################################*
*-----------------------------------------------------Dynamic libraries---------------------------------------------------*
*#########################################################################################################################*/
/* system library names must reach dlopen unmodified so the dynamic linker
   searches the default paths; the data dir prefix is for game data files */
void* DynamicLib_Load2(const cc_string* path) {
	cc_filepath str;
	String_EncodeUtf8(str.buffer, path);
	return dlopen(str.buffer, RTLD_NOW);
}

/*########################################################################################################################*
*-----------------------------------------------------Configuration-------------------------------------------------------*
*#########################################################################################################################*/
int Platform_GetCommandLineArgs(int argc, STRING_REF char** argv, cc_string* args) {
	int i, count, numArgs;
	argc--; argv++; /* skip executable path argument */

	count = min(argc, GAME_MAX_CMDARGS);
	numArgs = 0;
	for (i = 0; i < count; i++) 
	{
		/* -d[directory] argument used to change directory data is stored in */
		if (argv[i][0] == '-' && argv[i][1] == 'd' && argv[i][2]) {
			Process_Abort("-d argument no longer supported - cd to desired working directory instead");
			continue;
		}
		/* The webOS app manager passes the appInfo JSON as a command line argument.
		   Skip it so the launcher runs; the game fork (`ClassiCube <user>` etc) is
		   still parsed normally. */
		if (argv[i][0] == '{') continue;
		args[numArgs] = String_FromReadonly(argv[i]);
		numArgs++;
	}
	return numArgs;
}
#endif
