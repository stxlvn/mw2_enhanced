// The mod's own textures on either renderer (see modtex.h).
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dx/natboot.h"
#include "log.h"
#include "modtex.h"

static unsigned level_dim(unsigned v, unsigned level) { return (v >> level) ? (v >> level) : 1; }
static int is_block(unsigned fmt) { return fmt == D3DFMT_DXT1 || fmt == D3DFMT_DXT3 || fmt == D3DFMT_DXT5; }
static unsigned level_pitch(unsigned fmt, unsigned w)
{
	if (fmt == D3DFMT_DXT1)
		return ((w + 3) / 4) * 8;
	if (is_block(fmt))
		return ((w + 3) / 4) * 16;
	if (fmt == D3DFMT_A8 || fmt == D3DFMT_L8)
		return w;
	return w * 4;
}

// the native renderer's locks: a level is a buffer (read-only: the level read back; otherwise zeros), written to the image at the unlock
static struct {
	void *tex;
	unsigned level;
	void *buf;
	unsigned pitch;
	int write;
} locks[8];

HRESULT modtex_get_level_desc(void *tex, UINT level, D3DSURFACE_DESC *desc)
{
	if (!tex)
		return E_POINTER;
	if (!natboot_active())
		return IDirect3DTexture9_GetLevelDesc((IDirect3DTexture9 *)tex, level, desc);
	unsigned w, h, levels, max_size, fmt;
	if (!natboot_tex_info(tex, &w, &h, &levels, &max_size, &fmt))
		return E_FAIL;
	memset(desc, 0, sizeof(*desc));
	desc->Width = level_dim(w, level);
	desc->Height = level_dim(h, level);
	desc->Format = (D3DFORMAT)fmt;
	desc->Pool = D3DPOOL_MANAGED;
	return S_OK;
}

UINT modtex_levels(void *tex)
{
	if (!natboot_active())
		return (UINT)IDirect3DTexture9_GetLevelCount((IDirect3DTexture9 *)tex);
	unsigned w, h, levels, max_size, fmt;
	return natboot_tex_info(tex, &w, &h, &levels, &max_size, &fmt) ? levels : 1;
}

void modtex_max_size(void *tex, int *w, int *h)
{
	*w = *h = 0;
	if (natboot_active()) {
		unsigned tw, th, levels, max_size, fmt;
		if (natboot_tex_info(tex, &tw, &th, &levels, &max_size, &fmt))
			*w = *h = (int)max_size;
		return;
	}
	IDirect3DDevice9 *dev = NULL;
	D3DCAPS9 caps;
	if (SUCCEEDED(IDirect3DTexture9_GetDevice((IDirect3DTexture9 *)tex, &dev)) && dev) {
		if (SUCCEEDED(IDirect3DDevice9_GetDeviceCaps(dev, &caps))) {
			*w = (int)caps.MaxTextureWidth;
			*h = (int)caps.MaxTextureHeight;
		}
		IDirect3DDevice9_Release(dev);
	}
}

void *modtex_create(void *like, UINT w, UINT h, UINT levels, D3DFORMAT format)
{
	if (natboot_active())
		return natboot_tex_create(w, h, levels, (unsigned)format);
	IDirect3DDevice9 *dev = NULL;
	IDirect3DTexture9 *tex = NULL;
	if (!like || FAILED(IDirect3DTexture9_GetDevice((IDirect3DTexture9 *)like, &dev)) || !dev)
		return NULL;
	HRESULT hr = IDirect3DDevice9_CreateTexture(dev, w, h, levels, 0, format, D3DPOOL_MANAGED, &tex, NULL);
	IDirect3DDevice9_Release(dev);
	return FAILED(hr) ? NULL : tex;
}

HRESULT modtex_lock(void *tex, UINT level, D3DLOCKED_RECT *lr, DWORD flags)
{
	if (!natboot_active())
		return IDirect3DTexture9_LockRect((IDirect3DTexture9 *)tex, level, lr, NULL, flags);
	D3DSURFACE_DESC d;
	if (FAILED(modtex_get_level_desc(tex, level, &d)))
		return E_FAIL;
	int slot = -1;
	for (int i = 0; i < 8 && slot < 0; i++)
		if (!locks[i].tex)
			slot = i;
	if (slot < 0)
		return E_FAIL;
	void *buf = NULL;
	unsigned pitch = 0, rows = 0;
	const int read_only = (flags & D3DLOCK_READONLY) != 0;
	if (read_only) {
		if (!natboot_tex_download(tex, level, &buf, &pitch, &rows))
			return E_FAIL;
		const unsigned want_rows = is_block(d.Format) ? (d.Height + 3) / 4 : d.Height;
		if (rows < want_rows || pitch < level_pitch(d.Format, d.Width)) { // (the image's layout is not what its format says: the caller's loops would read past the buffer)
			log_printf("modtex: level %u of %p read back as %u rows of %u bytes, %ux%u format %u needs %u rows of %u: refused", level, tex, rows, pitch, d.Width, d.Height, (unsigned)d.Format, want_rows, level_pitch(d.Format, d.Width));
			free(buf);
			return E_FAIL;
		}
	} else {
		pitch = level_pitch(d.Format, d.Width);
		rows = is_block(d.Format) ? (d.Height + 3) / 4 : d.Height;
		buf = calloc(1, (size_t)pitch * rows + 1024); // (slack: the callers' mip loops write whole rows of the level above's width into the smallest levels)
		if (!buf)
			return E_OUTOFMEMORY;
	}
	locks[slot].tex = tex;
	locks[slot].level = level;
	locks[slot].buf = buf;
	locks[slot].pitch = pitch;
	locks[slot].write = !read_only;
	lr->pBits = buf;
	lr->Pitch = (INT)pitch;
	return S_OK;
}

HRESULT modtex_unlock(void *tex, UINT level)
{
	if (!natboot_active())
		return IDirect3DTexture9_UnlockRect((IDirect3DTexture9 *)tex, level);
	for (int i = 0; i < 8; i++)
		if (locks[i].tex == tex && locks[i].level == level) {
			if (locks[i].write)
				natboot_tex_upload(tex, level, locks[i].buf, locks[i].pitch);
			free(locks[i].buf);
			locks[i].tex = NULL;
			return S_OK;
		}
	return E_FAIL;
}

void modtex_release(void *tex)
{
	if (!tex)
		return;
	if (natboot_active())
		natboot_tex_release(tex);
	else
		IDirect3DTexture9_Release((IDirect3DTexture9 *)tex);
}
