// mw2coop_dlss.dll: the one part of the mod that talks to NVIDIA's NGX (DLSS Super Resolution). It is a separate DLL on purpose: NVIDIA's SDK is under the NVIDIA RTX SDKs License (not
// open source), so its headers, libraries and DLL are never part of this repository or of the main winmm.dll; the main mod loads this bridge at run time when [display] dlss=1
// (src/fsr3.c, the DLSS branch) and it is built only on a machine that has the SDK (src/dlss/build_dlss.sh, src/dlss/README.md).
//
// The C interface below (all functions take plain pointers, so the main mod needs no NVIDIA header):
//   dlss_bridge_init(device, dll_dir, data_dir, log)   NGX init on the game's Direct3D 12 device, checks that DLSS is available; 1 = ok
//   dlss_bridge_create(cmdlist, iw, ih, ow, oh, flags) (re)creates the DLSS feature for a render size and an output size on a command list that is being recorded; 1 = ok
//   dlss_bridge_evaluate(cmdlist, ...)                  one DLSS evaluation recorded on that list; 1 = ok
//   dlss_bridge_set_preset(n)                           the DLSS model for the next create (0 = NVIDIA's default, 10 = J, 11 = K, 12 = L, 13 = M)
//   dlss_bridge_last_result()                           the NGX result code of the last call that failed (for the log)
//   dlss_bridge_shutdown()
//
// This software contains source code provided by NVIDIA Corporation only through the headers it includes at build time; none of it is copied into this file.
#include <windows.h>
#include <d3d12.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_d3d.h"
#include "nvsdk_ngx_params.h"

typedef void (*bridge_log_t)(const char *);

static bridge_log_t g_log;
static ID3D12Device *g_dev;
static NVSDK_NGX_Parameter *g_params;
static NVSDK_NGX_Handle *g_handle;
static unsigned g_iw, g_ih;
static unsigned g_last_result;
static int g_ngx_lines;

static void say(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (g_log)
		g_log(buf);
}

// NGX's own log is long (hundreds of lines at start); only its errors and warnings are passed on, and not many
static void NVSDK_CONV ngx_log(const char *msg, NVSDK_NGX_Logging_Level lvl, NVSDK_NGX_Feature src)
{
	(void)lvl;
	(void)src;
	if (!msg || g_ngx_lines >= 40)
		return;
	if (strstr(msg, "error") || strstr(msg, "Error") || strstr(msg, "warn") || strstr(msg, "fail")) {
		if (strstr(msg, "failed to load (cmsid") || strstr(msg, "failed to load NGXCore") || strstr(msg, "newer GPU arch")) // normal cache lookups
			return;
		g_ngx_lines++;
		say("ngx: %.400s", msg);
	}
}

static NVSDK_NGX_PerfQuality_Value quality_for(unsigned iw, unsigned ow)
{
	const float s = (float)iw / (float)ow;
	if (s >= 0.95f)
		return NVSDK_NGX_PerfQuality_Value_DLAA;
	if (s >= 0.62f)
		return NVSDK_NGX_PerfQuality_Value_MaxQuality;
	if (s >= 0.54f)
		return NVSDK_NGX_PerfQuality_Value_Balanced;
	if (s >= 0.40f)
		return NVSDK_NGX_PerfQuality_Value_MaxPerf;
	return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

extern "C" __declspec(dllexport) int dlss_bridge_init(void *device, const wchar_t *dll_dir, const wchar_t *data_dir, bridge_log_t log)
{
	g_log = log;
	g_dev = (ID3D12Device *)device;
	if (!g_dev)
		return 0;
	static wchar_t dir[MAX_PATH];
	wcsncpy(dir, dll_dir ? dll_dir : L"", MAX_PATH - 1);
	wchar_t const *paths[1] = {dir};
	static NVSDK_NGX_FeatureCommonInfo ci;
	memset(&ci, 0, sizeof(ci));
	ci.PathListInfo.Path = paths;
	ci.PathListInfo.Length = 1;
	ci.LoggingInfo.LoggingCallback = ngx_log;
	ci.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
	ci.LoggingInfo.DisableOtherLoggingSinks = true;
	NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID("7a2f6f3c-3b1e-4d57-9a7e-5c0d9f2b8e41", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", data_dir ? data_dir : L"", g_dev, &ci, NVSDK_NGX_Version_API);
	if (NVSDK_NGX_FAILED(r)) {
		g_last_result = (unsigned)r;
		say("NVSDK_NGX_D3D12_Init failed: 0x%08x", (unsigned)r);
		return 0;
	}
	NVSDK_NGX_Parameter *caps = NULL;
	r = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
	if (NVSDK_NGX_FAILED(r) || !caps) {
		g_last_result = (unsigned)r;
		say("NVSDK_NGX_D3D12_GetCapabilityParameters failed: 0x%08x", (unsigned)r);
		return 0;
	}
	int avail = 0, needs_driver = 0;
	unsigned major = 0, minor = 0;
	NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
	NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
	NVSDK_NGX_Parameter_GetUI(caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
	NVSDK_NGX_Parameter_GetUI(caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
	say("DLSS Super Resolution available %d, needs an updated driver %d (minimum %u.%u)", avail, needs_driver, major, minor);
	if (!avail)
		return 0;
	r = NVSDK_NGX_D3D12_AllocateParameters(&g_params);
	if (NVSDK_NGX_FAILED(r) || !g_params) {
		g_last_result = (unsigned)r;
		say("NVSDK_NGX_D3D12_AllocateParameters failed: 0x%08x", (unsigned)r);
		return 0;
	}
	return 1;
}

// The DLSS model ("render preset") for the next dlss_bridge_create: 0 = NVIDIA's default for the quality mode (preset K, the transformer model, for DLAA / Quality / Balanced in SDK
// 310.9), otherwise a letter's number as in NVSDK_NGX_DLSS_Hint_Render_Preset (J = 10, K = 11, L = 12, M = 13).
static unsigned g_preset;
extern "C" __declspec(dllexport) void dlss_bridge_set_preset(unsigned preset)
{
	g_preset = preset;
}

extern "C" __declspec(dllexport) int dlss_bridge_create(void *cmdlist, unsigned iw, unsigned ih, unsigned ow, unsigned oh, unsigned extra_flags)
{
	if (!g_params || !cmdlist)
		return 0;
	if (g_handle) {
		NVSDK_NGX_D3D12_ReleaseFeature(g_handle);
		g_handle = NULL;
	}
	NVSDK_NGX_DLSS_Create_Params cp;
	memset(&cp, 0, sizeof(cp));
	cp.Feature.InWidth = iw;
	cp.Feature.InHeight = ih;
	cp.Feature.InTargetWidth = ow;
	cp.Feature.InTargetHeight = oh;
	cp.Feature.InPerfQualityValue = quality_for(iw, ow);
	// the game's color is LDR (not HDR); the motion vectors are at render size; the exposure is DLSS's own
	cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure | (int)extra_flags;
	// the model: the same hint for every quality mode (the mode in use is picked from the sizes above)
	NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, g_preset);
	NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, g_preset);
	NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, g_preset);
	NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, g_preset);
	NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, g_preset);
	NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT((ID3D12GraphicsCommandList *)cmdlist, 1, 1, &g_handle, g_params, &cp);
	if (NVSDK_NGX_FAILED(r)) {
		g_last_result = (unsigned)r;
		g_handle = NULL;
		say("DLSS feature (%ux%u -> %ux%u) could not be created: 0x%08x", iw, ih, ow, oh, (unsigned)r);
		return 0;
	}
	g_iw = iw;
	g_ih = ih;
	say("DLSS feature created: %ux%u -> %ux%u (quality hint %d, model preset %u; 0 = NVIDIA's default)", iw, ih, ow, oh, (int)cp.Feature.InPerfQualityValue, g_preset);
	return 1;
}

extern "C" __declspec(dllexport) int dlss_bridge_evaluate(void *cmdlist, void *color, void *depth, void *motion, void *output, float jitter_x, float jitter_y, int reset, float dt_ms)
{
	if (!g_handle || !g_params || !cmdlist)
		return 0;
	NVSDK_NGX_D3D12_DLSS_Eval_Params ep;
	memset(&ep, 0, sizeof(ep));
	ep.Feature.pInColor = (ID3D12Resource *)color;
	ep.Feature.pInOutput = (ID3D12Resource *)output;
	ep.pInDepth = (ID3D12Resource *)depth;
	ep.pInMotionVectors = (ID3D12Resource *)motion;
	ep.InJitterOffsetX = jitter_x; // in render pixels, the same offsets the projection got
	ep.InJitterOffsetY = jitter_y;
	ep.InRenderSubrectDimensions.Width = g_iw;
	ep.InRenderSubrectDimensions.Height = g_ih;
	ep.InReset = reset ? 1 : 0;
	ep.InMVScaleX = 1.0f;
	ep.InMVScaleY = 1.0f;
	ep.InPreExposure = 1.0f;
	ep.InExposureScale = 1.0f;
	ep.InFrameTimeDeltaInMsec = dt_ms;
	NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT((ID3D12GraphicsCommandList *)cmdlist, g_handle, g_params, &ep);
	if (NVSDK_NGX_FAILED(r)) {
		g_last_result = (unsigned)r;
		return 0;
	}
	return 1;
}

extern "C" __declspec(dllexport) unsigned dlss_bridge_last_result(void)
{
	return g_last_result;
}

extern "C" __declspec(dllexport) void dlss_bridge_shutdown(void)
{
	if (g_handle) {
		NVSDK_NGX_D3D12_ReleaseFeature(g_handle);
		g_handle = NULL;
	}
	if (g_params) {
		NVSDK_NGX_D3D12_DestroyParameters(g_params);
		g_params = NULL;
	}
	if (g_dev) {
		NVSDK_NGX_D3D12_Shutdown1(g_dev);
		g_dev = NULL;
	}
}
