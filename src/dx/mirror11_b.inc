// part B of mirror11.c (included there): the Direct3D 9 resources the game creates, and their Direct3D 11 twins
static UINT mip_count(UINT w, UINT h)
{
	UINT n = 1;
	while (w > 1 || h > 1) {
		w = w > 1 ? w >> 1 : 1;
		h = h > 1 ? h >> 1 : 1;
		n++;
	}
	return n;
}

static void level_size(const tex_t *t, UINT level, UINT *w, UINT *h)
{
	*w = t->w >> level ? t->w >> level : 1;
	*h = t->h >> level ? t->h >> level : 1;
}

static int fmt_block(D3DFORMAT f)
{
	DXGI_FORMAT a, b, c;
	int e, blk, bpp;
	return fmt_info(f, &a, &b, &c, &e, &blk, &bpp) ? blk : 0;
}
static int fmt_bpp(D3DFORMAT f)
{
	DXGI_FORMAT a, b, c;
	int e, blk, bpp;
	return fmt_info(f, &a, &b, &c, &e, &blk, &bpp) ? bpp : 0;
}

static UINT level_row_bytes(const tex_t *t, UINT level)
{
	UINT w, h;
	level_size(t, level, &w, &h);
	const int blk = fmt_block(t->fmt);
	return blk ? ((w + 3) / 4) * (UINT)blk : w * (UINT)fmt_bpp(t->fmt);
}
static UINT level_rows(const tex_t *t, UINT level)
{
	UINT w, h;
	level_size(t, level, &w, &h);
	return fmt_block(t->fmt) ? (h + 3) / 4 : h;
}

static tex_t *tex_make(int kind, UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT fmt)
{
	tex_t *t = calloc(1, sizeof(*t));
	t->kind = kind;
	t->w = w;
	t->h = h;
	t->d = d;
	t->usage = usage;
	t->fmt = fmt;
	t->rt = (usage & D3DUSAGE_RENDERTARGET) != 0;
	t->depth = (usage & D3DUSAGE_DEPTHSTENCIL) != 0;
	t->levels = levels ? levels : mip_count(w, h);
	if (kind != 1) {
		t->failed = 1;
		InterlockedIncrement((volatile LONG *)&stat.tex_unsupported);
		return t;
	}
	int blk, bpp;
	if (!fmt_info(fmt, &t->dxgi, &t->dxgi_srgb, &t->srv_fmt, &t->expand, &blk, &bpp)) {
		t->failed = 1;
		static LONG logged;
		if (InterlockedIncrement(&logged) <= 20)
			log_printf("mirror11: texture format %x (%ux%u) is not mapped yet", (unsigned)fmt, w, h);
		InterlockedIncrement((volatile LONG *)&stat.tex_unsupported);
		return t;
	}
	D3D11_TEXTURE2D_DESC td = {0};
	td.Width = w;
	td.Height = h;
	td.MipLevels = t->levels;
	td.ArraySize = 1;
	td.Format = t->dxgi;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (t->rt)
		td.BindFlags |= D3D11_BIND_RENDER_TARGET;
	if (t->depth || fmt == 0x5a544e49)
		td.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
	HRESULT hr = ID3D11Device_CreateTexture2D(dev11, &td, NULL, &t->tex);
	if (FAILED(hr)) {
		t->failed = 1;
		static LONG logged;
		if (InterlockedIncrement(&logged) <= 20)
			log_printf("mirror11: CreateTexture2D %ux%u levels %u format %x (dxgi %d) failed: %lx", w, h, t->levels, (unsigned)fmt, (int)t->dxgi, (unsigned long)hr);
		return t;
	}
	D3D11_SHADER_RESOURCE_VIEW_DESC sd = {0};
	sd.Format = t->srv_fmt;
	sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	sd.Texture2D.MipLevels = t->levels;
	ID3D11Device_CreateShaderResourceView(dev11, (ID3D11Resource *)t->tex, &sd, &t->srv);
	if (t->dxgi_srgb) {
		sd.Format = t->dxgi_srgb;
		ID3D11Device_CreateShaderResourceView(dev11, (ID3D11Resource *)t->tex, &sd, &t->srv_srgb);
	}
	if (t->rt) {
		D3D11_RENDER_TARGET_VIEW_DESC rd = {0};
		rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		rd.Format = t->srv_fmt;
		ID3D11Device_CreateRenderTargetView(dev11, (ID3D11Resource *)t->tex, &rd, &t->rtv);
		if (t->dxgi_srgb) {
			rd.Format = t->dxgi_srgb;
			ID3D11Device_CreateRenderTargetView(dev11, (ID3D11Resource *)t->tex, &rd, &t->rtv_srgb);
		}
	}
	if (td.BindFlags & D3D11_BIND_DEPTH_STENCIL) {
		D3D11_DEPTH_STENCIL_VIEW_DESC dd = {0};
		dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		dd.Format = t->dxgi == DXGI_FORMAT_R16_TYPELESS ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D24_UNORM_S8_UINT;
		ID3D11Device_CreateDepthStencilView(dev11, (ID3D11Resource *)t->tex, &dd, &t->dsv);
	}
	return t;
}

static void tex_sync(tex_t *t)
{
	if (!t->tex || !t->dirty || !t->shadow)
		return;
	t->dirty = 0;
	for (UINT l = 0; l < t->levels; l++) {
		if (!t->shadow[l])
			continue;
		const UINT rb = level_row_bytes(t, l), rows = level_rows(t, l);
		if (t->expand) { // L8 / A8L8 to 8888 (B, G, R = L; A = 255 or the data's)
			UINT w, h;
			level_size(t, l, &w, &h);
			uint32_t *tmp = malloc((size_t)w * h * 4);
			for (UINT y = 0; y < h; y++)
				for (UINT x = 0; x < w; x++) {
					const uint8_t *s = t->shadow[l] + (size_t)y * rb + x * (t->expand == 1 ? 1 : 2);
					const uint32_t lum = s[0], a = t->expand == 1 ? 255 : s[1];
					tmp[(size_t)y * w + x] = a << 24 | lum << 16 | lum << 8 | lum;
				}
			ID3D11DeviceContext_UpdateSubresource(ctx11, (ID3D11Resource *)t->tex, l, NULL, tmp, w * 4, 0);
			free(tmp);
		} else {
			(void)rows;
			ID3D11DeviceContext_UpdateSubresource(ctx11, (ID3D11Resource *)t->tex, l, NULL, t->shadow[l], rb, 0);
		}
	}
}

static tex_t *tex_of(void *base)
{
	return base ? (tex_t *)pmap_get(&textures, base) : NULL;
}

static HRESULT STDMETHODCALLTYPE create_texture_hook(IDirect3DDevice9 *d, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *shared)
{
	HRESULT hr = create_texture_o(d, w, h, levels, usage, fmt, pool, out, shared);
	if (SUCCEEDED(hr) && out && *out && dev11) {
		pmap_put(&textures, *out, tex_make(1, w, h, 1, levels, usage, fmt));
		void **vt = *(void ***)*out;
		if (hooked_vt_tex != vt) {
			hooked_vt_tex = vt;
			extern HRESULT STDMETHODCALLTYPE get_surface_level_hook(IDirect3DTexture9 *, UINT, IDirect3DSurface9 **);
			extern HRESULT STDMETHODCALLTYPE lock_rect_hook(IDirect3DTexture9 *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
			extern HRESULT STDMETHODCALLTYPE unlock_rect_hook(IDirect3DTexture9 *, UINT);
			PATCH(vt, 18, get_surface_level);
			PATCH(vt, 19, lock_rect);
			PATCH(vt, 20, unlock_rect);
		}
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE create_cube_hook(IDirect3DDevice9 *d, UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DCubeTexture9 **out, HANDLE *shared)
{
	HRESULT hr = create_cube_o(d, edge, levels, usage, fmt, pool, out, shared);
	if (SUCCEEDED(hr) && out && *out && dev11)
		pmap_put(&textures, *out, tex_make(2, edge, edge, 1, levels, usage, fmt));
	return hr;
}

// surfaces of textures (render targets) and of the back buffer
HRESULT STDMETHODCALLTYPE get_surface_level_hook(IDirect3DTexture9 *t, UINT level, IDirect3DSurface9 **out)
{
	HRESULT hr = get_surface_level_o(t, level, out);
	tex_t *mt = tex_of(t);
	if (SUCCEEDED(hr) && out && *out && mt && !pmap_get(&surfaces, *out)) {
		surf_t *s = calloc(1, sizeof(*s));
		s->tex = mt;
		s->level = level;
		pmap_put(&surfaces, *out, s);
	}
	return hr;
}

static struct {
	tex_t *t;
	UINT level;
	RECT r;
	int has_rect;
	D3DLOCKED_RECT lr;
	int active;
} tlock[8];

HRESULT STDMETHODCALLTYPE lock_rect_hook(IDirect3DTexture9 *t, UINT level, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags)
{
	HRESULT hr = lock_rect_o(t, level, lr, rect, flags);
	tex_t *mt = tex_of(t);
	if (SUCCEEDED(hr) && mt && !mt->failed && !(flags & D3DLOCK_READONLY)) {
		for (int i = 0; i < 8; i++)
			if (!tlock[i].active) {
				tlock[i].active = 1;
				tlock[i].t = mt;
				tlock[i].level = level;
				tlock[i].has_rect = rect != NULL;
				if (rect)
					tlock[i].r = *rect;
				tlock[i].lr = *lr;
				break;
			}
	}
	return hr;
}

HRESULT STDMETHODCALLTYPE unlock_rect_hook(IDirect3DTexture9 *t, UINT level)
{
	tex_t *mt = tex_of(t);
	for (int i = 0; i < 8; i++) {
		if (!tlock[i].active || tlock[i].t != mt || tlock[i].level != level)
			continue;
		tlock[i].active = 0;
		if (mt->rt || mt->depth || mt->failed || level >= mt->levels)
			break;
		if (!mt->shadow) {
			mt->shadow = calloc(mt->levels, sizeof(uint8_t *));
		}
		const UINT rb = level_row_bytes(mt, level), rows = level_rows(mt, level);
		if (!mt->shadow[level])
			mt->shadow[level] = calloc(1, (size_t)rb * rows);
		const int blk = fmt_block(mt->fmt), bpp = fmt_bpp(mt->fmt);
		UINT x0 = 0, y0 = 0, x1, y1; // in pixels or (blocks)
		UINT w, h;
		level_size(mt, level, &w, &h);
		if (blk) {
			x1 = (w + 3) / 4;
			y1 = (h + 3) / 4;
		} else {
			x1 = w;
			y1 = h;
		}
		if (tlock[i].has_rect) {
			RECT *r = &tlock[i].r;
			x0 = blk ? (UINT)r->left / 4 : (UINT)r->left;
			y0 = blk ? (UINT)r->top / 4 : (UINT)r->top;
			x1 = blk ? ((UINT)r->right + 3) / 4 : (UINT)r->right;
			y1 = blk ? ((UINT)r->bottom + 3) / 4 : (UINT)r->bottom;
		}
		const UINT unit = blk ? (UINT)blk : (UINT)bpp;
		const uint8_t *src = (const uint8_t *)tlock[i].lr.pBits;
		const UINT cols = blk ? (w + 3) / 4 : w;
		if (x1 > cols)
			x1 = cols;
		if (y1 > rows)
			y1 = rows;
		static LONG odd;
		if ((x0 >= x1 || y0 >= y1 || (UINT)tlock[i].lr.Pitch < (x1 - x0) * unit) && InterlockedIncrement(&odd) <= 10)
			log_printf("mirror11: odd texture lock: format %x %ux%u level %u rect %d (%ld,%ld,%ld,%ld) pitch %d unit %u -> %u..%u x %u..%u", (unsigned)mt->fmt, mt->w, mt->h, level, tlock[i].has_rect, tlock[i].r.left, tlock[i].r.top,
				   tlock[i].r.right, tlock[i].r.bottom, tlock[i].lr.Pitch, unit, x0, x1, y0, y1);
		for (UINT y = y0; y < y1; y++) {
			const uint8_t *from = src + (size_t)(y - y0) * tlock[i].lr.Pitch;
			if (IsBadReadPtr(from, (x1 - x0) * unit)) {
				static LONG bad;
				if (InterlockedIncrement(&bad) <= 10)
					log_printf("mirror11: locked texture memory unreadable: format %x %ux%u level %u row %u pitch %d", (unsigned)mt->fmt, mt->w, mt->h, level, y, tlock[i].lr.Pitch);
				break;
			}
			memcpy(mt->shadow[level] + (size_t)y * rb + (size_t)x0 * unit, from, (size_t)(x1 - x0) * unit);
		}
		mt->dirty = 1;
		break;
	}
	return unlock_rect_o(t, level);
}

// buffers
static HRESULT STDMETHODCALLTYPE vb_lock_hook(IDirect3DVertexBuffer9 *b, UINT off, UINT size, void **ptr, DWORD flags)
{
	HRESULT hr = vb_lock_o(b, off, size, ptr, flags);
	buf_t *mb = pmap_get(&buffers, b);
	if (SUCCEEDED(hr) && mb && ptr) {
		mb->lock_ptr = *ptr;
		mb->lock_off = off;
		mb->lock_size = size ? size : mb->length - off;
	}
	return hr;
}
static HRESULT STDMETHODCALLTYPE ib_lock_hook(IDirect3DIndexBuffer9 *b, UINT off, UINT size, void **ptr, DWORD flags)
{
	HRESULT hr = ib_lock_o(b, off, size, ptr, flags);
	buf_t *mb = pmap_get(&buffers, b);
	if (SUCCEEDED(hr) && mb && ptr) {
		mb->lock_ptr = *ptr;
		mb->lock_off = off;
		mb->lock_size = size ? size : mb->length - off;
	}
	return hr;
}
static void buf_unlocked(buf_t *mb)
{
	if (mb && mb->lock_ptr && mb->shadow && mb->lock_off + mb->lock_size <= mb->length) {
		memcpy(mb->shadow + mb->lock_off, mb->lock_ptr, mb->lock_size);
		mb->dirty = 1;
	}
	if (mb)
		mb->lock_ptr = NULL;
}
static HRESULT STDMETHODCALLTYPE vb_unlock_hook(IDirect3DVertexBuffer9 *b)
{
	buf_unlocked(pmap_get(&buffers, b));
	return vb_unlock_o(b);
}
static HRESULT STDMETHODCALLTYPE ib_unlock_hook(IDirect3DIndexBuffer9 *b)
{
	buf_unlocked(pmap_get(&buffers, b));
	return ib_unlock_o(b);
}

static buf_t *buf_make(UINT length, int is_ib, int is32)
{
	buf_t *b = calloc(1, sizeof(*b));
	b->length = length;
	b->is_ib = is_ib;
	b->is32 = is32;
	b->shadow = calloc(1, length ? length : 1);
	D3D11_BUFFER_DESC bd = {0};
	bd.ByteWidth = (length + 15) & ~15u;
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = is_ib ? D3D11_BIND_INDEX_BUFFER : D3D11_BIND_VERTEX_BUFFER;
	if (FAILED(ID3D11Device_CreateBuffer(dev11, &bd, NULL, &b->buf)))
		b->buf = NULL;
	return b;
}

static HRESULT STDMETHODCALLTYPE create_vb_hook(IDirect3DDevice9 *d, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9 **out, HANDLE *shared)
{
	HRESULT hr = create_vb_o(d, length, usage, fvf, pool, out, shared);
	if (SUCCEEDED(hr) && out && *out && dev11) {
		pmap_put(&buffers, *out, buf_make(length, 0, 0));
		void **vt = *(void ***)*out;
		if (hooked_vt_vb != vt) {
			hooked_vt_vb = vt;
			PATCH(vt, 11, vb_lock);
			PATCH(vt, 12, vb_unlock);
		}
	}
	return hr;
}
static HRESULT STDMETHODCALLTYPE create_ib_hook(IDirect3DDevice9 *d, UINT length, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9 **out, HANDLE *shared)
{
	HRESULT hr = create_ib_o(d, length, usage, fmt, pool, out, shared);
	if (SUCCEEDED(hr) && out && *out && dev11) {
		pmap_put(&buffers, *out, buf_make(length, 1, fmt == D3DFMT_INDEX32));
		void **vt = *(void ***)*out;
		if (hooked_vt_ib != vt) {
			hooked_vt_ib = vt;
			PATCH(vt, 11, ib_lock);
			PATCH(vt, 12, ib_unlock);
		}
	}
	return hr;
}

static void buf_sync(buf_t *b)
{
	if (b->dirty && b->buf) {
		ID3D11DeviceContext_UpdateSubresource(ctx11, (ID3D11Resource *)b->buf, 0, NULL, b->shadow, 0, 0);
		b->dirty = 0;
	}
}

// depth-stencil surfaces: a texture entity of their own
static HRESULT STDMETHODCALLTYPE create_depth_hook(IDirect3DDevice9 *d, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL discard, IDirect3DSurface9 **out, HANDLE *shared)
{
	HRESULT hr = create_depth_o(d, w, h, fmt, ms, q, discard, out, shared);
	if (SUCCEEDED(hr) && out && *out && dev11) {
		surf_t *s = calloc(1, sizeof(*s));
		s->tex = tex_make(1, w, h, 1, 1, D3DUSAGE_DEPTHSTENCIL, fmt);
		pmap_put(&surfaces, *out, s);
	}
	return hr;
}

// shaders and declarations
static size_t shader_dwords(const DWORD *c)
{
	size_t i = 1;
	for (;;) {
		const DWORD t = c[i];
		if ((t & 0xffff) == 0xfffe) {
			i += 1 + ((t >> 16) & 0x7fff);
			continue;
		}
		if ((t & 0xffff) == 0xffff)
			return i + 1;
		i += 1 + ((t >> 24) & 0xf);
	}
}
static shader_t *shader_make(const DWORD *code, int is_vs)
{
	shader_t *s = calloc(1, sizeof(*s));
	s->is_vs = is_vs;
	s->dwords = shader_dwords(code);
	s->code = malloc(s->dwords * 4);
	memcpy(s->code, code, s->dwords * 4);
	return s;
}
static HRESULT STDMETHODCALLTYPE create_vs_hook(IDirect3DDevice9 *d, const DWORD *code, IDirect3DVertexShader9 **out)
{
	HRESULT hr = create_vs_o(d, code, out);
	if (SUCCEEDED(hr) && out && *out && code)
		pmap_put(&shaders, *out, shader_make(code, 1));
	return hr;
}
static HRESULT STDMETHODCALLTYPE create_ps_hook(IDirect3DDevice9 *d, const DWORD *code, IDirect3DPixelShader9 **out)
{
	HRESULT hr = create_ps_o(d, code, out);
	if (SUCCEEDED(hr) && out && *out && code)
		pmap_put(&shaders, *out, shader_make(code, 0));
	return hr;
}
static HRESULT STDMETHODCALLTYPE create_decl_hook(IDirect3DDevice9 *d, const D3DVERTEXELEMENT9 *el, IDirect3DVertexDeclaration9 **out)
{
	HRESULT hr = create_decl_o(d, el, out);
	if (SUCCEEDED(hr) && out && *out && el) {
		decl_t *dl = calloc(1, sizeof(*dl));
		dl->decl = *out;
		while (dl->n < 31 && el[dl->n].Stream != 0xff) {
			dl->el[dl->n] = el[dl->n];
			dl->n++;
		}
		pmap_put(&decls, *out, dl);
	}
	return hr;
}
