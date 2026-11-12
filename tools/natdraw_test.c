// Test of the flush half (src/dx/natdraw11.c) on a real Direct3D 11 device (src/dx/nat11.c): shader bytecode of the game's own kind (SM3), a vertex declaration, the shadow state of natgfx and the
// render-state words in, pixels out. Draws a textured quad, an alpha-blended quad, and a draw whose input texture is the current render target. Windows only.
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>

#include "../src/dx/natdraw11.h"
#include "../src/dx/state11.h"

static int fails;
#define CHECK(c, ...) \
	do { \
		if (!(c)) { \
			fails++; \
			printf("FAIL line %d: ", __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} while (0)

static uint32_t read_pixel(nat_device *dev, ID3D11Resource *tex, unsigned x, unsigned y, unsigned w, unsigned h)
{
	ID3D11Device *d3d = nat_d3d_device(dev);
	ID3D11DeviceContext *ctx = nat_d3d_context(dev);
	D3D11_TEXTURE2D_DESC td = {0};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_STAGING;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ID3D11Texture2D *st = NULL;
	ID3D11Device_CreateTexture2D(d3d, &td, NULL, &st);
	ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, tex);
	D3D11_MAPPED_SUBRESOURCE m;
	uint32_t v = 0;
	if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
		v = ((uint32_t *)((uint8_t *)m.pData + (size_t)y * m.RowPitch))[x];
		ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
	}
	ID3D11Texture2D_Release(st);
	return v;
}

// vs_3_0: dcl_position v0, dcl_texcoord0 v1, dcl_position o0, dcl_texcoord0 o1, mov o0, v0, mov o1, v1
static const uint32_t VS3[] = {0xfffe0300, 0x0200001f, 0x80000000, 0x900f0000, 0x0200001f, 0x80000005, 0x900f0001, 0x0200001f, 0x80000000, 0xe00f0000, 0x0200001f, 0x80000005, 0xe00f0001,
							   0x02000001, 0xe00f0000, 0x90e40000, 0x02000001, 0xe00f0001, 0x90e40001, 0x0000ffff};
// ps_3_0: dcl_texcoord0 v0, dcl_2d s0, texld r0, v0, s0, mov oC0, r0
static const uint32_t PS_TEX[] = {0xffff0300, 0x0200001f, 0x80000005, 0x900f0000, 0x0200001f, 0x90000000, 0xa00f0800, 0x03000042, 0x800f0000, 0x90e40000, 0xa0e40800, 0x02000001, 0x800f0800, 0x80e40000, 0x0000ffff};
// ps_3_0: mov oC0, c0
static const uint32_t PS_CONST[] = {0xffff0300, 0x02000001, 0x800f0800, 0xa0e40000, 0x0000ffff};

static nd_shader *id_shader(void *p) { return p; }
static nd_decl *id_decl(void *p) { return p; }
static nat_image *id_image(void *p) { return p; }
static const nd_ops OPS = {id_shader, id_decl, id_image, NULL};

int main(void)
{
	WNDCLASSA wc = {0};
	wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.lpszClassName = "natdrawtest";
	RegisterClassA(&wc);
	HWND hwnd = CreateWindowA("natdrawtest", "natdraw test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 50, 50, 300, 300, NULL, NULL, wc.hInstance, NULL);
	char err[256] = "";
	nat_params p = {hwnd, 64, 64, 0, 0, 0, 0};
	nat_device *dev = NULL;
	if (!nat_create(&p, &dev, err, sizeof(err))) {
		printf("FAIL create: %s\n", err);
		return 1;
	}
	nd *d = nd_create(dev, &OPS);
	nd_shader *vs = nd_shader_create(d, VS3, sizeof(VS3) / 4), *ps_tex = nd_shader_create(d, PS_TEX, sizeof(PS_TEX) / 4), *ps_const = nd_shader_create(d, PS_CONST, sizeof(PS_CONST) / 4);
	const nd_elem els[2] = {{0, 0, 2, 0, 0}, {0, 12, 1, 5, 0}}; // FLOAT3 position, FLOAT2 texcoord
	nd_decl *decl = nd_decl_create(els, 2);
	static const float verts[4][5] = {{-1, 1, 0, 0, 0}, {1, 1, 0, 1, 0}, {-1, -1, 0, 0, 1}, {1, -1, 0, 1, 1}};
	static const uint16_t idx[6] = {0, 1, 2, 2, 1, 3};
	nat_buffer *vb = nat_buffer_create(dev, sizeof(verts), 0, 0, verts), *ib = nat_buffer_create(dev, sizeof(idx), 1, 0, idx);
	CHECK(vb && ib, "buffers");
	nd_set_stream(d, 0, vb, 0, 20);
	nd_set_indices(d, ib, 0);
	nd_set_targets(d, NULL, ND_NO_DEPTH);
	nd_set_viewport(d, 0, 0, 64, 64, 0, 1);

	nat_image_desc id = {NAT_IMAGE_2D, 2, 2, 1, 1, 21, 0, 0};
	nat_image *im = nat_image_create(dev, &id);
	const uint32_t texels[4] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff};
	nat_image_upload(im, 0, 0, texels, 8, 0);

	ng_state g;
	ng_init(&g);
	g.vs = vs;
	g.ps = ps_tex;
	g.decl = decl;
	g.words_valid = 1;
	g.word0 = 1u << 27 | 1u << 28 | 1u << 14 | 1u << 11; // write all channels, no culling, alpha test off, blending off
	g.word1 = 1u << 1;                                   // depth test off
	g.tex[0] = im;
	g.sampler_word[0] = 1u << 8 | 1u << 12 | 3u << 20 | 3u << 22 | 3u << 24; // point min / mag, no mip filter, clamp
	const float black[4] = {0, 0, 0, 1};
	nd_clear(d, 1, black, 1.0f, 0);
	CHECK(nd_draw_indexed(d, &g, 0, 0, 2), "draw refused: %s", nd_last_error(d));
	ID3D11Resource *scene = NULL;
	ID3D11RenderTargetView_GetResource((ID3D11RenderTargetView *)nat_scene_rtv(dev), &scene);
	{
		const uint32_t a = read_pixel(dev, scene, 10, 10, 64, 64), b = read_pixel(dev, scene, 50, 10, 64, 64), c = read_pixel(dev, scene, 10, 50, 64, 64), e = read_pixel(dev, scene, 50, 50, 64, 64);
		CHECK(a == 0xffff0000 && b == 0xff00ff00 && c == 0xff0000ff && e == 0xffffffff, "textured quad %08x %08x %08x %08x", a, b, c, e);
	}
	// an alpha-blended constant colour over it: blue at 50%
	g.ps = ps_const;
	g.word0 = (g.word0 & ~0x7ffu) | 5u | 6u << 4 | 1u << 8; // SRCALPHA, INVSRCALPHA, ADD
	g.ps_c[0][0] = 0;
	g.ps_c[0][1] = 0;
	g.ps_c[0][2] = 1;
	g.ps_c[0][3] = 0.5f;
	g.ps_dirty[0] |= 1;
	CHECK(nd_draw_indexed(d, &g, 0, 0, 2), "blend draw refused: %s", nd_last_error(d));
	{
		const uint32_t a = read_pixel(dev, scene, 10, 10, 64, 64); // red under 50% blue: (127, 0, 128)
		const int r = (a >> 16) & 255, gg = (a >> 8) & 255, b = a & 255;
		CHECK(r >= 126 && r <= 129 && gg == 0 && b >= 126 && b <= 129, "blended pixel %08x", a);
		const uint32_t w = read_pixel(dev, scene, 50, 50, 64, 64); // white under 50% blue: (127, 127, 255)
		CHECK(((w >> 16) & 255) >= 126 && ((w >> 16) & 255) <= 129 && (w & 255) == 255, "blended white %08x", w);
	}
	// write mask: only alpha channel writes leave the colour alone
	g.word0 = (g.word0 & ~(3u << 27)) | 1u << 28 | 0; // write mask alpha only, blending still on
	g.ps_c[0][2] = 0;
	g.ps_c[0][0] = 1;
	g.ps_dirty[0] |= 1;
	nd_draw_indexed(d, &g, 0, 0, 2);
	{
		const uint32_t a = read_pixel(dev, scene, 10, 10, 64, 64);
		CHECK(((a >> 16) & 255) >= 126 && ((a >> 16) & 255) <= 129, "colour untouched with an alpha-only write mask: %08x", a);
	}
	// an input texture that is the render target is not bound (no crash, samples as unbound = zero)
	{
		nat_image_desc rtd = {NAT_IMAGE_2D, 64, 64, 1, 1, 21, NAT_RENDER_TARGET, 0};
		nat_image *rt = nat_image_create(dev, &rtd);
		CHECK(rt != NULL, "render target image");
		nd_set_targets(d, rt, ND_NO_DEPTH);
		ID3D11RenderTargetView *rv = nat_image_rtv(rt, 0);
		const float red[4] = {1, 0, 0, 1};
		ID3D11DeviceContext_ClearRenderTargetView((ID3D11DeviceContext *)nat_d3d_context(dev), rv, red);
		g.ps = ps_tex;
		g.word0 = 1u << 27 | 1u << 28 | 1u << 14 | 1u << 11;
		g.tex[0] = rt;
		CHECK(nd_draw_indexed(d, &g, 0, 0, 2), "feedback draw refused: %s", nd_last_error(d));
		ID3D11Resource *res = NULL;
		ID3D11RenderTargetView_GetResource(rv, &res);
		const uint32_t a = read_pixel(dev, res, 20, 20, 64, 64);
		CHECK(a == 0x00000000u, "target as input samples zero: %08x", a);
		ID3D11Resource_Release(res);
		nat_image_destroy(rt);
	}
	// no shader: refused with a reason
	{
		ng_state empty;
		ng_init(&empty);
		CHECK(!nd_draw_indexed(d, &empty, 0, 0, 2) && nd_last_error(d)[0], "an empty state must be refused");
	}
	unsigned st, la, sh, up, dr, rf;
	nd_stats(d, &st, &la, &sh, &up, &dr, &rf);
	printf("state objects %u, layouts %u, shader variants %u, constant uploads %u, draws %u, refused %u\n", st, la, sh, up, dr, rf);
	CHECK(la == 1 && sh == 3, "caches: %u layouts, %u shader variants", la, sh);
	ID3D11Resource_Release(scene);
	nat_image_destroy(im);
	nd_shader_destroy(vs);
	nd_shader_destroy(ps_tex);
	nd_shader_destroy(ps_const);
	nd_decl_destroy(decl);
	nat_buffer_destroy(vb);
	nat_buffer_destroy(ib);
	nd_destroy(d);
	nat_destroy(dev);
	printf(fails ? "%d checks FAILED\n" : "all checks passed\n", fails);
	return fails ? 1 : 0;
}
