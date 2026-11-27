// mw2coop: campaign co-op for Modern Warfare 2 (2009), 64-bit Steam build.
// Milestone 2: run console commands from a file and watch dvars (QA remote control).
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "access.h"
#include "enhopts.h"
#include "netwatch.h"
#include "control.h"
#include "crash.h"
#include "d3dcap.h"
#include "dx/natboot.h"
#include "firewall.h"
#include "update.h"
#include "devices.h"
#include "console.h"
#include "guards.h"
#include "climbads.h"
#include "window.h"
#include "native.h"
#include "game.h"
#include "localize.h"
#include "log.h"
#include "menus.h"
#include "mirror.h"
#include "rumble.h"
#include "net.h"
#include "party.h"
#include "progress.h"
#include "rejoin.h"
#include "report.h"
#include "weaponfov.h"
#include "markers.h"
#include "voice.h"
#include "chat.h"
#include "saves.h"
#include "scrdebug.h"
#include "settings.h"
#include "shot.h"
#include "fov.h"
#include "steamnet.h"
#include "transplant.h"
#include "upnp.h"
#include "zones.h"
#include "scripts.h"
#include "ping.h"

int proxy_init(void);
int proxy_host_is_game(void);

static BOOL CALLBACK find_game_window(HWND h, LPARAM out)
{
	DWORD pid;
	char cls[32];
	GetWindowThreadProcessId(h, &pid);
	if (pid == GetCurrentProcessId() && GetClassNameA(h, cls, sizeof(cls)) && lstrcmpA(cls, "IW4") == 0) {
		*(HWND *)out = h;
		return FALSE;
	}
	return TRUE;
}

static DWORD WINAPI watcher(LPVOID unused)
{
	(void)unused;
	crash_stack_watch(); // (a start that hangs before the window can still be asked for its stacks)
	DWORD start = GetTickCount();
	HWND game = NULL;
	while (!game && GetTickCount() - start < 120000) {
		Sleep(100);
		EnumWindows(find_game_window, (LPARAM)&game);
	}
	if (!game) {
		log_printf("main: no game window after 120 s");
		return 0;
	}
	log_printf("main: game window %p after %lu ms", (void *)game, GetTickCount() - start);
	if (game_init()) {
		crash_install();
		addr_scratch_check("after crash_install");
		scripts_install();
		addr_scratch_check("after scripts_install");
		mirror_install();
		addr_scratch_check("after mirror_install");
		rumble_install();
		addr_scratch_check("after rumble_install");
		transplant_install();
		addr_scratch_check("after transplant_install");
		zones_install();
		addr_scratch_check("after zones_install");
		localize_install();
		addr_scratch_check("after localize_install");
		scrdebug_install();
		addr_scratch_check("after scrdebug_install");
		net_install();
		addr_scratch_check("after net_install");
		party_install();
		addr_scratch_check("after party_install");
		progress_install();
		addr_scratch_check("after progress_install");
		rejoin_install();
		report_install();
		weaponfov_install();
		markers_install();
		voice_install();
		chat_install();
		saves_install();
		addr_scratch_check("after saves_install");
		menus_install();
		addr_scratch_check("after menus_install");
		access_install();
		enhopts_install();
		netwatch_install();
		addr_scratch_check("after access_install");
		upnp_install();
		addr_scratch_check("after upnp_install");
		steamnet_install();
		addr_scratch_check("after steamnet_install");
		firewall_install();
		addr_scratch_check("after firewall_install");
		update_install(); // (asks GitHub for a newer release: [update] check)
		ping_install();   // (the co-op markers' key: [input] ping_key)
		shot_install();
		addr_scratch_check("after shot_install");
		fov_install();
		addr_scratch_check("after fov_install");
		devices_install();
		addr_scratch_check("after devices_install");
		native_install();
		addr_scratch_check("after native_install");
		guards_install();
		addr_scratch_check("after guards_install");
		climbads_install();
		addr_scratch_check("after climbads_install");
		window_install();
		addr_scratch_check("after window_install");
		console_install();
		addr_scratch_check("after console_install");
		control_start();
	}
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_DETACH && reserved) {
		// the process is ending: log where in the game it was asked to (an exit before the game
		// window shows leaves no other trace; the game's own console log is not open yet)
		void *frames[48];
		USHORT n = RtlCaptureStackBackTrace(0, 48, frames, NULL);
		uint8_t *exe = (uint8_t *)GetModuleHandleA(NULL);
		char line[512] = "";
		for (USHORT i = 0; i < n; i++) {
			HMODULE mod = NULL;
			char name[MAX_PATH] = "?";
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					       (LPCSTR)frames[i], &mod)) {
				GetModuleFileNameA(mod, name, MAX_PATH);
			}
			const char *file = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
			snprintf(line + strlen(line), sizeof(line) - strlen(line), " %s+0x%llx", (uint8_t *)mod == exe ? "exe" : file,
				 (unsigned long long)((uint8_t *)frames[i] - (uint8_t *)mod));
		}
		log_printf("main: process exit, stack:%s", line);
		return TRUE;
	}
	if (reason != DLL_PROCESS_ATTACH)
		return TRUE;
	DisableThreadLibraryCalls(self);
	if (!proxy_host_is_game()) // not the single-player game (the multiplayer iw4mp.exe, a tool): only the system's winmm calls passed through (proxy.c)
		return proxy_init() ? TRUE : FALSE;
	log_open();
	console_com_maxfps_capture_config(); // snapshot before the engine can rewrite config.cfg
	settings_load(); // before the game registers net_port

	char exe[MAX_PATH];
	GetModuleFileNameA(NULL, exe, MAX_PATH);
	HMODULE base = GetModuleHandleA(NULL);
	SYSTEMTIME day;
	GetLocalTime(&day);
	log_printf("main: %04d-%02d-%02d, attached to %s, image base %p, pid %lu", day.wYear, day.wMonth, day.wDay, exe,
		   (void *)base, GetCurrentProcessId());

	natboot_install(); // MW2COOP_NATIVE11=1: the native renderer in place of the Direct3D 9 back end
	d3dcap_install(); // [display] d3d9ex: the exe's import of Direct3DCreate9 is pointed at the extended Direct3D 9 before the game creates its device
	if (!proxy_init()) {
		log_printf("main: could not load the real winmm.dll");
		return FALSE;
	}
	CloseHandle(CreateThread(NULL, 0, watcher, NULL, 0, NULL));
	return TRUE;
}
