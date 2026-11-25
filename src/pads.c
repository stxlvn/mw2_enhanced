// The mod's gamepads (pads.h). The game's single player reads no gamepad itself; every pad the mod uses (the split screen's players, the lobby's device pick, the menus, the
// rumble) goes through pads_get_state / pads_set_state, the mod's XInputGetState / XInputSetState.
//
// Xbox pads (and anything that is an XInput pad: Steam Input's and DS4Windows' virtual pads, an 8BitDo in its X-input mode) are read through XInput as before, in their own slots.
// Every other pad is read by SDL3 (SDL3.dll, zlib license, loaded at run time from mw2coop\SDL3.dll; version 3.4.20, local/third-party/PINS.md): PlayStation DualShock 4,
// DualSense and DualSense Edge, Nintendo Switch pads, 8BitDo and others in their own modes, over USB or Bluetooth, with rumble. SDL3 runs only its HIDAPI drivers here
// (its XInput, Raw Input, Windows.Gaming.Input, GameInput, DirectInput and HIDAPI Xbox paths are off), so an Xbox pad is never seen twice. A pad SDL3 reads takes the first
// XInput slot that has no XInput pad and leaves it when an XInput pad comes into that slot; its buttons by position (south = A, east = B, west = X, north = Y; Share / Create
// and the touchpad's click = Back, Options = Start), the sticks with Y up, the triggers 0..255.
//
// Double pads: when Steam Input turns a PlayStation pad into a virtual Xbox pad, Steam tells SDL which physical pads to leave alone (SDL_GAMECONTROLLER_IGNORE_DEVICES in the
// game's environment; SDL_ShouldIgnoreGamepad), so SDL does not open it. DS4Windows or DSX without HidHide is not told: while an XInput pad is connected, a pad SDL reads is
// shown only after its presses are seen to differ from every XInput pad's (two presses), and a pad whose presses are the same as an XInput pad's (three presses, none different)
// is that pad's double and is left out. With no XInput pad connected it is shown at once.
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "pads.h"

#define XB_UP 0x0001
#define XB_DOWN 0x0002
#define XB_LEFT 0x0004
#define XB_RIGHT 0x0008
#define XB_START 0x0010
#define XB_BACK 0x0020
#define XB_LTHUMB 0x0040
#define XB_RTHUMB 0x0080
#define XB_LB 0x0100
#define XB_RB 0x0200
#define XB_A 0x1000
#define XB_B 0x2000
#define XB_X 0x4000
#define XB_Y 0x8000

// ---------------------------------------------------------------------------------------------------------------- SDL3 (the parts used)
// From SDL 3.4.20's include/SDL3: SDL_init.h (SDL_INIT_GAMEPAD), SDL_gamepad.h (the button and axis enums, the functions), SDL_hints.h (the hint names). SDLCALL is __cdecl,
// the default here; `bool` is C's.
typedef struct SDL_Gamepad SDL_Gamepad;
typedef uint32_t SDL_JoystickID;
#define SDL_INIT_GAMEPAD 0x00002000u
enum { SB_SOUTH, SB_EAST, SB_WEST, SB_NORTH, SB_BACK, SB_GUIDE, SB_START, SB_LEFT_STICK, SB_RIGHT_STICK, SB_LEFT_SHOULDER, SB_RIGHT_SHOULDER, SB_DPAD_UP, SB_DPAD_DOWN,
       SB_DPAD_LEFT, SB_DPAD_RIGHT, SB_MISC1, SB_RIGHT_PADDLE1, SB_LEFT_PADDLE1, SB_RIGHT_PADDLE2, SB_LEFT_PADDLE2, SB_TOUCHPAD };
enum { SA_LEFTX, SA_LEFTY, SA_RIGHTX, SA_RIGHTY, SA_LEFT_TRIGGER, SA_RIGHT_TRIGGER };

static struct {
	bool (*Init)(uint32_t flags);
	bool (*SetHint)(const char *name, const char *value);
	SDL_JoystickID *(*GetGamepads)(int *count);
	void (*free)(void *mem);
	SDL_Gamepad *(*OpenGamepad)(SDL_JoystickID id);
	void (*CloseGamepad)(SDL_Gamepad *gp);
	bool (*GamepadConnected)(SDL_Gamepad *gp);
	void (*UpdateGamepads)(void);
	bool (*GetGamepadButton)(SDL_Gamepad *gp, int button);
	int16_t (*GetGamepadAxis)(SDL_Gamepad *gp, int axis);
	bool (*RumbleGamepad)(SDL_Gamepad *gp, uint16_t low, uint16_t high, uint32_t duration_ms);
	const char *(*GetGamepadName)(SDL_Gamepad *gp);
	uint16_t (*GetGamepadVendor)(SDL_Gamepad *gp);
	uint16_t (*GetGamepadProduct)(SDL_Gamepad *gp);
	void (*SetGamepadEventsEnabled)(bool enabled);
	void (*SetJoystickEventsEnabled)(bool enabled);
	void (*FlushEvents)(uint32_t min_type, uint32_t max_type);
	const char *(*GetError)(void);
	int (*GetVersion)(void);
} S;

static int load_sdl(void)
{
	char path[MAX_PATH];
	DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
	while (n > 0 && path[n - 1] != 0x5c)
		n--;
	path[n] = 0;
	lstrcatA(path, "mw2coop\\SDL3.dll");
	HMODULE m = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH); // (by its full path: never an SDL3.dll from elsewhere on the search path)
	if (!m)
		return 0;
#define SDL_FN(field, name)                                                  \
	do {                                                                     \
		*(FARPROC *)&S.field = GetProcAddress(m, name);                      \
		if (!S.field) {                                                      \
			log_printf("pads: SDL3.dll has no %s: other pads are off", name); \
			return 0;                                                        \
		}                                                                    \
	} while (0)
	SDL_FN(Init, "SDL_Init");
	SDL_FN(SetHint, "SDL_SetHint");
	SDL_FN(GetGamepads, "SDL_GetGamepads");
	SDL_FN(free, "SDL_free");
	SDL_FN(OpenGamepad, "SDL_OpenGamepad");
	SDL_FN(CloseGamepad, "SDL_CloseGamepad");
	SDL_FN(GamepadConnected, "SDL_GamepadConnected");
	SDL_FN(UpdateGamepads, "SDL_UpdateGamepads");
	SDL_FN(GetGamepadButton, "SDL_GetGamepadButton");
	SDL_FN(GetGamepadAxis, "SDL_GetGamepadAxis");
	SDL_FN(RumbleGamepad, "SDL_RumbleGamepad");
	SDL_FN(GetGamepadName, "SDL_GetGamepadName");
	SDL_FN(GetGamepadVendor, "SDL_GetGamepadVendor");
	SDL_FN(GetGamepadProduct, "SDL_GetGamepadProduct");
	SDL_FN(SetGamepadEventsEnabled, "SDL_SetGamepadEventsEnabled");
	SDL_FN(SetJoystickEventsEnabled, "SDL_SetJoystickEventsEnabled");
	SDL_FN(FlushEvents, "SDL_FlushEvents");
	SDL_FN(GetError, "SDL_GetError");
	SDL_FN(GetVersion, "SDL_GetVersion");
#undef SDL_FN
	return 1;
}

// ---------------------------------------------------------------------------------------------------------------- the pads SDL reads

#define MAX_SDL 8
typedef struct {
	int used;
	SDL_JoystickID id;
	SDL_Gamepad *gp;
	char name[64];
	pads_gamepad_t st; // (under state_cs)
	DWORD packet;
	// the double check (see the top of the file)
	int dup, shown_ok, matches, mismatches, stable;
	WORD last_buttons, judged_buttons;
	// rumble: (big << 8) | small as the game asked for it, what was sent and when
	volatile LONG want;
	LONG sent;
	DWORD sent_at;
} sp_t;
static sp_t sp[MAX_SDL];
static volatile LONG slot_sp[4]; // XInput slot -> 1 + the SDL pad shown there, 0 none
static CRITICAL_SECTION state_cs;

static DWORD(WINAPI *real_get)(DWORD, pads_state_t *);
static DWORD(WINAPI *real_set)(DWORD, pads_vibration_t *);
static int sdl_on;

static SHORT up(int16_t v) // SDL's Y grows downwards, XInput's upwards
{
	return (SHORT)(-1 - (int)v);
}
static BYTE trigger(int16_t v) // SDL: 0..32767
{
	return v <= 0 ? 0 : (BYTE)((int)v * 255 / 32767);
}

static void read_pad(sp_t *p)
{
	static const struct {
		int button;
		WORD bit;
	} map[] = {
	    {SB_SOUTH, XB_A}, {SB_EAST, XB_B}, {SB_WEST, XB_X}, {SB_NORTH, XB_Y}, {SB_BACK, XB_BACK}, {SB_START, XB_START}, {SB_LEFT_STICK, XB_LTHUMB},
	    {SB_RIGHT_STICK, XB_RTHUMB}, {SB_LEFT_SHOULDER, XB_LB}, {SB_RIGHT_SHOULDER, XB_RB}, {SB_DPAD_UP, XB_UP}, {SB_DPAD_DOWN, XB_DOWN}, {SB_DPAD_LEFT, XB_LEFT},
	    {SB_DPAD_RIGHT, XB_RIGHT}, {SB_TOUCHPAD, XB_BACK},
	};
	pads_gamepad_t g;
	memset(&g, 0, sizeof(g));
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (S.GetGamepadButton(p->gp, map[i].button))
			g.buttons |= map[i].bit;
	g.lx = S.GetGamepadAxis(p->gp, SA_LEFTX);
	g.ly = up(S.GetGamepadAxis(p->gp, SA_LEFTY));
	g.rx = S.GetGamepadAxis(p->gp, SA_RIGHTX);
	g.ry = up(S.GetGamepadAxis(p->gp, SA_RIGHTY));
	g.lt = trigger(S.GetGamepadAxis(p->gp, SA_LEFT_TRIGGER));
	g.rt = trigger(S.GetGamepadAxis(p->gp, SA_RIGHT_TRIGGER));
	EnterCriticalSection(&state_cs);
	if (memcmp(&g, &p->st, sizeof(g)) != 0) {
		p->st = g;
		p->packet++;
	}
	LeaveCriticalSection(&state_cs);
}

static void close_pad(int i)
{
	for (int s = 0; s < 4; s++)
		InterlockedCompareExchange(&slot_sp[s], 0, i + 1);
	EnterCriticalSection(&state_cs); // (pads_get_state copies under it)
	S.CloseGamepad(sp[i].gp);
	log_printf("pads: %s gone", sp[i].name);
	memset(&sp[i], 0, sizeof(sp[i]));
	LeaveCriticalSection(&state_cs);
}

static void enumerate(void)
{
	int n = 0;
	SDL_JoystickID *ids = S.GetGamepads(&n);
	for (int i = 0; i < MAX_SDL; i++) { // gone: not listed any more, or not connected
		if (!sp[i].used)
			continue;
		int listed = 0;
		for (int k = 0; ids && k < n; k++)
			if (ids[k] == sp[i].id)
				listed = 1;
		if (!listed || !S.GamepadConnected(sp[i].gp))
			close_pad(i);
	}
	for (int k = 0; ids && k < n; k++) { // new
		int known = 0, free_i = -1;
		for (int i = 0; i < MAX_SDL; i++) {
			if (sp[i].used && sp[i].id == ids[k])
				known = 1;
			if (!sp[i].used && free_i < 0)
				free_i = i;
		}
		if (known || free_i < 0)
			continue;
		SDL_Gamepad *gp = S.OpenGamepad(ids[k]);
		if (!gp) {
			log_printf("pads: SDL could not open a gamepad: %s", S.GetError());
			continue;
		}
		sp_t *p = &sp[free_i];
		memset(p, 0, sizeof(*p));
		p->used = 1;
		p->id = ids[k];
		p->gp = gp;
		p->sent = -1;
		const char *name = S.GetGamepadName(gp);
		snprintf(p->name, sizeof(p->name), "%s", name && name[0] ? name : "a gamepad");
		log_printf("pads: %s found through SDL (vendor %04x product %04x)", p->name, S.GetGamepadVendor(gp), S.GetGamepadProduct(gp));
	}
	if (ids)
		S.free(ids);
}

// ---------------------------------------------------------------------------------------------------------------- slots and the double check

static void judge(sp_t *p, int index, const pads_state_t real[4], int real_on)
{
	EnterCriticalSection(&state_cs);
	const WORD b = p->st.buttons;
	LeaveCriticalSection(&state_cs);
	p->stable = b == p->last_buttons ? p->stable + 1 : 0;
	p->last_buttons = b;
	if (p->dup || !b || p->stable != 2 || b == p->judged_buttons) // a press held still for two looks (40 ms), each press once
		return;
	p->judged_buttons = b;
	if (!real_on)
		return;
	int same = 0;
	for (int s = 0; s < 4; s++)
		if ((real_on & (1 << s)) && real[s].pad.buttons == b)
			same = 1;
	if (same)
		p->matches++;
	else
		p->mismatches++;
	if (!p->shown_ok && p->mismatches >= 2) {
		p->shown_ok = 1;
		log_printf("pads: %s is a pad of its own (its presses differ from the XInput pads'): shown", p->name);
	}
	if (p->matches >= 3 && !p->mismatches) {
		p->dup = 1;
		for (int s = 0; s < 4; s++)
			InterlockedCompareExchange(&slot_sp[s], 0, index + 1);
		log_printf("pads: %s is also an XInput pad (DS4Windows, DSX or Steam Input makes it one): left out, the XInput pad drives the player", p->name);
	}
	if (p->matches > 0 && p->mismatches > 0)
		p->matches = 0; // (a mix: not a double)
}

static void place(int real_on)
{
	static int last_on = -1;
	if (real_on != last_on) { // the set of XInput pads changed: who is a double is looked at again
		for (int i = 0; i < MAX_SDL; i++)
			if (sp[i].used && !sp[i].dup)
				sp[i].matches = sp[i].mismatches = 0;
		last_on = real_on;
	}
	for (int s = 0; s < 4; s++) { // an XInput pad came into a slot an SDL pad had, or the pad may not be shown: it moves or goes
		const LONG v = slot_sp[s];
		if (v && ((real_on & (1 << s)) || !sp[v - 1].used || sp[v - 1].dup || (real_on && !sp[v - 1].shown_ok)))
			InterlockedExchange(&slot_sp[s], 0);
	}
	for (int i = 0; i < MAX_SDL; i++) {
		sp_t *p = &sp[i];
		if (!p->used || p->dup || (real_on && !p->shown_ok))
			continue;
		int has = 0;
		for (int s = 0; s < 4; s++)
			if (slot_sp[s] == i + 1)
				has = 1;
		for (int s = 0; s < 4 && !has; s++)
			if (!(real_on & (1 << s)) && !slot_sp[s]) {
				InterlockedExchange(&slot_sp[s], i + 1);
				has = 1;
				log_printf("pads: %s is gamepad %d", p->name, s + 1);
			}
	}
}

static void rumble(sp_t *p, DWORD now)
{
	const LONG want = p->want;
	if (want == p->sent && (!want || now - p->sent_at < 1000)) // a running rumble is renewed every second (SDL stops it after the duration given)
		return;
	const uint16_t low = (uint16_t)(((want >> 8) & 0xff) * 257), high = (uint16_t)((want & 0xff) * 257);
	S.RumbleGamepad(p->gp, low, high, want ? 2000 : 0);
	p->sent = want;
	p->sent_at = now;
}

static DWORD WINAPI sdl_thread(LPVOID unused)
{
	(void)unused;
	// SDL3's HIDAPI drivers only: the Xbox pads stay with XInput (above), so none is seen twice
	static const char *const off[] = {"SDL_XINPUT_ENABLED", "SDL_JOYSTICK_RAWINPUT", "SDL_JOYSTICK_WGI", "SDL_JOYSTICK_GAMEINPUT", "SDL_JOYSTICK_DIRECTINPUT",
					  "SDL_JOYSTICK_HIDAPI_XBOX", "SDL_JOYSTICK_HIDAPI_XBOX_360", "SDL_JOYSTICK_HIDAPI_XBOX_ONE"};
	for (size_t i = 0; i < sizeof(off) / sizeof(off[0]); i++)
		S.SetHint(off[i], "0");
	S.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1"); // (no SDL window: the pads are read whatever window is in front; the mod decides what reaches the game)
	S.SetHint("SDL_NO_SIGNAL_HANDLERS", "1");		     // (the game's own handlers stay)
	if (!S.Init(SDL_INIT_GAMEPAD)) {
		log_printf("pads: SDL could not start its gamepads (%s): Xbox pads only", S.GetError());
		InterlockedExchange((volatile LONG *)&sdl_on, 0);
		return 0;
	}
	S.SetJoystickEventsEnabled(false); // (the state is read here, not through events: SDL_UpdateGamepads below)
	S.SetGamepadEventsEnabled(false);
	const int v = S.GetVersion();
	log_printf("pads: Xbox pads through XInput, other pads through SDL %d.%d.%d ([input] sdlpads=0 turns that off)", v / 1000000, v / 1000 % 1000, v % 1000);
	DWORD last_enum = 0, last_look = 0, last_flush = 0;
	for (;;) {
		const DWORD now = GetTickCount();
		int any_sdl = 0;
		for (int i = 0; i < MAX_SDL; i++)
			any_sdl |= sp[i].used;
		if (!last_enum || now - last_enum >= 500) {
			// SDL still needs an occasional state update to notice a newly attached device,
			// but the empty-device case does not need a 62.5 Hz update loop.
			if (!any_sdl)
				S.UpdateGamepads();
			enumerate();
			last_enum = now;
			any_sdl = 0;
			for (int i = 0; i < MAX_SDL; i++)
				any_sdl |= sp[i].used;
		}
		// With no SDL gamepad open, neither SDL state updates nor XInput-vs-SDL duplicate
		// detection can change anything. Device discovery still runs every 500 ms above.
		if (any_sdl) {
			S.UpdateGamepads();
			for (int i = 0; i < MAX_SDL; i++)
				if (sp[i].used)
					read_pad(&sp[i]);
		}
		if (any_sdl && now - last_look >= 20) {
			pads_state_t real[4];
			int real_on = 0;
			// an empty XInput slot is the costly query: one that held no pad is asked again once a second, not every 20 ms (audit 2026-11-18, glue F10)
			static int empty[4];
			static DWORD empty_at;
			const int probe_empty = now - empty_at >= 1000;
			if (probe_empty)
				empty_at = now;
			for (int s = 0; s < 4; s++) {
				memset(&real[s], 0, sizeof(real[s]));
				if (empty[s] && !probe_empty)
					continue;
				if (real_get && real_get((DWORD)s, &real[s]) == ERROR_SUCCESS)
					real_on |= 1 << s;
				empty[s] = !(real_on & (1 << s));
			}
			for (int i = 0; i < MAX_SDL; i++)
				if (sp[i].used) {
					judge(&sp[i], i, real, real_on);
					rumble(&sp[i], now);
				}
			place(real_on);
			last_look = now;
		}
		if (now - last_flush >= 1000) { // (device added / removed events still queue up; nothing reads them)
			S.FlushEvents(0, 0xffffffffu);
			last_flush = now;
		}
		Sleep(any_sdl ? 4 : 100); // no SDL pad: discover every 500 ms, without 62.5 Hz empty polling
	}
	return 0;
}

// ---------------------------------------------------------------------------------------------------------------- the XInput face

static BOOL CALLBACK start_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
	(void)once, (void)param, (void)ctx;
	InitializeCriticalSection(&state_cs);
	const char *dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
	for (int k = 0; k < 3 && !real_get; k++) {
		HMODULE h = LoadLibraryA(dlls[k]);
		if (h) {
			real_get = (DWORD(WINAPI *)(DWORD, pads_state_t *))(void *)GetProcAddress(h, "XInputGetState");
			real_set = (DWORD(WINAPI *)(DWORD, pads_vibration_t *))(void *)GetProcAddress(h, "XInputSetState");
		}
	}
	char ini[MAX_PATH];
	DWORD n = GetModuleFileNameA(NULL, ini, MAX_PATH);
	while (n > 0 && ini[n - 1] != 0x5c)
		n--;
	ini[n] = 0;
	lstrcatA(ini, "mw2coop\\settings.ini");
	if (!GetPrivateProfileIntA("input", "sdlpads", 1, ini)) {
		log_printf("pads: Xbox pads only ([input] sdlpads=0)%s", real_get ? "" : "; no XInput DLL either: no gamepads");
		return TRUE;
	}
	if (!load_sdl()) {
		log_printf("pads: no mw2coop\\SDL3.dll: Xbox pads only (PlayStation and other pads through Steam Input or DS4Windows)%s", real_get ? "" : "; no XInput DLL either");
		return TRUE;
	}
	sdl_on = 1;
	HANDLE t = CreateThread(NULL, 0, sdl_thread, NULL, 0, NULL);
	if (t)
		CloseHandle(t);
	else
		sdl_on = 0;
	return TRUE;
}
static void start(void)
{
	static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
	InitOnceExecuteOnce(&once, start_once, NULL, NULL);
}

DWORD WINAPI pads_get_state(DWORD index, pads_state_t *state)
{
	if (index >= 4 || !state)
		return ERROR_BAD_ARGUMENTS;
	start();
	if (real_get && real_get(index, state) == ERROR_SUCCESS)
		return ERROR_SUCCESS;
	const LONG v = sdl_on ? slot_sp[index] : 0;
	if (!v)
		return ERROR_DEVICE_NOT_CONNECTED;
	DWORD rc = ERROR_DEVICE_NOT_CONNECTED;
	EnterCriticalSection(&state_cs);
	const sp_t *p = &sp[v - 1];
	if (p->used) {
		state->packet = p->packet;
		state->pad = p->st;
		rc = ERROR_SUCCESS;
	}
	LeaveCriticalSection(&state_cs);
	return rc;
}

DWORD WINAPI pads_set_state(DWORD index, pads_vibration_t *vibration)
{
	if (index >= 4 || !vibration)
		return ERROR_BAD_ARGUMENTS;
	start();
	if (real_set && real_set(index, vibration) == ERROR_SUCCESS)
		return ERROR_SUCCESS;
	const LONG v = sdl_on ? slot_sp[index] : 0;
	if (!v || !sp[v - 1].used)
		return ERROR_DEVICE_NOT_CONNECTED;
	InterlockedExchange(&sp[v - 1].want, (LONG)(((vibration->left >> 8) << 8) | (vibration->right >> 8)));
	return ERROR_SUCCESS;
}

void *pads_xinput_proc(const char *name)
{
	if (!name)
		return NULL;
	if (!strcmp(name, "XInputGetState"))
		return (void *)pads_get_state;
	if (!strcmp(name, "XInputSetState"))
		return (void *)pads_set_state;
	return NULL;
}
