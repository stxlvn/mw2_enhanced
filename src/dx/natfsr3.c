// FSR 3 (AMD FidelityFX upscaler) on the native Direct3D 12 renderer ([display] fsr3=1 with directx12=1, fsr3_dir = the folder with AMD's amd_fidelityfx_loader_dx12.dll and its upscaler DLL).
//
// The picture the engine draws is at the game's size; when its 2D passes start (natboot.c hud_begin) nat_upscale_now calls natfsr3_upscale, which records on the renderer's own command list:
//   1. the game's depth (multisampled, not readable) is copied and converted to an R32_FLOAT texture, mapped to 0..1 over the world viewport's depth range (a compute pass);
//   2. the camera's motion vectors, from that depth and the matrix that takes this frame's clip space to the previous frame's (a compute pass);
//   3. FSR 3's dispatch with the color (the resolved scene), the depth, the vectors, the camera's sub-pixel jitter and the frame time;
//   4. the output is copied into the image the 2D passes draw into.
// The jitter is put into the world view-projection matrix when the engine uploads it (ng_vs_hook in natgfx.c, called with every write of the vertex constants c0..); the matrix without the jitter is
// what the vectors come from. This is the algorithm of src/fsr3.c (Direct3D 9 on 12) without its extras (motion refinement, the compose pass, DLSS, the QA rig): the HUD needs no compose pass here
// (it is drawn after the upscale, at the output size).
#define COBJMACROS
#include "nat12x.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "natgfx.h"
#include "../log.h"
#include "../settings.h"
#include "ffx_api.h"
#include "ffx_upscale.h"
#include "dx12/ffx_api_dx12.h"

static struct {
	int tried, ok;
	HMODULE dll;
	PfnFfxCreateContext create;
	PfnFfxDestroyContext destroy;
	PfnFfxDispatch dispatch;
	PfnFfxQuery query;
	ffxContext ctx;
	int phase_count;
	ID3D12Device *dev;
	UINT iw, ih, w, h; // render size, output size
	ID3D12Resource *out, *depth, *motion, *dcopy;
	UINT dcopy_w, dcopy_h, dcopy_samples;
	DXGI_FORMAT dcopy_fmt;
	int out_state, depth_state, motion_state, dcopy_state; // 0 = common (new), 1 = the working state
	ID3D12RootSignature *rs, *rs_mv;
	ID3D12PipelineState *pso_ms, *pso_1, *pso_mv;
	unsigned frame;
	LARGE_INTEGER last_tick, freq;
} S;

// the camera: this frame's and the previous frame's world view-projection (c0..c3 of the engine, clip_i = c_i . (x, y, z, 1)) without the jitter
static float g_new[16], g_prev[16];
static int g_new_ok, g_prev_ok;
static float g_minz, g_maxz;       // the depth range of the viewport the matrix came with
static float g_jit_x, g_jit_y;     // this frame's jitter in render pixels (what FSR asked for)
static unsigned g_frame_count;
static float g_cam_a, g_cam_bw;

static const char depth_cs_ms[] =
	"cbuffer C : register(b0) { float4 P; };\n"
	"Texture2DMS<float> src : register(t0);\n"
	"RWTexture2D<float> dst : register(u0);\n"
	"[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)\n"
	"{\n"
	"  uint w, h, s;\n"
	"  src.GetDimensions(w, h, s);\n"
	"  if (id.x >= w || id.y >= h) return;\n"
	"  dst[id.xy] = saturate((src.Load(int2(id.xy), 0) - P.x) * P.y);\n"
	"}\n";
static const char depth_cs_1[] =
	"cbuffer C : register(b0) { float4 P; };\n"
	"Texture2D<float> src : register(t0);\n"
	"RWTexture2D<float> dst : register(u0);\n"
	"[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)\n"
	"{\n"
	"  uint w, h;\n"
	"  src.GetDimensions(w, h);\n"
	"  if (id.x >= w || id.y >= h) return;\n"
	"  dst[id.xy] = saturate((src.Load(int3(id.xy, 0)) - P.x) * P.y);\n"
	"}\n";
// motion vectors of the camera (see src/fsr3.c): in render pixels, from the current pixel to where the same surface point was in the previous frame, without any jitter
static const char mv_cs[] =
	"cbuffer C : register(b0) { float4 T0; float4 T1; float4 T2; float4 T3; float4 P; float4 Q; };\n"
	"Texture2D<float> depth : register(t0);\n"
	"RWTexture2D<float2> mv : register(u0);\n"
	"[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)\n"
	"{\n"
	"  uint w, h;\n"
	"  depth.GetDimensions(w, h);\n"
	"  if (id.x >= w || id.y >= h) return;\n"
	"  float z = (depth.Load(int3(id.xy, 0)) - P.x) * P.y;\n"
	"  float2 ndc = float2((id.x + 0.5) / w * 2.0 - 1.0, 1.0 - (id.y + 0.5) / h * 2.0);\n"
	"  ndc.x -= 2.0 * P.z / w;\n"
	"  ndc.y += 2.0 * P.w / h;\n"
	"  float4 v = float4(ndc, z, 1.0);\n"
	"  float4 pc = v.x * T0 + v.y * T1 + v.z * T2 + v.w * T3;\n"
	"  float2 r = float2(0.0, 0.0);\n"
	"  if (pc.w > 1e-6 && z > 0.0) {\n"
	"    float2 pn = pc.xy / pc.w;\n"
	"    r = float2((pn.x - ndc.x) * 0.5 * w, -(pn.y - ndc.y) * 0.5 * h);\n"
	"  }\n"
	"  mv[id.xy] = r * Q.x;\n"
	"}\n";

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ DLSS ([display] dlss=1)
// NVIDIA's DLSS Super Resolution through mw2coop_dlss.dll (src/dlss: the mod's bridge, built apart from NVIDIA's SDK): the same inputs as FSR 3's (color, depth mapped to 0..1, motion vectors in
// render pixels without jitter, the jitter) and the same output; everything before and after it is shared with FSR 3. The output is sharpened by RCAS (nat12.c) as src/fsr3.c does.
typedef void (*dl_log_t)(const char *);
static struct {
	HMODULE dll;
	int (*init)(void *, const wchar_t *, const wchar_t *, dl_log_t);
	int (*create)(void *, unsigned, unsigned, unsigned, unsigned, unsigned);
	int (*eval)(void *, void *, void *, void *, void *, float, float, int, float);
	unsigned (*last)(void);
	void (*set_preset)(unsigned);
	int feature;
} DL;

static void dl_log(const char *m)
{
	log_printf("dlss: %s", m);
}

static int dlss_setup(void)
{
	const char *dir = settings_dlss_dir();
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%s\\mw2coop_dlss.dll", dir);
	SetDllDirectoryA(dir);
	DL.dll = LoadLibraryA(path);
	SetDllDirectoryA(NULL);
	if (!DL.dll) {
		log_printf("dlss: %s could not be loaded (error %lu): set [display] dlss_dir to the folder with mw2coop_dlss.dll and NVIDIA's nvngx_dlss.dll (src/dlss/README.md)", path, GetLastError());
		return 0;
	}
	DL.init = (int (*)(void *, const wchar_t *, const wchar_t *, dl_log_t))(void *)GetProcAddress(DL.dll, "dlss_bridge_init");
	DL.create = (int (*)(void *, unsigned, unsigned, unsigned, unsigned, unsigned))(void *)GetProcAddress(DL.dll, "dlss_bridge_create");
	DL.eval = (int (*)(void *, void *, void *, void *, void *, float, float, int, float))(void *)GetProcAddress(DL.dll, "dlss_bridge_evaluate");
	DL.last = (unsigned (*)(void))(void *)GetProcAddress(DL.dll, "dlss_bridge_last_result");
	DL.set_preset = (void (*)(unsigned))(void *)GetProcAddress(DL.dll, "dlss_bridge_set_preset");
	if (!DL.init || !DL.create || !DL.eval) {
		log_printf("dlss: %s lacks the bridge functions", path);
		return 0;
	}
	wchar_t wdir[MAX_PATH] = L"", wdata[MAX_PATH] = L"";
	MultiByteToWideChar(CP_ACP, 0, dir, -1, wdir, MAX_PATH);
	GetTempPathW(MAX_PATH, wdata);
	if (!DL.init(S.dev, wdir, wdata, dl_log)) {
		log_printf("dlss: NVIDIA's NGX could not be started or DLSS is not available on this GPU or driver (a GeForce RTX card and a current driver are needed); see the lines above");
		return 0;
	}
	log_printf("dlss: NGX is up (%s)", path);
	return 1;
}

static int dlss_dispatch(ID3D12GraphicsCommandList *l, ID3D12Resource *color, float jx, float jy, int reset, float dt_ms)
{
	static int bad;
	if (!DL.feature) {
		const char *want = settings_dlss_preset();
		const char c = want && want[0] ? (char)(want[0] & ~0x20) : 0;
		const unsigned preset = c >= 'J' && c <= 'M' ? (unsigned)(c - 'A' + 1) : 0;
		if (DL.set_preset)
			DL.set_preset(preset);
		if (!DL.create(l, S.iw, S.ih, S.w, S.h, 0)) {
			if (bad++ < 5)
				log_printf("dlss: the feature could not be created (NGX result 0x%08x)", DL.last ? DL.last() : 0u);
			return 0;
		}
		DL.feature = 1;
	}
	if (!DL.eval(l, color, S.depth, S.motion, S.out, jx, jy, reset, dt_ms)) {
		if (bad++ < 5)
			log_printf("dlss: evaluation failed (NGX result 0x%08x)", DL.last ? DL.last() : 0u);
		return 0;
	}
	return 1;
}

static void message(uint32_t type, const wchar_t *text)
{
	log_printf("natfsr3: ffx %s: %ls", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", text);
}

static struct FfxApiResource ffx_res(ID3D12Resource *r, UINT w, UINT h, uint32_t format, uint32_t usage, uint32_t state)
{
	struct FfxApiResource x;
	memset(&x, 0, sizeof(x));
	x.resource = r;
	x.state = state;
	x.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
	x.description.format = format;
	x.description.width = w;
	x.description.height = h;
	x.description.depth = 1;
	x.description.mipCount = 1;
	x.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
	x.description.usage = usage;
	return x;
}

static ID3D12Resource *make_texture(const D3D12_RESOURCE_DESC *rd)
{
	D3D12_HEAP_PROPERTIES hp;
	memset(&hp, 0, sizeof(hp));
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	ID3D12Resource *res = NULL;
	HRESULT hr = ID3D12Device_CreateCommittedResource(S.dev, &hp, D3D12_HEAP_FLAG_NONE, rd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&res);
	if (FAILED(hr))
		log_printf("natfsr3: texture %llux%u format %d: CreateCommittedResource hr %lx", (unsigned long long)rd->Width, rd->Height, (int)rd->Format, (unsigned long)hr);
	return SUCCEEDED(hr) ? res : NULL;
}

static ID3D12Resource *make_simple(UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags)
{
	D3D12_RESOURCE_DESC rd;
	memset(&rd, 0, sizeof(rd));
	rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rd.Width = w;
	rd.Height = h;
	rd.DepthOrArraySize = 1;
	rd.MipLevels = 1;
	rd.Format = fmt;
	rd.SampleDesc.Count = 1;
	rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	rd.Flags = flags;
	return make_texture(&rd);
}

typedef HRESULT(WINAPI *compile_t)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT, UINT, ID3DBlob **, ID3DBlob **);

static ID3D12PipelineState *make_compute_pso(compile_t compile, const char *src, ID3D12RootSignature *rs)
{
	ID3DBlob *code = NULL, *err = NULL;
	HRESULT hr = compile(src, strlen(src), NULL, NULL, NULL, "main", "cs_5_0", 0, 0, &code, &err);
	if (FAILED(hr) || !code) {
		log_printf("natfsr3: a compute shader did not compile (hr %lx): %.200s", (unsigned long)hr, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
		return NULL;
	}
	D3D12_COMPUTE_PIPELINE_STATE_DESC pd;
	memset(&pd, 0, sizeof(pd));
	pd.pRootSignature = rs;
	pd.CS.pShaderBytecode = ID3D10Blob_GetBufferPointer(code);
	pd.CS.BytecodeLength = ID3D10Blob_GetBufferSize(code);
	ID3D12PipelineState *pso = NULL;
	hr = ID3D12Device_CreateComputePipelineState(S.dev, &pd, &IID_ID3D12PipelineState, (void **)&pso);
	ID3D10Blob_Release(code);
	if (FAILED(hr))
		log_printf("natfsr3: CreateComputePipelineState hr %lx", (unsigned long)hr);
	return SUCCEEDED(hr) ? pso : NULL;
}

static ID3D12RootSignature *make_rs(int nconst)
{
	D3D12_DESCRIPTOR_RANGE ranges[2];
	memset(ranges, 0, sizeof(ranges));
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = 1;
	ranges[0].OffsetInDescriptorsFromTableStart = 0;
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[1].NumDescriptors = 1;
	ranges[1].OffsetInDescriptorsFromTableStart = 1;
	D3D12_ROOT_PARAMETER p[2];
	memset(p, 0, sizeof(p));
	p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	p[0].Constants.Num32BitValues = nconst;
	p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	p[1].DescriptorTable.NumDescriptorRanges = 2;
	p[1].DescriptorTable.pDescriptorRanges = ranges;
	p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	D3D12_ROOT_SIGNATURE_DESC rd;
	memset(&rd, 0, sizeof(rd));
	rd.NumParameters = 2;
	rd.pParameters = p;
	ID3DBlob *blob = NULL, *err = NULL;
	if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err)) || !blob)
		return NULL;
	ID3D12RootSignature *rs = NULL;
	HRESULT hr = ID3D12Device_CreateRootSignature(S.dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs);
	ID3D10Blob_Release(blob);
	return SUCCEEDED(hr) ? rs : NULL;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ the camera
static int invert4(const double *m, double *out)
{
	double a[4][8];
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++) {
			a[r][c] = m[r * 4 + c];
			a[r][4 + c] = r == c ? 1.0 : 0.0;
		}
	for (int col = 0; col < 4; col++) {
		int piv = col;
		for (int r = col + 1; r < 4; r++)
			if (fabs(a[r][col]) > fabs(a[piv][col]))
				piv = r;
		if (fabs(a[piv][col]) < 1e-30)
			return 0;
		if (piv != col)
			for (int c = 0; c < 8; c++) {
				double t = a[col][c];
				a[col][c] = a[piv][c];
				a[piv][c] = t;
			}
		const double inv = 1.0 / a[col][col];
		for (int c = 0; c < 8; c++)
			a[col][c] *= inv;
		for (int r = 0; r < 4; r++)
			if (r != col) {
				const double f = a[r][col];
				if (f != 0.0)
					for (int c = 0; c < 8; c++)
						a[r][c] -= f * a[col][c];
			}
	}
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
			out[r * 4 + c] = a[r][4 + c];
	return 1;
}

static void vp_to_matrix(const float *c, double *m)
{
	for (int r = 0; r < 4; r++)
		for (int i = 0; i < 4; i++)
			m[r * 4 + i] = c[i * 4 + r];
}

// vertical field of view, near and far of a world view-projection matrix (see src/fsr3.c camera_from_matrix); 0 when it is not a perspective one
static int camera_from_matrix(const float *m, float *vfov, float *znear, float *zfar)
{
	const float *r1 = m + 4, *r2 = m + 8, *r3 = m + 12;
	const float l1 = sqrtf(r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2]);
	const float l2 = sqrtf(r2[0] * r2[0] + r2[1] * r2[1] + r2[2] * r2[2]);
	const float l3 = sqrtf(r3[0] * r3[0] + r3[1] * r3[1] + r3[2] * r3[2]);
	if (l1 < 1e-6f || l2 < 1e-6f || l3 < 1e-6f)
		return 0;
	float a = l2 / l3;
	if (r2[0] * r3[0] + r2[1] * r3[1] + r2[2] * r3[2] < 0.0f)
		a = -a;
	const float bb = m[11] - a * m[15];
	g_cam_a = a;
	g_cam_bw = bb / l3;
	*vfov = 2.0f * atanf(l3 / l1);
	if (bb == 0.0f || a < 0.5f || a > 2.0f)
		return 0;
	*znear = -bb / (a * l3);
	*zfar = a > 1.00001f ? -bb / ((a - 1.0f) * l3) : 100000.0f;
	return *znear > 0.01f && *znear < 1000.0f && *zfar > *znear * 2.0f;
}

static int camera_pose(const float *m, double pos[3], double fwd[3])
{
	const float *r0 = m, *r1 = m + 4, *r3 = m + 12;
	const double a[3][3] = {{r0[0], r0[1], r0[2]}, {r1[0], r1[1], r1[2]}, {r3[0], r3[1], r3[2]}};
	const double b[3] = {-r0[3], -r1[3], -r3[3]};
	const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
	if (fabs(det) < 1e-12)
		return 0;
	for (int k = 0; k < 3; k++) {
		double c[3][3];
		memcpy(c, a, sizeof(c));
		for (int r = 0; r < 3; r++)
			c[r][k] = b[r];
		pos[k] = (c[0][0] * (c[1][1] * c[2][2] - c[1][2] * c[2][1]) - c[0][1] * (c[1][0] * c[2][2] - c[1][2] * c[2][0]) + c[0][2] * (c[1][0] * c[2][1] - c[1][1] * c[2][0])) / det;
	}
	const double l = sqrt((double)r3[0] * r3[0] + (double)r3[1] * r3[1] + (double)r3[2] * r3[2]);
	if (l < 1e-9)
		return 0;
	for (int k = 0; k < 3; k++)
		fwd[k] = r3[k] / l;
	return 1;
}

// a camera cut between two frames: FSR's history must not blend across it
static int camera_cut(const float *prev, const float *now, char *why, size_t why_size)
{
	double p0[3], f0[3], p1[3], f1[3];
	if (!camera_pose(prev, p0, f0) || !camera_pose(now, p1, f1))
		return 0;
	const double dx = p1[0] - p0[0], dy = p1[1] - p0[1], dz = p1[2] - p0[2];
	const double moved = sqrt(dx * dx + dy * dy + dz * dz);
	double cosang = f0[0] * f1[0] + f0[1] * f1[1] + f0[2] * f1[2];
	cosang = cosang > 1.0 ? 1.0 : cosang < -1.0 ? -1.0 : cosang;
	const double turned = acos(cosang) * 57.29578;
	float v0 = 0, v1 = 0, n0, n1, z0, z1;
	const int fov_ok = camera_from_matrix(prev, &v0, &n0, &z0) && camera_from_matrix(now, &v1, &n1, &z1) && v0 > 0.0f && v1 > 0.0f;
	const double zoom = fov_ok ? (v1 > v0 ? v1 / v0 : v0 / v1) : 1.0;
	if (moved > 200.0 || turned > 45.0 || zoom > 1.25) {
		snprintf(why, why_size, "camera cut: moved %.0f units, turned %.0f degrees, field of view %.1f -> %.1f degrees", moved, turned, v0 * 57.29578f, v1 * 57.29578f);
		return 1;
	}
	return 0;
}

static float halton(unsigned index, unsigned base)
{
	float f = 1.0f, r = 0.0f;
	while (index) {
		f /= (float)base;
		r += f * (float)(index % base);
		index /= base;
	}
	return r;
}

// the jitter of the next frame: the next point of FSR's sequence for this render / output size
static void next_jitter(void)
{
	g_jit_x = g_jit_y = 0.0f;
	if (!S.ok || S.phase_count <= 0)
		return;
	if (settings_dlss()) { // the Halton (2, 3) sequence, offsets in -0.5..0.5 render pixels, as FSR's
		const unsigned idx = S.frame % (unsigned)S.phase_count + 1;
		g_jit_x = halton(idx, 2) - 0.5f;
		g_jit_y = halton(idx, 3) - 0.5f;
		return;
	}
	if (!S.query)
		return;
	float x = 0.0f, y = 0.0f;
	struct ffxQueryDescUpscaleGetJitterOffset q;
	memset(&q, 0, sizeof(q));
	q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
	q.index = (int32_t)(S.frame % (unsigned)S.phase_count);
	q.phaseCount = S.phase_count;
	q.pOutX = &x;
	q.pOutY = &y;
	if (S.query(&S.ctx, &q.header) == FFX_API_RETURN_OK) {
		g_jit_x = x;
		g_jit_y = y;
	}
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ the engine's matrix upload
static float g_vp_w, g_vp_h; // the current viewport of the engine (render pixels), noted by ng_vp_note
static float g_vp_min, g_vp_max;

static void vp_hook(float x, float y, float w, float h, float minz, float maxz)
{
	(void)x;
	(void)y;
	g_vp_w = w;
	g_vp_h = h;
	g_vp_min = minz;
	g_vp_max = maxz;
}

// before every draw uploads its vertex constants: the world view-projection (c0..c3: a perspective matrix, uploaded while the viewport has the world's depth range) is remembered without the jitter
// for the motion vectors, and every 3D matrix (the weapon's camera-relative one too) gets the jitter in the copy that is uploaded (clip.xy += jitter * clip.w)
static int draw_hook(const float (*in)[4], float (*out)[4])
{
	if (!S.ok)
		return 0;
	{
		static unsigned calls, vp_ok, tr_ok;
		const float *dd = &in[0][0];
		calls++;
		vp_ok += (UINT)(g_vp_w + 0.5f) == S.iw && (UINT)(g_vp_h + 0.5f) == S.ih;
		tr_ok += fabsf(dd[12]) + fabsf(dd[13]) + fabsf(dd[14]) > 1e-3f;
		if (getenv("MW2COOP_FSR3LOG") && S.frame > 3000 && S.frame < 3003 && calls % 40 == 0) {
			log_printf("natfsr3: draw %u frame %u c0: %.4f %.4f %.4f %.4f | c1 %.4f %.4f %.4f %.4f | c2 %.4f %.4f %.4f %.4f | c3 %.4f %.4f %.4f %.4f | c4 %.4f %.4f %.4f %.4f | c5 %.4f %.4f %.4f %.4f | c7 %.4f %.4f %.4f %.4f", calls, S.frame, dd[0], dd[1], dd[2], dd[3], dd[4], dd[5], dd[6], dd[7], dd[8], dd[9], dd[10], dd[11], dd[12], dd[13], dd[14], dd[15], dd[16], dd[17], dd[18], dd[19], dd[20], dd[21], dd[22], dd[23], dd[28], dd[29], dd[30], dd[31]);
		}
		if (getenv("MW2COOP_FSR3LOG") && calls % 50000 == 0)
			log_printf("natfsr3: %u draws: viewport size ok %u, matrix with a view direction %u (last viewport %.0fx%.0f depth %.4f..%.4f)", calls, vp_ok, tr_ok, g_vp_w, g_vp_h, g_vp_min, g_vp_max);
	}
	if ((UINT)(g_vp_w + 0.5f) != S.iw || (UINT)(g_vp_h + 0.5f) != S.ih)
		return 0;
	const float *data = &in[0][0];
	if (!(fabsf(data[12]) + fabsf(data[13]) + fabsf(data[14]) > 1e-3f))
		return 0; // (a 2D matrix)
	if (fabsf(data[15]) > 1e-3f && g_vp_min > 0.01f) {
		memcpy(g_new, data, 64);
		g_new_ok = 1;
		g_minz = g_vp_min;
		g_maxz = g_vp_max;
	}
	float *o = &out[0][0];
	const float kx = 2.0f * g_jit_x / (float)S.iw;
	const float ky = -2.0f * g_jit_y / (float)S.ih;
	for (int i = 0; i < 4; i++) {
		o[i] += kx * o[12 + i];
		o[4 + i] += ky * o[12 + i];
	}
	return 1;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ set-up
static int setup(nat_device *d, UINT iw, UINT ih, UINT w, UINT h)
{
	if (S.ok)
		return 1;
	if (S.tried)
		return 0;
	S.tried = 1;
	S.dev = nat12_device(d);
	S.iw = iw;
	S.ih = ih;
	S.w = w;
	S.h = h;
	if (settings_dlss()) { // NVIDIA's DLSS instead of AMD's FSR 3: no AMD DLL, no FSR context; the passes are shared
		if (!dlss_setup())
			return 0;
		goto objects;
	}
	char path[MAX_PATH];
	const char *dir = settings_fsr3_dir();
	snprintf(path, sizeof(path), "%s\\amd_fidelityfx_loader_dx12.dll", dir);
	SetDllDirectoryA(dir);
	S.dll = LoadLibraryA(path);
	SetDllDirectoryA(NULL);
	if (!S.dll) {
		log_printf("natfsr3: %s could not be loaded (error %lu): set [display] fsr3_dir to the folder with AMD's FidelityFX DLLs", path, GetLastError());
		return 0;
	}
	S.create = (PfnFfxCreateContext)(void *)GetProcAddress(S.dll, "ffxCreateContext");
	S.destroy = (PfnFfxDestroyContext)(void *)GetProcAddress(S.dll, "ffxDestroyContext");
	S.dispatch = (PfnFfxDispatch)(void *)GetProcAddress(S.dll, "ffxDispatch");
	S.query = (PfnFfxQuery)(void *)GetProcAddress(S.dll, "ffxQuery");
	if (!S.create || !S.destroy || !S.dispatch) {
		log_printf("natfsr3: the loader DLL lacks the C API functions");
		return 0;
	}
	struct ffxCreateBackendDX12Desc backend;
	memset(&backend, 0, sizeof(backend));
	backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
	backend.device = S.dev;
	struct ffxCreateContextDescUpscale up;
	memset(&up, 0, sizeof(up));
	up.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
	up.header.pNext = &backend.header;
	up.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE | FFX_UPSCALE_ENABLE_DEPTH_INFINITE; // (MW2 projects to an infinite far plane)
	up.maxRenderSize.width = iw;
	up.maxRenderSize.height = ih;
	up.maxUpscaleSize.width = w;
	up.maxUpscaleSize.height = h;
	up.fpMessage = message;
	const ffxReturnCode_t rc = S.create(&S.ctx, &up.header, NULL);
	log_printf("natfsr3: ffxCreateContext (%ux%u to %ux%u) -> %u (0 = OK)", iw, ih, w, h, (unsigned)rc);
	if (rc != FFX_API_RETURN_OK)
		return 0;
objects:
	S.out = make_simple(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	S.depth = make_simple(iw, ih, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	S.motion = make_simple(iw, ih, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	HMODULE comp = LoadLibraryA("d3dcompiler_47.dll");
	compile_t compile = comp ? (compile_t)(void *)GetProcAddress(comp, "D3DCompile") : NULL;
	S.rs = make_rs(4);
	S.rs_mv = make_rs(24);
	if (!S.out || !S.depth || !S.motion || !compile || !S.rs || !S.rs_mv)
		return 0;
	S.pso_ms = make_compute_pso(compile, depth_cs_ms, S.rs);
	S.pso_1 = make_compute_pso(compile, depth_cs_1, S.rs);
	S.pso_mv = make_compute_pso(compile, mv_cs, S.rs_mv);
	if (!S.pso_ms || !S.pso_1 || !S.pso_mv)
		return 0;
	QueryPerformanceFrequency(&S.freq);
	S.ok = 1;
	if (settings_dlss()) { // DLSS asks for 8 * (output / render)^2 jitter phases (AMD's formula for FSR too)
		const float sc = (float)w / (float)iw;
		S.phase_count = (int)ceilf(8.0f * sc * sc);
	} else if (S.query) {
		int32_t count = 0;
		struct ffxQueryDescUpscaleGetJitterPhaseCount qp;
		memset(&qp, 0, sizeof(qp));
		qp.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
		qp.renderWidth = iw;
		qp.displayWidth = w;
		qp.pOutPhaseCount = &count;
		if (S.query(&S.ctx, &qp.header) == FFX_API_RETURN_OK && count > 0)
			S.phase_count = count;
	}
	next_jitter();
	ng_vp_hook = vp_hook;
	ng_draw_hook = draw_hook;
	log_printf("natfsr3: ready: render %ux%u -> output %ux%u (the game's depth, camera motion vectors, jitter phases %d)", iw, ih, w, h, S.phase_count);
	return 1;
}

static void barrier(D3D12_RESOURCE_BARRIER *b, int *n, ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	b[*n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b[*n].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	b[*n].Transition.pResource = r;
	b[*n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b[*n].Transition.StateBefore = before;
	b[*n].Transition.StateAfter = after;
	(*n)++;
}

// the game's depth into the R32_FLOAT texture: a copy (the engine's is not readable), then a compute pass that maps it to 0..1 over the viewport's depth range
static int depth_pass(nat_device *d, nat_image *depth_img)
{
	ID3D12GraphicsCommandList *l = nat12_list(d);
	ID3D12Resource *dsrc = nat12_image_res(depth_img);
	D3D12_RESOURCE_DESC drd = ID3D12Resource_GetDesc(dsrc);
	if (drd.Format != DXGI_FORMAT_D24_UNORM_S8_UINT && drd.Format != DXGI_FORMAT_R24G8_TYPELESS)
		return 0;
	if (!S.dcopy || S.dcopy_w != drd.Width || S.dcopy_h != drd.Height || S.dcopy_samples != drd.SampleDesc.Count || S.dcopy_fmt != drd.Format) {
		if (S.dcopy)
			ID3D12Resource_Release(S.dcopy);
		D3D12_RESOURCE_DESC cd = drd;
		cd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		S.dcopy = make_texture(&cd);
		S.dcopy_w = (UINT)drd.Width;
		S.dcopy_h = drd.Height;
		S.dcopy_samples = drd.SampleDesc.Count;
		S.dcopy_fmt = drd.Format;
		S.dcopy_state = 0;
		log_printf("natfsr3: the game's depth: %llux%u, %u samples: a readable copy %s", (unsigned long long)drd.Width, drd.Height, drd.SampleDesc.Count, S.dcopy ? "made" : "FAILED");
	}
	if (!S.dcopy)
		return 0;
	D3D12_RESOURCE_BARRIER b[4];
	int n = 0;
	nat12_image_transition(d, depth_img, D3D12_RESOURCE_STATE_COPY_SOURCE);
	barrier(b, &n, S.dcopy, S.dcopy_state ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
	barrier(b, &n, S.depth, S.depth_state ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ID3D12GraphicsCommandList_ResourceBarrier(l, n, b);
	ID3D12GraphicsCommandList_CopyResource(l, S.dcopy, dsrc);
	n = 0;
	barrier(b, &n, S.dcopy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	ID3D12GraphicsCommandList_ResourceBarrier(l, n, b);
	S.dcopy_state = 1;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu;
	D3D12_GPU_DESCRIPTOR_HANDLE gpu;
	if (!nat12_srv_alloc(d, 4, &cpu, &gpu))
		return 0;
	nat12_set_heaps(d);
	const UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(S.dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_SHADER_RESOURCE_VIEW_DESC srv;
	memset(&srv, 0, sizeof(srv));
	srv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	if (drd.SampleDesc.Count > 1) {
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
	} else {
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MipLevels = 1;
	}
	ID3D12Device_CreateShaderResourceView(S.dev, S.dcopy, &srv, cpu);
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav;
	memset(&uav, 0, sizeof(uav));
	uav.Format = DXGI_FORMAT_R32_FLOAT;
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	D3D12_CPU_DESCRIPTOR_HANDLE c1 = {cpu.ptr + inc};
	ID3D12Device_CreateUnorderedAccessView(S.dev, S.depth, NULL, &uav, c1);
	ID3D12GraphicsCommandList_SetComputeRootSignature(l, S.rs);
	ID3D12GraphicsCommandList_SetPipelineState(l, drd.SampleDesc.Count > 1 ? S.pso_ms : S.pso_1);
	float dvals[4] = {0.0f, 1.0f, 0.0f, 0.0f};
	if (g_maxz > g_minz) {
		dvals[0] = g_minz;
		dvals[1] = 1.0f / (g_maxz - g_minz);
	}
	ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(l, 0, 4, dvals, 0);
	ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(l, 1, gpu);
	ID3D12GraphicsCommandList_Dispatch(l, (S.iw + 7) / 8, (S.ih + 7) / 8, 1);
	n = 0;
	barrier(b, &n, S.depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	ID3D12GraphicsCommandList_ResourceBarrier(l, n, b);
	S.depth_state = 1;
	return 1;
}

// the camera's motion vectors: from the depth and the matrix that takes this frame's clip space to the previous frame's (row-vector convention)
static int motion_pass(nat_device *d)
{
	if (!g_new_ok || !g_prev_ok || !(g_maxz > g_minz))
		return 0;
	double mc[16], mp[16], ic[16], t[16];
	vp_to_matrix(g_new, mc);
	vp_to_matrix(g_prev, mp);
	if (!invert4(mc, ic))
		return 0;
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++) {
			double s = 0.0;
			for (int k = 0; k < 4; k++)
				s += ic[r * 4 + k] * mp[k * 4 + c];
			t[r * 4 + c] = s;
		}
	float vals[24] = {0};
	for (int i = 0; i < 16; i++)
		vals[i] = (float)t[i];
	vals[16] = 0.0f; // the depth texture is already mapped to 0..1
	vals[17] = 1.0f;
	vals[18] = g_jit_x;
	vals[19] = g_jit_y;
	vals[20] = 1.0f;
	ID3D12GraphicsCommandList *l = nat12_list(d);
	D3D12_RESOURCE_BARRIER b[2];
	int n = 0;
	barrier(b, &n, S.motion, S.motion_state ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ID3D12GraphicsCommandList_ResourceBarrier(l, n, b);
	D3D12_CPU_DESCRIPTOR_HANDLE cpu;
	D3D12_GPU_DESCRIPTOR_HANDLE gpu;
	if (!nat12_srv_alloc(d, 2, &cpu, &gpu))
		return 0;
	const UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(S.dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_SHADER_RESOURCE_VIEW_DESC srv;
	memset(&srv, 0, sizeof(srv));
	srv.Format = DXGI_FORMAT_R32_FLOAT;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	ID3D12Device_CreateShaderResourceView(S.dev, S.depth, &srv, cpu);
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav;
	memset(&uav, 0, sizeof(uav));
	uav.Format = DXGI_FORMAT_R16G16_FLOAT;
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	D3D12_CPU_DESCRIPTOR_HANDLE c1 = {cpu.ptr + inc};
	ID3D12Device_CreateUnorderedAccessView(S.dev, S.motion, NULL, &uav, c1);
	ID3D12GraphicsCommandList_SetComputeRootSignature(l, S.rs_mv);
	ID3D12GraphicsCommandList_SetPipelineState(l, S.pso_mv);
	ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(l, 0, 24, vals, 0);
	ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(l, 1, gpu);
	ID3D12GraphicsCommandList_Dispatch(l, (S.iw + 7) / 8, (S.ih + 7) / 8, 1);
	n = 0;
	barrier(b, &n, S.motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	ID3D12GraphicsCommandList_ResourceBarrier(l, n, b);
	S.motion_state = 1;
	return 1;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ the frame
int natfsr3_wanted(void)
{
	return settings_fsr3() && (settings_dlss() ? settings_dlss_dir() && settings_dlss_dir()[0] : settings_fsr3_dir() && settings_fsr3_dir()[0]);
}

int natfsr3_is_dlss(void)
{
	return settings_dlss() != 0;
}

// the upscale of the frame into `out_img` (the image of the output size the 2D passes draw into); 1 when it holds the upscaled picture
int natfsr3_upscale(nat_device *d, nat_image *color_img, nat_image *depth_img, nat_image *out_img)
{
	unsigned iw, ih, s, ow, oh;
	nat_image_size(color_img, &iw, &ih, &s);
	nat_image_size(out_img, &ow, &oh, &s);
	if (!setup(d, iw, ih, ow, oh) || iw != S.iw || ih != S.ih || ow != S.w || oh != S.h)
		return 0;
	if (!depth_img || !depth_pass(d, depth_img))
		return 0;
	const int mv_done = motion_pass(d);
	ID3D12GraphicsCommandList *l = nat12_list(d);
	D3D12_RESOURCE_BARRIER b[4];
	int n = 0;
	const int first = S.frame == 0;
	if (!mv_done && !S.motion_state) { // no vectors yet: the texture of zeros is read as it is
		barrier(b, &n, S.motion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		S.motion_state = 1;
	}
	if (!S.out_state) {
		barrier(b, &n, S.out, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		S.out_state = 1;
	}
	if (n)
		ID3D12GraphicsCommandList_ResourceBarrier(l, n, b);
	n = 0;
	nat12_image_transition(d, color_img, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	const float dt_ms = S.last_tick.QuadPart ? (float)((now.QuadPart - S.last_tick.QuadPart) * 1000.0 / (double)S.freq.QuadPart) : 16.6f;
	S.last_tick = now;
	struct ffxDispatchDescUpscale dd;
	memset(&dd, 0, sizeof(dd));
	dd.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
	dd.commandList = l;
	dd.color = ffx_res(nat12_image_res(color_img), S.iw, S.ih, FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dd.depth = ffx_res(S.depth, S.iw, S.ih, FFX_API_SURFACE_FORMAT_R32_FLOAT, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dd.motionVectors = ffx_res(S.motion, S.iw, S.ih, FFX_API_SURFACE_FORMAT_R16G16_FLOAT, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dd.output = ffx_res(S.out, S.w, S.h, FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
	dd.jitterOffset.x = g_jit_x;
	dd.jitterOffset.y = g_jit_y;
	dd.motionVectorScale.x = 1.0f;
	dd.motionVectorScale.y = 1.0f;
	dd.renderSize.width = S.iw;
	dd.renderSize.height = S.ih;
	dd.upscaleSize.width = S.w;
	dd.upscaleSize.height = S.h;
	{
		const int sh = settings_fsr3_sharpness();
		dd.enableSharpening = sh > 0;
		dd.sharpness = (float)sh / 100.0f;
	}
	dd.frameTimeDelta = dt_ms;
	dd.preExposure = 1.0f;
	{
		char why[160] = "first frame";
		int reset = first;
		if (!first) {
			if (!g_prev_ok) {
				reset = 1;
				snprintf(why, sizeof(why), "no 3D view in the frame before (menu, loading screen, film)");
			} else if (g_new_ok && camera_cut(g_prev, g_new, why, sizeof(why))) {
				reset = 1;
			}
		}
		dd.reset = reset;
		static unsigned resets;
		if (reset && !first && (++resets <= 20 || resets % 100 == 0))
			log_printf("natfsr3: history reset %u at frame %u: %s", resets, S.frame, why);
	}
	dd.cameraNear = 4.0f;
	dd.cameraFar = 40000.0f;
	dd.cameraFovAngleVertical = 1.0f;
	{
		float vfov = 0, zn = 0, zf = 0;
		if (g_new_ok && camera_from_matrix(g_new, &vfov, &zn, &zf)) {
			dd.cameraNear = zn;
			dd.cameraFar = zf;
			dd.cameraFovAngleVertical = vfov;
		}
	}
	dd.viewSpaceToMetersFactor = 0.0254f; // the game's units are inches
	dd.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
	ffxReturnCode_t rc;
	if (settings_dlss())
		rc = dlss_dispatch(l, nat12_image_res(color_img), g_jit_x, g_jit_y, dd.reset ? 1 : 0, dt_ms) ? FFX_API_RETURN_OK : FFX_API_RETURN_ERROR;
	else
		rc = S.dispatch(&S.ctx, &dd.header);
	static int dlog;
	if (dlog++ < 3 || rc != FFX_API_RETURN_OK)
		log_printf("natfsr3: %s -> %u (0 = OK), frame time %.1f ms, vectors %d, jitter %.3f,%.3f", settings_dlss() ? "DLSS evaluate" : "ffxDispatch", (unsigned)rc, dt_ms, mv_done, g_jit_x, g_jit_y);
	{ // a status line now and then: how many frames had vectors, resets
		static unsigned n_frames, n_mv, n_reset;
		n_frames++;
		n_mv += mv_done;
		n_reset += dd.reset;
		if (n_frames % 600 == 0)
			log_printf("natfsr3: %u upscaled frames, %u with camera vectors, %u history resets; jitter %.3f,%.3f", n_frames, n_mv, n_reset, g_jit_x, g_jit_y);
	}
	nat12_invalidate_state(d); // (FFX set its own descriptor heaps and root signatures on the list)
	if (rc != FFX_API_RETURN_OK)
		return 0;
	// the output into the image the 2D passes draw into
	barrier(b, &n, S.out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
	ID3D12GraphicsCommandList_ResourceBarrier(l, n, b);
	n = 0;
	nat12_image_transition(d, out_img, D3D12_RESOURCE_STATE_COPY_DEST);
	ID3D12GraphicsCommandList_CopyResource(l, nat12_image_res(out_img), S.out);
	barrier(b, &n, S.out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ID3D12GraphicsCommandList_ResourceBarrier(l, n, b);
	return 1;
}

// the end of the frame (the present): this frame's matrix is the next frame's previous one; a frame without a 3D view breaks the chain
void natfsr3_frame_end(void)
{
	if (!S.ok)
		return;
	if (g_new_ok) {
		memcpy(g_prev, g_new, sizeof(g_prev));
		g_prev_ok = 1;
	} else {
		g_prev_ok = 0;
	}
	g_new_ok = 0;
	S.frame++;
	g_frame_count++;
	next_jitter();
}
