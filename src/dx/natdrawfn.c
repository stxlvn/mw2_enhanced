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
	if (nat_buffer_is_staged((nat_buffer *)buffer)) // the frame's skinned vertices: what the worker threads have reserved so far goes to the GPU now
		nat_buffer_sync((nat_buffer *)buffer);
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
	const int left = RD_I32(state, 0xb78);
	int top = RD_I32(state, 0xb7c), bottom = top + RD_I32(state, 0xb84);
	if (ng_scissor_hook)
		ng_scissor_hook(&top, &bottom, left, left + RD_I32(state, 0xb80), nd_target_is_full(f->d));
	nd_set_scissor_rect(f->d, left, top, left + RD_I32(state, 0xb80), bottom);
}

void ngf_set_viewport(ngf *f, void *state, const int *vp4)
{
	if (vp4[0] == RD_I32(state, 0xb88) && vp4[1] == RD_I32(state, 0xb8c) && vp4[2] == RD_I32(state, 0xb90) && vp4[3] == RD_I32(state, 0xb94))
		return;
	memcpy(AT(state, 0xb88), vp4, 16);
	nd_set_viewport(f->d, (float)vp4[0], (float)vp4[1], (float)vp4[2], (float)vp4[3], RD_F32(state, 0x148), RD_F32(state, 0x14c));
	ng_weapon_viewport(f->g, RD_F32(state, 0x148));
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
	ng_weapon_viewport(f->g, mn);
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
		// Direct3D 9 resets the device's viewport and scissor rectangle to the new target's size when a target is set (the engine relies on it: its blur and downsample passes never set the viewport)
		nd_set_viewport(f->d, 0.0f, 0.0f, (float)RD_I32(entry, 0x18), (float)RD_I32(entry, 0x1c), 0.0f, 1.0f);
		ng_weapon_viewport(f->g, 0.0f);
		nd_set_scissor_rect(f->d, 0, 0, RD_I32(entry, 0x18), RD_I32(entry, 0x1c));
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

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ more draw loops
void ngf_draw_5cc10(ngf *f, void *ctx, uintptr_t *cursor)
{
	void *input = ((void **)ctx)[0], *state = ((void **)ctx)[1];
	uint8_t *pass = RD_PTR(state, NG_ST_PASS);
	if (RD_U8(pass, 0x1a))
		ng_apply_ps_stable(f->g, f->ops, input, state);
	if (f->fn_5c8d0)
		f->fn_5c8d0(input);
	RD_I32(state, NG_ST_VTYPE) = 0xe;
	ng_apply_decl_shader(f->g, f->ops, state);
	ngf_depth_range(f, state, (RD_I32(input, 0x16dc) != 0) - 1);
	uint8_t *scene = RD_PTR(input, 0x13b8);
	const uint32_t first = *(uint32_t *)*cursor;
	uint32_t key = first;
	void *index_buffer = RD_PTR(f->exe, 0x4359480);
	do {
		const size_t off = (size_t)(key >> 16) * 0x60;
		if (RD_PTR(state, 0xd8) != index_buffer) {
			RD_PTR(state, 0xd8) = index_buffer;
			nd_set_indices(f->d, (nat_buffer *)index_buffer, 0);
		}
		const int stride = RD_I32(scene, 0x1728), voff = stride * RD_I32(scene + off, 0xdf790);
		ngf_set_streams(f, state, RD_PTR(scene, 0x1718), voff, stride);
		if (f->fn_5d650)
			f->fn_5d650(input, scene + 0xdf798 + off, 0);
		pass = RD_PTR(state, NG_ST_PASS);
		ng_apply_object(f->g, f->ops, input, state);
		ng_apply_prim(f->g, f->ops, input, state);
		nd_draw_indexed(f->d, f->g, 0, 0, (unsigned)RD_I32(scene + off, 0xdf780));
		*cursor += 4;
		if (*cursor == cursor[1])
			break;
		key = *(uint32_t *)*cursor;
	} while ((int16_t)key == (int16_t)first);
}

void ngf_set_texture_raw(ngf *f, unsigned slot, void *texture);
static void tex_to_slot(ngf *f, unsigned slot, void *texture)
{
	memset(f->tex_thunk[slot], 0, 32);
	*(void **)f->tex_thunk[slot] = texture;
	f->g->tex[slot] = texture ? f->tex_thunk[slot] : NULL;
}

void ngf_set_texture_raw(ngf *f, unsigned slot, void *texture)
{
	if (slot < 16)
		tex_to_slot(f, slot, texture);
}

void ngf_draw_5fe50(ngf *f, uintptr_t *cursor, void *ctx)
{
	void *state = ((void **)ctx)[1];
	uint8_t *cmd = (uint8_t *)cursor[0];
	uint8_t *pass = RD_PTR(state, NG_ST_PASS);
	const uint8_t flags = RD_U8(pass, 0x1b);
	if (flags & 1)
		ng_set_sampler_state(f->g, f->ops, 1, 0x72);
	if (flags & 2)
		ng_set_sampler_state(f->g, f->ops, 2, 0x62);
	if (flags & 4)
		ng_set_sampler_state(f->g, f->ops, 3, 0x62);
	void *cache1 = RD_PTR(state, 0x58), *cache3 = RD_PTR(state, 0x68), *cache2 = RD_PTR(state, 0x60);
	uint8_t *globals = RD_PTR(f->exe, 0x410fd08);
	uint8_t *arr1 = RD_PTR(globals, 0x18), *arr3 = RD_PTR(globals, 0x38), *arr2 = RD_PTR(globals, 0x30);
	const int key = RD_I32(cmd, 0);
	unsigned prev_b5 = 0xff;
	uint32_t prev_b4 = 0xffffffffu;
	int last_voff = -1;
	do {
		const unsigned b5 = RD_U8(cmd, 5), b4 = RD_U8(cmd, 4);
		if (prev_b5 != b5 || prev_b4 != b4) {
			prev_b4 = b4;
			if (!(flags & 1)) {
				cache1 = NULL;
			} else {
				uint8_t *e = arr1 + (size_t)b5 * 8;
				if (cache1 != e) {
					tex_to_slot(f, 1, *(void **)e);
					cache1 = e;
				}
			}
			prev_b5 = b5;
			if (!(flags & 4)) {
				cache2 = NULL;
				cache3 = NULL;
			} else {
				uint8_t *e3 = arr3 + (size_t)b4 * 8;
				if (cache3 != e3) {
					tex_to_slot(f, 3, *(void **)e3);
					cache3 = e3;
				}
				if (!(flags & 2)) {
					cache2 = NULL;
				} else {
					uint8_t *e2 = arr2 + (size_t)b4 * 8;
					if (cache2 != e2) {
						tex_to_slot(f, 2, *(void **)e2);
						cache2 = e2;
					}
				}
			}
		}
		const int voff = RD_I32(cmd, 0xc);
		if (last_voff != voff) {
			ngf_set_streams(f, state, RD_PTR(cmd, 0x10), voff * 0x2c, 0x2c);
			last_voff = voff;
		}
		nd_draw_indexed(f->d, f->g, 0, (unsigned)RD_I32(cmd, 8), RD_U16(cmd, 6));
		cmd += 0x20;
		cursor[0] = (uintptr_t)cmd;
		if (cmd == (uint8_t *)cursor[1])
			break;
	} while (key == RD_I32(cmd, 0));
	RD_PTR(state, 0x58) = cache1;
	RD_PTR(state, 0x68) = cache3;
	RD_PTR(state, 0x60) = cache2;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ sprites and particles
enum { G_VERSION_XOR = 0x385ce0, G_PARTICLE_TABLE = 0x4115920, G_FLOAT_A = 0x385a98, G_FLOAT_B = 0x3859d8, G_FLOAT_C = 0x488a2c0, G_COLOR_SCALE = 0x3569e0 };

// the block these functions start with: when the "mode" flag of the input is set, the versions of the constants that depend on it are bumped (the game re-sends them) and the flag cleared
static void bump_mode_versions(ngf *f, void *input)
{
	if (RD_I32(input, 0x16b8) == 0)
		return;
	RD_U32(input, 0x129c) ^= RD_U32(f->exe, G_VERSION_XOR);
	const unsigned offs[] = {0x1676, 0x1678, 0x1682, 0x1688, 0x168e, 0x15fe};
	for (unsigned i = 0; i < 6; i++)
		RD_U16(input, offs[i]) = (uint16_t)(RD_U16(input, offs[i]) + 1);
	RD_I32(input, 0x16b8) = 0;
}

void ngf_draw_63850(ngf *f, const uint8_t *item, void *ctx)
{
	void *input = ((void **)ctx)[0], *state = ((void **)ctx)[1];
	const uint8_t *desc = RD_PTR(item, 0x28);
	const unsigned num_verts = RD_U16(desc, 2), prims = RD_U16(desc, 4);
	const uint8_t kind = RD_U8(desc, 6);
	void *ib = NULL, *vb = NULL;
	int start = 0, voff = 0;
	if (f->fn_efbe0)
		f->fn_efbe0(kind, RD_PTR(desc, 0x10), &ib, &start);
	if (RD_PTR(state, 0xd8) != ib) {
		RD_PTR(state, 0xd8) = ib;
		nd_set_indices(f->d, (nat_buffer *)ib, 0);
	}
	if (f->fn_efdd0)
		f->fn_efdd0(kind, RD_PTR(desc, 0x28), &vb, &voff);
	ngf_set_streams(f, state, vb, voff, 0x20);
	const float fa = RD_F32(f->exe, G_FLOAT_A), fb = RD_F32(f->exe, G_FLOAT_B), fc = RD_F32(f->exe, G_FLOAT_C);
	const unsigned count = RD_U32(item, 0x30);
	const uint16_t *list = RD_PTR(item, 0x38);
	uint8_t *table = RD_PTR(RD_PTR(f->exe, G_PARTICLE_TABLE), 0x328);
	for (unsigned i = 0; i < count; i++) {
		uint8_t *p = table + (size_t)list[i] * 0x58;
		if (f->fn_637a0)
			f->fn_637a0(p, input);
		if (RD_U16(p, 0x42) != 1) {
			const uint32_t u = (uint32_t)RD_U16(p, 0x42) - 1;
			RD_F32(input, 0x11a4) = ((float)(u >> 4 & 0xffffffc) + fa) * fc;
			RD_U32(input, 0x11a8) = 0x3f000000;
			RD_U32(input, 0x11ac) = 0x3f800000;
			RD_F32(input, 0x11a0) = ((float)((u & 0x3f) << 2) + fa) * fb;
			RD_U16(input, 0x15e0) = (uint16_t)(RD_U16(input, 0x15e0) + 1);
		}
		const float c0 = (float)RD_U8(p, 0x48) * RD_F32(f->exe, G_COLOR_SCALE), c1 = (float)RD_U8(p, 0x49) * RD_F32(f->exe, G_COLOR_SCALE + 4);
		const float c2 = (float)RD_U8(p, 0x4a) * RD_F32(f->exe, G_COLOR_SCALE + 8), c3 = (float)RD_U8(p, 0x4b) * RD_F32(f->exe, G_COLOR_SCALE + 12);
		RD_F32(input, 0x11b0) = c0 * c0;
		RD_F32(input, 0x11b8) = c2 * c2;
		RD_F32(input, 0x11b4) = c1 * c1;
		RD_F32(input, 0x11bc) = c3;
		RD_U16(input, 0x15e2) = (uint16_t)(RD_U16(input, 0x15e2) + 1);
		ng_apply_prim(f->g, f->ops, input, state);
		nd_draw_indexed(f->d, f->g, 0, (unsigned)start, prims);
		(void)num_verts;
	}
}

void ngf_draw_5f220(ngf *f, void *ctx, uintptr_t *cursor)
{
	void *input = ((void **)ctx)[0], *state = ((void **)ctx)[1];
	const int mode = *(int *)((void **)ctx)[2];
	uint8_t *pass = RD_PTR(state, NG_ST_PASS);
	if (RD_U8(pass, 0x1a))
		ng_apply_ps_stable(f->g, f->ops, input, state);
	RD_PTR(input, 0x1698) = (void *)3;
	bump_mode_versions(f, input);
	RD_I32(state, NG_ST_VTYPE) = 1;
	ng_apply_decl_shader(f->g, f->ops, state);
	ngf_depth_range(f, state, (RD_I32(input, 0x16dc) != 0) - 1);
	pass = RD_PTR(state, NG_ST_PASS);
	if (RD_U8(pass, 0x19))
		ng_apply_object(f->g, f->ops, input, state);
	uint8_t *base = (uint8_t *)cursor[0];
	uint16_t *cur = (uint16_t *)cursor[1];
	if (mode == 9) {
		const uint16_t head0 = cur[0], head1 = cur[1];
		cur += 2;
		cursor[1] = (uintptr_t)cur;
		if (RD_U8(pass, 0x1b) & 1) {
			uint8_t *globals = RD_PTR(f->exe, 0x410fd08);
			void *image = *(void **)((uint8_t *)RD_PTR(globals, 8) + (size_t)(head0 & 0xff) * 8);
			ng_bind_sampler(f->g, f->ops, 1, 0x72, image);
		}
		for (int n = (head1 >> 12) + 1; n > 0; n--) {
			const uint16_t v = *(uint16_t *)cursor[1];
			cursor[1] += 2;
			uint8_t *slot = base + (size_t)(v >> 4) * 4;
			uint8_t item[0x40] = {0};
			RD_PTR(item, 0x28) = (uint8_t *)RD_PTR(slot, 0) + (size_t)(v & 0xf) * 0x58;
			RD_U32(item, 0x30) = RD_U16(slot, 8);
			RD_PTR(item, 0x38) = slot + 10;
			ngf_draw_63850(f, item, ctx);
		}
	} else {
		const int n0 = (cur[1] >> 12) + 1;
		cursor[1] += 4;
		for (int n = n0; n > 0; n--) {
			const uint16_t v = *(uint16_t *)cursor[1];
			cursor[1] += 2;
			const size_t m = (size_t)(v & 0xf) * 0x58;
			uint8_t *slot = base + (size_t)(v >> 4) * 4;
			uint8_t *mesh = RD_PTR(slot, 0);
			const unsigned count = RD_U16(slot, 8);
			const unsigned num_verts = RD_U16(mesh + m, 2), prims = RD_U16(mesh + m, 4);
			void *ib = NULL, *vb = NULL;
			int start = 0, voff = 0;
			if (f->fn_efbe0)
				f->fn_efbe0(RD_U8(mesh + m, 6), RD_PTR(mesh + m, 0x10), &ib, &start);
			if (RD_PTR(state, 0xd8) != ib) {
				RD_PTR(state, 0xd8) = ib;
				nd_set_indices(f->d, (nat_buffer *)ib, 0);
			}
			if (f->fn_efdd0)
				f->fn_efdd0(RD_U8(mesh + m, 6), RD_PTR(mesh + m, 0x28), &vb, &voff);
			ngf_set_streams(f, state, vb, voff, 0x20);
			uint8_t *table = RD_PTR(RD_PTR(f->exe, G_PARTICLE_TABLE), 0x328);
			for (unsigned j = 0; j < count; j++) {
				if (f->fn_637a0)
					f->fn_637a0(table + (size_t)RD_U16(slot, 10 + j * 2) * 0x58, input);
				ng_apply_prim(f->g, f->ops, input, state);
				nd_draw_indexed(f->d, f->g, 0, (unsigned)start, prims);
			}
			(void)num_verts;
		}
	}
}

void ngf_draw_5c960(ngf *f, void *ctx, uintptr_t *cursor)
{
	void *input = ((void **)ctx)[0], *state = ((void **)ctx)[1];
	const int which = RD_I32(((void **)ctx)[2], 0x24);
	uint8_t *pass = RD_PTR(state, NG_ST_PASS);
	if (RD_U8(pass, 0x1a))
		ng_apply_ps_stable(f->g, f->ops, input, state);
	uint8_t *cmd = (uint8_t *)cursor[0];
	const int key = RD_I32(cmd, 0);
	if (f->fn_5c8d0)
		f->fn_5c8d0(input);
	RD_I32(state, NG_ST_VTYPE) = 1;
	ng_apply_decl_shader(f->g, f->ops, state);
	ngf_depth_range(f, state, (RD_I32(input, 0x16dc) != 0) - 1);
	uint8_t *scene = RD_PTR(input, 0x13b8);
	ngf_set_streams(f, state, RD_PTR(scene, 0x1658 + (size_t)which * 0x30), 0, RD_I32(scene, 0x1668 + (size_t)which * 0x30));
	const void *indices = NULL;
	int tris = 0;
	do {
		unsigned n_consts = RD_U16(cmd, 0x12);
		if (n_consts != 0 || (const uint8_t *)indices + (size_t)tris * 6 != (const uint8_t *)RD_PTR(cmd, 8)) {
			if (tris != 0) {
				const int start = ngf_ring_indices(f, state, indices, tris);
				nd_draw_indexed(f->d, f->g, 0, (unsigned)start, (unsigned)tris);
				n_consts = RD_U16(cmd, 0x12);
				tris = 0;
			}
			indices = RD_PTR(cmd, 8);
			const unsigned first = RD_U16(cmd, 0x10);
			if (n_consts != 0) {
				for (unsigned r = 0x4a, k = 0; k < n_consts; k++, r++) {
					const uint8_t *src = scene + 0x62980 + (size_t)(r + (which != 0 ? 0u - 0x4au : 0xb6u) + first) * 0x10;
					memcpy(AT(input, (r + 0xe0) * 0x10), src, 16);
					RD_U16(input, 0x156c + r * 2) = (uint16_t)(RD_U16(input, 0x156c + r * 2) + 1);
				}
			}
			ng_apply_object(f->g, f->ops, input, state);
			ng_apply_prim(f->g, f->ops, input, state);
		}
		tris += RD_I32(cmd, 4);
		cmd += 0x18;
		cursor[0] = (uintptr_t)cmd;
	} while (cmd != (uint8_t *)cursor[1] && RD_I32(cmd, 0) == key);
	if (tris != 0) {
		const int start = ngf_ring_indices(f, state, indices, tris);
		nd_draw_indexed(f->d, f->g, 0, (unsigned)start, (unsigned)tris);
	}
}

int ngf_warmup_28ce0(const uint8_t *technique_set, int which)
{
	if (which == 0)
		return (RD_PTR(technique_set, 0x30) != NULL) + (RD_PTR(technique_set, 0x38) != NULL) + (RD_PTR(technique_set, 0x40) != NULL) + (RD_PTR(technique_set, 0xa8) != NULL);
	return RD_PTR(technique_set, 0x30 + (size_t)which * 8) != NULL;
}
