// FSR 3 (AMD FidelityFX upscaler) on the game's frame, through Windows' Direct3D 9 on Direct3D 12 layer (settings.ini [display] d3d9on12, switched on by fsr3=1).
//
// What runs at each Present (after the mod's compositor, before the console):
//   1. the back buffer (a Direct3D 9 surface) is unwrapped to its Direct3D 12 resource (IDirect3DDevice9On12::UnwrapUnderlyingResource) and copied into a texture of ours;
//   2. FSR 3 is dispatched on a command list of ours with that texture as the color, a depth and a motion vector texture, a jitter, and an output texture;
//   3. the output is copied back into the back buffer, the resource is returned to Direct3D 9 with a fence, so Direct3D 9 waits for it on the GPU.
//
// STATE OF THE WORK: the frame path. The depth is a texture of zeros, the motion vectors are zero and there is no camera jitter, the render size is the output size (1:1): this is the
// plumbing proved, not yet an upscaler. The game's own depth (2 samples, not readable by shaders), the picture before the HUD, a jitter in the projection and motion vectors are
// what a real FSR 3 picture needs (docs/upscaling-plan.md).
//
// AMD's loader DLL is loaded at run time from fsr3_dir (the SDK's DLLs are not shipped with the mod: third_party/fidelityfx/README.md).
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "d3dcap.h"
#include "fsr3.h"
#include "log.h"
#include "settings.h"
#include "upscale.h"
#include "enhopts.h"

#include <d3dcompiler.h>
#include "ffx_api.h"
#include "ffx_upscale.h"
#include "dx12/ffx_api_dx12.h"

// getenv() walks the whole environment block on every call: the QA knobs of the per-frame code are read once (the environment does not change while the game runs)
#define ENV_HAS(name) __extension__({ static signed char has_ = -1; if (has_ < 0) has_ = getenv(name) != NULL; has_; })
#define ENV_NUM(name, def) __extension__({ static float num_; static signed char got_; if (!got_) { const char *e_ = getenv(name); num_ = e_ ? (float)atof(e_) : (def); got_ = 1; } num_; })

#define RING 3

static struct {
	int tried, ok;
	HMODULE dll;
	PfnFfxCreateContext create;
	PfnFfxDestroyContext destroy;
	PfnFfxDispatch dispatch;
	PfnFfxQuery query;
	int phase_count; // the jitter sequence length for this render/output size
	ffxContext ctx;
	ID3D12Device *dev;
	ID3D12CommandQueue *queue;
	ID3D12Fence *fence;
	UINT64 fence_value;
	HANDLE fence_event;
	ID3D12CommandAllocator *alloc[RING];
	UINT64 alloc_fence[RING];
	ID3D12GraphicsCommandList *list;
	ID3D12Resource *color, *output, *depth, *motion;
	// the depth pass: the game's depth (multisampled, not readable by shaders) is copied into a readable texture and converted to the R32_FLOAT texture `depth`
	ID3D12RootSignature *rs;
	ID3D12PipelineState *pso_ms, *pso_1;
	ID3D12RootSignature *rs_mv; // the motion vector pass: 20 root constants (reprojection matrix, depth range, jitter) + the depth SRV and motion UAV
	ID3D12PipelineState *pso_mv;
	ID3D12RootSignature *rs_comp; // the compose pass: FSR output + the game's own frame (for its HUD) -> final
	ID3D12PipelineState *pso_comp;
	ID3D12PipelineState *pso_sharp; // RCAS on DLSS's output (the compose pass's root signature: two textures, one output, four constants)
	ID3D12Resource *orig, *final;
	// motion refinement (moving characters, the weapon): the previous frame's input, the block vectors, the refined vectors
	ID3D12Resource *motion2, *prev, *block;
	ID3D12RootSignature *rs_rf;
	ID3D12PipelineState *pso_rfa, *pso_rfb;
	int prev_valid, m2_ok, block_ok, prev_ok;
	ID3D12DescriptorHeap *heap;
	UINT desc_inc;
	ID3D12Resource *dcopy;
	UINT dcopy_w, dcopy_h, dcopy_samples;
	int depth_state_ok; // the R32_FLOAT texture: 0 = common (first frame), 1 = read state
	int dcopy_state_ok;
	int depth_log;
	UINT w, h;   // the output (the back buffer)
	UINT iw, ih; // the render size: what the game draws, in the top left of the back buffer
	unsigned frame;
	LARGE_INTEGER last_tick;
	LARGE_INTEGER freq;
} S;

typedef struct {
	HRESULT(STDMETHODCALLTYPE *qi)(void *, const GUID *, void **);
	ULONG(STDMETHODCALLTYPE *addref)(void *);
	ULONG(STDMETHODCALLTYPE *release)(void *);
	HRESULT(STDMETHODCALLTYPE *get_device)(void *, const GUID *, void **);
	HRESULT(STDMETHODCALLTYPE *unwrap)(void *, IDirect3DResource9 *, IUnknown *, const GUID *, void **);
	HRESULT(STDMETHODCALLTYPE *give_back)(void *, IDirect3DResource9 *, UINT, UINT64 *, ID3D12Fence **);
} on12_vtbl_t;

static const GUID iid_on12 = {0xe7fda234, 0xb589, 0x4049, {0x94, 0x0d, 0x88, 0x78, 0x97, 0x75, 0x31, 0xc8}};
static const GUID iid_resource = {0x696442be, 0xa72e, 0x4059, {0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad}};
static const GUID iid_fence = {0x0a753dcf, 0xc4d8, 0x4b91, {0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76}};
static const GUID iid_allocator = {0x6102dee4, 0xaf59, 0x4b09, {0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24}};
static const GUID iid_list = {0x5b160d0f, 0xac1b, 0x4185, {0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55}};

int fsr3_enabled(void)
{
	return settings_fsr3();
}

static void message(uint32_t type, const wchar_t *text)
{
	log_printf("fsr3: ffx %s: %ls", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", text);
}

static ID3D12Resource *make_texture_ms(UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, UINT samples)
{
	static const GUID iid = {0x696442be, 0xa72e, 0x4059, {0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad}};
	D3D12_HEAP_PROPERTIES hp;
	D3D12_RESOURCE_DESC rd;
	memset(&hp, 0, sizeof(hp));
	memset(&rd, 0, sizeof(rd));
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rd.Width = w;
	rd.Height = h;
	rd.DepthOrArraySize = 1;
	rd.MipLevels = 1;
	rd.Format = fmt;
	rd.SampleDesc.Count = samples;
	rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	rd.Flags = flags;
	ID3D12Resource *res = NULL;
	HRESULT hr = ID3D12Device_CreateCommittedResource(S.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, NULL, &iid, (void **)&res);
	if (FAILED(hr))
		log_printf("fsr3: texture %ux%u format %d: CreateCommittedResource hr %lx", w, h, (int)fmt, (unsigned long)hr);
	return SUCCEEDED(hr) ? res : NULL;
}

static ID3D12Resource *make_texture(UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags)
{
	return make_texture_ms(w, h, fmt, flags, 1);
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

static void wait_fence(UINT64 value)
{
	if (ID3D12Fence_GetCompletedValue(S.fence) < value) {
		ID3D12Fence_SetEventOnCompletion(S.fence, value, S.fence_event);
		WaitForSingleObject(S.fence_event, 2000);
	}
}

static int build_depth_pass(void);
static int build_compose_pass(void);
static void next_jitter(void);

// ---------------------------------------------------------------------------------------------------- the DLSS backend ([display] dlss=1)
// NVIDIA's DLSS Super Resolution, called through mw2coop_dlss.dll (src/dlss: our bridge, built apart from NVIDIA's SDK, whose license does not allow it into this open source repository).
// It takes the place of FSR's dispatch with the same inputs (the color at render size, the game's depth mapped to 0..1, the motion vectors in render pixels without jitter, the jitter)
// and the same output; everything before and after it (the camera, the jitter in the projection, the depth and vector passes, the history resets) is shared with FSR 3.
typedef void (*dl_log_t)(const char *);
static struct {
	HMODULE dll;
	int (*init)(void *, const wchar_t *, const wchar_t *, dl_log_t);
	int (*create)(void *, unsigned, unsigned, unsigned, unsigned, unsigned);
	int (*eval)(void *, void *, void *, void *, void *, float, float, int, float);
	unsigned (*last)(void);
	void (*set_preset)(unsigned); // (a bridge DLL built before 2026-11-11 does not have it)
	int feature; // the DLSS feature exists for S.iw x S.ih -> S.w x S.h
} DL;

static void dl_log(const char *m)
{
	log_printf("dlss: %s", m);
}

// the render size: what the game draws (the top left of the back buffer), or the output size when it asked for that itself (anti-aliasing only)
static void pick_render_size(UINT w, UINT h)
{
	int rw = 0, rh = 0;
	if (!upscale_internal_size(&rw, &rh) || rw <= 0 || rh <= 0 || (UINT)rw > w || (UINT)rh > h) {
		rw = (int)w;
		rh = (int)h;
	}
	S.iw = (UINT)rw;
	S.ih = (UINT)rh;
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

// one DLSS evaluation on the open command list (the feature is created at the first one); 1 = ok
static int dlss_dispatch(ID3D12Resource *motion, float jx, float jy, int reset, float dt_ms)
{
	static int bad;
	if (!DL.feature) {
		{ // the DLSS model: [display] dlss_preset (a letter: J, K, L, M; empty = NVIDIA's default), QA: DLSS_PRESET
			const char *env = getenv("DLSS_PRESET");
			const char *want = env ? env : settings_dlss_preset();
			const char c = want && want[0] ? (char)(want[0] & ~0x20) : 0;
			const unsigned preset = c >= 'J' && c <= 'M' ? (unsigned)(c - 'A' + 1) : 0;
			if (preset && !DL.set_preset)
				log_printf("dlss: dlss_preset=%c is ignored: this mw2coop_dlss.dll is older than the setting (rebuild it: src/dlss/build_dlss.sh)", c);
			else if (DL.set_preset)
				DL.set_preset(preset);
		}
		if (!DL.create(S.list, S.iw, S.ih, S.w, S.h, 0)) {
			if (bad++ < 5)
				log_printf("dlss: the feature could not be created (NGX result 0x%08x)", DL.last ? DL.last() : 0u);
			return 0;
		}
		DL.feature = 1;
	}
	if (!DL.eval(S.list, S.color, S.depth, motion, S.output, jx, jy, reset, dt_ms)) {
		if (bad++ < 5)
			log_printf("dlss: evaluation failed (NGX result 0x%08x)", DL.last ? DL.last() : 0u);
		return 0;
	}
	return 1;
}

// the DLL, the context and the objects that stay; w x h: the size of the back buffer
static int setup(UINT w, UINT h)
{
	if (S.ok)
		return 1;
	if (S.tried)
		return 0;
	S.tried = 1;
	S.dev = (ID3D12Device *)d3dcap_d3d12_device();
	S.queue = (ID3D12CommandQueue *)d3dcap_d3d12_queue();
	if (!S.dev || !S.queue) {
		log_printf("fsr3: no Direct3D 12 device or queue (Direct3D 9 is not running on Direct3D 12): nothing to do");
		return 0;
	}
	if (settings_dlss()) { // NVIDIA's DLSS instead of AMD's FSR 3: no AMD DLL, no FSR context; the rest of the set-up (command list, textures, passes) is shared
		pick_render_size(w, h);
		if (!dlss_setup())
			return 0;
		goto device_objects;
	}
	char path[MAX_PATH];
	const char *dir = settings_fsr3_dir();
	snprintf(path, sizeof(path), "%s\\amd_fidelityfx_loader_dx12.dll", dir);
	SetDllDirectoryA(dir); // the loader finds the upscaler DLL next to itself
	S.dll = LoadLibraryA(path);
	SetDllDirectoryA(NULL);
	if (!S.dll) {
		log_printf("fsr3: %s could not be loaded (error %lu): set [display] fsr3_dir to the folder with AMD's FidelityFX DLLs", path, GetLastError());
		return 0;
	}
	S.create = (PfnFfxCreateContext)(void *)GetProcAddress(S.dll, "ffxCreateContext");
	S.destroy = (PfnFfxDestroyContext)(void *)GetProcAddress(S.dll, "ffxDestroyContext");
	S.dispatch = (PfnFfxDispatch)(void *)GetProcAddress(S.dll, "ffxDispatch");
	S.query = (PfnFfxQuery)(void *)GetProcAddress(S.dll, "ffxQuery");
	if (!S.create || !S.destroy || !S.dispatch) {
		log_printf("fsr3: the loader DLL lacks the C API functions");
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
	up.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
	// MW2 projects to an infinite far plane (A = 0.9995, camera_from_matrix): AMD's docs ask for this flag then, or FSR reconstructs the distances of what it keeps or drops from its
	// history wrongly (FSR3_FINITE=1: without it, as before)
	if (!getenv("FSR3_FINITE"))
		up.flags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
	if (getenv("FSR3_DEBUGVIEW")) // QA: FSR 3 draws its own debug views into the output (proves that the output is what is shown)
		up.flags |= FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION | FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
	pick_render_size(w, h);
	up.maxRenderSize.width = S.iw;
	up.maxRenderSize.height = S.ih;
	up.maxUpscaleSize.width = w;
	up.maxUpscaleSize.height = h;
	up.fpMessage = message;
	ffxReturnCode_t rc = S.create(&S.ctx, &up.header, NULL);
	log_printf("fsr3: ffxCreateContext (%ux%u to %ux%u) -> %u (0 = OK)", S.iw, S.ih, w, h, (unsigned)rc);
	if (rc != FFX_API_RETURN_OK)
		return 0;
device_objects:
	if (FAILED(ID3D12Device_CreateFence(S.dev, 0, D3D12_FENCE_FLAG_NONE, &iid_fence, (void **)&S.fence)))
		return 0;
	S.fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
	for (int i = 0; i < RING; i++)
		if (FAILED(ID3D12Device_CreateCommandAllocator(S.dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &iid_allocator, (void **)&S.alloc[i])))
			return 0;
	if (FAILED(ID3D12Device_CreateCommandList(S.dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, S.alloc[0], NULL, &iid_list, (void **)&S.list)))
		return 0;
	ID3D12GraphicsCommandList_Close(S.list);
	S.color = make_texture(S.iw, S.ih, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
	S.output = make_texture(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	S.depth = make_texture(S.iw, S.ih, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	S.motion = make_texture(S.iw, S.ih, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	S.orig = make_texture(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
	S.final = make_texture(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	if (!S.color || !S.output || !S.depth || !S.motion || !S.orig || !S.final)
		return 0;
	S.motion2 = make_texture(S.iw, S.ih, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	S.prev = make_texture(S.iw, S.ih, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
	S.block = make_texture((S.iw + 7) / 8, (S.ih + 7) / 8, DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	S.w = w;
	S.h = h;
	build_depth_pass();
	QueryPerformanceFrequency(&S.freq);
	S.ok = 1;
	if (settings_dlss()) { // DLSS asks for 8 * (output / render)^2 jitter phases (AMD's formula for FSR too)
		const float sc = (float)w / (float)S.iw;
		S.phase_count = (int)ceilf(8.0f * sc * sc);
	} else if (S.query) {
		int32_t count = 0;
		struct ffxQueryDescUpscaleGetJitterPhaseCount qp;
		memset(&qp, 0, sizeof(qp));
		qp.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
		qp.renderWidth = S.iw;
		qp.displayWidth = w;
		qp.pOutPhaseCount = &count;
		if (S.query(&S.ctx, &qp.header) == FFX_API_RETURN_OK && count > 0)
			S.phase_count = count;
	}
	next_jitter();
	log_printf("fsr3: ready: render %ux%u -> output %ux%u (the game's depth, camera motion vectors, jitter phases %d)", S.iw, S.ih, w, h, S.phase_count);
	return 1;
}


// ---------------------------------------------------------------------------------------------------- the depth pass
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

// motion vectors of the camera: from the depth, the reprojection matrix T (current clip space -> previous clip space, row-vector convention) and the jitter of this frame. The result is in render
// pixels, from the current pixel to where the same surface point was in the previous frame, without any jitter (as FSR wants it).
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

typedef HRESULT(WINAPI *compile_t)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT, UINT, void **, void **);
typedef HRESULT(WINAPI *serialize_rs_t)(const D3D12_ROOT_SIGNATURE_DESC *, int, void **, void **);

static ID3D12PipelineState *make_compute_pso(compile_t compile, const char *src, ID3D12RootSignature *rs)
{
	static const GUID iid_pso = {0x765a30f3, 0xf624, 0x4c6f, {0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45}};
	void *code = NULL, *err = NULL;
	HRESULT hr = compile(src, strlen(src), NULL, NULL, NULL, "main", "cs_5_0", 0, 0, &code, &err);
	if (FAILED(hr) || !code) {
		log_printf("fsr3: the depth shader did not compile (hr %lx): %.200s", (unsigned long)hr, err ? (const char *)((void *(*)(void *))(*(void ***)err)[3])(err) : "");
		return NULL;
	}
	D3D12_COMPUTE_PIPELINE_STATE_DESC pd;
	memset(&pd, 0, sizeof(pd));
	pd.pRootSignature = rs;
	pd.CS.pShaderBytecode = ((void *(*)(void *))(*(void ***)code)[3])(code);
	pd.CS.BytecodeLength = ((SIZE_T(*)(void *))(*(void ***)code)[4])(code);
	ID3D12PipelineState *pso = NULL;
	hr = ID3D12Device_CreateComputePipelineState(S.dev, &pd, &iid_pso, (void **)&pso);
	((ULONG(*)(void *))(*(void ***)code)[2])(code);
	if (FAILED(hr))
		log_printf("fsr3: CreateComputePipelineState hr %lx", (unsigned long)hr);
	return SUCCEEDED(hr) ? pso : NULL;
}


// Motion refinement. The camera vectors move every pixel as if it were part of the static world; a walking soldier or the weapon is not, and FSR then blends the picture with the wrong place of the
// previous one (smear, blocks, "grain"). Pass A (one thread per 8x8 block): where the previous picture, moved by the camera vector, does not match the current one, search the previous picture for the
// block (+-16 px, then one pixel finer) and keep the vector when it matches clearly better. Pass B (per pixel): of the camera vector and the vectors of the 3x3 neighbouring blocks, take the one
// whose 3x3 window of the current picture is best explained by the previous one. Compared in luma, with the difference of the two frames' jitter taken into account.
//
// Strict mode (the default; owner, 2026-11-08: "the picture ripples in the distance", "still blurry"; other projects that guess motion from the picture report the same on fine detail,
// see the notes): the picture alone cannot tell motion from the jitter's aliasing on distant fine detail, so a vector is only taken where all of these hold: the nearest surface of the
// block is nearer than `dgate` (the depth buffer value of a distance, ~75 m: farther away a walking soldier moves under a pixel a frame anyway), the match is clearly better than the
// camera's (`ratio`) and good in itself (`absmax`), and the vector differs from the camera's by at least `minmag` pixels (no sub-pixel vectors). Classic mode: dgate 2, ratio 0.6, no
// absolute limit, no minimum, quarter-pixel refinement.
#define HLSL(...) #__VA_ARGS__
static const char rf_common[] = HLSL(
	cbuffer C : register(b0) { uint iw; uint ih; float jcx; float jcy; float jpx; float jpy; float thr; float pad; float dgate; float ratio; float absmax; float minmag; };
	Texture2D<float4> cur : register(t0);
	Texture2D<float4> prv : register(t1);
	Texture2D<float2> cam : register(t2);
	Texture2D<float> dep : register(t3);
	RWTexture2D<uint> blk : register(u0);
	RWTexture2D<float2> outmv : register(u1);
	float nearest(int2 org)
	{
		float d = 2.0;
		for (int y = 0; y < 8; y += 4)
			for (int x = 0; x < 8; x += 4)
				d = min(d, dep.Load(int3(clamp(org + int2(x, y), int2(0, 0), int2(iw - 1, ih - 1)), 0)));
		return d;
	}
	float lum(float4 c) { return dot(c.rgb, float3(0.299, 0.587, 0.114)); }
	float curL(int2 p) { p = clamp(p, int2(0, 0), int2(iw - 1, ih - 1)); return lum(cur.Load(int3(p, 0))); }
	float prvL(int2 p) { p = clamp(p, int2(0, 0), int2(iw - 1, ih - 1)); return lum(prv.Load(int3(p, 0))); }
	float prvB(float2 pos)
	{
		float2 f = pos - 0.5;
		int2 i = int2(floor(f));
		float2 w = f - float2(i);
		float a = lerp(prvL(i), prvL(i + int2(1, 0)), w.x);
		float b = lerp(prvL(i + int2(0, 1)), prvL(i + int2(1, 1)), w.x);
		return lerp(a, b, w.y);
	}
);
static const char rf_a_main[] = HLSL(
	float sad(int2 org, int2 off)
	{
		float s = 0.0;
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++) {
				int2 p = org + int2(x, y);
				s += abs(curL(p) - prvL(p + off));
			}
		return s / 64.0;
	}
	float sadb(int2 org, float2 mv)
	{
		float s = 0.0;
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++) {
				int2 p = org + int2(x, y);
				s += abs(curL(p) - prvB(float2(p) + 0.5 + mv));
			}
		return s / 64.0;
	}
	float sad4(int2 org, int2 off)
	{
		float s = 0.0;
		for (int y = 0; y < 8; y += 2)
			for (int x = 0; x < 8; x += 2) {
				int2 p = org + int2(x, y);
				s += abs(curL(p) - prvL(p + off));
			}
		return s / 16.0;
	}
	[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)
	{
		uint bw = (iw + 7) / 8;
		uint bh = (ih + 7) / 8;
		if (id.x >= bw || id.y >= bh) return;
		int2 org = int2(id.xy) * 8;
		float2 jd = float2(jpx - jcx, jpy - jcy);
		float2 mvc = cam.Load(int3(min(org + 4, int2(iw - 1, ih - 1)), 0));
		int2 base = int2(round(mvc + jd));
		uint res = 0xFFFFFFFF;
		if (nearest(org) >= dgate) { blk[id.xy] = res; return; }
		float sad0 = sadb(org, mvc + jd);
		if (sad0 > thr) {
			float best = sad4(org, base);
			int2 bestr = int2(0, 0);
			for (int dy = -16; dy <= 16; dy += 4)
				for (int dx = -16; dx <= 16; dx += 4) {
					float c = sad4(org, base + int2(dx, dy)) + 0.0004 * (abs(dx) + abs(dy));
					if (c < best) { best = c; bestr = int2(dx, dy); }
				}
			best = sad(org, base + bestr);
			for (int st = 2; st >= 1; st--) {
				int2 c0 = bestr;
				for (int ey = -1; ey <= 1; ey++)
					for (int ex = -1; ex <= 1; ex++) {
						int2 r = c0 + int2(ex, ey) * st;
						float c = sad(org, base + r);
						if (c < best) { best = c; bestr = r; }
					}
			}
			if (best < ratio * sad0 && best < absmax) {
				float2 cand = float2(base + bestr) - jd;
				float2 dv = abs(cand - mvc);
				if (max(dv.x, dv.y) >= minmag)
					res = f32tof16(cand.x) | (f32tof16(cand.y) << 16);
			}
		}
		blk[id.xy] = res;
	}
);
static const char rf_b_main[] = HLSL(
	float err(int2 p, float2 mv, float2 jd)
	{
		float e = 0.0;
		for (int y = -1; y <= 1; y++)
			for (int x = -1; x <= 1; x++) {
				int2 q = p + int2(x, y);
				e += abs(curL(q) - prvB(float2(q) + 0.5 + mv + jd));
			}
		return e / 9.0;
	}
	[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)
	{
		if (id.x >= iw || id.y >= ih) return;
		float2 jd = float2(jpx - jcx, jpy - jcy);
		float2 mvc = cam.Load(int3(id.xy, 0));
		float2 best = mvc;
		if (dep.Load(int3(id.xy, 0)) >= dgate) { outmv[id.xy] = mvc; return; }
		uint bw = (iw + 7) / 8;
		uint bh = (ih + 7) / 8;
		int2 b = int2(id.xy / 8);
		bool any = false;
		for (int qy = -1; qy <= 1; qy++)
			for (int qx = -1; qx <= 1; qx++)
				if (blk[clamp(b + int2(qx, qy), int2(0, 0), int2(bw - 1, bh - 1))] != 0xFFFFFFFF) any = true;
		if (!any) { outmv[id.xy] = mvc; return; }
		float be = err(int2(id.xy), mvc, jd);
		bool dyn = false;
		if (be > thr * 0.5) {
			for (int oy = -1; oy <= 1; oy++)
				for (int ox = -1; ox <= 1; ox++) {
					int2 bb = clamp(b + int2(ox, oy), int2(0, 0), int2(bw - 1, bh - 1));
					uint v = blk[bb];
					if (v != 0xFFFFFFFF) {
						float2 cand = float2(f16tof32(v & 0xFFFF), f16tof32(v >> 16));
						float e = err(int2(id.xy), cand, jd);
						if (e < be * 0.75 - 0.002 && e < absmax) { be = e; best = cand; dyn = true; }
					}
				}
			if (dyn && pad > 0.5) {
				float step = 0.5;
				for (int it = 0; it < 2; it++) {
					float2 c = best;
					for (int sy = -1; sy <= 1; sy++)
						for (int sx = -1; sx <= 1; sx++) {
							if (sx == 0 && sy == 0) continue;
							float2 cand = c + float2(sx, sy) * step;
							float e = err(int2(id.xy), cand, jd);
							if (e < be) { be = e; best = cand; }
						}
					step *= 0.5;
				}
			}
		}
		outmv[id.xy] = best;
	}
);

static int build_refine_pass(compile_t compile, serialize_rs_t serialize)
{
	static const GUID iid_rs2 = {0xc54a6b66, 0x72df, 0x4ee8, {0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14}};
	D3D12_DESCRIPTOR_RANGE ranges[2];
	memset(ranges, 0, sizeof(ranges));
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = 4; // t0 this frame, t1 the previous frame, t2 the camera vectors, t3 the depth
	ranges[0].OffsetInDescriptorsFromTableStart = 0;
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[1].NumDescriptors = 2;
	ranges[1].OffsetInDescriptorsFromTableStart = 4;
	D3D12_ROOT_PARAMETER rp[2];
	memset(rp, 0, sizeof(rp));
	rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	rp[0].Constants.ShaderRegister = 0;
	rp[0].Constants.Num32BitValues = 12;
	rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rp[1].DescriptorTable.NumDescriptorRanges = 2;
	rp[1].DescriptorTable.pDescriptorRanges = ranges;
	rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	D3D12_ROOT_SIGNATURE_DESC rd;
	memset(&rd, 0, sizeof(rd));
	rd.NumParameters = 2;
	rd.pParameters = rp;
	void *blob = NULL, *err = NULL;
	if (FAILED(serialize(&rd, 1, &blob, &err)) || !blob)
		return 0;
	HRESULT hr = ID3D12Device_CreateRootSignature(S.dev, 0, ((void *(*)(void *))(*(void ***)blob)[3])(blob), ((SIZE_T(*)(void *))(*(void ***)blob)[4])(blob), &iid_rs2, (void **)&S.rs_rf);
	((ULONG(*)(void *))(*(void ***)blob)[2])(blob);
	if (FAILED(hr))
		return 0;
	static char src[16384];
	snprintf(src, sizeof(src), "%s\n%s", rf_common, rf_a_main);
	S.pso_rfa = make_compute_pso(compile, src, S.rs_rf);
	snprintf(src, sizeof(src), "%s\n%s", rf_common, rf_b_main);
	S.pso_rfb = make_compute_pso(compile, src, S.rs_rf);
	log_printf("fsr3: the motion refinement passes are %s", S.pso_rfa && S.pso_rfb ? "ready" : "FAILED");
	return S.pso_rfa && S.pso_rfb;
}

static int build_depth_pass(void)
{
	static const GUID iid_rs = {0xc54a6b66, 0x72df, 0x4ee8, {0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14}};
	static const GUID iid_heap = {0x8efb471d, 0x616c, 0x4f49, {0x90, 0xf7, 0x12, 0x7b, 0xb7, 0x63, 0xfa, 0x51}};
	HMODULE comp = LoadLibraryA("d3dcompiler_47.dll"), d12 = GetModuleHandleA("d3d12.dll");
	compile_t compile = comp ? (compile_t)(void *)GetProcAddress(comp, "D3DCompile") : NULL;
	serialize_rs_t serialize = d12 ? (serialize_rs_t)(void *)GetProcAddress(d12, "D3D12SerializeRootSignature") : NULL;
	if (!compile || !serialize) {
		log_printf("fsr3: no d3dcompiler_47 or D3D12SerializeRootSignature: no depth");
		return 0;
	}
	D3D12_DESCRIPTOR_RANGE ranges[2];
	memset(ranges, 0, sizeof(ranges));
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = 1;
	ranges[0].OffsetInDescriptorsFromTableStart = 0;
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[1].NumDescriptors = 1;
	ranges[1].OffsetInDescriptorsFromTableStart = 1;
	D3D12_ROOT_PARAMETER param;
	memset(&param, 0, sizeof(param));
	param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	param.DescriptorTable.NumDescriptorRanges = 2;
	param.DescriptorTable.pDescriptorRanges = ranges;
	param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	D3D12_ROOT_PARAMETER dpar[2];
	memset(dpar, 0, sizeof(dpar));
	dpar[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	dpar[0].Constants.ShaderRegister = 0;
	dpar[0].Constants.Num32BitValues = 4; // the depth range of the world viewport: min, 1 / (max - min)
	dpar[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	dpar[1] = param;
	D3D12_ROOT_SIGNATURE_DESC rd;
	memset(&rd, 0, sizeof(rd));
	rd.NumParameters = 2;
	rd.pParameters = dpar;
	void *blob = NULL, *err = NULL;
	HRESULT hr = serialize(&rd, 1 /* D3D_ROOT_SIGNATURE_VERSION_1_0 */, &blob, &err);
	if (FAILED(hr) || !blob) {
		log_printf("fsr3: D3D12SerializeRootSignature hr %lx", (unsigned long)hr);
		return 0;
	}
	hr = ID3D12Device_CreateRootSignature(S.dev, 0, ((void *(*)(void *))(*(void ***)blob)[3])(blob), ((SIZE_T(*)(void *))(*(void ***)blob)[4])(blob), &iid_rs, (void **)&S.rs);
	((ULONG(*)(void *))(*(void ***)blob)[2])(blob);
	if (FAILED(hr))
		return 0;
	D3D12_DESCRIPTOR_HEAP_DESC hd;
	memset(&hd, 0, sizeof(hd));
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	hd.NumDescriptors = 16; // 0 depth SRV, 1 depth UAV (the depth pass), 2 depth SRV, 3 motion UAV (motion vectors), 4 output SRV, 5 own frame SRV, 6 final UAV (compose)
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(ID3D12Device_CreateDescriptorHeap(S.dev, &hd, &iid_heap, (void **)&S.heap)))
		return 0;
	S.desc_inc = ID3D12Device_GetDescriptorHandleIncrementSize(S.dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	S.pso_ms = make_compute_pso(compile, depth_cs_ms, S.rs);
	S.pso_1 = make_compute_pso(compile, depth_cs_1, S.rs);
	log_printf("fsr3: the depth pass is ready (multisampled shader %s, single sample shader %s)", S.pso_ms ? "ok" : "FAILED", S.pso_1 ? "ok" : "FAILED");
	// the motion vector pass: its own root signature with 20 root constants and the same kind of descriptor table
	D3D12_ROOT_PARAMETER mp[2];
	memset(mp, 0, sizeof(mp));
	mp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	mp[0].Constants.ShaderRegister = 0;
	mp[0].Constants.Num32BitValues = 24;
	mp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	mp[1] = param;
	D3D12_ROOT_SIGNATURE_DESC md;
	memset(&md, 0, sizeof(md));
	md.NumParameters = 2;
	md.pParameters = mp;
	blob = NULL;
	err = NULL;
	hr = serialize(&md, 1, &blob, &err);
	if (SUCCEEDED(hr) && blob) {
		hr = ID3D12Device_CreateRootSignature(S.dev, 0, ((void *(*)(void *))(*(void ***)blob)[3])(blob), ((SIZE_T(*)(void *))(*(void ***)blob)[4])(blob), &iid_rs, (void **)&S.rs_mv);
		((ULONG(*)(void *))(*(void ***)blob)[2])(blob);
		if (SUCCEEDED(hr))
			S.pso_mv = make_compute_pso(compile, mv_cs, S.rs_mv);
	}
	log_printf("fsr3: the motion vector pass is %s", S.pso_mv ? "ready" : "FAILED");
	build_compose_pass();
	build_refine_pass(compile, serialize);
	return S.pso_ms && S.pso_1;
}

// the compose pass: the game's HUD is drawn in output-size space, around the corner that holds the 3D picture. The FSR output replaces the picture; whatever the game drew outside the corner
// (bright enough not to be the faint bloom spill) is kept from the game's own frame, so the HUD stays at its native sharpness.
static const char compose_cs[] =
	"cbuffer C : register(b0) { uint iw; uint ih; uint2 pad; };\n"
	"Texture2D<float4> fsr : register(t0);\n"
	"Texture2D<float4> orig : register(t1);\n"
	"RWTexture2D<float4> dst : register(u0);\n"
	"[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)\n"
	"{\n"
	"  uint w, h;\n"
	"  fsr.GetDimensions(w, h);\n"
	"  if (id.x >= w || id.y >= h) return;\n"
	"  float4 f = fsr.Load(int3(id.xy, 0));\n"
	"  float4 o = orig.Load(int3(id.xy, 0));\n"
	"  bool outside = id.x >= iw || id.y >= ih;\n"
	"  bool hud = outside && max(o.r, max(o.g, o.b)) > 0.12;\n"
	"  dst[id.xy] = hud ? o : f;\n"
	"}\n";

// The sharpening pass for DLSS. DLSS Super Resolution has no sharpening of its own any more (NVIDIA removed it), and its output measured about a third softer than the game's own picture
// and than FSR 3 with its sharpening (mean luminance gradient 12-14 against 18-20, 2026-11-08). FSR 3 sharpens inside its dispatch with RCAS; the same RCAS runs here on DLSS's output,
// with the same setting ([display] fsr3_sharpness, 0-100) and the same mapping to RCAS's constant as FSR 2/3 use (2 ^ -(2 - 2 * sharpness)).
// A port of RCAS from AMD's FidelityFX FSR 1 header (ffx_fsr1.h, MIT license, Copyright (C) Advanced Micro Devices, Inc.; the full notice is in src/upscale.c, whose Direct3D 9 port
// this follows: exact divisions kept finite, no noise detection).
static const char sharpen_cs[] =
	"cbuffer C : register(b0) { uint w; uint h; float con; uint pad; };\n"
	"Texture2D<float4> src : register(t0);\n"
	"RWTexture2D<float4> dst : register(u0);\n"
	"float3 Ld(int2 p) { return src.Load(int3(clamp(p, int2(0, 0), int2(int(w) - 1, int(h) - 1)), 0)).rgb; }\n"
	"[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)\n"
	"{\n"
	"  if (id.x >= w || id.y >= h) return;\n"
	"  int2 p = int2(id.xy);\n"
	"  float3 b = Ld(p + int2(0, -1));\n"
	"  float3 d = Ld(p + int2(-1, 0));\n"
	"  float3 e = Ld(p);\n"
	"  float3 f = Ld(p + int2(1, 0));\n"
	"  float3 g = Ld(p + int2(0, 1));\n"
	"  float bL = b.b * 0.5 + (b.r * 0.5 + b.g);\n"
	"  float dL = d.b * 0.5 + (d.r * 0.5 + d.g);\n"
	"  float eL = e.b * 0.5 + (e.r * 0.5 + e.g);\n"
	"  float fL = f.b * 0.5 + (f.r * 0.5 + f.g);\n"
	"  float gL = g.b * 0.5 + (g.r * 0.5 + g.g);\n"
	"  float3 mn4 = min(min(min(b, d), f), g);\n"
	"  float3 mx4 = max(max(max(b, d), f), g);\n"
	"  float lowerLimiter = saturate(eL / max(min(min(min(bL, dL), fL), gL), 1e-6));\n"
	"  float3 hitMin = mn4 * (1.0 / max(4.0 * mx4, 1e-6)) * lowerLimiter;\n"
	"  float3 hitMax = (1.0 - mx4) * (1.0 / min(4.0 * mn4 - 4.0, -1e-6));\n"
	"  float3 lobeV = max(-hitMin, hitMax);\n"
	"  float lobe = max(-(0.25 - (1.0 / 16.0)), min(max(lobeV.x, max(lobeV.y, lobeV.z)), 0.0)) * con;\n"
	"  float rcpL = 1.0 / (4.0 * lobe + 1.0);\n"
	"  float3 pix = (lobe * b + lobe * d + lobe * g + lobe * f + e) * rcpL;\n"
	"  dst[id.xy] = float4(saturate(pix), 1.0);\n"
	"}\n";

// the sharpening both upscalers use, 0-100 (the QA sweep's value, FSR3_SHARP, or [display] fsr3_sharpness)
static int g_sw_sharp; // (defined with the other sweep values below: -1 = the setting's own value)
static int sharpness_pct(void)
{
	if (g_sw_sharp >= 0)
		return g_sw_sharp;
	if (ENV_HAS("FSR3_SHARP"))
		return (int)ENV_NUM("FSR3_SHARP", 0);
	const int page = enhopts_sharpness(); // the options page's slider (Settings > Enhanced), once its dvars exist: it follows the player's hand at once
	return page >= 0 ? page : settings_fsr3_sharpness();
}

static int build_compose_pass(void)
{
	static const GUID iid_rs = {0xc54a6b66, 0x72df, 0x4ee8, {0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14}};
	HMODULE comp = GetModuleHandleA("d3dcompiler_47.dll"), d12 = GetModuleHandleA("d3d12.dll");
	compile_t compile = comp ? (compile_t)(void *)GetProcAddress(comp, "D3DCompile") : NULL;
	serialize_rs_t serialize = d12 ? (serialize_rs_t)(void *)GetProcAddress(d12, "D3D12SerializeRootSignature") : NULL;
	if (!compile || !serialize)
		return 0;
	D3D12_DESCRIPTOR_RANGE ranges[2];
	memset(ranges, 0, sizeof(ranges));
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = 2;
	ranges[0].OffsetInDescriptorsFromTableStart = 0;
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[1].NumDescriptors = 1;
	ranges[1].OffsetInDescriptorsFromTableStart = 2;
	D3D12_ROOT_PARAMETER mp[2];
	memset(mp, 0, sizeof(mp));
	mp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	mp[0].Constants.Num32BitValues = 4;
	mp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	mp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	mp[1].DescriptorTable.NumDescriptorRanges = 2;
	mp[1].DescriptorTable.pDescriptorRanges = ranges;
	mp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	D3D12_ROOT_SIGNATURE_DESC md;
	memset(&md, 0, sizeof(md));
	md.NumParameters = 2;
	md.pParameters = mp;
	void *blob = NULL, *err = NULL;
	HRESULT hr = serialize(&md, 1, &blob, &err);
	if (FAILED(hr) || !blob)
		return 0;
	hr = ID3D12Device_CreateRootSignature(S.dev, 0, ((void *(*)(void *))(*(void ***)blob)[3])(blob), ((SIZE_T(*)(void *))(*(void ***)blob)[4])(blob), &iid_rs, (void **)&S.rs_comp);
	((ULONG(*)(void *))(*(void ***)blob)[2])(blob);
	if (SUCCEEDED(hr)) {
		S.pso_comp = make_compute_pso(compile, compose_cs, S.rs_comp);
		S.pso_sharp = make_compute_pso(compile, sharpen_cs, S.rs_comp);
	}
	log_printf("fsr3: the compose pass (keeps the game's HUD outside the picture) is %s", S.pso_comp ? "ready" : "FAILED");
	return S.pso_comp != NULL;
}

// the game's depth surface at this Present, when it has the render size: the unwrapped resource (the caller returns it) and its description
static IDirect3DSurface9 *g_world_ds; // this frame: the depth surface of the world pass
static ID3D12Resource *depth_source(IDirect3DDevice9 *dev9, void *on12, IDirect3DSurface9 **surf, D3D12_RESOURCE_DESC *rd)
{
	on12_vtbl_t *vt = *(on12_vtbl_t **)on12;
	*surf = NULL;
	IDirect3DSurface9 *ds = NULL;
	D3DSURFACE_DESC sd;
	if (g_world_ds) {
		ds = g_world_ds;
		IDirect3DSurface9_AddRef(ds);
	} else if (FAILED(IDirect3DDevice9_GetDepthStencilSurface(dev9, &ds)) || !ds) {
		return NULL;
	}
	if (FAILED(IDirect3DSurface9_GetDesc(ds, &sd)) || sd.Width != S.iw || sd.Height != S.ih) {
		if (S.depth_log++ < 3 && SUCCEEDED(IDirect3DSurface9_GetDesc(ds, &sd)))
			log_printf("fsr3: the depth surface at Present is %ux%u, not the render size %ux%u: no depth this frame", sd.Width, sd.Height, S.iw, S.ih);
		IDirect3DSurface9_Release(ds);
		return NULL;
	}
	ID3D12Resource *res = NULL;
	if (FAILED(vt->unwrap(on12, (IDirect3DResource9 *)ds, (IUnknown *)S.queue, &iid_resource, (void **)&res)) || !res) {
		IDirect3DSurface9_Release(ds);
		return NULL;
	}
	((D3D12_RESOURCE_DESC * (STDMETHODCALLTYPE *)(void *, D3D12_RESOURCE_DESC *))(*(void ***)res)[10])(res, rd); // ID3D12Resource::GetDesc
	*surf = ds;
	return res;
}


// ---------------------------------------------------------------------------------------------------- the camera: the game's view-projection matrix and the jitter
// The game's vertex shaders take the view-projection matrix in c0..c3 (clip x, y, z, w are dot products of the vertex with c0, c1, c2, c3: weaponfov.c scales c0 and c1 for the weapon).
static D3DVIEWPORT9 g_vp9;
static HRESULT(STDMETHODCALLTYPE *g_vp_orig)(IDirect3DDevice9 *, const D3DVIEWPORT9 *);
static HRESULT(STDMETHODCALLTYPE *g_vsc_orig)(IDirect3DDevice9 *, UINT, const float *, UINT);
static int g_trace; // QA: FSR3_TRACE=n logs n viewport and view-projection events, starting at frame FSR3_TRACE_FROM
static int g_trace_from, g_frame, g_armed;
static int g_saw_world_vp; // a viewport of the world pass (render size, depth range from 1/64) was set in this frame
static int g_bias_switches; // this frame's switches of the texture bias (into or out of the world pass), 16 SetSamplerState calls each
static double g_bias_sw_sum;
static int g_autotrace;     // the frames traced because a 3D frame had no world matrix (3 at most)
static int g_draws; // trace only: draw calls since the last logged event
static int g_stretched;       // this frame: the game's post-process copy of the 3D picture is done (the next set of the back buffer as the render target starts its last pass: bloom and HUD)
static int g_ran;             // this frame: FSR 3 has run
static int g_vp_new_ok;
static int fsr3_frame(IDirect3DDevice9 *dev9);

// the jitter of the frame being drawn (pixels of the render size), applied to every perspective matrix the game uploads while the viewport has the render size, and passed to FSR at the Present
static float g_jit_x, g_jit_y;
static int g_jit_on = 1;      // QA: FSR3_NOJITTER=1 turns the jitter off
static int g_bias_on;         // the texture LOD bias of the world pass is set
static int g_mv_flip;         // QA: FSR3_MV_FLIP=1 negates the motion vectors (to test the sign)
static float g_shake_amp;     // QA: FSR3_SHAKE=<percent of the screen width> sways the 3D picture sideways in a sine, with a depth-dependent shift (to test the motion vectors on a still scene)
static float g_shake_x;
static int g_jit_flip;        // QA: FSR3_JITTER_FLIP bit 1 = mirror x, bit 2 = mirror y in the matrix (to test the sign)
static int g_mv_on = 1;       // QA: FSR3_NOMV=1 leaves the motion vectors at zero
// QA sweep (FSR3_SWEEP=1, with FSR3_METRIC=1): every 120 frames the next variant of the settings below is used and, after 60 frames to settle, measured for 60 (metric_finish logs one line)
static int g_sweep;
static int g_ran_log;
static int g_subpx = 1; // the refined vectors are searched to a quarter pixel (QA: FSR3_NOSUBPX=1, or a sweep variant)
static int g_refine = 1; // motion refinement on (QA: FSR3_NOREFINE=1, or a sweep variant)
static float g_pjit_x, g_pjit_y; // the jitter of the previous upscaled frame
static int g_sw_sharp = -1;   // -1: the setting's own value
static float g_sw_bias = 1000.0f; // 1000: the computed bias
static const char *g_sw_name = "";
static int g_sw_idx = -1;
static float g_vp_new[16];    // the world view-projection matrix uploaded in this frame (without jitter), as the game sent it: c0..c3

static float g_vp_prev[16];   // the one of the previous frame
static int g_vp_prev_ok;
// Camera-relative frames (the player on a vehicle's turret, Team Player's Humvee): the world pass uploads only a view-projection without translation and each object's matrix is relative
// to the eye, so no world matrix is caught and FSR did not run (owner's screenshots: the 1280x720 picture in a corner of the 1920x1080 window; QA trace 2026-11-08). The first such matrix
// of the world pass (depth range from 1/64: not the weapon's) is used instead. Its motion vectors know the camera's turning, not its movement (the refinement passes take some of that).
static float g_vp_rel[16];
static int g_vp_rel_ok;
static float g_rel_minz, g_rel_maxz = 1.0f;
static int g_vp_space, g_vp_prev_space; // 0 = world matrix, 1 = camera-relative: the history is not carried across a change
// diagnostics (owner, 2026-11-08, Team Player: "soap", the near ground smeared while people and the gun were sharp): world matrices in one frame that differ from the first
// one (another view, or objects with their own full matrix: the motion vectors would then come from the wrong one) and the jittered uploads per frame
static float g_vp_first[16];
static int g_vp_distinct, g_jit_uploads, g_multi_frames, g_diag_frames, g_multi_logged;
static double g_jit_upl_sum;
static float g_cam_minz, g_cam_maxz = 1.0f;
static int g_jit_log, g_status_log, g_seq_log;
static char g_vpseq[400];
static UINT g_vpseq_last;

static HRESULT(STDMETHODCALLTYPE *g_stretch_orig)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE);
static HRESULT(STDMETHODCALLTYPE *g_srt_orig)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);

static void describe(IDirect3DSurface9 *s, char *out, size_t n)
{
	D3DSURFACE_DESC sd;
	if (s && SUCCEEDED(IDirect3DSurface9_GetDesc(s, &sd)))
		snprintf(out, n, "%ux%u fmt %d ms %d", sd.Width, sd.Height, (int)sd.Format, (int)sd.MultiSampleType);
	else
		snprintf(out, n, "none");
}

// The game believes its back buffer has the render size. With the back buffer enlarged to the output size, its post-process copy of the picture (StretchRect of the whole back buffer into a
// render-size target) would squeeze the whole enlarged back buffer into the render size, shrinking the picture to render/output of its size. The copy takes the render-size corner instead.
static HRESULT STDMETHODCALLTYPE hook_stretch9(IDirect3DDevice9 *dev, IDirect3DSurface9 *src, const RECT *sr, IDirect3DSurface9 *dst, const RECT *dr, D3DTEXTUREFILTERTYPE f)
{
	RECT corner;
	if (S.ok && !sr && src && dst && (S.iw != S.w || S.ih != S.h)) {
		D3DSURFACE_DESC a, b;
		if (SUCCEEDED(IDirect3DSurface9_GetDesc(src, &a)) && SUCCEEDED(IDirect3DSurface9_GetDesc(dst, &b)) && a.Width == S.w && a.Height == S.h && b.Width == S.iw && b.Height == S.ih) {
			corner.left = 0;
			corner.top = 0;
			corner.right = (LONG)S.iw;
			corner.bottom = (LONG)S.ih;
			sr = &corner;
			g_stretched = 1;
			static int told;
			if (!told++)
				log_printf("fsr3: the game's StretchRect of the whole %ux%u back buffer into a %ux%u target now copies the %ux%u corner", a.Width, a.Height, b.Width, b.Height, S.iw, S.ih);
		}
	} else if (S.ok && !sr && src && dst && !g_stretched) {
		// render size = output size (FSR as anti-aliasing only): the same copy of the whole back buffer into a target of the same size, nothing to correct. Its sizes cannot tell it from
		// other copies, so the source must be the swap chain's own back buffer. It marks the end of the 3D picture: the next set of the back buffer as the render target starts the last
		// pass (bloom, HUD), where FSR runs. (Before, "the back buffer was set as the render target" alone counted at this size: FSR ran at the first such switch of a frame, before the
		// world was drawn, with a camera-relative matrix, and dropped its history in every frame: QA 2026-11-08.)
		D3DSURFACE_DESC a, b;
		if (SUCCEEDED(IDirect3DSurface9_GetDesc(src, &a)) && SUCCEEDED(IDirect3DSurface9_GetDesc(dst, &b)) && a.Width == S.w && a.Height == S.h && b.Width == S.iw && b.Height == S.ih) {
			IDirect3DSurface9 *bb = NULL;
			static int told, told_other;
			if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
				if (bb == src) {
					g_stretched = 1;
					if (!told++)
						log_printf("fsr3: the game's StretchRect of the whole %ux%u back buffer into a %ux%u target (render size = output size) marks the start of the last pass", a.Width, a.Height, b.Width, b.Height);
				} else if (told_other++ < 3) {
					log_printf("fsr3: a StretchRect of the whole %ux%u surface into a %ux%u target: the source is not the back buffer (not the last pass's cue)", a.Width, a.Height, b.Width, b.Height);
				}
				IDirect3DSurface9_Release(bb);
			}
		}
	}
	if (g_trace > 0 && g_armed) {
		char a[64], b[64];
		describe(src, a, sizeof(a));
		describe(dst, b, sizeof(b));
		g_trace--;
		log_printf("fsr3: trace: (draws before: %d) StretchRect %s [%ld,%ld,%ld,%ld] -> %s [%ld,%ld,%ld,%ld] filter %d", g_draws, a, sr ? sr->left : -1, sr ? sr->top : -1, sr ? sr->right : -1, sr ? sr->bottom : -1, b, dr ? dr->left : -1, dr ? dr->top : -1, dr ? dr->right : -1, dr ? dr->bottom : -1, (int)f);
		g_draws = 0;
	}
	return g_stretch_orig(dev, src, sr, dst, dr, f);
}

static HRESULT(STDMETHODCALLTYPE *g_dp_orig)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
static HRESULT(STDMETHODCALLTYPE *g_dip_orig)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
static HRESULT(STDMETHODCALLTYPE *g_dpup_orig)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
static HRESULT(STDMETHODCALLTYPE *g_dipup_orig)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT);
static HRESULT STDMETHODCALLTYPE hook_dp9(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, UINT b)
{
	g_draws++;
	return g_dp_orig(d, t, a, b);
}
static HRESULT STDMETHODCALLTYPE hook_dip9(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT a, UINT b, UINT c, UINT e, UINT f)
{
	g_draws++;
	return g_dip_orig(d, t, a, b, c, e, f);
}
static HRESULT STDMETHODCALLTYPE hook_dpup9(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, const void *b, UINT c)
{
	g_draws++;
	return g_dpup_orig(d, t, a, b, c);
}
static HRESULT STDMETHODCALLTYPE hook_dipup9(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, UINT b, UINT c, const void *e, D3DFORMAT f, const void *g, UINT h)
{
	g_draws++;
	return g_dipup_orig(d, t, a, b, c, e, f, g, h);
}

static HRESULT STDMETHODCALLTYPE hook_srt9(IDirect3DDevice9 *dev, DWORD idx, IDirect3DSurface9 *s)
{
	// no world matrix in this frame: the camera-relative one (FSR3_NOREL=1: not, FSR does not run then). Only at the point where FSR runs: normal frames upload a
	// camera-relative matrix early (before the world matrix), and taking it at any earlier render target switch marked them camera-relative (QA 2026-11-08)
	const int at_last_pass = S.ok && g_stretched && !g_ran && idx == 0 && s;
	if (at_last_pass && !g_vp_new_ok && g_vp_rel_ok && !ENV_HAS("FSR3_NOREL")) {
		D3DSURFACE_DESC rd;
		if (SUCCEEDED(IDirect3DSurface9_GetDesc(s, &rd)) && rd.Width == S.w && rd.Height == S.h) {
			memcpy(g_vp_new, g_vp_rel, sizeof(g_vp_new));
			g_vp_new_ok = 1;
			g_vp_space = 1;
			g_cam_minz = g_rel_minz;
			g_cam_maxz = g_rel_maxz;
		}
	}
	if (at_last_pass && g_vp_new_ok) { // the game's last pass (the bloom and the HUD) is about to draw into the back buffer: upscale the 3D picture first, so both land on the upscaled picture
		D3DSURFACE_DESC d;
		if (SUCCEEDED(IDirect3DSurface9_GetDesc(s, &d)) && d.Width == S.w && d.Height == S.h) {
			g_ran = 1;
			if (!fsr3_frame(dev))
				g_ran = 0;
		}
	}
	if (g_trace > 0 && g_armed && idx == 0) {
		char a[64];
		describe(s, a, sizeof(a));
		g_trace--;
		log_printf("fsr3: trace: (draws before: %d) SetRenderTarget %s", g_draws, a);
		g_draws = 0;
	}
	return g_srt_orig(dev, idx, s);
}

static int is_perspective(const float *m)
{
	return fabsf(m[15]) < 1e-4f && (fabsf(m[12]) + fabsf(m[13]) + fabsf(m[14])) > 1e-4f;
}

static HRESULT STDMETHODCALLTYPE hook_viewport9(IDirect3DDevice9 *dev, const D3DVIEWPORT9 *vp)
{
	if (vp) {
		g_vp9 = *vp;
		if (S.ok && vp->Width == S.iw && vp->Height == S.ih && vp->MinZ > 0.0f)
			g_saw_world_vp = 1;
		if (S.ok && !g_world_ds && vp->Width == S.iw && vp->Height == S.ih && vp->MinZ > 0.0f) // the world pass: remember its depth surface (the bloom passes after it have another bound)
			IDirect3DDevice9_GetDepthStencilSurface(dev, &g_world_ds);
		static int no_bias = -1;
		static const char *env_bias;
		if (no_bias < 0) { // QA: FSR3_NOBIAS=1 sets no texture bias at all (the 16 SetSamplerState calls at every switch into and out of the world pass are left out: frame time test)
			no_bias = getenv("FSR3_NOBIAS") != NULL;
			env_bias = getenv("FSR3_LODBIAS");
		}
		if (S.ok && S.iw != S.w && !no_bias) { // a texture bias in the world pass: the textures are sampled for the output size, as upscalers ask (FSR 3: log2(render / output) - 1)
			const int world = vp->Width == S.iw && vp->Height == S.ih;
			if (world != g_bias_on) {
				g_bias_on = world;
				g_bias_switches++;
				float set_bias = 0.0f;
				const int have_set = settings_fsr3_lodbias(&set_bias);
				float bias = world ? (g_sw_bias < 999.0f ? g_sw_bias : env_bias ? (float)atof(env_bias) : have_set ? set_bias : log2f((float)S.iw / (float)S.w) - 1.0f) : 0.0f;
				DWORD bits;
				memcpy(&bits, &bias, 4);
				for (DWORD i = 0; i < 16; i++)
					IDirect3DDevice9_SetSamplerState(dev, i, D3DSAMP_MIPMAPLODBIAS, bits);
			}
		}
		if ((vp->Width << 16 | vp->Height) != g_vpseq_last && strlen(g_vpseq) < sizeof(g_vpseq) - 24) {
			g_vpseq_last = vp->Width << 16 | vp->Height;
			snprintf(g_vpseq + strlen(g_vpseq), 24, " %lux%lu", vp->Width, vp->Height);
		}
		if (g_trace && !g_armed && vp->Width > 100 && (vp->Width < 1900 || S.iw == S.w) && g_frame >= g_trace_from)
			g_armed = 1;
		if (g_trace > 0 && g_armed) {
			g_trace--;
			log_printf("fsr3: trace: (draws before: %d) SetViewport %lu,%lu %lux%lu depth %g..%g", g_draws, vp->X, vp->Y, vp->Width, vp->Height, vp->MinZ, vp->MaxZ);
			g_draws = 0;
		}
	}
	return g_vp_orig(dev, vp);
}

static HRESULT STDMETHODCALLTYPE hook_constants9(IDirect3DDevice9 *dev, UINT start, const float *data, UINT count)
{
	if (S.ok && data && start == 0 && count >= 4 && count <= 32 && g_vp9.Width == S.iw && g_vp9.Height == S.ih && (fabsf(data[12]) + fabsf(data[13]) + fabsf(data[14])) > 1e-3f) {
		float tmp[4 * 32];
		memcpy(tmp, data, count * 16);
		if (g_shake_amp != 0.0f) // QA camera sway: x_ndc += k * z_ndc in every 3D matrix (a shift that depends on depth, like parallax; the captured matrix has it too, so the vectors must follow)
			for (int i = 0; i < 4; i++)
				tmp[i] += g_shake_x * tmp[8 + i];
		// the world view-projection (it has a translation, and it is uploaded while the viewport has the world's depth range, 1/64..1): remember it without the jitter,
		// for the motion vectors. Not in the weapon's range (0..1/64): Team Player's first-person weapon pass uploads a world matrix of its own (another projection), which
		// replaced the world's in every frame, so the vectors came from the wrong camera (FSR's debug view: motion everywhere on a standing camera) and FSR shifted its
		// history every frame: the owner's "soap" on the ground and the grain (QA 2026-11-08; the Trainer's weapon pass has a camera-relative matrix, so it was fine there)
		if (fabsf(data[15]) > 1e-3f && g_vp9.MinZ > 0.01f) {
			if (!g_vp_new_ok) {
				memcpy(g_vp_first, tmp, 64);
				g_vp_distinct = 1;
			} else {
				float dmax = 0.0f;
				for (int i = 0; i < 16; i++)
					dmax = fmaxf(dmax, fabsf(tmp[i] - g_vp_new[i]) / (1.0f + fabsf(g_vp_new[i])));
				if (dmax > 1e-4f) {
					g_vp_distinct++;
					if (g_multi_logged < 4) {
						g_multi_logged++;
						log_printf("fsr3: frame %d: world matrix %d differs from the one before in this frame (c0+%u, viewport %lux%lu depth %g..%g): before | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | now | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
								   g_frame, g_vp_distinct, count, g_vp9.Width, g_vp9.Height, g_vp9.MinZ, g_vp9.MaxZ, g_vp_new[0], g_vp_new[1], g_vp_new[2], g_vp_new[3], g_vp_new[4], g_vp_new[5], g_vp_new[6], g_vp_new[7], g_vp_new[8],
								   g_vp_new[9], g_vp_new[10], g_vp_new[11], g_vp_new[12], g_vp_new[13], g_vp_new[14], g_vp_new[15], tmp[0], tmp[1], tmp[2], tmp[3], tmp[4], tmp[5], tmp[6], tmp[7], tmp[8], tmp[9], tmp[10], tmp[11],
								   tmp[12], tmp[13], tmp[14], tmp[15]);
					}
				}
			}
			memcpy(g_vp_new, tmp, 64);
			g_vp_new_ok = 1;
			g_vp_space = 0;
			g_cam_minz = g_vp9.MinZ;
			g_cam_maxz = g_vp9.MaxZ;
		} else if (!g_vp_rel_ok && g_vp9.MinZ > 0.01f && is_perspective(tmp)) { // a camera-relative view-projection of the world pass (see g_vp_rel)
			memcpy(g_vp_rel, tmp, 64);
			g_vp_rel_ok = 1;
			g_rel_minz = g_vp9.MinZ;
			g_rel_maxz = g_vp9.MaxZ;
		}
		g_jit_uploads++;
		const float kx = (g_jit_flip & 1 ? -2.0f : 2.0f) * g_jit_x / (float)S.iw;
		const float ky = (g_jit_flip & 2 ? 2.0f : -2.0f) * g_jit_y / (float)S.ih;
		for (int i = 0; i < 4; i++) { // clip.xy += jitter * clip.w
			tmp[i] += kx * tmp[12 + i];
			tmp[4 + i] += ky * tmp[12 + i];
		}
		if (g_trace > 0 && g_armed) {
			g_trace--;
			log_printf("fsr3: trace: jittered %s matrix by %.3f,%.3f px (ndc %.5f,%.5f)", fabsf(data[15]) > 1e-3f ? "world" : "camera-relative", g_jit_x, g_jit_y, kx, ky);
		}
		return g_vsc_orig(dev, start, tmp, count);
	}
	if (g_trace > 0 && g_armed && data && count >= 4) {
		g_trace--;
		log_printf("fsr3: trace: frame %d consts c%u+%u %s viewport %lux%lu depth %g | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f", g_frame, start, count, is_perspective(data) ? "PERSP" : "other", g_vp9.Width, g_vp9.Height, g_vp9.MinZ, data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15]);
	}
	return g_vsc_orig(dev, start, data, count);
}

static void hook_device(IDirect3DDevice9 *dev9)
{
	static int done;
	if (done)
		return;
	done = 1;
	g_jit_on = getenv("FSR3_NOJITTER") ? 0 : 1;
	g_mv_on = getenv("FSR3_NOMV") ? 0 : 1;
	g_sweep = getenv("FSR3_SWEEP") != NULL;
	g_refine = getenv("FSR3_NOREFINE") ? 0 : getenv("FSR3_RF_MODE") ? atoi(getenv("FSR3_RF_MODE")) : settings_fsr3_refine();
	g_subpx = getenv("FSR3_NOSUBPX") ? 0 : getenv("FSR3_SUBPX") ? 1 : g_refine == 2; // quarter-pixel search only in the classic mode: sub-pixel vectors from noisy fine detail ripple
	g_mv_flip = getenv("FSR3_MV_FLIP") ? 1 : 0;
	g_shake_amp = getenv("FSR3_SHAKE") ? (float)atof(getenv("FSR3_SHAKE")) : 0.0f;
	g_jit_flip = getenv("FSR3_JITTER_FLIP") ? atoi(getenv("FSR3_JITTER_FLIP")) : 0;
	g_trace = getenv("FSR3_TRACE") ? atoi(getenv("FSR3_TRACE")) : 0;
	g_trace_from = getenv("FSR3_TRACE_FROM") ? atoi(getenv("FSR3_TRACE_FROM")) : 0;
	void **vt = *(void ***)dev9;
	DWORD old;
	if (VirtualProtect(&vt[47], sizeof(void *), PAGE_READWRITE, &old)) {
		g_vp_orig = (HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, const D3DVIEWPORT9 *))vt[47];
		vt[47] = (void *)hook_viewport9;
		VirtualProtect(&vt[47], sizeof(void *), old, &old);
	}
	if (VirtualProtect(&vt[94], sizeof(void *), PAGE_READWRITE, &old)) {
		g_vsc_orig = (HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, UINT, const float *, UINT))vt[94];
		vt[94] = (void *)hook_constants9;
		VirtualProtect(&vt[94], sizeof(void *), old, &old);
	}
	{
		if (VirtualProtect(&vt[34], sizeof(void *), PAGE_READWRITE, &old)) {
			g_stretch_orig = (HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE))vt[34];
			vt[34] = (void *)hook_stretch9;
			VirtualProtect(&vt[34], sizeof(void *), old, &old);
		}
		if (g_trace) {
			void *slots[4] = {(void *)hook_dp9, (void *)hook_dip9, (void *)hook_dpup9, (void *)hook_dipup9};
			void **origs[4] = {(void **)&g_dp_orig, (void **)&g_dip_orig, (void **)&g_dpup_orig, (void **)&g_dipup_orig};
			for (int i = 0; i < 4; i++)
				if (VirtualProtect(&vt[81 + i], sizeof(void *), PAGE_READWRITE, &old)) {
					*origs[i] = vt[81 + i];
					vt[81 + i] = slots[i];
					VirtualProtect(&vt[81 + i], sizeof(void *), old, &old);
				}
		}
		if (VirtualProtect(&vt[37], sizeof(void *), PAGE_READWRITE, &old)) {
			g_srt_orig = (HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *))vt[37];
			vt[37] = (void *)hook_srt9;
			VirtualProtect(&vt[37], sizeof(void *), old, &old);
		}
	}
	log_printf("fsr3: the device's SetViewport and SetVertexShaderConstantF are hooked (camera matrix and jitter)");
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

// the jitter of the next frame: the next point of FSR's sequence for this render/output size (DLSS: the Halton sequence)
static void next_jitter(void)
{
	g_jit_x = g_jit_y = 0.0f;
	if ((!S.query && !settings_dlss()) || S.phase_count <= 0 || !g_jit_on)
		return;
	if (settings_dlss()) { // DLSS has no query for it: the Halton (2, 3) sequence, offsets in -0.5..0.5 render pixels, as FSR's
		const unsigned idx = S.frame % (unsigned)S.phase_count + 1;
		g_jit_x = halton(idx, 2) - 0.5f;
		g_jit_y = halton(idx, 3) - 0.5f;
		return;
	}
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

// inverse of a 4x4 matrix (Gauss-Jordan in double precision); 0 when it is singular
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

// the game's c0..c3 (clip_i = ci . (x, y, z, 1)) as the matrix M of row-vector convention: clip = p * M, M[r][i] = ci[r]
static void vp_to_matrix(const float *c, double *m)
{
	for (int r = 0; r < 4; r++)
		for (int i = 0; i < 4; i++)
			m[r * 4 + i] = c[i * 4 + r];
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


// The camera of the world pass, read from its view-projection matrix (rows c0..c3 of clip = ci . (x, y, z, 1), see hook_constants9): with w = the depth along the view, tan(vertical half fov) =
// |row3| / |row1|, and clip.z / w = A + B / w with A = |row2| / |row3|, B = c2.w - A * c3.w gives near = -B / A and far = -B / (A - 1) (in units of w). 0 when the matrix is not a perspective one.
static float g_cam_a, g_cam_bw; // set by camera_from_matrix: depth buffer value = g_cam_a + g_cam_bw / distance (the refinement's distance gate)
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
	static int told;
	if (told++ < 2)
		log_printf("fsr3: camera matrix: |row1| %.5f |row2| %.5f |row3| %.5f, A %.6f, B %.3f, rows z %.5f %.5f %.5f %.5f / w %.5f %.5f %.5f %.5f", l1, l2, l3, a, bb, r2[0], r2[1], r2[2], m[11], r3[0], r3[1], r3[2], m[15]);
	if (bb == 0.0f || a < 0.5f || a > 2.0f)
		return 0;
	*znear = -bb / (a * l3);
	// MW2 uses an infinite far plane (A = 0.9995, B = -3.998 in the Trainer: near 4.0): a finite far plane has A > 1
	*zfar = a > 1.00001f ? -bb / ((a - 1.0f) * l3) : 100000.0f;
	return *znear > 0.01f && *znear < 1000.0f && *zfar > *znear * 2.0f;
}

// The camera's place and view direction from a view-projection matrix (row r = m[4r..4r+3], clip = row . (x, y, z, 1)): the place is where clip x, y and w
// are all 0 (three planes through the eye), the direction is row 3's (w grows along the view). 0 when the rows are degenerate.
static int camera_pose(const float *m, double pos[3], double fwd[3])
{
	const float *r0 = m, *r1 = m + 4, *r3 = m + 12;
	const double a[3][3] = {{r0[0], r0[1], r0[2]}, {r1[0], r1[1], r1[2]}, {r3[0], r3[1], r3[2]}};
	const double b[3] = {-r0[3], -r1[3], -r3[3]};
	const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
	if (fabs(det) < 1e-12)
		return 0;
	for (int k = 0; k < 3; k++) { // Cramer's rule
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

// A camera cut between two frames' matrices, which FSR's history must not blend across (a cutscene's new shot, a teleport, a scope snapping in): the eye
// moved more than 200 units (a fast vehicle moves about 25 per frame at 50 frames a second), the view turned more than 45 degrees, or the vertical field of
// view changed by more than a quarter. why: the reason, for the log.
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

// QA measuring rig (FSR3_METRIC=1): after each upscale the output and FSR's input are copied to the CPU (the GPU is waited for: a QA stall) and two numbers are taken of a central crop:
// the gradient (mean absolute luma difference to the right and lower neighbour: how much detail the picture has, higher = sharper) and the temporal difference (mean absolute luma
// difference to the same pixel of the previous frame: in a still scene the noise or grain the eye sees as flicker). FSR3_DUMPAT=<frame> writes both pictures as BMP files (mw2coop\shots).
static struct {
	ID3D12Resource *rb[2]; // 0 = FSR output, 1 = FSR input
	UINT pitch[2], w[2], h[2];
	uint8_t *prev[2];
	int pending;
	double grad[2], tdiff[2];
	long frames;
	long measured;
} M;

static int metric_on(void)
{
	static int on = -1;
	if (on < 0)
		on = getenv("FSR3_METRIC") != NULL;
	return on;
}

static ID3D12Resource *make_readback(UINT64 bytes)
{
	D3D12_HEAP_PROPERTIES hp;
	D3D12_RESOURCE_DESC rd;
	memset(&hp, 0, sizeof(hp));
	memset(&rd, 0, sizeof(rd));
	hp.Type = D3D12_HEAP_TYPE_READBACK;
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	rd.Width = bytes;
	rd.Height = 1;
	rd.DepthOrArraySize = 1;
	rd.MipLevels = 1;
	rd.Format = DXGI_FORMAT_UNKNOWN;
	rd.SampleDesc.Count = 1;
	rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ID3D12Resource *res = NULL;
	if (FAILED(ID3D12Device_CreateCommittedResource(S.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &iid_resource, (void **)&res)))
		return NULL;
	return res;
}

// the copies are recorded in the frame's list: `which` 0 = output (in the UAV state), 1 = input (in the read state)
static void metric_copy(D3D12_RESOURCE_BARRIER *b, int *n, int which)
{
	ID3D12Resource *src = which ? S.color : S.output;
	UINT w = which ? S.iw : S.w, h = which ? S.ih : S.h;
	D3D12_RESOURCE_STATES st = which ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	UINT pitch = (w * 4 + 255) & ~255u;
	if (!M.rb[which] || M.w[which] != w || M.h[which] != h) {
		if (M.rb[which])
			ID3D12Resource_Release(M.rb[which]);
		free(M.prev[which]);
		M.prev[which] = NULL;
		M.rb[which] = make_readback((UINT64)pitch * h);
		M.pitch[which] = pitch;
		M.w[which] = w;
		M.h[which] = h;
	}
	if (!M.rb[which])
		return;
	barrier(b, n, src, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
	ID3D12GraphicsCommandList_ResourceBarrier(S.list, *n, b);
	*n = 0;
	D3D12_TEXTURE_COPY_LOCATION dst, sc;
	memset(&dst, 0, sizeof(dst));
	memset(&sc, 0, sizeof(sc));
	dst.pResource = M.rb[which];
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	dst.PlacedFootprint.Footprint.Width = w;
	dst.PlacedFootprint.Footprint.Height = h;
	dst.PlacedFootprint.Footprint.Depth = 1;
	dst.PlacedFootprint.Footprint.RowPitch = pitch;
	sc.pResource = src;
	sc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	ID3D12GraphicsCommandList_CopyTextureRegion(S.list, &dst, 0, 0, 0, &sc, NULL);
	barrier(b, n, src, D3D12_RESOURCE_STATE_COPY_SOURCE, st);
	ID3D12GraphicsCommandList_ResourceBarrier(S.list, *n, b);
	*n = 0;
	M.pending |= 1 << which;
}

static void write_bmp(const char *name, const uint8_t *px, UINT pitch, UINT w, UINT h)
{
	CreateDirectoryA("mw2coop\\shots", NULL);
	FILE *f = fopen(name, "wb");
	if (!f)
		return;
	uint32_t row = (w * 3 + 3) & ~3u, size = row * h;
	uint8_t hdr[54] = {'B', 'M'};
	*(uint32_t *)(hdr + 2) = 54 + size;
	*(uint32_t *)(hdr + 10) = 54;
	*(uint32_t *)(hdr + 14) = 40;
	*(int32_t *)(hdr + 18) = (int32_t)w;
	*(int32_t *)(hdr + 22) = (int32_t)h;
	*(uint16_t *)(hdr + 26) = 1;
	*(uint16_t *)(hdr + 28) = 24;
	*(uint32_t *)(hdr + 34) = size;
	fwrite(hdr, 1, 54, f);
	uint8_t *line = (uint8_t *)calloc(1, row);
	for (int y = (int)h - 1; y >= 0 && line; y--) {
		for (UINT x = 0; x < w; x++)
			memcpy(line + x * 3, px + (size_t)y * pitch + x * 4, 3);
		fwrite(line, 1, row, f);
	}
	free(line);
	fclose(f);
}

// FSR3_DUMPAT: "<frame>" or a series "<first>+<step>x<count>" (e.g. "2000+150x20"). (The path had single backslashes, "\s" escapes: the pictures went to
// <game dir>\mw2coopshotsfsr-*.bmp, not into mw2coop\shots.)
static int dump_wanted(const char *spec, long frame)
{
	long first = 0, step = 0, count = 0;
	if (!spec)
		return 0;
	if (sscanf(spec, "%ld+%ldx%ld", &first, &step, &count) == 3 && step > 0 && count > 0)
		return frame >= first && (frame - first) % step == 0 && (frame - first) / step < count;
	return frame == atol(spec);
}

// GPU time of the mod's own work in a frame (owner, 2026-11-08: "it is laggy"): five timestamps in each frame's list (start, before the refinement, before FSR,
// after FSR, end), read back when that ring slot's fence has passed (no stall), the mean logged every 600 upscaled frames. What is not in them is the game's own
// drawing (and the Direct3D 12 layer's translation of it).
#define TS_PER_FRAME 5
static const GUID iid_query_heap = {0x0d9658ae, 0xed45, 0x469e, {0xa6, 0x1d, 0x97, 0x0e, 0xc5, 0x83, 0xca, 0xb4}};
static ID3D12QueryHeap *g_ts_heap;
static ID3D12Resource *g_ts_rb;
static UINT64 g_ts_freq;
static int g_ts_failed, g_ts_valid[RING], g_ts_n;
static double g_ts_acc[TS_PER_FRAME - 1];
// CPU time of the FSR pass in the game's render thread (the GPU timestamps do not see it): unwrapping the back buffer and the depth from the Direct3D 9 device (it flushes
// the game's work to its queue), recording our list and FSR's dispatch, submitting and handing the resources back; logged with the GPU line
static double g_cpu_acc[4]; // unwrap, wait for the ring slot, record + dispatch, submit + give back
static int g_cpu_n;
static double cpu_ms(LARGE_INTEGER a, LARGE_INTEGER b)
{
	return S.freq.QuadPart ? (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)S.freq.QuadPart : 0.0;
}

static int ts_ready(void)
{
	if (g_ts_heap)
		return 1;
	if (g_ts_failed)
		return 0;
	g_ts_failed = 1;
	D3D12_QUERY_HEAP_DESC qd;
	memset(&qd, 0, sizeof(qd));
	qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	qd.Count = RING * TS_PER_FRAME;
	if (FAILED(ID3D12CommandQueue_GetTimestampFrequency(S.queue, &g_ts_freq)) || !g_ts_freq)
		return 0;
	if (FAILED(ID3D12Device_CreateQueryHeap(S.dev, &qd, &iid_query_heap, (void **)&g_ts_heap)) || !g_ts_heap)
		return 0;
	g_ts_rb = make_readback((UINT64)RING * TS_PER_FRAME * sizeof(UINT64));
	if (!g_ts_rb) {
		ID3D12QueryHeap_Release(g_ts_heap);
		g_ts_heap = NULL;
		return 0;
	}
	g_ts_failed = 0;
	return 1;
}

static void ts_mark(unsigned slot, int i)
{
	if (g_ts_heap)
		ID3D12GraphicsCommandList_EndQuery(S.list, g_ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, slot * TS_PER_FRAME + i);
}

// this slot's list has finished on the GPU (its fence passed): add its times
static void ts_collect(unsigned slot)
{
	if (!g_ts_heap || !g_ts_valid[slot])
		return;
	g_ts_valid[slot] = 0;
	UINT64 *t = NULL;
	D3D12_RANGE rr = {slot * TS_PER_FRAME * sizeof(UINT64), (slot + 1) * TS_PER_FRAME * sizeof(UINT64)};
	if (FAILED(ID3D12Resource_Map(g_ts_rb, 0, &rr, (void **)&t)) || !t)
		return;
	const UINT64 *v = t + slot * TS_PER_FRAME;
	int ok = 1;
	for (int i = 1; i < TS_PER_FRAME; i++)
		if (v[i] < v[i - 1])
			ok = 0;
	if (ok) {
		for (int i = 1; i < TS_PER_FRAME; i++)
			g_ts_acc[i - 1] += (double)(v[i] - v[i - 1]) * 1000.0 / (double)g_ts_freq;
		g_ts_n++;
	}
	D3D12_RANGE wr = {0, 0};
	ID3D12Resource_Unmap(g_ts_rb, 0, &wr);
	if (g_ts_n >= 600) {
		if (g_cpu_n > 0)
			log_printf("fsr3: CPU time per frame in the render thread (mean of %d): unwrap %.2f ms, ring wait %.2f ms, record and FSR dispatch %.2f ms, submit and give back %.2f ms", g_cpu_n, g_cpu_acc[0] / g_cpu_n,
					   g_cpu_acc[1] / g_cpu_n, g_cpu_acc[2] / g_cpu_n, g_cpu_acc[3] / g_cpu_n);
		memset(g_cpu_acc, 0, sizeof(g_cpu_acc));
		g_cpu_n = 0;
		const double n = g_ts_n;
		log_printf("fsr3: GPU time per frame (mean of %d): depth, vectors and copies %.2f ms, refinement %.2f ms, FSR %.2f ms, the rest %.2f ms, all %.2f ms", g_ts_n, g_ts_acc[0] / n, g_ts_acc[1] / n,
				   g_ts_acc[2] / n, g_ts_acc[3] / n, (g_ts_acc[0] + g_ts_acc[1] + g_ts_acc[2] + g_ts_acc[3]) / n);
		memset(g_ts_acc, 0, sizeof(g_ts_acc));
		g_ts_n = 0;
	}
}

// after the GPU finished: read the copies, measure, log the mean of 30 frames
static void metric_finish(UINT64 value)
{
	if (!M.pending)
		return;
	wait_fence(value);
	const char *dumpat = getenv("FSR3_DUMPAT");
	for (int which = 0; which < 2; which++) {
		if (!(M.pending & (1 << which)) || !M.rb[which])
			continue;
		UINT w = M.w[which], h = M.h[which], pitch = M.pitch[which];
		uint8_t *data = NULL;
		D3D12_RANGE rr = {0, (SIZE_T)pitch * h};
		if (FAILED(ID3D12Resource_Map(M.rb[which], 0, &rr, (void **)&data)) || !data)
			continue;
		if (!M.prev[which])
			M.prev[which] = (uint8_t *)calloc((size_t)pitch, h);
		const UINT x0 = w * 20 / 100, x1 = w * 80 / 100, y0 = h * 25 / 100, y1 = h * 80 / 100;
		double g = 0, t = 0;
		long cnt = 0;
		uint8_t *pv = M.prev[which];
		for (UINT y = y0; y < y1; y++)
			for (UINT x = x0; x < x1; x++) {
				const uint8_t *a = data + (size_t)y * pitch + x * 4;
				const uint8_t *r = a + 4, *d = a + pitch;
				const uint8_t *q = pv + (size_t)y * pitch + x * 4;
				double la = 0.114 * a[0] + 0.587 * a[1] + 0.299 * a[2];
				double lr = 0.114 * r[0] + 0.587 * r[1] + 0.299 * r[2];
				double ld = 0.114 * d[0] + 0.587 * d[1] + 0.299 * d[2];
				double lq = 0.114 * q[0] + 0.587 * q[1] + 0.299 * q[2];
				g += fabs(la - lr) + fabs(la - ld);
				t += fabs(la - lq);
				cnt++;
			}
		if (S.frame > 30 && (!g_sweep || S.frame % 120 >= 60)) { // the first frames have no history yet (sweep: the first half of a variant's window settles)
			M.grad[which] += g / cnt;
			M.tdiff[which] += t / cnt;
		}
		if (dump_wanted(dumpat, (long)S.frame)) {
			char name[MAX_PATH];
			snprintf(name, sizeof(name), "mw2coop\\shots\\fsr-%s-%u.bmp", which ? "in" : "out", S.frame);
			write_bmp(name, data, pitch, w, h);
			log_printf("fsr3: metric: wrote %s", name);
		}
		if (g_sweep && S.frame % 120 == 119 && (which == 0 || g_sw_idx == 0 || !strcmp(g_sw_name, "nojitter"))) { // the last frame of a variant's window: the picture itself, to compare with the native one offline
			char name[MAX_PATH];
			snprintf(name, sizeof(name), "mw2coop\\shots\\sw-%s-%s.bmp", g_sw_name, which ? "in" : "out");
			write_bmp(name, data, pitch, w, h);
		}
		memcpy(pv, data, (size_t)pitch * h);
		D3D12_RANGE wr = {0, 0};
		ID3D12Resource_Unmap(M.rb[which], 0, &wr);
	}
	M.pending = 0;
	if (g_sweep) {
		if (S.frame % 120 >= 60)
			M.measured++;
		if (S.frame % 120 == 119 && M.measured > 0) {
			log_printf("fsr3: sweep [%s] frames %ld: output gradient %.3f temporal %.3f | input gradient %.3f temporal %.3f", g_sw_name, M.measured, M.grad[0] / M.measured, M.tdiff[0] / M.measured, M.grad[1] / M.measured,
				   M.tdiff[1] / M.measured);
			M.grad[0] = M.grad[1] = M.tdiff[0] = M.tdiff[1] = 0;
			M.measured = 0;
		}
		return;
	}
	if (S.frame > 30 && ++M.measured >= 30) {
		log_printf("fsr3: metric (30 frames, frame %u): output gradient %.3f temporal %.3f | input gradient %.3f temporal %.3f", S.frame, M.grad[0] / M.measured, M.tdiff[0] / M.measured, M.grad[1] / M.measured,
			   M.tdiff[1] / M.measured);
		M.grad[0] = M.grad[1] = M.tdiff[0] = M.tdiff[1] = 0;
		M.measured = 0;
	}
}

// the upscale itself, run from the SetRenderTarget hook when the game's last pass (bloom and HUD) is about to start; 1 when it ran
static int fsr3_frame(IDirect3DDevice9 *dev9)
{
	IDirect3DSurface9 *bb = NULL;
	D3DSURFACE_DESC sd;
	if (FAILED(IDirect3DDevice9_GetBackBuffer(dev9, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
		return 0;
	if (FAILED(IDirect3DSurface9_GetDesc(bb, &sd)) || !setup(sd.Width, sd.Height) || sd.Width != S.w || sd.Height != S.h || !g_vp_new_ok) {
		IDirect3DSurface9_Release(bb);
		return 0;
	}
	void *on12 = NULL;
	if (FAILED(IDirect3DDevice9_QueryInterface(dev9, &iid_on12, &on12)) || !on12) {
		IDirect3DSurface9_Release(bb);
		return 0;
	}
	on12_vtbl_t *vt = *(on12_vtbl_t **)on12;
	ID3D12Resource *bb12 = NULL;
	LARGE_INTEGER c0, c1, c2, c3, c4;
	QueryPerformanceCounter(&c0);
	HRESULT hr = vt->unwrap(on12, (IDirect3DResource9 *)bb, (IUnknown *)S.queue, &iid_resource, (void **)&bb12);
	if (FAILED(hr) || !bb12) {
		static int told;
		if (!told++)
			log_printf("fsr3: UnwrapUnderlyingResource hr %lx", (unsigned long)hr);
		vt->release(on12);
		IDirect3DSurface9_Release(bb);
		return 0;
	}

	QueryPerformanceCounter(&c1);
	const unsigned slot = S.frame % RING;
	wait_fence(S.alloc_fence[slot]);
	QueryPerformanceCounter(&c2);
	ts_collect(slot);
	ID3D12CommandAllocator_Reset(S.alloc[slot]);
	ID3D12GraphicsCommandList_Reset(S.list, S.alloc[slot], NULL);
	const int ts_on = ts_ready();
	if (ts_on)
		ts_mark(slot, 0);

	D3D12_RESOURCE_BARRIER b[8];
	int n = 0;
	const int first = S.frame == 0;

	// the game's depth: copy it into a readable texture, convert it to R32_FLOAT (a compute pass); a texture of zeros when the game's is not available at this Present
	IDirect3DSurface9 *dsurf = NULL;
	D3D12_RESOURCE_DESC drd;
	int depth_fresh = 0; // the depth texture holds this frame depth
	int mv_done = 0;     // the camera motion vectors were made this frame
	ID3D12Resource *dsrc = S.rs ? depth_source(dev9, on12, &dsurf, &drd) : NULL;
	if (dsrc && (drd.Format == DXGI_FORMAT_D24_UNORM_S8_UINT || drd.Format == DXGI_FORMAT_R24G8_TYPELESS)) {
		if (!S.dcopy || S.dcopy_w != drd.Width || S.dcopy_h != drd.Height || S.dcopy_samples != drd.SampleDesc.Count) {
			if (S.dcopy)
				ID3D12Resource_Release(S.dcopy);
			S.dcopy = make_texture_ms((UINT)drd.Width, drd.Height, DXGI_FORMAT_R24G8_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, drd.SampleDesc.Count);
			S.dcopy_w = (UINT)drd.Width;
			S.dcopy_h = drd.Height;
			S.dcopy_samples = drd.SampleDesc.Count;
			S.dcopy_state_ok = 0;
			log_printf("fsr3: the game's depth: %llux%u, %u samples: a readable copy %s", (unsigned long long)drd.Width, drd.Height, drd.SampleDesc.Count, S.dcopy ? "made" : "FAILED");
		}
		if (S.dcopy) {
			barrier(b, &n, S.dcopy, S.dcopy_state_ok ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			barrier(b, &n, dsrc, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
			barrier(b, &n, S.depth, S.depth_state_ok ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			ID3D12GraphicsCommandList_ResourceBarrier(S.list, n, b);
			n = 0;
			ID3D12GraphicsCommandList_CopyResource(S.list, S.dcopy, dsrc);
			barrier(b, &n, dsrc, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
			barrier(b, &n, S.dcopy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			ID3D12GraphicsCommandList_ResourceBarrier(S.list, n, b);
			n = 0;
			S.dcopy_state_ok = 1;
			// the views: the copy (t0) and the R32_FLOAT texture (u0)
			D3D12_CPU_DESCRIPTOR_HANDLE cpu;
			D3D12_GPU_DESCRIPTOR_HANDLE gpu;
			((D3D12_CPU_DESCRIPTOR_HANDLE * (STDMETHODCALLTYPE *)(void *, D3D12_CPU_DESCRIPTOR_HANDLE *))(*(void ***)S.heap)[9])(S.heap, &cpu);
			((D3D12_GPU_DESCRIPTOR_HANDLE * (STDMETHODCALLTYPE *)(void *, D3D12_GPU_DESCRIPTOR_HANDLE *))(*(void ***)S.heap)[10])(S.heap, &gpu);
			D3D12_SHADER_RESOURCE_VIEW_DESC srv;
			memset(&srv, 0, sizeof(srv));
			srv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			srv.Shader4ComponentMapping = 0x1688; // D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
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
			cpu.ptr += S.desc_inc;
			ID3D12Device_CreateUnorderedAccessView(S.dev, S.depth, NULL, &uav, cpu);
			ID3D12DescriptorHeap *heaps[1] = {S.heap};
			ID3D12GraphicsCommandList_SetDescriptorHeaps(S.list, 1, heaps);
			ID3D12GraphicsCommandList_SetComputeRootSignature(S.list, S.rs);
			ID3D12GraphicsCommandList_SetPipelineState(S.list, drd.SampleDesc.Count > 1 ? S.pso_ms : S.pso_1);
			{ // the game's depth is mapped into the viewport's range (the world is drawn in 1/64..1; the first-person weapon in front of it, below 1/64): FSR gets 0..1, the weapon at 0
				float dvals[4] = {0.0f, 1.0f, 0.0f, 0.0f};
				if (g_cam_maxz > g_cam_minz) {
					dvals[0] = g_cam_minz;
					dvals[1] = 1.0f / (g_cam_maxz - g_cam_minz);
				}
				ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(S.list, 0, 4, dvals, 0);
			}
			ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(S.list, 1, gpu);
			ID3D12GraphicsCommandList_Dispatch(S.list, (S.iw + 7) / 8, (S.ih + 7) / 8, 1);
			barrier(b, &n, S.depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			ID3D12GraphicsCommandList_ResourceBarrier(S.list, n, b);
			n = 0;
			S.depth_state_ok = 1;
			depth_fresh = 1;
		}
	}
	if (first) { // the first frame: the textures are in the common state; the constant ones go to the read state, the output to the UAV state
		if (!S.depth_state_ok) { // no depth from the game yet: the texture of zeros is read as it is
			barrier(b, &n, S.depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			S.depth_state_ok = 1;
		}
		barrier(b, &n, S.motion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		barrier(b, &n, S.output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		barrier(b, &n, S.orig, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		barrier(b, &n, S.final, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		ID3D12GraphicsCommandList_ResourceBarrier(S.list, n, b);
		n = 0;
	}
	// the camera motion vectors: the game's depth and the matrix that takes this frame's clip space to the previous frame's (a compute pass)
	const float jit_x = g_jit_x, jit_y = g_jit_y;
	if (g_mv_on && depth_fresh && S.pso_mv && g_vp_new_ok && g_vp_prev_ok && g_cam_maxz > g_cam_minz) {
		double mc[16], mp[16], ic[16], t[16];
		vp_to_matrix(g_vp_new, mc);
		vp_to_matrix(g_vp_prev, mp);
		if (invert4(mc, ic)) {
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
			vals[18] = jit_x;
			vals[19] = jit_y;
			vals[20] = g_mv_flip ? -1.0f : 1.0f;
			barrier(b, &n, S.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			ID3D12GraphicsCommandList_ResourceBarrier(S.list, n, b);
			n = 0;
			D3D12_CPU_DESCRIPTOR_HANDLE cpu;
			D3D12_GPU_DESCRIPTOR_HANDLE gpu;
			((D3D12_CPU_DESCRIPTOR_HANDLE * (STDMETHODCALLTYPE *)(void *, D3D12_CPU_DESCRIPTOR_HANDLE *))(*(void ***)S.heap)[9])(S.heap, &cpu);
			((D3D12_GPU_DESCRIPTOR_HANDLE * (STDMETHODCALLTYPE *)(void *, D3D12_GPU_DESCRIPTOR_HANDLE *))(*(void ***)S.heap)[10])(S.heap, &gpu);
			cpu.ptr += 2 * S.desc_inc;
			gpu.ptr += 2 * S.desc_inc;
			D3D12_SHADER_RESOURCE_VIEW_DESC srv;
			memset(&srv, 0, sizeof(srv));
			srv.Format = DXGI_FORMAT_R32_FLOAT;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = 0x1688;
			srv.Texture2D.MipLevels = 1;
			ID3D12Device_CreateShaderResourceView(S.dev, S.depth, &srv, cpu);
			D3D12_UNORDERED_ACCESS_VIEW_DESC uav;
			memset(&uav, 0, sizeof(uav));
			uav.Format = DXGI_FORMAT_R16G16_FLOAT;
			uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			cpu.ptr += S.desc_inc;
			ID3D12Device_CreateUnorderedAccessView(S.dev, S.motion, NULL, &uav, cpu);
			ID3D12DescriptorHeap *heaps[1] = {S.heap};
			ID3D12GraphicsCommandList_SetDescriptorHeaps(S.list, 1, heaps);
			ID3D12GraphicsCommandList_SetComputeRootSignature(S.list, S.rs_mv);
			ID3D12GraphicsCommandList_SetPipelineState(S.list, S.pso_mv);
			ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(S.list, 0, 24, vals, 0);
			ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(S.list, 1, gpu);
			ID3D12GraphicsCommandList_Dispatch(S.list, (S.iw + 7) / 8, (S.ih + 7) / 8, 1);
			barrier(b, &n, S.motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			ID3D12GraphicsCommandList_ResourceBarrier(S.list, n, b);
			n = 0;
			mv_done = 1;
			if (g_jit_log++ < 3)
				log_printf("fsr3: motion vectors computed (jitter %.3f,%.3f px, depth range %g..%g, phases %d)", jit_x, jit_y, g_cam_minz, g_cam_maxz, S.phase_count);
		}
	}
	// the game's whole frame (for its HUD, which the compose pass keeps): only for that pass, which is the old way (QA: FSR3_COMPOSE=1). It was copied in every frame
	// before 2026-11-11, a whole output-size copy that nothing read
	if (ENV_HAS("FSR3_COMPOSE")) {
		barrier(b, &n, S.orig, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
		ID3D12GraphicsCommandList_ResourceBarrier(S.list, n, b);
		n = 0;
		ID3D12GraphicsCommandList_CopyResource(S.list, S.orig, bb12);
		barrier(b, &n, S.orig, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		ID3D12GraphicsCommandList_ResourceBarrier(S.list, n, b);
		n = 0;
	}
	// the back buffer into the color texture
	barrier(b, &n, S.color, first ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
	ID3D12GraphicsCommandList_ResourceBarrier(S.list, n, b);
	n = 0;
	{ // the game's picture is in the top left of the back buffer
		D3D12_TEXTURE_COPY_LOCATION dst, src;
		D3D12_BOX box = {0, 0, 0, S.iw, S.ih, 1};
		memset(&dst, 0, sizeof(dst));
		memset(&src, 0, sizeof(src));
		dst.pResource = S.color;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.pResource = bb12;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		ID3D12GraphicsCommandList_CopyTextureRegion(S.list, &dst, 0, 0, 0, &src, &box);
	}
	barrier(b, &n, S.color, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	ID3D12GraphicsCommandList_ResourceBarrier(S.list, n, b);
	n = 0;

	if (ts_on)
		ts_mark(slot, 1);
	// motion refinement: the camera vectors corrected where the previous picture, moved by them, does not match the current one (moving characters, the weapon)
	ID3D12Resource *mv_tex = S.motion;
	if (mv_done && S.prev_valid && S.pso_rfa && S.pso_rfb && S.motion2 && S.prev && S.block && g_refine) {
		const UINT bw = (S.iw + 7) / 8, bh = (S.ih + 7) / 8;
		barrier(b, &n, S.motion2, S.m2_ok ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		if (!S.block_ok)
			barrier(b, &n, S.block, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		ID3D12GraphicsCommandList_ResourceBarrier(S.list, n, b);
		n = 0;
		S.block_ok = 1;
		D3D12_CPU_DESCRIPTOR_HANDLE cpu;
		D3D12_GPU_DESCRIPTOR_HANDLE gpu;
		((D3D12_CPU_DESCRIPTOR_HANDLE * (STDMETHODCALLTYPE *)(void *, D3D12_CPU_DESCRIPTOR_HANDLE *))(*(void ***)S.heap)[9])(S.heap, &cpu);
		((D3D12_GPU_DESCRIPTOR_HANDLE * (STDMETHODCALLTYPE *)(void *, D3D12_GPU_DESCRIPTOR_HANDLE *))(*(void ***)S.heap)[10])(S.heap, &gpu);
		cpu.ptr += 8 * S.desc_inc;
		gpu.ptr += 8 * S.desc_inc;
		D3D12_SHADER_RESOURCE_VIEW_DESC srv;
		memset(&srv, 0, sizeof(srv));
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping = 0x1688;
		srv.Texture2D.MipLevels = 1;
		srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		ID3D12Device_CreateShaderResourceView(S.dev, S.color, &srv, cpu);
		cpu.ptr += S.desc_inc;
		ID3D12Device_CreateShaderResourceView(S.dev, S.prev, &srv, cpu);
		cpu.ptr += S.desc_inc;
		srv.Format = DXGI_FORMAT_R16G16_FLOAT;
		ID3D12Device_CreateShaderResourceView(S.dev, S.motion, &srv, cpu);
		srv.Format = DXGI_FORMAT_R32_FLOAT;
		cpu.ptr += S.desc_inc;
		ID3D12Device_CreateShaderResourceView(S.dev, S.depth, &srv, cpu);
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav;
		memset(&uav, 0, sizeof(uav));
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		uav.Format = DXGI_FORMAT_R32_UINT;
		cpu.ptr += S.desc_inc;
		ID3D12Device_CreateUnorderedAccessView(S.dev, S.block, NULL, &uav, cpu);
		uav.Format = DXGI_FORMAT_R16G16_FLOAT;
		cpu.ptr += S.desc_inc;
		ID3D12Device_CreateUnorderedAccessView(S.dev, S.motion2, NULL, &uav, cpu);
		const float jsign = ENV_HAS("FSR3_RF_JFLIP") ? -1.0f : 1.0f;
		const float thr = ENV_NUM("FSR3_RF_THR", 0.03f);
		// mode 2 (classic): every block may take a vector; mode 1 (strict): near, clearly better, good in itself, at least minmag pixels off the camera's (see rf_common)
		float dgate = 2.0f, ratio = 0.6f, absmax = 1e9f, minmag = 0.0f;
		if (g_refine == 1) {
			ratio = ENV_NUM("FSR3_RF_RATIO", 0.4f);
			absmax = ENV_NUM("FSR3_RF_ABS", 0.045f);
			minmag = ENV_NUM("FSR3_RF_MINMAG", 1.5f);
			const float dist = ENV_NUM("FSR3_RF_DIST", 3000.0f); // game units (inches): ~75 m
			float vf, zn, zf;
			if (g_vp_new_ok && dist > 0.0f && g_cam_maxz > g_cam_minz && camera_from_matrix(g_vp_new, &vf, &zn, &zf))
				dgate = (g_cam_a + g_cam_bw / dist - g_cam_minz) / (g_cam_maxz - g_cam_minz); // the same mapping as the depth pass
		}
		const float cvals[12] = {0, 0, jit_x * jsign, jit_y * jsign, g_pjit_x * jsign, g_pjit_y * jsign, thr, g_subpx ? 1.0f : 0.0f, dgate, ratio, absmax, minmag};
		unsigned ci[12];
		memcpy(ci, cvals, sizeof(ci));
		ci[0] = S.iw;
		ci[1] = S.ih;
		ID3D12DescriptorHeap *heaps[1] = {S.heap};
		ID3D12GraphicsCommandList_SetDescriptorHeaps(S.list, 1, heaps);
		ID3D12GraphicsCommandList_SetComputeRootSignature(S.list, S.rs_rf);
		ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(S.list, 0, 12, ci, 0);
		ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(S.list, 1, gpu);
		ID3D12GraphicsCommandList_SetPipelineState(S.list, S.pso_rfa);
		ID3D12GraphicsCommandList_Dispatch(S.list, (bw + 7) / 8, (bh + 7) / 8, 1);
		D3D12_RESOURCE_BARRIER ub;
		memset(&ub, 0, sizeof(ub));
		ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		ub.UAV.pResource = S.block;
		ID3D12GraphicsCommandList_ResourceBarrier(S.list, 1, &ub);
		ID3D12GraphicsCommandList_SetPipelineState(S.list, S.pso_rfb);
		ID3D12GraphicsCommandList_Dispatch(S.list, (S.iw + 7) / 8, (S.ih + 7) / 8, 1);
		barrier(b, &n, S.motion2, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		ID3D12GraphicsCommandList_ResourceBarrier(S.list, n, b);
		n = 0;
		S.m2_ok = 1;
		mv_tex = S.motion2;
		static int told;
		if (!told++)
			log_printf("fsr3: motion refinement on, %s (blocks %ux%u, jitter now %.3f,%.3f previous %.3f,%.3f; distance gate depth %.5f, ratio %.2f, absolute limit %.3f, minimum %.1f px, quarter pixels %d)", g_refine == 1 ? "strict" : "classic", bw, bh, jit_x, jit_y, g_pjit_x, g_pjit_y, dgate, ratio, absmax, minmag, g_subpx);
	}

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	float dt_ms = S.last_tick.QuadPart ? (float)((now.QuadPart - S.last_tick.QuadPart) * 1000.0 / (double)S.freq.QuadPart) : 16.6f;
	S.last_tick = now;

	struct ffxDispatchDescUpscale d;
	memset(&d, 0, sizeof(d));
	d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
	d.commandList = S.list;
	d.color = ffx_res(S.color, S.iw, S.ih, FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	d.depth = ffx_res(S.depth, S.iw, S.ih, FFX_API_SURFACE_FORMAT_R32_FLOAT, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	d.motionVectors = ffx_res(mv_tex, S.iw, S.ih, FFX_API_SURFACE_FORMAT_R16G16_FLOAT, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	d.output = ffx_res(S.output, S.w, S.h, FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
	d.jitterOffset.x = jit_x;
	d.jitterOffset.y = jit_y;
	d.motionVectorScale.x = 1.0f;
	d.motionVectorScale.y = 1.0f;
	d.renderSize.width = S.iw;
	d.renderSize.height = S.ih;
	d.upscaleSize.width = S.w;
	d.upscaleSize.height = S.h;
	{ // FSR's own sharpening (RCAS): the soft look of an upscaled frame without it was reported as "soapy"
		int sh = sharpness_pct();
		d.enableSharpening = sh > 0;
		d.sharpness = (float)sh / 100.0f;
	}
	d.frameTimeDelta = dt_ms;
	d.preExposure = 1.0f;
	{ // FSR's history is dropped when the picture does not continue the previous one: the first frame, a frame after one without a 3D view (menu, loading screen, film:
	  // no previous matrix), or a camera cut (camera_cut). Without it a cutscene's new shot was blended with the old one for a few frames. FSR3_NORESET=1: the first frame only.
		static unsigned resets;
		char why[160] = "first frame";
		int reset = first;
		if (!first && !ENV_HAS("FSR3_NORESET")) {
			if (!g_vp_prev_ok) {
				reset = 1;
				snprintf(why, sizeof(why), "no 3D view in the frame before (menu, loading screen, film)");
			} else if (g_vp_space != g_vp_prev_space) {
				reset = 1;
				snprintf(why, sizeof(why), "the camera matrix changed from %s to %s", g_vp_prev_space ? "camera-relative" : "world", g_vp_space ? "camera-relative" : "world");
			} else if (g_vp_new_ok && camera_cut(g_vp_prev, g_vp_new, why, sizeof(why)))
				reset = 1;
		}
		d.reset = reset;
		if (reset && !first && (++resets <= 20 || resets % 100 == 0))
			log_printf("fsr3: history reset %u at frame %u: %s", resets, S.frame, why);
	}
	d.cameraNear = 4.0f;
	d.cameraFar = 40000.0f;
	d.cameraFovAngleVertical = 1.0f;
	{ // the real camera of this frame (the fixed numbers above are the fallback)
		float vfov = 0, zn = 0, zf = 0;
		static int told;
		if (g_vp_new_ok && !ENV_HAS("FSR3_FIXEDCAM") && camera_from_matrix(g_vp_new, &vfov, &zn, &zf)) {
			d.cameraNear = zn;
			d.cameraFar = zf;
			d.cameraFovAngleVertical = vfov;
			if (told++ < 3 || told % 6000 == 0)
				log_printf("fsr3: camera from the matrix: vertical fov %.3f rad (%.1f deg), near %.2f, far %.1f", vfov, vfov * 57.29578f, zn, zf);
		} else if (told++ < 3) {
			log_printf("fsr3: camera from the matrix failed (vfov %.3f near %.3f far %.3f): fixed numbers", vfov, zn, zf);
		}
	}
	d.viewSpaceToMetersFactor = 0.0254f; // the game's units are inches
	d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
	if (ENV_HAS("FSR3_DEBUGVIEW"))
		d.flags |= FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
	if (ts_on)
		ts_mark(slot, 2);
	ffxReturnCode_t rc;
	if (settings_dlss())
		rc = dlss_dispatch(mv_tex, jit_x, jit_y, d.reset ? 1 : 0, dt_ms) ? FFX_API_RETURN_OK : FFX_API_RETURN_ERROR;
	else
		rc = S.dispatch(&S.ctx, &d.header);
	if (ts_on)
		ts_mark(slot, 3);
	static int dlog;
	if (dlog++ < 3 || rc != FFX_API_RETURN_OK) {
		static int bad;
		if (rc == FFX_API_RETURN_OK || bad++ < 5)
			log_printf("fsr3: %s -> %u (0 = OK), frame time %.1f ms", settings_dlss() ? "DLSS evaluate" : "ffxDispatch", (unsigned)rc, dt_ms);
	}

	if (rc == FFX_API_RETURN_OK && metric_on()) {
		metric_copy(b, &n, 0);
		metric_copy(b, &n, 1);
	}
	if (S.prev && S.pso_rfa) { // this frame's input is the next frame's previous picture
		barrier(b, &n, S.color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
		barrier(b, &n, S.prev, S.prev_ok ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
		ID3D12GraphicsCommandList_ResourceBarrier(S.list, n, b);
		n = 0;
		ID3D12GraphicsCommandList_CopyResource(S.list, S.prev, S.color);
		barrier(b, &n, S.color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		barrier(b, &n, S.prev, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		ID3D12GraphicsCommandList_ResourceBarrier(S.list, n, b);
		n = 0;
		S.prev_ok = 1;
		S.prev_valid = mv_done;
		g_pjit_x = jit_x;
		g_pjit_y = jit_y;
	}
	if (rc == FFX_API_RETURN_OK && !ENV_HAS("FSR3_SKIPCOPY") && S.pso_comp && ENV_HAS("FSR3_COMPOSE")) { // the old way: the output into the back buffer after the HUD, the HUD kept from the game's own frame (QA: FSR3_COMPOSE=1)
		barrier(b, &n, S.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		ID3D12GraphicsCommandList_ResourceBarrier(S.list, n, b);
		n = 0;
		D3D12_CPU_DESCRIPTOR_HANDLE cpu;
		D3D12_GPU_DESCRIPTOR_HANDLE gpu;
		((D3D12_CPU_DESCRIPTOR_HANDLE * (STDMETHODCALLTYPE *)(void *, D3D12_CPU_DESCRIPTOR_HANDLE *))(*(void ***)S.heap)[9])(S.heap, &cpu);
		((D3D12_GPU_DESCRIPTOR_HANDLE * (STDMETHODCALLTYPE *)(void *, D3D12_GPU_DESCRIPTOR_HANDLE *))(*(void ***)S.heap)[10])(S.heap, &gpu);
		cpu.ptr += 4 * S.desc_inc;
		gpu.ptr += 4 * S.desc_inc;
		D3D12_SHADER_RESOURCE_VIEW_DESC srv;
		memset(&srv, 0, sizeof(srv));
		srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping = 0x1688;
		srv.Texture2D.MipLevels = 1;
		ID3D12Device_CreateShaderResourceView(S.dev, S.output, &srv, cpu);
		cpu.ptr += S.desc_inc;
		ID3D12Device_CreateShaderResourceView(S.dev, S.orig, &srv, cpu);
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav;
		memset(&uav, 0, sizeof(uav));
		uav.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		cpu.ptr += S.desc_inc;
		ID3D12Device_CreateUnorderedAccessView(S.dev, S.final, NULL, &uav, cpu);
		const unsigned consts[4] = {S.iw, S.ih, 0, 0};
		ID3D12DescriptorHeap *heaps[1] = {S.heap};
		ID3D12GraphicsCommandList_SetDescriptorHeaps(S.list, 1, heaps);
		ID3D12GraphicsCommandList_SetComputeRootSignature(S.list, S.rs_comp);
		ID3D12GraphicsCommandList_SetPipelineState(S.list, S.pso_comp);
		ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(S.list, 0, 4, consts, 0);
		ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(S.list, 1, gpu);
		ID3D12GraphicsCommandList_Dispatch(S.list, (S.w + 7) / 8, (S.h + 7) / 8, 1);
		barrier(b, &n, S.final, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
		barrier(b, &n, S.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		ID3D12GraphicsCommandList_ResourceBarrier(S.list, n, b);
		n = 0;
		ID3D12GraphicsCommandList_CopyResource(S.list, bb12, S.final);
		barrier(b, &n, S.final, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		ID3D12GraphicsCommandList_ResourceBarrier(S.list, n, b);
		n = 0;
	} else if (rc == FFX_API_RETURN_OK && !ENV_HAS("FSR3_SKIPCOPY") && settings_dlss() && S.pso_sharp && sharpness_pct() > 0) {
		// DLSS: its output is sharpened here (RCAS, see sharpen_cs) into `final`, which goes to the back buffer
		static int told;
		const int pct = sharpness_pct();
		const float con = powf(2.0f, -(2.0f - 2.0f * (float)pct / 100.0f));
		barrier(b, &n, S.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		ID3D12GraphicsCommandList_ResourceBarrier(S.list, n, b);
		n = 0;
		D3D12_CPU_DESCRIPTOR_HANDLE cpu;
		D3D12_GPU_DESCRIPTOR_HANDLE gpu;
		((D3D12_CPU_DESCRIPTOR_HANDLE * (STDMETHODCALLTYPE *)(void *, D3D12_CPU_DESCRIPTOR_HANDLE *))(*(void ***)S.heap)[9])(S.heap, &cpu);
		((D3D12_GPU_DESCRIPTOR_HANDLE * (STDMETHODCALLTYPE *)(void *, D3D12_GPU_DESCRIPTOR_HANDLE *))(*(void ***)S.heap)[10])(S.heap, &gpu);
		cpu.ptr += 4 * S.desc_inc;
		gpu.ptr += 4 * S.desc_inc;
		D3D12_SHADER_RESOURCE_VIEW_DESC srv;
		memset(&srv, 0, sizeof(srv));
		srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping = 0x1688;
		srv.Texture2D.MipLevels = 1;
		ID3D12Device_CreateShaderResourceView(S.dev, S.output, &srv, cpu);
		cpu.ptr += S.desc_inc;
		ID3D12Device_CreateShaderResourceView(S.dev, S.output, &srv, cpu); // (the table's second texture: not read by this shader)
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav;
		memset(&uav, 0, sizeof(uav));
		uav.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		cpu.ptr += S.desc_inc;
		ID3D12Device_CreateUnorderedAccessView(S.dev, S.final, NULL, &uav, cpu);
		unsigned consts[4] = {S.w, S.h, 0, 0};
		memcpy(&consts[2], &con, 4);
		ID3D12DescriptorHeap *heaps[1] = {S.heap};
		ID3D12GraphicsCommandList_SetDescriptorHeaps(S.list, 1, heaps);
		ID3D12GraphicsCommandList_SetComputeRootSignature(S.list, S.rs_comp);
		ID3D12GraphicsCommandList_SetPipelineState(S.list, S.pso_sharp);
		ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(S.list, 0, 4, consts, 0);
		ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(S.list, 1, gpu);
		ID3D12GraphicsCommandList_Dispatch(S.list, (S.w + 7) / 8, (S.h + 7) / 8, 1);
		barrier(b, &n, S.final, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
		barrier(b, &n, S.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		ID3D12GraphicsCommandList_ResourceBarrier(S.list, n, b);
		n = 0;
		ID3D12GraphicsCommandList_CopyResource(S.list, bb12, S.final);
		barrier(b, &n, S.final, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		ID3D12GraphicsCommandList_ResourceBarrier(S.list, n, b);
		n = 0;
		if (!told++)
			log_printf("dlss: the output is sharpened (RCAS, sharpness %d of 100, constant %.3f)", pct, con);
	} else if (rc == FFX_API_RETURN_OK && !ENV_HAS("FSR3_SKIPCOPY")) { // no compose pass: the output as it is
		barrier(b, &n, S.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
		ID3D12GraphicsCommandList_ResourceBarrier(S.list, n, b);
		n = 0;
		ID3D12GraphicsCommandList_CopyResource(S.list, bb12, S.output);
		barrier(b, &n, S.output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		ID3D12GraphicsCommandList_ResourceBarrier(S.list, n, b);
		n = 0;
	} else {
		// FFX left the textures as they were declared: put the color texture back to where the next frame expects it (nothing to do: it is in the read state)
	}
	QueryPerformanceCounter(&c3);
	if (ts_on) {
		ts_mark(slot, 4);
		ID3D12GraphicsCommandList_ResolveQueryData(S.list, g_ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, slot * TS_PER_FRAME, TS_PER_FRAME, g_ts_rb, (UINT64)slot * TS_PER_FRAME * sizeof(UINT64));
		g_ts_valid[slot] = 1;
	}
	ID3D12GraphicsCommandList_Close(S.list);
	ID3D12CommandList *lists[1] = {(ID3D12CommandList *)S.list};
	ID3D12CommandQueue_ExecuteCommandLists(S.queue, 1, lists);
	UINT64 value = ++S.fence_value;
	ID3D12CommandQueue_Signal(S.queue, S.fence, value);
	S.alloc_fence[slot] = value;
	metric_finish(value);
	ID3D12Fence *fences[1] = {S.fence};
	vt->give_back(on12, (IDirect3DResource9 *)bb, 1, &value, fences);
	if (dsrc) {
		vt->give_back(on12, (IDirect3DResource9 *)dsurf, 1, &value, fences);
		ID3D12Resource_Release(dsrc);
		IDirect3DSurface9_Release(dsurf);
	}
	ID3D12Resource_Release(bb12);
	vt->release(on12);
	IDirect3DSurface9_Release(bb);
	QueryPerformanceCounter(&c4);
	g_cpu_acc[0] += cpu_ms(c0, c1); // (the depth's unwrap is inside the recording part: depth_source)
	g_cpu_acc[1] += cpu_ms(c1, c2);
	g_cpu_acc[2] += cpu_ms(c2, c3);
	g_cpu_acc[3] += cpu_ms(c3, c4);
	g_cpu_n++;
	S.frame++;
	return 1;
}

void fsr3_present(IDirect3DDevice9 *dev9)
{
	if (!dev9 || !fsr3_enabled())
		return;
	hook_device(dev9);
	g_frame++;
	{
		static char lastseq[400];
		if (g_armed && strcmp(lastseq, g_vpseq) && g_seq_log++ < 30) {
			log_printf("fsr3: viewports in frame %d:%s", g_frame, g_vpseq);
			snprintf(lastseq, sizeof(lastseq), "%s", g_vpseq);
		}
	}
	g_vpseq[0] = 0;
	g_vpseq_last = 0;
	{ // the set-up runs at every Present, so the hooks know the sizes before the first 3D frame
		IDirect3DSurface9 *bb = NULL;
		D3DSURFACE_DESC sd;
		if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev9, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
			if (SUCCEEDED(IDirect3DSurface9_GetDesc(bb, &sd)))
				setup(sd.Width, sd.Height);
			IDirect3DSurface9_Release(bb);
		}
	}
	if (g_frame % 120 == 0 && (g_ran ? g_ran_log++ < 60 : g_status_log++ < 8)) { // 3D frames and the others are counted apart (the menus run first)
		static LARGE_INTEGER t_last;
		LARGE_INTEGER t_now;
		QueryPerformanceCounter(&t_now);
		const double ms = t_last.QuadPart && S.freq.QuadPart ? (double)(t_now.QuadPart - t_last.QuadPart) * 1000.0 / (double)S.freq.QuadPart / 120.0 : 0.0;
		t_last = t_now;
		log_printf("fsr3: status frame %d: set up for %ux%u (render %ux%u), upscaled this frame %d, vp %d prev %d, %.2f ms per frame (refine %d)", g_frame, S.w, S.h, S.iw, S.ih, g_ran, g_vp_new_ok, g_vp_prev_ok, ms, g_refine);
	}
	// a frame of the world pass in which no world matrix was caught (FSR did not run: the game's picture stayed in the corner of the enlarged back buffer, owner's screenshot
	// in a Humvee turret): the next frame's viewports and vertex shader constants go to the log, three times at most
	if (g_saw_world_vp && !g_vp_new_ok && S.ok && g_autotrace < 3 && g_trace <= 0) {
		g_autotrace++;
		g_trace = 400;
		g_armed = 1;
		log_printf("fsr3: frame %d drew the world pass but no world view-projection matrix was caught: the next frame is traced", g_frame);
	}
	g_saw_world_vp = 0;
	// The last pass is normally found by the game setting the back buffer as the render target after its post-process copy (hook_srt9). In the sandstorm of Team Player it never does: the back
	// buffer stays the target and the game's composite pass draws its own render-size picture into the corner (owner's screenshots 2026-11-08: the 1280x720 picture in a corner; FSR run at
	// the first draw after the copy left the corner showing the un-upscaled picture over the upscaled one). So the frame is upscaled at the Present: the whole back buffer corner, the
	// game's composite and its HUD with it (the HUD is then as soft as the picture). FSR3_NOLATE=1: not.
	if (S.ok && g_stretched && !g_ran && g_vp_new_ok && !ENV_HAS("FSR3_NOLATE")) {
		static int told;
		g_ran = 1;
		if (!fsr3_frame(dev9))
			g_ran = 0;
		else if (told++ < 5)
			log_printf("fsr3: frame %d: FSR ran at the Present (the game set no render target after its post-process copy)", g_frame);
	}
	{ // a frame with a world matrix in which FSR did not run: the last pass was not found (no post-process copy of the back buffer, or its source was not the back buffer)
		static int nolast;
		if (S.ok && g_vp_new_ok && !g_ran && nolast++ < 5)
			log_printf("fsr3: frame %d: a world matrix was caught but FSR did not run (the post-process copy of the back buffer was %s)", g_frame, g_stretched ? "seen, but no render target set of the back buffer followed" : "not seen");
	}
	if (g_ran) {
		g_diag_frames++;
		g_jit_upl_sum += g_jit_uploads;
		g_bias_sw_sum += g_bias_switches;
		if (g_vp_distinct > 1)
			g_multi_frames++;
		if (g_diag_frames >= 600) {
			log_printf("fsr3: of %d upscaled frames %d had more than one world matrix; jittered matrix uploads %.1f per frame; texture bias switches %.1f per frame", g_diag_frames, g_multi_frames,
					   g_jit_upl_sum / g_diag_frames, g_bias_sw_sum / g_diag_frames);
			g_diag_frames = g_multi_frames = 0;
			g_jit_upl_sum = g_bias_sw_sum = 0.0;
		}
	}
	g_jit_uploads = 0;
	g_vp_distinct = 0;
	g_bias_switches = 0;
	if (g_ran && g_vp_new_ok) { // this frame's matrix is the previous one of the next frame
		memcpy(g_vp_prev, g_vp_new, sizeof(g_vp_prev));
		g_vp_prev_ok = 1;
		if (g_vp_space != g_vp_prev_space) {
			static int told;
			if (told++ < 10)
				log_printf("fsr3: frame %d: the camera matrix is %s now", g_frame, g_vp_space ? "camera-relative (no world matrix: a vehicle's turret)" : "a world matrix again");
		}
		g_vp_prev_space = g_vp_space;
	} else {
		g_vp_prev_ok = 0; // no 3D view in this frame (menu, loading): no reprojection next frame
	}
	g_vp_new_ok = 0;
	g_vp_rel_ok = 0;
	g_vp_space = 0;
	g_ran = 0;
	g_stretched = 0;
	if (g_world_ds) {
		IDirect3DSurface9_Release(g_world_ds);
		g_world_ds = NULL;
	}
	if (g_sweep && S.frame > 300) {
		static const struct {
			const char *name;
			float bias;
			int sharp, jit, mv, rf, sp;
		} var_all[] = {
			{"default", 1000.0f, -1, 1, 1, 1, 0},
			{"classic", 1000.0f, -1, 1, 1, 2, 1},
			{"norefine", 1000.0f, -1, 1, 1, 0, 0},
			{"sharp70", 1000.0f, 70, 1, 1, 1, 0},
			{"sharp85", 1000.0f, 85, 1, 1, 1, 0},
			{"bias-1.0", -1.0f, -1, 1, 1, 1, 0},
			{"bias-2.0", -2.0f, -1, 1, 1, 1, 0},
			{"nojitter", 1000.0f, -1, 0, 1, 1, 0},
		};
		// FSR3_SWEEP=refine: how FSR gets motion for what moves by itself (owner, 2026-11-08: "the picture ripples in the distance"): the strict correction (the default), the
		// camera's motion alone, the classic correction (every block, quarter pixels), and the strict one with quarter pixels; rf = fsr3_refine mode, sp = quarter pixels
		static const struct {
			const char *name;
			float bias;
			int sharp, jit, mv, rf, sp;
		} var_refine[] = {
			{"strict", 1000.0f, -1, 1, 1, 1, 0},
			{"camera", 1000.0f, -1, 1, 1, 0, 0},
			{"classic", 1000.0f, -1, 1, 1, 2, 1},
			{"strict_subpx", 1000.0f, -1, 1, 1, 1, 1},
		};
		// FSR3_SWEEP=grain: the texture bias and the sharpening, which trade grain (shimmer on MW2's sharp alpha-tested grass and fences) against softness (owner,
		// 2026-11-08: "soft and grainy"); AMD's bias for 1.5x is -1.58 and its docs say to back off for high-frequency content
		static const struct {
			const char *name;
			float bias;
			int sharp, jit, mv, rf, sp;
		} var_grain[] = {
			{"b-1.58_s60", -1.585f, 60, 1, 1, 1, 0},
			{"b-1.0_s60", -1.0f, 60, 1, 1, 1, 0},
			{"b-0.58_s60", -0.585f, 60, 1, 1, 1, 0},
			{"b-1.58_s30", -1.585f, 30, 1, 1, 1, 0},
			{"b-1.0_s30", -1.0f, 30, 1, 1, 1, 0},
			{"b-0.58_s30", -0.585f, 30, 1, 1, 1, 0},
			{"b-0.58_s0", -0.585f, 0, 1, 1, 1, 0},
		};
		const char *sweep_kind = getenv("FSR3_SWEEP");
		const int grain = !strcmp(sweep_kind, "grain");
		const int refine = !strcmp(sweep_kind, "refine");
		const int nvar = grain ? (int)(sizeof(var_grain) / sizeof(var_grain[0])) : refine ? (int)(sizeof(var_refine) / sizeof(var_refine[0])) : (int)(sizeof(var_all) / sizeof(var_all[0]));
		const int idx = (int)((S.frame - 300) / 120) % nvar;
		const struct {
			const char *name;
			float bias;
			int sharp, jit, mv, rf, sp;
		} *var = grain ? (const void *)var_grain : refine ? (const void *)var_refine : (const void *)var_all;
		if (idx != g_sw_idx) {
			g_sw_idx = idx;
			g_sw_name = var[idx].name;
			g_sw_bias = var[idx].bias;
			g_sw_sharp = var[idx].sharp;
			g_jit_on = var[idx].jit;
			g_mv_on = var[idx].mv;
			g_refine = var[idx].rf;
			g_subpx = var[idx].sp;
			g_bias_on = -1; // the bias of the world pass is set again
		}
	}
	next_jitter();
	if (g_shake_amp != 0.0f) { // the sway of the next frame: a sine of the clock, period 3 s
		const float t = (float)(GetTickCount() % 3000) / 3000.0f * 6.2831853f;
		g_shake_x = g_shake_amp * 0.01f * sinf(t);
	}
}
