// part C of mirror11.c (included there): shaders to Direct3D 11, state objects, the replay of draws, the comparison at Present
typedef struct {
	unsigned char key[40];
	void *val;
} cent_t;
typedef struct {
	cent_t *e;
	int n, cap;
} cache_t;

static void *cache_get(cache_t *c, const void *key, size_t size)
{
	for (int i = 0; i < c->n; i++)
		if (!memcmp(c->e[i].key, key, size))
			return c->e[i].val;
	return NULL;
}
static void cache_put(cache_t *c, const void *key, size_t size, void *val)
{
	if (c->n == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 64;
		c->e = realloc(c->e, (size_t)c->cap * sizeof(cent_t));
	}
	memset(c->e[c->n].key, 0, sizeof(c->e[c->n].key));
	memcpy(c->e[c->n].key, key, size);
	c->e[c->n].val = val;
	c->n++;
}

static cache_t blend_cache, depth_cache, raster_cache, sampler_cache, layout_cache;
static ID3D11Buffer *cb_vs0, *cb_vs1, *cb_ps0, *cb_ps1, *cb_ps2;

static ID3D11Buffer *make_cb(UINT size)
{
	D3D11_BUFFER_DESC bd = {0};
	bd.ByteWidth = size;
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	ID3D11Buffer *b = NULL;
	ID3D11Device_CreateBuffer(dev11, &bd, NULL, &b);
	return b;
}
static void put_cb(ID3D11Buffer *b, const void *data, size_t size)
{
	D3D11_MAPPED_SUBRESOURCE m;
	if (b && SUCCEEDED(ID3D11DeviceContext_Map(ctx11, (ID3D11Resource *)b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
		memcpy(m.pData, data, size);
		ID3D11DeviceContext_Unmap(ctx11, (ID3D11Resource *)b, 0);
	}
}

static int shader_ready(shader_t *s)
{
	if (s->tried)
		return !s->failed;
	s->tried = 1;
	char *hlsl = NULL, err[128] = "";
	int is_vs = 0;
	if (!sm3_translate_ex(s->code, s->dwords, SM3_ALPHA_TEST, &hlsl, &is_vs, err, sizeof(err))) {
		s->failed = 1;
		if (InterlockedIncrement((volatile LONG *)&stat.shader_fail) <= 10)
			log_printf("mirror11: shader not translated: %s", err);
		return 0;
	}
	ID3DBlob *code = NULL, *msgs = NULL;
	HRESULT hr = p_compile(hlsl, strlen(hlsl), "sm3", NULL, NULL, "main", is_vs ? "vs_5_0" : "ps_5_0", (1 << 11) | (1 << 14), 0, &code, &msgs);
	free(hlsl);
	if (FAILED(hr) || !code) {
		s->failed = 1;
		if (InterlockedIncrement((volatile LONG *)&stat.shader_fail) <= 10)
			log_printf("mirror11: shader not compiled: hr %lx %s", (unsigned long)hr, msgs ? (const char *)ID3D10Blob_GetBufferPointer(msgs) : "");
		return 0;
	}
	if (is_vs)
		hr = ID3D11Device_CreateVertexShader(dev11, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, (ID3D11VertexShader **)&s->obj11);
	else
		hr = ID3D11Device_CreatePixelShader(dev11, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, (ID3D11PixelShader **)&s->obj11);
	if (FAILED(hr)) {
		s->failed = 1;
		return 0;
	}
	s->blob = code;
	return 1;
}

static const char *const SEMANTICS[14] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"};

static DXGI_FORMAT decl_format(int type)
{
	switch (type) {
	case D3DDECLTYPE_FLOAT1: return DXGI_FORMAT_R32_FLOAT;
	case D3DDECLTYPE_FLOAT2: return DXGI_FORMAT_R32G32_FLOAT;
	case D3DDECLTYPE_FLOAT3: return DXGI_FORMAT_R32G32B32_FLOAT;
	case D3DDECLTYPE_FLOAT4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
	case D3DDECLTYPE_D3DCOLOR: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case D3DDECLTYPE_UBYTE4: return DXGI_FORMAT_R8G8B8A8_UINT;
	case D3DDECLTYPE_SHORT2: return DXGI_FORMAT_R16G16_SINT;
	case D3DDECLTYPE_SHORT4: return DXGI_FORMAT_R16G16B16A16_SINT;
	case D3DDECLTYPE_UBYTE4N: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case D3DDECLTYPE_SHORT2N: return DXGI_FORMAT_R16G16_SNORM;
	case D3DDECLTYPE_SHORT4N: return DXGI_FORMAT_R16G16B16A16_SNORM;
	case D3DDECLTYPE_USHORT2N: return DXGI_FORMAT_R16G16_UNORM;
	case D3DDECLTYPE_USHORT4N: return DXGI_FORMAT_R16G16B16A16_UNORM;
	case D3DDECLTYPE_FLOAT16_2: return DXGI_FORMAT_R16G16_FLOAT;
	case D3DDECLTYPE_FLOAT16_4: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	}
	return DXGI_FORMAT_UNKNOWN;
}

static ID3D11InputLayout *layout_for(decl_t *dl, shader_t *vs)
{
	unsigned char key[40] = {0};
	memcpy(key, &dl, 8);
	memcpy(key + 8, &vs, 8);
	ID3D11InputLayout *l = cache_get(&layout_cache, key, 16);
	if (l)
		return l == (ID3D11InputLayout *)1 ? NULL : l;
	D3D11_INPUT_ELEMENT_DESC ed[32];
	int n = 0;
	for (int i = 0; i < dl->n; i++) {
		const D3DVERTEXELEMENT9 *e = &dl->el[i];
		const DXGI_FORMAT f = decl_format(e->Type);
		if (f == DXGI_FORMAT_UNKNOWN || e->Usage >= 14)
			continue;
		ed[n].SemanticName = SEMANTICS[e->Usage];
		ed[n].SemanticIndex = e->UsageIndex;
		ed[n].Format = f;
		ed[n].InputSlot = e->Stream;
		ed[n].AlignedByteOffset = e->Offset;
		ed[n].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
		ed[n].InstanceDataStepRate = 0;
		n++;
	}
	HRESULT hr = ID3D11Device_CreateInputLayout(dev11, ed, n, ID3D10Blob_GetBufferPointer(vs->blob), ID3D10Blob_GetBufferSize(vs->blob), &l);
	if (FAILED(hr)) {
		if (InterlockedIncrement((volatile LONG *)&stat.layout_fail) <= 10)
			log_printf("mirror11: input layout failed: hr %lx (%d elements)", (unsigned long)hr, n);
		cache_put(&layout_cache, key, 16, (void *)1);
		return NULL;
	}
	cache_put(&layout_cache, key, 16, l);
	return l;
}

static DWORD rsv(int id, DWORD def)
{
	return st.rs[id] ? st.rs[id] : def;
}

static D3D11_BLEND alpha_factor(DWORD b)
{
	switch (b) {
	case 3: return D3D11_BLEND_SRC_ALPHA;
	case 4: return D3D11_BLEND_INV_SRC_ALPHA;
	case 9: return D3D11_BLEND_DEST_ALPHA;
	case 10: return D3D11_BLEND_INV_DEST_ALPHA;
	}
	return (D3D11_BLEND)b;
}

static ID3D11BlendState *blend_state(void)
{
	DWORD k[8] = {st.rs[27], rsv(19, 2), rsv(20, 1), rsv(171, 1), rsv(207, 2), rsv(208, 1), rsv(209, 1), st.rs[168] ? st.rs[168] & 15 : 15};
	if (st.rs[207] == 0 && st.rs[208] == 0) { // no separate alpha in this state
		k[4] = k[1];
		k[5] = k[2];
		k[6] = k[3];
	}
	ID3D11BlendState *b = cache_get(&blend_cache, k, sizeof(k));
	if (b)
		return b;
	D3D11_BLEND_DESC bd = {0};
	bd.RenderTarget[0].BlendEnable = k[0] != 0;
	bd.RenderTarget[0].SrcBlend = (D3D11_BLEND)k[1];
	bd.RenderTarget[0].DestBlend = (D3D11_BLEND)k[2];
	bd.RenderTarget[0].BlendOp = (D3D11_BLEND_OP)k[3];
	bd.RenderTarget[0].SrcBlendAlpha = alpha_factor(k[4]);
	bd.RenderTarget[0].DestBlendAlpha = alpha_factor(k[5]);
	bd.RenderTarget[0].BlendOpAlpha = (D3D11_BLEND_OP)k[6];
	bd.RenderTarget[0].RenderTargetWriteMask = (UINT8)k[7];
	if (FAILED(ID3D11Device_CreateBlendState(dev11, &bd, &b)))
		b = NULL;
	cache_put(&blend_cache, k, sizeof(k), b);
	return b;
}

static ID3D11DepthStencilState *depth_state(void)
{
	DWORD k[12] = {rsv(7, 1) != 0, rsv(14, 1) != 0, rsv(23, 4), st.rs[52] != 0, rsv(55, 1) | rsv(53, 1) << 4 | rsv(54, 1) << 8 | rsv(56, 8) << 12, rsv(188, 1) | rsv(186, 1) << 4 | rsv(187, 1) << 8 | rsv(189, 8) << 12,
		       (st.rs[58] ? st.rs[58] : 0xff) & 0xff, (st.rs[59] ? st.rs[59] : 0xff) & 0xff};
	ID3D11DepthStencilState *s = cache_get(&depth_cache, k, sizeof(k));
	if (s)
		return s;
	D3D11_DEPTH_STENCIL_DESC dd = {0};
	dd.DepthEnable = k[0];
	dd.DepthWriteMask = k[1] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
	dd.DepthFunc = (D3D11_COMPARISON_FUNC)k[2];
	dd.StencilEnable = k[3];
	dd.StencilReadMask = (UINT8)k[6];
	dd.StencilWriteMask = (UINT8)k[7];
	dd.FrontFace.StencilPassOp = (D3D11_STENCIL_OP)(k[4] & 15);
	dd.FrontFace.StencilFailOp = (D3D11_STENCIL_OP)((k[4] >> 4) & 15);
	dd.FrontFace.StencilDepthFailOp = (D3D11_STENCIL_OP)((k[4] >> 8) & 15);
	dd.FrontFace.StencilFunc = (D3D11_COMPARISON_FUNC)((k[4] >> 12) & 15);
	dd.BackFace.StencilPassOp = (D3D11_STENCIL_OP)(k[5] & 15);
	dd.BackFace.StencilFailOp = (D3D11_STENCIL_OP)((k[5] >> 4) & 15);
	dd.BackFace.StencilDepthFailOp = (D3D11_STENCIL_OP)((k[5] >> 8) & 15);
	dd.BackFace.StencilFunc = (D3D11_COMPARISON_FUNC)((k[5] >> 12) & 15);
	if (FAILED(ID3D11Device_CreateDepthStencilState(dev11, &dd, &s)))
		s = NULL;
	cache_put(&depth_cache, k, sizeof(k), s);
	return s;
}

static ID3D11RasterizerState *raster_state(void)
{
	DWORD k[6] = {rsv(22, 3), rsv(8, 3), st.rs[174] != 0, st.rs[195], st.rs[175], 0};
	ID3D11RasterizerState *r = cache_get(&raster_cache, k, sizeof(k));
	if (r)
		return r;
	D3D11_RASTERIZER_DESC rd = {0};
	rd.FillMode = k[1] == 2 ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
	rd.CullMode = k[0] == 1 ? D3D11_CULL_NONE : k[0] == 2 ? D3D11_CULL_FRONT : D3D11_CULL_BACK;
	rd.DepthClipEnable = TRUE;
	rd.ScissorEnable = k[2];
	float bias, slope;
	memcpy(&bias, &k[3], 4);
	memcpy(&slope, &k[4], 4);
	rd.DepthBias = (INT)(bias * 16777215.0f);
	rd.SlopeScaledDepthBias = slope;
	if (FAILED(ID3D11Device_CreateRasterizerState(dev11, &rd, &r)))
		r = NULL;
	cache_put(&raster_cache, k, sizeof(k), r);
	return r;
}

static ID3D11SamplerState *sampler_state(int s)
{
	DWORD k[8] = {rsv(0, 0), 0, 0, 0, 0, 0, 0, 0};
	const DWORD *p = st.smp[s];
	const DWORD min = p[6] ? p[6] : 2, mag = p[5] ? p[5] : 2, mip = p[7];
	k[0] = min;
	k[1] = mag;
	k[2] = mip;
	k[3] = p[1] ? p[1] : 1;
	k[4] = p[2] ? p[2] : 1;
	k[5] = p[3] ? p[3] : 1;
	k[6] = p[10] ? p[10] : 1;
	ID3D11SamplerState *ss = cache_get(&sampler_cache, k, sizeof(k));
	if (ss)
		return ss;
	D3D11_SAMPLER_DESC sd = {0};
	if (min == 3 || mag == 3 || mip == 3)
		sd.Filter = D3D11_FILTER_ANISOTROPIC;
	else
		sd.Filter = (D3D11_FILTER)((min == 2 ? 0x10 : 0) | (mag == 2 ? 4 : 0) | (mip == 2 ? 1 : 0));
	sd.AddressU = (D3D11_TEXTURE_ADDRESS_MODE)k[3];
	sd.AddressV = (D3D11_TEXTURE_ADDRESS_MODE)k[4];
	sd.AddressW = (D3D11_TEXTURE_ADDRESS_MODE)k[5];
	sd.MaxAnisotropy = k[6] > 16 ? 16 : k[6];
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sd.MinLOD = 0;
	sd.MaxLOD = mip == 0 ? 0 : D3D11_FLOAT32_MAX;
	if (FAILED(ID3D11Device_CreateSamplerState(dev11, &sd, &ss)))
		ss = NULL;
	cache_put(&sampler_cache, k, sizeof(k), ss);
	return ss;
}

// ---- targets
static tex_t *surface_tex(IDirect3DSurface9 *s)
{
	if (!s)
		return NULL;
	if (s == bb_surface)
		return &main_target;
	surf_t *e = pmap_get(&surfaces, s);
	return e ? e->tex : NULL;
}

static void bind_targets(ID3D11RenderTargetView **rtv_out, ID3D11DepthStencilView **dsv_out)
{
	tex_t *rt = surface_tex(st.rt0), *ds = surface_tex(st.ds);
	const int srgb = st.rs[194] != 0;
	*rtv_out = rt ? (srgb && rt->rtv_srgb ? rt->rtv_srgb : rt->rtv) : NULL;
	*dsv_out = ds ? ds->dsv : NULL;
}

// ---- draw
static int topology(D3DPRIMITIVETYPE p, UINT count, UINT *vertices, D3D11_PRIMITIVE_TOPOLOGY *t)
{
	switch (p) {
	case D3DPT_POINTLIST: *t = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; *vertices = count; return 1;
	case D3DPT_LINELIST: *t = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; *vertices = count * 2; return 1;
	case D3DPT_LINESTRIP: *t = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; *vertices = count + 1; return 1;
	case D3DPT_TRIANGLELIST: *t = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; *vertices = count * 3; return 1;
	case D3DPT_TRIANGLESTRIP: *t = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; *vertices = count + 2; return 1;
	default: return 0;
	}
}

static ID3D11Buffer *up_vb, *up_ib;
static UINT up_vb_size, up_ib_size;

// binds everything but the geometry; returns 0 when the draw cannot be replayed
static int setup_draw(D3DPRIMITIVETYPE prim, UINT count, UINT *vertices, int *has_ps)
{
	D3D11_PRIMITIVE_TOPOLOGY topo;
	if (!topology(prim, count, vertices, &topo)) {
		stat.skip_prim++;
		return 0;
	}
	shader_t *vs = st.vs ? pmap_get(&shaders, st.vs) : NULL, *ps = st.ps ? pmap_get(&shaders, st.ps) : NULL;
	if (!vs) {
		stat.skip_no_vs++;
		return 0;
	}
	if (!shader_ready(vs) || (ps && !shader_ready(ps))) {
		stat.skip_shader++;
		return 0;
	}
	decl_t *dl = st.decl ? pmap_get(&decls, st.decl) : NULL;
	ID3D11InputLayout *layout = dl ? layout_for(dl, vs) : NULL;
	if (!layout) {
		stat.skip_layout++;
		return 0;
	}
	ID3D11RenderTargetView *rtv;
	ID3D11DepthStencilView *dsv;
	bind_targets(&rtv, &dsv);
	if (!rtv) {
		stat.skip_target++;
		return 0;
	}
	// textures first: one that is also the target must not be bound for reading
	ID3D11ShaderResourceView *srv[16] = {0};
	ID3D11SamplerState *smp[16] = {0};
	for (int i = 0; i < 16; i++) {
		tex_t *t = tex_of(st.tex[i]);
		if (!t || t->failed || !t->tex)
			continue;
		tex_sync(t);
		if (t->tex == main_target.tex || (surface_tex(st.rt0) && surface_tex(st.rt0)->tex == t->tex))
			continue;
		srv[i] = (st.smp[i][11] && t->srv_srgb) ? t->srv_srgb : t->srv;
		smp[i] = sampler_state(i);
	}
	ID3D11DeviceContext_OMSetRenderTargets(ctx11, 1, &rtv, dsv);
	D3D11_VIEWPORT vp = {(float)st.vp.X, (float)st.vp.Y, (float)st.vp.Width, (float)st.vp.Height, st.vp.MinZ, st.vp.MaxZ};
	ID3D11DeviceContext_RSSetViewports(ctx11, 1, &vp);
	if (st.rs[174]) {
		D3D11_RECT r = {st.scissor.left, st.scissor.top, st.scissor.right, st.scissor.bottom};
		ID3D11DeviceContext_RSSetScissorRects(ctx11, 1, &r);
	}
	ID3D11DeviceContext_RSSetState(ctx11, raster_state());
	ID3D11DeviceContext_OMSetBlendState(ctx11, blend_state(), NULL, 0xffffffffu);
	ID3D11DeviceContext_OMSetDepthStencilState(ctx11, depth_state(), st.rs[57]);
	ID3D11DeviceContext_IASetInputLayout(ctx11, layout);
	ID3D11DeviceContext_IASetPrimitiveTopology(ctx11, topo);
	ID3D11DeviceContext_VSSetShader(ctx11, (ID3D11VertexShader *)vs->obj11, NULL, 0);
	ID3D11DeviceContext_PSSetShader(ctx11, ps ? (ID3D11PixelShader *)ps->obj11 : NULL, NULL, 0);
	*has_ps = ps != NULL;
	// constants
	struct { int ic[16][4]; unsigned bc[8][4]; } ib;
	memset(&ib, 0, sizeof(ib));
	memcpy(ib.ic, st.vs_i, sizeof(ib.ic));
	for (int i = 0; i < 16; i++)
		ib.bc[i >> 2][i & 3] = st.vs_b[i] ? 1 : 0;
	put_cb(cb_vs0, st.vs_f, sizeof(st.vs_f));
	put_cb(cb_vs1, &ib, sizeof(ib));
	memcpy(ib.ic, st.ps_i, sizeof(ib.ic));
	for (int i = 0; i < 16; i++)
		ib.bc[i >> 2][i & 3] = st.ps_b[i] ? 1 : 0;
	put_cb(cb_ps0, st.ps_f, sizeof(st.ps_f));
	put_cb(cb_ps1, &ib, sizeof(ib));
	float at[4] = {(float)rsv(25, 8), (float)(st.rs[24] & 255), st.rs[15] ? 1.0f : 0.0f, 0};
	put_cb(cb_ps2, at, sizeof(at));
	ID3D11Buffer *vcb[2] = {cb_vs0, cb_vs1}, *pcb[3] = {cb_ps0, cb_ps1, cb_ps2};
	ID3D11DeviceContext_VSSetConstantBuffers(ctx11, 0, 2, vcb);
	ID3D11DeviceContext_PSSetConstantBuffers(ctx11, 0, 3, pcb);
	ID3D11DeviceContext_PSSetShaderResources(ctx11, 0, 16, srv);
	ID3D11DeviceContext_PSSetSamplers(ctx11, 0, 16, smp);
	return 1;
}

static void bind_streams(void)
{
	for (int i = 0; i < 4; i++) {
		buf_t *b = st.stream[i].vb ? pmap_get(&buffers, st.stream[i].vb) : NULL;
		ID3D11Buffer *buf = NULL;
		if (b) {
			buf_sync(b);
			buf = b->buf;
		} else if (st.stream[i].vb) {
			stat.buf_unknown++;
		}
		UINT stride = st.stream[i].stride, off = st.stream[i].off;
		ID3D11DeviceContext_IASetVertexBuffers(ctx11, i, 1, &buf, &stride, &off);
	}
}

static HRESULT STDMETHODCALLTYPE draw_indexed_hook(IDirect3DDevice9 *d, D3DPRIMITIVETYPE prim, INT base, UINT minv, UINT numv, UINT start, UINT count)
{
	HRESULT hr = draw_indexed_o(d, prim, base, minv, numv, start, count);
	stat.draws++;
	UINT n;
	int has_ps;
	buf_t *ib = st.ib ? pmap_get(&buffers, st.ib) : NULL;
	if (!ib || !ib->buf) {
		stat.draws_skipped++;
		return hr;
	}
	if (!setup_draw(prim, count, &n, &has_ps)) {
		stat.draws_skipped++;
		return hr;
	}
	bind_streams();
	buf_sync(ib);
	ID3D11DeviceContext_IASetIndexBuffer(ctx11, ib->buf, ib->is32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0);
	ID3D11DeviceContext_DrawIndexed(ctx11, n, start, base);
	return hr;
}

static HRESULT STDMETHODCALLTYPE draw_prim_hook(IDirect3DDevice9 *d, D3DPRIMITIVETYPE prim, UINT start, UINT count)
{
	HRESULT hr = draw_prim_o(d, prim, start, count);
	stat.draws++;
	UINT n;
	int has_ps;
	if (!setup_draw(prim, count, &n, &has_ps)) {
		stat.draws_skipped++;
		return hr;
	}
	bind_streams();
	ID3D11DeviceContext_Draw(ctx11, n, start);
	return hr;
}

static ID3D11Buffer *up_buffer(ID3D11Buffer **b, UINT *size, UINT need, UINT bind, const void *data)
{
	if (!*b || *size < need) {
		if (*b)
			ID3D11Buffer_Release(*b);
		D3D11_BUFFER_DESC bd = {0};
		*size = need < 65536 ? 65536 : need * 2;
		bd.ByteWidth = *size;
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = bind;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		ID3D11Device_CreateBuffer(dev11, &bd, NULL, b);
	}
	D3D11_MAPPED_SUBRESOURCE m;
	if (*b && SUCCEEDED(ID3D11DeviceContext_Map(ctx11, (ID3D11Resource *)*b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
		memcpy(m.pData, data, need);
		ID3D11DeviceContext_Unmap(ctx11, (ID3D11Resource *)*b, 0);
	}
	return *b;
}

static HRESULT STDMETHODCALLTYPE draw_prim_up_hook(IDirect3DDevice9 *d, D3DPRIMITIVETYPE prim, UINT count, const void *data, UINT stride)
{
	HRESULT hr = draw_prim_up_o(d, prim, count, data, stride);
	stat.draws++;
	UINT n;
	int has_ps;
	if (!setup_draw(prim, count, &n, &has_ps)) {
		stat.draws_skipped++;
		return hr;
	}
	ID3D11Buffer *vb = up_buffer(&up_vb, &up_vb_size, n * stride, D3D11_BIND_VERTEX_BUFFER, data);
	UINT off = 0;
	ID3D11DeviceContext_IASetVertexBuffers(ctx11, 0, 1, &vb, &stride, &off);
	ID3D11DeviceContext_Draw(ctx11, n, 0);
	return hr;
}

static HRESULT STDMETHODCALLTYPE draw_indexed_up_hook(IDirect3DDevice9 *d, D3DPRIMITIVETYPE prim, UINT minv, UINT numv, UINT count, const void *idx, D3DFORMAT ifmt, const void *data, UINT stride)
{
	HRESULT hr = draw_indexed_up_o(d, prim, minv, numv, count, idx, ifmt, data, stride);
	stat.draws++;
	UINT n;
	int has_ps;
	if (!setup_draw(prim, count, &n, &has_ps)) {
		stat.draws_skipped++;
		return hr;
	}
	const UINT isz = ifmt == D3DFMT_INDEX32 ? 4 : 2;
	ID3D11Buffer *vb = up_buffer(&up_vb, &up_vb_size, (minv + numv) * stride, D3D11_BIND_VERTEX_BUFFER, data);
	ID3D11Buffer *ib = up_buffer(&up_ib, &up_ib_size, n * isz, D3D11_BIND_INDEX_BUFFER, idx);
	UINT off = 0;
	ID3D11DeviceContext_IASetVertexBuffers(ctx11, 0, 1, &vb, &stride, &off);
	ID3D11DeviceContext_IASetIndexBuffer(ctx11, ib, isz == 4 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0);
	ID3D11DeviceContext_DrawIndexed(ctx11, n, 0, 0);
	return hr;
}

static HRESULT STDMETHODCALLTYPE clear_hook(IDirect3DDevice9 *d, DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	HRESULT hr = clear_o(d, count, rects, flags, color, z, stencil);
	stat.clears++;
	ID3D11RenderTargetView *rtv;
	ID3D11DepthStencilView *dsv;
	bind_targets(&rtv, &dsv);
	if (count) // partial clears: not replayed (counted as clears; the compare will show it)
		stat.skip_target += 0;
	if ((flags & D3DCLEAR_TARGET) && rtv && !count) {
		const float c[4] = {((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f, ((color >> 24) & 255) / 255.0f};
		ID3D11DeviceContext_ClearRenderTargetView(ctx11, rtv, c);
	}
	if ((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && dsv && !count)
		ID3D11DeviceContext_ClearDepthStencilView(ctx11, dsv, ((flags & D3DCLEAR_ZBUFFER) ? D3D11_CLEAR_DEPTH : 0) | ((flags & D3DCLEAR_STENCIL) ? D3D11_CLEAR_STENCIL : 0), z, (UINT8)stencil);
	return hr;
}

static HRESULT STDMETHODCALLTYPE stretch_rect_hook(IDirect3DDevice9 *d, IDirect3DSurface9 *src, const RECT *sr, IDirect3DSurface9 *dst, const RECT *dr, D3DTEXTUREFILTERTYPE f)
{
	HRESULT hr = stretch_rect_o(d, src, sr, dst, dr, f);
	tex_t *a = surface_tex(src), *b = surface_tex(dst);
	static LONG logged;
	if (a && b && a->tex && b->tex && !sr && !dr && a->w == b->w && a->h == b->h && a->dxgi == b->dxgi)
		ID3D11DeviceContext_CopySubresourceRegion(ctx11, (ID3D11Resource *)b->tex, 0, 0, 0, 0, (ID3D11Resource *)a->tex, 0, NULL);
	else if (InterlockedIncrement(&logged) <= 10)
		log_printf("mirror11: StretchRect not replayed (src %p %s %ux%u dxgi %d, dst %p %s %ux%u dxgi %d, rects %d/%d)", (void *)src, a ? "known" : "unknown", a ? a->w : 0, a ? a->h : 0, a ? (int)a->dxgi : 0, (void *)dst, b ? "known" : "unknown",
			   b ? b->w : 0, b ? b->h : 0, b ? (int)b->dxgi : 0, sr != NULL, dr != NULL);
	return hr;
}

// ---- state shadows
static HRESULT STDMETHODCALLTYPE set_rt_hook(IDirect3DDevice9 *d, DWORD i, IDirect3DSurface9 *s)
{
	if (i == 0)
		st.rt0 = s;
	return set_rt_o(d, i, s);
}
static HRESULT STDMETHODCALLTYPE set_ds_hook(IDirect3DDevice9 *d, IDirect3DSurface9 *s)
{
	st.ds = s;
	return set_ds_o(d, s);
}
static HRESULT STDMETHODCALLTYPE set_viewport_hook(IDirect3DDevice9 *d, const D3DVIEWPORT9 *v)
{
	if (v)
		st.vp = *v;
	return set_viewport_o(d, v);
}
static HRESULT STDMETHODCALLTYPE set_rs_hook(IDirect3DDevice9 *d, D3DRENDERSTATETYPE s, DWORD v)
{
	if (s < 256)
		st.rs[s] = v;
	return set_rs_o(d, s, v);
}
static HRESULT STDMETHODCALLTYPE set_texture_hook(IDirect3DDevice9 *d, DWORD s, IDirect3DBaseTexture9 *t)
{
	if (s < 16)
		st.tex[s] = t;
	return set_texture_o(d, s, t);
}
static HRESULT STDMETHODCALLTYPE set_ss_hook(IDirect3DDevice9 *d, DWORD s, D3DSAMPLERSTATETYPE t, DWORD v)
{
	if (s < 16 && t < 16)
		st.smp[s][t] = v;
	return set_ss_o(d, s, t, v);
}
static HRESULT STDMETHODCALLTYPE set_scissor_hook(IDirect3DDevice9 *d, const RECT *r)
{
	if (r)
		st.scissor = *r;
	return set_scissor_o(d, r);
}
static HRESULT STDMETHODCALLTYPE set_decl_hook(IDirect3DDevice9 *d, IDirect3DVertexDeclaration9 *v)
{
	st.decl = v;
	return set_decl_o(d, v);
}
static HRESULT STDMETHODCALLTYPE set_vs_hook(IDirect3DDevice9 *d, IDirect3DVertexShader9 *v)
{
	st.vs = v;
	return set_vs_o(d, v);
}
static HRESULT STDMETHODCALLTYPE set_ps_hook(IDirect3DDevice9 *d, IDirect3DPixelShader9 *v)
{
	st.ps = v;
	return set_ps_o(d, v);
}
static HRESULT STDMETHODCALLTYPE set_vs_f_hook(IDirect3DDevice9 *d, UINT r, const float *v, UINT n)
{
	if (v && r + n <= 256)
		memcpy(st.vs_f[r], v, (size_t)n * 16);
	return set_vs_f_o(d, r, v, n);
}
static HRESULT STDMETHODCALLTYPE set_ps_f_hook(IDirect3DDevice9 *d, UINT r, const float *v, UINT n)
{
	if (v && r + n <= 256)
		memcpy(st.ps_f[r], v, (size_t)n * 16);
	return set_ps_f_o(d, r, v, n);
}
static HRESULT STDMETHODCALLTYPE set_vs_i_hook(IDirect3DDevice9 *d, UINT r, const int *v, UINT n)
{
	if (v && r + n <= 16)
		memcpy(st.vs_i[r], v, (size_t)n * 16);
	return set_vs_i_o(d, r, v, n);
}
static HRESULT STDMETHODCALLTYPE set_ps_i_hook(IDirect3DDevice9 *d, UINT r, const int *v, UINT n)
{
	if (v && r + n <= 16)
		memcpy(st.ps_i[r], v, (size_t)n * 16);
	return set_ps_i_o(d, r, v, n);
}
static HRESULT STDMETHODCALLTYPE set_vs_b_hook(IDirect3DDevice9 *d, UINT r, const BOOL *v, UINT n)
{
	if (v && r + n <= 16)
		memcpy(&st.vs_b[r], v, (size_t)n * 4);
	return set_vs_b_o(d, r, v, n);
}
static HRESULT STDMETHODCALLTYPE set_ps_b_hook(IDirect3DDevice9 *d, UINT r, const BOOL *v, UINT n)
{
	if (v && r + n <= 16)
		memcpy(&st.ps_b[r], v, (size_t)n * 4);
	return set_ps_b_o(d, r, v, n);
}
static HRESULT STDMETHODCALLTYPE set_stream_hook(IDirect3DDevice9 *d, UINT s, IDirect3DVertexBuffer9 *b, UINT off, UINT stride)
{
	if (s < 4) {
		st.stream[s].vb = b;
		st.stream[s].off = off;
		st.stream[s].stride = stride;
	}
	return set_stream_o(d, s, b, off, stride);
}
static HRESULT STDMETHODCALLTYPE set_indices_hook(IDirect3DDevice9 *d, IDirect3DIndexBuffer9 *b)
{
	st.ib = b;
	return set_indices_o(d, b);
}
static HRESULT STDMETHODCALLTYPE get_backbuffer_hook(IDirect3DDevice9 *d, UINT chain, UINT i, D3DBACKBUFFER_TYPE t, IDirect3DSurface9 **out)
{
	return get_backbuffer_o(d, chain, i, t, out);
}

// ---- the comparison at Present
static void save_bmp(const char *path, const uint8_t *bgra, UINT w, UINT h, UINT stride)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	const UINT row = (w * 3 + 3) & ~3u;
	uint8_t hdr[54] = {'B', 'M'};
	const UINT size = 54 + row * h;
	memcpy(hdr + 2, &size, 4);
	const UINT off = 54, dib = 40;
	memcpy(hdr + 10, &off, 4);
	memcpy(hdr + 14, &dib, 4);
	memcpy(hdr + 18, &w, 4);
	memcpy(hdr + 22, &h, 4);
	hdr[26] = 1;
	hdr[28] = 24;
	fwrite(hdr, 1, 54, f);
	uint8_t *line = calloc(1, row);
	for (UINT y = h; y-- > 0;) {
		for (UINT x = 0; x < w; x++) {
			line[x * 3] = bgra[(size_t)y * stride + x * 4];
			line[x * 3 + 1] = bgra[(size_t)y * stride + x * 4 + 1];
			line[x * 3 + 2] = bgra[(size_t)y * stride + x * 4 + 2];
		}
		fwrite(line, 1, row, f);
	}
	free(line);
	fclose(f);
}

static IDirect3DSurface9 *cmp_rt, *cmp_sys;
static ID3D11Texture2D *staging;

static void compare_frame(IDirect3DDevice9 *d)
{
	if (!bb_surface || !main_target.tex)
		return;
	HRESULT hr0;
	if (!cmp_rt && FAILED(hr0 = IDirect3DDevice9_CreateRenderTarget(d, bb_w, bb_h, bb_fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &cmp_rt, NULL))) {
		log_printf("mirror11: compare: CreateRenderTarget failed %lx", (unsigned long)hr0);
		return;
	}
	if (!cmp_sys && FAILED(hr0 = IDirect3DDevice9_CreateOffscreenPlainSurface(d, bb_w, bb_h, bb_fmt, D3DPOOL_SYSTEMMEM, &cmp_sys, NULL))) {
		log_printf("mirror11: compare: CreateOffscreenPlainSurface failed %lx", (unsigned long)hr0);
		return;
	}
	if (!staging) {
		D3D11_TEXTURE2D_DESC td = {0};
		td.Width = bb_w;
		td.Height = bb_h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_STAGING;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(ID3D11Device_CreateTexture2D(dev11, &td, NULL, &staging)))
			return;
	}
	if (FAILED(hr0 = stretch_rect_o(d, bb_surface, NULL, cmp_rt, NULL, D3DTEXF_NONE))) {
		log_printf("mirror11: compare: StretchRect of the back buffer failed %lx", (unsigned long)hr0);
		return;
	}
	if (FAILED(hr0 = IDirect3DDevice9_GetRenderTargetData(d, cmp_rt, cmp_sys))) {
		log_printf("mirror11: compare: GetRenderTargetData failed %lx", (unsigned long)hr0);
		return;
	}
	D3DLOCKED_RECT lr;
	if (FAILED(hr0 = IDirect3DSurface9_LockRect(cmp_sys, &lr, NULL, D3DLOCK_READONLY))) {
		log_printf("mirror11: compare: LockRect failed %lx", (unsigned long)hr0);
		return;
	}
	ID3D11DeviceContext_CopyResource(ctx11, (ID3D11Resource *)staging, (ID3D11Resource *)main_target.tex);
	D3D11_MAPPED_SUBRESOURCE m;
	if (SUCCEEDED(ID3D11DeviceContext_Map(ctx11, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) {
		double sum = 0;
		size_t over = 0, total = (size_t)bb_w * bb_h;
		for (UINT y = 0; y < bb_h; y++) {
			const uint8_t *a = (const uint8_t *)lr.pBits + (size_t)y * lr.Pitch, *b = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
			for (UINT x = 0; x < bb_w; x++) {
				int worst = 0;
				for (int c = 0; c < 3; c++) {
					int df = a[x * 4 + c] - b[x * 4 + c];
					if (df < 0)
						df = -df;
					sum += df;
					if (df > worst)
						worst = df;
				}
				if (worst > 24)
					over++;
			}
		}
		log_printf("mirror11: frame %d: mean difference %.2f / 255 per channel, %.2f%% of pixels differ by more than 24 | draws %ld replayed %ld, clears %ld", frames, sum / (3.0 * (double)total), 100.0 * (double)over / (double)total,
			   stat.draws, stat.draws - stat.draws_skipped, stat.clears);
		log_printf("mirror11:   skipped: no vs %ld, no layout %ld, shader %ld, prim %ld, target %ld; failures: shaders %ld layouts %ld, textures unsupported %ld, unknown buffers %ld", stat.skip_no_vs, stat.skip_layout, stat.skip_shader, stat.skip_prim,
			   stat.skip_target, stat.shader_fail, stat.layout_fail, stat.tex_unsupported, stat.buf_unknown);
		if (frames == 120 || frames == 600 || frames == 1800) {
			char p[MAX_PATH];
			snprintf(p, sizeof(p), "%sd3d9-%d.bmp", out_dir, frames);
			save_bmp(p, lr.pBits, bb_w, bb_h, (UINT)lr.Pitch);
			snprintf(p, sizeof(p), "%sd3d11-%d.bmp", out_dir, frames);
			save_bmp(p, m.pData, bb_w, bb_h, m.RowPitch);
		}
		ID3D11DeviceContext_Unmap(ctx11, (ID3D11Resource *)staging, 0);
	}
	IDirect3DSurface9_UnlockRect(cmp_sys);
}

// ---- display: the Direct3D 11 picture is what the player sees (MW2COOP_D3D11=1). The window gets a DXGI flip-model swap chain; the Direct3D 9 device keeps running under it (the game still calls it) and
// presents to a hidden window.
static int display_mode;
static IDXGISwapChain1 *swapchain;
static ID3D11Texture2D *sc_buffer;
static HWND hidden_window;

static int display_start(void)
{
	IDirect3DSwapChain9 *sc9 = NULL;
	D3DPRESENT_PARAMETERS pp;
	if (FAILED(IDirect3DDevice9_GetSwapChain(dev9, 0, &sc9)) || FAILED(IDirect3DSwapChain9_GetPresentParameters(sc9, &pp))) {
		log_printf("mirror11: display: no present parameters");
		return 0;
	}
	IDirect3DSwapChain9_Release(sc9);
	D3DDEVICE_CREATION_PARAMETERS cp;
	IDirect3DDevice9_GetCreationParameters(dev9, &cp);
	HWND hwnd = pp.hDeviceWindow ? pp.hDeviceWindow : cp.hFocusWindow;
	if (!pp.Windowed) {
		log_printf("mirror11: display: the game is in exclusive full screen; only windowed and borderless windows are done");
		return 0;
	}
	IDXGIDevice *dxd = NULL;
	IDXGIAdapter *ad = NULL;
	IDXGIFactory2 *fac = NULL;
	if (FAILED(ID3D11Device_QueryInterface(dev11, &IID_IDXGIDevice, (void **)&dxd)) || FAILED(IDXGIDevice_GetAdapter(dxd, &ad)) || FAILED(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&fac))) {
		log_printf("mirror11: display: no DXGI factory");
		return 0;
	}
	DXGI_SWAP_CHAIN_DESC1 sd;
	memset(&sd, 0, sizeof(sd));
	sd.Width = bb_w;
	sd.Height = bb_h;
	sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = 2;
	sd.Scaling = DXGI_SCALING_NONE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	HRESULT hr = IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown *)dev11, hwnd, &sd, NULL, NULL, &swapchain);
	if (FAILED(hr)) {
		log_printf("mirror11: display: CreateSwapChainForHwnd failed: %lx (window %p)", (unsigned long)hr, (void *)hwnd);
		return 0;
	}
	IDXGIFactory2_MakeWindowAssociation(fac, hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
	IDXGISwapChain1_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D, (void **)&sc_buffer);
	hidden_window = CreateWindowExA(0, "STATIC", "mw2coop d3d9", WS_POPUP, -32000, -32000, (int)bb_w, (int)bb_h, NULL, NULL, GetModuleHandleA(NULL), NULL);
	log_printf("mirror11: display: the window %p shows the Direct3D 11 picture (%ux%u); Direct3D 9 presents to the hidden window %p", (void *)hwnd, bb_w, bb_h, (void *)hidden_window);
	return 1;
}

static HRESULT STDMETHODCALLTYPE present_hook(IDirect3DDevice9 *d, const RECT *a, const RECT *b, HWND w, const RGNDATA *r)
{
	frames++;
	if (frames == 1 || frames == 60)
		log_printf("mirror11: present %d (draws %ld, replayed %ld)", frames, stat.draws, stat.draws - stat.draws_skipped);
	if (!display_mode && (frames % 60 == 0 || frames == 120))
		compare_frame(d);
	if (display_mode && sc_buffer && main_target.tex) {
		ID3D11DeviceContext_CopyResource(ctx11, (ID3D11Resource *)sc_buffer, (ID3D11Resource *)main_target.tex);
		ID3D11DeviceContext_ClearState(ctx11);
		IDXGISwapChain1_Present(swapchain, 0, 0);
		if (frames == 120 || frames == 600) // a look at what the swap chain shows: the mirror's own comparison is off in this mode, so compare once
			compare_frame(d);
		return present_o(d, a, b, hidden_window, r);
	}
	ID3D11DeviceContext_ClearState(ctx11);
	return present_o(d, a, b, w, r);
}

// ---- start
static void init_defaults(void)
{
	st.rs[7] = 1; st.rs[14] = 1; st.rs[23] = 4; st.rs[22] = 3; st.rs[8] = 3; st.rs[19] = 2; st.rs[20] = 1; st.rs[171] = 1; st.rs[168] = 15; st.rs[25] = 8;
	st.rs[56] = st.rs[189] = 8; st.rs[55] = st.rs[53] = st.rs[54] = st.rs[188] = st.rs[186] = st.rs[187] = 1; st.rs[58] = st.rs[59] = 0xff;
}

void mirror11_hook_device(struct IDirect3DDevice9 *device)
{
	static int started;
	display_mode = getenv("MW2COOP_D3D11") != NULL;
	if ((!getenv("MW2COOP_D3D11MIRROR") && !display_mode) || started)
		return;
	started = 1;
	dev9 = (IDirect3DDevice9 *)device;
	HMODULE h11 = LoadLibraryA("d3d11.dll");
	h_d3dcompiler = LoadLibraryA("d3dcompiler_47.dll");
	if (!h11 || !h_d3dcompiler) {
		log_printf("mirror11: d3d11.dll or d3dcompiler_47.dll missing");
		return;
	}
	p_compile = (void *)GetProcAddress(h_d3dcompiler, "D3DCompile");
	HRESULT (WINAPI *create)(void *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **) = (void *)GetProcAddress(h11, "D3D11CreateDevice");
	const D3D_FEATURE_LEVEL lv[] = {D3D_FEATURE_LEVEL_11_0};
	if (!create || FAILED(create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, lv, 1, D3D11_SDK_VERSION, &dev11, NULL, &ctx11))) {
		log_printf("mirror11: no Direct3D 11 device");
		return;
	}
	IDirect3DSurface9 *bb = NULL;
	if (FAILED(IDirect3DDevice9_GetBackBuffer(dev9, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
		return;
	D3DSURFACE_DESC sd;
	IDirect3DSurface9_GetDesc(bb, &sd);
	bb_surface = bb; // the reference from GetBackBuffer is kept: the pointer stays the one the game gets back
	bb_w = sd.Width;
	bb_h = sd.Height;
	bb_fmt = sd.Format;
	{ // the target the mirror draws the back buffer's content into
		tex_t *t = &main_target;
		memset(t, 0, sizeof(*t));
		t->kind = 1; t->w = bb_w; t->h = bb_h; t->levels = 1; t->rt = 1; t->fmt = D3DFMT_A8R8G8B8;
		D3D11_TEXTURE2D_DESC td = {0};
		td.Width = bb_w; td.Height = bb_h; td.MipLevels = 1; td.ArraySize = 1;
		td.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		ID3D11Device_CreateTexture2D(dev11, &td, NULL, &t->tex);
		D3D11_RENDER_TARGET_VIEW_DESC rd = {0};
		rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		ID3D11Device_CreateRenderTargetView(dev11, (ID3D11Resource *)t->tex, &rd, &t->rtv);
		rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
		ID3D11Device_CreateRenderTargetView(dev11, (ID3D11Resource *)t->tex, &rd, &t->rtv_srgb);
	}
	cb_vs0 = make_cb(4096); cb_vs1 = make_cb(384); cb_ps0 = make_cb(4096); cb_ps1 = make_cb(384); cb_ps2 = make_cb(16);
	init_defaults();
	if (display_mode && !display_start())
		display_mode = 0;
	DWORD n = GetModuleFileNameA(NULL, out_dir, MAX_PATH);
	while (n > 0 && out_dir[n - 1] != '\\')
		n--;
	out_dir[n] = 0;
	lstrcatA(out_dir, "mw2coop\\mirror11\\");
	CreateDirectoryA(out_dir, NULL);
	void **vt = *(void ***)dev9;
	PATCH(vt, 17, present);
	PATCH(vt, 23, create_texture);
	PATCH(vt, 25, create_cube);
	PATCH(vt, 26, create_vb);
	PATCH(vt, 27, create_ib);
	PATCH(vt, 29, create_depth);
	PATCH(vt, 34, stretch_rect);
	PATCH(vt, 37, set_rt);
	PATCH(vt, 39, set_ds);
	PATCH(vt, 43, clear);
	PATCH(vt, 47, set_viewport);
	PATCH(vt, 57, set_rs);
	PATCH(vt, 65, set_texture);
	PATCH(vt, 69, set_ss);
	PATCH(vt, 75, set_scissor);
	PATCH(vt, 81, draw_prim);
	PATCH(vt, 82, draw_indexed);
	PATCH(vt, 83, draw_prim_up);
	PATCH(vt, 84, draw_indexed_up);
	PATCH(vt, 86, create_decl);
	PATCH(vt, 87, set_decl);
	PATCH(vt, 91, create_vs);
	PATCH(vt, 92, set_vs);
	PATCH(vt, 94, set_vs_f);
	PATCH(vt, 96, set_vs_i);
	PATCH(vt, 98, set_vs_b);
	PATCH(vt, 100, set_stream);
	PATCH(vt, 104, set_indices);
	PATCH(vt, 106, create_ps);
	PATCH(vt, 107, set_ps);
	PATCH(vt, 109, set_ps_f);
	PATCH(vt, 111, set_ps_i);
	PATCH(vt, 113, set_ps_b);
	log_printf("mirror11: Direct3D 11 mirror on (%ux%u back buffer, format %d), pictures go to %s", bb_w, bb_h, (int)bb_fmt, out_dir);
}
