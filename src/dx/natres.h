#pragma once
// The game's image creation and upload functions, rewritten for the native renderer (docs/native-dx-plan.md, M4): 0x492d0 (with 0x23950, 0x23be0, 0x23a90 and the sizes of 0x48bd0), the
// upload 0x49170 (with 0x48fa0 for volumes and 0x48e80, the row copy), the shared depth buffer 0x391d0 and the ring buffers of 0xf760 / 0x24d00. They fill the game's own image struct
// (+0 the texture field, which holds a nat_image * here, +8 kind, +0x18 / +0x1a / +0x1c size, +0xc / +0xd the picmip shifts, +0x10 / +0x14 the byte sizes) and use nat11 for the resources.
#include <stddef.h>
#include <stdint.h>

#include "nat11.h"
#include "natdraw11.h"

typedef struct {
	nat_device *dev;
	int mip_cube; // DAT_144115b16: cube textures may have mip levels
	// the game's own size bookkeeping (0x48bd0), a pure CPU function: (image, flags, format, w, h, d); NULL = the sizes at +0x10 / +0x14 are left alone
	void (*fn_48bd0)(void *image, unsigned flags, int format, unsigned w, unsigned h, unsigned d);
	// the engine's error report, used when a resource cannot be made ("Create2DTexture( name, ... ) failed")
	void (*error)(const char *what, const char *name, unsigned w, unsigned h, unsigned format);
	// for the shader, declaration, ring and particle-buffer creators
	nd *d;
	uint8_t *exe;                                             // the game's image base (tables and globals by rva)
	int (*fn_rand)(void);                                     // 0x2f715c: the engine's random number
	void (*fn_qsort)(void *base, size_t num, size_t size, void *cmp); // 0x2f6ab0: the engine's qsort (the particle buffer set-up sorts with the comparator at rva 0x25280)
} ngr;

// image kinds (+8)
enum { NGR_KIND_1D = 2, NGR_KIND_2D = 3, NGR_KIND_VOLUME = 4, NGR_KIND_CUBE = 5 };
// flags bits of 0x492d0: 1 no picmip, 2 no mip chain, 15 dynamic, 16 / 17 (0x30000: 0 2D, 0x10000 cube, 0x20000 volume), 25 render target or depth, 26 system memory
// 0x492d0: creates the native image for `image` (w, h, d, flags, d3d format) and fills the struct; returns 1, or 0 (and the error report) when the resource could not be made
int ngr_image_create(ngr *r, uint8_t *image, unsigned w, unsigned h, unsigned d, unsigned flags, int format);
// 0x49170 / 0x48fa0: uploads one level (and face) from `data`, laid out as the game's loader has it (rows of w pixels or 4x4 blocks, the formats of 0x48e80); `format` is the d3d format number
void ngr_image_upload(ngr *r, uint8_t *image, int format, unsigned face, unsigned level, const void *data);
// releases the native image of the struct
void ngr_image_release(uint8_t *image);
// 0x391d0: a depth-stencil image the size of the render target (multisampled like the scene when msaa > 1)
nat_image *ngr_depth_create(ngr *r, unsigned w, unsigned h, int msaa);

// 0x8f60 / 0x9070: the shader object `obj` (its +8 field is the Direct3D shader in the original, an nd_shader here) from the game's shader source `src` (+0 the bytecode pointer, +10 a version stamp
// that must equal the dvar value at rva 0x4116198 + 0x10; the game's "shaders enabled" dvar is at 0x41161a0 + 0x10). The field is set to NULL when the shader is not made.
void ngr_create_shader(ngr *r, const uint8_t *src, uint8_t *obj);
// 0x258a0: the 16 vertex declarations of a vertex format given as (element, usage) byte pairs: `elems` / `count`; they are stored (nd_decl pointers) at +0x20 .. +0x9f of `set`
void ngr_make_decls(ngr *r, uint8_t *set, const uint8_t *elems, int count);
// 0x24f00: a dynamic vertex ring {offset, capacity, buffer} of `size` bytes; the struct is the game's (offset at +0, capacity at +4, nat_buffer at +8)
void ngr_ring_init(ngr *r, void *ring, unsigned size);
// 0x24fa0: all of the game's dynamic rings (1 MB vertex ring, two 4.5 MB vertex rings, three 2 MB index rings) with their bookkeeping globals
void ngr_create_rings(ngr *r);
// 0x24d00 / 0x24df0 + the Unlock calls of 0x252e0: a static buffer is written through a CPU block and made when it is "unlocked"; staged = ngr_stage_*, then the filled block, then ngr_commit
typedef struct {
	void *mem;
	unsigned size;
	int is_index;
} ngr_staged;
ngr_staged *ngr_stage(unsigned size, int is_index);
nat_buffer *ngr_commit(ngr *r, ngr_staged *s); // creates the buffer from the block and frees the stage
// 0x252e0: the particle cloud vertex / index buffers, filled with the engine's random numbers; the four buffers are stored at rva 0x4359468, 0x4359470, 0x4359478, 0x4359480
void ngr_particle_buffers(ngr *r);
