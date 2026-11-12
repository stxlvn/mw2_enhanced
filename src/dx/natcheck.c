// QA, MW2COOP_NATCHECK=1 (64-bit retail exe only): runs the new back end's apply half (src/dx/natgfx.c) next to the game's own pass-apply functions and compares, at every indexed draw, what natgfx
// derived from the game's tables with what the game really set on its Direct3D 9 device (constant registers, shaders, declaration, bound images). The game still draws through Direct3D 9; natgfx
// only reads. A mismatch is a bug of natgfx (or a rule it does not know yet). docs/native-dx-plan.md, M3.
#include "natcheck.h"

#ifdef _WIN64
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../hook.h"
#include "../log.h"
#include "natgfx.h"

#define VT_SET_TEXTURE 65
#define VT_DRAW_INDEXED 82
#define VT_SET_DECL 87
#define VT_SET_VS 92
#define VT_SET_VS_F 94
#define VT_SET_PS 107
#define VT_SET_PS_F 109

static uint8_t *exe_base;
static ng_state ng;
static ng_ops ops;

// the Direct3D 9 device's state as the game left it
static float d9_vs[256][4], d9_ps[256][4];
static void *d9_vs_obj, *d9_ps_obj, *d9_decl, *d9_tex[16];
static int d9_tex_seen[16]; // a slot bound before the hooks went in is not known (the game does not bind an image it believes is bound)

static long draws, regs_compared, regs_bad, shader_bad, decl_bad, tex_compared, tex_bad, tex_unbound, warns;

static void (*pass_orig)(void *, unsigned);
static void (*decl_orig)(void *);
static void (*ps_args_orig)(void *, unsigned, const void *);
static void (*obj_orig)(void *, unsigned, const void *);
static void (*prim_orig)(void *);

static const float *derived_fn(void *input, unsigned index, unsigned first_row)
{
	typedef const float *(*fn_t)(void *, unsigned long long, char);
	return ((fn_t)(exe_base + 0x502f0))(input, index, (char)first_row);
}

static void *material_image(void *input, const uint8_t *entry)
{
	(void)input;
	void *image = *(void **)(entry + 8);
	if (entry[7] == 11 && image) // the water: the game binds the image its first field points to
		image = *(void **)((uint8_t *)image + 0x58);
	return image;
}

static void warn_fn(const char *what, unsigned arg)
{
	if (InterlockedIncrement(&warns) <= 10)
		log_printf("natcheck: natgfx warns: %s (%u)", what, arg);
}

static void pass_hook(void *ctx, unsigned pass)
{
	void *input = ((void **)ctx)[0], *state = ((void **)ctx)[1];
	ng_select_pass(&ng, &ops, input, state, pass);
	pass_orig(ctx, pass);
}
static void decl_hook(void *state)
{
	ng_apply_decl_shader(&ng, &ops, state);
	decl_orig(state);
}
static void ps_args_hook(void *ctx, unsigned count, const void *args)
{
	ng_apply_ps_args(&ng, &ops, ((void **)ctx)[0], ((void **)ctx)[1], args, count);
	ps_args_orig(ctx, count, args);
}
static void obj_hook(void *ctx, unsigned count, const void *args)
{
	ng_apply_object_args(&ng, &ops, ((void **)ctx)[0], ((void **)ctx)[1], args, count);
	obj_orig(ctx, count, args);
}
static void prim_hook(void *ctx)
{
	ng_apply_prim(&ng, &ops, ((void **)ctx)[0], ((void **)ctx)[1]);
	prim_orig(ctx);
}

// ---- the Direct3D 9 side
static HRESULT(STDMETHODCALLTYPE *set_vs_f_o)(void *, UINT, const float *, UINT);
static HRESULT(STDMETHODCALLTYPE *set_ps_f_o)(void *, UINT, const float *, UINT);
static HRESULT(STDMETHODCALLTYPE *set_vs_o)(void *, void *);
static HRESULT(STDMETHODCALLTYPE *set_ps_o)(void *, void *);
static HRESULT(STDMETHODCALLTYPE *set_decl_o)(void *, void *);
static HRESULT(STDMETHODCALLTYPE *set_texture_o)(void *, DWORD, void *);
static HRESULT(STDMETHODCALLTYPE *draw_o)(void *, int, INT, UINT, UINT, UINT, UINT);

static HRESULT STDMETHODCALLTYPE set_vs_f_hook(void *d, UINT r, const float *v, UINT n)
{
	if (r < 256 && v)
		memcpy(d9_vs[r], v, (size_t)(r + n > 256 ? 256 - r : n) * 16);
	return set_vs_f_o(d, r, v, n);
}
static HRESULT STDMETHODCALLTYPE set_ps_f_hook(void *d, UINT r, const float *v, UINT n)
{
	if (r < 256 && v)
		memcpy(d9_ps[r], v, (size_t)(r + n > 256 ? 256 - r : n) * 16);
	return set_ps_f_o(d, r, v, n);
}
static HRESULT STDMETHODCALLTYPE set_vs_hook(void *d, void *s)
{
	d9_vs_obj = s;
	return set_vs_o(d, s);
}
static HRESULT STDMETHODCALLTYPE set_ps_hook(void *d, void *s)
{
	d9_ps_obj = s;
	return set_ps_o(d, s);
}
static HRESULT STDMETHODCALLTYPE set_decl_hook(void *d, void *s)
{
	d9_decl = s;
	return set_decl_o(d, s);
}
static HRESULT STDMETHODCALLTYPE set_texture_hook(void *d, DWORD s, void *t)
{
	if (s < 16) {
		d9_tex[s] = t;
		d9_tex_seen[s] = 1;
	}
	return set_texture_o(d, s, t);
}

static void compare(void)
{
	draws++;
	for (int r = 0; r < 256; r++) {
		if (ng.vs_dirty[r >> 5] >> (r & 31) & 1) {
			regs_compared++;
			if (memcmp(ng.vs_c[r], d9_vs[r], 16) != 0 && InterlockedIncrement(&regs_bad) <= 20)
				log_printf("natcheck: MISMATCH vertex constant c%d: natgfx %g %g %g %g, device %g %g %g %g (draw %ld)", r, ng.vs_c[r][0], ng.vs_c[r][1], ng.vs_c[r][2], ng.vs_c[r][3], d9_vs[r][0], d9_vs[r][1], d9_vs[r][2], d9_vs[r][3], draws);
		}
		if (ng.ps_dirty[r >> 5] >> (r & 31) & 1) {
			regs_compared++;
			if (memcmp(ng.ps_c[r], d9_ps[r], 16) != 0 && InterlockedIncrement(&regs_bad) <= 20)
				log_printf("natcheck: MISMATCH pixel constant c%d: natgfx %g %g %g %g, device %g %g %g %g (draw %ld)", r, ng.ps_c[r][0], ng.ps_c[r][1], ng.ps_c[r][2], ng.ps_c[r][3], d9_ps[r][0], d9_ps[r][1], d9_ps[r][2], d9_ps[r][3], draws);
		}
	}
	memset(ng.vs_dirty, 0, sizeof(ng.vs_dirty));
	memset(ng.ps_dirty, 0, sizeof(ng.ps_dirty));
	if (ng.vs != d9_vs_obj || ng.ps != d9_ps_obj) {
		if (InterlockedIncrement(&shader_bad) <= 10)
			log_printf("natcheck: MISMATCH shaders: natgfx vs %p ps %p, device vs %p ps %p (draw %ld)", ng.vs, ng.ps, d9_vs_obj, d9_ps_obj, draws);
	}
	if (ng.decl != d9_decl && InterlockedIncrement(&decl_bad) <= 10)
		log_printf("natcheck: MISMATCH vertex declaration: natgfx %p, device %p (draw %ld)", ng.decl, d9_decl, draws);
	for (int i = 0; i < 16; i++) {
		if (!ng.tex[i] || !d9_tex_seen[i])
			continue;
		void *want = *(void **)ng.tex[i]; // the image's Direct3D texture (its first field)
		if (!d9_tex[i]) { // the game unbinds a texture that is the render target it switches to (0x50ec0): not part of the pass-apply functions
			tex_unbound++;
			continue;
		}
		tex_compared++;
		if (want != d9_tex[i] && InterlockedIncrement(&tex_bad) <= 10)
			log_printf("natcheck: MISMATCH sampler %d: natgfx image %p (texture %p), device texture %p (draw %ld)", i, ng.tex[i], want, d9_tex[i], draws);
	}
	if (draws % 500000 == 0)
		log_printf("natcheck: %ld draws, %ld constant registers compared (%ld differ), shader mismatches %ld, declaration %ld, samplers %ld compared (%ld differ, %ld left unbound by the game for a target switch), natgfx warnings %ld", draws, regs_compared, regs_bad, shader_bad, decl_bad, tex_compared, tex_bad, tex_unbound, warns);
}

static HRESULT STDMETHODCALLTYPE draw_hook(void *d, int prim, INT base, UINT minv, UINT numv, UINT start, UINT count)
{
	compare();
	return draw_o(d, prim, base, minv, numv, start, count);
}

static void patch(void **vt, int index, void *hook, void **orig)
{
	DWORD old;
	if (vt[index] == hook || !VirtualProtect(&vt[index], sizeof(void *), PAGE_READWRITE, &old))
		return;
	*orig = vt[index];
	vt[index] = hook;
	VirtualProtect(&vt[index], sizeof(void *), old, &old);
}

void natcheck_hook_device(struct IDirect3DDevice9 *dev)
{
	static int started;
	if (!getenv("MW2COOP_NATCHECK") || started || !dev)
		return;
	started = 1;
	exe_base = (uint8_t *)GetModuleHandleA(NULL);
	IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(exe_base + ((IMAGE_DOS_HEADER *)exe_base)->e_lfanew);
	if (nt->OptionalHeader.SizeOfImage < 0x4585400) {
		log_printf("natcheck: not the retail exe, off");
		return;
	}
	ng_init(&ng);
	ops.derived = derived_fn;
	ops.material_image = material_image;
	ops.warn = warn_fn;
	ops.filter_table = (const uint32_t *)(exe_base + 0x4585290);
	ops.write_game_state = 0;
	static const uint8_t h_pass[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20};
	static const uint8_t h_decl[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18, 0x41, 0x56, 0x48, 0x83, 0xec, 0x30};
	static const uint8_t h_ps[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xec, 0x20};
	static const uint8_t h_obj[] = {0x40, 0x53, 0x55, 0x57, 0x48, 0x83, 0xec, 0x50, 0x41, 0x0f, 0xb7, 0x00, 0x49, 0x8b, 0xd8, 0x8b, 0xea};
	static const uint8_t h_prim[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8b, 0x41, 0x08, 0x48, 0x8b, 0x98, 0x38, 0x01, 0x00, 0x00};
	int ok = 1;
	ok &= hook_function(exe_base + 0x54a10, h_pass, sizeof(h_pass), (void *)pass_hook, (void **)&pass_orig);
	ok &= hook_function(exe_base + 0x54940, h_decl, sizeof(h_decl), (void *)decl_hook, (void **)&decl_orig);
	ok &= hook_function(exe_base + 0x54070, h_ps, sizeof(h_ps), (void *)ps_args_hook, (void **)&ps_args_orig);
	ok &= hook_function(exe_base + 0x53d00, h_obj, sizeof(h_obj), (void *)obj_hook, (void **)&obj_orig);
	ok &= hook_function(exe_base + 0x53f10, h_prim, sizeof(h_prim), (void *)prim_hook, (void **)&prim_orig);
	if (!ok) {
		log_printf("natcheck: some pass-apply function does not have the expected first bytes: the comparison would be incomplete, off");
		return;
	}
	void **vt = *(void ***)dev;
	patch(vt, VT_SET_VS_F, set_vs_f_hook, (void **)&set_vs_f_o);
	patch(vt, VT_SET_PS_F, set_ps_f_hook, (void **)&set_ps_f_o);
	patch(vt, VT_SET_VS, set_vs_hook, (void **)&set_vs_o);
	patch(vt, VT_SET_PS, set_ps_hook, (void **)&set_ps_o);
	patch(vt, VT_SET_DECL, set_decl_hook, (void **)&set_decl_o);
	patch(vt, VT_SET_TEXTURE, set_texture_hook, (void **)&set_texture_o);
	patch(vt, VT_DRAW_INDEXED, draw_hook, (void **)&draw_o);
	log_printf("natcheck: natgfx runs next to the game's pass-apply functions; every indexed draw compares its constants, shaders, declaration and images with the Direct3D 9 device");
}
#else
void natcheck_hook_device(struct IDirect3DDevice9 *dev)
{
	(void)dev;
}
#endif
