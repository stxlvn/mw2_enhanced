// The native Direct3D 11 device layer (see nat11.h). No Direct3D 9 header, no Direct3D 9 object. Windows only (64 and 32 bit).
#include "nat11.h"

#ifdef _WIN32
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sm3.h"

#ifndef D3DCOMPILE_ENABLE_STRICTNESS
#define D3DCOMPILE_ENABLE_STRICTNESS (1 << 11)
#endif

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ the device
struct nat_device {
	ID3D11Device *dev;
	ID3D11DeviceContext *ctx;
	IDXGIFactory *factory;
	IDXGISwapChain *swap;
	HWND hwnd;
	nat_params p;
	nat_caps caps;
	unsigned samples;
	ID3D11Texture2D *scene, *resolved, *depth;
	ID3D11RenderTargetView *scene_rtv, *scene_rtv_srgb;
	ID3D11ShaderResourceView *scene_srv;
	ID3D11DepthStencilView *depth_dsv;
	ID3D11Texture2D *backbuffer;
	int fullscreen_set;
};

static HMODULE h_compiler;
static HRESULT(WINAPI *p_compile)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT, UINT, ID3DBlob **, ID3DBlob **);

static void set_err(char *err, size_t n, const char *fmt, unsigned long a)
{
	if (err && n)
		snprintf(err, n, fmt, a);
}

static void release_targets(nat_device *d)
{
#define REL(x) \
	do { \
		if (x) { \
			IUnknown_Release((IUnknown *)(x)); \
			(x) = NULL; \
		} \
	} while (0)
	REL(d->scene_rtv);
	REL(d->scene_rtv_srgb);
	REL(d->scene_srv);
	REL(d->depth_dsv);
	REL(d->scene);
	REL(d->resolved);
	REL(d->depth);
	REL(d->backbuffer);
}

static int build_targets(nat_device *d, char *err, size_t err_size)
{
	HRESULT hr = IDXGISwapChain_GetBuffer(d->swap, 0, &IID_ID3D11Texture2D, (void **)&d->backbuffer);
	if (FAILED(hr)) {
		set_err(err, err_size, "swap chain buffer: %lx", (unsigned long)hr);
		return 0;
	}
	D3D11_TEXTURE2D_DESC td = {0};
	td.Width = d->p.width;
	td.Height = d->p.height;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
	td.SampleDesc.Count = d->samples;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | (d->samples > 1 ? 0 : D3D11_BIND_SHADER_RESOURCE);
	hr = ID3D11Device_CreateTexture2D(d->dev, &td, NULL, &d->scene);
	if (FAILED(hr)) {
		set_err(err, err_size, "scene target: %lx", (unsigned long)hr);
		return 0;
	}
	D3D11_RENDER_TARGET_VIEW_DESC rd = {0};
	rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	rd.ViewDimension = d->samples > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
	ID3D11Device_CreateRenderTargetView(d->dev, (ID3D11Resource *)d->scene, &rd, &d->scene_rtv);
	rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	ID3D11Device_CreateRenderTargetView(d->dev, (ID3D11Resource *)d->scene, &rd, &d->scene_rtv_srgb);
	ID3D11Texture2D *srv_src = d->scene;
	if (d->samples > 1) {
		td.SampleDesc.Count = 1;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		hr = ID3D11Device_CreateTexture2D(d->dev, &td, NULL, &d->resolved);
		if (FAILED(hr)) {
			set_err(err, err_size, "resolve target: %lx", (unsigned long)hr);
			return 0;
		}
		srv_src = d->resolved;
	}
	D3D11_SHADER_RESOURCE_VIEW_DESC sd = {0};
	sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	sd.Texture2D.MipLevels = 1;
	ID3D11Device_CreateShaderResourceView(d->dev, (ID3D11Resource *)srv_src, &sd, &d->scene_srv);
	D3D11_TEXTURE2D_DESC dd = {0};
	dd.Width = d->p.width;
	dd.Height = d->p.height;
	dd.MipLevels = 1;
	dd.ArraySize = 1;
	dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	dd.SampleDesc.Count = d->samples;
	dd.Usage = D3D11_USAGE_DEFAULT;
	dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	hr = ID3D11Device_CreateTexture2D(d->dev, &dd, NULL, &d->depth);
	if (FAILED(hr)) {
		set_err(err, err_size, "depth target: %lx", (unsigned long)hr);
		return 0;
	}
	ID3D11Device_CreateDepthStencilView(d->dev, (ID3D11Resource *)d->depth, NULL, &d->depth_dsv);
	return d->scene_rtv && d->scene_srv && d->depth_dsv;
}

static unsigned pick_samples(nat_device *d, int wanted)
{
	for (unsigned s = wanted >= 8 ? 8 : wanted >= 4 ? 4 : wanted >= 2 ? 2 : 1; s > 1; s >>= 1) {
		UINT q = 0;
		if (SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(d->dev, DXGI_FORMAT_B8G8R8A8_UNORM, s, &q)) && q > 0)
			return s;
	}
	return 1;
}

static void fill_caps(nat_device *d)
{
	nat_caps *c = &d->caps;
	memset(c, 0, sizeof(*c));
	c->dynamic_textures = 1;
	c->aniso_min_mag = 1;
	c->max_aniso = 16;
	c->max_texture = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
	c->max_units = 16;
	c->clip_planes = 6;
	c->slope_scale_bias = 1;
	c->mip_cube = 1;
	c->hw_shadow_depth = 1;
	c->msaa_max = (int)pick_samples(d, 8);
	c->desktop_w = (unsigned)GetSystemMetrics(SM_CXSCREEN);
	c->desktop_h = (unsigned)GetSystemMetrics(SM_CYSCREEN);
	IDXGIAdapter *ad = NULL;
	if (SUCCEEDED(IDXGIFactory_EnumAdapters(d->factory, 0, &ad)) && ad) {
		DXGI_ADAPTER_DESC desc;
		if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &desc)))
			WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, c->adapter, sizeof(c->adapter), NULL, NULL);
		if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &desc)))
			c->vram_mb = (unsigned)(desc.DedicatedVideoMemory >> 20);
		IDXGIOutput *out = NULL;
		if (SUCCEEDED(IDXGIAdapter_EnumOutputs(ad, 0, &out)) && out) {
			DXGI_OUTPUT_DESC od;
			if (SUCCEEDED(IDXGIOutput_GetDesc(out, &od))) {
				c->desktop_w = (unsigned)(od.DesktopCoordinates.right - od.DesktopCoordinates.left);
				c->desktop_h = (unsigned)(od.DesktopCoordinates.bottom - od.DesktopCoordinates.top);
			}
			IDXGIOutput_Release(out);
		}
		IDXGIAdapter_Release(ad);
	}
}

static int load_compiler(void)
{
	if (p_compile)
		return 1;
	h_compiler = LoadLibraryA("d3dcompiler_47.dll");
	if (h_compiler)
		p_compile = (void *)GetProcAddress(h_compiler, "D3DCompile");
	return p_compile != NULL;
}

int nat_create(const nat_params *p, nat_device **out, char *err, size_t err_size)
{
	*out = NULL;
	nat_device *d = calloc(1, sizeof(*d));
	d->p = *p;
	d->hwnd = (HWND)p->hwnd;
	if (!d->p.width || !d->p.height) {
		set_err(err, err_size, "empty size %lu", 0);
		free(d);
		return 0;
	}
	const D3D_FEATURE_LEVEL lv[] = {D3D_FEATURE_LEVEL_11_0};
	HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, lv, 1, D3D11_SDK_VERSION, &d->dev, NULL, &d->ctx);
	if (FAILED(hr) && getenv("MW2COOP_NAT_WARP")) // tests on machines without a usable adapter
		hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, lv, 1, D3D11_SDK_VERSION, &d->dev, NULL, &d->ctx);
	if (FAILED(hr)) {
		set_err(err, err_size, "D3D11CreateDevice: %lx", (unsigned long)hr);
		free(d);
		return 0;
	}
	// the game calls the device from its loader thread too (it asked for D3DCREATE_MULTITHREADED): the immediate context is serialised by the runtime
	ID3D10Multithread *mt = NULL;
	if (SUCCEEDED(ID3D11DeviceContext_QueryInterface(d->ctx, &IID_ID3D10Multithread, (void **)&mt)) && mt) {
		ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
		ID3D10Multithread_Release(mt);
	}
	IDXGIDevice *dxdev = NULL;
	IDXGIAdapter *ad = NULL;
	if (FAILED(ID3D11Device_QueryInterface(d->dev, &IID_IDXGIDevice, (void **)&dxdev)) || FAILED(IDXGIDevice_GetAdapter(dxdev, &ad)) || FAILED(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory, (void **)&d->factory))) {
		set_err(err, err_size, "DXGI factory %lu", 0);
		if (ad)
			IDXGIAdapter_Release(ad);
		if (dxdev)
			IDXGIDevice_Release(dxdev);
		nat_destroy(d);
		return 0;
	}
	IDXGIAdapter_Release(ad);
	IDXGIDevice_Release(dxdev);
	d->samples = pick_samples(d, p->msaa);
	DXGI_SWAP_CHAIN_DESC sc = {0};
	sc.BufferDesc.Width = d->p.width;
	sc.BufferDesc.Height = d->p.height;
	sc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sc.SampleDesc.Count = 1;
	sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sc.BufferCount = 1;
	sc.OutputWindow = d->hwnd;
	sc.Windowed = TRUE;
	sc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	sc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	hr = IDXGIFactory_CreateSwapChain(d->factory, (IUnknown *)d->dev, &sc, &d->swap);
	if (FAILED(hr)) {
		set_err(err, err_size, "CreateSwapChain: %lx", (unsigned long)hr);
		nat_destroy(d);
		return 0;
	}
	IDXGIFactory_MakeWindowAssociation(d->factory, d->hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER); // the game handles Alt+Enter itself
	if (!build_targets(d, err, err_size)) {
		nat_destroy(d);
		return 0;
	}
	fill_caps(d);
	if (d->p.fullscreen) {
		DXGI_MODE_DESC m = sc.BufferDesc;
		m.RefreshRate.Numerator = d->p.refresh;
		m.RefreshRate.Denominator = d->p.refresh ? 1 : 0;
		IDXGISwapChain_ResizeTarget(d->swap, &m);
		if (SUCCEEDED(IDXGISwapChain_SetFullscreenState(d->swap, TRUE, NULL)))
			d->fullscreen_set = 1;
	}
	*out = d;
	return 1;
}

void nat_destroy(nat_device *d)
{
	if (!d)
		return;
	if (d->swap && d->fullscreen_set)
		IDXGISwapChain_SetFullscreenState(d->swap, FALSE, NULL);
	if (d->ctx)
		ID3D11DeviceContext_ClearState(d->ctx);
	release_targets(d);
	REL(d->swap);
	REL(d->factory);
	REL(d->ctx);
	REL(d->dev);
	free(d);
}

const nat_caps *nat_get_caps(const nat_device *d)
{
	return &d->caps;
}

int nat_reset(nat_device *d, const nat_params *p, char *err, size_t err_size)
{
	ID3D11DeviceContext_ClearState(d->ctx);
	release_targets(d);
	d->p = *p;
	d->samples = pick_samples(d, p->msaa);
	if (d->fullscreen_set && !p->fullscreen) {
		IDXGISwapChain_SetFullscreenState(d->swap, FALSE, NULL);
		d->fullscreen_set = 0;
	}
	DXGI_MODE_DESC m = {0};
	m.Width = p->width;
	m.Height = p->height;
	m.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	IDXGISwapChain_ResizeTarget(d->swap, &m);
	if (p->fullscreen && !d->fullscreen_set && SUCCEEDED(IDXGISwapChain_SetFullscreenState(d->swap, TRUE, NULL)))
		d->fullscreen_set = 1;
	HRESULT hr = IDXGISwapChain_ResizeBuffers(d->swap, 0, p->width, p->height, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
	if (FAILED(hr)) {
		set_err(err, err_size, "ResizeBuffers: %lx", (unsigned long)hr);
		return 0;
	}
	return build_targets(d, err, err_size);
}

void nat_scene_resolve(nat_device *d)
{
	if (d->samples > 1)
		ID3D11DeviceContext_ResolveSubresource(d->ctx, (ID3D11Resource *)d->resolved, 0, (ID3D11Resource *)d->scene, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
}

int nat_present(nat_device *d)
{
	if (d->samples > 1)
		ID3D11DeviceContext_ResolveSubresource(d->ctx, (ID3D11Resource *)d->backbuffer, 0, (ID3D11Resource *)d->scene, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
	else
		ID3D11DeviceContext_CopyResource(d->ctx, (ID3D11Resource *)d->backbuffer, (ID3D11Resource *)d->scene);
	HRESULT hr = IDXGISwapChain_Present(d->swap, d->p.vsync ? 1 : 0, 0);
	if (hr == DXGI_STATUS_OCCLUDED)
		return 1;
	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
		return -1;
	return FAILED(hr) ? -1 : 0;
}

void nat_get_size(const nat_device *d, unsigned *w, unsigned *h)
{
	*w = d->p.width;
	*h = d->p.height;
}
void *nat_d3d_device(nat_device *d) { return d->dev; }
void *nat_d3d_context(nat_device *d) { return d->ctx; }
void *nat_scene_rtv(nat_device *d) { return d->scene_rtv; }
void *nat_scene_rtv_srgb(nat_device *d) { return d->scene_rtv_srgb; }
void *nat_scene_dsv(nat_device *d) { return d->depth_dsv; }
void *nat_scene_srv(nat_device *d) { return d->scene_srv; }

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ images
typedef struct {
	DXGI_FORMAT dxgi, srv, srgb;
	int expand; // 1: L8, 2: A8L8, widened to 8888
	int block; // bytes per 4x4 block, 0 = not compressed
	int bpp;
	int depth;
} fmt_t;

static int fmt_info(unsigned f, fmt_t *o)
{
	memset(o, 0, sizeof(*o));
	switch (f) {
	case 21: o->dxgi = DXGI_FORMAT_B8G8R8A8_TYPELESS; o->srv = DXGI_FORMAT_B8G8R8A8_UNORM; o->srgb = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; o->bpp = 4; return 1;
	case 22: o->dxgi = DXGI_FORMAT_B8G8R8X8_TYPELESS; o->srv = DXGI_FORMAT_B8G8R8X8_UNORM; o->srgb = DXGI_FORMAT_B8G8R8X8_UNORM_SRGB; o->bpp = 4; return 1;
	case 0x31545844: o->dxgi = DXGI_FORMAT_BC1_TYPELESS; o->srv = DXGI_FORMAT_BC1_UNORM; o->srgb = DXGI_FORMAT_BC1_UNORM_SRGB; o->block = 8; return 1;
	case 0x33545844: o->dxgi = DXGI_FORMAT_BC2_TYPELESS; o->srv = DXGI_FORMAT_BC2_UNORM; o->srgb = DXGI_FORMAT_BC2_UNORM_SRGB; o->block = 16; return 1;
	case 0x35545844: o->dxgi = DXGI_FORMAT_BC3_TYPELESS; o->srv = DXGI_FORMAT_BC3_UNORM; o->srgb = DXGI_FORMAT_BC3_UNORM_SRGB; o->block = 16; return 1;
	case 28: o->dxgi = o->srv = DXGI_FORMAT_A8_UNORM; o->bpp = 1; return 1;
	case 50: o->dxgi = DXGI_FORMAT_B8G8R8A8_TYPELESS; o->srv = DXGI_FORMAT_B8G8R8A8_UNORM; o->expand = 1; o->bpp = 1; return 1;
	case 51: o->dxgi = DXGI_FORMAT_B8G8R8A8_TYPELESS; o->srv = DXGI_FORMAT_B8G8R8A8_UNORM; o->expand = 2; o->bpp = 2; return 1;
	case 113: o->dxgi = o->srv = DXGI_FORMAT_R16G16B16A16_FLOAT; o->bpp = 8; return 1;
	case 116: o->dxgi = o->srv = DXGI_FORMAT_R32G32B32A32_FLOAT; o->bpp = 16; return 1;
	case 111: o->dxgi = o->srv = DXGI_FORMAT_R16_FLOAT; o->bpp = 2; return 1;
	case 112: o->dxgi = o->srv = DXGI_FORMAT_R16G16_FLOAT; o->bpp = 4; return 1;
	case 114: o->dxgi = o->srv = DXGI_FORMAT_R32_FLOAT; o->bpp = 4; return 1;
	case 115: o->dxgi = o->srv = DXGI_FORMAT_R32G32_FLOAT; o->bpp = 8; return 1;
	case 34: o->dxgi = o->srv = DXGI_FORMAT_R16G16_UNORM; o->bpp = 4; return 1;
	case 36: o->dxgi = o->srv = DXGI_FORMAT_R16G16B16A16_UNORM; o->bpp = 8; return 1;
	case 31: o->dxgi = o->srv = DXGI_FORMAT_R10G10B10A2_UNORM; o->bpp = 4; return 1;
	case 0x5a544e49: case 75: case 77: o->dxgi = DXGI_FORMAT_R24G8_TYPELESS; o->srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; o->bpp = 4; o->depth = 1; return 1; // INTZ, D24S8, D24X8
	case 80: case 70: o->dxgi = DXGI_FORMAT_R16_TYPELESS; o->srv = DXGI_FORMAT_R16_UNORM; o->bpp = 2; o->depth = 1; return 1;
	case 0x4c4c554e: o->dxgi = DXGI_FORMAT_B8G8R8A8_TYPELESS; o->srv = DXGI_FORMAT_B8G8R8A8_UNORM; o->bpp = 4; return 1; // "NULL": the depth-only render target trick, a plain target here
	}
	return 0;
}

struct nat_image {
	nat_device *dev;
	nat_image_desc desc;
	fmt_t f;
	ID3D11Resource *res;
	ID3D11ShaderResourceView *srv, *srv_srgb;
	ID3D11RenderTargetView *rtv, *rtv_srgb;
	ID3D11DepthStencilView *dsv;
	unsigned levels;
};

static unsigned mips_of(unsigned w, unsigned h, unsigned d)
{
	unsigned n = 1;
	while (w > 1 || h > 1 || d > 1) {
		w = w > 1 ? w >> 1 : 1;
		h = h > 1 ? h >> 1 : 1;
		d = d > 1 ? d >> 1 : 1;
		n++;
	}
	return n;
}

nat_image *nat_image_create(nat_device *d, const nat_image_desc *desc)
{
	fmt_t f;
	if (!fmt_info(desc->d3d9_format, &f))
		return NULL;
	nat_image *im = calloc(1, sizeof(*im));
	im->dev = d;
	im->desc = *desc;
	im->f = f;
	const int rt = (desc->flags & NAT_RENDER_TARGET) != 0, ds = (desc->flags & NAT_DEPTH) != 0 && desc->kind == NAT_IMAGE_2D;
	im->levels = desc->levels ? desc->levels : mips_of(desc->w, desc->h, desc->kind == NAT_IMAGE_VOLUME ? desc->d : 1);
	const unsigned samples = rt || ds ? (unsigned)(desc->msaa > 1 ? desc->msaa : 1) : 1;
	HRESULT hr;
	if (desc->kind == NAT_IMAGE_VOLUME) {
		D3D11_TEXTURE3D_DESC vd = {0};
		vd.Width = desc->w;
		vd.Height = desc->h;
		vd.Depth = desc->d ? desc->d : 1;
		vd.MipLevels = im->levels;
		vd.Format = f.dxgi;
		vd.Usage = D3D11_USAGE_DEFAULT;
		vd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		hr = ID3D11Device_CreateTexture3D(d->dev, &vd, NULL, (ID3D11Texture3D **)&im->res);
	} else {
		D3D11_TEXTURE2D_DESC td = {0};
		td.Width = desc->w;
		td.Height = desc->h;
		td.MipLevels = im->levels;
		td.ArraySize = desc->kind == NAT_IMAGE_CUBE ? 6 : 1;
		td.Format = f.dxgi;
		td.SampleDesc.Count = samples;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = samples > 1 ? 0 : D3D11_BIND_SHADER_RESOURCE;
		if (desc->kind == NAT_IMAGE_CUBE)
			td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
		if (rt)
			td.BindFlags |= D3D11_BIND_RENDER_TARGET;
		if (ds)
			td.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
		hr = ID3D11Device_CreateTexture2D(d->dev, &td, NULL, (ID3D11Texture2D **)&im->res);
	}
	if (FAILED(hr)) {
		free(im);
		return NULL;
	}
	if (samples == 1) {
		D3D11_SHADER_RESOURCE_VIEW_DESC sd = {0};
		sd.Format = f.srv;
		if (desc->kind == NAT_IMAGE_CUBE) {
			sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
			sd.TextureCube.MipLevels = im->levels;
		} else if (desc->kind == NAT_IMAGE_VOLUME) {
			sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
			sd.Texture3D.MipLevels = im->levels;
		} else {
			sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			sd.Texture2D.MipLevels = im->levels;
		}
		ID3D11Device_CreateShaderResourceView(d->dev, im->res, &sd, &im->srv);
		if (f.srgb) {
			sd.Format = f.srgb;
			ID3D11Device_CreateShaderResourceView(d->dev, im->res, &sd, &im->srv_srgb);
		}
	}
	if (rt) {
		D3D11_RENDER_TARGET_VIEW_DESC rd = {0};
		rd.ViewDimension = samples > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
		rd.Format = f.srv;
		ID3D11Device_CreateRenderTargetView(d->dev, im->res, &rd, &im->rtv);
		if (f.srgb) {
			rd.Format = f.srgb;
			ID3D11Device_CreateRenderTargetView(d->dev, im->res, &rd, &im->rtv_srgb);
		}
	}
	if (ds) {
		D3D11_DEPTH_STENCIL_VIEW_DESC dd = {0};
		dd.ViewDimension = samples > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
		dd.Format = f.dxgi == DXGI_FORMAT_R16_TYPELESS ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D24_UNORM_S8_UINT;
		ID3D11Device_CreateDepthStencilView(d->dev, im->res, &dd, &im->dsv);
	}
	return im;
}

void nat_image_destroy(nat_image *i)
{
	if (!i)
		return;
	REL(i->srv);
	REL(i->srv_srgb);
	REL(i->rtv);
	REL(i->rtv_srgb);
	REL(i->dsv);
	REL(i->res);
	free(i);
}

int nat_image_upload(nat_image *i, unsigned face, unsigned level, const void *data, unsigned row_pitch, unsigned slice_pitch)
{
	if (!i || level >= i->levels || (i->desc.kind == NAT_IMAGE_CUBE && face > 5))
		return 0;
	const unsigned w = i->desc.w >> level ? i->desc.w >> level : 1, h = i->desc.h >> level ? i->desc.h >> level : 1;
	unsigned dl = i->desc.kind == NAT_IMAGE_VOLUME ? (i->desc.d >> level ? i->desc.d >> level : 1) : 1;
	const UINT sub = (UINT)(i->desc.kind == NAT_IMAGE_CUBE ? face * i->levels + level : level);
	ID3D11DeviceContext *ctx = i->dev->ctx;
	if (i->f.expand) {
		uint32_t *tmp = malloc((size_t)w * h * dl * 4);
		const uint8_t *src = data;
		const unsigned unit = i->f.expand == 1 ? 1 : 2;
		for (unsigned z = 0; z < dl; z++)
			for (unsigned y = 0; y < h; y++)
				for (unsigned x = 0; x < w; x++) {
					const uint8_t *s = src + (size_t)z * slice_pitch + (size_t)y * row_pitch + (size_t)x * unit;
					const uint32_t lum = s[0], a = unit == 1 ? 255 : s[1];
					tmp[((size_t)z * h + y) * w + x] = a << 24 | lum << 16 | lum << 8 | lum;
				}
		ID3D11DeviceContext_UpdateSubresource(ctx, i->res, sub, NULL, tmp, w * 4, w * h * 4);
		free(tmp);
		return 1;
	}
	ID3D11DeviceContext_UpdateSubresource(ctx, i->res, sub, NULL, data, row_pitch, slice_pitch);
	return 1;
}

void *nat_image_srv(nat_image *i, int srgb) { return srgb && i->srv_srgb ? i->srv_srgb : i->srv; }
void *nat_image_rtv(nat_image *i, int srgb) { return srgb && i->rtv_srgb ? i->rtv_srgb : i->rtv; }
void *nat_image_dsv(nat_image *i) { return i->dsv; }
void *nat_image_resource(nat_image *i) { return i->res; }
void nat_image_size(const nat_image *i, unsigned *w, unsigned *h, unsigned *samples)
{
	*w = i->desc.w;
	*h = i->desc.h;
	*samples = i->desc.msaa > 1 ? (unsigned)i->desc.msaa : 1;
}
unsigned nat_image_dxgi_format(const nat_image *i) { return (unsigned)i->f.dxgi; }
void *nat_scene_resource(nat_device *d) { return d->scene; }
unsigned nat_scene_samples(const nat_device *d) { return d->samples; }
unsigned nat_image_levels(const nat_image *i) { return i->levels; }

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ buffers
// The first member is the vtable pointer of a Direct3D 9 vertex / index buffer: the engine's loader creates static buffers through 0x24d00 / 0x24df0 and then writes and releases them
// through the stored pointer with the Direct3D 9 calls (Lock +0x58, Unlock +0x60, Release +0x10); only those slots are real, the others do nothing.
struct nat_buffer {
	void **vt;
	nat_device *dev;
	ID3D11Buffer *buf;
	int dynamic;
	int is_index;
	unsigned size;
	uint8_t *cpu; // a static buffer being written (Lock .. Unlock): the block, uploaded and freed at Unlock
	uint8_t *stage;        // a staged buffer: the memory the engine writes through Lock for the whole frame (its worker threads fill it), copied to the GPU buffer by nat_buffer_sync
	unsigned synced;       // the bytes of the stage already copied
	int mapped;            // a dynamic buffer locked through the vtable (Map until Unlock)
	int discarded;         // a dynamic buffer that has had its first DISCARD (a no-overwrite map is only valid after one)
	const volatile int *used; // the engine's own count of the bytes it has reserved in the stage (the ring struct's first field)
};

static long buffer_nop(void *self)
{
	(void)self;
	return 0;
}
static long buffer_addref(void *self)
{
	(void)self;
	return 1;
}
static long buffer_release(void *self);
static long buffer_lock(void *self, unsigned off, unsigned size, void **out, unsigned flags);
static long buffer_unlock(void *self);
static void *buffer_vt[32];
static void buffer_vt_init(void)
{
	if (buffer_vt[0])
		return;
	for (int i = 0; i < 32; i++)
		buffer_vt[i] = (void *)buffer_nop;
	buffer_vt[1] = (void *)buffer_addref;
	buffer_vt[2] = (void *)buffer_release; // +0x10
	buffer_vt[11] = (void *)buffer_lock;   // +0x58
	buffer_vt[12] = (void *)buffer_unlock; // +0x60
}

nat_buffer *nat_buffer_create(nat_device *d, unsigned size, int is_index, int dynamic, const void *initial)
{
	D3D11_BUFFER_DESC bd = {0};
	bd.ByteWidth = size;
	bd.BindFlags = is_index ? D3D11_BIND_INDEX_BUFFER : D3D11_BIND_VERTEX_BUFFER;
	if (dynamic) {
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	} else {
		bd.Usage = D3D11_USAGE_DEFAULT;
	}
	D3D11_SUBRESOURCE_DATA sd = {initial, 0, 0};
	nat_buffer *b = calloc(1, sizeof(*b));
	buffer_vt_init();
	b->vt = buffer_vt;
	b->dev = d;
	b->dynamic = dynamic;
	b->is_index = is_index;
	b->size = size;
	if (FAILED(ID3D11Device_CreateBuffer(d->dev, &bd, initial ? &sd : NULL, &b->buf))) {
		free(b);
		return NULL;
	}
	return b;
}

void nat_buffer_destroy(nat_buffer *b)
{
	if (!b)
		return;
	REL(b->buf);
	free(b->cpu);
	free(b->stage);
	free(b);
}

// A vertex buffer the engine locks for a whole frame and writes from several threads (the skinned-vertex rings): a GPU buffer plus a stage of the same size. Lock hands out the stage;
// `used` is the engine's byte counter for it; nat_buffer_sync copies what is reserved so far to the GPU buffer, and is called before a draw reads the buffer.
nat_buffer *nat_buffer_create_staged(nat_device *d, unsigned size, const volatile int *used)
{
	nat_buffer *b = nat_buffer_create(d, size, 0, 0, NULL);
	if (!b)
		return NULL;
	b->stage = calloc(1, size);
	if (!b->stage) {
		nat_buffer_destroy(b);
		return NULL;
	}
	b->used = used;
	return b;
}

int nat_buffer_is_staged(const nat_buffer *b) { return b && b->stage != NULL; }

void nat_buffer_sync(nat_buffer *b)
{
	if (!b || !b->stage || !b->used)
		return;
	int u = *b->used;
	if (u < 0)
		return;
	if ((unsigned)u > b->size)
		u = (int)b->size;
	if ((unsigned)u <= b->synced)
		return;
	D3D11_BOX box = {b->synced, 0, 0, (UINT)u, 1, 1};
	ID3D11DeviceContext_UpdateSubresource(b->dev->ctx, (ID3D11Resource *)b->buf, 0, &box, b->stage + b->synced, 0, 0);
	b->synced = (unsigned)u;
}

// A static buffer the engine writes after creating it (0x24d00 / 0x24df0): no Direct3D 11 buffer exists until Unlock, which makes it from the written block. Returns the block to write.
nat_buffer *nat_buffer_create_static(nat_device *d, unsigned size, int is_index, void **block)
{
	nat_buffer *b = calloc(1, sizeof(*b));
	buffer_vt_init();
	b->vt = buffer_vt;
	b->dev = d;
	b->is_index = is_index;
	b->size = size;
	b->cpu = calloc(1, size ? size : 1);
	if (!b->cpu) {
		free(b);
		return NULL;
	}
	*block = b->cpu;
	return b;
}

static long buffer_release(void *self)
{
	nat_buffer_destroy((nat_buffer *)self);
	return 0;
}

void (*nat_log_hook)(const char *fmt, ...); // diagnostics (set by natboot.c)
static long buffer_lock(void *self, unsigned off, unsigned size, void **out, unsigned flags)
{
	nat_buffer *b = (nat_buffer *)self;
	{
		static int n_logged;
		if (nat_log_hook && n_logged < 24 && (b->dynamic || b->stage)) {
			n_logged++;
			nat_log_hook("native11: Lock buffer %p size %u dynamic %d staged %d, off %u size %u flags %x", (void *)b, b->size, b->dynamic, b->stage != NULL, off, size, flags);
		}
	}
	(void)size;
	(void)flags;
	if (b->stage) { // a new frame (Lock with DISCARD): the whole stage is free again
		if (off > b->size)
			return -1;
		b->synced = 0;
		*out = b->stage + off;
		return 0;
	}
	if (b->dynamic) { // a dynamic buffer locked by the engine's front end (the frame's vertex buffer): Map until the engine's Unlock; D3DLOCK_NOOVERWRITE = 0x1000
		D3D11_MAPPED_SUBRESOURCE m;
		if (off > b->size || b->mapped)
			return -1;
		const int no_overwrite = (flags & 0x1000) && b->discarded;
		if (FAILED(ID3D11DeviceContext_Map(b->dev->ctx, (ID3D11Resource *)b->buf, 0, no_overwrite ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, 0, &m)))
			return -1;
		b->mapped = 1;
		b->discarded = 1;
		*out = (uint8_t *)m.pData + off;
		return 0;
	}
	if (off > b->size)
		return -1;
	if (!b->cpu) // a second Lock of a made buffer: a fresh block (the first contents are not read back)
		b->cpu = calloc(1, b->size ? b->size : 1);
	if (!b->cpu)
		return -1;
	*out = b->cpu + off;
	return 0;
}

static long buffer_unlock(void *self)
{
	nat_buffer *b = (nat_buffer *)self;
	{
		static int n_logged;
		if (nat_log_hook && n_logged < 12 && (b->dynamic || b->stage)) {
			n_logged++;
			nat_log_hook("native11: Unlock buffer %p (mapped %d)", (void *)b, b->mapped);
		}
	}
	if (b->dynamic) {
		if (b->mapped) {
			ID3D11DeviceContext_Unmap(b->dev->ctx, (ID3D11Resource *)b->buf, 0);
			b->mapped = 0;
		}
		return 0;
	}
	if (!b->cpu)
		return 0;
	D3D11_BUFFER_DESC bd = {0};
	bd.ByteWidth = b->size;
	bd.BindFlags = b->is_index ? D3D11_BIND_INDEX_BUFFER : D3D11_BIND_VERTEX_BUFFER;
	bd.Usage = D3D11_USAGE_DEFAULT;
	D3D11_SUBRESOURCE_DATA sd = {b->cpu, 0, 0};
	ID3D11Buffer *nb = NULL;
	if (SUCCEEDED(ID3D11Device_CreateBuffer(b->dev->dev, &bd, &sd, &nb))) {
		REL(b->buf);
		b->buf = nb;
	}
	free(b->cpu);
	b->cpu = NULL;
	return 0;
}

void *nat_buffer_map(nat_buffer *b, int mode)
{
	D3D11_MAPPED_SUBRESOURCE m;
	if (!b->dynamic || FAILED(ID3D11DeviceContext_Map(b->dev->ctx, (ID3D11Resource *)b->buf, 0, (mode == NAT_MAP_NOOVERWRITE && b->discarded) ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, 0, &m)))
		return NULL;
	b->discarded = 1;
	return m.pData;
}

void nat_buffer_unmap(nat_buffer *b)
{
	ID3D11DeviceContext_Unmap(b->dev->ctx, (ID3D11Resource *)b->buf, 0);
}

void *nat_buffer_resource(nat_buffer *b) { return b->buf; }

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ shaders
struct nat_shader {
	void *obj;
	ID3DBlob *blob;
	int is_vs;
};

static nat_shader *shader_from_hlsl(nat_device *d, const char *hlsl, int is_vs, char *err, size_t err_size)
{
	if (!load_compiler()) {
		set_err(err, err_size, "d3dcompiler_47.dll %lu", 0);
		return NULL;
	}
	ID3DBlob *code = NULL, *msgs = NULL;
	HRESULT hr = p_compile(hlsl, strlen(hlsl), "nat", NULL, NULL, "main", is_vs ? "vs_5_0" : "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS | (1 << 14), 0, &code, &msgs);
	if (FAILED(hr) || !code) {
		if (err && err_size)
			snprintf(err, err_size, "compile: %lx %s", (unsigned long)hr, msgs ? (const char *)ID3D10Blob_GetBufferPointer(msgs) : "");
		if (msgs)
			ID3D10Blob_Release(msgs);
		return NULL;
	}
	nat_shader *s = calloc(1, sizeof(*s));
	s->is_vs = is_vs;
	s->blob = code;
	hr = is_vs ? ID3D11Device_CreateVertexShader(d->dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, (ID3D11VertexShader **)&s->obj)
			   : ID3D11Device_CreatePixelShader(d->dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, (ID3D11PixelShader **)&s->obj);
	if (FAILED(hr)) {
		set_err(err, err_size, "create shader: %lx", (unsigned long)hr);
		ID3D10Blob_Release(code);
		free(s);
		return NULL;
	}
	return s;
}

nat_shader *nat_shader_from_hlsl(nat_device *d, const char *hlsl, int is_vs, char *err, size_t err_size)
{
	return shader_from_hlsl(d, hlsl, is_vs, err, err_size);
}

nat_shader *nat_shader_from_sm3(nat_device *d, const uint32_t *code, size_t dwords, int alpha_test, const void *int_inputs, int nint_inputs, char *err, size_t err_size)
{
	char *hlsl = NULL, why[160] = "";
	int is_vs = 0;
	if (!sm3_translate_vs(code, dwords, alpha_test ? SM3_ALPHA_TEST : 0, (const sm3_int_input *)int_inputs, nint_inputs, &hlsl, &is_vs, why, sizeof(why))) {
		if (err && err_size)
			snprintf(err, err_size, "translate: %s", why);
		return NULL;
	}
	nat_shader *s = shader_from_hlsl(d, hlsl, is_vs, err, err_size);
	free(hlsl);
	return s;
}

void nat_shader_destroy(nat_shader *s)
{
	if (!s)
		return;
	REL(s->obj);
	if (s->blob)
		ID3D10Blob_Release(s->blob);
	free(s);
}

void *nat_shader_object(nat_shader *s) { return s->obj; }
void *nat_shader_blob(nat_shader *s, size_t *size)
{
	if (size)
		*size = ID3D10Blob_GetBufferSize(s->blob);
	return ID3D10Blob_GetBufferPointer(s->blob);
}
int nat_shader_is_vs(const nat_shader *s) { return s->is_vs; }

#else
int nat_create(const nat_params *p, nat_device **out, char *err, size_t err_size)
{
	(void)p;
	(void)err;
	(void)err_size;
	*out = NULL;
	return 0;
}
#endif

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ queries and gamma
#ifdef _WIN32
struct nat_query {
	void **vt; // a Direct3D 9 query vtable first (see query_vt)
	nat_device *dev;
	ID3D11Query *q;
	int occlusion;
};

// A nat_query starts with the vtable pointer of a Direct3D 9 query, because the engine's loops that were not rewritten call GetData (+0x38) and Issue (+0x30) on the stored pointer.
void nat_query_destroy(nat_query *q);
void nat_query_end(nat_query *q);
int nat_query_get(nat_query *q, unsigned *value);
static void *query_vt[16];
static long query_nop(void *self)
{
	(void)self;
	return 0;
}
static long query_release(void *self)
{
	nat_query_destroy((nat_query *)self);
	return 0;
}
static long query_issue(void *self, unsigned flags)
{
	nat_query *q = (nat_query *)self;
	if (flags & 2) { // D3DISSUE_BEGIN (occlusion queries only)
		if (q->occlusion)
			ID3D11DeviceContext_Begin(q->dev->ctx, (ID3D11Asynchronous *)q->q);
	} else {
		nat_query_end(q);
	}
	return 0;
}
static long query_getdata(void *self, void *data, unsigned size, unsigned flags)
{
	(void)flags;
	nat_query *q = (nat_query *)self;
	unsigned v = 0;
	const int r = nat_query_get(q, &v);
	if (r == 1)
		return 1; // S_FALSE: not ready yet
	if (r < 0)
		return (long)0x88760868; // D3DERR_DEVICELOST
	if (data && size >= 4)
		*(unsigned *)data = v;
	return 0;
}
static void query_vt_init(void)
{
	if (query_vt[0])
		return;
	for (int i = 0; i < 16; i++)
		query_vt[i] = (void *)query_nop;
	query_vt[1] = (void *)query_nop;
	query_vt[2] = (void *)query_release; // +0x10
	query_vt[6] = (void *)query_issue;   // +0x30
	query_vt[7] = (void *)query_getdata; // +0x38
}

nat_query *nat_query_create(nat_device *d, int occlusion)
{
	D3D11_QUERY_DESC qd = {occlusion ? D3D11_QUERY_OCCLUSION : D3D11_QUERY_EVENT, 0};
	nat_query *q = calloc(1, sizeof(*q));
	query_vt_init();
	q->vt = query_vt;
	q->dev = d;
	q->occlusion = occlusion;
	if (FAILED(ID3D11Device_CreateQuery(d->dev, &qd, &q->q))) {
		free(q);
		return NULL;
	}
	return q;
}

void nat_query_destroy(nat_query *q)
{
	if (!q)
		return;
	REL(q->q);
	free(q);
}

void nat_query_end(nat_query *q)
{
	ID3D11DeviceContext_End(q->dev->ctx, (ID3D11Asynchronous *)q->q);
}

int nat_query_get(nat_query *q, unsigned *value)
{
	UINT64 v64 = 0;
	BOOL b = FALSE;
	HRESULT hr = q->occlusion ? ID3D11DeviceContext_GetData(q->dev->ctx, (ID3D11Asynchronous *)q->q, &v64, sizeof(v64), 0) : ID3D11DeviceContext_GetData(q->dev->ctx, (ID3D11Asynchronous *)q->q, &b, sizeof(b), 0);
	if (hr == S_OK) {
		if (value)
			*value = q->occlusion ? (unsigned)(v64 > 0xffffffffu ? 0xffffffffu : v64) : (b ? 1u : 0u);
		return 0;
	}
	if (hr == S_FALSE)
		return 1;
	return -1;
}

int nat_set_gamma(nat_device *d, const uint16_t ramp[3][256])
{
	if (!d->fullscreen_set)
		return 0;
	IDXGIOutput *out = NULL;
	if (FAILED(IDXGISwapChain_GetContainingOutput(d->swap, &out)) || !out)
		return 0;
	DXGI_GAMMA_CONTROL gc;
	memset(&gc, 0, sizeof(gc));
	gc.Scale.Red = gc.Scale.Green = gc.Scale.Blue = 1.0f;
	for (int i = 0; i < 1025; i++) {
		const int j = i * 255 / 1024;
		gc.GammaCurve[i].Red = ramp[0][j] / 65535.0f;
		gc.GammaCurve[i].Green = ramp[1][j] / 65535.0f;
		gc.GammaCurve[i].Blue = ramp[2][j] / 65535.0f;
	}
	const HRESULT hr = IDXGIOutput_SetGammaControl(out, &gc);
	IDXGIOutput_Release(out);
	return SUCCEEDED(hr);
}
#endif

#ifdef _WIN32
int nat_list_modes(int output, nat_mode *out, int max)
{
	IDXGIFactory *fac = NULL;
	IDXGIAdapter *ad = NULL;
	IDXGIOutput *o = NULL;
	int n = 0;
	if (FAILED(CreateDXGIFactory(&IID_IDXGIFactory, (void **)&fac)) || FAILED(IDXGIFactory_EnumAdapters(fac, 0, &ad)) || FAILED(IDXGIAdapter_EnumOutputs(ad, (UINT)output, &o)))
		goto done;
	{
		UINT count = 0;
		IDXGIOutput_GetDisplayModeList(o, DXGI_FORMAT_B8G8R8A8_UNORM, 0, &count, NULL);
		DXGI_MODE_DESC *m = calloc(count ? count : 1, sizeof(*m));
		if (m && SUCCEEDED(IDXGIOutput_GetDisplayModeList(o, DXGI_FORMAT_B8G8R8A8_UNORM, 0, &count, m))) {
			for (UINT i = 0; i < count && n < max; i++) {
				out[n].w = m[i].Width;
				out[n].h = m[i].Height;
				out[n].hz = m[i].RefreshRate.Denominator ? (m[i].RefreshRate.Numerator + m[i].RefreshRate.Denominator / 2) / m[i].RefreshRate.Denominator : 0;
				n++;
			}
		}
		free(m);
	}
done:
	if (o)
		IDXGIOutput_Release(o);
	if (ad)
		IDXGIAdapter_Release(ad);
	if (fac)
		IDXGIFactory_Release(fac);
	return n;
}

void *nat_output_monitor(int output)
{
	IDXGIFactory *fac = NULL;
	IDXGIAdapter *ad = NULL;
	IDXGIOutput *o = NULL;
	void *mon = NULL;
	if (SUCCEEDED(CreateDXGIFactory(&IID_IDXGIFactory, (void **)&fac)) && SUCCEEDED(IDXGIFactory_EnumAdapters(fac, 0, &ad)) && SUCCEEDED(IDXGIAdapter_EnumOutputs(ad, (UINT)output, &o))) {
		DXGI_OUTPUT_DESC od;
		if (SUCCEEDED(IDXGIOutput_GetDesc(o, &od)))
			mon = od.Monitor;
	}
	if (o)
		IDXGIOutput_Release(o);
	if (ad)
		IDXGIAdapter_Release(ad);
	if (fac)
		IDXGIFactory_Release(fac);
	return mon;
}

int nat_output_count(void)
{
	IDXGIFactory *fac = NULL;
	IDXGIAdapter *ad = NULL;
	int n = 0;
	if (SUCCEEDED(CreateDXGIFactory(&IID_IDXGIFactory, (void **)&fac)) && SUCCEEDED(IDXGIFactory_EnumAdapters(fac, 0, &ad))) {
		IDXGIOutput *o = NULL;
		while (SUCCEEDED(IDXGIAdapter_EnumOutputs(ad, (UINT)n, &o))) {
			IDXGIOutput_Release(o);
			n++;
		}
	}
	if (ad)
		IDXGIAdapter_Release(ad);
	if (fac)
		IDXGIFactory_Release(fac);
	return n;
}
#endif

#ifdef _WIN32
int nat_save_scene_bmp(nat_device *d, const char *path)
{
	D3D11_TEXTURE2D_DESC td = {0};
	td.Width = d->p.width;
	td.Height = d->p.height;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_STAGING;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ID3D11Texture2D *st = NULL;
	if (FAILED(ID3D11Device_CreateTexture2D(d->dev, &td, NULL, &st)))
		return 0;
	ID3D11Texture2D *src = d->scene;
	ID3D11Texture2D *tmp = NULL;
	if (d->samples > 1) {
		D3D11_TEXTURE2D_DESC rd = td;
		rd.Usage = D3D11_USAGE_DEFAULT;
		rd.CPUAccessFlags = 0;
		rd.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
		if (SUCCEEDED(ID3D11Device_CreateTexture2D(d->dev, &rd, NULL, &tmp))) {
			ID3D11DeviceContext_ResolveSubresource(d->ctx, (ID3D11Resource *)tmp, 0, (ID3D11Resource *)d->scene, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
			src = tmp;
		}
	}
	ID3D11DeviceContext_CopyResource(d->ctx, (ID3D11Resource *)st, (ID3D11Resource *)src);
	D3D11_MAPPED_SUBRESOURCE m;
	int ok = 0;
	if (SUCCEEDED(ID3D11DeviceContext_Map(d->ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
		FILE *f = fopen(path, "wb");
		if (f) {
			const unsigned w = d->p.width, h = d->p.height;
			uint8_t hdr[54] = {'B', 'M'};
			const uint32_t size = 54 + w * h * 4, off = 54, dib = 40, planes_bpp = 1 | 32u << 16;
			memcpy(hdr + 2, &size, 4);
			memcpy(hdr + 10, &off, 4);
			memcpy(hdr + 14, &dib, 4);
			memcpy(hdr + 18, &w, 4);
			const int32_t nh = -(int32_t)h;
			memcpy(hdr + 22, &nh, 4);
			memcpy(hdr + 26, &planes_bpp, 4);
			fwrite(hdr, 1, 54, f);
			for (unsigned y = 0; y < h; y++)
				fwrite((uint8_t *)m.pData + (size_t)y * m.RowPitch, 4, w, f);
			fclose(f);
			ok = 1;
		}
		ID3D11DeviceContext_Unmap(d->ctx, (ID3D11Resource *)st, 0);
	}
	if (tmp)
		ID3D11Texture2D_Release(tmp);
	ID3D11Texture2D_Release(st);
	return ok;
}
#endif
