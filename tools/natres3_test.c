// Test of src/dx/natres3.c: model lighting blocks into a volume and the film planes, on a real device with synthetic game memory.
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/dx/natres.h"

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
#define W32(p, off, v) (*(uint32_t *)((uint8_t *)(p) + (off)) = (uint32_t)(v))
#define WP(p, off, v) (*(void **)((uint8_t *)(p) + (off)) = (void *)(v))
#define RP(p, off) (*(void **)((uint8_t *)(p) + (off)))
#define RI(p, off) (*(int32_t *)((uint8_t *)(p) + (off)))

static unsigned writer_calls;
static uint8_t *g_exe;
static void fake_writer(const uint8_t *entry)
{
	// the game's writer uses the lock globals: pBits + SlicePitch * z + x * 16 + offsets[k]
	writer_calls++;
	const unsigned code = *(const uint16_t *)entry, x = code & 0x3f, zq = (code >> 4) & 0xffffffcu;
	uint8_t *bits = *(uint8_t **)(g_exe + 0x488a378);
	const int slice = *(int *)(g_exe + 0x488a374);
	const int *offs = (const int *)(g_exe + 0x489a400);
	for (int k = 0; k < 64; k++)
		*(uint32_t *)(bits + (size_t)slice * zq + (size_t)x * 0x10 + offs[k]) = 0xff112233u;
}
static int rects_reported = 1;
static int fake_rects(void *bink)
{
	(void)bink;
	return rects_reported;
}

int main(void)
{
	WNDCLASSA wc = {0};
	wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.lpszClassName = "natres3test";
	RegisterClassA(&wc);
	HWND hwnd = CreateWindowA("natres3test", "natres3 test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 50, 50, 200, 200, NULL, NULL, wc.hInstance, NULL);
	char err[256] = "";
	nat_params p = {hwnd, 64, 64, 0, 0, 0, 0};
	nat_device *dev = NULL;
	if (!nat_create(&p, &dev, err, sizeof(err))) {
		printf("FAIL create: %s\n", err);
		return 1;
	}
	uint8_t *exe = calloc(1, 0x4900000);
	g_exe = exe;
	ngr r = {0};
	r.dev = dev;
	r.exe = exe;
	r.fn_1fa40 = fake_writer;
	r.fn_bink_rects = fake_rects;

	// model lighting: a volume of 16 x 4 x 8 texels (the game's is 256 wide); entry 0 fills the block at x = 4 (code & 63 = 1), z = 0 with one colour, entry 1 uses the writer at x = 8, z = 4
	static uint8_t vol_image[0x40];
	CHECK(ngr_image_create(&r, vol_image, 16, 4, 8, 0x20000, 0x15), "volume");
	uint8_t entries[2 * 0x34] = {0};
	W16(entries, 0, 1);
	entries[3] = 0;
	W32(entries, 4, 0xff0a0b0cu);
	W16(entries + 0x34, 0, 2 | (4 << 4));
	entries[0x34 + 3] = 1;
	ngr_model_lighting(&r, vol_image, entries, 2);
	CHECK(writer_calls == 1, "the game's writer is used for flagged entries");
	{
		ID3D11Device *d3d = nat_d3d_device(dev);
		ID3D11DeviceContext *ctx = nat_d3d_context(dev);
		D3D11_TEXTURE3D_DESC td = {0};
		td.Width = 16;
		td.Height = 4;
		td.Depth = 8;
		td.MipLevels = 1;
		td.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
		td.Usage = D3D11_USAGE_STAGING;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ID3D11Texture3D *st = NULL;
		CHECK(SUCCEEDED(ID3D11Device_CreateTexture3D(d3d, &td, NULL, &st)), "staging volume");
		ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)st, 0, 0, 0, 0, (ID3D11Resource *)nat_image_resource(*(nat_image **)vol_image), 0, NULL);
		D3D11_MAPPED_SUBRESOURCE m;
		CHECK(SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m)), "map");
		const uint8_t *bits = m.pData;
		#define TEXEL(x, y, z) (*(const uint32_t *)(bits + (size_t)(z) * m.DepthPitch + (size_t)(y) * m.RowPitch + (size_t)(x) * 4))
		CHECK(TEXEL(4, 0, 0) == 0xff0a0b0cu && TEXEL(7, 3, 3) == 0xff0a0b0cu && TEXEL(3, 0, 0) == 0 && TEXEL(8, 0, 0) == 0, "colour block 4x4x4 at x = 4: %08x", TEXEL(4, 0, 0));
		CHECK(TEXEL(8, 0, 4) == 0xff112233u && TEXEL(11, 3, 7) == 0xff112233u && TEXEL(8, 0, 3) == 0, "writer block at x = 8, z = 4: %08x", TEXEL(8, 0, 4));
		ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
		ID3D11Texture3D_Release(st);
	}

	// film: 2 frames, planes Y (8x4), Cr and Cb (4x2), no alpha; the real planes are separate images
	static uint8_t film[0x200];
	nat_image_desc yd = {NAT_IMAGE_2D, 8, 4, 1, 1, 50, 0, 0}, cd = {NAT_IMAGE_2D, 4, 2, 1, 1, 50, 0, 0};
	nat_image *sys[2][3], *gpu[3];
	for (int f = 0; f < 2; f++) {
		sys[f][0] = nat_image_create(dev, &yd);
		sys[f][1] = nat_image_create(dev, &cd);
		sys[f][2] = nat_image_create(dev, &cd);
		for (int pl = 0; pl < 3; pl++)
			WP(film, 0x10 + f * 0x30 + pl * 8, sys[f][pl]);
	}
	gpu[0] = nat_image_create(dev, &yd);
	gpu[1] = nat_image_create(dev, &cd);
	gpu[2] = nat_image_create(dev, &cd);
	for (int pl = 0; pl < 3; pl++)
		WP(film, 0x148 + pl * 8, gpu[pl]);
	W32(film, 0x60, 2);
	W32(film, 0x74, 1); // the current frame
	ngr_film_lock(&r, film);
	CHECK(RP(film, 0x80 + 1 * 0x60) != NULL && RI(film, 0x88 + 1 * 0x60) == 8 && RI(film, 0xa0 + 1 * 0x60) == 4 && RP(film, 0x80 + 3 * 0x18 + 0x60) == NULL, "locked plane pointers and pitches");
	uint8_t *y = RP(film, 0x80 + 1 * 0x60);
	for (int i = 0; i < 8 * 4; i++)
		y[i] = (uint8_t)(i * 8);
	ngr_film_unlock(&r, film, NULL);
	CHECK(RP(film, 0x80) == NULL && RP(film, 0x80 + 0x60) == NULL, "locked pointers cleared");
	{
		ID3D11Device *d3d = nat_d3d_device(dev);
		ID3D11DeviceContext *ctx = nat_d3d_context(dev);
		D3D11_TEXTURE2D_DESC td = {0};
		td.Width = 8;
		td.Height = 4;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_STAGING;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ID3D11Texture2D *st = NULL;
		ID3D11Device_CreateTexture2D(d3d, &td, NULL, &st);
		ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)nat_image_resource(gpu[0]));
		D3D11_MAPPED_SUBRESOURCE m;
		CHECK(SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m)), "map");
		const uint32_t a = ((uint32_t *)m.pData)[1], b = *(uint32_t *)((uint8_t *)m.pData + 3 * m.RowPitch + 7 * 4);
		CHECK((a & 255) == 8 && (b & 255) == (uint8_t)(31 * 8), "film Y plane reached the real image: %08x %08x", a, b);
		ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
		ID3D11Texture2D_Release(st);
	}
	rects_reported = 0; // no changed rectangles: nothing is uploaded
	ngr_film_lock(&r, film);
	((uint8_t *)RP(film, 0x80 + 0x60))[0] = 200;
	ngr_film_unlock(&r, film, NULL);
	printf(fails ? "%d checks FAILED\n" : "all checks passed\n", fails);
	return fails ? 1 : 0;
}
