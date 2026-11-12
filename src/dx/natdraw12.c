// The flush half of the new back end on Direct3D 12 (the same API as natdraw11.c, see natdraw11.h): pipeline state objects made from the pass state words, one root signature, descriptor tables
// and constants from the device's per-frame rings, the draw. Needs nat12.c (nat12x.h).
#include "natdraw11.h"

#ifdef _WIN32
#include "nat12x.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sm3.h"
#include "state11.h"

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ key -> object caches
#define KW 24
typedef struct {
	uint32_t key[KW];
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
	for (int i = 0; i < KW; i++)
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
		if (!memcmp(c->e[i].key, key, sizeof(c->e[i].key))) {
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
			memcpy(c->e[i].key, key, sizeof(c->e[i].key));
			c->e[i].val = val;
			c->e[i].used = 1;
			c->n++;
			return;
		}
	}
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ declarations and shaders
// (they start with the vtable pointer of a Direct3D 9 object: the engine's unload code calls Release on the pointer it kept, see natdraw11.c)
static long obj_nop(void *self)
{
	(void)self;
	return 0;
}
static long decl_release(void *self);
static long shader_release(void *self);
static void *decl_vt[4], *shader_vt[4];
static void obj_vts_init(void)
{
	if (decl_vt[0])
		return;
	decl_vt[0] = decl_vt[1] = decl_vt[3] = (void *)obj_nop;
	decl_vt[2] = (void *)decl_release;
	shader_vt[0] = shader_vt[1] = shader_vt[3] = (void *)obj_nop;
	shader_vt[2] = (void *)shader_release;
}

struct nd_decl {
	void **vt;
	nd_elem el[32];
	int n;
	sm3_int_input ints[32];
	int nints;
	unsigned sig;
};

nd_decl *nd_decl_create(const nd_elem *elems, int count)
{
	nd_decl *d = calloc(1, sizeof(*d));
	obj_vts_init();
	d->vt = decl_vt;
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
void nd_decl_destroy(nd_decl *d) { free(d); }

struct nd_shader {
	void **vt;
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
	obj_vts_init();
	s->vt = shader_vt;
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
static long decl_release(void *self)
{
	nd_decl_destroy((nd_decl *)self);
	return 0;
}
static long shader_release(void *self)
{
	nd_shader_destroy((nd_shader *)self);
	return 0;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ the flush
struct nd {
	nat_device *dev;
	ID3D12Device *d3d;
	ID3D12GraphicsCommandList *list;
	nd_ops ops;
	ID3D12RootSignature *root;
	kc pso, sampler;
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
	// what is bound in the list being recorded (valid while list_gen is the device's)
	unsigned list_gen;
	D3D12_GPU_VIRTUAL_ADDRESS cb_vs_va, cb_ps_va, cb_ints_va, cb_alpha_va;
	float alpha_last[4];
	UINT64 srv_last[16];
	D3D12_GPU_DESCRIPTOR_HANDLE srv_table_last;
	int srv_table_valid;
	UINT64 smp_last_key;
	D3D12_CPU_DESCRIPTOR_HANDLE smp_default;
	int smp_default_ok;
	ID3D12PipelineState *pso_cur;
	const void *decls_seen[64]; // statistics: distinct vertex declarations drawn with
	unsigned ndecls;
	// blit
	nat_shader *blit_vs, *blit_ps;
	int blit_alpha;
	D3D12_CPU_DESCRIPTOR_HANDLE blit_smp[2];
	int blit_smp_ok[2];
};

#define RS_VS_B0 0
#define RS_VS_B1 1
#define RS_PS_B3 2
#define RS_PS_B4 3
#define RS_PS_B5 4
#define RS_SRV 5
#define RS_SMP 6

static int make_root(nd *d)
{
	D3D12_DESCRIPTOR_RANGE rsrv = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 16, 0, 0, 0}, rsmp = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 16, 0, 0, 0};
	D3D12_ROOT_PARAMETER p[7] = {0};
	const UINT regs[5] = {0, 1, 3, 4, 5};
	const D3D12_SHADER_VISIBILITY vis[5] = {D3D12_SHADER_VISIBILITY_VERTEX, D3D12_SHADER_VISIBILITY_VERTEX, D3D12_SHADER_VISIBILITY_PIXEL, D3D12_SHADER_VISIBILITY_PIXEL, D3D12_SHADER_VISIBILITY_PIXEL};
	for (int i = 0; i < 5; i++) {
		p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		p[i].Descriptor.ShaderRegister = regs[i];
		p[i].Descriptor.RegisterSpace = 0;
		p[i].ShaderVisibility = vis[i];
	}
	p[RS_SRV].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	p[RS_SRV].DescriptorTable.NumDescriptorRanges = 1;
	p[RS_SRV].DescriptorTable.pDescriptorRanges = &rsrv;
	p[RS_SRV].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	p[RS_SMP].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	p[RS_SMP].DescriptorTable.NumDescriptorRanges = 1;
	p[RS_SMP].DescriptorTable.pDescriptorRanges = &rsmp;
	p[RS_SMP].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC rd = {7, p, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
	ID3DBlob *blob = NULL, *errs = NULL;
	if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &errs))) {
		if (errs)
			ID3D10Blob_Release(errs);
		return 0;
	}
	const HRESULT hr = ID3D12Device_CreateRootSignature(d->d3d, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&d->root);
	ID3D10Blob_Release(blob);
	return SUCCEEDED(hr);
}

nd *nd_create(nat_device *dev, const nd_ops *ops)
{
	nd *d = calloc(1, sizeof(*d));
	d->dev = dev;
	d->d3d = nat12_device(dev);
	d->list = nat12_list(dev);
	d->ops = *ops;
	if (!make_root(d)) {
		free(d);
		return NULL;
	}
	unsigned w, h;
	nat_get_size(dev, &w, &h);
	d->vp[2] = (float)w;
	d->vp[3] = (float)h;
	d->vp[5] = 1.0f;
	d->stencil_rmask = d->stencil_wmask = 0xff;
	return d;
}

void nd_destroy(nd *d)
{
	if (!d)
		return;
	for (size_t i = 0; i < d->pso.cap; i++)
		if (d->pso.e[i].used && d->pso.e[i].val && d->pso.e[i].val != (void *)1)
			ID3D12PipelineState_Release((ID3D12PipelineState *)d->pso.e[i].val);
	free(d->pso.e);
	free(d->sampler.e);
	if (d->root)
		ID3D12RootSignature_Release(d->root);
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
}
void nd_set_scissor(nd *d, int enable, int left, int top, int right, int bottom)
{
	d->scissor_on = enable;
	d->scissor.left = left;
	d->scissor.top = top;
	d->scissor.right = right;
	d->scissor.bottom = bottom;
}
void nd_set_scissor_rect(nd *d, int left, int top, int right, int bottom)
{
	d->scissor.left = left;
	d->scissor.top = top;
	d->scissor.right = right;
	d->scissor.bottom = bottom;
}
void nd_set_scissor_enable(nd *d, int enable) { d->scissor_on = enable; }
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
const char *nd_last_error(const nd *d) { return d->err; }
void nd_stats(const nd *d, unsigned *states, unsigned *layouts, unsigned *shaders, unsigned *uploads, unsigned *draws, unsigned *refused)
{
	*states = d->n_states;
	*layouts = d->n_layouts;
	*shaders = d->n_shaders;
	*uploads = d->n_uploads;
	*draws = d->n_draws;
	*refused = d->n_refused;
}

// the list got reset (a new frame or a flush): the descriptor heaps and the root bindings are gone
static void check_list(nd *d)
{
	const unsigned gen = nat12_list_generation(d->dev);
	d->list = nat12_list(d->dev);
	if (d->list_gen == gen)
		return;
	d->list_gen = gen;
	d->cb_vs_va = d->cb_ps_va = d->cb_ints_va = d->cb_alpha_va = 0;
	d->srv_table_valid = 0;
	d->smp_last_key = 0;
	d->pso_cur = NULL;
	memset(d->alpha_last, 0xff, sizeof(d->alpha_last));
	nat12_set_heaps(d->dev);
	ID3D12GraphicsCommandList_SetGraphicsRootSignature(d->list, d->root);
}

static nat_image *rt_image(nd *d) { return d->rt ? d->rt : nat12_scene_image(d->dev); }
static nat_image *ds_image(nd *d) { return d->ds == ND_NO_DEPTH ? NULL : d->ds ? d->ds : nat12_scene_depth_image(d->dev); }

static void set_targets(nd *d, nat_image *rt, nat_image *ds, int srgb)
{
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = {0}, dsv = {0};
	if (rt && nat12_image_has_rtv(rt)) {
		nat12_image_transition(d->dev, rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
		rtv = nat12_image_rtv(rt, srgb);
	}
	if (ds && nat12_image_has_dsv(ds)) {
		nat12_image_transition(d->dev, ds, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		dsv = nat12_image_dsv(ds);
	}
	ID3D12GraphicsCommandList_OMSetRenderTargets(d->list, rtv.ptr ? 1 : 0, rtv.ptr ? &rtv : NULL, FALSE, dsv.ptr ? &dsv : NULL);
}

void nd_clear(nd *d, unsigned flags, const float rgba[4], float depth, unsigned stencil)
{
	check_list(d);
	nat_image *rt = rt_image(d), *ds = ds_image(d);
	if ((flags & 1) && rt && nat12_image_has_rtv(rt)) {
		nat12_image_transition(d->dev, rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
		ID3D12GraphicsCommandList_ClearRenderTargetView(d->list, nat12_image_rtv(rt, 0), rgba, 0, NULL);
	}
	if ((flags & 6) && ds && nat12_image_has_dsv(ds)) {
		nat12_image_transition(d->dev, ds, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		ID3D12GraphicsCommandList_ClearDepthStencilView(d->list, nat12_image_dsv(ds), (D3D12_CLEAR_FLAGS)(((flags & 2) ? D3D12_CLEAR_FLAG_DEPTH : 0) | ((flags & 4) ? D3D12_CLEAR_FLAG_STENCIL : 0)), depth, (UINT8)stencil, 0, NULL);
	}
}

static DXGI_FORMAT decl_format(int type)
{
	switch (type) {
	case 0: return DXGI_FORMAT_R32_FLOAT;
	case 1: return DXGI_FORMAT_R32G32_FLOAT;
	case 2: return DXGI_FORMAT_R32G32B32_FLOAT;
	case 3: return DXGI_FORMAT_R32G32B32A32_FLOAT;
	case 4: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case 5: return DXGI_FORMAT_R8G8B8A8_UINT;
	case 6: return DXGI_FORMAT_R16G16_SINT;
	case 7: return DXGI_FORMAT_R16G16B16A16_SINT;
	case 8: return DXGI_FORMAT_R8G8B8A8_UNORM;
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

static nat_shader *variant(nd *d, nd_shader *s, const nd_decl *decl, unsigned shadow_mask)
{
	const unsigned sig = s->is_vs ? (decl ? decl->sig : 0) : shadow_mask;
	for (int i = 0; i < s->nvar; i++)
		if (s->var[i].sig == sig)
			return s->var[i].s;
	if (s->nvar >= 8 || s->failed)
		return NULL;
	char err[160] = "";
	nat_shader *n = nat_shader_from_sm3(d->dev, s->code, s->dwords, 1 | (s->is_vs ? 0 : (int)((shadow_mask & 0xffffu) << 8)), s->is_vs && decl ? (const void *)decl->ints : NULL, s->is_vs && decl ? decl->nints : 0, err, sizeof(err));
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

static D3D12_BLEND alpha_factor(int b)
{
	switch (b) { // the alpha channel takes no color factors
	case 3: return D3D12_BLEND_SRC_ALPHA;
	case 4: return D3D12_BLEND_INV_SRC_ALPHA;
	case 9: return D3D12_BLEND_DEST_ALPHA;
	case 10: return D3D12_BLEND_INV_DEST_ALPHA;
	}
	return (D3D12_BLEND)b;
}

static void stencil_face(D3D12_DEPTH_STENCILOP_DESC *o, const state11_stencil_t *f)
{
	o->StencilFunc = (D3D12_COMPARISON_FUNC)(f->func >= 1 && f->func <= 8 ? f->func : 8);
	o->StencilPassOp = (D3D12_STENCIL_OP)(f->pass >= 1 && f->pass <= 8 ? f->pass : 1);
	o->StencilFailOp = (D3D12_STENCIL_OP)(f->fail >= 1 && f->fail <= 8 ? f->fail : 1);
	o->StencilDepthFailOp = (D3D12_STENCIL_OP)(f->zfail >= 1 && f->zfail <= 8 ? f->zfail : 1);
}

// the pipeline state for the shaders, the vertex declaration, the pass state and the target formats
static ID3D12PipelineState *get_pso(nd *d, nat_shader *vs, nat_shader *ps, const nd_decl *decl, const state11_t *s, float bias, float slope, DXGI_FORMAT rtvf, DXGI_FORMAT dsvf, unsigned samples)
{
	uint32_t key[KW] = {0};
	uint64_t a = (uintptr_t)vs, b = (uintptr_t)ps, c = (uintptr_t)decl;
	memcpy(key + 0, &a, 8);
	memcpy(key + 2, &b, 8);
	memcpy(key + 4, &c, 8);
	key[6] = (uint32_t)s->src | (uint32_t)s->dst << 8 | (uint32_t)s->op << 16 | (uint32_t)(s->blend_enable != 0) << 24;
	key[7] = (uint32_t)s->src_a | (uint32_t)s->dst_a << 8 | (uint32_t)s->op_a << 16 | (uint32_t)s->write_mask << 24;
	key[8] = (uint32_t)(s->z_enable != 0) | (uint32_t)(s->z_write != 0) << 1 | (uint32_t)s->z_func << 2 | (uint32_t)(s->stencil_enable != 0) << 8 | (uint32_t)(s->stencil_two_sided != 0) << 9;
	key[9] = (uint32_t)(s->front.func | s->front.pass << 4 | s->front.fail << 8 | s->front.zfail << 12);
	key[10] = (uint32_t)(s->back.func | s->back.pass << 4 | s->back.fail << 8 | s->back.zfail << 12);
	key[11] = d->stencil_rmask | d->stencil_wmask << 8;
	key[12] = (uint32_t)s->cull | (uint32_t)s->fill << 4;
	memcpy(&key[13], &bias, 4);
	memcpy(&key[14], &slope, 4);
	key[15] = (uint32_t)rtvf;
	key[16] = (uint32_t)dsvf;
	key[17] = samples;
	int found;
	ID3D12PipelineState *o = kc_get(&d->pso, key, &found);
	if (found)
		return o == (ID3D12PipelineState *)1 ? NULL : o;
	D3D12_INPUT_ELEMENT_DESC ed[32];
	int n = 0;
	for (int i = 0; decl && i < decl->n; i++) {
		const DXGI_FORMAT f = decl_format(decl->el[i].type);
		if (f == DXGI_FORMAT_UNKNOWN || decl->el[i].usage >= 14)
			continue;
		ed[n].SemanticName = SEMANTICS[decl->el[i].usage];
		ed[n].SemanticIndex = decl->el[i].index;
		ed[n].Format = f;
		ed[n].InputSlot = decl->el[i].stream;
		ed[n].AlignedByteOffset = decl->el[i].offset;
		ed[n].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
		ed[n].InstanceDataStepRate = 0;
		n++;
	}
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
	pd.pRootSignature = d->root;
	size_t vsz = 0, psz = 0;
	pd.VS.pShaderBytecode = nat12_shader_blob(vs, &vsz);
	pd.VS.BytecodeLength = vsz;
	if (ps) {
		pd.PS.pShaderBytecode = nat12_shader_blob(ps, &psz);
		pd.PS.BytecodeLength = psz;
	}
	D3D12_RENDER_TARGET_BLEND_DESC *rb = &pd.BlendState.RenderTarget[0];
	rb->BlendEnable = s->blend_enable != 0;
	rb->SrcBlend = (D3D12_BLEND)s->src;
	rb->DestBlend = (D3D12_BLEND)s->dst;
	rb->BlendOp = (D3D12_BLEND_OP)s->op;
	rb->SrcBlendAlpha = alpha_factor(s->src_a);
	rb->DestBlendAlpha = alpha_factor(s->dst_a);
	rb->BlendOpAlpha = (D3D12_BLEND_OP)s->op_a;
	rb->LogicOp = D3D12_LOGIC_OP_NOOP;
	rb->RenderTargetWriteMask = (UINT8)s->write_mask;
	if (!s->blend_enable) {
		rb->SrcBlend = D3D12_BLEND_ONE;
		rb->DestBlend = D3D12_BLEND_ZERO;
		rb->BlendOp = D3D12_BLEND_OP_ADD;
		rb->SrcBlendAlpha = D3D12_BLEND_ONE;
		rb->DestBlendAlpha = D3D12_BLEND_ZERO;
		rb->BlendOpAlpha = D3D12_BLEND_OP_ADD;
	}
	pd.SampleMask = 0xffffffffu;
	pd.RasterizerState.FillMode = s->fill == 2 ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
	pd.RasterizerState.CullMode = s->cull == 1 ? D3D12_CULL_MODE_NONE : s->cull == 2 ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK;
	pd.RasterizerState.DepthBias = (INT)(bias * 16777215.0f);
	pd.RasterizerState.SlopeScaledDepthBias = slope;
	pd.RasterizerState.DepthClipEnable = TRUE;
	pd.DepthStencilState.DepthEnable = s->z_enable != 0;
	pd.DepthStencilState.DepthWriteMask = s->z_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	pd.DepthStencilState.DepthFunc = (D3D12_COMPARISON_FUNC)(s->z_func >= 1 && s->z_func <= 8 ? s->z_func : 8);
	pd.DepthStencilState.StencilEnable = s->stencil_enable != 0;
	pd.DepthStencilState.StencilReadMask = (UINT8)d->stencil_rmask;
	pd.DepthStencilState.StencilWriteMask = (UINT8)d->stencil_wmask;
	stencil_face(&pd.DepthStencilState.FrontFace, &s->front);
	stencil_face(&pd.DepthStencilState.BackFace, s->stencil_two_sided ? &s->back : &s->front);
	pd.InputLayout.pInputElementDescs = ed;
	pd.InputLayout.NumElements = (UINT)n;
	pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pd.NumRenderTargets = rtvf != DXGI_FORMAT_UNKNOWN ? 1 : 0;
	pd.RTVFormats[0] = rtvf;
	pd.DSVFormat = dsvf;
	pd.SampleDesc.Count = samples ? samples : 1;
	if (FAILED(ID3D12Device_CreateGraphicsPipelineState(d->d3d, &pd, &IID_ID3D12PipelineState, (void **)&o))) {
		snprintf(d->err, sizeof(d->err), "pipeline state (%d elements)", n);
		kc_put(&d->pso, key, (void *)1);
		return NULL;
	}
	kc_put(&d->pso, key, o);
	d->n_states++;
	{
		unsigned k = 0;
		while (k < d->ndecls && d->decls_seen[k] != decl)
			k++;
		if (k == d->ndecls && d->ndecls < 64)
			d->decls_seen[d->ndecls++] = decl;
		d->n_layouts = d->ndecls;
	}
	return o;
}

// the CPU sampler descriptor of a sampler word (cached)
static int sampler_cpu(nd *d, uint32_t word, int compare, D3D12_CPU_DESCRIPTOR_HANDLE *out)
{
	uint32_t key[KW] = {0};
	key[0] = word & ~0x4000000u; // the sRGB bit belongs to the view, not the sampler
	key[1] = (uint32_t)compare;
	int found;
	void *o = kc_get(&d->sampler, key, &found);
	if (found) {
		out->ptr = (SIZE_T)o;
		return o != NULL;
	}
	state11_sampler_t sd;
	state11_sampler_decode(word, &sd);
	D3D12_SAMPLER_DESC desc = {0};
	if (sd.min == 3 || sd.mag == 3 || sd.mip == 3)
		desc.Filter = D3D12_FILTER_ANISOTROPIC;
	else
		desc.Filter = (D3D12_FILTER)((sd.min == 2 ? 0x10 : 0) | (sd.mag == 2 ? 4 : 0) | (sd.mip == 2 ? 1 : 0));
	desc.AddressU = (D3D12_TEXTURE_ADDRESS_MODE)(sd.addr_u >= 1 && sd.addr_u <= 5 ? sd.addr_u : 1);
	desc.AddressV = (D3D12_TEXTURE_ADDRESS_MODE)(sd.addr_v >= 1 && sd.addr_v <= 5 ? sd.addr_v : 1);
	desc.AddressW = (D3D12_TEXTURE_ADDRESS_MODE)(sd.addr_w >= 1 && sd.addr_w <= 5 ? sd.addr_w : 1);
	desc.MaxAnisotropy = sd.aniso >= 2 ? (UINT)(sd.aniso > 16 ? 16 : sd.aniso) : 1;
	desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	if (compare) {
		desc.Filter = (D3D12_FILTER)(0x80 | (sd.min == 2 || sd.mag == 2 ? 0x15 : 0));
		desc.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		desc.AddressU = desc.AddressV = desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		desc.MaxAnisotropy = 1;
	}
	desc.MaxLOD = sd.mip == 0 ? 0 : D3D12_FLOAT32_MAX;
	D3D12_CPU_DESCRIPTOR_HANDLE h;
	if (!nat12_sampler_create(d->dev, &desc, &h)) {
		kc_put(&d->sampler, key, NULL);
		return 0;
	}
	kc_put(&d->sampler, key, (void *)h.ptr);
	d->n_states++;
	*out = h;
	return 1;
}

static void default_sampler(nd *d)
{
	if (d->smp_default_ok)
		return;
	D3D12_SAMPLER_DESC desc = {0};
	desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	desc.AddressU = desc.AddressV = desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	desc.MaxAnisotropy = 1;
	desc.MaxLOD = D3D12_FLOAT32_MAX;
	d->smp_default_ok = nat12_sampler_create(d->dev, &desc, &d->smp_default);
}

// constants from the frame's ring (a full ring: finish the frame's commands, then everything must be bound again)
static void *alloc_const(nd *d, unsigned size, D3D12_GPU_VIRTUAL_ADDRESS *va)
{
	void *p = nat12_upload_alloc(d->dev, size, va);
	if (p)
		return p;
	nat12_flush(d->dev);
	check_list(d);
	return nat12_upload_alloc(d->dev, size, va);
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
	unsigned shadow_mask = 0;
	for (int i = 0; i < 16; i++) {
		nat_image *im = g->tex[i] ? d->ops.image(g->tex[i]) : NULL;
		if (im && im != d->rt && im != d->ds && nat_image_is_depth(im))
			shadow_mask |= 1u << i;
	}
	nat_shader *vs = variant(d, vsg, decl, 0), *ps = psg ? variant(d, psg, NULL, shadow_mask) : NULL;
	if (!vs || (psg && !ps)) {
		d->n_refused++;
		return 0;
	}
	if (!d->ib || !d->vb[0]) {
		snprintf(d->err, sizeof(d->err), "%s", !d->ib ? "no index buffer" : "no vertex stream");
		d->n_refused++;
		return 0;
	}
	state11_t st;
	state11_decode(g->word0, g->word1, &st);
	nat_image *rt = rt_image(d), *ds = ds_image(d);
	if ((!rt || !nat12_image_has_rtv(rt)) && (!ds || !nat12_image_has_dsv(ds))) {
		snprintf(d->err, sizeof(d->err), "no render target");
		d->n_refused++;
		return 0;
	}
	check_list(d);
	float bias = 0, slope = 0;
	if (st.polygon_offset && d->ops.poly_offset)
		d->ops.poly_offset(st.polygon_offset, &bias, &slope);
	const DXGI_FORMAT rtvf = rt && nat12_image_has_rtv(rt) ? nat12_image_rtv_format(rt, st.srgb_write) : DXGI_FORMAT_UNKNOWN;
	const DXGI_FORMAT dsvf = ds && nat12_image_has_dsv(ds) ? nat12_image_dsv_format(ds) : DXGI_FORMAT_UNKNOWN;
	unsigned rw, rh, samples = 1;
	if (rt)
		nat_image_size(rt, &rw, &rh, &samples);
	else
		samples = 1;
	ID3D12PipelineState *pso = get_pso(d, vs, ps, decl, &st, bias, slope, rtvf, dsvf, samples);
	if (!pso) {
		d->n_refused++;
		return 0;
	}
	// targets first: an image that is both a target and a source of the draw is not bound as a source (the game unbinds it too)
	set_targets(d, rt, ds, st.srgb_write);
	// images and samplers
	D3D12_CPU_DESCRIPTOR_HANDLE src_h[16], smp_h[16];
	UINT64 key[16];
	default_sampler(d);
	for (int i = 0; i < 16; i++) {
		nat_image *im = g->tex[i] ? d->ops.image(g->tex[i]) : NULL;
		if (im && im != rt && im != ds && nat12_image_has_srv(im)) {
			nat12_image_transition(d->dev, im, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			src_h[i] = nat12_image_srv(im, (g->sampler_word[i] >> 26) & 1);
			if (!sampler_cpu(d, g->sampler_word[i], (shadow_mask >> i) & 1, &smp_h[i]))
				smp_h[i] = d->smp_default;
		} else {
			src_h[i] = nat12_null_srv(d->dev);
			smp_h[i] = d->smp_default;
		}
		key[i] = src_h[i].ptr;
	}
	D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = d->srv_table_last;
	if (!d->srv_table_valid || memcmp(key, d->srv_last, sizeof(key))) {
		D3D12_CPU_DESCRIPTOR_HANDLE cpu;
		if (!nat12_srv_alloc(d->dev, 16, &cpu, &srv_gpu)) {
			nat12_flush(d->dev);
			check_list(d);
			set_targets(d, rt, ds, st.srgb_write);
			nat12_srv_alloc(d->dev, 16, &cpu, &srv_gpu);
		}
		const UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(d->d3d, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		for (int i = 0; i < 16; i++) {
			D3D12_CPU_DESCRIPTOR_HANDLE one = {cpu.ptr + (SIZE_T)i * inc};
			ID3D12Device_CopyDescriptorsSimple(d->d3d, 1, one, src_h[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		}
		memcpy(d->srv_last, key, sizeof(key));
		d->srv_table_last = srv_gpu;
		d->srv_table_valid = 1;
		ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(d->list, RS_SRV, srv_gpu);
	}
	{
		D3D12_GPU_DESCRIPTOR_HANDLE smp_gpu;
		nat12_sampler_table(d->dev, smp_h, &smp_gpu);
		if (smp_gpu.ptr != d->smp_last_key) {
			ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(d->list, RS_SMP, smp_gpu);
			d->smp_last_key = smp_gpu.ptr;
		}
	}
	// constants
	int vs_dirty = 0, ps_dirty = 0;
	for (int i = 0; i < 8; i++) {
		vs_dirty |= g->vs_dirty[i] != 0;
		ps_dirty |= g->ps_dirty[i] != 0;
	}
	if (vs_dirty || !d->cb_vs_va) {
		void *p = alloc_const(d, sizeof(g->vs_c), &d->cb_vs_va);
		if (p)
			memcpy(p, g->vs_c, sizeof(g->vs_c));
		memset(g->vs_dirty, 0, sizeof(g->vs_dirty));
		ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(d->list, RS_VS_B0, d->cb_vs_va);
		d->n_uploads++;
	}
	if (ps_dirty || !d->cb_ps_va) {
		void *p = alloc_const(d, sizeof(g->ps_c), &d->cb_ps_va);
		if (p)
			memcpy(p, g->ps_c, sizeof(g->ps_c));
		memset(g->ps_dirty, 0, sizeof(g->ps_dirty));
		ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(d->list, RS_PS_B3, d->cb_ps_va);
		d->n_uploads++;
	}
	if (!d->cb_ints_va) { // integer / boolean constants: nothing in the pass lists sets them
		void *p = alloc_const(d, 384, &d->cb_ints_va);
		if (p)
			memset(p, 0, 384);
		ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(d->list, RS_VS_B1, d->cb_ints_va);
		ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(d->list, RS_PS_B4, d->cb_ints_va);
	}
	const float at[4] = {(float)st.alpha_func, (float)st.alpha_ref, st.alpha_test_enable ? 1.0f : 0.0f, 0};
	if (!d->cb_alpha_va || memcmp(at, d->alpha_last, sizeof(at))) {
		void *p = alloc_const(d, 16, &d->cb_alpha_va);
		if (p)
			memcpy(p, at, sizeof(at));
		memcpy(d->alpha_last, at, sizeof(at));
		ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(d->list, RS_PS_B5, d->cb_alpha_va);
	}
	// state
	if (pso != d->pso_cur) {
		ID3D12GraphicsCommandList_SetPipelineState(d->list, pso);
		d->pso_cur = pso;
	}
	D3D12_VIEWPORT vp = {d->vp[0], d->vp[1], d->vp[2], d->vp[3], d->vp[4], d->vp[5]};
	ID3D12GraphicsCommandList_RSSetViewports(d->list, 1, &vp);
	unsigned tw, th, ts;
	nat_image *size_img = rt ? rt : ds;
	nat_image_size(size_img, &tw, &th, &ts);
	D3D12_RECT sc = {0, 0, (LONG)tw, (LONG)th};
	if (d->scissor_on) {
		sc.left = d->scissor.left < 0 ? 0 : d->scissor.left;
		sc.top = d->scissor.top < 0 ? 0 : d->scissor.top;
		sc.right = d->scissor.right > (LONG)tw ? (LONG)tw : d->scissor.right;
		sc.bottom = d->scissor.bottom > (LONG)th ? (LONG)th : d->scissor.bottom;
	}
	ID3D12GraphicsCommandList_RSSetScissorRects(d->list, 1, &sc);
	ID3D12GraphicsCommandList_OMSetStencilRef(d->list, d->stencil_ref);
	ID3D12GraphicsCommandList_IASetPrimitiveTopology(d->list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	D3D12_VERTEX_BUFFER_VIEW vbv[4];
	for (int i = 0; i < 4; i++) {
		memset(&vbv[i], 0, sizeof(vbv[i]));
		if (d->vb[i]) {
			const unsigned size = nat12_buffer_size(d->vb[i]);
			if (d->vb_off[i] < size) {
				vbv[i].BufferLocation = nat12_buffer_address(d->vb[i]) + d->vb_off[i];
				vbv[i].SizeInBytes = size - d->vb_off[i];
				vbv[i].StrideInBytes = d->vb_stride[i];
			}
			nat12_buffer_used(d->dev, d->vb[i]);
		}
	}
	ID3D12GraphicsCommandList_IASetVertexBuffers(d->list, 0, 4, vbv);
	D3D12_INDEX_BUFFER_VIEW ibv = {nat12_buffer_address(d->ib), nat12_buffer_size(d->ib), d->ib32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
	ID3D12GraphicsCommandList_IASetIndexBuffer(d->list, &ibv);
	nat12_buffer_used(d->dev, d->ib);
	ID3D12GraphicsCommandList_DrawIndexedInstanced(d->list, prim_count * 3, 1, start_index, base_vertex, 0);
	d->n_draws++;
	return 1;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ StretchRect and the overlay
static const char *BLIT_VS = "cbuffer R : register(b0) { float4 src; float4 dst; }; float4 main(uint id : SV_VertexID, out float2 uv : TEXCOORD0) : SV_Position { float2 c = float2((id << 1) & 2, id & 2); uv = c; "
							 "return float4(c * float2(2, -2) + float2(-1, 1), 0, 1); }";
static const char *BLIT_PS = "Texture2D t : register(t0); SamplerState s : register(s0); cbuffer R : register(b3) { float4 src; float4 dst; }; "
							 "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return t.Sample(s, src.xy + uv * src.zw); }";

static DXGI_FORMAT unorm_of(DXGI_FORMAT f)
{
	return f == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM : f == DXGI_FORMAT_B8G8R8X8_TYPELESS ? DXGI_FORMAT_B8G8R8X8_UNORM : f;
}

static ID3D12PipelineState *blit_pso(nd *d, DXGI_FORMAT rtvf, unsigned samples, int alpha)
{
	uint32_t key[KW] = {0};
	key[0] = 0xb11700;
	key[1] = (uint32_t)rtvf;
	key[2] = samples;
	key[3] = (uint32_t)alpha;
	int found;
	ID3D12PipelineState *o = kc_get(&d->pso, key, &found);
	if (found)
		return o == (ID3D12PipelineState *)1 ? NULL : o;
	if (!d->blit_vs) {
		d->blit_vs = nat_shader_from_hlsl(d->dev, BLIT_VS, 1, d->err, sizeof(d->err));
		d->blit_ps = nat_shader_from_hlsl(d->dev, BLIT_PS, 0, d->err, sizeof(d->err));
	}
	if (!d->blit_vs || !d->blit_ps)
		return NULL;
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
	pd.pRootSignature = d->root;
	size_t sz = 0;
	pd.VS.pShaderBytecode = nat12_shader_blob(d->blit_vs, &sz);
	pd.VS.BytecodeLength = sz;
	pd.PS.pShaderBytecode = nat12_shader_blob(d->blit_ps, &sz);
	pd.PS.BytecodeLength = sz;
	D3D12_RENDER_TARGET_BLEND_DESC *rb = &pd.BlendState.RenderTarget[0];
	rb->RenderTargetWriteMask = 15;
	rb->LogicOp = D3D12_LOGIC_OP_NOOP;
	rb->SrcBlend = alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_ONE;
	rb->DestBlend = alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ZERO;
	rb->BlendOp = D3D12_BLEND_OP_ADD;
	rb->SrcBlendAlpha = D3D12_BLEND_ONE;
	rb->DestBlendAlpha = alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ZERO;
	rb->BlendOpAlpha = D3D12_BLEND_OP_ADD;
	rb->BlendEnable = alpha != 0;
	pd.SampleMask = 0xffffffffu;
	pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pd.RasterizerState.DepthClipEnable = TRUE;
	pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pd.NumRenderTargets = 1;
	pd.RTVFormats[0] = rtvf;
	pd.SampleDesc.Count = samples ? samples : 1;
	if (FAILED(ID3D12Device_CreateGraphicsPipelineState(d->d3d, &pd, &IID_ID3D12PipelineState, (void **)&o))) {
		kc_put(&d->pso, key, (void *)1);
		return NULL;
	}
	kc_put(&d->pso, key, o);
	return o;
}

static void barrier_cur(nd *d, nat_image *i, D3D12_RESOURCE_STATES s) { nat12_image_transition(d->dev, i, s); }

int nd_stretch(nd *d, nat_image *src, const nd_rect *sr, nat_image *dst, const nd_rect *dr, int linear)
{
	check_list(d);
	nat_image *s_img = src ? src : nat12_scene_image(d->dev), *d_img = dst ? dst : nat12_scene_image(d->dev);
	unsigned sw, sh, ss, dw, dh, ds;
	nat_image_size(s_img, &sw, &sh, &ss);
	nat_image_size(d_img, &dw, &dh, &ds);
	const DXGI_FORMAT sfmt = nat12_image_resource_format(s_img), dfmt = nat12_image_resource_format(d_img);
	const nd_rect fs = {0, 0, (int)sw, (int)sh}, fd = {0, 0, (int)dw, (int)dh};
	if (!sr)
		sr = &fs;
	if (!dr)
		dr = &fd;
	const int same_rect = sr->right - sr->left == dr->right - dr->left && sr->bottom - sr->top == dr->bottom - dr->top;
	const int whole = sr->left == 0 && sr->top == 0 && sr->right == (int)sw && sr->bottom == (int)sh && dr->left == 0 && dr->top == 0 && dr->right == (int)dw && dr->bottom == (int)dh;
	if (same_rect && sfmt == dfmt && !d->blit_alpha) {
		if (ss > 1 && ds == 1 && whole) {
			barrier_cur(d, s_img, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
			barrier_cur(d, d_img, D3D12_RESOURCE_STATE_RESOLVE_DEST);
			ID3D12GraphicsCommandList_ResolveSubresource(d->list, nat12_image_res(d_img), 0, nat12_image_res(s_img), 0, unorm_of(sfmt));
			return 1;
		}
		if (ss == ds) {
			barrier_cur(d, s_img, D3D12_RESOURCE_STATE_COPY_SOURCE);
			barrier_cur(d, d_img, D3D12_RESOURCE_STATE_COPY_DEST);
			D3D12_TEXTURE_COPY_LOCATION a = {0}, b = {0};
			a.pResource = nat12_image_res(d_img);
			a.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			b.pResource = nat12_image_res(s_img);
			b.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			D3D12_BOX box = {(UINT)sr->left, (UINT)sr->top, 0, (UINT)sr->right, (UINT)sr->bottom, 1};
			ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &a, (UINT)dr->left, (UINT)dr->top, 0, &b, whole ? NULL : &box);
			return 1;
		}
	}
	// a scaling (or format-converting) draw: the source needs a view of one sample, the destination a render target view
	if (ss > 1) {
		if (s_img != nat12_scene_image(d->dev) || !nat12_scene_resolved_image(d->dev))
			return 0;
		nat_scene_resolve(d->dev);
		s_img = nat12_scene_resolved_image(d->dev);
	}
	if (!nat12_image_has_srv(s_img) || !nat12_image_has_rtv(d_img))
		return 0;
	ID3D12PipelineState *pso = blit_pso(d, nat12_image_rtv_format(d_img, 0), ds, d->blit_alpha);
	if (!pso)
		return 0;
	barrier_cur(d, s_img, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	barrier_cur(d, d_img, D3D12_RESOURCE_STATE_RENDER_TARGET);
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = nat12_image_rtv(d_img, 0);
	ID3D12GraphicsCommandList_OMSetRenderTargets(d->list, 1, &rtv, FALSE, NULL);
	const float c[8] = {(float)sr->left / (float)sw, (float)sr->top / (float)sh, (float)(sr->right - sr->left) / (float)sw, (float)(sr->bottom - sr->top) / (float)sh, 0, 0, 0, 0};
	D3D12_GPU_VIRTUAL_ADDRESS va;
	void *p = alloc_const(d, sizeof(c), &va);
	if (!p)
		return 0;
	memcpy(p, c, sizeof(c));
	ID3D12GraphicsCommandList_SetPipelineState(d->list, pso);
	d->pso_cur = pso;
	ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(d->list, RS_VS_B0, va);
	ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(d->list, RS_PS_B3, va);
	d->cb_vs_va = d->cb_ps_va = 0; // the draw path binds its own constants again
	D3D12_CPU_DESCRIPTOR_HANDLE cpu;
	D3D12_GPU_DESCRIPTOR_HANDLE gpu;
	if (!nat12_srv_alloc(d->dev, 16, &cpu, &gpu))
		return 0;
	const UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(d->d3d, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	for (int i = 0; i < 16; i++) {
		D3D12_CPU_DESCRIPTOR_HANDLE one = {cpu.ptr + (SIZE_T)i * inc};
		ID3D12Device_CopyDescriptorsSimple(d->d3d, 1, one, i == 0 ? nat12_image_srv(s_img, 0) : nat12_null_srv(d->dev), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}
	d->srv_table_valid = 0;
	ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(d->list, RS_SRV, gpu);
	const int lk = linear ? 1 : 0;
	if (!d->blit_smp_ok[lk]) {
		D3D12_SAMPLER_DESC sd = {0};
		sd.Filter = linear ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sd.MaxAnisotropy = 1;
		sd.MaxLOD = D3D12_FLOAT32_MAX;
		d->blit_smp_ok[lk] = nat12_sampler_create(d->dev, &sd, &d->blit_smp[lk]);
		if (!d->blit_smp_ok[lk])
			return 0;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE smp_h[16];
	for (int i = 0; i < 16; i++)
		smp_h[i] = d->blit_smp[lk];
	D3D12_GPU_DESCRIPTOR_HANDLE sg;
	nat12_sampler_table(d->dev, smp_h, &sg);
	ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(d->list, RS_SMP, sg);
	d->smp_last_key = 0;
	D3D12_VIEWPORT vp = {(float)dr->left, (float)dr->top, (float)(dr->right - dr->left), (float)(dr->bottom - dr->top), 0, 1};
	D3D12_RECT sc = {0, 0, (LONG)dw, (LONG)dh};
	ID3D12GraphicsCommandList_RSSetViewports(d->list, 1, &vp);
	ID3D12GraphicsCommandList_RSSetScissorRects(d->list, 1, &sc);
	ID3D12GraphicsCommandList_IASetPrimitiveTopology(d->list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D12GraphicsCommandList_DrawInstanced(d->list, 3, 1, 0, 0);
	return 1;
}

int nd_overlay(nd *d, nat_image *src)
{
	d->blit_alpha = 1;
	const int ok = nd_stretch(d, src, NULL, NULL, NULL, 1);
	d->blit_alpha = 0;
	return ok;
}
#endif
