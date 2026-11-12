#pragma once
#define COBJMACROS
#include <d3d9.h>

// FSR 1 spatial upscaling of the game's Direct3D 9 picture (upscale.c), settings.ini [display] upscale=1, upscale_width, upscale_height, upscale_sharpness.
int upscale_enabled(void);
// the native renderers: sets the sizes when [display] upscale is on and the game's size is not the output size (see upscale.c); the shader texts below are SM3 HLSL that natup.h turns into SM5
int upscale_native_sizes(int game_w, int game_h, int *out_width, int *out_height);
extern const char upscale_easu_hlsl[], upscale_rcas_hlsl[];
// CreateDevice: the back buffer gets the output size, the size the game asked for is kept as the internal one. 1 if changed.
int upscale_adjust_params(D3DPRESENT_PARAMETERS *pp);
// After CreateDevice: the window's client area is made the output size.
void upscale_device_made(HWND window);
// The size the game renders at (for the mouse's scale); 0 if the upscaler is off.
int upscale_internal_size(int *w, int *h);
// At a Present, after the picture is composed and before overlays: the internal part of the back buffer is upscaled to all of it.
void upscale_present(IDirect3DDevice9 *dev);
