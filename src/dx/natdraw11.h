#pragma once
// The flush half of the new back end for Direct3D 11 (docs/native-dx-plan.md, M3): turns what natgfx collected (constant register files, shaders, declaration, images, sampler words, render-state
// words) plus the things the draw functions set directly (targets, viewport, scissor, streams, indices) into Direct3D 11 state objects, constant buffers, input layouts and a DrawIndexed. It
// uses nat11 for the device and resources and state11 for the decoding of the words. No Direct3D 9 anywhere.
#include <stddef.h>
#include <stdint.h>

#include "nat11.h"
#include "natgfx.h"

typedef struct nd nd;

// a vertex declaration: the game's D3DVERTEXELEMENT9 list as plain data
typedef struct {
	uint16_t stream, offset;
	uint8_t type, usage, index; // D3DDECLTYPE, D3DDECLUSAGE, usage index
} nd_elem;
typedef struct nd_decl nd_decl;
nd_decl *nd_decl_create(const nd_elem *elems, int count);
void nd_decl_destroy(nd_decl *d);

// a shader object of the game (what the pass's shader field points to): the SM3 bytecode, compiled on first use for the vertex declaration it is drawn with (integer inputs, see sm3_int_input)
typedef struct nd_shader nd_shader;
nd_shader *nd_shader_create(nd *d, const uint32_t *code, size_t dwords);
void nd_shader_destroy(nd_shader *s);

// how natgfx's opaque pointers resolve (the pass fields hold nd_shader / nd_decl pointers in the real back end, the images hold nat_image pointers in their first field)
typedef struct {
	nd_shader *(*shader)(void *game_object);
	nd_decl *(*decl)(void *game_object);
	nat_image *(*image)(void *game_image);
	// polygon offset: mode 1..3 -> Direct3D 9 style depth bias (a fraction of the depth range) and slope scale; the game's 0x511d0
	void (*poly_offset)(int mode, float *bias, float *slope);
} nd_ops;

nd *nd_create(nat_device *dev, const nd_ops *ops);
void nd_destroy(nd *d);

// targets: color / depth images made by nat_image_create (NULL color = the device's scene target, NULL depth = the scene depth, NO_DEPTH below = none)
#define ND_NO_DEPTH ((nat_image *)1)
void nd_set_targets(nd *d, nat_image *color, nat_image *depth);
void nd_set_viewport(nd *d, float x, float y, float w, float h, float min_z, float max_z);
void nd_set_scissor(nd *d, int enable, int left, int top, int right, int bottom);
// the scissor rectangle is always set by the game (0x50c60); whether it is applied is a separate switch (the UI clip commands 0x42fd0 / 0x43100)
void nd_set_scissor_rect(nd *d, int left, int top, int right, int bottom);
void nd_set_scissor_enable(nd *d, int enable);
void nd_set_stencil(nd *d, unsigned ref, unsigned read_mask, unsigned write_mask);
void nd_set_stream(nd *d, unsigned slot, nat_buffer *vb, unsigned offset, unsigned stride);
void nd_set_indices(nd *d, nat_buffer *ib, int is32);
void nd_clear(nd *d, unsigned flags /* 1 color, 2 depth, 4 stencil (Direct3D 9's) */, const float rgba[4], float depth, unsigned stencil);

// Applies the shadow in `g` (constants dirty since the last call are uploaded and their dirty bits cleared) and draws `prim_count` triangles from the bound indices. Returns 0 when the draw cannot be made
// (no shader, no layout, no target): the reason is in nd_last_error.
int nd_draw_indexed(nd *d, ng_state *g, int base_vertex, unsigned start_index, unsigned prim_count);
// StretchRect: copies (or resolves, or scales with a full-screen draw) `src` into `dst`; NULL = the scene target. Rectangles are in pixels, NULL = the whole image. linear selects the filter of a scaling draw.
typedef struct {
	int left, top, right, bottom;
} nd_rect;
// draws `src` (a whole image, premultiplication not assumed: SRCALPHA / INVSRCALPHA) over the whole scene target: the mod's console panel
int nd_overlay(nd *d, nat_image *src);
int nd_stretch(nd *d, nat_image *src, const nd_rect *src_rect, nat_image *dst, const nd_rect *dst_rect, int linear);
const char *nd_last_error(const nd *d);
// statistics: state objects and layouts created, shader variants compiled, constant uploads, draws, draws refused
void nd_stats(const nd *d, unsigned *states, unsigned *layouts, unsigned *shaders, unsigned *uploads, unsigned *draws, unsigned *refused);
