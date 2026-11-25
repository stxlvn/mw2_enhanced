// Network co-op watch (Vlad, 2026-11-19: "a friend hosted; there were moments when on his screen I was already dead while I played on through the level on mine,
// and the other way round. An on-screen notice when the other player shows no sign of life for more than 3 seconds would help: now there is a 12 s answer check
// only when the lobby gathers, none during the game itself").
//
// The network co-op is the game's own lockstep (docs/native-splitscreen-research/coop-host-frame.md): the host builds a frame of both players' usercmds every
// server tick and both PCs execute the same frames. Nothing corrects a divergence: once the two simulations differ (one PC's player dies, the other's does not),
// each PC goes on with its own game. Two checks, both shown in the game's own message window (top left: CG_GameMessage, this PC only; nothing of the lockstep game
// changes, so the check itself cannot make the PCs differ) and written to the log:
//
//  1. Silence: no packet from the partner for 3 s while a level runs here (the client state 6 or more); the time is said again every 5 s, and once more when his
//     packets come back.
//  2. Divergence: the game keeps a running state hash on both PCs (64: rva 0x192d7e4, 32: VA 0x1a7f0e0): in every server frame each player and each actor adds
//     its number, health, position and angles (G_RunFrameForEntity, 64: rva 0x174920). The host writes its hash into the header of every lockstep frame
//     (64: rva 0x22f830; serverId, hash, frame number, validate byte), and the partner's game reads it and ignores it. Here the partner reads it when it starts
//     executing a frame (the frame executor, 64: rva 0x2302d0, 32: VA 0x40c4f0) and takes the difference to its own hash: while both PCs play the same game, both
//     hashes take the same additions and the difference stays the same (whatever it started at: a restart, a loaded checkpoint). The frame where it changes is
//     the first that differed: logged, told to the host (packet mw2coop_desync), shown on both PCs. When the difference holds still again for 10 s (a checkpoint
//     restart brought the games together), that is said too.
//
// [netwatch] in settings.ini is not needed: the checks are on in every network co-op session; [native] netwatch=0 turns both off, netwatchlog=1 logs every
// frame's hashes (QA).
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "hook.h"
#include "localize.h"
#include "log.h"
#include "net.h"
#include "netwatch.h"
#include "saves.h"
#include "settings.h"

static int cfg(const char *key, int def) // [native] in settings.ini, as native.c reads it
{
	return (int)GetPrivateProfileIntA("native", key, def, settings_ini_path());
}

#ifdef _WIN64
#define NW_EXEC XR(0x2302d0)          // void (int serverId, int time): execute the lockstep frame stamped `time`
#define NW_RING XR(0x192d380)         // the frame ring: 20 entries {int frameNo; int serverId; msg_t msg (data pointer at +8)}
#define NW_RING_STRIDE 0x38
#define NW_HASH XR(0x192d7e4)         // int: the running state hash
#define NW_IS_HOST XR(0x1905302)      // uint8: this PC is the host of the lockstep session
#define NW_GAME_MESSAGE XR(0xa6400)   // void CG_GameMessage(int localClientNum, const char *text, int flags): channel 2 (gamenotify), message window 0
#define NW_CLS_STATE XR(0x533cb8)     // int: the client's connection state; 6 and up: a level runs
#define NW_PUMP XR(0x2308a0)          // void (void): the UDP-only co-op packet dispatcher of the engine's waiting loops (0x22e7a0, 0x22e920): the host's frames go into the ring
#define NW_PUMP_RESOLVED() addr_resolved(0x2308a0)
static const uint8_t exec_bytes[] = {0x89, 0x54, 0x24, 0x10, 0x53, 0x41, 0x57, 0x48, 0x81, 0xec, 0x88, 0x08, 0x00, 0x00}; // mov [rsp+10],edx; push rbx; push r15; sub rsp,0x888
#else
#define NW_EXEC XR(0xc4f0)            // VA 0x40c4f0, cdecl
#define NW_RING (XV(0x1a6ad00) - 0x400000u)
#define NW_RING_STRIDE 0x30
#define NW_HASH (XV(0x1a7f0e0) - 0x400000u)
#define NW_IS_HOST (XV(0x1a7f0d9) - 0x400000u)
#define NW_GAME_MESSAGE XR(0x87010)   // VA 0x487010, cdecl (localClientNum, text, flags)
#define NW_CLS_STATE XR(0x525bc0)
#define NW_PUMP XR(0xacca0)           // VA 0x4acca0, cdecl (void): the same dispatcher (the partner's wait 0x49d360 calls it first)
#define NW_PUMP_RESOLVED() addr_resolved(0xacca0)
static const uint8_t exec_bytes[] = {0x51, 0x53, 0x8b, 0x5c, 0x24, 0x0c, 0x55, 0x8b, 0x6c, 0x24, 0x14}; // push ecx; push ebx; mov ebx,[esp+0xc]; push ebp; mov ebp,[esp+0x14]
#endif
#define NW_RING_ENTRIES 20
static uint8_t *base; // the game's image (netwatch_install)

static int enabled = -1;
static volatile LONG last_packet;	  // GetTickCount of the partner's last packet (net.c)
static volatile LONG last_exec;		  // GetTickCount of the last lockstep frame executed here
static volatile LONG pending;		  // the notice the main thread is to show (NOTE_*)
static volatile LONG pending_secs;	  // its number of seconds
static volatile LONG partner_desync_time; // the host: the level time the client reported a divergence at (packet), 0 none
static volatile LONG desync_since;	  // the client: the level time of the first differing frame (0: they agree)
static volatile LONG desync_state;	  // the client: 1 found (the host is told), 2 the host loads the common checkpoint, 3 it cannot
static volatile LONG resync_wait;	  // the client: a checkpoint load for this is on its way: say so when the hashes agree after it
static volatile LONG partner_insync;	  // the host: the client says the games agree again
static volatile LONG desync_sid;	  // the client: the lockstep frames' serverId of the level where it found the difference (sent with the report)
static volatile LONG exec_sid;		  // the serverId of the last lockstep frame executed here
static volatile LONG switching;		  // a restart or a load runs here (netwatch_level_switch) and no frame of the level after it has been executed yet
static volatile LONG switch_sid, switch_time; // the last frame's serverId and level time when it began
static volatile LONG level_gen;		  // the number of level switches
enum { NOTE_NONE, NOTE_SILENT, NOTE_BACK, NOTE_DESYNC, NOTE_INSYNC, NOTE_RESYNC, NOTE_RESYNCFAIL, NOTE_RESYNCED };

// --- what went into the hash (2026-11-21, issue 27; 64 bits first, the 32-bit build and iw4x-sp the same day) ---
// The hash takes, between its on and off functions (64: 0x22dec0 / 0x22ded0): for each player, vehicle and actor (G_RunFrameForEntity, 64: 0x174920, 32: VA 0x5bfdb0)
// its type, number and health (the int adder: 64 0x22dee0 at 0x174a82, 0x174a98, 0x174ab0; 32 VA 0x47af10 at 0x5bfe44, 0x5bfe58, 0x5bfe6e) and its position and angles
// (the vector adder: 64 0x22dfb0 at 0x174ac9, 0x174ae2; 32 VA 0x422950 at 0x5bfe84, 0x5bfe9a); four values of the level step (64: 0x171370 at 0x1714d2, 0x171607,
// 0x17161d, 0x171633; 32: VA 0x4bf8b0 at 0x4bfa01, 0x4bfb4d, 0x4bfb61, 0x4bfb75) and strings (the string adder: 64 0x22df10 at 0x190ee9 and the script's GetDvar
// 0x231026, 0x23103b; 32 VA 0x42ecc0 at GetDvar's 0x4e4cd7, 0x4e4ce7). All are (value, file, line); on 32 bits cdecl, the caller pops. Each addition is kept in a ring
// with the level time of the lockstep frame executed last; when the games go apart, both PCs write the additions of the step before the first differing frame to the
// log (the client when it finds it, the host when the client tells it): the two logs side by side name the entity and the value. This diagnostic recorder is off by
// default; [native] netwatchrec=1 enables its twelve call-site hooks and per-addition ring writes for a desync investigation (GetDvar's two are
// hooked in any case: they keep the request number out of the hash, nw_private_dvar).
// The byte that says the hash takes additions (64: rva 0x192d8f9, 32: VA 0x1a93324) is read where the int adder reads it: it begins with `cmp byte [addr], 0` (80 3d,
// 64: rip+disp32, 32: an absolute address), so the resolver needs no signature for it. When the adder does not begin so, every call is recorded.
#ifdef _WIN64
#define NW_ADD_INT XR(0x22dee0)
#define NW_ADD_VEC XR(0x22dfb0)
#define NW_ADD_STR XR(0x22df10)
#else
#define NW_ADD_INT XR(0x7af10)
#define NW_ADD_VEC XR(0x22950)
#define NW_ADD_STR XR(0x2ecc0)
#endif
static const volatile uint8_t *nw_accum;
typedef struct {
	int time;
	uint32_t site; // the return address's rva: which addition
	int v[3];
	char s[16];
} nw_rec_t;
#define NW_REC 65536
static nw_rec_t *nw_rec;
static unsigned nw_rec_n;
static int nw_cur_time, nw_dumps;
static uint32_t nw_ret_type, nw_ret_str[3]; // the return addresses (rva) of the entity block's first addition and of the string additions
static void nw_record(uintptr_t ret, const int *v, int n, const char *s)
{
	if (!nw_rec || (nw_accum && !*nw_accum))
		return;
	nw_rec_t *r = &nw_rec[nw_rec_n++ % NW_REC];
	r->time = nw_cur_time;
	r->site = (uint32_t)(ret - (uintptr_t)base);
	r->v[0] = n > 0 ? v[0] : 0, r->v[1] = n > 1 ? v[1] : 0, r->v[2] = n > 2 ? v[2] : 0;
	r->s[0] = 0;
	if (s)
		lstrcpynA(r->s, s, sizeof(r->s));
}
static void (*nw_add_int_original)(int, const char *, int);
static void nw_add_int(int v, const char *file, int line)
{
	nw_record((uintptr_t)__builtin_return_address(0), &v, 1, NULL);
	nw_add_int_original(v, file, line);
}
static void (*nw_add_vec_original)(const int *, const char *, int);
static void nw_add_vec(const int *v, const char *file, int line)
{
	nw_record((uintptr_t)__builtin_return_address(0), v, 3, NULL);
	nw_add_vec_original(v, file, line);
}
// The script's GetDvar adds the dvar's name, then its value (two calls in a row, no branch between: 64 0x231026 / 0x23103b, 32 VA 0x4e4cd7 / 0x4e4ce7). A dvar that
// only carries the scripts' messages to this DLL and may hold another value on the other PC is left out of the hash, name and value: mw2coop_cmd_n, the requests'
// number (maps\_mw2coop::send_requests reads it to number the next request). It counts on each PC by itself, and a PC that never executed a frame with a request
// (the host carried out a "restart" before the client got to that frame) kept a number one lower for the rest of the process: from then on every checkpoint or
// restart request was a "divergence" (only the number's string differed), and the resync loaded the checkpoint again and again (a friend's session, 2026-11-25:
// Cliffhanger, 32 times, the airport's level change broke the link). The number decides nothing in the game.
static uint32_t nw_getdvar_name_ret, nw_getdvar_value_ret; // the return addresses (rva) of GetDvar's two additions
static int nw_skip_value;							   // the next value addition is left out (the main thread runs the scripts)
static int nw_private_dvar(const char *s)
{
	return s && strcmp(s, "mw2coop_cmd_n") == 0;
}
static void (*nw_add_str_original)(const char *, const char *, int);
static void nw_add_str(const char *s, const char *file, int line)
{
	const uint32_t ret = (uint32_t)((uintptr_t)__builtin_return_address(0) - (uintptr_t)base);
	if (ret == nw_getdvar_value_ret && nw_skip_value) {
		nw_skip_value = 0;
		return;
	}
	nw_skip_value = 0;
	if (ret == nw_getdvar_name_ret && nw_private_dvar(s)) {
		nw_skip_value = 1;
		return;
	}
	nw_record((uintptr_t)__builtin_return_address(0), NULL, 0, s);
	nw_add_str_original(s, file, line);
}
static void nw_rec_install(void)
{
	const int rec = cfg("netwatchrec", 0);
#ifdef _WIN64
	uint8_t *const ints[] = {base + XR(0x1714d2), base + XR(0x171607), base + XR(0x17161d), base + XR(0x171633), base + XR(0x174a82), base + XR(0x174a98), base + XR(0x174ab0)};
	uint8_t *const vecs[] = {base + XR(0x174ac9), base + XR(0x174ae2)};
	uint8_t *const strs[] = {base + XR(0x190ee9), base + XR(0x231026), base + XR(0x23103b)};
#else
	uint8_t *const ints[] = {base + XR(0xbfa01), base + XR(0xbfb4d), base + XR(0xbfb61), base + XR(0xbfb75), base + XR(0x1bfe44), base + XR(0x1bfe58), base + XR(0x1bfe6e)};
	uint8_t *const vecs[] = {base + XR(0x1bfe84), base + XR(0x1bfe9a)};
	uint8_t *const strs[] = {base + XR(0xe4cd7), base + XR(0xe4ce7)};
#endif
	const int nstrs = (int)(sizeof(strs) / sizeof(strs[0]));
	const int getdvar = nstrs - 2; // GetDvar's two string additions are the last two (the private dvars are left out of the hash also with netwatchrec=0)
	int ok = 0, want = 0, getdvar_ok = 0;
	for (size_t i = 0; rec && i < sizeof(ints) / sizeof(ints[0]); i++, want++)
		ok += ints[i][0] == 0xe8 && *(const int32_t *)(ints[i] + 1) + (intptr_t)(ints[i] + 5) == (intptr_t)(base + NW_ADD_INT) && hook_call(ints[i], (void *)nw_add_int, (void **)&nw_add_int_original);
	for (size_t i = 0; rec && i < sizeof(vecs) / sizeof(vecs[0]); i++, want++)
		ok += vecs[i][0] == 0xe8 && *(const int32_t *)(vecs[i] + 1) + (intptr_t)(vecs[i] + 5) == (intptr_t)(base + NW_ADD_VEC) && hook_call(vecs[i], (void *)nw_add_vec, (void **)&nw_add_vec_original);
	for (int i = rec ? 0 : getdvar; i < nstrs; i++, want++) {
		const int hooked = strs[i][0] == 0xe8 && *(const int32_t *)(strs[i] + 1) + (intptr_t)(strs[i] + 5) == (intptr_t)(base + NW_ADD_STR) && hook_call(strs[i], (void *)nw_add_str, (void **)&nw_add_str_original);
		ok += hooked;
		getdvar_ok += i >= getdvar && hooked;
	}
	if (getdvar_ok == 2) {
		nw_getdvar_name_ret = (uint32_t)(strs[getdvar] + 5 - base);
		nw_getdvar_value_ret = (uint32_t)(strs[getdvar + 1] + 5 - base);
	}
	log_printf("netwatch: the request number mw2coop_cmd_n %s", getdvar_ok == 2 ? "is left out of the hash" : "is NOT left out of the hash (GetDvar's additions not found)");
	if (!rec) {
		log_printf("netwatch: detailed hash-addition recording is off ([native] netwatchrec=1 enables it)");
		return;
	}
	nw_ret_type = (uint32_t)(ints[4] + 5 - base);
	for (int i = 0; i < nstrs; i++)
		nw_ret_str[i] = (uint32_t)(strs[i] + 5 - base);
	const uint8_t *adder = base + NW_ADD_INT; // cmp byte [addr], 0: 80 3d <4 bytes> 00
	if (adder[0] == 0x80 && adder[1] == 0x3d && adder[6] == 0x00)
#ifdef _WIN64
		nw_accum = adder + 7 + *(const int32_t *)(adder + 2); // (rip-relative)
#else
		nw_accum = (const uint8_t *)(uintptr_t) * (const uint32_t *)(adder + 2); // (absolute)
#endif
	if (ok == want)
		nw_rec = (nw_rec_t *)calloc(NW_REC, sizeof(nw_rec_t));
	log_printf("netwatch: the hash's additions are %s (%d of %d calls hooked; the hash's on-byte %s)", nw_rec ? "recorded for a divergence" : "NOT recorded", ok, want,
		   nw_accum ? "read from the adder's code" : "not found: every call is recorded");
}
// The additions of the steps whose frames were executed at level times from..to, one line per entity (type, number, health, position, angles as the hash took
// them, raw and as numbers) and one per other addition. At most four dumps a level.
static void nw_dump(int from, int to, const char *who)
{
	if (!nw_rec || nw_dumps >= 4)
		return;
	nw_dumps++;
	log_printf("netwatch: %s: the hash's additions of the steps after the frames %d..%d (compare this block with the other PC's log):", who, from, to);
	const unsigned n = nw_rec_n, first = n > NW_REC ? n - NW_REC : 0;
	int lines = 0;
	char line[400];
	for (unsigned i = first; i < n && lines < 3000; i++) {
		const nw_rec_t *r = &nw_rec[i % NW_REC];
		if (r->time < from || r->time > to)
			continue;
		if (r->site == nw_ret_type && i + 4 < n) { // an entity's block: type (0x174a82), number, health, position, angles
			const nw_rec_t *e = &nw_rec[(i + 1) % NW_REC], *h = &nw_rec[(i + 2) % NW_REC], *o = &nw_rec[(i + 3) % NW_REC], *a = &nw_rec[(i + 4) % NW_REC];
			float of[3], af[3];
			memcpy(of, o->v, sizeof(of));
			memcpy(af, a->v, sizeof(af));
			snprintf(line, sizeof(line), "netwatch: add t %d ent %d type %d health %d pos %08x %08x %08x (%.3f %.3f %.3f) angles %08x %08x %08x (%.3f %.3f %.3f)", r->time, e->v[0], r->v[0], h->v[0],
				 (unsigned)o->v[0], (unsigned)o->v[1], (unsigned)o->v[2], (double)of[0], (double)of[1], (double)of[2], (unsigned)a->v[0], (unsigned)a->v[1], (unsigned)a->v[2],
				 (double)af[0], (double)af[1], (double)af[2]);
			i += 4;
		} else if (r->s[0] || r->site == nw_ret_str[0] || r->site == nw_ret_str[1] || r->site == nw_ret_str[2])
			snprintf(line, sizeof(line), "netwatch: add t %d at %x string \"%s\"", r->time, r->site, r->s);
		else
			snprintf(line, sizeof(line), "netwatch: add t %d at %x value %d %d %d", r->time, r->site, r->v[0], r->v[1], r->v[2]);
		log_printf("%s", line);
		lines++;
	}
	log_printf("netwatch: %s: %d lines", who, lines);
}

// --- divergence (the frame executor's hook: main thread, every lockstep frame) ---
static void (*exec_original)(int, int);
static struct {
	int server_id, last_time, last_frame, have_diff, diff, frames, changes, said;
	DWORD steady_since;
	unsigned long long matched;
} dv;

static void diverged(int time, int was, int now, int host_hash, int ours)
{
	dv.changes++;
	if (dv.changes <= 20 || dv.changes % 200 == 0)
		log_printf("netwatch: the games differ from level time %d on: the host's state hash %08x, ours %08x (the difference was %08x, now %08x; %llu frames agreed before, change %d)", time,
			   (unsigned)host_hash, (unsigned)ours, (unsigned)was, (unsigned)now, dv.matched, dv.changes);
	dv.matched = 0;
	dv.steady_since = GetTickCount();
	nw_dump(dv.last_frame - 50, dv.last_frame, "this PC (the client, it found the difference)"); // the step after the frame before: its additions made the difference
	if (!dv.said) {
		dv.said = 1;
		InterlockedExchange(&desync_sid, dv.server_id);
		InterlockedExchange(&desync_since, time ? time : 1); // netwatch_poll shows it and tells the host, who loads the common checkpoint (saves_resync)
		InterlockedExchange(&desync_state, 1);
	}
}

static int ring_has(int server_id, int time)
{
	const uint8_t *e = base + NW_RING;
	for (int i = 0; i < NW_RING_ENTRIES; i++, e += NW_RING_STRIDE)
		if (*(const int *)e == time && *(const int *)(e + 4) == server_id)
			return 1;
	return 0;
}

// The partner's first frame after a checkpoint load (Favela, network tour 2026-11-23): the partner runs the frame executor for its own level time each frame
// (64: 0x15c9e0 -> 0x2302d0, 32: 0x5b3790 -> 0x40c4f0) and its step loop waits for the host's frame (0x22e120 / 0x22e7a0 look at the ring), but the
// first frame after the load runs at once: a client that loaded faster than the host asked for a frame the host had not sent yet, and the engine ended the
// session ("Unable to find player data for server time 350, serverId 19"; the 9 loads where the host was first were fine). NOT REPRODUCED that way: with the
// host held 400 ms after its load (dvar mw2coop_qa_loaddelay), after a resync load and after a real failed mission, the client waited for the host's frame by
// itself. So this is a guard, and the ring is logged when the frame is missing, for the next case: the frame is waited for here (the packets are taken in
// until it is in the ring, at most 5 s; the engine's own error follows after that).
static void wait_for_frame(int server_id, int time)
{
	if (base[NW_IS_HOST] || ring_has(server_id, time))
		return;
	char ring[NW_RING_ENTRIES * 24] = "";
	size_t n = 0;
	const uint8_t *e = base + NW_RING;
	for (int i = 0; i < NW_RING_ENTRIES && n + 24 < sizeof(ring); i++, e += NW_RING_STRIDE)
		if (*(const int *)e)
			n += (size_t)snprintf(ring + n, sizeof(ring) - n, " %d/%d", *(const int *)e, *(const int *)(e + 4));
	log_printf("netwatch: the frame %d (serverId %d) this partner runs is not in its ring (time/serverId:%s)", time, server_id, n ? ring : " empty");
	static int pump_ok = -1;
	if (pump_ok < 0) // (QA: MW2COOP_QA_NOFRAMEWAIT=1 leaves the wait out, to see the engine's error again)
		pump_ok = NW_PUMP_RESOLVED() && !GetEnvironmentVariableA("MW2COOP_QA_NOFRAMEWAIT", NULL, 0);
	if (!pump_ok)
		return;
	const DWORD t0 = GetTickCount();
	int got = 0;
	while (GetTickCount() - t0 < 5000) {
		((void (*)(void))(base + NW_PUMP))();
		if (ring_has(server_id, time)) {
			got = 1;
			break;
		}
		Sleep(1);
	}
	log_printf("netwatch: the host's frame %d (serverId %d) was not here yet: %s after %lu ms", time, server_id, got ? "it came" : "still missing, the engine stops the session",
		   (unsigned long)(GetTickCount() - t0));
}

static void exec_hook(int server_id, int time)
{
	wait_for_frame(server_id, time);
	InterlockedExchange(&last_exec, (LONG)GetTickCount());
	static int rec_last;
	if (time < rec_last)
		nw_dumps = 0; // (a new level or a load)
	rec_last = time;
	nw_cur_time = time; // the additions that follow belong to the step of this frame
	InterlockedExchange(&exec_sid, server_id);
	if (switching && (server_id != switch_sid || time < switch_time))
		InterlockedExchange(&switching, 0); // the first frame of the level after the restart or the load
	if (enabled && !base[NW_IS_HOST]) {
		if (server_id != dv.server_id || time < dv.last_time) { // a new session or a new level: begin again
			if (dv.frames)
				log_printf("netwatch: %d frames compared in the last level (%d changes of the hash difference)", dv.frames, dv.changes);
			memset(&dv, 0, sizeof(dv));
			dv.server_id = server_id;
			InterlockedExchange(&desync_since, 0);
			InterlockedExchange(&desync_sid, 0);
			InterlockedExchange(&desync_state, 0); // (a checkpoint load for it keeps resync_wait: the comparison after it says whether it worked)
		}
		dv.last_frame = dv.last_time;
		dv.last_time = time;
		const uint8_t *e = base + NW_RING;
		for (int i = 0; i < NW_RING_ENTRIES; i++, e += NW_RING_STRIDE) {
			if (*(const int *)e != time || *(const int *)(e + 4) != server_id)
				continue;
			const uint8_t *data = *(const uint8_t *const *)(e + 0x10);
			if (!data || *(const int *)data != server_id) // the header begins with the serverId: else the layout is not what this reads
				break;
			const int host_hash = *(const int *)(data + 4), ours = *(const int *)(base + NW_HASH);
			const int d = host_hash - ours;
			dv.frames++;
			static int frame_log = -1;
			if (frame_log < 0)
				frame_log = cfg("netwatchlog", 0);
			if (frame_log && dv.frames % 20 == 1)
				log_printf("netwatch: frame %d host %08x ours %08x difference %08x", time, (unsigned)host_hash, (unsigned)ours, (unsigned)d);
			if (!dv.have_diff) {
				dv.have_diff = 1, dv.diff = d, dv.steady_since = GetTickCount();
				log_printf("netwatch: comparing the host's state hash with ours from level time %d on (difference %08x)", time, (unsigned)d);
			} else if (d != dv.diff) {
				diverged(time, dv.diff, d, host_hash, ours);
				dv.diff = d;
			} else {
				dv.matched++;
				if (resync_wait && !dv.said && GetTickCount() - dv.steady_since > 3000) { // the checkpoint loaded for it: the hashes agree for 3 s
					InterlockedExchange(&resync_wait, 0);
					log_printf("netwatch: after the checkpoint load the games agree again (level time %d, %llu frames)", time, dv.matched);
					InterlockedExchange(&pending, NOTE_RESYNCED);
					net_send_mod("mw2coop_nw resynced");
				}
				if (dv.said && GetTickCount() - dv.steady_since > 10000) { // the same for 10 s again: together again (a checkpoint restart)
					dv.said = 0;
					InterlockedExchange(&desync_since, 0);
					log_printf("netwatch: the hashes agree again from level time %d on (%llu frames)", time, dv.matched);
					InterlockedExchange(&pending, NOTE_INSYNC);
				}
			}
			break;
		}
	}
	exec_original(server_id, time);
}

// --- silence (net.c and the control thread) ---
void netwatch_partner_packet(void)
{
	InterlockedExchange(&last_packet, (LONG)GetTickCount());
}

// A report about the level before a restart or a load is dropped: the restart brings both games back to one start anyway, and the checkpoint load it would ask for
// came on top of that restart as a second one (2026-11-21, network tour of Roadkill: the client found the games apart in the last frames before the tour's
// restart, its report reached the host 220 ms after that restart, and the host restarted the level again 1.7 s into it and crashed in the entity spawn).
void netwatch_on_desync_packet(int time, int sid)
{
	if (switching || (sid && exec_sid && sid != exec_sid)) {
		static DWORD said;
		if (GetTickCount() - said > 5000) // (three copies of each report, and the client repeats it every 15 s)
			log_printf("netwatch: the partner's report of a difference at level time %d is about the level before the restart or load here (%s): left alone", time,
				   switching ? "the next level is not running yet" : "another server id");
		said = GetTickCount();
		return;
	}
	log_printf("netwatch: the partner's PC says the games differ from level time %d on", time);
	nw_dump(time - 100, time - 50, "this PC (the host, told by the client)");
	InterlockedExchange(&partner_desync_time, time ? time : 1);
}

// "mw2coop_nw <word> [n]": the host could not bring the games together (fail), the client says they agree again (insync)
void netwatch_on_packet(const char *text)
{
	if (!strncmp(text, "fail", 4)) {
		static DWORD at;
		if (GetTickCount() - at > 5000) // (one a report)
			log_printf("netwatch: the host cannot load a common checkpoint for this");
		at = GetTickCount();
		if (InterlockedExchange(&desync_state, 3) != 3) { // the notice at once (then every 15 s with the report's repeats)
			InterlockedExchange(&pending_secs, desync_since / 1000);
			InterlockedExchange(&pending, NOTE_RESYNCFAIL);
		}
	} else if (!strncmp(text, "resynced", 8)) {
		log_printf("netwatch: the partner's PC says the games agree again after the checkpoint load");
		InterlockedExchange(&partner_insync, 2);
	} else if (!strncmp(text, "insync", 6)) {
		log_printf("netwatch: the partner's PC says the games agree again");
		InterlockedExchange(&partner_insync, 1);
	}
}

// The client: the host's mw2coop_resync came (saves.c): both PCs load the common checkpoint
void netwatch_resync_started(void)
{
	InterlockedExchange(&desync_state, 2);
	InterlockedExchange(&resync_wait, 1);
	InterlockedExchange(&pending_secs, desync_since / 1000);
	InterlockedExchange(&pending, NOTE_RESYNC);
}

static int level_runs(void)
{
	return *(volatile int *)(base + NW_CLS_STATE) >= 6;
}

// Every 250 ms (control.c): decides the notices; the main thread shows them (netnotice_f).
void netwatch_poll(void)
{
	if (enabled <= 0)
		return;
	static int silent_said, silent_secs;
	static DWORD silent_next;
	const DWORD now = GetTickCount(), pkt = (DWORD)last_packet, ex = (DWORD)last_exec;
	const int peer = net_partner_known();
	// silence: the level ran here when his packets stopped (this PC executed lockstep frames within 2 s of his last packet), and a level still runs here
	if (peer && pkt && level_runs() && now - pkt > 3000 && (int)(ex - pkt) > -2000) {
		const int secs = (int)((now - pkt) / 1000);
		if (!silent_said || (int)(now - silent_next) >= 0) {
			if (!silent_said)
				log_printf("netwatch: no packet from the partner for %d s while the level runs", secs);
			silent_said = 1, silent_secs = secs, silent_next = now + 5000;
			InterlockedExchange(&pending_secs, secs);
			InterlockedExchange(&pending, NOTE_SILENT);
		}
	} else if (silent_said && pkt && now - pkt < 1000) {
		log_printf("netwatch: the partner's packets are back after about %d s", silent_secs);
		InterlockedExchange(&pending_secs, silent_secs);
		InterlockedExchange(&pending, NOTE_BACK);
		silent_said = 0;
	} else if (silent_said && !level_runs())
		silent_said = 0; // (the level ended meanwhile: a load)
	// the host: the client found the games apart: both load the last checkpoint they hold in common (saves.c), or the players are told they cannot
	const LONG pd = InterlockedExchange(&partner_desync_time, 0);
	if (pd) {
		char why[200] = "";
		const int r = saves_resync(pd, why, sizeof(why));
		if (r != 0)
			InterlockedExchange(&pending_secs, pd / 1000);
		if (r > 0)
			InterlockedExchange(&pending, NOTE_RESYNC);
		else if (r < 0) {
			log_printf("netwatch: the games cannot be brought together: %s", why);
			InterlockedExchange(&pending, NOTE_RESYNCFAIL);
			net_send_mod("mw2coop_nw fail");
		}
	}
	const LONG pi = InterlockedExchange(&partner_insync, 0);
	if (pi)
		InterlockedExchange(&pending, pi == 2 ? NOTE_RESYNCED : NOTE_INSYNC);
	// the client: until the host has answered, the report again every 15 s; when it cannot, the notice again every 15 s (the game's message stays about 5 s)
	static DWORD desync_next;
	const LONG ds = desync_since, st = desync_state;
	if (ds && st != 2 && (!desync_next || (int)(now - desync_next) >= 0)) {
		desync_next = now + 15000;
		InterlockedExchange(&pending_secs, ds / 1000);
		InterlockedExchange(&pending, st == 3 ? NOTE_RESYNCFAIL : NOTE_DESYNC);
		if (st == 1) {
			char text[64];
			snprintf(text, sizeof(text), "mw2coop_desync %ld %ld", (long)ds, (long)desync_sid);
			net_send_mod(text);
		}
	} else if (!ds)
		desync_next = 0;
	if (pending != NOTE_NONE && level_runs())
		game_cbuf_addtext("mw2coop_netnotice\n");
}

// report.c: a line in the game's message window while a level runs (main thread); the log has it either way
void netwatch_say(const char *text)
{
	log_printf("netwatch: say: %s", text);
	if (base && level_runs())
		((void (*)(int, const char *, int))(base + NW_GAME_MESSAGE))(0, text, 0);
}

// --- the notice (console command, main thread) ---
static void notice_f(void)
{
	const LONG kind = InterlockedExchange(&pending, NOTE_NONE);
	if (kind == NOTE_NONE || !level_runs())
		return;
	const int secs = (int)pending_secs;
	const char *name = kind == NOTE_SILENT ? "MW2COOP_NET_SILENT_FMT" : kind == NOTE_BACK ? "MW2COOP_NET_BACK_FMT" : kind == NOTE_DESYNC ? "MW2COOP_NET_DESYNC_FMT" :
			   kind == NOTE_RESYNC ? "MW2COOP_NET_RESYNC_FMT" : kind == NOTE_RESYNCFAIL ? "MW2COOP_NET_RESYNCFAIL_FMT" : kind == NOTE_RESYNCED ? "MW2COOP_NET_RESYNCED" :
															       "MW2COOP_NET_INSYNC";
	const char *fmt = localize_text(name);
	char text[400];
	if (!fmt)
		fmt = name;
	if (kind == NOTE_INSYNC || kind == NOTE_RESYNCED)
		lstrcpynA(text, fmt, sizeof(text));
	else
		snprintf(text, sizeof(text), fmt, secs);
	((void (*)(int, const char *, int))(base + NW_GAME_MESSAGE))(0, text, 0); // (0: the whole line at once; 0x11, the game's "Game saved", types it out letter by letter)
	log_printf("netwatch: shown: %s", name);
}
// QA: mw2coop_netpause <ms> (this PC sends the partner nothing for that long: net.c asks netwatch_send_paused), mw2coop_netqa_desync (64 bits: player 1's
// health changes on this PC only, one of the values the state hash takes: the two games go apart on purpose)
static volatile LONG pause_until;
int netwatch_send_paused(void)
{
	const LONG u = pause_until;
	return u && (LONG)(GetTickCount() - (DWORD)u) < 0;
}
static void netpause_f(void)
{
	const int ms = atoi(game_cmd_argv(1));
	InterlockedExchange(&pause_until, (LONG)(GetTickCount() + (DWORD)(ms > 0 ? ms : 5000)));
	log_printf("netwatch: QA: nothing is sent to the partner for %d ms", ms > 0 ? ms : 5000);
}
static void netqa_desync_f(void)
{
#ifdef _WIN64
	int *health = (int *)(base + XR(0xd9e070) + 0x90); // gentity 0 (player 1), +0x90: the value G_RunFrameForEntity puts in the hash after the number and +0x7e
#else
	int *health = (int *)(base + XR(0xaa76b8) + 0x90); // (32 bits: the gentities at VA 0xea76b8, the same +0x90 the hash takes, VA 0x5bfe5d)
#endif
	log_printf("netwatch: QA: player 1's entity value at +0x90 %d -> %d on this PC only", *health, *health - 3);
	*health -= 3;
}
static void netqa_say_f(void) // QA: mw2coop_netqa_say <flags> <text name>: one of the notices at once, with those flags of CG_GameMessage
{
	const char *t = localize_text(game_cmd_argv(2));
	((void (*)(int, const char *, int))(base + NW_GAME_MESSAGE))(0, t ? t : game_cmd_argv(2), atoi(game_cmd_argv(1)));
}
static cmd_function_t commands[] = {
	{NULL, "mw2coop_netnotice", NULL, NULL, notice_f CMD_NODE_FLAG},
	{NULL, "mw2coop_netpause", NULL, NULL, netpause_f CMD_NODE_FLAG},
	{NULL, "mw2coop_netqa_desync", NULL, NULL, netqa_desync_f CMD_NODE_FLAG},
	{NULL, "mw2coop_netqa_say", NULL, NULL, netqa_say_f CMD_NODE_FLAG},
};

void netwatch_install(void)
{
	if (enabled >= 0)
		return;
	base = (uint8_t *)GetModuleHandleA(NULL);
	enabled = cfg("netwatch", 1) != 0;
	if (enabled)
		for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
			game_add_command(&commands[i]);
	// (hooked with the watch off too: the hook also makes the partner wait for the host's frame, wait_for_frame)
	if (!hook_function(base + NW_EXEC, exec_bytes, sizeof(exec_bytes), (void *)exec_hook, (void **)&exec_original)) {
		log_printf("netwatch: the lockstep frame executor is not as expected: no divergence check (the silence check stays), no wait for the host's frame");
		return;
	}
	if (!enabled) {
		log_printf("netwatch: off ([native] netwatch=0); the partner still waits for the host's frame after a load");
		return;
	}
	log_printf("netwatch: the partner's silence (3 s) and the games' divergence (the host's state hash in each frame) are watched");
	nw_rec_install();
}

// saves.c: a fast_restart or a map command runs here (any session): until the first frame of the level after it, a report of a difference is about the level before
void netwatch_level_switch(void)
{
	InterlockedExchange(&switch_sid, exec_sid);
	InterlockedExchange(&switch_time, nw_cur_time);
	InterlockedExchange(&switching, 1);
	InterlockedIncrement(&level_gen);
}
unsigned netwatch_level_gen(void)
{
	return (unsigned)level_gen;
}

int netwatch_this_pc_hosts(void)
{
	return base && base[NW_IS_HOST];
}
