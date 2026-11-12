// Shader, declaration, ring and particle-buffer creation of the native back end (see natres.h).
#include <stdlib.h>
#include <string.h>

#include "natres.h"

#define AT(p, off) ((uint8_t *)(p) + (off))
#define RD_PTR(p, off) (*(void **)AT(p, off))
#define RD_U8(p, off) (*(uint8_t *)AT(p, off))
#define RD_U16(p, off) (*(uint16_t *)AT(p, off))
#define RD_U32(p, off) (*(uint32_t *)AT(p, off))
#define RD_F32(p, off) (*(float *)AT(p, off))

enum { G_SHADERS_ON = 0x41161a0, G_SHADER_VERSION = 0x4116198 };

static size_t token_count(const uint32_t *c)
{
	size_t i = 1;
	for (;;) {
		const uint32_t t = c[i];
		if ((t & 0xffff) == 0xfffe) {
			i += 1 + ((t >> 16) & 0x7fff);
			continue;
		}
		if ((t & 0xffff) == 0xffff)
			return i + 1;
		i += 1 + ((t >> 24) & 0xf);
	}
}

void ngr_create_shader(ngr *r, const uint8_t *src, uint8_t *obj)
{
	const uint32_t *code = RD_PTR(src, 0);
	if (!code || !RD_U8(RD_PTR(r->exe, G_SHADERS_ON), 0x10) || (uint32_t)RD_U16(src, 10) != RD_U32(RD_PTR(r->exe, G_SHADER_VERSION), 0x10)) {
		RD_PTR(obj, 8) = NULL;
		return;
	}
	RD_PTR(obj, 8) = nd_shader_create(r->d, code, token_count(code));
}

void ngr_make_decls(ngr *r, uint8_t *set, const uint8_t *elems, int count)
{
	if (!RD_U8(RD_PTR(r->exe, G_SHADERS_ON), 0x10) || count == 0) {
		memset(set + 0x20, 0, 0x80);
		return;
	}
	const uint8_t *t1 = r->exe + 0x36fa90, *t2 = r->exe + 0x36fc40;
	for (unsigned s = 0; s < 16; s++) {
		nd_elem el[32];
		int n = 0, bad = 0;
		for (int i = 0; i < count && n < 31; i++) {
			const uint8_t *e = t1 + ((size_t)elems[i * 2] + (size_t)s * 9) * 3;
			if (e[0] == 0xff) {
				bad = 1;
				break;
			}
			const uint8_t usage_idx = elems[i * 2 + 1];
			nd_elem x;
			x.stream = e[0];
			x.offset = e[1];
			x.type = e[2];
			x.usage = t2[usage_idx * 2];
			x.index = t2[usage_idx * 2 + 1];
			int pos = n; // insertion sort by stream, stable
			while (pos > 0 && x.stream < el[pos - 1].stream) {
				el[pos] = el[pos - 1];
				pos--;
			}
			el[pos] = x;
			n++;
		}
		RD_PTR(set, 0x20 + s * 8) = bad ? NULL : nd_decl_create(el, n);
	}
}

void ngr_ring_init(ngr *r, void *ring, unsigned size)
{
	*(int32_t *)AT(ring, 0) = 0;
	*(int32_t *)AT(ring, 4) = (int32_t)size;
	RD_PTR(ring, 8) = nat_buffer_create(r->dev, size, 0, 1, NULL);
	RD_PTR(ring, 0x10) = NULL;
}

static void index_ring(ngr *r, uint8_t *ring)
{
	*(int32_t *)AT(ring, 0) = 0;
	*(int32_t *)AT(ring, 4) = 0x100000;
	RD_PTR(ring, 8) = nat_buffer_create(r->dev, 0x200000, 1, 1, NULL);
}

void ngr_create_rings(ngr *r)
{
	uint8_t *e = r->exe;
	ngr_ring_init(r, e + 0x41165c0, 0x100000);
	RD_PTR(e, 0x41165d8) = e + 0x41165c0;
	for (uint8_t *b = e + 0x4116590; b != e + 0x41165c0; b += 0x18) { // two vertex rings of 0x480000 bytes (struct = buffer address - 8)
		*(int32_t *)AT(b, -8) = 0;
		*(int32_t *)AT(b, -4) = 0x480000;
		RD_PTR(b, 0) = nat_buffer_create_staged(r->dev, 0x480000, (const volatile int *)AT(b, -8));
		RD_PTR(b, 8) = NULL;
	}
	index_ring(r, e + 0x4116568);
	RD_PTR(e, 0x4116580) = e + 0x4116568;
	for (uint8_t *b = e + 0x4116530; b != e + 0x4116560; b += 0x18)
		index_ring(r, b - 8);
	RD_PTR(e, 0x4116558) = e + 0x4116528;
	RD_PTR(e, 0x4116560) = NULL;
}

ngr_staged *ngr_stage(unsigned size, int is_index)
{
	ngr_staged *s = calloc(1, sizeof(*s));
	s->mem = calloc(1, size);
	s->size = size;
	s->is_index = is_index;
	return s;
}

nat_buffer *ngr_commit(ngr *r, ngr_staged *s)
{
	nat_buffer *b = nat_buffer_create(r->dev, s->size, s->is_index, 0, s->mem);
	free(s->mem);
	free(s);
	return b;
}

void ngr_particle_buffers(ngr *r)
{
	uint8_t *e = r->exe;
	static const short base_idx[18] = {0, 2, 1, 1, 2, 3, 2, 4, 3, 3, 4, 5, 4, 6, 5, 5, 6, 7};
	ngr_staged *vb1 = ngr_stage(0x14000, 0), *ib1 = ngr_stage(0x3000, 1), *ib2 = ngr_stage(0x9000, 1), *vb2 = ngr_stage(0x28000, 0);
	float *cloud = vb1->mem, *spark = vb2->mem;
	short *tri = ib1->mem, *wide = ib2->mem;
	const float k_div = RD_F32(e, 0x385b80), k_one = RD_F32(e, 0x385a54), k_scale_a = RD_F32(e, 0x385a18), k_scale_b = RD_F32(e, 0x385a10);
	for (int a = 0; a < 8; a++) {
		int base = a << 7;
		for (int b = 0; b < 8; b++) {
			for (int c = 0; c < 16; c++) {
				const int r1 = r->fn_rand(), r2 = r->fn_rand(), r3 = r->fn_rand();
				const float x = ((float)r1 / k_div + (float)a) * k_scale_a - k_one;
				const float y = ((float)r2 / k_div + (float)b) * k_scale_a - k_one;
				const float z = ((float)r3 / k_div + (float)c) * k_scale_b - k_one;
				const short id = (short)(c + base);
				const float uv[4][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};
				for (int v = 0; v < 4; v++) {
					cloud[0] = x;
					cloud[1] = y;
					cloud[2] = z;
					cloud[3] = uv[v][0];
					cloud[4] = uv[v][1];
					cloud += 5;
				}
				const short s4 = (short)(id * 4);
				tri[0] = s4;
				tri[1] = (short)(s4 + 1);
				tri[2] = (short)(s4 + 2);
				tri[3] = (short)(s4 + 2);
				tri[4] = (short)(s4 + 1);
				tri[5] = (short)(s4 + 3);
				tri += 6;
				static const float sv[8][2] = {{0, 0}, {1, 0}, {0, 0.3333f}, {1, 0.3333f}, {0, 0.6667f}, {1, 0.6667f}, {0, 1}, {1, 1}};
				for (int v = 0; v < 8; v++) {
					spark[0] = x;
					spark[1] = y;
					spark[2] = z;
					spark[3] = sv[v][0];
					spark[4] = sv[v][1];
					spark += 5;
				}
				for (int k = 0; k < 18; k++)
					*wide++ = (short)(id * 8 + base_idx[k]);
			}
			base += 16;
		}
	}
	if (r->fn_qsort) { // the engine sorts both vertex sets by the comparator at rva 0x25280 (1024 particles of 80 and 160 bytes)
		r->fn_qsort(vb1->mem, 0x400, 0x50, e + 0x25280);
		r->fn_qsort(vb2->mem, 0x400, 0xa0, e + 0x25280);
	}
	RD_PTR(e, 0x4359468) = ngr_commit(r, vb1);
	RD_PTR(e, 0x4359470) = ngr_commit(r, ib1);
	RD_PTR(e, 0x4359480) = ngr_commit(r, ib2);
	RD_PTR(e, 0x4359478) = ngr_commit(r, vb2);
}
