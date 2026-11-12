// Test of src/dx/natdrawfn.c on a real Direct3D 11 device with synthetic game memory (an "exe image" with the rva globals the functions read, a state, an input, a material, a technique with one pass).
//   natdrawfn_test   (exit code 0 = all checks passed)
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/dx/natdrawfn.h"

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

#define W16(p, off, v) (*(uint16_t *)((uint8_t *)(p) + (off)) = (uint16_t)(v))
#define W8(p, off, v) (*(uint8_t *)((uint8_t *)(p) + (off)) = (uint8_t)(v))
#define W32(p, off, v) (*(uint32_t *)((uint8_t *)(p) + (off)) = (uint32_t)(v))
#define WP(p, off, v) (*(void **)((uint8_t *)(p) + (off)) = (void *)(v))
#define RP(p, off) (*(void **)((uint8_t *)(p) + (off)))
#define RI(p, off) (*(int32_t *)((uint8_t *)(p) + (off)))

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

static const uint32_t VS3[] = {0xfffe0300, 0x0200001f, 0x80000000, 0x900f0000, 0x0200001f, 0x80000005, 0x900f0001, 0x0200001f, 0x80000000, 0xe00f0000, 0x0200001f, 0x80000005, 0xe00f0001,
							   0x02000001, 0xe00f0000, 0x90e40000, 0x02000001, 0xe00f0001, 0x90e40001, 0x0000ffff};
static const uint32_t PS_TEX[] = {0xffff0300, 0x0200001f, 0x80000005, 0x900f0000, 0x0200001f, 0x90000000, 0xa00f0800, 0x03000042, 0x800f0000, 0x90e40000, 0xa0e40800, 0x02000001, 0x800f0800, 0x80e40000, 0x0000ffff};

static nd_shader *id_shader(void *p) { return p; }
static nd_decl *id_decl(void *p) { return p; }
static nat_image *id_image(void *p) { return p; }
static const nd_ops OPS = {id_shader, id_decl, id_image, NULL};

int main(void)
{
	WNDCLASSA wc = {0};
	wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.lpszClassName = "natdrawfntest";
	RegisterClassA(&wc);
	HWND hwnd = CreateWindowA("natdrawfntest", "natdrawfn test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 50, 50, 300, 300, NULL, NULL, wc.hInstance, NULL);
	char err[256] = "";
	nat_params p = {hwnd, 64, 64, 0, 0, 0, 0};
	nat_device *dev = NULL;
	if (!nat_create(&p, &dev, err, sizeof(err))) {
		printf("FAIL create: %s\n", err);
		return 1;
	}
	nd *d = nd_create(dev, &OPS);

	// the exe image: only the rva globals read by the functions
	uint8_t *exe = calloc(1, 0x4600000);
	uint8_t *input = calloc(1, 0x2000), *state = calloc(1, 0x1000);
	static uint32_t filter_table[32];
	filter_table[0] = 1u << 8 | 1u << 12; // point min / mag
	ng_state g;
	ng_init(&g);
	ng_ops nops = {NULL, NULL, NULL, filter_table, 1};
	ngf f = {d, &g, &nops, exe, NULL, NULL, NULL};

	// rings: vertex ring (a dynamic buffer of 1024 bytes), index ring 0 (a dynamic buffer of 64 indices)
	static ngf_ring vring, iring;
	vring.capacity = 1024;
	vring.buffer = nat_buffer_create(dev, 1024, 0, 1, NULL);
	iring.capacity = 64;
	iring.buffer = nat_buffer_create(dev, 64 * 2, 1, 1, NULL);
	WP(exe, 0x41165d8, &vring);
	memcpy(exe + 0x4116568, &iring, sizeof(iring));
	// (the game's first index ring lives inside the exe image: the function writes its offset back there)
	WP(exe, 0x4116580, exe + 0x4116568);
	// the frame's vertices: 4 of 32 bytes (float3 position, float2 uv at 12, padding)
	static const float quad[4][8] = {{-1, 1, 0, 0, 0}, {1, 1, 0, 1, 0}, {-1, -1, 0, 0, 1}, {1, -1, 0, 1, 1}};
	memcpy(exe + 0x435a500, quad, sizeof(quad));
	W32(exe, 0x4584e4c, 4);
	*(float *)(exe + 0x3859e4) = 0.0f;
	*(float *)(exe + 0x385a54) = 1.0f;

	// render targets: entry 0 = the scene, entry 1 = a render-target image
	nat_image_desc rtd = {NAT_IMAGE_2D, 64, 64, 1, 1, 21, NAT_RENDER_TARGET, 0};
	nat_image *rt = nat_image_create(dev, &rtd);
	static int rt_game_image[16]; // the game's image object whose first field is the texture; here the sampler binding uses the nat_image directly
	(void)rt_game_image;
	uint8_t *table = exe + 0x4359000;
	W32(table, 0x18, 64);
	W32(table, 0x1c, 64);
	WP(table, 8, NULL);
	WP(table, 0x10, ND_NO_DEPTH);
	WP(table + 32, 0, rt);
	WP(table + 32, 8, rt);
	WP(table + 32, 0x10, ND_NO_DEPTH);
	W32(table + 32, 0x18, 64);
	W32(table + 32, 0x1c, 64);

	// the game's tables: shaders, declaration, material with one sampler, a technique with one pass
	nd_shader *vsg = nd_shader_create(d, VS3, sizeof(VS3) / 4), *psg = nd_shader_create(d, PS_TEX, sizeof(PS_TEX) / 4);
	const nd_elem els[2] = {{0, 0, 2, 0, 0}, {0, 12, 1, 5, 0}};
	nd_decl *decl = nd_decl_create(els, 2);
	uint8_t *vs_obj = calloc(1, 0x20), *ps_obj = calloc(1, 0x20), *decl_set = calloc(1, 0x80);
	WP(vs_obj, 8, vsg);
	WP(ps_obj, 8, psg);
	WP(decl_set, 0x30, decl);
	nat_image_desc id = {NAT_IMAGE_2D, 2, 2, 1, 1, 21, 0, 0};
	nat_image *im = nat_image_create(dev, &id);
	const uint32_t texels[4] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff};
	nat_image_upload(im, 0, 0, texels, 8, 0);
	uint8_t *mat = calloc(1, 0x200), *textures = calloc(2, 16), *statebits = calloc(4, 8), *tech = calloc(1, 0x200), *args = calloc(4, 16);
	W32(textures, 0, 0x3333);
	W8(textures, 6, 0x00);
	WP(textures, 8, im);
	WP(mat, NG_MAT_TEXTURES, textures);
	WP(mat, NG_MAT_STATEBITS, statebits);
	W32(statebits, 0, 1u << 27 | 1u << 28 | 1u << 14 | 1u << 11);
	W32(statebits, 4, 1u << 1);
	uint8_t *pass = tech + 2 * 8;
	WP(pass, 0, decl_set);
	WP(pass, 8, vs_obj);
	WP(pass, 0x10, ps_obj);
	W8(pass, 0x1a, 1); // one stable argument: the material sampler into sampler 0
	WP(pass, 0x20, args);
	W16(args, 0, 2);
	W16(args, 2, 0);
	W32(args, 8, 0x3333);
	W16(tech, 10, 1); // one pass
	WP(state, NG_ST_MATERIAL, mat);
	WP(state, NG_ST_TECH, tech);
	W32(state, NG_ST_TECH_TYPE, 0);
	RI(state, 0xb98) = -1; // no target set yet
	W32(state, 0xb90, 64);
	W32(state, 0xb94, 64);
	void *ctx[2] = {input, state};

	// target switch to the scene, viewport
	ngf_target_switch(&f, input, state, 0);
	CHECK(RI(state, 0xb98) == 0 && RI(state, 0xb90) == 64 && RI(state, 0x144) == -1, "target 0 bookkeeping");
	const int vp[4] = {0, 0, 64, 64};
	state[0xb88] = 0xff; // force the viewport to be applied
	ngf_set_viewport(&f, state, vp);
	CHECK(RI(state, 0xb90) == 64, "viewport fields");
	ngf_depth_range(&f, state, 0);
	CHECK(*(float *)(state + 0x14c) == 1.0f, "full depth range");

	// the index ring: two triangles; then the draw loop
	static const uint16_t idx[6] = {0, 1, 2, 2, 1, 3};
	const int start = ngf_ring_indices(&f, state, idx, 2);
	CHECK(start == 0 && iring.offset == 0, "index ring start %d offset %d (the ring struct lives in the exe image)", start, iring.offset);
	ngf_ring *real_iring = (ngf_ring *)(exe + 0x4116568);
	CHECK(real_iring->offset == 6, "index ring advanced to %d", real_iring->offset);
	const float black[4] = {0, 0, 0, 1};
	nd_clear(d, 1, black, 1.0f, 0);
	const int cmd[3] = {4, 2, 0};
	ngf_draw_26840(&f, ctx, cmd);
	CHECK(vring.offset == 128, "vertex ring advanced to %d", vring.offset);
	CHECK(RP(state, 0xf0) == vring.buffer && RI(state, 0xe8) == 0x20 && RI(state, 0xf8) == 0, "stream 0 cache fields");
	{
		ID3D11Resource *scene = NULL;
		ID3D11RenderTargetView_GetResource((ID3D11RenderTargetView *)nat_scene_rtv(dev), &scene);
		const uint32_t a = read_pixel(dev, scene, 10, 10, 64, 64), b = read_pixel(dev, scene, 50, 10, 64, 64), c = read_pixel(dev, scene, 10, 50, 64, 64), e = read_pixel(dev, scene, 50, 50, 64, 64);
		CHECK(a == 0xffff0000 && b == 0xff00ff00 && c == 0xff0000ff && e == 0xffffffff, "draw loop %08x %08x %08x %08x", a, b, c, e);
		ID3D11Resource_Release(scene);
	}
	// a second frame continues the ring; the draw goes through again
	ngf_draw_26840(&f, ctx, cmd);
	CHECK(vring.offset == 256, "second frame: vertex ring at %d", vring.offset);
	// the ring wraps when it would overflow: capacity 1024, six more frames of 128 bytes fit, the ninth restarts at 0
	for (int i = 0; i < 6; i++)
		ngf_draw_26840(&f, ctx, cmd);
	CHECK(vring.offset == 1024, "ring full at %d", vring.offset);
	ngf_draw_26840(&f, ctx, cmd);
	CHECK(vring.offset == 128, "ring wrapped to %d", vring.offset);

	// render target switch: the sampler that holds the target image is unbound
	g.tex[3] = rt;
	WP(state, 0x50 + 3 * 8, rt);
	ngf_target_switch(&f, input, state, 1);
	CHECK(g.tex[3] == NULL && RP(state, 0x50 + 3 * 8) == NULL, "sampler holding the new target is unbound");
	CHECK(RI(state, 0xb98) == 1 && RI(state, 0xb90) == 64, "target 1 bookkeeping");
	nd_clear(d, 1, (const float[4]){0, 0, 1, 1}, 1.0f, 0);
	ngf_draw_26840(&f, ctx, cmd);
	{
		ID3D11Resource *res = NULL;
		ID3D11RenderTargetView_GetResource((ID3D11RenderTargetView *)nat_image_rtv(rt, 0), &res);
		const uint32_t a = read_pixel(dev, res, 10, 10, 64, 64), e = read_pixel(dev, res, 50, 50, 64, 64);
		CHECK(a == 0xffff0000 && e == 0xffffffff, "drawn into the render-target image %08x %08x", a, e);
		ID3D11Resource_Release(res);
	}
	unsigned st, la, sh, up, dr, rf;
	nd_stats(d, &st, &la, &sh, &up, &dr, &rf);
	printf("draws %u, refused %u\n", dr, rf);
	CHECK(rf == 0, "no draw may be refused (last: %s)", nd_last_error(d));
	printf(fails ? "%d checks FAILED\n" : "all checks passed\n", fails);
	return fails ? 1 : 0;
}
