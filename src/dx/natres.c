// The game's image creation and upload, native (see natres.h).
#include "natres.h"

#include <string.h>

#define AT(p, off) ((uint8_t *)(p) + (off))
#define RD_PTR(p, off) (*(void **)AT(p, off))
#define RD_U8(p, off) (*(uint8_t *)AT(p, off))
#define RD_U16(p, off) (*(uint16_t *)AT(p, off))

// the bytes of one row of `w` pixels (or 4x4 blocks) in the loader's data (0x48e80's formats); 0 for a format the game's copy ignores
static unsigned row_bytes(int fmt, unsigned w)
{
	switch ((unsigned)fmt) {
	case 0x15: case 0x16: case 0x4b: return w * 4;
	case 0x33: return w * 2;
	case 0x1c: case 0x32: return w;
	case 0x31545844: return ((w + 3) >> 2) * 8;
	case 0x33545844: case 0x35545844: return ((w + 3) >> 2) * 16;
	}
	return 0;
}

int ngr_image_create(ngr *r, uint8_t *image, unsigned w, unsigned h, unsigned d, unsigned flags, int format)
{
	*(uint16_t *)AT(image, 0x18) = (uint16_t)w;
	*(uint16_t *)AT(image, 0x1a) = (uint16_t)h;
	*(uint16_t *)AT(image, 0x1c) = (uint16_t)d;
	nat_image_desc desc = {0};
	desc.d3d9_format = (unsigned)format;
	const unsigned dim = flags & 0x30000;
	const unsigned usage_rt = (flags >> 25) & 1;
	const int is_depth_fmt = format == 0x4b || format == 0x4d || format == 0x50 || (unsigned)format == 0x5a544e49u; // D24S8, D24X8, D16, INTZ
	if (usage_rt)
		desc.flags |= is_depth_fmt ? NAT_DEPTH : NAT_RENDER_TARGET;
	if (!usage_rt && (flags >> 15 & 0x200))
		desc.flags |= NAT_DYNAMIC;
	const unsigned no_mips = (flags >> 1) & 1;
	desc.levels = no_mips ? 1 : 0;
	const char *what = "Create2DTexture";
	if (dim == 0x10000) {
		desc.kind = NAT_IMAGE_CUBE;
		desc.w = desc.h = w;
		desc.d = 1;
		if (!r->mip_cube)
			desc.levels = 1;
		RD_U8(image, 8) = NGR_KIND_CUBE;
		*(uint16_t *)AT(image, 0x1a) = (uint16_t)w;
		*(uint16_t *)AT(image, 0x1c) = 1;
		what = "CreateCubeTexture";
	} else if (dim == 0x20000) {
		desc.kind = NAT_IMAGE_VOLUME;
		desc.w = w;
		desc.h = h;
		desc.d = d;
		RD_U8(image, 8) = NGR_KIND_VOLUME;
		what = "Create3DTexture";
	} else if (dim == 0) {
		desc.kind = NAT_IMAGE_2D;
		desc.w = w;
		desc.h = h;
		desc.d = 1;
		*(uint16_t *)AT(image, 0x1c) = 1;
		RD_U8(image, 8) = NGR_KIND_2D;
	} else { // a 1D image: a texture one pixel high
		desc.kind = NAT_IMAGE_2D;
		desc.w = w;
		desc.h = 1;
		desc.d = 1;
		*(uint32_t *)AT(image, 0x1a) = 0x10001u;
		RD_U8(image, 8) = NGR_KIND_1D;
		what = "Create1DTexture";
	}
	nat_image *im = nat_image_create(r->dev, &desc);
	RD_PTR(image, 0) = im;
	if (!im && r->error)
		r->error(what, RD_PTR(image, 0x20), w, h, (unsigned)format);
	if (r->fn_48bd0)
		r->fn_48bd0(image, flags, format, w, h, d);
	return im != NULL;
}

// The game's creators called on their own (0x23950, 0x23a90, 0x23be0: the asset loader 0x23db0 and a function table call them without 0x492d0): the same as the image half of 0x492d0,
// with the header kind they set themselves (3 = 2D, 4 = volume, 5 = cube). `levels` is the Direct3D 9 Levels argument (0 = a full chain).
int ngr_creator_2d(ngr *r, uint8_t *image, unsigned w, unsigned h, unsigned levels, unsigned flags, int format)
{
	*(uint16_t *)AT(image, 0x18) = (uint16_t)w;
	*(uint16_t *)AT(image, 0x1a) = (uint16_t)h;
	*(uint16_t *)AT(image, 0x1c) = 1;
	RD_U8(image, 8) = NGR_KIND_2D;
	nat_image_desc desc = {0};
	desc.kind = NAT_IMAGE_2D;
	desc.d3d9_format = (unsigned)format;
	desc.w = w;
	desc.h = h;
	desc.d = 1;
	desc.levels = levels;
	const int is_depth_fmt = format == 0x4b || format == 0x4d || format == 0x50 || (unsigned)format == 0x5a544e49u;
	if ((flags >> 25) & 1)
		desc.flags |= is_depth_fmt ? NAT_DEPTH : NAT_RENDER_TARGET;
	else if (flags >> 15 & 0x200)
		desc.flags |= NAT_DYNAMIC;
	nat_image *im = nat_image_create(r->dev, &desc);
	RD_PTR(image, 0) = im;
	if (!im && r->error)
		r->error("Create2DTexture", RD_PTR(image, 0x20), w, h, (unsigned)format);
	return im != NULL;
}

int ngr_creator_volume(ngr *r, uint8_t *image, unsigned w, unsigned h, unsigned d, unsigned flags, int format)
{
	*(uint16_t *)AT(image, 0x18) = (uint16_t)w;
	*(uint16_t *)AT(image, 0x1a) = (uint16_t)h;
	*(uint16_t *)AT(image, 0x1c) = (uint16_t)d;
	RD_U8(image, 8) = NGR_KIND_VOLUME;
	nat_image_desc desc = {0};
	desc.kind = NAT_IMAGE_VOLUME;
	desc.d3d9_format = (unsigned)format;
	desc.w = w;
	desc.h = h;
	desc.d = d;
	desc.levels = 0;
	if (!((flags >> 25) & 1) && (flags >> 15 & 0x200))
		desc.flags |= NAT_DYNAMIC;
	nat_image *im = nat_image_create(r->dev, &desc);
	RD_PTR(image, 0) = im;
	if (!im && r->error)
		r->error("Create3DTexture", RD_PTR(image, 0x20), w, h, (unsigned)format);
	return im != NULL;
}

int ngr_creator_cube(ngr *r, uint8_t *image, unsigned w, unsigned nomip, int format)
{
	*(uint16_t *)AT(image, 0x18) = (uint16_t)w;
	*(uint16_t *)AT(image, 0x1a) = (uint16_t)w;
	*(uint16_t *)AT(image, 0x1c) = 1;
	RD_U8(image, 8) = NGR_KIND_CUBE;
	nat_image_desc desc = {0};
	desc.kind = NAT_IMAGE_CUBE;
	desc.d3d9_format = (unsigned)format;
	desc.w = desc.h = w;
	desc.d = 1;
	desc.levels = (!r->mip_cube || nomip) ? 1 : 0;
	nat_image *im = nat_image_create(r->dev, &desc);
	RD_PTR(image, 0) = im;
	if (!im && r->error)
		r->error("CreateCubeTexture", RD_PTR(image, 0x20), w, w, (unsigned)format);
	return im != NULL;
}

void ngr_image_upload(ngr *r, uint8_t *image, int format, unsigned face, unsigned level, const void *data)
{
	nat_image *im = RD_PTR(image, 0);
	const unsigned kind = RD_U8(image, 8);
	if (!im)
		return;
	if (kind == NGR_KIND_CUBE && level != 0 && !r->mip_cube)
		return;
	unsigned w = (unsigned)RD_U16(image, 0x18) >> level, h = (unsigned)RD_U16(image, 0x1a) >> level;
	w = w < 2 ? 1 : w;
	h = h < 2 ? 1 : h;
	if (kind == NGR_KIND_VOLUME) {
		unsigned d = (unsigned)RD_U16(image, 0x1c) >> level;
		d = d < 2 ? 1 : d;
		const unsigned row = row_bytes(format, w);
		// the slice stride the game advances the source by (0x48fa0): 4 bytes a pixel for 0x4b / 0x15, 3 for 0x16 (as the game has it), 1 for 0x1c / 0x32, 2 for 0x33 / 0x50 / 0x72, blocks otherwise
		unsigned slice = 0;
		switch ((unsigned)format) {
		case 0x4b: case 0x15: case 0x72: slice = w * h * 4; break;
		case 0x16: slice = w * h * 3; break;
		case 0x1c: case 0x32: slice = w * h; break;
		case 0x33: case 0x50: slice = w * h * 2; break;
		case 0x31545844: slice = ((w + 3) >> 2) * ((h + 3) >> 2) * 8; break;
		case 0x33545844: case 0x35545844: slice = ((w + 3) >> 2) * ((h + 3) >> 2) * 16; break;
		}
		if (row && slice)
			nat_image_upload(im, 0, level, data, row, slice);
		return;
	}
	const unsigned row = row_bytes(format, w);
	if (!row)
		return; // the game's row copy ignores other formats
	nat_image_upload(im, kind == NGR_KIND_CUBE ? face : 0, level, data, row, 0);
}

void ngr_image_release(uint8_t *image)
{
	nat_image_destroy(RD_PTR(image, 0));
	RD_PTR(image, 0) = NULL;
}

nat_image *ngr_depth_create(ngr *r, unsigned w, unsigned h, int msaa)
{
	nat_image_desc d = {NAT_IMAGE_2D, w, h, 1, 1, 75, NAT_DEPTH, msaa};
	return nat_image_create(r->dev, &d);
}
