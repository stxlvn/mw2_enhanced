// QA mirror: MW2COOP_D3D11MIRROR=1 (64-bit retail exe only). docs/native-dx-plan.md, milestone M2.
// The game keeps drawing through Direct3D 9 as always. This module shadows the state of its Direct3D 9 device (shaders, constants, textures, buffers, render states, targets) and replays every draw on
// a second, Direct3D 11 device into an off-screen target. At Present the two pictures are compared and written to disk. Nothing here changes what the game shows. The pieces (shader translation,
// state objects, formats, constants) are the ones the real Direct3D 11 back end needs; only the data source (the Direct3D 9 call stream instead of the game's own pass tables) is scaffolding.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mirror11.h"

#ifdef _WIN64
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include "../log.h"
#include "sm3.h"

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ small pointer map
typedef struct {
	void **keys;
	void **vals;
	size_t cap, used;
} pmap_t;

static size_t phash(const void *p)
{
	uint64_t x = (uint64_t)(uintptr_t)p;
	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdull;
	x ^= x >> 33;
	return (size_t)x;
}
static void *pmap_get(const pmap_t *m, const void *k)
{
	if (!m->cap)
		return NULL;
	for (size_t i = phash(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
		if (!m->keys[i])
			return NULL;
		if (m->keys[i] == k)
			return m->vals[i];
	}
}
static void pmap_put(pmap_t *m, void *k, void *v);
static void pmap_grow(pmap_t *m)
{
	pmap_t n = {0};
	n.cap = m->cap ? m->cap * 2 : 1024;
	n.keys = calloc(n.cap, sizeof(void *));
	n.vals = calloc(n.cap, sizeof(void *));
	for (size_t i = 0; i < m->cap; i++)
		if (m->keys[i])
			pmap_put(&n, m->keys[i], m->vals[i]);
	free(m->keys);
	free(m->vals);
	*m = n;
}
static void pmap_put(pmap_t *m, void *k, void *v)
{
	if (!m->cap || (m->used + 1) * 2 > m->cap)
		pmap_grow(m);
	for (size_t i = phash(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
		if (!m->keys[i]) {
			m->keys[i] = k;
			m->vals[i] = v;
			m->used++;
			return;
		}
		if (m->keys[i] == k) {
			m->vals[i] = v;
			return;
		}
	}
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ entities
typedef struct {
	int is_vs;
	uint32_t *code;
	size_t dwords;
	void *obj11; // ID3D11VertexShader / ID3D11PixelShader
	ID3DBlob *blob;
	int tried, failed;
} shader_t;

typedef struct {
	int kind; // 1 2D, 2 cube, 3 volume
	UINT w, h, d, levels;
	DWORD usage;
	D3DFORMAT fmt;
	int rt, depth;
	DXGI_FORMAT dxgi, dxgi_srgb, srv_fmt;
	int expand; // L8 / A8L8 data is widened to 8888 on upload
	ID3D11Texture2D *tex;
	ID3D11ShaderResourceView *srv, *srv_srgb;
	ID3D11RenderTargetView *rtv, *rtv_srgb;
	ID3D11DepthStencilView *dsv;
	uint8_t **shadow; // per level (2D only)
	UINT *pitch;
	int dirty;
	int failed;
} tex_t;

typedef struct {
	tex_t *tex;
	UINT level;
	int backbuffer;
} surf_t;

typedef struct {
	UINT length;
	int is_ib, is32;
	uint8_t *shadow;
	ID3D11Buffer *buf;
	int dirty;
	uint8_t *lock_ptr;
	UINT lock_off, lock_size;
} buf_t;

typedef struct {
	IDirect3DVertexDeclaration9 *decl;
	D3DVERTEXELEMENT9 el[32];
	int n;
} decl_t;

static pmap_t shaders, textures, surfaces, buffers, decls;

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ globals
static ID3D11Device *dev11;
static ID3D11DeviceContext *ctx11;
static HMODULE h_d3dcompiler;
static HRESULT (WINAPI *p_compile)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT, UINT, ID3DBlob **, ID3DBlob **);
static IDirect3DDevice9 *dev9;
static UINT bb_w, bb_h;
static D3DFORMAT bb_fmt;
static tex_t main_target;
static IDirect3DSurface9 *bb_surface;
static int frames;
static char out_dir[MAX_PATH];

// shadow of the Direct3D 9 device state
static struct {
	IDirect3DVertexShader9 *vs;
	IDirect3DPixelShader9 *ps;
	IDirect3DVertexDeclaration9 *decl;
	float vs_f[256][4], ps_f[256][4];
	int vs_i[16][4], ps_i[16][4];
	int vs_b[16], ps_b[16];
	IDirect3DBaseTexture9 *tex[16];
	DWORD smp[16][16];
	DWORD rs[256];
	struct { IDirect3DVertexBuffer9 *vb; UINT off, stride; } stream[4];
	IDirect3DIndexBuffer9 *ib;
	IDirect3DSurface9 *rt0, *ds;
	D3DVIEWPORT9 vp;
	RECT scissor;
} st;

static struct {
	long draws, draws_skipped, clears, presents;
	long skip_no_vs, skip_no_ps, skip_layout, skip_shader, skip_prim, skip_target, tex_unsupported, buf_unknown, layout_fail, shader_fail;
} stat;

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ original methods (shared vtable of the game's device and resources)
#define ORIG(name, ret, args) static ret (STDMETHODCALLTYPE *name##_o) args
ORIG(present, HRESULT, (IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *));
ORIG(create_vs, HRESULT, (IDirect3DDevice9 *, const DWORD *, IDirect3DVertexShader9 **));
ORIG(create_ps, HRESULT, (IDirect3DDevice9 *, const DWORD *, IDirect3DPixelShader9 **));
ORIG(create_texture, HRESULT, (IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *));
ORIG(create_cube, HRESULT, (IDirect3DDevice9 *, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9 **, HANDLE *));
ORIG(create_vb, HRESULT, (IDirect3DDevice9 *, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9 **, HANDLE *));
ORIG(create_ib, HRESULT, (IDirect3DDevice9 *, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9 **, HANDLE *));
ORIG(create_depth, HRESULT, (IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **, HANDLE *));
ORIG(create_decl, HRESULT, (IDirect3DDevice9 *, const D3DVERTEXELEMENT9 *, IDirect3DVertexDeclaration9 **));
ORIG(stretch_rect, HRESULT, (IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE));
ORIG(set_rt, HRESULT, (IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *));
ORIG(set_ds, HRESULT, (IDirect3DDevice9 *, IDirect3DSurface9 *));
ORIG(clear, HRESULT, (IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD));
ORIG(set_viewport, HRESULT, (IDirect3DDevice9 *, const D3DVIEWPORT9 *));
ORIG(set_rs, HRESULT, (IDirect3DDevice9 *, D3DRENDERSTATETYPE, DWORD));
ORIG(set_texture, HRESULT, (IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *));
ORIG(set_ss, HRESULT, (IDirect3DDevice9 *, DWORD, D3DSAMPLERSTATETYPE, DWORD));
ORIG(set_scissor, HRESULT, (IDirect3DDevice9 *, const RECT *));
ORIG(draw_prim, HRESULT, (IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT));
ORIG(draw_indexed, HRESULT, (IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT));
ORIG(draw_prim_up, HRESULT, (IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT));
ORIG(draw_indexed_up, HRESULT, (IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT));
ORIG(set_decl, HRESULT, (IDirect3DDevice9 *, IDirect3DVertexDeclaration9 *));
ORIG(set_vs, HRESULT, (IDirect3DDevice9 *, IDirect3DVertexShader9 *));
ORIG(set_ps, HRESULT, (IDirect3DDevice9 *, IDirect3DPixelShader9 *));
ORIG(set_vs_f, HRESULT, (IDirect3DDevice9 *, UINT, const float *, UINT));
ORIG(set_ps_f, HRESULT, (IDirect3DDevice9 *, UINT, const float *, UINT));
ORIG(set_vs_i, HRESULT, (IDirect3DDevice9 *, UINT, const int *, UINT));
ORIG(set_ps_i, HRESULT, (IDirect3DDevice9 *, UINT, const int *, UINT));
ORIG(set_vs_b, HRESULT, (IDirect3DDevice9 *, UINT, const BOOL *, UINT));
ORIG(set_ps_b, HRESULT, (IDirect3DDevice9 *, UINT, const BOOL *, UINT));
ORIG(set_stream, HRESULT, (IDirect3DDevice9 *, UINT, IDirect3DVertexBuffer9 *, UINT, UINT));
ORIG(set_indices, HRESULT, (IDirect3DDevice9 *, IDirect3DIndexBuffer9 *));
ORIG(get_backbuffer, HRESULT, (IDirect3DDevice9 *, UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **));
ORIG(get_surface_level, HRESULT, (IDirect3DTexture9 *, UINT, IDirect3DSurface9 **));
ORIG(lock_rect, HRESULT, (IDirect3DTexture9 *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD));
ORIG(unlock_rect, HRESULT, (IDirect3DTexture9 *, UINT));
ORIG(vb_lock, HRESULT, (IDirect3DVertexBuffer9 *, UINT, UINT, void **, DWORD));
ORIG(vb_unlock, HRESULT, (IDirect3DVertexBuffer9 *));
ORIG(ib_lock, HRESULT, (IDirect3DIndexBuffer9 *, UINT, UINT, void **, DWORD));
ORIG(ib_unlock, HRESULT, (IDirect3DIndexBuffer9 *));

static void *hooked_vt_tex, *hooked_vt_vb, *hooked_vt_ib;

static void patch(void **vt, int index, void *hook, void **orig)
{
	DWORD old;
	if (vt[index] == hook || !VirtualProtect(&vt[index], sizeof(void *), PAGE_READWRITE, &old))
		return;
	*orig = vt[index];
	vt[index] = hook;
	VirtualProtect(&vt[index], sizeof(void *), old, &old);
}
#define PATCH(vt, idx, name) patch((vt), (idx), (void *)name##_hook, (void **)&name##_o)

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ formats
static int fmt_info(D3DFORMAT f, DXGI_FORMAT *dxgi, DXGI_FORMAT *srgb, DXGI_FORMAT *srv, int *expand, int *block, int *bpp)
{
	*srgb = DXGI_FORMAT_UNKNOWN;
	*expand = 0;
	*block = 0;
	switch ((unsigned)f) {
	case 21: *dxgi = DXGI_FORMAT_B8G8R8A8_TYPELESS; *srv = DXGI_FORMAT_B8G8R8A8_UNORM; *srgb = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; *bpp = 4; return 1;
	case 22: *dxgi = DXGI_FORMAT_B8G8R8X8_TYPELESS; *srv = DXGI_FORMAT_B8G8R8X8_UNORM; *srgb = DXGI_FORMAT_B8G8R8X8_UNORM_SRGB; *bpp = 4; return 1;
	case 0x31545844: *dxgi = DXGI_FORMAT_BC1_TYPELESS; *srv = DXGI_FORMAT_BC1_UNORM; *srgb = DXGI_FORMAT_BC1_UNORM_SRGB; *block = 8; *bpp = 0; return 1;
	case 0x33545844: *dxgi = DXGI_FORMAT_BC2_TYPELESS; *srv = DXGI_FORMAT_BC2_UNORM; *srgb = DXGI_FORMAT_BC2_UNORM_SRGB; *block = 16; *bpp = 0; return 1;
	case 0x35545844: *dxgi = DXGI_FORMAT_BC3_TYPELESS; *srv = DXGI_FORMAT_BC3_UNORM; *srgb = DXGI_FORMAT_BC3_UNORM_SRGB; *block = 16; *bpp = 0; return 1;
	case 28: *dxgi = *srv = DXGI_FORMAT_A8_UNORM; *bpp = 1; return 1;
	case 50: *dxgi = DXGI_FORMAT_B8G8R8A8_TYPELESS; *srv = DXGI_FORMAT_B8G8R8A8_UNORM; *expand = 1; *bpp = 1; return 1; // L8
	case 51: *dxgi = DXGI_FORMAT_B8G8R8A8_TYPELESS; *srv = DXGI_FORMAT_B8G8R8A8_UNORM; *expand = 2; *bpp = 2; return 1; // A8L8
	case 113: *dxgi = *srv = DXGI_FORMAT_R16G16B16A16_FLOAT; *bpp = 8; return 1;
	case 116: *dxgi = *srv = DXGI_FORMAT_R32G32B32A32_FLOAT; *bpp = 16; return 1;
	case 111: *dxgi = *srv = DXGI_FORMAT_R16_FLOAT; *bpp = 2; return 1;
	case 112: *dxgi = *srv = DXGI_FORMAT_R16G16_FLOAT; *bpp = 4; return 1;
	case 114: *dxgi = *srv = DXGI_FORMAT_R32_FLOAT; *bpp = 4; return 1;
	case 115: *dxgi = *srv = DXGI_FORMAT_R32G32_FLOAT; *bpp = 8; return 1;
	case 34: *dxgi = *srv = DXGI_FORMAT_R16G16_UNORM; *bpp = 4; return 1;
	case 36: *dxgi = *srv = DXGI_FORMAT_R16G16B16A16_UNORM; *bpp = 8; return 1;
	case 31: *dxgi = *srv = DXGI_FORMAT_R10G10B10A2_UNORM; *bpp = 4; return 1;
	case 0x5a544e49: case 75: case 77: *dxgi = DXGI_FORMAT_R24G8_TYPELESS; *srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; *bpp = 4; return 1; // INTZ, D24S8, D24X8
	case 80: case 70: *dxgi = DXGI_FORMAT_R16_TYPELESS; *srv = DXGI_FORMAT_R16_UNORM; *bpp = 2; return 1;
	}
	return 0;
}

#include "mirror11_b.inc"
#include "mirror11_c.inc"
#else
void mirror11_hook_device(struct IDirect3DDevice9 *device)
{
	(void)device;
}
#endif
