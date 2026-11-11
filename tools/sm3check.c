// Runs a pixel shader of the game (Direct3D 9, ps_3_0 bytecode) on a Direct3D 9 device and its translation (tools/sm3_to_hlsl.py, compiled to ps_5_0) on a Direct3D 11 WARP device with the
// same inputs, and compares what they write (tools/sm3_semantic_check.py drives it). Windows only; the game is not needed.
//
//   sm3check <manifest>      one test per line: <ps.bin> <ps.cso> <vs30.bin> <vs50.cso> <seed> <render targets> <name>
//   the answer, one line per test: <name> MATCH | DIFF <max abs difference> rt<n>.<c> d3d9 <a> d3d11 <b> | KILL (both killed the pixel) | ERR9 <step> <hr> | ERR11 <step> <hr>
//
// Inputs: a vertex shader writes the same vertex-shader constants (c0..c15, from the seed) to every interpolant, so the pixel shader sees one constant value per register; the pixel
// shader's constants c0..c223 come from the seed too (the def constants of the shader override them), the integer constants are (2, 0, 1, 0), the booleans alternate; every sampler s0..s15
// is a 2D 8x8 texture of seeded values, linear filtering, clamped. The render targets are A32B32G32R32F, 4x4, a full-screen triangle; the pixel (1,1) is compared.
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TEXW 8
#define RTW 4

static uint32_t hash32(uint32_t a, uint32_t b, uint32_t c)
{
	uint32_t h = a * 2654435761u ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2));
	h ^= c * 0x85ebca6bu + 0xc2b2ae35u + (h << 6) + (h >> 2);
	h ^= h >> 15;
	h *= 0x2c1b3c6du;
	h ^= h >> 12;
	return h;
}
static float frand(uint32_t a, uint32_t b, uint32_t c, float lo, float hi)
{
	return lo + (hi - lo) * ((hash32(a, b, c) & 0xffffff) / 16777215.0f);
}

static int g_flat; // SM3FLAT=1: every texel of a texture is the same, so what is compared is the shader's arithmetic, not the filtering of noise (cube face seams, 8-bit filter weights)
static uint32_t texel(uint32_t a, uint32_t x, uint32_t y)
{
	return g_flat ? hash32(a, 0, 0) : hash32(a, x, y);
}

static uint8_t *load(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc(n);
	if (fread(b, 1, n, f) != (size_t)n) {
		free(b);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*size = (size_t)n;
	return b;
}

typedef struct {
	IDirect3D9 *d3d;
	IDirect3DDevice9 *dev;
	ID3D11Device *d11;
	ID3D11DeviceContext *ctx;
	HWND wnd;
} env_t;

static LRESULT CALLBACK wproc(HWND w, UINT m, WPARAM a, LPARAM l)
{
	return DefWindowProcA(w, m, a, l);
}

static int init(env_t *e, int software)
{
	WNDCLASSA wc = {0};
	wc.lpfnWndProc = wproc;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.lpszClassName = "sm3check";
	RegisterClassA(&wc);
	e->wnd = CreateWindowA("sm3check", "sm3check", WS_OVERLAPPED, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
	e->d3d = Direct3DCreate9(D3D_SDK_VERSION);
	if (!e->d3d)
		return 0;
	D3DPRESENT_PARAMETERS pp = {0};
	pp.Windowed = TRUE;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.BackBufferFormat = D3DFMT_UNKNOWN;
	pp.BackBufferWidth = 64;
	pp.BackBufferHeight = 64;
	pp.hDeviceWindow = e->wnd;
	HRESULT hr = IDirect3D9_CreateDevice(e->d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, e->wnd, software ? D3DCREATE_SOFTWARE_VERTEXPROCESSING : D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &e->dev);
	if (FAILED(hr)) {
		fprintf(stderr, "Direct3D 9 device: %lx\n", (unsigned long)hr);
		return 0;
	}
	D3D_FEATURE_LEVEL fl;
	hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &e->d11, &fl, &e->ctx);
	if (FAILED(hr)) {
		fprintf(stderr, "Direct3D 11 WARP device: %lx\n", (unsigned long)hr);
		return 0;
	}
	return 1;
}

static const float tri[3][4] = {{-1, -1, 0.5f, 1}, {3, -1, 0.5f, 1}, {-1, 3, 0.5f, 1}};

// ---- one test on Direct3D 9; out: nrt x 4 floats; returns 0 or an error code (step in *step)
static int run9(env_t *e, const uint8_t *ps, size_t pssz, const uint8_t *vs, size_t vssz, uint32_t seed, int nrt, const char *kinds, float *out, const char **step, HRESULT *hrout)
{
	(void)pssz;
	(void)vssz;
	IDirect3DDevice9 *d = e->dev;
	IDirect3DVertexShader9 *vsh = NULL;
	IDirect3DPixelShader9 *psh = NULL;
	IDirect3DVertexDeclaration9 *decl = NULL;
	IDirect3DBaseTexture9 *tex[16] = {0};
	IDirect3DSurface9 *rt[4] = {0}, *sys[4] = {0};
	HRESULT hr;
	int rc = 0;
#define CK(call, name) do { hr = (call); if (FAILED(hr)) { *step = name; *hrout = hr; rc = 1; goto done; } } while (0)
	CK(IDirect3DDevice9_CreateVertexShader(d, (const DWORD *)vs, &vsh), "CreateVertexShader");
	CK(IDirect3DDevice9_CreatePixelShader(d, (const DWORD *)ps, &psh), "CreatePixelShader");
	D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
	CK(IDirect3DDevice9_CreateVertexDeclaration(d, el, &decl), "CreateVertexDeclaration");
	for (int s = 0; s < 16; s++) {
		const int kind = kinds[s] - '0';
		if (kind == 3) {
			IDirect3DCubeTexture9 *ct = NULL;
			CK(IDirect3DDevice9_CreateCubeTexture(d, TEXW, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &ct, NULL), "CreateCubeTexture");
			tex[s] = (IDirect3DBaseTexture9 *)ct;
			for (int face = 0; face < 6; face++) {
				D3DLOCKED_RECT lr;
				CK(IDirect3DCubeTexture9_LockRect(ct, (D3DCUBEMAP_FACES)face, 0, &lr, NULL, 0), "LockRect(cube)");
				for (int y = 0; y < TEXW; y++)
					for (int x = 0; x < TEXW; x++)
						((uint32_t *)((uint8_t *)lr.pBits + y * lr.Pitch))[x] = texel(seed + s, x, y + TEXW * face);
				IDirect3DCubeTexture9_UnlockRect(ct, (D3DCUBEMAP_FACES)face, 0);
			}
		} else if (kind == 4) {
			IDirect3DVolumeTexture9 *vt = NULL;
			CK(IDirect3DDevice9_CreateVolumeTexture(d, TEXW, TEXW, TEXW, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &vt, NULL), "CreateVolumeTexture");
			tex[s] = (IDirect3DBaseTexture9 *)vt;
			D3DLOCKED_BOX lb;
			CK(IDirect3DVolumeTexture9_LockBox(vt, 0, &lb, NULL, 0), "LockBox");
			for (int z = 0; z < TEXW; z++)
				for (int y = 0; y < TEXW; y++)
					for (int x = 0; x < TEXW; x++)
						((uint32_t *)((uint8_t *)lb.pBits + z * lb.SlicePitch + y * lb.RowPitch))[x] = texel(seed + s, x, y + TEXW * z);
			IDirect3DVolumeTexture9_UnlockBox(vt, 0);
		} else {
			IDirect3DTexture9 *t2 = NULL;
			CK(IDirect3DDevice9_CreateTexture(d, TEXW, TEXW, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t2, NULL), "CreateTexture");
			tex[s] = (IDirect3DBaseTexture9 *)t2;
			D3DLOCKED_RECT lr;
			CK(IDirect3DTexture9_LockRect(t2, 0, &lr, NULL, 0), "LockRect");
			for (int y = 0; y < TEXW; y++)
				for (int x = 0; x < TEXW; x++)
					((uint32_t *)((uint8_t *)lr.pBits + y * lr.Pitch))[x] = texel(seed + s, x, y);
			IDirect3DTexture9_UnlockRect(t2, 0);
		}
		IDirect3DDevice9_SetTexture(d, s, tex[s]);
		IDirect3DDevice9_SetSamplerState(d, s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		IDirect3DDevice9_SetSamplerState(d, s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		IDirect3DDevice9_SetSamplerState(d, s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		IDirect3DDevice9_SetSamplerState(d, s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		IDirect3DDevice9_SetSamplerState(d, s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		IDirect3DDevice9_SetSamplerState(d, s, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
		IDirect3DDevice9_SetSamplerState(d, s, D3DSAMP_SRGBTEXTURE, 0);
	}
	for (int i = 0; i < nrt; i++) {
		CK(IDirect3DDevice9_CreateRenderTarget(d, RTW, RTW, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &rt[i], NULL), "CreateRenderTarget");
		CK(IDirect3DDevice9_CreateOffscreenPlainSurface(d, RTW, RTW, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &sys[i], NULL), "CreateOffscreenPlainSurface");
		CK(IDirect3DDevice9_SetRenderTarget(d, i, rt[i]), "SetRenderTarget");
	}
	for (int i = nrt; i < 4; i++)
		IDirect3DDevice9_SetRenderTarget(d, i, NULL);
	IDirect3DDevice9_SetRenderState(d, D3DRS_ZENABLE, FALSE);
	IDirect3DDevice9_SetRenderState(d, D3DRS_CULLMODE, D3DCULL_NONE);
	IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHABLENDENABLE, FALSE);
	IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHATESTENABLE, FALSE);
	IDirect3DDevice9_SetRenderState(d, D3DRS_COLORWRITEENABLE, 15);
	IDirect3DDevice9_SetRenderState(d, D3DRS_COLORWRITEENABLE1, 15);
	IDirect3DDevice9_SetRenderState(d, D3DRS_COLORWRITEENABLE2, 15);
	IDirect3DDevice9_SetRenderState(d, D3DRS_COLORWRITEENABLE3, 15);
	for (int i = nrt - 1; i >= 0; i--) { // Clear works on render target 0 only: each target is put there in turn, then all are set
		IDirect3DDevice9_SetRenderTarget(d, 0, rt[i]);
		D3DVIEWPORT9 vp = {0, 0, RTW, RTW, 0, 1};
		IDirect3DDevice9_SetViewport(d, &vp);
		IDirect3DDevice9_Clear(d, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_COLORVALUE(0, 0, 0, 0), 1, 0);
	}
	for (int i = 0; i < nrt; i++)
		IDirect3DDevice9_SetRenderTarget(d, i, rt[i]);
	CK(IDirect3DDevice9_SetVertexDeclaration(d, decl), "SetVertexDeclaration");
	CK(IDirect3DDevice9_SetVertexShader(d, vsh), "SetVertexShader");
	float vsc[16][4], psc[224][4];
	for (int i = 0; i < 16; i++)
		for (int k = 0; k < 4; k++)
			vsc[i][k] = frand(seed, 1000 + i, k, 0.0f, 1.0f);
	for (int i = 0; i < 224; i++)
		for (int k = 0; k < 4; k++)
			psc[i][k] = frand(seed, 2000 + i, k, -1.5f, 1.5f);
	int ic[16][4];
	BOOL bc[16];
	for (int i = 0; i < 16; i++) {
		ic[i][0] = 2;
		ic[i][1] = 0;
		ic[i][2] = 1;
		ic[i][3] = 0;
		bc[i] = i & 1;
	}
	CK(IDirect3DDevice9_SetVertexShaderConstantF(d, 0, &vsc[0][0], 16), "SetVertexShaderConstantF");
	CK(IDirect3DDevice9_SetPixelShaderConstantF(d, 0, &psc[0][0], 224), "SetPixelShaderConstantF");
	CK(IDirect3DDevice9_SetPixelShaderConstantI(d, 0, &ic[0][0], 16), "SetPixelShaderConstantI");
	CK(IDirect3DDevice9_SetPixelShaderConstantB(d, 0, bc, 16), "SetPixelShaderConstantB");
	CK(IDirect3DDevice9_SetPixelShader(d, psh), "SetPixelShader"); // after the constants: the shader's own def constants are applied when it is set and must win over ours
	CK(IDirect3DDevice9_BeginScene(d), "BeginScene");
	hr = IDirect3DDevice9_DrawPrimitiveUP(d, D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0]));
	IDirect3DDevice9_EndScene(d);
	if (FAILED(hr)) {
		*step = "DrawPrimitiveUP";
		*hrout = hr;
		rc = 1;
		goto done;
	}
	for (int i = 0; i < nrt; i++) {
		CK(IDirect3DDevice9_GetRenderTargetData(d, rt[i], sys[i]), "GetRenderTargetData");
		D3DLOCKED_RECT lr;
		CK(IDirect3DSurface9_LockRect(sys[i], &lr, NULL, D3DLOCK_READONLY), "LockRect(target)");
		memcpy(out + i * 4, (uint8_t *)lr.pBits + 1 * lr.Pitch + 1 * 16, 16);
		IDirect3DSurface9_UnlockRect(sys[i]);
	}
done:
	for (int i = 0; i < 4; i++)
		IDirect3DDevice9_SetRenderTarget(d, i, NULL);
	IDirect3DDevice9_SetPixelShader(d, NULL);
	IDirect3DDevice9_SetVertexShader(d, NULL);
	for (int s = 0; s < 16; s++) {
		IDirect3DDevice9_SetTexture(d, s, NULL);
		if (tex[s])
			IDirect3DBaseTexture9_Release(tex[s]);
	}
	for (int i = 0; i < 4; i++) {
		if (rt[i])
			IDirect3DSurface9_Release(rt[i]);
		if (sys[i])
			IDirect3DSurface9_Release(sys[i]);
	}
	if (decl)
		IDirect3DVertexDeclaration9_Release(decl);
	if (vsh)
		IDirect3DVertexShader9_Release(vsh);
	if (psh)
		IDirect3DPixelShader9_Release(psh);
	return rc;
#undef CK
}

static int run11(env_t *e, const uint8_t *ps, size_t pssz, const uint8_t *vs, size_t vssz, uint32_t seed, int nrt, const char *kinds, float *out, const char **step, HRESULT *hrout)
{
	ID3D11Device *d = e->d11;
	ID3D11DeviceContext *c = e->ctx;
	ID3D11VertexShader *vsh = NULL;
	ID3D11PixelShader *psh = NULL;
	ID3D11InputLayout *lay = NULL;
	ID3D11Buffer *vb = NULL, *cbv = NULL, *cbp = NULL, *cbi = NULL;
	ID3D11Resource *tex[16] = {0};
	ID3D11Texture2D *rt[4] = {0}, *stg[4] = {0};
	ID3D11ShaderResourceView *srv[16] = {0};
	ID3D11RenderTargetView *rtv[4] = {0};
	ID3D11SamplerState *smp = NULL;
	ID3D11RasterizerState *rs = NULL;
	ID3D11BlendState *bs = NULL;
	HRESULT hr;
	int rc = 0;
#define CK(call, name) do { hr = (call); if (FAILED(hr)) { *step = name; *hrout = hr; rc = 1; goto done; } } while (0)
	CK(ID3D11Device_CreateVertexShader(d, vs, vssz, NULL, &vsh), "CreateVertexShader");
	CK(ID3D11Device_CreatePixelShader(d, ps, pssz, NULL, &psh), "CreatePixelShader");
	D3D11_INPUT_ELEMENT_DESC ie[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
	CK(ID3D11Device_CreateInputLayout(d, ie, 1, vs, vssz, &lay), "CreateInputLayout");
	D3D11_BUFFER_DESC bd = {sizeof(tri), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
	D3D11_SUBRESOURCE_DATA sd = {tri, 0, 0};
	CK(ID3D11Device_CreateBuffer(d, &bd, &sd, &vb), "CreateBuffer(vertices)");
	float vsc[16][4], psc[256][4];
	memset(psc, 0, sizeof(psc));
	for (int i = 0; i < 16; i++)
		for (int k = 0; k < 4; k++)
			vsc[i][k] = frand(seed, 1000 + i, k, 0.0f, 1.0f);
	for (int i = 0; i < 224; i++)
		for (int k = 0; k < 4; k++)
			psc[i][k] = frand(seed, 2000 + i, k, -1.5f, 1.5f);
	if (getenv("SM3DBG"))
		printf("seed %u vs c0 = %g %g %g %g\n", seed, vsc[0][0], vsc[0][1], vsc[0][2], vsc[0][3]);
	int ib[16 * 4 + 8 * 4];
	for (int i = 0; i < 16; i++) {
		ib[i * 4] = 2;
		ib[i * 4 + 1] = 0;
		ib[i * 4 + 2] = 1;
		ib[i * 4 + 3] = 0;
	}
	for (int i = 0; i < 32; i++)
		ib[64 + i] = (i & 1);
	D3D11_BUFFER_DESC cb = {sizeof(vsc), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
	D3D11_SUBRESOURCE_DATA cd = {vsc, 0, 0};
	CK(ID3D11Device_CreateBuffer(d, &cb, &cd, &cbv), "CreateBuffer(vs constants)");
	cb.ByteWidth = sizeof(psc);
	cd.pSysMem = psc;
	CK(ID3D11Device_CreateBuffer(d, &cb, &cd, &cbp), "CreateBuffer(ps constants)");
	cb.ByteWidth = sizeof(ib);
	cd.pSysMem = ib;
	CK(ID3D11Device_CreateBuffer(d, &cb, &cd, &cbi), "CreateBuffer(ps int/bool constants)");
	for (int s = 0; s < 16; s++) {
		const int kind = kinds[s] - '0';
		if (kind == 4) {
			static uint32_t vol[TEXW * TEXW * TEXW];
			for (int z = 0; z < TEXW; z++)
				for (int y = 0; y < TEXW; y++)
					for (int x = 0; x < TEXW; x++)
						vol[(z * TEXW + y) * TEXW + x] = texel(seed + s, x, y + TEXW * z);
			D3D11_TEXTURE3D_DESC td3 = {TEXW, TEXW, TEXW, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
			D3D11_SUBRESOURCE_DATA d3 = {vol, TEXW * 4, TEXW * TEXW * 4};
			ID3D11Texture3D *t3 = NULL;
			CK(ID3D11Device_CreateTexture3D(d, &td3, &d3, &t3), "CreateTexture3D");
			tex[s] = (ID3D11Resource *)t3;
		} else if (kind == 3) {
			static uint32_t faces[6][TEXW * TEXW];
			D3D11_SUBRESOURCE_DATA dc[6];
			for (int face = 0; face < 6; face++) {
				for (int y = 0; y < TEXW; y++)
					for (int x = 0; x < TEXW; x++)
						faces[face][y * TEXW + x] = texel(seed + s, x, y + TEXW * face);
				dc[face].pSysMem = faces[face];
				dc[face].SysMemPitch = TEXW * 4;
				dc[face].SysMemSlicePitch = 0;
			}
			D3D11_TEXTURE2D_DESC tdc = {TEXW, TEXW, 1, 6, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_TEXTURECUBE};
			ID3D11Texture2D *tc = NULL;
			CK(ID3D11Device_CreateTexture2D(d, &tdc, dc, &tc), "CreateTexture2D(cube)");
			tex[s] = (ID3D11Resource *)tc;
		} else {
			uint32_t px[TEXW * TEXW];
			for (int y = 0; y < TEXW; y++)
				for (int x = 0; x < TEXW; x++)
					px[y * TEXW + x] = texel(seed + s, x, y);
			D3D11_TEXTURE2D_DESC td = {TEXW, TEXW, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
			D3D11_SUBRESOURCE_DATA td_data = {px, TEXW * 4, 0};
			ID3D11Texture2D *t2 = NULL;
			CK(ID3D11Device_CreateTexture2D(d, &td, &td_data, &t2), "CreateTexture2D");
			tex[s] = (ID3D11Resource *)t2;
		}
		CK(ID3D11Device_CreateShaderResourceView(d, tex[s], NULL, &srv[s]), "CreateShaderResourceView");
	}
	D3D11_SAMPLER_DESC sm = {D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, {0, 0, 0, 0}, 0, D3D11_FLOAT32_MAX};
	CK(ID3D11Device_CreateSamplerState(d, &sm, &smp), "CreateSamplerState");
	D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
	CK(ID3D11Device_CreateRasterizerState(d, &rd, &rs), "CreateRasterizerState");
	D3D11_BLEND_DESC bld;
	memset(&bld, 0, sizeof(bld));
	for (int i = 0; i < 8; i++)
		bld.RenderTarget[i].RenderTargetWriteMask = 15;
	CK(ID3D11Device_CreateBlendState(d, &bld, &bs), "CreateBlendState");
	for (int i = 0; i < nrt; i++) {
		D3D11_TEXTURE2D_DESC td = {RTW, RTW, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
		CK(ID3D11Device_CreateTexture2D(d, &td, NULL, &rt[i]), "CreateTexture2D(target)");
		CK(ID3D11Device_CreateRenderTargetView(d, (ID3D11Resource *)rt[i], NULL, &rtv[i]), "CreateRenderTargetView");
		td.Usage = D3D11_USAGE_STAGING;
		td.BindFlags = 0;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		CK(ID3D11Device_CreateTexture2D(d, &td, NULL, &stg[i]), "CreateTexture2D(staging)");
		const float clear[4] = {0, 0, 0, 0};
		ID3D11DeviceContext_ClearRenderTargetView(c, rtv[i], clear);
	}
	D3D11_VIEWPORT vp = {0, 0, RTW, RTW, 0, 1};
	UINT stride = 16, off = 0;
	ID3D11DeviceContext_IASetInputLayout(c, lay);
	ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11DeviceContext_IASetVertexBuffers(c, 0, 1, &vb, &stride, &off);
	ID3D11DeviceContext_VSSetShader(c, vsh, NULL, 0);
	ID3D11DeviceContext_VSSetConstantBuffers(c, 0, 1, &cbv);
	ID3D11DeviceContext_PSSetShader(c, psh, NULL, 0);
	ID3D11Buffer *pcb[2] = {cbp, cbi};
	ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 2, pcb);
	ID3D11DeviceContext_PSSetShaderResources(c, 0, 16, srv);
	ID3D11SamplerState *ss[16];
	for (int i = 0; i < 16; i++)
		ss[i] = smp;
	ID3D11DeviceContext_PSSetSamplers(c, 0, 16, ss);
	ID3D11DeviceContext_RSSetState(c, rs);
	ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
	ID3D11DeviceContext_OMSetBlendState(c, bs, NULL, 0xffffffff);
	ID3D11DeviceContext_OMSetRenderTargets(c, nrt, rtv, NULL);
	ID3D11DeviceContext_Draw(c, 3, 0);
	for (int i = 0; i < nrt; i++) {
		ID3D11DeviceContext_CopyResource(c, (ID3D11Resource *)stg[i], (ID3D11Resource *)rt[i]);
		D3D11_MAPPED_SUBRESOURCE m;
		CK(ID3D11DeviceContext_Map(c, (ID3D11Resource *)stg[i], 0, D3D11_MAP_READ, 0, &m), "Map");
		memcpy(out + i * 4, (uint8_t *)m.pData + 1 * m.RowPitch + 1 * 16, 16);
		ID3D11DeviceContext_Unmap(c, (ID3D11Resource *)stg[i], 0);
	}
done:
	ID3D11DeviceContext_ClearState(c);
	for (int i = 0; i < 16; i++) {
		if (srv[i])
			ID3D11ShaderResourceView_Release(srv[i]);
		if (tex[i])
			ID3D11Resource_Release(tex[i]);
	}
	for (int i = 0; i < 4; i++) {
		if (rtv[i])
			ID3D11RenderTargetView_Release(rtv[i]);
		if (rt[i])
			ID3D11Texture2D_Release(rt[i]);
		if (stg[i])
			ID3D11Texture2D_Release(stg[i]);
	}
	if (smp)
		ID3D11SamplerState_Release(smp);
	if (rs)
		ID3D11RasterizerState_Release(rs);
	if (bs)
		ID3D11BlendState_Release(bs);
	if (cbv)
		ID3D11Buffer_Release(cbv);
	if (cbp)
		ID3D11Buffer_Release(cbp);
	if (cbi)
		ID3D11Buffer_Release(cbi);
	if (vb)
		ID3D11Buffer_Release(vb);
	if (lay)
		ID3D11InputLayout_Release(lay);
	if (vsh)
		ID3D11VertexShader_Release(vsh);
	if (psh)
		ID3D11PixelShader_Release(psh);
	return rc;
#undef CK
}


// ---------------------------------------------------------------------------------------------------- vertex shaders
// sm3check --vs <manifest>: one test per line: <vs.bin> <vs.cso> <seed> <name> <inputs> <outputs>; the inputs and outputs are "usage:index" lists (the D3DDECLUSAGE numbers, comma separated).
// Direct3D 9 runs the vertex shader with ProcessVertices on a device with software vertex processing (the runtime's own CPU implementation), Direct3D 11 WARP with stream output (the vertex
// shader's bytecode is the geometry shader's, which writes the outputs to a buffer); three vertices of seeded values, the same constants; every output component is compared.
static const char *const usage_names[14] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"};

static int parse_spec(const char *spec, int *usage, int *index, int max)
{
	int n = 0;
	const char *p = spec;
	while (*p && n < max) {
		int u, i, used = 0;
		if (sscanf(p, "%d:%d%n", &u, &i, &used) < 2)
			break;
		usage[n] = u;
		index[n] = i;
		n++;
		p += used;
		if (*p == ',')
			p++;
	}
	return n;
}

static int runvs9(env_t *e, const uint8_t *vs, uint32_t seed, int nin, const int *iu, const int *ii, int nout, const int *ou, const int *oi, const float *vin, float *out, const char **step, HRESULT *hrout)
{
	IDirect3DDevice9 *d = e->dev;
	IDirect3DVertexShader9 *vsh = NULL;
	IDirect3DVertexDeclaration9 *din = NULL, *dout = NULL;
	IDirect3DVertexBuffer9 *vb = NULL, *ob = NULL;
	HRESULT hr;
	int rc = 0;
#define CK(call, name) do { hr = (call); if (FAILED(hr)) { *step = name; *hrout = hr; rc = 1; goto done; } } while (0)
	CK(IDirect3DDevice9_CreateVertexShader(d, (const DWORD *)vs, &vsh), "CreateVertexShader");
	D3DVERTEXELEMENT9 el_in[17], el_out[17];
	for (int k = 0; k < nin; k++) {
		D3DVERTEXELEMENT9 x = {0, (WORD)(k * 16), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, (BYTE)iu[k], (BYTE)ii[k]};
		el_in[k] = x;
	}
	D3DVERTEXELEMENT9 end = D3DDECL_END();
	el_in[nin] = end;
	for (int k = 0; k < nout; k++) {
		D3DVERTEXELEMENT9 x = {0, (WORD)(k * 16), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, (BYTE)ou[k], (BYTE)oi[k]};
		el_out[k] = x;
	}
	el_out[nout] = end;
	CK(IDirect3DDevice9_CreateVertexDeclaration(d, el_in, &din), "CreateVertexDeclaration(in)");
	CK(IDirect3DDevice9_CreateVertexDeclaration(d, el_out, &dout), "CreateVertexDeclaration(out)");
	CK(IDirect3DDevice9_CreateVertexBuffer(d, 3 * nin * 16, D3DUSAGE_SOFTWAREPROCESSING, 0, D3DPOOL_SYSTEMMEM, &vb, NULL), "CreateVertexBuffer(in)");
	CK(IDirect3DDevice9_CreateVertexBuffer(d, 3 * nout * 16, D3DUSAGE_SOFTWAREPROCESSING, 0, D3DPOOL_SYSTEMMEM, &ob, NULL), "CreateVertexBuffer(out)");
	void *p;
	CK(IDirect3DVertexBuffer9_Lock(vb, 0, 0, &p, 0), "Lock(in)");
	memcpy(p, vin, 3 * nin * 16);
	IDirect3DVertexBuffer9_Unlock(vb);
	float vsc[256][4];
	for (int i = 0; i < 256; i++)
		for (int k = 0; k < 4; k++)
			vsc[i][k] = frand(seed, 3000 + i, k, -1.5f, 1.5f);
	int ic[16][4];
	BOOL bc[16];
	for (int i = 0; i < 16; i++) {
		ic[i][0] = 2;
		ic[i][1] = 0;
		ic[i][2] = 1;
		ic[i][3] = 0;
		bc[i] = i & 1;
	}
	CK(IDirect3DDevice9_SetVertexShaderConstantF(d, 0, &vsc[0][0], 256), "SetVertexShaderConstantF");
	CK(IDirect3DDevice9_SetVertexShaderConstantI(d, 0, &ic[0][0], 16), "SetVertexShaderConstantI");
	CK(IDirect3DDevice9_SetVertexShaderConstantB(d, 0, bc, 16), "SetVertexShaderConstantB");
	CK(IDirect3DDevice9_SetVertexShader(d, vsh), "SetVertexShader"); // after the constants: the def constants of the shader win
	CK(IDirect3DDevice9_SetVertexDeclaration(d, din), "SetVertexDeclaration");
	CK(IDirect3DDevice9_SetStreamSource(d, 0, vb, 0, nin * 16), "SetStreamSource");
	CK(IDirect3DDevice9_ProcessVertices(d, 0, 0, 3, ob, dout, 0), "ProcessVertices");
	CK(IDirect3DVertexBuffer9_Lock(ob, 0, 0, &p, D3DLOCK_READONLY), "Lock(out)");
	memcpy(out, p, 3 * nout * 16);
	IDirect3DVertexBuffer9_Unlock(ob);
done:
	IDirect3DDevice9_SetVertexShader(d, NULL);
	IDirect3DDevice9_SetStreamSource(d, 0, NULL, 0, 0);
	if (vb)
		IDirect3DVertexBuffer9_Release(vb);
	if (ob)
		IDirect3DVertexBuffer9_Release(ob);
	if (din)
		IDirect3DVertexDeclaration9_Release(din);
	if (dout)
		IDirect3DVertexDeclaration9_Release(dout);
	if (vsh)
		IDirect3DVertexShader9_Release(vsh);
	return rc;
#undef CK
}

static int runvs11(env_t *e, const uint8_t *vs, size_t vssz, uint32_t seed, int nin, const int *iu, const int *ii, int nout, const int *ou, const int *oi, const float *vin, float *out, const char **step, HRESULT *hrout)
{
	ID3D11Device *d = e->d11;
	ID3D11DeviceContext *c = e->ctx;
	ID3D11VertexShader *vsh = NULL;
	ID3D11GeometryShader *gs = NULL;
	ID3D11InputLayout *lay = NULL;
	ID3D11Buffer *vb = NULL, *cbf = NULL, *cbi = NULL, *sob = NULL, *stg = NULL;
	HRESULT hr;
	int rc = 0;
#define CK(call, name) do { hr = (call); if (FAILED(hr)) { *step = name; *hrout = hr; rc = 1; goto done; } } while (0)
	CK(ID3D11Device_CreateVertexShader(d, vs, vssz, NULL, &vsh), "CreateVertexShader");
	D3D11_INPUT_ELEMENT_DESC ie[16];
	for (int k = 0; k < nin; k++) {
		D3D11_INPUT_ELEMENT_DESC x = {usage_names[iu[k]], (UINT)ii[k], DXGI_FORMAT_R32G32B32A32_FLOAT, 0, (UINT)(k * 16), D3D11_INPUT_PER_VERTEX_DATA, 0};
		ie[k] = x;
	}
	CK(ID3D11Device_CreateInputLayout(d, ie, nin, vs, vssz, &lay), "CreateInputLayout");
	D3D11_SO_DECLARATION_ENTRY so[16];
	for (int k = 0; k < nout; k++) {
		D3D11_SO_DECLARATION_ENTRY x = {0, ou[k] == 0 && oi[k] == 0 ? "SV_Position" : usage_names[ou[k]], (UINT)oi[k], 0, 4, 0};
		so[k] = x;
	}
	UINT stride = nout * 16;
	CK(ID3D11Device_CreateGeometryShaderWithStreamOutput(d, vs, vssz, so, nout, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, NULL, &gs), "CreateGeometryShaderWithStreamOutput");
	D3D11_BUFFER_DESC bd = {(UINT)(3 * nin * 16), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
	D3D11_SUBRESOURCE_DATA sd = {vin, 0, 0};
	CK(ID3D11Device_CreateBuffer(d, &bd, &sd, &vb), "CreateBuffer(vertices)");
	float vsc[256][4];
	for (int i = 0; i < 256; i++)
		for (int k = 0; k < 4; k++)
			vsc[i][k] = frand(seed, 3000 + i, k, -1.5f, 1.5f);
	int ib[16 * 4 + 8 * 4];
	for (int i = 0; i < 16; i++) {
		ib[i * 4] = 2;
		ib[i * 4 + 1] = 0;
		ib[i * 4 + 2] = 1;
		ib[i * 4 + 3] = 0;
	}
	for (int i = 0; i < 32; i++)
		ib[64 + i] = (i & 1);
	D3D11_BUFFER_DESC cb = {sizeof(vsc), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
	D3D11_SUBRESOURCE_DATA cd = {vsc, 0, 0};
	CK(ID3D11Device_CreateBuffer(d, &cb, &cd, &cbf), "CreateBuffer(constants)");
	cb.ByteWidth = sizeof(ib);
	cd.pSysMem = ib;
	CK(ID3D11Device_CreateBuffer(d, &cb, &cd, &cbi), "CreateBuffer(int/bool constants)");
	D3D11_BUFFER_DESC sod = {(UINT)(3 * nout * 16), D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0};
	CK(ID3D11Device_CreateBuffer(d, &sod, NULL, &sob), "CreateBuffer(stream output)");
	D3D11_BUFFER_DESC sg = {(UINT)(3 * nout * 16), D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
	CK(ID3D11Device_CreateBuffer(d, &sg, NULL, &stg), "CreateBuffer(staging)");
	UINT vstride = nin * 16, off = 0;
	ID3D11Buffer *cbs[2] = {cbf, cbi};
	ID3D11DeviceContext_IASetInputLayout(c, lay);
	ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
	ID3D11DeviceContext_IASetVertexBuffers(c, 0, 1, &vb, &vstride, &off);
	ID3D11DeviceContext_VSSetShader(c, vsh, NULL, 0);
	ID3D11DeviceContext_VSSetConstantBuffers(c, 0, 2, cbs);
	ID3D11DeviceContext_GSSetShader(c, gs, NULL, 0);
	ID3D11DeviceContext_PSSetShader(c, NULL, NULL, 0);
	UINT soff = 0;
	ID3D11DeviceContext_SOSetTargets(c, 1, &sob, &soff);
	ID3D11DeviceContext_Draw(c, 3, 0);
	ID3D11Buffer *none = NULL;
	ID3D11DeviceContext_SOSetTargets(c, 1, &none, &soff);
	ID3D11DeviceContext_CopyResource(c, (ID3D11Resource *)stg, (ID3D11Resource *)sob);
	D3D11_MAPPED_SUBRESOURCE mp;
	CK(ID3D11DeviceContext_Map(c, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &mp), "Map");
	memcpy(out, mp.pData, 3 * nout * 16);
	ID3D11DeviceContext_Unmap(c, (ID3D11Resource *)stg, 0);
done:
	ID3D11DeviceContext_ClearState(c);
	if (stg)
		ID3D11Buffer_Release(stg);
	if (sob)
		ID3D11Buffer_Release(sob);
	if (cbf)
		ID3D11Buffer_Release(cbf);
	if (cbi)
		ID3D11Buffer_Release(cbi);
	if (vb)
		ID3D11Buffer_Release(vb);
	if (lay)
		ID3D11InputLayout_Release(lay);
	if (gs)
		ID3D11GeometryShader_Release(gs);
	if (vsh)
		ID3D11VertexShader_Release(vsh);
	return rc;
#undef CK
}

static int vs_main(env_t *e, const char *manifest)
{
	FILE *m = fopen(manifest, "r");
	if (!m)
		return 2;
	char line[4096];
	while (fgets(line, sizeof(line), m)) {
		char b9[512], c11[512], name[256], insp[512], outsp[512];
		unsigned seed;
		if (sscanf(line, "%511s %511s %u %255s %511s %511s", b9, c11, &seed, name, insp, outsp) != 6)
			continue;
		int iu[16], ii[16], ou[16], oi[16];
		const int nin = parse_spec(insp, iu, ii, 16), nout = parse_spec(outsp, ou, oi, 16);
		size_t s9, s11;
		uint8_t *x9 = load(b9, &s9), *x11 = load(c11, &s11);
		if (!x9 || !x11 || nin < 1 || nout < 1) {
			printf("%s ERRFILE\n", name);
			fflush(stdout);
			continue;
		}
		float vin[3 * 16 * 4], a[3 * 16 * 4], b[3 * 16 * 4];
		for (int v = 0; v < 3; v++)
			for (int k = 0; k < nin; k++)
				for (int c = 0; c < 4; c++)
					vin[(v * nin + k) * 4 + c] = iu[k] == 0 ? frand(seed, 5000 + v * 40 + k, c, -1.0f, 1.0f) : frand(seed, 5000 + v * 40 + k, c, 0.0f, 1.0f);
		for (int i = 0; i < 3 * 16 * 4; i++)
			a[i] = b[i] = -12345.0f;
		const char *step = "";
		HRESULT hr = 0;
		if (runvs9(e, x9, seed, nin, iu, ii, nout, ou, oi, vin, a, &step, &hr)) {
			printf("%s ERR9 %s %lx\n", name, step, (unsigned long)hr);
		} else if (runvs11(e, x11, s11, seed, nin, iu, ii, nout, ou, oi, vin, b, &step, &hr)) {
			printf("%s ERR11 %s %lx\n", name, step, (unsigned long)hr);
		} else {
			float worst = 0;
			int wi = -1;
			for (int i = 0; i < 3 * nout * 4; i++) {
				float diff = fabsf(a[i] - b[i]);
				float tol = 1e-3f + 1e-3f * fmaxf(fabsf(a[i]), fabsf(b[i]));
				if (isnan(a[i]) != isnan(b[i]))
					diff = 1e30f;
				if (diff > tol && diff - tol > worst) {
					worst = diff - tol;
					wi = i;
				}
			}
			if (wi >= 0)
				printf("%s DIFF %g vertex %d output %d.%d d3d9 %g d3d11 %g\n", name, worst, wi / (nout * 4), (wi / 4) % nout, wi % 4, a[wi], b[wi]);
			else
				printf("%s MATCH\n", name);
		}
		fflush(stdout);
		free(x9);
		free(x11);
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: sm3check <manifest>\n");
		return 2;
	}
	g_flat = getenv("SM3FLAT") != NULL;
	env_t e = {0};
	const int vs_mode = !strcmp(argv[1], "--vs") && argc > 2;
	if (!init(&e, vs_mode))
		return 1;
	if (vs_mode)
		return vs_main(&e, argv[2]);
	FILE *m = fopen(argv[1], "r");
	if (!m)
		return 2;
	char line[2048];
	while (fgets(line, sizeof(line), m)) {
		char ps9[512], ps11[512], vs9[512], vs11[512], name[256], kinds[64];
		unsigned seed;
		int nrt;
		if (sscanf(line, "%511s %511s %511s %511s %u %d %255s %63s", ps9, ps11, vs9, vs11, &seed, &nrt, name, kinds) != 8)
			continue;
		size_t s1, s2, s3, s4;
		uint8_t *b1 = load(ps9, &s1), *b2 = load(ps11, &s2), *b3 = load(vs9, &s3), *b4 = load(vs11, &s4);
		if (!b1 || !b2 || !b3 || !b4) {
			printf("%s ERRFILE\n", name);
			fflush(stdout);
			continue;
		}
		float a[16], b[16];
		for (int i = 0; i < 16; i++)
			a[i] = b[i] = -12345.0f;
		const char *step = "";
		HRESULT hr = 0;
		if (run9(&e, b1, s1, b3, s3, seed, nrt, kinds, a, &step, &hr)) {
			printf("%s ERR9 %s %lx\n", name, step, (unsigned long)hr);
		} else if (run11(&e, b2, s2, b4, s4, seed, nrt, kinds, b, &step, &hr)) {
			printf("%s ERR11 %s %lx\n", name, step, (unsigned long)hr);
		} else {
			float worst = 0;
			int wi = -1;
			int killed = 1;
			for (int i = 0; i < nrt * 4; i++) {
				if (a[i] != 0 || b[i] != 0)
					killed = 0;
				float diff = fabsf(a[i] - b[i]);
				float tol = 2e-3f + 2e-3f * fmaxf(fabsf(a[i]), fabsf(b[i]));
				if (isnan(a[i]) != isnan(b[i]))
					diff = 1e30f;
				if (diff > tol && diff - tol > worst) {
					worst = diff - tol;
					wi = i;
				}
			}
			if (wi >= 0)
				printf("%s DIFF %g rt%d.%d d3d9 %g d3d11 %g | rt0 d3d9 %g %g %g %g d3d11 %g %g %g %g\n", name, worst, wi / 4, wi % 4, a[wi], b[wi], a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
			else
				printf("%s %s\n", name, killed ? "KILL" : "MATCH");
		}
		fflush(stdout);
		free(b1);
		free(b2);
		free(b3);
		free(b4);
	}
	return 0;
}
