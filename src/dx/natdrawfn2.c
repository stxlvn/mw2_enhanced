// More draw-side functions of the native back end (see natdrawfn.h): the polygon offset, clear, unbind helpers, scissor and four more draw loops.
#include <string.h>

#include "natdrawfn.h"

#define AT(p, off) ((uint8_t *)(p) + (off))
#define RD_PTR(p, off) (*(void **)AT(p, off))
#define RD_U8(p, off) (*(uint8_t *)AT(p, off))
#define RD_U16(p, off) (*(uint16_t *)AT(p, off))
#define RD_I32(p, off) (*(int32_t *)AT(p, off))
#define RD_U32(p, off) (*(uint32_t *)AT(p, off))
#define RD_F32(p, off) (*(float *)AT(p, off))

enum {
	G_DVAR_SLOPE3 = 0x41163d8, G_DVAR_BIAS3 = 0x4116298, G_DVAR_SLOPE = 0x41162d8, G_DVAR_BIAS = 0x4116370, G_BIAS_SCALE = 0x3859c0, G_SLOPE_BIAS_OK = 0x4115b15,
	G_VERSION_XOR = 0x385ce0, G_TEX_CACHE = 0x4356c10, G_IB_A = 0x4359470, G_VB_A = 0x4359468, G_IB_B = 0x4359480, G_VB_B = 0x4359478,
};

void ngf_poly_offset(const uint8_t *exe, int mode, float *bias, float *slope)
{
	float b, sl;
	if (mode == 3) {
		sl = RD_F32(RD_PTR(exe, G_DVAR_SLOPE3), 0x10);
		b = RD_F32(RD_PTR(exe, G_DVAR_BIAS3), 0x10);
	} else {
		const float m = (float)mode;
		sl = m * RD_F32(RD_PTR(exe, G_DVAR_SLOPE), 0x10);
		b = m * RD_F32(RD_PTR(exe, G_DVAR_BIAS), 0x10);
	}
	b *= RD_F32(exe, G_BIAS_SCALE);
	if (RD_U8(exe, G_SLOPE_BIAS_OK) == 0) { // no slope-scaled bias on this device: the game folds it into the constant bias
		b += b;
		sl = 0.0f;
	}
	*bias = b;
	*slope = sl;
}

void ngf_clear(ngf *f, unsigned flags, const float *rgba, float depth, unsigned stencil)
{
	nd_clear(f->d, flags, rgba, depth, stencil);
}

void ngf_unbind_textures(ngf *f, unsigned units)
{
	for (unsigned i = 0; i < units && i < 16; i++) {
		f->g->tex[i] = NULL;
		RD_PTR(f->exe, G_TEX_CACHE + i * 8) = NULL;
	}
}
void ngf_unbind_ps(ngf *f)
{
	f->g->ps = NULL;
	f->g->cur_ps_pass_field = NULL;
}
void ngf_unbind_vs(ngf *f)
{
	f->g->vs = NULL;
	f->g->cur_vs_pass_field = NULL;
}
void ngf_unbind_decl(ngf *f)
{
	f->g->decl = NULL;
	f->g->cur_decl = NULL;
}

void ngf_clear_streams(ngf *f, void *state)
{
	if (RD_PTR(state, 0xf0) != NULL || RD_I32(state, 0xf8) != 0 || RD_I32(state, 0xe8) != 0) {
		RD_PTR(state, 0xf0) = NULL;
		RD_I32(state, 0xf8) = 0;
		RD_I32(state, 0xe8) = 0;
		nd_set_stream(f->d, 0, NULL, 0, 0);
	}
	if (RD_PTR(state, 0x108) != NULL || RD_I32(state, 0x110) != 0 || RD_I32(state, 0x100) != 0) {
		RD_PTR(state, 0x108) = NULL;
		RD_I32(state, 0x110) = 0;
		RD_I32(state, 0x100) = 0;
		nd_set_stream(f->d, 1, NULL, 0, 0);
	}
}

void ngf_set_scissor(ngf *f, void *state, const int *rect4)
{
	nd_set_scissor_rect(f->d, rect4[0], rect4[1], rect4[0] + rect4[2], rect4[1] + rect4[3]);
	memcpy(AT(state, 0xb78), rect4, 16);
}

static void bump_versions(ngf *f, void *input)
{
	if (RD_I32(input, 0x16b8) == 0)
		return;
	RD_U32(input, 0x129c) ^= RD_U32(f->exe, G_VERSION_XOR);
	const unsigned offs[] = {0x1676, 0x1678, 0x1682, 0x1688, 0x168e, 0x15fe};
	for (unsigned i = 0; i < 6; i++)
		RD_U16(input, offs[i]) = (uint16_t)(RD_U16(input, offs[i]) + 1);
	RD_I32(input, 0x16b8) = 0;
}

static void set_index_buffer(ngf *f, void *state, void *ib)
{
	if (RD_PTR(state, 0xd8) != ib) {
		RD_PTR(state, 0xd8) = ib;
		nd_set_indices(f->d, (nat_buffer *)ib, 0);
	}
}

void ngf_draw_3ec20(ngf *f, void *ctx, void *cmd)
{
	void *input = ((void **)ctx)[0], *state = ((void **)ctx)[1];
	const int mode = *(int *)((void **)ctx)[2];
	uint8_t *pass = RD_PTR(state, NG_ST_PASS);
	if (RD_U8(pass, 0x1a))
		ng_apply_ps_stable(f->g, f->ops, input, state);
	if (f->fn_5c8d0)
		f->fn_5c8d0(input);
	uint8_t *tech = RD_PTR(state, NG_ST_TECH);
	int vtype = 2;
	if (RD_U8(tech, 8) & 0x20)
		vtype = RD_U8(RD_PTR(RD_PTR(state, NG_ST_MATERIAL), 0x58), 8) + 2;
	RD_I32(state, NG_ST_VTYPE) = vtype;
	ng_apply_decl_shader(f->g, f->ops, state);
	ngf_depth_range(f, state, (RD_I32(input, 0x16dc) != 0) - 1);
	set_index_buffer(f, state, RD_PTR(RD_PTR(input, 0x13b8), 0x419e0));
	ng_apply_object(f->g, f->ops, input, state);
	ng_apply_prim(f->g, f->ops, input, state);
	if (mode == 9) {
		uintptr_t cursor[2] = {(uintptr_t)RD_PTR(cmd, 0), (uintptr_t)RD_PTR(cmd, 8)};
		ngf_draw_5fe50(f, cursor, ctx);
	} else if (f->fn_60120) {
		f->fn_60120(cmd, ctx, mode);
	}
}

void ngf_draw_5da10(ngf *f, void *ctx, uintptr_t *cursor)
{
	void *input = ((void **)ctx)[0], *state = ((void **)ctx)[1];
	const uint8_t b = *(uint8_t *)(*cursor + 2);
	*cursor += 4;
	uint8_t *obj = (uint8_t *)RD_PTR(input, 0x13b8) + ((size_t)b + 0x383e) * 0x40;
	const int has3 = (RD_U8(obj, 0x38) & 4) != 0;
	if (f->fn_5d650) {
		f->fn_5d650(input, obj, 0);
		if (has3) {
			f->fn_5d650(input, obj + 0x40, 1);
			f->fn_5d650(input, obj + 0x80, 2);
		}
	}
	uint8_t *pass = RD_PTR(state, NG_ST_PASS);
	if (RD_U8(pass, 0x1a))
		ng_apply_ps_stable(f->g, f->ops, input, state);
	bump_versions(f, input);
	RD_I32(state, NG_ST_VTYPE) = 0xe;
	ng_apply_decl_shader(f->g, f->ops, state);
	ngf_depth_range(f, state, (RD_I32(input, 0x16dc) != 0) - 1);
	int verts, prims;
	if (!has3) {
		set_index_buffer(f, state, RD_PTR(f->exe, G_IB_A));
		ngf_set_streams(f, state, RD_PTR(f->exe, G_VB_A), 0, 0x14);
		verts = 4;
		prims = 2;
	} else {
		set_index_buffer(f, state, RD_PTR(f->exe, G_IB_B));
		verts = 8;
		prims = 6;
		ngf_set_streams(f, state, RD_PTR(f->exe, G_VB_B), 0, 0x14);
	}
	if (RD_PTR(input, 0x1698) != obj) {
		RD_PTR(input, 0x1698) = obj;
		if (f->fn_4fbe0)
			f->fn_4fbe0(input, obj, RD_I32(obj, 0x1c), 0);
	}
	if (has3) {
		if (RD_PTR(input, 0x16a0) != obj + 0x40) {
			RD_PTR(input, 0x16a0) = obj + 0x40;
			if (f->fn_4fbe0)
				f->fn_4fbe0(input, obj + 0x40, RD_I32(obj, 0x5c), 1);
		}
		if (RD_PTR(input, 0x16a8) != obj + 0x80) {
			RD_PTR(input, 0x16a8) = obj + 0x80;
			if (f->fn_4fbe0)
				f->fn_4fbe0(input, obj + 0x80, RD_I32(obj, 0x9c), 2);
		}
	}
	ng_apply_object(f->g, f->ops, input, state);
	ng_apply_prim(f->g, f->ops, input, state);
	const int n = 0x400 >> (RD_U32(obj, 0x38) & 3);
	(void)verts;
	nd_draw_indexed(f->d, f->g, 0, 0, (unsigned)(n * prims));
}

void ngf_draw_5dde0(ngf *f, void *ctx, int *cmd)
{
	void *input = ((void **)ctx)[0], *state = ((void **)ctx)[1];
	if (cmd[0] >= 0) {
		uint8_t *desc = *(uint8_t **)(cmd + 8);
		void *ib = NULL;
		int start = 0;
		if (f->fn_efbe0)
			f->fn_efbe0(RD_U8(desc, 6), RD_PTR(desc, 0x10), &ib, &start);
		set_index_buffer(f, state, ib);
		void *ring = RD_PTR(RD_PTR(input, 0x13b8), 0x419c0);
		ngf_set_streams(f, state, RD_PTR(ring, 8), cmd[0], 0x20);
		nd_draw_indexed(f->d, f->g, 0, (unsigned)start, RD_U16(desc, 4));
		return;
	}
	if (f->fn_5dd00)
		f->fn_5dd00(ctx, *(void **)(cmd + 8), *(void **)(cmd + 10));
}

void ngf_draw_63f50(ngf *f, uintptr_t *cursor, unsigned packed, void *ctx)
{
	void *input = ((void **)ctx)[0], *state = ((void **)ctx)[1];
	ng_apply_prim(f->g, f->ops, input, state);
	set_index_buffer(f, state, RD_PTR(RD_PTR(input, 0x13b8), 0x419e0));
	void *vbuf = RD_PTR(f->exe, 0x4116520);
	for (int n = (int)(packed >> 28) + 1; n > 0; n--) {
		const uint16_t key = *(uint16_t *)cursor[1];
		cursor[1] += 2;
		uint8_t *slot = (uint8_t *)cursor[0] + (size_t)(key >> 4) * 4;
		uint8_t *mesh = (uint8_t *)RD_PTR(slot, 0) + (size_t)(key & 0xf) * 0x58;
		const unsigned count = RD_U16(slot, 8), prims_per = RD_U16(mesh, 4), verts_per = RD_U16(mesh, 8);
		const int start_base = RD_I32(slot, 0xc);
		const int voff = (int)(((uint16_t)(RD_U16(slot, 10) - 1) & 0xfffff000u) << 9);
		ngf_set_streams(f, state, vbuf, voff, 0x20);
		nd_draw_indexed(f->d, f->g, 0, (unsigned)(verts_per * count * 3 + start_base), prims_per * count);
	}
}
