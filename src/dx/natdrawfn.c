// The draw-side functions of the native back end (see natdrawfn.h).
#include "natdrawfn.h"

#include <string.h>

#define AT(p, off) ((uint8_t *)(p) + (off))
#define RD_PTR(p, off) (*(void **)AT(p, off))
#define RD_U8(p, off) (*(uint8_t *)AT(p, off))
#define RD_U16(p, off) (*(uint16_t *)AT(p, off))
#define RD_I32(p, off) (*(int32_t *)AT(p, off))
#define RD_U32(p, off) (*(uint32_t *)AT(p, off))
#define RD_F32(p, off) (*(float *)AT(p, off))

// rva globals of the game
enum {
	G_VERTEX_RING = 0x41165d8,   // pointer to the vertex ring struct
	G_INDEX_RING0 = 0x4116568,   // the first index ring struct
	G_INDEX_RING_CUR = 0x4116580, // pointer to the index ring in use
	G_FRAME_VERTS = 0x435a500,   // the frame's 32-byte vertices
	G_FRAME_VERT_COUNT = 0x4584e4c,
	G_TARGET_TABLE = 0x4359000,
	G_DEPTH_MIN = 0x3859e4,      // a float; the full depth range is [G_DEPTH_MIN, G_DEPTH_MAX]
	G_DEPTH_MAX = 0x385a54,
};

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ rings and streams
int ngf_ring_vertices(ngf *f, const void *src, int count)
{
	ngf_ring *ring = *(ngf_ring **)AT(f->exe, G_VERTEX_RING);
	const int bytes = count * 0x20, old = ring->offset;
	uint8_t *base = nat_buffer_map((nat_buffer *)ring->buffer, old ? NAT_MAP_NOOVERWRITE : NAT_MAP_DISCARD);
	if (base) {
		memcpy(base + old, src, (size_t)bytes);
		nat_buffer_unmap((nat_buffer *)ring->buffer);
	}
	ring->offset += bytes;
	return old;
}

int ngf_ring_indices(ngf *f, void *state, const void *src, int tri_count)
{
	ngf_ring **cur_p = (ngf_ring **)AT(f->exe, G_INDEX_RING_CUR);
	ngf_ring *cur = *cur_p;
	if (cur->capacity < cur->offset + tri_count * 3)
		cur->offset = 0;
	ngf_ring *ring = cur->offset != 0 ? cur : (ngf_ring *)AT(f->exe, G_INDEX_RING0);
	const int old = ring->offset;
	*cur_p = ring;
	uint8_t *base = nat_buffer_map((nat_buffer *)ring->buffer, old ? NAT_MAP_NOOVERWRITE : NAT_MAP_DISCARD);
	if (base) {
		memcpy(base + (size_t)old * 2, src, (size_t)tri_count * 6);
		nat_buffer_unmap((nat_buffer *)ring->buffer);
	}
	if (RD_PTR(state, 0xd8) != ring->buffer) {
		RD_PTR(state, 0xd8) = ring->buffer;
		nd_set_indices(f->d, (nat_buffer *)ring->buffer, 0);
	}
	ring->offset += tri_count * 3;
	return old;
}

void ngf_set_streams(ngf *f, void *state, void *buffer, int offset, int stride)
{
	if (RD_PTR(state, 0xf0) != buffer || RD_I32(state, 0xf8) != offset || RD_I32(state, 0xe8) != stride) {
		RD_PTR(state, 0xf0) = buffer;
		RD_I32(state, 0xf8) = offset;
		RD_I32(state, 0xe8) = stride;
		nd_set_stream(f->d, 0, (nat_buffer *)buffer, (unsigned)offset, (unsigned)stride);
	}
	if (RD_PTR(state, 0x108) != NULL || RD_I32(state, 0x110) != 0 || RD_I32(state, 0x100) != 0) {
		RD_PTR(state, 0x108) = NULL;
		RD_I32(state, 0x110) = 0;
		RD_I32(state, 0x100) = 0;
		nd_set_stream(f->d, 1, NULL, 0, 0);
	}
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ viewport, depth range, targets
static void apply_scissor(ngf *f, void *state)
{
	const int left = RD_I32(state, 0xb78), top = RD_I32(state, 0xb7c);
	nd_set_scissor_rect(f->d, left, top, left + RD_I32(state, 0xb80), top + RD_I32(state, 0xb84));
}

void ngf_set_viewport(ngf *f, void *state, const int *vp4)
{
	if (vp4[0] == RD_I32(state, 0xb88) && vp4[1] == RD_I32(state, 0xb8c) && vp4[2] == RD_I32(state, 0xb90) && vp4[3] == RD_I32(state, 0xb94))
		return;
	memcpy(AT(state, 0xb88), vp4, 16);
	nd_set_viewport(f->d, (float)vp4[0], (float)vp4[1], (float)vp4[2], (float)vp4[3], RD_F32(state, 0x148), RD_F32(state, 0x14c));
	apply_scissor(f, state);
}

void ngf_depth_range(ngf *f, void *state, int hack)
{
	if (hack == RD_I32(state, 0x144))
		return;
	RD_I32(state, 0x144) = hack;
	const float full_min = RD_F32(f->exe, G_DEPTH_MIN), full_max = RD_F32(f->exe, G_DEPTH_MAX);
	const float mn = hack ? 0.0f : full_min, mx = hack ? full_min : full_max;
	RD_F32(state, 0x148) = mn;
	RD_F32(state, 0x14c) = mx;
	nd_set_viewport(f->d, (float)RD_I32(state, 0xb88), (float)RD_I32(state, 0xb8c), (float)RD_I32(state, 0xb90), (float)RD_I32(state, 0xb94), mn, mx);
	apply_scissor(f, state);
}

void ngf_target_switch(ngf *f, void *input, void *state, int n)
{
	if (n == RD_I32(state, 0xb98))
		return;
	if (RD_U32(state, 0xb5c) >> 30 & 1) { // the sRGB write bit is cleared in the requested and the applied word (the next pass-apply sets it again if the pass wants it)
		RD_U32(state, 0xb54) &= 0xbfffffffu;
		RD_U32(state, 0xb5c) &= 0xbfffffffu;
		f->g->word0 &= 0xbfffffffu;
	}
	uint8_t *table = AT(f->exe, G_TARGET_TABLE);
	uint8_t *entry = table + (size_t)n * 32;
	void *image = RD_PTR(entry, 0);
	if (image) { // a texture that becomes the target is unbound from every sampler (Direct3D 11 would drop the binding anyway)
		for (unsigned i = 0; i < 16; i++) {
			if (RD_PTR(state, 0x50 + i * 8) == image)
				RD_PTR(state, 0x50 + i * 8) = NULL;
			if (f->g->tex[i] == image)
				f->g->tex[i] = NULL;
		}
	}
	const int cur = RD_I32(state, 0xb98);
	uint8_t *old = cur >= 0 && cur < 12 ? table + (size_t)cur * 32 : NULL;
	nat_image *color = (nat_image *)RD_PTR(entry, 8), *depth = (nat_image *)RD_PTR(entry, 0x10);
	if (!(old && RD_PTR(old, 8) == RD_PTR(entry, 8))) {
		RD_I32(state, 0xb88) = 0;
		RD_I32(state, 0xb8c) = 0;
		RD_I32(state, 0xb90) = RD_I32(entry, 0x18);
		RD_I32(state, 0xb94) = RD_I32(entry, 0x1c);
		RD_I32(state, 0x144) = -1;
		RD_F32(state, 0x148) = 0.0f;
		RD_F32(state, 0x14c) = 1.0f;
	}
	nd_set_targets(f->d, color, depth);
	RD_I32(state, 0xb98) = n;
	RD_I32(input, 0x16e0) = 0;
	RD_U8(input, 0x1738) = 1;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ draw loops
static void pass_sequence_head(ngf *f, void *ctx, unsigned pass)
{
	void *input = ((void **)ctx)[0], *state = ((void **)ctx)[1];
	ng_select_pass(f->g, f->ops, input, state, pass);
	ng_apply_decl_shader(f->g, f->ops, state);
	uint8_t *p = RD_PTR(state, NG_ST_PASS);
	if (RD_U8(p, 0x1a))
		ng_apply_ps_stable(f->g, f->ops, input, state);
}

void ngf_draw_26840(ngf *f, void *ctx, const int *cmd)
{
	void *input = ((void **)ctx)[0], *state = ((void **)ctx)[1];
	uint8_t *tech = RD_PTR(state, NG_ST_TECH);
	ngf_ring *ring = *(ngf_ring **)AT(f->exe, G_VERTEX_RING);
	const int nverts = RD_I32(f->exe, G_FRAME_VERT_COUNT);
	if (ring->capacity < ring->offset + nverts * 0x20)
		ring->offset = 0;
	const int off = ngf_ring_vertices(f, AT(f->exe, G_FRAME_VERTS), nverts);
	for (unsigned p = 0; p < RD_U16(tech, 10); p++) {
		pass_sequence_head(f, ctx, p);
		ngf_set_streams(f, state, ring->buffer, off, 0x20);
		ng_apply_object(f->g, f->ops, input, state);
		ng_apply_prim(f->g, f->ops, input, state);
		nd_draw_indexed(f->d, f->g, 0, (unsigned)cmd[2], (unsigned)cmd[1]);
	}
}

void ngf_draw_399f0(ngf *f, void *ctx, void *material, void *draw_info)
{
	void *input = ((void **)ctx)[0], *state = ((void **)ctx)[1];
	void *tech = *(void **)AT(*(void **)AT(*(void **)AT(material, 0x58), 0x10), 0x38);
	if (!tech)
		return;
	RD_PTR(state, NG_ST_MATERIAL) = material;
	RD_I32(state, NG_ST_TECH_TYPE) = 4;
	RD_PTR(state, NG_ST_TECH) = tech;
	RD_I32(state, NG_ST_VTYPE) = 0;
	if (f->fn_2d_constants)
		f->fn_2d_constants(input);
	if (RD_U8(input, 0x1738) && f->fn_viewport_rect && f->fn_projection) {
		int rect[4];
		f->fn_viewport_rect(input, rect);
		ngf_set_viewport(f, state, rect);
		f->fn_projection(input, rect);
	}
	pass_sequence_head(f, ctx, 0);
	ngf_set_streams(f, state, RD_PTR(draw_info, 0x18), 0, RD_I32(draw_info, 0x28));
	const int start = ngf_ring_indices(f, state, RD_PTR(draw_info, 8), 2);
	ng_apply_object(f->g, f->ops, input, state);
	ng_apply_prim(f->g, f->ops, input, state);
	nd_draw_indexed(f->d, f->g, 0, (unsigned)start, 2);
}
