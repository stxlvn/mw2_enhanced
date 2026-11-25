// A field of view of its own for the weapon in the player's hands: the dvar cg_weaponFov (Vlad, 0.6.0 ideas; the owner's night list 2026-11-23). 65 (the default)
// gives the weapon the console's field of view; 0 keeps the game's way (the weapon follows the view's field of view); 1..160 any other value. The view FOV
// is read from the game's cg_fov * cg_fovScale, so the weapon's correction follows the Enhanced page's direct cg_fov slider.
//
// How the game draws the weapon (static reading of the 64-bit exe, 2026-11-23): one set of view parameters for the scene; the weapon's draws (the view model,
// "depth hack") run in the depth range 0..1/64 with a projection of their own that the game derives from the world's at rva 0x4fd00, the only reader of the
// weapon's near clip (r_znear_depthhack): it copies the view's projection to the source state's +0x100 and, when the depth hack flag (+0x16b8) is 2, puts the
// weapon's near clip in. Every matrix of the weapon's draws (view-projection, world-view-projection, the inverses) is derived from that one by the game's own
// version cache, and the world's draws never go through it. So the weapon gets another field of view by scaling the x and y columns of that matrix by
// k = tan(view fov / 2) / tan(weapon fov / 2) right after the game has made it: on Direct3D 9 and on the mod's own DirectX 11/12 renderers alike (they call the
// game's matrix code). The earlier try (a device hook on SetViewport, 2026-11-04) took the depth range the wrong way round (MinZ > 0 is the world's) and bent
// the world.
//
// Aiming down the sights: the game narrows the view to the weapon's own ADS field of view and the sights must sit where the game puts them, so k goes back to
// 1 as the view zooms in (k = 1 + (k_hip - 1) * (1 - zoom)). The zoom is the game's own fraction, the one its view's field of view is blended by: the raw
// ADS fraction (the weapon on its way to the eye) remapped by the weapon's zoom-in and zoom-out fractions, so the view starts to narrow only once the weapon
// is part of the way up (64-bit rva 0xc3f70, 32-bit VA 0x41d630: f(cg, weaponDef); every caller passes local client 0's cgame). The mod keeps what that
// getter returned last, for each local client (a stub around it; the split screen asks it for both players in turn); before 2026-11-24 the blend followed the raw fraction and the weapon changed size before the view zoomed (owner:
// "do it the way the view zooms", after Vlad's CoD UO weapon FOV, which follows the view's field of view). MW2COOP_WF_RAWADS=1: the raw fraction (QA).
//
// The weapon's effects (owner 2026-11-24, after the FoV changer of CoD SCZ): the muzzle flash, the barrel's smoke and the spent cases are effects bolted to
// the view model's bones (the view weapon's draw, 64-bit rva 0xc0b80, plays the flash when the fire event has set the entity's flag +0x18c bit 4 or 8:
// FX_PlayBoltedEffect 64-bit rva 0x119ee0 with the view model's DObj handle, 0x800 and up). The effects system finds a bolt's place with the game's bone
// matrix (64-bit rva 0x9d470, 32-bit VA 0x501070: (pose, DObj, bone, axis out, origin out)) when it spawns one (64-bit FX_GetBoneOrientation rva 0x11dd90,
// its call at 0x11ddff; 32-bit VA 0x4fd2a0, call at 0x4fd30a) and in every frame's update of an effect that runs relative to its bolt (64-bit rva 0x120230,
// call at 0x12036b; 32-bit VA 0x598510 goes through 0x4fd2a0 too), and draws the effect with the world's projection: so it stayed where the weapon would be
// without cg_weaponFov (a smaller weapon hid it behind itself, a larger one left it short of the barrel; until 2026-11-24 the mod moved the place of the
// fire sound, 64-bit 0xc3acb, by mistake, and the frames looked at the flash's random size and time). A point at view depth z drawn with the weapon's
// projection (x and y scaled by k) lands where a point at depth z/k with the same x and y lands with the world's, at k times the size: so the bone matrix's
// origin is moved along the view's forward axis to z/k (the view's origin and axis: the cgame's refdef, 64-bit rva 0x42d310 / 32-bit VA 0x77fae8, forward at
// +0xc) for the view model's poses only (64-bit rva 0x43b618 and the left hand's 0x43b688; 32-bit VA 0x78d618, 0x78d67c), with the k of the player whose
// cgame runs. The laser's beam starts at a view model tag the same way (64-bit rva 0xa5860, its bone call at 0xa58d4; 32-bit VA 0x4f2d40, call at 0x4f2dc7):
// its start is moved too (Exodus' laser designator left a gap to a larger weapon's muzzle). QA switches: MW2COOP_FLASH_OFF=1 the effects where the game puts them; MW2COOP_FLASH_ZMAX=<units> no deeper than that (MW2COOP_FLASH_SIDE1=1:
// then the sideways offset kept); MW2COOP_FLASH_SHOT=<name> frames <name>-<n>.bmp when an effect is bolted to the view model (at most one in 0.3 s; 32-bit:
// FX_PlayBoltedEffect's call at VA 0x596063 marks a spawn), 30 and 60 ms later (<n>b, <n>c), its place in the log. Known limits: the hands of scripted scenes (climbs, the knife fight) are a world model and keep the view's
// FOV.
//
// The hook: 0x4fd00's caller (0x4fd60) goes on using RCX and R8 after the call (the whole-program optimiser's own convention), so the replacement is a small
// machine code stub that keeps every register, calls the original through the trampoline, then the C step with the source state.
//
// The 32-bit game (the official exe and iw4x-sp's 1.0.159, 2026-11-24): the same function at VA 0x541310 (1.0.159 0x541b30), the source state in EAX (the
// optimiser's own convention again): it copies the view's projection from +0x1390 to +0x100 and, when +0x1628 is 2, puts the weapon's near clip (+0x149c) in.
// Its callers are VA 0x541360 (the weapon's matrices, while the flag is 2) and a tail jump from the matrix getter VA 0x5415c0. The ADS fraction is the cgame's
// +0x15c4c (cg VA 0x75f2a0; VA 0x41d630 reads it, the view's field of view at VA 0x440d20 blends with it). cg_fov and cg_fovScale are found by name.
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "hook.h"
#include "log.h"
#include "native.h"
#include "shot.h"
#include "weaponfov.h"

#ifdef _WIN64
#define REF_WF_PROJECTION 0x4fd00	   // the weapon's projection from the view's (source state in RCX)
#define REF_WF_FOV_DVAR 0x51e1a8	   // dvar_t *cg_fov
#define REF_WF_FOVSCALE_DVAR 0x51e1b8  // dvar_t *cg_fovScale
#define REF_WF_CG 0x40caa0		   // the cgame's state (local client 0); +0x15c60 is the ADS fraction (0..1)
#define REF_WF_REGISTER_FLOAT 0x26cb90 // dvar_t *Dvar_RegisterFloat(name, value, min, max, flags, description)
#define WF_ADS_OFFSET 0x15c60
#define WF_FLAGS 0x16b8			   // the source state's depth hack flag: 2 while the weapon's draws are made
#define WF_MATRIX 0x100			   // the source state's projection (row vectors: clip = v * P)
#define REF_WF_ZOOM 0xc3f70		   // float zoom(cg, weaponDef): the raw ADS fraction remapped by the weapon's zoom-in/out fractions (+0x448, +0x44c)
#define REF_WF_BONE 0x9d470		   // int (pose, DObj, bone, axis out, origin out): a bone's world matrix
#define REF_WF_FX_SPAWN 0x11ddff	   // FX_GetBoneOrientation's call of it (a bolted effect is spawned)
#define REF_WF_FX_UPDATE 0x12036b	   // the bolted effect's frame update's call of it
#define REF_WF_LASER 0xa58d4		   // the laser's call of it: the beam's start (rva 0xa5860, tag_laser or tag_flash)
#define REF_WF_VMPOSE 0x43b618	   // the view model's pose (the left hand's of akimbo at +0x70)
#define WF_VMPOSE2 0x70
#define REF_WF_REFDEF 0x42d310	   // the cgame's view origin, then its axis (forward, left, up)
static const uint8_t zoom_head[14] = {0xf3, 0x0f, 0x10, 0x89, 0x60, 0x5c, 0x01, 0x00, 0x0f, 0x57, 0xd2, 0x0f, 0x2f, 0xd1}; // movss xmm1,[rcx+0x15c60]; xorps; comiss
#else
#define REF_WF_PROJECTION 0x141310	   // VA 0x541310: the weapon's projection from the view's (source state in EAX)
#define REF_WF_CG 0x35f2a0		   // VA 0x75f2a0: the cgame's state (local client 0); +0x15c4c is the ADS fraction (0..1)
#define REF_WF_REGISTER_FLOAT 0x9f700  // VA 0x49f700, cdecl: dvar_t *Dvar_RegisterFloat(name, value, min, max, flags, description)
#define WF_ADS_OFFSET 0x15c4c
#define WF_FLAGS 0x1628			   // the source state's depth hack flag: 2 while the weapon's draws are made
#define WF_MATRIX 0x100			   // the source state's projection
#define REF_WF_ZOOM 0x1d630		   // VA 0x41d630, cdecl float zoom(cg, weaponDef) in st0 (the weapon's zoom-in/out fractions at +0x300, +0x304)
#define REF_WF_ZOOM_CALL 0x40dbc	   // VA 0x440dbc: the view's field of view (VA 0x440d20) calls it here; the getter itself lies in a range the official exe hashes
#define REF_WF_BONE 0x101070	   // VA 0x501070, cdecl int (pose, DObj, bone, axis out, origin out)
#define REF_WF_FX_SPAWN 0xfd30a	   // VA 0x4fd30a: FX_GetBoneOrientation's call of it (the spawn and every frame's update go through it)
#define REF_WF_LASER 0xf2dc7		   // VA 0x4f2dc7: the laser's call of it (VA 0x4f2d40, "Can't draw laser from ent")
#define REF_WF_FX_BOLT 0x196063	   // VA 0x596063: FX_PlayBoltedEffect's call of FX_GetBoneOrientation (VA 0x4fd2a0): tells a spawn from an update
#define REF_WF_FX_BONEORIENT 0xfd2a0  // VA 0x4fd2a0, cdecl bool (local client, DObj handle, bone, orientation out)
#define REF_WF_VMPOSE 0x38d618	   // VA 0x78d618: the view model's pose (the left hand's at +0x64)
#define WF_VMPOSE2 0x64
#define REF_WF_REFDEF 0x37fae8	   // VA 0x77fae8: the view origin, then the axis
static const uint8_t zoom_head[10] = {0x8b, 0x4c, 0x24, 0x04, 0xd9, 0x81, 0x4c, 0x5c, 0x01, 0x00}; // mov ecx,[esp+4]; fld dword [ecx+0x15c4c]
#endif

static uint8_t *base;
static dvar_t *weapon_fov;
static void *original; // the trampoline (the stub calls through this variable)
static volatile LONG scaled_count;
static float last_k = 1.0f;
static void *zoom_original;		 // the zoom getter's trampoline
static volatile float zoom_last[2]; // what it returned last for each local client (zoom_note, from the stub: the split screen asks it for both players in turn)
static int zoom_hooked;
static int (*bone_original)(void *pose, void *dobj, int bone, float *axis, float *origin);
static volatile LONG flash_moved;
static float flash_last_k = 1.0f;

#ifdef _WIN64
static float dvar_float(uintptr_t rva_of_pointer, float fallback)
{
	dvar_t *d = *(dvar_t **)(base + rva_of_pointer);
	return d ? d->current.value : fallback;
}

static float view_fov(void)
{
	return dvar_float(XR(REF_WF_FOV_DVAR), 65.0f) * dvar_float(XR(REF_WF_FOVSCALE_DVAR), 1.0f);
}
#else
static dvar_t *fov_dvar, *fovscale_dvar; // (found by name on the main thread, weaponfov_tick)

static float view_fov(void)
{
	dvar_t *f = fov_dvar, *s = fovscale_dvar;
	return (f ? f->current.value : 65.0f) * (s ? s->current.value : 1.0f);
}
#endif

// The view/weapon FOV ratio is stable across the many projection and bolted-effect calls in a
// frame. Cache it per thread: render/backend threads do not contend, and either dvar changing
// invalidates only that thread's entry. The ADS blend below remains per call and per player.
static float weapon_hip_scale(float view, float weapon)
{
	static _Thread_local float cached_view = -1.0f;
	static _Thread_local float cached_weapon = -1.0f;
	static _Thread_local float cached_scale = 1.0f;
	if (view != cached_view || weapon != cached_weapon) {
		cached_view = view;
		cached_weapon = weapon;
		cached_scale = tanf(view * 3.14159265f / 360.0f) / tanf(weapon * 3.14159265f / 360.0f);
	}
	return cached_scale;
}

// The weapon's scale k for a player (his ADS fraction, the zoom getter's last answer for him), or 0 when cg_weaponFov is 0 (the game's way)
static float weapon_k(float ads, int player)
{
	const float g = weapon_fov ? weapon_fov->current.value : 0.0f;
	if (!(g >= 1.0f && g <= 170.0f))
		return 0.0f;
	const float f = view_fov();
	if (!(f >= 1.0f && f <= 170.0f))
		return 0.0f;
	const float k_hip = weapon_hip_scale(f, g);
	if (!(ads >= 0.0f && ads <= 1.0f))
		ads = 0.0f;
	float zoom = ads;
	if (zoom_hooked && ads > 0.0f && ads < 1.0f) {
		// (at 0 and 1 the getter gives 0 and 1 itself; between them its last answer for the player drawn, this frame's: the view's field of view asks it)
		zoom = zoom_last[player];
		if (!(zoom >= 0.0f && zoom <= 1.0f))
			zoom = ads;
	}
	// Keep the full projection ratio. Capping k (previously to 0.3..3.0) makes the weapon
	// drift with cg_fov again at the extremes: for cg_weaponFov 65 those caps are reached
	// at view FOVs of about 21.6 and 124.8 degrees. The game FOV validity checks above
	// already keep tan() away from its singularities; no arbitrary scale cap is needed.
	return 1.0f + (k_hip - 1.0f) * (1.0f - zoom);
}

static char flash_later_name[64];
static DWORD WINAPI flash_later_shot(LPVOID name) // QA (MW2COOP_FLASH_SHOT): frames 30 and 60 ms after an effect is bolted to the view model (<name>b, <name>c)
{
	char n[72];
	Sleep(30);
	snprintf(n, sizeof(n), "%sb", (const char *)name);
	native_split_active() ? shot_request_present(n) : shot_request(n);
	Sleep(30);
	snprintf(n, sizeof(n), "%sc", (const char *)name);
	native_split_active() ? shot_request_present(n) : shot_request(n);
	return 0;
}

static int fx_off = -1, fx_side_keep;
#ifndef _WIN64
static int fx_spawning; // 32 bits: FX_PlayBoltedEffect's call of FX_GetBoneOrientation is under way
static int (*boneorient_original)(int lc, int handle, int bone, float *orient);
static int fx_bolt_hook(int lc, int handle, int bone, float *orient)
{
	fx_spawning = 1;
	const int r = boneorient_original(lc, handle, bone, orient);
	fx_spawning = 0;
	return r;
}
#endif
static float fx_zcap;
static const char *fx_shot_prefix;

// The main thread (the cgame of the player whose view it is): a bone of the view model as the effects system sees it, its origin moved to the depth the
// weapon's projection shows it at (z/k along the view's forward axis); `spawn`: an effect is being bolted to it now
static void fx_bone_move(float *origin, int spawn)
{
	if (fx_off < 0) {
		fx_off = getenv("MW2COOP_FLASH_OFF") != NULL;
		const char *e = getenv("MW2COOP_FLASH_ZMAX");
		fx_zcap = e ? (float)atof(e) : 0.0f;
		fx_side_keep = getenv("MW2COOP_FLASH_SIDE1") != NULL;
		fx_shot_prefix = getenv("MW2COOP_FLASH_SHOT");
	}
	static LONG shots;
	static DWORD shot_at;
	int shot_now = 0;
	if (spawn && fx_shot_prefix && *fx_shot_prefix && GetTickCount() - shot_at > 300) {
		shot_at = GetTickCount();
		char name[64];
		const long sn = (long)InterlockedIncrement(&shots);
		snprintf(name, sizeof(name), "%s-%ld", fx_shot_prefix, sn);
		if (native_split_active()) // (the split screen: the composed picture, both views)
			shot_request_present(name);
		else
			shot_request(name);
		lstrcpynA(flash_later_name, name, sizeof(flash_later_name));
		HANDLE t = CreateThread(NULL, 0, flash_later_shot, flash_later_name, 0, NULL);
		if (t)
			CloseHandle(t);
		shot_now = 1;
	}
	if (fx_off || !original)
		return;
	const float k = weapon_k(*(volatile float *)(base + XR(REF_WF_CG) + WF_ADS_OFFSET), native_live_ctx());
	if (k == 0.0f || fabsf(k - 1.0f) < 0.001f)
		return;
	const float *rd = (const float *)(base + XR(REF_WF_REFDEF)); // origin [0..2], forward [3..5]
	const float v[3] = {origin[0] - rd[0], origin[1] - rd[1], origin[2] - rd[2]};
	const float d = v[0] * rd[3] + v[1] * rd[4] + v[2] * rd[5];
	if (d <= 0.0f)
		return;
	float z = d / k, side = 1.0f;
	if (fx_zcap > 0.0f && z > fx_zcap && z > d) { // (QA: a depth limit, the sideways offset then scaled to keep the screen place)
		z = fx_zcap > d ? fx_zcap : d;
		side = fx_side_keep ? 1.0f : k * z / d;
	}
	const float at[3] = {origin[0], origin[1], origin[2]};
	for (int i = 0; i < 3; i++)
		origin[i] = rd[i] + rd[3 + i] * z + (v[i] - rd[3 + i] * d) * side;
	const LONG n = InterlockedIncrement(&flash_moved);
	if (shot_now)
		log_printf("weaponfov: shot frame %ld: a bone of the view model at %.2f %.2f %.2f, moved to %.2f %.2f %.2f; the eye %.2f %.2f %.2f, forward %.4f %.4f %.4f (k %.3f)",
			   (long)shots, at[0], at[1], at[2], origin[0], origin[1], origin[2], rd[0], rd[1], rd[2], rd[3], rd[4], rd[5], k);
	if (spawn && fabsf(k - flash_last_k) > 0.01f) { // (a line when the scale changes)
		flash_last_k = k;
		log_printf("weaponfov: an effect on the weapon moved from %.1f to %.1f units in front of the eye (the weapon's scale %.3f; %ld bone places moved so far)", d, z, k,
			   (long)n);
	}
}

static int is_view_model_pose(const void *pose)
{
	const uint8_t *vm = base + XR(REF_WF_VMPOSE);
	return pose == vm || pose == vm + WF_VMPOSE2;
}

// the effects system's call of the bone matrix when it bolts an effect (both builds) and, on 32 bits, in every frame's update too
static int fx_bone_spawn_hook(void *pose, void *dobj, int bone, float *axis, float *origin)
{
	const int r = bone_original(pose, dobj, bone, axis, origin);
	if (r && origin && is_view_model_pose(pose))
#ifdef _WIN64
		fx_bone_move(origin, 1);
#else
		fx_bone_move(origin, fx_spawning); // (the frame updates go through the same call)
#endif
	return r;
}

// the bolted effect's frame update (64 bits: a call of its own) and the laser's beam start
static int fx_bone_frame_hook(void *pose, void *dobj, int bone, float *axis, float *origin)
{
	const int r = bone_original(pose, dobj, bone, axis, origin);
	if (r && origin && is_view_model_pose(pose))
		fx_bone_move(origin, 0);
	return r;
}

// the render thread (or the game's backend thread): the weapon's projection was just made from the view's
static void post_step(uint8_t *src)
{
	if (!weapon_fov || *(volatile int *)(src + WF_FLAGS) != 2)
		return;
	// the split screen: the ADS fraction of the player whose view is being drawn (his context's copy of the cgame), not the live one: player 1 raising his
	// sights grew player 2's weapon too (QA spwf 2026-11-24, 64-bit and iw4x-sp)
	const int rp = native_split_active() ? native_render_player() : -1;
	const volatile float *ap = rp >= 0 ? (const volatile float *)native_ctx_addr(rp, (unsigned)(XR(REF_WF_CG) + WF_ADS_OFFSET)) : NULL;
	const float k = weapon_k(ap ? *ap : *(volatile float *)(base + XR(REF_WF_CG) + WF_ADS_OFFSET), rp == 1 ? 1 : 0);
	if (k == 0.0f)
		return; // 0: the game's way
	float *m = (float *)(src + WF_MATRIX);
	for (int r = 0; r < 4; r++)
		m[r * 4 + 0] *= k, m[r * 4 + 1] *= k;
	last_k = k;
	InterlockedIncrement(&scaled_count);
}

#ifdef _WIN64
// the stub: push rcx, rdx, r8-r11, rax; xmm0-5 to the stack; call [original]; keep its rax; rcx = the saved one; call post_step; everything back; ret
static uint8_t *make_stub(void)
{
	uint8_t c[192];
	size_t n = 0;
#define B(...)                                                                                                                                                         \
	do {                                                                                                                                                           \
		const uint8_t b_[] = {__VA_ARGS__};                                                                                                                    \
		memcpy(c + n, b_, sizeof(b_));                                                                                                                       \
		n += sizeof(b_);                                                                                                                                      \
	} while (0)
	B(0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x50); // push rcx, rdx, r8, r9, r10, r11, rax (entry rsp = 8 mod 16: now 0 mod 16)
	B(0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00);			    // sub rsp, 0x80 (0x20 home space, 0x60 for xmm0-5)
	B(0xf3, 0x0f, 0x7f, 0x44, 0x24, 0x20);				    // movdqu [rsp+0x20], xmm0
	B(0xf3, 0x0f, 0x7f, 0x4c, 0x24, 0x30);				    // ... xmm1
	B(0xf3, 0x0f, 0x7f, 0x54, 0x24, 0x40);				    // ... xmm2
	B(0xf3, 0x0f, 0x7f, 0x5c, 0x24, 0x50);				    // ... xmm3
	B(0xf3, 0x0f, 0x7f, 0x64, 0x24, 0x60);				    // ... xmm4
	B(0xf3, 0x0f, 0x7f, 0x6c, 0x24, 0x70);				    // ... xmm5
	B(0x48, 0xb8);								    // mov rax, &original
	const uint64_t orig_var = (uint64_t)(uintptr_t)&original;
	memcpy(c + n, &orig_var, 8), n += 8;
	B(0xff, 0x10);							    // call [rax] (rcx is still the game's)
	B(0x48, 0x89, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00);			    // mov [rsp+0x80], rax (the pushed rax's slot: the original's result goes back)
	B(0x48, 0x8b, 0x8c, 0x24, 0xb0, 0x00, 0x00, 0x00);			    // mov rcx, [rsp+0xb0] (the pushed rcx)
	B(0x48, 0xb8);								    // mov rax, post_step
	const uint64_t post = (uint64_t)(uintptr_t)post_step;
	memcpy(c + n, &post, 8), n += 8;
	B(0xff, 0xd0);							    // call rax
	B(0xf3, 0x0f, 0x6f, 0x44, 0x24, 0x20);				    // movdqu xmm0, [rsp+0x20]
	B(0xf3, 0x0f, 0x6f, 0x4c, 0x24, 0x30);
	B(0xf3, 0x0f, 0x6f, 0x54, 0x24, 0x40);
	B(0xf3, 0x0f, 0x6f, 0x5c, 0x24, 0x50);
	B(0xf3, 0x0f, 0x6f, 0x64, 0x24, 0x60);
	B(0xf3, 0x0f, 0x6f, 0x6c, 0x24, 0x70);
	B(0x48, 0x81, 0xc4, 0x80, 0x00, 0x00, 0x00);			    // add rsp, 0x80
	B(0x58, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59); // pop rax, r11, r10, r9, r8, rdx, rcx
	B(0xc3);								    // ret
#undef B
	return hook_code(c, n);
}
#else
// the stub: the original runs with the game's EAX; its EAX, ECX and EDX are kept for the caller; the x87 and SSE state is put aside (fxsave) and the
// x87 stack emptied for the C step (the callers are x87 code that may keep values on that stack across the call); post_step(src); all of it back; ret
static uint8_t *make_stub(void)
{
	uint8_t c[96];
	size_t n = 0;
#define B(...)                                                                                                                                                         \
	do {                                                                                                                                                           \
		const uint8_t b_[] = {__VA_ARGS__};                                                                                                                    \
		memcpy(c + n, b_, sizeof(b_));                                                                                                                       \
		n += sizeof(b_);                                                                                                                                      \
	} while (0)
	B(0x55);				 // push ebp
	B(0x89, 0xe5);				 // mov ebp, esp
	B(0x50);				 // push eax ([ebp-4]: the source state)
	B(0xff, 0x15);				 // call [&original]
	const uint32_t orig_var = (uint32_t)(uintptr_t)&original;
	memcpy(c + n, &orig_var, 4), n += 4;
	B(0x50, 0x51, 0x52);			 // push eax, ecx, edx (the original's, for the caller)
	B(0x81, 0xec, 0x10, 0x02, 0x00, 0x00);	 // sub esp, 0x210
	B(0x83, 0xe4, 0xf0);			 // and esp, -16
	B(0x0f, 0xae, 0x04, 0x24);		 // fxsave [esp]
	B(0xdb, 0xe3);				 // fninit
	B(0xff, 0x75, 0xfc);			 // push dword [ebp-4]
	B(0xb8);				 // mov eax, post_step
	const uint32_t post = (uint32_t)(uintptr_t)post_step;
	memcpy(c + n, &post, 4), n += 4;
	B(0xff, 0xd0);				 // call eax
	B(0x83, 0xc4, 0x04);			 // add esp, 4
	B(0x0f, 0xae, 0x0c, 0x24);		 // fxrstor [esp]
	B(0x8d, 0x65, 0xf0);			 // lea esp, [ebp-0x10]
	B(0x5a, 0x59, 0x58);			 // pop edx, ecx, eax
	B(0xc9);				 // leave
	B(0xc3);				 // ret
#undef B
	return hook_code(c, n);
}
#endif

// the zoom getter's answer, for the local client whose client state is live (the getter runs on the main thread inside that client's view)
#ifdef _WIN64
static void zoom_note(float z)
{
	zoom_last[native_live_ctx()] = z;
}
#else
static void zoom_note_bits(uint32_t bits) // (32 bits: the answer as its bits, copied as bits: no x87 use while the caller may hold values on that stack)
{
	memcpy((void *)&zoom_last[native_live_ctx()], &bits, 4);
}
#endif

// the zoom getter's stub: the original runs (through the trampoline), its answer goes to zoom_note and is returned; every register is the original's
#ifdef _WIN64
static uint8_t *make_zoom_stub(void)
{
	uint8_t c[192];
	size_t n = 0;
#define B(...)                                                                                                                                                         \
	do {                                                                                                                                                           \
		const uint8_t b_[] = {__VA_ARGS__};                                                                                                                    \
		memcpy(c + n, b_, sizeof(b_));                                                                                                                       \
		n += sizeof(b_);                                                                                                                                      \
	} while (0)
	B(0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x50); // push rcx, rdx, r8, r9, r10, r11, rax (entry rsp = 8 mod 16: now 0 mod 16)
	B(0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00);			    // sub rsp, 0x80 (0x20 home space, 0x60 for xmm0-5)
	B(0x48, 0xb8);								    // mov rax, &zoom_original
	const uint64_t orig_var = (uint64_t)(uintptr_t)&zoom_original;
	memcpy(c + n, &orig_var, 8), n += 8;
	B(0xff, 0x10);							    // call [rax] (rcx, rdx are the caller's)
	B(0xf3, 0x0f, 0x7f, 0x44, 0x24, 0x20);				    // movdqu [rsp+0x20], xmm0 (the answer)
	B(0xf3, 0x0f, 0x7f, 0x4c, 0x24, 0x30);				    // ... xmm1-5 as the original left them
	B(0xf3, 0x0f, 0x7f, 0x54, 0x24, 0x40);
	B(0xf3, 0x0f, 0x7f, 0x5c, 0x24, 0x50);
	B(0xf3, 0x0f, 0x7f, 0x64, 0x24, 0x60);
	B(0xf3, 0x0f, 0x7f, 0x6c, 0x24, 0x70);
	B(0x48, 0xb8);								    // mov rax, zoom_note (its float argument: xmm0)
	const uint64_t note = (uint64_t)(uintptr_t)zoom_note;
	memcpy(c + n, &note, 8), n += 8;
	B(0xff, 0xd0);							    // call rax
	B(0xf3, 0x0f, 0x6f, 0x44, 0x24, 0x20);				    // movdqu xmm0, [rsp+0x20]
	B(0xf3, 0x0f, 0x6f, 0x4c, 0x24, 0x30);
	B(0xf3, 0x0f, 0x6f, 0x54, 0x24, 0x40);
	B(0xf3, 0x0f, 0x6f, 0x5c, 0x24, 0x50);
	B(0xf3, 0x0f, 0x6f, 0x64, 0x24, 0x60);
	B(0xf3, 0x0f, 0x6f, 0x6c, 0x24, 0x70);
	B(0x48, 0x81, 0xc4, 0x80, 0x00, 0x00, 0x00);			    // add rsp, 0x80
	B(0x58, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59); // pop rax, r11, r10, r9, r8, rdx, rcx
	B(0xc3);								    // ret
#undef B
	return hook_code(c, n);
}
#else
static uint8_t *make_zoom_stub(void)
{
	uint8_t c[64];
	size_t n = 0;
#define B(...)                                                                                                                                                         \
	do {                                                                                                                                                           \
		const uint8_t b_[] = {__VA_ARGS__};                                                                                                                    \
		memcpy(c + n, b_, sizeof(b_));                                                                                                                       \
		n += sizeof(b_);                                                                                                                                      \
	} while (0)
	B(0xff, 0x74, 0x24, 0x08);	    // push dword [esp+8] (weaponDef)
	B(0xff, 0x74, 0x24, 0x08);	    // push dword [esp+8] (cg)
	B(0xff, 0x15);			    // call [&zoom_original]
	const uint32_t orig_var = (uint32_t)(uintptr_t)&zoom_original;
	memcpy(c + n, &orig_var, 4), n += 4;
	B(0x83, 0xc4, 0x08);		    // add esp, 8
	B(0x83, 0xec, 0x04);		    // sub esp, 4
	B(0xd9, 0x1c, 0x24);		    // fstp dword [esp] (the answer off the x87 stack while C runs)
	B(0x50, 0x51, 0x52);		    // push eax, ecx, edx (the original's, for the caller)
	B(0xff, 0x74, 0x24, 0x0c);	    // push dword [esp+12] (the answer's bits)
	B(0xb8);			    // mov eax, zoom_note_bits
	const uint32_t note = (uint32_t)(uintptr_t)zoom_note_bits;
	memcpy(c + n, &note, 4), n += 4;
	B(0xff, 0xd0);			    // call eax
	B(0x83, 0xc4, 0x04);		    // add esp, 4
	B(0x5a, 0x59, 0x58);		    // pop edx, ecx, eax
	B(0xd9, 0x04, 0x24);		    // fld dword [esp] (the answer back in st0)
	B(0x83, 0xc4, 0x04);		    // add esp, 4
	B(0xc3);			    // ret
#undef B
	return hook_code(c, n);
}
#endif

static void zoom_install(void)
{
	if (getenv("MW2COOP_WF_RAWADS")) {
		log_printf("weaponfov: the ADS blend follows the raw ADS fraction (MW2COOP_WF_RAWADS)");
		return;
	}
	uint8_t *stub;
#ifdef _WIN64
	if (!addr_resolved(REF_WF_ZOOM) || !(stub = make_zoom_stub()) ||
	    !hook_function(base + XR(REF_WF_ZOOM), zoom_head, sizeof(zoom_head), stub, &zoom_original)) {
#else
	// 32 bits: not the getter (VA 0x41d630 lies in 0x418000-0x428000, which the official exe hashes when a turret is used, Turret_Use VA 0x42c911 -> 0x4ca740, and
	// quits with 0x8000dead when it differs: the Barrett of Of Their Own Accord ended the game, QA barrettpad 2026-11-24) but the view field of view's call of it,
	// which is the one the blend follows; the getter's bytes are only checked
	uint8_t *site = base + XR(REF_WF_ZOOM_CALL);
	if (!addr_resolved(REF_WF_ZOOM) || !addr_resolved(REF_WF_ZOOM_CALL) || memcmp(base + XR(REF_WF_ZOOM), zoom_head, sizeof(zoom_head)) || site[0] != 0xe8 ||
	    site + 5 + *(const int32_t *)(site + 1) != base + XR(REF_WF_ZOOM) || !(stub = make_zoom_stub()) || !hook_call(site, stub, &zoom_original)) {
#endif
		log_printf("weaponfov: the game's ADS zoom fraction (rva 0x%x) was not found: the blend follows the raw ADS fraction", (unsigned)REF_WF_ZOOM);
		return;
	}
	zoom_hooked = 1;
	log_printf("weaponfov: the ADS blend follows the game's zoom fraction");
}

void weaponfov_install(void)
{
	base = (uint8_t *)GetModuleHandleA(NULL);
#ifdef _WIN64
	if (!addr_resolved(REF_WF_PROJECTION) || !addr_resolved(REF_WF_FOV_DVAR) || !addr_resolved(REF_WF_FOVSCALE_DVAR) || !addr_resolved(REF_WF_CG)) {
		log_printf("weaponfov: the weapon's projection was not found in this build: no cg_weaponFov");
		return;
	}
	static const uint8_t head[14] = {0x0f, 0x10, 0x81, 0x00, 0x14, 0x00, 0x00, 0x0f, 0x29, 0x81, 0x00, 0x01, 0x00, 0x00}; // movups xmm0,[rcx+0x1400]; movaps [rcx+0x100],xmm0
#else
	if (!addr_resolved(REF_WF_PROJECTION) || !addr_resolved(REF_WF_CG)) {
		log_printf("weaponfov: the weapon's projection was not found in this build: no cg_weaponFov");
		return;
	}
	static const uint8_t head[14] = {0x56, 0x57, 0x8d, 0x90, 0x00, 0x01, 0x00, 0x00, 0x8d, 0xb0, 0x90, 0x13, 0x00, 0x00}; // push esi; push edi; lea edx,[eax+0x100]; lea esi,[eax+0x1390]
#endif
	uint8_t *stub = make_stub();
	if (!stub || !hook_function(base + XR(REF_WF_PROJECTION), head, sizeof(head), stub, &original)) {
		log_printf("weaponfov: the weapon's projection (rva 0x%x) is not the expected code: no cg_weaponFov", (unsigned)REF_WF_PROJECTION);
		return;
	}
	log_printf("weaponfov: the weapon's projection goes through the mod (cg_weaponFov)");
	zoom_install();
	// the weapon's effects: the effects system's calls of the bone matrix, each checked to be that call
	int fx = 0;
	if (addr_resolved(REF_WF_BONE) && addr_resolved(REF_WF_FX_SPAWN) && addr_resolved(REF_WF_VMPOSE) && addr_resolved(REF_WF_REFDEF)) {
		uint8_t *bone_fn = base + XR(REF_WF_BONE), *site = base + XR(REF_WF_FX_SPAWN);
		if (site[0] == 0xe8 && site + 5 + *(const int32_t *)(site + 1) == bone_fn)
			fx = hook_call(site, (void *)fx_bone_spawn_hook, (void **)&bone_original);
#ifndef _WIN64
		if (fx && addr_resolved(REF_WF_FX_BOLT) && addr_resolved(REF_WF_FX_BONEORIENT)) { // (only the spawn's mark: the effects move without it)
			site = base + XR(REF_WF_FX_BOLT);
			if (site[0] == 0xe8 && site + 5 + *(const int32_t *)(site + 1) == base + XR(REF_WF_FX_BONEORIENT))
				hook_call(site, (void *)fx_bolt_hook, (void **)&boneorient_original);
		}
#endif
#ifdef _WIN64
		if (fx && addr_resolved(REF_WF_FX_UPDATE)) {
			void *again = NULL;
			site = base + XR(REF_WF_FX_UPDATE);
			if (!(site[0] == 0xe8 && site + 5 + *(const int32_t *)(site + 1) == bone_fn && hook_call(site, (void *)fx_bone_frame_hook, &again)))
				fx = 0; // (half of it would bolt a flash to one place and run it at another)
		} else
			fx = 0;
#endif
	}
	// the laser's beam starts at the weapon's tag too
	int laser = 0;
	if (fx && addr_resolved(REF_WF_LASER)) {
		void *again = NULL;
		uint8_t *site = base + XR(REF_WF_LASER);
		if (site[0] == 0xe8 && site + 5 + *(const int32_t *)(site + 1) == base + XR(REF_WF_BONE))
			laser = hook_call(site, (void *)fx_bone_frame_hook, &again);
	}
	if (fx && !laser)
		log_printf("weaponfov: the laser's bone call is not the expected code: the laser starts where the game puts it");
	log_printf("weaponfov: %s", fx ? "the weapon's effects (muzzle flash, smoke, spent cases, laser) follow cg_weaponFov"
				       : "the effects system's bone calls are not the expected code: the weapon's effects keep the view's FOV");
}

// the menu tick (main thread): cg_weaponFov once the game's dvars are there; a line in the log when the scale changes
void weaponfov_tick(void)
{
	if (!base || !original)
		return;
	if (!weapon_fov && game_find_dvar("com_maxfps") && addr_resolved(REF_WF_REGISTER_FLOAT)) { // archived in the game's config; 65 is the console-like default
		dvar_t *(*reg_float)(const char *, float, float, float, unsigned, const char *) = (void *)(base + XR(REF_WF_REGISTER_FLOAT));
		weapon_fov = reg_float("cg_weaponFov", 65.0f, 0.0f, 160.0f, 1,
				       "The weapon's own field of view (65: console-like default; 0: follow the view's field of view)");
		log_printf("weaponfov: cg_weaponFov registered (%g)", weapon_fov ? weapon_fov->current.value : -1.0f);
	}
#ifndef _WIN64
	if (!fov_dvar)
		fov_dvar = game_find_dvar("cg_fov");
	if (!fovscale_dvar)
		fovscale_dvar = game_find_dvar("cg_fovScale");
#endif
	static float told_k = 1.0f;
	if (fabsf(last_k - told_k) > 0.01f) {
		told_k = last_k;
		log_printf("weaponfov: the weapon's projection scaled by %.3f (%ld scaled so far)", last_k, (long)scaled_count);
	}
}
