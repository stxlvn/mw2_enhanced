// The model-lighting upload (0x2a0a0) and the film planes (0x2b7840 / 0x2b7980) of the native back end (see natres.h).
#include <stdlib.h>
#include <string.h>

#include "natres.h"

#define AT(p, off) ((uint8_t *)(p) + (off))
#define RD_PTR(p, off) (*(void **)AT(p, off))
#define RD_U8(p, off) (*(uint8_t *)AT(p, off))
#define RD_U16(p, off) (*(uint16_t *)AT(p, off))
#define RD_I32(p, off) (*(int32_t *)AT(p, off))
#define RD_U32(p, off) (*(uint32_t *)AT(p, off))

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ model lighting
// The game's globals this function works through (rva): the "locked box" {RowPitch, SlicePitch, pBits} at 0x488a370, the byte offsets of the 64 texels of a 4x4x4 block at 0x489a400, the pitches kept
// at 0x481b734 / 0x481b748. The volume is 4 slices deep in memory terms (x = 4 * (code & 63) texels, z = a multiple of 4 slices): the CPU shadow is laid out like a real lock so that the game's own writer
// (0x1fa40) can fill blocks in it.
enum { G_LOCK = 0x488a370, G_BLOCK_OFFS = 0x489a400, G_ROW_PITCH = 0x481b734, G_PITCH_FIX = 0x481b748 };

static uint8_t *light_shadow;
static size_t light_shadow_size;

void ngr_model_lighting(ngr *r, uint8_t *volume_image, const uint8_t *entries, unsigned count)
{
	uint8_t *e = r->exe;
	nat_image *vol = RD_PTR(volume_image, 0);
	if (!count || !vol)
		return;
	const unsigned w = RD_U16(volume_image, 0x18), h = RD_U16(volume_image, 0x1a), d = RD_U16(volume_image, 0x1c) ? RD_U16(volume_image, 0x1c) : 1;
	const unsigned row = w * 4, slice = row * h;
	const size_t total = (size_t)slice * d;
	if (light_shadow_size < total) {
		free(light_shadow);
		light_shadow = calloc(1, total);
		light_shadow_size = total;
	}
	// the "lock": row pitch, slice pitch, pointer
	RD_I32(e, G_LOCK) = (int32_t)row;
	RD_I32(e, G_LOCK + 4) = (int32_t)slice;
	RD_PTR(e, G_LOCK + 8) = light_shadow;
	RD_I32(e, G_ROW_PITCH) = (int32_t)row;
	int32_t *offs = (int32_t *)AT(e, G_BLOCK_OFFS);
	int idx = 0, base = 8;
	for (int g = 0; g < 4; g++) {
		int at = base;
		for (int k = 0; k < 4; k++) {
			offs[idx] = at - 8;
			offs[idx + 1] = at - 4;
			offs[idx + 2] = at;
			offs[idx + 3] = at + 4;
			at += (int)row;
			idx += 4;
		}
		base += (int)slice;
	}
	RD_I32(e, G_PITCH_FIX) = (int32_t)slice - 3 * (int32_t)row;
	for (unsigned i = 0; i < count; i++) {
		const uint8_t *en = entries + (size_t)i * 0x34;
		const unsigned code = RD_U16(en, 0), x = code & 0x3f, zq = ((unsigned)(code >> 4)) & 0xffffffcu;
		uint8_t *at = light_shadow + (size_t)slice * zq + (size_t)x * 0x10;
		if (RD_U8(en, 3) == 0) {
			const uint32_t color = RD_U32(en, 4);
			for (int k = 0; k < 64; k++)
				*(uint32_t *)(at + offs[k]) = color;
		} else if (r->fn_1fa40) {
			r->fn_1fa40(en);
		}
	}
	nat_image_upload(vol, 0, 0, light_shadow, row, slice);
	RD_I32(e, G_LOCK + 8) = 0;
	RD_I32(e, G_LOCK + 12) = 0;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ film planes
typedef struct {
	void *key;
	uint8_t *mem;
	size_t size;
} plane_t;
static plane_t planes[64];

static uint8_t *plane_memory(nat_image *im, unsigned *pitch)
{
	unsigned w, h, s;
	nat_image_size(im, &w, &h, &s);
	*pitch = (w + 3) & ~3u;
	const size_t need = (size_t)*pitch * h;
	for (int i = 0; i < 64; i++) {
		if (planes[i].key == im)
			return planes[i].mem;
		if (!planes[i].key) {
			planes[i].key = im;
			planes[i].mem = calloc(1, need);
			planes[i].size = need;
			return planes[i].mem;
		}
	}
	return NULL;
}

void ngr_film_lock(ngr *r, uint8_t *film)
{
	(void)r;
	const int frames = RD_I32(film, 0x60);
	for (int i = 0; i < frames; i++) {
		for (int pl = 0; pl < 4; pl++) {
			nat_image *im = RD_PTR(film, 0x10 + (size_t)i * 0x30 + (size_t)pl * 8);
			if (!im)
				continue;
			unsigned pitch = 0;
			uint8_t *m = plane_memory(im, &pitch);
			RD_PTR(film, 0x80 + (size_t)i * 0x60 + (size_t)pl * 0x18) = m;
			RD_I32(film, 0x88 + (size_t)i * 0x60 + (size_t)pl * 0x18) = (int32_t)pitch;
		}
	}
}

void ngr_film_unlock(ngr *r, uint8_t *film, void *bink)
{
	const int frames = RD_I32(film, 0x60);
	for (int i = 0; i < frames; i++)
		for (int pl = 0; pl < 4; pl++)
			RD_PTR(film, 0x80 + (size_t)i * 0x60 + (size_t)pl * 0x18) = NULL;
	const int cur = RD_I32(film, 0x74);
	if (cur < 0 || cur >= frames || !r->fn_bink_rects || r->fn_bink_rects(bink) <= 0)
		return;
	// the decoded frame goes to the real planes (Y, Cr, Cb and the alpha plane when there is one): the whole plane, the dirty rectangles are not tracked
	for (int pl = 0; pl < 4; pl++) {
		nat_image *src = RD_PTR(film, 0x10 + (size_t)cur * 0x30 + (size_t)pl * 8), *dst = RD_PTR(film, 0x148 + (size_t)pl * 8);
		if (!src || !dst)
			continue;
		unsigned pitch = 0;
		uint8_t *m = plane_memory(src, &pitch);
		if (m)
			nat_image_upload(dst, 0, 0, m, pitch, 0);
	}
}
