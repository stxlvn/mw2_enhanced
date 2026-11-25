// Native split screen (docs/native-splitscreen-360.md): one game process, two local players, each player's view drawn in a renderer frame of his own in every engine
// frame, as the Xbox 360 does (mode 11). The cgame's draw-active-frame (rva 0xbca00, local client number first) builds the view and queues the 3D scene; player 2's
// draw runs in a second client context (his client statics swapped in, see ctx_swap_mask).
#define COBJMACROS
#include <windows.h>
#include <tlhelp32.h>
#include <math.h>
#include <d3d9.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "progress.h"
#include "dx/natboot.h"
#include "game.h"
#include "pads.h"
#include "hook.h"
#include "log.h"
#include "localize.h"
#include "menus_ui.h"
#include "scrdebug_stack.h"
#include "native.h"
#include "chat.h"
#include "ping.h"
#include "console.h"
#include "control.h"
#include "padopts.h"
#include "gfxopts.h"
#include "d3dcap.h"
#include "fsr3.h"
#include "settings.h"
#include "padglyphs.h"
#include "rawinput.h"
#include "weaponfov.h"
#include "inputlog.h"
#include "loadprof.h"
#include "rumble.h"
#include "shot.h"
#include "d3dcap.h"
#include "zones.h"

// Both builds compile this file; the 64-bit addresses below are used by the 64-bit build, the 32-bit ones are found in docs/32bit-native-map/ and come in piece by piece
// (NATIVE32_READY in native_install says what is switched on).
#ifdef _WIN64
#define RVA_CG_DRAW XR(0x0bca00)         // void (unsigned local_client, int, int): the cgame's frame; it calls the begin-frame below in the middle
#define RVA_BEGIN_FRAME XR(0x00c840)     // void (void): swaps the renderer's two frame data sets and resets their command buffers: once per frame
#define RVA_SUBWINDOW_DVAR XR(0x4116158) // dvar_t *, the value (4 floats) at +0x10

static const uint8_t cg_draw_bytes[] = {0x48, 0x8b, 0xc4, 0x55, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0xc0, 0x00, 0x00, 0x00};
static const uint8_t begin_frame_bytes[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20};
#define RVA_CG_TIME XR(0x422434) // cg.time
#else
// The 32-bit official build (docs/32bit-native-map/render.md): the cgame's DrawActiveFrame is cdecl (local client, int, int); its first five bytes (sub esp,0xc; push ebx; push ebp) are clean to copy.
// The begin-frame is not hooked at its start (its first instructions are a push run and a relative call) but at its call in the cgame draw.
#define RVA_CG_DRAW XR(0x0666c0)
#define RVA_BEGIN_FRAME_CALL XR(0x066831) // the call of the begin-frame (VA 0x50d800) inside the cgame draw
#define RVA_SUBWINDOW_DVAR XR(0x1b19f98)
#define RVA_CG_TIME XR(0x374c20)
static const uint8_t cg_draw_bytes[] = {0x83, 0xec, 0x0c, 0x53, 0x55};
#endif
#define RVA_CG_PS (RVA_CG_TIME + 0xc) // cg.predictedPlayerState: 0xc after cg.time in both builds (64: rva 0x422440)
#define RVA_CG_PS_ORIGIN (RVA_CG_TIME + 0x28) // the origin in cg.ps (the camera of the live context): 0x1c into the playerState, which starts 0xc after cg.time, in both builds
static void (*cg_draw_original)(unsigned, int, int);
static void (*begin_frame_original)(void);
static uint8_t *base;
#ifdef _WIN64
#define CTX_IP(c) ((uintptr_t)(c).Rip)
#define CTX_SP(c) ((uintptr_t)(c).Rsp)
#else
#define CTX_IP(c) ((uintptr_t)(c).Eip)
#define CTX_SP(c) ((uintptr_t)(c).Esp)
#endif
#ifdef _WIN64
#define RVA_LOCAL_PLAYERS_STORE XR(0x1714a6)
#define RVA_SV_ADD_COMMAND XR(0x232c30)
#define RVA_COM_ERROR XR(0x1fb5f0) // void (int code, const char *format, ...)
#define RVA_SV_CLIENTS XR(0x19ada98)
#define SV_CLIENT_STRIDE 0x1678
#define RVA_COOP_BYTE XR(0x52fb5c)
#define RVA_LOCAL_PLAYERS XR(0x3c3eb0)
#define RVA_COOP_SLOTS XR(0x540bb0) // slot p: type int at +p*0x2650
#define SLOT_STRIDE 0x2650
#define RVA_SV_CLIENT_THINK XR(0x22d000) // void (int lc, const usercmd_t *): copies the cmd into client_t[lc] + 0x1630 and runs the player's think
#define RVA_CLIENT_COMMAND XR(0x166710) // void (int client, const char *text): the server's ClientCommand (thunk 0x22d150); "n <index>" wakes a NotifyOnPlayerCommand
#define CLC_SEQ 0   // the client's reliable command ring (rva 0x545844 for local client 0): the sequence queued
#define CLC_ACK 4   // the number handed to the server
#define CLC_TEXT 8  // the texts, 0x40 bytes each, indexed by number & 0x7f
#define RVA_KEY_CATCHERS XR(0x533cb0)
#define RVA_PM_FLAGS XR(0x535d44) // the live context's playerstate flags that CL_MouseMove (rva 0xcfcf0) tests with 0x800: set, it skips the whole mouse look and its view angle limits
#define RVA_CLS_STATE XR(0x533cb8)
#define RVA_ACC_PITCH XR(0x533d14) // the live view angle accumulators (pitch, then yaw)
#define ACC_IN_CLS (0x533d14 - 0x533cb8)
#define RVA_CG_CLIENTNUM XR(0x40caa0) // the cgame's copy of the client number; +4: its local client number
#define RVA_CL_PS XR(0x535d38) // the live cl block's playerstate
#define CL_RING_MASK 127
#define CL_CMDNUM 0x2000
#define CL_SENT 0x2004
#define CL_BLOCK_PS 0x2018
#define RVA_NOPREDICT_DVAR XR(0x51e028)
#define RVA_FOV_DVAR XR(0x51e1a8)
#define RVA_WAIT_TYPE XR(0x300a0)
#define RVA_CLIENT_INIT_A XR(0xb8830) // the per-client parts of the cgame init
#define RVA_CLIENT_INIT_B XR(0xb87b0)
#define RVA_COOP_DVAR XR(0x1905308)
#define MSG_T_SIZE 0x30
#define RVA_CENT XR(0x441070) // cg_entities
#define CENT_SIZE 0x1a8
#define CENT_TYPE 0xe0
#define CENT_FLAGS 0xec
#define CENT_VALID 0x18c
#define CF_YAW0 10 // float indices: yaw of the pose, of the current state, of the next state
#define CF_YAW1 42
#define CF_YAW2 73
#define CF_ORG0 6 // origin of the pose, of the current state, of the next state
#define CF_ORG1 32
#define CF_ORG2 63
#define P0_HDR 0x18 // the pose header a copy of the other player's entity keeps
#define P0_REST 0x30 // the rest of the pose
#define P0_REST_LEN 0x30
#define P0_TREE 0x170 // the event sequence and the event bytes (cgame check events, rva 0x9f840: +0x170 sequence, +0x174 events, +0x194 previous sequence); once taken for an animation tree reference
#define SLOT_END_PAST(q) ((q) > base + XR(0x545857))
#define RVA_AIM_GLOB XR(0x3cf180) // aaGlob
#define RVA_AIM_TARGET_GLOB XR(0x3d0530)
#define RVA_XFER_APPLY XR(0x29a210) // the animation transfer queue's apply loop (the entity cleanup function's first call)
#define RVA_LEVEL_NUM_ENT XR(0xd96e80)
#define RVA_GENTITIES XR(0xd9e070)
#define GENT_SIZE 0x2a0
#define GENT_SENTIENT 0x118
#define SENT_TEAM 8
#define GENT_HEALTH 0x164
#define GENT_SEEN 0x150
#define WD_AIM_RANGE 0x42c // the weapon def's aim assist ranges: aiming down the sight, auto aim
#define WD_AIM_RANGE_ADS 0x430
#define WD_AUTO_RANGE 0x428
#define WEAPON_DEF(w) (((const uint8_t *(*)(int64_t))(base + XR(0x85850)))(w))
#define RVA_RENDER_WIDTH XR(0x4113720) // the active render/backbuffer dimensions
#define RVA_RENDER_HEIGHT XR(0x4113724)
#define RVA_CG_ASPECT XR(0x43c640) // the cgame's aspect
#define RVA_WAIT_ALPHA_SITE XR(0x94039)
#define RVA_ENT_FLAGS XR(0x52c300) // the bit set of entities the snapshot transition flagged for the cleanup
#define RVA_CLIENT_DOBJ XR(0x1fece0) // DObj *(int entity): the client's DObj table
#define RVA_SERVER_DOBJ XR(0x1fee30)
#define RVA_ENT_INFO XR(0x234520) // the server's entity info of an entity
#define RVA_SDOBJ_INFO XR(0x2968e0) // the first field of a DObj (the tree)
#define RVA_DOBJ_CLRBIT XR(0x1fefa0)
#define DOBJ_NUMMODELS 0xd
#define DOBJ_MODELS 0xa8
#define P0GENERAL_RET(r) ((r) >= base + XR(0x9ea20) && (r) < base + XR(0x9ea20) + 1102)
#else
// the 32-bit official build: docs/32bit-native-map/clientctx.md
#define RVA_LOCAL_PLAYERS_STORE XR(0xbf9db)
#define RVA_SV_ADD_COMMAND XR(0xa550)
#define RVA_COM_ERROR XR(0x584f0)
#define RVA_SV_CLIENTS XR(0x1723988)
#define SV_CLIENT_STRIDE 0x1674
#define RVA_COOP_BYTE XR(0x48897c)
#define RVA_LOCAL_PLAYERS XR(0x338c6c)
#define RVA_COOP_SLOTS XR(0x48f560)
#define SLOT_STRIDE 0x263c
#define RVA_SV_CLIENT_THINK XR(0xe7750)
#define RVA_CLIENT_COMMAND XR(0x86920) // cdecl void (int client, const char *text): the server's ClientCommand (VA 0x486920, what VA 0x4c6f30 calls for each text of the ring)
#define CLC_SEQ 4   // the clc range (VA 0x88c330) starts 4 bytes before the ring: the sequence queued at VA 0x88c334
#define CLC_ACK 8   // the number handed to the server
#define CLC_TEXT 0xc // the texts, 0x40 bytes each, indexed by number & 0x7f
#define RVA_KEY_CATCHERS XR(0x525bb8)
#define RVA_CLS_STATE XR(0x525bc0)
#define RVA_ACC_PITCH XR(0x525c34)
#define ACC_IN_CLS (0x525c34 - 0x525bc0)
#define RVA_PM_FLAGS (RVA_CL_PS + 0xc) // the playerstate flags (VA 0x926c64): CL_MouseMove, VA 0x578fe0, skips the look when `& 0x800`
#define RVA_CG_CLIENTNUM XR(0x35f2a0)
#define RVA_CL_PS XR(0x526c58)
#define CL_RING_MASK 63
#define CL_CMDNUM 0x1000
#define CL_SENT 0x1004
#define CL_BLOCK_PS 0x1018
#define RVA_NOPREDICT_DVAR XR(0x45e8e8)
#define RVA_FOV_DVAR XR(0x45e968)
#define RVA_WAIT_TYPE XR(0x12ac60)
#define RVA_CLIENT_INIT_A XR(0x1dfa0)
#define RVA_CLIENT_INIT_B XR(0x956e0)
#define RVA_COOP_DVAR XR(0x167f0ec)
#define MSG_T_SIZE 0x28
#define RVA_CENT XR(0x392848)
#define CENT_SIZE 0x198
#define CENT_TYPE 0xd4
#define CENT_FLAGS 0xe0
#define CENT_VALID 0x180
#define CF_YAW0 9
#define CF_YAW1 39
#define CF_YAW2 70
#define CF_ORG0 5
#define CF_ORG1 29
#define CF_ORG2 60
#define P0_HDR 0x14
#define P0_REST 0x2c
#define P0_REST_LEN 0x2c
#define P0_TREE 0x164
#define SLOT_END_PAST(q) ((q) >= base + XR(0x4941e0))
#define RVA_AIM_GLOB XR(0x33ba90)
#define RVA_AIM_TARGET_GLOB XR(0x33cec0)
#define RVA_XFER_APPLY XR(0x5a8f0) // the animation transfer queue's apply loop (the entity cleanup function's first call)
#define RVA_LEVEL_NUM_ENT XR(0xca3c18)
#define RVA_GENTITIES XR(0xaa76b8)
#define GENT_SIZE 0x270
#define GENT_SENTIENT 0x110
#define SENT_TEAM 4
#define GENT_HEALTH 0x14c
#define GENT_SEEN 0x138
#define WD_AIM_RANGE 0x2e4
#define WD_AIM_RANGE_ADS 0x2e8
#define WD_AUTO_RANGE 0x2e0
#define WEAPON_DEF(w) (*(const uint8_t *const *)(base + XR(0x1669d08) + (size_t)(w) * 4)) // the table of weapon def pointers
#define RVA_RENDER_WIDTH XR(0x1c8ed9c) // the active render/backbuffer dimensions
#define RVA_RENDER_HEIGHT XR(0x1c8eda0)
#define RVA_CG_ASPECT XR(0x35f018)
#define RVA_WAIT_ALPHA_SITE 0x15ef17 // (a call site: hook_calls32 translates it)
#define RVA_ENT_FLAGS XR(0x46b280)
#define RVA_CLIENT_DOBJ XR(0xf3c70)
#define RVA_SERVER_DOBJ XR(0x95740)
#define RVA_ENT_INFO XR(0xdd30)
#define RVA_SDOBJ_INFO XR(0x3f4e0)
#define RVA_DOBJ_CLRBIT XR(0x10f50)
#define DOBJ_NUMMODELS 9
#define DOBJ_MODELS 0x98
// the add-by-type cases that add the "general" entity types: 0x561d00 (case 0), 0x561d60 (case 2), 0x5621d0 (case 5)
#define P0GENERAL_RET(r) (((r) >= base + XR(0x161d00) && (r) < base + XR(0x161d5a)) || ((r) >= base + XR(0x161d60) && (r) < base + XR(0x161e17)) || ((r) >= base + XR(0x1621d0) && (r) < base + XR(0x162294)))
#endif
// Mode 11 (both players drawn in every engine frame, each in his own renderer frame) in both builds. The 32-bit addresses: docs/32bit-native-map/mode11-frame.md,
// mode11-cgame.md, mode11-hud.md (2026-11-17, both decompiles compared).
#ifdef _WIN64
#define M11_SCR_BUSY XR(0x5db410)        // the screen update's busy flag (a set flag makes the client frame's screen update return at once)
#define M11_CL_SERVERTIME XR(0x51dca8)   // cl.serverTime (in the swapped cl range)
#define M11_CS_INDEX XR(0x5d8d02)        // the cgame's table of configstring indices (uint16), entries 0xc + 2 * lc / 0xd + 2 * lc: the popup friendly name
#define M11_VIEWRECT XR(0x3bd808)        // the renderer's per-player view rectangle, 4 floats (what 0xbd520 returns)
#define M11_CG_VIEWRECT XR(0x43c630)     // the cgame's view rectangle in pixels, 4 ints
#define M11_DISP_W XR(0x4113728)         // the picture's size (the renderer's display globals)
#define M11_DISP_H XR(0x411372c)
#define M11_FD_CUR XR(0x410fd00)         // the frame data the main thread fills
#define M11_RT_FRAMES XR(0x1429b5c)      // frames handed to the render thread and not finished
#define M11_RLC XR(0x51deb8)             // the renderer's local player
#define M11_LIST_END XR(0xc800)          // the frame's command list: end of the views' commands
#define M11_LIST_2DSTART XR(0xc7e0)      // ... the final 2D section's start
#define M11_LIST_2DFIRST XR(0xe900)      // ... its first command
#define M11_LIST_CLOSE XR(0xe440)        // ... the close
#define M11_END_FRAME XR(0xc4c0)         // void (unsigned flags): the end-frame
#define M11_MODEL_CLEAR XR(0x7b60)       // void (void): the scene's model records cleared
#define M11_RT_WAIT XR(0x301e0)          // void (int (*predicate)(void)): wait, helping the workers, until the predicate holds
#define M11_PLACE_SW XR(0x548bfc)        // the screen size the 2D placements are made for
#define M11_PLACE_SH XR(0x548c00)
#define M11_PLACE_CLIENT XR(0x5e0c60)    // the per-client 2D placement
#define M11_PLACE_SET XR(0xdb0b0)        // void (float *placement, float x, y, w, h, int screen w, int screen h)
#define M11_BE_FD XR(0x4358f58)          // the frame data the back end (render thread) is drawing
#define M11_CG_VIEWORG XR(0x42d310)      // the cgame's view: origin, then the axis (forward at +0xc): inside the cgame range of both builds
#define M11_FD_SCENES 0x41a34            // frame data: scene count
#define M11_FD_VIEWS 0x419e8             // frame data: view count
#else
#define M11_SCR_BUSY XR(0x531ee0)
#define M11_CL_SERVERTIME XR(0x46a5b4)
#define M11_CS_INDEX XR(0x52443a)
#define M11_VIEWRECT XR(0x32f3ec)
#define M11_CG_VIEWRECT XR(0x35f008)     // (not inside a swapped range in 32 bits: mode 11 sets it before each player's draw)
#define M11_DISP_W XR(0x188eda4)
#define M11_DISP_H XR(0x188eda8)
#define M11_FD_CUR XR(0x1893238)
#define M11_RT_FRAMES XR(0x1151b60)
#define M11_RLC XR(0x35e688)
#define M11_LIST_END XR(0x10d7c0)
#define M11_LIST_2DSTART XR(0x10d7a0)
#define M11_LIST_2DFIRST XR(0x10f1f0)
#define M11_LIST_CLOSE XR(0x10edc0)
#define M11_END_FRAME XR(0x10d630)
#define M11_MODEL_CLEAR XR(0x107410)
#define M11_PLACE_SW XR(0x494374)
#define M11_PLACE_SH XR(0x494378)
#define M11_PLACE_CLIENT XR(0x537500)
#define M11_PLACE_SET XR(0xa7320)        // (cdecl; it hands the placement to its helper in EBX itself)
#define M11_BE_FD XR(0x1e1e5b4)
#define M11_CG_VIEWORG XR(0x37fae8)
#define M11_FD_SCENES 0x419c4
#define M11_FD_VIEWS 0x41980
#endif
static char settings_path[MAX_PATH];
static int mode;
static int second_pass;

// The journal of the code this file patches: native_deactivate() puts every byte back, so a session that used the split screen can go on with the game's normal modes (the network
// co-op among them) untouched. Every patch goes through jhook_* or j_save().
static struct {
	uint8_t *at;
	uint8_t orig[32];
	size_t len;
	int group;
} journal[160];
static int njournal;
static int jgroup; // 1: the patches that make the game a two local players game (put back when the session ends), 0: the hooks that stay and do nothing while no session runs
static void j_save(uint8_t *at, size_t len)
{
	if (njournal >= (int)(sizeof(journal) / sizeof(journal[0])) || len > sizeof(journal[0].orig))
		return;
	for (int i = 0; i < njournal; i++)
		if (journal[i].at == at && journal[i].len >= len)
			return; // saved already (the first copy is the game's)
	journal[njournal].at = at, journal[njournal].len = len, journal[njournal].group = jgroup;
	memcpy(journal[njournal].orig, at, len);
	njournal++;
}
static void journal_restore(int group)
{
	int kept = 0;
	for (int i = njournal - 1; i >= 0; i--) {
		if (journal[i].group != group)
			continue;
		DWORD old;
		if (VirtualProtect(journal[i].at, journal[i].len, PAGE_EXECUTE_READWRITE, &old)) {
			memcpy(journal[i].at, journal[i].orig, journal[i].len);
			VirtualProtect(journal[i].at, journal[i].len, old, &old);
			FlushInstructionCache(GetCurrentProcess(), journal[i].at, journal[i].len);
		}
		journal[i].at = NULL;
	}
	for (int i = 0; i < njournal; i++)
		if (journal[i].at)
			journal[kept++] = journal[i];
	njournal = kept;
}
static int jhook_function(uint8_t *target, const uint8_t *expected, size_t len, void *replacement, void **original)
{
	j_save(target, len);
	return hook_function(target, expected, len, replacement, original);
}
static int jhook_call(uint8_t *site, void *replacement, void **original)
{
	j_save(site, 5);
	return hook_call(site, replacement, original);
}
static void jhook_jump(uint8_t *target, size_t len, const void *replacement, uint8_t filler)
{
	j_save(target, len);
	hook_jump(target, len, replacement, filler);
}

// The settings of the split screen: settings.ini [native] for the QA runs; a session started from the menu uses the values the feature ships with (from_menu) unless the file says otherwise
int native_active; // (read by the asm stub of the 32-bit view model hook)
#ifdef _WIN64
void *snd_ctx_ptr, *snd_count_orig; // (the sound counter's stub, see snd_count_stub)
volatile LONG snd_counts[2];
struct snd_name { const char *name; LONG n[2]; };
static struct snd_name snd_names[512];
static struct { uint32_t rva; LONG n[2]; } snd_callers[64]; // ([native] sndcount=3: the callers' return addresses too)
#endif
static int from_menu;
static int menu_pad = -1, menu_pad0 = -1; // the lobby's devices of player 2 and player 1 (-1: the keyboard and mouse)
static int cfg(const char *key, int def)
{
	static const struct {
		const char *key;
		int value;
	} shipped[] = {{"players", 2}, {"coopdvar", 1}, {"p1cmd", 1}, {"listplayers", 1}, {"p1yawdeg", 0}, {"p0turn", 0}, {"ctx", 1}};
	if (from_menu) {
		if (!lstrcmpA(key, "p1pad"))
			return menu_pad;
		if (!lstrcmpA(key, "p0pad"))
			return menu_pad0;
		for (size_t i = 0; i < sizeof(shipped) / sizeof(shipped[0]); i++)
			if (!lstrcmpA(key, shipped[i].key))
				def = shipped[i].value;
	}
	return (int)GetPrivateProfileIntA("native", key, def, settings_path);
}
// cfg() reads settings.ini from the disk on every call; the per-frame paths read a key once per session with CFG_ONCE (a session start bumps cfg_gen)
static unsigned cfg_gen = 1;
#define CFG_ONCE(key, def) __extension__({ static unsigned g_; static int v_; if (g_ != cfg_gen) { v_ = cfg(key, def); g_ = cfg_gen; } v_; })

static float *subwindow(void)
{
	uint8_t *dvar = *(uint8_t **)(base + RVA_SUBWINDOW_DVAR);
	return dvar ? (float *)(dvar + 0x10) : NULL;
}

#define RVA_D3D_DEVICE XR64(0x410fd20)
static volatile DWORD split_tick; // when the picture was last split

// Observe Bink playback and identify its output quad through D3D resource identity.
typedef struct {
	unsigned int width;
	unsigned int height;
} bink_public_header_t;
typedef void *(WINAPI *bink_open_t)(const char *, unsigned int);
typedef int (WINAPI *bink_do_frame_t)(void *);
typedef int (WINAPI *bink_copy_to_buffer_t)(void *, void *, int, unsigned int, unsigned int, unsigned int, unsigned int);
typedef void (WINAPI *bink_close_t)(void *);
static bink_open_t bink_open_original;
static bink_do_frame_t bink_do_frame_original;
static bink_copy_to_buffer_t bink_copy_to_buffer_original;
static bink_close_t bink_close_original;
static volatile LONG bink_open_handles;
static volatile LONG bink_d3d_active;

// Bink Open/Close publish this aligned flag with InterlockedExchange. The render hooks read it
// for every indexed draw (including ordinary gameplay), so use a relaxed atomic load rather than
// InterlockedCompareExchange, which emits a locked read-modify-write even though nothing changes.
static int bink_d3d_is_active(void)
{
	return __atomic_load_n(&bink_d3d_active, __ATOMIC_RELAXED) != 0;
}

#define BINK_VIDEO_TEXTURES 8
static struct {
	volatile PVOID texture;
	int scale;
} bink_video_textures[BINK_VIDEO_TEXTURES];
static volatile LONG bink_movie_width, bink_movie_height;
static volatile LONG bink_fit_logged;
static uintptr_t bink_movie_pixel_shader;
static uintptr_t bink_movie_vertex_shader;
static volatile LONG bink_movie_shader_known;
static IDirect3DDevice9 *bink_fit_device;
// Patch named Bink imports in every loaded client module. This avoids executable RVAs
// and Bink-version-specific function prologues, and works for x86/x64/IW4x module layouts.
static int bink_patch_module_import(HMODULE image, const char *symbol, void *replacement, void **original)
{
	if (!image || !symbol || !replacement || !original)
		return 0;
	uint8_t *module = (uint8_t *)image;
	IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)module;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
		return 0;
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(module + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return 0;
	IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir->VirtualAddress || !dir->Size)
		return 0;
	int patched = 0;
	IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(module + dir->VirtualAddress);
	for (; imp->Name; imp++) {
		const char *dll = (const char *)(module + imp->Name);
		if (lstrlenA(dll) < 4 || (dll[0] | 0x20) != 'b' || (dll[1] | 0x20) != 'i' || (dll[2] | 0x20) != 'n' || (dll[3] | 0x20) != 'k')
			continue;
		IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(module + imp->FirstThunk);
		IMAGE_THUNK_DATA *names = imp->OriginalFirstThunk ? (IMAGE_THUNK_DATA *)(module + imp->OriginalFirstThunk) : NULL;
		HMODULE imported = GetModuleHandleA((const char *)(module + imp->Name));
		void *exported = imported ? (void *)GetProcAddress(imported, symbol) : NULL;
		for (; iat->u1.Function; iat++) {
			int match = 0;
			if (names && names->u1.AddressOfData) {
				if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
					match = exported && (void *)(uintptr_t)iat->u1.Function == exported;
				else {
					IMAGE_IMPORT_BY_NAME *by_name = (IMAGE_IMPORT_BY_NAME *)(module + names->u1.AddressOfData);
					match = lstrcmpA((const char *)by_name->Name, symbol) == 0;
				}
				names++;
			} else {
				match = exported && (void *)(uintptr_t)iat->u1.Function == exported;
			}
			if (!match)
				continue;
			void **slot = (void **)&iat->u1.Function;
			DWORD old;
			if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old))
				continue;
			void *current = InterlockedCompareExchangePointer((PVOID volatile *)slot, NULL, NULL);
			if (current != replacement && current) {
				if (!*original)
					*original = current;
				InterlockedExchangePointer((PVOID volatile *)slot, replacement);
				patched++;
			}
			DWORD ignored;
			VirtualProtect(slot, sizeof(*slot), old, &ignored);
		}
	}
	return patched;
}

static int bink_patch_import(const char *symbol, void *replacement, void **original)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
	if (snap == INVALID_HANDLE_VALUE)
		return 0;
	MODULEENTRY32 me;
	memset(&me, 0, sizeof(me));
	me.dwSize = sizeof(me);
	int patched = 0;
	if (Module32First(snap, &me)) {
		do {
			patched += bink_patch_module_import(me.hModule, symbol, replacement, original);
		} while (Module32Next(snap, &me));
	}
	CloseHandle(snap);
	return patched;
}

static void *WINAPI bink_open_hook(const char *name, unsigned int flags)
{
	void *handle = bink_open_original ? bink_open_original(name, flags) : NULL;
	if (handle) {
		const bink_public_header_t *movie = (const bink_public_header_t *)handle;
		InterlockedExchange(&bink_movie_width, (LONG)movie->width);
		InterlockedExchange(&bink_movie_height, (LONG)movie->height);
		LONG count = InterlockedIncrement(&bink_open_handles);
		if (count == 1) {
			InterlockedExchange(&bink_d3d_active, 1);
			for (int i = 0; i < BINK_VIDEO_TEXTURES; i++) {
				InterlockedExchangePointer((PVOID volatile *)&bink_video_textures[i].texture, NULL);
				bink_video_textures[i].scale = 0;
			}
			InterlockedExchange(&bink_fit_logged, 0);
			bink_movie_pixel_shader = 0;
			bink_movie_vertex_shader = 0;
			InterlockedExchange(&bink_movie_shader_known, 0);
		}
	}
	return handle;
}

static int WINAPI bink_do_frame_hook(void *handle)
{
	int result = bink_do_frame_original ? bink_do_frame_original(handle) : 0;
	return result;
}

static int WINAPI bink_copy_to_buffer_hook(void *handle, void *dest, int pitch, unsigned int height, unsigned int x, unsigned int y, unsigned int flags)
{
	int result = bink_copy_to_buffer_original ? bink_copy_to_buffer_original(handle, dest, pitch, height, x, y, flags) : 0;
	return result;
}

static void WINAPI bink_close_hook(void *handle)
{
	if (bink_close_original)
		bink_close_original(handle);
	if (handle) {
		LONG count = InterlockedDecrement(&bink_open_handles);
		if (count <= 0) {
			InterlockedExchange(&bink_open_handles, 0);
			InterlockedExchange(&bink_d3d_active, 0);
			for (int i = 0; i < BINK_VIDEO_TEXTURES; i++) {
				InterlockedExchangePointer((PVOID volatile *)&bink_video_textures[i].texture, NULL);
				bink_video_textures[i].scale = 0;
			}
			InterlockedExchange(&bink_movie_width, 0);
			InterlockedExchange(&bink_movie_height, 0);
			InterlockedExchange(&bink_fit_logged, 0);
			bink_movie_pixel_shader = 0;
			bink_movie_vertex_shader = 0;
			InterlockedExchange(&bink_movie_shader_known, 0);
		}
	}
}

static void bink_hooks_install(void)
{
	int open = bink_patch_import("BinkOpen", (void *)bink_open_hook, (void **)&bink_open_original);
	int frame = bink_patch_import("BinkDoFrame", (void *)bink_do_frame_hook, (void **)&bink_do_frame_original);
	int copy = bink_patch_import("BinkCopyToBuffer", (void *)bink_copy_to_buffer_hook, (void **)&bink_copy_to_buffer_original);
	int close = bink_patch_import("BinkClose", (void *)bink_close_hook, (void **)&bink_close_original);
	if (!open || (!frame && !copy) || !close)
		log_printf("native: Bink 16:9 fitting unavailable: import hooks incomplete (Open %d, frame %d, Close %d)", open, frame + copy, close);
}

typedef HRESULT(STDMETHODCALLTYPE *present_t)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT(STDMETHODCALLTYPE *present_ex_t)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD);
typedef HRESULT(STDMETHODCALLTYPE *sc_present_t)(IDirect3DSwapChain9 *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD);
static present_t present_orig;
static present_t present_base_orig;
static present_ex_t present_ex_orig;
static present_ex_t present_ex_base_orig;
static sc_present_t sc_present_orig;
static sc_present_t sc_present_base_orig;
static IDirect3DDevice9 *the_device;
static volatile LONG present_hooks_pending;
static volatile LONG comp_hook_busy;
static volatile LONG present_bootstrap_failure_logged;
static volatile LONG present_hook_owner;
static volatile LONG present_passthrough_count[3];

// A graphics wrapper can call another Present route while our hook is active.
// Only the outermost call may draw the console. Re-entry goes
// to the implementation captured before our hook was first installed.
static int present_hook_enter(void)
{
	LONG tid = (LONG)GetCurrentThreadId();
	LONG owner = InterlockedCompareExchange(&present_hook_owner, tid, 0);
	return owner == 0 ? 1 : owner == tid ? 0 : -1;
}

static void present_hook_leave(int owner)
{
	if (owner == 1)
		InterlockedExchange(&present_hook_owner, 0);
}

static void present_log_passthrough(int via, int owner)
{
	LONG count = InterlockedIncrement(&present_passthrough_count[via]);
	if (count == 1)
		log_printf("native: %s Present via %d bypassed console re-entry (owner=%ld, thread=%lu)",
			   owner == 0 ? "nested" : "parallel", via,
			   (long)InterlockedCompareExchange(&present_hook_owner, 0, 0),
			   (unsigned long)GetCurrentThreadId());
}
static const GUID iid_device9ex = {0xb18b10ce, 0x2649, 0x405a, {0x87, 0x0f, 0x95, 0xf7, 0x77, 0xd4, 0x31, 0x3a}};

static void trace_present(void);
static int skip_hud_mask;
static int skip_ctx = 1; // [native] skipctx=<0|1>: the context whose HUD parts skiphud / skiprot drop
// Diagnostics ([native] skiprot=<seconds>): the HUD part skipped in context 1 changes every <seconds> through the candidates, and a frame is saved ("rot<k>") before the next change
static int split_on;
static void skip_rotation(IDirect3DDevice9 *dev)
{
	static int rot = -1;
	static DWORD t0;
	static int shot_win = -1;
	if (rot < 0)
		rot = cfg("skiprot", 0);
	if (rot <= 0 || mode != 11 || !split_on)
		return;
	DWORD now = GetTickCount();
	if (!t0)
		t0 = now;
	unsigned period = (unsigned)rot * 1000, win = (now - t0) / period;
	int k = (int)(win % 11);
	skip_hud_mask = 1 << k;
	if ((int)win != shot_win && (now - t0) % period > period * 3 / 4 && win < 33) {
		char name[16];
		shot_win = (int)win;
		snprintf(name, sizeof(name), "rot%02d_%d", (int)win, k);
		shot_save_now(dev, name);
	}
}
// QA ([native] exp11cycle=<seconds>, mode 11): player 2's 2D switches every <seconds> through three states, one frame saved ("cyc<n>_<state>") at the end of each window:
// 0 per-client placement + his 2D on, 1 no placement (full-screen placement as before) + his 2D on, 2 placement + his 2D off.
static int exp11_cycle_state(void)
{
	static int sec = -1;
	static DWORD t0;
	if (sec < 0)
		sec = cfg("exp11cycle", 0);
	if (sec <= 0 || mode != 11 || !split_on)
		return -1;
	if (!t0)
		t0 = GetTickCount();
	return (int)(((GetTickCount() - t0) / ((unsigned)sec * 1000)) % 3);
}
static void exp11_cycle_shot(IDirect3DDevice9 *dev)
{
	static int sec = -1, shot_win = -1;
	static DWORD t0;
	if (sec < 0)
		sec = cfg("exp11cycle", 0);
	if (sec <= 0 || mode != 11 || !split_on)
		return;
	if (!t0)
		t0 = GetTickCount();
	const unsigned period = (unsigned)sec * 1000, win = (GetTickCount() - t0) / period;
	if ((int)win != shot_win && (GetTickCount() - t0) % period > period * 3 / 4 && win < 12) {
		char name[24];
		shot_win = (int)win;
		snprintf(name, sizeof(name), "cyc%02d_%d", (int)win, (int)(win % 3));
		shot_save_now(dev, name);
	}
}
// [native] perflog=1: where a second of split screen goes (milliseconds, QueryPerformanceCounter): the waits for the renderer's workers, the copying of the contexts' ranges,
// the two contexts' client frames, the gap between the engine's frames, and the Present hook's work (render thread)
static LARGE_INTEGER perf_freq;
static volatile LONGLONG perf_wait, perf_copy, perf_ctx[2], perf_comp, perf_gap_sum, perf_step, perf_aim, perf_sync;
static volatile LONG perf_swaps, perf_ctx_n[2], perf_comp_n, perf_gap_n, perf_gap_long;
static LONGLONG perf_gap_max;
// smoothness ([native] perflog=1): how even the frames are. Buckets in ms: <2, <4, <6, <8, <12, <16, <24, <33, <50, more
#define PERF_BUCKETS 10
static volatile LONG perf_gap_hist[PERF_BUCKETS], perf_present_hist[PERF_BUCKETS];
static int perf_bucket(double ms)
{
	static const double edge[PERF_BUCKETS - 1] = {2, 4, 6, 8, 12, 16, 24, 33, 50};
	for (int i = 0; i < PERF_BUCKETS - 1; i++)
		if (ms < edge[i])
			return i;
	return PERF_BUCKETS - 1;
}
static LONGLONG perf_now(void)
{
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return c.QuadPart;
}
static void perf_report(void)
{
	static LONGLONG last;
	LONGLONG now = perf_now();
	if (!perf_freq.QuadPart)
		QueryPerformanceFrequency(&perf_freq);
	if (!last)
		last = now;
	if (now - last < perf_freq.QuadPart)
		return;
	double sec = (double)(now - last) / (double)perf_freq.QuadPart, ms = 1000.0 / (double)perf_freq.QuadPart;
	LONG n0 = perf_ctx_n[0], n1 = perf_ctx_n[1], nc = perf_comp_n, ng = perf_gap_n, sw = perf_swaps;
	log_printf("native: perf per second: engine frames %.0f (gap %.2f ms), ctx0 frames %ld (%.2f ms each), ctx1 frames %ld (%.2f ms each), swaps %ld (waits %.1f ms, copies %.1f ms, per second in total), present hook %ld frames (%.2f ms each)",
		   ng / sec, ng ? (double)perf_gap_sum * ms / ng : 0.0, n0, n0 ? (double)perf_ctx[0] * ms / n0 : 0.0, n1, n1 ? (double)perf_ctx[1] * ms / n1 : 0.0, sw,
		   (double)perf_wait * ms / sec, (double)perf_copy * ms / sec, nc, nc ? (double)perf_comp * ms / nc : 0.0);
#ifdef _WIN64
	if (CFG_ONCE("sndlog", 0)) { // the engine's two sound listeners (rva 0x1a28f48, 0x3c bytes each: origin, active flag at +0x38)
		const uint8_t *l = base + XR(0x1a28f48);
		log_printf("native: sound listeners: 0 %s at %.0f %.0f %.0f, 1 %s at %.0f %.0f %.0f", l[0x38] ? "on" : "off", (double)((const float *)l)[0], (double)((const float *)l)[1], (double)((const float *)l)[2],
			   l[0x3c + 0x38] ? "on" : "off", (double)((const float *)(l + 0x3c))[0], (double)((const float *)(l + 0x3c))[1], (double)((const float *)(l + 0x3c))[2]);
		if (snd_ctx_ptr)
			log_printf("native: world sounds started per second: context 0 %ld, context 1 %ld", (long)InterlockedExchange(&snd_counts[0], 0), (long)InterlockedExchange(&snd_counts[1], 0));
		for (int i = 0; i < 512 && snd_ctx_ptr && snd_names[i].name; i++) {
			if (snd_names[i].n[0] + snd_names[i].n[1] > 0 && !IsBadReadPtr(snd_names[i].name, 4))
				log_printf("native:   sound %s: context 0 %ld, context 1 %ld", snd_names[i].name, (long)snd_names[i].n[0], (long)snd_names[i].n[1]);
			snd_names[i].n[0] = snd_names[i].n[1] = 0;
		}
		for (int i = 0; i < 64 && snd_callers[i].rva; i++) {
			if (snd_callers[i].n[0] + snd_callers[i].n[1] > 0)
				log_printf("native:   sound caller rva 0x%x: context 0 %ld, context 1 %ld", snd_callers[i].rva, (long)snd_callers[i].n[0], (long)snd_callers[i].n[1]);
			snd_callers[i].n[0] = snd_callers[i].n[1] = 0;
		}
	}
#else
	if (!game_is_iw4x() && CFG_ONCE("sndlog", 0)) {
		const uint8_t *l = base + XR(0x17f5b4c); // the listener array (official 32-bit rva)
		log_printf("native: sound listeners: 0 %s at %.0f %.0f %.0f, 1 %s at %.0f %.0f %.0f", l[0x38] ? "on" : "off", (double)((const float *)l)[0], (double)((const float *)l)[1], (double)((const float *)l)[2],
			   l[0x3c + 0x38] ? "on" : "off", (double)((const float *)(l + 0x3c))[0], (double)((const float *)(l + 0x3c))[1], (double)((const float *)(l + 0x3c))[2]);
	}
#endif
	{
		char line[400];
		int n = snprintf(line, sizeof(line), "native: perf smoothness: engine frame gaps <2/4/6/8/12/16/24/33/50/more ms:");
		for (int i = 0; i < PERF_BUCKETS; i++)
			n += snprintf(line + n, sizeof(line) - n, " %ld", (long)perf_gap_hist[i]), perf_gap_hist[i] = 0;
		n += snprintf(line + n, sizeof(line) - n, " | shown frames:");
		for (int i = 0; i < PERF_BUCKETS; i++)
			n += snprintf(line + n, sizeof(line) - n, " %ld", (long)perf_present_hist[i]), perf_present_hist[i] = 0;
		log_printf("%s", line);
	}
	log_printf("native: perf per second, inside the client frames (ms per second in total): server steps %.1f, aim targets %.1f, body sync %.1f; longest frame %.1f ms, "
		   "frames over 50 ms %ld", (double)perf_step * ms / sec, (double)perf_aim * ms / sec, (double)perf_sync * ms / sec, (double)perf_gap_max * ms, perf_gap_long);
	perf_gap_max = 0;
	perf_gap_long = 0;
	perf_step = perf_aim = perf_sync = 0;
	perf_wait = perf_copy = perf_ctx[0] = perf_ctx[1] = perf_comp = perf_gap_sum = 0;
	perf_swaps = perf_ctx_n[0] = perf_ctx_n[1] = perf_comp_n = perf_gap_n = 0;
	last = now;
}

#ifdef _WIN64 // (natframe2.c is only built into the 64-bit DLL: no name to link to in the 32-bit one)
extern int ngfr_trace_scene; // natframe2.c: the frame end's preconditions are logged while set (QA [native] tracescene=1)
#endif
static volatile int skip_this_present;
static volatile int title_shot_req; // QA ([native] titleshot=1): a capture of the frame with the intro title lines (set in hudelem_hook)
static void before_present_inner(IDirect3DDevice9 *dev)
{
	trace_present();
	if (mode == 11 && split_on) { // QA: [native] shot11=<present> saves the frame "m11-<n>.bmp", four of them shot11step (600) presents apart
		static LONG n;
		static int at = -2, step;
		if (at == -2)
			at = cfg("shot11", -1), step = cfg("shot11step", 600);
		if (title_shot_req) {
			char tn[24];
			snprintf(tn, sizeof(tn), "title%d", title_shot_req);
			title_shot_req = 0;
			shot_save_now(dev, tn);
		}
		exp11_cycle_shot(dev);
		if (CFG_ONCE("burst11", -1) == -2) { // armed by the dvar mw2coop_qa_burst (set by a QA script): the next burst11n presents are saved
			static int left = -1;
			static LONG bn2;
			char bv[16];
			if (left < 0 && (++bn2 % 8) == 0 && game_dvar_raw("mw2coop_qa_burst", bv, sizeof(bv)) && bv[0] == '1')
				left = CFG_ONCE("burst11n", 60);
			if (left > 0) {
				char bname[32];
				snprintf(bname, sizeof(bname), "b11-%04d", CFG_ONCE("burst11n", 60) - left);
				shot_save_now(dev, bname);
				left--;
			}
		}
		if (CFG_ONCE("burst11", -1) >= 0) { // QA: [native] burst11=<present>;burst11n=<count>: that many consecutive presents are saved ("b11-<present>"), to compare how the poses change from frame to frame
			static LONG bn;
			const LONG bk = InterlockedIncrement(&bn);
			if (bk >= CFG_ONCE("burst11", -1) && bk < CFG_ONCE("burst11", -1) + CFG_ONCE("burst11n", 30)) {
				char bname[32];
				snprintf(bname, sizeof(bname), "b11-%ld", (long)bk);
				shot_save_now(dev, bname);
			}
		}
		skip_rotation(dev); // QA: [native] skiprot=<seconds> in mode 11 too: the HUD part player 2 skips changes every <seconds>, a frame is saved before each change
		const LONG k = InterlockedIncrement(&n);
		if (k == 300 || (k % 1200) == 0) // QA: how many presents happen once the split is on (the capture's <present> counts these)
			log_printf("native: shot11: present %ld, mode 11 on, at %d step %d", (long)k, at, step);
		if (at >= 0 && k >= at && (k - at) % (step > 0 ? step : 1) == 0 && (k - at) / (step > 0 ? step : 1) < 4) {
			char name[32];
			snprintf(name, sizeof(name), "m11-%ld", (long)k);
			shot_save_now(dev, name);
		}
	}
	if (CFG_ONCE("perflog", 0)) { // how evenly the pictures are shown: the time between two presents
		static LONGLONG prev;
		LARGE_INTEGER c, f;
		QueryPerformanceCounter(&c);
		QueryPerformanceFrequency(&f);
		if (prev)
			perf_present_hist[perf_bucket((double)(c.QuadPart - prev) * 1000.0 / (double)f.QuadPart)]++;
		prev = c.QuadPart;
	}
}

// [native] presentlog=1: the real cadence of the picture: the time between two Present calls (render thread), 100 frames at a time: mean, sd, the longest, the shortest.
static void present_stat(void)
{
	static LONGLONG last;
	static double n, sum, sq, mx, mn = 1e9;
	LARGE_INTEGER c, f;
	QueryPerformanceCounter(&c);
	QueryPerformanceFrequency(&f);
	if (last) {
		double ms = (double)(c.QuadPart - last) * 1000.0 / (double)f.QuadPart;
		if (ms < 200.0) {
			n++, sum += ms, sq += ms * ms;
			if (ms > mx)
				mx = ms;
			if (ms < mn)
				mn = ms;
		}
		if (n >= 150) {
			double m = sum / n;
			log_printf("native: present cadence: %.0f frames, mean %.2f ms (%.0f fps), sd %.2f ms, min %.1f max %.1f ms", n, m, 1000.0 / m, sqrt(fmax(0, sq / n - m * m)), mn, mx);
			n = sum = sq = mx = 0;
			mn = 1e9;
		}
	}
	last = c.QuadPart;
}

// Player 1's frame in mode 11 is ended with the "show" bit too (0xc4c0(3), the bit 0x80 only marks it for the back end): the back end runs a frame's post-process composite and its
// 2D (the HUD) only for a frame that has bit 1 (0x453c0 tests `flags & 2`; 0x45060 presents on the same bit). Player 1's frame had flags 1, so it got neither a post-process nor
// its HUD; only player 2's frame (flags 3) did, which is also why his 2D painted over the half (2D of one frame only). The 360 draws each view with its own 2D.
// The back end's call of 0x45060 (at 0x45757) strips the mark, and the Present that frame makes is skipped, so one picture is shown per engine frame ([native] exp11p1show=0 off).
static volatile int be_skip_present;
#ifdef _WIN64
static void (*be_end_original)(unsigned flags);
static void be_end_hook(unsigned flags)
{
	if (flags & 0x80) {
		flags &= ~0x80u;
		be_skip_present = 1;
	}
	be_end_original(flags);
	be_skip_present = 0;
}
#else
// 32 bits (mode11-frame.md): the back end's frame (VA 0x5397e0, cdecl void (void), called three times from the render thread's loop 0x5398e0) tests the show bit itself and
// tail-jumps to the frame end, which presents; there is no call that receives the flags. The frame it draws is the one the loop has just stored at VA 0x2534c74; its flags
// are at +0x41a38 (nothing in the 32-bit back end tests 0x80). While a frame marked 0x80 (player 1's) is drawn, its Present is skipped.
#define BE_FRAME_DATA XR(0x2134c74)
#define BE_FD_FLAGS 0x41a38
static void (*be_frame_original)(void);
static void rthitch_note(const char *what, LONGLONG t0, int p1);
static volatile LONGLONG be_t0; // QA (rthitch=2): the back end frame under way since (QPC), 0 between frames
static volatile DWORD be_tid;
// QA ([native] rthitch=2): while a back end frame runs longer than 40 ms, the render thread is stopped for a moment (at 40, 90 and 160 ms into the frame) and where it stands
// is logged: its instruction and the return addresses on its stack, by module (the game, this mod, the graphics driver)
static void rthitch_where(const char *tag, uintptr_t a, char *out, size_t cap, size_t *n)
{
	HMODULE m = NULL;
	char name[MAX_PATH] = "?";
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
		DWORD k = GetModuleFileNameA(m, name, sizeof(name));
		while (k > 0 && name[k - 1] != 0x5c)
			k--;
		memmove(name, name + k, strlen(name + k) + 1);
	} else
		return;
	if (*n < cap)
		*n += (size_t)snprintf(out + *n, cap - *n, " %s%s+%lx", tag, name, (unsigned long)(a - (uintptr_t)m));
}
static DWORD WINAPI rthitch_thread(LPVOID unused)
{
	(void)unused;
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	LONGLONG seen = 0;
	int stage = 0;
	unsigned logged = 0;
	for (;;) {
		Sleep(2);
		const LONGLONG t0 = be_t0;
		if (!t0 || logged >= 60)
			continue;
		if (t0 != seen)
			seen = t0, stage = 0;
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		const double ms = (double)(now.QuadPart - t0) * 1000.0 / (double)f.QuadPart;
		static const double at[3] = {40.0, 90.0, 160.0};
		if (stage >= 3 || ms < at[stage])
			continue;
		stage++;
		HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, be_tid);
		if (!th)
			continue;
		if (SuspendThread(th) != (DWORD)-1) {
			CONTEXT c;
			memset(&c, 0, sizeof(c));
			c.ContextFlags = CONTEXT_CONTROL;
			if (GetThreadContext(th, &c)) {
				char line[900];
				size_t n = (size_t)snprintf(line, sizeof(line), "native: rthitch: render thread %.0f ms into its frame at", ms);
				rthitch_where("", (uintptr_t)CTX_IP(c), line, sizeof(line), &n);
				const uintptr_t *sp = (const uintptr_t *)CTX_SP(c);
				int found = 0;
				for (int i = 0; i < 400 && found < 14 && !IsBadReadPtr(sp + i, sizeof(*sp)); i++) {
					const size_t before = n;
					rthitch_where("< ", sp[i], line, sizeof(line), &n);
					if (n != before)
						found++;
				}
				// the game's code on the stack (below the driver's frames): return addresses into the exe's code (a call right before them)
				int game = 0;
				if (n < sizeof(line))
					n += (size_t)snprintf(line + n, sizeof(line) - n, " | game:");
				for (int i = 0; i < 3000 && game < 10 && !IsBadReadPtr(sp + i, sizeof(*sp)); i++) {
					const uintptr_t v = sp[i];
					if (v > (uintptr_t)base + 0x1005 && v < (uintptr_t)base + 0x294000 && !IsBadReadPtr((void *)(v - 5), 5) && ((const uint8_t *)v)[-5] == 0xe8 && n < sizeof(line)) // raw-ok: QA, the code section's bounds as a plausibility test
						n += (size_t)snprintf(line + n, sizeof(line) - n, " %lx", (unsigned long)(v - (uintptr_t)base)), game++;
				}
				logged++;
				log_printf("%s", line);
			}
			ResumeThread(th);
		}
		CloseHandle(th);
	}
	return 0;
}
static void be_frame_hook(void)
{
	const uint8_t *fd = *(const uint8_t *const *)(base + BE_FRAME_DATA);
	const int p1 = fd && (*(const unsigned *)(fd + BE_FD_FLAGS) & 0x80u);
	if (p1)
		be_skip_present = 1;
	LARGE_INTEGER t0;
	QueryPerformanceCounter(&t0);
	if (CFG_ONCE("rthitch", 0) >= 2) {
		static int started;
		be_tid = GetCurrentThreadId();
		if (!started++)
			CreateThread(NULL, 0, rthitch_thread, NULL, 0, NULL);
		be_t0 = t0.QuadPart;
	}
	be_frame_original();
	be_t0 = 0;
	be_skip_present = 0;
	rthitch_note("render thread frame", t0.QuadPart, p1);
	if (CFG_ONCE("rthitch", 0)) { // QA: the texture memory Direct3D 9 still has (asked on this, the device's, thread), every 5 s
		static DWORD at;
		IDirect3DDevice9 *dev = (IDirect3DDevice9 *)d3dcap_device();
		if (dev && GetTickCount() - at >= 5000) {
			at = GetTickCount();
			log_printf("native: rthitch: available texture memory %u MB", (unsigned)(IDirect3DDevice9_GetAvailableTextureMem(dev) >> 20));
		}
	}
}
#endif
// QA ([native] rthitch=1): the render thread's frames and the real Presents that take 30 ms or more, and (32 bits) the process's used address space every 10 s
static void rthitch_note(const char *what, LONGLONG t0, int p1)
{
	static int on = -1;
	static LONGLONG freq;
	if (on < 0) {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		freq = f.QuadPart;
		on = cfg("rthitch", 0);
	}
	if (!on)
		return;
	LARGE_INTEGER t1;
	QueryPerformanceCounter(&t1);
	const double ms = (double)(t1.QuadPart - t0) * 1000.0 / (double)freq;
	static unsigned logged;
	if (ms >= 30.0 && logged < 300)
		logged++, log_printf("native: rthitch: %s %.1f ms (%s)", what, ms, p1 < 0 ? "-" : p1 ? "player 1's frame" : "player 2's frame");
}
static void before_present(int via, IDirect3DDevice9 *dev)
{
	skip_this_present = 0;
	if (be_skip_present) { // the frame that is not shown (player 1's): everything the Present hook does for a shown frame waits for player 2's
		skip_this_present = 1;
		return;
	}
	int on = CFG_ONCE("perflog", 0);
	if (CFG_ONCE("presentlog", 0))
		present_stat();
	d3dcap_on12_probe(dev);
	LONGLONG t0 = on ? perf_now() : 0;
	before_present_inner(dev);
	if (on) {
		perf_comp += perf_now() - t0;
		InterlockedIncrement(&perf_comp_n);
	}
	if (!skip_this_present) { // the console and the QA frames are on what the player sees
		fsr3_present(dev); // the upscaled picture is what the console draws over
		console_present(via, dev);
		shot_present_tick(dev);
	}
}

static HRESULT STDMETHODCALLTYPE present_hook(IDirect3DDevice9 *dev, const RECT *a, const RECT *b, HWND c, const RGNDATA *d)
{
	int owner = present_hook_enter();
	if (owner != 1) {
		present_log_passthrough(0, owner);
		present_t target = present_base_orig ? present_base_orig : present_orig;
		return target && target != present_hook ? target(dev, a, b, c, d) : D3DERR_INVALIDCALL;
	}
	before_present(0, dev);
	if (skip_this_present) {
		present_hook_leave(owner);
		return D3D_OK;
	}
	HRESULT hr = present_orig ? present_orig(dev, a, b, c, d) : D3DERR_INVALIDCALL;
	present_hook_leave(owner);
	return hr;
}

static HRESULT STDMETHODCALLTYPE present_ex_hook(IDirect3DDevice9 *dev, const RECT *a, const RECT *b, HWND c, const RGNDATA *d, DWORD e)
{
	int owner = present_hook_enter();
	if (owner != 1) {
		present_log_passthrough(1, owner);
		present_ex_t target = present_ex_base_orig ? present_ex_base_orig : present_ex_orig;
		return target && target != present_ex_hook ? target(dev, a, b, c, d, e) : D3DERR_INVALIDCALL;
	}
	before_present(1, dev);
	if (skip_this_present) {
		present_hook_leave(owner);
		return D3D_OK;
	}
	HRESULT hr = present_ex_orig ? present_ex_orig(dev, a, b, c, d, e) : D3DERR_INVALIDCALL;
	present_hook_leave(owner);
	return hr;
}

static HRESULT STDMETHODCALLTYPE sc_present_hook(IDirect3DSwapChain9 *sc, const RECT *a, const RECT *b, HWND c, const RGNDATA *d, DWORD e)
{
	int owner = present_hook_enter();
	if (owner != 1) {
		present_log_passthrough(2, owner);
		sc_present_t target = sc_present_base_orig ? sc_present_base_orig : sc_present_orig;
		return target && target != sc_present_hook ? target(sc, a, b, c, d, e) : D3DERR_INVALIDCALL;
	}
	// the swap chain's own device, not the one kept in the_device: a vid_restart frees that one (32-bit: it is never refreshed; 64-bit: NULL until
	// the next capture), and the console, FSR and shots were handed a freed or NULL device (audit 2026-11-18, native 1)
	IDirect3DDevice9 *sc_dev = NULL;
	if (FAILED(IDirect3DSwapChain9_GetDevice(sc, &sc_dev)))
		sc_dev = NULL;
	before_present(2, sc_dev ? sc_dev : the_device);
	if (sc_dev)
		IDirect3DDevice9_Release(sc_dev);
	if (skip_this_present) {
		present_hook_leave(owner);
		return D3D_OK;
	}
	LARGE_INTEGER pt0;
	QueryPerformanceCounter(&pt0);
	HRESULT hr = sc_present_orig ? sc_present_orig(sc, a, b, c, d, e) : D3DERR_INVALIDCALL;
	rthitch_note("Present", pt0.QuadPart, -1);
	present_hook_leave(owner);
	return hr;
}

// The vtable slots of the D3D9 device and swap chain that this file patches. The Steam overlay (gameoverlayrenderer64.dll) hooks the same functions and puts its hooks back after
// a device Reset; two hooks on one slot that each saved the other's address call each other until the stack is gone (0xc00000fd inside the overlay). So around a Reset all of
// ours are taken out first and put back on top of whatever is there afterwards.
static struct {
	void **slot;
	void *hook;
	void **orig;
	int in;
} slots[64];
static int nslots;
// slots[] is written from four threads (the Bink thread, the render thread, the main thread, the console's) and read around a Reset: one at a time
// (audit 2026-11-18, native 3)
static SRWLOCK slots_lock = SRWLOCK_INIT;

static int patch_slot_locked(void **slot, void *hook, void **orig);
static int patch_slot(void **slot, void *hook, void **orig)
{
	AcquireSRWLockExclusive(&slots_lock);
	const int r = patch_slot_locked(slot, hook, orig);
	ReleaseSRWLockExclusive(&slots_lock);
	return r;
}

static int patch_slot_locked(void **slot, void *hook, void **orig)
{
	DWORD old, ignored;
	void *previous;
	if (!slot || !hook || !orig) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (nslots >= (int)(sizeof(slots) / sizeof(slots[0]))) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return 0;
	}
	if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
		return 0;
	previous = InterlockedCompareExchangePointer((PVOID volatile *)slot, NULL, NULL);
	if (previous == hook) {
		VirtualProtect(slot, sizeof(void *), old, &ignored);
		return 1;
	}
	if (!previous) {
		VirtualProtect(slot, sizeof(void *), old, &ignored);
		SetLastError(ERROR_INVALID_ADDRESS);
		return 0;
	}
	*orig = previous;
	InterlockedExchangePointer((PVOID volatile *)slot, hook);
	VirtualProtect(slot, sizeof(void *), old, &ignored);
	slots[nslots].slot = slot, slots[nslots].hook = hook, slots[nslots].orig = orig, slots[nslots].in = 1;
	nslots++;
	return 1;
}

// From the last patched down: of two hooks of ours on one slot the upper one comes out first, and the lower one is then on top and comes out too (going
// up, the lower one was skipped as covered and later forgotten, audit 2026-11-18, native 3).
static void slots_take_out(void)
{
	AcquireSRWLockExclusive(&slots_lock);
	for (int i = nslots - 1; i >= 0; i--) {
		DWORD old;
		if (!slots[i].in || *slots[i].slot != slots[i].hook || !VirtualProtect(slots[i].slot, sizeof(void *), PAGE_READWRITE, &old))
			continue; // somebody else is on top of ours: left as it is
		*slots[i].slot = *slots[i].orig;
		VirtualProtect(slots[i].slot, sizeof(void *), old, &old);
		slots[i].in = 0;
	}
	ReleaseSRWLockExclusive(&slots_lock);
}

// (vid_restart) the entries that are out are forgotten (they go back on the new device); one that could not be taken out (something else on top of
// it) is kept, so a later take-out still knows it is ours (vid_restart, both builds since 2026-11-20)
static void slots_forget_out(void)
{
	AcquireSRWLockExclusive(&slots_lock);
	int k = 0;
	for (int i = 0; i < nslots; i++)
		if (slots[i].in)
			slots[k++] = slots[i];
	nslots = k;
	ReleaseSRWLockExclusive(&slots_lock);
}

static void slots_put_back(void)
{
	AcquireSRWLockExclusive(&slots_lock);
	for (int i = 0; i < nslots; i++) {
		DWORD old;
		if (slots[i].in || !VirtualProtect(slots[i].slot, sizeof(void *), PAGE_READWRITE, &old))
			continue;
		*slots[i].orig = *slots[i].slot; // whatever is there now is what ours calls
		*slots[i].slot = slots[i].hook;
		VirtualProtect(slots[i].slot, sizeof(void *), old, &old);
		slots[i].in = 1;
	}
	ReleaseSRWLockExclusive(&slots_lock);
}

// [native] resetlog=1 (default off): the vtable slots around a device Reset, to see who else writes them (the Steam and Discord overlays hook the device as well): where each slot points, as module+offset
static void slot_owner(void *p, char *out, size_t size)
{
	HMODULE m = NULL;
	char name[MAX_PATH] = "?";
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p, &m) && m) {
		GetModuleFileNameA(m, name, sizeof(name));
		const char *slash = strrchr(name, '\\');
		snprintf(out, size, "%s+%lx", slash ? slash + 1 : name, (unsigned long)((uintptr_t)p - (uintptr_t)m));
	} else {
		snprintf(out, size, "%p", p);
	}
}
static void slots_log(const char *when)
{
	if (!cfg("resetlog", 0))
		return;
	for (int i = 0; i < nslots; i++) {
		char who[MAX_PATH + 24];
		slot_owner(*slots[i].slot, who, sizeof(who));
		log_printf("native: slot %d %s: %s%s", i, when, who, *slots[i].slot == slots[i].hook ? " (ours)" : "");
	}
}
// after a Reset: any change of a slot made by somebody else is logged for a few seconds (when the overlays put their hooks back, and what they leave in the slot)
static DWORD WINAPI slots_watch_thread(LPVOID unused)
{
	(void)unused;
	void *last[24];
	for (int i = 0; i < nslots; i++)
		last[i] = *slots[i].slot;
	for (int t = 0; t < 60; t++) {
		Sleep(100);
		for (int i = 0; i < nslots; i++) {
			void *now = *slots[i].slot;
			if (now == last[i])
				continue;
			char from[MAX_PATH + 24], to[MAX_PATH + 24];
			slot_owner(last[i], from, sizeof(from));
			slot_owner(now, to, sizeof(to));
			log_printf("native: slot %d changed %d ms after the Reset: %s -> %s%s", i, (t + 1) * 100, from, to, now == slots[i].hook ? " (ours)" : "");
			last[i] = now;
		}
	}
	return 0;
}

// A device Reset (vtable 16: a lost device, a vid_restart, a change of the desktop): the hooks are taken out for it and put back after it (see above)
typedef HRESULT(STDMETHODCALLTYPE *reset_t)(IDirect3DDevice9 *, void *);
static reset_t reset_orig;
// A Reset that comes back into ours while ours runs (an overlay put on top of ours, whose saved original is our hook, once ours had saved the overlay's)
// went round until the stack ran out: such a call goes to the first original ours saw (audit 2026-11-18, native 3).
static __thread int reset_depth;
static HRESULT STDMETHODCALLTYPE reset_hook(IDirect3DDevice9 *dev, void *params)
{
	static reset_t first;
	if (!first)
		first = reset_orig;
	if (reset_depth)
		return first ? first(dev, params) : D3DERR_INVALIDCALL;
	log_printf("native: device reset: the hooks are taken out for it");
	reset_t call = reset_orig;
	slots_log("before the Reset");
	slots_take_out();
	slots_log("taken out");
	reset_depth++;
	HRESULT hr = call(dev, params);
	reset_depth--;
	slots_log("after the Reset");
	slots_put_back();
	slots_log("put back");
	if (cfg("resetlog", 0))
		CloseHandle(CreateThread(NULL, 0, slots_watch_thread, NULL, 0, NULL));
	return hr;
}
typedef HRESULT(STDMETHODCALLTYPE *reset_ex_t)(IDirect3DDevice9 *, void *, void *);
static reset_ex_t reset_ex_orig;
static HRESULT STDMETHODCALLTYPE reset_ex_hook(IDirect3DDevice9 *dev, void *params, void *mode)
{
	static reset_ex_t first;
	if (!first)
		first = reset_ex_orig;
	if (reset_depth)
		return first ? first(dev, params, mode) : D3DERR_INVALIDCALL;
	log_printf("native: device reset (Ex): the hooks are taken out for it");
	reset_ex_t call = reset_ex_orig;
	slots_log("before the Reset");
	slots_take_out();
	slots_log("taken out");
	reset_depth++;
	HRESULT hr = call(dev, params, mode);
	reset_depth--;
	slots_log("after the Reset");
	slots_put_back();
	slots_log("put back");
	if (cfg("resetlog", 0))
		CloseHandle(CreateThread(NULL, 0, slots_watch_thread, NULL, 0, NULL));
	return hr;
}

// ---- diagnostics ([native] d3dtrace=1): the render thread's calls of one frame (render targets, viewports, clears, stretches, draw counts), to see where the 3D ends and the 2D begins
static int trace_on;
static char trace_buf[400000];
static int trace_len, trace_draws;
static IDirect3DDevice9 *trace_dev;
static char trace_key[192], trace_key_last[192], trace_stack[160], trace_stack_last[160];
static int bb_pass, bb_pass_done, bb_draws;
static void trace_flush_draws(void)
{
	if (trace_draws && trace_len < (int)sizeof(trace_buf) - 160)
		trace_len += snprintf(trace_buf + trace_len, sizeof(trace_buf) - trace_len, "draw x%d %s  @%s\n", trace_draws, trace_key_last, trace_stack_last);
	trace_draws = 0;
}
// one draw: the shaders and states it uses; a run of draws with the same ones is one line
static void trace_draw(IDirect3DDevice9 *dev)
{
	IDirect3DVertexShader9 *vs = NULL;
	IDirect3DPixelShader9 *ps = NULL;
	DWORD z = 0, zw = 0, ab = 0, sb = 0, db = 0, sep = 0, sba = 0, dba = 0, cw = 0, sc = 0;
	IDirect3DDevice9_GetVertexShader(dev, &vs);
	IDirect3DDevice9_GetPixelShader(dev, &ps);
	IDirect3DDevice9_GetRenderState(dev, D3DRS_ZENABLE, &z);
	IDirect3DDevice9_GetRenderState(dev, D3DRS_ZWRITEENABLE, &zw);
	IDirect3DDevice9_GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &ab);
	IDirect3DDevice9_GetRenderState(dev, D3DRS_SRCBLEND, &sb);
	IDirect3DDevice9_GetRenderState(dev, D3DRS_DESTBLEND, &db);
	IDirect3DDevice9_GetRenderState(dev, D3DRS_SEPARATEALPHABLENDENABLE, &sep);
	IDirect3DDevice9_GetRenderState(dev, D3DRS_SRCBLENDALPHA, &sba);
	IDirect3DDevice9_GetRenderState(dev, D3DRS_DESTBLENDALPHA, &dba);
	IDirect3DDevice9_GetRenderState(dev, D3DRS_COLORWRITEENABLE, &cw);
	IDirect3DDevice9_GetRenderState(dev, D3DRS_SCISSORTESTENABLE, &sc);
	if (bb_pass && !bb_pass_done && bb_draws % 60 == 0 && bb_draws > 0) { // what the picture holds after this many draws of the pass that ends with the screen copy
		char name[24];
		trace_on = 0;
		snprintf(name, sizeof(name), "tr-d%03d", bb_draws);
		shot_save_now(dev, name);
		trace_on = 1;
	}
	bb_draws++;
	snprintf(trace_key, sizeof(trace_key), "vs=%p ps=%p z=%lu zw=%lu blend=%lu %lu/%lu sep=%lu %lu/%lu cw=%lx sc=%lu", (void *)vs, (void *)ps, (unsigned long)z, (unsigned long)zw,
		 (unsigned long)ab, (unsigned long)sb, (unsigned long)db, (unsigned long)sep, (unsigned long)sba, (unsigned long)dba, (unsigned long)cw, (unsigned long)sc);
	if (vs)
		IDirect3DVertexShader9_Release(vs);
	if (ps)
		IDirect3DPixelShader9_Release(ps);
	{
		void *frames[8];
		USHORT n = CaptureStackBackTrace(2, 8, frames, NULL); // the callers of the engine's draw wrapper, as offsets into the game's image
		int len = 0;
		trace_stack[0] = 0;
		for (USHORT i = 0; i < n && len < (int)sizeof(trace_stack) - 12; i++) {
			uintptr_t off = (uintptr_t)frames[i] - (uintptr_t)base;
			if (off < 0x4000000)
				len += snprintf(trace_stack + len, sizeof(trace_stack) - len, "%lx,", (unsigned long)off);
		}
	}
	if (strcmp(trace_key, trace_key_last) || strcmp(trace_stack, trace_stack_last)) {
		trace_flush_draws();
		strcpy(trace_key_last, trace_key);
		strcpy(trace_stack_last, trace_stack);
	}
	trace_draws++;
}
static int trace_on_now(void) { return trace_on; }
static void trace_event(const char *fmt, ...)
{
	trace_flush_draws();
	if (trace_len > (int)sizeof(trace_buf) - 200)
		return;
	va_list ap;
	va_start(ap, fmt);
	trace_len += vsnprintf(trace_buf + trace_len, sizeof(trace_buf) - trace_len, fmt, ap);
	va_end(ap);
	trace_buf[trace_len++] = '\n';
}
static const char *trace_surf(IDirect3DDevice9 *dev, IDirect3DSurface9 *s)
{
	static char out[64];
	IDirect3DSurface9 *back = NULL;
	D3DSURFACE_DESC d = {0};
	if (s)
		IDirect3DSurface9_GetDesc(s, &d);
	if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back)
		IDirect3DSurface9_Release(back);
	snprintf(out, sizeof(out), "%p %ux%u fmt%d%s", (void *)s, d.Width, d.Height, (int)d.Format, s && s == back ? " BACKBUFFER" : "");
	return out;
}
typedef HRESULT(STDMETHODCALLTYPE *set_rt_t)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
typedef HRESULT(STDMETHODCALLTYPE *clear_t)(IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD);
typedef HRESULT(STDMETHODCALLTYPE *stretch_t)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE);
typedef HRESULT(STDMETHODCALLTYPE *update_texture_t)(IDirect3DDevice9 *, IDirect3DBaseTexture9 *, IDirect3DBaseTexture9 *);
typedef HRESULT(STDMETHODCALLTYPE *viewport_t)(IDirect3DDevice9 *, const D3DVIEWPORT9 *);
typedef HRESULT(STDMETHODCALLTYPE *dip_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *dp_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *dpup_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
typedef HRESULT(STDMETHODCALLTYPE *dipup_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT);
typedef HRESULT(STDMETHODCALLTYPE *scissor_t)(IDirect3DDevice9 *, const RECT *);
typedef HRESULT(STDMETHODCALLTYPE *scene_t)(IDirect3DDevice9 *);
static set_rt_t set_rt_orig;
static clear_t clear_orig;
static stretch_t stretch_orig;
static update_texture_t update_texture_orig;
static viewport_t viewport_orig;
static dip_t dip_orig;
static dp_t dp_orig;
static scissor_t scissor_orig;
static int bink_track_video_texture(IDirect3DBaseTexture9 *texture, const D3DSURFACE_DESC *desc)
{
	if (!texture || !desc)
		return 0;
	UINT movie_w = (UINT)InterlockedCompareExchange(&bink_movie_width, 0, 0);
	UINT movie_h = (UINT)InterlockedCompareExchange(&bink_movie_height, 0, 0);
	int scale = 0;
	if (desc->Width == movie_w && desc->Height == movie_h)
		scale = 1;
	else if (movie_w >= 2 && movie_h >= 2 && desc->Width == movie_w / 2 && desc->Height == movie_h / 2)
		scale = 2;
	else if (movie_w >= 4 && movie_h >= 4 && desc->Width == movie_w / 4 && desc->Height == movie_h / 4)
		scale = 4;
	if (!scale)
		return 0;
	for (int i = 0; i < BINK_VIDEO_TEXTURES; i++) {
		PVOID current = InterlockedCompareExchangePointer((PVOID volatile *)&bink_video_textures[i].texture, NULL, NULL);
		if (current == (PVOID)texture)
			return 0;
	}
	for (int i = 0; i < BINK_VIDEO_TEXTURES; i++) {
		if (InterlockedCompareExchangePointer((PVOID volatile *)&bink_video_textures[i].texture, (PVOID)(uintptr_t)1, NULL) == NULL) {
			bink_video_textures[i].scale = scale;
			InterlockedExchangePointer((PVOID volatile *)&bink_video_textures[i].texture, texture);
			return 1;
		}
	}
	return 0;
}

static int bink_is_movie_quad(IDirect3DDevice9 *dev, UINT primitive, UINT primitive_count, const char *api)
{
	if (!bink_d3d_is_active())
		return 0;
	// only an indexed quad of two triangles on a pixel stage can be the movie: everything else is passed by before 20 GetTexture calls (an in-game movie keeps
	// Bink open in levels, and this ran for every draw without a depth test, audit 2026-11-18, native 9)
	if (primitive != D3DPT_TRIANGLELIST || primitive_count != 2 || strcmp(api, "DrawIndexedPrimitive") != 0)
		return 0;
	// A full-screen movie is drawn by the 2D pass, with no depth test. A movie a level plays on things in the world (CinematicInGame: Just Like Old Times' control room
	// TVs, the countdown) is drawn on 3D quads with the depth test on: those are not fitted (2026-11-18, owner's screenshot of the control room in split screen: each
	// TV's draw narrowed the viewport to 4:3 and cleared the sides, vertical strips at 400 and 880 of a 1280x360 half), and their shaders are not learned as the movie's.
	DWORD zenable = D3DZB_FALSE;
	if (SUCCEEDED(IDirect3DDevice9_GetRenderState(dev, D3DRS_ZENABLE, &zenable)) && zenable != D3DZB_FALSE)
		return 0;
	static const DWORD sampler_stages[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}; // (the vertex samplers never held the movie for a fit)
	int movie_quad = 0;
	for (size_t i = 0; i < sizeof(sampler_stages) / sizeof(sampler_stages[0]); i++) {
		DWORD stage = sampler_stages[i];
		IDirect3DBaseTexture9 *tex = NULL;
		if (FAILED(IDirect3DDevice9_GetTexture(dev, stage, &tex)) || !tex)
			continue;
		PVOID texture = (PVOID)tex;
		for (int k = 0; k < BINK_VIDEO_TEXTURES; k++) {
			PVOID movie_tex = InterlockedCompareExchangePointer((PVOID volatile *)&bink_video_textures[k].texture, NULL, NULL);
			if (!movie_tex || movie_tex == (PVOID)(uintptr_t)1 || movie_tex != texture ||
			    IDirect3DBaseTexture9_GetType(tex) != D3DRTYPE_TEXTURE)
				continue;
			if (stage < 16 && bink_video_textures[k].scale == 1 && primitive == D3DPT_TRIANGLELIST &&
			    primitive_count == 2 && !strcmp(api, "DrawIndexedPrimitive")) {
				IDirect3DPixelShader9 *ps = NULL;
				IDirect3DVertexShader9 *vs = NULL;
				if (SUCCEEDED(IDirect3DDevice9_GetPixelShader(dev, &ps)) && ps &&
				    SUCCEEDED(IDirect3DDevice9_GetVertexShader(dev, &vs)) && vs) {
					uintptr_t pixel_identity = (uintptr_t)ps;
					uintptr_t vertex_identity = (uintptr_t)vs;
					if (!InterlockedCompareExchange(&bink_movie_shader_known, 0, 0)) {
						bink_movie_pixel_shader = pixel_identity;
						bink_movie_vertex_shader = vertex_identity;
						InterlockedExchange(&bink_movie_shader_known, 1);
					}
					if (pixel_identity == bink_movie_pixel_shader && vertex_identity == bink_movie_vertex_shader)
						movie_quad = 1;
				}
				if (vs) IDirect3DVertexShader9_Release(vs);
				if (ps) IDirect3DPixelShader9_Release(ps);
			}
		}
		IDirect3DBaseTexture9_Release(tex);
	}
	return movie_quad;
}

// Fit the Bink draw into the current viewport. D3D9 maps clip-space vertex positions
// through the viewport, so a temporary viewport affects only this draw call; subsequent
// subtitle/UI draws see the exact viewport that was active before the movie quad.
static int bink_fit_movie_viewport(IDirect3DDevice9 *dev, D3DVIEWPORT9 *saved, D3DVIEWPORT9 *fit_out)
{
	UINT movie_w = (UINT)InterlockedCompareExchange(&bink_movie_width, 0, 0);
	UINT movie_h = (UINT)InterlockedCompareExchange(&bink_movie_height, 0, 0);
	if (!movie_w || !movie_h || !viewport_orig || !saved || !fit_out ||
	    FAILED(IDirect3DDevice9_GetViewport(dev, saved)) || !saved->Width || !saved->Height)
		return 0;
	D3DVIEWPORT9 fit = *saved;
	double movie_aspect = (double)movie_w / (double)movie_h;
	double viewport_aspect = (double)saved->Width / (double)saved->Height;
	if (viewport_aspect > movie_aspect + 0.0001) {
		UINT width = (UINT)((double)saved->Height * movie_aspect + 0.5);
		if (!width || width > saved->Width)
			return 0;
		fit.X = saved->X + (saved->Width - width) / 2;
		fit.Width = width;
	} else if (viewport_aspect + 0.0001 < movie_aspect) {
		UINT height = (UINT)((double)saved->Width / movie_aspect + 0.5);
		if (!height || height > saved->Height)
			return 0;
		fit.Y = saved->Y + (saved->Height - height) / 2;
		fit.Height = height;
	} else {
		return 0;
	}
	if (fit.X == saved->X && fit.Y == saved->Y && fit.Width == saved->Width && fit.Height == saved->Height)
		return 0;
	if (FAILED(viewport_orig(dev, &fit)))
		return 0;
	*fit_out = fit;
	return 1;
}

// The movie is drawn over a screen that may already contain the preloaded menu/UI.
// Clear only the left/right margins outside the fitted movie rectangle after restoring
// the full viewport. Top/bottom letterboxing remains the game's responsibility. D3D9
// Clear is clipped by the scissor state, so restore its exact enable value afterwards.
static int bink_clear_movie_bars(IDirect3DDevice9 *dev, const D3DVIEWPORT9 *full, const D3DVIEWPORT9 *fit)
{
	if (!clear_orig || !full || !fit || !full->Width || !full->Height || fit->Y != full->Y || fit->Height != full->Height)
		return 0; // only mask left/right side bars; top/bottom letterboxing remains the game's responsibility
	uint64_t right64 = (uint64_t)full->X + full->Width;
	uint64_t fit_right64 = (uint64_t)fit->X + fit->Width;
	if (right64 > 0x7fffffffU || fit_right64 > right64 || fit->X < full->X || fit->X > right64)
		return 0;
	D3DRECT rects[2];
	DWORD count = 0;
	LONG left = (LONG)full->X, top = (LONG)full->Y;
	LONG right = (LONG)right64, bottom = (LONG)((uint64_t)full->Y + full->Height);
	LONG fit_left = (LONG)fit->X, fit_right = (LONG)fit_right64;
	if (fit_left > left)
		rects[count++] = (D3DRECT){left, top, fit_left, bottom};
	if (fit_right < right)
		rects[count++] = (D3DRECT){fit_right, top, right, bottom};
	if (!count)
		return 0;
	DWORD scissor = FALSE;
	if (FAILED(IDirect3DDevice9_GetRenderState(dev, D3DRS_SCISSORTESTENABLE, &scissor)))
		return 0;
	if (scissor && FAILED(IDirect3DDevice9_SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE)))
		return 0;
	HRESULT hr = clear_orig(dev, count, rects, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
	if (scissor)
		(void)IDirect3DDevice9_SetRenderState(dev, D3DRS_SCISSORTESTENABLE, scissor);
	return SUCCEEDED(hr);
}

static HRESULT STDMETHODCALLTYPE t_scissor(IDirect3DDevice9 *dev, const RECT *r)
{
	if (trace_on && r)
		trace_event("SetScissorRect %ld,%ld %ld,%ld", (long)r->left, (long)r->top, (long)r->right, (long)r->bottom);
	return scissor_orig(dev, r);
}
static dpup_t dpup_orig;
static dipup_t dipup_orig;
static scene_t begin_scene_orig, end_scene_orig;
static int comp_hook(IDirect3DDevice9 *hint);
static HRESULT STDMETHODCALLTYPE t_dpup(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE t, UINT pc, const void *v, UINT st)
{
	if (trace_on)
		trace_draw(dev);
	return dpup_orig(dev, t, pc, v, st);
}
static HRESULT STDMETHODCALLTYPE t_dipup(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT pc, const void *i, D3DFORMAT f, const void *v, UINT st)
{
	if (trace_on)
		trace_draw(dev);
	return dipup_orig(dev, t, mi, nv, pc, i, f, v, st);
}
static HRESULT STDMETHODCALLTYPE t_set_rt(IDirect3DDevice9 *dev, DWORD i, IDirect3DSurface9 *s)
{
	if (trace_on && i == 0) {
		const char *d = trace_surf(dev, s);
		trace_event("SetRenderTarget %s", d);
		if (strstr(d, "BACKBUFFER") && !bb_pass_done)
			bb_pass = 1, bb_draws = 0;
	}
	return set_rt_orig(dev, i, s);
}
static void *volatile p2_fd; // mode 11, exp11frames: the frame data of player 2's renderer frame (set at its end-frame)
static HRESULT STDMETHODCALLTYPE t_clear(IDirect3DDevice9 *dev, DWORD n, const D3DRECT *r, DWORD f, D3DCOLOR c, float z, DWORD st)
{
	if (trace_on)
		trace_event("Clear flags %lx color %08lx rects %lu", (unsigned long)f, (unsigned long)c, (unsigned long)n);
	// mode 11, one renderer frame per player: player 2's frame begins the picture again (on the PC the frame begin also uploads the frame's dynamic data) and its
	// first clear, made before its viewport is set, would erase player 1's half: it clears player 2's half only (the back end's frame data is at rva 0x4358f58)
	// Player 1's frame has the same first clear, and the viewport it finds is the last one set: player 2's bottom half from the frame before. So player 1's half was
	// never cleared, colour nor depth, and each frame drew only where it was nearer than the old depth: the "smeared" picture of mode 11 (old frames showing through
	// in bands, the bodies most). Each player's frame clears its own half of the back buffer.
	if (mode == 11 && split_on && CFG_ONCE("clearlog11", 0) && GetTickCount() - split_tick > 15000) { // QA: the device clears of a few frames, 15 s into the split: flags, rectangles, viewport, which frame's data the back end holds
		static unsigned cl_n;
		D3DVIEWPORT9 lv = {0};
		IDirect3DDevice9_GetViewport(dev, &lv);
		if (cl_n < 60)
			cl_n++, log_printf("native: clear11 #%u: flags %lx n %lu rect %ld,%ld,%ld,%ld color %08lx | viewport %lu,%lu %lux%lu | fd %p (p2 fd %p) | caller rva %llx", cl_n, (unsigned long)f, (unsigned long)n, r && n ? (long)r[0].x1 : 0, r && n ? (long)r[0].y1 : 0, r && n ? (long)r[0].x2 : 0,
					   r && n ? (long)r[0].y2 : 0, (unsigned long)c, (unsigned long)lv.X, (unsigned long)lv.Y, (unsigned long)lv.Width, (unsigned long)lv.Height, *(void **)(base + M11_BE_FD), p2_fd,
					   (unsigned long long)((uint8_t *)__builtin_return_address(0) - base));
	}
	IDirect3DSurface9 *rt0 = NULL, *bb = NULL;
	const int on_bb = mode == 11 && split_on && p2_fd && n == 0 && (f & D3DCLEAR_TARGET) && CFG_ONCE("exp11clip", 1) &&
			  SUCCEEDED(IDirect3DDevice9_GetRenderTarget(dev, 0, &rt0)) && SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && rt0 == bb;
	if (rt0)
		IDirect3DSurface9_Release(rt0);
	if (bb)
		IDirect3DSurface9_Release(bb);
	if (on_bb) {
		const int w = *(int *)(base + M11_DISP_W), h = *(int *)(base + M11_DISP_H);
		D3DVIEWPORT9 vp, full = {0, 0, (DWORD)w, (DWORD)h, 0.0f, 1.0f};
		if (w > 0 && h > 0 && SUCCEEDED(IDirect3DDevice9_GetViewport(dev, &vp))) {
			const int p2 = *(void **)(base + M11_BE_FD) == p2_fd;
			const D3DRECT rc = {0, p2 ? h / 2 : 0, w, p2 ? h : h / 2};
			IDirect3DDevice9_SetViewport(dev, &full); // a clear's rectangles are clipped to the viewport
			const HRESULT hr = clear_orig(dev, 1, &rc, f, c, z, st);
			IDirect3DDevice9_SetViewport(dev, &vp);
			return hr;
		}
	}
	return clear_orig(dev, n, r, f, c, z, st);
}
static HRESULT STDMETHODCALLTYPE t_stretch(IDirect3DDevice9 *dev, IDirect3DSurface9 *a, const RECT *ra, IDirect3DSurface9 *b, const RECT *rb, D3DTEXTUREFILTERTYPE f)
{
	if (trace_on) {
		char s1[80];
		snprintf(s1, sizeof(s1), "%s", trace_surf(dev, a));
		trace_event("StretchRect %s -> %s", s1, trace_surf(dev, b));
		if (strstr(s1, "BACKBUFFER") && !strstr(trace_surf(dev, b), "1920x540")) {
			trace_on = 0;
			shot_save_now(dev, "tr-copy");
			trace_on = 1;
			bb_pass_done = 1;
			trace_event("(%d draws in the pass before the screen copy)", bb_draws);
		}
	}
	return stretch_orig(dev, a, ra, b, rb, f);
}
static HRESULT STDMETHODCALLTYPE t_update_texture(IDirect3DDevice9 *dev, IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst)
{
	if (bink_d3d_is_active() && dst &&
	    IDirect3DBaseTexture9_GetType(dst) == D3DRTYPE_TEXTURE) {
		D3DSURFACE_DESC desc;
		if (SUCCEEDED(IDirect3DTexture9_GetLevelDesc((IDirect3DTexture9 *)dst, 0, &desc)))
			(void)bink_track_video_texture(dst, &desc);
	}
	return update_texture_orig(dev, src, dst);
}
static HRESULT STDMETHODCALLTYPE t_viewport(IDirect3DDevice9 *dev, const D3DVIEWPORT9 *v)
{
	if (trace_on && v)
		trace_event("SetViewport %lu,%lu %lux%lu", (unsigned long)v->X, (unsigned long)v->Y, (unsigned long)v->Width, (unsigned long)v->Height);
	return viewport_orig(dev, v);
}
static HRESULT STDMETHODCALLTYPE t_dip(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv, UINT si, UINT pc)
{
	if (trace_on)
		trace_draw(dev);
	if (trace_on && CFG_ONCE("traceprims", 0)) { // QA: every indexed draw with its geometry (is a mesh drawn twice with two skins?)
		IDirect3DVertexBuffer9 *vb = NULL;
		UINT off = 0, stride = 0;
		IDirect3DVertexShader9 *vs = NULL;
		IDirect3DDevice9_GetStreamSource(dev, 0, &vb, &off, &stride);
		IDirect3DDevice9_GetVertexShader(dev, &vs);
		trace_event("dip pc %u si %u bv %d nv %u vb %p off %u stride %u vs %p", pc, si, bv, nv, (void *)vb, off, stride, (void *)vs);
		if (vb)
			IDirect3DVertexBuffer9_Release(vb);
		if (vs)
			IDirect3DVertexShader9_Release(vs);
	}
	int movie_quad = bink_is_movie_quad(dev, (UINT)t, pc, "DrawIndexedPrimitive");
	D3DVIEWPORT9 movie_viewport, fitted_viewport;
	int viewport_changed = movie_quad && bink_fit_movie_viewport(dev, &movie_viewport, &fitted_viewport);
	HRESULT hr = dip_orig(dev, t, bv, mi, nv, si, pc);
	if (viewport_changed) {
		HRESULT restore_hr = viewport_orig(dev, &movie_viewport);
		int side_bars = fitted_viewport.Width < movie_viewport.Width;
		int masked = side_bars && SUCCEEDED(hr) && SUCCEEDED(restore_hr) && bink_clear_movie_bars(dev, &movie_viewport, &fitted_viewport);
		if (InterlockedCompareExchange(&bink_fit_logged, 1, 0) == 0) {
			UINT movie_w = (UINT)InterlockedCompareExchange(&bink_movie_width, 0, 0);
			UINT movie_h = (UINT)InterlockedCompareExchange(&bink_movie_height, 0, 0);
			if (masked)
				log_printf("native: Bink 16:9 fit and black side bars applied (%ux%u)", (unsigned)movie_w, (unsigned)movie_h);
			else if (side_bars)
				log_printf("native: Bink 16:9 fit applied; black side bars unavailable (%ux%u)", (unsigned)movie_w, (unsigned)movie_h);
			else
				log_printf("native: Bink 16:9 fit applied (%ux%u)", (unsigned)movie_w, (unsigned)movie_h);
		}
	}
	return hr;
}
static HRESULT STDMETHODCALLTYPE t_dp(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE t, UINT sv, UINT pc)
{
	if (trace_on)
		trace_draw(dev);
	return dp_orig(dev, t, sv, pc);
}
static HRESULT STDMETHODCALLTYPE t_begin(IDirect3DDevice9 *dev)
{
	if (trace_on)
		trace_event("BeginScene");
	return begin_scene_orig(dev);
}
static HRESULT STDMETHODCALLTYPE t_end(IDirect3DDevice9 *dev)
{
	HRESULT result = end_scene_orig(dev);
	if (__atomic_load_n(&present_hooks_pending, __ATOMIC_RELAXED) && comp_hook(dev))
		InterlockedExchange(&present_hooks_pending, 0);
	if (trace_on)
		trace_event("EndScene");
	return result;
}
static void trace_install(IDirect3DDevice9 *dev)
{
	void **vt = *(void ***)dev;
	patch_slot(&vt[37], (void *)t_set_rt, (void **)&set_rt_orig);
	patch_slot(&vt[43], (void *)t_clear, (void **)&clear_orig);
	patch_slot(&vt[34], (void *)t_stretch, (void **)&stretch_orig);
	patch_slot(&vt[47], (void *)t_viewport, (void **)&viewport_orig);
	patch_slot(&vt[82], (void *)t_dip, (void **)&dip_orig);
	patch_slot(&vt[81], (void *)t_dp, (void **)&dp_orig);
	patch_slot(&vt[83], (void *)t_dpup, (void **)&dpup_orig);
	patch_slot(&vt[84], (void *)t_dipup, (void **)&dipup_orig);
	patch_slot(&vt[75], (void *)t_scissor, (void **)&scissor_orig);
	patch_slot(&vt[41], (void *)t_begin, (void **)&begin_scene_orig);
	if (!end_scene_orig)
		patch_slot(&vt[42], (void *)t_end, (void **)&end_scene_orig);
	trace_dev = dev;
}
static int bink_fit_install(IDirect3DDevice9 *dev)
{
	if (!dev || !*(void ***)dev)
		return 0;
	void **vt = *(void ***)dev;
	int ok = 0;
	ok += patch_slot(&vt[31], (void *)t_update_texture, (void **)&update_texture_orig);
	ok += patch_slot(&vt[43], (void *)t_clear, (void **)&clear_orig);
	ok += patch_slot(&vt[47], (void *)t_viewport, (void **)&viewport_orig);
	ok += patch_slot(&vt[82], (void *)t_dip, (void **)&dip_orig);
	if (ok > 0)
		bink_fit_device = dev;
	return ok;
}
// called at each Present of the render thread: in the split screen the frame after [native] tracepops (600) presents is traced
static void trace_present(void)
{
	static int armed = -1;
	if (armed < 0)
		armed = cfg("d3dtrace", 0);
	if (!armed || !trace_dev)
		return;
	if (trace_on) {
		trace_event("Present");
		shot_save_now(trace_dev, "tr-end");
		trace_on = 0;
		{
			char path[MAX_PATH];
			snprintf(path, sizeof(path), "%s", settings_path);
			char *slash = strrchr(path, '\\');
			if (slash)
				strcpy(slash + 1, "d3dtrace.txt");
			FILE *f = fopen(path, "wb");
			if (f) {
				fwrite(trace_buf, 1, trace_len, f);
				fclose(f);
			}
			log_printf("native: D3D TRACE of one frame written to %s (%d bytes)", path, trace_len);
		}
		armed = 0;
	} else if (mode == 11) { // the frame after tracepops presents of the split screen
		static int n11;
		if (split_on && ++n11 == cfg("tracepops", 600)) {
			trace_len = 0, trace_draws = 0, bb_pass = 0, bb_pass_done = 0, bb_draws = 0;
			trace_on = 1;
		}
	}
}


// [native] weaponfov on the native renderers (src/dx/natgfx.c ng_weapon_viewport): tan(view fov / 2) / tan(weapon fov / 2), 0 when the weapon keeps the game's field of view
float native_weapon_k(void);
float native_weapon_k(void)
{
	return 0; // (off: cg_weaponFov, src/weaponfov.c, scales the weapon's own projection, which the native renderers take from the game too; the old depth range test
		  // of ng_weapon_viewport took the world's range for the weapon's)
}

#ifdef _WIN64
// Mode 11 on the native renderers. Player 1's renderer frame ends with the show bit for the back end (its post-process composite and its HUD), but it is not presented
// (be_end_hook, exp11p1show): natboot's frame end (w_45060) leaves out the present and the console / QA work of a shown frame while this says so.
int native_nat_frame_hidden(void);
int native_nat_frame_hidden(void)
{
	return be_skip_present;
}
// A clear of the screen stays in the half of the renderer frame being drawn (t_clear's exp11clip on Direct3D 9): player 2's frame (its frame data, rva 0x4358f58, is p2_fd)
// clears the bottom half, player 1's the top. Without this player 2's frame start cleared the whole screen: the top half was black on Direct3D 11 / 12 (2026-11-16).
int native_nat_clear_rect(unsigned flags, long rect[4]);
int native_nat_clear_rect(unsigned flags, long rect[4])
{
	if (mode != 11 || !split_on || !p2_fd || !(flags & 1) || !CFG_ONCE("exp11clip", 1))
		return 0;
	const int w = *(int *)(base + XR64(0x4113728)), h = *(int *)(base + XR64(0x411372c));
	if (w <= 0 || h <= 0)
		return 0;
	const int p2 = *(void **)(base + XR64(0x4358f58)) == p2_fd;
	rect[0] = 0, rect[1] = p2 ? h / 2 : 0, rect[2] = w, rect[3] = p2 ? h : h / 2;
	static volatile LONG logged[2];
	if (InterlockedIncrement(&logged[p2]) == 1)
		log_printf("native: mode 11 on the native renderer: player %d's frame clears the screen in its half only (%ld..%ld of %d)", p2 + 1, rect[1], rect[3], h);
	return 1;
}
#endif

// the native frame end (natframe2.c ngfr_453c0) calls the frame's 2D command list (rva 0x5c010) through this
// The frame's 2D command list (rva 0x5c010: the HUD, texts, fades), called by the native renderers. With an upscaled picture the upscale is made here, when the 2D starts,
// and the 2D draws at the output resolution. Until 2026-11-18 it was made earlier, at the scene's last pass (rva 0x63680, called at 0x5c5bf), which draws the late 3D
// effects with the depth test on (dust, smoke, sparks; MW2COOP_TRACEFRAME: all its draws have z 1, the 2D list's have z 0): those then went to the output-sized picture
// with no depth buffer and were drawn over everything nearer (owner: the partner looked see-through in the dust, only with an upscaler), and the view's post passes after
// that pass (the glow) went into the 1280x720 picture after it had been upscaled.
void native_final2d_entry(void *vp);
void native_final2d_entry(void *vp)
{
#ifdef _WIN64 // (the native renderers are 64-bit only)
	{ extern int nd_trace; if (nd_trace) log_printf("trace: ==== final2d begin"); }
	{ extern int ngfr_trace_scene; static LONG f2; if (ngfr_trace_scene && (InterlockedIncrement(&f2) % 50) == 1) log_printf("native: final2d reached #%ld vp %p", (long)f2, vp); }
#endif
	if (natboot_hud_begin()) { // upscaled picture: the 2D list (fades, texts) is drawn at the output resolution
		((void (*)(void *))(base + XR64(0x5c010)))(vp);
		natboot_hud_end();
		return;
	}
	((void (*)(void *))(base + XR64(0x5c010)))(vp);
}

// the native renderers' movie letterboxing (src/dx/natfilm.h): the size of the Bink movie being played
int native_nat_film_size(unsigned *w, unsigned *h);
int native_nat_film_size(unsigned *w, unsigned *h)
{
	*w = (unsigned)InterlockedCompareExchange(&bink_movie_width, 0, 0);
	*h = (unsigned)InterlockedCompareExchange(&bink_movie_height, 0, 0);
	return *w && *h;
}


static DWORD comp_not_before; // GetTickCount: the device hooks are not (re)installed before it (after a vid_restart the Steam overlay hooks the new device first)

static int comp_hook(IDirect3DDevice9 *hint)
{
	if (InterlockedCompareExchange(&comp_hook_busy, 1, 0) != 0)
		return 0;
	if (the_device) {
		InterlockedExchange(&comp_hook_busy, 0);
		return 1;
	}
	/* After vid_restart, let the Steam overlay install its hooks on the new device
	   before wrapping Present again. The EndScene callback retries until this gate opens. */
	if (GetTickCount() < comp_not_before) {
		InterlockedExchange(&comp_hook_busy, 0);
		return 0;
	}
#ifdef _WIN64
	IDirect3DDevice9 *dev = hint ? hint : (base ? *(IDirect3DDevice9 **)(base + RVA_D3D_DEVICE) : NULL);
#else
	IDirect3DDevice9 *dev = hint ? hint : (IDirect3DDevice9 *)d3dcap_device();
#endif
	if (!dev) {
		InterlockedExchange(&comp_hook_busy, 0);
		return 0;
	}
	void **vt = *(void ***)dev;
	if (!vt) {
		InterlockedExchange(&comp_hook_busy, 0);
		return 0;
	}
	{ static LONG once; if (InterlockedCompareExchange(&once, 1, 0) == 0) log_printf("native: comp_hook on device %p, vt[17] %p vt[42] (EndScene) %p, the_device %p", (void *)dev, vt[17], vt[42], (void *)the_device); }
	if (!present_base_orig && vt[17] != (void *)present_hook)
		present_base_orig = (present_t)vt[17];
	if (!patch_slot(&vt[17], (void *)present_hook, (void **)&present_orig)) {
		InterlockedExchange(&comp_hook_busy, 0);
		return 0;
	}
	if (!present_orig)
		present_orig = present_base_orig;
	if (!present_orig) {
		InterlockedExchange(&comp_hook_busy, 0);
		return 0;
	}
	the_device = dev;
	// (the weapon's own field of view is cg_weaponFov now, src/weaponfov.c: a hook on the game's weapon projection, not on the device)
	if (cfg("resethook", 1))
		patch_slot(&vt[16], (void *)reset_hook, (void **)&reset_orig);
	IDirect3DDevice9Ex *ex = NULL;
	if (SUCCEEDED(IDirect3DDevice9_QueryInterface(dev, &iid_device9ex, (void **)&ex)) && ex) {
		if (!present_ex_base_orig && vt[121] != (void *)present_ex_hook)
			present_ex_base_orig = (present_ex_t)vt[121];
		patch_slot(&vt[121], (void *)present_ex_hook, (void **)&present_ex_orig);
		if (cfg("resethook", 1))
			patch_slot(&vt[132], (void *)reset_ex_hook, (void **)&reset_ex_orig);
		IDirect3DDevice9Ex_Release(ex);
	}
	IDirect3DSwapChain9 *sc = NULL;
	if (SUCCEEDED(IDirect3DDevice9_GetSwapChain(dev, 0, &sc)) && sc) {
		void **svt = *(void ***)sc;
		if (!sc_present_base_orig && svt[3] != (void *)sc_present_hook)
			sc_present_base_orig = (sc_present_t)svt[3];
		patch_slot(&svt[3], (void *)sc_present_hook, (void **)&sc_present_orig);
		IDirect3DSwapChain9_Release(sc);
	}
	{
		HMODULE m1 = NULL, m2 = NULL;
		char n1[MAX_PATH] = "?", n2[MAX_PATH] = "?";
		if (present_orig)
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)present_orig, &m1);
		if (sc_present_orig)
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)sc_present_orig, &m2);
		if (m1)
			GetModuleFileNameA(m1, n1, sizeof(n1));
		if (m2)
			GetModuleFileNameA(m2, n2, sizeof(n2));
		log_printf("native: device Present in %s, swap chain Present in %s", n1, n2);
	}
	if (cfg("d3dtrace", 0))
		trace_install(dev);
	log_printf("native: Present hooks set (device %p, Ex %s)", (void *)dev, present_ex_orig ? "yes" : "no");
	InterlockedExchange(&present_hooks_pending, 0);
	InterlockedExchange(&comp_hook_busy, 0);
	return 1;
}

int native_present_hooks(void)
{
	IDirect3DDevice9 *dev;
	void **vt;
	if (InterlockedCompareExchange(&comp_hook_busy, 1, 0) != 0)
		return 0;
	if (the_device) {
		InterlockedExchange(&comp_hook_busy, 0);
		return 1;
	}
#ifdef _WIN64
	dev = base ? *(IDirect3DDevice9 **)(base + RVA_D3D_DEVICE) : NULL;
#else
	dev = (IDirect3DDevice9 *)d3dcap_device();
#endif
	if (!dev) {
		InterlockedExchange(&comp_hook_busy, 0);
		return 0;
	}
	vt = *(void ***)dev;
	if (!vt) {
		if (InterlockedCompareExchange(&present_bootstrap_failure_logged, 1, 0) == 0)
			log_printf("native: EndScene bootstrap failed: device %p has no vtable", (void *)dev);
		InterlockedExchange(&comp_hook_busy, 0);
		return 0;
	}
	if (vt[42] != (void *)t_end && !patch_slot(&vt[42], (void *)t_end, (void **)&end_scene_orig)) {
		if (InterlockedCompareExchange(&present_bootstrap_failure_logged, 1, 0) == 0)
			log_printf("native: EndScene bootstrap patch failed (device %p, slot %p, error %lu)",
				   (void *)dev, (void *)&vt[42], (unsigned long)GetLastError());
		InterlockedExchange(&comp_hook_busy, 0);
		return 0;
	}
	if (vt[42] != (void *)t_end || !end_scene_orig) {
		if (InterlockedCompareExchange(&present_bootstrap_failure_logged, 1, 0) == 0)
			log_printf("native: EndScene bootstrap could not be installed (device %p, slot %p, original %p)",
				   (void *)dev, (void *)&vt[42], (void *)end_scene_orig);
		InterlockedExchange(&comp_hook_busy, 0);
		return 0;
	}
	InterlockedExchange(&present_bootstrap_failure_logged, 0);
	InterlockedExchange(&present_hooks_pending, 1);
	InterlockedExchange(&comp_hook_busy, 0);
	return 1;
}

void native_present_poll(void)
{
	static DWORD pending_since;
	static int timeout_logged;
	DWORD now = GetTickCount();
	if (!InterlockedCompareExchange(&present_hooks_pending, 0, 0)) {
		pending_since = 0;
		timeout_logged = 0;
		return;
	}
	if (!pending_since)
		pending_since = now ? now : 1;
	if (!timeout_logged && now - pending_since >= 2000) {
		log_printf("native: EndScene bootstrap is still waiting for the render thread after %lu ms; re-arming the callback",
			   (unsigned long)(now - pending_since));
		timeout_logged = 1;
		(void)native_present_hooks();
	}
}

// [native] weaponfov / weaponfovlog in the plain game: request the EndScene bootstrap
// once the device exists; Present itself is installed later from the render thread.
static DWORD WINAPI device_hooks_thread(LPVOID unused)
{
	(void)unused;
	for (int i = 0; i < 600; i++) {
#ifdef _WIN64
		if (base && *(void **)(base + RVA_D3D_DEVICE)) {
#else
		if (d3dcap_device()) { // (32 bits: started only after a vid_restart, below)
#endif
			Sleep(2000); // let the game and Steam overlay finish setting up the device
			(void)native_present_hooks();
			return 0;
		}
		Sleep(500);
	}
	return 0;
}

// Cinematics use the captured D3D9 device on all supported clients. Keep this bootstrap
// separate from device_hooks_thread(), which remains the existing weapon-FOV/Present path.
static DWORD WINAPI bink_device_hooks_thread(LPVOID unused)
{
	(void)unused;
	for (int i = 0; i < 600; i++) {
		IDirect3DDevice9 *dev = (IDirect3DDevice9 *)d3dcap_device();
		if (dev) {
			Sleep(2000); // let Steam and graphics overlays finish wrapping the device
			dev = (IDirect3DDevice9 *)d3dcap_device();
			if (dev && cfg("filmfit", 1)) {
				int installed = bink_fit_install(dev);
				if (installed != 4)
					log_printf("native: Bink aspect fit unavailable: D3D hooks incomplete (%d/4)", installed);
			} else if (!dev) {
				log_printf("native: Bink aspect fit unavailable: D3D9 device changed during hook setup");
			}
			return 0;
		}
		Sleep(100);
	}
	log_printf("native: Bink aspect fit unavailable: no D3D9 device captured");
	return 0;
}

// vid_restart (and alt+enter, which runs it) destroys the D3D device and makes a new one. Our hooks sit on the d3d9 vtable slots; the Steam overlay hooks the new device again,
// finds ours where its own should be and the two call each other until the stack is gone (0xc00000fd inside gameoverlayrenderer64, v0.6.0 report; reproduced here with a
// split session). The same happens at the start when the hooks go in too early (see console.c), so the way out is the same: the hooks are taken out before the command
// runs and put in again once the new device has been up for a while. The command's function pointer is in its table entry (rva 0x5dae38 + 0x20, the function 0xd51d0).
#ifdef _WIN64
static void (*vid_restart_original)(void);
static void vid_restart_hook(void)
{
	// The engine restores the level after a vid_restart from a save it makes itself; a co-op session saves nothing (progress.c), so the reload ends in the engine's error "Trying to read
	// corrupted file" and the players are thrown out to the menu (checked with a split session, v0.6.0 report: alt+enter). A running co-op level therefore does not restart its video
	// ([native] vidrestart=1 lets it through); in the menus it is as before.
	if (!cfg("vidrestart", 0) && *(volatile int *)(base + XR(0x533cb8)) == 6) { // RVA_CLIENT_STATE: 6 = a level is running
		char coop[8] = "";
		game_dvar_string("coop", coop, sizeof(coop));
		if (coop[0] == '1') {
			log_printf("native: vid_restart ignored: a co-op level is running (the engine could not restore it; change the video settings in the main menu)");
			return;
		}
	}
	if (getenv("MW2COOP_VIDPLAIN")) { // QA: the game's own vid_restart, none of the mod's preparations (to see which of them a failure belongs to)
		log_printf("native: vid_restart: plain (MW2COOP_VIDPLAIN)");
		vid_restart_original();
		return;
	}
	zones_pools_reset(); // the restart initialises the asset database again: the raised pool sizes of the co-op levels must not be what it builds from
	const int had_present_hooks = the_device != NULL;
	if (the_device || bink_fit_device) {
		log_printf("native: vid_restart: the device hooks are taken out, they go back on the new device");
		slots_take_out();
		slots_forget_out();
		the_device = NULL;
		bink_fit_device = NULL;
		comp_not_before = GetTickCount() + 4000;
	}
	vid_restart_original();
	if (cfg("filmfit", 1)) {
		HANDLE t = CreateThread(NULL, 0, bink_device_hooks_thread, NULL, 0, NULL);
		if (t)
			CloseHandle(t); // (one handle a vid_restart was never closed)
	}
	// The Present hooks (the console, FSR 3, weaponfov, QA shots) were taken out above; outside split screen nothing asked for them again, and after a
	// resolution change or Alt+Enter the console never drew again (audit 2026-11-18, native 4). The same worker as at the start puts them on the
	// new device once it exists.
	if (had_present_hooks) {
		HANDLE t = CreateThread(NULL, 0, device_hooks_thread, NULL, 0, NULL);
		if (t)
			CloseHandle(t);
	}
}
static void vid_restart_install(void)
{
	void **entry = (void **)(base + XR(0x5dae38) + 0x20);
	DWORD old;
	if (*entry != (void *)(base + XR(0xd51d0))) {
		log_printf("native: vid_restart not hooked (the command table entry differs)");
		return;
	}
	if (!VirtualProtect(entry, sizeof(void *), PAGE_READWRITE, &old))
		return;
	vid_restart_original = (void (*)(void))*entry;
	*entry = (void *)vid_restart_hook;
	VirtualProtect(entry, sizeof(void *), old, &old);
}
#else
// 32 bits: the same guard on the server command's node (VA 0x931e8c, registered by 0x4f3480; handler 0x4e5110). The engine restores the level after the restart from a save
// it makes itself, and the official 32-bit exe quits with 0x8000dead whenever a save is loaded with the mod in (saves.c): in any running level it is refused there, in a
// co-op level on the iw4x-sp client too, as on 64 bits (an Alt+Enter in a level was a crash; Vlad's 32-bit vid_restart report, 2026-11-03).
#define REF_SV_CMD_VID_RESTART 0x531e8c
#define REF_VID_RESTART_FN     0xe5110
static void (*vid_restart_original)(void);
static void vid_restart_hook(void)
{
	if (!cfg("vidrestart", 0) && *(volatile int *)(base + RVA_CLS_STATE) == 6) { // a level is running
		char coop[8] = "";
		game_dvar_string("coop", coop, sizeof(coop));
		if (coop[0] == '1' || !game_is_iw4x()) {
			log_printf("native: vid_restart ignored: a level is running (%s; change the video settings in the main menu)",
				   coop[0] == '1' ? "a co-op level: the engine could not restore it" : "this exe quits when the engine restores the level from its save");
			return;
		}
	}
	zones_pools_reset(); // the restart initialises the asset database again: the raised pool sizes of the co-op levels must not be what it builds from
	// The device hooks out before the restart and back on the new device afterwards, as on 64 bits (above). Until 2026-11-20 the 32-bit builds left them in: once the
	// console had been opened (its Present hooks go in then), a vid_restart in the menus with the Steam overlay loaded ended in a stack overflow between the overlay's Present
	// and ours (iw4x-sp test: the console opened and closed, then r_aaSamples 4 and vid_restart; a player's report the same day: "error and heavy freezes" after video changes).
	const int had_present_hooks = the_device != NULL;
	if (the_device || bink_fit_device) {
		log_printf("native: vid_restart: the device hooks are taken out, they go back on the new device");
		slots_take_out();
		slots_forget_out();
		the_device = NULL;
		bink_fit_device = NULL;
		comp_not_before = GetTickCount() + 4000;
	}
	vid_restart_original();
	if (cfg("filmfit", 1)) {
		HANDLE t = CreateThread(NULL, 0, bink_device_hooks_thread, NULL, 0, NULL);
		if (t)
			CloseHandle(t);
	}
	if (had_present_hooks) {
		HANDLE t = CreateThread(NULL, 0, device_hooks_thread, NULL, 0, NULL);
		if (t)
			CloseHandle(t);
	}
}
static void vid_restart_install(void)
{
	cmd_function_t *node = (cmd_function_t *)(base + XR(REF_SV_CMD_VID_RESTART));
	if (node->function != (void (*)(void))(base + XR(REF_VID_RESTART_FN))) {
		log_printf("native: vid_restart not hooked (the command table entry differs)");
		return;
	}
	vid_restart_original = node->function;
	node->function = vid_restart_hook;
	log_printf("native: vid_restart guarded in levels");
}
#endif

// Phase A (docs/native-splitscreen-360.md): the static model list builders (rva 0x3ad40, 0x3b810, 0x3be80, 0x3c440) take their output chunks from two regions of the
// renderer's frame data by atomic bumps of two counters that begin-frame resets: frame data + 0xb7f80 + [+0x41964] (0xa000 a chunk) and + 0xd2f80 + [+0x41968] (0x4000).
// The regions hold one view's chunks (the first one has 0x1b000 bytes; two views used up to 0x24000 and wrote into the second). For the second view the counters are
// set so that the bumps land in two buffers of ours, 1 MB each, one pair for each of the two frame data sets, allocated above the frame data (the counter is an unsigned
// 32-bit offset from it).
static void split_smc_redirect(uint8_t *fd)
{
	static uint8_t *fds[2], *bufs[2][2];
	int k = fd == fds[0] ? 0 : fd == fds[1] ? 1 : -1;
	if (k < 0) {
		k = !fds[0] ? 0 : !fds[1] ? 1 : -1;
		if (k < 0)
			return;
		fds[k] = fd;
		for (int r = 0; r < 2; r++) {
			for (uintptr_t at = ((uintptr_t)fd + 0x10000000u + (uintptr_t)(k * 2 + r) * 0x400000u) & ~(uintptr_t)0xffff; !bufs[k][r] && at < (uintptr_t)fd + 0xf0000000u; at += 0x10000)
				bufs[k][r] = VirtualAlloc((void *)at, 0x100000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			log_printf("native: phase A: frame data %p: second view's static model chunks (region %d) at %p", (void *)fd, r, (void *)bufs[k][r]);
		}
	}
	if (!bufs[k][0] || !bufs[k][1])
		return;
	*(uint32_t *)(fd + 0x41964) = (uint32_t)(bufs[k][0] - (fd + 0xb7f80));
	*(uint32_t *)(fd + 0x41968) = (uint32_t)(bufs[k][1] - (fd + 0xd2f80));
}

// The scene set-up (0x8210 -> 0x5160) writes per-scene lighting into frame data fields that exist once (the sun +0x41a48..+0x41a54, the sun shadow flag +0x4195c, +0x41958,
// the light list +0x421e0 / +0x423b0 with the shadow bits +0x41970, ...): with two scenes the last one's won and player 1's view was lit with player 2's values. The block
// +0x41940..+0x62980 is kept after each scene's set-up (one copy per scene and per frame data set) and put back before that scene's 3D pass; the last scene's copy afterwards.
#define FD_SCENE_LO 0x41940
#define FD_SCENE_HI 0x62980
static uint8_t *fd_scene_copy[2][4];
static uint8_t *fd_scene_owner[2];
static uint8_t *fd_scene_glob[2][4];
static int fd_scene_set(uint8_t *fd)
{
	for (int k = 0; k < 2; k++)
		if (fd_scene_owner[k] == fd)
			return k;
	for (int k = 0; k < 2; k++)
		if (!fd_scene_owner[k]) {
			fd_scene_owner[k] = fd;
			return k;
		}
	return -1;
}
static void fd_scene_save(uint8_t *fd, unsigned scene)
{
	if (!fd || scene >= 4 || !CFG_ONCE("scenekeep", 0))
		return;
	const int k = fd_scene_set(fd);
	if (k < 0)
		return;
	if (!fd_scene_copy[k][scene])
		fd_scene_copy[k][scene] = HeapAlloc(GetProcessHeap(), 0, FD_SCENE_HI - FD_SCENE_LO);
	if (fd_scene_copy[k][scene])
		memcpy(fd_scene_copy[k][scene], fd + FD_SCENE_LO, FD_SCENE_HI - FD_SCENE_LO);
	static uint8_t *glob_copy[2][4];
	if (!glob_copy[k][scene])
		glob_copy[k][scene] = HeapAlloc(GetProcessHeap(), 0, 0x400);
	if (glob_copy[k][scene])
		memcpy(glob_copy[k][scene], base + XR64(0x4113400), 0x400); // the view values 0x8210 writes per scene (origin and axes 0x4113434.., fog / LOD / fov 0x41135a4..)
	fd_scene_glob[k][scene] = glob_copy[k][scene];
}
// what a scene's 3D pass reads of its own lighting: +0x41940..+0x41a30 (with the sun shadow flag, +0x41958, the shadow bits +0x41970), the sun +0x41a48..+0x41a58, the light
// list +0x421e0..; the scene list, counters and flags in between (+0x41a30..+0x41a48, +0x41a58..+0x421e0) are the frame's and stay as they are
static const uint32_t fd_scene_parts[][2] = {{0x41940, 0x41a30}, {0x41a48, 0x41a58}, {0x421e0, FD_SCENE_HI}, {0x41f20, 0x421e0}}; // bit 3: the sun shadow cascades (2 x 0x160, read by the cascade pass 0x659a0)
static int fd_scene_load(uint8_t *fd, unsigned scene)
{
	if (!fd || scene >= 4 || !CFG_ONCE("scenekeep", 0))
		return 0;
	for (int k = 0; k < 2; k++)
		if (fd_scene_owner[k] == fd && fd_scene_copy[k][scene]) {
			if ((CFG_ONCE("scenekeepparts", 7) & 16) && fd_scene_glob[k][scene])
				memcpy(base + XR64(0x4113400), fd_scene_glob[k][scene], 0x400);
			for (size_t p = 0; p < sizeof(fd_scene_parts) / sizeof(fd_scene_parts[0]); p++)
				if (CFG_ONCE("scenekeepparts", 7) & (1 << p))
				memcpy(fd + fd_scene_parts[p][0], fd_scene_copy[k][scene] + (fd_scene_parts[p][0] - FD_SCENE_LO), fd_scene_parts[p][1] - fd_scene_parts[p][0]);
			return 1;
		}
	return 0;
}

// The back end's frame (rva 0x45690) draws the 3D of ONE scene, the one at the frame data's "current scene" (+0x41a30, the last scene submitted), at 0x456ee (call 0x5c620),
// then the per-view composites and 2D (0x453c0). With two scenes in a frame (mode 11, both players drawn in it) only the last one got its 3D. Each scene of the frame
// gets its 3D pass here, in order.
static void (*scene3d_original)(void *slot);
static int scene3d_pass = -1; // the scene whose 3D pass runs (several scenes in a frame), -1 otherwise
static void scene3d_hook(void *slot)
{
	uint8_t *scene = *(uint8_t **)(base + XR64(0x4358f58)); // the frame data the back end is drawing
	const unsigned count = scene ? *(unsigned *)(scene + 0x41a34) : 0;
	if (mode != 11 || count < 2 || !CFG_ONCE("allscenes", 1)) {
		scene3d_original(slot);
		return;
	}
	uint8_t *list = *(uint8_t **)(scene + 0x41a38);
	const unsigned current = *(unsigned *)(scene + 0x41a30);
	static uint8_t *entry_copy;
	if (!entry_copy)
		entry_copy = HeapAlloc(GetProcessHeap(), 0, FD_SCENE_HI - FD_SCENE_LO);
	if (entry_copy)
		memcpy(entry_copy, scene + FD_SCENE_LO, FD_SCENE_HI - FD_SCENE_LO); // the frame data as the back end found it: put back after the passes
	int loaded = 0;
	for (unsigned i = 0; i < count && i < 4; i++) {
		loaded |= fd_scene_load(scene, i);
		*(unsigned *)(scene + 0x41a30) = i; // the pass reads the "current scene" for some of its steps (0x4df70, 0x4e030)
		scene3d_pass = i;
		scene3d_original(list + (size_t)i * 0x13a0);
	}
	scene3d_pass = -1;
	if (loaded && entry_copy)
		memcpy(scene + FD_SCENE_LO, entry_copy, FD_SCENE_HI - FD_SCENE_LO);
	*(unsigned *)(scene + 0x41a30) = current;
}

// The pass's clear (0x5c484, call 0x51070): after its target switch the viewport is the whole target, so the second view's clear wiped the first view. The first pass clears the
// whole picture (both views' rectangles); the later ones do not clear.
static void (*scene_clear_original)(void *, int, void *, float, int);
static void scene_clear_hook(void *dev, int flags, void *rgba, float depth, int stencil)
{
	if (scene3d_pass > 0) {
		if (CFG_ONCE("clearred", 0)) { // QA: the clear of the second scene is drawn as pure red, to see whether it covers a view
			const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
			scene_clear_original(dev, flags, (void *)red, depth, stencil);
			return;
		}
		return;
	}
	if (CFG_ONCE("clearlog", 0)) { // QA: which scene's clear runs, and over what
		static unsigned n;
		if (n++ < 24) {
			uint8_t *fd = *(uint8_t **)(base + XR64(0x410fd00));
			const float *c = (const float *)rgba;
			(void)dev;
			log_printf("native: [x%d] clear scene %d (current %d) flags 0x%x rgba %.3f %.3f %.3f %.3f depth %g | view rect %.3f %.3f %.3f %.3f | cg view %.3f %.3f %.3f %.3f",
				   (int)(GetCurrentThreadId() & 0xff), scene3d_pass < 0 ? -1 : scene3d_pass, fd ? *(int *)(fd + 0x41a30) : -1, flags, c ? (double)c[0] : 0.0, c ? (double)c[1] : 0.0, c ? (double)c[2] : 0.0,
				   c ? (double)c[3] : 0.0, (double)depth, (double)((float *)(base + XR(0x3bd808)))[0], (double)((float *)(base + XR(0x3bd808)))[1],
				   (double)((float *)(base + XR(0x3bd808)))[2], (double)((float *)(base + XR(0x3bd808)))[3], (double)((float *)(base + XR(0x43c630)))[0],
				   (double)((float *)(base + XR(0x43c630)))[1], (double)((float *)(base + XR(0x43c630)))[2], (double)((float *)(base + XR(0x43c630)))[3]);
		}
	}
	scene_clear_original(dev, flags, rgba, depth, stencil);
}

// The renderer's local player (rva 0x51deb8, written by CG_DrawActiveFrame 0xbca00 from its argument; the 360's 0x8244b424, set by its CG_DrawActiveFrame 0x82113110).
// The scene set-up (0x5160) and its helpers (0x4780, 0x17740, 0x19000, 0x1b9b0, 0x1bee0, 0x38cc0) keep per-player view data in a block that still has room for 4 players
// (0x4812878 + lc * 0x2b8: sun shadow and cull planes), and run the once-per-frame steps only for player 0 (0xdb4d0 "lc == 0": the effects' time step 0x208a10).
// Mode 11 draws player 2 through the cgame as player 0 (the PC client arrays have one element), so both scenes shared player 0's slot and the effects stepped
// twice a frame. As on the 360, player 2's R_RenderScene runs as renderer player 1; the two cgame callbacks inside it that index one-element cgame arrays
// (0xbd3d0 shellshock at 0x82dd, 0x93ac0 at 0x6356: 0x51e330 + lc * 0x2cd0) still get 0. [native] exp11lc=0 turns it off.
static void (*render_scene_original)(void *refdef, uintptr_t scene, uintptr_t c, uintptr_t d); // R_RenderScene(refdef, scene number): every register argument passed on
static void (*rs_shock_original)(uintptr_t lc, uintptr_t b, uintptr_t c, uintptr_t d);
static void (*rs_cgents_original)(uintptr_t lc, uintptr_t b, uintptr_t c, uintptr_t d);
static int rs_player1; // inside player 2's R_RenderScene with the renderer's local player at 1
static int p2_frame; // exp11frames: player 2's draw is under way in its own renderer frame
#ifdef _WIN64 // (natframe2.c is 64-bit only)
extern int ng_skip_reset_2d; // natgfx.c (one flag for the Direct3D 11 and 12 copies of natframe2.c): the second frame of an engine frame does not reset the 2D state again
#endif
static unsigned end_frame_flags; // exp11frames: the flags the screen update's end-frame call (0xd5aaf) uses instead of 3, once
static void (*end_frame_original)(unsigned flags);
static void end_frame_hook(unsigned flags)
{
	if (end_frame_flags) {
		flags = end_frame_flags;
		end_frame_flags = 0;
		p2_frame = 0;
#ifdef _WIN64
		ng_skip_reset_2d = 0;
#endif
		p2_fd = *(void **)(base + M11_FD_CUR);
		static int nlog2;
		if (nlog2 < 4) {
			nlog2++;
			log_printf("native: exp11frames: p2 end-frame %u (fd %p) ...", flags, *(void **)(base + M11_FD_CUR));
			end_frame_original(flags);
			log_printf("native: exp11frames: p2 end-frame done");
			return;
		}
	}
	end_frame_original(flags);
}
static void ctx_wait_workers(void);
static int rt_frames_done(void) // the render thread has no frame in hand (rva 0x1429b5c: frames handed over by 0x204230 and not finished)
{
	return *(volatile int *)(base + M11_RT_FRAMES) <= 0;
}
// Waits until the predicate holds: on 64 bits the engine's own wait, which helps the front end's workers meanwhile (M11_RT_WAIT). The 32-bit one (VA 0x52afc0) takes the
// predicate in ESI and has another shape in the iw4x-sp exe: the predicate is polled here instead, as render_wait_idle does (200 ms at most; the workers are threads of their own).
static void m11_rt_wait(int (*pred)(void))
{
#ifdef _WIN64
	((void (*)(int (*)(void)))(base + M11_RT_WAIT))(pred);
#else
	// The engine's own wait, as on 64 bits: it runs the workers' jobs on this thread meanwhile, so the render thread's frame ends sooner. Polling the counter instead
	// (until 2026-11-17) left 13 % of these waits at 8 ms or more and up to 47 ms, the frames up to 60 ms (Second Sun, owner: "the sound lagged at the mission's start").
	// The official exe's (VA 0x52afc0) takes the predicate in ESI; the iw4x-sp exe's (rva 0x12b6e0) one stack argument more, 1 where its end-frame calls it.
	const DWORD t0 = GetTickCount();
	void *fn = base + XR(0x12afc0);
	if (game_is_iw4x())
		__asm__ volatile("pushl $1\n\tcall *%1\n\taddl $4, %%esp" : "+S"(pred) : "r"(fn) : "eax", "ecx", "edx", "memory", "cc");
	else
		__asm__ volatile("call *%1" : "+S"(pred) : "r"(fn) : "eax", "ecx", "edx", "memory", "cc");
	{ // how long these waits take (the main thread stands meanwhile: the sound and the input wait with it), logged every 10 s
		static DWORD at, worst;
		static unsigned n, slow;
		const DWORD d = GetTickCount() - t0;
		n++;
		if (d >= 8)
			slow++;
		if (d > worst)
			worst = d;
		if (GetTickCount() - at >= 10000) {
			if (at)
				log_printf("native: mode 11 waits for the render thread: %u in 10 s, %u of them 8 ms or longer, the longest %lu ms", n, slow, (unsigned long)worst);
			at = GetTickCount(), n = slow = 0, worst = 0;
		}
	}
#endif
}
static int trace_on_now(void);
static void body_rec_log(const char *when) // QA (traceprims): the scene records of the player bodies (entities 0, 1) in the traced frame: index, state (+0x28: skin offset + 4), DObj
{
	if (!CFG_ONCE("traceprims", 0) || !trace_on_now())
		return;
	const unsigned count = *(const unsigned *)(base + XR64(0x40dd754));
	for (unsigned i = 0; i < count && i < 0x200; i++) {
		const uint8_t *r = base + XR64(0x40dd760) + (size_t)i * 0x90;
		const unsigned ent = (*(const unsigned *)(r + 0x70) >> 7) & 0xfff;
		if (ent <= 1)
			log_printf("native: bodyrec %s: rec %u ent %u state %u dobj %p", when, i, ent, *(const unsigned *)(r + 0x28), *(void *const *)(r + 0x78));
	}
	unsigned st[6] = {0};
	for (unsigned i = 0; i < count && i < 0x200; i++) {
		const unsigned v = *(const unsigned *)(base + XR64(0x40dd760) + (size_t)i * 0x90 + 0x28);
		st[v < 5 ? v : 5]++;
	}
	log_printf("native: bodyrec %s: %u records, states 0:%u 1:%u 2:%u 3:%u 4:%u skinned(>4):%u | scene list start %u count %u", when, count, st[0], st[1], st[2], st[3], st[4], st[5],
		   *(const unsigned *)(base + XR64(0x40f09e4)), *(const unsigned *)(base + XR64(0x40f09e8)));
}
static void rs_log(const char *when)
{
#ifdef _WIN64 // (QA: the frame data's 64-bit fields)
	static int n;
	if (n >= 12 || !CFG_ONCE("rslog", 0))
		return;
	n++;
	uint8_t *fd = *(uint8_t **)(base + XR64(0x410fd00));
	if (!fd)
		return;
	const uint64_t *e = (const uint64_t *)(fd + 0x41a68);
	log_printf("native: rslog %s: lc %d second %d | spot lights %u | entry0 %llx %llx %llx %llx | scenes %u views %u | spot dvar %d",
		when, *(int *)(base + XR(0x51deb8)), second_pass, *(unsigned *)(fd + 0x41ea8), e[0], e[1], e[2], e[3],
		*(unsigned *)(fd + 0x41a34), *(unsigned *)(fd + 0x419e8), *(int *)(*(uint8_t **)(base + XR64(0x4115fe0)) + 0x10));
#else
	(void)when;
#endif
}
static void render_scene_hook(void *refdef, uintptr_t scene, uintptr_t c, uintptr_t d)
{
	if (mode == 11 && p2_frame && CFG_ONCE("exp11noscene", 0)) // QA: player 2's cgame runs, his scene is not built
		return;
	if (mode != 11 || !(second_pass || p2_frame) || !CFG_ONCE("exp11lc", 1)) {
		render_scene_original(refdef, scene, c, d);
		rs_log("after p1");
		if (CFG_ONCE("traceprims", 0) && trace_on_now()) {
			ctx_wait_workers();
			body_rec_log("p1");
		}
		return;
	}
	rs_log("before p2");
	int *rlc = (int *)(base + M11_RLC);
	const int saved = *rlc;
	*rlc = 1;
	rs_player1 = 1;
	render_scene_original(refdef, scene, c, d);
	rs_player1 = 0;
	rs_log("after p2");
	if (CFG_ONCE("exp11lc", 1) == 1)
		ctx_wait_workers(); // the front end's workers of this scene read the renderer's local player too: let them finish with 1 (exp11lc=2: keep 1 until the next frame's draw instead)
	else
		return;
	*rlc = saved;
}
// The shell shock's saved screen ("$savedscreen", docs/32bit-native-map/shock-savedscreen.md). The cgame's shock pass (the call above) writes render commands: a save of
// the screen (type 2: +4 the timer slot, = the local client), a blend of the saved picture over the view (type 0x12: +0x18 the slot; the flashed blend 0x13 has none).
// The back end's save handler copies the WHOLE current target into the one $savedscreen and stamps savedTime[slot] with the view's time. In mode 11 both players' commands
// carried slot 0: player 2's save was skipped as done (the same time), and player 1's copy of the whole back buffer put a stale picture in player 2's half, so his blend
// smeared it over his view (owner's screenshot of the Endgame wake-up). The Xbox 360 gives each player his own slot (his local client) and copies only his rectangle (its
// type 3 handler 0x823a9d30 resolves the player's rect of the target). The same here: player 2's commands get slot 1 (shock_slot_rewrite), and the save copies the view's
// own pixel rectangle (64-bit: the rectangle the save's StretchRect is given; 32-bit: its own copy). [native] shockslot=0 / shocksect=0 off.
static size_t shock_list_used(uint8_t **cmds)
{
#ifdef _WIN64
	const uintptr_t *list = *(uintptr_t **)(base + XR(0x458fab0)); // the frame's command list: +0 the buffer, +8 the bytes used (rva 0xe530 writes the save command)
	*cmds = list ? (uint8_t *)list[0] : NULL;
	return list ? (size_t)list[1] : 0;
#else
	const uint32_t *list = *(uint32_t **)(base + XR(0x1893268)); // VA 0x1c93268: +0 the buffer, +4 the bytes used (VA 0x50eeb0 writes the save command)
	*cmds = list ? (uint8_t *)(uintptr_t)list[0] : NULL;
	return list ? (size_t)list[1] : 0;
#endif
}
static void shock_slot_rewrite(size_t from)
{
	uint8_t *cmds;
	const size_t used = shock_list_used(&cmds);
	static int logged;
	for (size_t at = from; cmds && at + 4 <= used;) { // {u16 type, u16 size} each
		const unsigned type = *(const uint16_t *)(cmds + at), size = *(const uint16_t *)(cmds + at + 2);
		if (size < 4)
			break;
		int *slot = type == 2 && size >= 8 ? (int *)(cmds + at + 4) : type == 3 && size >= 0x18 ? (int *)(cmds + at + 0x14) : type == 0x12 && size >= 0x1c ? (int *)(cmds + at + 0x18) : NULL;
		if (slot) {
			if (logged < 6) {
				logged++;
				log_printf("native: shock: player 2's command type 0x%x gets timer slot 1 (was %d)", type, *slot);
			}
			*slot = 1;
		}
		at += size;
	}
}
static void rs_shock_hook(uintptr_t lc, uintptr_t b, uintptr_t c, uintptr_t d)
{
	uint8_t *cmds;
	const size_t used0 = rs_player1 ? shock_list_used(&cmds) : 0;
	rs_shock_original(rs_player1 ? 0 : lc, b, c, d);
	if (rs_player1 && CFG_ONCE("shockslot", 1))
		shock_slot_rewrite(used0);
}
#ifdef _WIN64
// 64 bits: the save handler (rva 0x41160, the handler table's type 2 entry at rva 0x3705c0) gives its StretchRect (`call [r10+0x110]` at 0x41214, which the native
// renderers turn into their own StretchRect) no rectangles: `xor r8d,r8d` at 0x411d1 and `mov qword [rsp+20h],0` at 0x411ee. Those 38 bytes are rewritten with the same
// instructions plus `mov r8,[rip+x]; mov [rsp+20h],r8`, x a pointer in a page near the exe: 0 (the whole target, as the game does) except while the hook below runs the handler
// for a split view, when it points at the view's pixel rectangle (the 2D state at 0x4358e78: x, y, w, h, set by the back end for the view whose list runs).
static uint64_t *shock_rect_page;
static void (*save_screen_original)(uint8_t **cmd);
static void save_screen_hook(uint8_t **cmd)
{
	if (mode == 11 && native_active && split_on && shock_rect_page) {
		const int *vr = (const int *)(base + XR(0x4358e78));
		if (vr[2] > 0 && vr[3] > 0) {
			RECT *r = (RECT *)(shock_rect_page + 2);
			r->left = vr[0], r->top = vr[1], r->right = vr[0] + vr[2], r->bottom = vr[1] + vr[3];
			shock_rect_page[0] = (uint64_t)(uintptr_t)r;
			static int logged;
			if (logged < 4) {
				logged++;
				log_printf("native: shock: the saved screen copies the view's rectangle %ld %ld %ld %ld (slot %d)", r->left, r->top, r->right, r->bottom, *(const int *)(*cmd + 4));
			}
		}
	}
	save_screen_original(cmd);
	if (shock_rect_page)
		shock_rect_page[0] = 0;
}
static void shock_sect_install(void)
{
	static const uint8_t expect[38] = {0x45, 0x33, 0xc0, 0x48, 0x8b, 0x5c, 0x24, 0x50, 0x66, 0x0f, 0x73, 0xd8, 0x08, 0x4c, 0x8b, 0xcb, 0x66, 0x48, 0x0f, 0x7e, 0xc0,
					   0xc7, 0x44, 0x24, 0x28, 0x02, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x44, 0x24, 0x20, 0x00, 0x00, 0x00, 0x00};
	uint8_t *site = base + XR(0x411d1);
	uint8_t **entry = (uint8_t **)(base + XR(0x3705c0));
	if (!CFG_ONCE("shocksect", 1))
		return;
	if (memcmp(site, expect, sizeof(expect)) || *entry != base + XR(0x41160)) {
		log_printf("native: unexpected code at the saved screen's handler (0x41160 / 0x411d1): the shell shock's saved screen is copied whole (shocksect)");
		return;
	}
	if (!shock_rect_page) {
		shock_rect_page = (uint64_t *)hook_page_near(site); // read-write, never run: only read by the rewritten handler
		if (!shock_rect_page) {
			log_printf("native: no page near the exe for the saved screen's rectangle (shocksect)");
			return;
		}
	}
	shock_rect_page[0] = 0;
	uint8_t code[38];
	memcpy(code, expect + 3, 26); // mov rbx,[rsp+50h]; psrldq xmm0,8; mov r9,rbx; movq rax,xmm0; mov dword [rsp+28h],2
	const int64_t disp = (int64_t)(intptr_t)shock_rect_page - (int64_t)(intptr_t)(site + 33);
	if (disp > INT32_MAX || disp < INT32_MIN)
		return;
	const int32_t d32 = (int32_t)disp;
	code[26] = 0x4c, code[27] = 0x8b, code[28] = 0x05; // mov r8,[rip+d32]
	memcpy(code + 29, &d32, 4);
	code[33] = 0x4c, code[34] = 0x89, code[35] = 0x44, code[36] = 0x24, code[37] = 0x20; // mov [rsp+20h],r8
	DWORD old;
	j_save(site, sizeof(code));
	VirtualProtect(site, sizeof(code), PAGE_EXECUTE_READWRITE, &old);
	memcpy(site, code, sizeof(code));
	VirtualProtect(site, sizeof(code), old, &old);
	FlushInstructionCache(GetCurrentProcess(), site, sizeof(code));
	save_screen_original = (void (*)(uint8_t **))(void *)(base + XR(0x41160));
	j_save((uint8_t *)entry, sizeof(*entry));
	VirtualProtect(entry, sizeof(*entry), PAGE_READWRITE, &old);
	*entry = (uint8_t *)(void *)save_screen_hook;
	VirtualProtect(entry, sizeof(*entry), old, &old);
	log_printf("native: the shell shock's saved screen is copied per view (shocksect)");
}
#else
// 32 bits: the save handler (VA 0x536330) calls its copy helper at VA 0x53636f (VA 0x5340c0: EAX the $savedscreen GfxImage *, two stack words of the back end, the
// second its state: device +0x90, current target +0xb14, the targets' surfaces at VA 0x244a304 + target * 0x14), which StretchRects the whole target. In a split view
// the copy is made here, of the view's pixel rectangle (state - 0x34: x, y, w, h); the 32-bit game draws with its own Direct3D 9 device only.
static void *save_help_target;
void n32_save_help(uint8_t **img, uint32_t a1, uint8_t *state);
__asm__(".text\n"
	".globl _n32_save_help_stub\n"
	"_n32_save_help_stub:\n"
	"	pushl 8(%esp)\n"
	"	pushl 8(%esp)\n"
	"	pushl %eax\n"
	"	call _n32_save_help\n"
	"	addl $12, %esp\n"
	"	ret\n");
extern char n32_save_help_stub[];
void n32_save_help(uint8_t **img, uint32_t a1, uint8_t *state)
{
	if (mode == 11 && native_active && split_on && CFG_ONCE("shocksect", 1) && img && state) {
		const int *vr = (const int *)(state - 0x34); // (VA 0x2163b1c with the state at VA 0x2163b50 on the official build; one struct, so the iw4x-sp exe needs no address of it)
		IDirect3DDevice9 *dev = *(IDirect3DDevice9 **)(state + 0x90);
		const int cur = *(const int *)(state + 0xb14);
		IDirect3DSurface9 *src = cur >= 0 && cur < 16 ? *(IDirect3DSurface9 **)(base + XR(0x204a304) + cur * 0x14) : NULL;
		IDirect3DTexture9 *tex = *(IDirect3DTexture9 **)img;
		IDirect3DSurface9 *dst = NULL;
		if (vr[2] > 0 && vr[3] > 0 && dev && src && tex && SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(tex, 0, &dst)) && dst) {
			RECT r = {vr[0], vr[1], vr[0] + vr[2], vr[1] + vr[3]};
			const HRESULT hr = IDirect3DDevice9_StretchRect(dev, src, &r, dst, &r, D3DTEXF_LINEAR);
			IDirect3DSurface9_Release(dst);
			static int logged;
			if (logged < 4) {
				logged++;
				log_printf("native: shock: the saved screen copies the view's rectangle %ld %ld %ld %ld (hr %08lx)", r.left, r.top, r.right, r.bottom, (unsigned long)hr);
			}
			if (SUCCEEDED(hr))
				return;
		}
	}
	uint8_t **eax = img;
	__asm__ volatile("pushl %3\n\tpushl %2\n\tcall *%1\n\taddl $8, %%esp" : "+a"(eax) : "r"(save_help_target), "r"(a1), "r"(state) : "ecx", "edx", "memory", "cc");
}
#endif
// The effects clock advances once per engine frame as on the 360 (its 0x82141e70 steps the effects of player 0 only). In mode 11 the second renderer
// frame runs the same cgame step, so 0xbccdf called the step a second time for the same effects system: smoke and muzzle flashes lived one frame ahead
// in the other half (owner's report, 2026-11-15). [native] fxclock=0 off.
static int ctx1_ready; // the second context is live (set when its statics are initialised)
static volatile int p2_cmd_hold; // a new level is loading: context 1 still holds the previous level's state, its commands are not given to the server (native_level_load)
static void (*fx_step_original)(int *ctx, int time);
static void fx_step_hook(int *ctx, int time)
{
	if (mode == 11 && split_on && ctx1_ready && p2_frame && CFG_ONCE("fxclock", 1))
		return;
	fx_step_original(ctx, time);
}
// The animations advance once per engine frame, as the engine has it (2026-11-21). The cgame draw (64 bits rva 0xbca00) steps the animation of every DObj (rva 0x9d8b0:
// the time by the client frame time, notetracks) inside `if (local client == 0)`: the entities of cg.nextSnap's list (call 0xbcb24), then the players' entities 0..1
// (call 0xbcb71). Player 2's view is drawn as local client 0 too, so every animation was stepped twice a frame, ran at twice its speed between two snapshots and was
// set back by the snapshot's copy of the server's state 20 times a second (measured, animleaf: the partner's body in player 2's half advanced in player 1's draw and
// again in player 2's, about 8 frames back at each snapshot). The owner: "his idle animation loses frames, in movement it jitters like the airport's terrorists".
// Measured further (animsteplog): in mode 11 the snapshot's entity list holds both players' entities, so even in one draw player 1's body was stepped twice, by the list
// and by the players' loop (the engine expects the local player's own entity in no list). Each DObj is now stepped at most once per engine frame, whoever calls first
// (anim_frame counts the frames, cg_step_hook; [native] animonce=0 off).
static unsigned anim_frame; // (cg_step_hook: one per engine frame)
static void (*anim_step_original)(void *dobj, uintptr_t notify);
static int anim_step_seen(void *dobj)
{
	static struct {
		void *dobj;
		unsigned frame;
	} seen[2048];
	unsigned h = (unsigned)(((uintptr_t)dobj >> 4) * 2654435761u) & 2047;
	for (int i = 0; i < 32; i++, h = (h + 1) & 2047) {
		if (seen[h].dobj == dobj) {
			if (seen[h].frame == anim_frame)
				return 1;
			seen[h].frame = anim_frame;
			return 0;
		}
		if (seen[h].frame != anim_frame || !seen[h].dobj) { // (a slot of an earlier frame is free)
			seen[h].dobj = dobj;
			seen[h].frame = anim_frame;
			return 0;
		}
	}
	return 0; // (table crowded: step it)
}
static void anim_step_hook(void *dobj, uintptr_t notify)
{
#ifdef _WIN64
	if (CFG_ONCE("animsteplog", 0)) { // QA: the steps of the players' bodies per second: [context][body][site 0xbcb24 snapshot list, 0xbcb71 players]
		static LONG n[2][2][2];
		static DWORD at;
		const int site = __builtin_return_address(0) == (void *)(base + XR(0xbcb76)) ? 1 : 0;
		for (int k = 0; k < 2; k++)
			if (dobj == (void *)((const uint8_t *(*)(int))(base + XR(0x1fece0)))(k))
				n[p2_frame ? 1 : 0][k][site]++;
		if (GetTickCount() - at >= 2000) {
			at = GetTickCount();
			log_printf("native: animsteps 2 s: p1 draw: body0 list %ld players %ld, body1 list %ld players %ld | p2 draw: body0 list %ld players %ld, body1 list %ld players %ld", n[0][0][0], n[0][0][1], n[0][1][0],
				   n[0][1][1], n[1][0][0], n[1][0][1], n[1][1][0], n[1][1][1]);
			memset(n, 0, sizeof(n));
		}
	}
#endif
	static LONG stepped, skipped;
	static DWORD said;
	if (CFG_ONCE("animsteplog", 0) && GetTickCount() - said >= 2000) { // QA: all DObjs: steps made and second steps left out, per 2 s
		said = GetTickCount();
		log_printf("native: animsteps 2 s: %ld steps made, %ld second steps of the same frame left out", stepped, skipped);
		stepped = skipped = 0;
	}
	if (mode == 11 && split_on && ctx1_ready && anim_frame && dobj && CFG_ONCE("animonce", 1) && anim_step_seen(dobj)) {
		skipped++;
		return;
	}
	stepped++;
	anim_step_original(dobj, notify);
}

static void rs_cgents_hook(uintptr_t lc, uintptr_t b, uintptr_t c, uintptr_t d)
{
	rs_cgents_original(rs_player1 ? 0 : lc, b, c, d);
}

// the second draw of the frame must not start a new frame: it adds its view to the one the first draw began
static void begin_frame_hook(void)
{
	if (second_pass)
		return;
	begin_frame_original();
}

static unsigned cg_draw_count, hook_counts[8]; // QA counters: snapshot take, ps publish, server step, usercmd step, snapshot msg, server cmds
static int ctx_on;

// The engine's r_aspectRatio dvar selects a small set of display ratios. The cgame's FOV calculation
// instead needs the actual active render dimensions, including arbitrary window sizes and ultrawide modes.
static int cg_aspect_full_window_value(int width, int height, float *aspect)
{
	if (!aspect || width <= 0 || height <= 0)
		return 0;
	*aspect = (float)width / (float)height;
	return 1;
}
static void cg_aspect_apply_full_window(void)
{
	static int logged_width, logged_height, refused;
	// iw4x-sp: XR() gives 0 for an address tools/gen_addr159.py could not map (the render size is one): base + 0 is the exe's "MZ" header, read
	// as 9460301x9460301, which made the cgame's aspect 1.0 and stretched the view across the window (owner's screenshot, 2026-11-08). Then, or
	// for a size no window has, the game's own aspect stays.
	if (!RVA_RENDER_WIDTH || !RVA_RENDER_HEIGHT || !RVA_CG_ASPECT) {
		if (!refused) {
			refused = 1;
			log_printf("native: full-window aspect correction off: the render size's address is not known in this build");
		}
		return;
	}
	int width = *(int *)(base + RVA_RENDER_WIDTH);
	int height = *(int *)(base + RVA_RENDER_HEIGHT);
	float aspect;
	if (width < 160 || height < 120 || width > 16384 || height > 16384) {
		if (refused != 2) {
			refused = 2;
			log_printf("native: full-window aspect correction skipped: render size %dx%d is not a window size", width, height);
		}
		return;
	}
	if (!cg_aspect_full_window_value(width, height, &aspect))
		return;
	*(float *)(base + RVA_CG_ASPECT) = aspect;
	if (width != logged_width || height != logged_height) {
		logged_width = width;
		logged_height = height;
		log_printf("native: full-window cgame aspect %.4f from %dx%d", (double)aspect, width, height);
	}
}
static int split_on = 1; // the scripts set the dvar mw2coop_split_view once the intro is over; before that there is one picture
static int frame_bottom; // this client frame is context 1's (player 2's client logic, client_frame_inner)
static int ctx1_clc_synced; // ctx1_deliver_commands: the ring of context 1 has been made equal (nothing pending) since its init
static void ctx_swap(int to);
static void ctx_swap_after_p2_draw(void);
static int sun_range_owner(void);
#ifdef _WIN64
static void flip_watch(int ending); // (QA, see below)
#endif
void native_unswap_entities(void);
static void ctx_after_draw(void);
static void ctx_wait_workers(void);

// Diagnostics ([native] scanplace=1, once at the first split frame): where the 2D placement keeps the screen scale. The values the placement holds for a 1920x1080 screen are looked for in the
// game's image: the pair of floats (3.0, 2.25) (real pixels per virtual unit), (1/3, 0.4444), and the int pair (1920, 1080); every hit is logged with the floats around it.
static void scan_place(void)
{
#ifndef _WIN64
	return; // QA scan of the 64-bit placement array
#endif
	static int done;
	if (done || !CFG_ONCE("scanplace", 0))
		return;
	done = 1;
	uint8_t *lo = base, *hi = base + ((IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew))->OptionalHeader.SizeOfImage;
	int hits = 0;
	for (uint8_t *a = lo; a < hi && hits < 60;) {
		MEMORY_BASIC_INFORMATION mbi;
		if (!VirtualQuery(a, &mbi, sizeof(mbi)))
			break;
		uint8_t *end = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
		if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) && !(mbi.Protect & PAGE_GUARD)) {
			for (uint8_t *q = (uint8_t *)mbi.BaseAddress; q + 8 <= end && hits < 60; q += 4) {
				float f0 = *(float *)q, f1 = *(float *)(q + 4);
				int i0 = *(int *)q, i1 = *(int *)(q + 4);
				const char *kind = NULL;
				if (f0 == 3.0f && f1 == 2.25f)
					kind = "scale 3.0 2.25";
				else if (f0 > 0.33332f && f0 < 0.33334f && f1 > 0.44443f && f1 < 0.44446f)
					kind = "inverse 1/3 0.4444";
				else if (i0 == 1920 && i1 == 1080)
					kind = "ints 1920 1080";
				if (kind) {
					hits++;
					char line[700];
					int n = snprintf(line, sizeof(line), "native: scanplace %s at rva 0x%llx:", kind, (unsigned long long)(q - base));
					for (int k = -2; k < 28 && n < (int)sizeof(line) - 24; k++)
						n += snprintf(line + n, sizeof(line) - n, " %g", (double)*(float *)(q + k * 4));
					log_printf("%s", line);
				}
			}
		}
		a = end;
	}
	log_printf("native: scanplace done, %d hits", hits);
}

static int ctx_current; // defined with the context ranges below
static unsigned p0cnt_draws, p0cnt_adds, p0cnt_flags1, p0cnt_flags0; // flags (4th argument) of the last add of entity 0 in context 1 / of entity 1 in context 0
static uint8_t p0dump_slot[2][CENT_SIZE], p0dump_dobj[2][0x200]; // [native] p0cnt=2: the entity slot and the DObj bytes of the other player as each context draws him (0: context 0's entity 1, 1: context 1's entity 0)
static int p0dump_have[2];
static void *p0ptr[6];
static unsigned p0ptr_cnt[6], p0ptr_b8[6], p0ptr_bc[6]; // the distinct DObjs added as entity 0 in context 1 since the last p0cnt line, how often, and their dwords +0xb8 / +0xbc
static void p0dump_diff(const char *what, const uint8_t *a, const uint8_t *b, int n)
{
	char buf[1200];
	int len = 0, cnt = 0;
	for (int i = 0; i + 4 <= n && len < (int)sizeof(buf) - 40; i += 4) {
		uint32_t x = *(const uint32_t *)(a + i), y = *(const uint32_t *)(b + i);
		if (x != y && ++cnt <= 60)
			len += snprintf(buf + len, sizeof(buf) - len, " %x:%x/%x", i, x, y);
	}
	buf[len] = 0;
	log_printf("native: p0dump %s %d dwords differ:%s", what, cnt, buf);
}
static float p0cnt_draw_org[3], p0cnt_draw_org_b[3]; // the two origins the last two adds of entity 0 were given (5th argument and the pose at rdx) // [native] p0cnt=1: context 1's draws, and the adds of entity 0's DObj to its scene, per second
static void cg_draw_hook_inner(unsigned local_client, int a, int b);
static void animleaf_log(const char *where, int ent, const uint8_t *dobj); // (QA, before dobj_add_hook)
#ifdef _WIN64
static double lag_now(void);
// QA ([native] viewjit=<seconds>): from that many seconds after the split, 150 consecutive draws of each context: the player state's origin, pitch and yaw (what the view is built
// from) with the wall clock, to see the frame to frame jitter of a camera at a standstill
static void viewjit_log(int ctx)
{
	static int at = -2;
	static unsigned n[2];
	if (at == -2)
		at = cfg("viewjit", -1);
	if (at < 0 || !split_tick || GetTickCount() - split_tick < (DWORD)at * 1000 || ctx < 0 || ctx > 1 || n[ctx] >= 150)
		return;
	n[ctx]++;
	const uint8_t *ps = base + XR(0x422440);
	log_printf("native: viewjit ctx %d #%u t=%.2f cg.time %d origin %.2f %.2f %.2f pitch %.3f yaw %.3f roll %.3f", ctx, n[ctx], lag_now() * 1000.0, *(int *)(base + XR(0x422434)), (double)*(float *)(ps + 0x1c),
		   (double)*(float *)(ps + 0x20), (double)*(float *)(ps + 0x24), (double)*(float *)(ps + 0x108), (double)*(float *)(ps + 0x10c), (double)*(float *)(ps + 0x110));
}
// QA ([native] vehjit=<seconds after the split began>): the state the cgame's vehicle view (rva 0xbc3e0, the camera of a driven vehicle: snowmobile, boat) left in a context, logged
// before each of that context's next 400 cgame frames: cg time and frame time, the playerstate's vehicle flags and number, the vehicle's entity (current / next origin and the two
// yaw words the view takes its turn rate from), the view's smoothing state (rva 0x431ae0 flag, 0x431ae4 / 0x431ae8 pitch / yaw offsets), the predicted view angles and the client's
// view angle accumulators that CL_SetViewAngles (0xca760) sets from them. Vlad: in the snowmobile races the camera jitters for both players.
static void vehjit_log(int ctx)
{
	static int at = -2;
	static unsigned n[2];
	if (at == -2)
		at = cfg("vehjit", -1);
	if (at < 0 || !split_tick || GetTickCount() - split_tick < (DWORD)at * 1000 || ctx < 0 || ctx > 1 || n[ctx] >= 400)
		return;
	const uint8_t *ps = base + XR(0x422440);
	const int veh = *(const int *)(base + XR(0x4225ac));
	if (veh < 0 || veh >= 2048)
		return;
	n[ctx]++;
	const uint8_t *ce = base + XR(0x441070) + (size_t)veh * 0x1a8;
	const float *acc = (const float *)(base + RVA_ACC_PITCH);
	log_printf("native: vehjit ctx %d #%u t=%.2f cg %d ft %d | flags %08x mode %d veh %d | org %.1f %.1f > %.1f %.1f | a8 %.2f 124 %.2f 9c %.2f | sm %d %.3f %.3f | view %.2f %.2f | acc %.2f %.2f", ctx, n[ctx],
		   lag_now() * 1000.0, *(const int *)(base + XR(0x422434)), *(const int *)(base + XR(0x422430)), *(const unsigned *)(ps + 0xac), *(const int *)(ps + 0x190), veh,
		   (double)*(const float *)(ce + 0x80), (double)*(const float *)(ce + 0x84), (double)*(const float *)(ce + 0xfc), (double)*(const float *)(ce + 0x100),
		   (double)*(const float *)(ce + 0xa8), (double)*(const float *)(ce + 0x124), (double)*(const float *)(ce + 0x9c), *(const uint8_t *)(base + XR(0x431ae0)),
		   (double)*(const float *)(base + XR(0x431ae4)), (double)*(const float *)(base + XR(0x431ae8)), (double)*(const float *)(ps + 0x108), (double)*(const float *)(ps + 0x10c),
		   (double)acc[0], (double)acc[1]);
}
#endif
static int be_end_installed; // (32 bits: the back end's frame is wrapped, be_frame_hook)
static unsigned p1_frame_flags(void)
{
	return be_end_installed && CFG_ONCE("exp11p1show", 1) ? 0x83u : 1u;
}
// The two views of mode 11 as fractions of the picture (x, y, w, h). Default: the top and the bottom half, full width. [native] viewlayout=1 (or 360; the menu row Enhanced > console-style view boxes; read at each split screen start): the console's own boxes, read from its code
// (0x82110a70 registers cg_coopSplitscreenView0X 0, View1X 0.2, ViewWidth 0.8, ViewHeight 0.49; 0x82111cf8 builds view 0 = (0, 0, 0.8, 0.49) and view 1 = (0.2, 1 - 0.49, 0.8, 0.49), the
// pixels fraction * size + 0.5 truncated, the view's aspect (w / h) * the display's): two staggered boxes, top left and bottom right, with black between and around them.
static void exp11_view(int idx, float r[4])
{
	if (CFG_ONCE("viewlayout", 0) != 0) {
		r[0] = idx ? 0.2f : 0.0f, r[1] = idx ? 0.51f : 0.0f, r[2] = 0.8f, r[3] = 0.49f;
	} else {
		r[0] = 0.0f, r[1] = idx ? 0.5f : 0.0f, r[2] = 1.0f, r[3] = 0.5f;
	}
}
static float exp11_aspect(int idx, int sw, int sh)
{
	float r[4];
	exp11_view(idx, r);
	return r[2] / r[3] * (float)sw / (float)sh;
}
static void exp11_pixels(int idx, int sw, int sh, int *cvr)
{
	float r[4];
	exp11_view(idx, r);
	if (CFG_ONCE("viewlayout", 0) != 0) {
		cvr[0] = (int)(r[0] * (float)sw + 0.5f), cvr[1] = (int)(r[1] * (float)sh + 0.5f), cvr[2] = (int)(r[2] * (float)sw + 0.5f), cvr[3] = (int)(r[3] * (float)sh + 0.5f);
	} else {
		cvr[0] = 0, cvr[1] = idx ? sh / 2 : 0, cvr[2] = sw, cvr[3] = idx ? sh - sh / 2 : sh / 2;
	}
}
// Each player's 2D in his own half, the engine's way (2026-11-16, from the 360 and the PC's own placement code; [native] exp11place=0 off). The cgame's 2D code takes its
// screen placement from rva 0xdaf10(lc): while CG_DrawActiveFrame has switched the placements to "per client" (0xdad10, called at 0xbca35) that is the one at rva
// 0x5e0c60, otherwise the full screen one at 0x5e0bf0. The placement is set by 0xdb0b0(placement, x, y, w, h, screen w, screen h) (0xdb060 does it for the whole screen, at the
// video start and restart): scales, the safe area and the viewable rectangles of a viewport at (x, y). The 360 sets its per-client placement to the half of the player whose view
// is drawn; so each player's draw gets the rectangle of his half (player 1 the top, player 2 the bottom) and the 2D he records (HUD, fades, hints) lands in it.
static int place_use_k = 1; // the HUD scale below applies (0: the half's own scale, for scripted HUD elements: see hudelem_hook)
static void place_client_set(int bottom, int whole)
{
	static int on = -1;
	if (on < 0)
		on = cfg("exp11place", 1);
	if (!on)
		return;
	if (exp11_cycle_state() == 1) // QA cycle: the whole-screen placement for both players
		bottom = 0, whole = 1;
	const int sw = *(const int *)(base + M11_PLACE_SW), sh = *(const int *)(base + M11_PLACE_SH);
	if (sw <= 0 || sh <= 0)
		return;
	int vp[4];
	exp11_pixels(bottom ? 1 : 0, sw, sh, vp);
	const float x = whole ? 0.0f : (float)vp[0], y = whole ? 0.0f : (float)vp[1], w = whole ? (float)sw : (float)vp[2], h = whole ? (float)sh : (float)vp[3];
	static unsigned logged;
	if (logged < 6)
		logged++, log_printf("native: exp11place: the 2D placement of the %s is (%g, %g, %g, %g) of a %dx%d screen", whole ? "whole screen" : bottom ? "bottom half" : "top half", (double)x, (double)y, (double)w, (double)h, sw, sh);
	float *pl = (float *)(base + M11_PLACE_CLIENT);
	((void (*)(float *, float, float, float, float, int, int))(base + M11_PLACE_SET))(pl, x, y, w, h, sw, sh);
	// The size of the 2D in a half. A half is 0.5 of the screen high, so the engine's scale (height / 480) makes a HUD that is half the size of the single picture's: barely
	// readable. The 360 enlarges the HUD of its split views: cg_hudLegacySplitscreenScale (registered at 0x82140e60: default 1.5, minimum 1; "Screen scale for hud elements in
	// splitscreen", with cg_hudSplitscreenOffsetsUseScale 1: the offsets scale too), the active factor kept in 0x8266be6c (0x82140de8: that dvar while the screen is split, else 1) and
	// applied to the HUD menu items of the console's *_ss menus. Here the whole placement gets it, so every 2D item of the half grows with it, from its own anchor ([native]
	// hudsplitscale=<percent>, 150 default, 100 off): the scales are multiplied, the virtual rectangles (viewable, adjustable) shrink by the same factor, the 4:3 centring is made again.
	if (!whole) {
		const float k = place_use_k ? (float)CFG_ONCE("hudsplitscale", 150) / 100.0f : 1.0f;
		if (k > 1.001f && k < 4.0f) {
			pl[0] *= k, pl[1] *= k, pl[2] *= k, pl[3] *= k;
			pl[4] /= k, pl[5] /= k;
			for (int i = 10; i <= 13; i++)
				pl[i] /= k;
			for (int i = 18; i <= 21; i++)
				pl[i] /= k;
			pl[26] = (pl[8] - pl[0] * 640.0f) * 0.5f; // the free width left and right of the 4:3 area (0xdb0b0: (w - 4:3 width) * 0.5)
		}
	}
}
// [native] smoothlog=1: how evenly the camera of each view moves from one draw to the next while it walks (the draws in a row that did not move although it did, the speed per draw)
static void smooth_sample(int k)
{
	static float last[2][3];
	static LONGLONG last_t[2];
	static struct {
		long n, still, moving;
		double sum, sq;
		float max;
	} st[2];
	static LONGLONG report;
	LARGE_INTEGER c, f;
	QueryPerformanceCounter(&c);
	QueryPerformanceFrequency(&f);
	const float *o = (const float *)(base + RVA_CG_PS_ORIGIN);
	if (last_t[k]) {
		float dx = o[0] - last[k][0], dy = o[1] - last[k][1], dz = o[2] - last[k][2];
		float d = sqrtf(dx * dx + dy * dy + dz * dz);
		double ms = (double)(c.QuadPart - last_t[k]) * 1000.0 / (double)f.QuadPart;
		if (CFG_ONCE("smoothlog", 0) >= 2 && d > 0.05f && d < 100.0f) { // raw samples of a walking camera: the step, the wall time since the view's last draw, the game time step
			static int dumped;
			static int last_cgt[2];
			int cgt = *(const int *)(base + RVA_CG_TIME);
			if (dumped < 300)
				dumped++, log_printf("native: smooth raw view %d: step %.3f units in %.2f wall ms, game time step %d ms (%.3f units/ms of game time)", k, d, ms, cgt - last_cgt[k],
						     cgt > last_cgt[k] ? d / (cgt - last_cgt[k]) : 0.0f);
			last_cgt[k] = cgt;
		}
		{ // the dvars that set the frame rhythm, once ([native] smoothlog)
			static int dvars_logged;
			if (!dvars_logged++) {
				static const char *const names[] = {"com_maxfps", "r_vsync", "r_smp_backend", "r_smp_frontend", "r_multiGpu", "cl_maxpackets", "snaps", "sv_fps", "com_waitEndFrameMode", "timescale", "r_showTris"};
				char v[64], line[512];
				int len = 0;
				for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
					len += snprintf(line + len, sizeof(line) - (size_t)len, "%s=%s  ", names[i], game_dvar_raw(names[i], v, sizeof(v)) ? v : "(none)");
				log_printf("native: smooth dvars: %s", line);
			}
		}
		{ // the game's time step per draw against the wall clock: an uneven game step at an even wall step shows as judder in everything the cgame moves by its time
			static int lt[2];
			static struct { long n; double ws, wq, gs, gq, es, eq, emax; } ts[2];
			int cgt2 = *(const int *)(base + RVA_CG_TIME);
			if (lt[k] && cgt2 >= lt[k] && cgt2 - lt[k] < 1000) {
				double g = cgt2 - lt[k], e = g - ms;
				ts[k].n++, ts[k].ws += ms, ts[k].wq += ms * ms, ts[k].gs += g, ts[k].gq += g * g, ts[k].es += e, ts[k].eq += e * e;
				if (fabs(e) > ts[k].emax)
					ts[k].emax = fabs(e);
				if (ts[k].n >= 170) {
					double wm = ts[k].ws / ts[k].n, gm = ts[k].gs / ts[k].n;
					log_printf("native: smooth time view %d: %ld draws, wall step mean %.2f sd %.2f ms, game step mean %.2f sd %.2f ms, game minus wall: sd %.2f max %.2f ms", k, ts[k].n, wm,
						   sqrt(fmax(0, ts[k].wq / ts[k].n - wm * wm)), gm, sqrt(fmax(0, ts[k].gq / ts[k].n - gm * gm)), sqrt(fmax(0, ts[k].eq / ts[k].n - (ts[k].es / ts[k].n) * (ts[k].es / ts[k].n))),
						   ts[k].emax);
					memset(&ts[k], 0, sizeof(ts[k]));
				}
			}
			{ // [native] smoothlog=3: 80 consecutive wall steps and game steps (ms) after the first 20 s of draws, to see a pattern (a hitch every server frame, every Nth draw)
				static int seen;
				static double raw_w[80];
				static int raw_g[80], raw_n;
				if (CFG_ONCE("smoothlog", 0) >= 3 && lt[k] && cgt2 >= lt[k] && ++seen > 600 && raw_n < 80) {
					raw_w[raw_n] = ms;
					raw_g[raw_n++] = cgt2 - lt[k];
					if (raw_n == 80) {
						char line[1200];
						int len = 0;
						for (int i = 0; i < 80; i++)
							len += snprintf(line + len, sizeof(line) - (size_t)len, "%.0f/%d ", raw_w[i], raw_g[i]);
						log_printf("native: smooth raw wall/game steps (ms): %s", line);
					}
				}
			}
			lt[k] = cgt2;
		}
		if (d < 100.0f) {
			st[k].n++;
			if (d > 0.0005f) {
				double v = d / ms; // units per ms
				st[k].moving++, st[k].sum += v, st[k].sq += v * v;
				if ((float)v > st[k].max)
					st[k].max = (float)v;
			} else {
				st[k].still++;
			}
		}
	}
	memcpy(last[k], o, sizeof(last[k]));
	last_t[k] = c.QuadPart;
	if (!report)
		report = c.QuadPart;
	if (c.QuadPart - report >= f.QuadPart) {
		report = c.QuadPart;
		for (int i = 0; i < 2; i++) {
			double mean = st[i].moving ? st[i].sum / st[i].moving : 0, var = st[i].moving ? st[i].sq / st[i].moving - mean * mean : 0;
			if (st[i].n)
				log_printf("native: smooth view %d: %ld draws, %ld without camera movement, camera speed per draw mean %.4f sd %.4f max %.4f units/ms", i, st[i].n, st[i].still, mean,
					   var > 0 ? sqrt(var) : 0.0, (double)st[i].max);
			memset(&st[i], 0, sizeof(st[i]));
		}
	}
}
#ifdef _WIN64
// The cgame draw (rva 0xbca00) runs the server's actor update with a time budget when a next snapshot is pending: at rva 0xbcc5a it calls 0x233de0, which (budget from rdtsc)
// calls the server frame body 0x173e90 where the actors' origins are written. [native] svslice=1: count the calls per context (log every 200 draws); svslice=2: also skip the
// call in context 1, so the update runs once per frame, in the first view's draw.
static uint64_t (*svslice_orig)(void);
static long svslice_calls[2], svslice_draws;
static uint64_t svslice_hook(void)
{
	int c = ctx_current == 1 ? 1 : 0;
	svslice_calls[c]++;
	int m = CFG_ONCE("svslice", 0);
	if ((m == 2 && c == 1 && ctx_on) || m == 3) // 3: the draw does not run the update at all, the server step does (svslice_now)
		return 0;
	return svslice_orig();
}
// CG_TransitionEntity (rva 0xb8d80): the entity's current state becomes its next state. Entities from 135 on are not in the swapped ranges, so both contexts' cgames run this
// on the same entity. [native] centskip=1: count the calls (per context, own / shared entities); centskip=2: context 1 does not touch the shared ones.
static void (*cent_trans_orig)(uint8_t *);
static long cent_trans_calls[2][2];
static void cent_trans_hook(uint8_t *cent)
{
	int idx = -1;
	if (cent >= base + RVA_CENT)
		idx = (int)((size_t)(cent - (base + RVA_CENT)) / CENT_SIZE);
	int shared = idx >= 135;
	int c = ctx_current == 1 ? 1 : 0;
	cent_trans_calls[c][shared]++;
	if (CFG_ONCE("centskip", 0) >= 2 && c == 1 && ctx_on && shared)
		return;
	cent_trans_orig(cent);
}
static void cent_trans_install(void)
{
	static int done;
	static const uint8_t b[15] = {0x40, 0x53, 0x57, 0x41, 0x57, 0x48, 0x83, 0xec, 0x20, 0x8b, 0x81, 0x58, 0x01, 0x00, 0x00};
	if (done)
		return;
	done = 1;
	log_printf("native: centskip hook %s", hook_function(base + XR(0xb8d80), b, sizeof(b), (void *)cent_trans_hook, (void **)&cent_trans_orig) ? "installed" : "FAILED");
}
// The server frame body (rva 0x173e90) is a state machine (phase at rva 0xd97bf8, the entity think index at 0xd97c04) that a time budget can suspend: mode 1 = run until the rdtsc
// deadline, mode 0 = run to the end. [native] slicelog=1 records every call (context, mode, phase and index before and after, result, duration) and the server steps between them,
// 700 events after the first 400 draws, as one line per 35 events.
static uint64_t (*slice_orig)(int, int);
static struct {
	double t, dur;
	int kind, mode, ph0, ph1, i0, i1, ret, ctx;
} slice_ev[700];
static int slice_n, slice_seen;
static LONGLONG slice_t0;
static void slice_flush(void)
{
	char line[1800];
	for (int part = 0; part < 20; part++) {
		int len = 0;
		for (int i = part * 35; i < part * 35 + 35 && len < (int)sizeof(line) - 1; i++) { // (stops when the line is full: past it the size wrapped, audit 2026-11-18)
			if (slice_ev[i].kind == 1)
				len += snprintf(line + len, sizeof(line) - (size_t)len, "|STEP@%.1f| ", slice_ev[i].t);
			else
				len += snprintf(line + len, sizeof(line) - (size_t)len, "c%d m%d p%d>%d i%d>%d r%d %.1fms@%.1f; ", slice_ev[i].ctx, slice_ev[i].mode, slice_ev[i].ph0, slice_ev[i].ph1, slice_ev[i].i0, slice_ev[i].i1, slice_ev[i].ret,
					   slice_ev[i].dur, slice_ev[i].t);
		}
		log_printf("native: slicelog %d: %s", part, line);
	}
}
static uint64_t slice_hook(int mode, int deadline)
{
	if (!CFG_ONCE("slicelog", 0) || slice_n >= 700)
		return slice_orig(mode, deadline);
	LARGE_INTEGER c, hz;
	QueryPerformanceCounter(&c);
	QueryPerformanceFrequency(&hz);
	const int ph0 = *(const int *)(base + XR(0xd97bf8)), i0 = *(const int *)(base + XR(0xd97c04));
	LONGLONG t0 = c.QuadPart;
	uint64_t r = slice_orig(mode, deadline);
	QueryPerformanceCounter(&c);
	if (++slice_seen > 400) {
		if (!slice_t0)
			slice_t0 = t0;
		const int k = slice_n++;
		slice_ev[k].kind = 0;
		slice_ev[k].t = (double)(t0 - slice_t0) * 1000.0 / (double)hz.QuadPart;
		slice_ev[k].dur = (double)(c.QuadPart - t0) * 1000.0 / (double)hz.QuadPart;
		slice_ev[k].mode = mode;
		slice_ev[k].ph0 = ph0;
		slice_ev[k].ph1 = *(const int *)(base + XR(0xd97bf8));
		slice_ev[k].i0 = i0;
		slice_ev[k].i1 = *(const int *)(base + XR(0xd97c04));
		slice_ev[k].ret = (int)(r & 0xff);
		slice_ev[k].ctx = ctx_current == 1 ? 1 : 0;
		if (slice_n == 700)
			slice_flush();
	}
	return r;
}
static void slice_step_note(void)
{
	if (slice_n >= 700 || !slice_t0)
		return;
	LARGE_INTEGER c, hz;
	QueryPerformanceCounter(&c);
	QueryPerformanceFrequency(&hz);
	slice_ev[slice_n].kind = 1;
	slice_ev[slice_n].t = (double)(c.QuadPart - slice_t0) * 1000.0 / (double)hz.QuadPart;
	if (++slice_n == 700)
		slice_flush();
}
// QA ([native] quadlog=1, 64-bit): the back end's 2D quad (rva 0x3cff0: material, x, y, w, h, s0, t0, s1, t1, color), the big ones of a few frames, 15 s into the split: which
// material paints how much of the picture (the black half of player 2's view while his 2D is on).
static void (*quad_orig)(void *, float, float, float, float, unsigned, unsigned, unsigned, unsigned, unsigned);
static void quad_hook(void *mat, float x, float y, float w, float h, unsigned s0, unsigned t0, unsigned s1, unsigned t1, unsigned col)
{
	static unsigned n;
	const int sw = *(const int *)(base + XR(0x548bfc)), sh = *(const int *)(base + XR(0x548c00));
	if (n < 120 && split_on && GetTickCount() - split_tick > 15000 && w * h > 0.15f * (float)sw * (float)sh * 0.5f) {
		const char *name = mat && !IsBadReadPtr(mat, 8) && *(const char *const *)mat && !IsBadReadPtr(*(const char *const *)mat, 4) ? *(const char *const *)mat : "?";
		n++;
		log_printf("native: quad #%u: material %.40s at %g %g size %g x %g uv %g %g %g %g color %08x | fd %p", n, name, (double)x, (double)y, (double)w, (double)h, (double)*(float *)&s0, (double)*(float *)&t0, (double)*(float *)&s1,
			   (double)*(float *)&t1, col, *(void **)(base + XR64(0x4358f58)));
	}
	quad_orig(mat, x, y, w, h, s0, t0, s1, t1, col);
}
static void quad_install(void)
{
	static const uint8_t b[15] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x50, 0x0f, 0x29, 0x74, 0x24, 0x40};
	log_printf("native: quadlog hook %s", hook_function(base + XR(0x3cff0), b, sizeof(b), (void *)quad_hook, (void **)&quad_orig) ? "installed" : "FAILED");
}
static void slice_install(void)
{
	static int done;
	static const uint8_t b[15] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20};
	if (done)
		return;
	done = 1;
	log_printf("native: slicelog hook %s", hook_function(base + XR(0x173e90), b, sizeof(b), (void *)slice_hook, (void **)&slice_orig) ? "installed" : "FAILED");
}
#endif // _WIN64 (the diagnostics above)
// The snapshot's entity states. In single player the server and the cgame share ONE array of them (rva 0x19c6410, 0xac bytes each, entity number as the index): the server's
// snapshot build (FUN_140234540, reached by a tail jump at rva 0x234a44, called with the server time = level time at rva 0xd96ec8 as the snapshot's time) overwrites it for
// every snapshot, and the cgame (CG_SetNextSnap, rva 0xb9320, at its call 0xb9772 of the getter 0x234520) copies the states of cg.nextSnap out of it when it reads that snapshot.
// A read that comes after the NEXT build gets the newer snapshot's states: the actor's value is skipped, then read a second time (traced 2026-11-02: server origin regular,
// client next-state values skipped and doubled in the split screen, where the client's snapshot reads are late). A ring of copies, one per build and tagged with the
// snapshot's time, gives the read the states of the snapshot it is reading. [native] entring=0 turns it off.
#ifdef _WIN64
#define ENTRING_ARRAY XR(0x19c6410)
#define ENTRING_TIME XR(0xd96ec8)       // level time (the snapshot's time), getter 0x171360
#define ENTRING_BUILD_SITE XR(0x234a44) // a tail jmp to the snapshot build 0x234540
#define ENTRING_GET_SITE XR(0xb9772)    // the getter call in CG_SetNextSnap 0xb9320
#define ENTRING_NEXTSNAP XR(0x51b080)   // cg.nextSnap
#else
// 32-bit (VA - 0x400000): the array VA 0x1b2aca0 (0x56000 bytes, the next array starts right behind it), level time VA 0x10a3c44 (getter 0x4f4700), the build 0x65cc90 called at
// VA 0x42d18b, the getter 0x40dd30 (cdecl) called at VA 0x41a649 by the function (0x41a3a0) that makes cg.snap = cg.nextSnap, cg.nextSnap at VA 0x867990
#define ENTRING_ARRAY XR(0x172aca0)
#define ENTRING_TIME XR(0xca3c44)
#define ENTRING_BUILD_SITE XR(0x2d18b)
#define ENTRING_GET_SITE XR(0x1a649)
#define ENTRING_GETTER XR(0xdd30)
#define ENTRING_NEXTSNAP XR(0x467990)
#endif
#define ENTRING_STRIDE 0xacu
#define ENTRING_COUNT 2048u
#define ENTRING_SLOTS 8
static long transfix_runs; // (ent_clean_hook below: transitions run for context 0 from context 1's frame)
static uint8_t *entring_buf;
static int entring_time[ENTRING_SLOTS], entring_next;
static void (*entring_build_orig)(void);
static uint8_t *(*entring_get_orig)(int);
static long entring_hits, entring_misses, entring_reads_late, entring_snap_reads;
static void entring_build_hook(void)
{
	entring_build_orig();
	if (!native_active) // (the hooks stay after a split session: no copies outside one; night audit A7)
		return;
	const int t = *(const int *)(base + ENTRING_TIME);
	static int last_t = -1;
	if (t < last_t) // the server time started over (a new level, a loaded game): the old copies are of another timeline
		for (int i = 0; i < ENTRING_SLOTS; i++)
			entring_time[i] = -1;
	last_t = t;
	const int slot = entring_next++ % ENTRING_SLOTS;
	memcpy(entring_buf + (size_t)slot * ENTRING_COUNT * ENTRING_STRIDE, base + ENTRING_ARRAY, (size_t)ENTRING_COUNT * ENTRING_STRIDE);
	entring_time[slot] = t;
	if (entring_next % 200 == 0)
		log_printf("native: entring: %d builds, %ld reads from a copy, %ld from the live array, %ld of %ld reads were of an older snapshot than the newest build (last build time %d); transitions run for context 0 from context 1's frame: %ld", entring_next, entring_hits, entring_misses, entring_reads_late, entring_snap_reads, t, transfix_runs);
}
static uint8_t *entring_get_hook(int n)
{
	if (!native_active)
		return entring_get_orig(n);
	const uintptr_t ns = *(const uintptr_t *)(base + ENTRING_NEXTSNAP); // the snapshot being read: cg.nextSnap (context-private)
	if (ns && n >= 0 && n < (int)ENTRING_COUNT) {
		const int t = *(const int *)(ns + 4);
		if (entring_next > 0) { // statistics: a read of a snapshot that is not the newest build any more is a late read (the live array would have given the newer snapshot's states)
			entring_snap_reads++;
			if (entring_time[(entring_next - 1) % ENTRING_SLOTS] != t)
				entring_reads_late++;
		}
		for (int i = 0; i < ENTRING_SLOTS; i++)
			if (entring_time[i] == t && entring_next > 0) {
				entring_hits++;
				return entring_buf + (size_t)i * ENTRING_COUNT * ENTRING_STRIDE + (size_t)n * ENTRING_STRIDE;
			}
	}
	entring_misses++;
	return entring_get_orig(n);
}
#ifndef _WIN64
// 32 bits: the getter's call site (VA 0x41a649) lies in a range the official exe hashes (docs/32bit-plan.md), and loading a save checks it: the game quit with
// 0x8000dead when a failed mission loaded the last checkpoint (QA nativesave 2026-11-17, the only hashed bytes that differed were that call's). The getter itself
// (VA 0x40dd30, outside the hashed ranges) is hooked instead, and only the read from that call site goes to the ring.
static uint8_t *entring_getter_hook(int n)
{
	if (__builtin_return_address(0) != (void *)(base + ENTRING_GET_SITE + 5))
		return entring_get_orig(n);
	return entring_get_hook(n);
}
#endif
static void entring_install(void)
{
	static int done;
	if (done)
		return;
	done = 1;
	entring_buf = (uint8_t *)VirtualAlloc(NULL, (size_t)ENTRING_SLOTS * ENTRING_COUNT * ENTRING_STRIDE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	for (int i = 0; i < ENTRING_SLOTS; i++)
		entring_time[i] = -1;
	const int parts = cfg("entring", 1) > 1 ? 3 & ~(cfg("entring", 1) == 2 ? 1 : 0) & ~(cfg("entring", 1) == 3 ? 2 : 0) : 3; // QA: entring=2 only the getter, 3 only the builder
	int ok = entring_buf != NULL;
	if (ok && (parts & 1))
		ok = hook_call(base + ENTRING_BUILD_SITE, (void *)entring_build_hook, (void **)&entring_build_orig);
	if (ok && (parts & 2)) {
#ifdef _WIN64
		ok = hook_call(base + ENTRING_GET_SITE, (void *)entring_get_hook, (void **)&entring_get_orig);
#else
		static const uint8_t getter[10] = {0x8b, 0x44, 0x24, 0x04, 0x69, 0xc0, 0xac, 0x00, 0x00, 0x00}; // mov eax, [esp+4]; imul eax, eax, 0xac
		ok = hook_function(base + ENTRING_GETTER, getter, sizeof(getter), (void *)entring_getter_hook, (void **)&entring_get_orig);
#endif
	}
	log_printf("native: entring (a copy of the snapshot entity states per server build) %s", ok ? "installed" : "FAILED");
}
#ifdef _WIN64
// [native] transitionlog=1: the two call sites of the cgame's snapshot transition (FUN_1400b9320): 0xb8d55 inside b8d30 right after the server hand-over (cgame frame, mode != 1) and
// 0xb8cee inside b8c50 (the cgame frame when the client time has passed the next snapshot, mode == 1); per context, with the client time.
static void (*trans_a_orig)(void);
static void (*trans_b_orig)(void);
static long trans_n[2][2];
static void trans_note(int site)
{
	const int c = ctx_current == 1 ? 1 : 0;
	trans_n[site][c]++;
	static int logged;
	if (logged < 3000 && ++logged > 20)
		log_printf("native: transition site %s ctx %d: client time %d, cg.snap %d, cg.nextSnap %d", site ? "b8c50" : "b8d30", ctx_current, *(const int *)(base + XR(0x51dca8)), *(const int *)(*(const uintptr_t *)(base + XR(0x51b078)) + 4),
			   *(const int *)(*(const uintptr_t *)(base + XR(0x51b080)) + 4));
	static LONGLONG next;
	LARGE_INTEGER q, f;
	QueryPerformanceCounter(&q);
	QueryPerformanceFrequency(&f);
	if (!next)
		next = q.QuadPart + 5 * f.QuadPart;
	if (q.QuadPart >= next) {
		next = q.QuadPart + 5 * f.QuadPart;
		log_printf("native: transition calls per 5 s: after the hand-over (b8d30) ctx0 %ld ctx1 %ld | in the cgame frame (b8c50) ctx0 %ld ctx1 %ld", trans_n[0][0], trans_n[0][1], trans_n[1][0], trans_n[1][1]);
		memset(trans_n, 0, sizeof(trans_n));
	}
}
static void trans_a_hook(void)
{
	trans_note(0);
	trans_a_orig();
}
static void trans_b_hook(void)
{
	trans_note(1);
	trans_b_orig();
}
static void trans_install(void)
{
	static int done;
	if (done)
		return;
	done = 1;
	log_printf("native: transitionlog hooks %s", hook_call(base + XR(0xb8d55), (void *)trans_a_hook, (void **)&trans_a_orig) && hook_call(base + XR(0xb8cee), (void *)trans_b_hook, (void **)&trans_b_orig) ? "installed" : "FAILED");
}
// (the diagnostics again)
static void svslice_install(void)
{
	static int done;
	if (done)
		return;
	done = 1;
	log_printf("native: svslice hook %s", hook_call(base + XR(0xbcc5a), (void *)svslice_hook, (void **)&svslice_orig) ? "installed" : "FAILED");
}
// [native] framelog=1 (plain game, 2026-11-02): where the time of a frame goes. Com_Frame (rva 0x1fbde0) reads the clock, runs the server frame (rva 0x233190) and the client
// frame (rva 0xd4ad0); the cgame draw sits inside. For 80 frames in a row (after the first 600 draws): the period from one Com_Frame start to the next, the server frame's and the
// client frame's duration, when the draw starts after the Com_Frame start, the draw's duration and the whole Com_Frame, all in ms.
static void (*fs_com_orig)(void);
static void (*fs_sv_orig)(int);
static void (*fs_cl_orig)(int, float);
static LONGLONG fs_com0, fs_com_prev, fs_sv0, fs_sv1, fs_cl0, fs_cl1, fs_draw0, fs_draw1;
static int fs_on;
static LONGLONG fs_qpc(void)
{
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return c.QuadPart;
}
static struct {
	double period, sv, cl, draw_at, draw, total;
} fs_rec[80];
static int fs_n, fs_seen;
static void fs_com_hook(void)
{
	fs_com_prev = fs_com0;
	fs_com0 = fs_qpc();
	fs_sv0 = fs_sv1 = fs_cl0 = fs_cl1 = fs_draw0 = fs_draw1 = 0;
	fs_com_orig();
	LONGLONG end = fs_qpc(), f;
	LARGE_INTEGER hz;
	QueryPerformanceFrequency(&hz);
	f = hz.QuadPart;
	if (fs_draw0 && fs_com_prev && ++fs_seen > 600 && fs_n < 80) {
		double k = 1000.0 / (double)f;
		fs_rec[fs_n].period = (double)(fs_com0 - fs_com_prev) * k;
		fs_rec[fs_n].sv = fs_sv1 ? (double)(fs_sv1 - fs_sv0) * k : 0;
		fs_rec[fs_n].cl = fs_cl1 ? (double)(fs_cl1 - fs_cl0) * k : 0;
		fs_rec[fs_n].draw_at = (double)(fs_draw0 - fs_com0) * k;
		fs_rec[fs_n].draw = fs_draw1 ? (double)(fs_draw1 - fs_draw0) * k : 0;
		fs_rec[fs_n].total = (double)(end - fs_com0) * k;
		if (++fs_n == 80) {
			char line[1500];
			for (int part = 0; part < 2; part++) {
				int len = 0;
				for (int i = part * 40; i < part * 40 + 40; i++)
					len += snprintf(line + len, sizeof(line) - (size_t)len, "%.0f|%.1f|%.1f|%.1f|%.1f|%.1f ", fs_rec[i].period, fs_rec[i].sv, fs_rec[i].cl, fs_rec[i].draw_at, fs_rec[i].draw, fs_rec[i].total);
				log_printf("native: framelog (period|sv|cl|draw at|draw|total ms) %s", line);
			}
		}
	}
}
static void fs_sv_hook(int msec)
{
	fs_sv0 = fs_qpc();
	fs_sv_orig(msec);
	fs_sv1 = fs_qpc();
}
static void fs_cl_hook(int msec, float f)
{
	fs_cl0 = fs_qpc();
	fs_cl_orig(msec, f);
	fs_cl1 = fs_qpc();
}
static void fs_install(void)
{
	static int done;
	if (done)
		return;
	done = 1;
	// the server and client frames start with RIP-relative loads, so they are hooked at their call sites in Com_Frame (rva 0x1fc2ef, 0x1fc304); Com_Frame itself has 15 plain bytes
	static const uint8_t com_b[15] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18};
	fs_on = hook_function(base + XR(0x1fbde0), com_b, sizeof(com_b), (void *)fs_com_hook, (void **)&fs_com_orig) && hook_call(base + XR(0x1fc2ef), (void *)fs_sv_hook, (void **)&fs_sv_orig) &&
		hook_call(base + XR(0x1fc304), (void *)fs_cl_hook, (void **)&fs_cl_orig);
	log_printf("native: framelog hooks %s", fs_on ? "installed" : "FAILED");
}
// [native] pace=1 (experiment, needs framelog=1): the draw ends at a fixed offset from the clock read of its Com_Frame. The offset is the 85th percentile of the last 128 frames'
// (draw end - clock read); a frame that is done earlier waits (spinning on the performance counter) until then, so the picture of a frame is shown at the same distance from
// the game time it shows. [native] pacestats=1 logs the spread of that offset every 170 frames, paced or not.
static double fs_off[128];
static int fs_off_n;
static int pace_on = -1; // [native] pace at the start, F8 flips it while playing (the game window's title says which)
static BOOL CALLBACK pace_title_cb(HWND w, LPARAM lp)
{
	DWORD pid = 0;
	char title[128];
	GetWindowThreadProcessId(w, &pid);
	if (pid != GetCurrentProcessId() || !IsWindowVisible(w) || GetWindow(w, GW_OWNER))
		return TRUE;
	if (GetWindowTextA(w, title, sizeof(title)) > 0 && strncmp(title, "Modern Warfare 2", 16) == 0) {
		char t[160];
		snprintf(t, sizeof(t), "Modern Warfare 2 - pace %s (F8)", lp ? "ON" : "OFF");
		SetWindowTextA(w, t);
		return FALSE;
	}
	return TRUE;
}
static void fs_pace(void)
{
	static LONGLONG hz;
	static int f8_was, titled;
	if (pace_on < 0)
		pace_on = CFG_ONCE("pace", 0) != 0;
	if (!titled) {
		titled = 1;
		EnumWindows(pace_title_cb, pace_on);
	}
	{ // F8, only while the game's window has the focus
		DWORD pid = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &pid);
		int down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
		if (down && !f8_was) {
			pace_on = !pace_on;
			log_printf("native: pace %s (F8)", pace_on ? "on" : "off");
			EnumWindows(pace_title_cb, pace_on);
		}
		f8_was = down;
	}
	static double target;
	static int report_n;
	if (!hz) {
		LARGE_INTEGER h;
		QueryPerformanceFrequency(&h);
		hz = h.QuadPart;
	}
	double k = 1000.0 / (double)hz;
	double natural = (double)(fs_qpc() - fs_com0) * k; // before any wait: the target is taken from these, or it would only ever grow
	if (pace_on && target > 0) {
		double now = natural;
		while (now < target && now < 12.0) {
			YieldProcessor();
			now = (double)(fs_qpc() - fs_com0) * k;
		}
	}
	double off = (double)(fs_qpc() - fs_com0) * k;
	static double fin[128];
	fin[fs_off_n & 127] = off;
	fs_off[fs_off_n++ & 127] = natural;
	if (fs_off_n >= 128 && (fs_off_n & 15) == 0) {
		double tmp[128];
		memcpy(tmp, fs_off, sizeof(tmp));
		for (int a = 1; a < 128; a++) { // insertion sort
			double v = tmp[a];
			int b = a - 1;
			while (b >= 0 && tmp[b] > v)
				tmp[b + 1] = tmp[b], b--;
			tmp[b + 1] = v;
		}
		target = tmp[108]; // p85
	}
	if (CFG_ONCE("pacestats", 0) && fs_off_n >= 128 && ++report_n >= 170) {
		double m = 0, q = 0;
		for (int a = 0; a < 128; a++)
			m += fin[a], q += fin[a] * fin[a];
		m /= 128;
		report_n = 0;
		log_printf("native: pace stats: draw end after the clock read: mean %.2f sd %.2f ms, target %.2f ms (pace %d)", m, sqrt(fmax(0, q / 128 - m * m)), target, CFG_ONCE("pace", 0));
	}
}
#endif
static const char *vision_name_of(int ctx);
static void npc_log(void);

static void cg_draw_hook(unsigned local_client, int a, int b)
{
#ifdef _WIN64
	if (fs_on)
		fs_draw0 = fs_qpc();
	if (cent_trans_orig && (cent_trans_calls[0][1] + cent_trans_calls[1][1] > 0) && ++svslice_draws % 400 == 0)
		log_printf("native: centskip transitions: ctx0 own %ld shared %ld | ctx1 own %ld shared %ld", cent_trans_calls[0][0], cent_trans_calls[0][1], cent_trans_calls[1][0], cent_trans_calls[1][1]);
	if (svslice_orig && ++svslice_draws % 400 == 0)
		log_printf("native: svslice calls in %ld frames: context 0 %ld, context 1 %ld", svslice_draws, svslice_calls[0], svslice_calls[1]);
#endif
	cg_draw_hook_inner(local_client, a, b);
	chat_draw(); // the co-op text chat's open line (chat.c; network co-op only: one cgame draw a frame there)
	{ // [native] burn=<ms>: busy-wait this long after every cgame draw (QA: does a longer frame alone make the plain game jitter like the split screen?)
		static int burn = -1;
		if (burn < 0)
			burn = cfg("burn", 0), log_printf("native: burn = %d ms per draw", burn);
		if (burn > 0) {
			LARGE_INTEGER b0, now, hz;
			QueryPerformanceFrequency(&hz);
			QueryPerformanceCounter(&b0);
			do
				QueryPerformanceCounter(&now);
			while ((now.QuadPart - b0.QuadPart) * 1000 < (LONGLONG)burn * hz.QuadPart);
		}
	}
#ifdef _WIN64
	if (fs_on) {
		fs_draw1 = fs_qpc();
		fs_pace();
	}
#endif
	if (CFG_ONCE("smoothlog", 0))
		smooth_sample(ctx_current == 1 ? 1 : 0);
}
// Per-player visibility (hideOnClient / showOnClient): the server keeps it in each entity's flags, 0x10 = hidden from local client 0, 0x20 = from local client 1 (rva 0x1928f0,
// the method's bit chosen by the client's number), and the cgame tests the bit of the local client it draws for. Player 2's draw runs as local client 0, so his half showed what
// player 1 may see and hid what player 1 may not: on the Cliffhanger climb his own climbing arms were hidden from him and the stand-in body meant for his partner was in front of
// his eyes (owner's screenshot, 2026-11-16); the same for every scene a script hides from one player (the boat, the snowmobile, Estate). For his draw the two bits trade places in
// the current and the next state of every entity, and are put back after it ([native] visswap=0 off).
static void vis_swap(void)
{
#ifdef _WIN64
	static const int offs[2] = {0x70, 0xec}; // the current state's flags, the next state's (the lerp states' eFlags, 0x10 in front of their positions CF_ORG1 / CF_ORG2)
#else
	static const int offs[2] = {0x64, 0xe0}; // 32 bits: the same 0x10 in front of CF_ORG1 / CF_ORG2 (0x74 / 0xf0); 0xe0 is CENT_FLAGS (night audit A6: the 32-bit build had no visswap)
#endif
	for (int n = 0; n < 2048; n++) {
		uint8_t *c = base + RVA_CENT + (size_t)n * CENT_SIZE;
		for (int k = 0; k < 2; k++) {
			uint32_t *f = (uint32_t *)(c + offs[k]);
			const uint32_t v = *f, b0 = v & 0x10u, b1 = v & 0x20u;
			if (b0 != (b1 >> 1)) {
				*f = (v & ~0x30u) | (b0 << 1) | (b1 >> 1);
				if (k == 0 && CFG_ONCE("vislog", 0)) {
					static unsigned vl;
					if (vl++ % 2000 == 0)
						log_printf("native: visswap: entity %d (type %u) flags %08x: hidden from %s", n, (unsigned)c[CENT_TYPE], v, b0 ? "player 1" : "player 2");
				}
			}
		}
	}
}
static int vis_swapped; // vis_swap has given player 2's draw his own marks: add_entity_hook's older per-entity swap (swapbits) must not undo them at +0xec
static int ent_bits_swapped; // add_entity_hook's per-entity swap (swapbits, the 32-bit builds) is in force for the entity being added: the renderer needs the bits back (m11mask)
static void cg_draw_vis(unsigned local_client, int a, int b)
{
#ifdef _WIN64
	const int swap = ctx_current == 1 && split_on && CFG_ONCE("visswap", 1);
#else
	// 32 bits: off by default; the offsets are derived (CF_ORG1 / CF_ORG2), and the climb A/B of 2026-11-17 (QA climblb, composed frame) showed no difference to confirm them
	const int swap = ctx_current == 1 && split_on && CFG_ONCE("visswap", 0);
#endif
	if (swap)
		vis_swap(), vis_swapped = 1;
	cg_draw_original(local_client, a, b);
	if (swap && ctx_current == 1)
		vis_swap();
	vis_swapped = 0;
}
static void cg_draw_hook_inner(unsigned local_client, int a, int b)
{
	cg_draw_count++;
	// In ordinary single-player the hook only exists for the full-window aspect correction.
	// Skip the split-screen bookkeeping and its many cached diagnostic switches on this hot
	// per-frame path. npclog=2 deliberately opts back into the plain-game comparison trace.
	if (!mode && !native_active && !ctx_on && CFG_ONCE("npclog", 0) < 2) {
		cg_aspect_apply_full_window();
		cg_draw_original(local_client, a, b);
		return;
	}
	float *sw = subwindow();
	if (split_on)
		scan_place();
	// Time measurement ([native] timelog=1, 2026-10-28: the owner sees entities and players "as on ice" in the split): per cgame draw, the wall clock and the live context's
	// client times: cg.time (rva 0x422434), cg.frametime (0x422430), cg.frameInterpolation (0x42242c), cl.serverTime (0x51dca8) and the snapshot lerp fraction (0x51dcb0).
	if (CFG_ONCE("p0cnt", 0) && ctx_on && ctx_current == 1) {
		if (++p0cnt_draws >= 100) {
			const float *e = (const float *)(base + RVA_CENT);
			log_printf("native: p0cnt ctx1 draws %u adds of entity 0 %u | slot0 pos %.1f %.1f %.1f | cg.ps (p2) %.1f %.1f | last add: arg5 %.1f %.1f %.1f pose %.1f %.1f %.1f | flags ctx1 ent0 %08x ctx0 ent1 %08x", p0cnt_draws, p0cnt_adds, (double)e[63], (double)e[64], (double)e[65],
				   (double)*(const float *)(base + XR64(0x42245c)), (double)*(const float *)(base + XR64(0x422460)), (double)p0cnt_draw_org[0], (double)p0cnt_draw_org[1], (double)p0cnt_draw_org[2],
				   (double)p0cnt_draw_org_b[0], (double)p0cnt_draw_org_b[1], (double)p0cnt_draw_org_b[2], p0cnt_flags1, p0cnt_flags0);
			p0cnt_draws = p0cnt_adds = 0;
			{
				char b[300];
				int n = 0;
				for (int k = 0; k < 6 && p0ptr[k]; k++)
					n += snprintf(b + n, sizeof(b) - n, " [%p x%u b8 %x bc %x]", p0ptr[k], p0ptr_cnt[k], p0ptr_b8[k], p0ptr_bc[k]);
				b[n] = 0;
				log_printf("native: p0dobjs%s", b);
				memset(p0ptr, 0, sizeof(p0ptr));
				memset(p0ptr_cnt, 0, sizeof(p0ptr_cnt));
			}
			if (CFG_ONCE("p0cnt", 0) >= 2 && p0dump_have[0] && p0dump_have[1]) {
				static uint8_t prev_slot[CENT_SIZE], prev_dobj[0x200];
				p0dump_diff("slot ctx1ent0/ctx0ent1", p0dump_slot[1], p0dump_slot[0], sizeof(p0dump_slot[0]));
				p0dump_diff("dobj ctx1ent0/ctx0ent1", p0dump_dobj[1], p0dump_dobj[0], sizeof(p0dump_dobj[0]));
				p0dump_diff("slot ctx1 since last", p0dump_slot[1], prev_slot, sizeof(prev_slot));
				p0dump_diff("dobj ctx1 since last", p0dump_dobj[1], prev_dobj, sizeof(prev_dobj));
				memcpy(prev_slot, p0dump_slot[1], sizeof(prev_slot));
				memcpy(prev_dobj, p0dump_dobj[1], sizeof(prev_dobj));
			}
		}
	}
	// View angle check ([native] viewlog=1, 2026-10-28: a script's SetPlayerAngles turns player 2's body but not his camera): per context, every 25th draw, the live context's
	// playerState view angles (cg.ps rva 0x422440: pitch +0x108, yaw +0x10c, roll +0x110) and dwords +0x100..+0x120 (delta_angles are among them), the view angle accumulators
	// (rva 0x533d14 pitch, 0x533d18 yaw) and the cl block's playerState copy (rva RVA_CL_PS) yaw
	// Camera watch: player 2's body (the script's server position, dvar mw2coop_p2pos every half second) against where his own view is (context 1's player state origin). A
	// mismatch of more than 48 units for two looks in a row is logged with both places and his state, so a report of "the bottom camera is in a wall" has numbers.
	if (CFG_ONCE("npclog", 0) && ((ctx_on && split_on && ctx1_ready) || (CFG_ONCE("npclog", 0) >= 2 && !native_active))) // (npclog=2 in the plain game, with smoothlog=1: the same numbers without a split, for comparison)
		npc_log();
	if (CFG_ONCE("p1entlog", 0) && ctx_on && ctx_current == 1) { // diagnostics: where context 1 (player 2's screen) holds entity 0, player 1's body: pose origin and the next state's origin, once a second
		static unsigned en1;
		if (++en1 % 60 == 0) {
			const uint8_t *e = base + RVA_CENT;
			const float *o = (const float *)(e + 0x18), *st = (const float *)(e + 0xfc);
			log_printf("native: p1 body in player 2's screen: pose origin %.1f %.1f %.1f, state origin %.1f %.1f %.1f", (double)o[0], (double)o[1], (double)o[2], (double)st[0], (double)st[1], (double)st[2]);
		}
	}
	if (CFG_ONCE("p2entlog", 0) && ctx_on && ctx_current == 0) { // diagnostics: where context 0 (player 1's screen) holds entity 1, player 2's body: origin, valid flag, eType, once a second
		static unsigned en;
		if (++en % 60 == 0) {
			const uint8_t *e = base + RVA_CENT + CENT_SIZE;
			const float *o = (const float *)(e + 0x18);
			const float *st = (const float *)(e + 0xfc); // the entity state's origin (trBase) as the snapshot delivered it
			log_printf("native: p2 body in player 1's screen: pose origin %.1f %.1f %.1f, state origin %.1f %.1f %.1f, valid flag %u, eType %u", (double)o[0], (double)o[1], (double)o[2], (double)st[0], (double)st[1], (double)st[2],
				   *(const uint32_t *)(e + CENT_VALID) & 1, *(const uint32_t *)(e + 0xe0));
		}
	}
	if (ctx_on && ctx_current == 1) {
		static unsigned watch_n, watch_logs;
		static int watch_bad;
		if (++watch_n % 30 == 0) {
			char enabled[8] = "";
			if (game_dvar_raw("mw2coop_diagnostics", enabled, sizeof(enabled)) && enabled[0] == '1') {
				char v[96];
				float bx, by, bz;
				int linked, stance;
				if (game_dvar_raw("mw2coop_p2pos", v, sizeof(v)) && sscanf(v, "%f %f %f %d %d", &bx, &by, &bz, &linked, &stance) == 5) {
#ifdef _WIN64
					const uint8_t *ps = base + XR(0x422440);
#else
					const uint8_t *ps = base + RVA_CL_PS;
#endif
					float cx = *(const float *)(ps + 0x1c), cy = *(const float *)(ps + 0x20), cz = *(const float *)(ps + 0x24);
					float dx = cx - bx, dy = cy - by, dz = cz - bz;
					int off = dx * dx + dy * dy > 48.0f * 48.0f || dz * dz > 64.0f * 64.0f;
					// the body is a sample up to half a second old: while the view is moving (about 150 units a second walking out of the airport elevator, QA 2026-11-02:
					// a false "57 apart" for 0.4 s) only a still view can be judged
					static float lcx, lcy, lcz;
					if ((cx - lcx) * (cx - lcx) + (cy - lcy) * (cy - lcy) + (cz - lcz) * (cz - lcz) > 4.0f)
						off = 0;
					lcx = cx;
					lcy = cy;
					lcz = cz;
					static int first_look;
					if (!first_look++)
						log_printf("native: camera watch running: player 2's view %.1f %.1f %.1f, his body %.1f %.1f %.1f", (double)cx, (double)cy, (double)cz, (double)bx, (double)by, (double)bz);
					if (off && ++watch_bad == 2 && watch_logs < 40) {
						watch_logs++;
						log_printf("native: CAMERA MISMATCH: player 2's view at %.1f %.1f %.1f, his body at %.1f %.1f %.1f (%.0f apart), linked %d, stance %d", (double)cx, (double)cy, (double)cz,
							   (double)bx, (double)by, (double)bz, (double)sqrtf(dx * dx + dy * dy + dz * dz), linked, stance);
					} else if (!off) {
						if (watch_bad >= 2)
							log_printf("native: camera is back with player 2's body");
						watch_bad = 0;
					}
				}
			}
		}
	}
#ifdef _WIN64
	if (CFG_ONCE("visionlog", 0) && ctx_on && ctx_current == 1 && vision_name_of(0)) { // diagnostics: each context's visionset_naked name (cgame block, rva 0x43c0ac: "visionset_naked <name> <time>" server command)
		static char last[2][64] = {"?", "?"};
		const char *names[2] = {vision_name_of(0), vision_name_of(1)};
		for (int c = 0; c < 2; c++)
			if (strncmp(last[c], names[c], 63)) {
				strncpy(last[c], names[c], 63);
				log_printf("native: vision: player %d's naked visionset is now '%.63s'", c + 1, names[c]);
			}
	}
#endif
	if (CFG_ONCE("sunlog", 0) && ctx_on && split_on && ctx1_ready) { // diagnostics: the sun blind/glare state of the context that is live (RANGE_SUN), about every half second per context
		static unsigned sn[2];
		if (++sn[ctx_current] % 30 == 0) {
			#ifdef _WIN64
			const float *s = (const float *)(base + XR(0x45851b0)); // (the RANGE_SUN range, defined further down)
#else
			const float *s = (const float *)(base + XR(0x204a260));
#endif
			log_printf("native: sunlog ctx %d state %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f cg.time %d", sun_range_owner(), (double)s[0], (double)s[1], (double)s[2], (double)s[3], (double)s[4], (double)s[5],
				   (double)s[6], (double)s[7], (double)s[8], (double)s[9], *(int *)(base + XR64(0x422434)));
		}
	}
	if (CFG_ONCE("viewlog", 0) >= 2 && ctx_on) {
		// viewlog=2 (2026-11-06, the downed player 2's camera tumbles on his screen while his server angles stand still): per context, a line whenever the cgame's
		// player state pitch, yaw or roll moved by more than a degree since the last line, and one every ~10 s while still; no cap on draws, so a long session is covered
		static float last[2][3];
		static unsigned quiet[2], lines[2];
#ifdef _WIN64
		const uint8_t *ps = base + XR(0x422440);
		int cgtime = *(int *)(base + XR(0x422434));
#else
		const uint8_t *ps = base + RVA_CL_PS; // (32 bits: the client block's copy, as the viewlog=1 lines do)
		int cgtime = 0;
#endif
		float a[3] = {*(float *)(ps + 0x108), *(float *)(ps + 0x10c), *(float *)(ps + 0x110)};
		int c = ctx_current & 1;
		float dp = a[0] - last[c][0], dy = a[1] - last[c][1], dr = a[2] - last[c][2];
		dy = fmodf(dy + 540.0f, 360.0f) - 180.0f;
		if ((fabsf(dp) > 1.0f || fabsf(dy) > 1.0f || fabsf(dr) > 1.0f || ++quiet[c] >= 600) && lines[c] < 60000) {
			quiet[c] = 0;
			lines[c]++;
			log_printf("native: view ctx %d pitch %.1f yaw %.1f roll %.1f (moved %.1f %.1f %.1f) cg.time %d", c, (double)a[0], (double)a[1], (double)a[2], (double)dp, (double)dy, (double)dr,
				   cgtime);
			memcpy(last[c], a, sizeof(a));
		}
	}
	if (CFG_ONCE("viewlog", 0) == 1 && ctx_on) {
		static unsigned vn;
		if (++vn % 25 == 0 && vn < 20000) {
#ifdef _WIN64
			const uint8_t *ps = base + XR(0x422440);
#else
			const uint8_t *ps = base + RVA_CL_PS; // (32 bits: the cgame's own copy was not looked for; the client block's copy is the one the next frame is predicted from)
#endif
			const unsigned *d = (const unsigned *)(ps + 0x100);
			log_printf("native: viewlog origin ctx %d: %.1f %.1f %.1f", ctx_current, (double)*(float *)(ps + 0x1c), (double)*(float *)(ps + 0x20), (double)*(float *)(ps + 0x24));
			log_printf("native: viewlog ctx %d cg.ps pitch %.1f yaw %.1f roll %.1f | acc pitch %.1f yaw %.1f | cl.ps yaw %.1f | ps+100: %08x %08x %08x %08x %08x %08x %08x %08x", ctx_current,
				   (double)*(float *)(ps + 0x108), (double)*(float *)(ps + 0x10c), (double)*(float *)(ps + 0x110), (double)*(float *)(base + RVA_ACC_PITCH), (double)*(float *)(base + RVA_ACC_PITCH + 4),
				   (double)*(float *)(base + RVA_CL_PS + 0x10c), d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
		}
	}
	if (CFG_ONCE("timelog", 0) && ctx_on) {
		static unsigned seq;
		unsigned skip = (unsigned)CFG_ONCE("timelogskip", 0);
		if (seq >= skip && seq < skip + 6000) {
			LARGE_INTEGER pc, hz;
			QueryPerformanceCounter(&pc);
			QueryPerformanceFrequency(&hz);
			log_printf("native: timelog #%u wall %.1f ctx %d cg.time %d frametime %d interp %.3f cl.serverTime %d frac %.3f", seq, (double)pc.QuadPart * 1000.0 / (double)hz.QuadPart,
				   ctx_current, *(int *)(base + XR64(0x422434)), *(int *)(base + XR64(0x422430)), (double)*(float *)(base + XR64(0x42242c)), *(int *)(base + XR64(0x51dca8)), (double)*(float *)(base + XR64(0x51dcb0)));
			if (CFG_ONCE("timelog", 0) > 1) {
				// "as on ice": where player 2 is by his own context's playerState (cg.ps origin, rva 0x42245c, live in context 1) and where context 0's cgame has his entity (slot 1,
				// interpolated origin floats 63..65); both on the same cg.time scale
				const float *ps = (const float *)(base + XR64(0x42245c));
				const float *e1 = (const float *)(base + RVA_CENT + CENT_SIZE);
				if (ctx_current == 1)
					log_printf("native: timelog p2ps cg.time %d pos %.1f %.1f %.1f", *(int *)(base + XR64(0x422434)), (double)ps[0], (double)ps[1], (double)ps[2]);
				else
					log_printf("native: timelog p2ent cg.time %d pos %.1f %.1f %.1f", *(int *)(base + XR64(0x422434)), (double)e1[63], (double)e1[64], (double)e1[65]);
			}
		}
		seq++;
	}
	int full_window = !mode || (native_active && (!split_on || !sw));
	if (!mode || !sw || second_pass || !split_on) {
		if (mode == 11 && !split_on) { // back to one picture: player 1's view rectangle covers the screen again
			int *cvr = (int *)(base + M11_CG_VIEWRECT);
			const int sw_px = *(int *)(base + M11_DISP_W), sh_px = *(int *)(base + M11_DISP_H);
			if (sw_px > 0 && sh_px > 0 && (cvr[2] != sw_px || cvr[3] != sh_px))
				cvr[0] = 0, cvr[1] = 0, cvr[2] = sw_px, cvr[3] = sh_px;
		}
		if (full_window)
			cg_aspect_apply_full_window();
		cg_draw_original(local_client, a, b);
		if (ctx_on)
			ctx_after_draw();
		return;
	}
	if (mode == 11) { // both views in this frame: the per-player view rectangle (rva 0x3bd808, what 0xbd520 returns) top for player 1, bottom for player 2 (context 1, drawn
			  // as local client 0 with its statics swapped in); the second draw skips begin-frame (second_pass) and its static model chunks go to our buffers
		// The Present hooks (comp_hook): player 1's frame is ended with the show bit and marked 0x80, and the back end skips its Present only through them
		// (be_skip_present). Without this call the hooks came only with the developer console open or [native] presentkeep=1, so a player's session presented
		// both renderer frames of an engine frame (2026-11-17, found porting mode 11 to the 32-bit build).
		comp_hook(NULL);
		float *vrect = (float *)(base + M11_VIEWRECT);
		float *aspect = (float *)(base + RVA_CG_ASPECT);
		{
			static unsigned n11;
			if (n11++ % 1200 == 0)
				log_printf("native: mode 11 draw #%u (ctx1 ready %d)", n11, ctx1_ready);
		}
		const float aspect_was = *aspect;
		const int sw_px = *(int *)(base + M11_DISP_W), sh_px = *(int *)(base + M11_DISP_H); // the picture's size (the renderer's display globals)
		if (sw_px > 0 && sh_px > 0 && CFG_ONCE("exp11aspect", 1))
			*aspect = exp11_aspect(0, sw_px, sh_px); // a half's shape: the cgame builds the field of view for it
		int *cvr = (int *)(base + M11_CG_VIEWRECT); // the cgame's view rectangle in pixels (x, y, w, h; the refdef's, copied by 0xbe710; set at the level start by 0xa6680): one per context
		exp11_view(0, vrect);
		if (sw_px > 0 && sh_px > 0)
			exp11_pixels(0, sw_px, sh_px, cvr);
#ifdef _WIN64 // (the native renderers are 64-bit only)
		{ extern int nd_clear_trace; static int nc = -1; if (nc < 0) nc = CFG_ONCE("ndclearlog", 0); nd_clear_trace = nc; } // QA: colours clears of the native device, written once per frame end
#endif
		if (CFG_ONCE("clearlog", 0)) { // QA: the rectangles as they are set, once a second
			static unsigned n;
			if (n++ % 120 == 0)
				log_printf("native: split rects p1: view rect %.3f %.3f %.3f %.3f | cg view %d %d %d %d | picture %d %d", (double)vrect[0], (double)vrect[1], (double)vrect[2],
					   (double)vrect[3], cvr[0], cvr[1], cvr[2], cvr[3], sw_px, sh_px);
		}
		place_client_set(0, 0); // player 1's 2D goes to the top half
#ifdef _WIN64
		viewjit_log(0);
		vehjit_log(0);
#endif
		if (CFG_ONCE("animleaf", 0)) // QA: both bodies' trees before and after player 1's view
			animleaf_log("d0", 0, NULL), animleaf_log("d0", CFG_ONCE("animleafent", 1), NULL);
		cg_draw_original(local_client, a, b);
		if (CFG_ONCE("animleaf", 0))
			animleaf_log("e0", 0, NULL), animleaf_log("e0", CFG_ONCE("animleafent", 1), NULL);
		if (CFG_ONCE("camlog", 0)) { // QA: player 1's camera (the refdef origin, rva 0x42d310) and his own entity's lerp origin, sampled
			static unsigned k1;
			const float *v = (const float *)(base + XR(0x42d310)), *e0 = (const float *)(base + RVA_CENT + 0x18), *e1 = (const float *)(base + RVA_CENT + CENT_SIZE + 0x18);
			if (++k1 % 120 == 0 && k1 < 20000)
				log_printf("native: camlog p1 cam %.1f %.1f %.1f | ctx0 slot0 %.1f %.1f | slot1 %.1f %.1f", (double)v[0], (double)v[1], (double)v[2], (double)e0[0], (double)e0[1], (double)e1[0], (double)e1[1]);
		}
		if (CFG_ONCE("scenekeep", 0)) { // the scene's culling and light lists are finished by the front end's workers after the draw returns: wait for them, then keep its frame data
			ctx_wait_workers();
			fd_scene_save(*(uint8_t **)(base + XR64(0x410fd00)), 0);
		}
		if (ctx_on)
			ctx_after_draw();
		if (CFG_ONCE("exp11raw", 0)) { // QA: the picture as it is after player 1's draw, before player 2's: "raw1-<n>.bmp", four of them from exp11rawat on, exp11rawstep presents apart
			static LONG raw_n;
			const LONG at = CFG_ONCE("exp11rawat", 0), st = CFG_ONCE("exp11rawstep", 600) > 0 ? CFG_ONCE("exp11rawstep", 600) : 1;
			const LONG k = InterlockedIncrement(&raw_n);
#ifdef _WIN64
			{ static int tr0 = -1; if (tr0 < 0) tr0 = CFG_ONCE("tracescene", 0); ngfr_trace_scene = tr0; }
			if (k == 900 || k == 2700 || k == 5400)
				log_printf("native: exp11raw tick %ld (at %ld), trace %d", (long)k, (long)at, ngfr_trace_scene);
#endif
			if (k >= at && (k - at) % st == 0 && (k - at) / st < 4) {
				char raw_name[32];
				snprintf(raw_name, sizeof(raw_name), "raw1-%ld", (long)k);
				if (native_active)
					natboot_shot(raw_name);
				else if (*(IDirect3DDevice9 **)(base + RVA_D3D_DEVICE))
					shot_save_now(*(IDirect3DDevice9 **)(base + RVA_D3D_DEVICE), raw_name);
			}
		}
		const int p2draw = ctx1_ready && !CFG_ONCE("exp11nop2", 0); // QA: exp11nop2=1 skips player 2's draw (his client still runs)
		const int frames = CFG_ONCE("exp11frames", 2); // the 360's way: each player's view is its own renderer frame (docs/native-splitscreen-360.md)
		if (p2draw && frames) {
			// 360 (screen update 0x82135818): CG_DrawActiveFrame(lc) for each player, and after the first of two views the end-frame 0x82390e20(1): the frame
			// goes to the back end with "begin" (bit 0) but without "show" (bit 1); the last view ends with 2 (show). The PC kept that end-frame (0xc4c0(flags):
			// flags at frame data +0x41a60, frame begin 0x44d00 on bit 0, Present in 0x45060 on bit 1). So each view has its own frame data: its own scene,
			// sun cascades, light lists and model chunks, and the second draw begins its own frame (0xc840 inside 0xbca00).
			// First the frame's command list is closed, as the screen update does before its own end-frame (0xd5aa5) and as the 360 does after every view
			// (0x823912a8 in its loop): 0xe440 writes the end-of-list command (0x40000), clears the frame's list fields (+0x41a28), flips the command buffer
			// index and resets the scene counter (0x41136f0). Without it the back end read past player 1's list (crash in the 2D loop at 0x645bd) and player 2's
			// frame numbered its scene 1 ("scenes 2 views 1").
			if (CFG_ONCE("exp11wait", 1))
				ctx_wait_workers(); // player 1's front-end jobs (skinning, surface lists) read the frame data pointer 0x410fd00, which the end-frame clears and player 2's begin moves
			// The list's shape, as the screen update 0xd5890 builds it: the views' commands, end marker (0xc800, 0x40000; the 360's 0x823912a8 after each view),
			// the final 2D section's start kept at frame data +0x41a40 (0xc7e0), its first command 0x80017 (0xe900), then the close (0xe440).
			((void (*)(void))(base + M11_LIST_END))();
			((void (*)(void))(base + M11_LIST_2DSTART))();
			((void (*)(void))(base + M11_LIST_2DFIRST))();
			((void (*)(void))(base + M11_LIST_CLOSE))();
			// The PC's frame begin (0x44d00 on bit 0) is more than the 360's: BeginScene and the upload of the frame's dynamic data, so player 2's frame needs
			// bit 0 too (exp11frames=2: flags 1 then 3).
			static int nlog1;
			if (nlog1 < 4)
				log_printf("native: exp11frames: p1 end-frame (fd %p) ...", *(void **)(base + M11_FD_CUR));
			((void (*)(unsigned))(base + M11_END_FRAME))(p1_frame_flags());
			if (nlog1 < 4)
				nlog1++, log_printf("native: exp11frames: p1 end-frame done");
			if (CFG_ONCE("p2skindump", 0)) { // QA: which records and skin offsets player 2's frame leaves behind, before the clear
				const unsigned count = *(const unsigned *)(base + XR64(0x40dd754));
				unsigned shown = 0;
				for (unsigned i = 0; i < count && i < 0x200 && shown < 24; i++) {
					const uint8_t *r = base + XR64(0x40dd760) + (size_t)i * 0x90;
					const unsigned ent = (*(const unsigned *)(r + 0x70) >> 7) & 0xfff;
					const unsigned st = *(const unsigned *)(r + 0x28);
					if (ent <= 1 || st > 4) {
						log_printf("native: p2skin rec %u ent %u state %u dobj %p pose %p", i, ent, st, *(void *const *)(r + 0x78), *(void *const *)(r + 0x30));
						shown++;
					}
				}
				log_printf("native: p2skin %u records before the clear", count);
			}
			// The scene's model records (0x1440dd760, 0x90 each, count 0x1440dd754; state at +0x28 up to "skinned at arena offset + 4") are cleared once per screen
			// update (0x7b60 from 0xbc1a0), and the front end's loops (cull 0x17740, 0x16cc0, 0x16f80, 0x19000, 0x319a0, ...) walk all of them. So player 2's frame
			// also drew player 1's records, skinned into player 1's frame data (crash in the model surfaces 0x642e0). His frame starts with a clean model list.
			if (CFG_ONCE("exp11clear", 1)) {
				if (CFG_ONCE("exp11wait", 1))
					ctx_wait_workers();
				// The render thread draws player 1's frame meanwhile, and the back end reads these records too (0x46cc0): clearing them under it skinned the
				// bodies of both players into shards. Wait until it has finished the frame: the engine's own count of frames handed over and not done
				// (rva 0x1429b5c; begin-frame waits for <= 1 through 0x2039f0), with its own wait that helps the workers (0x301e0).
				if (CFG_ONCE("exp11rtwait", 1))
					m11_rt_wait(rt_frames_done);
				((void (*)(void))(base + M11_MODEL_CLEAR))();
			}
			p2_frame = 1;
#ifdef _WIN64
			if (CFG_ONCE("exp11reset", 1)) // the mod's reset_2d runs once per engine frame, as the game's own state reset does (natframe2.c)
				ng_skip_reset_2d = 1;
#endif
		} else if (p2draw) {
			uint8_t *fd = *(uint8_t **)(base + XR64(0x410fd00));
			if (fd)
				split_smc_redirect(fd);
		}
		if (p2draw) {
			exp11_view(1, vrect);
#ifdef _WIN64
			if (CFG_ONCE("flipwatch", 0))
				flip_watch(0);
#endif
			if (!CFG_ONCE("exp11same", 0)) // QA: exp11same=1 draws player 1 again in the second frame (no context swap)
				ctx_swap(1);
			// The cgame's aspect (RVA_CG_ASPECT, read by CG_CalcFov) is part of each context's cgame block: the value set above for player 1's draw went into context 0's copy, and
			// player 2's block still held the aspect of the whole screen, so his field of view was built for another shape than player 1's (a different zoom in the bottom half).
			// His copy gets the half's shape too ([native] exp11aspect=0 off).
			if (sw_px > 0 && sh_px > 0 && CFG_ONCE("exp11aspect", 1)) {
				static unsigned said_a;
				if (said_a++ < 3)
					log_printf("native: exp11aspect: player 2's cgame aspect was %.4f, player 1's draw used %.4f", (double)*(float *)(base + RVA_CG_ASPECT), (double)exp11_aspect(1, sw_px, sh_px));
				*(float *)(base + RVA_CG_ASPECT) = exp11_aspect(1, sw_px, sh_px);
			}
			if (sw_px > 0 && sh_px > 0)
				exp11_pixels(1, sw_px, sh_px, cvr);
			static dvar_t *draw2d;
			if (!draw2d)
				draw2d = game_find_dvar("cg_draw2D");
			// player 2's 2D (HUD, fades) is drawn over the whole screen unless the 2D is placed per view (exp11place, on by default: it goes into his half)
			const int no2d = draw2d && (exp11_cycle_state() >= 0 ? exp11_cycle_state() == 2 : CFG_ONCE("exp11no2d", CFG_ONCE("exp11place", 1) ? 0 : 1));
			const int was2d = no2d ? draw2d->current.enabled : 0;
			if (no2d)
				draw2d->current.enabled = 0;
			if (CFG_ONCE("camlog", 0)) { // QA: context 1's slots right after the swap, before player 2's draw
				static unsigned k3;
				const float *e0 = (const float *)(base + RVA_CENT + 0x18), *e1 = (const float *)(base + RVA_CENT + CENT_SIZE + 0x18);
				if (++k3 % 120 == 0 && k3 < 20000)
					log_printf("native: camlog p2 pre | ctx1 slot0 %.1f %.1f | slot1 %.1f %.1f", (double)e0[0], (double)e0[1], (double)e1[0], (double)e1[1]);
			}
			static dvar_t *gun11;
			if (!gun11)
				gun11 = game_find_dvar("cg_drawGun");
			const int nogun11 = gun11 && CFG_ONCE("exp11nogun", 0); // QA: player 2's draw without his view model
			const int wasgun = nogun11 ? gun11->current.enabled : 0;
			if (nogun11)
				gun11->current.enabled = 0;
			if (CFG_ONCE("timelog", 0)) { // QA: context 1's times at its draw, in the same form as context 0's lines (the draw hook below does not see this call)
				static unsigned seq1;
				LARGE_INTEGER pc, hz;
				QueryPerformanceCounter(&pc);
				QueryPerformanceFrequency(&hz);
				if (seq1 >= (unsigned)CFG_ONCE("timelogskip", 0) && seq1 < (unsigned)CFG_ONCE("timelogskip", 0) + 6000)
					log_printf("native: timelog #%u wall %.1f ctx %d cg.time %d frametime %d interp %.3f cl.serverTime %d frac %.3f", seq1, (double)pc.QuadPart * 1000.0 / (double)hz.QuadPart, ctx_current,
						   *(int *)(base + XR64(0x422434)), *(int *)(base + XR64(0x422430)), (double)*(float *)(base + XR64(0x42242c)), *(int *)(base + XR64(0x51dca8)), (double)*(float *)(base + XR64(0x51dcb0)));
				seq1++;
			}
			second_pass = frames ? 0 : 1;
			place_client_set(1, 0); // player 2's 2D goes to the bottom half
#ifdef _WIN64
			viewjit_log(1);
			vehjit_log(1);
#endif
			if (CFG_ONCE("animleaf", 0)) // QA: both bodies' trees before player 2's view
				animleaf_log("d1", 0, NULL), animleaf_log("d1", CFG_ONCE("animleafent", 1), NULL);
			cg_draw_vis(0, a, b);
			if (CFG_ONCE("npclog", 0) && ctx_current == 1) // QA: the actors as player 2's draw placed them (the frame hook logs context 0 only in mode 11)
				npc_log();
			if (CFG_ONCE("sunlog", 0) && ctx_current == 1) { // QA: player 2's sun state right after his draw (the frame hook logs context 0 only in mode 11)
				static unsigned sn1;
				if (++sn1 % 30 == 0) {
#ifdef _WIN64
					const float *sv = (const float *)(base + XR(0x45851b0)); // (RANGE_SUN, defined further down)
#else
					const float *sv = (const float *)(base + XR(0x204a260));
#endif
					log_printf("native: sunlog ctx 1 state %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f cg.time %d", (double)sv[0], (double)sv[1], (double)sv[2], (double)sv[3], (double)sv[4],
						   (double)sv[5], (double)sv[6], (double)sv[7], (double)sv[8], (double)sv[9], *(int *)(base + RVA_CG_TIME));
				}
			}
			place_client_set(0, 1); // the whole screen again, as at the video start
			second_pass = 0;
			if (nogun11)
				gun11->current.enabled = (uint8_t)wasgun;
			if (CFG_ONCE("camlog", 0)) {
				static unsigned k2;
				const float *v = (const float *)(base + XR(0x42d310)), *e0 = (const float *)(base + RVA_CENT + 0x18), *e1 = (const float *)(base + RVA_CENT + CENT_SIZE + 0x18);
				if (++k2 % 120 == 0 && k2 < 20000)
					log_printf("native: camlog p2 cam %.1f %.1f %.1f | ctx1 slot0 %.1f %.1f | slot1 %.1f %.1f", (double)v[0], (double)v[1], (double)v[2], (double)e0[0], (double)e0[1], (double)e1[0], (double)e1[1]);
			}
			if (frames)
				end_frame_flags = frames == 1 ? 2 : 3; // the screen update's own end-frame (0xd5aaf, 3) ends player 2's frame: show it (exp11frames=1: do not begin again)
			if (CFG_ONCE("scenekeep", 0)) {
				ctx_wait_workers();
				fd_scene_save(*(uint8_t **)(base + XR64(0x410fd00)), 1);
			}
			if (no2d)
				draw2d->current.enabled = (uint8_t)was2d;
#ifdef _WIN64
			if (CFG_ONCE("flipwatch", 0))
				flip_watch(1);
#endif
			if (!CFG_ONCE("exp11same", 0))
				ctx_swap_after_p2_draw();
		}
		{
			static unsigned n11b;
			uint8_t *fd = *(uint8_t **)(base + M11_FD_CUR);
			if (fd && n11b++ % 1200 == 0) {
				const int views = *(int *)(fd + M11_FD_VIEWS);
				char line[400];
				const int *cvr = (const int *)(base + M11_CG_VIEWRECT);
				int n = snprintf(line, sizeof(line), "native: mode 11 frame: scenes %d views %d | cg view rect %d %d %d %d", *(int *)(fd + M11_FD_SCENES), views, cvr[0], cvr[1], cvr[2], cvr[3]);
#ifdef _WIN64 // (the frame data's view array: 64-bit offsets)
				for (int i = 0; i < views && i < 4; i++) {
					const int *vp = (const int *)(fd + 0x1100 + (size_t)i * 0x150 + 0x130);
					n += snprintf(line + n, sizeof(line) - n, " | view %d rect %d %d %d %d", i, vp[0], vp[1], vp[2], vp[3]);
				}
#endif
				(void)n;
				log_printf("%s", line);
			}
		}
		vrect[0] = 0, vrect[1] = 0, vrect[2] = 1, vrect[3] = 1;
		*aspect = aspect_was;
	}
}

// ---- the engine's own local-players configuration (docs/native-splitscreen-plan.md, "Packet flow")
// G_InitGame stores the number of local players (rva 0x3c3eb0) as the immediate 1 of `mov dword [rip+x], 1` (c7 05 <rel32> 01 00 00 00 at rva 0x1714a6) when the co-op byte is 0.
// settings.ini [native] players=2 makes it 2: the server then makes two clients and two player entities, with no network and no co-op session. The client side has one
// element of each array, so this only shows what the server side does alone.
// Player 2's server commands in the split screen (64-bit, [native] p2cmds=0 off): the server queues them for client 1 and they reach context 1's cgame, which runs the ones that
// belong to a player (p2cmd_allowed). Without them every per-player command a script sent player 2 was lost: VisionSetNakedForPlayer and the night / thermal / missile camera / pain
// vision sets (the Predator's and the AC-130's views when he is the operator), SetBlurForPlayer, the screen fade, water drops, PlayLocalSound, the lock-on reticle, Show/HideViewModel.
// The Xbox 360 hands every local client its own commands (its CG_ServerCommand 0x82108618 runs per client and keeps only "s" and "checksound" for client 0).
static int p2cmd_filter_ok; // (only with the filter in place: unfiltered, context 1 would run the whole game's commands a second time)
static int p2cmds_on(void)
{
#ifdef _WIN64
	return native_active && ctx_on && ctx1_ready && p2cmd_filter_ok && CFG_ONCE("p2cmds", 1);
#else
	// 32 bits: off unless [native] p2cmds32=1. The port installs (QA p2cmds on mw2-32, 2026-11-17) but no command reached context 1's cgame, and while client 1's
	// commands are queued without being handed out the server's ring could run over ("Server command overflow"); not shipped on until that is understood.
	return native_active && ctx_on && ctx1_ready && p2cmd_filter_ok && CFG_ONCE("p2cmds32", 0);
#endif
}
static void sv_queue(uint8_t *cl, const char *text);
// Player 2's weapon switches: a script's SwitchToWeapon and every loadout's switch are the server command "sw <weapon index>" (G_SelectWeaponIndex, 64-bit rva 0x1a1f50), and
// his client follows only that: without it his usercmd keeps the weapon his context last chose, and the server goes back to it. The level's start hands out the loadouts before
// context 1 takes commands, so that "sw" was dropped and player 2 stood with the weapon context 0 had when context 1 was copied from it (Vlad 2026-11-21, so_ac130_co_hunted:
// player 1 in the AC-130 has no weapons, player 2 on the ground had empty hands until he switched himself). The last one dropped is sent once context 1 takes commands.
static char p2_sw_pending[32];
static DWORD p2_sw_pending_at;
static void p2_sw_flush(void)
{
	if (!p2_sw_pending[0] || !p2cmds_on())
		return;
	char sw[32];
	lstrcpynA(sw, p2_sw_pending, sizeof(sw));
	p2_sw_pending[0] = 0;
	if (GetTickCount() - p2_sw_pending_at > 60000)
		return; // (an earlier session's)
	sv_queue(base + RVA_SV_CLIENTS + SV_CLIENT_STRIDE, sw);
	log_printf("native: p2cmds: player 2's weapon switch from before his context took commands sent now (%s)", sw);
}
static void sv_add_command(uint8_t *cl, const char *text)
{
	int idx = (int)((cl - (base + RVA_SV_CLIENTS)) / SV_CLIENT_STRIDE);
	int in_use = *(base + RVA_COOP_BYTE) ? *(int *)(base + RVA_COOP_SLOTS + idx * SLOT_STRIDE) == 2 : idx < *(int *)(base + RVA_LOCAL_PLAYERS);
	if (!in_use)
		return;
	if (idx == 1 && !*(base + RVA_COOP_BYTE)) {
		if (!p2cmds_on()) {
			if (!strncmp(text, "sw ", 3) && strlen(text) < sizeof(p2_sw_pending)) {
				lstrcpynA(p2_sw_pending, text, sizeof(p2_sw_pending));
				p2_sw_pending_at = GetTickCount();
			}
			return;
		}
		if (!strncmp(text, "sw ", 3))
			p2_sw_pending[0] = 0; // (a newer switch)
		else
			p2_sw_flush();
	}
	sv_queue(cl, text);
}
static void sv_queue(uint8_t *cl, const char *text)
{
	void (*com_error)(int, const char *, ...) = (void (*)(int, const char *, ...))(base + RVA_COM_ERROR);
	int *p = (int *)cl;
	int seq = p[2];
	if (seq - p[3] == 0x80) {
		com_error(1, "Server command overflow");
		seq = p[2];
	}
	p[2] = seq + 1;
	int len = (int)strlen(text);
	int off = p[1];
	if (0x1000 < off + 1 + len) {
		com_error(1, "Reliable command buffer overflow");
		off = p[1];
	}
	*(int *)(cl + 0x1010 + ((seq + 1) & 0x7f) * 4) = off;
	char *d = (char *)(cl + 0x10 + p[1]);
	for (int i = 0; i < len; i++)
		*d++ = text[i] == '%' ? '.' : text[i];
	*d = 0;
	p[1] += len + 1;
}

// Debug aid ([native] logcbuf=1): every text the engine adds to the command buffer, in the log.
static const uint8_t cbuf_bytes[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x8b, 0xf9, 0x48, 0x8b, 0xda};
static void (*cbuf_original)(int, const char *);
static void cbuf_hook(int lc, const char *text)
{
	char line[200];
	size_t i = 0;
	for (; text && text[i] && text[i] != '\n' && i < sizeof(line) - 1; i++)
		line[i] = text[i];
	line[i] = 0;
	log_printf("cbuf(%d): %s", lc, line);
	cbuf_original(lc, text);
}

// ---- a second client context (docs/native-splitscreen-plan.md): the engine's client code runs for local client 1 as local client 0, with the client statics swapped
// The arrays of the client side (cl, clc, cls, the cgame's cg/cgs) have one element, so the loops over the local clients (i < number of local players) would write local client 1's
// state over unrelated statics. Each call site of such a loop is redirected to a wrapper: for client 1 it swaps the client statics to context 1's copy, calls the function as
// client 0, and swaps back. The ranges are the first guess (cgame block, snapshots, cl, clc, cls, input); the ones the experiments show missing are added.
typedef struct {
	uint32_t rva, size;
} range_t;
#ifdef _WIN64
static const range_t ranges[] = {
    {0x404450, 0x44f000 - 0x404450}, // cgs, cg, cg_entities and neighbours
    {0x51b078, 0x51dcb0 - 0x51b078}, // cl snapshots and their pointers
    {0x52c400, 0x100},               // cleared by the cgame init for each client
    {0x52fbc0, 0x21c},               // input
    {0x533cb8, 0x68},                // cls state, the mouse state and the view angle accumulators (0x533cc0, 0x533d0c-0x533d18)
    {0x533d20, 0x540b60 - 0x533d20}, // cl: usercmd ring, cmdNumber, playerstate, packet history
    {0x545844, 0x3218},              // clc: reliable command ring
    {0x548ce8, 8},                   // the snapshot message's time and flag
    {0x3cf180, 0xe80},               // aim assist of local client 0 (aaGlob: the targets on screen, the aim_* regions): each context's cgame fills its own
    {0xbc7300, 0xcdb110 - 0xbc7300}, // the effects system of local client 0 (FX_InitSystem 0x119a00: header 0xb18, its pools from 0xbc7e80, 0x10ea80 bytes, then 0xcd6900 + 0x4810):
                                     // with one system both halves drew the muzzle flash and smoke of the first-person weapon of the other ([native] fxsplit=0 off)
    {0x4812878, 0xdb0},              // the renderer's view of each local client (0x2b8 each: frustum planes, the sides of the view; R_RenderScene rva 0x5160 reads it at
                                     // 0x4812878 + local client * 0x2b8) and the camera's visible cell bits after them (0x4813548 pointer, 0x4813550 bits, cleared and filled by the
                                     // portal walk rva 0x19760 each frame). The sun shadow set-up comes before the walk and culls its casters with the LAST frame's cells: the other
                                     // view's, so a half's static sun shadows came and went with where the other player looked (the camo net over the trainer's grenade box).
                                     // [native] viewsplit=0 off
    {0x45851b0, 0x38},               // the sun's blind and glare state of local client 0 (rva 0x4dd80 lerps its blind / glare amounts, 0x4e030 its flare; the array is indexed by local client,
                                     // stride 0x38): with one state the fade of a player who looks into the sun (white-out, or the dark blind) was drawn in the other half too
    {0x3efcc8, 0x1c},                // the screen blur of local client 0 (scr_blur / clear_blur, set by 0x8cf00 and 0x8cf90, stepped by 0x8cfe0, read by 0x8cfc0 for the view; stride 0x1c):
                                     // with one state the damage blur of a player who was hit showed in the other half too (v0.6.0 report). [native] blursplit=0 off
    // (tried: 0x548c00 .. 0x589400, 260 KB changed by both contexts with different values and outside every range, the size of the snapshot entity states: swapping it garbled the
    // geometry of the second half in a test, so it is not per-client snapshot data: the shared renderer / model data lives there)
    {0x51dcb0, 0x51de80 - 0x51dcb0}, // the player bodies' bone controllers (rva 0x9df60 from the pass 0xb0860: new 0x51dcc4, old 0x51dd54, blended 0x51dde4 + entity * 0x48, the
                                     // interpolation 0x51dcb0), right after the snapshot range: with one table, player 2's cgame wrote over what player 1's had set before player 1's
                                     // frame skinned the body (tried for the smeared bodies of mode 11: it brought the shards back, the copy of player 1's body into context 1 relies on one table). [native] ctlsplit=1 on, off by default
    {0x3efcf0, 0x94},                // the camera shakes (CG_StartShakeCamera 0x8d420, called from the event handler 0x9f9a0: four slots of 0x24 from 0x3efcf0, a field to 0x3efd84): one set for
                                     // both views, into which both contexts' event passes put every explosion's shake (twice). The Xbox 360 keeps 0x94 bytes per player (0x824289d8 +
                                     // lc * 0x94, filled by its loop 0x820dcb68). Night audit, engine fix 6. [native] shakesplit=0 off
    {0x1b761a8, 0x38},               // the view of a player who rides a vehicle (rva 0x27f9e0, the cgame's rider view: it sets the client's view angles every frame through CL_SetViewAngles
                                     // 0xca760): its smoothing state, a table by client number at 0x1b761a8 (stride 0x38) with room for client 0 only (guards.c runs client 1 as client 0, the
                                     // next bytes are the weapon definition table). One state for both players: in the Spec Ops snowmobile races both cameras made the same turns, neither
                                     // with its own sled (Vlad: "camera turns do not work, strong jitter for both"; QA 2026-11-17: both view yaws on one curve while the sleds' yaws
                                     // differed). Each context keeps its own. [native] ridesplit=0 off
    {0x52fb70, 8},                   // frame_msec and the time the usercmds were last made (CL_CreateNewCommands rva 0xcf800: msec = com_frameTime - that time, then that time = now):
                                     // with one pair the context made second in a frame always got msec 0, and CL_MouseMove (0xcfcf0) does nothing at 0: player 2 on the keyboard
                                     // and mouse could not turn his view at all (Vlad, Big Brother "reverse": the minigun's camera did not turn; QA cmdlog1: frame_msec 0, no mouse
                                     // read in context 1). Each context keeps its own. [native] msecsplit=0 off
    {0x400518, 0x2a8},               // the lasers to draw this frame (count 0x400518, entity numbers 0x400520 (0x60 x 2 bytes), "on" bytes 0x4005e0, entity flags 0x400640): every
                                     // entity, event and weapon fire pass appends to it (0xa57f0), the screen update's client time call empties it (0xa5850 from 0xbc1a0, player 1's
                                     // only) and the cgame draw draws it (0xa5920): player 2's draw drew player 1's entries and then his own, each laser twice in his half (QA laserdup
                                     // 2026-11-21: 4 lasers drawn for player 1, 8 for player 2; Spec Ops engine audit row E). Each context keeps its own, player 2's emptied before his
                                     // step (cg_step_hook). [native] lasersplit=0 off
};
#define RANGE_FX 9
#define RANGE_VIEW 10
#define RANGE_SUN 11
#define RANGE_BLUR 12
#define RANGE_CTL 13
#define RANGE_SHAKE 14
#define RANGE_RIDE 15
#define RANGE_MSEC 16
#define RANGE_LASER 17
#define RANGE_LASER_N 1
#define RVA_LASER_COUNT 0x400518
#define SUNSPLIT_DEFAULT 1
#else
// 32 bits (docs/32bit-native-map/clientctx.md): the same eight kinds of state in the same order (the code indexes them), the cgame block in two parts: [0] is its main part
// (the first 135 entities included, as in 64 bits) and [9] the view model pointers behind the entity array
static const range_t ranges[] = {
    {0x35f280, 0x7a0000 - 0x75f280}, // cgs, cg, media tables, cg_entities 0..134
    {0x46798c, 0x2c30},              // cl snapshots and their pointers
    {0x46b180, 0x100},               // cleared by the cgame init for each client
    {0x4889b0, 0x21c},               // input
    {0x525bc0, 0x80},                // cls state, the mouse state and the view angle accumulators (0x925c34 pitch, 0x925c38 yaw)
    {0x525c40, 0xbe3c},              // cl: usercmd ring (64 deep), cmdNumber, playerstate (+0x1018), packet history
    {0x48c330, 0x3218},              // clc: reliable command ring
    {0x494424, 8},                   // the snapshot message's time and flag
    {0x33ba90, 0xe80},               // aim assist of local client 0 (aaGlob)
    {0x45e8a4, 0x2c},                // the cgame's view model pointers (2 hands x 0x14)
    {0x983e80, 0xa90c80 - 0x983e80}, // the effects system of local client 0 (FX_InitSystem VA 0x49e390: header VA 0xd83e80 (0xad0), the block 0xd84980 (0x4810), the pools from
                                     // 0xd89200 (0x107a80 bytes)): see the 64-bit range; [native] fxsplit=0 turns it off
    {0x1af9bc8, 0xbd8},              // the renderer's view of each local client (VA 0x1ef9bd0 + local client * 0x2b8, copied by VA 0x516950) and the camera's visible cell
                                     // bits (pointer VA 0x1efa6c0, bits 0x1efa6c4, cleared and filled by the portal walk VA 0x516320): see the 64-bit range ([native] viewsplit=0 off)
    {0x204a260, 0x28},               // the sun's blind and glare state of local client 0 (VA 0x244a260 + local client * 40, read by VA 0x543290 and its callees): see the 64-bit range
                                     // ([native] sunsplit=0 off)
    {0x341230, 0x1c},                // the screen blur of local client 0 (VA 0x741230 + local client * 0x1c; clear_blur is VA 0x4a5690): see the 64-bit range ([native] blursplit=0 off)
    {0x4889a0, 8},                   // the time the usercmds were last made and frame_msec (CL_CreateNewCommands VA 0x579bb0; CL_MouseMove VA 0x578fe0 does nothing at msec 0): see the
                                     // 64-bit range ([native] msecsplit=0 off)
    {0x1799808, 0x38},               // the view of a player who rides a vehicle (VA 0x4cbb00, the 64-bit 0x27f9e0): its state by local client at VA 0x1b99808 (stride 0x38) has room for
                                     // client 0 only, and right after it are the vehicles' client blocks (VA 0x1b99848, 64 x 0x80). Context 1 (local client number 1) wrote its state
                                     // into vehicle slot 0's block: its definition index became a float and the next vehicle update faulted (Cliffhanger's snowmobiles, 32-bit and
                                     // iw4x-sp, the moment the split began; found with !watchw, 2026-11-21). guards.c runs client 1 as client 0; each context keeps its own state
                                     // here, as in 64 bits ([native] ridesplit=0 off)
    {0x3517b8, 0x180},               // the lasers to draw this frame (see the 64-bit range; the append VA 0x48ed80, the draw VA 0x4ba470), in four parts here: the cgame's dvar pointers lie
    {0x351938, 0x60},                // between them. Entity flags VA 0x7517b8 (0x60 x 4 bytes), "on" bytes VA 0x751938, the count VA 0x7519c4, entity numbers VA 0x7519d8 (0x60 x 2 bytes)
    {0x3519c4, 4},                   // ([native] lasersplit=0 off)
    {0x3519d8, 0xc0},
};
#define RANGE_FX 10
#define RANGE_VIEW 11
#define RANGE_SUN 12
#define RANGE_BLUR 13
#define RANGE_MSEC 14
#define RANGE_RIDE 15
#define RANGE_LASER 16
#define RANGE_LASER_N 4
#define RVA_LASER_COUNT 0x3519c4
#define SUNSPLIT_DEFAULT 1 // (with the wait for the frame in flight, see sunwait in ctx_swap_mask)
#endif
#define NRANGES (sizeof(ranges) / sizeof(ranges[0]))
// Waits until the render thread has no frame in flight (the frame it draws reads the sun and the shadow state that a context swap changes). The engine's own wait loop takes the
// predicate: 64-bit rva 0x301e0 (an argument); the 32-bit counter is VA 0x1551b60 (the engine's own predicate VA 0x4d5ba0 is "< 2", this one is "< 1"; iw4x-sp: rva 0x11550e0).
static int render_idle_pred(void)
{
#ifdef _WIN64
	return *(volatile int *)(base + XR(0x1429b5c)) < 1;
#else
	return *(volatile int *)(base + XR(0x1151b60)) < 1;
#endif
}
static void render_wait_idle(void)
{
#ifdef _WIN64
	((void (*)(int (*)(void)))(base + XR(0x301e0)))(render_idle_pred);
#else
	// 32 bits: the engine's own wait (m11_rt_wait: VA 0x52afc0, the iw4x-sp exe's shape handled there), which runs the workers' jobs meanwhile, as the 64-bit build does. It
	// polled the counter with Sleep(0) for up to 50 ms before: the first context swap of every mode-11 frame waits here for the render thread (the sun range), and on
	// Second Sun 5 % of the 32-bit frames took 16 ms or more (2026-11-19; the swap's copy time was 174 ms a second, most of it this wait). [native] rtpoll=1 polls again.
	if (CFG_ONCE("rtpoll", 0)) {
		const DWORD t0 = GetTickCount();
		while (!render_idle_pred() && GetTickCount() - t0 < 50)
			Sleep(0);
	} else
		m11_rt_wait(rt_frames_done);
#endif
}
#define RANGE_RVA(i) XR(ranges[i].rva) // (iw4x-sp: the range's start in the 159 image, the same length)
static uint8_t *ctx_store[2][NRANGES];
static int ctx_current;
int native_log_ctx(void) { return native_active ? ctx_current : 0; } // (for the script debug log of the 32-bit builds)
#ifdef _WIN64
static const char *vision_name_of(int ctx) // the cgame block's visionset_naked name of a context (live when it is the current one)
{
	return ctx == ctx_current ? (const char *)(base + XR(0x43c0ac)) : ctx_store[ctx][0] ? (const char *)(ctx_store[ctx][0] + (0x43c0ac - 0x404450)) : NULL;
}
#else
static const char *vision_name_of(int ctx) { (void)ctx; return NULL; }
#endif

#ifdef _WIN64
// Diagnostics ([native] xswap=<mask>): renderer statics swapped with the contexts too (bit 0: shadow light ring and parameters, 1: the scene frontend globals, 2: the sun shadow cache per view)
static const struct { unsigned rva, size; } xranges[] = {{0x40d2580, 0x480}, {0x4113400, 0x400}, {0x4359180, 0x200}};
static uint8_t *x_store[2][3];
// [native] xswapr=<rva>:<size>,<rva>:<size>,... (diagnostics, up to 24 ranges, hex): more renderer statics swapped with the contexts, to find per-view state
static struct { unsigned rva, size; uint8_t *store[2]; } xr[24];
static int xr_n = -1;
static int xr_render_idle(void)
{
	return *(volatile int *)(base + XR(0x1429b5c)) < 1;
}
static void xr_parse(void)
{
	char v[600];
	xr_n = 0;
	GetPrivateProfileStringA("native", "xswapr", "", v, sizeof(v), settings_path);
	for (char *tok = strtok(v, ","); tok && xr_n < 24; tok = strtok(NULL, ",")) {
		unsigned rva = 0, size = 0;
		if (sscanf(tok, "%x:%x", &rva, &size) != 2 || !size || size > 0x200000)
			continue;
		xr[xr_n].rva = rva, xr[xr_n].size = size;
		xr[xr_n].store[0] = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		xr[xr_n].store[1] = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		memcpy(xr[xr_n].store[1], base + rva, size);
		log_printf("native: xswapr: rva 0x%x, 0x%x bytes swapped per context", rva, size);
		xr_n++;
	}
}
#endif
// The laser list goes with the contexts ([native] lasersplit=0 off) when every part of it was found in this build.
static int laser_split_on(void)
{
	static int on = -1;
	if (on < 0) {
		on = cfg("lasersplit", 1);
		for (int i = RANGE_LASER; on && i < RANGE_LASER + RANGE_LASER_N; i++)
			if (!addr_resolved(ranges[i].rva))
				on = 0;
	}
	return on;
}
static void ctx_ensure(void)
{
	if (ctx_store[0][0])
		return;
	for (size_t i = 0; i < NRANGES; i++) { // first use: both copies start as what the live memory holds now
		ctx_store[0][i] = VirtualAlloc(NULL, ranges[i].size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		ctx_store[1][i] = VirtualAlloc(NULL, ranges[i].size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		memcpy(ctx_store[1][i], base + RANGE_RVA(i), ranges[i].size);
	}
#ifdef _WIN64
	for (int i = 0; i < 3; i++)
		if (cfg("xswap", 0) & (1 << i)) {
			x_store[0][i] = VirtualAlloc(NULL, xranges[i].size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			x_store[1][i] = VirtualAlloc(NULL, xranges[i].size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			memcpy(x_store[1][i], base + XR(xranges[i].rva), xranges[i].size);
		}
#endif
}

// `mask`: the ranges (bit i = ranges[i]) that are swapped; a function that touches only the cl block need not expose the other threads to a swapped cgame block. A masked swap
// must be undone with the same mask, and not nest with another.
#define CTX_ALL ((1u << NRANGES) - 1)
#define CTX_CL (1u << 5)
#define CTX_CL_CLC ((1u << 5) | (1u << 6))
// The renderer's worker threads finish a frame's scene while the client goes on (types at rva 0x370340: "add scene ent", "skin model", the cell and shadow entities...):
// they read the cgame's entities and the skeletons through pointers into the live client statics. A swap rewrote those under them, and the bottom half's far NPCs were
// skinned with context 0's data for a frame (2026-10-28: gone with r_smp_worker 0, not with r_smp_backend 0). Before a swap, the engine's own wait for a type of worker
// command (rva 0x300a0: it helps the workers until the type's count of commands in flight is 0) runs for types 0-18; 19 and 20 are physics ([native] workerwait=0 off).
static void ctx_wait_workers(void)
{
	static int on = -1;
	if (on < 0)
		on = cfg("workerwait", 1);
	if (!on)
		return;
	void (*wait_type)(int) = (void (*)(int))(base + RVA_WAIT_TYPE);
	int perf_on = CFG_ONCE("perflog", 0);
	LONGLONG t0 = perf_on ? perf_now() : 0;
	for (int t = 0; t <= 18; t++)
		wait_type(t);
	if (perf_on)
		perf_wait += perf_now() - t0;
}

static struct {
	uintptr_t site;
	unsigned mask;
	long n, real;
	LONGLONG wait, copy;
} swap_sites[40];
static int swap_site_cur = -1; // (the entry of the swap in progress: its wait and copy time go there)
static LONGLONG perf_now(void);
static void swap_site_note(uintptr_t site, unsigned mask)
{
	static LONGLONG next;
	swap_site_cur = -1;
	for (int i = 0; i < 40; i++)
		if ((swap_sites[i].site == site && swap_sites[i].mask == mask) || !swap_sites[i].site) {
			swap_sites[i].site = site, swap_sites[i].mask = mask, swap_sites[i].n++;
			swap_site_cur = i;
			break;
		}
	LARGE_INTEGER c, f;
	QueryPerformanceCounter(&c);
	QueryPerformanceFrequency(&f);
	if (!next)
		next = c.QuadPart + 10 * f.QuadPart;
	if (c.QuadPart >= next) {
		next = c.QuadPart + 10 * f.QuadPart;
		for (int i = 0; i < 40 && swap_sites[i].site; i++) {
			{ HMODULE me = NULL; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)swap_sites[i].site, &me); log_printf("native: swapsites: caller %s+0x%x mask 0x%x: %ld swaps in 10 s, %ld moved data, worker waits %.1f ms, copies %.1f ms", me == (HMODULE)base ? "exe" : "dll", (unsigned)(swap_sites[i].site - (uintptr_t)me), swap_sites[i].mask, swap_sites[i].n, swap_sites[i].real, (double)swap_sites[i].wait * 1000.0 / (double)f.QuadPart, (double)swap_sites[i].copy * 1000.0 / (double)f.QuadPart); }
			swap_sites[i].n = swap_sites[i].real = 0, swap_sites[i].wait = swap_sites[i].copy = 0;
		}
	}
}
// Which context's data the live memory holds for each range. A swap only moves the ranges whose live data is not the target context's: the hand-over hooks (the server step,
// the snapshot, the server commands) swap only the ranges they use (CTX_HAND: not the effects system, 1.1 MB, nor the renderer's view and sun state), so those stay with the context
// whose draw they belong to and the next draw swap finds them in place.
static int range_owner[NRANGES];
// A copy for the big ranges: `rep movsb` (fast-string microcode on current CPUs), against the C library's memcpy; [native] fastcopy=0 uses memcpy.
static void swap_copy(void *dst, const void *src, size_t n)
{
	static int fast = -1;
	if (fast < 0)
		fast = cfg("fastcopy", 1);
	if (fast && n >= 4096)
		__asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
	else
		memcpy(dst, src, n);
}
// The player's own field-of-view scale (2026-11-15, from the 360). Scripts set one per player: maps/_ac130.gsc and _cobrapilot.gsc write cg_playerFovScale0 for
// level.player and cg_playerFovScale1 for player 2. The 360's FOV function (0x8210fcd8) takes the one of the client slot of its local player (0x820d81a8, the PC's
// 0xa5ec0); the PC's (0xbc1e0, at 0xbc2de) takes it by the local player number, which is 0 for both players of the split screen: player 1 in the AC-130 zoomed
// player 2's view too, and player 2's own scale was never read. The 20 bytes of that choice are replaced by a load from fov_scale_sel, which follows the context:
// context 1 (player 2) reads cg_playerFovScale1 ([native] fovsplit=0 off).
#ifdef _WIN64
static void *volatile fov_scale_sel;
static void fov_scale_install(void);
static void fov_scale_follow(int to)
{
	if (!fov_scale_sel) { // the dvars are registered by the cgame: installed at the first swap that finds them
		static int tries;
		if (tries < 1000 && native_active) {
			tries++;
			fov_scale_install();
		}
		if (!fov_scale_sel)
			return;
	}
	fov_scale_sel = *(void **)(base + XR(to == 1 ? 0x51e1c8 : 0x51e1c0));
}
static void fov_scale_install(void)
{
	static const uint8_t want[20] = {0x8b, 0xd3, 0x48, 0x8b, 0x05, 0xe1, 0x1e, 0x46, 0x00, 0x85, 0xd2, 0x75, 0x07, 0x48, 0x8b, 0x05, 0xce, 0x1e, 0x46, 0x00};
	uint8_t *p = base + XR(0xbc2de);
	if (fov_scale_sel || !CFG_ONCE("fovsplit", 1))
		return;
	if (!*(void **)(base + XR(0x51e1c0)) || !*(void **)(base + XR(0x51e1c8)))
		return;
	if (memcmp(p, want, sizeof(want))) {
		static int said;
		if (!said++)
			log_printf("native: fovsplit: unexpected code at rva 0xbc2de, the FOV scale stays the game's");
		return;
	}
	fov_scale_sel = *(void **)(base + XR(0x51e1c0));
	uint8_t put[20];
	const uint64_t at = (uint64_t)(uintptr_t)&fov_scale_sel;
	put[0] = 0x48, put[1] = 0xb8; // mov rax, &fov_scale_sel
	memcpy(put + 2, &at, 8);
	put[10] = 0x48, put[11] = 0x8b, put[12] = 0x00; // mov rax, [rax]
	memset(put + 13, 0x90, 7);
	DWORD old;
	VirtualProtect(p, sizeof(put), PAGE_EXECUTE_READWRITE, &old);
	memcpy(p, put, sizeof(put));
	VirtualProtect(p, sizeof(put), old, &old);
	FlushInstructionCache(GetCurrentProcess(), p, sizeof(put));
	log_printf("native: fovsplit: each player's view takes his own cg_playerFovScale (as on the 360)");
	fov_scale_sel = *(void **)(base + XR(ctx_current == 1 ? 0x51e1c8 : 0x51e1c0));
}
#else
static void fov_scale_follow(int to) { (void)to; }
#endif
// The players' entities are ONE table for both local players, as on the 360 (2026-11-15). The 360 keeps a cg_t per local player but one cg_entities
// (0x824ae9c0, 0x194 each, indexed by entity number only): its own-entity write (0x82102f28) puts each local player's state into the slot of his client
// (0x820d81a8) and the pass over the other players' bodies (0x82100ff8) draws every slot but that one, so each player's body in the other view is what his own
// frame wrote. The PC's context swap carried a copy of slots 0 and 1 per context, and the server's snapshot list leaves the clients out (0x234540: the
// entities below 2 that are clients are skipped), so player 1's slot in player 2's context stayed where it was at the start and the mod copied player 1's
// entity in for each draw (p0copy, p0frame). Here the two slots stay in place through a swap of the cgame range ([native] centshare=0: the copies as before).
static int cent_share_on(void)
{
	return mode == 11 && CFG_ONCE("centshare", 1);
}
// The whole table, not only the players' slots (2026-11-21). The cgame range of the swap ends inside cg_entities (64 bits: 0x44f000, in slot 134; 32 bits: 0x3a0000, in slot
// 135), so entities 2..134 had a copy per context, while every entity from 135 on was one for both. Context 1 never refreshes the entity states (its snapshot transition is
// context 0's, see ent_clean_hook), so its copies kept what they held when the split began: a helicopter among them (so_killspree_trainer, entities 26 and 33) was drawn in
// player 2's half at that old place, interpolated from the same two states with every new snapshot's fraction: the same 50 ms of flight again and again, back to its start
// at each snapshot (Vlad and the owner, 2026-11-20: "looks stuck, trying to get out, jitter back and forth"; npclog: 16% of player 2's draws went backwards, 0% of player 1's,
// and the helicopter from entity 698 was smooth in both). The swap now leaves the table in place, as the 360 keeps one cg_entities for all local players
// ([native] centall=0: the copies of slots 2..134 per context as before).
static int cent_all_on(void)
{
	return cent_share_on() && CFG_ONCE("centall", 1) && RVA_CENT > RANGE_RVA(0) && RVA_CENT < RANGE_RVA(0) + ranges[0].size;
}
static int p0copy_on(void)
{
	return CFG_ONCE("p0copy", cent_share_on() ? 0 : 1);
}
#if defined(_WIN64) && defined(RANGE_FX)
// Diagnostic ([native] fxspotcheck=1): the live effects system's spot light (one per system: count +0xa2c, element count +0xa30, effect handle +0xa34) must point at a live
// effect (its definition pointer not null); logged when the effects range leaves or enters a context with it broken.
static void fx_spot_check(const char *what, int ctx)
{
	static int logged;
	static long checks;
	static int last[2][4], have[2];
	if (logged >= 60 || !CFG_ONCE("fxspotcheck", 0) || ctx < 0 || ctx > 1)
		return;
	checks++;
	const uint8_t *sys = base + XR(0xbc7300);
	const int ecount = *(const int *)(sys + 0xa2c), lcount = *(const int *)(sys + 0xa30);
	const int eh = *(const uint16_t *)(sys + 0xa34), lh = *(const uint16_t *)(sys + 0xa36);
	const uint8_t *eff = *(uint8_t *const *)(sys + 0x180) + (size_t)eh * 4;
	const int def_live = *(void *const *)eff != NULL;
	const int now[4] = {ecount, lcount, eh, lh};
	if (have[ctx] && !memcmp(now, last[ctx], sizeof(now)))
		return;
	memcpy(last[ctx], now, sizeof(now));
	have[ctx] = 1;
	logged++;
	log_printf("native: fxspot: context %d %s: spot effects %d elems %d, effect %d (%s), elem %d%s (check %ld)", ctx, what, ecount, lcount, eh, def_live ? "live" : "FREE", lh,
		   (lcount > 0 && (ecount <= 0 || !def_live)) ? " BROKEN" : "", checks);
}

// Debris with physics: an effect element of a model with physics (element type 7, definition flag 0x8000000) owns a physics object of the effects' world (world 1), made at
// rva 0x11c65f (Phys_ObjCreate 0x1e5de0, kept at element +0x18) and destroyed when the element is freed (FX_FreeElem 0x1192c0, the call at 0x119382 to Phys_ObjDestroy 0x1e5f40).
// The physics pool is one for the process (0x12657a0 bodies, 0x12df1e0 their states). Context 1's effects system starts as a copy of context 0's (ctx_seed_cgame), so each debris
// element alive at that moment was in both systems with the same physics object, and both freed it: a double free in the pool, whose next allocation read -1 (the Gulag "cafe"
// start crashed 3 s after the split began, every run: rva 0x2713ee). Now context 1 destroys only the objects its own system made; the copies stay context 0's, which frees them
// once. Until then both views draw the same debris (the effects only read an object's state, through the step 0x11f0a0 -> 0x1e7ca0). [native] fxbodyown=0 off.
static void *volatile fxbody_ctx1[512]; // the physics objects context 1's system made (the pool holds 448)
static volatile LONG fxbody_kept, fxbody_full;
static void *(*fxbody_create_original)(int, void *, void *, void *, void *);
static void (*fxbody_destroy_original)(int, void *);
static void *fxbody_create_hook(int world, void *origin, void *quat, void *velocity, void *preset)
{
	void *body = fxbody_create_original(world, origin, quat, velocity, preset);
	if (body && range_owner[RANGE_FX] == 1) { // runs on the workers too: a slot is taken with a compare-exchange
		for (int i = 0; i < 512; i++)
			if (!fxbody_ctx1[i] && !InterlockedCompareExchangePointer((void *volatile *)&fxbody_ctx1[i], body, NULL))
				return body;
		InterlockedIncrement(&fxbody_full);
	}
	return body;
}
static void fxbody_destroy_hook(int world, void *body)
{
	if (body && range_owner[RANGE_FX] == 1) {
		for (int i = 0; i < 512; i++)
			if (fxbody_ctx1[i] == body && InterlockedCompareExchangePointer((void *volatile *)&fxbody_ctx1[i], NULL, body) == body) {
				fxbody_destroy_original(world, body);
				return;
			}
		InterlockedIncrement(&fxbody_kept); // a copy of context 0's object: context 0 frees it
		return;
	}
	fxbody_destroy_original(world, body);
}
static void fxbody_reset(void) // context 1 starts again as a copy (each level): what it made before went with its old system
{
	for (int i = 0; i < 512; i++)
		fxbody_ctx1[i] = NULL;
}
static void fxbody_report(void) // on the main thread (the swap), when the counts changed
{
	static LONG kept_logged, full_logged;
	const LONG kept = fxbody_kept, full = fxbody_full;
	if (kept != kept_logged || full != full_logged) {
		if (kept_logged < 20 || full != full_logged)
			log_printf("native: fxbody: context 1 left %ld copied debris objects to context 0%s", (long)kept, full ? " (its table was full: some of its own objects are never freed)" : "");
		kept_logged = kept;
		full_logged = full;
	}
}
// Context 1's cgame has cg.localClientNum (rva 0x40caa4) = 1 (ctx1lc: the sound listener mask 0xa6d20 needs it), but every client table that these readers index
// has room for one client (Spec Ops engine audit 2026-11-17, local/scratch/snow1117/engine-oneplayer-audit.md): player 2 read past them. Each reader is pinned to
// 0 (in single player and network co-op the value is 0 anyway; in the split screen entry 0 is each context's own, its memory swapped):
// - 0x27f9c0: the snowmobile / boat camera's speed field of view term, 0x1b761d4 + lc * 0x38 (player 2 read the vehicle definition table: no widening);
// - 0xdaf1f: the 2D placement of a client view, 0x5e0c60 + lc * 0x6c (player 2 got the placement mode int as a placement: Spec Ops' mission-failed quote,
//   the splatter, damage flash and friendly-fire cross hair drawn with garbage scales); exp11place sets entry 0 to the half being drawn;
// - 0xbcf7a, 0xbcf8d: the usercmd of the vehicle turret scope zoom (CL_GetCurrentCmdNumber / CL_GetUserCmd(lc): past the one cl block);
// - 0xaee4c, 0x9a47a, 0x8e630, 0x90700: mov ecx,[cg.localClientNum] for the dead quote, the splatter and the objective-on-entity origin (connection state).
// [native] lcpin=0 off.
static void lcnum_pins_install(void)
{
	if (!cfg("lcpin", 1)) // (again at each session start: the journal puts the bytes back at its end)
		return;
	int pinned = 0;
	static const uint32_t movs[] = {0xbcf7a, 0xbcf8d, 0xaee4c, 0x9a47a, 0x8e630, 0x90700};
	for (size_t i = 0; i < sizeof(movs) / sizeof(movs[0]); i++) {
		uint8_t *q = base + XR(movs[i]);
		if (q[0] != 0x8b || (q[1] & 0xc7) != 0x05 || *(int32_t *)(q + 2) + (intptr_t)(q + 6) != (intptr_t)(base + XR(0x40caa4))) {
			log_printf("native: lcpin: unexpected code at rva 0x%x, not pinned", movs[i]);
			continue;
		}
		const uint8_t reg = (q[1] >> 3) & 7; // mov r32, [rip+disp] -> xor r32, r32 (the same register) + nops
		const uint8_t put[6] = {0x31, (uint8_t)(0xc0 | (reg << 3) | reg), 0x90, 0x90, 0x90, 0x90};
		j_save(q, 6);
		DWORD old;
		VirtualProtect(q, 6, PAGE_EXECUTE_READWRITE, &old);
		memcpy(q, put, 6);
		VirtualProtect(q, 6, old, &old);
		FlushInstructionCache(GetCurrentProcess(), q, 6);
		pinned++;
	}
	static const struct {
		uint32_t rva;
		uint8_t len, want[4], put[4];
	} pins[] = {
		{0x27f9c0, 4, {0x48, 0x63, 0x41, 0x04}, {0x31, 0xc0, 0x90, 0x90}}, // movsxd rax,[rcx+4] -> xor eax,eax
		{0xdaf1f, 3, {0x48, 0x63, 0xc1}, {0x31, 0xc0, 0x90}},             // movsxd rax,ecx -> xor eax,eax
	};
	for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint8_t *q = base + XR(pins[i].rva);
		if (memcmp(q, pins[i].want, pins[i].len)) {
			log_printf("native: lcpin: unexpected code at rva 0x%x, not pinned", pins[i].rva);
			continue;
		}
		j_save(q, pins[i].len);
		DWORD old;
		VirtualProtect(q, pins[i].len, PAGE_EXECUTE_READWRITE, &old);
		memcpy(q, pins[i].put, pins[i].len);
		VirtualProtect(q, pins[i].len, old, &old);
		FlushInstructionCache(GetCurrentProcess(), q, pins[i].len);
		pinned++;
	}
	log_printf("native: lcpin: %d client tables read with local client 0 (each context's own entry)", pinned);
}
static void fxbody_install(void)
{
	uint8_t *c = base + XR(0x11c65f), *d = base + XR(0x119382);
	if (!cfg("fxbodyown", 1) || !cfg("fxsplit", 0))
		return;
	if (c[0] == 0xe8 && c + 5 + *(int32_t *)(c + 1) == base + XR(0x1e5de0) && d[0] == 0xe8 && d + 5 + *(int32_t *)(d + 1) == base + XR(0x1e5f40) &&
	    jhook_call(c, (void *)fxbody_create_hook, (void **)&fxbody_create_original) && jhook_call(d, (void *)fxbody_destroy_hook, (void **)&fxbody_destroy_original))
		log_printf("native: debris physics objects copied into context 1's effects are freed by context 0 only");
	else
		log_printf("native: the effects' physics calls at 0x11c65f / 0x119382 are not as expected: copied debris is freed by both contexts");
}
#endif
static void ctx_swap_mask(int to, unsigned mask)
{
	if (CFG_ONCE("swapsites", 0))
		swap_site_note((uintptr_t)__builtin_return_address(0), mask);
	ctx_ensure();
	unsigned need = 0;
	for (size_t i = 0; i < NRANGES; i++)
		if ((mask & (1u << i)) && range_owner[i] != to)
			need |= 1u << i;
	if (!need) {
		ctx_current = to;
		fov_scale_follow(to);
		return;
	}
	const int site_i = CFG_ONCE("swapsites", 0) ? swap_site_cur : -1;
	const LONGLONG tw = site_i >= 0 ? perf_now() : 0;
	ctx_wait_workers();
	int perf_on = CFG_ONCE("perflog", 0);
	LONGLONG t0 = perf_on || site_i >= 0 ? perf_now() : 0;
	if (site_i >= 0)
		swap_sites[site_i].real++, swap_sites[site_i].wait += t0 - tw;
	static int fx_on = -1;
	if (fx_on < 0) // mode 11: one effects system for both views, as the 360 has (it steps the effects of player 0 only, 0x82141e70): with a copy for player 2 the effects a level's
		       // scripts start (PlayFX: a temporary entity whose event only context 0 handles; the exploders' smoke) were in player 1's half only (owner, Gulag fly-in: "when the
		       // first player sees the smoke, the other doesn't"; checked: with the copy the smoke column in one half only, without it in both). [native] fxsplit=1 the copy again
		fx_on = cfg("fxsplit", 0);
#ifdef RANGE_FX
	// EXPERIMENT, off ([native] fxphyswait=1 on): ctx_wait_workers leaves out worker type 19, "fx physics" (the names at rva 0x370340), whose jobs may write the live
	// effects system; this waits for it too before the effects range moves. Tried 2026-11-17 against the Gulag "cafe" crashes with fxseed (0x11b0d9: a trail list of
	// context 1's system looped in FX_UpdateEffect 0x11afd0; 0xfd0d5: context 0's spot light pointed at a freed effect): both kept happening with it, so not the cause.
	if ((need & (1u << RANGE_FX)) && fx_on && CFG_ONCE("fxphyswait", 0))
		((void (*)(int))(base + RVA_WAIT_TYPE))(19);
#endif
#if defined(RANGE_SUN)
	// The sun state is written by the render thread while it draws a view's frame ([native] sunwait=0 does not wait for it; QA 2026-11-02: player 1 turning moved player
	// 2's state). In mode 11 player 2's frame is drawn after his draw has swapped back: the range now stays with him until the next swap that takes it (ctx_swap_after_p2_draw).
	if ((need & (1u << RANGE_SUN)) && CFG_ONCE("sunwait", 1) && CFG_ONCE("sunsplit", SUNSPLIT_DEFAULT)) {
		render_wait_idle();
	}
#endif
	uint8_t cent_keep[2 * CENT_SIZE]; // the players' slots, shared by the contexts (cent_share_on; with cent_all_on the whole table stays in place)
	const int cent_all = (need & 1u) && cent_all_on();
	const int cent_keep_on = (need & 1u) && cent_share_on() && !cent_all;
	if (cent_keep_on)
		memcpy(cent_keep, base + RVA_CENT, sizeof(cent_keep));
	for (size_t i = 0; i < NRANGES; i++) {
		if (!(need & (1u << i)))
			continue;
#ifdef RANGE_FX
		if (i == RANGE_FX && !fx_on)
			continue;
#endif
#ifdef RANGE_VIEW
		if (i == RANGE_VIEW && !CFG_ONCE("viewsplit", 1))
			continue;
#endif
#ifdef RANGE_SUN
		if (i == RANGE_SUN && !CFG_ONCE("sunsplit", SUNSPLIT_DEFAULT))
			continue;
#endif
		if (i == RANGE_BLUR && !CFG_ONCE("blursplit", 1))
			continue;
#ifdef RANGE_CTL
		if (i == RANGE_CTL && !CFG_ONCE("ctlsplit", 0))
			continue;
#endif
#ifdef RANGE_SHAKE
		if (i == RANGE_SHAKE && !CFG_ONCE("shakesplit", 1))
			continue;
#endif
#ifdef RANGE_RIDE
		if (i == RANGE_RIDE && !CFG_ONCE("ridesplit", 1))
			continue;
#endif
		if (i == RANGE_MSEC && (!CFG_ONCE("msecsplit", 1) || !addr_resolved(ranges[i].rva)))
			continue;
		if (i >= RANGE_LASER && i < RANGE_LASER + RANGE_LASER_N && !laser_split_on())
			continue;
		uint8_t *live = base + RANGE_RVA(i);
#if defined(_WIN64) && defined(RANGE_FX)
		if (i == RANGE_FX)
			fx_spot_check("as it leaves", range_owner[i]);
#endif
		const size_t swap_size = i == 0 && cent_all ? (size_t)(RVA_CENT - RANGE_RVA(0)) : ranges[i].size; // (cent_all_on: the entity table at the range's end stays live)
		swap_copy(ctx_store[range_owner[i]][i], live, swap_size);
		swap_copy(live, ctx_store[to][i], swap_size);
		range_owner[i] = to;
#if defined(_WIN64) && defined(RANGE_FX)
		if (i == RANGE_FX) {
			fx_spot_check("as it comes back", to);
			fxbody_report();
		}
#endif
	}
	if (cent_keep_on)
		memcpy(base + RVA_CENT, cent_keep, sizeof(cent_keep));
#ifdef _WIN64
	if (mask == CTX_ALL)
		for (int i = 0; i < 3; i++)
			if (x_store[0][i]) {
				memcpy(x_store[ctx_current][i], base + XR(xranges[i].rva), xranges[i].size);
				memcpy(base + XR(xranges[i].rva), x_store[to][i], xranges[i].size);
			}
	if (mask == CTX_ALL) {
		if (xr_n < 0)
			xr_parse();
		if (xr_n > 0) { // the render thread reads some of these while it draws the last frame: let it finish first (64: frames in flight at rva 0x1429b5c, the wait loop rva 0x301e0)
			static int (*idle)(void);
			if (!idle)
				idle = xr_render_idle;
			((void (*)(int (*)(void)))(base + XR(0x301e0)))(idle);
		}
		for (int i = 0; i < xr_n; i++) {
			memcpy(xr[i].store[ctx_current], base + xr[i].rva, xr[i].size);
			memcpy(base + xr[i].rva, xr[i].store[to], xr[i].size);
		}
	}
#endif
	if (perf_on) {
		perf_copy += perf_now() - t0;
		InterlockedIncrement(&perf_swaps);
	}
	if (site_i >= 0)
		swap_sites[site_i].copy += perf_now() - t0;
	ctx_current = to;
	fov_scale_follow(to);
}

// QA ([native] flipwatch=1, 64 bits, mode 11): global state that the two views hold at different values. Right before and right after player 2's draw the dwords of a
// block are read (the sound system's by default, rva 0x1a10000, 256 KB; [native] fliprva=<decimal rva> another); a dword counts when player 1's draw and player 2's each
// left it at its own value, the same values as in the frame before (written by both views, each its own: whoever reads it in between sees it flip every frame). The 20
// most frequent are logged every 5 s.
#ifdef _WIN64
#define FLIP_RVA 0x1a10000u
#define FLIP_N 0x10000u
static uint32_t flip_cur[2][FLIP_N], flip_prev[2][FLIP_N];
static uint16_t flip_cnt[FLIP_N];
static void flip_watch(int ending)
{
	const uint32_t *live = (const uint32_t *)(base + XR((uint32_t)CFG_ONCE("fliprva", (int)FLIP_RVA)));
	memcpy(flip_cur[ending], live, sizeof(flip_cur[0]));
	if (ending != 1)
		return;
	static unsigned frames;
	static DWORD at;
	frames++;
	for (uint32_t i = 0; i < FLIP_N; i++)
		if (flip_cur[0][i] != flip_cur[1][i] && flip_cur[0][i] == flip_prev[0][i] && flip_cur[1][i] == flip_prev[1][i] && flip_cnt[i] < 0xffff)
			flip_cnt[i]++;
	memcpy(flip_prev, flip_cur, sizeof(flip_prev));
	if (!at)
		at = GetTickCount();
	if (GetTickCount() - at < 5000)
		return;
	at = GetTickCount();
	log_printf("native: flipwatch 5 s (%u frames):", frames);
	for (int k = 0; k < 20; k++) {
		uint32_t best = 0;
		for (uint32_t i = 1; i < FLIP_N; i++)
			if (flip_cnt[i] > flip_cnt[best])
				best = i;
		if (flip_cnt[best] < frames / 4)
			break;
		float f0, f1;
		memcpy(&f0, &flip_cur[0][best], 4), memcpy(&f1, &flip_cur[1][best], 4);
		char name[64] = "";
		if (!(best & 1) && best + 1 < FLIP_N) { // a pointer in view 1 to something that starts with a name pointer (a sound alias): its name
			const uint64_t q = (uint64_t)flip_cur[0][best] | (uint64_t)flip_cur[0][best + 1] << 32;
			const char *const *obj = (const char *const *)(uintptr_t)q;
			if (q > 0x10000 && !IsBadReadPtr(obj, 8) && *obj && !IsBadReadPtr(*obj, 1))
				snprintf(name, sizeof(name), " -> \"%.40s\"", *obj);
		}
		log_printf("native:   rva 0x%x: %u frames, view 1 %08x (%g), view 2 %08x (%g)%s", (uint32_t)CFG_ONCE("fliprva", (int)FLIP_RVA) + best * 4, flip_cnt[best], flip_cur[0][best], (double)f0, flip_cur[1][best], (double)f1, name);
		flip_cnt[best] = 0;
	}
	memset(flip_cnt, 0, sizeof(flip_cnt));
	frames = 0;
}
#endif
static void ctx_swap(int to)
{
	ctx_swap_mask(to, CTX_ALL);
}
// Mode 11, after player 2's draw: everything back to context 0 but the sun state. The render thread draws player 2's frame after this and writes his sun state (blind,
// glare) into the live range; swapped back at once, it wrote into context 0's: QA sunlook (Boneyard, player 1 turns once around, player 2 stands; field 5 = view forward .
// sun direction) held player 1's state at player 2's value through the whole turn, in both builds (2026-11-19). The range stays with context 1 now, and the next swap
// that takes it to context 0 (player 2's client logic at the next frame's start) waits for the render thread first. Then player 1's went from -0.906 to 0.906 and
// player 2's stayed put. [native] sunkeep=0 swaps it back at once again.
static int sun_range_owner(void) // (QA: whose sun state the live range holds, for the sunlog lines)
{
#ifdef RANGE_SUN
	return range_owner[RANGE_SUN];
#else
	return ctx_current;
#endif
}
static void ctx_swap_after_p2_draw(void)
{
#ifdef RANGE_SUN
	if (mode == 11 && CFG_ONCE("sunkeep", 1)) {
		ctx_swap_mask(0, CTX_ALL & ~(1u << RANGE_SUN));
		return;
	}
#endif
	ctx_swap(0);
}
// For the hand-over hooks (server step, snapshot, server commands, usercmds): everything but the effects system, the renderer's view and the sun state, which they never touch.
#define CTX_HAND (CTX_ALL & ~((1u << RANGE_FX) | (1u << RANGE_VIEW) | (1u << RANGE_SUN)))
static void ctx_swap_hand(int to)
{
	if (CFG_ONCE("handswap", 1))
		ctx_swap_mask(to, CTX_HAND);
	else
		ctx_swap_mask(to, CTX_ALL);
}

// The cgame block holds what the cgame registered for the whole process (the media handles the 2D drawing uses even while the cgame is still being initialised). Context 1 starts
// from context 0's block after its init, so its own init can draw the loading screen.
static int ctx_seeded; // once for each level of a session (session_reset and the cgame init of a new level clear it): a second session in the same process starts context 1 from the new level's cgame, not from the first level's
static void ctx_seed_cgame(void)
{
	if (ctx_seeded || ctx_current != 0)
		return;
	ctx_ensure();
	// Context 1 starts as a copy of context 0 as it is now (the cgame registered dvars and media for the whole process, the snapshot pointers point into their own block), except for
	// the playerstate the server published for client 1, which is its own.
	for (size_t i = 0; i < NRANGES; i++) {
		uint8_t *live = base + RANGE_RVA(i);
		if (i == 5) { // the cl block: everything but the playerstate at +0x2018 (32 bits: +0x1018)
			memcpy(ctx_store[1][i], live, CL_BLOCK_PS);
			memcpy(ctx_store[1][i] + CL_BLOCK_PS + 0xaca4, live + CL_BLOCK_PS + 0xaca4, ranges[i].size - CL_BLOCK_PS - 0xaca4);
		} else {
			memcpy(ctx_store[1][i], live, ranges[i].size);
		}
	}
#if defined(_WIN64) && defined(RANGE_FX)
	fxbody_reset();
#endif
#ifdef _WIN64
	// EXPERIMENT, off ([native] fxseed=1 on): context 1's own effects system (fxsplit=1) starts EMPTY, made by the game's FX_InitSystem (rva 0x119a00), instead of as
	// a copy of context 0's. Why it was tried: the Gulag's start "cafe" crashes 3 s after the split began, every run (rva 0x2713ee, read of -1 taking a node from the shared
	// physics pool, 0x2713e0); with fxsplit=0 no crash in 160 s. The copy holds context 0's live effects, and with them the physics objects of their debris, which both
	// systems then free. With the empty start that crash is gone, but the game crashed later in all 7 runs (38-90 s: 0x11b0d9, a trail list of context 1's system looping
	// in FX_UpdateEffect 0x11afd0; 0xfd0d5, context 0's spot light pointing at a freed effect), and effects started before the split (the Gulag's helicopter spot light,
	// PlayFXOnTag) are missing in player 2's view. Likely why: the copied cgame state of context 1 (its entities) still points at effects of context 0's system, which the
	// empty system does not have. FX_InitSystem also resets the impact marks (globals rva 0xbc6280..0xbc72e0, pools 0xb7ac00 + 0x4b000), shared by both views: they are kept.
	if (cfg("fxsplit", 0) && cfg("fxseed", 0)) {
		const size_t fx_size = ranges[RANGE_FX].size, marks_g = 0xbc72e0 - 0xbc6280, marks_p = 0x4b000;
		uint8_t *fx_live = base + RANGE_RVA(RANGE_FX), *mg = base + XR(0xbc6280), *mp = base + XR(0xb7ac00);
		uint8_t *keep = VirtualAlloc(NULL, fx_size + marks_g + marks_p, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (keep && XR(0x119a00) && XR(0xbc6280) && XR(0xb7ac00)) {
			ctx_wait_workers();
			((void (*)(int))(base + RVA_WAIT_TYPE))(19); // fx physics jobs write the live system (see fxphyswait)
			memcpy(keep, fx_live, fx_size);
			memcpy(keep + fx_size, mg, marks_g);
			memcpy(keep + fx_size + marks_g, mp, marks_p);
			((void (*)(int))(base + XR(0x119a00)))(0);
			memcpy(ctx_store[1][RANGE_FX], fx_live, fx_size);
			memcpy(fx_live, keep, fx_size);
			memcpy(mg, keep + fx_size, marks_g);
			memcpy(mp, keep + fx_size + marks_g, marks_p);
			log_printf("native: context 1's effects system starts empty (its own FX_InitSystem), not as a copy of context 0's");
		}
		if (keep)
			VirtualFree(keep, 0, MEM_RELEASE);
	}
#endif
	ctx_seeded = 1;
}

// The client code reads the number of local players (rva 0x3c3eb0) at these instructions (rva, length, offset of the rip-relative displacement). The server and the game module keep
// reading the real one (2); the client code is pointed at a copy that stays 1, so its loops over the local clients only ever touch client 0 and nothing of the arrays of one element.
static const struct {
	uint32_t rva;
	uint8_t length, disp;
} client_n_sites[] = {
	{0x6bae9, 6, 2},
	{0x85f44, 6, 2},
	{0x89e44, 6, 2},
	{0x93d94, 6, 2},
	{0xa08c2, 7, 3},
	{0xa6703, 7, 3},
	{0xa69cf, 6, 2},
	{0xa6a76, 6, 2},
	{0xa6b29, 7, 3},
	{0xa6c44, 6, 2},
	{0xa6c8c, 6, 2},
	{0xa8f25, 6, 2},
	{0xa905d, 6, 2},
	{0xa9248, 6, 2},
	{0xa936b, 6, 2},
	{0xb2154, 6, 2},
	{0xb60ec, 7, 3},
	{0xb629a, 6, 2},
	{0xb630a, 6, 2},
	{0xb71df, 6, 2},
	{0xb8e34, 6, 2},
	{0xbcb49, 6, 2},
	{0xc2835, 6, 2},
	{0xc3e5e, 6, 2},
	{0xc3eb8, 6, 2},
	{0xc9667, 6, 2},
	{0xc96f5, 6, 2},
	{0xc9754, 6, 2},
	{0xc9a04, 6, 2},
	{0xca135, 6, 2},
	{0xca184, 6, 2},
	{0xca607, 6, 2},
	{0xd22ac, 6, 2},
	{0xd2304, 6, 2},
	{0xd5595, 6, 2}
};

#ifdef _WIN64
static void client_n_install(void)
{
	int32_t *n_client = (int32_t *)hook_page_near(base + RVA_LOCAL_PLAYERS_STORE);
	if (!n_client) {
		log_printf("native: no page near the image for the client's player count");
		return;
	}
	*n_client = 1;
	int done = 0;
	for (size_t i = 0; i < sizeof(client_n_sites) / sizeof(client_n_sites[0]); i++) {
		uint8_t *site = base + XR(client_n_sites[i].rva);
		int32_t old;
		memcpy(&old, site + client_n_sites[i].disp, 4);
		if ((uint8_t *)(site + client_n_sites[i].length + old) != base + XR(0x3c3eb0))
			continue; // not the instruction the build has
		int64_t to = (int64_t)((uint8_t *)n_client - (site + client_n_sites[i].length));
		if (to > INT32_MAX || to < INT32_MIN)
			continue;
		int32_t rel = (int32_t)to;
		DWORD prot;
		j_save(site, 8);
		VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &prot);
		memcpy(site + client_n_sites[i].disp, &rel, 4);
		VirtualProtect(site, 8, prot, &prot);
		done++;
	}
	FlushInstructionCache(GetCurrentProcess(), base, 0xe0000);
	log_printf("native: %d of %d client reads of the local player count point at a copy (1)", done, (int)(sizeof(client_n_sites) / sizeof(client_n_sites[0])));
}
#endif

// The server's entity list for the snapshot leaves out the local players' entities (they are fed through the playerstate); with two local players each context would never see the other
// one's body. The read of the player count at the start of the publisher (rva 0x23459a, `mov reg, [0x3c3eb0]`, used only for that exclusion) is pointed at a variable that is 0.
// [native] listplayers=1.
#ifdef _WIN64
static void list_players_install(void)
{
	uint8_t *site = base + XR(0x23459a);
	int32_t old;
	memcpy(&old, site + 3, 4);
	int32_t *zero = (int32_t *)hook_page_near(base + RVA_LOCAL_PLAYERS_STORE);
	if (!zero || (uint8_t *)(site + 7 + old) != base + XR(0x3c3eb0)) {
		log_printf("native: entity list patch: unexpected code");
		return;
	}
	*zero = 0;
	int32_t rel = (int32_t)((uint8_t *)zero - (site + 7));
	DWORD prot;
	j_save(site, 8);
	VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &prot);
	memcpy(site + 3, &rel, 4);
	VirtualProtect(site, 8, prot, &prot);
	FlushInstructionCache(GetCurrentProcess(), site, 8);
	log_printf("native: the entity list keeps the local players' entities");
}
#endif

static void (*ps_publish_original)(unsigned, const void *);
static void (*server_cmds_original)(uintptr_t, int *);
static void (*usercmd_step_original)(int);
static void (*snapshot_msg_original)(void *, int);
static void (*cg_init_all_original)(void);
static void (*snapshot_take_original)(void);
#define RVA_LOCAL_PLAYERS_N XR64(0x3c3eb0)

// A client-side function that loops over the local clients (and calls others that touch the client statics without a client number): run for context 0 and, once context 1
// exists, for context 1, each time with the number of local players set to 1 so that every loop sees only client 0, which in context 1 is client 1.
static int cg_init_done;
static int draws_after_init;

static void run_both(void (*fn)(void))
{
	int was = ctx_current;
	if (was)
		ctx_swap_hand(0);
	fn();
	if (ctx1_ready) {
		ctx_swap_hand(1);
		fn();
		ctx_swap_hand(0);
	}
	if (was)
		ctx_swap_hand(was);
}

// Hang watch ([native] hangwatch=0 off): after a level's cgame init, when no frame comes for 10 s, or frames stop for 6 s once they came, every other thread of the game is
// stopped for a moment and its instruction pointer and the code addresses on its stack are logged (module+offset), once per hang. The threads run again before anything is logged.
static volatile LONG hang_frames; // frames that reached the draw hooks (draw_hook)
static volatile DWORD hang_init_tick;
static void hang_dump_thread(const char *why, DWORD only) // only: one thread's id, 0: every thread but this one
{
	DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return;
	static char lines[24][1400];
	int nl = 0;
	THREADENTRY32 te;
	te.dwSize = sizeof(te);
	for (BOOL ok = Thread32First(snap, &te); ok && nl < 24; ok = Thread32Next(snap, &te)) {
		if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || (only && te.th32ThreadID != only))
			continue;
		HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
		if (!th)
			continue;
		uintptr_t addrs[40];
		int na = 0;
		char regs[160] = "";
		if (SuspendThread(th) != (DWORD)-1) {
			CONTEXT ctx;
			memset(&ctx, 0, sizeof(ctx));
			ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
			if (GetThreadContext(th, &ctx)) {
				addrs[na++] = CTX_IP(ctx);
#ifndef _WIN64
				snprintf(regs, sizeof(regs), " regs eax %lx ebx %lx ecx %lx edx %lx esi %lx edi %lx ebp %lx", ctx.Eax, ctx.Ebx, ctx.Ecx, ctx.Edx, ctx.Esi, ctx.Edi, ctx.Ebp);
#endif
#ifndef _WIN64
				// the string release loop of the iw4x exe (SL_RemoveRef..., rva 0x213d5c): the string (ebp), its hash bucket chain
				if (game_is_iw4x() && CTX_IP(ctx) >= (uintptr_t)base + 0x213d50 && CTX_IP(ctx) <= (uintptr_t)base + 0x213d63) { // raw-ok: an address of the 159 image, iw4x-sp only
					const uint8_t *str = (const uint8_t *)ctx.Ebp;
					char text[48] = "?";
					MEMORY_BASIC_INFORMATION mbi;
					if (VirtualQuery(str, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) {
						for (int i = 0; i < 40 && str[4 + i]; i++)
							text[i] = str[4 + i] >= 32 && str[4 + i] < 127 ? (char)str[4 + i] : '.', text[i + 1] = 0;
						log_printf("native: string release loop: string %p refs %u text '%s' bytes %02x%02x%02x%02x %02x%02x%02x%02x", str, *(const uint16_t *)str, text, str[0], str[1], str[2], str[3], str[4], str[5], str[6], str[7]);
					}
					const uint8_t *tbl = (const uint8_t *)base + 0x128f380; // raw-ok: an address of the 159 image, iw4x-sp only
					unsigned idx = ((unsigned)ctx.Eax - 0x168f380u) / 8;
					char chain[600] = "";
					int cn = 0;
					for (int k = 0; k < 24 && cn < (int)sizeof(chain) - 40 && idx < 0x80000; k++) { // (bounded: the buffer, and the table's 0x80000 entries; night audit A5)
						unsigned nxt = *(const uint16_t *)(tbl + idx * 8);
						unsigned val = *(const uint32_t *)(tbl + idx * 8 + 4);
						cn += snprintf(chain + cn, sizeof(chain) - cn, " %x(next %x val %x)", idx, nxt, val);
						idx = nxt;
					}
					log_printf("native: string release loop: chain from eax:%s", chain);
				}
#endif
				const uintptr_t *sp = (const uintptr_t *)CTX_SP(ctx);
				for (int i = 0; i < 600 && na < 40; i++) {
					uintptr_t v = 0;
					if (!ReadProcessMemory(GetCurrentProcess(), sp + i, &v, sizeof(v), NULL))
						break;
					MEMORY_BASIC_INFORMATION mbi;
					if (v > 0x10000 && VirtualQuery((void *)(uintptr_t)v, &mbi, sizeof(mbi)) && mbi.Type == MEM_IMAGE &&
					    (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
						addrs[na++] = v;
				}
			}
			ResumeThread(th);
		}
		CloseHandle(th);
		int n = snprintf(lines[nl], sizeof(lines[nl]), "native: HANG (%s) thread %lu:%s", why, (unsigned long)te.th32ThreadID, regs);
		for (int i = 0; i < na && n < (int)sizeof(lines[nl]) - 60; i++) {
			HMODULE m = NULL;
			char name[MAX_PATH] = "?";
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)addrs[i], &m) && m) {
				GetModuleFileNameA(m, name, sizeof(name));
				const char *slash = strrchr(name, '\\');
				n += snprintf(lines[nl] + n, sizeof(lines[nl]) - n, " %s+%llx", slash ? slash + 1 : name, (unsigned long long)(addrs[i] - (uintptr_t)m));
			}
		}
		nl++;
	}
	CloseHandle(snap);
	for (int i = 0; i < nl; i++)
		log_printf("%s", lines[i]);
}
static void hang_dump(const char *why)
{
	log_printf("native: GSC position of the script VM at the hang (%s):", why);
	scrdebug_stack_log();
	hang_dump_thread(why, 0);
}
// Stall watch (QA, [native] stallwatch=<ms>, off by default): when the thread of the client frames has not started a frame for that long, its stack is logged in the middle of
// the stall (and once more at three times as long): where the engine spends a hitch. At most 80 per session.
static volatile DWORD stall_tid;
static volatile LONGLONG stall_frame_t;
static int stall_watch_active;
static DWORD WINAPI stall_watch_thread(LPVOID p)
{
	const LONGLONG limit = (LONGLONG)(intptr_t)p;
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	LONGLONG seen = 0;
	int stage = 0, dumps = 0;
	for (;;) {
		Sleep(4);
		LONGLONG t = stall_frame_t;
		if (!t || !stall_tid || dumps >= 80)
			continue;
		if (t != seen)
			seen = t, stage = 0;
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		LONGLONG since = (now.QuadPart - t) * 1000 / f.QuadPart;
		if ((stage == 0 && since > limit) || (stage == 1 && since > 3 * limit)) {
			char why[48];
			snprintf(why, sizeof(why), "stall %lld ms", since);
			hang_dump_thread(why, stall_tid);
			stage++, dumps++;
		}
	}
	return 0;
}
static DWORD WINAPI hang_watch_thread(LPVOID unused)
{
	(void)unused;
	LONG last = -1;
	DWORD since = GetTickCount();
	int dumped = 0;
	for (;;) {
		Sleep(1000);
		DWORD now = GetTickCount();
		LONG f = hang_frames;
		// the frames are counted by the split screen's draw (ctx_after_draw) only: with no split session the count stands still, and the first level
		// after a session logged a false hang and walked every thread (audit 2026-11-18, native 7)
		if (!native_active) {
			last = -1, since = now, dumped = 0;
			continue;
		}
		if (f != last) {
			last = f, since = now, dumped = 0;
			continue;
		}
		if (dumped || !hang_init_tick)
			continue;
		if (f == 0 && now - hang_init_tick > 10000) // (hang_frames is zeroed at each cgame init)
			dumped = 1, hang_dump("no frame 10 s after the cgame init");
		else if (f > 0 && now - since > 6000)
			dumped = 1, hang_dump("no frame for 6 s");
	}
	return 0;
}

static void cg_init_all_hook(void)
{
	log_printf("native: cgame init (context 0; context 1 follows after the first frames), dvar ptr 0x52bfc0 = %p", *(void **)(base + XR64(0x52bfc0)));
	{
		static int started;
		if (!started && cfg("hangwatch", 1)) {
			started = 1;
			CreateThread(NULL, 0, hang_watch_thread, NULL, 0, NULL);
		}
		static int stall_started;
		int stall_ms = cfg("stallwatch", 0);
		if (!stall_started && stall_ms > 0) {
			stall_started = 1;
			if (CreateThread(NULL, 0, stall_watch_thread, (LPVOID)(intptr_t)stall_ms, 0, NULL))
				stall_watch_active = 1;
		}
		hang_frames = 0;
		hang_init_tick = GetTickCount();
	}
	cg_init_all_original();
	cg_init_done = 1;
	if (native_active) { // a new level: context 1 starts again from this level's cgame
		ctx1_ready = 0;
		draws_after_init = 0;
		// ... and from a fresh copy of it: context 1's block still held the previous level's cgame (pointers into memory freed with that level) because the seeding is made once per
		// session. A level change inside one session (the mission's end, the next mission) then ran context 1 on stale lists: access violations a second or two after the split view
		// came up, in the render thread (rva 0x11b6f6) and the main thread (0x1bdef0), in 1 of 2 v0.6.0 runs of QA scenario hop (Trainer to Roadkill).
		ctx_seeded = 0;
	}
	log_printf("native: cgame init done, dvar ptr 0x52bfc0 = %p", *(void **)(base + XR64(0x52bfc0)));
}

static void snapshot_take_hook(void)
{
	hook_counts[0]++;
	run_both(snapshot_take_original);
}

// The cgame registers its dvars and media while it runs the first frames, and the 2D drawing of every thread reads them: context 1 can only start from a cgame block that has them.
// So its init runs a few frames after context 0's, from its first frame's hook, from a copy of context 0's block.
static void ctx_dump(const char *what)
{
#ifdef _WIN64
	static const uint32_t probes[] = {0x422434, 0x40caa0, 0x40cad0, 0x40cad8, 0x40cab8, 0x422440};
#else
	static const uint32_t probes[] = {0x374c20, 0x35f2a0, 0x35f2c8, 0x35f2bc, 0x35f2b8, 0x374c2c}; // cg.time, the cgame's client numbers and pointers, cg.ps
#endif
	uint8_t *r1_live = base + RANGE_RVA(0);
	uint8_t *r1 = ctx_current == 0 && ctx_store[1][0] ? ctx_store[1][0] : r1_live;
	char line[400];
	int n = snprintf(line, sizeof(line), "native: %s ctx1:", what);
	for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++)
		n += snprintf(line + n, sizeof(line) - n, " %x=%llx", XR(probes[i]), *(unsigned long long *)(r1 + XR(probes[i]) - RANGE_RVA(0)));
	n += snprintf(line + n, sizeof(line) - n, " | ctx0:");
	for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++)
		n += snprintf(line + n, sizeof(line) - n, " %x=%llx", XR(probes[i]), *(unsigned long long *)(r1_live + XR(probes[i]) - RANGE_RVA(0)));
	log_printf("%s", line);
}

static void ctx_jitter_log(void)
{
	static unsigned seen;
	if (ctx_current != 1 || !CFG_ONCE("jitterlog", 0))
		return;
	seen++;
	if (seen < 1500 || seen >= 1506)
		return;
	char line[1200];
	int n = snprintf(line, sizeof(line), "native: ctx1 ps frame %u:", seen);
	const float *f = (const float *)(base + XR64(0x422440));
	for (int i = 0; i < 70; i++) {
		float v = f[i];
		if (v > -100000.0f && v < 100000.0f && (v > 0.0001f || v < -0.0001f))
			n += snprintf(line + n, sizeof(line) - n, " %x=%.1f", i * 4, v);
	}
	log_printf("%s", line);
}

// Diagnostics ([native] entlog=1): the entities of the live cgame near the camera (cg_entities at rva RVA_CENT, CENT_SIZE bytes each: eType +0xe0, origin +0x18, clientNum +0x160)
static void ctx_entity_log(void)
{
	static unsigned seen[2];
	if (!CFG_ONCE("entlog", 0))
		return;
	if (++seen[ctx_current] != 600)
		return;
	{
		const float *f = (const float *)(base + XR64(0x422440));
		char line[1500];
		int n = snprintf(line, sizeof(line), "native: ctx %d cg ps floats:", ctx_current);
		for (int i = 0; i < 100; i++)
			if (f[i] > -400.0f && f[i] < 400.0f && (f[i] > 0.01f || f[i] < -0.01f))
				n += snprintf(line + n, sizeof(line) - n, " %x=%.1f", i * 4, f[i]);
		log_printf("%s", line);
	}
	{
		const float *f = (const float *)(base + XR64(0x43c200));
		char line[1500];
		int n = snprintf(line, sizeof(line), "native: ctx %d refdef-ish floats @43c200:", ctx_current);
		for (int i = 0; i < 90; i++)
			if (f[i] > -1000000.0f && f[i] < 1000000.0f && (f[i] > 0.001f || f[i] < -0.001f))
				n += snprintf(line + n, sizeof(line) - n, " %x=%.2f", i * 4, f[i]);
		log_printf("%s", line);
	}
	const float *cam = (const float *)(base + XR64(0x42245c));
	log_printf("native: ctx %d camera %.0f %.0f %.0f, clientNum %d", ctx_current, cam[0], cam[1], cam[2], *(int *)(base + XR64(0x40caa0)));
	for (int i = 0; i < 1024; i++) {
		const uint8_t *e = base + RVA_CENT + (size_t)i * CENT_SIZE;
		const float *o = (const float *)(e + 0x18);
		float dx = o[0] - cam[0], dy = o[1] - cam[1], dz = o[2] - cam[2];
		if (dx * dx + dy * dy + dz * dz < 200.0f * 200.0f && e[CENT_TYPE] != 0)
			log_printf("native:   ent %d type %d flags %08x clientNum %d at %.0f %.0f %.0f", i, e[CENT_TYPE], *(unsigned *)(e + CENT_FLAGS), *(unsigned short *)(e + 0x160), o[0], o[1], o[2]);
	}
}

// Diagnostics ([native] npclog=1, jitter): the entity (index >= 2) that travelled furthest in the last 300 frames of context 0 (whoever is walking now) is followed in both contexts: its pose origin at the
// start of each draw, with wall time, cg.time and the interpolation numbers. [native] npcwait=<seconds> starts the measuring that long after the first draw (the airport squad walks after about 25 s).
static int npc_probe_ctx(void);
static int npc_pick = -1, npc_goff = -1; // the tracked entity and where its origin lies in the server entity (npc_log)
#define pick npc_pick
#define goff npc_goff
static void npc_log(void)
{
	static float last[2][1024][3];
	static float moved[1024];
	static unsigned frames[2], logged[2];
	static double first_wall = -1;
	unsigned c = (unsigned)ctx_current & 1;
	LARGE_INTEGER pc, hz;
	QueryPerformanceCounter(&pc);
	QueryPerformanceFrequency(&hz);
	double wall_now = (double)pc.QuadPart / (double)hz.QuadPart;
	if (first_wall < 0)
		first_wall = wall_now;
	if (wall_now - first_wall < (double)CFG_ONCE("npcwait", 0))
		return;
	frames[c]++;
	if (CFG_ONCE("npcsnap", 0) && frames[c] % 50 == 1 && frames[c] < 60) { // which dwords near cg.time point into the client's snapshot ring (rva 0x51b078..0x51dcb0): cg.snap, cg.nextSnap
		char line[1800];
		int n = snprintf(line, sizeof(line), "native: npcsnap ctx %d:", ctx_current);
		for (int off = 0x404450; off < 0x44f000; off += 8) {
			uintptr_t v = *(const uintptr_t *)(base + off);
			if (n >= (int)sizeof(line) - 160)
				break; // (QA only: the line is full; past its end the size wrapped and the stack was overwritten, audit 2026-11-18)
			if (v >= (uintptr_t)base + XR64(0x51b078) && v < (uintptr_t)base + XR64(0x51dcb0) + 0x100)
			{
				n += snprintf(line + n, sizeof(line) - n, " [%x]=snap+%x:", off, (unsigned)(v - (uintptr_t)base - 0x51b078));
				for (int k = 0; k < 12; k++)
					n += snprintf(line + n, sizeof(line) - n, " %d", *(const int *)(v + k * 4));
			}
		}
		log_printf("%s", line);
	}
	for (int i = 2; i < 1024; i++) {
		const uint8_t *e = base + RVA_CENT + (size_t)i * CENT_SIZE;
		const float *o = (const float *)(e + 0x18);
		if (!e[CENT_TYPE] || !(*(const uint32_t *)(e + CENT_VALID) & 1))
			continue;
		float step = fabsf(o[0] - last[0][i][0]) + fabsf(o[1] - last[0][i][1]);
		if (c == 0 && step < 30.0f)
			moved[i] += step;
		last[c][i][0] = o[0];
		last[c][i][1] = o[1];
		last[c][i][2] = o[2];
	}
	if (c == 0 && frames[0] % 300 == 0) {
		float best = (float)CFG_ONCE("npcmin", 25); // [native] npcmin: units an entity must have travelled in 300 frames to be tracked
		int np = -1;
		int want_type = CFG_ONCE("npctype", 0); // [native] npctype=13: only AI actors (the furthest traveller is often a scriptmover otherwise)
		for (int i = 2; i < 1024; i++) {
			if (moved[i] > best && (!want_type || base[RVA_CENT + (size_t)i * CENT_SIZE + CENT_TYPE] == want_type) && (!CFG_ONCE("npcmax", 0) || i <= CFG_ONCE("npcmax", 0))) // [native] npcmax=134: only the entities each context keeps for itself
				best = moved[i], np = i;
			moved[i] = 0;
		}
		if (np >= 0 && np != pick && !(CFG_ONCE("npcfix", 0) && pick >= 0)) { // npcfix=1: keep the first entity found
			pick = np;
			log_printf("native: npclog tracks entity %d (it travelled %.0f units in the last 300 frames)", pick, (double)best);
		}
	}
	if (CFG_ONCE("npcent", -1) >= 0 && pick != CFG_ONCE("npcent", -1)) { // [native] npcent=<n>: track entity n (1: player 2's body as player 1's half draws it, 0: player 1's in player 2's half)
		pick = CFG_ONCE("npcent", -1);
		log_printf("native: npclog tracks entity %d (npcent)", pick);
	}
	static int goff_pick = -1; // the server entity's origin (gentity r.currentOrigin), found by matching the snapshot's origin of the tracked entity
	float gx = 0, gy = 0;
	if (pick >= 0) {
		const uint8_t *g = base + RVA_GENTITIES + (size_t)pick * GENT_SIZE;
		if (goff_pick != pick) {
			const float *n3 = (const float *)(base + RVA_CENT + (size_t)pick * CENT_SIZE + 0xfc);
			goff = -1;
			for (int o = 0; o + 12 <= GENT_SIZE && goff < 0; o += 4) {
				const float *f = (const float *)(g + o);
				if (fabsf(f[0] - n3[0]) < 6.0f && fabsf(f[1] - n3[1]) < 6.0f && fabsf(n3[0]) > 1.0f)
					goff = o;
			}
			goff_pick = pick;
			log_printf("native: npclog entity %d: server origin found at gentity+0x%x", pick, goff);
			if (goff >= 0 && CFG_ONCE("npcwatch", 0) == 1) { // who writes that origin: a write watchpoint on its x, in every thread (control.c)
				control_watch_write((uintptr_t)(g + goff - base), 4);
				log_printf("native: npcwatch: watching writes to entity %d's server origin x", pick);
			}
			if (CFG_ONCE("npcwatch", 0) == 5 && goff >= 0) { // all three: the client's current state, its next state and the server origin of the tracked actor, each hit tagged with the live context
				const uintptr_t rv[3] = {RVA_CENT + (uintptr_t)pick * CENT_SIZE + 0x80, RVA_CENT + (uintptr_t)pick * CENT_SIZE + 0xfc, (uintptr_t)(g + goff - base)};
				control_set_probe(npc_probe_ctx);
				control_watch_multi(rv, 3);
				log_printf("native: npcwatch: watching entity %d: slot 0 client current x, slot 1 client next x, slot 2 server origin x", pick);
			}
			if (CFG_ONCE("npcwatch", 0) == 4) { // the client's NEXT state (cent + 0xfc, its origin x): who sets it from the snapshot
				control_watch_write(RVA_CENT + (uintptr_t)pick * CENT_SIZE + 0xfc, 4);
				log_printf("native: npcwatch: watching writes to entity %d's client next-state origin x", pick);
			}
			if (CFG_ONCE("npcwatch", 0) == 3) { // the entity's drawn pose (cent + 0x18): who computes it, and from which context
				control_watch_write(RVA_CENT + (uintptr_t)pick * CENT_SIZE + 0x18, 4);
				log_printf("native: npcwatch: watching writes to entity %d's pose origin x", pick);
			}
			if (CFG_ONCE("npcwatch", 0) == 2) { // the client's CURRENT state of the tracked entity (cent + 0x80, its origin x): who transitions it
				control_watch_write(RVA_CENT + (uintptr_t)pick * CENT_SIZE + 0x80 + 0x0, 4);
				log_printf("native: npcwatch: watching writes to entity %d's client current-state origin x", pick);
			}
		}
		if (goff >= 0) {
			gx = *(const float *)(g + goff);
			gy = *(const float *)(g + goff + 4);
		}
	}
	if (CFG_ONCE("npcptr", 0) >= 2 && c == 0) { // the head of the next snapshot structure, to find numEntities
		static int last_t = -1, dumps;
		const uintptr_t ns = *(const uintptr_t *)(base + XR64(0x40cac8));
		const int tm = ns ? *(const int *)(ns + 4) : -1;
		if (tm != last_t && dumps < 12) {
			last_t = tm;
			dumps++;
			const int *w = (const int *)ns;
			log_printf("native: snaphead t %d: %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d", tm, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15], w[16],
				   w[17], w[18], w[19]);
		}
	}
	if (pick >= 0 && logged[c] < 9000 && CFG_ONCE("npcptr", 0)) { // the snapshot pointers and the fraction the pose interpolation (rva 0x9da90) reads: 0x51b078 / 0x51b080 -> snapshot, +4 = serverTime; 0x51dcb0
		const uintptr_t pa = *(const uintptr_t *)(base + XR64(0x51b078)), pb = *(const uintptr_t *)(base + XR64(0x51b080));
		log_printf("native: npcptr ctx %d ent %d poseA %d poseB %d frac %.3f | cg.snap %d cg.next %d | cg.time %d", ctx_current, pick, pa ? *(const int *)(pa + 4) : -1, pb ? *(const int *)(pb + 4) : -1,
			   (double)*(const float *)(base + XR64(0x51dcb0)), *(const int *)(*(const uintptr_t *)(base + XR64(0x40cac0)) + 4), *(const int *)(*(const uintptr_t *)(base + XR64(0x40cac8)) + 4), *(const int *)(base + XR64(0x422434)));
	}
#ifndef _WIN64
	if (pick >= 0 && logged[c] < 9000) { // 32 bits: the drawn pose and the cgame's time only (the snapshot fields of the 64-bit line are not mapped here), in the same shape
		const float *o = (const float *)(base + RVA_CENT + (size_t)pick * CENT_SIZE + 0x18);
		logged[c]++;
		const float *cam = (const float *)(base + RVA_CG_PS_ORIGIN);
		log_printf("native: npc ctx %d ent %d wall %.2f cg.time %d interp 0 snap 0 frac 0 pos %.2f %.2f next %.2f %.2f cam %.2f %.2f", ctx_current, pick, wall_now * 1000.0, *(const int *)(base + RVA_CG_TIME),
			   (double)o[0], (double)o[1], (double)o[0], (double)o[1], (double)cam[0], (double)cam[1]);
	}
	return;
#endif
	if (pick >= 0 && logged[c] < 9000) {
		const uint8_t *e = base + RVA_CENT + (size_t)pick * CENT_SIZE;
		const float *o = (const float *)(e + 0x18), *n2 = (const float *)(e + 0xfc), *c2 = (const float *)(e + 0x80);
		logged[c]++;
		log_printf("native: npc ctx %d ent %d wall %.2f cg.time %d interp %.3f snap %d frac %.3f pos %.2f %.2f next %.2f %.2f cur %.2f %.2f type %u flags %08x s0 %d s1 %d srv %.2f %.2f", ctx_current, pick, wall_now * 1000.0, *(int *)(base + XR64(0x422434)),
			   (double)*(float *)(base + XR64(0x42242c)), *(int *)(base + XR64(0x51dca8)), (double)*(float *)(base + XR64(0x51dcb0)), (double)o[0], (double)o[1], (double)n2[0], (double)n2[1], (double)c2[0], (double)c2[1], (unsigned)e[CENT_TYPE], *(const unsigned *)(e + CENT_FLAGS),
			   *(const int *)(*(const uintptr_t *)(base + XR64(0x40cac0)) + 4), *(const int *)(*(const uintptr_t *)(base + XR64(0x40cac8)) + 4), (double)gx, (double)gy);
		const float *cam = (const float *)(base + RVA_CG_PS_ORIGIN), *rd = (const float *)(base + XR64(0x42d310));
		log_printf("native: npccam ctx %d cg.time %d cam %.2f %.2f refdef %.2f %.2f", ctx_current, *(const int *)(base + RVA_CG_TIME), (double)cam[0], (double)cam[1], (double)rd[0], (double)rd[1]);
	}
}

// Diagnostics ([native] timelog=1): the cgame's time (rva 0x422434) at the end of each context's draw, a few dozen frames in a row
static void time_log(void)
{
	static int armed = -1, n;
	static char line[900];
	static int len;
	if (armed < 0)
		armed = cfg("timelog", 0);
	if (!armed || !split_on || !ctx1_ready)
		return;
	if (++n < 3000 || n >= 3040)
		return;
	len += snprintf(line + len, sizeof(line) - len, " %d:%d", ctx_current, *(int *)(base + XR64(0x422434)));
	if (n == 3039)
		log_printf("native: cg.time per drawn frame (context:time):%s", line);
}

static void ctx_after_draw(void)
{
	static int texts_done;
	InterlockedIncrement(&hang_frames);
	time_log();
	if (!texts_done) { // both players are on this machine: the co-op "waiting for the other player" texts have nothing to wait for (an asset lookup: not while the level is loading)
		texts_done = 1;
		localize_override("COOP_WAITINGFORPLAYER", " ");
		localize_override("SPECIAL_OPS_WAITING_OTHER_PLAYER", " ");
	}
	ctx_entity_log();
	ctx_jitter_log();
	static unsigned count;
	if (ctx1_ready && ++count % 240 == 0 && count < 1200)
		ctx_dump("state");
	static int logged;
	if (!logged++)
		log_printf("native: first draw after the hooks: cg_init_done %d ready %d", cg_init_done, ctx1_ready);
	if (ctx1_ready || !cg_init_done || ++draws_after_init < 5 + CFG_ONCE("ctx1delay", 0)) // (QA: ctx1delay=<draws> starts context 1 that much later)
		return;
	log_printf("native: starting context 1");
	ctx_seed_cgame();
#ifdef _WIN64
	memset(ctx_store[1][0] + (0x440730 - ranges[0].rva), 0, 0x40); // (an offset inside range 0) // context 1 makes its own view model (see viewmodel_setup_hook)
#else
	memset(ctx_store[1][9], 0, ranges[9].size); // (32 bits: the view model pointers are a range of their own)
#endif
	if (cfg("p1cmd", 0)) {
		// player 1's view: the view angle accumulators (pitch 0x533d14, yaw 0x533d18, degrees) of context 1's block; the server turns them into his view with the
		// angles it set at his spawn, so this is relative to that (player 1 stood at yaw -77 with 0): [native] p1yaw=<degrees>
		float *acc = (float *)(ctx_store[1][4] + ACC_IN_CLS);
		acc[0] = 0;
		acc[1] = (float)cfg("p1yawdeg", 60);
	}
	if (cfg("p1cmd", 0)) {
		// and player 0's: turned once by [native] p0turn degrees (default 241: from the spawn yaw -78 to the direction of player 1, 163) through his own view angle accumulator
		float *acc0 = (float *)(base + RVA_ACC_PITCH);
		acc0[1] += (float)cfg("p0turn", 241);
	}
	ctx_swap(1);
	// not the whole cgame init (it also resets what is shared: effects, media, the renderer's per-level state): only its two per-client parts, on the copied block
	((void (*)(int))(base + RVA_CLIENT_INIT_A))(0);
	((void (*)(int))(base + RVA_CLIENT_INIT_B))(0);
	ctx_swap(0);
	// the cgame's own idea of its local client number (rva 0x40caa4): the draw hides "my" entity by it and answers per-client visibility masks by it; context 1 runs as local client 0
	// but is client 1
	*(int *)(ctx_store[1][0] + ((RVA_CG_CLIENTNUM + 4) - RANGE_RVA(0))) = cfg("ctx1lc", 1);
	ctx1_ready = 1;
	ctx1_clc_synced = 0;
	if (p2_cmd_hold && ctx_store[1][5]) { // the commands context 1 made before this level's init are not his: none of them goes to the server
		*(int *)(ctx_store[1][5] + CL_SENT) = *(const int *)(ctx_store[1][5] + CL_CMDNUM);
		p2_cmd_hold = 0;
		log_printf("native: player 2's commands go to the server again (this level's context 1)");
	}
	log_printf("native: context 1 initialised");
	ctx_dump("after init");
}

// The server's copy of a local player's playerstate into the cl block (rva RVA_CL_PS is +0x2018 into the block at 0x533d20): for client 1 straight into context 1's copy, so no other thread
// ever sees a swapped block for it.
#define PS_SIZE 0xaca4
// Issue 22 (v1.0.0: after a restart from a checkpoint player 2 could not be controlled for as long as the failed run had lasted). The engine restores player 1 from the checkpoint's saved
// game, but player 2's server playerstate is not part of it: its commandTime (the first dword; the server ignores a usercmd that is not newer than it) kept the time of the failed run
// (28.7 s in the test), so every command of the new level, which starts again near 0, was ignored until the new clock passed that time (the QA scenario failwalk: player 2 moved after 28 s).
// A command time more than 2 s ahead of the last command the server took from client 1 can only be from the old run (the two are within a frame of each other while playing): it is set
// to just before that command. [native] p2cmdtime=0 turns it off.
static void p2_stale_command_time(void *ps)
{
	if (!CFG_ONCE("p2cmdtime", 1))
		return;
	int *command_time = (int *)ps;
	const int last = *(const int *)(base + RVA_SV_CLIENTS + SV_CLIENT_STRIDE + 0x1630);
	if (last > 0 && *command_time > last + 2000) {
		static unsigned fixed;
		if (fixed++ < 8)
			log_printf("native: player 2's server command time %d is ahead of his last command %d (a restart from a checkpoint): set back", *command_time, last);
		*command_time = last - 1;
	}
}

static void ps_publish_hook(unsigned lc, const void *ps)
{
	hook_counts[1]++;
	if (lc == 1 && native_active)
		p2_stale_command_time((void *)ps);
	if (lc < 2 && CFG_ONCE("vehwatch", -1) >= 0 && split_tick && GetTickCount() - split_tick > (DWORD)CFG_ONCE("vehwatch", -1) * 1000) {
		// QA ([native] vehwatch=<seconds after the split began>): write watchpoints on both players' server view angle offsets (delta_angles yaw, ps + 0x64): which code
		// turns a rider's view with his vehicle (the snowmobile races: both players' cameras turned alike, not with their own sleds, 2026-11-17)
		static const void *seen[2];
		static int set;
		seen[lc] = ps;
		if (!set && seen[0] && seen[1]) {
			set = 1;
			if (CFG_ONCE("vehwatchcl", 0)) { // the client side instead ([native] vehwatchcl=1): the cgame's predicted view yaw and the client's yaw accumulator (both swapped per context)
				const uintptr_t rc[2] = {XR64(0x42254c), XR64(0x533d18)};
				control_watch_multi(rc, 2);
				log_printf("native: vehwatch: the client view yaw (0x42254c) and yaw accumulator (0x533d18) are watched");
			} else {
				const uintptr_t rv[2] = {(uintptr_t)seen[0] + 0x64 - (uintptr_t)base, (uintptr_t)seen[1] + 0x64 - (uintptr_t)base};
				control_watch_multi(rv, 2);
				log_printf("native: vehwatch: client 0 ps %p, client 1 ps %p (delta yaw rvas 0x%llx 0x%llx)", seen[0], seen[1], (unsigned long long)rv[0], (unsigned long long)rv[1]);
			}
		}
	}
	if (lc == 0 && ctx_current == 0)
		return ps_publish_original(0, ps);
	ctx_ensure();
	// the playerstate of client lc belongs to context lc: into the live block when that context is the live one, else into its stored copy
	uint8_t *dest = (int)lc == ctx_current ? base + RVA_CL_PS : ctx_store[lc][5] + CL_BLOCK_PS;
	memcpy(dest, ps, PS_SIZE);
}

static void server_cmds_hook(uintptr_t lc, int *msg)
{
	hook_counts[5]++;
	if ((int)lc == 1 && p2cmds_on()) { // player 2's commands into context 1's client connection (its cgame takes them in its own frame, filtered by p2cmd_allowed)
		int was1 = ctx_current;
		if (was1 != 1)
			ctx_swap_hand(1);
		server_cmds_original(0, msg);
		if (was1 != 1)
			ctx_swap_hand(was1);
		return;
	}
	if ((int)lc != 0)
		return; // (p2cmds off: client 1 gets no server commands)
	int was = ctx_current;
	if (was)
		ctx_swap_hand(0);
	server_cmds_original(0, msg);
	if (was)
		ctx_swap_hand(was);
}

// Context 1's cgame runs only the server commands that belong to a player. The rest (music, sound mixes, slow motion, objectives, prints, menus, saves, achievements,
// client dvars: on the PC these are one state for the whole game) context 0 already runs; "s" and "checksound" the engine itself keeps for local client 0, which context 1
// pretends to be. CG_ServerCommand is rva 0xb4890 (the command in the client's tokenizer), called at 0xb5ed7.
static int p2cmd_allowed(const char *c)
{
	static const char *const ok[] = {"visionset_naked", "visionset_night", "visionset_missilecam", "visionset_thermal", "visionset_pain", "scr_blur", "clear_blur", "scr_fade",
					 "watersheeting", "ls", "ls_stop", "offhand", "ret_lock_on", "showViewModel", "hideViewModel", "hud_drawHUD",
					 "sw"}; // sw: the server switched his weapon (SwitchToWeapon, a loadout, a weapon picked up)
	for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
		if (!strcmp(c, ok[i]))
			return 1;
	return 0;
}
// the addresses (rva; 32 bits: VA - 0x400000, the official build, cdecl calls)
#ifdef _WIN64
#define P2C_SCMD_CALL 0xb5ed7  // call of CG_ServerCommand
#define P2C_SCMD_FN 0xb4890
#define P2C_EXEC_CALL 0xb8a7d  // call of CG_ExecuteNewServerCommands
#define P2C_EXEC_FN 0xb5ea0
#define P2C_CGS_DONE 0x40cab0  // cgs: the number of the last command run
#define P2C_CLC_NEWEST 0x547850 // clc: the newest command received
#define P2C_CLC_ACK 0x547854   // clc: the last one handed out (the server's acknowledged number after a delivery)
#else
#define P2C_SCMD_CALL 0xfb44a
#define P2C_SCMD_FN 0x16d060
#define P2C_EXEC_CALL 0x16e90e
#define P2C_EXEC_FN 0xfb420
#define P2C_CGS_DONE 0x35f2b0
#define P2C_CLC_NEWEST 0x48e340
#define P2C_CLC_ACK 0x48e344
#endif
static void (*cg_scmd_original)(int);
static void cg_scmd_hook(int lc)
{
	if (ctx_current == 1 && p2cmds_on()) {
		const char *c = game_cmd_argv(0);
		const int run = c && p2cmd_allowed(c);
		static unsigned n;
		if (CFG_ONCE("p2cmdlog", 0) && n++ < 300)
			log_printf("native: p2cmds: player 2's server command '%.60s' %s", c ? c : "", run ? "run" : "left to player 1's context");
		if (!run)
			return;
	}
	cg_scmd_original(lc);
}
// The cgame runs the server commands up to the number the snapshot names (rva 0xb5ea0(lc, latest), called at 0xb8a7d with cgs+4, rva 0x40cab4). Both contexts parse the
// same snapshot, client 0's, so context 1 asked its own connection for client 0's command numbers and ran nothing. In context 1 the latest is its own connection's newest
// command (clc +0x547850), and its counter of executed commands (0x40cab0, copied from context 0 when context 1 was made) is kept inside the last delivery's window
// (clc's acknowledged number +0x547854 .. the newest): older numbers point into a buffer the next delivery overwrote.
static void (*cg_exec_original)(int, int);
static void cg_exec_hook(int lc, int latest)
{
	if (ctx_current == 1 && p2cmds_on()) {
		p2_sw_flush(); // (a weapon switch from before this context took commands: to the server's queue, run in a later frame)
		int *done = (int *)(base + XR(P2C_CGS_DONE));
		const int newest = *(int *)(base + XR(P2C_CLC_NEWEST)), ack = *(int *)(base + XR(P2C_CLC_ACK));
		if (*done < ack || *done > newest)
			*done = ack;
		latest = newest;
	}
	cg_exec_original(lc, latest);
}
static void p2cmd_filter_install(void)
{
	if (!CFG_ONCE("p2cmds", 1))
		return;
	if (!XR(P2C_SCMD_CALL) || !XR(P2C_EXEC_CALL) || !XR(P2C_CGS_DONE) || !XR(P2C_CLC_NEWEST)) {
		log_printf("native: p2cmds: the addresses are not known in this build: player 2 gets no server commands");
		return;
	}
	uint8_t *site = base + XR(P2C_SCMD_CALL);
	if (site[0] != 0xe8 || site + 5 + *(int32_t *)(site + 1) != base + XR(P2C_SCMD_FN) || !jhook_call(site, (void *)cg_scmd_hook, (void **)&cg_scmd_original)) {
		log_printf("native: p2cmds: the server command call at rva 0x%x is not as expected: player 2 gets no server commands", P2C_SCMD_CALL);
		return;
	}
	uint8_t *exec_site = base + XR(P2C_EXEC_CALL);
	if (exec_site[0] != 0xe8 || exec_site + 5 + *(int32_t *)(exec_site + 1) != base + XR(P2C_EXEC_FN) || !jhook_call(exec_site, (void *)cg_exec_hook, (void **)&cg_exec_original)) {
		log_printf("native: p2cmds: the command run call at rva 0x%x is not as expected: player 2 gets no server commands", P2C_EXEC_CALL);
		return;
	}
	p2cmd_filter_ok = 1;
	log_printf("native: p2cmds: player 2's own server commands (vision sets, blur, local sounds ...) reach his view");
}

#ifdef _WIN64
// The naked vision set for player 2 (Vlad, nightly 2026-11-08: Oil Rig, player 2's picture stayed underwater-blue while player 1's changed). Server commands reach context 0 only
// (server_cmds_hook drops client 1's), and VisionSetNaked, a script function for everybody, is sent as the command "visionset_naked <name> <time>" that the cgame handles at rva 0xb4890
// (the call of the apply function 0xbfd90 at 0xb592e). The script builtin is wrapped to note the names of the calls made for everybody; when the cgame of context 0 then applies such
// a name, the same call runs again in context 1 (its own cgame block: its own time and vision state). VisionSetNakedForPlayer (the Predator's black flashes of the pilot) is no call
// of the wrapped builtin, so it stays with the player it was made for. [native] visionfwd=0 turns it off.
#define RVA_VISION_NAKED_CALL XR(0xb592e)
#define RVA_FN_VISIONSETNAKED_ENTRY XR(0x3c2c28)
#define RVA_CG_VISION_NAKED_NAME XR(0x43c0ac)
static uint64_t (*vision_apply_original)(uintptr_t, int, int, const char *, int);
static void (*vision_global_original)(void);
static struct {
	char name[64];
	int used;
} vision_global[16];
static unsigned vision_global_w;
static int vision_forwarding;

static void vision_global_wrap(void)
{
	const char *n = ((const char *(*)(unsigned))(base + RVA_SCR_GET_STRING))(0);
	if (n && n[0]) {
		unsigned i = vision_global_w++ & 15;
		snprintf(vision_global[i].name, sizeof(vision_global[i].name), "%s", n);
		vision_global[i].used = 1;
	}
	vision_global_original();
}

static uint64_t vision_apply_hook(uintptr_t lc, int type, int state, const char *name, int time)
{
	uint64_t r = vision_apply_original(lc, type, state, name, time);
	if (ctx_on && ctx1_ready && ctx_current == 0 && !vision_forwarding && name && name[0] && !p2cmds_on()) { // (with p2cmds context 1 gets the command itself)
		for (int i = 0; i < 16; i++) {
			if (!vision_global[i].used || strcmp(vision_global[i].name, name) != 0)
				continue;
			vision_global[i].used = 0;
			char tmp[64];
			snprintf(tmp, sizeof(tmp), "%s", name);
			vision_forwarding = 1;
			ctx_swap_hand(1);
			char *live = (char *)(base + RVA_CG_VISION_NAKED_NAME);
			snprintf(live, 64, "%s", tmp);
			vision_apply_original(lc, type, state, live, time);
			ctx_swap_hand(0);
			vision_forwarding = 0;
			static int told;
			if (told++ < 30)
				log_printf("native: vision: '%s' (%d s) for everybody also applied to player 2's screen", tmp, time);
			break;
		}
	}
	return r;
}


// QA ([native] cmdlog=1, 64-bit): every client command the engine hands to the server for local client 0 (rva 0xd02a0, its ClientCommand call at 0xd0306), with the
// live context: which player's "n <action>" texts reach which client
#ifdef _WIN64
static void (*cmd_deliver_original)(int, const char *);
static void cmd_deliver_hook(int client, const char *text)
{
	static unsigned n;
	if (n++ < 200)
		log_printf("native: cmdlog: engine delivers '%.40s' for client %d (context %d live, frame bottom %d)", text, client, ctx_current, frame_bottom);
	cmd_deliver_original(client, text);
}
static void cmd_deliver_install(void)
{
	if (!CFG_ONCE("cmdlog", 0) || *(base + XR64(0xd0306)) != 0xe8)
		return;
	if (jhook_call(base + XR64(0xd0306), (void *)cmd_deliver_hook, (void **)&cmd_deliver_original))
		log_printf("native: cmdlog on");
}
#endif
// IsSplitScreen() (script builtin, entry rva 0x3c3048, function 0x194720 = "xor ecx,ecx; jmp 0x222140": always false on the PC). The Xbox 360's returns its splitscreen
// dvar (0x82224558), so the stock scripts' split screen branches run there: the HUD of Special Ops (positions, the AC-130's font, timers), the blood overlay, the revive
// HUD, is_coop_online() false, the hint arrows. In a split session it now says so (night audit, engine fix 5); outside one (single player, online co-op) it is false as
// before. The mod's own "|| GetDvarInt( "mw2coop_native_split" )" hunks stay. [native] issplit=0 off.
static void issplitscreen_builtin(void)
{
	((void (*)(int))(base + XR(0x222140)))(native_active ? 1 : 0);
}
static void issplitscreen_install(void)
{
	if (!cfg("issplit", 1))
		return;
	uint8_t *entry = game_builtin_entry(XR(0x3c3048), "issplitscreen");
	if (!entry) {
		log_printf("native: no issplitscreen builtin entry: IsSplitScreen() stays false in the split screen");
		return;
	}
	void (**func)(void) = (void (**)(void))(entry + sizeof(char *));
	if (*func == issplitscreen_builtin)
		return;
	DWORD old;
	VirtualProtect(func, sizeof(*func), PAGE_READWRITE, &old);
	*func = issplitscreen_builtin;
	VirtualProtect(func, sizeof(*func), old, &old);
	log_printf("native: IsSplitScreen() is true in the split screen session (as the console's)");
}
static void vision_forward_install(void)
{
	issplitscreen_install();
	if (!cfg("visionfwd", 1))
		return;
	uint8_t *entry = game_builtin_entry(RVA_FN_VISIONSETNAKED_ENTRY, "visionsetnaked");
	if (!entry) {
		log_printf("native: no visionsetnaked entry at rva 0x%x, player 2's vision does not follow", RVA_FN_VISIONSETNAKED_ENTRY);
		return;
	}
	if (!jhook_call(base + RVA_VISION_NAKED_CALL, vision_apply_hook, (void **)&vision_apply_original)) {
		log_printf("native: no call at the vision set apply, player 2's vision does not follow");
		return;
	}
	void (**func)(void) = (void (**)(void))(entry + sizeof(char *));
	vision_global_original = *func;
	DWORD old;
	VirtualProtect(func, sizeof(*func), PAGE_READWRITE, &old);
	*func = vision_global_wrap;
	VirtualProtect(func, sizeof(*func), old, &old);
	log_printf("native: player 2's screen follows the vision sets the scripts set for everybody");
}
#else
static void vision_forward_install(void) {}
#endif

// The server's snapshot message (entities, as shared lists) is parsed into the client's snapshot ring: once for each context, the message read again from its start.
static void snapshot_msg_hook(void *msg, int time)
{
	hook_counts[4]++;
	if (CFG_ONCE("snaplog", 0)) { // diagnostics (jitter): when the snapshot messages come: the server time they carry, the message's size fields, the live context
		static unsigned sn;
		LARGE_INTEGER pc, hz;
		QueryPerformanceCounter(&pc);
		QueryPerformanceFrequency(&hz);
		if (++sn > (unsigned)CFG_ONCE("snaplog", 0) && sn < (unsigned)CFG_ONCE("snaplog", 0) + 400)
			log_printf("native: snapmsg #%u wall %.1f time %d ctx %d msg ints %d %d %d %d %d %d cg.time %d", sn, (double)pc.QuadPart * 1000.0 / (double)hz.QuadPart, time, ctx_current, ((int *)msg)[0], ((int *)msg)[1], ((int *)msg)[2],
				   ((int *)msg)[3], ((int *)msg)[4], ((int *)msg)[5], *(int *)(base + XR64(0x422434)));
	}
	int was = ctx_current;
	if (was)
		ctx_swap_hand(0);
	uint8_t copy[MSG_T_SIZE]; // the engine's msg_t: 0x30 bytes in 64 bits, 0x28 in 32
	memcpy(copy, msg, sizeof(copy));
	snapshot_msg_original(msg, time);
	if (ctx1_ready) {
		memcpy(msg, copy, sizeof(copy));
		ctx_swap_hand(1);
		snapshot_msg_original(msg, time);
		ctx_swap_hand(0);
	}
	if (was)
		ctx_swap_hand(was);
}

// ---- the players' devices: [native] p0pad (player 1, the top half) and p1pad (player 2): an XInput pad 0-3, or -1 for the keyboard and mouse. A player's pad is read in his
// own context's client frame, with his context live. Stick left: movement (analog, into the usercmd by make_cmd_hook), stick right: the view (the view angle accumulators of the
// live block, rva 0x533d14 pitch / 0x533d18 yaw, degrees), the buttons run the game's own actions for the live kbuttons (rva 0xd1810 name -> index in the action table at rva
// 0x3be770, 0xce450 runs it for the local client with a key number no keyboard sends). The layout is the console's: A jump, B stance (hold: prone, cl_stanceHoldTime),
// X reload (hold: use), Y next weapon, LB special grenade, RB frag, LT aim, RT fire, LS sprint / hold breath, RS melee, d-pad the action slots, Back objectives, Start pause.
// In a menu (key catchers 0x10) player 1's pad sends the keys a menu knows: d-pad / left stick arrows, A Enter, B / Start Escape; player 2's pad does nothing there.
typedef struct {
	WORD buttons;
	BYTE lt, rt;
	SHORT lx, ly, rx, ry;
} xin_gamepad_t;
typedef struct {
	DWORD packet;
	xin_gamepad_t pad;
} xin_state_t;
static DWORD(WINAPI *xinput_get)(DWORD, xin_state_t *);
static int pad_dev[2] = {-1, -1}; // per player: an XInput pad 0-3, or -1 (the keyboard and mouse)
static float pad_zs_hip[2];       // per player: the game's look sensitivity for his unzoomed view (the pad's turn by the zoom; logged)
#define p1pad pad_dev[1]
typedef struct {
	int forward, right; // -127..127, for the usercmd of this frame
	int active;         // the pad is connected
	WORD held;
	int rt, lt;
	LONGLONG last_count; // the performance counter at the last frame (the turn is per second)
	WORD menu_held;
	DWORD menu_repeat;   // when a held direction in a menu sends its key again
	int rc_pitch, rc_yaw; // -127..127: the right stick for a remote-controlled missile (usercmd +0x3c / +0x3d, see make_cmd_hook)
} pad_player_t;
static pad_player_t pads[2];
// the player whose context gets the keyboard and the mouse: player 2 only when player 1 plays on a pad and player 2 on none
static int kb_player(void)
{
	return pad_dev[0] >= 0 && pad_dev[1] < 0 ? 1 : 0;
}
int native_kb_player(void)
{
	return kb_player();
}

// an action of the table at rva 0x3be770 by its name ("+attack" / "-attack", "weapnext"), for local client 0 of the live context
#ifndef _WIN64
// 32 bits (docs/32bit-native-map/inputui.md): there is no action table; the buttons are console commands (+attack, -attack, weapnext ...). The command text goes into the command buffer
// of local client 0 and the buffer is executed at once (Cbuf_Execute, EBX = local client), so it acts on the live context's input block: the player's own, as in the 64-bit build.
static void cbuf_run_now(void)
{
	void *fn = base + XR(0x2001c0);
	__asm__ volatile("call *%0" : : "m"(fn), "b"(0) : "eax", "ecx", "edx", "esi", "edi", "memory", "cc");
}
static void pad_action(const char *name)
{
	((void (*)(int, const char *))(base + XR(0x9b40)))(0, name); // Cbuf_AddText
	cbuf_run_now();
}
#else
static void pad_action(const char *name)
{
	int index = ((int (*)(const char *))(base + XR(0xd1810)))(name);
	if (!index) {
		static char logged[16][24];
		static int count;
		for (int i = 0; i < count; i++)
			if (!strcmp(logged[i], name))
				return;
		if (count < 16)
			snprintf(logged[count++], sizeof(logged[0]), "%s", name);
		log_printf("native: pad: no action '%s'", name);
		return;
	}
	const int seq0 = *(int *)(base + XR64(0x545844));
	((void (*)(int, int, int))(base + XR(0xce450)))(0, index, 0x1ff);
	if (CFG_ONCE("cmdlog", 0)) { // QA: did the action queue its "n <index>" in the live context's reliable command ring (rva 0xd21a0)?
		static unsigned n;
		if (n++ < 200)
			log_printf("native: cmdlog: pad action '%s' index %d, context %d live, ring %d -> %d", name, index, ctx_current, seq0, *(int *)(base + XR64(0x545844)));
	}
}
#endif
static void pad_binding(const char *name, int down)
{
	char full[64];
#ifndef _WIN64
	// 32 bits: run it as a key binding runs it, "+name <arguments> <key> <time>". The client's notify for a script's NotifyOnPlayerCommand (VA 0x4a2950, called for every
	// command) sends the command's index with its arguments EXCEPT THE LAST TWO (a binding's key number and time): the pad's bare "+actionslot 4" reached the server as
	// "+actionslot", so a level waiting for "+actionslot 4" never heard it (Exodus: the laser designator did not come on from the d-pad, 32-bit only; QA laserpad), while
	// "+attack" (no arguments) worked. The key is 0x1ff, one no keyboard sends (as the 64-bit action call), the time the one the pad's key events use.
	snprintf(full, sizeof(full), "%c%s %d %u", down ? '+' : '-', name, 0x1ff, (unsigned)GetTickCount());
#else
	snprintf(full, sizeof(full), "%c%s", down ? '+' : '-', name);
#endif
	pad_action(full);
}

// the engine's key event (rva 0xd0f80: local client, key, down, time), for a menu: K_ENTER 13, K_ESCAPE 27, K_UPARROW 154 .. K_RIGHTARROW 157 (the key name table at rva 0x3bdb70)
#ifdef _WIN64
static void pad_key(int key, int down)
{
	((void (*)(int, int, int, unsigned))(base + XR(0xd0f80)))(0, key, down, (unsigned)GetTickCount());
}
#else
static void (*key_event_original)(int, int, int, unsigned);
static void pad_key(int key, int down)
{
	if (!key_event_original)
		return; // (its hook is not in on this build: the install only logged it, audit 2026-11-18, native 6)
	key_event_original(0, key, down, (unsigned)GetTickCount());
	cbuf_run_now(); // (the key's binding went into the command buffer as text)
}
#endif

// a stick with a radial dead zone: the length past the dead zone is rescaled to 0..1 and raised to the response curve's exponent
static void pad_stick(float *x, float *y, float dead, float curve)
{
	float m = sqrtf(*x * *x + *y * *y);
	if (m <= dead) {
		*x = *y = 0;
		return;
	}
	float n = (m > 1.0f ? 1.0f : m);
	n = powf((n - dead) / (1.0f - dead), curve);
	*x = *x / m * n;
	*y = *y / m * n;
}

static int qa_noassist[2];
static int pad_read(int pl, xin_state_t *st)
{
	if (!xinput_get) {
		xinput_get = (DWORD(WINAPI *)(DWORD, xin_state_t *))pads_xinput_proc("XInputGetState"); // (XInput and PlayStation pads, pads.c)
		if (!xinput_get) {
			static int logged;
			if (!logged++)
				log_printf("native: no XInput, no pads");
			return 0;
		}
	}
	memset(st, 0, sizeof(*st));
	int have = xinput_get((DWORD)pad_dev[pl], st) == 0;
	{ // QA: mw2coop\fakepad.txt "lx ly rx ry buttons lt rt" (stick values -32767..32767, buttons as the XInput mask) stands in for player 2's pad, fakepad0.txt for player 1's
		static DWORD checked[2];
		static int fake_have[2];
		static int f[2][7];
		DWORD now = GetTickCount();
		if (now - checked[pl] > 250) {
			checked[pl] = now;
			char path[MAX_PATH];
			lstrcpyA(path, settings_path);
			char *slash = strrchr(path, '\\');
			if (slash) {
				lstrcpyA(slash + 1, pl ? "fakepad.txt" : "fakepad0.txt");
				FILE *fp = fopen(path, "r");
				fake_have[pl] = 0;
				if (fp) {
					fake_have[pl] = fscanf(fp, "%d %d %d %d %d %d %d", &f[pl][0], &f[pl][1], &f[pl][2], &f[pl][3], &f[pl][4], &f[pl][5], &f[pl][6]) == 7;
					fclose(fp);
				}
			}
		}
		if (fake_have[pl]) {
			st->pad.lx = (SHORT)f[pl][0], st->pad.ly = (SHORT)f[pl][1], st->pad.rx = (SHORT)f[pl][2], st->pad.ry = (SHORT)f[pl][3];
			st->pad.buttons = (WORD)f[pl][4], st->pad.lt = (BYTE)f[pl][5], st->pad.rt = (BYTE)f[pl][6];
			have = 1;
		}
		// QA: the dvar mw2coop_qa_pad1 / mw2coop_qa_pad2, same format, stands in for the pad too: a level script presses at the moment it wants. An eighth
		// value 1 turns the aim assist off meanwhile (a script cannot write the option's dvar: the engine refuses script writes to code dvars)
		char name[24], v[96];
		int g[8] = {0};
		snprintf(name, sizeof(name), "mw2coop_qa_pad%d", pl + 1);
		qa_noassist[pl] = 0;
		if (game_dvar_raw(name, v, sizeof(v)) && sscanf(v, "%d %d %d %d %d %d %d %d", &g[0], &g[1], &g[2], &g[3], &g[4], &g[5], &g[6], &g[7]) >= 7) {
			st->pad.lx = (SHORT)g[0], st->pad.ly = (SHORT)g[1], st->pad.rx = (SHORT)g[2], st->pad.ry = (SHORT)g[3];
			st->pad.buttons = (WORD)g[4], st->pad.lt = (BYTE)g[5], st->pad.rt = (BYTE)g[6];
			qa_noassist[pl] = g[7] != 0;
			have = 1;
		}
	}
	return have;
}

// a menu has the keys (key catchers of local client 0, rva 0x533cb0, bit 0x10): the pad of either player moves through it (a held direction repeats after 400 ms, then every 120 ms)
static void pad_menu(int pl, const xin_state_t *st)
{
	pad_player_t *p = &pads[pl];
	WORD b = st->pad.buttons;
	float lx = st->pad.lx / 32767.0f, ly = st->pad.ly / 32767.0f;
	if (ly > 0.6f)
		b |= 0x0001;
	if (ly < -0.6f)
		b |= 0x0002;
	if (lx < -0.6f)
		b |= 0x0004;
	if (lx > 0.6f)
		b |= 0x0008;
	static const struct {
		WORD mask;
		int key;
	} keys[] = {{0x0001, 154}, {0x0002, 155}, {0x0004, 156}, {0x0008, 157}, {0x1000, 13}, {0x2000, 27}, {0x0010, 27}};
	DWORD now = GetTickCount();
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		int was = (p->menu_held & keys[i].mask) != 0, is = (b & keys[i].mask) != 0;
		int send = is && !was;
		if (is && was && i < 4 && (int)(now - p->menu_repeat) >= 0)
			send = 1;
		if (!send)
			continue;
		static int logged;
		if (logged++ < 300) // (a few lines per menu visit: which player's pad worked it, for reports)
			log_printf("native: player %d's pad works the menu: key %d", pl + 1, keys[i].key);
		pad_key(keys[i].key, 1);
		pad_key(keys[i].key, 0);
		if (i < 4)
			p->menu_repeat = now + (was ? 120 : 400);
	}
	p->menu_held = b;
}

// The client's DObj of a body (the table of rva 0x1fece0) is built from the server's (rva 0x1fee30: the one the scripts' Attach / DetachAll change) when the entity
// goes through a snapshot transition (rva 0xb8000, through 0xb86f0). A player's own entity is in no snapshot of his own, so after the scripts re-attach a changed weapon
// the body of player 1 that context 1 draws kept the weapon it first had (QA scenario armed). The transition's own update, run by hand for the two players' entities
// whenever their client DObj and the server's do not carry the same models: 0xb86f0(server entity info 0x234520(n), 0x2968e0(server DObj)) ([native] bodysync=0 off).
static const uint8_t *dobj_model_names(const uint8_t *dobj, const char *out[8], int *count)
{
	*count = dobj ? dobj[DOBJ_NUMMODELS] : 0;
	const uint8_t *const *models = dobj ? *(const uint8_t *const **)(dobj + DOBJ_MODELS) : NULL;
	for (int i = 0; i < 8; i++)
		out[i] = NULL;
	for (int i = 0; models && i < *count && i < 8; i++)
		out[i] = models[i] && !IsBadReadPtr(models[i], 8) ? *(const char *const *)models[i] : NULL;
	return dobj;
}
#ifdef _WIN64
#define DOBJ_MAKE_DIRECT(info, sinfo) ((int)((int (*)(void *, void *))(base + XR(0xb86f0)))(info, sinfo))
#else
static unsigned dobj_make_original(void *info, void *dobj_info); // (below) the 32-bit engine's make takes EDI and EBX
#define DOBJ_MAKE_DIRECT(info, sinfo) ((int)dobj_make_original(info, sinfo))
#endif
// On a build whose addresses moved, a function that was not found is a `ret` block: called, it hands back whatever was in the return register, and
// the callers below read through it (audit 2026-11-18, native 5). They run only when their functions were found.
static int dobj_fns_ok(void)
{
	static int ok = -1;
	if (ok < 0)
#ifdef _WIN64
		ok = addr_resolved(0x1fece0) && addr_resolved(0x1fee30);
#else
		ok = addr_resolved(0xf3c70) && addr_resolved(0x95740);
#endif
	return ok;
}

static int weapon_def_ok(void)
{
#ifdef _WIN64
	static int ok = -1;
	if (ok < 0)
		ok = addr_resolved(0x85850);
	return ok;
#else
	return 1; // (a table read: an address that was not found reads the zero block)
#endif
}

static void body_dobj_sync(void)
{
	static DWORD at;
	if (!native_active || ctx_current != 0 || !CFG_ONCE("bodysync", 1) || GetTickCount() - at < 200 || !dobj_fns_ok())
		return;
	at = GetTickCount();
	const uint8_t *(*client_dobj)(int) = (const uint8_t *(*)(int))(base + RVA_CLIENT_DOBJ);
	const uint8_t *(*server_dobj)(int) = (const uint8_t *(*)(int))(base + RVA_SERVER_DOBJ);
	for (int n = 0; n < 2; n++) {
		const uint8_t *cd = client_dobj(n), *sd = server_dobj(n);
		if (!cd || !sd)
			continue;
		const char *cn[8], *sn[8];
		int cc, sc;
		dobj_model_names(cd, cn, &cc);
		dobj_model_names(sd, sn, &sc);
		int same = cc == sc;
		for (int i = 0; same && i < cc && i < 8; i++)
			same = cn[i] && sn[i] && !strcmp(cn[i], sn[i]);
		if (same)
			continue;
		void *info = ((void *(*)(int))(base + RVA_ENT_INFO))(n);
		void *sinfo = ((void *(*)(const uint8_t *))(base + RVA_SDOBJ_INFO))(sd);
		int r = DOBJ_MAKE_DIRECT(info, sinfo);
		if (r && CFG_ONCE("bodysyncfree", 1)) { // what the transition does with a 1: the client DObj is dropped and the entity marked for the cleanup that follows
			((void (*)(unsigned))(base + RVA_DOBJ_CLRBIT))((unsigned)n);
			((uint32_t *)(base + RVA_ENT_FLAGS))[n >> 5] |= 0x80000000u >> (n & 31);
		}
		if (CFG_ONCE("bodysynclog", 0))
			log_printf("native: body DObj sync entity %d: client %d models (last %s), server %d models (last %s) -> %d", n, cc, cc && cn[cc - 1] ? cn[cc - 1] : "-", sc,
				   sc && sn[sc - 1] ? sn[sc - 1] : "-", r);
	}
}

// The snapshot transition (rva 0xb8000) queues a transfer of the server DObj's animation state into the client DObj of each entity (rva 0x29a120(server DObj info, client
// DObj, time), a queue of 0x28 bytes records processed later in the frame). Both contexts process every snapshot and the client DObjs are shared, so every entity's animation
// was queued once per context: [native] xfercount=1 logs the calls per second and context, [native] xferskip=1 drops context 1's queueing for every entity but player 1
// (his client DObj only goes through context 1's snapshots).
static void (*xfer_original)(void *, void *, unsigned);
static const uint8_t xfer_bytes[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xec, 0x30};
static void xfer_hook(void *src, void *dest, unsigned t)
{
	static LONG calls[2], skipped;
	static double sum[2];
	static DWORD at;
	if ((ctx_current == 0 || ctx_current == 1) && native_active) {
		InterlockedIncrement(&calls[ctx_current]);
		float tf;
		memcpy(&tf, &t, sizeof(tf));
		sum[ctx_current] += tf;
		if (CFG_ONCE("xfercount", 0) && GetTickCount() - at >= 1000) {
			at = GetTickCount();
			log_printf("native: DObj transfers in the last second: ctx0 %ld (time arg total %.3f) ctx1 %ld (%.3f) (skipped %ld)", calls[0], sum[0], calls[1], sum[1], skipped);
			calls[0] = calls[1] = 0;
			sum[0] = sum[1] = 0;
		}
		if (ctx_current == 1 && (CFG_ONCE("xferskip", 0) >= 2 || (CFG_ONCE("xferskip", 0) && dest != (void *)((const uint8_t *(*)(int))(base + XR64(0x1fece0)))(0)))) { // (xferskip=2: none at all from context 1)
			skipped++;
			return;
		}
		if (CFG_ONCE("xferlog01", 0) && split_tick && GetTickCount() - split_tick > (DWORD)CFG_ONCE("animlogat", 30) * 1000) { // QA: the transfers into the players' bodies: who queues them, with which time
			static int lines;
			for (int k = 0; k < 2 && lines < 400; k++)
				if (dest == (void *)((const uint8_t *(*)(int))(base + XR64(0x1fece0)))(k)) {
					lines++;
					log_printf("native: xfer01 ent %d ctx %d time %.4f (%08x) cg.time %d cl.serverTime %d snap %d next %d", k, ctx_current, (double)tf, t, *(const int *)(base + XR64(0x422434)),
						   *(const int *)(base + XR64(0x51dca8)), *(const int *)(*(const uintptr_t *)(base + XR64(0x40cac0)) + 4), *(const int *)(*(const uintptr_t *)(base + XR64(0x40cac8)) + 4));
				}
		}
	}
	xfer_original(src, dest, t);
}

// The feet of an actor are adjusted to the ground by rva 0x89780 (state per entity in the cgame block at rva 0x440770, 0x48 bytes each, so each context has its own
// smoothed state) applied to the shared client DObj: both contexts adjust the same feet each frame with their own state. [native] ikskip=1: context 1 leaves them alone.
static void (*ik_original)(void *, void *);
static void ik_hook(void *ent, void *dobj)
{
	if (ctx_current == 1 && native_active && CFG_ONCE("ikskip", 0))
		return;
	ik_original(ent, dobj);
}

// The snapshot transition (rva 0xb8000, 0xb8300) makes an entity's client DObj with rva 0xb86f0(server entity info, server DObj info); for an actor it takes one of 112 pre-made
// DObjs from a pool (rva 0x12c6d0: in-use flags at rva 0xf50ec0, the DObjs at 0xf50b40) and gives none when all are in use: such an actor is not drawn at all (Price on Contingency,
// a different one in each run). Both contexts process every snapshot and the client DObjs are shared, so every actor made one twice, and the one the table lost stayed in use
// for ever. [native] dobjonce=1 (default): context 1 makes none for an entity but player 1 (his own entity is in no snapshot of context 0).
#ifdef _WIN64
static unsigned (*dobj_make_original)(void *, void *);
#else
static void *dobj_make_target;
static unsigned dobj_make_original(void *info, void *dobj_info)
{
	unsigned r;
	__asm__ volatile("call *%1" : "=a"(r) : "r"(dobj_make_target), "D"(info), "b"(dobj_info) : "ecx", "edx", "memory", "cc");
	return r;
}
#endif
static const uint8_t dobj_make_bytes[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xfa, 0x48, 0x8b, 0xf1, 0x48, 0x85, 0xd2};
static unsigned dobj_make_hook(void *info, void *dobj_info)
{
	if (native_active && ctx_current == 1 && info && CFG_ONCE("dobjonce", 1) && *(const uint16_t *)((const uint8_t *)info + 0x7e) != 0)
		return 0;
	int ent = info ? *(const uint16_t *)((const uint8_t *)info + 0x7e) : -1;
	const uint8_t *(*client_dobj)(int) = (const uint8_t *(*)(int))(base + RVA_CLIENT_DOBJ);
	const uint8_t *before = ent >= 0 && ent < 2048 && dobj_fns_ok() ? client_dobj(ent) : NULL;
	unsigned r = dobj_make_original(info, dobj_info);
	if (CFG_ONCE("dobjmakelog", 0) && ent >= 64 && dobj_fns_ok()) {
		static int logged;
		const uint8_t *after = client_dobj(ent);
		if (logged < 400 || ent == 1399) {
			logged++;
			log_printf("native: client DObj make: entity %d ctx %d type %d -> %u, client DObj %p -> %p, server DObj info %p", ent, ctx_current, *(const uint8_t *)info, r, (const void *)before,
				   (const void *)after, dobj_info);
		}
	}
	return r;
}

// ---- aim assist for a pad player, the console game's: this PC build left its code out but keeps its data. The cgame fills a list of the targets on the screen
// each frame (aaGlob[0], rva 0x3cf180, by rva 0x6a570 from the view at 0xbd2c5; one per context: it is in the swapped ranges), and the mouse move (0xcfcf0) computes
// the regions of the aim_* dvars into it (0x6a350, before its auto melee 0x685c0). On that list, like the console: the turn slows down over a target (aim_slowdown_*),
// the view stays on a target while the player moves (aim_lockon_*), and aiming down the sight turns onto the nearest target in the auto aim region (aim_autoaim_*).
// The weapon's ranges: BG_GetWeaponDef (rva 0x85850) +0x42c aim assist range, +0x430 the same aiming down the sight, +0x428 auto aim range.
#define AA_GLOB RVA_AIM_GLOB
typedef struct {
	int ent;
	float mins[2], maxs[2]; // its box on the screen, -1..1 around the cross hair
	float pos[3];           // where to aim (a tag of it)
	float vel[3];
	float dist2;  // squared distance from the eye
	float xhair2; // squared distance from the cross hair on the screen
} aa_target_t;        // 0x34 bytes, from +0x13c, the count at +0xe3c
typedef struct {
	int lock_ent, snap_ent, ads_was;
	float snap_t;
	float lock_yaw, lock_pitch; // the direction to the lock-on target at the last frame
} aa_state_t;
static aa_state_t aa_state[2] = {{-1, -1, 0, 0, 0, 0}, {-1, -1, 0, 0, 0, 0}};

#ifndef _WIN64
// the 38 aim_* dvar pointers (rva of the globals holding them, in the order of the 64-bit table 0x3cf050 + 8 * n; docs/32bit-native-map/aimrumble.md 1.1): 32-bit places them all over
const uint32_t aim_dvar_rvas32[38] = {0x33ba84, 0x33ce70, 0x33c934, 0x33ceb4, 0x33c920, 0x33ce74, 0x33c930, 0x33ba8c, 0x33ce88, 0x33ba88, 0x33ce6c, 0x33ceac, 0x33c93c,
				      0x33ce9c, 0x33ce60, 0x33ce68, 0x33ce94, 0x33ceb0, 0x33ce8c, 0x33ce90, 0x33ce7c, 0x33ce98, 0x33c914, 0x33cea8, 0x33cea0, 0x33c92c,
				      0x33c910, 0x33c938, 0x33c928, 0x33ce78, 0x33c918, 0x33ce84, 0x33ba80, 0x33ce64, 0x33c924, 0x33ce80, 0x33c91c, 0x33cea4};
#endif
static float aa_dvar(uint32_t rva) // the aim_* dvar pointers: rva 0x3cf050 .. 0x3cf178 (registered by 0x69a60); 32 bits: the same index in aim_dvar_rvas32
{
#ifndef _WIN64
	rva = XR(aim_dvar_rvas32[(rva - 0x3cf050) / 8]);
#else
	rva = XR(0x3cf050) + (rva - 0x3cf050); // the dvar pointers are one array: its start through the resolver, the index by the reference layout
#endif
	const dvar_t *d = *(const dvar_t **)(base + rva);
	return !d ? 0.0f : d->type == DVAR_TYPE_BOOL ? (float)d->current.enabled : d->current.value;
}

static void aa_angles(const float eye[3], const float pos[3], float *pitch, float *yaw)
{
	float d[3] = {pos[0] - eye[0], pos[1] - eye[1], pos[2] - eye[2]};
	*yaw = atan2f(d[1], d[0]) * 57.2957795f;
	*pitch = -atan2f(d[2], sqrtf(d[0] * d[0] + d[1] * d[1])) * 57.2957795f; // down is positive
}

static float aa_delta(float to, float from)
{
	return fmodf(to - from + 540.0f, 360.0f) - 180.0f;
}

// the target nearest the cross hair whose screen box meets the region (half width / height around the cross hair, the screen being -1..1) within range
static const aa_target_t *aa_pick(const uint8_t *aa, float hw, float hh, float range)
{
	int n = *(const int *)(aa + 0xe3c);
	const aa_target_t *t = (const aa_target_t *)(aa + 0x13c), *best = NULL;
	for (int i = 0; i < n && i < 64; i++) {
		if (t[i].dist2 > range * range || hw < t[i].mins[0] || t[i].maxs[0] < -hw || hh < t[i].mins[1] || t[i].maxs[1] < -hh)
			continue;
		if (!best || t[i].xhair2 < best->xhair2)
			best = &t[i];
	}
	return best;
}

static const aa_target_t *aa_find(const uint8_t *aa, int ent)
{
	int n = *(const int *)(aa + 0xe3c);
	const aa_target_t *t = (const aa_target_t *)(aa + 0x13c);
	for (int i = 0; i < n && i < 64; i++)
		if (t[i].ent == ent)
			return &t[i];
	return NULL;
}

// yaw, pitch: this frame's turn from the stick, in the accumulators' senses (yaw left, pitch down positive); move, look: the sticks' deflections (0..1); ads: aiming
static void aim_assist(int pl, float dt, float *yaw, float *pitch, float move, float look, int ads)
{
	aa_state_t *s = &aa_state[pl];
	const uint8_t *aa = base + AA_GLOB;
	int ads_was = s->ads_was;
	s->ads_was = ads;
	int weapon = *(const int *)(aa + 0x24);
	const uint8_t *wd = aa[0xb0] && weapon > 0 && weapon_def_ok() ? WEAPON_DEF(weapon) : NULL;
	if (!wd) {
		s->lock_ent = s->snap_ent = -1;
		return;
	}
	const float ads_lerp = *(const float *)(aa + 0x128);
	const float range = (*(const float *)(wd + WD_AIM_RANGE) * (1.0f - ads_lerp) + *(const float *)(wd + WD_AIM_RANGE_ADS) * ads_lerp) * aa_dvar(0x3cf050);
	float auto_range = *(const float *)(wd + WD_AUTO_RANGE) * aa_dvar(0x3cf058);
	if (auto_range <= 0) // this PC build's weapons have no auto aim range: their aim assist range then
		auto_range = range;
	const float *eye = (const float *)(aa + 0xd8), *view = (const float *)(aa + 0xf0);
	const float *region = (const float *)(aa + 0xb8); // half width and height: slowdown, auto aim, auto melee, lock-on
	const float stick_yaw = *yaw, stick_pitch = *pitch;
	const aa_target_t *slow = aa_dvar(0x3cf0b0) != 0 ? aa_pick(aa, region[0], region[1], range) : NULL;
	if (slow) { // the turn slows down over a target
		*yaw *= aa_dvar(0x3cf0e0) * (1.0f - ads_lerp) + aa_dvar(0x3cf0e8) * ads_lerp;
		*pitch *= aa_dvar(0x3cf0d0) * (1.0f - ads_lerp) + aa_dvar(0x3cf0d8) * ads_lerp;
	}
	// lock-on: while a stick is pushed, the view follows (aim_lockon_strength of) the angle by which the direction to the target turned since the last frame, as the
	// player or the target moved; a jump (a teleport) is not followed
	const float deflection = aa_dvar(0x3cf158);
	const aa_target_t *lock = aa_dvar(0x3cf148) != 0 && (move > deflection || look > deflection) ? aa_pick(aa, region[6], region[7], range) : NULL;
	float lock_yaw = 0, lock_pitch = 0;
	if (lock) {
		float p, y, k = aa_dvar(0x3cf160);
		aa_angles(eye, lock->pos, &p, &y);
		if (lock->ent == s->lock_ent) {
			lock_yaw = aa_delta(y, s->lock_yaw) * k, lock_pitch = aa_delta(p, s->lock_pitch) * k;
			if (fabsf(lock_yaw) > 5.0f || fabsf(lock_pitch) > 5.0f)
				lock_yaw = lock_pitch = 0;
			*yaw += lock_yaw, *pitch += lock_pitch;
		}
		s->lock_yaw = y, s->lock_pitch = p;
	}
	s->lock_ent = lock ? lock->ent : -1;
	// aiming down the sight turns onto the target nearest the cross hair in the auto aim region, at aim_autoaim_lerp degrees a second, for half a second at most
	if (ads && !ads_was && aa_dvar(0x3cf0f0) != 0) {
		const aa_target_t *t = aa_pick(aa, region[2], region[3], auto_range);
		s->snap_ent = t ? t->ent : -1;
		s->snap_t = 0;
	}
	float snap_yaw = 0, snap_pitch = 0;
	const aa_target_t *snap = ads && s->snap_ent >= 0 ? aa_find(aa, s->snap_ent) : NULL;
	if (snap && s->snap_t < 0.5f) {
		float tp, ty, step = aa_dvar(0x3cf100) * dt;
		aa_angles(eye, snap->pos, &tp, &ty);
		snap_yaw = aa_delta(ty, view[1] + *yaw), snap_pitch = aa_delta(tp, view[0] + *pitch);
		snap_yaw = snap_yaw > step ? step : snap_yaw < -step ? -step : snap_yaw;
		snap_pitch = snap_pitch > step ? step : snap_pitch < -step ? -step : snap_pitch;
		*yaw += snap_yaw, *pitch += snap_pitch;
		s->snap_t += dt;
	} else
		s->snap_ent = -1;
	if (CFG_ONCE("aalog", 0)) {
		static int tuned;
		if (!tuned++)
			log_printf("native: aim dvars: slowdown %g yaw %g/%g pitch %g/%g, lock-on %g strength %g deflection %g, auto aim %g lerp %g, range scales %g %g",
				   aa_dvar(0x3cf0b0), aa_dvar(0x3cf0e0), aa_dvar(0x3cf0e8), aa_dvar(0x3cf0d0), aa_dvar(0x3cf0d8), aa_dvar(0x3cf148), aa_dvar(0x3cf160),
				   aa_dvar(0x3cf158), aa_dvar(0x3cf0f0), aa_dvar(0x3cf100), aa_dvar(0x3cf050), aa_dvar(0x3cf058));
		static DWORD last[2];
		DWORD now = GetTickCount();
		if (now - last[pl] >= 250 || (snap && s->snap_t <= dt)) {
			last[pl] = now;
			int n = *(const int *)(aa + 0xe3c);
			const aa_target_t *closest = aa_pick(aa, 1.0f, 1.0f, 1e9f);
			log_printf("native: aim %d: weapon %d ranges %.0f/%.0f ads %.2f, %d targets (nearest %d at %.0f, xhair %.3f), regions %.3f %.3f / %.3f %.3f / %.3f %.3f, "
				   "stick %.2f %.2f -> slow %d, lock %d (%.3f %.3f), snap %d (%.2f %.2f)",
				   pl, weapon, range, auto_range, ads_lerp, n, closest ? closest->ent : -1, closest ? sqrtf(closest->dist2) : 0, closest ? sqrtf(closest->xhair2) : 0, region[0],
				   region[1], region[2], region[3], region[6], region[7], stick_yaw, stick_pitch, slow ? slow->ent : -1, lock ? lock->ent : -1, lock_yaw,
				   lock_pitch, snap ? snap->ent : -1, snap_yaw, snap_pitch);
		}
	}
}

// the pad's buttons: a held button runs its binding "+name" / "-name", a pressed one an action
typedef struct {
	WORD mask;
	const char *hold;  // a held button (+/-)
	const char *press; // an action on the press
} pad_map_t;
static const pad_map_t pad_map[] = {
    {0x1000, "gostand", NULL},       // A
    {0x2000, "stance", NULL},        // B (held: prone)
    {0x4000, "usereload", NULL},     // X (held: use)
    {0x8000, NULL, "weapnext"},      // Y
    {0x0100, "smoke", NULL},         // left bumper
    {0x0200, "frag", NULL},          // right bumper
    {0x0040, "breath_sprint", NULL}, // left stick
    {0x0080, "melee", NULL},         // right stick
    {0x0020, "scores", NULL},        // Back: the objectives
    {0x0010, NULL, "togglemenu"},    // Start: pause (sent as the Escape key: pad_frame_player)
    {0x0001, "actionslot 1", NULL},  // d-pad up
    {0x0002, "actionslot 2", NULL},  // d-pad down
    {0x0004, "actionslot 3", NULL},  // d-pad left
    {0x0008, "actionslot 4", NULL},  // d-pad right
};
#define PAD_MAP_N (sizeof(pad_map) / sizeof(pad_map[0]))

// For the hints' button pictures (padglyphs.c): the button a game command is on in this layout, read from the same tables the input goes through (pad_map,
// pad_frame_player's triggers and left stick, Back's markers: ping_pad), the tactical layout's swap of B and the right stick click applied. An XInput button mask,
// or NATIVE_PAD_LT / RT / LUP / LDOWN / LLEFT / LRIGHT (native.h); 0: the command is on no button of the pad (Back's "scores" is not: Back is the markers).
unsigned native_pad_button_of(const char *cmd, int tactical)
{
	static const struct {
		const char *cmd;
		unsigned button;
	} more[] = {
	    {"attack", NATIVE_PAD_RT},     {"speed_throw", NATIVE_PAD_LT},     // the triggers (pad_frame_player: pad_binding "attack" / "speed_throw")
	    {"forward", NATIVE_PAD_LUP},   {"back", NATIVE_PAD_LDOWN},         // the left stick (pad_frame_player: the usercmd's forward and right moves)
	    {"moveleft", NATIVE_PAD_LLEFT}, {"moveright", NATIVE_PAD_LRIGHT},
	    {"mw2coop_ping", 0x0020},                                          // Back: the markers
	};
	if (!cmd)
		return 0;
	if (*cmd == '+' || *cmd == '-')
		cmd++;
	unsigned b = 0;
	for (size_t i = 0; i < PAD_MAP_N && !b; i++) {
		const char *c = pad_map[i].hold ? pad_map[i].hold : pad_map[i].press;
		if (pad_map[i].mask != 0x0020 && c && !_stricmp(cmd, c))
			b = pad_map[i].mask;
	}
	for (size_t i = 0; i < sizeof(more) / sizeof(more[0]) && !b; i++)
		if (!_stricmp(cmd, more[i].cmd))
			b = more[i].button;
	if (tactical && (b == 0x2000 || b == 0x0080)) // (pad_frame_player: "the tactical layout: B and the right stick click change places")
		b ^= 0x2000 | 0x0080;
	return b;
}

// one player's pad, his context live; play=0 reads it for the menus only (his context is not drawn: one picture)
static void pad_frame_player(int pl, int play)
{
	pad_player_t *p = &pads[pl];
	if (pad_dev[pl] < 0)
		return;
	xin_state_t st;
	static LARGE_INTEGER freq;
	LARGE_INTEGER now;
	if (!freq.QuadPart)
		QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&now);
	float dt = p->last_count ? (float)(now.QuadPart - p->last_count) / (float)freq.QuadPart : 0.016f;
	p->last_count = now.QuadPart;
	if (dt > 0.1f)
		dt = 0.1f;
	if (!pad_read(pl, &st)) { // no pad: no input
		if (inputlog_on()) {
			static DWORD lost_at[2];
			if (GetTickCount() - lost_at[pl] > 1000) {
				lost_at[pl] = GetTickCount();
				inputlog("PADLOST,%d,%d,%d", pl, pad_dev[pl], ctx_current);
			}
		}
		p->forward = p->right = 0;
		p->active = 0;
		// what it held is let go (night audit A3: a wireless pad that dropped with the trigger, aim, sprint or stance held kept that player firing / aiming / crouched
		// until it came back and the button was pressed and released again); his context is the live one here
		for (size_t i = 0; i < PAD_MAP_N && p->held; i++)
			if ((p->held & pad_map[i].mask) && pad_map[i].hold)
				pad_binding(pad_map[i].hold, 0);
		if (p->rt)
			pad_binding("attack", 0);
		if (p->lt)
			pad_binding("speed_throw", 0);
		p->held = 0;
		p->rt = p->lt = 0;
		return;
	}
	p->active = 1;
	{ // the log says when a menu takes the keys and when it gives them back (pause, objectives, options: for reports and the QA of the pads in menus)
		static int menu_was = -1, menu_logged;
		const int menu_now = (*(unsigned *)(base + RVA_KEY_CATCHERS) & 0x10) != 0;
		if (menu_now != menu_was && menu_logged++ < 200)
			log_printf("native: %s", menu_now ? "a menu has the keys" : "no menu has the keys");
		menu_was = menu_now;
	}
	if (*(unsigned *)(base + RVA_KEY_CATCHERS) & 0x10) { // a menu: either player's pad works it (the menu is one, shared; until 2026-11-17 player 2's pad was left out here and he could only pause)
		if (inputlog_on()) {
			static DWORD menu_at[2];
			if (GetTickCount() - menu_at[pl] > 250 || st.pad.buttons) {
				menu_at[pl] = GetTickCount();
				inputlog("PADMENU,%d,%d,%d,%u,%d,%d,%d,%d,%u", pl, pad_dev[pl], ctx_current, *(unsigned *)(base + RVA_KEY_CATCHERS), st.pad.lx, st.pad.ly, st.pad.rx, st.pad.ry, st.pad.buttons);
			}
		}
		pad_menu(pl, &st);
		p->forward = p->right = 0;
		rumble_frame(pl, pad_dev[pl], 0); // paused: still
		return;
	}
	p->menu_held = st.pad.buttons; // (a button that closed a menu does not act in the game too)
	if (!play)
		return;
	padopts_t o; // the player's options (Options > Controls > Gamepad: Alpha / Bravo)
	padopts_get(pl, &o);
	rumble_frame(pl, pad_dev[pl], o.rumble);
	const float move_dead = CFG_ONCE("padmovedead", 18) / 100.0f, look_dead = CFG_ONCE("paddead", 22) / 100.0f;
	const float curve = CFG_ONCE("padcurve", 160) / 100.0f, trig = CFG_ONCE("padtrigger", 30) * 255 / 100.0f;
	float lx = st.pad.lx / 32767.0f, ly = st.pad.ly / 32767.0f, rx = st.pad.rx / 32767.0f, ry = st.pad.ry / 32767.0f;
	if (o.southpaw) { // the left stick looks, the right one moves
		float x = lx, y = ly;
		lx = rx, ly = ry, rx = x, ry = y;
	}
	pad_stick(&lx, &ly, move_dead, 1.0f);
	pad_stick(&rx, &ry, look_dead, curve);
	p->forward = (int)(ly * 127.0f);
	p->right = (int)(lx * 127.0f);
	const int rt = st.pad.rt > trig, lt = st.pad.lt > trig;
	// A zoomed view turns slower, as the game turns the mouse: the game multiplies the mouse's turn by its look sensitivity (64-bit rva 0x533d04, 32-bit VA 0x925c24:
	// the live client's, written each frame by 0x941b0 from cg's tan(view fov / 2) / tan(32.5), rva 0xbc1e0, the view fov being cg_fov or the zoom's, times cg_fovScale,
	// cg_playerFovScale0/1 and a split screen adjustment). The pad's turn takes the same against the player's unzoomed view: the same formula for cg_fov times
	// cg_fovScale times cg_playerFovScale of his slot (a running maximum of the game's value took a level intro's wider view for the unzoomed one and made every
	// zoom of that level 30% slower, QA barrettpad on the official 32-bit exe: 1.000 against 0.715). The aim key's slower turn (padads) stays as it was where the zoom is less (a rifle's sights); a scope or a scoped turret turns by its zoom. Issue 28 (the official 32-bit
	// split screen): on the Barrett of Of Their Own Accord a little stick swung the scope to the screen's edge: the stick turned 15 degrees a second there as on foot,
	// while the game turned the mouse by 0.29 of its unzoomed turn there, by 0.06 zoomed in (QA barrettpad). [native] padzoom=0: a pad turns at one speed, zoomed or not.
	float zoom_f = 1.0f;
	if (CFG_ONCE("padzoom", 1)) {
		static dvar_t *fov_d, *fovs_d, *pfovs_d[2];
		if (!fov_d)
			fov_d = game_find_dvar("cg_fov");
		if (!fovs_d)
			fovs_d = game_find_dvar("cg_fovScale");
		if (!pfovs_d[pl])
			pfovs_d[pl] = game_find_dvar(pl ? "cg_playerFovScale1" : "cg_playerFovScale0");
		const float hip = (fov_d ? fov_d->current.value : 65.0f) * (fovs_d ? fovs_d->current.value : 1.0f) * (pfovs_d[pl] ? pfovs_d[pl]->current.value : 1.0f);
		const float zs = *(volatile float *)(base + RVA_ACC_PITCH - 0x10);
		pad_zs_hip[pl] = hip > 1.0f && hip < 170.0f ? tanf(hip * 0.0087266463f) / 0.63707024f : 1.0f;
		if (zs > 0.0f && pad_zs_hip[pl] > 0.0f)
			zoom_f = zs / pad_zs_hip[pl];
		zoom_f = zoom_f > 1.0f ? 1.0f : zoom_f < 0.01f ? 0.01f : zoom_f;
		static int zoomed_was[2];
		static int zoom_lines;
		const int zoomed = zoom_f < 0.4f;
		if (zoomed != zoomed_was[pl] && zoom_lines < 100) {
			zoomed_was[pl] = zoomed, zoom_lines++;
			log_printf("native: pad %d: %s (the game's look sensitivity %.3f, unzoomed %.3f)", pl, zoomed ? "a zoomed view: the stick turns by the zoom" : "the zoom is off", zs,
				   pad_zs_hip[pl]);
		}
	}
	const float ads_f = lt ? CFG_ONCE("padads", 42) / 100.0f : 1.0f;
	const float scale = (ads_f < zoom_f ? ads_f : zoom_f) * o.sens / 3.0f; // sensitivity 3: the speeds of padyaw / padpitch
	float yaw = -rx * (float)CFG_ONCE("padyaw", 180) * scale * dt;                       // left is positive
	float pitch = ((CFG_ONCE("padinvert", 0) != 0) != o.invert ? 1.0f : -1.0f) * ry * (float)CFG_ONCE("padpitch", 130) * scale * dt; // down is positive
	// The engine's own look (CL_MouseMove, rva 0xcfcf0; 32-bit VA 0x578fe0) does nothing while the playerstate flag 0x800 is set (a scripted or linked scene: the car ride of Favela, the elevator of the
	// airport): the mouse cannot turn the view then, and its limits are not applied. The pad added its look to the accumulator regardless, so a stick held through such a scene
	// wound player 2's view angle up by whole turns (the input log of a test: -3198 degrees in 30 seconds) and the server's arc limit of a linked player then flipped between its
	// two ends as the angle went round: the camera turned back and forth by itself, or, when the scene ended, jumped to wherever the angle had got to. [native] padlock=0 off.
	if ((*(unsigned *)(base + RVA_PM_FLAGS) & 0x800) && CFG_ONCE("padlock", 1))
		yaw = pitch = 0;
	// A remote-controlled missile (the Predator of Contingency and Wolverines!; ControlsLinkTo sets the playerstate flag 0x80000): the usercmd maker (rva 0xd03a0; 32-bit VA
	// 0x579a80) then skips the look and the movement and calls 0xcff70 (VA 0x579960), which steers with the MOUSE's movement only: its delta times m_yaw / m_pitch, held to
	// -1..1, times 127, into the usercmd's two remote control bytes (+0x3c pitch, +0x3d yaw). The view angles do not steer it. So a pad could not steer the missile at all (Nexus
	// report, 2026-11-16: "when you try to move it to aim at the enemy tank, it won't"; QA predsteer: the missile's angles stayed 25 157 0 with the right stick held), and its
	// stick wound the view angle up meanwhile (-241 degrees in 2.4 s). The stick now goes into those two bytes as the mouse does (make_cmd_hook), and the view angle is left
	// alone, as the mouse leaves it. [native] padremote=0 off.
	p->rc_pitch = p->rc_yaw = 0;
	const int remote = (*(unsigned *)(base + RVA_PM_FLAGS) & 0x80000) && CFG_ONCE("padremote", 1);
	if (remote) {
		const float k = o.sens / 3.0f > 1.0f ? o.sens / 3.0f : 1.0f; // a higher sensitivity reaches the full turn rate with less stick
		float ry_rc = ((CFG_ONCE("padinvert", 0) != 0) != o.invert ? 1.0f : -1.0f) * ry * k, rx_rc = -rx * k; // pitch: down is positive; yaw: left is positive
		ry_rc = ry_rc > 1.0f ? 1.0f : ry_rc < -1.0f ? -1.0f : ry_rc;
		rx_rc = rx_rc > 1.0f ? 1.0f : rx_rc < -1.0f ? -1.0f : rx_rc;
		p->rc_pitch = (int)floorf(ry_rc * 127.0f + 0.5f);
		p->rc_yaw = (int)floorf(rx_rc * 127.0f + 0.5f);
		yaw = pitch = 0;
	}
	// the markers' wheel (src/ping.c): Back held opens it and the look stick picks a kind of marker; the view stays where it was meanwhile (owner, 2026-11-23)
	const int wheel = ping_pad(pl, (st.pad.buttons & 0x0020) != 0, rx, ry);
	if (wheel)
		yaw = pitch = 0, p->rc_pitch = p->rc_yaw = 0;
	const float yaw_in = yaw, pitch_in = pitch;
	if (o.assist && !qa_noassist[pl] && !remote && !wheel) // (the aim assist turns the view, which a remote-controlled missile does not follow)
		aim_assist(pl, dt, &yaw, &pitch, sqrtf(lx * lx + ly * ly), sqrtf(rx * rx + ry * ry), lt);
	else
		aa_state[pl].lock_ent = aa_state[pl].snap_ent = -1, aa_state[pl].ads_was = lt;
	float *acc = (float *)(base + RVA_ACC_PITCH); // the live context's
	acc[1] += yaw;
	acc[0] += pitch;
	if (acc[0] > 85.0f)
		acc[0] = 85.0f;
	if (acc[0] < -85.0f)
		acc[0] = -85.0f;
	if (inputlog_on()) {
		static DWORD row_at[2];
		DWORD now_ms = GetTickCount();
		if (st.pad.lx || st.pad.ly || st.pad.rx || st.pad.ry || st.pad.buttons || st.pad.lt || st.pad.rt || now_ms - row_at[pl] > 250) {
			row_at[pl] = now_ms;
			inputlog("PAD,%d,%d,%d,%d,%.4f,%d,%d,%d,%d,%u,%d,%d,%u,%d,%d,%.4f,%.4f,%.4f,%.4f,%.3f,%.3f,%d,%08x", pl, pad_dev[pl], ctx_current, play, dt, st.pad.lx, st.pad.ly, st.pad.rx, st.pad.ry, st.pad.buttons, st.pad.lt, st.pad.rt,
				 *(unsigned *)(base + RVA_KEY_CATCHERS), p->forward, p->right, yaw_in, yaw, pitch_in, pitch, acc[1], acc[0], o.assist && !qa_noassist[pl], *(unsigned *)(base + RVA_PM_FLAGS));
		}
	}
	if (pl == 1) { // player 2's hands are empty and a script asks for his weapon (maps\_mw2coop::sync_weapons): his client keeps its own weapon choice, so the script's SwitchToWeapon never
		       // held (tester reports: "player 2 has no weapon until he presses 1 or Y"; QA heldweapons: his own pad Y fixed it at once): the pad's Y is pressed for him
		char v[24];
		static char last[24];
		if (game_dvar_raw("mw2coop_p2_weapnext", v, sizeof(v)) && v[0] && strcmp(v, last)) {
			snprintf(last, sizeof(last), "%s", v);
			pad_action("weapnext");
			log_printf("native: player 2's hands were empty: weapnext pressed for him (%s)", v);
		}
	}
	const pad_map_t *map = pad_map;
	WORD buttons = st.pad.buttons;
	if (o.tactical && ((buttons & 0x2000) != 0) != ((buttons & 0x0080) != 0)) // the tactical layout: B and the right stick click change places
		buttons ^= 0x2000 | 0x0080;
	for (size_t i = 0; i < PAD_MAP_N; i++) {
		int was = (p->held & map[i].mask) != 0, is = (buttons & map[i].mask) != 0;
		if (was == is)
			continue;
		if (map[i].mask == 0x0020) // Back: the markers (ping_pad above: a short press marks, held it opens the wheel; the objectives are in the pause menu)
			continue;
		if (map[i].hold)
			pad_binding(map[i].hold, is);
		else if (map[i].press && is && map[i].mask == 0x0010) {
			// Start: the Escape key, as the keyboard pauses (the action "togglemenu" only queued a client command no part of the game handles: no pad could
			// open the pause menu; QA pausepad2 2026-11-17, two presses of player 2's Start and no menu took the keys)
			pad_key(27, 1);
			pad_key(27, 0);
		} else if (map[i].press && is)
			pad_action(map[i].press);
	}
	p->held = buttons;
	if (rt != p->rt)
		pad_binding("attack", rt);
	if (lt != p->lt)
		pad_binding("speed_throw", lt);
	p->rt = rt;
	p->lt = lt;
}

// The HUD laid out as in the console's split screen. The Xbox 360 game draws a half's HUD with split screen versions of its HUD menus (ui/hud_splitscreen.txt:
// compassv2_ss, weaponinfo_ss, ...): the ammo, the grenades and the compass sit further in from the corner, the hints nearer the cross hair, the low ammo warning
// bigger, right of the cross hair. The PC game ships that menu list empty and its code never reads the split screen dvars, so while the picture is split the
// items of the PC's own HUD menus (the cgame's menu list, which both contexts draw) are moved the same way, and put back when it is one picture again ([native]
// hudss=0 off). The HUD is drawn from an item's rectClient (added to its menu's place), so both rects are moved. A menu is left as it is when its first item is not
// where the full screen HUD has it (another build of the menus). The PC's d-pad (the action slot keys, bottom centre) has no console counterpart and stays.
#ifdef _WIN64
#define CG_DC_RVA XR(0x51e330)   // the cgame's UI context of local client 0 (ui/hud.txt is added to it at rva 0xa6b7f), menus[] at +0x38, their count at +0x1438
#define ITEM_TEXT_SCALE 0xe0 // itemDef_t: type, dataType, alignment, font, textAlignMode, textalignx, textaligny, textscale
#else
#define CG_DC_RVA 0x354d28 // VA 0x754d28 (+ 0x1684 per local client; the list is added at VA 0x5658a4)
#define ITEM_TEXT_SCALE 0xd4
#endif
#define ITEM_RECT_W 0x10
#define ITEM_RECT_CLIENT (ITEM_RECT + 0x14) // rectClient: x, y, w, h, horizontal and vertical alignment (+0x10, +0x11)
#ifdef _WIN64
#define ITEM_OWNER_DRAW 0x40 // windowDef_t: name, rect, rectClient, group, style, border, ownerDraw, ownerDrawFlags, borderSize, staticFlags, dynamicFlags (+0x50)
#else
#define ITEM_OWNER_DRAW 0x38
#endif
typedef struct {
	const char *menu;
	int item;	  // -1: every item of the menu without a line of its own
	float x0, y0;	  // where the full screen HUD has it (the first item when item is -1): the check that these are the menus the moves are for
	float dx, dy, w;  // the move; w > 0: the new width
	int align_v;	  // >= 0: the new vertical alignment
	float scale;	  // > 0: the new text scale
} hud_move_t;
static const hud_move_t hud_moves[] = {
    {"weaponinfo", 0, -347.333f, -74.6667f, -19.333f, 1.333f, 0, -1, 0}, // the bar under the weapon's name moves less than the rest
    {"weaponinfo", -1, -347.333f, -74.6667f, -50, 2, 0, -1, 0},		  // ammo, clip pictures, grenades
    // (the same menu, item by item from the console disc's ui/hud.txt and ui/hud_splitscreen.txt, items paired by their draw type: the two thin lines and the two ammo counts move 88.667 to the left, not 50)
    {"weaponinfo", 1, -131.333f, -32.6667f, -88.6667f, 2, 0, -1, 0},	  // the line above the grenade icons (draw type 119)
    {"weaponinfo", 12, -135.333f, -40, -88.6667f, -0.6667f, 0, -1, 0},	  // clip ammo count (117)
    {"weaponinfo", 13, -135.333f, -46, -88.6667f, 22.6667f, 0, -1, 0},	  // stock ammo count (121): lower than on the full screen HUD
    {"weaponinfo", 14, -103.333f, -52.6667f, -88.6667f, 2, 0, -1, 0},	  // the line under the weapon's name (81)
    {"compassv2", -1, -86, -74.6667f, -20, 0, 0, -1, 0},
    {"stance", -1, -84, -76, -20, 0, 0, -1, 0}, // (the PC draws the stance inside the compass, not above the ammo as the console: it moves with the compass)
    {"reload", 0, -50, -20, 110, -12, 200, -1, 0.5833f}, // "Low ammo" / "Reload": right of the cross hair, bigger
    {"cursorhints", 0, 0, 70, 0, -42, 0, -1, 0},		      // "Press [Use] to ...": nearer the cross hair
    {"mantlehint", 0, 0, 126, 0, -46, 0, -1, 0},
    // (the console puts the scope's "hold breath" hint and the invalid command hint at the top of the half, with an alignment the PC places at the top left:
    // they stay where the PC has them, above the cross hair, which fits a half)
};
#define HUD_MOVES (sizeof(hud_moves) / sizeof(hud_moves[0]))
typedef struct {
	uint8_t *item;
	float was[3], now[3];	// rect: x, y, w
	float cwas[3], cnow[3]; // rectClient (the one the HUD is drawn from: the menu's place plus this): x, y, w
	uint8_t was_ah, now_ah, cwas_ah, cnow_ah;
	uint8_t was_av, now_av, cwas_av, now_av_client;
	uint8_t center_hint;
	float was_scale, now_scale;
} hud_slot_t;
static hud_slot_t hud_slots[64];
static int hud_nslots, hud_applied;
static uint8_t *hud_menu(const char *name, int *count)
{
#ifdef _WIN64
	uint32_t r = CG_DC_RVA;
#else
	uint32_t r = XR(0x354d28); // CG_DC_RVA, written out for tools/gen_addr159.py (iw4x-sp: 0 when the 159 build has no match)
#endif
	if (!r)
		return NULL;
	uint8_t *dc = base + r;
#ifdef _WIN64
	uint8_t **menus = (uint8_t **)(dc + 0x38);
	int n = *(int *)(dc + 0x1438);
#else
	uint8_t **menus = (uint8_t **)(dc + 0x38);
	int n = *(int *)(dc + 0xa38);
#endif
	if (n <= 0 || n > 640)
		return NULL;
	for (int i = 0; i < n; i++) {
		const char *mn = menus[i] ? *(const char **)menus[i] : NULL;
		if (mn && !strcmp(mn, name)) {
			*count = *(int *)(menus[i] + MENU_ITEM_COUNT);
			return menus[i];
		}
	}
	return NULL;
}
uint8_t *native_cg_find_menu(const char *name)
{
	int count;
	return hud_menu(name, &count);
}
static int near_f(float a, float b)
{
	return fabsf(a - b) < 0.05f;
}
static void hud_log_menus(void) // QA ([native] hudlog=1): every menu of the cgame's list with its items' rects, alignments and text scales
{
#ifdef _WIN64
	uint32_t r = CG_DC_RVA;
	int off_count = 0x1438;
#else
	uint32_t r = XR(0x354d28);
	int off_count = 0xa38;
#endif
	if (!r)
		return;
	uint8_t *dc = base + r;
	uint8_t **menus = (uint8_t **)(dc + 0x38);
	int n = *(int *)(dc + off_count);
	log_printf("native: hudlog: %d menus in the cgame's list", n);
	for (int m = 0; m < n && m < 640; m++) {
		if (!menus[m])
			continue;
		int ic = *(int *)(menus[m] + MENU_ITEM_COUNT);
		float *mr = (float *)(menus[m] + ITEM_RECT);
		log_printf("native: hudlog: %s rect %g %g %g %g align %d %d, %d items", *(const char **)menus[m], mr[0], mr[1], mr[2], mr[3], menus[m][ITEM_RECT_ALIGN_H],
			   menus[m][ITEM_RECT_ALIGN_V], ic);
		uint8_t **items = *(uint8_t ***)(menus[m] + MENU_ITEMS);
		for (int i = 0; i < ic && i < 40; i++) {
			if (!items[i])
				continue;
			float *rc = (float *)(items[i] + ITEM_RECT), *cl = (float *)(items[i] + ITEM_RECT_CLIENT);
			const char *nm = *(const char **)items[i];
			log_printf("native: hudlog:   [%d] %s rect %g %g %g %g align %d %d client %g %g type %d ownerdraw %d scale %g", i, nm ? nm : "-", rc[0], rc[1], rc[2], rc[3],
				   items[i][ITEM_RECT_ALIGN_H], items[i][ITEM_RECT_ALIGN_V], cl[0], cl[1], *(int *)(items[i] + ITEM_TYPE), *(int *)(items[i] + ITEM_OWNER_DRAW),
				   *(float *)(items[i] + ITEM_TEXT_SCALE));
		}
	}
}
static void hud_apply(void)
{
	static int logged_menus;
	if (CFG_ONCE("hudlog", 0) && !logged_menus++)
		hud_log_menus();
	hud_nslots = 0;
	int moved_menus = 0;
	for (size_t m = 0; m < HUD_MOVES; m++) {
		if (m && !strcmp(hud_moves[m].menu, hud_moves[m - 1].menu))
			continue; // each menu once, with all its lines
		int count = 0;
		uint8_t *menu = hud_menu(hud_moves[m].menu, &count);
		if (!menu || count <= 0 || count > 32) {
			log_printf("native: hudss: no HUD menu %s", hud_moves[m].menu);
			continue;
		}
		uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
		const hud_move_t *all = NULL;
		int ok = 1;
		for (size_t k = m; k < HUD_MOVES && !strcmp(hud_moves[k].menu, hud_moves[m].menu); k++) {
			const hud_move_t *mv = &hud_moves[k];
			int i = mv->item < 0 ? 0 : mv->item;
			if (mv->item < 0)
				all = mv;
			float *rc = i < count && items[i] ? (float *)(items[i] + ITEM_RECT) : NULL;
			if (!rc || !near_f(rc[0], mv->x0) || !near_f(rc[1], mv->y0)) {
				log_printf("native: hudss: %s[%d] is at %g %g, not %g %g: the menu is left as it is", mv->menu, i, rc ? rc[0] : 0, rc ? rc[1] : 0, mv->x0, mv->y0);
				ok = 0;
			}
		}
		if (!ok)
			continue;
		if (CFG_ONCE("hintdx", 0) && (!strcmp(hud_moves[m].menu, "cursorhints") || !strcmp(hud_moves[m].menu, "mantlehint"))) { // QA experiment: the menu's own rect too
			static float moved[4];
			static int n;
			float *mr = (float *)(menu + ITEM_RECT);
			if (n < 4)
				moved[n++] = mr[0];
			mr[0] += (float)CFG_ONCE("hintdx", 0);
		}
		for (int i = 0; i < count && hud_nslots < (int)(sizeof(hud_slots) / sizeof(hud_slots[0])); i++) {
			const hud_move_t *mv = all;
			for (size_t k = m; k < HUD_MOVES && !strcmp(hud_moves[k].menu, hud_moves[m].menu); k++)
				if (hud_moves[k].item == i)
					mv = &hud_moves[k];
			if (!mv || !items[i])
				continue;
			hud_slot_t *s = &hud_slots[hud_nslots++];
			float *rc = (float *)(items[i] + ITEM_RECT);
			s->item = items[i];
			s->was[0] = rc[0], s->was[1] = rc[1], s->was[2] = rc[2];
			s->was_ah = items[i][ITEM_RECT_ALIGN_H];
			s->was_av = items[i][ITEM_RECT_ALIGN_V];
			s->was_scale = *(float *)(items[i] + ITEM_TEXT_SCALE);
			s->now[0] = rc[0] + mv->dx, s->now[1] = rc[1] + mv->dy, s->now[2] = mv->w > 0 ? mv->w : rc[2];
			float *cl = (float *)(items[i] + ITEM_RECT_CLIENT);
			s->cwas[0] = cl[0], s->cwas[1] = cl[1], s->cwas[2] = cl[2];
			s->cwas_ah = items[i][ITEM_RECT_CLIENT + 0x10];
			s->cwas_av = items[i][ITEM_RECT_CLIENT + 0x11];
			s->now_ah = s->was_ah;
			s->cnow_ah = s->cwas_ah;
			s->cnow[0] = cl[0] + mv->dx, s->cnow[1] = cl[1] + mv->dy, s->cnow[2] = mv->w > 0 ? mv->w : cl[2];
			if (!strcmp(hud_moves[m].menu, "cursorhints") || !strcmp(hud_moves[m].menu, "mantlehint")) { // QA ([native] hintdx=<units>): where the hints land for a given shift
				s->now[0] += (float)CFG_ONCE("hintdx", 0);
				s->cnow[0] += (float)CFG_ONCE("hintdx", 0);
				s->center_hint = 1;
				s->now_ah = s->cnow_ah = menu[ITEM_RECT_ALIGN_H];
				s->now_av = s->now_av_client = menu[ITEM_RECT_ALIGN_V];
			} else {
				s->center_hint = 0;
				s->now_av = mv->align_v >= 0 ? (uint8_t)mv->align_v : s->was_av;
				s->now_av_client = s->now_av;
			}
			s->now_scale = mv->scale > 0 ? mv->scale : s->was_scale;
		}
		moved_menus++;
	}
	hud_applied = 1;
	static int logged;
	if (!logged++)
		log_printf("native: hudss: the HUD is laid out as in the console's split screen (%d items of %d menus)", hud_nslots, moved_menus);
}
// Keep cursor/mantle hint items aligned to their parent menus on both axes. The engine may
// rebuild rectClient while reopening a menu, so reapply these four bytes without moving the rect.
static void hud_hint_align_write(hud_slot_t *s)
{
	if (!s || !s->item || !s->center_hint)
		return;
	uint8_t *item = s->item;
	if (item[ITEM_RECT_ALIGN_H] == s->was_ah || item[ITEM_RECT_ALIGN_H] == s->now_ah)
		item[ITEM_RECT_ALIGN_H] = s->now_ah;
	if (item[ITEM_RECT_CLIENT + 0x10] == s->cwas_ah || item[ITEM_RECT_CLIENT + 0x10] == s->cnow_ah)
		item[ITEM_RECT_CLIENT + 0x10] = s->cnow_ah;
	if (item[ITEM_RECT_ALIGN_V] == s->was_av || item[ITEM_RECT_ALIGN_V] == s->now_av)
		item[ITEM_RECT_ALIGN_V] = s->now_av;
	if (item[ITEM_RECT_CLIENT + 0x11] == s->cwas_av || item[ITEM_RECT_CLIENT + 0x11] == s->now_av_client)
		item[ITEM_RECT_CLIENT + 0x11] = s->now_av_client;
}
static void hud_hint_align_restore(hud_slot_t *s)
{
	if (!s || !s->item || !s->center_hint)
		return;
	uint8_t *item = s->item;
	if (item[ITEM_RECT_ALIGN_H] == s->now_ah)
		item[ITEM_RECT_ALIGN_H] = s->was_ah;
	if (item[ITEM_RECT_CLIENT + 0x10] == s->cnow_ah)
		item[ITEM_RECT_CLIENT + 0x10] = s->cwas_ah;
	if (item[ITEM_RECT_ALIGN_V] == s->now_av)
		item[ITEM_RECT_ALIGN_V] = s->was_av;
	if (item[ITEM_RECT_CLIENT + 0x11] == s->now_av_client)
		item[ITEM_RECT_CLIENT + 0x11] = s->cwas_av;
}
// every frame while split: the layout is (re)written where an item still has the full screen values (the engine lays a menu out again when it opens it)
static void hud_write(void)
{
	for (int i = 0; i < hud_nslots; i++) {
		hud_slot_t *s = &hud_slots[i];
		float *rc = (float *)(s->item + ITEM_RECT), *cl = (float *)(s->item + ITEM_RECT_CLIENT);
		if (near_f(cl[0], s->cwas[0]) && near_f(cl[1], s->cwas[1])) {
			cl[0] = s->cnow[0], cl[1] = s->cnow[1], cl[2] = s->cnow[2];
			s->item[ITEM_RECT_CLIENT + 0x11] = s->center_hint ? s->now_av_client : s->now_av;
			*(float *)(s->item + ITEM_TEXT_SCALE) = s->now_scale;
		}
		if (near_f(rc[0], s->was[0]) && near_f(rc[1], s->was[1])) {
			rc[0] = s->now[0], rc[1] = s->now[1], rc[2] = s->now[2];
			s->item[ITEM_RECT_ALIGN_V] = s->now_av;
		}
		hud_hint_align_write(s);
	}
}
static void hud_restore(void)
{
	for (int i = 0; i < hud_nslots; i++) {
		hud_slot_t *s = &hud_slots[i];
		float *rc = (float *)(s->item + ITEM_RECT), *cl = (float *)(s->item + ITEM_RECT_CLIENT);
		if (near_f(cl[0], s->cnow[0]) && near_f(cl[1], s->cnow[1])) {
			cl[0] = s->cwas[0], cl[1] = s->cwas[1], cl[2] = s->cwas[2];
			s->item[ITEM_RECT_CLIENT + 0x11] = s->center_hint ? s->cwas_av : s->was_av;
			*(float *)(s->item + ITEM_TEXT_SCALE) = s->was_scale;
		}
		if (near_f(rc[0], s->now[0]) && near_f(rc[1], s->now[1])) {
			rc[0] = s->was[0], rc[1] = s->was[1], rc[2] = s->was[2];
			s->item[ITEM_RECT_ALIGN_V] = s->was_av;
		}
		hud_hint_align_restore(s);
	}
	hud_applied = 0;
}
/* The stock cursor/mantle hint menus are centered, but item and rectClient can retain
 * stale edge alignment after the engine rebuilds a menu. Keep their alignment equal to
 * the parent menu without changing any rectangle coordinates or sizes. */
static void hud_hint_single_tick(void)
{
	static const char *const names[] = {"cursorhints", "mantlehint"};
	for (int k = 0; k < 2; k++) {
		int count = 0;
		uint8_t *menu = hud_menu(names[k], &count);
		if (!menu || count <= 0 || count > 4)
			continue;
		uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
		uint8_t *item = items ? items[0] : NULL;
		if (!item)
			continue;
		uint8_t parent_h = menu[ITEM_RECT_ALIGN_H];
		uint8_t parent_v = menu[ITEM_RECT_ALIGN_V];
		uint8_t *item_h = item + ITEM_RECT_ALIGN_H;
		uint8_t *item_v = item + ITEM_RECT_ALIGN_V;
		uint8_t *client_h = item + ITEM_RECT_CLIENT + 0x10;
		uint8_t *client_v = item + ITEM_RECT_CLIENT + 0x11;
		if (parent_h != 2 || parent_v != 2 || (*item_h != 0 && *item_h != 2) || (*client_h != 0 && *client_h != 2) ||
		    *item_v > 2 || *client_v > 2)
			continue; // only stock/default hint alignments are corrected
		*item_h = *client_h = parent_h;
		*item_v = *client_v = parent_v;
	}
}
static void hud_layout_tick(int split)
{
	if (split && mode == 11 && CFG_ONCE("hudss", 1)) {
		if (!hud_applied)
			hud_apply();
		hud_write();
		padglyphs_log = CFG_ONCE("hudlog", 0) == 3;
		if (CFG_ONCE("hudlog", 0) == 3) { // QA: the hint items as the engine has them, about once a second
			static DWORD last;
			DWORD now = GetTickCount();
			if (now - last > 1000) {
				last = now;
				static const char *const names[] = {"cursorhints", "mantlehint"};
				for (int k = 0; k < 2; k++) {
					int c = 0;
					uint8_t *m = hud_menu(names[k], &c);
					if (!m)
						continue;
					float *mr = (float *)(m + ITEM_RECT);
					uint8_t **items = *(uint8_t ***)(m + MENU_ITEMS);
					log_printf("native: hudhint: %s menu rect %g %g %g %g align %d %d", names[k], mr[0], mr[1], mr[2], mr[3], m[ITEM_RECT_ALIGN_H], m[ITEM_RECT_ALIGN_V]);
					for (int i = 0; i < c && i < 4; i++)
						if (items[i]) {
							float *rc = (float *)(items[i] + ITEM_RECT), *cl = (float *)(items[i] + ITEM_RECT_CLIENT);
							log_printf("native: hudhint:   [%d] rect %g %g %g %g align %d %d client %g %g %g %g align %d %d", i, rc[0], rc[1], rc[2], rc[3], items[i][ITEM_RECT_ALIGN_H], items[i][ITEM_RECT_ALIGN_V], cl[0], cl[1], cl[2], cl[3], items[i][ITEM_RECT_CLIENT + 0x10], items[i][ITEM_RECT_CLIENT + 0x11]);
						}
				}
			}
		}
		static int logged_after;
		if (CFG_ONCE("hudlog", 0) == 2 && !logged_after++) { // QA: the items after the move
			int c = 0;
			uint8_t *m = hud_menu("weaponinfo", &c);
			if (m) {
				uint8_t **items = *(uint8_t ***)(m + MENU_ITEMS);
				for (int i = 0; i < c && i < 20; i++)
					if (items[i]) {
						float *rc = (float *)(items[i] + ITEM_RECT);
						log_printf("native: hudafter: weaponinfo[%d] rect %g %g %g %g", i, rc[0], rc[1], rc[2], rc[3]);
					}
			}
		}
	} else {
		if (hud_applied)
			hud_restore();
		hud_hint_single_tick();
	}
}

// Diagnostics ([native] inputlog=1): a row about the state of the input at the start of each context's client frame (4 a second per context) and the four XInput pads once a second
static void inputlog_frame(int ctx)
{
	if (!inputlog_on())
		return;
	static DWORD at[2], xi_at;
	DWORD now = GetTickCount();
	if (ctx < 0 || ctx > 1 || now - at[ctx] < 250)
		return;
	at[ctx] = now;
	POINT cp = {0, 0};
	GetCursorPos(&cp);
	DWORD pid = 0;
	HWND fg = GetForegroundWindow();
	if (fg)
		GetWindowThreadProcessId(fg, &pid);
	char map[40] = "";
	game_dvar_raw("mapname", map, sizeof(map));
	inputlog("FRAME,%d,%d,%d,%d,%d,%d,%u,%ld,%ld,%d,%s", ctx, split_on, native_active, native_active ? kb_player() : 0, pad_dev[0], pad_dev[1], *(unsigned *)(base + RVA_KEY_CATCHERS), (long)cp.x, (long)cp.y,
		 pid == GetCurrentProcessId(), map);
	if (xinput_get && now - xi_at >= 1000) {
		xi_at = now;
		for (DWORD i = 0; i < 4; i++) {
			xin_state_t s;
			memset(&s, 0, sizeof(s));
			DWORD rc = xinput_get(i, &s);
			inputlog("XI,%lu,%lu,%d,%d,%d,%d,%u,%d,%d", (unsigned long)i, (unsigned long)rc, s.pad.lx, s.pad.ly, s.pad.rx, s.pad.ry, s.pad.buttons, s.pad.lt, s.pad.rt);
		}
	}
}
static void (*client_frame_original)(void);
// [native] scrwatch=1 (iw4x-sp only, diagnostics): the script variable table (159 image VA 0x16c7480, 0x22000 entries of 16 bytes) should change only while the server
// frame runs the scripts. A copy is taken when a server step ends and compared at the next points outside it (the client frame's begin and end, the next step's start): every
// 16-byte entry that differs there was written by something that is not the server step, and is logged with its old and new bytes and the window it was found in.
#ifndef _WIN64
#define SCRW_RVA 0x12c7480u // raw-ok: iw4x-sp diagnostics, 159 image
#define SCRW_SIZE 0x220000u
static uint8_t *scrw_copy;
static unsigned scrw_logged;
static void strw_check(const char *label)
{
	// the script VM between scripts: value stack top (VA 0x190de00; the stack starts at 0x190e110, 8 bytes a value), call depth (0x190ddf8), parameter counts (0x190de08, 0x190de0c):
	// at a server step's edge nothing runs, so a top or depth that drifts away from the start shows a builtin that pushed or popped too many
	const uint32_t top = *(const uint32_t *)(base + 0x150de00); // raw-ok: iw4x-sp diagnostics, 159 image
	const int depth = *(const int *)(base + 0x150ddf8); // raw-ok: iw4x-sp diagnostics, 159 image
	const int pc1 = *(const int *)(base + 0x150de08), pc2 = *(const int *)(base + 0x150de0c); // raw-ok: iw4x-sp diagnostics, 159 image
	static int last_top = -1, last_depth = -1, logged;
	const int idx = (int)((top - 0x190e110u) / 8);
	if ((idx != last_top || depth != last_depth) && logged < 400) {
		logged++;
		log_printf("vmwatch: %s ctx %d step %u cg %u: stack top index %d, depth %d, params %d %d", label, ctx_current, hook_counts[2], cg_draw_count, idx, depth, pc1, pc2);
		last_top = idx;
		last_depth = depth;
	}
}
static void scrw_diff(const char *label, int log_it)
{
	if (!game_is_iw4x())
		return; // (iw4x-sp only, diagnostics)
	if (!CFG_ONCE("scrwatch", 0))
		return;
	strw_check(label);
	const uint8_t *live = base + SCRW_RVA;
	if (!scrw_copy) {
		scrw_copy = VirtualAlloc(NULL, SCRW_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!scrw_copy)
			return;
		memcpy(scrw_copy, live, SCRW_SIZE);
		return;
	}
	unsigned changed = 0;
	for (unsigned i = 0; i < SCRW_SIZE / 16; i++) {
		const uint32_t *a = (const uint32_t *)(live + i * 16), *b = (const uint32_t *)(scrw_copy + i * 16);
		if (a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3])
			continue;
		changed++;
		if (log_it && CFG_ONCE("scrwatch", 0) >= 2 && split_on && hook_counts[2] > 200 && scrw_logged < 4000) {
			scrw_logged++;
			log_printf("scrwatch: %s ctx %d step %u cg %u: entry 0x%x %08x %08x %08x %08x -> %08x %08x %08x %08x", label, ctx_current, hook_counts[2], cg_draw_count, i, b[0], b[1], b[2], b[3], a[0], a[1], a[2], a[3]);
		}
	}
	if (log_it && changed && CFG_ONCE("scrwatch", 0) >= 2 && split_on && hook_counts[2] > 200)
		log_printf("scrwatch: %s: %u entries changed outside the server step", label, changed);
	memcpy(scrw_copy, live, SCRW_SIZE);
}
#else
static void scrw_diff(const char *label, int log_it) { (void)label; (void)log_it; }
#endif
static void client_frame_inner(void);
static void client_frame_hook(void)
{
	scrw_diff("client frame begin", 1);
	client_frame_inner();
	scrw_diff("client frame end", 1);
}
static void client_frame_inner(void)
{
	static int fov = -1; // shared by the fixed-fov and fov-scale paths below; never reread settings.ini per frame
	{
		static int seen;
		static DWORD last;
		DWORD now = GetTickCount();
		if (seen < 4 || now - last >= 2000) {
			log_printf("native: client frame #%d (active %d, ctx %d, ready %d, split %d, cg draws %u) cls %d | sv clients %d %d | hooks: snap %u ps %u step %u ucmd %u smsg %u scmd %u | entities %d", seen, native_active, ctx_current, ctx1_ready, split_on, cg_draw_count, *(int *)(base + RVA_CLS_STATE), *(int *)(base + RVA_SV_CLIENTS), *(int *)(base + RVA_SV_CLIENTS + SV_CLIENT_STRIDE), hook_counts[0], hook_counts[1], hook_counts[2], hook_counts[3], hook_counts[4], hook_counts[5], *(int *)(base + RVA_LEVEL_NUM_ENT));
#ifndef _WIN64
			if (game_is_iw4x()) // the script variable pool (159 image): AllocVariable counts the variables of the root object (VA 0x18e7540) and of all others (0x18e7544); the table has 0x22000 entries
			log_printf("native: script variables in use: root %d, others %d, of 139264 (free list head entry %u)", *(int *)(base + 0x14e7540), *(int *)(base + 0x14e7544), (unsigned)*(const uint16_t *)(base + 0x12c7480)); // raw-ok: iw4x-sp diagnostics, 159 image
#endif
			last = now;
		}
		seen++;
	}
	if (!native_active) {
		client_frame_original();
		return;
	}
	frame_bottom = 0;
	if (stall_watch_active)
		stall_tid = GetCurrentThreadId(), stall_frame_t = perf_now(); // (the QA stall watch)
	static LONGLONG perf_prev, perf_start;
	int perf_on = CFG_ONCE("perflog", 0);
	if (perf_on) {
		LONGLONG now = perf_now();
		if (perf_prev) {
			LONGLONG g = now - perf_prev;
			perf_gap_sum += g;
			perf_gap_n++;
			if (perf_freq.QuadPart)
				perf_gap_hist[perf_bucket((double)g * 1000.0 / (double)perf_freq.QuadPart)]++;
			if (g > perf_gap_max)
				perf_gap_max = g;
			static LONGLONG c0, c1, cp, wt, st; // the counters at the frame before
			if (perf_freq.QuadPart && g * 1000 > 50 * perf_freq.QuadPart) { // a hitch: what the last frame spent in the mod's parts (the rest is the engine's)
				double ms = 1000.0 / (double)perf_freq.QuadPart;
				perf_gap_long++;
				log_printf("native: perf hitch: frame %.1f ms; since the frame before: ctx0 %.1f ms, ctx1 %.1f ms, swap copies %.1f ms, worker waits %.1f ms, server steps %.1f ms",
					   (double)g * ms, (double)(perf_ctx[0] - c0) * ms, (double)(perf_ctx[1] - c1) * ms, (double)(perf_copy - cp) * ms, (double)(perf_wait - wt) * ms,
					   (double)(perf_step - st) * ms);
			}
			c0 = perf_ctx[0], c1 = perf_ctx[1], cp = perf_copy, wt = perf_wait, st = perf_step;
		}
		perf_prev = perf_start = now;
		perf_report();
	}
	{ // the scripts' switch: the dvar mw2coop_split_view is "1" once the intro is over
		uint8_t *d = (uint8_t *)game_find_dvar("mw2coop_split_view");
		const char *v = d ? *(const char **)(d + 0x10) : NULL;
		int on = v && v[0] == '1';
		if (*(unsigned *)(base + RVA_KEY_CATCHERS) & 0x11) // key catchers of client 0 (rva 0x533cb0, set by 0xd1c10): a menu (pause, objectives, end of game) or the console has the keys: one picture
			on = 0;
		if (on != split_on) {
			log_printf("native: split view %s", on ? "on" : "off");
			inputlog("EVENT,split view %s", on ? "on" : "off");
		}
		if (on && !split_on) {
			split_tick = GetTickCount();
			// QA: the dvar mw2coop_qa_set ("name value;name value", or | between them) is set when the picture splits: inside the level, where cheat protected dvars take a set
			// (commands.txt runs before devmap, and a co-op script's GetDvar does not see it)
			char qs[256];
			static int qs_done;
			if (!qs_done && game_dvar_raw("mw2coop_qa_set", qs, sizeof(qs)) && qs[0]) {
				qs_done = 1;
				char cmd[300];
				for (char *tok = strtok(qs, ";|"); tok; tok = strtok(NULL, ";|")) {
					while (*tok == ' ')
						tok++;
					if (!*tok)
						continue;
					snprintf(cmd, sizeof(cmd), "set %s\n", tok);
					game_cbuf_addtext(cmd);
					log_printf("native: QA set at the split: %s", tok);
				}
			}
		}
		split_on = on;
		hud_layout_tick(on); // the console's split screen HUD layout while split, the full screen one otherwise
	}
	{ // QA: [native] qa_ads=<ms>: player 0 aims down the sights that long after the picture was split (and [native] qa_ads_off=<ms>: lets go), so that a frame series can be taken in the scope
		static int ads_at = -2, ads_off = -2, ads_state;
		if (ads_at == -2) {
			ads_at = cfg("qa_ads", -1);
			ads_off = cfg("qa_ads_off", -1);
		}
		if (split_on && ctx_current == 0 && ads_at >= 0) {
			DWORD since = GetTickCount() - split_tick;
			if (!ads_state && since >= (DWORD)ads_at) {
				pad_binding("speed_throw", 1);
				ads_state = 1;
				log_printf("native: qa: aiming down the sights");
			} else if (ads_state == 1 && ads_off >= 0 && since >= (DWORD)ads_off) {
				pad_binding("speed_throw", 0);
				ads_state = 2;
				log_printf("native: qa: sights released");
			}
		}
	}
	static int nopredict = -1;
	if (nopredict < 0)
		nopredict = cfg("nopredict", 0);
	uint8_t *np_dvar = *(uint8_t **)(base + RVA_NOPREDICT_DVAR); // cg_nopredict: context 1 shows the server's playerstate as it is (its own prediction fought the server)
	{ // [native] fov=<degrees>: the cg_fov (rva 0x51e1a8 holds the dvar; the angle across a 4:3 picture, 65 by default) while the picture is split
		static float fov_saved = -1;
		if (fov < 0)
			fov = cfg("fov", 0); // 0: the console's way, cg_fovScale 0.75 (below)
		uint8_t *d = *(uint8_t **)(base + RVA_FOV_DVAR);
		if (fov > 0 && d) {
			float *v = (float *)(d + 0x10);
			if (split_on) {
				if (fov_saved < 0)
					fov_saved = *v;
				*v = (float)fov;
			} else if (fov_saved >= 0) {
				*v = fov_saved;
				fov_saved = -1;
			}
		}
	}
	if (mode == 11) { // the console's split screen field of view: cg_fovScale 0.75 (maps/_load.gsc), which the game multiplies into every view's angle (cg_fov, a weapon's sight zoom, a script's lerp),
			 // where writing cg_fov itself (what [native] fov=<degrees> does) leaves the sights' zoom and the scripts' angles as for the full screen. [native] fovscale=<percent> (default 75); the
			 // game's own scripts write the dvar at each level load (1 when IsSplitScreen() is false), so it is held here
		static int fovscale = -1;
		static float scale_saved = -1;
		static dvar_t *scale_dvar;
		if (fovscale < 0)
			fovscale = cfg("fovscale", 75);
		if (!scale_dvar)
			scale_dvar = game_find_dvar("cg_fovScale");
		if (fovscale > 0 && fov == 0 && scale_dvar && scale_dvar->type == DVAR_TYPE_FLOAT) {
			if (split_on) {
				if (scale_saved < 0)
					scale_saved = scale_dvar->current.value;
				scale_dvar->current.value = (float)fovscale / 100.0f;
			} else if (scale_saved >= 0) {
				scale_dvar->current.value = scale_saved;
				scale_saved = -1;
			}
		}
	}
	if (mode == 11 && ctx1_ready && split_on) { // the 360's way (docs/native-splitscreen-360.md, phase C): player 2's client logic every frame, without its own screen update (the
						     // screen update's busy flag, rva 0x5db410, makes 0xd5b00 return); player 1's frame below draws both views (cg_draw_hook_inner, mode 11)
		frame_bottom = 1;
		if (nopredict && np_dvar)
			*(int *)(np_dvar + 0x10) = 1;
		ctx_swap(1);
		pad_frame_player(1, 1);
		inputlog_frame(1);
		uint8_t *scr_busy = base + M11_SCR_BUSY;
		const uint8_t was = *scr_busy;
		*scr_busy = 1;
		client_frame_original();
		*scr_busy = was;
		native_unswap_entities();
		ctx_swap(0);
		frame_bottom = 0;
		if (nopredict && np_dvar)
			*(int *)(np_dvar + 0x10) = 0;
		if (perf_on) {
			perf_ctx[1] += perf_now() - perf_start;
			perf_ctx_n[1]++;
		}
	}
	if (nopredict && np_dvar)
		*(int *)(np_dvar + 0x10) = 0;
	pad_frame_player(0, 1);
	if (CFG_ONCE("perflog", 0)) {
		LONGLONG t0 = perf_now();
		body_dobj_sync();
		perf_sync += perf_now() - t0;
	} else {
		body_dobj_sync();
	}
	if (native_active && CFG_ONCE("clocklog", 0)) { // QA: the game's clock (QA scenario clock) against the wall clock: milliseconds of game time per wall second
		static DWORD wall0;
		static int game0, n;
		char v[16];
		DWORD wall = GetTickCount();
		if ((!wall0 || wall - wall0 >= 1000) && game_dvar_raw("mw2coop_qa_t", v, sizeof(v))) {
			int game = atoi(v);
			if (wall0 && n < 60)
				log_printf("native: clock: game time +%d ms in %lu ms of wall time", game - game0, (unsigned long)(wall - wall0)), n++;
			wall0 = wall, game0 = game;
		}
	}
	if (native_active) { // the menu tick does not run in a level: the pause menu's Options > Controls gets the gamepad pages from here (a level load may reload the menus)
		static DWORD opts_at;
		if (GetTickCount() - opts_at >= 1000) {
			opts_at = GetTickCount();
			padopts_tick();
			gfxopts_tick();
		}
	}
	if (!(mode == 11 && ctx1_ready && split_on))
		pad_frame_player(1, 0); // one picture: player 2's pad still works the menus
	inputlog_frame(0);
	client_frame_original();
	if (perf_on) {
		perf_ctx[0] += perf_now() - perf_start;
		perf_ctx_n[0]++;
	}
}

// Diagnostics: the alpha of the co-op "waiting for the other player" text (rva 0x22eb70, called from the cgame's 2D at 0x94039).
static float (*wait_alpha_original)(int);
static float wait_alpha_hook(int lc)
{
	float a = wait_alpha_original(lc);
	static int logged;
	if ((a != 0.0f || !logged) && logged < 5) {
		logged++;
#ifdef _WIN64
		log_printf("native: waiting alpha for lc %d = %f (coop byte %d, cls %d)", lc, a, *(base + XR(0x52fb5c)), *(int *)(base + XR(0x533cb8)));
#else
		log_printf("native: waiting alpha for lc %d = %f", lc, (double)a);
#endif
	}
	// Both players are on this PC: nobody waits for the other (the message showed while level loading, until context 1's client state caught up, 2026-10-29: owner). [native] waitmsg=1 shows it again.
	if (native_active && !CFG_ONCE("waitmsg", 0))
		return 0.0f;
	return a;
}

// Context 1 has no mouse (its player has no device yet): the engine's mouse and joystick reads (rva 0xcf4b0, called at 0xcfd23 and 0xd0061) return no movement for it, so the
// real cursor only turns player 0.
#ifdef _WIN64
static void (*mouse_read_original)(intptr_t, float *, float *);
#else
// 32 bits: the mouse read has the mouse state in ESI and two stack arguments (the caller cleans them): a stub below passes plain cdecl arguments to the hook,
// and the original is called through a wrapper that sets ESI (docs/32bit-native-map/clientctx.md)
static void *mouse_read_target, *mouse_read2_target;
static void mouse_read_original(intptr_t state, float *x, float *y);
static void mouse_read2_original(intptr_t state, float *x, float *y);
#endif
static int mouse_early; // the mouse read below is hooked from the start (mouse_early_install: the markers' wheel in network co-op), not by the split screen's context hooks
static unsigned mouse1_reads; // QA (cmdlog1): player 2's mouse reads and their sum since the last log line
static float mouse1_sum[2];
static void mouse_read_hook(intptr_t state, float *x, float *y)
{
	if (ctx_current != (native_active ? kb_player() : 0) || (ctx_current == 1 && CFG_ONCE("mouse1off", 0))) { // not the mouse's player (left unread: it stays for his context); QA mouse1off: player 2 never reads it
		*x = 0;
		*y = 0;
		return;
	}
	mouse_read_original(state, x, y);
	if (ctx_current == 1 && CFG_ONCE("qa_mouse1dx", 0)) // QA: a steady mouse move for player 2 (no real cursor is moved): does his view turn? (Big Brother's gunner)
		*x += (float)CFG_ONCE("qa_mouse1dx", 0);
	if (ctx_current == 1) { // (for the QA log cmdlog1: how often player 2's mouse was read and how far it went)
		mouse1_reads++;
		mouse1_sum[0] += *x;
		mouse1_sum[1] += *y;
	}
	ping_mouse(native_active ? kb_player() : 0, x, y); // an open markers' wheel takes the movement (src/ping.c): the view stays
	if (inputlog_on() && (*x != 0 || *y != 0))
		inputlog("MOUSE,%d,%d,%d,%.3f,%.3f", ctx_current, 1, native_active ? kb_player() : 0, *x, *y);
}
#ifdef _WIN64
static void (*mouse_read2_original)(intptr_t, float *, float *);
#endif
static void mouse_read2_hook(intptr_t state, float *x, float *y)
{
	if (ctx_current != (native_active ? kb_player() : 0) || (ctx_current == 1 && CFG_ONCE("mouse1off", 0))) { // not the mouse's player (left unread: it stays for his context)
		*x = 0;
		*y = 0;
		return;
	}
	mouse_read2_original(state, x, y);
}

// The keyboard's player 2: the event loop's key events (rva 0xd0f80, called at 0x1fbc1d) run with context 1 live, so the keys press his kbuttons (in the swapped input block).
// Player 2 does not work the menus (owner, 2026-10-28: he only pauses the game): while a menu is open (key catchers 0x10) his key presses go nowhere; the console (0x1) takes them.
#ifdef _WIN64
static void (*key_event_original)(int, int, int, unsigned);
#endif
static uint8_t key_in_ctx1[512]; // the keys whose press went to context 1 (player 2 on the keyboard): their release goes there too
static void key_event_hook(int lc, int key, int down, unsigned time)
{
	unsigned catchers = *(unsigned *)(base + RVA_KEY_CATCHERS);
	const unsigned k = (unsigned)key;
	// A key player 2 pressed is released in his context whatever is open meanwhile (night audit A2: a release during the pause menu or the console went to context 0,
	// whose button never saw the press, and player 2 kept walking or firing after the pause)
	if (!down && k < sizeof(key_in_ctx1) && key_in_ctx1[k]) {
		key_in_ctx1[k] = 0;
		if (native_active && ctx1_ready && ctx_current == 0) {
			ctx_swap(1);
			key_event_original(lc, key, down, time);
#ifndef _WIN64
			cbuf_run_now();
#endif
			ctx_swap(0);
			return;
		}
	}
	if (native_active && kb_player() == 1 && (catchers & 0x10) && !(catchers & 0x1) && down)
		return;
	if (native_active && kb_player() == 1 && ctx1_ready && ctx_current == 0 && !(catchers & 0x11)) {
		if (down && k < sizeof(key_in_ctx1))
			key_in_ctx1[k] = 1;
		ctx_swap(1);
		key_event_original(lc, key, down, time);
#ifndef _WIN64
		cbuf_run_now(); // the binding's text runs now, in context 1 (the command buffer would otherwise run it in whichever context is live later)
#endif
		ctx_swap(0);
		return;
	}
	key_event_original(lc, key, down, time);
}

// The keyboard and the pad reach context 1 too (the keys are read by the same input code): its usercmd, made by 0xd03a0 (called at 0xcf8b1), loses its buttons and movement,
// so that the player who has no device stands still and his prediction agrees with the server.
#ifdef _WIN64
static uint8_t *(*make_cmd_original)(uint8_t *, int);
#else
// 32 bits: the usercmd maker takes its output buffer in EAX and the local client in EBX and returns the buffer in EAX
static void *make_cmd_target;
static uint8_t *make_cmd_original(uint8_t *out, int lc);
#endif
static volatile int qa_c0_fwd, qa_c0_fire; // QA (console mw2coop_qa_move <forward 0|1> <fire 0|1>): player 0's movement and trigger set straight in his usercmds
// Frozen controls: the playerstate flag 0x800 (a script's FreezeControls, rva 0x1617d0 sets the client's flag 4, ClientThink makes it this flag; a linked scene) makes the
// engine's key move (CL_KeyMove, 64-bit rva 0xcfa0e) give no move at all (unless cl_freemove is 2, a developer's fly mode). The pad's move written into the usercmd here
// must stay out then too: a player on foot is held by the server's own freeze anyway, but a vehicle takes its throttle and steering straight from the usercmd. The Spec
// Ops snowmobile races freeze both players for the countdown: the keyboard player waited and the pad player drove off before "GO" (owner 2026-11-23,
// so_snowrace2_cliffhanger). [native] padfreeze=0 the old way.
static int cmd_frozen(void)
{
	return (*(unsigned *)(base + RVA_PM_FLAGS) & 0x800) && CFG_ONCE("padfreeze", 1);
}
static uint8_t *make_cmd_hook(uint8_t *out, int a)
{
	uint8_t *cmd = make_cmd_original(out, a);
	if (ctx_current == 0 && cmd && (qa_c0_fwd || qa_c0_fire)) {
		if (qa_c0_fwd && !cmd_frozen()) // (as the forward key: nothing while frozen)
			cmd[0x1a] = 127;
		if (qa_c0_fire)
			*(int *)(cmd + 4) |= 1;
	}
	if (CFG_ONCE("padlog", 0) && cmd && native_active) { // QA: once a second, per context, what the usercmd maker saw: the context, its local client argument, the pad state of the player
		static DWORD at[3];
		int k = ctx_current >= 0 && ctx_current < 3 ? ctx_current : 2;
		if (GetTickCount() - at[k] >= 1000) {
			at[k] = GetTickCount();
			const pad_player_t *q = &pads[ctx_current == 1 ? 1 : 0];
			log_printf("native: padlog make_cmd ctx %d lc %d | pad_dev %d active %d forward %d right %d kb_player %d | cmd fwd %d right %d", ctx_current, a, pad_dev[ctx_current == 1 ? 1 : 0], q->active, q->forward, q->right, kb_player(),
				   (int)(int8_t)cmd[0x1a], (int)(int8_t)cmd[0x1b]);
		}
	}
	if ((ctx_current == 0 || ctx_current == 1) && cmd && native_active) {
		int pl = ctx_current;
		pad_player_t *p = &pads[pl];
		if (pad_dev[pl] >= 0 && p->active && cmd_frozen()) { // frozen: no move, as the keys give none (see cmd_frozen)
			static DWORD frozen_logged[2];
			if ((p->forward || p->right) && GetTickCount() - frozen_logged[pl] > 2000) {
				frozen_logged[pl] = GetTickCount();
				log_printf("native: padfreeze: player %d's pad move %d %d held back (controls frozen)", pl + 1, p->forward, p->right);
			}
			cmd[0x1a] = cmd[0x1b] = 0;
		} else if (pad_dev[pl] >= 0 && p->active && (p->forward || p->right || kb_player() != pl)) {
			cmd[0x1a] = (uint8_t)(int8_t)p->forward; // usercmd_s: forwardmove +0x1a, rightmove +0x1b, upmove +0x1c, downmove +0x1d
			cmd[0x1b] = (uint8_t)(int8_t)p->right;
		} else if (pl == 1 && kb_player() != 1 && !(pad_dev[1] >= 0 && p->active)) { // player 2 with no device: still
			*(int *)(cmd + 4) = 0;
			cmd[0x1a] = cmd[0x1b] = cmd[0x1c] = cmd[0x1d] = 0;
		}
		// a remote-controlled missile (flag 0x80000, see pad_frame_player): the pad's right stick steers it through the usercmd's remote control bytes, as the mouse does
		// in 0xcff70 (+0x3c pitch, +0x3d yaw, -127..127); the mouse's value stays when the stick is still and this player also has the keyboard and mouse
		if ((*(unsigned *)(base + RVA_PM_FLAGS) & 0x80000) && pad_dev[pl] >= 0 && p->active && (p->rc_pitch || p->rc_yaw || kb_player() != pl) && CFG_ONCE("padremote", 1)) {
			cmd[0x3c] = (uint8_t)(int8_t)p->rc_pitch;
			cmd[0x3d] = (uint8_t)(int8_t)p->rc_yaw;
		}
	}
	if (CFG_ONCE("cmdlog1", 0) && cmd && native_active && ctx_current == 1) { // QA: player 2's usercmd angles as made, 4 a second (Big Brother's gunner)
		static DWORD at;
		if (GetTickCount() - at >= 250) {
			at = GetTickCount();
			log_printf("native: cmdlog1 time %d buttons %08x angles %d %d %d (short units) remote %d %d pm_flags %08x kb_player %d", *(int *)cmd, *(int *)(cmd + 4), *(int *)(cmd + 8), *(int *)(cmd + 12),
				   *(int *)(cmd + 16), (int)(int8_t)cmd[0x3c], (int)(int8_t)cmd[0x3d], *(unsigned *)(base + RVA_PM_FLAGS), kb_player());
#ifdef _WIN64
			// what CL_CreateCmd (rva 0xd03a0) and CL_MouseMove (0xcfcf0) test before the mouse turns the view: the key catchers (8 with the cgame word 0x4225f4 set: the mouse
			// moves the cgame's cursor 0x4320c8.. in 0xd09d0 instead), frame_msec (0x52fb70; 0: no mouse look), the accumulators, the mouse reads of player 2 since the last line
			const float *acc = (const float *)(base + RVA_ACC_PITCH);
			log_printf("native: cmdlog1 catchers %08x cgword %08x frame_msec %d acc %.2f %.2f mouse reads %u sum %.1f %.1f", *(unsigned *)(base + RVA_KEY_CATCHERS),
				   *(unsigned *)(base + XR(0x422440) + 0x1b4), *(int *)(base + XR(0x52fb70)), (double)acc[0], (double)acc[1], mouse1_reads, (double)mouse1_sum[0], (double)mouse1_sum[1]);
			mouse1_reads = 0;
			mouse1_sum[0] = mouse1_sum[1] = 0;
#endif
		}
	}
	if (inputlog_on() && cmd && native_active && (ctx_current == 0 || ctx_current == 1)) { // the usercmd as it leaves the maker (about 20 a second per context)
		static DWORD cmd_at[2];
		DWORD now_ms = GetTickCount();
		if (now_ms - cmd_at[ctx_current] >= 50) {
			cmd_at[ctx_current] = now_ms;
			inputlog("CMD,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", ctx_current, *(int *)cmd, *(int *)(cmd + 4), *(int *)(cmd + 8), *(int *)(cmd + 12), (int)(int8_t)cmd[0x1a], (int)(int8_t)cmd[0x1b], (int)(int8_t)cmd[0x1c], (int)(int8_t)cmd[0x1d],
				 (int)(int8_t)cmd[0x3c], (int)(int8_t)cmd[0x3d]);
		}
	}
	return cmd;
}

// The simulation step and the hand-over of its results (rva 0x2343a0) run from inside the client frame (called at 0xb8d50): in context 1's frame they would write context 0's data
// into the live (context 1's) client block. They run with context 0 live; the hand-over hooks above put each client's data where its context keeps it.
// (jitter diagnostics) the tracked entity's server origin before and after each simulation step, with the wall time and the context whose frame called it
static int npc_probe_ctx(void)
{
	return ctx_current;
}
static void npc_step_note(const float before[2], int ctx)
{
	if (!CFG_ONCE("npclog", 0) || npc_pick < 0 || npc_goff < 0)
		return;
	static unsigned n;
	if (n++ > 4000)
		return;
	const uint8_t *g = base + RVA_GENTITIES + (size_t)npc_pick * GENT_SIZE;
	LARGE_INTEGER pc, hz;
	QueryPerformanceCounter(&pc);
	QueryPerformanceFrequency(&hz);
	log_printf("native: npcstep ent %d wall %.2f ctx %d before %.2f %.2f after %.2f %.2f", npc_pick, (double)pc.QuadPart * 1000.0 / (double)hz.QuadPart, ctx, (double)before[0], (double)before[1], (double)*(const float *)(g + npc_goff),
		   (double)*(const float *)(g + npc_goff + 4));
}
static void (*server_step_original)(void);
static void aim_targets_update(void);
#ifndef _WIN64
static void *aim_for_client_fn; // set at install: VA 0x55b040, EAX = gentity, [esp+4] = client
#endif
static void svslice_now(void)
{
#ifdef _WIN64
	if (svslice_orig && CFG_ONCE("svslice", 0) == 3)
		svslice_orig();
#endif
}
static void server_step_inner(void);
#ifndef _WIN64
extern volatile DWORD mt_main_tid;
#endif
static void server_step_hook(void)
{
	scrw_diff("step start", 1);
#ifndef _WIN64
	mt_main_tid = GetCurrentThreadId();
#endif
	server_step_inner();
	scrw_diff("step end", 0);
}
static void server_step_inner(void)
{
	hook_counts[2]++;
#ifdef _WIN64
	slice_step_note();
#endif
	int perf_on = CFG_ONCE("perflog", 0);
	LONGLONG t0 = perf_on ? perf_now() : 0;
	float npc_before[2] = {0, 0};
	if (CFG_ONCE("npclog", 0) && npc_pick >= 0 && npc_goff >= 0) {
		const uint8_t *g = base + RVA_GENTITIES + (size_t)npc_pick * GENT_SIZE;
		npc_before[0] = *(const float *)(g + npc_goff);
		npc_before[1] = *(const float *)(g + npc_goff + 4);
	}
	if (ctx_current == 1) {
		ctx_swap_hand(0);
		server_step_original();
		svslice_now();
		if (native_active) {
			if (perf_on) {
				LONGLONG t1 = perf_now();
				aim_targets_update();
				perf_aim += perf_now() - t1;
			} else {
				aim_targets_update();
			}
		}
		ctx_swap_hand(1);
		if (perf_on)
			perf_step += perf_now() - t0;
		npc_step_note(npc_before, 1);
		return;
	}
	server_step_original();
	svslice_now();
	if (native_active) {
		if (perf_on) {
			LONGLONG t1 = perf_now();
			aim_targets_update();
			perf_aim += perf_now() - t1;
		} else {
			aim_targets_update();
		}
	}
	if (perf_on)
		perf_step += perf_now() - t0;
	npc_step_note(npc_before, 0);
}

// The client commands of local client 1. Every action a player's input runs (rva 0xce450) queues the text "n <index>" in the client's reliable command ring (clc: the sequence at +0,
// the number acknowledged at +4, the texts of 0x40 bytes from +8, indexed by number & 0x7f; rva 0xd21a0), and the engine hands the ring to the server in rva 0xd02a0, which calls
// ClientCommand (rva 0x166710) for each text: that is how a script's NotifyOnPlayerCommand hears a player's key. 0xd02a0 only runs for local client 0 here, so the ring of context 1
// was never delivered: player 2's keys never reached a script that waits for a command (Takedown's duck in the opening car: Makarov shot player 2 whatever he pressed, a tester's
// report). Delivered here, with context 0 live, next to his usercmds. [native] p1cmds=0 turns it off.
static void ctx1_deliver_commands(void)
{
	static unsigned fn_rva = ~0u;
	if (fn_rva == ~0u)
		fn_rva = RVA_CLIENT_COMMAND; // (0 in the iw4x-sp profile: tools/gen_addr159.py cannot map it, the delivery is off there)
	uint8_t *clc = ctx_store[1][6];
	if (!clc || !fn_rva || !CFG_ONCE("p1cmds", 1))
		return;
	int *seq = (int *)(clc + CLC_SEQ), *ack = (int *)(clc + CLC_ACK);
	if (!ctx1_clc_synced) { // the ring was copied from context 0's at the init: what was pending there is not player 2's
		*ack = *seq;
		ctx1_clc_synced = 1;
		return;
	}
	int pending = *seq - *ack;
	if (pending < 0 || pending > 0x80) // a ring that was cleared or lapped (a level restart, a reconnect): only what the ring still holds, never a count through the whole range
		*ack = pending < 0 ? *seq : *seq - 0x80;
	while (*ack != *seq) {
		(*ack)++;
		const char *text = (const char *)(clc + CLC_TEXT + (*ack & 0x7f) * 0x40);
		static unsigned logged;
		if (logged++ < 40)
			log_printf("native: client command of local client 1 to the server: '%.40s'", text);
		((void (*)(int, const char *))(base + fn_rva))(cfg("ctx1lc", 1), text);
	}
}

// Player 1 (local client 1) has no input device yet: every simulation step he gets the usercmd of player 0 with the buttons and the movement cleared and the view turned by
// [native] p1yaw (a 16-bit angle, 0x8000 = half a turn), through the engine's own SV_ClientThink analogue (rva 0x22d000, client_t[n] + 0x1630 holds the last cmd).
static int p1_yaw;
static void usercmd_step_inner(int lc)
{
	if (lc == 0) {
		if (inputlog_on() && native_active) { // what the server holds as each client's last usercmd at each simulation step (every 4th)
			static unsigned steps;
			if (++steps % 4 == 0) {
				const uint8_t *c0 = base + RVA_SV_CLIENTS + 0x1630, *c1 = base + RVA_SV_CLIENTS + SV_CLIENT_STRIDE + 0x1630;
				inputlog("STEP,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", steps, ctx_current, *(int *)c0, *(int *)(c0 + 8), *(int *)(c0 + 12), (int)(int8_t)c0[0x1a], *(int *)c1, *(int *)(c1 + 8), *(int *)(c1 + 12),
					 (int)(int8_t)c1[0x1a], (int)(int8_t)c1[0x1b]);
			}
		}
		usercmd_step_original(0);
		return;
	}
	if (!CFG_ONCE("p1cmd", 0))
		return;
	{
		static unsigned seen;
		if (++seen % 10 == 0 && seen < 3000) {
			const uint8_t *c0 = base + RVA_SV_CLIENTS + 0x1630, *c1 = base + RVA_SV_CLIENTS + SV_CLIENT_STRIDE + 0x1630;
			log_printf("native: step %u ctx %d: client 0 cmd t %d yaw %d pitch %d | client 1 cmd t %d yaw %d pitch %d | ctx1 acc yaw %.1f pitch %.1f", seen, ctx_current, *(int *)c0, *(int *)(c0 + 12),
				   *(int *)(c0 + 8), *(int *)c1, *(int *)(c1 + 12), *(int *)(c1 + 8), ctx_store[1][4] ? *(float *)(ctx_store[1][4] + ACC_IN_CLS + 4) : -1.0f,
				   ctx_store[1][4] ? *(float *)(ctx_store[1][4] + ACC_IN_CLS) : -1.0f);
		}
	}
	if (ctx1_ready && !p2_cmd_hold && ctx_current == 0 && ctx_store[1][5]) {
		// context 1's own client frame makes its usercmds in its cl block (ring of 128 x 0x40 at +0, cmdNumber at +0x2000, the number sent at +0x2004): player 1 gets them
		// through the engine's SV_ClientThink analogue, as player 0 gets his through rva 0xd02a0
		uint8_t *cl = ctx_store[1][5];
		ctx1_deliver_commands();
		int number = *(int *)(cl + CL_CMDNUM);
		int *sent = (int *)(cl + CL_SENT);
		if (number - *sent > 32)
			*sent = number - 32;
		for (int n = *sent + 1; n <= number; n++)
			((void (*)(int, void *))(base + RVA_SV_CLIENT_THINK))(1, cl + (n & CL_RING_MASK) * 0x40);
		*sent = number;
		return;
	}
	static int logged;
	uint8_t cmd[0x40];
	memcpy(cmd, base + RVA_SV_CLIENTS + 0x1630, sizeof(cmd)); // player 0's last cmd
	if (!*(int *)cmd) // not set yet
		return;
	static unsigned steps;
	if (++steps % 400 == 0 && steps < 6000) {
		char hex[0x40 * 3 + 1];
		for (int i = 0; i < 0x40; i++)
			snprintf(hex + i * 3, 4, "%02x ", cmd[i]);
		log_printf("native: player 0's cmd: %s", hex);
	}
	*(int *)(cmd + 4) = 0; // no buttons
	cmd[0x1a] = cmd[0x1b] = cmd[0x1c] = cmd[0x1d] = 0; // no movement
	if (!logged) {
		p1_yaw = cfg("p1yaw", 0);
		log_printf("native: player 1 cmd from player 0's: serverTime %d, angles %d %d %d, p1yaw %x", *(int *)cmd, *(int *)(cmd + 8), *(int *)(cmd + 12), *(int *)(cmd + 16), p1_yaw);
		logged = 1;
	}
	*(int *)(cmd + 8) = 0; // a fixed view (pitch 0, yaw p1yaw, roll 0): player 0's mouse must not turn player 1
	*(int *)(cmd + 12) = p1_yaw;
	*(int *)(cmd + 16) = 0;
	((void (*)(int, void *))(base + RVA_SV_CLIENT_THINK))(1, cmd);
}

// These server-side hand-overs can be reached while context 1 is the live one (they run from inside the client frame): they always run with context 0 live.
static void usercmd_step_hook(int lc)
{
	hook_counts[3]++;
	int was = ctx_current;
	if (was)
		ctx_swap_hand(0);
	usercmd_step_inner(lc);
	if (was)
		ctx_swap_hand(was);
}

// Diagnostics ([native] entdraw=1, skipent=<n>): the entities the cgame adds to the scene (rva 0x9c960), logged for one frame of context 1; skipent drops one of them.
static const uint8_t add_entity_bytes[] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20, 0x57, 0x48, 0x81, 0xec, 0x90, 0x00, 0x00, 0x00};
static void (*add_entity_original)(int);
static int skip_entity = 1; // context 1 is player 1: his own entity is not drawn (the cgame hides "my" entity by the local client number, which is 0 in context 1)
static int log_entities;
static int swap_entities = 1;
static int entities_swapped;
void native_unswap_entities(void)
{
	if (!entities_swapped)
		return;
	uint8_t *e0 = base + RVA_CENT, *e1 = e0 + CENT_SIZE, t[CENT_SIZE];
	memcpy(t, e0, sizeof(t));
	memcpy(e0, e1, sizeof(t));
	memcpy(e1, t, sizeof(t));
	entities_swapped = 0;
}
// Lag fix ([native] lagfix=1, default 1): the other local player's body is drawn from the cgame's entity, which comes through the server's snapshot and its
// interpolation (measured ~125 ms behind the player's own view). Both players are on this PC, so his body is turned to his own view yaw of this very moment.
// The default [native] psyaw=1 reads that yaw directly from the owner's playerState. The older accumulator-history / tracked-offset estimate remains available
// as [native] psyaw=0; position extrapolation ([native] lagpos=1) is independent. The entity's yaw fields (floats 10, 42 and 73) are replaced for the draw.
// Context 1's own entity table holds player 2's data in both slots 0 and 1 (swaplog), so player 1's body was never drawn in the bottom half and player 2's own body
// was (from inside, late behind the camera). Context 0's entity 0 is player 1's valid data: it is copied here at the start of context 0's draw and put into slot 1
// for context 1's draw ([native] p0copy=0 turns it off).
static uint8_t p0_ent[CENT_SIZE];
static int p0_ent_ok;
#define LAG_HIST 128
static struct { double t; float acc[2]; } lag_hist[LAG_HIST];
static unsigned lag_hist_n;
static float lag_offset[2];
static int lag_offset_set[2];
static double lag_now(void)
{
	static LONGLONG frequency;
	if (!frequency) {
		LARGE_INTEGER f;
		if (QueryPerformanceFrequency(&f))
			frequency = f.QuadPart;
	}
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return frequency > 0 ? (double)c.QuadPart / (double)frequency : 0.0;
}
static float lag_acc_of(int k)
{
	if (ctx_current == k)
		return *(float *)(base + RVA_ACC_PITCH + 4);
	return ctx_store[k][4] ? *(float *)(ctx_store[k][4] + ACC_IN_CLS + 4) : 0.0f;
}
static float lag_wrap(float d)
{
	while (d > 180.0f)
		d -= 360.0f;
	while (d < -180.0f)
		d += 360.0f;
	return d;
}
static float lag_acc_at(int k, double t)
{
	for (unsigned i = 0; i < LAG_HIST - 1 && i < lag_hist_n; i++) {
		unsigned a = (lag_hist_n - 1 - i) % LAG_HIST, b = (lag_hist_n - 2 - i) % LAG_HIST;
		if (lag_hist_n < 2 + i)
			break;
		if (lag_hist[b].t <= t && t <= lag_hist[a].t) {
			double span = lag_hist[a].t - lag_hist[b].t;
			double w = span > 0 ? (t - lag_hist[b].t) / span : 1.0;
			return lag_hist[b].acc[k] + (float)w * (lag_hist[a].acc[k] - lag_hist[b].acc[k]);
		}
	}
	return lag_hist[(lag_hist_n - 1) % LAG_HIST].acc[k];
}
static void lag_record(void)
{
	double now = lag_now();
	if (lag_hist_n && fabs(lag_hist[(lag_hist_n - 1) % LAG_HIST].t - now) < 0.0005)
		return;
	unsigned at = lag_hist_n % LAG_HIST;
	lag_hist[at].t = now;
	lag_hist[at].acc[0] = lag_acc_of(0);
	lag_hist[at].acc[1] = lag_acc_of(1);
	lag_hist_n++;
}
static int lag_history_needed(void)
{
	static int on = -1, ps_yaw = -1;
	if (!native_active)
		return 0;
	if (on < 0) {
		on = cfg("lagfix", 1);
		ps_yaw = CFG_ONCE("psyaw", 1);
	}
	// Keep the legacy history only for psyaw=0, or while player 2's client block is not ready
	// and the old accumulator path is still needed as a fallback.
	return on && (!ps_yaw || (ctx_current == 0 && !ctx_store[1][5]));
}
// the entity of the OTHER player (index 1 in both contexts: context 1's list has the two traded); k is the player whose body it is.
// Yaw: replaced by the owner's own view yaw of this moment (offset tracked). Position: the interpolated origin (floats 63..65, and its copies 6..8 and 32..34)
// is pushed forward by its velocity over the measured delay (a window of the last ~0.2 s), the Unlagged / CPMA way of drawing a remote player.
#define LAG_POS_N 16
static struct { double t; float p[3]; } lag_pos[2][LAG_POS_N];
static unsigned lag_pos_n[2];
#define LAG_SAVED 15 // 3 yaws, 3 x 3 origins, the extra pushed onto the origins
static void lag_fix_yaw(int n, int k, float saved[LAG_SAVED], int *applied)
{
	*applied = 0;
	static int on = -1, pos_on = -1, ps_yaw = -1;
	if (on < 0) {
		on = cfg("lagfix", 1);
		pos_on = cfg("lagpos", 0); // not verified yet (players did not walk in the tests)
		ps_yaw = CFG_ONCE("psyaw", 1);
	}
	// only while a split screen session runs: the hooks stay in place afterwards and must not touch a normal game's entities (review 2026-10-28)
	if (!on || !native_active || n != (ctx_current == 0 ? 1 : 0))
		return;
	const uint8_t *ps = ctx_current == k ? base + RVA_CL_PS : (ctx_store[k][5] ? ctx_store[k][5] + CL_BLOCK_PS : NULL);
	// psyaw=1 is the default and takes the exact view yaw from playerState. In that mode the accumulator history/offset is only a fallback while player 2's client block is unavailable.
	if (!ps_yaw && lag_hist_n < 8)
		return;
	if (ps_yaw && !ps && lag_hist_n < 8)
		return;
	float *e = (float *)(base + RVA_CENT + (size_t)n * CENT_SIZE);
	const int yidx[3] = {CF_YAW0, CF_YAW1, CF_YAW2}, pidx[3] = {CF_ORG0, CF_ORG1, CF_ORG2};
	double now = 0.0;
	float seen = e[CF_YAW2];
	float fresh;
	if (ps_yaw && ps) {
		fresh = *(const float *)(ps + 0x10c);
	} else {
		now = lag_now();
		float measured = lag_wrap(seen - lag_acc_at(k, now - 0.125));
		if (!lag_offset_set[k]) {
			lag_offset[k] = measured;
			lag_offset_set[k] = 1;
		} else {
			lag_offset[k] += 0.05f * lag_wrap(measured - lag_offset[k]);
		}
		// Legacy route retained for [native] psyaw=0: view accumulator plus the tracked offset to the entity yaw.
		fresh = lag_wrap(lag_acc_of(k) + lag_offset[k]);
	}
	fresh = lag_wrap(fresh + (float)CFG_ONCE("lagtest", 0)); // lagtest: a QA turn to see that the draw uses these fields
	float ex[3] = {0, 0, 0};
	if (pos_on) {
		if (!now)
			now = lag_now();
		unsigned at = lag_pos_n[k] % LAG_POS_N;
		float *lp = &e[63];
		// a jump (teleport, level change) starts the window again
		if (lag_pos_n[k] && (fabsf(lp[0] - lag_pos[k][(lag_pos_n[k] - 1) % LAG_POS_N].p[0]) > 150.0f || fabsf(lp[1] - lag_pos[k][(lag_pos_n[k] - 1) % LAG_POS_N].p[1]) > 150.0f))
			lag_pos_n[k] = at = 0;
		lag_pos[k][at].t = now;
		lag_pos[k][at].p[0] = lp[0];
		lag_pos[k][at].p[1] = lp[1];
		lag_pos[k][at].p[2] = lp[2];
		lag_pos_n[k]++;
		unsigned have = lag_pos_n[k] < LAG_POS_N ? lag_pos_n[k] : LAG_POS_N, old = 0;
		for (unsigned i = 1; i < have; i++) { // the oldest sample not older than 0.25 s
			unsigned idx = (lag_pos_n[k] - 1 - i) % LAG_POS_N;
			if (now - lag_pos[k][idx].t <= 0.25)
				old = i;
		}
		if (old > 0) {
			unsigned oi = (lag_pos_n[k] - 1 - old) % LAG_POS_N;
			double dt = now - lag_pos[k][oi].t;
			if (dt > 0.03) {
				float len = 0;
				for (int i = 0; i < 3; i++) {
					ex[i] = (float)((lp[i] - lag_pos[k][oi].p[i]) / dt * 0.125);
					len += ex[i] * ex[i];
				}
				len = sqrtf(len);
				if (len > 70.0f)
					for (int i = 0; i < 3; i++)
						ex[i] *= 70.0f / len;
			}
		}
	}
	if (CFG_ONCE("laglog", 0)) {
		static unsigned fx;
		if (++fx % 4 == 0 && fx < 3000)
			log_printf("native: lagfix player %d seen %.1f %.1f extra %.1f %.1f yaw seen %.1f fresh %.1f", k, e[63], e[64], ex[0], ex[1], seen, fresh);
	}
	for (int i = 0; i < 3; i++) {
		saved[i] = e[yidx[i]];
		e[yidx[i]] = fresh;
		for (int c = 0; c < 3; c++) {
			saved[3 + i * 3 + c] = e[pidx[i] + c];
			e[pidx[i] + c] += ex[c];
		}
	}
	for (int c = 0; c < 3; c++)
		saved[12 + c] = ex[c];
	*applied = 1;
}
// The bodies are drawn by the cgame's second per-entity pass (rva 0xb0680) and posed between the two passes: a yaw put back after the first pass was never seen
// on a body, which kept its spawn yaw while its camera turned (owner 2026-10-28: "the bottom camera looks left while the body looks straight"). The yaw now stays
// for the whole frame; the next snapshot and the next frame's lerp write the fields again anyway ([native] yawkeep=0 puts it back as before).
static void lag_unfix_yaw(int n, float saved[LAG_SAVED], int applied)
{
	if (!applied)
		return;
	static int keep = -1, posfix = -1;
	if (keep < 0) {
		keep = cfg("yawkeep", 1);
		posfix = cfg("posfix", 1);
	}
	float *e = (float *)(base + RVA_CENT + (size_t)n * CENT_SIZE);
	const int yidx[3] = {CF_YAW0, CF_YAW1, CF_YAW2}, pidx[3] = {CF_ORG0, CF_ORG1, CF_ORG2};
	for (int i = 0; i < 3; i++) {
		if (!keep)
			e[yidx[i]] = saved[i];
		for (int c = 0; c < 3; c++) {
			// Only the extra is taken off: the pose origin (floats 6..8) is what the draw's lerp has just worked out from the snapshot's states. Putting the values from before the draw back froze
			// the body of the other player where it stood at the split: it did not follow its player (owner 2026-11-02, S.S.D.D. and the airport). [native] posfix=0 puts them back as before.
			if (posfix)
				e[pidx[i] + c] -= saved[12 + c];
			else
				e[pidx[i] + c] = saved[3 + i * 3 + c];
		}
	}
}

// "Mine" for the cgame is rva 0xa5ec0 (localClientNum -> the entity/slot number of that local client): with the coop connection table filled (flag byte at rva
// 0x52fb5c) it is the slot of the table (rva 0x540ba0, stride 0x2650, int type at +0x10 == 2 for a local client, its local client number as a ushort at +0x18),
// otherwise the number itself. Context 1 is local client 0 as far as the engine can tell, so it answered "player 0": the cgame's second per-entity pass (rva 0xb0680,
// one call per frame) then drew player 2's OWN entity from the inside (the head at the camera, late behind it; found with Hide() on the server entity) and player 1 counted
// as "mine" and was never drawn. The replacement below is the original function with one change: in context 1 local client 0 is player 2, entity 1 ([native] ownfix=0 off).
#ifdef _WIN64
static const uint8_t slot_bytes[] = {0x80, 0x3d, 0x95, 0x9c, 0x48, 0x00, 0x00, 0x44, 0x8b, 0xc1, 0x75, 0x03, 0x8b, 0xc1, 0xc3}; // cmp [flag],0; mov r8d,ecx; jne +3; mov eax,ecx; ret
static unsigned (*slot_original)(unsigned);
#endif
static unsigned local_slot_hook(unsigned lc)
{
	static int on = -1;
	if (on < 0)
		on = cfg("ownfix", 1);
	if (on && ctx_current == 1 && lc == 0)
		return 1;
	if (!*(const uint8_t *)(base + RVA_COOP_BYTE))
		return lc;
	const uint8_t *q = base + RVA_COOP_SLOTS + 8;
	for (unsigned i = 0;;) {
		if (*(const int *)(q - 8) == 2 && *(const uint16_t *)q == lc)
			return i;
		i++;
		q += SLOT_STRIDE;
		if (SLOT_END_PAST(q))
			return 0;
	}
}

// The game message windows (rva 0x52f348, outside the swapped ranges: one set, local client 0's) are drawn by both contexts (both views of a frame), so their texts came in
// both halves. The HUD's draw of a game message window (rva 0xcb4f0: local client, window, x, y, ...) draws window 2 (console channel
// "subtitle") in the bottom half only, window 0 ("gamenotify obituary", top left) in the top half only ([native] subonce=0 off).
// Like the Xbox 360 game (its subtitles_ss menu is visible for local player 0 only, at y 375 of 480 of that view) the subtitles are in the top half; [native] subtop=0 puts them back in the bottom half.
static const uint8_t msgwin_bytes[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x70};
static void (*msgwin_original)(int, int, int, int, int, int, void *, float, const float *, int, int, int); // x and y come in integer registers (r8d, r9d)
static void msgwin_hook(int lc, int window, int x, int y, int horz, int vert, void *font, float scale, const float *color, int style, int align, int how)
{
	static int on = -1, sub_top = 0;
	if (on < 0) {
		on = cfg("subonce", 1);
		sub_top = cfg("subtop", 1);
	}
	const int once = on && mode == 11 && native_active && split_on && ((window == 2 && ctx_current == (sub_top ? 1 : 0)) || (window == 0 && ctx_current == 1));
	if (CFG_ONCE("msglog", 0) && mode == 11 && split_on) { // QA: how many times each context's draw reaches each message window, and how many of those calls are dropped, every 5 s
		static unsigned calls[2][4], dropped[2][4];
		static DWORD last;
		if (ctx_current >= 0 && ctx_current < 2 && window >= 0 && window < 4) {
			calls[ctx_current][window]++;
			dropped[ctx_current][window] += once;
		}
		if (GetTickCount() - last > 5000) {
			last = GetTickCount();
			log_printf("native: msgwin calls ctx0 [%u %u %u %u] ctx1 [%u %u %u %u], dropped ctx0 [%u %u %u %u] ctx1 [%u %u %u %u]", calls[0][0], calls[0][1], calls[0][2], calls[0][3], calls[1][0], calls[1][1], calls[1][2], calls[1][3],
				   dropped[0][0], dropped[0][1], dropped[0][2], dropped[0][3], dropped[1][0], dropped[1][1], dropped[1][2], dropped[1][3]);
		}
	}
	if (CFG_ONCE("msglog", 0) >= 2 && mode == 11 && split_on && window == 2 && ctx_current == 0) { // QA: where the subtitle window is drawn, once a second
		static DWORD lastp;
		if (GetTickCount() - lastp > 1000) {
			lastp = GetTickCount();
			log_printf("native: subtitle window: x %d y %d horz %d vert %d scale %g style %d align %d how %d", x, y, horz, vert, (double)scale, style, align, how);
		}
	}
	if (once)
		return;
	msgwin_original(lc, window, x, y, horz, vert, font, scale, color, style, align, how);
}

// The names over the other players (rva 0x93ac0, the loop at 0x93d90 over clients 0 and 1): a client is skipped when it is the drawing local client ("CMP EBP,EAX" at 0x93db3).
// Context 1 is not player 2 to that test, so the bottom half had no name over player 1. The 17 bytes from 0x93daa go to a stub that does the same test, but in context 1 the
// client skipped is player 2 (itself): it names player 1 ([native] namefix=0 off).
static const uint8_t name_self_bytes[] = {0x8b, 0xc7, 0x84, 0xc9, 0x74, 0x03, 0x0f, 0xb7, 0x06, 0x3b, 0xe8, 0x0f, 0x84, 0x21, 0x01, 0x00, 0x00};
#ifdef _WIN64
static void name_self_install(void)
{
	uint8_t *site = base + XR(0x93daa);
	if (memcmp(site, name_self_bytes, sizeof(name_self_bytes))) {
		log_printf("native: name loop bytes differ");
		return;
	}
	uint8_t code[64];
	size_t n = 0;
	static const uint8_t head[] = {0x8b, 0xc7, 0x84, 0xc9, 0x74, 0x03, 0x0f, 0xb7, 0x06, 0x52, 0x48, 0xba}; // mov eax,edi; test cl,cl; jz +3; movzx eax,word [rsi]; push rdx; mov rdx,imm64
	memcpy(code + n, head, sizeof(head));
	n += sizeof(head);
	uint64_t at = (uint64_t)(uintptr_t)&ctx_current;
	memcpy(code + n, &at, 8);
	n += 8;
	// cmp dword [rdx],1; pop rdx; jne +5; cmp eax,1 (context 1: the client to skip is player 2); jmp +2; cmp ebp,eax (otherwise the drawing local client); jz +14 (the second jump)
	static const uint8_t mid[] = {0x83, 0x3a, 0x01, 0x5a, 0x75, 0x05, 0x83, 0xf8, 0x01, 0xeb, 0x02, 0x3b, 0xe8, 0x74, 0x0e};
	memcpy(code + n, mid, sizeof(mid));
	n += sizeof(mid);
	hook_jump_bytes(code + n, base + XR(0x93dbb)); // not this client: the name is drawn
	n += 14;
	hook_jump_bytes(code + n, base + XR(0x93edc)); // this client: the next one
	n += 14;
	uint8_t *stub = hook_code(code, n);
	if (!stub) {
		log_printf("native: no page for the name loop stub");
		return;
	}
	jhook_jump(site, sizeof(name_self_bytes), stub, 0x90);
}
#else
// 32 bits (docs/32bit-native-map/inputui.md): the loop is its own function (rva 0x15ea00) and the drawing local client is on the stack at [esp+0x3c]; 19 bytes at rva 0x15ea9e
// (test dl,dl; mov eax,ebp; jz +3; movzx eax,[ecx]; cmp [esp+0x3c],eax; jz next) are replaced by the same test with context 1 skipping client 1 instead (the stub is jumped to, so esp is the loop's)
static void name_self_install(void)
{
	static const uint8_t expected[19] = {0x84, 0xd2, 0x8b, 0xc5, 0x74, 0x03, 0x0f, 0xb7, 0x01, 0x39, 0x44, 0x24, 0x3c, 0x0f, 0x84, 0xe4, 0x00, 0x00, 0x00};
	uint8_t *site = base + XR(0x15ea9e);
	if (memcmp(site, expected, sizeof(expected))) {
		log_printf("native: the name loop's self test is not the expected code");
		return;
	}
	uint8_t code[64];
	size_t n = 0;
	static const uint8_t head[] = {0x84, 0xd2, 0x8b, 0xc5, 0x74, 0x03, 0x0f, 0xb7, 0x01, 0x83, 0x3d}; // ... cmp dword [ctx_current], 1
	memcpy(code + n, head, sizeof(head));
	n += sizeof(head);
	uint32_t at = (uint32_t)(uintptr_t)&ctx_current;
	memcpy(code + n, &at, 4);
	n += 4;
	static const uint8_t mid[] = {0x01, 0x75, 0x05, 0x83, 0xf8, 0x01, 0xeb, 0x04, 0x39, 0x44, 0x24, 0x3c}; // jne +5; cmp eax,1; jmp +4; cmp [esp+0x3c],eax
	memcpy(code + n, mid, sizeof(mid));
	n += sizeof(mid);
	size_t jz_at = n; // jz next (rel32), then jmp to the name drawing
	code[n++] = 0x0f, code[n++] = 0x84, n += 4;
	size_t jmp_at = n;
	code[n++] = 0xe9, n += 4;
	uint8_t *stub = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!stub) {
		log_printf("native: no page for the name loop stub");
		return;
	}
	memcpy(stub, code, n);
	int32_t rel = (int32_t)((uintptr_t)(site + (0x15eb95 - 0x15ea9e)) - (uintptr_t)(stub + jz_at + 6));
	memcpy(stub + jz_at + 2, &rel, 4);
	rel = (int32_t)((uintptr_t)(site + (0x15eab1 - 0x15ea9e)) - (uintptr_t)(stub + jmp_at + 5));
	memcpy(stub + jmp_at + 1, &rel, 4);
	DWORD old;
	VirtualProtect(stub, 0x1000, PAGE_EXECUTE_READ, &old);
	FlushInstructionCache(GetCurrentProcess(), stub, n);
	jhook_jump(site, sizeof(expected), stub, 0x90);
}
#endif

// The HUD element draw (rva 0xa3b80: local client, element; the sort at +0x88): the mission's title lines (maps\_introscreen patch: sort 1.0078125 instead of 1) are laid out
// for the whole screen, and the scripted HUD elements keep the half's own scale (below; [native] titles=0: the hook is not put in).
static const uint8_t hudelem_bytes[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x48, 0x8d, 0xac, 0x24, 0x68, 0xfc, 0xff, 0xff, 0x48, 0x81, 0xec, 0x98, 0x04, 0x00, 0x00};
static void (*hudelem_original)(int, void *);
#ifdef _WIN64
#define TITLE_BYTES 0xc0
#else
#define TITLE_BYTES 0xa8 // (the element is 0xa8 bytes in both builds: mode11-hud.md; 0xc0 read past it)
#endif
static void hudelem_hook(int lc, void *e)
{
#ifdef _WIN64
	if (mode == 11 && split_on && CFG_ONCE("hudelemlog", 0) && GetTickCount() - split_tick > 15000) { // QA: the HUD elements each context draws: shader name (configstring of the index at +0x4c), colour, sort, position
		static unsigned hn, marks, marks_logged;
		if ((hn < 150 || marks_logged < 60) && e && !IsBadReadPtr(e, 0xb0)) {
			char nm[64] = "?";
			const int idx = *(const int *)((const uint8_t *)e + 0x4c);
			((int (*)(int, int, char *, int))(base + XR(0xb0610)))(lc, idx, nm, sizeof(nm));
			const uint32_t *d = (const uint32_t *)e;
			hn++;
			// (the co-op markers' icons, src/ping.c: one line in 40 draws of them)
			const int mark = strstr(nm, "coop_player") || strstr(nm, "waypoint") || !strncmp(nm, "hud_", 4);
			int show = 0;
			if (mark) {
				if (marks++ % 40 == 0 && marks_logged < 60)
					marks_logged++, show = 1;
			} else if (hn < 150 && (!strcmp(nm, "black") || hn < 12))
				show = 1;
			if (show) {
				uint8_t rgba[8] = {0};
				const int tnow = *(const int *)(base + XR(0x51dca8));
				((void (*)(void *, int, void *))(base + XR(0x73eb0)))(e, tnow, rgba); // the colour at the client's time, as the draw reads it
				char all[400] = "";
				for (int k = 0; k < 0x2c; k++)
					snprintf(all + strlen(all), sizeof(all) - strlen(all), "%x ", d[k]);
				log_printf("native: hudelem #%u ctx %d lc %d: shader %.40s idx %d sort %g | time %d colour %02x %02x %02x %02x | dwords %s", hn, ctx_current, lc, nm, idx, (double)*(const float *)((const uint8_t *)e + 0x88), tnow, rgba[0], rgba[1], rgba[2], rgba[3], all);
			}
		}
	}
#endif
	if (mode == 11 && native_active && split_on && CFG_ONCE("exp11titles", 1) && e && !IsBadReadPtr(e, TITLE_BYTES) && *(const float *)((const uint8_t *)e + 0x88) == 1.0078125f) {
		// The title lines (maps\_introscreen patch: sort 1.0078125) are laid out for the whole screen, as in single player, and each half's
		// 2D shows its part: a half's 2D is placed relative to its own viewport, so the whole-screen placement is set for the call and player 2's copy is moved up by half the screen
		// (240 virtual units, whatever the resolution: half the height over a scale of height / 480). One copy across the boundary ([native] exp11titles=0: each half's own title).
		uint8_t tmp[TITLE_BYTES];
		memcpy(tmp, e, sizeof(tmp));
		if (ctx_current == 1) {
			float vr[4];
			exp11_view(1, vr);
			const int sw_t = *(const int *)(base + M11_PLACE_SW), sh_t = *(const int *)(base + M11_PLACE_SH);
			*(float *)(tmp + 8) -= vr[1] * 480.0f; // the virtual height of the screen is 480: the half's origin in it
			if (sw_t > 0 && sh_t > 0 && vr[0] > 0.0f)
				*(float *)(tmp + 4) -= vr[0] * (float)sw_t * 480.0f / (float)sh_t; // (the boxes' layout: his view also starts to the right)
		}
		static unsigned tc;
		if (CFG_ONCE("titleshot", 0) && ctx_current == 0 && ++tc % 80 == 5)
			title_shot_req = (int)tc; // QA: a capture of every 80th frame with title lines
		place_client_set(0, 1);
#ifndef _WIN64
		extern volatile uint8_t snd_mute32;
		if (ctx_current == 1) // the lines' typing sound (ui_pulse_text_type) once: player 1's draw makes it (sndcount: 98 / 98 a second in contexts 0 / 1)
			snd_mute32 = 1;
#endif
		hudelem_original(lc, tmp);
#ifndef _WIN64
		snd_mute32 = 0;
#endif
		place_client_set(ctx_current == 1, 0);
		return;
	}
	// The scripted HUD elements keep the half's own scale ([native] hudelemk=1 gives them hudsplitscale too). The console enlarges only its HUD menus (the *_ss menus flagged
	// LEGACY_SPLIT_SCREEN_SCALE, cg_hudLegacySplitscreenScale 1.5); the scripts that make HUD elements adapt themselves to the split screen (IsSplitscreen(): the AC-130 HUD uses font
	// scale 1.5 instead of 2.5, shorter text blocks, 80 % cross hair), so a 1.5 times bigger placement on top of that cut the AC-130's text and overlapped the timers (owner's screenshots).
	if (mode == 11 && native_active && split_on && !CFG_ONCE("hudelemk", 0)) {
		place_use_k = 0;
		place_client_set(ctx_current == 1, 0);
		hudelem_original(lc, e);
		place_use_k = 1;
		place_client_set(ctx_current == 1, 0);
		return;
	}
	hudelem_original(lc, e);
}

// The name the loop shows comes from rva 0xd3110, which outside the engine's co-op gives the profile's name for every client: player 2 got player 1's name. The call at 0x93e2f
// gets both players' names in the split screen: Alpha and Bravo, the callsigns of the lobby's cards, unless [native] p1name / p2name give others (they used to be the profile's name
// and the profile's name with " (2)" after it: both halves showed the same nickname twice).
static const char *(*name_get_original)(int);
static const char *name_get_hook(int client)
{
	static char p2[64], v[48], p1[64], u[48];
	static unsigned gen, logged;
	const char *own = name_get_original(client);
	if (native_active && client >= 0 && client < 2 && !(logged & (1u << (client * 2 + ctx_current)))) { // once per client and context: whose name each half draws
		logged |= 1u << (client * 2 + ctx_current);
		log_printf("native: name over client %d drawn in context %d", client, ctx_current);
	}
	if (client < 0 || client > 1 || !native_active)
		return own;
	if (client == 1 && ctx_current == 1) // the loop takes client 1 for a remote one when the local player count it reads is 1 (it never reaches the test above): no name over player 2 in his own half
		return "";
	if (gen != cfg_gen) { // read once per session
		GetPrivateProfileStringA("native", "p1name", "", u, sizeof(u), settings_path);
		GetPrivateProfileStringA("native", "p2name", "", v, sizeof(v), settings_path);
		gen = cfg_gen;
	}
	char *out = client ? p2 : p1;
	snprintf(out, 64, "%s", (client ? v : u)[0] ? (client ? v : u) : (client ? "Bravo" : "Alpha"));
	return out;
}

// The cgame's second per-entity pass (rva 0xb0680) draws every entity that is not "mine". In context 1, with ownfix, the other player is entity 0, whose slot holds a copy
// of player 2's own data there (it is filled from the player state): for the call it gets player 1's entity as context 0 holds it.
// Player 1's entity data for context 1's slot 0: origin and angles of the pose (+0x18..0x30) and the entity states (+0x60..0x170, and +0x178 on) from context 0,
// while the slot keeps its own pose bookkeeping (local client number, lighting and ragdoll handles: +0..0x18, +0x30..0x60) and its own animation tree pointer
// (+0x170): a whole copy took context 0's "mine" bookkeeping along and the body was drawn as a stretched mesh ([native] p0full=1 copies everything as before).
// The tail of a centity (from the valid flag on: interpolate, currentValid, the previous event sequence, miscTime, the lighting origin) is the cgame's bookkeeping between frames. The
// copy of player 1's entity is put into context 1's slot 0 for the draw and the slot is put back afterwards, so what the draw wrote there (the events it has handled) was lost and the
// next frame played the last event again, for ever: the muzzle flash of a player who had fired stayed on his gun in the other half. The tail the draw left is kept and used by the
// next copy ([native] p0tail=0 off).
#define P0_TAIL CENT_VALID
#define P0_TAIL_LEN (CENT_SIZE - CENT_VALID)
static uint8_t p0_tail[P0_TAIL_LEN];
static int p0_tail_ok, p0_tail_on = -1; // (read when the session starts, not on the first call in the middle of the first split frame)
static void p0_tail_take(const uint8_t *slot)
{
	if (p0_tail_on < 0)
		p0_tail_on = cfg("p0tail", 1);
	if (p0_tail_on) {
		memcpy(p0_tail, slot + P0_TAIL, P0_TAIL_LEN);
		p0_tail_ok = 1;
	}
}
static void p0_put(uint8_t *slot)
{
	// [native] p0keep: bit 0 keeps the slot's own +0..0x18 (pose header), bit 1 its +0x30..0x60 (rest of the pose), bit 2 its +0x170..0x178 (animation tree
	// pointer); bit 3 clears the per-client hide bits (0x30) of the copied eFlags (+0xec): context 0's own entity has them, the second pass needs them clear.
	static int keep = -1;
	if (keep < 0)
		keep = cfg("p0keep", 8 | 1);
	uint8_t saved[CENT_SIZE];
	memcpy(saved, slot, sizeof(saved));
	memcpy(slot, p0_ent, sizeof(p0_ent));
	if (p0_tail_ok && p0_tail_on > 0)
		memcpy(slot + P0_TAIL, p0_tail, P0_TAIL_LEN);
	if (keep & 1)
		memcpy(slot, saved, P0_HDR);
	if (keep & 2)
		memcpy(slot + P0_REST, saved + P0_REST, P0_REST_LEN);
	if (keep & 4)
		memcpy(slot + P0_TREE, saved + P0_TREE, 8);
	if (keep & 8)
		*(uint32_t *)(slot + CENT_FLAGS) &= ~0x30u;
	// player 1's yaw as he looks now (his playerState in context 0's block), in the pose and in both entity states: the copy is put back into the slot several
	// times a frame (own_write_hook, the passes), and each time it carried the yaw the entity states froze at ([native] yawkeep=0 off)
	static int yaw_on = -1;
	if (yaw_on < 0)
		yaw_on = cfg("yawkeep", 1) && cfg("psyaw", 1);
	if (yaw_on && ctx_store[0][5]) {
		float y = *(const float *)(ctx_store[0][5] + CL_BLOCK_PS + 0x10c);
		float *f = (float *)slot;
		f[CF_YAW0] = f[CF_YAW1] = f[CF_YAW2] = y;
	}
	// ... and his pitch (the float before each yaw): the body's torso aims by it. Without it the body of player 1 in player 2's half held his rifle low whatever he looked at
	// (aiming at player 2's head, the muzzle was well below it) ([native] pitchkeep=0 off)
	// Off by default (night 2026-11-17, GitHub issue #25: "depending on where player 1 looks, the entire body rotates", S.S.D.D., split screen): these angles turn the
	// whole model, so player 1 looking down folded his body to the floor in player 2's half, as p0aim's pitch did in mode 11 (b7ae6cd). The Xbox 360's own-entity write
	// sets the yaw only (0x82102f28). [native] pitchkeep=1 puts the pitch back.
	static int pitch_on = -1;
	if (pitch_on < 0)
		pitch_on = cfg("pitchkeep", 0);
	if (pitch_on && ctx_store[0][5]) {
		float pt = *(const float *)(ctx_store[0][5] + CL_BLOCK_PS + 0x108);
		float *f = (float *)slot;
		f[CF_YAW0 - 1] = f[CF_YAW1 - 1] = f[CF_YAW2 - 1] = pt;
	}
}

#ifdef _WIN64
static void (*pass2_original)(unsigned);
#else
// 32 bits: the second pass (rva 0x202d0) lies in the engine's checked code, so it is replaced as a whole from its call in the cgame draw: the two player entities that are not "mine"
// are drawn by the entity function 0x569ba0 (centity in ESI, local client on the stack), then 0x438f80(lc, centity) when the dword at 0x85e8cc is set. Its "mine" test is local_slot_hook.
static unsigned local_slot_hook(unsigned lc);
static void pass2_original(unsigned lc)
{
	for (int i = 0; i < 2; i++) {
		uint8_t *e = base + RVA_CENT + (size_t)i * CENT_SIZE;
		if (local_slot_hook(lc) == (unsigned)i || !(*(uint32_t *)(e + CENT_VALID) & 1))
			continue;
		uint8_t *esi = e;
		__asm__ volatile("pushl %1\n\tcall *%2\n\taddl $4, %%esp" : "+S"(esi) : "r"(lc), "r"(base + XR(0x169ba0)) : "eax", "ecx", "edx", "memory", "cc");
		if (*(int *)(base + XR(0x45e8cc)))
			((void (*)(unsigned, void *))(base + XR(0x38f80)))(lc, e);
	}
}
#endif
static const uint8_t pass2_bytes[] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x68, 0x18, 0x56, 0x57, 0x41, 0x57, 0x48, 0x81, 0xec, 0xa0, 0x00, 0x00, 0x00};
// The world yaw a context's camera looks along: its refdef's view axis (forward vector right after the view origin, rva 0x42d310), from the store when that context is not live.
// The playerState's view yaw is relative to what the player is linked to (a helicopter's seat, an elevator): as a body's yaw it turned the body round (owner, Gulag fly-in:
// Alpha's body faced the back of the helicopter).
static float ctx_camera_yaw(int ctx, int *ok)
{
	const uint8_t *blk = ctx == ctx_current ? base + RANGE_RVA(0) : ctx_store[ctx][0];
	*ok = 0;
	if (!blk)
		return 0.0f;
	const float *fwd = (const float *)(blk + (M11_CG_VIEWORG - RANGE_RVA(0)) + 0xc);
	if (fabsf(fwd[0]) < 1e-4f && fabsf(fwd[1]) < 1e-4f)
		return 0.0f;
	*ok = 1;
	return atan2f(fwd[1], fwd[0]) * (180.0f / 3.14159265f);
}
static void pass2_hook(unsigned lc)
{
	if (ctx_current == 1 && CFG_ONCE("p2skip", 0)) // QA: context 1 draws no other-player body (is a stray mesh player 1's body?)
		return;
	// The other player's body in player 2's view (centshare: slot 0, written by context 0): its yaw and pitch (the torso's and head's aim) come from the entity state, which the
	// cgame refreshes only when a snapshot arrives (20 a second), so the partner's aim moved in steps. Player 1's playerState (his client block, in the context-0 store) is
	// current every frame: his yaw and pitch go into the slot's three yaws and pitches for the pass, as the earlier copy did ([native] p0aim=0 off).
	// The source is player 1's cgame playerState (cg.predictedPlayerState, rva 0x422440, context 0's copy in the store): the cgame builds it every frame. The client block's playerState
	// (CL_BLOCK_PS) is the last snapshot's, 20 a second: with it his body in the bottom half turned in steps of 6-8 draws (owner: "the other player's model runs at 10-15 fps";
	// measured with animlog: the pose yaw changed in 1 of 7 draws). The 360 does the same per frame: its own-entity write (0x82102f28) takes each local player's interpolated state.
	// ([native] p0aimsrc=0: the snapshot's, as before)
	if (ctx_current == 1 && mode == 11 && cent_share_on() && ctx_store[0][0] && ctx_store[0][5] && CFG_ONCE("p0aim", 1)) {
		float *f = (float *)(base + RVA_CENT);
		const uint8_t *src = CFG_ONCE("p0aimsrc", 1) ? ctx_store[0][0] + (RVA_CG_PS - RANGE_RVA(0)) : ctx_store[0][5] + CL_BLOCK_PS;
		float y = *(const float *)(src + 0x10c);
		const float pt = *(const float *)(src + 0x108);
		int cam_ok = 0;
		const float cy = ctx_camera_yaw(0, &cam_ok);
		if (cam_ok && CFG_ONCE("bodycamyaw", 1)) // the world direction of his camera ([native] bodycamyaw=0: the playerState's view yaw)
			y = cy;
		f[CF_YAW0] = f[CF_YAW1] = f[CF_YAW2] = y;
		// Only the yaw: these angles turn the whole model, and with the view's pitch in them the body leaned over as far as the player looked up or down (owner's screenshots:
		// Alpha folded to the floor, thrown back with the gun up, Airport). The 360's own-entity write also sets yaw only (pitch and roll 0, 0x82102f28); the torso's aim comes from
		// the entity state as the game draws it. [native] p0aimpitch=1 writes the pitch as before.
		if (CFG_ONCE("p0aimpitch", 0))
			f[CF_YAW0 - 1] = f[CF_YAW1 - 1] = f[CF_YAW2 - 1] = pt;
	}
	// The other player's body rides a moving vehicle in steps (owner: "only when the helicopter is moving", the Gulag fly-in; the Airport elevator): his slot's position is his
	// context's own-entity write, which takes the last snapshot's playerState (20 a second): measured on the Gulag, his body's pose stood still for 6-7 draws, then moved 80 units,
	// while both cameras moved 11-13 units every draw. The 360's own-entity write (0x82102f28) puts each local player's position interpolated between the snapshots, every frame.
	// The pass now gets the other player's cgame playerState origin (cg.predictedPlayerState +0x1c, built every frame, the one his camera stands on) in the pose and in both states
	// of the slot, in both views ([native] bodylerp=0 off).
	if (mode == 11 && cent_share_on() && CFG_ONCE("bodylerp", 1) && (ctx_current == 0 || ctx_current == 1) && ctx_store[1 - ctx_current][0]) {
		const int other = 1 - ctx_current;
		uint8_t *slot = base + RVA_CENT + (size_t)other * CENT_SIZE;
		// The game's position of his body (his own-entity write, the snapshot's while he rides something) is carried on between two snapshots at the speed of the last two:
		// no lag behind what he rides and no tie to his camera (the camera's own sway shook both bodies, owner 2026-11-16). On foot the position changes every frame and this
		// leaves it as it is. A jump of more than 300 units (a teleport) starts over; past 150 ms without a new position it holds.
		static float last_go[2][3], vel[2][3], written[2][3], err[2][3];
		const float *slot_o = (const float *)(slot + CF_ORG0 * 4);
		// (what is there may be our own write of the last frame, when nothing wrote the slot since: then the game's position is the one we had)
		const float *go = memcmp(slot_o, written[other], 12) == 0 ? last_go[other] : slot_o;
		static double last_t[2];
		LARGE_INTEGER qc, qf;
		QueryPerformanceCounter(&qc);
		QueryPerformanceFrequency(&qf);
		const double now = (double)qc.QuadPart / (double)qf.QuadPart;
		if (go[0] != last_go[other][0] || go[1] != last_go[other][1] || go[2] != last_go[other][2]) {
			const double dt = now - last_t[other];
			const float d0 = go[0] - last_go[other][0], d1 = go[1] - last_go[other][1], d2 = go[2] - last_go[other][2];
			if (last_t[other] > 0 && dt > 0.003 && dt < 0.2 && d0 * d0 + d1 * d1 + d2 * d2 < 300.0f * 300.0f) {
				// the speed averaged over the last changes (each change's own time is uneven), and what we drew against the new position kept as an error that fades over a few
				// draws: no jump of the body when a snapshot comes in
				// (measured over at least 40 ms back to an anchor: the game's position moves by one big step and a couple of small ones per snapshot)
				static float anchor[2][3];
				static double anchor_t[2];
				const double span = now - anchor_t[other];
				if (anchor_t[other] > 0 && span >= 0.04 && span < 0.3) {
					const float a = 0.5f;
					for (int k = 0; k < 3; k++)
						vel[other][k] += a * ((go[k] - anchor[other][k]) / (float)span - vel[other][k]);
				}
				if (anchor_t[other] <= 0 || span >= 0.04) {
					memcpy(anchor[other], go, 12);
					anchor_t[other] = now;
				}
				for (int k = 0; k < 3; k++)
					err[other][k] = written[other][k] - go[k];
				if (err[other][0] * err[other][0] + err[other][1] * err[other][1] + err[other][2] * err[other][2] > 64.0f * 64.0f)
					err[other][0] = err[other][1] = err[other][2] = 0.0f;
			} else {
				vel[other][0] = vel[other][1] = vel[other][2] = 0.0f;
				err[other][0] = err[other][1] = err[other][2] = 0.0f;
			}
			memcpy(last_go[other], go, 12);
			last_t[other] = now;
		}
		double ahead = now - last_t[other];
		if (ahead > 0.15)
			ahead = 0.15;
		for (int k = 0; k < 3; k++)
			err[other][k] *= 0.6f; // (per draw of this view)
		float po[3] = {go[0] + vel[other][0] * (float)ahead + err[other][0], go[1] + vel[other][1] * (float)ahead + err[other][1], go[2] + vel[other][2] * (float)ahead + err[other][2]};
		// Riding a vehicle the scripts name (dvar mw2coop_ride: its entity number, set while the players sit in it): the body is held to it as the level's men are (owner: "the
		// way he is fixed to the helicopter", MacTavish beside the players): the game's position of the body moved by what the vehicle has moved since the same snapshot, i.e. the
		// vehicle's drawn position minus its current state's (both of the snapshot the body's position is of). No extrapolation then.
		{
			char rv[16] = "";
			const int ride = game_dvar_raw("mw2coop_ride", rv, sizeof(rv)) && rv[0] ? atoi(rv) : -1;
			if (ride >= 2 && ride < 2048 && CFG_ONCE("ridefix", 0)) { // (off: measured worse than the carrying-on above, sd 11.5 against 3.4 units a draw; the vehicle state's yaw read is not right yet)
				// He sits still in it, so his place in the vehicle's own frame hardly changes: that place (from the game's position against the vehicle's state of the same
				// snapshot, turned by the state's yaw) is averaged, and the body is drawn there from the vehicle's drawn pose (origin +0x18, yaw +0x28), every draw: rigid with it.
				const uint8_t *v = base + RVA_CENT + (size_t)ride * CENT_SIZE;
				const float *vp = (const float *)(v + 0x18), *vs = (const float *)(v + 0x80);
				const float pyaw = *(const float *)(v + 0x28) * (3.14159265f / 180.0f), syaw = *(const float *)(v + 0xa8) * (3.14159265f / 180.0f);
				const float wx = go[0] - vs[0], wy = go[1] - vs[1], wz = go[2] - vs[2];
				static float loc[2][3];
				static int have_loc[2], loc_ride[2];
				if (wx * wx + wy * wy + wz * wz < 400.0f * 400.0f) {
					const float lx = cosf(syaw) * wx + sinf(syaw) * wy, ly = -sinf(syaw) * wx + cosf(syaw) * wy; // into the vehicle's frame
					if (!have_loc[other] || loc_ride[other] != ride) {
						loc[other][0] = lx, loc[other][1] = ly, loc[other][2] = wz;
						have_loc[other] = 1, loc_ride[other] = ride;
					} else {
						const float a = 0.08f;
						loc[other][0] += a * (lx - loc[other][0]), loc[other][1] += a * (ly - loc[other][1]), loc[other][2] += a * (wz - loc[other][2]);
					}
					po[0] = vp[0] + cosf(pyaw) * loc[other][0] - sinf(pyaw) * loc[other][1];
					po[1] = vp[1] + sinf(pyaw) * loc[other][0] + cosf(pyaw) * loc[other][1];
					po[2] = vp[2] + loc[other][2];
					err[other][0] = err[other][1] = err[other][2] = 0.0f;
				}
			}
		}
		// [native] bodysrc=1 (default; owner, 2026-11-16: "put the players' seating back as it was"): his cgame playerState origin (cg.predictedPlayerState +0x1c, every frame: in a
		// helicopter the point of his seat); 2: the game's position carried on as above
		if (CFG_ONCE("bodysrc", 1) == 1) {
			const float *pso = (const float *)(ctx_store[other][0] + (RVA_CG_PS - RANGE_RVA(0)) + 0x1c);
			if (pso[0] != 0.0f || pso[1] != 0.0f)
				po[0] = pso[0], po[1] = pso[1], po[2] = pso[2];
			ahead = 1.0; // (written)
		}
		if (ahead > 0.0) {
			static const int offs[3] = {CF_ORG0 * 4, CF_ORG1 * 4, CF_ORG2 * 4}; // the pose's origin, the current and the next state's origin
			for (int k = 0; k < 3; k++)
				memcpy(slot + offs[k], po, 12);
			memcpy(written[other], po, 12);
		}
		if (CFG_ONCE("bodylog", 0) == 2 && split_tick && GetTickCount() - split_tick > (DWORD)CFG_ONCE("bodylogat", 40) * 1000) { // QA: the body's position in each view, every draw (60 per view)
			static unsigned bn[2];
			if (bn[ctx_current] < 60) {
				bn[ctx_current]++;
				const float *q = (const float *)(slot + CF_ORG0 * 4);
				log_printf("native: bodypos ctx %d %.2f %.2f %.2f (game %.2f %.2f, ahead %.0f ms)", ctx_current, (double)q[0], (double)q[1], (double)q[2], (double)go[0], (double)go[1], ahead * 1000.0);
			}
		}
		if (CFG_ONCE("bodylog", 0)) { // QA: the other player's view yaw against his camera's world yaw, and the position put in
			static unsigned bl;
			int okc = 0;
			const float cyw = ctx_camera_yaw(other, &okc);
			if (++bl % 150 == 0 && bl < 6000)
				log_printf("native: bodylog ctx %d: other's ps yaw %.1f, camera yaw %.1f (%d), origin %.1f %.1f %.1f", ctx_current, (double)*(const float *)(ctx_store[other][0] + (RVA_CG_PS - RANGE_RVA(0)) + 0x10c), (double)cyw, okc, (double)po[0], (double)po[1], (double)po[2]);
		}
		if (ctx_current == 0 && CFG_ONCE("p0aim", 1)) { // and his yaw, as p0aim does for player 1's body in context 1
			float *f = (float *)slot;
			int cam_ok = 0;
			const float cy = ctx_camera_yaw(1, &cam_ok);
			f[CF_YAW0] = f[CF_YAW1] = f[CF_YAW2] = cam_ok && CFG_ONCE("bodycamyaw", 1) ? cy : *(const float *)(ctx_store[1][0] + (RVA_CG_PS - RANGE_RVA(0)) + 0x10c);
		}
	}
	if (CFG_ONCE("p2hooklog", 0)) { // QA: what the second entity pass sees in both slots
		static unsigned k;
		if (++k % 60 == 0 && k < 6000) {
			const float *s0 = (const float *)(base + RVA_CENT + 0x18), *s1 = (const float *)(base + RVA_CENT + CENT_SIZE + 0x18);
			log_printf("native: p2hook ctx %d lc %u ok %d | slot0 pose %.1f %.1f num %u | slot1 pose %.1f %.1f num %u | mine %u", ctx_current, lc, p0_ent_ok, (double)s0[0], (double)s0[1],
				   *(const uint16_t *)(base + RVA_CENT + 0x15e), (double)s1[0], (double)s1[1], *(const uint16_t *)(base + RVA_CENT + CENT_SIZE + 0x15e), local_slot_hook(lc));
		}
	}
	{ // [native] timelog2=1: each context's cgame time (rva 0x422434) at its entity pass, the two side by side every 120 frames and whenever they are more than 50 ms apart
		static int tl = -1;
		static int last_t[2];
		static unsigned n, logged;
		if (tl < 0)
			tl = cfg("timelog2", 0);
		if (tl && (ctx_current == 0 || ctx_current == 1)) {
			int t = *(const int *)(base + XR64(0x422434));
			int was = last_t[ctx_current];
			last_t[ctx_current] = t;
			if (ctx_current == 1 && last_t[0] && abs(t - last_t[0]) > 50 && logged < 200)
				logged++, log_printf("native: entity pass time ctx1 %d ctx0 %d (diff %d)", t, last_t[0], t - last_t[0]);
			if (was && t < was && logged < 200)
				logged++, log_printf("native: entity pass time of ctx %d went back %d -> %d", ctx_current, was, t);
			if (++n % 120 == 0)
				log_printf("native: entity pass times ctx0 %d ctx1 %d", last_t[0], last_t[1]);
		}
	}
	static int on = -1;
	if (on < 0)
		on = p0copy_on() && cfg("ownfix", 1);
	if (on && ctx_current == 1 && p0_ent_ok) {
		uint8_t saved[CENT_SIZE];
		memcpy(saved, base + RVA_CENT, sizeof(saved));
		p0_put(base + RVA_CENT);
		if (CFG_ONCE("p2hooklog", 0)) {
			static unsigned k2;
			if (++k2 % 60 == 0 && k2 < 6000)
				log_printf("native: p2hook after put: slot0 pose %.1f %.1f | p0_ent pose %.1f %.1f", (double)((const float *)(base + RVA_CENT + 0x18))[0], (double)((const float *)(base + RVA_CENT + 0x18))[1],
					   (double)((const float *)(p0_ent + 0x18))[0], (double)((const float *)(p0_ent + 0x18))[1]);
		}
		pass2_original(lc);
		p0_tail_take(base + RVA_CENT);
		memcpy(base + RVA_CENT, saved, sizeof(saved));
		return;
	}
	pass2_original(lc);
}

#ifdef _WIN64
// QA ([native] animleaf=1, animlogat=<s>): the partner's body as player 2 sees it (owner 2026-11-21: "his idle animation loses frames, in movement it jitters like the
// airport's terrorists did"). The animation tree of a player body's client DObj (entity 0 or 1), every node reachable from the root (first child +0xc, next sibling +0xa,
// nodes of 0x40 bytes at rva 0x1bc0a30): its index, the u16 links and the floats from +0x14 (the state: time, weights, rate), at points of a frame: after player 1's
// cgame step ("s0"), after player 2's ("s1"), and when a view adds the body ("add"). 400 frames from animlogat seconds after the split. One line per point.
static void animleaf_log(const char *where, int ent, const uint8_t *dobj)
{
	static unsigned lines;
	if (!CFG_ONCE("animleaf", 0) || lines >= 9000 || !split_tick || GetTickCount() - split_tick < (DWORD)CFG_ONCE("animlogat", 30) * 1000)
		return;
	if (!dobj)
		dobj = ((const uint8_t *(*)(int))(base + XR64(0x1fece0)))(ent);
	if (!dobj || IsBadReadPtr(dobj, 0xb0))
		return;
	const uint8_t *tree = *(const uint8_t *const *)dobj;
	if (!tree || IsBadReadPtr(tree, 16))
		return;
	lines++;
	char line[3000];
	int len = snprintf(line, sizeof(line), "native: animleaf %s ent %d ctx %d t=%.2f cg.time %d dobj %p:", where, ent, ctx_current, lag_now() * 1000.0, *(const int *)(base + XR64(0x422434)), (const void *)dobj);
	int stack[32], sp = 0, seen = 0;
	const int root = *(const short *)(tree + 8);
	if (root > 0)
		stack[sp++] = root;
	while (sp > 0 && seen < 24 && len < (int)sizeof(line) - 140) {
		const int idx = stack[--sp];
		if (idx <= 0 || idx >= 0x8000)
			continue;
		seen++;
		const uint8_t *node = base + XR64(0x1bc0a30) + (size_t)idx * 0x40;
		const uint16_t *u = (const uint16_t *)node;
		const float *f = (const float *)(node + 0x14);
		len += snprintf(line + len, sizeof(line) - len, " [%d p%u n%u c%u a%u | %.4f %.4f %.4f %.4f %.4f %.4f]", idx, u[7], u[5], u[6], u[8], (double)f[0], (double)f[1], (double)f[3], (double)f[4], (double)f[5], (double)f[6]);
		if (u[5] && sp < 32)
			stack[sp++] = u[5];
		if (u[6] && sp < 32)
			stack[sp++] = u[6];
		static int watched;
		if (!u[6] && !watched && ent == 0 && CFG_ONCE("animwatch", 0) && !strcmp(where, "add")) { // [native] animwatch=1: who writes this leaf's time (+0x20), with the live context
			watched = 1;
			control_set_probe(npc_probe_ctx);
			control_watch_write(XR64(0x1bc0a30) + (uintptr_t)idx * 0x40 + 0x20, 4);
			log_printf("native: animwatch: watching node %d's time of entity 0", idx);
		}
	}
	log_printf("%s", line);
}
#else
static void animleaf_log(const char *where, int ent, const uint8_t *dobj) { (void)where, (void)ent, (void)dobj; }
#endif
// Diagnostics ([native] dobjlog=1): the scene's "add a DObj" function (rva 0x2bb0: DObj in rcx, origin pointer in rdx, entity number as a ushort in r8, flags in r9) logged
// for context 1's frames, to see which entity a stray model comes from and where it is placed.
static void *(*dobj_add_original)(void *, void *, unsigned, unsigned, void *, void *, void *, void *);
static const uint8_t dobj_add_bytes[] = {0x40, 0x55, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57, 0x48, 0x83, 0xec, 0x40, 0xf3, 0x0f, 0x10, 0x8c, 0x24, 0x98, 0x00, 0x00, 0x00};
static void *dobj_add_hook(void *a, void *b, unsigned c, unsigned d, void *e, void *f, void *g, void *h)
{
	static int on = -1;
	if (on < 0)
		on = cfg("dobjlog", 0);
	if (CFG_ONCE("bodyadds", 0) && (ctx_current == 0 || ctx_current == 1) && (c & 0xffff) < 2) { // QA ([native] bodyadds=1): which context adds which player body, and with which DObj
		static LONG adds[2][2];
		static void *dobjs[2][2];
		static DWORD at;
		InterlockedIncrement(&adds[ctx_current][c & 1]);
		dobjs[ctx_current][c & 1] = a;
		if (GetTickCount() - at >= 2000) {
			at = GetTickCount();
			log_printf("native: bodyadds 2 s: ctx0 adds ent0 %ld (dobj %p) ent1 %ld (dobj %p) | ctx1 adds ent0 %ld (dobj %p) ent1 %ld (dobj %p) | flags %x", adds[0][0], dobjs[0][0], adds[0][1], dobjs[0][1],
				   adds[1][0], dobjs[1][0], adds[1][1], dobjs[1][1], d);
			memset(adds, 0, sizeof(adds));
		}
	}
	if (a && CFG_ONCE("dobjmodels", 0) && ((ctx_current == 1 && (c & 0xffff) == 0) || (ctx_current == 0 && (c & 0xffff) == 1)) && !IsBadReadPtr(a, 0xb0)) { // QA: the models of a body's DObj (names), twice a second
		static DWORD at[2];
		if (GetTickCount() - at[ctx_current] >= 500) {
			at[ctx_current] = GetTickCount();
			const uint8_t *dobj = (const uint8_t *)a;
			int n = dobj[0xd];
			const uint8_t *const *models = *(const uint8_t *const **)(dobj + 0xa8);
			char line[600];
			int len = snprintf(line, sizeof(line), "native: dobjmodels ctx %d ent %u dobj %p models %d:", ctx_current, c & 0xffff, a, n);
			for (int i = 0; models && i < n && i < 8 && len < (int)sizeof(line) - 80; i++) {
				const char *name = models[i] && !IsBadReadPtr(models[i], 8) && !IsBadReadPtr(*(const char *const *)models[i], 4) ? *(const char *const *)models[i] : "?";
				len += snprintf(line + len, sizeof(line) - len, " %s", name);
			}
			log_printf("%s", line);
			for (int h = 0; h < 2; h++) { // the engine's own DObj of entity h, and the slot's client number the draw looked it up by
				const uint8_t *pool = ((const uint8_t *(*)(int))(base + XR64(0x1fece0)))(h);
				int nm = pool ? pool[0xd] : -1;
				const uint8_t *const *mm = pool ? *(const uint8_t *const **)(pool + 0xa8) : NULL;
				char names[300] = "";
				for (int i = 0; mm && i < nm && i < 6 && strlen(names) < 200; i++) {
					const char *nn = mm[i] && !IsBadReadPtr(mm[i], 8) && !IsBadReadPtr(*(const char *const *)mm[i], 4) ? *(const char *const *)mm[i] : "?";
					snprintf(names + strlen(names), sizeof(names) - strlen(names), " %s", nn);
				}
				log_printf("native: dobjmodels   pool[%d] %p models %d:%s | slot0 clientnum %u slot1 clientnum %u", h, (const void *)pool, nm, names,
					   (unsigned)*(const uint16_t *)(base + RVA_CENT + 0x15e), (unsigned)*(const uint16_t *)(base + RVA_CENT + CENT_SIZE + 0x15e));
			}
		}
	}
	if (CFG_ONCE("speedlog", 0) && e && !IsBadReadPtr(e, 12) && (ctx_current == 0 || ctx_current == 1)) { // QA (scenario speed): the drawn path length of the ally the script follows, per context, per second
		static int ent = -1;
		static float last[2][3], path[2];
		static int have[2];
		static DWORD at;
		const float *o = (const float *)e;
		if ((int)(c & 0xffff) == ent) {
			if (have[ctx_current]) {
				float dx = o[0] - last[ctx_current][0], dy = o[1] - last[ctx_current][1], dz = o[2] - last[ctx_current][2], dd = sqrtf(dx * dx + dy * dy + dz * dz);
				if (dd < 100.0f)
					path[ctx_current] += dd;
			}
			memcpy(last[ctx_current], o, sizeof(last[0]));
			have[ctx_current] = 1;
		}
		if (GetTickCount() - at >= 1000) {
			char v[64] = "";
			int n = 0, script_len = 0;
			float rate = 1;
			if (game_dvar_raw("mw2coop_qa_s", v, sizeof(v)))
				sscanf(v, "%d %d %f", &n, &script_len, &rate);
			if (at && ent >= 0)
			{
				const uint8_t *cd = ((const uint8_t *(*)(int))(base + XR64(0x1fece0)))(ent), *sd = ((const uint8_t *(*)(int))(base + XR64(0x1fee30)))(ent);
				int pool = 0;
				for (int k = 0; k < 0x70; k++)
					pool += base[XR64(0xf50ec0) + k] != 0;
				log_printf("native: speed entity %d: drawn path ctx0 %.0f ctx1 %.0f units per second, script %d units per second (playback rate %.2f), client DObj %p (%d models), server DObj %p (%d models), actor DObj pool %d of 112 in use, flags150 %08x",
					   ent, path[0], path[1], script_len, rate, (const void *)cd, cd ? cd[0xd] : -1, (const void *)sd, sd ? sd[0xd] : -1, pool,
					   ent >= 0 && ent < 2048 ? *(const unsigned *)(base + XR64(0xd9e070) + (size_t)ent * 0x2a0 + 0x150) : 0u);
			}
			ent = n;
			path[0] = path[1] = 0;
			have[0] = have[1] = 0;
			at = GetTickCount();
		}
	}
	if (a && CFG_ONCE("treelog", 0) && (c & 0xffff) >= 2 && (c & 0xffff) < 2048 && ctx_current == 0 && !IsBadReadPtr(a, 0xb0)) { // QA: the animation tree nodes of Price, ten times a second (split or plain game)
		const uint8_t *dobj = (const uint8_t *)a;
		int nm = dobj[0xd];
		const uint8_t *const *models = *(const uint8_t *const **)(dobj + 0xa8);
		const char *name = nm > 0 && models && models[0] && !IsBadReadPtr(models[0], 8) && !IsBadReadPtr(*(const char *const *)models[0], 4) ? *(const char *const *)models[0] : "";
		static DWORD at;
		static int ent = -1;
		static float first[2048][3];
		static uint8_t seen[2048];
		if (ent < 0 && strncmp(name, "body_", 5) == 0 && e && !IsBadReadPtr(e, 12)) { // the first character that walks away from where it was first drawn: Price or a squad mate in the first seconds
			const float *o = (const float *)e;
			unsigned k = c & 0xffff;
			if (!seen[k])
				seen[k] = 1, memcpy(first[k], o, sizeof(first[k]));
			else if ((o[0] - first[k][0]) * (o[0] - first[k][0]) + (o[1] - first[k][1]) * (o[1] - first[k][1]) > 150.0f * 150.0f) {
				ent = (int)k;
				log_printf("native: treelog follows entity %d (%s)", ent, name);
			}
		}
		if (ent == (int)(c & 0xffff) && GetTickCount() - at >= 50 && e && !IsBadReadPtr(e, 12)) {
			at = GetTickCount();
			const uint8_t *tree = *(const uint8_t *const *)dobj;
			int idx = tree && !IsBadReadPtr(tree, 16) ? *(const short *)(tree + 8) : 0;
			char line[1500];
			const float *org = (const float *)e;
			int len = snprintf(line, sizeof(line), "native: tree t=%lu ent %d idx %d pos %.1f %.1f:", (unsigned long)GetTickCount(), ent, idx, e ? org[0] : 0.0f, e ? org[1] : 0.0f);
			for (int k = 0; idx > 0 && k < 5 && len < (int)sizeof(line) - 200; k++) {
				const uint8_t *node = base + XR64(0x1bc0a30) + (size_t)idx * 0x40;
				const float *f = (const float *)node;
				len += snprintf(line + len, sizeof(line) - len, " [n%d", idx);
				for (int j = 0; j < 16; j++) {
					float v = f[j];
					if (v != 0 && fabsf(v) > 1e-6f && fabsf(v) < 1e6f)
						len += snprintf(line + len, sizeof(line) - len, " %x:%.3f", j * 4, v);
					else if (((const uint32_t *)node)[j])
						len += snprintf(line + len, sizeof(line) - len, " %x:#%x", j * 4, ((const uint32_t *)node)[j]);
				}
				len += snprintf(line + len, sizeof(line) - len, "]");
				idx = *(const uint16_t *)(node + 0xc); // the first child
			}
			log_printf("%s", line);
		}
	}
	if (CFG_ONCE("animleaf", 0) && ((ctx_current == 1 && (c & 0xffff) == 0) || (ctx_current == 0 && (c & 0xffff) == 1)))
		animleaf_log("add", (int)(c & 0xffff), (const uint8_t *)a);
	if (CFG_ONCE("animlog", 0) && a && !IsBadReadPtr(a, 0xb0) && ((ctx_current == 1 && (c & 0xffff) == 0) || (ctx_current == 0 && (c & 0xffff) == 1)) && split_tick && GetTickCount() - split_tick > (DWORD)CFG_ONCE("animlogat", 30) * 1000) {
		// QA ([native] animlog=1, animlogat=<s>): the other player's body (entity 0 in context 1, entity 1 in context 0): wall clock, the draw origin and the animation tree's first nodes at each
		// draw, 60 draws per context, to see whether the animation advances every frame or in steps
		static unsigned an[2];
		if (an[ctx_current] < 60) {
			an[ctx_current]++;
			const uint8_t *tree = *(const uint8_t *const *)a;
			int idx = tree && !IsBadReadPtr(tree, 16) ? *(const short *)(tree + 8) : 0;
			char line[900];
			const float *org = (const float *)e;
			const float *pz = b && !IsBadReadPtr(b, 0x30) ? (const float *)b : NULL; // the pose (cpose: origin at +0x18, angles at +0x24)
			int len = snprintf(line, sizeof(line), "native: anim ctx %d t=%.1f cg.time %d idx %d pos %.1f %.1f pose %.2f %.2f yaw %.3f pitch %.3f:", ctx_current, lag_now() * 1000.0, *(int *)(base + XR64(0x422434)), idx, e ? org[0] : 0.0f, e ? org[1] : 0.0f,
					   pz ? pz[6] : 0.0f, pz ? pz[7] : 0.0f, pz ? pz[10] : 0.0f, pz ? pz[9] : 0.0f);
			for (int k = 0; idx > 0 && k < 3 && len < (int)sizeof(line) - 160; k++) {
				const uint8_t *node = base + XR64(0x1bc0a30) + (size_t)idx * 0x40;
				const float *f = (const float *)node;
				len += snprintf(line + len, sizeof(line) - len, " [n%d", idx);
				for (int j = 0; j < 16; j++)
					if (CFG_ONCE("animlog", 0) >= 2) // ([native] animlog=2: every dword of the node, raw)
						len += snprintf(line + len, sizeof(line) - len, " %08x", ((const uint32_t *)node)[j]);
					else if (f[j] != 0 && fabsf(f[j]) > 1e-6f && fabsf(f[j]) < 1e6f)
						len += snprintf(line + len, sizeof(line) - len, " %x:%.3f", j * 4, (double)f[j]);
				len += snprintf(line + len, sizeof(line) - len, "]");
				idx = *(const uint16_t *)(node + 0xc);
			}
			log_printf("%s", line);
		}
	}
	if (CFG_ONCE("animrate", 0) && a && !IsBadReadPtr(a, 0xb0) && ((ctx_current == 1 && (c & 0xffff) == 0) || (ctx_current == 0 && (c & 0xffff) == 1))) {
		// QA ([native] animrate=1): how often the other player's body animation changes between two of its draws: a hash of the first nodes of the animation tree (every dword, so
		// time and weight fields count), compared with the same body's previous draw; the rates are logged every 300 draws with the origin step, to tell a body that is updated every
		// frame from one that steps (the owner's "10-15 fps" body in the bottom half).
		static uint32_t last_h[2];
		static unsigned total[2], changed[2], moved[2];
		static float last_o[2][2];
		const uint8_t *tree = *(const uint8_t *const *)a;
		int idx = tree && !IsBadReadPtr(tree, 16) ? *(const short *)(tree + 8) : 0;
		uint32_t h = 2166136261u;
		for (int k = 0; idx > 0 && k < 8; k++) {
			const uint8_t *node = base + XR64(0x1bc0a30) + (size_t)idx * 0x40;
			for (int j = 0; j < 16; j++)
				h = (h ^ ((const uint32_t *)node)[j]) * 16777619u;
			idx = *(const uint16_t *)(node + 0xc);
		}
		const int x = ctx_current;
		const float *org = (const float *)e;
		total[x]++;
		changed[x] += h != last_h[x];
		moved[x] += org && (org[0] != last_o[x][0] || org[1] != last_o[x][1]);
		last_h[x] = h;
		if (org)
			last_o[x][0] = org[0], last_o[x][1] = org[1];
		if (total[x] % 300 == 0)
			log_printf("native: animrate ctx %d: the body's animation changed in %u of %u draws, its origin in %u (cg.time %d)", x, changed[x], total[x], moved[x], *(int *)(base + XR64(0x422434)));
	}
	if (ctx_current == 0 && (c & 0xffff) == 1)
		p0cnt_flags0 = d;

	if (CFG_ONCE("p0cnt", 0) >= 2 && ((ctx_current == 0 && (c & 0xffff) == 1) || (ctx_current == 1 && (c & 0xffff) == 0))) {
		int k = ctx_current;
		memcpy(p0dump_slot[k], base + RVA_CENT + (size_t)(c & 0xffff) * CENT_SIZE, sizeof(p0dump_slot[k]));
		if (a && !IsBadReadPtr(a, sizeof(p0dump_dobj[k]))) {
			memcpy(p0dump_dobj[k], a, sizeof(p0dump_dobj[k]));
			p0dump_have[k] = 1;
		}
	}
	if (ctx_current == 1 && (c & 0xffff) == 0) {
		p0cnt_adds++;
		p0cnt_flags1 = d;
		if (CFG_ONCE("p0cnt", 0) && a && !IsBadReadPtr(a, 0x100)) {
			int k = 0;
			while (k < 6 && p0ptr[k] && p0ptr[k] != a)
				k++;
			if (k < 6) {
				p0ptr[k] = a;
				p0ptr_cnt[k]++;
				p0ptr_b8[k] = *(const uint32_t *)((const uint8_t *)a + 0xb8);
				p0ptr_bc[k] = *(const uint32_t *)((const uint8_t *)a + 0xbc);
			}
		}
		const float *dp = (const float *)e, *pp = (const float *)b;
		for (int i = 0; i < 3; i++) {
			p0cnt_draw_org[i] = dp ? dp[i] : 0;
			p0cnt_draw_org_b[i] = pp ? pp[6 + i] : 0; // the pose's origin sits at +0x18 of what rdx points to (see jumplog)
		}
	}
	if (CFG_ONCE("nearlog", 0) == 2 && ctx_current == 1 && (c & 0xffff) == 0 && e) { // QA: entity 0's adds in context 1, how many sit on player 2's camera, every 2 s
		static unsigned near_n, all_n;
		static DWORD t0;
		const float *v = (const float *)(base + XR(0x42d310)), *o = (const float *)e;
		const float dx = o[0] - v[0], dy = o[1] - v[1];
		all_n++;
		if (dx * dx + dy * dy < 900.0f)
			near_n++;
		const DWORD now = GetTickCount();
		if (!t0)
			t0 = now;
		if (now - t0 >= 2000) {
			log_printf("native: nearcount ent0 in ctx1: %u of %u adds on player 2's camera (last at %.1f %.1f, cam %.1f %.1f)", near_n, all_n, (double)o[0], (double)o[1], (double)v[0], (double)v[1]);
			near_n = all_n = 0, t0 = now;
		}
	}
	if (CFG_ONCE("nearlog", 0) == 1 && ctx_current == 1 && a && !IsBadReadPtr(a, 0xb0) && e) { // QA: every model player 2's scene adds within 60 units of his camera
		const float *v = (const float *)(base + XR(0x42d310)), *o = (const float *)e;
		const float dx = o[0] - v[0], dy = o[1] - v[1], dz = o[2] - v[2];
		static unsigned kn;
		if (dx * dx + dy * dy + dz * dz < 3600.0f && GetTickCount() - split_tick > 5000 && ++kn < 400 && kn % 4 == 0) {
			const uint8_t *dj = (const uint8_t *)a;
			const uint8_t *const *mm = *(const uint8_t *const **)(dj + DOBJ_MODELS);
			char names[200] = "";
			for (int mi = 0; mi < dj[DOBJ_NUMMODELS] && mi < 6 && mm && !IsBadReadPtr(mm + mi, 8); mi++) {
				const uint8_t *xm = mm[mi];
				const char *nm = xm && !IsBadReadPtr(xm, 8) ? *(const char *const *)xm : NULL;
				if (nm && !IsBadReadPtr(nm, 4))
					snprintf(names + strlen(names), sizeof(names) - strlen(names), "%s ", nm);
			}
			log_printf("native: nearlog ctx1 ent %u dobj %p at %.1f %.1f %.1f (cam %.1f %.1f %.1f) models %s from rva %llx", c & 0xffff, a, (double)o[0], (double)o[1], (double)o[2], (double)v[0], (double)v[1],
				   (double)v[2], names, (unsigned long long)((uint8_t *)__builtin_return_address(0) - base));
		}
	}
	if (CFG_ONCE("bodyaddlog", 0) == 2 && a && !IsBadReadPtr(a, 0xb0)) { // QA: every add of a coop body model, any entity, both contexts
		const uint8_t *dj = (const uint8_t *)a;
		const uint8_t *const *mm = *(const uint8_t *const **)(dj + DOBJ_MODELS);
		static unsigned kb2;
		for (int mi = 0; mi < dj[DOBJ_NUMMODELS] && mi < 8 && mm && !IsBadReadPtr(mm + mi, 8); mi++) {
			const uint8_t *xm = mm[mi];
			const char *nm = xm && !IsBadReadPtr(xm, 8) ? *(const char *const *)xm : NULL;
			if (nm && !IsBadReadPtr(nm, 4) && strstr(nm, "coop_body") && ++kb2 > 2000 && kb2 < 2060) {
				const float *p = (const float *)e;
				log_printf("native: coopbody add #%u ctx %d ent %u dobj %p models %d [%d] %s draw %.1f %.1f %.1f from rva %llx", kb2, ctx_current, c & 0xffff, a, dj[DOBJ_NUMMODELS], mi, nm,
					   p ? (double)p[0] : 0, p ? (double)p[1] : 0, p ? (double)p[2] : 0, (unsigned long long)((uint8_t *)__builtin_return_address(0) - base));
			}
		}
	}
	if ((c & 0xffff) <= 1 && CFG_ONCE("bodyaddlog", 0)) { // QA: every add of a player entity (0, 1) in a window of the run: context, record index, origin, caller
		static unsigned kb;
		const float *p = (const float *)e;
		if (++kb > 3000 && kb < 3060)
			log_printf("native: bodyadd #%u ctx %d ent %u rec %u draw %.1f %.1f %.1f from rva %llx", kb, ctx_current, c & 0xffff, *(const unsigned *)(base + XR64(0x40dd754)), p ? (double)p[0] : 0,
				   p ? (double)p[1] : 0, p ? (double)p[2] : 0, (unsigned long long)((uint8_t *)__builtin_return_address(0) - base));
	}
	if (ctx_current == 1 && (c & 0xffff) == 0 && CFG_ONCE("p2hooklog", 0)) { // QA: entity 0's body as context 1 adds it, sampled over the whole run
		static unsigned k0;
		const float *p = (const float *)e;
		if (++k0 % 60 == 0 && k0 < 6000)
			log_printf("native: ent0 add ctx1 #%u draw %.1f %.1f %.1f from rva %llx", k0, p ? (double)p[0] : 0, p ? (double)p[1] : 0, p ? (double)p[2] : 0, (unsigned long long)((uint8_t *)__builtin_return_address(0) - base));
	}
	if (on && ctx_current == 1) {
		static unsigned seen, frame;
		if (c == 0 || ++seen % 1 == 0) {
			if (seen < 5000 && seen > 800) {
				const float *o = (const float *)b;
				{ const float *p = (const float *)e; const char *mname = "?";
				if (a && !IsBadReadPtr(a, 0xb0)) { const uint8_t *dj = (const uint8_t *)a; const uint8_t *const *mm = *(const uint8_t *const **)(dj + DOBJ_MODELS); if (dj[DOBJ_NUMMODELS] && mm && !IsBadReadPtr(mm, 4) && mm[0] && !IsBadReadPtr(mm[0], 8) && *(const char *const *)mm[0] && !IsBadReadPtr(*(const char *const *)mm[0], 4)) mname = *(const char *const *)mm[0]; }
				log_printf("native: dobj add ctx1 ent %u flags %08x draw %.1f %.1f %.1f | model %s | from rva %llx", c & 0xffff, d, p ? p[0] : 0, p ? p[1] : 0, p ? p[2] : 0, mname, (unsigned long long)((uint8_t *)__builtin_return_address(0) - base)); (void)o; }
			}
		}
		(void)frame;
	}
	{ // [native] jumplog=1: an entity whose drawn origin (the pose at +0x18 of rdx) moves more than 16 units from one add to the next in the same context is logged (the far
	  // models' jumps in the bottom half: in the data the cgame hands over, or later in the renderer)
		static int jl = -1;
		static float last[2][1024][3];
		static uint8_t have[2][1024];
		static unsigned logged;
		if (jl < 0)
			jl = cfg("jumplog", 0);
		unsigned k = c & 0xffff;
		if (jl && (ctx_current == 0 || ctx_current == 1) && k >= 2 && k < 1024 && b) {
			const float *o = (const float *)((const uint8_t *)b + 0x18);
			float *l = last[ctx_current][k];
			if (have[ctx_current][k]) {
				float dx = o[0] - l[0], dy = o[1] - l[1], dz = o[2] - l[2], d = dx * dx + dy * dy + dz * dz;
				if (d > 256.0f && logged < 400) {
					logged++;
					log_printf("native: jump ctx %d ent %u by %.1f: %.1f %.1f %.1f -> %.1f %.1f %.1f", ctx_current, k, sqrtf(d), l[0], l[1], l[2], o[0], o[1], o[2]);
				}
			}
			l[0] = o[0], l[1] = o[1], l[2] = o[2];
			have[ctx_current][k] = 1;
		}
	}
	{ // [native] dobjcount=1: counts only (no per-call lines, they change the timing): context 1's adds of entities 0 and 1, by where they are drawn
		static int cnt_on = -1;
		static unsigned at_p0[2], at_p2[2], other[2], calls;
		if (cnt_on < 0)
			cnt_on = cfg("dobjcount", 0);
		if (cnt_on && ctx_current == 1 && (c & 0xffff) < 2 && e) {
			const float *pp = (const float *)e;
			unsigned k = c & 0xffff;
			float d0 = (pp[0] + 2515.2f) * (pp[0] + 2515.2f) + (pp[1] - 5190.2f) * (pp[1] - 5190.2f);
			float d2 = (pp[0] + 2588.0f) * (pp[0] + 2588.0f) + (pp[1] - 5212.0f) * (pp[1] - 5212.0f);
			if (d0 < 400.0f)
				at_p0[k]++;
			else if (d2 < 400.0f)
				at_p2[k]++;
			else
				other[k]++;
			if (++calls % 300 == 0)
				log_printf("native: dobjcount ctx1: ent0 at p1 %u at p2 %u other %u | ent1 at p1 %u at p2 %u other %u", at_p0[0], at_p2[0], other[0], at_p0[1], at_p2[1], other[1]);
		}
	}
	{ // Player 1's body in context 1 is added twice: by the cgame's add-by-type function (rva 0x9ea20, its "general" branch: context 1's slot 0 holds a copy of context 0's
	  // own player) and by the pass that draws the other co-op player (rva 0xb0680, as context 0 draws player 2). With both in the scene the body was not drawn in the
	  // bottom half, only the name over it; with the second one alone it is (2026-10-28, frames of Cliffhanger tarmac). The first is dropped ([native] p0general=1 keeps it).
		static int keep_general = -1;
		if (keep_general < 0)
			keep_general = cfg("p0general", 0);
		if (ctx_current == 1 && (c & 0xffff) == 0 && !keep_general) {
			const uint8_t *ret = (const uint8_t *)__builtin_return_address(0);
			if (P0GENERAL_RET(ret))
				return 0;
		}
	}
	// Mode 11: player 2's frame is the renderer's view of local client 1, and the renderer tests a model's "hidden from" bit (the entity flags & 0x3f0 handed over in r9,
	// 0x10 << local client) by that view. vis_swap gives the cgame, which draws him as local client 0, his bits in local client 0's place: right for the cgame's own tests,
	// wrong for the renderer's, so a model the script hid from player 2 alone was drawn in his half and one hidden from player 1 alone was not (Vlad, Spec Ops snowmobile
	// races 2026-11-17: player 2 saw his own rider body from inside; entity type 13, gone with skiptypes=8192). The two bits go back for the renderer ([native] m11mask=0 off).
	// (the 32-bit builds have no vis_swap: there add_entity_hook swaps each entity's bits, ent_bits_swapped; the rider body of a Cliffhanger snowmobile, hidden from its rider,
	// stood in front of player 2's own camera, 2026-11-21)
	if (ctx_current == 1 && mode == 11 && (vis_swapped || ent_bits_swapped) && CFG_ONCE("m11mask", 1)) {
		const unsigned b0 = d & 0x10u, b1 = d & 0x20u;
		d = (d & ~0x30u) | (b0 << 1) | (b1 >> 1);
	}
	return dobj_add_original(a, b, c, d, e, f, g, h);
}

// The local player's own origin and angles are written into cg_entities by rva 0xb28b0(lc) (after the snapshot's entity states), into the slot that the lc -> slot rule
// gives (inline at 0xb28d2, the same rule as 0xa5ec0: lc itself without the coop table). Context 1 runs with lc 0, so player 2's position went into slot 0 every frame and
// player 1's body was drawn in player 2's camera (dobjlog). In context 1 the write is moved to slot 1 and slot 0 keeps what it had ([native] ownpos=0 off).
static void (*own_write_original)(unsigned);
static unsigned local_slot_hook(unsigned lc);
// The 360's rule for "which entity is me" (2026-11-15): the own-entity write takes the client slot of its local player (360: 0x820d81a8; PC: 0xa5ec0, answered by
// local_slot_hook, slot 1 in context 1). The PC's 0xb28b0 has its own inline copy of the old rule at 0xb28f3..0xb292d (no co-op table: the slot is the local player
// number, 0 in both contexts). Those 58 bytes become a call of local_slot_hook, so player 2's own state is written into his own slot 1 and slot 0 keeps player 1's
// entity, as on the 360 ([native] ownslot360=0: the old write into slot 0 and the moves after it).
static int own_slot_patched;
#ifdef _WIN64
static void own_slot_install(void)
{
	static const uint8_t want[] = {0x75, 0x04, 0x8b, 0xd1, 0xeb, 0x34, 0x8b, 0xd5, 0x48, 0x8d, 0x0d};
	uint8_t *p = base + XR(0xb28f3);
	if (own_slot_patched || !CFG_ONCE("ownslot360", 1))
		return;
	if (memcmp(p, want, sizeof(want))) {
		log_printf("native: ownslot360: unexpected code at rva 0xb28f3, the old slot rule stays");
		return;
	}
	uint8_t put[58];
	memset(put, 0x90, sizeof(put));
	const uint64_t fn = (uint64_t)(uintptr_t)local_slot_hook;
	int i = 0;
	put[i++] = 0x89, put[i++] = 0xd9;                 // mov ecx, ebx (the local player number)
	put[i++] = 0x48, put[i++] = 0xb8;                 // mov rax, local_slot_hook
	memcpy(put + i, &fn, 8), i += 8;
	put[i++] = 0xff, put[i++] = 0xd0;                 // call rax (rsp is 16-aligned here; rsp+0..0x2f is the outgoing argument area)
	put[i++] = 0x89, put[i++] = 0xc2;                 // mov edx, eax (the slot)
	put[i++] = 0x89, put[i++] = 0xd9;                 // mov ecx, ebx
	const int next = i + 2;
	put[i++] = 0xeb, put[i++] = (uint8_t)(0x3a - next); // jmp short 0xb292d
	DWORD old;
	VirtualProtect(p, sizeof(put), PAGE_EXECUTE_READWRITE, &old);
	memcpy(p, put, sizeof(put));
	VirtualProtect(p, sizeof(put), old, &old);
	FlushInstructionCache(GetCurrentProcess(), p, sizeof(put));
	own_slot_patched = 1;
	log_printf("native: ownslot360: the own-entity write takes the client slot (as on the 360)");
	// The same old rule, inline in CG_DrawActiveFrame (0xbcc07..0xbcc3e, lc in esi): the slot given to 0x30d0, which replays the second model record list (count
	// 0x1440dd758) and keeps the records of that client slot (flag 0x800000 on a record = the other slot's). With slot 0 in context 1, player 2's frame replayed
	// player 1's records. The 360 asks the client slot here too.
	static const uint8_t want2[] = {0x75, 0x04, 0x8b, 0xd6, 0xeb, 0x31, 0x41, 0x8b, 0xd7, 0x48, 0x8d, 0x0d};
	uint8_t *q = base + XR(0xbcc07);
	if (memcmp(q, want2, sizeof(want2))) {
		log_printf("native: ownslot360: unexpected code at rva 0xbcc07, the replayed records keep the old slot rule");
		return;
	}
	uint8_t put2[55];
	memset(put2, 0x90, sizeof(put2));
	int j = 0;
	put2[j++] = 0x89, put2[j++] = 0xf1;               // mov ecx, esi (the local player number)
	put2[j++] = 0x48, put2[j++] = 0xb8;               // mov rax, local_slot_hook
	memcpy(put2 + j, &fn, 8), j += 8;
	put2[j++] = 0xff, put2[j++] = 0xd0;               // call rax
	put2[j++] = 0x89, put2[j++] = 0xc2;               // mov edx, eax
	const int next2 = j + 2;
	put2[j++] = 0xeb, put2[j++] = (uint8_t)(0x37 - next2); // jmp short 0xbcc3e
	VirtualProtect(q, sizeof(put2), PAGE_EXECUTE_READWRITE, &old);
	memcpy(q, put2, sizeof(put2));
	VirtualProtect(q, sizeof(put2), old, &old);
	FlushInstructionCache(GetCurrentProcess(), q, sizeof(put2));
	log_printf("native: ownslot360: the replayed model records take the client slot too");
}
#endif
static const uint8_t own_write_bytes[] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18};
static void own_write_hook(unsigned lc)
{
	static int on = -1;
	if (on < 0)
		on = cfg("ownpos", 1);
	if ((ctx_current == 0 || ctx_current == 1) && CFG_ONCE("entdump", 0)) { // QA: the cg_entities slots 0 and 1 of each context: the dwords that changed since the last look (which fields carry a body's weapon)
		static DWORD at[2];
		static uint32_t before[2][2][CENT_SIZE / 4];
		static int have[2];
		if (split_tick && GetTickCount() - split_tick >= (DWORD)CFG_ONCE("entdump", 0) * 1000 && GetTickCount() - at[ctx_current] >= 500) {
			at[ctx_current] = GetTickCount();
			for (int k = 0; k < 2; k++) {
				const uint32_t *d = (const uint32_t *)(base + RVA_CENT + (size_t)k * CENT_SIZE);
				char line[1400];
				int n = snprintf(line, sizeof(line), "native: entdump ctx %d ent %d:", ctx_current, k);
				for (int i = 0; have[ctx_current] && i < CENT_SIZE / 4 && n < (int)sizeof(line) - 24; i++)
					if (d[i] != before[ctx_current][k][i] && i != 0 && i != 6 && i != 7 && i != 8 && i != 0x20 && i != 0x21 && i != 0x22)
						n += snprintf(line + n, sizeof(line) - n, " %x:%x>%x", i * 4, before[ctx_current][k][i], d[i]);
				if (have[ctx_current] && n > 30)
					log_printf("%s", line);
				memcpy(before[ctx_current][k], d, sizeof(before[0][0]));
			}
			have[ctx_current] = 1;
		}
	}
	if (ctx_current == 0 && CFG_ONCE("psdiff", 0)) { // QA: the dwords of player 1's playerState (context 0's block) that changed since the last look (a weapon switch)
		static uint32_t before[0xc00];
		static int have;
		static DWORD at;
		if (GetTickCount() - at >= 500) {
			at = GetTickCount();
			const uint32_t *now = (const uint32_t *)(base + RVA_CL_PS); // context 0 is live: its playerState is in the live block
			char line[1200];
			int n = snprintf(line, sizeof(line), "native: psdiff:");
			for (int i = 0; have && i < 0xc00 && n < (int)sizeof(line) - 24; i++)
				if (now[i] != before[i])
					n += snprintf(line + n, sizeof(line) - n, " %x:%x>%x", i * 4, before[i], now[i]);
			if (have && n > 20)
				log_printf("%s", line);
			memcpy(before, now, sizeof(before));
			have = 1;
		}
	}
	if (ctx_current == 1 && CFG_ONCE("pslog", 0)) {
		static unsigned seen;
		if (++seen % 60 == 1 && seen < 4000) {
			const uint8_t *nps = *(const uint8_t **)(base + XR64(0x40cad8)), *pps = *(const uint8_t **)(base + XR64(0x40cad0));
			const float *f0 = (const float *)(base + RVA_CENT), *f1 = (const float *)(base + RVA_CENT + CENT_SIZE);
			{
				const float *q1 = (const float *)(base + RVA_CL_PS), *q0 = ctx_store[0][5] ? (const float *)(ctx_store[0][5] + CL_BLOCK_PS) : NULL;
				log_printf("native: ps1(live) +0x100..: %d %.1f %.1f %.1f %.1f %.1f %.1f | ps0(store) %d %.1f %.1f %.1f %.1f %.1f %.1f | acc1 %.1f", *(const int *)(q1 + 64), q1[65], q1[66], q1[67], q1[68], q1[69], q1[70],
					   q0 ? *(const int *)(q0 + 64) : -9, q0 ? q0[65] : 0, q0 ? q0[66] : 0, q0 ? q0[67] : 0, q0 ? q0[68] : 0, q0 ? q0[69] : 0, q0 ? q0[70] : 0, *(const float *)(base + XR64(0x533d18)));
			}
			log_printf("native: ctx1 b28b0 lc %u: nextPs clientNum %d prevPs clientNum %d | slot0 %.1f %.1f valid %x | slot1 %.1f %.1f valid %x", lc, nps ? *(const int *)(nps + 0x100) : -9,
				   pps ? *(const int *)(pps + 0x100) : -9, f0[6], f0[7], *(const uint32_t *)((const uint8_t *)f0 + CENT_VALID), f1[6], f1[7], *(const uint32_t *)((const uint8_t *)f1 + CENT_VALID));
		}
	}
	if (on && ctx_current == 1 && lc == 0) {
		uint8_t *s0 = base + RVA_CENT;
#ifdef _WIN64
		if (own_slot_patched) { // the write itself goes to player 2's slot (the 360's rule, own_slot_install): nothing to move
			own_write_original(lc);
			if (CFG_ONCE("p2hooklog", 0)) {
				static unsigned kw;
				if (++kw % 60 == 0 && kw < 6000)
					log_printf("native: p2hook after own write: slot0 pose %.1f %.1f | slot1 pose %.1f %.1f", (double)((const float *)(s0 + 0x18))[0], (double)((const float *)(s0 + 0x18))[1],
						   (double)((const float *)(s0 + CENT_SIZE + 0x18))[0], (double)((const float *)(s0 + CENT_SIZE + 0x18))[1]);
			}
		}
		else {
			uint8_t *s1 = s0 + CENT_SIZE;
			uint8_t keep0[0x18];
			memcpy(keep0, s0 + 0x18, sizeof(keep0));
			own_write_original(lc);
			memcpy(s1 + 0x18, s0 + 0x18, sizeof(keep0));
			memcpy(s0 + 0x18, keep0, sizeof(keep0));
		}
#else
		own_write_original(lc); // (the 32-bit function asks local_slot_hook for its slot: it writes slot 1 itself)
#endif
		// player 1's entity for the whole of context 1's frame, not only for the draws: the skeleton and controllers (rva 0xb0860 between the two passes) are
		// computed from this slot too, and with player 2's leftover position in it the body was posed in player 2's camera, depending on timing ([native] p0frame=0 off)
		static int whole = -1;
		if (whole < 0)
			whole = cfg("p0frame", 1);
		if (whole && p0_ent_ok && p0copy_on())
			p0_put(s0);
		return;
	}
	own_write_original(lc);
#ifndef _WIN64
	// 32 bits: context 0's entity pass never hands entity 0 (the local player) to add_entity_hook, so player 1's entity is taken here, right after the cgame has written his
	// origin and angles into slot 0 (the 64-bit capture in add_entity_hook runs for n == 0)
	if (ctx_current == 0 && lc == 0) {
		memcpy(p0_ent, base + RVA_CENT, sizeof(p0_ent));
		p0_ent_ok = 1;
	}
#endif
}

static void add_entity_hook(int n)
{
	static unsigned frames;
	static int logging;
	if (ctx_current == 0) { // diagnostics: [native] skip0=<n> drops entity n in context 0's draw (is the bottom half's head a leftover of the top half's scene?)
		static int skip0 = -2;
		if (skip0 == -2)
			skip0 = cfg("skip0", -1);
		if (n == skip0)
			return;
	}

	// Lag measurement ([native] laglog=1): while context 0 draws, the floats of player 1's entity as the cgame holds them, next to context 1's own view yaw
	// (its accumulator), both stamped by the log's own clock: the field that follows the yaw and its delay behind the accumulator show where the time goes.
	if (ctx_current == 0 && n == 1 && CFG_ONCE("laglog", 0)) {
		static unsigned lag_frames;
		if (++lag_frames % 2 == 0 && lag_frames < 6000 && ctx_store[1][4]) {
			const float *f = (const float *)(base + RVA_CENT + (size_t)n * CENT_SIZE);
			char line[1400];
			int len = snprintf(line, sizeof(line), "native: lag acc %.2f f:", *(float *)(ctx_store[1][4] + ACC_IN_CLS + 4));
			for (int i = 0; i < CENT_SIZE / 4 && len < (int)sizeof(line) - 12; i++)
				len += snprintf(line + len, sizeof(line) - len, " %.1f", f[i]);
			log_printf("%s", line);
		}
	}
	if (ctx_current == 1) {
		if (swap_entities && n == 0 && !entities_swapped) {
			// the cgame hides the entity numbered like the local client (0) as "mine"; context 1 is local client 0 but player 1: his entity and player 0's trade places for the draw
			uint8_t *e0 = base + RVA_CENT, *e1 = e0 + CENT_SIZE, t[CENT_SIZE];
			memcpy(t, e0, sizeof(t));
			memcpy(e0, e1, sizeof(t));
			memcpy(e1, t, sizeof(t));
			entities_swapped = 1;
		}
		if (log_entities && n == 0 && logging == 0 && ++frames == 700)
			logging = 1;
		if (logging == 1) {
			const uint8_t *e = base + RVA_CENT + (size_t)n * CENT_SIZE;
			log_printf("native: ctx1 adds entity %d type %d flags %08x pos %.1f %.1f %.1f yaw %.1f", n, e[CENT_TYPE], *(unsigned *)(e + CENT_FLAGS), ((const float *)e)[63], ((const float *)e)[64], ((const float *)e)[65], ((const float *)e)[73]);
		}
		if (CFG_ONCE("swaplog", 0) && n <= 1) {
			static unsigned sl;
			const float *q0 = (const float *)(base + RVA_CENT), *q1 = (const float *)(base + RVA_CENT + CENT_SIZE);
			if (++sl % 40 == 0 && sl < 1600)
				log_printf("native: swaplog ctx1 n %d swapped %d slot0 pos %.1f %.1f type %d | slot1 pos %.1f %.1f type %d", n, entities_swapped, q0[63], q0[64], ((const uint8_t *)q0)[CENT_TYPE], q1[63], q1[64], ((const uint8_t *)q1)[CENT_TYPE]);
		}
		{ // diagnostics: [native] skiptypes=<mask> drops, in context 1's draw, every entity whose type (byte 0xe0) has its bit set (bisect for the garbled mesh)
			static int skiptypes = -1;
			if (skiptypes < 0)
				skiptypes = cfg("skiptypes", 0);
			unsigned type = ((const uint8_t *)(base + RVA_CENT + (size_t)n * CENT_SIZE))[CENT_TYPE];
			if (type < 31 && (skiptypes & (1 << type)))
				return;
		}
		if (n == skip_entity)
			return;
		// the per-client "hide from" bits of an entity (flags & 0x3f0, bit 4 + local client) are made for local client numbers: context 1 is local client 0 but is client 1
		// ([native] swapbits=0 turns it off)
		static int swapbits = -1;
		if (swapbits < 0)
			swapbits = cfg("swapbits", 1);
		unsigned *flags = (unsigned *)(base + RVA_CENT + (size_t)n * CENT_SIZE + CENT_FLAGS);
		unsigned saved = *flags;
		if (swapbits && !vis_swapped) { // (not when vis_swap already swapped them for the whole draw: a second swap gave +0xec back player 1's marks)
			unsigned b0 = saved & 0x10, b1 = saved & 0x20;
			*flags = (saved & ~0x30u) | (b0 ? 0x20 : 0) | (b1 ? 0x10 : 0);
			ent_bits_swapped = 1;
		}
		{
			float sv[LAG_SAVED];
			int ap;
			uint8_t slot1_saved[CENT_SIZE];
			int p0copy = n == 0 && p0_ent_ok && p0copy_on();
			if (p0copy) {
				memcpy(slot1_saved, base + RVA_CENT, sizeof(slot1_saved));
				p0_put(base + RVA_CENT);
			}
			if (n == 0 && CFG_ONCE("p0log", 0)) { // player 1's body in context 1's draw: the slot as it was drawn (copy of context 0's entity 0)
				static unsigned pl;
				const uint8_t *e = base + RVA_CENT;
				if (++pl % 10 == 0 && pl < 30000)
					log_printf("native: p0log ctx1 draws entity0: ok %d copied %d pos %.1f %.1f %.1f yaw %.1f type %d flags %08x valid %08x | own p2 pos %.1f %.1f", p0_ent_ok, p0copy, (double)((const float *)e)[CF_ORG2], (double)((const float *)e)[CF_ORG2 + 1], (double)((const float *)e)[CF_ORG2 + 2],
						   (double)((const float *)e)[CF_YAW2], e[CENT_TYPE], *(const unsigned *)(e + CENT_FLAGS), *(const unsigned *)(e + CENT_VALID), (double)*(const float *)(base + XR64(0x42245c)), (double)*(const float *)(base + XR64(0x422460)));
			}
			if (n == 0 && lag_history_needed())
				lag_record();
			lag_fix_yaw(n, 0, sv, &ap); // player 0's body in context 1's picture (his entity sits at index 1 after the swap)
			add_entity_original(n);
			lag_unfix_yaw(n, sv, ap);
			if (p0copy) { // (braces: without them the slot was "put back" from an unset buffer after every entity, the 32-bit crash in the ragdoll code at the split)
				p0_tail_take(base + RVA_CENT);
				memcpy(base + RVA_CENT, slot1_saved, sizeof(slot1_saved));
			}
		}
		*flags = saved;
		ent_bits_swapped = 0;
		return;
	}
	{
		float sv[LAG_SAVED];
		int ap;
		if (n == 0) {
			memcpy(p0_ent, base + RVA_CENT, sizeof(p0_ent));
			p0_ent_ok = 1;
			if (lag_history_needed())
				lag_record();
			if (CFG_ONCE("p0log", 0)) { // what context 0's draw hands over as player 1's entity
				static unsigned cl0;
				if (++cl0 % 10 == 0 && cl0 < 30000)
					log_printf("native: p0log ctx0 captured entity0: pose %.1f %.1f %.1f | pos %.1f %.1f %.1f yaw %.1f type %d flags %08x valid %08x", (double)((const float *)(p0_ent + 0x18))[0], (double)((const float *)(p0_ent + 0x18))[1], (double)((const float *)(p0_ent + 0x18))[2], (double)((const float *)p0_ent)[CF_ORG2], (double)((const float *)p0_ent)[CF_ORG2 + 1], (double)((const float *)p0_ent)[CF_ORG2 + 2],
						   (double)((const float *)p0_ent)[CF_YAW2], p0_ent[CENT_TYPE], *(const unsigned *)(p0_ent + CENT_FLAGS), *(const unsigned *)(p0_ent + CENT_VALID));
			}
		}
		lag_fix_yaw(n, 1, sv, &ap); // player 1's body in context 0's picture
		add_entity_original(n);
		lag_unfix_yaw(n, sv, ap);
	}
}

// Diagnostics / fix for the 2D texts that both halves draw (the objective text, subtitles, "who stands in front" hints are global): [native] skiphud=<mask> drops, in context 1's
// frames, the HUD parts called at the end of the cgame draw: bit 0 rva 0xc6070, bit 1 rva 0xc14e0, bit 2 rva 0x94d70, bit 3 rva 0xc5b70.
static void (*hud_c6070_original)(unsigned);
static void (*hud_c14e0_original)(unsigned);
static void (*hud_94d70_original)(unsigned);
static void (*hud_c5b70_original)(unsigned);
static void hud_c6070_hook(unsigned lc) { if (!(ctx_current == skip_ctx && (skip_hud_mask & 1))) hud_c6070_original(lc); }
static void hud_c14e0_hook(unsigned lc) { if (!(ctx_current == skip_ctx && (skip_hud_mask & 2))) hud_c14e0_original(lc); }
static void hud_94d70_hook(unsigned lc) { if (!(ctx_current == skip_ctx && (skip_hud_mask & 4))) hud_94d70_original(lc); }
static void hud_c5b70_hook(unsigned lc) { if (!(ctx_current == skip_ctx && (skip_hud_mask & 8))) hud_c5b70_original(lc); }
// more candidates: the calls between and after them in the cgame draw (rva of the call instruction, the callee)
typedef void (*hud_fn_t)(void *, void *, void *, void *, void *, void *);
static hud_fn_t hud_x_original[8]; // called with the six argument registers/stack slots as they come (the callees take up to four register arguments and one on the stack)
#define HUD_X(n, bit) static void hud_x##n##_hook(void *a, void *b, void *c, void *d, void *e, void *f) { if (!(ctx_current == skip_ctx && (skip_hud_mask & (bit)))) hud_x_original[n](a, b, c, d, e, f); }
HUD_X(0, 0x10)  // 0xbd03b be710
HUD_X(1, 0x20)  // 0xbd0fb 11f0c0 (4 arguments)
HUD_X(2, 0x40)  // 0xbd102 1214e0
HUD_X(3, 0x80)  // 0xbd109 fd010
#ifdef _WIN64
// Diagnostics ([native] sndcount=1, with sndlog=1 in the report): how often each context starts a world sound (rva 0x23a9b0); a stub that counts and jumps on.
// The same stub mutes the starts while snd_mute is set ([native] evsnd1, below): it returns "no sound" (-1, and -1 in the handle the 8th argument points to, as the
// function itself does when sound is off).
extern char snd_count_stub[];
volatile uint8_t snd_mute, snd_count_on;
void snd_note(const char **alias, uintptr_t ret); // (below: counts per alias name)
__asm__(".text\n.globl snd_count_stub\nsnd_count_stub:\n"
	"\tcmpb $0, snd_mute(%rip)\n"
	"\tjne 2f\n"
	"\tcmpb $0, snd_count_on(%rip)\n"
	"\tje 1f\n"
	"\tpush %rax\n"
	"\tpush %rcx\n"
	"\tpush %rdx\n"
	"\tpush %r8\n"
	"\tpush %r9\n"
	"\tpush %r10\n"
	"\tpush %r11\n"
	"\tsub $0x60, %rsp\n"
	"\tmovdqu %xmm0, 0x20(%rsp)\n"
	"\tmovdqu %xmm1, 0x30(%rsp)\n"
	"\tmovdqu %xmm2, 0x40(%rsp)\n"
	"\tmovdqu %xmm3, 0x50(%rsp)\n"
	"\tmov 0x98(%rsp), %rdx\n" // (the caller's return address: 7 pushes and 0x60 above it)
	"\tcall snd_note\n"
	"\tmovdqu 0x20(%rsp), %xmm0\n"
	"\tmovdqu 0x30(%rsp), %xmm1\n"
	"\tmovdqu 0x40(%rsp), %xmm2\n"
	"\tmovdqu 0x50(%rsp), %xmm3\n"
	"\tadd $0x60, %rsp\n"
	"\tpop %r11\n"
	"\tpop %r10\n"
	"\tpop %r9\n"
	"\tpop %r8\n"
	"\tpop %rdx\n"
	"\tpop %rcx\n"
	"\tpop %rax\n"
	"1:\tjmp *snd_count_orig(%rip)\n"
	"2:\tmov 0x40(%rsp), %rax\n"
	"\ttest %rax, %rax\n"
	"\tjz 3f\n"
	"\tmovl $-1, (%rax)\n"
	"3:\tmov $-1, %eax\n"
	"\tret\n"
	);
// The entity events of the shared snapshot ran in both contexts (each context keeps its own "last event" mark, cent+0x194, while both see the same entities), so every
// event sound of the world (shots, explosions, impacts, the AI's footsteps, the scripted one-shot sounds) started twice in the one mixer: measured on Wolverines! with
// sndcount=2, every alias as often in context 1 as in context 0 (e.g. aagun_fire_skyburst 13 / 13 a second). The Xbox 360 runs each event once (its last-event
// mark is in its one shared entity table, 0x820f1658) and plays it to the listeners of both players. Context 1's event checks (rva 0x9f840 and 0x9f940, called at
// 0x9c924 / 0x9c954) now start no sound; their effects still run in his view. His own gun and steps come from his playerstate's events (other calls), untouched.
// [native] evsnd1=0 off.
static void (*ev_check_original[2])(int, uint8_t *);
static void ev_check_run(int k, int lc, uint8_t *cent)
{
	const int mute = ctx_current == 1 && native_active;
	if (mute)
		snd_mute = 1;
	ev_check_original[k](lc, cent);
	if (mute)
		snd_mute = 0;
}
// EXPERIMENT, off ([native] dynpush1=1 on). The push of the world's movable objects by an explosion (rva 0xf53c0, from the event handler 0x9f9a0 and 0xc2410: it wakes the
// dynamic entities near the blast and makes a physics object for each, 0x1e5de0 -> 0x1e3120, from a shared pool). The dynamic entities and their physics are one state for
// the whole game, but both contexts run every event, so each explosion may wake the same objects twice (the 360 runs each event once). With this on, context 1 skips the
// push. It was tried 2026-11-17 as the cause of the Gulag "cafe" crash at rva 0x2713ee (read of -1 taking a node from that pool, context 1 live, 3 s after the split
// began): the crash stayed, so it is not; and how the objects move with the skip was not checked in a level.
void *dynpush_orig;
int dynpush_skip(void)
{
	return ctx_current == 1 && native_active && CFG_ONCE("dynpush1", 0);
}
extern char dynpush_stub[];
__asm__(".text\n.globl dynpush_stub\ndynpush_stub:\n"
	"\tpush %rcx\n"
	"\tpush %rdx\n"
	"\tpush %r8\n"
	"\tpush %r9\n"
	"\tsub $0x68, %rsp\n" // (4 pushes + 0x68: the stack stays 16-aligned at the call; 0x20 shadow, then the four xmm)
	"\tmovdqu %xmm0, 0x20(%rsp)\n"
	"\tmovdqu %xmm1, 0x30(%rsp)\n"
	"\tmovdqu %xmm2, 0x40(%rsp)\n"
	"\tmovdqu %xmm3, 0x50(%rsp)\n"
	"\tcall dynpush_skip\n"
	"\tmovdqu 0x20(%rsp), %xmm0\n"
	"\tmovdqu 0x30(%rsp), %xmm1\n"
	"\tmovdqu 0x40(%rsp), %xmm2\n"
	"\tmovdqu 0x50(%rsp), %xmm3\n"
	"\tadd $0x68, %rsp\n"
	"\tpop %r9\n"
	"\tpop %r8\n"
	"\tpop %rdx\n"
	"\tpop %rcx\n"
	"\ttest %eax, %eax\n"
	"\tjnz 1f\n"
	"\tjmp *dynpush_orig(%rip)\n"
	"1:\tret\n"
	);
static void ev_check_hook0(int lc, uint8_t *cent)
{
	ev_check_run(0, lc, cent);
}
static void ev_check_hook1(int lc, uint8_t *cent)
{
	ev_check_run(1, lc, cent);
}
// The sounds of effects (an explosion's debris, fire, sparks: the effects system's sound elements, 0x11be20 -> 0xa6280) go into the engine's queue of pending sounds
// (rva 0x237a70: up to 32 entries of alias, entity, origin at 0x1a2d270, count 0x1a2cf58), played and emptied by each draw (0x23a1f0 from 0xbca00). With an effects
// system of its own in each context ([native] fxsplit=1) every effect's sound was queued twice (measured: the queue played 33 / 36 sounds a second in the two contexts). Context 1's are
// not queued (evsnd1). The function is replaced whole (its first instruction is rip-relative): the same ten lines otherwise.
static void fx_sound_queue_hook(void *alias, int ent, const uint8_t *origin)
{
	if (ctx_current == 1 && native_active)
		return;
	uint32_t *count = (uint32_t *)(base + XR(0x1a2cf58));
	if (*count == 0x20)
		return;
	uint8_t *e = base + XR(0x1a2d270) + (size_t)*count * 0x18;
	(*count)++;
	*(void **)e = alias;
	*(int *)(e + 8) = ent;
	memcpy(e + 0xc, origin, 12);
}
// the aliases' start counts of the last second, per context ([native] sndcount=1: totals, 2: names too)
void snd_note(const char **alias, uintptr_t ret)
{
	int c = ctx_current & 1;
	InterlockedIncrement(&snd_counts[c]);
	if (CFG_ONCE("sndcount", 0) >= 3) {
		const uint32_t r = (uint32_t)(ret - (uintptr_t)base);
		for (int i = 0; i < 64; i++) {
			if (snd_callers[i].rva == r) {
				snd_callers[i].n[c]++;
				break;
			}
			if (!snd_callers[i].rva) {
				snd_callers[i].rva = r;
				snd_callers[i].n[c] = 1;
				break;
			}
		}
	}
	const char *name = alias ? *alias : NULL;
	if (!name || CFG_ONCE("sndcount", 0) < 2)
		return;
	for (int i = 0; i < 512; i++) {
		if (snd_names[i].name == name) {
			snd_names[i].n[c]++;
			return;
		}
		if (!snd_names[i].name) {
			snd_names[i].name = name;
			snd_names[i].n[c] = 1;
			return;
		}
	}
}
#endif
// 0xbd130 is the cgame's SND_SetListener(local client, ...) (rva 0x23db90): the engine keeps two listeners (rva 0x1a28f48, 0x3c bytes each; every sound is heard by the nearer one, like on the console's split screen).
// Context 1 runs as local client 0, so both contexts set listener 0 and the hearing jumped between the players every frame (constant judder of the sound); context 1 sets listener 1.
static void hud_x4_hook(void *a, void *b, void *c, void *d, void *e, void *f)
{
	if (ctx_current == skip_ctx && (skip_hud_mask & 0x100))
		return;
	if (native_active && ctx_current == 1 && CFG_ONCE("snd2", 1))
		a = (void *)(uintptr_t)1;
	hud_x_original[4](a, b, c, d, e, f);
}
#ifndef _WIN64
// The same for the 32-bit build: the call at rva 0x66a20 in the cgame draw (SND_SetListener at rva 0x47b70, cdecl: local client first)
static void (*snd_listener_original)(int, int, void *, void *, void *);
static void snd_listener_hook(int lc, int a, void *origin, void *axis, void *velocity)
{
	static int calls;
	if (native_active && ctx_current == 1 && CFG_ONCE("snd2", 1) == 2) // QA ([native] snd2=2): player 2 sets no listener, only player 1's place hears (is the second listener what the owner hears tremble?)
		return;
	if (native_active && ctx_current == 1 && CFG_ONCE("snd2", 1))
		lc = 1;
	if (CFG_ONCE("sndlog", 0) >= 2) { // QA: every call for a while (does a listener jump between two places from one frame to the next?)
		static int every;
		if (++every >= 3000 && every < 3240)
			log_printf("native: listener #%d lc %d ctx %d p2frame %d origin %.1f %.1f %.1f", every, lc, ctx_current, p2_frame, (double)((float *)origin)[0], (double)((float *)origin)[1], (double)((float *)origin)[2]);
	}
	if (calls < 400 && CFG_ONCE("sndlog", 0) && (calls++ % 50) == 0)
		log_printf("native: sound listener call %d: local client %d, context %d, origin %.0f %.0f %.0f", calls, lc, ctx_current, (double)((float *)origin)[0], (double)((float *)origin)[1], (double)((float *)origin)[2]);
	snd_listener_original(lc, a, origin, axis, velocity);
}
#endif
HUD_X(5, 0x200) // 0xbd1a4 11a0b0
HUD_X(6, 0x400) // 0xbd206 c8600

// Diagnostics ([native] entcleanlog=1): the cgame's cleanup of removed entities (rva 0xb9320; the flags are the bit set at rva 0x52c300) logs which entities it frees and for which context
static const uint8_t ent_clean_bytes[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20};
static void (*ent_clean_original)(void);
static void ent_clean_hook(void)
{
	static int logged;
	if (logged < 60 && CFG_ONCE("entcleanlog", 0)) {
		uint32_t *f = (uint32_t *)(base + RVA_ENT_FLAGS);
		char line[400];
		int n = snprintf(line, sizeof(line), "native: entity cleanup in context %d, flags:", ctx_current);
		int any = 0;
		for (int i = 0; i < 64 && n < (int)sizeof(line) - 24; i++)
			if (f[i]) {
				n += snprintf(line + n, sizeof(line) - n, " [%d]=%08x", i, f[i]);
				any = 1;
			}
		if (any) {
			logged++;
			log_printf("%s", line);
		}
	}
	if (ctx_current == 1) { // both contexts process every snapshot: the cleanup frees the engine's shared per-entity objects (the DObjs), so only context 0 may do it (a second free of the same entity crashed in the pool free).
		// The entities flagged for it are NOT forgotten (as they were: the flags were cleared here): a client DObj made by a transition is only the pending one until the cleanup of its flag
		// makes it the current one (rva 0x1fec30), and a flag wiped before context 0's cleanup ran left the entity without a client DObj for good: not drawn (Price on Contingency, in some runs).
		// Context 0's next cleanup takes them. [native] cleanwipe=1 clears them as before.
		if (CFG_ONCE("cleanwipe", 0)) {
			uint32_t *f = (uint32_t *)(base + RVA_ENT_FLAGS);
			memset(f, 0, 64 * sizeof(uint32_t));
		}
		// The server hand-over happens in whichever view's client frame finds a new snapshot due (a flag the first one clears), and the transition that follows it (this function, called
		// from rva 0xb8d30 right after the hand-over) used to run only when that view was context 0: for about every other snapshot the entities (the shared ones are written by it
		// alone) stayed where they were and the snapshot after it was taken in a double step, while the interpolation clock ran on: the actors snapped back and lurched (rubber banding).
		// Now context 0's transition runs at every hand-over, from context 1's frame too ([native] transfix=0 off).
		// (64-bit: the call at rva 0xb8d55 in 0xb8d30; 32-bit: the call at rva 0x59d71, the one after the server step call 0x59d6c)
#ifdef _WIN64
		if (CFG_ONCE("transfix", 1) && ctx1_ready && split_on && __builtin_return_address(0) == (void *)(base + XR(0xb8d5a))) {
#else
		if (CFG_ONCE("transfix", 1) && ctx1_ready && split_on && __builtin_return_address(0) == (void *)(base + XR(0x59d71) + 5)) {
#endif
			transfix_runs++;
			ctx_swap(0);
			ent_clean_hook();
			ctx_swap(1);
			return;
		}
		// The animation transfer queue: every client frame the engine saves the animation state of every DObj it frees (about 540 of them, each holding a reference to the string of its
		// notify name) into a global queue, and the start of this function applies the queue (restoring the states, giving the strings back); the caller empties the queue right after.
		// Context 1 used to skip this whole function, so the empty-the-queue that follows it in its frame threw away what context 0 had saved: the animation states of all DObjs were lost
		// (the actors' animation "drawdown") and the references of the strings never given back (a string's count is 16 bits: it wrapped after some 35 s in a busy level, the string was
		// freed while it was in use and its next release froze or crashed the game: the failure of the Roadkill split screen). Only the apply is run here ([native] xferfix=0 off).
		if (CFG_ONCE("xferfix", 1) && ctx1_ready && split_on)
			((void (*)(void))(base + RVA_XFER_APPLY))();
		return;
	}
#ifdef _WIN64
	// The cleanup also carries the effects attached to an entity over to its new DObj (a weapon change makes a new one) or kills them: rva 0x115110 saves the old DObj's models and tags
	// before the old one is freed, rva 0x1151d0 re-attaches (or kills) afterwards, in the live effects system, which is context 0's. Context 1 has a system of its own (fxsplit) that
	// never got this: the muzzle flash of a body whose weapon was changed stayed on it for ever in the other half. The same two calls are made for context 1's system.
	if (native_active && ctx1_ready && ctx_store[1][RANGE_FX] && CFG_ONCE("fxsplit", 0) && CFG_ONCE("fxentfix", 1)) {
		struct {
			uint8_t buf[528];
			int ent, flag;
			unsigned short key;
		} saved[64];
		int count = 0;
		const uint32_t *flags = (const uint32_t *)(base + RVA_ENT_FLAGS);
		const uint8_t *(*client_dobj)(int) = (const uint8_t *(*)(int))(base + RVA_CLIENT_DOBJ);
		for (int w = 0; w < 64 && count < 64; w++)
			for (int bit = 0; bit < 32 && count < 64; bit++)
				if (flags[w] & (0x80000000u >> bit)) {
					int n = w * 32 + bit;
					const uint8_t *slot = base + RVA_CENT + (size_t)n * CENT_SIZE;
					saved[count].ent = n;
					saved[count].flag = *(const int *)(slot + 0x16c) == 0xffffff;
					saved[count].key = *(const unsigned short *)(slot + 0x164);
					((void (*)(void *, const uint8_t *, uint8_t, uint16_t))(base + XR(0x115110)))(saved[count].buf, client_dobj(n), (uint8_t)saved[count].flag, saved[count].key);
					count++;
				}
		ent_clean_original();
		if (count) {
			ctx_swap(1);
			for (int i = 0; i < count; i++)
				((void (*)(void *, int, int, const uint8_t *, char, short))(base + XR(0x1151d0)))(saved[i].buf, 0, saved[i].ent, client_dobj(saved[i].ent), (char)saved[i].flag, (short)saved[i].key);
			ctx_swap(0);
		}
		return;
	}
#endif
	ent_clean_original();
}

// The first-person weapon's animation blend (rva 0xc6ee0, called at 0xc7499 from the HUD's weapon part): its tree sits behind a pointer of the cgame block (rva 0x440730 + 0x20 * client) that is
// null while a context has no view model yet (right after a weapon change in context 1: a read at null + 8, the crash of the first test of a script's SwitchToWeapon); the blend waits a frame then.
static void (*viewmodel_blend_original)(float, void *, int);
static void viewmodel_blend_hook(float a, void *tree, int which)
{
	if (native_active && (!tree || !*(void **)tree))
		return;
	viewmodel_blend_original(a, tree, which);
}

// The first-person weapon's setup (rva 0xc6480, called at 0xc41b1, 0xc58ed, 0xc594a, 0xc5dfb, 0xc6033) makes its DObjs under the handles 0x800 + 2 * local client + hand, a pool both contexts
// would share (context 1 runs as local client 0): a weapon change in one freed what the other still pointed to (the crash at the first change of a weapon). Context 1's calls are made for
// local client 1 (handles 0x802 and 0x803: the table has room for four local clients), and its copy of the cgame's pointers to the view model is cleared when it starts (ctx_after_draw).
static void (*viewmodel_setup_original)(int, void *, unsigned, void *, char, char);
static void viewmodel_setup_hook(int lc, void *a, unsigned weapon, void *b, char c, char d)
{
	viewmodel_setup_original(native_active && ctx_current == 1 ? 1 : lc, a, weapon, b, c, d);
}

// The sound listener (rva 0x6a570, called at 0xbd2c5 near the end of the cgame draw) takes the camera's position and axes of local client 0: both contexts would set it, the hearing would jump between
// the players every frame. Only context 0's player is heard.
// The call at rva 0xbd2c5 is the aim assist's screen target update (rva 0x6a570: eye, view origin, view angles), not the sound listener it was once taken for: it runs
// in both contexts, each into its own copy of aaGlob (a swapped range). Its target list (rva 0x6b230, called at 0x6a711) is the server client's of local client 0 in
// both: context 1 gets client 1's.
static void (*screen_targets_original)(int, void *, void *, void *, unsigned, float);
static void screen_targets_hook(int lc, void *eye, void *origin, void *angles, unsigned a, float b)
{
	screen_targets_original(lc, eye, origin, angles, a, b);
}
static void (*target_list_original)(int, void **, int *);
static void target_list_hook(int lc, void **list, int *count)
{
	target_list_original(native_active && ctx_current == 1 ? 1 : lc, list, count);
}

// The server side of the aim targets: the PC build no longer runs AimTarget_ProcessEntity (rva 0x6b5a0) for each entity, so the lists the cgame takes its screen
// targets from (aimTargetGlob, rva 0x3d0530, 0x210c bytes per client: the list being built at +0 with its count at +0xb00, the one read at +0x1608 with its count
// at +0x2108, 0x2c bytes a target) stay empty. After each simulation step: each client's list cleared, every entity that ProcessEntity would take checked for
// both clients (an enemy the client can see goes into his list, rva 0x6b630 -> 0x6a9a0; ProcessEntity's own loop asks rva 0x6bae0 for the number of clients,
// which reads the client code's copy of the local player count, 1), and the lists published. g_entities: rva 0xd9e070, 0x2a0 bytes each: in use +0xac / +0xb0,
// type +0 (13 an actor: health +0x164, sentient +0x118 with its team at +8), eFlags +0xc, +0x8c, the clients' "seen" bits 0x80000 << client at +0x150;
// level.num_entities at rva 0xd96e80.
#ifndef _WIN64
static void aim_for_client32(int c, uint8_t *ent)
{
	__asm__ volatile("pushl %1\n\tcall *%2\n\taddl $4, %%esp" : "+a"(ent) : "r"(c), "m"(aim_for_client_fn) : "ecx", "edx", "memory", "cc");
}
#define AIM_FOR_CLIENT32(c, ent) aim_for_client32(c, ent)
#endif
static void aim_targets_update(void)
{
	if (!CFG_ONCE("aimtargets", 1))
		return;
	uint8_t *glob = base + RVA_AIM_TARGET_GLOB;
	int count = *(int *)(base + RVA_LEVEL_NUM_ENT);
	if (count <= 0 || count > 2048)
		count = 2048;
	for (int c = 0; c < 2; c++)
		*(int *)(glob + c * 0x210c + 0xb00) = 0;
	// Only living actors of another team than player 1's; and the check's "seen" bits are kept here, not in the entity: this PC build is not known to leave those
	// bits of +0x150 alone (with them written for every entity, Price was not drawn on Contingency, owner 2026-10-28), so the entity gets ours for the call and its
	// own back after it
	static uint32_t seen[2048];
	const uint8_t *own = *(uint8_t **)(base + RVA_GENTITIES + GENT_SENTIENT); // player 1's sentient: his team at +8 (32 bits +4)
	const int team = own ? *(const int *)(own + SENT_TEAM) : -1;
#ifdef _WIN64
	void (*for_client)(int, uint8_t *) = (void (*)(int, uint8_t *))(base + XR(0x6b630));
#else
	// 32 bits: the per-client check takes the gentity in EAX and the client on the stack
#define for_client(c, ent) AIM_FOR_CLIENT32(c, ent)
#endif
	for (int i = 0; i < count; i++) {
		uint8_t *ent = base + RVA_GENTITIES + (size_t)i * GENT_SIZE;
		// Most slots are empty or are not actors. Do not fetch their sentient pointer until these
		// cheap byte checks pass; this loop scans the whole entity table after every server step.
		if (!ent[0xac] || !ent[0xb0] || ent[0] != 13) {
			seen[i] = 0;
			continue;
		}
		const uint8_t *sentient = *(uint8_t **)(ent + GENT_SENTIENT);
		if (CFG_ONCE("aalog", 0) && sentient && i < 2048) { // QA: the teams of the actors seen, once each
			static unsigned char logged[8];
			int tm = *(const int *)(sentient + SENT_TEAM) & 7;
			if (!logged[tm]) {
				logged[tm] = 1;
				log_printf("native: aim targets: an actor of team %d (entity %d), player 1's team %d", tm, i, team);
			}
		}
		// only the player's enemies (the axis team, 1): an ally in the list ran the check on him, and Price was not drawn (owner, Contingency, 2026-10-29)
		if (*(int *)(ent + GENT_HEALTH) <= 0 || !sentient || *(const int *)(sentient + SENT_TEAM) != 1) {
			seen[i] = 0;
			continue;
		}
		uint32_t *flags = (uint32_t *)(ent + GENT_SEEN), keep = *flags;
		*flags = (keep & ~0x180000u) | seen[i];
		for (int c = 0; c < 2; c++)
			for_client(c, ent);
		seen[i] = *flags & 0x180000u;
		*flags = keep;
	}
	for (int c = 0; c < 2; c++) {
		uint8_t *list = glob + c * 0x210c;
		int n = *(int *)(list + 0xb00);
		if (n < 0 || n > 64)
			n = 0;
		memcpy(list + 0x1608, list, (size_t)n * 0x2c);
		*(int *)(list + 0x2108) = n;
	}
	static DWORD logged;
	if (CFG_ONCE("aalog", 0) && GetTickCount() - logged >= 1000) {
		logged = GetTickCount();
		log_printf("native: aim targets: %d entities, client 0 %d, client 1 %d", count, *(int *)(glob + 0x2108), *(int *)(glob + 0x210c + 0x2108));
	}
}

// Split screen: the sound mixer is one for both views. Its environment effects (the reverb room per priority) and its channel volumes (per priority) are set every frame
// by the cgame of a player in a state that has them (the shell shock sound state, 32-bit VA 0x56deb0: environment priority 2 and channel volumes priority 4; the remote
// camera, VA 0x570f00: priority 3) and deactivated every frame by the other view's cgame, whose player is not in that state. Second Sun's opening (player 1 dazed, "hands
// up"): about 90 sets and 90 deactivations of priority 2 a second, so the mixer flipped between the muffled and the clear sound every frame (owner: "звук лагает", gone once
// the hands go down; envlog QA 2026-11-17). Rule: a priority stays as the views that hold it want it. A view holds a priority while it sets it, and while it fades it out
// (a deactivation with a fade time: the dazed player coming to, while the other view deactivated it at once every frame and the fade was lost). A view's deactivation is
// dropped while the other view set the priority in the last 250 ms or fades it out; when both set it, or both fade it out, player 1's calls win. Single player passes
// everything. [native] mixor=0 off; envlog=1 logs the calls that went through and the dropped ones every 5 s.
enum { MIX_ENV, MIX_VOL };
static void (*env_set_original)(int, const char *, float, float, int);
static void (*env_off_original)(int, int);
static void (*vol_set_original)(int, const void *, int);
static void (*vol_off_original)(int, int);
static DWORD mix_set_at[2][2][8], mix_fade_at[2][2][8]; // [kind][context][priority]: the tick of the view's last set / fading deactivation, 0 once it stopped holding
static unsigned mix_n[2][4][8], mix_dropped[2];		 // envlog: [context][environment set, off, volumes set, off][priority] passed; dropped per context
static void mix_report(void)
{
	static DWORD at;
	if (!at)
		at = GetTickCount();
	if (GetTickCount() - at < 5000)
		return;
	at = GetTickCount();
	static const char *const what[4] = {"env set", "env off", "vol set", "vol off"};
	char line[600];
	size_t n = (size_t)snprintf(line, sizeof(line), "native: envlog 5 s: dropped ctx0 %u ctx1 %u;", mix_dropped[0], mix_dropped[1]);
	for (int c = 0; c < 2; c++)
		for (int k = 0; k < 4; k++)
			for (int p = 0; p < 8; p++)
				if (mix_n[c][k][p] && n < sizeof(line))
					n += (size_t)snprintf(line + n, sizeof(line) - n, " ctx%d %s %d: %u", c, what[k], p, mix_n[c][k][p]);
	log_printf("%s", line);
	memset(mix_n, 0, sizeof(mix_n));
	memset(mix_dropped, 0, sizeof(mix_dropped));
}
static int mix_recent(DWORD at, DWORD now)
{
	return at && now - at < 250;
}
// set: 1 for a set, 0 for a deactivation with this fade time (ms)
static int mix_pass(int kind, int prio, int set, int fade)
{
	const int me = ctx_current == 1;
	const unsigned p = (unsigned)prio & 7;
	int pass = 1;
	if (native_active && (unsigned)prio < 8 && CFG_ONCE("mixor", 1)) {
		const DWORD now = GetTickCount(), stamp = now ? now : 1;
		const int other_sets = mix_recent(mix_set_at[kind][!me][p], now), other_fades = mix_recent(mix_fade_at[kind][!me][p], now);
		if (set) {
			mix_set_at[kind][me][p] = stamp, mix_fade_at[kind][me][p] = 0;
			pass = !(me && other_sets);
		} else {
			mix_set_at[kind][me][p] = 0, mix_fade_at[kind][me][p] = fade > 0 ? stamp : 0;
			pass = !other_sets && !(other_fades && (me || fade <= 0));
		}
	}
	if (CFG_ONCE("envlog", 0)) {
		if (pass)
			mix_n[me][kind * 2 + !set][p]++;
		else
			mix_dropped[me]++;
		mix_report();
	}
	return pass;
}
static void mix_level_load(void)
{
	memset(mix_set_at, 0, sizeof(mix_set_at));
	memset(mix_fade_at, 0, sizeof(mix_fade_at));
}
static void env_set_hook(int prio, const char *room, float dry, float wet, int fade)
{
	if (mix_pass(MIX_ENV, prio, 1, fade))
		env_set_original(prio, room, dry, wet, fade);
}
static void env_off_hook(int prio, int fade)
{
	if (mix_pass(MIX_ENV, prio, 0, fade))
		env_off_original(prio, fade);
}
static void vol_set_hook(int prio, const void *volumes, int fade)
{
	if (mix_pass(MIX_VOL, prio, 1, fade))
		vol_set_original(prio, volumes, fade);
}
static void vol_off_hook(int prio, int fade)
{
	if (mix_pass(MIX_VOL, prio, 0, fade))
		vol_off_original(prio, fade);
}
// The vehicles' client state (rva 0x280fb0, called by the vehicle draw 0x280860 at 0x2808c1; 32 bits: VA 0x662730 from VA 0x42771a, cdecl): one block per vehicle for
// the whole game (rva 0x1b762f0, 64 x 0x80; VA 0x1b99848): the time since its last update, its velocity, the tread scroll and the engine sound's state machine (0x27efb0:
// the idle, engine and rev aliases started and stopped through 0x27ec00). Both views draw every vehicle, so both ran the update in every frame: the second one with the
// time since the first (0, or less than 0 when player 2's client time was behind), and the engine sounds started a second time (QA 2026-11-21, Cliffhanger's
// snowmobiles, sndcount=2: snowmobile_1_idle 642 / 642 starts a second in contexts 0 / 1; Spec Ops engine audit row I). Context 1 skips it: the state is the world's,
// player 1's view keeps it and player 2's draw reads it. [native] vehupd1=0 off.
// 32 bits: the call site (VA 0x42771a) lies in 0x418000-0x428000, which the official exe hashes when a turret is used (Turret_Use VA 0x42c911 -> 0x4ca740) and quits
// with 0x8000dead when it differs: in the split screen every turret ended the game (the Barrett of Of Their Own Accord, QA barrettpad 2026-11-24, issue 28). The
// callee (VA 0x662730) is hooked instead, and only its call from that site is held back.
static void (*veh_update_original)(int, void *);
static uint8_t *veh_update_ret; // the vehicle draw's return address after its call
static void veh_update_hook(int lc, void *cent)
{
	if (ctx_current == 1 && native_active && CFG_ONCE("vehupd1", 1) && __builtin_return_address(0) == (void *)veh_update_ret)
		return;
	veh_update_original(lc, cent);
}
static void veh_update_install(void)
{
#ifdef _WIN64
	uint8_t *at = base + XR(0x2808c1), *fn = base + XR(0x280fb0);
	veh_update_ret = at + 5;
	const int ok = at[0] == 0xe8 && at + 5 + *(const int32_t *)(at + 1) == fn && jhook_call(at, (void *)veh_update_hook, (void **)&veh_update_original);
#else
	uint8_t *at = base + XR(0x2771a), *fn = base + XR(0x262730);
	static const uint8_t head[10] = {0x8b, 0x44, 0x24, 0x04, 0x53, 0x55, 0x8b, 0x6c, 0x24, 0x10}; // mov eax,[esp+4]; push ebx; push ebp; mov ebp,[esp+0x10]
	veh_update_ret = at + 5;
	const int ok = at[0] == 0xe8 && at + 5 + *(const int32_t *)(at + 1) == fn && jhook_function(fn, head, sizeof(head), (void *)veh_update_hook, (void **)&veh_update_original);
#endif
	log_printf("native: the vehicles' client state and engine sounds %s", ok ? "are updated once a frame (context 0)" : "(the call at the vehicle draw) are not as expected: both views update them");
}
#ifdef _WIN64
// The entities' looping sounds in 64 bits (the 32-bit loopsnd1 below; owner 2026-11-17: on the 64-bit split screen a sound "like an ADSL connection" in Second Sun, not on
// 32 bits). The entity pass rva 0x9c960 calls the entity loop sound 0xa6f90 (entity, origin, alias name) for every emitter in every frame, and the entity type draw 0x9ea20
// the blended loops (0x23a060 at 0x9ee46), and the vehicles' engine sound 0x27eae0 their blended engine loops (0x23a060 at 0x27ebcd: idle and engine aliases, the
// lerp and pitch from the vehicle's speed), in both contexts: sndcount=3 on Second Sun counted 11500 calls a second of the play helper 0x23a880 and 2900 of the blend from
// each view, the same numbers in both (the falling helicopters of the opening: mi17_idle_low / mi17_engine_low 800 a second from each). One voice updated by both views
// in every frame, each with its own lerp and pitch, warbles at the frame rate (the "ADSL" sound; muting the emitters alone did not end it, owner). Context 1's calls return
// "no sound": the engine has a listener for each player and plays every sound to the nearer one, and in mode 11 the views share the entities. [native] loopsnd1=0 off.
static int (*ent_loop64_original)(int, const float *, const char *);
static int ent_loop64_hook(int ent, const float *origin, const char *alias)
{
	if (ctx_current == 1 && native_active && CFG_ONCE("loopsnd1", 1))
		return -1;
	return ent_loop64_original(ent, origin, alias);
}
static int (*blend64_original)(const void *, const void *, float, float, int, const float *, int, int);
static int blend64_hook(const void *alias0, const void *alias1, float lerp, float volume, int ent, const float *origin, int timeshift, int system)
{
	if (ctx_current == 1 && native_active && CFG_ONCE("loopsnd1", 1))
		return -1;
	return blend64_original(alias0, alias1, lerp, volume, ent, origin, timeshift, system);
}
// QA ([native] stoplog=1): who stops sound channels while player 2's view runs (the channel stop functions rva 0x2afd70 and 0x2ad1b0): the call stacks, as rvas, every 5 s.
static uintptr_t (*chan_stop_original[2])(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static struct {
	uint32_t rva[4];
	unsigned n;
} stop_stacks[24];
static void stop_note(int k)
{
	void *frames[6] = {0};
	const USHORT got = RtlCaptureStackBackTrace(2, 6, frames, NULL);
	uint32_t r[4] = {0};
	for (int i = 0; i < 4 && i < got; i++)
		r[i] = (uintptr_t)frames[i] >= (uintptr_t)base && (uintptr_t)frames[i] < (uintptr_t)base + 0x5000000 ? (uint32_t)((uint8_t *)frames[i] - base) : 0xffffffffu; // raw-ok: an image bound for a QA log
	r[3] = (r[3] & 0x0fffffffu) | (uint32_t)k << 28;
	for (int i = 0; i < 24; i++) {
		if (stop_stacks[i].n && !memcmp(stop_stacks[i].rva, r, sizeof(r))) {
			stop_stacks[i].n++;
			break;
		}
		if (!stop_stacks[i].n) {
			memcpy(stop_stacks[i].rva, r, sizeof(r));
			stop_stacks[i].n = 1;
			break;
		}
	}
	static DWORD at;
	if (!at)
		at = GetTickCount();
	if (GetTickCount() - at < 5000)
		return;
	at = GetTickCount();
	for (int i = 0; i < 24 && stop_stacks[i].n; i++)
		log_printf("native: stoplog 5 s: ctx1 stop%d x%u from 0x%x <- 0x%x <- 0x%x", (int)(stop_stacks[i].rva[3] >> 28), stop_stacks[i].n, stop_stacks[i].rva[0], stop_stacks[i].rva[1],
			   stop_stacks[i].rva[2]);
	memset(stop_stacks, 0, sizeof(stop_stacks));
}
static uintptr_t chan_stop0_hook(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
	if (ctx_current == 1 && native_active)
		stop_note(0);
	return chan_stop_original[0](a, b, c, d);
}
static uintptr_t chan_stop1_hook(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
	if (ctx_current == 1 && native_active)
		stop_note(1);
	return chan_stop_original[1](a, b, c, d);
}
// The sound frame (rva 0xd5190: the stop pass 0x23f4c0, then SND_Update 0x23ed90) stops every looping voice that nobody refreshed since the last sound frame (each refresh
// stamps the voice with the frame mark 0x1a28fc4) and runs once in each client frame. Player 2's client frame ran it too, so there were two sound frames in every engine
// frame, and a loop that player 1's view refreshes (fire, the dazed player's shellshock_loop, a plane's flyby) died in player 2's and started again in player 1's: a voice
// restarted about 100 times a second, the "ADSL" warble on the 64-bit split screen (owner 2026-11-17, Second Sun; stronger as a plane flew over). flipwatch saw the voice
// slot taken after player 1's draw and free after player 2's in every frame; stoplog saw player 2's view stop it from 0xd5190. Player 2's client frame skips it now: one
// sound frame per engine frame, as in single player. [native] sndframe1=0 off.
static void (*snd_frame_original)(void);
static void snd_frame_hook(void)
{
	if (ctx_current == 1 && native_active && CFG_ONCE("sndframe1", 1))
		return;
	snd_frame_original();
}
static void snd_frame_install64(void)
{
	const uint32_t calls[4] = {XR(0x634b), XR(0x11a8e), XR(0x11b89), XR(0xd5920)};
	int n = 0;
	for (int i = 0; i < 4; i++) {
		uint8_t *at = base + calls[i];
		if (calls[i] && at[0] == 0xe8 && at + 5 + *(const int32_t *)(at + 1) == base + XR(0xd5190) && jhook_call(at, (void *)snd_frame_hook, (void **)&snd_frame_original))
			n++;
	}
	log_printf("native: the sound frame runs once per engine frame (player 1's client frame): %d of 4 calls", n);
}
static void ent_loops_install64(void)
{
	uint8_t *a = base + XR(0x9cc2f), *b = base + XR(0x9ee46), *v = base + XR(0x27ebcd);
	const int ok_a = a[0] == 0xe8 && a + 5 + *(const int32_t *)(a + 1) == base + XR(0xa6f90) && jhook_call(a, (void *)ent_loop64_hook, (void **)&ent_loop64_original);
	const int ok_b = b[0] == 0xe8 && b + 5 + *(const int32_t *)(b + 1) == base + XR(0x23a060) && jhook_call(b, (void *)blend64_hook, (void **)&blend64_original);
	const int ok_v = v[0] == 0xe8 && v + 5 + *(const int32_t *)(v + 1) == base + XR(0x23a060) && jhook_call(v, (void *)blend64_hook, (void **)&blend64_original);
	if (cfg("stoplog", 0)) {
		static const uint8_t b0[15] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x40};
		static const uint8_t b1[15] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xec, 0x40};
		log_printf("native: stoplog: %d %d", jhook_function(base + XR(0x2afd70), b0, sizeof(b0), (void *)chan_stop0_hook, (void **)&chan_stop_original[0]),
			   jhook_function(base + XR(0x2ad1b0), b1, sizeof(b1), (void *)chan_stop1_hook, (void **)&chan_stop_original[1]));
	}
	log_printf("native: the entities' looping sounds %s, their blended loops %s, the vehicles' engine loops %s", ok_a ? "start once (context 0)" : "are not hooked (rva 0x9cc2f)",
		   ok_b ? "once" : "not hooked (rva 0x9ee46)", ok_v ? "once" : "not hooked (rva 0x27ebcd)");
}
#endif
#ifdef _WIN64
// the 64-bit mixer functions (the shell shock sound state rva 0xb7de0 calls all four; environment set 0x23d140, channel volumes set 0x23d050, environment deactivate
// 0x2381a0). The channel volumes' deactivation 0x2380b0 starts with a rip-relative test, so its six callers (the cgame's sound states) are hooked instead.
static void mix_install64(void)
{
	static const uint8_t env_set_bytes[14] = {0x40, 0x57, 0x48, 0x83, 0xec, 0x40, 0x0f, 0x29, 0x74, 0x24, 0x30, 0x0f, 0x28, 0xf3}; // push rdi; sub rsp,0x40; movaps [rsp+0x30],xmm6; movaps xmm6,xmm3
	static const uint8_t vol_set_bytes[14] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x63, 0xd9, 0x4c, 0x8b, 0xca, 0x8d, 0x43, 0xff}; // mov [rsp+8],rbx; movsxd rbx,ecx; mov r9,rdx; lea eax,[rbx-1]
	static const uint8_t env_off_bytes[16] = {0x40, 0x57, 0x48, 0x83, 0xec, 0x20, 0x44, 0x8d, 0x41, 0xff, 0x8b, 0xfa, 0x41, 0x83, 0xf8, 0x02}; // push rdi; sub rsp,0x20; lea r8d,[rcx-1]; mov edi,edx; cmp r8d,2
	const uint32_t vol_off_calls[6] = {XR(0xb4d1d), XR(0xb54ea), XR(0xb7ca1), XR(0xb7d39), XR(0xb7ed2), XR(0xc12ca)}; // (XR literals: the signature generator collects them)
	const int a = jhook_function(base + XR(0x23d140), env_set_bytes, sizeof(env_set_bytes), (void *)env_set_hook, (void **)&env_set_original);
	const int b = a && jhook_function(base + XR(0x2381a0), env_off_bytes, sizeof(env_off_bytes), (void *)env_off_hook, (void **)&env_off_original);
	int c = jhook_function(base + XR(0x23d050), vol_set_bytes, sizeof(vol_set_bytes), (void *)vol_set_hook, (void **)&vol_set_original), sites = 0;
	if (c)
		for (size_t i = 0; i < 6; i++) {
			uint8_t *at = base + vol_off_calls[i];
			if (vol_off_calls[i] && at[0] == 0xe8 && at + 5 + *(const int32_t *)(at + 1) == base + XR(0x2380b0) && jhook_call(at, (void *)vol_off_hook, (void **)&vol_off_original))
				sites++;
		}
	log_printf("native: the sound mixer's environment %s, its channel volumes %s (deactivations at %d of 6 calls)", b ? "is held while either view holds it" : "is not hooked (rva 0x23d140 / 0x2381a0)",
		   c ? "too" : "are not hooked (rva 0x23d050)", sites);
}
#endif

#ifndef _WIN64
// ---- 32 bits: the stubs the register-convention hooks go through, and the wrappers that call the originals (a C function cannot receive ESI or EAX/EBX)
void n32_mouse_hook(intptr_t state, float *x, float *y);
void n32_mouse2_hook(intptr_t state, float *x, float *y);
uint8_t *n32_make_cmd_hook(uint8_t *out, int lc);
void n32_mouse_hook(intptr_t state, float *x, float *y)
{
	mouse_read_hook(state, x, y);
}
void n32_mouse2_hook(intptr_t state, float *x, float *y)
{
	mouse_read2_hook(state, x, y);
}
uint8_t *n32_make_cmd_hook(uint8_t *out, int lc)
{
	return make_cmd_hook(out, lc);
}
__asm__(".text\n"
	".globl _n32_mouse_stub\n"
	"_n32_mouse_stub:\n"
	"	pushl 8(%esp)\n"
	"	pushl 8(%esp)\n"
	"	pushl %esi\n"
	"	call _n32_mouse_hook\n"
	"	addl $12, %esp\n"
	"	ret\n"
	".globl _n32_mouse2_stub\n"
	"_n32_mouse2_stub:\n"
	"	pushl 8(%esp)\n"
	"	pushl 8(%esp)\n"
	"	pushl %esi\n"
	"	call _n32_mouse2_hook\n"
	"	addl $12, %esp\n"
	"	ret\n"
	".globl _n32_make_cmd_stub\n"
	"_n32_make_cmd_stub:\n"
	"	pushl %ebx\n"
	"	pushl %eax\n"
	"	call _n32_make_cmd_hook\n"
	"	addl $8, %esp\n"
	"	ret\n");
extern char n32_mouse_stub[], n32_mouse2_stub[], n32_make_cmd_stub[];
static void mouse_read_original(intptr_t state, float *x, float *y)
{
	__asm__ volatile("pushl %3\n\tpushl %2\n\tcall *%1\n\taddl $8, %%esp" : : "S"(state), "r"(mouse_read_target), "r"(x), "r"(y) : "eax", "ecx", "edx", "memory", "cc");
}
static void mouse_read2_original(intptr_t state, float *x, float *y)
{
	__asm__ volatile("pushl %3\n\tpushl %2\n\tcall *%1\n\taddl $8, %%esp" : : "S"(state), "r"(mouse_read2_target), "r"(x), "r"(y) : "eax", "ecx", "edx", "memory", "cc");
}
static uint8_t *make_cmd_original(uint8_t *out, int lc)
{
	uint8_t *r;
	__asm__ volatile("call *%2" : "=a"(r) : "a"(out), "r"(make_cmd_target), "b"(lc) : "ecx", "edx", "memory", "cc");
	return r;
}

// The engine checks its own code: seven hash checks (functions 0x4e30f0, 0x4ca740, 0x4f8710, 0x43ffb0 ... run at the renderer init and level loads) over VA 0x401000-0x408000, 0x418000-0x428000 and
// 0x430000-0x440000 and over 0x6aa000-0x6aa378 (data), and the game quits with exit code 0x8000dead when one differs, so nothing in those ranges can be patched (the 10 client reads of the player count
// that lie there made the game quit at devmap). The four server reads in them (0x4014e2, 0x4015e9, 0x418d20, 0x418d6b) are left alone. The 64-bit design (the client's reads
// point at a copy that stays 1) is turned around: the client reads stay on the engine's own count (1) and the other reads (the server, the game module: these 54 sites, docs/32bit-native-map/clientctx.md)
// point at a variable that holds the session's number of players.
static const struct {
	uint32_t va;
	uint8_t length, disp;
} server_n_sites32[] = {
	{0x408889, 6, 2}, {0x40a579, 6, 2}, {0x40c714, 6, 2}, {0x42aed0, 6, 2}, {0x42af2f, 6, 2}, {0x42d116, 6, 2}, {0x42d15a, 6, 2}, {0x46ce9f, 6, 2}, {0x46cecc, 6, 2}, {0x47840d, 6, 2}, {0x482ea4, 6, 2}, {0x49d008, 6, 2}, {0x49d0e3, 6, 2}, {0x49d3e4, 6, 2}, {0x4a9754, 6, 2}, {0x4a97ca, 6, 2}, {0x4ae390, 7, 2}, {0x4ae6b0, 6, 2}, {0x4d3f7c, 6, 2}, {0x4d6774, 6, 2}, {0x4e7c89, 6, 2}, {0x4e8366, 6, 2}, {0x4e83b7, 6, 2}, {0x501e99, 6, 2}, {0x59a476, 6, 2}, {0x59d86e, 6, 2}, {0x59d939, 6, 2}, {0x5c5ca8, 6, 2}, {0x5c6ba4, 6, 2}, {0x5c74e8, 6, 2}, {0x5d7b19, 6, 2}, {0x5e2870, 6, 2}, {0x659f3f, 7, 2}, {0x659fc8, 6, 2}, {0x65a01d, 6, 2}, {0x65a096, 6, 2}, {0x65a0fa, 6, 2}, {0x65a4d7, 6, 2}, {0x65a898, 6, 2}, {0x65ab2e, 6, 2}, {0x65acfd, 5, 1}, {0x65af35, 6, 2}, {0x65b16f, 6, 2}, {0x65b463, 6, 2}, {0x65b48d, 6, 2}, {0x65b7a9, 6, 2}, {0x65c140, 6, 2}, {0x65c167, 6, 2}, {0x65ca9b, 6, 2}, {0x65cb65, 6, 2}
};
static int32_t n_server_copy; // what the server and the game module read instead of the number of local players: the engine's own count, N once a split level has started
static int32_t n_zero_copy;       // what the server's entity list reads (a count of 0: no local player is left out)
static void absolute_patch(uint8_t *site, int disp, const void *to)
{
	DWORD prot;
	j_save(site, 8);
	VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &prot);
	uint32_t v = (uint32_t)(uintptr_t)to;
	memcpy(site + disp, &v, 4);
	VirtualProtect(site, 8, prot, &prot);
	FlushInstructionCache(GetCurrentProcess(), site, 8);
}
// QA ([native] countwatch=1): every write to the engine's local player count, found with a hardware data breakpoint on all threads (the log says which instruction wrote it)
static volatile int32_t count_seen;
static LONG CALLBACK count_veh(EXCEPTION_POINTERS *ep)
{
	if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
		return EXCEPTION_CONTINUE_SEARCH;
	CONTEXT *c = ep->ContextRecord;
	if (!(c->Dr6 & 1))
		return EXCEPTION_CONTINUE_SEARCH;
	c->Dr6 = 0;
	int32_t v = *(volatile int32_t *)(base + RVA_LOCAL_PLAYERS);
	log_printf("native: write to the local player count by eip %p (rva 0x%x), value now %d, copy %d", (void *)(uintptr_t)c->Eip, (unsigned)((uintptr_t)c->Eip - (uintptr_t)base), (int)v, (int)n_server_copy);
	return EXCEPTION_CONTINUE_EXECUTION;
}
// QA ([native] exitwatch=1, 32 bits): who ends the process: the return addresses on the stack when ExitProcess is called
static void (WINAPI *exit_process_original)(UINT);
static void WINAPI exit_process_hook(UINT code)
{
	uintptr_t *sp = (uintptr_t *)__builtin_frame_address(0);
	char line[1200];
	int n = snprintf(line, sizeof(line), "native: ExitProcess(%#x) from:", (unsigned)code);
	for (int i = 0; i < 300 && n < (int)sizeof(line) - 60; i++) {
		uintptr_t v;
		if (IsBadReadPtr(sp + i, 4))
			break;
		v = sp[i];
		MEMORY_BASIC_INFORMATION mbi;
		if (v > 0x10000 && VirtualQuery((void *)v, &mbi, sizeof(mbi)) && mbi.Type == MEM_IMAGE && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
			char name[MAX_PATH] = "?";
			GetModuleFileNameA((HMODULE)mbi.AllocationBase, name, sizeof(name));
			const char *slash = strrchr(name, 0x5c);
			n += snprintf(line + n, sizeof(line) - n, " %s+%x", slash ? slash + 1 : name, (unsigned)(v - (uintptr_t)mbi.AllocationBase));
		}
	}
	log_printf("%s", line);
	exit_process_original(code);
}
static void (*die_original)(void);
static void die_hook(void)
{
	uintptr_t *sp = (uintptr_t *)__builtin_frame_address(0);
	char line[1500];
	int n = snprintf(line, sizeof(line), "native: the engine's fatal exit (rva 0x103690) called from:");
	for (int i = 0; i < 400 && n < (int)sizeof(line) - 40; i++) {
		if (IsBadReadPtr(sp + i, 4))
			break;
		uintptr_t v = sp[i];
		if (v >= (uintptr_t)base + 0x1000 && v < (uintptr_t)base + 0x2a0000) // raw-ok: the image bounds as a plausibility test
			n += snprintf(line + n, sizeof(line) - n, " %x", (unsigned)(v - (uintptr_t)base));
	}
	log_printf("%s", line);
	die_original();
}
static DWORD WINAPI count_watch(LPVOID unused)
{
	(void)unused;
	AddVectoredExceptionHandler(1, count_veh);
	for (int round = 0; round < 40; round++) {
		log_printf("native: count watch: engine %d, copy %d at %p", (int)*(volatile int32_t *)(base + RVA_LOCAL_PLAYERS), (int)*(volatile int32_t *)&n_server_copy, (void *)&n_server_copy);
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		THREADENTRY32 te;
		te.dwSize = sizeof(te);
		for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
			if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId())
				continue;
			HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
			if (!th)
				continue;
			if (SuspendThread(th) != (DWORD)-1) {
				CONTEXT ctx;
				memset(&ctx, 0, sizeof(ctx));
				ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
				if (GetThreadContext(th, &ctx) && ctx.Dr0 != (DWORD)(uintptr_t)(base + RVA_LOCAL_PLAYERS)) {
					ctx.Dr0 = (DWORD)(uintptr_t)(base + RVA_LOCAL_PLAYERS);
					ctx.Dr7 = (ctx.Dr7 & ~0xf0003u) | 1u | (1u << 16) | (3u << 18); // Dr0 enabled, write, 4 bytes
					SetThreadContext(th, &ctx);
				}
				ResumeThread(th);
			}
			CloseHandle(th);
		}
		CloseHandle(snap);
		Sleep(100);
	}
	return 0;
}
static void client_n_install(void)
{
	if (cfg("exitwatch", 0)) {
		uint8_t *fn = (uint8_t *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "ExitProcess");
		static const uint8_t prolog[5] = {0x55, 0x8b, 0xec, 0x6a, 0xff};
		if (!hook_function(fn, prolog, sizeof(prolog), (void *)exit_process_hook, (void **)&exit_process_original))
			log_printf("native: ExitProcess starts with %02x %02x %02x %02x %02x", fn[0], fn[1], fn[2], fn[3], fn[4]);
	}
	if (cfg("exitwatch", 0)) {
		static const uint8_t die_bytes[6] = {0x81, 0xec, 0x00, 0x08, 0x00, 0x00};
		if (!hook_function(base + XR(0x103690), die_bytes, sizeof(die_bytes), (void *)die_hook, (void **)&die_original))
			log_printf("native: the fatal exit function's bytes differ");
	}
	if (cfg("countwatch", 0))
		CreateThread(NULL, 0, count_watch, NULL, 0, NULL);
	int done = 0;
	int lo = cfg("n32lo", 0), hi = cfg("n32hi", 999); // QA bisect
	n_server_copy = *(int32_t *)(base + RVA_LOCAL_PLAYERS); // what the engine has now (see players_apply)
	for (size_t i = 0; i < sizeof(server_n_sites32) / sizeof(server_n_sites32[0]); i++) {
		if ((int)i < lo || (int)i > hi)
			continue;
		uint8_t *site = (uint8_t *)(uintptr_t)XV(server_n_sites32[i].va) - 0x400000 + (uintptr_t)base;
		uint32_t at;
		memcpy(&at, site + server_n_sites32[i].disp, 4);
		if (at != (uint32_t)(uintptr_t)(base + RVA_LOCAL_PLAYERS))
			continue; // not the instruction the build has
		absolute_patch(site, server_n_sites32[i].disp, &n_server_copy);
		done++;
	}
	log_printf("native: %d of %d server reads of the local player count point at a copy (%d now)", done, (int)(sizeof(server_n_sites32) / sizeof(server_n_sites32[0])), (int)n_server_copy);
}
static void list_players_install(void)
{
	uint8_t *site = base + XR(0x25cc10); // mov ebp, [count] in the entity list of the snapshot: the local players' own entities stay out of it
	uint32_t at;
	memcpy(&at, site + 2, 4);
	if (site[0] != 0x8b || site[1] != 0x2d || at != (uint32_t)(uintptr_t)(base + RVA_LOCAL_PLAYERS)) {
		log_printf("native: entity list patch: unexpected code");
		return;
	}
	absolute_patch(site, 2, &n_zero_copy);
}

void *n32_vm_blend_target;
unsigned n32_dobj_make_hook(void *info, void *dobj_info);
unsigned n32_dobj_make_hook(void *info, void *dobj_info)
{
	return dobj_make_hook(info, dobj_info);
}
// the entity make (EDI = entity info, EBX = DObj info, result in EAX) and the first-person weapon's blend (the DObj in ESI, two stack arguments the caller cleans)
__asm__(".text\n"
	".globl _n32_dobj_make_stub\n"
	"_n32_dobj_make_stub:\n"
	"	pushl %ebx\n"
	"	pushl %edi\n"
	"	call _n32_dobj_make_hook\n"
	"	addl $8, %esp\n"
	"	ret\n"
	".globl _n32_vm_blend_stub\n"
	"_n32_vm_blend_stub:\n"
	"	cmpl $0, _native_active\n"
	"	je 1f\n"
	"	testl %esi, %esi\n"
	"	je 2f\n"
	"	cmpl $0, (%esi)\n"
	"	je 2f\n"
	"1:	jmp *_n32_vm_blend_target\n"
	"2:	ret\n");
extern char n32_dobj_make_stub[], n32_vm_blend_stub[];

static int hook_calls32(const uint32_t *sites, size_t n, void *hook, void **original, const char *what)
{
	void *dummy = NULL;
	for (size_t i = 0; i < n; i++)
		if (!jhook_call(base + XR(sites[i]), hook, original ? original : &dummy)) {
			log_printf("native: no call at rva 0x%x (%s)", XR(sites[i]), what);
			return 0;
		}
	return 1;
}

// The world's event sounds once, as the 64-bit evsnd1 (2026-11-17, owner: "Second Sun, the sound trembles in split screen"; docs/32bit-native-map/sound-once.md). Both
// contexts run the shared snapshot's entity events, so each shot, explosion and impact started twice in the one mixer: with both views drawn in every frame (mode 11) the two
// copies played almost together. The world sound start (VA 0x66d250, cdecl, 13 arguments, the handle pointer 8th: [esp+0x20] at the entry) gets a stub that returns "no
// sound" while snd_mute32 is set (-1, and -1 in the handle, as the function does with sound off); context 1's two entity event calls (VA 0x4f6321 -> 0x438f80 and
// 0x4f631a -> 0x4f4d20, cdecl (lc, centity *)) set it; context 1 does not fill the effects' pending sound queue (VA 0x40a3c0, cdecl (alias, entity, origin), its one call
// at VA 0x44c509). [native] evsnd1=0 off.
volatile uint8_t snd_mute32;
volatile uint8_t snd_count32; // QA ([native] sndcount=1): the starts counted by caller and context, logged every 5 s (snd_note32)
volatile LONG snd_muted32; // starts muted so far (logged by native_poll_players)
void snd_note32(uintptr_t ret, const void *alias, const uintptr_t *sp);
void snd_note32(uintptr_t ret, const void *alias, const uintptr_t *sp)
{
	if (ret == (uintptr_t)(base + XR(0xd91d3))) { // the sound helper (VA 0x4d9190): its own caller instead (one of its 8 calls), found on the stack
		static const uint32_t helper_calls[] = {0x2f94c, 0x631b9, 0x662e3, 0x9dab1, 0xe6034, 0x16dea2, 0x16e075, 0x17ba2e};
		for (int k = 1; k < 48 && !IsBadReadPtr(sp + k, sizeof(*sp)); k++)
			for (size_t h = 0; h < sizeof(helper_calls) / sizeof(helper_calls[0]); h++)
				if (sp[k] == (uintptr_t)(base + addr_xr(helper_calls[h]) + 5)) // raw-ok: QA only (sndcount), the official exe's call sites
					ret = sp[k], k = 48;
	}
	static struct {
		uintptr_t ret;
		const char *name;
		unsigned n[2];
	} t[48];
	static DWORD at;
	const int c = ctx_current == 1 ? 1 : 0;
	const char *name = alias && !IsBadReadPtr(alias, 4) && *(const char *const *)alias && !IsBadReadPtr(*(const char *const *)alias, 8) ? *(const char *const *)alias : "?";
	int i = 0;
	for (; i < 48 && t[i].ret && !(t[i].ret == ret && t[i].name == name); i++)
		;
	if (i < 48) {
		t[i].ret = ret, t[i].name = name;
		t[i].n[c]++;
	}
	if (!at)
		at = GetTickCount();
	if (GetTickCount() - at >= 5000) {
		at = GetTickCount();
		for (int k = 0; k < 48 && t[k].ret; k++) {
			if (t[k].n[0] + t[k].n[1])
				log_printf("native: sndcount: caller rva 0x%x %.48s: context 0 %u, context 1 %u", (unsigned)(t[k].ret - (uintptr_t)base), t[k].name, t[k].n[0], t[k].n[1]);
			t[k].ret = 0, t[k].n[0] = t[k].n[1] = 0;
		}
	}
}
void *snd_start_orig32;
extern char snd_mute_stub32[];
__asm__(".text\n.globl _snd_mute_stub32\n_snd_mute_stub32:\n"
	"\tcmpb $0, _snd_mute32\n"
	"\tjne 1f\n"
	"\tcmpb $0, _snd_count32\n"
	"\tje 3f\n"
	"\tmovl 4(%esp), %eax\n" // (the alias)
	"\tmovl (%esp), %ecx\n" // (the caller)
	"\tmovl %esp, %edx\n"
	"\tpushl %edx\n"
	"\tpushl %eax\n"
	"\tpushl %ecx\n"
	"\tcall _snd_note32\n"
	"\taddl $12, %esp\n"
	"3:\tjmp *_snd_start_orig32\n"
	"1:\tlock incl _snd_muted32\n"
	"\tmovl 0x20(%esp), %eax\n"
	"\ttestl %eax, %eax\n"
	"\tjz 2f\n"
	"\tmovl $-1, (%eax)\n"
	"2:\tmovl $-1, %eax\n"
	"\tret\n");
static void (*ev_check_original32[2])(int, uint8_t *);
static void ev_check32(int k, int lc, uint8_t *cent)
{
	const int mute = ctx_current == 1 && native_active;
	if (mute)
		snd_mute32 = 1;
	ev_check_original32[k](lc, cent);
	if (mute)
		snd_mute32 = 0;
}
static void ev_check32_hook0(int lc, uint8_t *cent)
{
	ev_check32(0, lc, cent);
}
static void ev_check32_hook1(int lc, uint8_t *cent)
{
	ev_check32(1, lc, cent);
}
static void (*fxq_original32)(void *, int, const float *);
static void fxq_hook32(void *alias, int ent, const float *origin)
{
	if (ctx_current == 1 && native_active)
		return;
	fxq_original32(alias, ent, origin);
}
// The entities' looping sounds (the map's ambient emitters: fire, rain on metal, creaks, flags; VA 0x463190, cdecl (5 arguments), -1 when there is no sound): the cgame's
// entity pass starts them again in every frame, in both contexts, so each emitter played twice, a little apart: the trembling sound the owner heard in Second Sun on the
// 32-bit split screen ("as soon as the character lowers his hands everything is normal"; sndcount: fire_dumpster_medium 4557 / 4452 starts in 5 s in contexts 0 / 1).
// Context 1 starts none: the engine has a listener for each player and plays every sound to the nearer one. [native] loopsnd1=0 off.
static int (*ent_loop_original)(int, int, int, int, int);
static int ent_loop_hook(int a, int b, int c, int d, int e)
{
	if (ctx_current == 1 && native_active && CFG_ONCE("loopsnd1", 1))
		return -1;
	return ent_loop_original(a, b, c, d, e);
}
// The vehicles' engine and idle loops (VA 0x4db816: btr80_engine_low, havoc_engine_low...) and the turret spin (VA 0x488796) are started by both contexts too, each a
// direct call of the world sound start: context 1's calls return "no sound" (cdecl, 13 arguments, the handle pointer 8th).
typedef int (*snd_start32_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, int *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static snd_start32_t veh_snd_original[2];
static int veh_snd(int k, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7, int *h, uintptr_t a9, uintptr_t a10, uintptr_t a11,
		   uintptr_t a12, uintptr_t a13)
{
	if (ctx_current == 1 && native_active && CFG_ONCE("loopsnd1", 1)) {
		if (h)
			*h = -1;
		return -1;
	}
	return veh_snd_original[k](a1, a2, a3, a4, a5, a6, a7, h, a9, a10, a11, a12, a13);
}
static int veh_snd0(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7, int *h, uintptr_t a9, uintptr_t a10, uintptr_t a11, uintptr_t a12,
		    uintptr_t a13)
{
	return veh_snd(0, a1, a2, a3, a4, a5, a6, a7, h, a9, a10, a11, a12, a13);
}
static int veh_snd1(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7, int *h, uintptr_t a9, uintptr_t a10, uintptr_t a11, uintptr_t a12,
		    uintptr_t a13)
{
	return veh_snd(1, a1, a2, a3, a4, a5, a6, a7, h, a9, a10, a11, a12, a13);
}
static int call_of32(const uint8_t *site, const uint8_t *target)
{
	return site[0] == 0xe8 && site + 5 + *(const int32_t *)(site + 1) == target;
}
// the mixer's environment and channel volume setters (see mix_pass)
static void mix_install32(void)
{
	uint8_t want_flag[8] = {0x51, 0x80, 0x3d, 0, 0, 0, 0, 0x00}; // push ecx; cmp byte [sound off flag],0 (environment set and channel volumes off)
	const uint32_t off_flag = XV(0x1bf0b50);
	memcpy(want_flag + 3, &off_flag, 4);
	static const uint8_t want_env_off[7] = {0x8b, 0x44, 0x24, 0x04, 0x56, 0x8b, 0xf0}; // mov eax,[esp+4]; push esi; mov esi,eax
	static const uint8_t want_vol_set[5] = {0x57, 0x8b, 0x7c, 0x24, 0x08};		    // push edi; mov edi,[esp+8]
	const int a = jhook_function(base + XR(0xcb5b0), want_flag, sizeof(want_flag), (void *)env_set_hook, (void **)&env_set_original);
	const int b = a && jhook_function(base + XR(0xd3e90), want_env_off, sizeof(want_env_off), (void *)env_off_hook, (void **)&env_off_original);
	const int c = jhook_function(base + XR(0x60140), want_vol_set, sizeof(want_vol_set), (void *)vol_set_hook, (void **)&vol_set_original);
	const int d = c && jhook_function(base + XR(0xf9840), want_flag, sizeof(want_flag), (void *)vol_off_hook, (void **)&vol_off_original);
	log_printf("native: the sound mixer's environment %s, its channel volumes %s", b ? "is held while either view holds it" : "is not hooked (rva 0xcb5b0 / 0xd3e90)",
		   d ? "too" : "are not hooked (rva 0x60140 / 0xf9840)");
}
// The sound system's loop expiry (VA 0x49da00, run once per client frame from VA 0x506b24: VA 0x4b24d0 stops each looping channel whose frame stamp is not the one
// of the previous pass, then moves the stamp on). In mode 11 it ran in both players' client frames, and only context 0's entity pass keeps the loops alive (context
// 1's starts none, loopsnd1: the doubled emitters of Second Sun). So context 1's pass stopped every loop and context 0's frame started each one again: a looping
// stream (No Russian's emt_airport_traffic1 / emt_airport_int_flyovers1, packed in an .iwd) was opened again in every frame and set to its play position, which
// is reached by inflating the file from its start: about 200 MB a second, and 30 frames a second on the 32-bit split screen instead of 280 (2026-11-19, the
// stream stop's caller found on context 1's stack, [native] seeklog=1). The pass runs in context 0's frame only; [native] loopexpire1=1 lets context 1 run it too.
static void (*loop_expire_original)(void);
static void loop_expire_hook(void)
{
	if (ctx_current == 1 && native_active && mode == 11 && !CFG_ONCE("loopexpire1", 0))
		return;
	loop_expire_original();
}
static void loop_expire_install32(void)
{
	static int done;
	if (done)
		return;
	done = 1;
	uint8_t want[7] = {0x83, 0x3d, 0, 0, 0, 0, 0x00}; // cmp dword [sound system up], 0
	const uint32_t up = XV(0x894330);
	memcpy(want + 2, &up, 4);
	log_printf("native: the looping sounds' expiry %s", jhook_function(base + XR(0x9da00), want, sizeof(want), (void *)loop_expire_hook, (void **)&loop_expire_original)
		   ? "runs in context 0's frame only" : "(rva 0x9da00) is not as expected: it runs in both views' frames, and the looping streams start again in every frame");
}
// 32 bits: the scoped turret's zoom (VA 0x570d80, the cgame frame: cg's turret fov += frame time x the latest usercmd's forwardmove / 127 x the turret's zoom rate,
// held to its min and max; the Barrett of Of Their Own Accord, "press Forward to zoom") reads the latest usercmd of cg's local client number through the client's
// arrays (cmdNumber VA 0x4c1200, the command VA 0x4329e0: base + local client x 0xbe9c). Context 1's cg says local client 1, but the client arrays have one element
// (context 1's statics are swapped into element 0), so player 2's zoom read past them, found no command and never zoomed (QA barrettpad 2026-11-24, a log of what it read: ring
// forwardmove 127, cg turret fov stayed 20; player 1's pad zoomed it to 2). In context 1 both calls get local client 0. [native] turretcmd1=0: player 2 cannot zoom a turret's scope again.
static int (*turret_cmdnum_orig)(int);
static int (*turret_getcmd_orig)(int, unsigned, void *);
static int turret_cmdnum_hook(int lc)
{
	return turret_cmdnum_orig(ctx_current == 1 && native_active ? 0 : lc);
}
static int turret_getcmd_hook(int lc, unsigned n, void *out)
{
	return turret_getcmd_orig(ctx_current == 1 && native_active ? 0 : lc, n, out);
}
static void turret_zoom_install32(void)
{
	if (!cfg("turretcmd1", 1))
		return;
	uint8_t *a = base + XR(0x170e1c), *b = base + XR(0x170e2b);
	const int ok = a[0] == 0xe8 && a + 5 + *(const int32_t *)(a + 1) == base + XR(0xc1200) && b[0] == 0xe8 &&
		       b + 5 + *(const int32_t *)(b + 1) == base + XR(0x329e0) && jhook_call(a, (void *)turret_cmdnum_hook, (void **)&turret_cmdnum_orig) &&
		       jhook_call(b, (void *)turret_getcmd_hook, (void **)&turret_getcmd_orig);
	log_printf("native: a scoped turret's zoom %s", ok ? "reads player 2's commands in his context" : "(VA 0x570d80) is not as expected: player 2 cannot zoom a turret's scope");
}
static void sound_once_install32(void)
{
	loop_expire_install32();
	veh_update_install();
	turret_zoom_install32();
	if (!cfg("evsnd1", 1) && !cfg("sndcount", 0))
		return;
	snd_count32 = cfg("sndcount", 0) != 0;
	uint8_t want[10] = {0x83, 0xec, 0x40, 0x80, 0x3d, 0, 0, 0, 0, 0x00}; // sub esp,0x40; cmp byte [sound off flag],0
	const uint32_t flag = XV(0x1bf0b50);
	memcpy(want + 5, &flag, 4);
	if (!jhook_function(base + XR(0x26d250), want, sizeof(want), (void *)snd_mute_stub32, &snd_start_orig32)) {
		log_printf("native: the world sound start (rva 0x26d250) is not as expected: event sounds start in both contexts");
		return;
	}
	uint8_t *s0 = base + XR(0xf6321), *s1 = base + XR(0xf631a);
	if (call_of32(s0, base + XR(0x38f80)) && call_of32(s1, base + XR(0xf4d20)) && jhook_call(s0, (void *)ev_check32_hook0, (void **)&ev_check_original32[0]) &&
	    jhook_call(s1, (void *)ev_check32_hook1, (void **)&ev_check_original32[1]))
		log_printf("native: the world's event sounds start once (context 0); context 1's copies are muted");
	else
		log_printf("native: the entity event calls (rva 0xf6321 / 0xf631a) are not as expected: event sounds start in both contexts");
	uint8_t *q = base + XR(0x4c509);
	if (call_of32(q, base + XR(0xa3c0)) && jhook_call(q, (void *)fxq_hook32, (void **)&fxq_original32))
		log_printf("native: effects' sounds are queued once (context 0)");
	else
		log_printf("native: the pending sound queue call (rva 0x4c509) is not as expected: effects' sounds play in both contexts");
	{
		uint8_t *v0 = base + XR(0xdb816), *v1 = base + XR(0x88796);
		const int ok0 = call_of32(v0, base + XR(0x26d250)) && jhook_call(v0, (void *)veh_snd0, (void **)&veh_snd_original[0]);
		const int ok1 = call_of32(v1, base + XR(0x26d250)) && jhook_call(v1, (void *)veh_snd1, (void **)&veh_snd_original[1]);
		log_printf("native: the vehicles' engine loops %s, the turret spin %s", ok0 ? "start once (context 0)" : "are not hooked (rva 0xdb816)", ok1 ? "once" : "not hooked (rva 0x88796)");
	}
	static const uint8_t loop_bytes[] = {0x8b, 0x44, 0x24, 0x0c, 0x57, 0x50}; // mov eax,[esp+0xc]; push edi; push eax
	log_printf("native: the entities' looping sounds %s", jhook_function(base + XR(0x63190), loop_bytes, sizeof(loop_bytes), (void *)ent_loop_hook, (void **)&ent_loop_original)
		   ? "start once (context 0)" : "(rva 0x63190) are not as expected: they start in both contexts");
}

// the entity side (docs/32bit-native-map/entities.md): none of these places may lie in the engine's checked code (docs/32bit-plan.md)
static int hooks_apply32_entities(void)
{
	static const uint32_t ent_clean_sites[] = {0xc1c24, 0x59d71, 0x16e9b1, 0x9cb6a}; // (the fifth caller, 0x432872, is in checked code: left alone)
	static const uint32_t slot_sites[] = {0x40e0e, 0x667ff, 0x904ab, 0xa7e45, 0xba4e9}; // callers of the "mine" rule 0x4215a0 outside the checked code (pass2's own call is replaced with pass2)
	static const uint32_t make_sites[] = {0xa5a28, 0xa5ae7, 0xc1d35, 0xc1e73};
	static const uint32_t vm_setup_sites[] = {0xb2e52, 0xacea0, 0xb72a, 0xaf6c1, 0xaf719};
	static const uint32_t vm_blend_sites[] = {0x1723ec};
	static const uint32_t pass2_sites[] = {0x6685b};
	static const uint8_t add_entity_bytes32[] = {0x83, 0xec, 0x18, 0x53, 0x55};
	static const uint8_t own_write_bytes32[] = {0x83, 0xec, 0x10, 0x55, 0x8b, 0x6c, 0x24, 0x18};
	static const uint8_t dobj_add_bytes32[] = {0xd9, 0xee, 0x83, 0xec, 0x14};
	if (!hook_calls32(ent_clean_sites, sizeof(ent_clean_sites) / sizeof(ent_clean_sites[0]), ent_clean_hook, (void **)&ent_clean_original, "entity cleanup") ||
	    !hook_calls32(slot_sites, sizeof(slot_sites) / sizeof(slot_sites[0]), local_slot_hook, NULL, "local slot") ||
	    !hook_calls32(make_sites, sizeof(make_sites) / sizeof(make_sites[0]), n32_dobj_make_stub, &dobj_make_target, "DObj make") ||
	    !hook_calls32(vm_setup_sites, sizeof(vm_setup_sites) / sizeof(vm_setup_sites[0]), viewmodel_setup_hook, (void **)&viewmodel_setup_original, "view model setup") ||
	    !hook_calls32(vm_blend_sites, 1, n32_vm_blend_stub, &n32_vm_blend_target, "view model blend") ||
	    !hook_calls32(pass2_sites, 1, pass2_hook, NULL, "second entity pass"))
		return 0;
	if (!jhook_function(base + XR(0x6df50), add_entity_bytes32, sizeof(add_entity_bytes32), add_entity_hook, (void **)&add_entity_original) ||
	    !jhook_function(base + XR(0x904a0), own_write_bytes32, sizeof(own_write_bytes32), own_write_hook, (void **)&own_write_original) ||
	    !jhook_function(base + XR(0x103c00), dobj_add_bytes32, sizeof(dobj_add_bytes32), dobj_add_hook, (void **)&dobj_add_original)) {
		log_printf("native: entity function bytes differ");
		return 0;
	}
	log_printf("native: 32-bit entity hooks on");
	return 1;
}

static int hooks_apply32_contexts(void)
{
	static const struct {
		uint32_t site;
		void *hook;
		void **original;
	} sites[] = {
		{0x7af60, cg_init_all_hook, (void **)&cg_init_all_original},
		{0x55484, snapshot_take_hook, (void **)&snapshot_take_original},
		{0x25cab8, ps_publish_hook, (void **)&ps_publish_original},
		{0x2d155, server_cmds_hook, (void **)&server_cmds_original},
		{0xd6794, usercmd_step_hook, (void **)&usercmd_step_original},
		{0x25ccfa, snapshot_msg_hook, (void **)&snapshot_msg_original},
		{0x96dd7, client_frame_hook, (void **)&client_frame_original},
		{0x179be9, n32_make_cmd_stub, &make_cmd_target},
		{0x59d6c, server_step_hook, (void **)&server_step_original},
		{0x179000, n32_mouse_stub, &mouse_read_target},
		{0x1799e8, n32_mouse2_stub, &mouse_read2_target},
	};
	for (size_t i = 0; i < sizeof(sites) / sizeof(sites[0]); i++)
		if (mouse_early && sites[i].hook == (void *)n32_mouse_stub)
			continue; // hooked at the start already (a second hook of the call would call itself)
		else if (!jhook_call(base + XR(sites[i].site), sites[i].hook, sites[i].original)) {
			log_printf("native: no call at rva 0x%x, context hook %zu off", XR(sites[i].site), i);
			return 0;
		}
	if (cfg("entities", 1) && !hooks_apply32_entities())
		return 0;
	{ // the 2D texts of the HUD (docs/32bit-native-map/inputui.md): message windows, the title lines' element draw, the names over the other player
		static const uint8_t key_event_bytes32[] = {0x8b, 0x4c, 0x24, 0x0c, 0x53};
		uint8_t msgwin_bytes32[] = {0xa1, 0xc0, 0xa7, 0x86, 0x00}; // mov eax, [0x86a7c0]
		{
			uint32_t at = XV(0x86a7c0); // (the same number on the official build)
			memcpy(msgwin_bytes32 + 1, &at, 4);
		}
		if (!jhook_function(base + XR(0xd2350), key_event_bytes32, sizeof(key_event_bytes32), key_event_hook, (void **)&key_event_original))
			log_printf("native: key event bytes differ");
		uint8_t hudelem_bytes32[] = {0xa1, 0x50, 0x86, 0x75, 0x00}; // mov eax, [0x758650]
		{
			uint32_t at = XV(0x758650);
			memcpy(hudelem_bytes32 + 1, &at, 4);
		}
		if (!jhook_function(base + XR(0x713b0), msgwin_bytes32, sizeof(msgwin_bytes32), msgwin_hook, (void **)&msgwin_original))
			log_printf("native: game message window draw bytes differ");
		if (cfg("titles", 1) && !jhook_function(base + XR(0x164b40), hudelem_bytes32, sizeof(hudelem_bytes32), hudelem_hook, (void **)&hudelem_original))
			log_printf("native: HUD element draw bytes differ");
		if (cfg("namefix", 1)) {
			static const uint32_t name_site[] = {0x15eaf9};
			name_self_install();
			if (!hook_calls32(name_site, 1, name_get_hook, (void **)&name_get_original, "name lookup"))
				log_printf("native: no call at the name loop's name lookup");
		}
	}
	sound_once_install32();
	mix_install32();
	aim_for_client_fn = base + XR(0x15b040);
	{ // the aim assist's screen targets of each context's cgame and the server's target list of the client it is (docs/32bit-native-map/aimrumble.md 1.4, 1.5)
		static const uint32_t screen_site[] = {0x66adc}, list_site[] = {0x88ba8};
		static const uint32_t snd_listener_site[] = {0x66a20};
		if (!hook_calls32(screen_site, 1, screen_targets_hook, (void **)&screen_targets_original, "screen targets") ||
		    !hook_calls32(list_site, 1, target_list_hook, (void **)&target_list_original, "target list"))
			return 0;
		if (!hook_calls32(snd_listener_site, 1, snd_listener_hook, (void **)&snd_listener_original, "sound listener"))
			log_printf("native: the sound listener stays shared");
	}
	{ // the alpha of the co-op "waiting for the other player" text (diagnostics, wait_alpha_hook)
		static const uint32_t wait_site[] = {RVA_WAIT_ALPHA_SITE};
		if (!hook_calls32(wait_site, 1, wait_alpha_hook, (void **)&wait_alpha_original, "wait alpha"))
			return 0;
	}
	swap_entities = cfg("swapent", 0);
	skip_entity = cfg("skipent", 1);
	log_entities = cfg("entdraw", 0);
	p1pad = cfg("p1pad", -1);
	pad_dev[0] = cfg("p0pad", -1);
	skip_hud_mask = cfg("skiphud", 0);
	skip_ctx = cfg("skipctx", 1);
	ctx_on = 1;
	p2cmd_filter_install();
	log_printf("native: 32-bit client contexts on (call sites redirected)");
	return 1;
}
#endif

#ifdef _WIN64
static void contexts_install(void)
{
	static const struct {
		uint32_t site;
		void *hook;
		void **original;
	} sites[] = {
	    {0x2336c7, cg_init_all_hook, (void **)&cg_init_all_original},
	    {0x2343b8, snapshot_take_hook, (void **)&snapshot_take_original},
	    {0x2346ca, ps_publish_hook, (void **)&ps_publish_original},
	    {0x234a02, server_cmds_hook, (void **)&server_cmds_original},
	    {0x15ce84, usercmd_step_hook, (void **)&usercmd_step_original},
	    {0x234925, snapshot_msg_hook, (void **)&snapshot_msg_original},
	    {0x1fc331, client_frame_hook, (void **)&client_frame_original},
	    {0xbd093, hud_c6070_hook, (void **)&hud_c6070_original},
	    {0xbd09a, hud_c14e0_hook, (void **)&hud_c14e0_original},
	    {0xbd0a1, hud_94d70_hook, (void **)&hud_94d70_original},
	    {0xbd034, hud_c5b70_hook, (void **)&hud_c5b70_original},
	    {0xc7499, viewmodel_blend_hook, (void **)&viewmodel_blend_original},
	    {0xc41b1, viewmodel_setup_hook, (void **)&viewmodel_setup_original},
	    {0xc58ed, viewmodel_setup_hook, (void **)&viewmodel_setup_original},
	    {0xc594a, viewmodel_setup_hook, (void **)&viewmodel_setup_original},
	    {0xc5dfb, viewmodel_setup_hook, (void **)&viewmodel_setup_original},
	    {0xc6033, viewmodel_setup_hook, (void **)&viewmodel_setup_original},
	    {0xbd2c5, screen_targets_hook, (void **)&screen_targets_original},
	    {0x6a711, target_list_hook, (void **)&target_list_original},
	    {0xbd03b, hud_x0_hook, (void **)&hud_x_original[0]},
	    {0xbd0fb, hud_x1_hook, (void **)&hud_x_original[1]},
	    {0xbd102, hud_x2_hook, (void **)&hud_x_original[2]},
	    {0xbd109, hud_x3_hook, (void **)&hud_x_original[3]},
	    {0xbd130, hud_x4_hook, (void **)&hud_x_original[4]},
	    {0xbd1a4, hud_x5_hook, (void **)&hud_x_original[5]},
	    {0xbd206, hud_x6_hook, (void **)&hud_x_original[6]},
	    {0x94039, wait_alpha_hook, (void **)&wait_alpha_original},
	    {0xcfd23, mouse_read_hook, (void **)&mouse_read_original},
	    {0xcf8b1, make_cmd_hook, (void **)&make_cmd_original},
	    {0xb8d50, server_step_hook, (void **)&server_step_original},
	    {0xd0061, mouse_read2_hook, (void **)&mouse_read2_original},
	};
	int enabled = cfg("sites", 0x7fffffff);
	for (size_t i = 0; i < sizeof(sites) / sizeof(sites[0]); i++)
		if (mouse_early && sites[i].hook == (void *)mouse_read_hook)
			continue; // hooked at the start already (a second hook of the call would call itself)
		else if ((enabled & (1 << i)) && !jhook_call(base + XR(sites[i].site), sites[i].hook, sites[i].original))
			log_printf("native: no call at rva 0x%x, context hook %zu off", XR(sites[i].site), i);
	swap_entities = cfg("swapent", 0); // the old workaround for context 1's "mine" (superseded by ownfix, local_slot_hook)
	skip_entity = cfg("skipent", 1); // context 1's own entity (player 2) is never drawn by the first pass either
	log_entities = cfg("entdraw", 0);
	vision_forward_install();
#ifdef _WIN64
	cmd_deliver_install();
	p2cmd_filter_install();
#ifdef RANGE_FX
	fxbody_install();
	lcnum_pins_install();
#endif
#endif
	{
		if (cfg("sndcount", 0) || cfg("evsnd1", 1)) {
			static const uint8_t snd_play_bytes[] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20};
			snd_count_on = cfg("sndcount", 0) != 0;
			if (snd_count_on)
				snd_ctx_ptr = &ctx_current;
			if (!jhook_function(base + XR(0x23a9b0), snd_play_bytes, sizeof(snd_play_bytes), snd_count_stub, &snd_count_orig))
				{
				const uint8_t *t = base + XR(0x23a9b0);
				log_printf("native: sound play bytes differ: %02x %02x %02x %02x %02x %02x %02x %02x", t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7]);
			} else if (cfg("evsnd1", 1)) {
				uint8_t *s0 = base + XR(0x9c924), *s1 = base + XR(0x9c954);
				if (s0[0] == 0xe8 && s0 + 5 + *(int32_t *)(s0 + 1) == base + XR(0x9f840) && s1[0] == 0xe8 && s1 + 5 + *(int32_t *)(s1 + 1) == base + XR(0x9f940) &&
				    jhook_call(s0, (void *)ev_check_hook0, (void **)&ev_check_original[0]) && jhook_call(s1, (void *)ev_check_hook1, (void **)&ev_check_original[1]))
					log_printf("native: the world's event sounds start once (context 0); context 1's copies are muted");
				else
					log_printf("native: the entity event calls at 0x9c924 / 0x9c954 are not as expected: event sounds start in both contexts");
				static const uint8_t dynpush_bytes[21] = {0x88, 0x54, 0x24, 0x10, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x53, 0x57, 0x41, 0x54,
									 0x48, 0x8d, 0xac, 0x24, 0xb8, 0xdf, 0xff, 0xff}; // mov [rsp+10],dl; mov [rsp+8],ecx; push rbp/rbx/rdi/r12; lea rbp,[rsp-0x2048]
				if (cfg("dynpush1", 0))
					log_printf("native: explosions push the world's objects once (context 0): %s",
						   jhook_function(base + XR(0xf53c0), dynpush_bytes, sizeof(dynpush_bytes), dynpush_stub, &dynpush_orig) ? "on" : "the code at 0xf53c0 is not as expected, off");
				static const uint8_t fxq_head[] = {0x44, 0x8b, 0x15}; // mov r10d, [rip+count]; then cmp r10d, 0x20; jz
				uint8_t *fxq = base + XR(0x237a70);
				if (!memcmp(fxq, fxq_head, sizeof(fxq_head)) && fxq[7] == 0x41 && fxq[8] == 0x83 && fxq[9] == 0xfa && fxq[10] == 0x20 && fxq + 7 + *(int32_t *)(fxq + 3) == base + XR(0x1a2cf58)) {
					jhook_jump(fxq, 14, (const void *)fx_sound_queue_hook, 0xcc);
					log_printf("native: effects' sounds are queued once (context 0)");
				} else
					log_printf("native: the pending sound queue at 0x237a70 is not as expected: effects' sounds play in both contexts");
			}
		}
		mix_install64();
		ent_loops_install64();
		veh_update_install();
		snd_frame_install64();
		if (!jhook_function(base + XR(0x9c960), add_entity_bytes, sizeof(add_entity_bytes), add_entity_hook, (void **)&add_entity_original))
			log_printf("native: add entity bytes differ");
		if (!jhook_function(base + XR(0xb0680), pass2_bytes, sizeof(pass2_bytes), pass2_hook, (void **)&pass2_original))
			log_printf("native: second entity pass bytes differ");
		if (!jhook_function(base + XR(0xb86f0), dobj_make_bytes, sizeof(dobj_make_bytes), dobj_make_hook, (void **)&dobj_make_original))
			log_printf("native: client DObj make bytes differ");
		if (!jhook_call(base + XR(0x896d3), ik_hook, (void **)&ik_original))
			log_printf("native: no call at the foot adjustment");
		if (!jhook_function(base + XR(0x29a120), xfer_bytes, sizeof(xfer_bytes), xfer_hook, (void **)&xfer_original))
			log_printf("native: DObj transfer bytes differ");
		if (!jhook_function(base + XR(0x2bb0), dobj_add_bytes, sizeof(dobj_add_bytes), dobj_add_hook, (void **)&dobj_add_original))
			log_printf("native: dobj add bytes differ");
		if (!jhook_function(base + XR(0xb28b0), own_write_bytes, sizeof(own_write_bytes), own_write_hook, (void **)&own_write_original))
			log_printf("native: own entity write bytes differ");
		if (!jhook_function(base + XR(0xa5ec0), slot_bytes, sizeof(slot_bytes), local_slot_hook, (void **)&slot_original))
			log_printf("native: local slot function bytes differ");
		if (!jhook_function(base + XR(0xcb4f0), msgwin_bytes, sizeof(msgwin_bytes), msgwin_hook, (void **)&msgwin_original))
			log_printf("native: game message window draw bytes differ");
		if (!jhook_call(base + XR(0x1fbc1d), key_event_hook, (void **)&key_event_original))
			log_printf("native: no call at the event loop's key event");
		if (cfg("titles", 1) && !jhook_function(base + XR(0xa3b80), hudelem_bytes, sizeof(hudelem_bytes), hudelem_hook, (void **)&hudelem_original))
			log_printf("native: HUD element draw bytes differ");
		if (cfg("namefix", 1)) {
			name_self_install();
			if (!jhook_call(base + XR(0x93e2f), name_get_hook, (void **)&name_get_original))
				log_printf("native: no call at the name loop's name lookup");
		}
	}
	p1pad = cfg("p1pad", -1);
	pad_dev[0] = cfg("p0pad", -1);
	rumble_set_log(cfg("rumblelog", 0));
	skip_hud_mask = cfg("skiphud", 0);
	skip_ctx = cfg("skipctx", 1);
	if (cfg("entclean", 1) &&
	    !jhook_function(base + XR(0xb9320), ent_clean_bytes, sizeof(ent_clean_bytes), ent_clean_hook, (void **)&ent_clean_original))
		log_printf("native: entity cleanup bytes differ");
	ctx_on = 1;
	log_printf("native: client contexts on (call sites redirected)");
}
#endif

// The scripts decide "co-op" from the dvar `coop` (rva 0x1905308, a bool whose script-side writes are refused); the engine's own co-op byte (rva 0x52fb5c) stays 0 so that the engine takes its
// local-players branch. [native] coopdvar=1 keeps the dvar at 1 (written from a thread, every 20 ms) so that the level scripts, the mod's among them, run their two-player code.
static volatile LONG coopdvar_on; // the thread below writes only while a split session runs
// The session's stop takes this lock to turn the forcing off: a write of 1 that was under way when it stopped then always lands before the stop's
// own 0 (a 60 ms sleep stood for that, and a thread held up longer than that wrote 1 after it: the next solo game ran as co-op, audit 2026-11-18,
// native 8). With no session the thread looks four times a second, not fifty.
static SRWLOCK coopdvar_lock = SRWLOCK_INIT;
static DWORD WINAPI coop_dvar_thread(LPVOID unused)
{
	(void)unused;
	for (;;) {
		AcquireSRWLockExclusive(&coopdvar_lock);
		uint8_t *dvar = *(uint8_t **)(base + RVA_COOP_DVAR);
		const int on = coopdvar_on != 0;
		if (dvar && on)
			*(int *)(dvar + 0x10) = 1;
		ReleaseSRWLockExclusive(&coopdvar_lock);
		Sleep(on ? 20 : 250);
	}
	return 0;
}

static int hooks_installed;
static int hooks_ok;

// What makes the game a two local players game: put back by native_deactivate() (journal group 1)
static int players_apply(int n)
{
	uint8_t *at = base + RVA_LOCAL_PLAYERS_STORE;
	if (at[0] != 0xc7 || at[1] != 0x05 || at[6] != 1 || at[7] || at[8] || at[9]) {
		log_printf("native: unexpected code at the local players store, players=%d ignored", n);
		return 0;
	}
	DWORD old;
	jgroup = 1;
#ifdef _WIN64
	j_save(at + 6, 1);
	if (!VirtualProtect(at + 6, 1, PAGE_EXECUTE_READWRITE, &old)) {
		log_printf("native: VirtualProtect failed (%lu)", GetLastError());
		jgroup = 0;
		return 0;
	}
	at[6] = (uint8_t)n;
	VirtualProtect(at + 6, 1, old, &old);
#else
	(void)old;
	// 32 bits: the engine's own count stays what it is (1: the client code, which cannot be patched, reads it); the store in G_InitGame gets code after it that sets the variable
	// the server's reads were pointed at (client_n_install) to n. The store itself (mov dword [count], 1) is copied into the code as it is.
	{
		uint32_t copy_at = (uint32_t)(uintptr_t)&n_server_copy;
		uint32_t nn = (uint32_t)n;
		uint8_t cave[25] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xc7, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0xe9, 0, 0, 0, 0};
		memcpy(cave, at, 10);
		memcpy(cave + 12, &copy_at, 4);
		memcpy(cave + 16, &nn, 4);
		uint8_t *stub = hook_code(cave, sizeof(cave));
		if (!stub) {
			jgroup = 0;
			return 0;
		}
		// the jump back is relative to where the page is: build it in place
		DWORD o2;
		VirtualProtect(stub, sizeof(cave), PAGE_EXECUTE_READWRITE, &o2);
		int32_t rel = (int32_t)((uintptr_t)(at + 10) - (uintptr_t)(stub + 25));
		memcpy(stub + 21, &rel, 4);
		VirtualProtect(stub, sizeof(cave), PAGE_EXECUTE_READ, &o2);
		FlushInstructionCache(GetCurrentProcess(), stub, sizeof(cave));
		if (!(cfg("n32skip", 0) & 1))
			jhook_jump(at, 10, stub, 0x90);
	}
#endif
	log_printf("native: the number of local players is %d (patched G_InitGame)", n);
#ifdef _WIN64
	jhook_jump(base + RVA_SV_ADD_COMMAND, 14, sv_add_command, 0xcc);
#else
	if (!(cfg("n32skip", 0) & 2))
		jhook_jump(base + RVA_SV_ADD_COMMAND, 5, sv_add_command, 0xcc);
#endif
#ifndef _WIN64
	if (!(cfg("n32skip", 0) & 4))
#endif
	client_n_install();
	if (cfg("listplayers", 0))
		list_players_install();
	jgroup = 0;
	log_printf("native: server commands to client 1 are dropped (no client behind it yet)");
	return 1;
}

// The cgame draw hook also applies the full-window aspect correction in ordinary one-viewport gameplay.
// It is installed at startup and reused by the split-screen hook set if a local session is started later.
static int cg_draw_install(void)
{
	if (cg_draw_original)
		return 1;
	int group = jgroup;
	jgroup = 0;
	int ok = jhook_function(base + RVA_CG_DRAW, cg_draw_bytes, sizeof(cg_draw_bytes), cg_draw_hook, (void **)&cg_draw_original);
	jgroup = group;
	return ok && cg_draw_original != NULL;
}

// The client-side hooks stay installed; session-specific work is gated by native_active (journal group 0).
#ifndef _WIN64
static int hooks_apply32_contexts(void);
static void hooks_apply32_mode11(void);
static DWORD WINAPI coop_dvar_thread(LPVOID unused);
#endif
#ifndef _WIN64
// The 32-bit hooks (docs/32bit-native-map/)
static int hooks_apply32(void)
{
	if (hooks_installed)
		return hooks_ok;
	hooks_installed = 1;
	jgroup = 0;
	uint8_t *site = base + RVA_BEGIN_FRAME_CALL;
	if (!jhook_call(site, (void *)begin_frame_hook, (void **)&begin_frame_original) || !cg_draw_install()) {
		log_printf("native: unexpected code at the cgame draw or its begin-frame call, the 32-bit split screen is off");
		return 0;
	}
	if (cfg("ctx", 1) && !hooks_apply32_contexts())
		return 0;
	if (mode == 11)
		hooks_apply32_mode11();
	CreateThread(NULL, 0, coop_dvar_thread, NULL, 0, NULL);
	hooks_ok = 1;
	return 1;
}
#endif

// The popup friendly name (the name of the friendly under the cross hair, "Sgt. Foley": rva 0x94e00(local client), called from the HUD at 0x93cf0). The game's server sets it per local
// client as two configstrings, 0xc + 2 * client and 0xd + 2 * client (the cgame reads the table of 16-bit string indices at rva 0x5d8d02 through 0xca1e0); the HUD draw is called for
// local client 0 in both contexts, so player 2's half showed the name that player 1's cross hair had put in 0xc / 0xd (the owner: "Sgt. Foley on both halves when the top one looks at
// him"). In context 1 the entries of client 1 take their place for the call ([native] exp11friend=0 off).
static void (*friend_original)(int);
static void friend_hook(int lc)
{
	if (mode == 11 && split_on && ctx_current == 1 && CFG_ONCE("exp11friend", 1)) {
		uint16_t *t = (uint16_t *)(base + M11_CS_INDEX);
		uint16_t a = t[0xc], b = t[0xd];
		t[0xc] = t[0xe], t[0xd] = t[0xf];
		t[0xe] = a, t[0xf] = b;
		friend_original(lc);
		t[0xe] = t[0xc], t[0xf] = t[0xd];
		t[0xc] = a, t[0xd] = b;
		return;
	}
	friend_original(lc);
}
// Each player's cgame step before the views are drawn, as on the 360 (2026-11-15). The 360's screen update (0x82135818) first runs, for every local player,
// 0x821127a0 (0x82102e60, the snapshots 0x8210d1c0, 0x820e67e0, and the own-entity write 0x82102f28), and only then the loop that draws each player's view.
// The PC's screen update (0xd5890) has the same step, 0xbc170 (0xb2860, the snapshots 0xb8c50, 0x97ea0, the own-entity write 0xb28b0), called at 0xd58e3
// for player 1 only: in mode 11 player 2's client runs under the screen update's busy flag and his view is drawn inside player 1's screen update, so his
// step never ran: his snapshots were not processed there and his own entity (slot 1 with centshare) was not written ([native] p2step=0 off).
static void (*cg_step_original)(unsigned lc);
// The screen update sets the client time just before the step: 0xbc1a0(serverTime, ...) writes it to cl.serverTime (rva 0x51dca8, in the swapped cl range; the 360 has a client
// time per local player). Player 2's screen update never runs in mode 11, so his cl.serverTime stayed at the value of his first frame (450 ms) and everything the HUD code
// computes from it was wrong: the script's black fade-in (a HUD element with a fade start and duration) never began to fade in his view, and his half stayed black ([native] p2time=0 off).
static void (*cg_time_original)(unsigned, unsigned, unsigned);
static unsigned cg_time_arg;
static int cg_time_have;
static void cg_time_hook(unsigned t, unsigned b, unsigned c)
{
	cg_time_arg = t;
	cg_time_have = 1;
	cg_time_original(t, b, c);
}
static void animleaf_log(const char *where, int ent, const uint8_t *dobj);
static void cg_step_hook(unsigned lc)
{
	if (ctx_current == 0 && lc == 0)
		anim_frame++; // (anim_step_hook: a new engine frame)
	cg_step_original(lc);
	if (mode == 11 && split_on && ctx1_ready && ctx_current == 0 && lc == 0 && CFG_ONCE("animleaf", 0)) // QA: the partner body's tree after player 1's step
		animleaf_log("s0", 0, NULL), animleaf_log("s0", CFG_ONCE("animleafent", 1), NULL);
	if (mode == 11 && split_on && ctx1_ready && ctx_current == 0 && lc == 0 && CFG_ONCE("p2step", 1)) {
		ctx_swap(1);
		if (cg_time_have && CFG_ONCE("p2time", 1)) {
			int *t = (int *)(base + M11_CL_SERVERTIME);
			static unsigned said;
			if (said++ < 3)
				log_printf("native: p2time: player 2's cl.serverTime %d -> %d", *t, (int)cg_time_arg);
			*t = (int)cg_time_arg;
		}
		if (laser_split_on()) // (his own laser list, which nothing else empties: 0xa5850 runs for player 1 only)
			*(int *)(base + XR(RVA_LASER_COUNT)) = 0;
		cg_step_original(0);
		if (CFG_ONCE("animleaf", 0)) // QA: the partner body's tree after player 2's step
			animleaf_log("s1", 0, NULL), animleaf_log("s1", CFG_ONCE("animleafent", 1), NULL);
#ifdef _WIN64
		if (CFG_ONCE("camlog", 0)) { // QA: player 2's slot after his step, next to his snapshot's player state origin (cg.nextSnap ps, rva 0x40cad8, origin +0x1c)
			static unsigned ks;
			const uint8_t *nps = *(const uint8_t *const *)(base + XR(0x40cad8));
			const float *e1 = (const float *)(base + RVA_CENT + CENT_SIZE + 0x18), *e0 = (const float *)(base + RVA_CENT + 0x18);
			if (++ks % 120 == 1 && ks < 20000 && nps)
				log_printf("native: p2step #%u: ps origin %.1f %.1f | slot1 %.1f %.1f | slot0 %.1f %.1f", ks, (double)((const float *)(nps + 0x1c))[0], (double)((const float *)(nps + 0x1c))[1],
					   (double)e1[0], (double)e1[1], (double)e0[0], (double)e0[1]);
		}
#endif
		ctx_swap(0);
	}
}

#ifndef _WIN64
// Mode 11 on the 32-bit build (2026-11-17, owner: "mode 11 for the 32-bit version too"): the same hooks as the 64-bit hooks_apply, at the call sites found in
// docs/32bit-native-map/mode11-*.md (VA - 0x400000). Every call is checked to be a call of the expected function first. The scene3d / scene clear hooks are left out: they
// only act with two scenes in one renderer frame (exp11frames=0), and each player has his own frame by default.
static int m11_call32(uint8_t *site, uint8_t *target, void *hook, void **original, const char *what)
{
	if (site[0] != 0xe8 || site + 5 + *(const int32_t *)(site + 1) != target) {
		log_printf("native: mode 11: unexpected code at the %s call (rva 0x%x), not hooked", what, (unsigned)(site - base));
		return 0;
	}
	if (!jhook_call(site, hook, original)) {
		log_printf("native: mode 11: the %s call (rva 0x%x) could not be hooked", what, (unsigned)(site - base));
		return 0;
	}
	return 1;
}
static void hooks_apply32_mode11(void)
{
	int ok = 0;
	ok += m11_call32(base + XR(0x17ae8a), base + M11_END_FRAME, (void *)end_frame_hook, (void **)&end_frame_original, "screen update's end-frame");
	ok += m11_call32(base + XR(0x17adff), base + XR(0x4a530), (void *)cg_time_hook, (void **)&cg_time_original, "screen update's client time");
	ok += m11_call32(base + XR(0x17ae05), base + XR(0x4a1b0), (void *)cg_step_hook, (void **)&cg_step_original, "screen update's cgame step");
	ok += m11_call32(base + XR(0x668b2), base + XR(0x9d120), (void *)fx_step_hook, (void **)&fx_step_original, "cgame's effects step");
	// (anim_step_hook: the animation steps of the snapshot's entities and of the players, rva 0x170bb0, called by the cgame draw for local client 0; cdecl (dobj, notify))
	if (m11_call32(base + XR(0x170bda), base + XR(0x605b0), (void *)anim_step_hook, (void **)&anim_step_original, "snapshot entities' animation step") +
		m11_call32(base + XR(0x170c20), base + XR(0x605b0), (void *)anim_step_hook, (void **)&anim_step_original, "players' animation step") != 2)
		log_printf("native: mode 11: the animations may advance twice a frame (animonce)");
	ok += m11_call32(base + XR(0xf319a), base + XR(0x15e7d0), (void *)friend_hook, (void **)&friend_original, "HUD's popup friendly name");
	ok += m11_call32(base + XR(0x496b7), base + XR(0x107b10), (void *)render_scene_hook, (void **)&render_scene_original, "R_RenderScene");
	ok += m11_call32(base + XR(0x107b70), base + XR(0xa31e0), (void *)rs_shock_hook, (void **)&rs_shock_original, "R_RenderScene's shellshock");
	if (CFG_ONCE("shocksect", 1))
		ok += m11_call32(base + XR(0x13636f), base + XR(0x1340c0), (void *)n32_save_help_stub, (void **)&save_help_target, "saved screen's copy");
	ok += m11_call32(base + XR(0x106b3c), base + XR(0xf3020), (void *)rs_cgents_hook, (void **)&rs_cgents_original, "R_RenderScene's cgame 2D");
	if (CFG_ONCE("exp11p1show", 1)) {
		int be = m11_call32(base + XR(0x1399a6), base + XR(0x1397e0), (void *)be_frame_hook, (void **)&be_frame_original, "render thread's back-end frame (1)");
		be += m11_call32(base + XR(0x139a02), base + XR(0x1397e0), (void *)be_frame_hook, (void **)&be_frame_original, "render thread's back-end frame (2)");
		be += m11_call32(base + XR(0x139a67), base + XR(0x1397e0), (void *)be_frame_hook, (void **)&be_frame_original, "render thread's back-end frame (3)");
		be_end_installed = be == 3; // (all three, or player 1's frame keeps flags 1: a frame that is not shown must never be presented)
		ok += be;
	}
	if (CFG_ONCE("exp11lc", 1) && CFG_ONCE("exp11pin", 1)) {
		// The readers of the renderer's local player (VA 0x75e688) that index one-element arrays or the cull byte: they take player 0 (see the 64-bit pins). The 64-bit pin at
		// 0xa4cbf has no 32-bit counterpart that matters (the one read at VA 0x5646c0 goes to an argument nobody reads): not patched.
		const struct { uint32_t rva; uint8_t len, op; } pins[] = { // (XR at run time: the iw4x-sp table maps them too)
			{XR(0x89134), 6, 0x0d}, // mov ecx,[lc] -> xor ecx,ecx: the cull byte = 2 (cpose + 3 + lc)
			{XR(0xe2800), 5, 0xa1}, // mov eax,[lc] -> xor eax,eax: the cull byte 0 -> 1
			{XR(0xea374), 5, 0xa1}, // the cull byte reset
			{XR(0xe6a90), 5, 0xa1}, // the cgame's local entities callback (stores lc in its argument)
			{XR(0xe7460), 5, 0xa1}, // the HUD type-13 callback (the same)
		};
		const uint32_t lc_va = (uint32_t)(uintptr_t)(base + M11_RLC);
		int pinned = 0;
		for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
			uint8_t *p = base + pins[i].rva;
			const int six = pins[i].len == 6;
			if ((six ? (p[0] != 0x8b || p[1] != pins[i].op) : p[0] != 0xa1) || *(const uint32_t *)(p + (six ? 2 : 1)) != lc_va) {
				log_printf("native: mode 11: unexpected code at the local player read rva 0x%x, not pinned", pins[i].rva);
				continue;
			}
			uint8_t put[6] = {0x31, (uint8_t)(six ? 0xc9 : 0xc0), 0x90, 0x90, 0x90, 0x90};
			DWORD old;
			j_save(p, pins[i].len);
			VirtualProtect(p, pins[i].len, PAGE_EXECUTE_READWRITE, &old);
			memcpy(p, put, pins[i].len);
			VirtualProtect(p, pins[i].len, old, &old);
			FlushInstructionCache(GetCurrentProcess(), p, pins[i].len);
			pinned++;
		}
		log_printf("native: mode 11: %d of %d local player reads pinned to player 0", pinned, (int)(sizeof(pins) / sizeof(pins[0])));
	}
	log_printf("native: mode 11 (32-bit): %d of %d calls hooked%s", ok, (CFG_ONCE("exp11p1show", 1) ? 11 : 8) + (CFG_ONCE("shocksect", 1) ? 1 : 0), be_end_installed ? "" : " (player 1's frame is not shown with its own 2D: the back end's frame is not wrapped)");
}
#endif

static int hooks_apply(void)
{
#ifndef _WIN64
	return hooks_apply32();
}
#else
	if (hooks_installed)
		return hooks_ok;
	hooks_installed = 1;
	jgroup = 0;
	if (cfg("ctx", 1))
		contexts_install();
	if (cfg("logcbuf", 0) && !jhook_function(base + XR(0x1ee890), cbuf_bytes, sizeof(cbuf_bytes), cbuf_hook, (void **)&cbuf_original))
		log_printf("native: Cbuf_AddText bytes differ");
	if (!jhook_function(base + RVA_BEGIN_FRAME, begin_frame_bytes, sizeof(begin_frame_bytes), begin_frame_hook, (void **)&begin_frame_original) || !cg_draw_install()) {
		log_printf("native: unexpected code at the cgame draw or begin frame, the split screen is off");
		return 0;
	}
	if (*(base + XR(0x5c484)) != 0xe8 || !jhook_call(base + XR(0x5c484), (void *)scene_clear_hook, (void **)&scene_clear_original))
		log_printf("native: unexpected code at the scene pass's clear (0x5c484)");
	if (*(base + XR(0x456ee)) != 0xe8 || !jhook_call(base + XR(0x456ee), (void *)scene3d_hook, (void **)&scene3d_original))
		log_printf("native: unexpected code at the back end's scene pass (0x456ee), one 3D scene per frame");
	if (mode == 11 && CFG_ONCE("exp11lc", 1)) {
		// The PC's cpose_t has ONE "culled in" byte per entity (+3); the readers index it with the renderer's local player (+3 + lc), so local player 1 writes the next field.
		// These readers take local player 0 (both views share the byte, as before), and the entity lookup at 0xa4cbf too (its DObj table is the cgame's).
		static const struct { uint32_t rva; uint8_t want[7]; uint8_t put[7]; } pins[] = {
			{ 0xb20d0, { 0x48, 0x63, 0x05, 0xe1, 0xbd, 0x46, 0x00 }, { 0x31, 0xc0, 0x90, 0x90, 0x90, 0x90, 0x90 } }, // movsxd rax,[lc] -> xor eax,eax
			{ 0xb20e0, { 0x48, 0x63, 0x15, 0xd1, 0xbd, 0x46, 0x00 }, { 0x31, 0xd2, 0x90, 0x90, 0x90, 0x90, 0x90 } }, // movsxd rdx,[lc] -> xor edx,edx
			{ 0x9e547, { 0x48, 0x63, 0x05, 0x6a, 0xf9, 0x47, 0x00 }, { 0x31, 0xc0, 0x90, 0x90, 0x90, 0x90, 0x90 } },
		};
		for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]) && CFG_ONCE("exp11pin", 1); i++) { // QA: exp11pin=0 leaves the cull byte per player (+3 + lc, as on the 360)
			uint8_t *p = base + XR(pins[i].rva);
			if (memcmp(p, pins[i].want, 7)) {
				log_printf("native: exp11lc: unexpected code at rva 0x%x, not pinned", pins[i].rva);
				continue;
			}
			DWORD old;
			VirtualProtect(p, 7, PAGE_EXECUTE_READWRITE, &old);
			memcpy(p, pins[i].put, 7);
			VirtualProtect(p, 7, old, &old);
			FlushInstructionCache(GetCurrentProcess(), p, 7);
		}
		// mov reg,[lc] (8b 15/0d rel32) -> xor reg,reg + nops: the entity lookup at 0xa4cbf, and the cgame's local entity callbacks of the front end's workers
		// (0x934b0 -> 0xa5f10, 0x94bc0 -> 0xa23c0, reached from 0x121490: with 1, "CG_FreeLocalEntity: not active", the cgame has one set of local entities)
		static const struct { uint32_t rva; uint8_t modrm; } movs[] = { { 0xa4cbf, 0x15 }, { 0x934b0, 0x0d }, { 0x94bc0, 0x0d } };
		for (size_t i = 0; i < sizeof(movs) / sizeof(movs[0]); i++) {
			uint8_t *q = base + XR(movs[i].rva);
			if (q[0] != 0x8b || q[1] != movs[i].modrm || *(int32_t *)(q + 2) + (intptr_t)(q + 6) != (intptr_t)(base + XR(0x51deb8))) {
				log_printf("native: exp11lc: unexpected code at rva 0x%x, not pinned", movs[i].rva);
				continue;
			}
			DWORD old;
			VirtualProtect(q, 6, PAGE_EXECUTE_READWRITE, &old);
			const uint8_t put[6] = { 0x31, movs[i].modrm == 0x15 ? 0xd2 : 0xc9, 0x90, 0x90, 0x90, 0x90 };
			memcpy(q, put, 6);
			VirtualProtect(q, 6, old, &old);
			FlushInstructionCache(GetCurrentProcess(), q, 6);
		}
	}
#ifdef _WIN64
	if (mode == 11 && (*(base + XR(0xbccf0)) != 0xe8 || !jhook_call(base + XR(0xbccf0), (void *)fx_step_hook, (void **)&fx_step_original)))
		log_printf("native: unexpected code at the cgame's effects step (0xbccf0): the effects clock may advance twice a frame (fxclock)");
	if (mode == 11) { // (anim_step_hook: both calls of the animation step in the cgame draw)
		uint8_t *const sites[2] = {base + XR(0xbcb24), base + XR(0xbcb71)}; // (written out for the signature tools)
		int ok = 0;
		for (int i = 0; i < 2; i++) {
			uint8_t *s = sites[i];
			if (s[0] == 0xe8 && *(int32_t *)(s + 1) + (intptr_t)(s + 5) == (intptr_t)(base + XR(0x9d8b0)) && jhook_call(s, (void *)anim_step_hook, (void **)&anim_step_original))
				ok++;
		}
		if (ok != 2)
			log_printf("native: unexpected code at the cgame draw's animation steps (0xbcb24, 0xbcb71; %d of 2 hooked): the animations may advance twice a frame (animonce)", ok);
	}
#endif
	if (mode == 11 && (*(base + XR(0xd5aaf)) != 0xe8 || !jhook_call(base + XR(0xd5aaf), (void *)end_frame_hook, (void **)&end_frame_original)))
		log_printf("native: unexpected code at the screen update's end-frame call (0xd5aaf): exp11frames cannot end player 2's frame");
#ifdef _WIN64
	if (mode == 11 && (*(base + XR(0xd58e3)) != 0xe8 || *(int32_t *)(base + XR(0xd58e4)) + (intptr_t)(base + XR(0xd58e8)) != (intptr_t)(base + XR(0xbc170)) ||
			   !jhook_call(base + XR(0xd58e3), (void *)cg_step_hook, (void **)&cg_step_original)))
		log_printf("native: unexpected code at the screen update's cgame step call (0xd58e3): player 2's step does not run (p2step)");
	if (mode == 11 && (*(base + XR(0x93cf0)) != 0xe8 || *(int32_t *)(base + XR(0x93cf1)) + (intptr_t)(base + XR(0x93cf5)) != (intptr_t)(base + XR(0x94e00)) ||
			   !jhook_call(base + XR(0x93cf0), (void *)friend_hook, (void **)&friend_original)))
		log_printf("native: unexpected code at the HUD's popup friendly name call (0x93cf0): both halves show player 1's (exp11friend)");
	if (mode == 11 && CFG_ONCE("exp11p1show", 1)) { // (the native renderers too: natboot's frame end skips the present, native_nat_frame_hidden)
		if (*(base + XR(0x45757)) == 0xe8 && *(int32_t *)(base + XR(0x45758)) + (intptr_t)(base + XR(0x4575c)) == (intptr_t)(base + XR(0x45060)) &&
		    jhook_call(base + XR(0x45757), (void *)be_end_hook, (void **)&be_end_original))
			be_end_installed = 1;
		else
			log_printf("native: unexpected code at the back end's frame end call (0x45757): player 1's frame keeps flags 1, no HUD for him (exp11p1show)");
	}
	if (mode == 11 && (*(base + XR(0xd58dc)) != 0xe8 || *(int32_t *)(base + XR(0xd58dd)) + (intptr_t)(base + XR(0xd58e1)) != (intptr_t)(base + XR(0xbc1a0)) ||
			   !jhook_call(base + XR(0xd58dc), (void *)cg_time_hook, (void **)&cg_time_original)))
		log_printf("native: unexpected code at the screen update's client time call (0xd58dc): player 2's cl.serverTime is not kept (p2time)");
#endif
	if (*(base + XR(0xca523)) != 0xe8 || *(base + XR(0x82dd)) != 0xe8 || *(base + XR(0x6356)) != 0xe8 ||
	    !jhook_call(base + XR(0xca523), (void *)render_scene_hook, (void **)&render_scene_original) ||
	    !jhook_call(base + XR(0x82dd), (void *)rs_shock_hook, (void **)&rs_shock_original) ||
	    !jhook_call(base + XR(0x6356), (void *)rs_cgents_hook, (void **)&rs_cgents_original))
		log_printf("native: unexpected code at R_RenderScene's call (0xca523) or its cgame calls (0x82dd, 0x6356): player 2 renders as player 0");
	if (mode == 11)
		shock_sect_install();
	CreateThread(NULL, 0, coop_dvar_thread, NULL, 0, NULL);
	hooks_ok = 1;
	return 1;
}
#endif

// the client state of a session: context 1 starts again with every level
static void session_reset(void)
{
	char sv[16] = "";
	game_dvar_raw("sv_running", sv, sizeof(sv));
	if (sv[0] != '1' && (ctx_current || ctx_store[0][0])) {
		// the level is gone: the engine has torn the cgame, the renderer's views and the rest down already, and what the stores hold is a picture of the last frame before that (pointers
		// into memory that has been freed since). Putting it back over the engine's own reset made both the main thread and the render thread crash right after the session ended
		// (v0.6.0 reports: access violation at iw4sp.exe+0x2a7d / +0x9eb2b from the viewmodel code, session "already ended"; the render thread at +0x11b6f6 reading a stale pointer)
		for (size_t i = 0; i < NRANGES; i++)
			range_owner[i] = 0;
		ctx_current = 0;
		log_printf("native: session over with the level gone: the swapped ranges are left as the engine reset them");
	} else if (ctx_current || ctx_store[0][0]) // (the stores exist once a swap has been made; ranges left with context 1 by a light swap go back too)
		ctx_swap(0);
	ctx1_ready = 0;
	ctx_seeded = 0;
	cg_init_done = 0;
	draws_after_init = 0;
	split_on = 0;
	frame_bottom = 0;
	// A mode-11 frame sets these and puts them back after the game's own call; an engine error inside that call (a server command overflow, a GSC
	// runtime error that drops the level) longjmps past the put-back. The screen update's busy flag then stayed set and no later screen update ran,
	// the error menu included (audit 2026-11-18, native 2). The session's end clears them: no frame is under way here.
	p2_frame = 0;
#ifdef _WIN64
	ng_skip_reset_2d = 0;
#endif
	if (*(base + M11_SCR_BUSY)) {
		*(base + M11_SCR_BUSY) = 0;
		log_printf("native: the screen update's busy flag was still set at the session's end (an error inside a split frame): cleared");
	}
}

// The split screen swaps whole ranges of the game's memory between two client contexts and draws through the game's device pointer. On a build whose addresses were looked up (addr.c), an address
// that was not found stands in as a private block of zeros: a swap of such a range would silently copy nothing, and the Present hooks would find no device. The split screen is refused instead
// (docs/address-resolution.md). Nothing changes on the known build or a build with its layout (addr_mode() 0: every address resolves to itself).
static int split_addresses_ok(void)
{
	if (!addr_mode())
		return 1;
	int bad = 0;
	for (size_t i = 0; i < NRANGES; i++)
		if (!addr_resolved(ranges[i].rva)) {
			if (i == RANGE_MSEC) { // (only player 2's mouse look needs it: ctx_swap_mask leaves it out, as before 2026-11-18)
				log_printf("native: the range of frame_msec (reference rva 0x%x) was not found in this build: player 2's mouse does not turn the view in split screen (a pad does)", ranges[i].rva);
				continue;
			}
			if (i >= RANGE_LASER && i < RANGE_LASER + RANGE_LASER_N) { // (ctx_swap_mask leaves it out: one list, the lasers drawn twice in player 2's half)
				log_printf("native: a part of the laser list (reference rva 0x%x) was not found in this build: player 2's half draws each laser twice", ranges[i].rva);
				continue;
			}
			if (bad++ < 6)
				log_printf("native: the split screen needs the range at reference rva 0x%x, which was not found in this build", ranges[i].rva);
		}
#ifdef _WIN64
	if (!addr_resolved(0x410fd20)) { // the game's device pointer (RVA_D3D_DEVICE): the Present hooks are put on it
		bad++;
		log_printf("native: the split screen needs the game's device pointer (reference rva 0x410fd20), which was not found in this build");
	}
#endif
	if (bad)
		log_printf("native: split screen refused on this build: %d address(es) it needs were not found (the rest of the mod runs)", bad);
	return bad == 0;
}

static int activate(int players)
{
	if (native_active)
		return 1;
	if (players != 2) // (every per-player table of this file holds two players; night audit A9)
		return 0;
	if (!split_addresses_ok())
		return 0;
	cfg_gen++;
	mode = 11;
	p0_tail_on = cfg("p0tail", 1);
	if (!hooks_apply() || !players_apply(players)) {
		mode = 0;
		return 0;
	}
#ifdef _WIN64
	if (cfg("svslice", 0))
		svslice_install();
	if (cfg("centskip", 0))
		cent_trans_install();
	if (cfg("slicelog", 0))
		slice_install();
	if (cfg("quadlog", 0))
		quad_install();
	if (cfg("transitionlog", 0))
		trans_install();
	if (cfg("framelog", 0) || cfg("pace", 0))
		fs_install(); // the split screen: one cg_draw_hook call draws both views, so the pacing waits after both
#endif
#ifndef _WIN64
	if (cfg("seeklog", 0)) {
		void seeklog_install_(void);
		seeklog_install_();
	}
#endif
	if (cfg("entring", 1)) // default on in a split screen session ([native] entring=0 off): the cgame reads each snapshot's entity states from a copy of its own
		entring_install();
	session_reset();
	ctx_on = 1;
	InterlockedExchange(&coopdvar_on, cfg("coopdvar", 1));
	if (coopdvar_on && !getenv("MW2COOP_SPLITZONES")) { // at once: the thread looks only four times a second without a session (audit 2026-11-18, native 8), and the level's zones were loaded before its first
			   // write (they read coop 0: no Spec Ops zone, no player bodies in the split screen; zones.c also counts the session itself now)
		AcquireSRWLockExclusive(&coopdvar_lock);
		uint8_t *dvar = *(uint8_t **)(base + RVA_COOP_DVAR);
		if (dvar)
			*(int *)(dvar + 0x10) = 1;
		ReleaseSRWLockExclusive(&coopdvar_lock);
	}
	native_active = 1;
#ifdef _WIN64
	fov_scale_install(); // (the dvars exist once the cgame is up: the session starts in a level)
	own_slot_install();
#endif
	log_printf("native: split screen session on (%d local players, mode %d%s)", players, mode, from_menu ? ", from the menu" : "");
	return 1;
}

#ifndef _WIN64
// QA ([native] seeklog=1, the official 32-bit exe only): the game's FS_Seek (VA 0x4b2c80, cdecl: handle, offset, origin 0 cur / 1 end / 2 set; the file handles at
// VA 0x1952dd0, 0x11c each, +0x14 set for a file inside an .iwd) and FS_FTell (VA 0x4b4d60). A backward seek in a packed file re-opens it and inflates it again from
// its start up to the offset (VA 0x459470 in a loop): the split screen's 32-bit frames spent 20-80 ms there, called from the Miles sound library's file callback
// (VA 0x636b70). Every 5 s: per file, the seeks, the backward ones and the bytes they skipped.
static int (*seek_orig)(int, int, int);
static struct {
	char name[64];
	LONG n, back, packed, handles;
	long long skipped, fwd;
	int last_h;
} seek_files[32];
static int seek_hook(int h, int off, int origin)
{
	const uint8_t *e = (const uint8_t *)(uintptr_t)XV(0x1952dd0) + (size_t)h * 0x11c;
	const int packed = *(const int *)(e + 0x14) != 0;
	const int cur = ((int (*)(int))(base + XR(0xb4d60)))(h);
	char name[64] = "";
	for (int o = 0x18; o < 0x11c - 8 && !name[0]; o += 4) { // the handle's name: the first run of 6+ printable characters in the entry
		int k = 0;
		while (o + k < 0x11c && k < 63 && e[o + k] >= 0x20 && e[o + k] < 0x7f)
			k++;
		if (k >= 6 && !e[o + k]) {
			memcpy(name, e + o, (size_t)k);
			name[k] = 0;
		}
	}
	int i;
	for (i = 0; i < 32; i++)
		if (!seek_files[i].name[0] || !strcmp(seek_files[i].name, name))
			break;
	if (i < 32) {
		if (!seek_files[i].name[0])
			lstrcpynA(seek_files[i].name, name[0] ? name : "?", sizeof(seek_files[i].name));
		seek_files[i].n++;
		seek_files[i].packed = packed;
		const int target = origin == 2 ? off : origin == 0 ? cur + off : -1;
		if (target >= 0 && target < cur) {
			seek_files[i].back++;
			if (packed)
				seek_files[i].skipped += target;
		} else if (target > cur && packed)
			seek_files[i].fwd += target - cur;
		if (seek_files[i].last_h != h + 1)
			seek_files[i].handles++, seek_files[i].last_h = h + 1;
	}
	static DWORD at;
	if (!at)
		at = GetTickCount();
	if (GetTickCount() - at >= 5000) {
		at = GetTickCount();
		for (int k = 0; k < 32 && seek_files[k].name[0]; k++)
			if (seek_files[k].n) {
				log_printf("native: seeklog 5 s: %s%s: %ld seeks (%ld handle changes), %ld backwards, %lld KB inflated again from the start, %lld KB skipped forward", seek_files[k].name,
					   seek_files[k].packed ? " (packed)" : "", seek_files[k].n, seek_files[k].handles, seek_files[k].back, seek_files[k].skipped / 1024, seek_files[k].fwd / 1024);
				seek_files[k].n = seek_files[k].back = seek_files[k].handles = 0, seek_files[k].skipped = seek_files[k].fwd = 0;
			}
	}
	return seek_orig(h, off, origin);
}
static struct {
	uintptr_t caller;
	LONG n;
	const char *what;
} mss_callers[24];
static void mss_note(const char *what, uintptr_t caller)
{
	for (int i = 0; i < 24; i++)
		if ((mss_callers[i].caller == caller && mss_callers[i].what == what) || !mss_callers[i].caller) {
			mss_callers[i].caller = caller, mss_callers[i].what = what;
			InterlockedIncrement(&mss_callers[i].n);
			break;
		}
	static DWORD at;
	if (!at)
		at = GetTickCount();
	if (GetTickCount() - at >= 5000) {
		at = GetTickCount();
		for (int i = 0; i < 24 && mss_callers[i].caller; i++)
			if (mss_callers[i].n) {
				log_printf("native: seeklog 5 s: %s from exe+0x%x: %ld calls", mss_callers[i].what, (unsigned)(mss_callers[i].caller - (uintptr_t)base), mss_callers[i].n);
				mss_callers[i].n = 0;
			}
	}
}
static int(__stdcall *set_ms_orig)(void *, int);
static int __stdcall set_ms_hook(void *stream, int ms)
{
	mss_note("AIL_set_stream_ms_position", (uintptr_t)__builtin_return_address(0));
	return set_ms_orig(stream, ms);
}
static void(__stdcall *close_stream_orig)(void *);
static void __stdcall close_stream_hook(void *stream)
{
	mss_note(ctx_current == 1 ? "AIL_close_stream (context 1)" : "AIL_close_stream (context 0)", (uintptr_t)__builtin_return_address(0));
	{ // QA: the code addresses on the stack (values inside the exe's code that follow a call instruction), once every 5 s, for the stream stops of context 1
		static DWORD at;
		if (ctx_current == 1 && GetTickCount() - at >= 5000) {
			at = GetTickCount();
			const volatile uintptr_t *sp = (const volatile uintptr_t *)__builtin_frame_address(0);
			char line[400];
			int n = snprintf(line, sizeof(line), "native: seeklog stream stop stack (context 1, thread %lu):", GetCurrentThreadId()), k = 0;
			for (int i = 0; i < 600 && k < 14 && n < (int)sizeof(line) - 12; i++) {
				const uintptr_t v = sp[i];
				if (v < 0x401006 || v > 0x6f0000)
					continue;
				const uint8_t *c = (const uint8_t *)v;
				if (c[-5] != 0xe8 && !(c[-2] == 0xff && (c[-1] & 0xf8) == 0xd0) && !(c[-6] == 0xff && c[-5] == 0x15) && !(c[-3] == 0xff && (c[-2] & 0x38) == 0x10))
					continue;
				n += snprintf(line + n, sizeof(line) - n, " %x", (unsigned)v);
				k++;
			}
			log_printf("%s", line);
		}
	}
	close_stream_orig(stream);
}
static void *(__stdcall *open_stream_orig)(void *, const char *, int);
static void *__stdcall open_stream_hook(void *dig, const char *name, int mem)
{
	mss_note(ctx_current == 1 ? "AIL_open_stream (context 1)" : "AIL_open_stream (context 0)", (uintptr_t)__builtin_return_address(0));
	return open_stream_orig(dig, name, mem);
}
static void seeklog_iat(const char *sym, void *hook, void **orig)
{
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); imp->Name; imp++) {
		if (_strnicmp((const char *)(base + imp->Name), "mss32", 5) || !imp->OriginalFirstThunk)
			continue;
		IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk), *names = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
		for (; iat->u1.Function; iat++, names++) {
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal) || strcmp((const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, sym))
				continue;
			DWORD old;
			if (VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
				*orig = (void *)(uintptr_t)iat->u1.Function;
				iat->u1.Function = (uintptr_t)hook;
				VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
				log_printf("native: seeklog: %s counted", sym);
			}
		}
	}
}
void seeklog_install_(void)
{
	seeklog_iat("_AIL_set_stream_ms_position@8", (void *)set_ms_hook, (void **)&set_ms_orig);
	seeklog_iat("_AIL_open_stream@12", (void *)open_stream_hook, (void **)&open_stream_orig);
	seeklog_iat("_AIL_close_stream@4", (void *)close_stream_hook, (void **)&close_stream_orig);
	static int done;
	if (done || game_is_iw4x())
		return;
	done = 1;
	static const uint8_t pro[11] = {0x56, 0x8b, 0x74, 0x24, 0x08, 0x69, 0xf6, 0x1c, 0x01, 0x00, 0x00};
	log_printf("native: seeklog %s", hook_function(base + XR(0xb2c80), pro, sizeof(pro), (void *)seek_hook, (void **)&seek_orig) ? "on" : "FAILED");
}
#endif

// Console commands for the QA bots and for the lobby: mw2coop_native_start [pad], mw2coop_native_stop, mw2coop_native_launch <map> [pad] [start] [player 1 device] (session on, then the map)
static void native_start_f(void)
{
	const char *a = game_cmd_argv(1);
	native_split_start(a[0] ? atoi(a) : -1);
}
static void native_stop_f(void)
{
	native_split_stop();
}
static void native_launch_f(void)
{
	char map[64];
	lstrcpynA(map, game_cmd_argv(1), sizeof(map));
	const char *a = game_cmd_argv(2), *a0 = game_cmd_argv(4); // (QA: a fourth argument is player 1's device, as the lobby gives it; default the keyboard)
	if (!map[0] || strpbrk(map, " \t;\"\n"))
		return;
	if (!native_split_start2(a0[0] ? atoi(a0) : -1, a[0] ? atoi(a) : -1))
		return;
	char text[200], start[64];
	lstrcpynA(start, game_cmd_argv(3), sizeof(start));
	if (strpbrk(start, " \t;\"\n"))
		start[0] = 0;
	{ // QA: [native] qa_skill=<0..3> stands for the lobby's difficulty pick of both players, through the lobby's own path (progress_split_skill)
		const int qs = cfg("qa_skill", -1);
		if (qs >= 0 && qs <= 3)
			progress_split_skill(qs, qs);
	}
	snprintf(text, sizeof(text), "set start \"%s\"\nset coop_mapName %s\nmap %s\n", start, map, map);
	game_cbuf_addtext(text);
}
// QA: mw2coop_press <binding> <1|0> presses or releases a game button ("speed_throw", "forward", "attack") for player 0 through the engine's key handler, as the pad code does for player 1
// (console "+forward" does not move a player without a key number)
static void native_press_f(void)
{
	char name[40];
	lstrcpynA(name, game_cmd_argv(1), sizeof(name));
	log_printf("native: press %s %s (context %d live)", name, game_cmd_argv(2), ctx_current);
	if (!name[0] || ctx_current != 0)
		return;
	pad_binding(name, game_cmd_argv(2)[0] != '0');
}
static void native_qa_move_f(void)
{
	qa_c0_fwd = game_cmd_argv(1)[0] == '1';
	qa_c0_fire = game_cmd_argv(2)[0] == '1';
}
static cmd_function_t native_commands[] = {
	{NULL, "mw2coop_qa_move", NULL, NULL, native_qa_move_f CMD_NODE_FLAG},
	{NULL, "mw2coop_press", NULL, NULL, native_press_f CMD_NODE_FLAG},
	{NULL, "mw2coop_native_start", NULL, NULL, native_start_f CMD_NODE_FLAG},
	{NULL, "mw2coop_native_stop", NULL, NULL, native_stop_f CMD_NODE_FLAG},
	{NULL, "mw2coop_native_launch", NULL, NULL, native_launch_f CMD_NODE_FLAG},
};

// For padglyphs.c: the player whose half's texts are being made (his context live), if he plays on a pad; *tactical: his layout
static int text_pad_player(int *tactical)
{
	if (!native_active || ctx_current < 0 || ctx_current > 1 || pad_dev[ctx_current] < 0)
		return -1;
	padopts_t o;
	padopts_get(ctx_current, &o);
	*tactical = o.tactical;
	return ctx_current;
}

// A new level's zones are loading (zones.c). Player 2's commands of the previous level must not reach the new level's first moves: their weapon index names a weapon
// of that level. Endgame (af_chase) loads few weapons; after Just Like Old Times the index (52) had no definition there and player 2's first move crashed
// (BG_GetWeaponDef NULL: 32-bit read of 0x66f at rva 0x2c6d, 64-bit read of 0x877 at rva 0x86f1b; split screen level chains, 2026-11-17). Three places held them:
// context 1's own new commands (held back until this level's context 1 is up), the server's last command of client 1 and the client's two session commands
// (cleared here; the engine clears client 0's itself). Checked: Just Like Old Times -> Endgame -> the Museum in the 64-bit, 32-bit and iw4x-sp split screen and in a
// 64-bit network pair. [native] p2cmdclear=0 turns it all off.
static uint8_t *volatile p2_gclient; // player 2's gclient as native_poll_players last saw it
void native_level_load(void)
{
#ifdef _WIN64
	const unsigned ref_clients = 0x19ada98;
#else
	const unsigned ref_clients = 0x1723988;
#endif
	mix_level_load();
	if (!base || !cfg("p2cmdclear", 1))
		return;
	// Context 1 still runs on the previous level's state until this level's cgame is up and it is seeded again ("context 1 initialised"); the commands it makes
	// meanwhile carry that level's weapon and time, and usercmd_step_inner gave them to the server as player 2's: the crash below came from one of them
	// (time 14800, older than the cleared ones; WINMM -> SV_ClientThink rva 0xe7750 -> ClientThink_real -> Pmove). They wait for the new context.
	if (ctx1_ready) {
		p2_cmd_hold = 1;
		log_printf("native: a new level: player 2's commands wait for this level's context 1");
	}
	if (!addr_resolved(ref_clients))
		return;
	uint8_t *cmd = base + RVA_SV_CLIENTS + SV_CLIENT_STRIDE + 0x1630;
	const int t = *(const int *)cmd, weapon = *(const uint16_t *)(cmd + 0x14);
	memset(cmd, 0, 0x40);
	log_printf("native: client 1's last server command of the previous level cleared (time %d, weapon %d)", t, weapon);
	// The game's own copies are the ones the move reads: ClientThink_real (64-bit rva 0x15be30, 32-bit VA 0x457390, the same in the iw4x-sp exe) builds player 2's
	// move from his session command (gclient + 0xaca8, the previous one at + 0xace8), and the level spawn keeps these too (the crash with the clear above alone: time
	// 14628, weapon 52, offhand 9; the 64-bit build crashed the same way, read of 0x877 at rva 0x86f1b). At the level load the entity has no client any more: the
	// client (a static array of the game) is the one native_poll_players saw while the previous level ran.
	uint8_t *gc = p2_gclient;
	if (gc && !IsBadWritePtr(gc + 0xaca8, 0x80)) {
		log_printf("native: client 1's session commands of the previous level cleared (time %d weapon %d offhand %d, previous time %d weapon %d)", *(const int *)(gc + 0xaca8),
			   *(const uint16_t *)(gc + 0xaca8 + 0x14), *(const uint16_t *)(gc + 0xaca8 + 0x18), *(const int *)(gc + 0xace8), *(const uint16_t *)(gc + 0xace8 + 0x14));
		memset(gc + 0xaca8, 0, 0x80);
	}
}

// Every 250 ms (menus_poll, control thread): player 2's client, g_entities[1].client, while a level runs.
void native_poll_players(void)
{
#ifdef _WIN64
	const unsigned ref_entities = 0xd9e070;
#else
	const unsigned ref_entities = 0xaa76b8;
#endif
	if (!base || !addr_resolved(ref_entities))
		return;
	uint8_t *gc = *(uint8_t *volatile *)(base + RVA_GENTITIES + GENT_SIZE + 0x108);
	if (gc)
		p2_gclient = gc;
#ifndef _WIN64
	{ // how many world sound starts of context 1 were muted (sound_once_install32), every 10 s while it grows
		extern volatile LONG snd_muted32;
		static LONG last;
		static DWORD at;
		const LONG now_n = snd_muted32;
		if (now_n != last && GetTickCount() - at >= 10000) {
			log_printf("native: %ld world sound starts of context 1 muted so far (+%ld)", (long)now_n, (long)(now_n - last));
			last = now_n, at = GetTickCount();
		}
	}
	if (CFG_ONCE("rthitch", 0)) { // QA: the process's used address space (a 32-bit process has 4 GB at most; the driver's allocations live in it too)
		static DWORD at2;
		if (GetTickCount() - at2 >= 10000) {
			MEMORYSTATUSEX ms = {sizeof(ms)};
			if (GlobalMemoryStatusEx(&ms))
				log_printf("native: rthitch: address space used %.0f MB of %.0f MB", (double)(ms.ullTotalVirtual - ms.ullAvailVirtual) / 1048576.0, (double)ms.ullTotalVirtual / 1048576.0);
			at2 = GetTickCount();
		}
	}
#endif
}


// The markers' wheel (src/ping.c) holds the view of a player who has it open and gives his mouse's movement to it, in network co-op too, where no split screen
// session hooks anything: the mouse read of CL_MouseMove (64-bit call at rva 0xcfd23; 32-bit 0x179000) is hooked from the start. The hook passes the movement on as
// the game has it (context 0's, no session) unless the wheel is open. The split screen's context hooks leave that call to this one. [native] pingmouse=0 off.
static void mouse_early_install(void)
{
	if (mouse_early || !cfg("pingmouse", 1))
		return;
#ifdef _WIN64
	mouse_early = jhook_call(base + XR(0xcfd23), mouse_read_hook, (void **)&mouse_read_original);
#else
	mouse_early = jhook_call(base + XR(0x179000), n32_mouse_stub, &mouse_read_target);
#endif
	log_printf("native: the mouse read %s", mouse_early ? "is hooked (the markers' wheel holds the view)" : "is not as expected: the markers' wheel cannot hold the view");
}


void native_install(void)
{
	char path[MAX_PATH];
	DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
	while (n > 0 && path[n - 1] != '\\')
		n--;
	path[n] = 0;
	lstrcatA(path, "mw2coop\\settings.ini");
	lstrcpyA(settings_path, path);
	base = (uint8_t *)GetModuleHandleA(NULL);
	if (cg_draw_install())
		log_printf("native: full-window aspect correction hook on");
	else
		log_printf("native: full-window aspect correction hook not installed");
	if (cfg("filmfit", 1)) {
		log_printf("native: Bink 16:9 movie fitting enabled");
		bink_hooks_install();
		CreateThread(NULL, 0, bink_device_hooks_thread, NULL, 0, NULL);
	} else {
		log_printf("native: Bink 16:9 movie fitting disabled by settings");
	}
	for (size_t i = 0; i < sizeof(native_commands) / sizeof(native_commands[0]); i++)
		game_add_command(&native_commands[i]);
	inputlog_open();
	padglyphs_install(text_pad_player);
#ifdef _WIN64
	if (cfg("weaponfov", 0) > 0 || cfg("weaponfovlog", 0) || cfg("presentlog", 0) || settings_fsr3()) // presentlog: the frame cadence of the plain game too (QA: FSR against native)
		CreateThread(NULL, 0, device_hooks_thread, NULL, 0, NULL);
#endif
#ifndef _WIN64
	vid_restart_install(); // (the server command table is filled at the start, before this runs)
#endif
#ifdef _WIN64
	rawinput_install();
	if (cfg("loadprof", 0) > 0)
		loadprof_start((unsigned)cfg("loadprof", 0), (unsigned)cfg("loadprofat", 0));
	if (cfg("aspectlog", 0))
		aspectlog_start();
	if (cfg("readlog", 0))
		readlog_start();
	vid_restart_install();
	if (cfg("treelog", 0) && cfg("players", 0) < 2 && !jhook_function(base + XR(0x2bb0), dobj_add_bytes, sizeof(dobj_add_bytes), dobj_add_hook, (void **)&dobj_add_original))
		log_printf("native: dobj add bytes differ (treelog in the plain game)");
#endif
	mouse_early_install();
	// the QA runs: settings.ini [native] players=2 starts the session at once (the menu's button does it later, native_split_start)
	int players = cfg("players", 0);
	if (players >= 2)
		activate(players);
}

const char *native_crash_state(void)
{
	static char text[96];
	snprintf(text, sizeof(text), "session %d, context %d live, context 1 ready %d, split %d, frame_bottom %d", native_active, ctx_current, ctx1_ready, split_on, frame_bottom);
	return text;
}

// One key press for the split screen lobby's pad (menus.c mw2coop_menukey): the engine's key event, down and up
void native_menu_key(int key)
{
	unsigned t = (unsigned)GetTickCount();
#ifdef _WIN64
	((void (*)(int, int, int, unsigned))(base + XR(0xd0f80)))(0, key, 1, t);
	((void (*)(int, int, int, unsigned))(base + XR(0xd0f80)))(0, key, 0, t);
#else
	// (the key event's hook goes on with the split screen session; outside one, mw2coop_menukey calls the engine's key event itself, VA 0x4d2350, cdecl: QA of a
	// network pair's menus, 2026-11-24)
	void (*key_event)(int, int, int, unsigned) = key_event_original;
	if (!key_event && addr_resolved(0xd2350))
		key_event = (void (*)(int, int, int, unsigned))(base + XR(0xd2350));
	if (!key_event)
		return;
	key_event(0, key, 1, t);
	key_event(0, key, 0, t);
	cbuf_run_now();
#endif
}

int native_split_active(void)
{
	return native_active;
}

// Where context `ctx` keeps the bytes the game has at `rva` (a reference rva inside one of the swapped ranges): the live memory while ctx is the current context, else its
// store. NULL when no split screen session has its two contexts, or rva is in no range. For code that handles local client <ctx> while only client 0's statics exist
// (the game's saves: saves.c).
void *native_ctx_addr(int ctx, unsigned rva)
{
	if (!native_active || !ctx_on || !ctx1_ready || !ctx_store[0][0] || (ctx != 0 && ctx != 1))
		return NULL;
	for (size_t i = 0; i < NRANGES; i++)
		if (rva >= ranges[i].rva && rva < ranges[i].rva + ranges[i].size) {
			if (i == 0 && cent_all_on() && RANGE_RVA(0) + (rva - ranges[0].rva) >= RVA_CENT) // (the entity table is one for both contexts: always the live one)
				return base + RANGE_RVA(0) + (rva - ranges[0].rva);
			return ctx == ctx_current ? (void *)(base + RANGE_RVA(i) + (rva - ranges[i].rva)) : (void *)(ctx_store[ctx][i] + (rva - ranges[i].rva));
		}
	return NULL;
}

// weaponfov.c: whose view the render thread is drawing in the mode 11 split screen, 0 or 1 (player 2's renderer frame is the one whose frame data is p2_fd, as the
// clears tell the halves apart), -1 outside one
int native_live_ctx(void)
{
	return native_active && ctx_on && ctx_current == 1 ? 1 : 0;
}

int native_render_player(void)
{
	if (!native_active || mode != 11 || !split_on || !p2_fd)
		return -1;
	return *(void *volatile *)(base + M11_BE_FD) == p2_fd ? 1 : 0;
}

// The Start of a split screen lobby: the game becomes a two local players game; the caller launches the mission. dev0 / dev1: the players' devices (an XInput pad 0-3,
// -1 the keyboard and mouse)
int native_split_start2(int dev0, int dev1)
{
	if (native_active)
		return 1;
	from_menu = 1;
	menu_pad0 = dev0;
	menu_pad = dev1;
	pad_dev[0] = dev0;
	p1pad = dev1;
	int ok = activate(2);
	if (ok)
		game_cbuf_addtext("set mw2coop_native_split 1\n"); // the scripts' issplitscreen() is false here: maps\_mw2coop sets the console's split layout of the revive HUD from this
	if (ok && !cfg("waitmsg", 0)) { // both players are on this PC: nothing to wait for, not even on the loading screen (set now: an asset lookup does not go while a level loads)
		localize_override("COOP_WAITINGFORPLAYER", " ");
		localize_override("SPECIAL_OPS_WAITING_OTHER_PLAYER", " ");
	}
	return ok;
}
int native_split_start(int pad)
{
	return native_split_start2(-1, pad);
}

// The session ends (back in the menus): the patches come out and the game is what it was
void native_split_stop(void)
{
	if (!native_active)
		return;
	localize_override("COOP_WAITINGFORPLAYER", NULL);
	localize_override("SPECIAL_OPS_WAITING_OTHER_PLAYER", NULL);
	hud_layout_tick(0); // the HUD menus get the full screen layout back
	game_cbuf_addtext("set mw2coop_native_split 0\n");
	native_active = 0;
	mode = 0;
	ctx_on = 0;
	AcquireSRWLockExclusive(&coopdvar_lock); // (the forcing thread has written its last value once this is held)
	InterlockedExchange(&coopdvar_on, 0);
	ReleaseSRWLockExclusive(&coopdvar_lock);
	{
		uint8_t *dvar = *(uint8_t **)(base + RVA_COOP_DVAR); // the scripts' co-op dvar goes back to 0 (the game's own co-op sets it itself)
		if (dvar)
			*(int *)(dvar + 0x10) = 0;
	}
	session_reset();
	journal_restore(1);
	rumble_reset();
	log_printf("native: split screen session over, the game's own player count is back");
}
