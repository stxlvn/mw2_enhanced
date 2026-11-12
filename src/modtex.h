#pragma once
// The textures the mod makes or reads on the game's device (the logo, the gamepad pictures, the font atlas): Direct3D 9 textures in the game's own renderer, images of the native renderer when
// that is on (the same field of the game's image struct holds either). These functions are the Direct3D 9 calls the mod used (GetLevelDesc, LockRect, UnlockRect, CreateTexture ...) for both.
#include <windows.h>
#include <d3d9.h>

// the description of level `level` (Width, Height, Format, Pool: the native renderer's images count as managed)
HRESULT modtex_get_level_desc(void *tex, UINT level, D3DSURFACE_DESC *desc);
UINT modtex_levels(void *tex);
// a texture of `like`'s renderer (like: an existing texture of the game); NULL on failure
void *modtex_create(void *like, UINT w, UINT h, UINT levels, D3DFORMAT format);
HRESULT modtex_lock(void *tex, UINT level, D3DLOCKED_RECT *lr, DWORD flags);
HRESULT modtex_unlock(void *tex, UINT level);
void modtex_release(void *tex);
// the largest texture the renderer takes (0 when it cannot say)
void modtex_max_size(void *tex, int *w, int *h);
