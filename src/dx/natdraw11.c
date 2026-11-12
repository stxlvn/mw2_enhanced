// The flush half of the new back end on Direct3D 11 (see natdraw11.h).
#include "natdraw11.h"

#ifdef _WIN32
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sm3.h"
#include "state11.h"

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ key -> object caches
typedef struct {
	uint32_t key[8];
	void *val;
	int used;
} kc_entry;
typedef struct {
	kc_entry *e;
	size_t cap, n;
} kc;

static size_t kc_hash(const uint32_t *k)
{
	uint64_t h = 1469598103934665603ull;
	for (int i = 0; i < 8; i++)
		h = (h ^ k[i]) * 1099511628211ull;
	return (size_t)(h ^ h >> 29);
}
static void *kc_get(kc *c, const uint32_t *key, int *found)
{
	*found = 0;
	if (!c->cap)
		return NULL;
	for (size_t i = kc_hash(key) & (c->cap - 1);; i = (i + 1) & (c->cap - 1)) {
		if (!c->e[i].used)
			return NULL;
		if (!memcmp(c->e[i].key, key, 32)) {
			*found = 1;
			return c->e[i].val;
		}
	}
}
static void kc_put(kc *c, const uint32_t *key, void *val)
{
	if (!c->cap || (c->n + 1) * 2 > c->cap) {
		kc_entry *old = c->e;
		const size_t oc = c->cap;
		c->cap = oc ? oc * 2 : 256;
		c->e = calloc(c->cap, sizeof(kc_entry));
		c->n = 0;
		for (size_t i = 0; i < oc; i++)
			if (old[i].used)
				kc_put(c, old[i].key, old[i].val);
		free(old);
	}
	for (size_t i = kc_hash(key) & (c->cap - 1);; i = (i + 1) & (c->cap - 1)) {
		if (!c->e[i].used) {
			memcpy(c->e[i].key, key, 32);
			c->e[i].val = val;
			c->e[i].used = 1;
			c->n++;
			return;
		}
	}
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ declarations and shaders
struct nd_decl {
	nd_elem el[32];
	int n;
	sm3_int_input ints[32];
	int nints;
	unsigned sig;
};

nd_decl *nd_decl_create(const nd_elem *elems, int count)
{
	nd_decl *d = calloc(1, sizeof(*d));
	d->n = count > 32 ? 32 : count;
	memcpy(d->el, elems, (size_t)d->n * sizeof(nd_elem));
	unsigned h = 17;
	for (int i = 0; i < d->n; i++) {
		const int t = d->el[i].type;
		if (t != 5 && t != 6 && t != 7) // UBYTE4, SHORT2, SHORT4
			continue;
		d->ints[d->nints].usage = d->el[i].usage;
		d->ints[d->nints].index = d->el[i].index;
		d->ints[d->nints].kind = t == 5 ? 1 : 2;
		h = h * 31 + (unsigned)(d->el[i].usage | d->el[i].index << 4 | d->ints[d->nints].kind << 8);
		d->nints++;
	}
	d->sig = d->nints ? h : 0;
	return d;
}
void nd_decl_destroy(nd_decl *d)
{
	free(d);
}

struct nd_shader {
	nat_device *dev;
	uint32_t *code;
	size_t dwords;
	int is_vs;
	struct {
		unsigned sig;
		nat_shader *s;
	} var[8];
	int nvar;
	int failed;
};

nd_shader *nd_shader_create(nd *d, const uint32_t *code, size_t dwords)
{
	(void)d;
	nd_shader *s = calloc(1, sizeof(*s));
	s->code = malloc(dwords * 4);
	memcpy(s->code, code, dwords * 4);
	s->dwords = dwords;
	s->is_vs = (code[0] >> 16) == 0xfffe;
	return s;
}
void nd_shader_destroy(nd_shader *s)
{
	if (!s)
		return;
	for (int i = 0; i < s->nvar; i++)
		nat_shader_destroy(s->var[i].s);
	free(s->code);
	free(s);
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ the flush
struct nd {
	nat_device *dev;
	ID3D11Device *d3d;
	ID3D11DeviceContext *ctx;
	nd_ops ops;
	ID3D11Buffer *cb_vs, *cb_ps, *cb_ints, *cb_alpha;
	kc blend, depth, raster, sampler, layout;
	nat_image *rt, *ds;
	float vp[6];
	int scissor_on;
	RECT scissor;
	unsigned stencil_ref, stencil_rmask, stencil_wmask;
	nat_buffer *vb[4];
	unsigned vb_off[4], vb_stride[4];
	nat_buffer *ib;
	int ib32;
	char err[128];
	unsigned n_states, n_layouts, n_shaders, n_uploads, n_draws, n_refused;
	ID3D11Buffer *vb_cur[4];
	int vp_dirty;
	int consts_init;
};

static ID3D11Buffer *make_cb(nd *d, unsigned size)
{
	D3D11_BUFFER_DESC bd = {0};
	bd.ByteWidth = size;
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	ID3D11Buffer *b = NULL;
	ID3D11Device_CreateBuffer(d->d3d, &bd, NULL, &b);
	return b;
}

static void put_cb(nd *d, ID3D11Buffer *b, const void *data, size_t size)
{
	D3D11_MAPPED_SUBRESOURCE m;
	if (b && SUCCEEDED(ID3D11DeviceContext_Map(d->ctx, (ID3D11Resource *)b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
		memcpy(m.pData, data, size);
		ID3D11DeviceContext_Unmap(d->ctx, (ID3D11Resource *)b, 0);
	}
}

nd *nd_create(nat_device *dev, const nd_ops *ops)
{
	nd *d = calloc(1, sizeof(*d));
	d->dev = dev;
	d->d3d = nat_d3d_device(dev);
	d->ctx = nat_d3d_context(dev);
	d->ops = *ops;
	d->cb_vs = make_cb(d, 4096);
	d->cb_ps = make_cb(d, 4096);
	d->cb_ints = make_cb(d, 384);
	d->cb_alpha = make_cb(d, 16);
	static const uint8_t zeros[384];
	put_cb(d, d->cb_ints, zeros, sizeof(zeros)); // integer / boolean constants: nothing in the pass lists sets them
	unsigned w, h;
	nat_get_size(dev, &w, &h);
	d->vp[2] = (float)w;
	d->vp[3] = (float)h;
	d->vp[5] = 1.0f;
	d->vp_dirty = 1;
	d->stencil_rmask = d->stencil_wmask = 0xff;
	return d;
}

void nd_destroy(nd *d)
{
	if (!d)
		return;
	kc *all[] = {&d->blend, &d->depth, &d->raster, &d->sampler, &d->layout};
	for (int k = 0; k < 5; k++) {
		for (size_t i = 0; i < all[k]->cap; i++)
			if (all[k]->e[i].used && all[k]->e[i].val && all[k]->e[i].val != (void *)1)
				IUnknown_Release((IUnknown *)all[k]->e[i].val);
		free(all[k]->e);
	}
	if (d->cb_vs)
		ID3D11Buffer_Release(d->cb_vs);
	if (d->cb_ps)
		ID3D11Buffer_Release(d->cb_ps);
	if (d->cb_ints)
		ID3D11Buffer_Release(d->cb_ints);
	if (d->cb_alpha)
		ID3D11Buffer_Release(d->cb_alpha);
	free(d);
}

void nd_set_targets(nd *d, nat_image *color, nat_image *depth)
{
	d->rt = color;
	d->ds = depth;
}
void nd_set_viewport(nd *d, float x, float y, float w, float h, float min_z, float max_z)
{
	d->vp[0] = x;
	d->vp[1] = y;
	d->vp[2] = w;
	d->vp[3] = h;
	d->vp[4] = min_z;
	d->vp[5] = max_z;
	d->vp_dirty = 1;
}
void nd_set_scissor(nd *d, int enable, int left, int top, int right, int bottom)
{
	d->scissor_on = enable;
	d->scissor.left = left;
	d->scissor.top = top;
	d->scissor.right = right;
	d->scissor.bottom = bottom;
}
void nd_set_stencil(nd *d, unsigned ref, unsigned read_mask, unsigned write_mask)
{
	d->stencil_ref = ref;
	d->stencil_rmask = read_mask;
	d->stencil_wmask = write_mask;
}
void nd_set_stream(nd *d, unsigned slot, nat_buffer *vb, unsigned offset, unsigned stride)
{
	if (slot < 4) {
		d->vb[slot] = vb;
		d->vb_off[slot] = offset;
		d->vb_stride[slot] = stride;
	}
}
void nd_set_indices(nd *d, nat_buffer *ib, int is32)
{
	d->ib = ib;
	d->ib32 = is32;
}
const char *nd_last_error(const nd *d)
{
	return d->err;
}
void nd_stats(const nd *d, unsigned *states, unsigned *layouts, unsigned *shaders, unsigned *uploads, unsigned *draws, unsigned *refused)
{
	*states = d->n_states;
	*layouts = d->n_layouts;
	*shaders = d->n_shaders;
	*uploads = d->n_uploads;
	*draws = d->n_draws;
	*refused = d->n_refused;
}

static void get_views(nd *d, int srgb, ID3D11RenderTargetView **rtv, ID3D11DepthStencilView **dsv)
{
	*rtv = d->rt ? nat_image_rtv(d->rt, srgb) : (srgb ? nat_scene_rtv_srgb(d->dev) : nat_scene_rtv(d->dev));
	*dsv = d->ds == ND_NO_DEPTH ? NULL : d->ds ? nat_image_dsv(d->ds) : nat_scene_dsv(d->dev);
}

void nd_clear(nd *d, unsigned flags, const float rgba[4], float depth, unsigned stencil)
{
	ID3D11RenderTargetView *rtv;
	ID3D11DepthStencilView *dsv;
	get_views(d, 0, &rtv, &dsv);
	if ((flags & 1) && rtv)
		ID3D11DeviceContext_ClearRenderTargetView(d->ctx, rtv, rgba);
	if ((flags & 6) && dsv)
		ID3D11DeviceContext_ClearDepthStencilView(d->ctx, dsv, ((flags & 2) ? D3D11_CLEAR_DEPTH : 0) | ((flags & 4) ? D3D11_CLEAR_STENCIL : 0), depth, (UINT8)stencil);
}

static DXGI_FORMAT decl_format(int type)
{
	switch (type) {
	case 0: return DXGI_FORMAT_R32_FLOAT;
	case 1: return DXGI_FORMAT_R32G32_FLOAT;
	case 2: return DXGI_FORMAT_R32G32B32_FLOAT;
	case 3: return DXGI_FORMAT_R32G32B32A32_FLOAT;
	case 4: return DXGI_FORMAT_B8G8R8A8_UNORM;     // D3DCOLOR
	case 5: return DXGI_FORMAT_R8G8B8A8_UINT;      // UBYTE4 (the shader converts, see sm3_int_input)
	case 6: return DXGI_FORMAT_R16G16_SINT;        // SHORT2
	case 7: return DXGI_FORMAT_R16G16B16A16_SINT;  // SHORT4
	case 8: return DXGI_FORMAT_R8G8B8A8_UNORM;     // UBYTE4N
	case 9: return DXGI_FORMAT_R16G16_SNORM;
	case 10: return DXGI_FORMAT_R16G16B16A16_SNORM;
	case 11: return DXGI_FORMAT_R16G16_UNORM;
	case 12: return DXGI_FORMAT_R16G16B16A16_UNORM;
	case 15: return DXGI_FORMAT_R16G16_FLOAT;
	case 16: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	}
	return DXGI_FORMAT_UNKNOWN;
}
static const char *const SEMANTICS[14] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"};

static nat_shader *variant(nd *d, nd_shader *s, const nd_decl *decl)
{
	const unsigned sig = s->is_vs && decl ? decl->sig : 0;
	for (int i = 0; i < s->nvar; i++)
		if (s->var[i].sig == sig)
			return s->var[i].s;
	if (s->nvar >= 8 || s->failed)
		return NULL;
	char err[160] = "";
	nat_shader *n = nat_shader_from_sm3(d->dev, s->code, s->dwords, 1, s->is_vs && decl ? (const void *)decl->ints : NULL, s->is_vs && decl ? decl->nints : 0, err, sizeof(err));
	if (!n) {
		s->failed = 1;
		snprintf(d->err, sizeof(d->err), "shader: %.110s", err);
		return NULL;
	}
	s->var[s->nvar].sig = sig;
	s->var[s->nvar].s = n;
	s->nvar++;
	d->n_shaders++;
	return n;
}

static ID3D11InputLayout *layout_for(nd *d, const nd_decl *decl, nat_shader *vs)
{
	uint32_t key[8] = {0};
	memcpy(key, &decl, sizeof(decl));
	memcpy(key + 2, &vs, sizeof(vs));
	int found;
	ID3D11InputLayout *l = kc_get(&d->layout, key, &found);
	if (found)
		return l == (ID3D11InputLayout *)1 ? NULL : l;
	D3D11_INPUT_ELEMENT_DESC ed[32];
	int n = 0;
	for (int i = 0; i < decl->n; i++) {
		const DXGI_FORMAT f = decl_format(decl->el[i].type);
		if (f == DXGI_FORMAT_UNKNOWN || decl->el[i].usage >= 14)
			continue;
		ed[n].SemanticName = SEMANTICS[decl->el[i].usage];
		ed[n].SemanticIndex = decl->el[i].index;
		ed[n].Format = f;
		ed[n].InputSlot = decl->el[i].stream;
		ed[n].AlignedByteOffset = decl->el[i].offset;
		ed[n].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
		ed[n].InstanceDataStepRate = 0;
		n++;
	}
	size_t bsz = 0;
	void *blob = nat_shader_blob(vs, &bsz);
	if (FAILED(ID3D11Device_CreateInputLayout(d->d3d, ed, n, blob, bsz, &l))) {
		snprintf(d->err, sizeof(d->err), "input layout (%d elements)", n);
		kc_put(&d->layout, key, (void *)1);
		return NULL;
	}
	kc_put(&d->layout, key, l);
	d->n_layouts++;
	return l;
}

static D3D11_BLEND alpha_factor(int b)
{
	switch (b) { // the alpha channel takes no color factors in Direct3D 11
	case 3: return D3D11_BLEND_SRC_ALPHA;
	case 4: return D3D11_BLEND_INV_SRC_ALPHA;
	case 9: return D3D11_BLEND_DEST_ALPHA;
	case 10: return D3D11_BLEND_INV_DEST_ALPHA;
	}
	return (D3D11_BLEND)b;
}

static ID3D11BlendState *blend_state(nd *d, const state11_t *s)
{
	uint32_t key[8] = {(uint32_t)s->blend_enable, (uint32_t)s->src, (uint32_t)s->dst, (uint32_t)s->op, (uint32_t)s->src_a, (uint32_t)s->dst_a, (uint32_t)s->op_a, (uint32_t)s->write_mask};
	int found;
	ID3D11BlendState *b = kc_get(&d->blend, key, &found);
	if (found)
		return b;
	D3D11_BLEND_DESC bd = {0};
	bd.RenderTarget[0].BlendEnable = s->blend_enable != 0;
	bd.RenderTarget[0].SrcBlend = (D3D11_BLEND)s->src;
	bd.RenderTarget[0].DestBlend = (D3D11_BLEND)s->dst;
	bd.RenderTarget[0].BlendOp = (D3D11_BLEND_OP)s->op;
	bd.RenderTarget[0].SrcBlendAlpha = alpha_factor(s->src_a);
	bd.RenderTarget[0].DestBlendAlpha = alpha_factor(s->dst_a);
	bd.RenderTarget[0].BlendOpAlpha = (D3D11_BLEND_OP)s->op_a;
	bd.RenderTarget[0].RenderTargetWriteMask = (UINT8)s->write_mask;
	if (!s->blend_enable) { // a disabled blend still needs valid values
		bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
		bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
		bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
		bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	}
	if (FAILED(ID3D11Device_CreateBlendState(d->d3d, &bd, &b)))
		b = NULL;
	kc_put(&d->blend, key, b);
	d->n_states++;
	return b;
}

static void stencil_face(D3D11_DEPTH_STENCILOP_DESC *o, const state11_stencil_t *f)
{
	o->StencilFunc = (D3D11_COMPARISON_FUNC)(f->func >= 1 && f->func <= 8 ? f->func : 8);
	o->StencilPassOp = (D3D11_STENCIL_OP)(f->pass >= 1 && f->pass <= 8 ? f->pass : 1);
	o->StencilFailOp = (D3D11_STENCIL_OP)(f->fail >= 1 && f->fail <= 8 ? f->fail : 1);
	o->StencilDepthFailOp = (D3D11_STENCIL_OP)(f->zfail >= 1 && f->zfail <= 8 ? f->zfail : 1);
}

static ID3D11DepthStencilState *depth_state(nd *d, const state11_t *s)
{
	uint32_t key[8] = {(uint32_t)s->z_enable, (uint32_t)s->z_write, (uint32_t)s->z_func, (uint32_t)s->stencil_enable, (uint32_t)(s->front.func | s->front.pass << 4 | s->front.fail << 8 | s->front.zfail << 12),
					   (uint32_t)(s->back.func | s->back.pass << 4 | s->back.fail << 8 | s->back.zfail << 12), d->stencil_rmask | d->stencil_wmask << 8, 0};
	int found;
	ID3D11DepthStencilState *o = kc_get(&d->depth, key, &found);
	if (found)
		return o;
	D3D11_DEPTH_STENCIL_DESC dd = {0};
	dd.DepthEnable = s->z_enable != 0;
	dd.DepthWriteMask = s->z_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
	dd.DepthFunc = (D3D11_COMPARISON_FUNC)(s->z_func >= 1 && s->z_func <= 8 ? s->z_func : 8);
	dd.StencilEnable = s->stencil_enable != 0;
	dd.StencilReadMask = (UINT8)d->stencil_rmask;
	dd.StencilWriteMask = (UINT8)d->stencil_wmask;
	stencil_face(&dd.FrontFace, &s->front);
	stencil_face(&dd.BackFace, s->stencil_two_sided ? &s->back : &s->front);
	if (FAILED(ID3D11Device_CreateDepthStencilState(d->d3d, &dd, &o)))
		o = NULL;
	kc_put(&d->depth, key, o);
	d->n_states++;
	return o;
}

static ID3D11RasterizerState *raster_state(nd *d, const state11_t *s, int scissor, float bias, float slope)
{
	uint32_t key[8] = {(uint32_t)s->cull, (uint32_t)s->fill, (uint32_t)scissor, 0, 0, 0, 0, 0};
	memcpy(&key[3], &bias, 4);
	memcpy(&key[4], &slope, 4);
	int found;
	ID3D11RasterizerState *r = kc_get(&d->raster, key, &found);
	if (found)
		return r;
	D3D11_RASTERIZER_DESC rd = {0};
	rd.FillMode = s->fill == 2 ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
	rd.CullMode = s->cull == 1 ? D3D11_CULL_NONE : s->cull == 2 ? D3D11_CULL_FRONT : D3D11_CULL_BACK;
	rd.DepthClipEnable = TRUE;
	rd.ScissorEnable = scissor != 0;
	rd.DepthBias = (INT)(bias * 16777215.0f);
	rd.SlopeScaledDepthBias = slope;
	if (FAILED(ID3D11Device_CreateRasterizerState(d->d3d, &rd, &r)))
		r = NULL;
	kc_put(&d->raster, key, r);
	d->n_states++;
	return r;
}

static ID3D11SamplerState *sampler_state(nd *d, uint32_t word)
{
	uint32_t key[8] = {word & ~0x4000000u, 0, 0, 0, 0, 0, 0, 0}; // the sRGB bit belongs to the view, not the sampler
	int found;
	ID3D11SamplerState *o = kc_get(&d->sampler, key, &found);
	if (found)
		return o;
	state11_sampler_t sd;
	state11_sampler_decode(word, &sd);
	D3D11_SAMPLER_DESC desc = {0};
	if (sd.min == 3 || sd.mag == 3 || sd.mip == 3 || sd.aniso >= 2)
		desc.Filter = D3D11_FILTER_ANISOTROPIC;
	else
		desc.Filter = (D3D11_FILTER)((sd.min == 2 ? 0x10 : 0) | (sd.mag == 2 ? 4 : 0) | (sd.mip == 2 ? 1 : 0));
	desc.AddressU = (D3D11_TEXTURE_ADDRESS_MODE)(sd.addr_u >= 1 && sd.addr_u <= 4 ? sd.addr_u : 1);
	desc.AddressV = (D3D11_TEXTURE_ADDRESS_MODE)(sd.addr_v >= 1 && sd.addr_v <= 4 ? sd.addr_v : 1);
	desc.AddressW = (D3D11_TEXTURE_ADDRESS_MODE)(sd.addr_w >= 1 && sd.addr_w <= 4 ? sd.addr_w : 1);
	desc.MaxAnisotropy = sd.aniso >= 2 ? (UINT)(sd.aniso > 16 ? 16 : sd.aniso) : 1;
	desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	desc.MaxLOD = sd.mip == 0 ? 0 : D3D11_FLOAT32_MAX;
	if (FAILED(ID3D11Device_CreateSamplerState(d->d3d, &desc, &o)))
		o = NULL;
	kc_put(&d->sampler, key, o);
	d->n_states++;
	return o;
}

int nd_draw_indexed(nd *d, ng_state *g, int base_vertex, unsigned start_index, unsigned prim_count)
{
	d->err[0] = 0;
	nd_shader *vsg = g->vs ? d->ops.shader(g->vs) : NULL, *psg = g->ps ? d->ops.shader(g->ps) : NULL;
	nd_decl *decl = g->decl ? d->ops.decl(g->decl) : NULL;
	if (!vsg || !decl || !g->words_valid) {
		snprintf(d->err, sizeof(d->err), "%s", !vsg ? "no vertex shader" : !decl ? "no vertex declaration" : "no state words");
		d->n_refused++;
		return 0;
	}
	nat_shader *vs = variant(d, vsg, decl), *ps = psg ? variant(d, psg, NULL) : NULL;
	if (!vs || (psg && !ps)) {
		d->n_refused++;
		return 0;
	}
	ID3D11InputLayout *layout = layout_for(d, decl, vs);
	if (!layout || !d->ib || !d->vb[0]) {
		if (!d->err[0])
			snprintf(d->err, sizeof(d->err), "%s", !d->ib ? "no index buffer" : "no vertex stream");
		d->n_refused++;
		return 0;
	}
	state11_t st;
	state11_decode(g->word0, g->word1, &st);
	ID3D11RenderTargetView *rtv;
	ID3D11DepthStencilView *dsv;
	get_views(d, st.srgb_write, &rtv, &dsv);
	if (!rtv && !dsv) {
		snprintf(d->err, sizeof(d->err), "no render target");
		d->n_refused++;
		return 0;
	}
	// images: an image that is the current target is not bound as an input (the game unbinds it too)
	ID3D11ShaderResourceView *srv[16] = {0};
	ID3D11SamplerState *smp[16] = {0};
	for (int i = 0; i < 16; i++) {
		nat_image *im = g->tex[i] ? d->ops.image(g->tex[i]) : NULL;
		if (!im || im == d->rt || im == d->ds)
			continue;
		srv[i] = nat_image_srv(im, (g->sampler_word[i] >> 26) & 1);
		smp[i] = sampler_state(d, g->sampler_word[i]);
	}
	// constants
	int vs_dirty = 0, ps_dirty = 0;
	for (int i = 0; i < 8; i++) {
		vs_dirty |= g->vs_dirty[i] != 0;
		ps_dirty |= g->ps_dirty[i] != 0;
	}
	if (vs_dirty || !d->consts_init) {
		put_cb(d, d->cb_vs, g->vs_c, sizeof(g->vs_c));
		memset(g->vs_dirty, 0, sizeof(g->vs_dirty));
		d->n_uploads++;
	}
	if (ps_dirty || !d->consts_init) {
		put_cb(d, d->cb_ps, g->ps_c, sizeof(g->ps_c));
		memset(g->ps_dirty, 0, sizeof(g->ps_dirty));
		d->n_uploads++;
	}
	d->consts_init = 1;
	const float at[4] = {(float)st.alpha_func, (float)st.alpha_ref, st.alpha_test_enable ? 1.0f : 0.0f, 0};
	put_cb(d, d->cb_alpha, at, sizeof(at));
	float bias = 0, slope = 0;
	if (st.polygon_offset && d->ops.poly_offset)
		d->ops.poly_offset(st.polygon_offset, &bias, &slope);
	ID3D11DeviceContext *c = d->ctx;
	ID3D11DeviceContext_OMSetRenderTargets(c, rtv ? 1 : 0, &rtv, dsv);
	D3D11_VIEWPORT vp = {d->vp[0], d->vp[1], d->vp[2], d->vp[3], d->vp[4], d->vp[5]};
	ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
	if (d->scissor_on)
		ID3D11DeviceContext_RSSetScissorRects(c, 1, &d->scissor);
	ID3D11DeviceContext_RSSetState(c, raster_state(d, &st, d->scissor_on, bias, slope));
	ID3D11DeviceContext_OMSetBlendState(c, blend_state(d, &st), NULL, 0xffffffffu);
	ID3D11DeviceContext_OMSetDepthStencilState(c, depth_state(d, &st), d->stencil_ref);
	ID3D11DeviceContext_IASetInputLayout(c, layout);
	ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11DeviceContext_VSSetShader(c, nat_shader_object(vs), NULL, 0);
	ID3D11DeviceContext_PSSetShader(c, ps ? nat_shader_object(ps) : NULL, NULL, 0);
	ID3D11Buffer *vcb[2] = {d->cb_vs, d->cb_ints}, *pcb[3] = {d->cb_ps, d->cb_ints, d->cb_alpha};
	ID3D11DeviceContext_VSSetConstantBuffers(c, 0, 2, vcb);
	ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 3, pcb);
	ID3D11DeviceContext_PSSetShaderResources(c, 0, 16, srv);
	ID3D11DeviceContext_PSSetSamplers(c, 0, 16, smp);
	ID3D11Buffer *vbs[4];
	UINT strides[4], offs[4];
	for (int i = 0; i < 4; i++) {
		vbs[i] = d->vb[i] ? nat_buffer_resource(d->vb[i]) : NULL;
		strides[i] = d->vb_stride[i];
		offs[i] = d->vb_off[i];
	}
	ID3D11DeviceContext_IASetVertexBuffers(c, 0, 4, vbs, strides, offs);
	ID3D11DeviceContext_IASetIndexBuffer(c, nat_buffer_resource(d->ib), d->ib32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0);
	ID3D11DeviceContext_DrawIndexed(c, prim_count * 3, start_index, base_vertex);
	d->n_draws++;
	return 1;
}

#else
nd *nd_create(nat_device *dev, const nd_ops *ops)
{
	(void)dev;
	(void)ops;
	return NULL;
}
#endif
