// FSR 1 spatial upscaling for the game's Direct3D 9 picture ([display] upscale=1).
//
// The game renders at a lower size than the window (the size its own video settings say: the "internal" size); this file makes the device's back buffer the output size
// (upscale_adjust_params, called when the game creates the device) and, at each Present, upscales the top left internal part to the whole back buffer in two passes:
//   EASU (edge adaptive spatial upsampling) into a texture of the output size, then RCAS (robust contrast adaptive sharpening) into the back buffer.
// The two pixel shaders are ports of the algorithms in AMD's FidelityFX FSR 1 header (ffx_fsr1.h, FidelityFX SDK, MIT license, Copyright (C) Advanced Micro Devices, Inc.):
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without
//   restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
//   Software is furnished to do so, subject to the following conditions: The above copyright notice and this permission notice shall be included in all copies or substantial portions of
//   the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
//   PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
//   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// Changes for Direct3D 9 (shader model 3): the gather instruction is made of four point samples, integer approximations of reciprocals are exact divisions (kept finite), the half precision
// paths and the noise detection of RCAS (off in AMD's default) are left out. Everything else follows the original.
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "settings.h"
#include "upscale.h"

const char upscale_easu_hlsl[] =
	"sampler2D src : register(s0);\n"
	"float4 c0 : register(c0);\n" // viewport / output size, offsets
	"float4 c1 : register(c1);\n" // 1 / input size, first gather offsets
	"float4 c2 : register(c2);\n"
	"float4 c3 : register(c3);\n"
	"float3 T(float2 uv) { return tex2D(src, uv).rgb; }\n"
	"float L2(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }\n"
	"float R(float x) { return 1.0 / max(x, 1e-12); }\n"
	"void SetF(inout float2 dir, inout float len, float2 pp, bool biS, bool biT, bool biU, bool biV, float lA, float lB, float lC, float lD, float lE)\n"
	"{\n"
	"  float w = 0.0;\n"
	"  if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);\n"
	"  if (biT) w = pp.x * (1.0 - pp.y);\n"
	"  if (biU) w = (1.0 - pp.x) * pp.y;\n"
	"  if (biV) w = pp.x * pp.y;\n"
	"  float dc = lD - lC;\n"
	"  float cb = lC - lB;\n"
	"  float lengthX = max(abs(dc), abs(cb));\n"
	"  lengthX = R(lengthX);\n"
	"  float directionX = lD - lB;\n"
	"  dir.x += directionX * w;\n"
	"  lengthX = saturate(abs(directionX) * lengthX);\n"
	"  lengthX *= lengthX;\n"
	"  len += lengthX * w;\n"
	"  float ec = lE - lC;\n"
	"  float ca = lC - lA;\n"
	"  float lengthY = max(abs(ec), abs(ca));\n"
	"  lengthY = R(lengthY);\n"
	"  float directionY = lE - lA;\n"
	"  dir.y += directionY * w;\n"
	"  lengthY = saturate(abs(directionY) * lengthY);\n"
	"  lengthY *= lengthY;\n"
	"  len += lengthY * w;\n"
	"}\n"
	"void Tap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 col)\n"
	"{\n"
	"  float2 rot;\n"
	"  rot.x = off.x * dir.x + off.y * dir.y;\n"
	"  rot.y = off.x * (-dir.y) + off.y * dir.x;\n"
	"  rot *= len;\n"
	"  float d2 = rot.x * rot.x + rot.y * rot.y;\n"
	"  d2 = min(d2, clp);\n"
	"  float wB = 2.0 / 5.0 * d2 - 1.0;\n"
	"  float wA = lob * d2 - 1.0;\n"
	"  wB *= wB;\n"
	"  wA *= wA;\n"
	"  wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);\n"
	"  float w = wB * wA;\n"
	"  aC += col * w;\n"
	"  aW += w;\n"
	"}\n"
	"float4 main(float2 vpos : VPOS) : COLOR\n"
	"{\n"
	"  float2 ip = floor(vpos);\n"
	"  float2 pp = ip * c0.xy + c0.zw;\n"
	"  float2 fp = floor(pp);\n"
	"  pp -= fp;\n"
	"  float2 p0 = fp * c1.xy + c1.zw;\n"
	"  float2 p1 = p0 + c2.xy;\n"
	"  float2 p2 = p0 + c2.zw;\n"
	"  float2 p3 = p0 + c3.xy;\n"
	"  float2 t = c1.xy;\n"
	// the 12 taps (a gather is four point samples: bottom left, bottom right, top right, top left)
	"  float3 b = T(p0 + float2(-0.5, 0.5) * t);\n"
	"  float3 c = T(p0 + float2(0.5, 0.5) * t);\n"
	"  float3 i = T(p1 + float2(-0.5, 0.5) * t);\n"
	"  float3 j = T(p1 + float2(0.5, 0.5) * t);\n"
	"  float3 f = T(p1 + float2(0.5, -0.5) * t);\n"
	"  float3 e = T(p1 + float2(-0.5, -0.5) * t);\n"
	"  float3 k = T(p2 + float2(-0.5, 0.5) * t);\n"
	"  float3 l = T(p2 + float2(0.5, 0.5) * t);\n"
	"  float3 h = T(p2 + float2(0.5, -0.5) * t);\n"
	"  float3 g = T(p2 + float2(-0.5, -0.5) * t);\n"
	"  float3 o = T(p3 + float2(0.5, -0.5) * t);\n"
	"  float3 n = T(p3 + float2(-0.5, -0.5) * t);\n"
	"  float bL = L2(b), cL = L2(c), iL = L2(i), jL = L2(j), fL = L2(f), eL = L2(e), kL = L2(k), lL = L2(l), hL = L2(h), gL = L2(g), oL = L2(o), nL = L2(n);\n"
	"  float2 dir = float2(0.0, 0.0);\n"
	"  float len = 0.0;\n"
	"  SetF(dir, len, pp, true, false, false, false, bL, eL, fL, gL, jL);\n"
	"  SetF(dir, len, pp, false, true, false, false, cL, fL, gL, hL, kL);\n"
	"  SetF(dir, len, pp, false, false, true, false, fL, iL, jL, kL, nL);\n"
	"  SetF(dir, len, pp, false, false, false, true, gL, jL, kL, lL, oL);\n"
	"  float2 dir2 = dir * dir;\n"
	"  float dirR = dir2.x + dir2.y;\n"
	"  bool zro = dirR < (1.0 / 32768.0);\n"
	"  dirR = rsqrt(max(dirR, 1e-12));\n"
	"  dirR = zro ? 1.0 : dirR;\n"
	"  dir.x = zro ? 1.0 : dir.x;\n"
	"  dir *= dirR;\n"
	"  len = len * 0.5;\n"
	"  len *= len;\n"
	"  float stretch = (dir.x * dir.x + dir.y * dir.y) * R(max(abs(dir.x), abs(dir.y)));\n"
	"  float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 + (-0.5) * len);\n"
	"  float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;\n"
	"  float clp = 1.0 / lob;\n"
	"  float3 min4 = min(min(min(f, g), j), k);\n"
	"  float3 max4 = max(max(max(f, g), j), k);\n"
	"  float3 aC = float3(0.0, 0.0, 0.0);\n"
	"  float aW = 0.0;\n"
	"  Tap(aC, aW, float2(0.0, -1.0) - pp, dir, len2, lob, clp, b);\n"
	"  Tap(aC, aW, float2(1.0, -1.0) - pp, dir, len2, lob, clp, c);\n"
	"  Tap(aC, aW, float2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);\n"
	"  Tap(aC, aW, float2(0.0, 1.0) - pp, dir, len2, lob, clp, j);\n"
	"  Tap(aC, aW, float2(0.0, 0.0) - pp, dir, len2, lob, clp, f);\n"
	"  Tap(aC, aW, float2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);\n"
	"  Tap(aC, aW, float2(1.0, 1.0) - pp, dir, len2, lob, clp, k);\n"
	"  Tap(aC, aW, float2(2.0, 1.0) - pp, dir, len2, lob, clp, l);\n"
	"  Tap(aC, aW, float2(2.0, 0.0) - pp, dir, len2, lob, clp, h);\n"
	"  Tap(aC, aW, float2(1.0, 0.0) - pp, dir, len2, lob, clp, g);\n"
	"  Tap(aC, aW, float2(1.0, 2.0) - pp, dir, len2, lob, clp, o);\n"
	"  Tap(aC, aW, float2(0.0, 2.0) - pp, dir, len2, lob, clp, n);\n"
	"  float3 pix = min(max4, max(min4, aC * (1.0 / aW)));\n"
	"  return float4(pix, 1.0);\n"
	"}\n";

const char upscale_rcas_hlsl[] =
	"sampler2D src : register(s0);\n"
	"float4 k0 : register(c0);\n" // x: the sharpness (2 ^ -stops), yz: 1 / size
	"float3 Ld(float2 ip, float2 o) { return tex2D(src, (ip + o + 0.5) * k0.yz).rgb; }\n"
	"float4 main(float2 vpos : VPOS) : COLOR\n"
	"{\n"
	"  float2 ip = floor(vpos);\n"
	"  float3 b = Ld(ip, float2(0.0, -1.0));\n"
	"  float3 d = Ld(ip, float2(-1.0, 0.0));\n"
	"  float3 e = Ld(ip, float2(0.0, 0.0));\n"
	"  float3 f = Ld(ip, float2(1.0, 0.0));\n"
	"  float3 h = Ld(ip, float2(0.0, 1.0));\n"
	"  float bL = b.b * 0.5 + (b.r * 0.5 + b.g);\n"
	"  float dL = d.b * 0.5 + (d.r * 0.5 + d.g);\n"
	"  float eL = e.b * 0.5 + (e.r * 0.5 + e.g);\n"
	"  float fL = f.b * 0.5 + (f.r * 0.5 + f.g);\n"
	"  float hL = h.b * 0.5 + (h.r * 0.5 + h.g);\n"
	"  float3 mn4 = min(min(min(b, d), f), h);\n"
	"  float3 mx4 = max(max(max(b, d), f), h);\n"
	"  float lowerLimiter = saturate(eL / max(min(min(min(bL, dL), fL), hL), 1e-6));\n"
	"  float3 hitMin = mn4 * (1.0 / max(4.0 * mx4, 1e-6)) * lowerLimiter;\n"
	"  float3 hitMax = (1.0 - mx4) * (1.0 / min(4.0 * mn4 - 4.0, -1e-6));\n"
	"  float3 lobeV = max(-hitMin, hitMax);\n"
	"  float lobe = max(-(0.25 - (1.0 / 16.0)), min(max(lobeV.x, max(lobeV.y, lobeV.z)), 0.0)) * k0.x;\n"
	"  float rcpL = 1.0 / (4.0 * lobe + 1.0);\n"
	"  float3 pix = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;\n"
	"  return float4(pix, 1.0);\n"
	"}\n";

#define easu_hlsl upscale_easu_hlsl
#define rcas_hlsl upscale_rcas_hlsl

static const char vs_hlsl[] = "float4 main(float4 p : POSITION) : POSITION { return p; }\n";

typedef HRESULT(WINAPI *compile_t)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT, UINT, void **, void **);

static int out_w, out_h;      // the output size ([display] upscale_width / upscale_height)
static int in_w, in_h;        // the size the game renders at (what it asked the device for)

static IDirect3DDevice9 *res_dev;
static IDirect3DTexture9 *in_tex, *easu_tex, *rcas_tex;
static IDirect3DSurface9 *in_surf, *easu_surf, *rcas_surf;
static IDirect3DPixelShader9 *easu_ps, *rcas_ps;
static IDirect3DVertexShader9 *quad_vs;
static IDirect3DVertexDeclaration9 *quad_decl;
static IDirect3DStateBlock9 *state_sb; // the device state before the passes: the game caches its state and does not set it again
static D3DFORMAT res_format;
static int shaders_failed;

int upscale_enabled(void)
{
	// the sizing (the game renders small, the back buffer has the output size, the window and the cursor follow) is shared with FSR 3 (fsr3.c); the FSR 1 passes only run with upscale=1
	return (settings_upscale() || settings_fsr3()) && settings_upscale_width() > 0 && settings_upscale_height() > 0;
}

// the native renderers (nat11.c / nat12.c): the game renders in_w x in_h, the swap chain (and the window) is out_w x out_h; 0 when nothing is upscaled
int upscale_native_sizes(int game_w, int game_h, int *out_width, int *out_height)
{
	if (!upscale_enabled() || !(settings_upscale() || settings_fsr3()))
		return 0;
	const int w = settings_upscale_width(), h = settings_upscale_height();
	if (game_w == w && game_h == h) {
		log_printf("upscale: the game asked for the output size %dx%d itself: nothing to upscale (set the game's own resolution lower)", w, h);
		return 0;
	}
	out_w = w;
	out_h = h;
	in_w = game_w;
	in_h = game_h;
	*out_width = w;
	*out_height = h;
	log_printf("upscale: native renderer: the game renders %dx%d, the swap chain is %dx%d (FSR 1: EASU + RCAS, sharpness %d/100 stop)", in_w, in_h, w, h, settings_upscale_sharpness());
	return 1;
}

int upscale_adjust_params(D3DPRESENT_PARAMETERS *pp)
{
	if (!pp || !upscale_enabled())
		return 0;
	int w = settings_upscale_width(), h = settings_upscale_height();
	if ((int)pp->BackBufferWidth == w && (int)pp->BackBufferHeight == h) {
		log_printf("upscale: the game asked for the output size %dx%d itself: nothing to upscale (set the game's own resolution lower)", w, h);
		return 0;
	}
	out_w = w;
	out_h = h;
	in_w = (int)pp->BackBufferWidth;
	in_h = (int)pp->BackBufferHeight;
	pp->BackBufferWidth = (UINT)w;
	pp->BackBufferHeight = (UINT)h;
	log_printf("upscale: the game renders %dx%d, the back buffer is %dx%d (FSR 1: EASU + RCAS, sharpness %d/100 stop)", in_w, in_h, w, h, settings_upscale_sharpness());
	return 1;
}

// The game believes the window is the internal size: it reads the cursor in client coordinates (ScreenToClient) and recentres it for mouse look (ClientToScreen). Its own two imports
// are given the scaled positions (the mod's code, which has its own calls, is not touched), both ways, so the menu cursor lands where it is and the recentring stays consistent.
static HWND game_window;
typedef BOOL(WINAPI *cursor_fn_t)(HWND, POINT *);
static cursor_fn_t real_screen_to_client, real_client_to_screen;

static BOOL WINAPI screen_to_client_hook(HWND h, POINT *pt)
{
	BOOL ok = real_screen_to_client(h, pt);
	if (ok && pt && h == game_window && out_w > 0 && in_w > 0) {
		pt->x = (LONG)((long long)pt->x * in_w / out_w);
		pt->y = (LONG)((long long)pt->y * in_h / out_h);
	}
	return ok;
}

static BOOL WINAPI client_to_screen_hook(HWND h, POINT *pt)
{
	if (pt && h == game_window && out_w > 0 && in_w > 0) {
		POINT scaled = {(LONG)((long long)pt->x * out_w / in_w), (LONG)((long long)pt->y * out_h / in_h)};
		BOOL ok = real_client_to_screen(h, &scaled);
		if (ok)
			*pt = scaled;
		return ok;
	}
	return real_client_to_screen(h, pt);
}

static void **find_import_slot(HMODULE exe, const char *dll, const char *func)
{
	uint8_t *base = (uint8_t *)exe;
	IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return NULL;
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
	IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir->VirtualAddress)
		return NULL;
	for (IMAGE_IMPORT_DESCRIPTOR *id = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); id->Name; id++) {
		if (lstrcmpiA((const char *)(base + id->Name), dll))
			continue;
		IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (id->OriginalFirstThunk ? id->OriginalFirstThunk : id->FirstThunk));
		IMAGE_THUNK_DATA *thunk = (IMAGE_THUNK_DATA *)(base + id->FirstThunk);
		for (; names->u1.AddressOfData; names++, thunk++) {
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
				continue;
			IMAGE_IMPORT_BY_NAME *by_name = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
			if (!lstrcmpA((const char *)by_name->Name, func))
				return (void **)&thunk->u1.Function;
		}
	}
	return NULL;
}

static void hook_import(const char *func, void *hook, void **orig)
{
	void **slot = find_import_slot(GetModuleHandleA(NULL), "user32.dll", func);
	DWORD old;
	if (!slot || !VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
		log_printf("upscale: the game's import of %s was not found: the menu cursor is off by the scale", func);
		return;
	}
	*orig = *slot;
	*slot = hook;
	VirtualProtect(slot, sizeof(void *), old, &old);
	log_printf("upscale: the game's %s gets the cursor in its own (internal) coordinates", func);
}

// the window fits the output size
void upscale_device_made(HWND window)
{
	if (!window || out_w <= 0)
		return;
	game_window = window;
	if (!real_screen_to_client) {
		hook_import("ScreenToClient", (void *)screen_to_client_hook, (void **)&real_screen_to_client);
		hook_import("ClientToScreen", (void *)client_to_screen_hook, (void **)&real_client_to_screen);
	}
	RECT client, want = {0, 0, out_w, out_h};
	if (!GetClientRect(window, &client) || (client.right == out_w && client.bottom == out_h))
		return;
	AdjustWindowRectEx(&want, (DWORD)GetWindowLongPtrA(window, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrA(window, GWL_EXSTYLE));
	SetWindowPos(window, NULL, 0, 0, want.right - want.left, want.bottom - want.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
	log_printf("upscale: window client area %ldx%ld made %dx%d", client.right, client.bottom, out_w, out_h);
}

int upscale_internal_size(int *w, int *h)
{
	if (out_w <= 0 || in_w <= 0)
		return 0;
	*w = in_w;
	*h = in_h;
	return 1;
}

static void release_resources(void)
{
	if (in_surf) { IDirect3DSurface9_Release(in_surf); in_surf = NULL; }
	if (easu_surf) { IDirect3DSurface9_Release(easu_surf); easu_surf = NULL; }
	if (rcas_surf) { IDirect3DSurface9_Release(rcas_surf); rcas_surf = NULL; }
	if (rcas_tex) { IDirect3DTexture9_Release(rcas_tex); rcas_tex = NULL; }
	if (in_tex) { IDirect3DTexture9_Release(in_tex); in_tex = NULL; }
	if (easu_tex) { IDirect3DTexture9_Release(easu_tex); easu_tex = NULL; }
	if (easu_ps) { IDirect3DPixelShader9_Release(easu_ps); easu_ps = NULL; }
	if (rcas_ps) { IDirect3DPixelShader9_Release(rcas_ps); rcas_ps = NULL; }
	if (quad_vs) { IDirect3DVertexShader9_Release(quad_vs); quad_vs = NULL; }
	if (quad_decl) { IDirect3DVertexDeclaration9_Release(quad_decl); quad_decl = NULL; }
	if (state_sb) { IDirect3DStateBlock9_Release(state_sb); state_sb = NULL; }
	res_dev = NULL;
}

// The device's Reset needs every default pool resource released: hooked once, our resources go before it
typedef HRESULT(STDMETHODCALLTYPE *reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
typedef HRESULT(STDMETHODCALLTYPE *reset_ex_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *, D3DDISPLAYMODEEX *);
static reset_t reset_orig;
static reset_ex_t reset_ex_orig;

static HRESULT STDMETHODCALLTYPE reset_hook(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
	release_resources();
	if (pp && out_w > 0 && in_w > 0 && (int)pp->BackBufferWidth == in_w && (int)pp->BackBufferHeight == in_h) {
		pp->BackBufferWidth = (UINT)out_w;
		pp->BackBufferHeight = (UINT)out_h;
	}
	return reset_orig(dev, pp);
}

static HRESULT STDMETHODCALLTYPE reset_ex_hook(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp, D3DDISPLAYMODEEX *mode)
{
	release_resources();
	if (pp && out_w > 0 && in_w > 0 && (int)pp->BackBufferWidth == in_w && (int)pp->BackBufferHeight == in_h) {
		pp->BackBufferWidth = (UINT)out_w;
		pp->BackBufferHeight = (UINT)out_h;
	}
	return reset_ex_orig(dev, pp, mode);
}

static void hook_reset(IDirect3DDevice9 *dev)
{
	static int done;
	if (done)
		return;
	done = 1;
	void **vt = *(void ***)dev;
	DWORD old;
	if (VirtualProtect(&vt[16], sizeof(void *), PAGE_READWRITE, &old)) {
		reset_orig = (reset_t)vt[16];
		vt[16] = (void *)reset_hook;
		VirtualProtect(&vt[16], sizeof(void *), old, &old);
	}
	if (VirtualProtect(&vt[132], sizeof(void *), PAGE_READWRITE, &old)) {
		reset_ex_orig = (reset_ex_t)vt[132];
		if (reset_ex_orig) {
			vt[132] = (void *)reset_ex_hook;
		}
		VirtualProtect(&vt[132], sizeof(void *), old, &old);
	}
}

static void *compile_shader(compile_t compile, const char *src, const char *target, int is_vs, IDirect3DDevice9 *dev)
{
	void *code = NULL, *err = NULL;
	HRESULT hr = compile(src, strlen(src), NULL, NULL, NULL, "main", target, 0, 0, &code, &err);
	if (FAILED(hr) || !code) {
		const char *msg = err ? (const char *)((void *(*)(void *))(*(void ***)err)[3])(err) : "";
		log_printf("upscale: shader %s did not compile (hr %lx): %.300s", target, (unsigned long)hr, msg);
		if (err)
			((ULONG(*)(void *))(*(void ***)err)[2])(err);
		return NULL;
	}
	const DWORD *bytes = (const DWORD *)((void *(*)(void *))(*(void ***)code)[3])(code);
	void *shader = NULL;
	hr = is_vs ? IDirect3DDevice9_CreateVertexShader(dev, bytes, (IDirect3DVertexShader9 **)&shader) : IDirect3DDevice9_CreatePixelShader(dev, bytes, (IDirect3DPixelShader9 **)&shader);
	((ULONG(*)(void *))(*(void ***)code)[2])(code);
	if (FAILED(hr))
		log_printf("upscale: shader %s could not be made (hr %lx)", target, (unsigned long)hr);
	return SUCCEEDED(hr) ? shader : NULL;
}

static int make_resources(IDirect3DDevice9 *dev, D3DFORMAT fmt)
{
	if (res_dev == dev && res_format == fmt && easu_ps && rcas_ps && in_tex && easu_tex && rcas_tex)
		return 1;
	release_resources();
	if (shaders_failed)
		return 0;
	HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
	compile_t compile = m ? (compile_t)(void *)GetProcAddress(m, "D3DCompile") : NULL;
	if (!compile) {
		log_printf("upscale: no d3dcompiler_47.dll: no upscaling");
		shaders_failed = 1;
		return 0;
	}
	easu_ps = (IDirect3DPixelShader9 *)compile_shader(compile, easu_hlsl, "ps_3_0", 0, dev);
	rcas_ps = (IDirect3DPixelShader9 *)compile_shader(compile, rcas_hlsl, "ps_3_0", 0, dev);
	quad_vs = (IDirect3DVertexShader9 *)compile_shader(compile, vs_hlsl, "vs_3_0", 1, dev);
	D3DVERTEXELEMENT9 decl[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
	IDirect3DDevice9_CreateVertexDeclaration(dev, decl, &quad_decl);
	if (!easu_ps || !rcas_ps || !quad_vs || !quad_decl) {
		shaders_failed = 1;
		release_resources();
		return 0;
	}
	if (FAILED(IDirect3DDevice9_CreateTexture(dev, (UINT)in_w, (UINT)in_h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &in_tex, NULL)) ||
	    FAILED(IDirect3DDevice9_CreateTexture(dev, (UINT)out_w, (UINT)out_h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &easu_tex, NULL)) ||
	    FAILED(IDirect3DDevice9_CreateTexture(dev, (UINT)out_w, (UINT)out_h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &rcas_tex, NULL))) {
		log_printf("upscale: the render target textures could not be made");
		release_resources();
		return 0;
	}
	IDirect3DTexture9_GetSurfaceLevel(in_tex, 0, &in_surf);
	IDirect3DTexture9_GetSurfaceLevel(easu_tex, 0, &easu_surf);
	IDirect3DTexture9_GetSurfaceLevel(rcas_tex, 0, &rcas_surf);
	res_dev = dev;
	res_format = fmt;
	hook_reset(dev);
	log_printf("upscale: shaders and textures ready (%dx%d -> %dx%d)", in_w, in_h, out_w, out_h);
	return 1;
}

static void draw_quad(IDirect3DDevice9 *dev, int w, int h)
{
	static const float quad[4][4] = {{-1, 1, 0, 1}, {1, 1, 0, 1}, {-1, -1, 0, 1}, {1, -1, 0, 1}};
	D3DVIEWPORT9 vp = {0, 0, (DWORD)w, (DWORD)h, 0.0f, 1.0f};
	IDirect3DDevice9_SetViewport(dev, &vp);
	IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
}

static void present_body(IDirect3DDevice9 *dev)
{
	IDirect3DSurface9 *bb = NULL;
	D3DSURFACE_DESC sd;
	if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
		return;
	if (FAILED(IDirect3DSurface9_GetDesc(bb, &sd)) || (int)sd.Width != out_w || (int)sd.Height != out_h || !make_resources(dev, sd.Format)) {
		IDirect3DSurface9_Release(bb);
		return;
	}
	RECT src = {0, 0, in_w, in_h};
	if (FAILED(IDirect3DDevice9_StretchRect(dev, bb, &src, in_surf, NULL, D3DTEXF_NONE))) {
		static int told;
		if (!told++)
			log_printf("upscale: the back buffer could not be copied");
		IDirect3DSurface9_Release(bb);
		return;
	}
	// the game's depth surface is smaller than the back buffer (the internal size): none while the passes draw
	IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);
	if (SUCCEEDED(IDirect3DDevice9_BeginScene(dev))) {
		IDirect3DDevice9_SetVertexShader(dev, quad_vs);
		IDirect3DDevice9_SetVertexDeclaration(dev, quad_decl);
		IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
		IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
		IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
		IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
		IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
		IDirect3DDevice9_SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
		IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
		IDirect3DDevice9_SetRenderState(dev, D3DRS_FILLMODE, D3DFILL_SOLID);
		IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xf);
		IDirect3DDevice9_SetRenderState(dev, D3DRS_SRGBWRITEENABLE, FALSE);
		IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
		IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_SRGBTEXTURE, 0);

		// pass 1: EASU, the internal picture to a texture of the output size
		const float fin_w = (float)in_w, fin_h = (float)in_h, fout_w = (float)out_w, fout_h = (float)out_h;
		const float con[4][4] = {
			{fin_w / fout_w, fin_h / fout_h, 0.5f * fin_w / fout_w - 0.5f, 0.5f * fin_h / fout_h - 0.5f},
			{1.0f / fin_w, 1.0f / fin_h, 1.0f / fin_w, -1.0f / fin_h},
			{-1.0f / fin_w, 2.0f / fin_h, 1.0f / fin_w, 2.0f / fin_h},
			{0.0f, 4.0f / fin_h, 0.0f, 0.0f},
		};
		IDirect3DDevice9_SetRenderTarget(dev, 0, easu_surf);
		IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)in_tex);
		IDirect3DDevice9_SetPixelShader(dev, easu_ps);
		IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, &con[0][0], 4);
		draw_quad(dev, out_w, out_h);

		// pass 2: RCAS, the sharpening, into a second texture; the back buffer (multisampled when the game's antialiasing is on) gets it by a copy
		float sharp = 1.0f;
		int stops100 = settings_upscale_sharpness();
		for (int i = 0; i < stops100; i++)
			sharp *= 0.99309249543703590153f; // 2 ^ (-1/100)
		const float k0[4] = {sharp, 1.0f / fout_w, 1.0f / fout_h, 0.0f};
		IDirect3DDevice9_SetRenderTarget(dev, 0, rcas_surf);
		IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)easu_tex);
		IDirect3DDevice9_SetPixelShader(dev, rcas_ps);
		IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, k0, 1);
		draw_quad(dev, out_w, out_h);
		IDirect3DDevice9_SetTexture(dev, 0, NULL);
		IDirect3DDevice9_EndScene(dev);
		IDirect3DDevice9_StretchRect(dev, rcas_surf, NULL, bb, NULL, D3DTEXF_POINT);
		static int logged;
		if (!logged++)
			log_printf("upscale: first frame upscaled (%dx%d -> %dx%d)", in_w, in_h, out_w, out_h);
	}
	IDirect3DSurface9_Release(bb);
}

// The game keeps its own copy of the device state (render target, viewport, shaders, render states, textures) and sets only what differs from it: the state the passes leave behind
// made its next frames render wrong (black). The whole state, the render target and the depth surface are put back after the passes.
void upscale_present(IDirect3DDevice9 *dev)
{
	if (!dev || !settings_upscale() || out_w <= 0 || in_w <= 0)
		return;
	if (!state_sb && FAILED(IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &state_sb)))
		return;
	IDirect3DSurface9 *old_rt = NULL, *old_ds = NULL;
	IDirect3DStateBlock9_Capture(state_sb);
	IDirect3DDevice9_GetRenderTarget(dev, 0, &old_rt);
	IDirect3DDevice9_GetDepthStencilSurface(dev, &old_ds);
	present_body(dev);
	if (state_sb)
		IDirect3DStateBlock9_Apply(state_sb);
	if (old_rt) {
		IDirect3DDevice9_SetRenderTarget(dev, 0, old_rt);
		IDirect3DSurface9_Release(old_rt);
	}
	IDirect3DDevice9_SetDepthStencilSurface(dev, old_ds);
	if (old_ds)
		IDirect3DSurface9_Release(old_ds);
}
