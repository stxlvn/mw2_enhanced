// Test of src/dx/natres.c: the game's image creation and upload on a real Direct3D 11 device, with synthetic image structs. Windows only.
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

static unsigned errors;
static char last_error[64];
static void on_error(const char *what, const char *name, unsigned w, unsigned h, unsigned format)
{
	(void)name;
	(void)w;
	(void)h;
	(void)format;
	errors++;
	snprintf(last_error, sizeof(last_error), "%s", what);
}
static unsigned sized;
static void on_sizes(void *image, unsigned flags, int format, unsigned w, unsigned h, unsigned d)
{
	(void)flags;
	(void)format;
	(void)w;
	(void)h;
	(void)d;
	sized++;
	*(int *)((uint8_t *)image + 0x10) = 1234;
}

static uint32_t read_pixel(nat_device *dev, nat_image *im, unsigned x, unsigned y, unsigned w, unsigned h)
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
	ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)st, 0, 0, 0, 0, (ID3D11Resource *)nat_image_resource(im), 0, NULL);
	D3D11_MAPPED_SUBRESOURCE m;
	uint32_t v = 0;
	if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
		v = ((uint32_t *)((uint8_t *)m.pData + (size_t)y * m.RowPitch))[x];
		ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
	}
	ID3D11Texture2D_Release(st);
	return v;
}

int main(void)
{
	WNDCLASSA wc = {0};
	wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.lpszClassName = "natrestest";
	RegisterClassA(&wc);
	HWND hwnd = CreateWindowA("natrestest", "natres test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 50, 50, 200, 200, NULL, NULL, wc.hInstance, NULL);
	char err[256] = "";
	nat_params p = {hwnd, 64, 64, 0, 0, 0, 0};
	nat_device *dev = NULL;
	if (!nat_create(&p, &dev, err, sizeof(err))) {
		printf("FAIL create: %s\n", err);
		return 1;
	}
	ngr r = {dev, 1, on_sizes, on_error};
	static uint8_t img[0x40];

	// 2D A8R8G8B8 4x4 with a mip chain: upload level 0 and read it back
	memset(img, 0, sizeof(img));
	CHECK(ngr_image_create(&r, img, 4, 4, 1, 0, 0x15), "2D create");
	CHECK(img[8] == NGR_KIND_2D && *(uint16_t *)(img + 0x18) == 4 && *(uint16_t *)(img + 0x1a) == 4 && *(int *)(img + 0x10) == 1234 && sized == 1, "2D fields (kind %d, size slot %d)", img[8], *(int *)(img + 0x10));
	uint32_t px[16];
	for (int i = 0; i < 16; i++)
		px[i] = 0xff000000u | (uint32_t)i * 0x010101u * 8;
	ngr_image_upload(&r, img, 0x15, 0, 0, px);
	CHECK(read_pixel(dev, *(nat_image **)img, 3, 2, 4, 4) == px[2 * 4 + 3], "uploaded pixel %08x", read_pixel(dev, *(nat_image **)img, 3, 2, 4, 4));
	CHECK(nat_image_levels(*(nat_image **)img) == 3, "full mip chain of a 4x4: %u levels", nat_image_levels(*(nat_image **)img));
	uint32_t l1[4] = {1, 2, 3, 4}, l2[1] = {5};
	ngr_image_upload(&r, img, 0x15, 0, 1, l1);
	ngr_image_upload(&r, img, 0x15, 0, 2, l2);
	ngr_image_release(img);
	CHECK(*(void **)img == NULL, "released");

	// no mip chain (flag 2)
	memset(img, 0, sizeof(img));
	CHECK(ngr_image_create(&r, img, 8, 8, 1, 2, 0x15) && nat_image_levels(*(nat_image **)img) == 1, "no-mip flag");
	ngr_image_release(img);

	// DXT1 8x8 with levels 0..3 (8, 4, 2, 1 pixels: always at least one block)
	memset(img, 0, sizeof(img));
	CHECK(ngr_image_create(&r, img, 8, 8, 1, 0, 0x31545844), "DXT1 create");
	static uint8_t blocks[4 * 8];
	for (unsigned l = 0; l < 4; l++)
		ngr_image_upload(&r, img, 0x31545844, 0, l, blocks);
	ngr_image_release(img);

	// cube with mip support, six faces of level 0
	memset(img, 0, sizeof(img));
	CHECK(ngr_image_create(&r, img, 8, 8, 1, 0x10000, 0x15), "cube create");
	CHECK(img[8] == NGR_KIND_CUBE && *(uint16_t *)(img + 0x1a) == 8 && *(uint16_t *)(img + 0x1c) == 1, "cube fields");
	static uint32_t face[64];
	for (unsigned f = 0; f < 6; f++)
		ngr_image_upload(&r, img, 0x15, f, 0, face);
	ngr_image_release(img);
	// a cube without mip support has exactly one level, and level > 0 uploads are ignored
	r.mip_cube = 0;
	memset(img, 0, sizeof(img));
	CHECK(ngr_image_create(&r, img, 8, 8, 1, 0x10000, 0x15) && nat_image_levels(*(nat_image **)img) == 1, "cube without mip support");
	ngr_image_upload(&r, img, 0x15, 0, 1, face);
	ngr_image_release(img);
	r.mip_cube = 1;

	// volume 4x4x4 A8R8G8B8
	memset(img, 0, sizeof(img));
	CHECK(ngr_image_create(&r, img, 4, 4, 4, 0x20000, 0x15) && img[8] == NGR_KIND_VOLUME, "volume create");
	static uint32_t vol[64];
	ngr_image_upload(&r, img, 0x15, 0, 0, vol);
	ngr_image_release(img);

	// 1D: one pixel high
	memset(img, 0, sizeof(img));
	CHECK(ngr_image_create(&r, img, 16, 1, 1, 0x30000, 0x15) && img[8] == NGR_KIND_1D && *(uint32_t *)(img + 0x1a) == 0x10001u, "1D fields");
	ngr_image_release(img);

	// render target and depth images
	memset(img, 0, sizeof(img));
	CHECK(ngr_image_create(&r, img, 16, 16, 1, 1u << 25, 0x15) && nat_image_rtv(*(nat_image **)img, 0), "render target");
	ngr_image_release(img);
	memset(img, 0, sizeof(img));
	CHECK(ngr_image_create(&r, img, 16, 16, 1, 1u << 25, 0x4b) && nat_image_dsv(*(nat_image **)img), "depth image");
	ngr_image_release(img);
	nat_image *depth = ngr_depth_create(&r, 64, 64, 1);
	CHECK(depth && nat_image_dsv(depth), "shared depth buffer");
	nat_image_destroy(depth);

	// a format that cannot be made reports an error and leaves the texture field empty
	memset(img, 0, sizeof(img));
	CHECK(!ngr_image_create(&r, img, 4, 4, 1, 0, 12345) && errors == 1 && *(void **)img == NULL, "unknown format: errors %u (%s)", errors, last_error);
	// uploads into an image without a texture do nothing
	ngr_image_upload(&r, img, 0x15, 0, 0, px);

	nat_destroy(dev);
	printf(fails ? "%d checks FAILED\n" : "all checks passed\n", fails);
	return fails ? 1 : 0;
}
