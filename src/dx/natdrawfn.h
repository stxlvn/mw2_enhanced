#pragma once
// The draw-side functions of the game's back end, rewritten for the native renderer (docs/native-dx-plan.md, M3/M4): the ring buffers for dynamic vertices and indices (0x54b30, 0x54820), the stream
// set-up (0x26730), the viewport / scissor and depth-range functions (0x50c60, 0x3cc00), the render-target switch (0x50ec0) and the first draw loops (0x26840, 0x399f0). They keep the game's own
// cache fields (state +0xe8 .. +0x14c, +0xb78 .. +0xb98) so that the code that was not rewritten sees what it expects. All Direct3D work goes through natgfx (the apply half) and natdraw11 (nd).
#include <stdint.h>

#include "natdraw11.h"
#include "natgfx.h"

typedef struct {
	nd *d;
	ng_state *g;
	const ng_ops *ops;
	uint8_t *exe; // the game's image base (rva globals)
	// the game's own functions that do no Direct3D work and stay as they are (rva already added; 0 = not available, the rewrite then skips the step)
	void (*fn_2d_constants)(void *input);        // 0x4ec20
	void (*fn_viewport_rect)(void *input, int *rect4); // 0x50ba0
	void (*fn_projection)(void *input, int *rect4);    // 0x50d70
} ngf;

// the dynamic-ring structs of the game: {int offset, int capacity, buffer pointer} (vertex ring: *(rva 0x41165d8); index rings: rva 0x4116568 and the one *(rva 0x4116580) points to)
typedef struct {
	int32_t offset, capacity;
	void *buffer; // nat_buffer *
} ngf_ring;

// 0x54b30: copies count 32-byte vertices from src into the current vertex ring; returns the byte offset they were written at
int ngf_ring_vertices(ngf *f, const void *src, int count);
// 0x54820: copies tri_count * 3 16-bit indices into the index ring, binds the ring's buffer as the index buffer (state +0xd8 cache); returns the index offset they start at
int ngf_ring_indices(ngf *f, void *state, const void *src, int tri_count);
// 0x26730: stream 0 = (buffer, offset, stride), stream 1 cleared, with the game's cache fields (state +0xe8 stride, +0xf0 buffer, +0xf8 offset, +0x100 / +0x108 / +0x110 stream 1)
void ngf_set_streams(ngf *f, void *state, void *buffer, int offset, int stride);
// 0x50c60: the viewport (x, y, w, h) with the depth range of state +0x148 / +0x14c and the scissor rectangle of +0xb78..+0xb84
void ngf_set_viewport(ngf *f, void *state, const int *vp4);
// 0x3cc00: switches the depth range (0: full, nonzero: the "depth hack" range) and re-applies the viewport and scissor
void ngf_depth_range(ngf *f, void *state, int hack);
// 0x50ec0: render-target set `n` of the table at rva 0x4359000 (32-byte entries: +0 image, +8 colour target, +0x10 depth target, +0x18 / +0x1c size)
void ngf_target_switch(ngf *f, void *input, void *state, int n);
// 0x26840: the draw loop over the passes of the current technique; cmd = {numVerts, primCount, startIndex}
void ngf_draw_26840(ngf *f, void *ctx, const int *cmd);
// 0x399f0: a full-screen-style draw of one material with its own streams
void ngf_draw_399f0(ngf *f, void *ctx, void *material, void *draw_info);
