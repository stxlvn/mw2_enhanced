#pragma once
// Swap chain helpers shared by the Direct3D 11 and 12 back ends (nat11.c, nat12.c): tearing (variable refresh rate) and the frame latency waitable object. Flip model swap chains only.
#define COBJMACROS
#include <windows.h>
#include <dxgi1_5.h>

// the system allows a present with tearing (the display's variable refresh rate / uncapped frames in a window)
static int ns_tearing_supported(IUnknown *factory)
{
	IDXGIFactory5 *f5 = NULL;
	BOOL on = FALSE;
	if (SUCCEEDED(IUnknown_QueryInterface(factory, &IID_IDXGIFactory5, (void **)&f5)) && f5) {
		if (FAILED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &on, sizeof(on))))
			on = FALSE;
		IDXGIFactory5_Release(f5);
	}
	return on != FALSE;
}

// the swap chain's flags (the same ones go to every ResizeBuffers)
static UINT ns_swap_flags(int tearing)
{
	return DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0) | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
}

// sets how many frames may be queued and returns the handle that is signalled when the swap chain can take another one (NULL when unsupported)
static HANDLE ns_waitable(IUnknown *swap, unsigned frames)
{
	IDXGISwapChain2 *s2 = NULL;
	HANDLE h = NULL;
	if (SUCCEEDED(IUnknown_QueryInterface(swap, &IID_IDXGISwapChain2, (void **)&s2)) && s2) {
		IDXGISwapChain2_SetMaximumFrameLatency(s2, frames);
		h = IDXGISwapChain2_GetFrameLatencyWaitableObject(s2);
		IDXGISwapChain2_Release(s2);
	}
	return h;
}

// Present's flags: with tearing only when the swap chain was made for it, the sync interval is 0 and the window is not in exclusive full screen
static UINT ns_present_flags(int tearing_chain, int vsync, int fullscreen_set)
{
	return (tearing_chain && !vsync && !fullscreen_set) ? DXGI_PRESENT_ALLOW_TEARING : 0;
}
