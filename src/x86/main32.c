// mw2coop for the 32-bit build of Modern Warfare 2 (the build before the 2026 move to 64 bits). The first
// slice: the winmm.dll proxy loads into iw4sp.exe, logs what game it is in and whether it knows that build.
// The hooks of the 64-bit DLL come over one at a time, each with the addresses of this build (see
// docs/32bit-plan.md); so far the script loader (scripts.c) and the QA command file.
#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "game.h"
#include "addr.h"
#include "log.h"
#include "scripts.h"
#include "zones.h"
#include "control.h"
#include "d3dcap.h"
#include "crash.h"
#include "fov.h"
#include "iw4xtext.h"
#include "window.h"
#include "access.h"
#include "enhopts.h"
#include "netwatch.h"
#include "firewall.h"
#include "update.h"
#include "devices.h"
#include "console.h"
#include "localize.h"
#include "menus.h"
#include "net.h"
#include "progress.h"
#include "rejoin.h"
#include "report.h"
#include "weaponfov.h"
#include "markers.h"
#include "voice.h"
#include "chat.h"
#include "saves.h"
#include "scrdebug.h"
#include "steamnet.h"
#include "transplant.h"
#include "mirror.h"
#include "guards.h"
#include "climbads.h"
#include "native.h"
#include "rumble.h"
#include "shot.h"
#include "party.h"
#include "settings.h"
#include "upnp.h"
#include "ping.h"

int proxy_init(void);
int proxy_host_is_game(void);

// The official exe is wrapped by Steam's DRM and Steam rewrites that wrapper for each account at the first start
// (the file in a test copy grew from 3,497,984 to 3,503,672 bytes and its hash changed), so the file's hash
// says nothing about the build. The code is the same once the wrapper has unpacked it: the build is known by the
// SHA-256 of the exe's executable sections in memory, taken when the game window exists.
static const struct {
	const char *code_sha256; // upper case
	const char *name;
} builds[] = {
	{"FC9E4843FF9F0E2E17EAABBE254B96FAF3EE7A2750D143E65597AD33481F52D7", "official Steam build, 32-bit, linked 2010-02-10 (depots 10181, 10182)"},
};

static void hex_of(const uint8_t digest[32], char out[65])
{
	for (int i = 0; i < 32; i++)
		snprintf(out + 2 * i, 3, "%02X", digest[i]);
	out[64] = 0;
}

// SHA-256 of every executable section of the running exe, as it is in memory: 64 hex characters.
static int sha256_code(char out[65], unsigned *bytes)
{
	const uint8_t *base = (const uint8_t *)GetModuleHandleW(NULL);
	const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
	const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
	BCRYPT_ALG_HANDLE alg = NULL;
	BCRYPT_HASH_HANDLE hash = NULL;
	int ok = 0;
	*bytes = 0;
	if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 && BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) == 0) {
		ok = 1;
		for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
			if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
				continue;
			if (BCryptHashData(hash, (PUCHAR)(base + sec[i].VirtualAddress), sec[i].Misc.VirtualSize, 0) != 0)
				ok = 0;
			*bytes += sec[i].Misc.VirtualSize;
		}
		uint8_t digest[32];
		if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0)
			hex_of(digest, out);
		else
			ok = 0;
	}
	if (hash)
		BCryptDestroyHash(hash);
	if (alg)
		BCryptCloseAlgorithmProvider(alg, 0);
	return ok;
}

// The iw4x-sp client (docs: games/modern-warfare-2/NOTES.md, "iw4x-sp support") maps the 1.0.159 iw4sp.exe into its own process and patches
// the code in memory, so the code hash can change with the client's build. The build is known by the SHA-256 of the 1.0.159 file (iw4x_known).
static int sha256_file(const char *path, char out[65])
{
	HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return 0;
	BCRYPT_ALG_HANDLE alg = NULL;
	BCRYPT_HASH_HANDLE hash = NULL;
	int ok = 0;
	if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 && BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) == 0) {
		static uint8_t buf[1 << 16];
		DWORD got;
		ok = 1;
		while (ReadFile(f, buf, sizeof(buf), &got, NULL) && got)
			if (BCryptHashData(hash, buf, got, 0) != 0)
				ok = 0;
		uint8_t digest[32];
		if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0)
			hex_of(digest, out);
		else
			ok = 0;
	}
	if (hash)
		BCryptDestroyHash(hash);
	if (alg)
		BCryptCloseAlgorithmProvider(alg, 0);
	CloseHandle(f);
	return ok;
}

static int iw4x_process;
int game_is_iw4x(void)
{
	return iw4x_process;
}
static void detect_iw4x_process(void)
{
	char path[MAX_PATH];
	DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
	iw4x_process = n >= 11 && lstrcmpiA(path + n - 11, "iw4x-sp.exe") == 0;
}

// Where the 1.0.159 file is: AlterWare's current install puts it in data\iw4sp.exe (the client reads that name) and leaves the store's iw4sp.exe where it is, on
// Steam now the 64-bit exe; older installs replaced iw4sp.exe itself. Only iw4sp.exe was looked at until 2026-11-20: a player's log had the 64-bit iw4sp.exe
// next to the client and the 1.0.159 code running (code SHA-256 the same as ours), so the build counted as unknown and nothing was patched. Both files are
// looked at now, data\ first, and the code hash the tested clients give (iw4x_code_sha256: the same code bytes as the known build) is known on its own too.
static const char iw4x_exe_sha256[] = "9480909B18CF5A4EC483C989AAFCE929D2172C5F9448F8C23B723683D1A43565"; // SP_ALTER_HASH of iw4x-sp
static const char iw4x_code_sha256[] = "2706C3C385B954D22FC12F0AC3F6BB497D0C8B557829531356F9D23ADF80B3E2"; // sha256_code() on 1.0.159 under the clients of 2026-10/11
static int iw4x_known(const char *code_hex)
{
	char dir[MAX_PATH];
	DWORD n = GetModuleFileNameA(NULL, dir, MAX_PATH);
	while (n > 0 && dir[n - 1] != '\\')
		n--;
	dir[n] = 0;
	static const char *const names[] = {"data\\iw4sp.exe", "iw4sp.exe"};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		char path[MAX_PATH], hex[65] = "";
		snprintf(path, sizeof(path), "%s%s", dir, names[i]);
		if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
			continue;
		if (!sha256_file(path, hex)) {
			log_printf("main32: cannot hash %s next to the client", names[i]);
			continue;
		}
		log_printf("main32: %s file SHA-256 %s", names[i], hex);
		if (strcmp(hex, iw4x_exe_sha256) == 0)
			return 1;
	}
	if (strcmp(code_hex, iw4x_code_sha256) == 0) {
		log_printf("main32: no 1.0.159 file found by its hash, but the code is the known 1.0.159 image of the iw4x-sp client");
		return 1;
	}
	return 0;
}

// The game's own console on the iw4x-sp client (player report 2026-11-20: two consoles open, the game's prompt "iw4x-sp: 0.0.1>" over the mod's). The game's key handler (1.0.159
// VA 0x442e60) lets ` and ~ open its console only when the dvar monkeytoy ("restrict console access", VA 0x86e7ec) is 0: the official builds register it as 1, iw4x-sp as 0.
// The mod's window procedure takes the key, but a press that reached the game first (the seconds before the mod is in, the window made again by a vid_restart) opened the
// game's console, which the key then no longer closed. Here monkeytoy goes to 1, as on the official builds, and a game console already open is closed (key catcher bit 1 of
// local client 0, 1.0.159 VA 0x929140): the mod's console is the console here too.
static void iw4x_game_console_off(void)
{
	game_cbuf_addtext("set monkeytoy 1\n");
	volatile unsigned *catchers = (volatile unsigned *)((uint8_t *)GetModuleHandleA(NULL) + XR(0x525bb8));
	if (*catchers & 1) {
		*catchers &= ~1u;
		log_printf("main32: iw4x-sp: the game's own console was open: closed");
	}
	log_printf("main32: iw4x-sp: the game's own console is off (monkeytoy 1, as on the official builds); the mod's console opens with `");
}

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

// For reverse engineering: with the file <game dir>\mw2coop\dump_image present, the exe's image as it is in memory
// (the wrapper has unpacked the code) is written to <game dir>\mw2coop\image32.bin, laid out from the image base
// (pages that cannot be read are zeros), and the flag file is deleted. Load it in Ghidra as a raw binary at the
// image base (docs/32bit-plan.md).
static void dump_image(void)
{
	char dir[MAX_PATH], flag[MAX_PATH], out[MAX_PATH];
	DWORD n = GetModuleFileNameA(NULL, dir, MAX_PATH);
	while (n > 0 && dir[n - 1] != '\\')
		n--;
	dir[n] = 0;
	snprintf(flag, sizeof(flag), "%smw2coop\\dump_image", dir);
	snprintf(out, sizeof(out), "%smw2coop\\image32.bin", dir);
	if (GetFileAttributesA(flag) == INVALID_FILE_ATTRIBUTES)
		return;
	DeleteFileA(flag);
	const uint8_t *base = (const uint8_t *)GetModuleHandleW(NULL);
	const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
	DWORD size = nt->OptionalHeader.SizeOfImage;
	HANDLE f = CreateFileA(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		log_printf("main32: cannot write %s", out);
		return;
	}
	static uint8_t zeros[4096];
	DWORD readable = 0, wrote;
	for (DWORD at = 0; at < size; at += 4096) {
		MEMORY_BASIC_INFORMATION mbi;
		const uint8_t *page = base + at;
		if (VirtualQuery(page, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
			WriteFile(f, page, 4096, &wrote, NULL);
			readable += 4096;
		} else {
			WriteFile(f, zeros, 4096, &wrote, NULL);
		}
	}
	CloseHandle(f);
	log_printf("main32: image of %lu bytes written to %s (%lu bytes readable)", (unsigned long)size, out, (unsigned long)readable);
}

// the modules of the mod, in the order they have always been installed
static void install_all(void)
{
	game_init();
	crash_install();
	addr_scratch_check("after crash_install");
	log_printf("main32: settings port %u", (unsigned)settings_port());
	scripts_install();
	addr_scratch_check("after scripts_install");
	mirror_install();
	addr_scratch_check("after mirror_install");
	transplant_install();
	addr_scratch_check("after transplant_install");
	localize_install();
	addr_scratch_check("after localize_install");
	zones_install();
	addr_scratch_check("after zones_install");
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
	window_install();
	addr_scratch_check("after window_install");
	native_install(); // (iw4x-sp: the official addresses go through XR(), src/addr159.c)
	addr_scratch_check("after native_install");
	guards_install(); // (iw4x-sp only: see guards.c)
	addr_scratch_check("after guards_install");
	climbads_install(); // (not in the iw4x-sp profile)
	addr_scratch_check("after climbads_install");
	rumble_install();
	addr_scratch_check("after rumble_install");
	devices_install();
	addr_scratch_check("after devices_install");
	console_install();
	addr_scratch_check("after console_install");
	control_start();
	addr_scratch_check("after control_start");
}

static DWORD WINAPI identify(LPVOID unused)
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
		log_printf("main32: no game window after 120 s");
		return 0;
	}
	log_printf("main32: game window %p after %lu ms", (void *)game, GetTickCount() - start);
	Sleep(3000); // the wrapper has unpacked the code by now; a few seconds more for the game's own start
	dump_image();
	char hex[65] = "";
	unsigned bytes = 0;
	if (!sha256_code(hex, &bytes)) {
		log_printf("main32: could not hash the code of iw4sp.exe");
		return 0;
	}
	log_printf("main32: image base %p, code sections %u bytes, SHA-256 %s", (void *)GetModuleHandleW(NULL), bytes, hex);
	int forced_unknown = getenv("MW2COOP_FORCE_UNKNOWN") != NULL; // QA: take the unknown build path on the known build
	if (game_is_iw4x()) { // the iw4x-sp client: its 1.0.159 exe is known by its file's hash (the client patches the code in memory)
		if (!forced_unknown && iw4x_known(hex)) {
			log_printf("main32: known build: 1.0.159 iw4sp.exe of the iw4x-sp client");
			addr_use_159();
			install_all();
			iw4x_game_console_off();
			iw4xtext_install();
			return 0;
		}
	}
	for (size_t i = 0; !game_is_iw4x() && i < sizeof(builds) / sizeof(builds[0]); i++) {
		if (!forced_unknown && strcmp(hex, builds[i].code_sha256) == 0) {
			log_printf("main32: known build: %s", builds[i].name);
			install_all();
			return 0;
		}
	}
	int total = 0, found = 0;
	{ // look the addresses of this build up by their signatures (addr.c); nothing is patched, the log says what was found
		found = addr_resolve_running(&total);
		log_printf("main32: addresses found by signature: %d of %d", found, total);
		if (addr_layout_same() && !forced_unknown) {
			log_printf("main32: unknown build with the layout of the known one (same addresses, some code bytes differ): the mod runs with the known addresses");
			install_all();
			return 0;
		}
	}
	// a build whose addresses moved: best effort, only when the settings ask for it ([native] movedbuilds=1, or MW2COOP_MOVED for QA) and most addresses were found. Every
	// address goes through addr_xr(); one that was not found points at a harmless block (addr.c), so the features that need it do nothing and nothing else is touched.
	{
		char ini[MAX_PATH];
		DWORD n = GetModuleFileNameA(NULL, ini, MAX_PATH);
		while (n > 0 && ini[n - 1] != 0x5c)
			n--;
		ini[n] = 0;
		lstrcatA(ini, "mw2coop\\settings.ini");
		const char *why = "";
		if (addr_moved_allowed(found, total, ini, &why)) {
			log_printf("main32: unknown build, addresses moved: %d of %d found; the mod runs with them (%s)", found, total, why);
			install_all();
			return 0;
		}
		if (total > 0 && found * 100 >= total * 85) {
			log_printf("main32: unknown build, addresses moved: %d of %d found. Nothing is patched (%s)", found, total, why);
			return 0;
		}
	}
	log_printf("main32: unknown build: nothing is patched. The mod supports the unmodified official Steam 32-bit build (code SHA-256 above must be FC9E4843...); a copy with a different iw4sp.exe (another edition, or a modified exe) is not supported. In Steam: Properties > Installed Files > Verify integrity of game files restores the original");
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
	(void)reserved;
	if (reason != DLL_PROCESS_ATTACH)
		return TRUE;
	DisableThreadLibraryCalls(self);
	if (!proxy_host_is_game()) // not the single-player game (the multiplayer iw4mp.exe, IW4x's iw4x.exe, a tool): only the system's winmm calls passed through (proxy.c)
		return proxy_init() ? TRUE : FALSE;
	log_open();
	console_com_maxfps_capture_config(); // snapshot before the engine can rewrite config.cfg
	settings_load(); // before the game registers net_port
	SYSTEMTIME day;
	GetLocalTime(&day);
	log_printf("main32: %04d-%02d-%02d, 32-bit proxy attached, pid %lu", day.wYear, day.wMonth, day.wDay,
		   GetCurrentProcessId());
	detect_iw4x_process();
	d3dcap_install(); // the exe's imports are loaded and its code has not run: the game has not created its device yet
	if (!proxy_init()) {
		log_printf("main32: could not load the real winmm.dll");
		return FALSE;
	}
	CloseHandle(CreateThread(NULL, 0, identify, NULL, 0, NULL));
	return TRUE;
}
