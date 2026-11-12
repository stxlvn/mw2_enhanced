// Test of the native Direct3D 11 device layer (src/dx/nat11.c): a window, the device and swap chain without any Direct3D 9 object, a clear, a texture upload and a draw with HLSL shaders,
// read back and checked, a reset to another size, a multisampled scene, a dynamic buffer. Windows only.
//   nat11_test            (exit code 0 = all checks passed)
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>

#include "../src/dx/nat11.h"

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

static uint32_t read_pixel(nat_device *d, ID3D11Resource *tex, unsigned x, unsigned y, unsigned w, unsigned h)
{
	ID3D11Device *dev = nat_d3d_device(d);
	ID3D11DeviceContext *ctx = nat_d3d_context(d);
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
	ID3D11Device_CreateTexture2D(dev, &td, NULL, &st);
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

static const char *VS = "float4 main(uint id : SV_VertexID, out float2 uv : TEXCOORD0) : SV_Position { uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }";
static const char *PS = "Texture2D t : register(t0); SamplerState s : register(s0); float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return t.Sample(s, uv); }";

int main(void)
{
	WNDCLASSA wc = {0};
	wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.lpszClassName = "nat11test";
	RegisterClassA(&wc);
	HWND hwnd = CreateWindowA("nat11test", "nat11 test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 50, 50, 700, 500, NULL, NULL, wc.hInstance, NULL);
	char err[256] = "";
	nat_params p = {hwnd, 320, 200, 0, 0, 0, 0};
	nat_device *d = NULL;
	CHECK(nat_create(&p, &d, err, sizeof(err)), "create: %s", err);
	if (!d)
		return 1;
	const nat_caps *c = nat_get_caps(d);
	printf("adapter %s, desktop %ux%u, msaa max %d\n", c->adapter, c->desktop_w, c->desktop_h, c->msaa_max);
	CHECK(c->max_units == 16 && c->max_texture >= 8192 && c->desktop_w > 0, "caps");

	ID3D11DeviceContext *ctx = nat_d3d_context(d);
	const float orange[4] = {1.0f, 0.5f, 0.0f, 1.0f};
	ID3D11DeviceContext_ClearRenderTargetView(ctx, nat_scene_rtv(d), orange);
	CHECK(nat_present(d) >= 0, "present");
	{
		ID3D11Resource *res = NULL;
		ID3D11RenderTargetView_GetResource((ID3D11RenderTargetView *)nat_scene_rtv(d), &res);
		const uint32_t px = read_pixel(d, res, 10, 10, 320, 200);
		CHECK(px == 0xffff8000u || px == 0xffff7f00u, "clear color %08x", px);
		ID3D11Resource_Release(res);
	}

	// image upload (a 2x2 texture, red/green/blue/white) and a draw that samples it over the whole scene
	nat_image_desc id = {NAT_IMAGE_2D, 2, 2, 1, 1, 21, 0, 0};
	nat_image *im = nat_image_create(d, &id);
	CHECK(im != NULL, "image");
	const uint32_t texels[4] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff};
	CHECK(nat_image_upload(im, 0, 0, texels, 8, 0), "upload");
	char e2[300] = "";
	nat_shader *vs = nat_shader_from_hlsl(d, VS, 1, e2, sizeof(e2));
	nat_shader *ps = nat_shader_from_hlsl(d, PS, 0, e2, sizeof(e2));
	CHECK(vs && ps, "shaders: %s", e2);
	if (vs && ps && im) {
		ID3D11Device *dev = nat_d3d_device(d);
		D3D11_SAMPLER_DESC sd = {0};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		ID3D11SamplerState *smp = NULL;
		ID3D11Device_CreateSamplerState(dev, &sd, &smp);
		D3D11_VIEWPORT vp = {0, 0, 320, 200, 0, 1};
		ID3D11RenderTargetView *rtv = nat_scene_rtv(d);
		ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
		ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
		ID3D11DeviceContext_VSSetShader(ctx, nat_shader_object(vs), NULL, 0);
		ID3D11DeviceContext_PSSetShader(ctx, nat_shader_object(ps), NULL, 0);
		ID3D11ShaderResourceView *srv = nat_image_srv(im, 0);
		ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
		ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &smp);
		ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ID3D11DeviceContext_Draw(ctx, 3, 0);
		ID3D11Resource *res = NULL;
		ID3D11RenderTargetView_GetResource(rtv, &res);
		const uint32_t tl = read_pixel(d, res, 10, 10, 320, 200), tr = read_pixel(d, res, 310, 10, 320, 200), bl = read_pixel(d, res, 10, 190, 320, 200), br = read_pixel(d, res, 310, 190, 320, 200);
		CHECK(tl == 0xffff0000 && tr == 0xff00ff00 && bl == 0xff0000ff && br == 0xffffffff, "texture draw %08x %08x %08x %08x", tl, tr, bl, br);
		ID3D11Resource_Release(res);
		ID3D11SamplerState_Release(smp);
	}
	// an L8 image, a volume and a cube are creatable, an unknown format is refused
	{
		nat_image_desc l8 = {NAT_IMAGE_2D, 4, 4, 1, 1, 50, 0, 0};
		nat_image *a = nat_image_create(d, &l8);
		uint8_t lum[16];
		memset(lum, 0x80, sizeof(lum));
		CHECK(a && nat_image_upload(a, 0, 0, lum, 4, 0), "L8");
		nat_image_desc vol = {NAT_IMAGE_VOLUME, 4, 4, 4, 1, 21, 0, 0};
		nat_image *v = nat_image_create(d, &vol);
		static uint32_t vdata[64];
		CHECK(v && nat_image_upload(v, 0, 0, vdata, 16, 64), "volume");
		nat_image_desc cube = {NAT_IMAGE_CUBE, 8, 8, 1, 0, 0x31545844, 0, 0};
		nat_image *cu = nat_image_create(d, &cube);
		CHECK(cu && nat_image_levels(cu) == 4, "cube levels");
		static uint8_t blocks[32];
		CHECK(cu && nat_image_upload(cu, 5, 0, blocks, 16, 0), "cube upload");
		nat_image_desc bad = {NAT_IMAGE_2D, 4, 4, 1, 1, 12345, 0, 0};
		CHECK(nat_image_create(d, &bad) == NULL, "unknown format accepted");
		nat_image_destroy(a);
		nat_image_destroy(v);
		nat_image_destroy(cu);
	}
	// a dynamic buffer
	{
		nat_buffer *b = nat_buffer_create(d, 1024, 0, 1, NULL);
		void *m = b ? nat_buffer_map(b, NAT_MAP_DISCARD) : NULL;
		CHECK(m != NULL, "map");
		if (m) {
			memset(m, 1, 1024);
			nat_buffer_unmap(b);
		}
		nat_buffer_destroy(b);
	}
	// a pixel shader from Direct3D 9 shader model 3 bytecode: ps_3_0, mov oC0, c0
	{
		static const uint32_t ps3[] = {0xffff0300, 0x02000001, 0x800f0800, 0xa0e40000, 0x0000ffff};
		char e3[300] = "";
		nat_shader *s = nat_shader_from_sm3(d, ps3, sizeof(ps3) / 4, 0, NULL, 0, e3, sizeof(e3));
		CHECK(s != NULL, "sm3 shader: %s", e3);
		nat_shader_destroy(s);
	}
	// vid_restart: another size, then a multisampled scene
	p.width = 400;
	p.height = 240;
	CHECK(nat_reset(d, &p, err, sizeof(err)), "reset: %s", err);
	{
		unsigned w, h;
		nat_get_size(d, &w, &h);
		CHECK(w == 400 && h == 240, "size after reset %ux%u", w, h);
	}
	const float blue[4] = {0, 0, 1, 1}, green[4] = {0, 1, 0, 1};
	ID3D11DeviceContext_ClearRenderTargetView(ctx, nat_scene_rtv(d), blue);
	CHECK(nat_present(d) >= 0, "present after reset");
	p.msaa = 4;
	CHECK(nat_reset(d, &p, err, sizeof(err)), "reset msaa: %s", err);
	ID3D11DeviceContext_ClearRenderTargetView(ctx, nat_scene_rtv(d), green);
	nat_scene_resolve(d);
	CHECK(nat_present(d) >= 0, "present msaa");
	nat_image_destroy(im);
	nat_shader_destroy(vs);
	nat_shader_destroy(ps);
	nat_destroy(d);
	DestroyWindow(hwnd);
	printf(fails ? "%d checks FAILED\n" : "all checks passed\n", fails);
	return fails ? 1 : 0;
}
