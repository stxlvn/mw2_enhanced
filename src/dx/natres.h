#pragma once
// The game's image creation and upload functions, rewritten for the native renderer (docs/native-dx-plan.md, M4): 0x492d0 (with 0x23950, 0x23be0, 0x23a90 and the sizes of 0x48bd0), the
// upload 0x49170 (with 0x48fa0 for volumes and 0x48e80, the row copy), the shared depth buffer 0x391d0 and the ring buffers of 0xf760 / 0x24d00. They fill the game's own image struct
// (+0 the texture field, which holds a nat_image * here, +8 kind, +0x18 / +0x1a / +0x1c size, +0xc / +0xd the picmip shifts, +0x10 / +0x14 the byte sizes) and use nat11 for the resources.
#include <stddef.h>
#include <stdint.h>

#include "nat11.h"

typedef struct {
	nat_device *dev;
	int mip_cube; // DAT_144115b16: cube textures may have mip levels
	// the game's own size bookkeeping (0x48bd0), a pure CPU function: (image, flags, format, w, h, d); NULL = the sizes at +0x10 / +0x14 are left alone
	void (*fn_48bd0)(void *image, unsigned flags, int format, unsigned w, unsigned h, unsigned d);
	// the engine's error report, used when a resource cannot be made ("Create2DTexture( name, ... ) failed")
	void (*error)(const char *what, const char *name, unsigned w, unsigned h, unsigned format);
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
