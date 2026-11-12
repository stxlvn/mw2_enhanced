#include "state11.h"

static const int DEPTH_FUNC[4] = {8, 2, 3, 4};            // always, less, equal, less or equal (table at rva 0x3708c0)
static const int CULL[4] = {0, 1, 3, 2};                  // unused, none, back (counter-clockwise), front (table at rva 0x370980)

void state11_layer_fix(state11_t *s)
{
	enum { D3DBLEND_ZERO = 1, D3DBLEND_ONE = 2, D3DBLEND_SRCALPHA = 5, D3DBLEND_INVSRCALPHA = 6, D3DBLENDOP_ADD = 1 };
	if (!s->blend_enable || !(s->write_mask & 7))
		return;
	if (s->dst == D3DBLEND_INVSRCALPHA && (s->src == D3DBLEND_SRCALPHA || s->src == D3DBLEND_ONE)) {
		s->src_a = D3DBLEND_ONE;
		s->dst_a = D3DBLEND_INVSRCALPHA;
	} else {
		s->src_a = D3DBLEND_ZERO;
		s->dst_a = D3DBLEND_ONE;
	}
	s->op_a = D3DBLENDOP_ADD;
}

void state11_decode(uint32_t w0, uint32_t w1, state11_t *o)
{
	o->src = (int)(w0 & 0xf);
	o->dst = (int)((w0 >> 4) & 0xf);
	o->op = (int)((w0 >> 8) & 7);
	o->blend_enable = o->op != 0;
	if (((w0 >> 24) & 7) == 0) { // no separate alpha operation: the alpha channel blends as the colour does (0x505a0 copies bits 0-10 to 16-26)
		o->src_a = o->src;
		o->dst_a = o->dst;
		o->op_a = o->op;
	} else {
		o->src_a = (int)((w0 >> 16) & 0xf);
		o->dst_a = (int)((w0 >> 20) & 0xf);
		o->op_a = (int)((w0 >> 24) & 7);
	}
	o->write_mask = (int)(((w0 & 0x8000000) ? 7 : 0) | ((w0 >> 25) & 8));
	o->alpha_test_enable = !((w0 >> 11) & 1);
	switch ((w0 >> 12) & 3) {
	case 1: o->alpha_func = 5; o->alpha_ref = 0; break;    // greater than 0
	case 2: o->alpha_func = 2; o->alpha_ref = 0x80; break; // less than 128
	default: o->alpha_func = 7; o->alpha_ref = 0x80; break; // 0 and 3: greater or equal 128
	}
	o->cull = CULL[(w0 >> 14) & 3];
	o->fill = (w0 & 0x80000000u) ? 2 : 3;
	o->srgb_write = (int)((w0 >> 30) & 1);

	o->z_write = (int)(w1 & 1);
	o->z_enable = !((w1 >> 1) & 1);
	o->z_func = DEPTH_FUNC[(w1 >> 2) & 3];
	o->polygon_offset = (int)((w1 >> 4) & 3);
	o->stencil_enable = (int)((w1 >> 6) & 1);
	o->stencil_two_sided = (int)((w1 >> 7) & 1);
	uint32_t e = w1;
	if (!o->stencil_two_sided) // the back face takes the front face's bits (0x50810)
		e = (w1 & 0xfffff) | ((w1 & 0xfff00) << 12);
	o->front.pass = (int)((e >> 8) & 7) + 1;
	o->front.fail = (int)((e >> 11) & 7) + 1;
	o->front.zfail = (int)((e >> 14) & 7) + 1;
	o->front.func = (int)((e >> 17) & 7) + 1;
	o->back.pass = (int)((e >> 20) & 7) + 1;
	o->back.fail = (int)((e >> 23) & 7) + 1;
	o->back.zfail = (int)((e >> 26) & 7) + 1;
	o->back.func = (int)((e >> 29) & 7) + 1;
}

int state11_expected_d3d9(const state11_t *s, state11_rs_t *out, int max)
{
	int n = 0;
#define ADD(i, v, m) do { if (n < max) { out[n].id = (i); out[n].value = (uint32_t)(v); out[n].mask = (m); n++; } } while (0)
	ADD(27, s->blend_enable, 0xffffffffu);   // ALPHABLENDENABLE
	if (s->blend_enable) {
		ADD(19, s->src, 0xffffffffu);        // SRCBLEND
		ADD(20, s->dst, 0xffffffffu);        // DESTBLEND
		ADD(171, s->op, 0xffffffffu);        // BLENDOP
		ADD(207, s->src_a, 0xffffffffu);     // SRCBLENDALPHA
		ADD(208, s->dst_a, 0xffffffffu);     // DESTBLENDALPHA
		ADD(209, s->op_a, 0xffffffffu);      // BLENDOPALPHA
	}
	ADD(168, s->write_mask, 0xffffffffu);    // COLORWRITEENABLE
	ADD(15, s->alpha_test_enable, 0xffffffffu); // ALPHATESTENABLE
	if (s->alpha_test_enable) {
		ADD(25, s->alpha_func, 0xffffffffu); // ALPHAFUNC
		ADD(24, s->alpha_ref, 0xffu);        // ALPHAREF (the game passes a signed byte)
	}
	ADD(22, s->cull, 0xffffffffu);           // CULLMODE
	ADD(8, s->fill, 0xffffffffu);            // FILLMODE
	ADD(194, s->srgb_write, 0xffffffffu);    // SRGBWRITEENABLE
	ADD(14, s->z_write, 0xffffffffu);        // ZWRITEENABLE
	ADD(7, s->z_enable, 0xffffffffu);        // ZENABLE
	if (s->z_enable)
		ADD(23, s->z_func, 0xffffffffu);     // ZFUNC
	ADD(52, s->stencil_enable, 0xffffffffu); // STENCILENABLE
	if (s->stencil_enable) {
		ADD(55, s->front.pass, 0xffffffffu); // STENCILPASS
		ADD(53, s->front.fail, 0xffffffffu); // STENCILFAIL
		ADD(54, s->front.zfail, 0xffffffffu); // STENCILZFAIL
		ADD(56, s->front.func, 0xffffffffu); // STENCILFUNC
		ADD(188, s->back.pass, 0xffffffffu); // CCW_STENCILPASS
		ADD(186, s->back.fail, 0xffffffffu); // CCW_STENCILFAIL
		ADD(187, s->back.zfail, 0xffffffffu); // CCW_STENCILZFAIL
		ADD(189, s->back.func, 0xffffffffu); // CCW_STENCILFUNC
	}
#undef ADD
	return n;
}

void state11_sampler_decode(uint32_t w, state11_sampler_t *o)
{
	o->aniso = (int)(w & 0xff);
	o->min = (int)((w >> 8) & 0xf);
	o->mag = (int)((w >> 12) & 0xf);
	o->mip = (int)((w >> 16) & 0xf);
	o->addr_u = (int)((w >> 20) & 3) ;
	o->addr_v = (int)((w >> 22) & 3);
	o->addr_w = (int)((w >> 24) & 3);
	o->srgb = (int)((w >> 26) & 1);
}

uint32_t state11_sampler_word(uint8_t p, int srgb_image, const uint32_t table[32])
{
	// ((((p & 0x80 | 0x54) * 2 | p & 0x40) * 2 | p & 0x20) << 16: three 2-bit fields at bits 20, 22, 24 that are 1 (wrap) or 3 (clamp)
	const uint32_t addr = ((((uint32_t)(p & 0x80) | 0x54) * 2 | (uint32_t)(p & 0x40)) * 2 | (uint32_t)(p & 0x20)) << 16;
	return addr | table[p & 0x1f] | (srgb_image ? 0x4000000u : 0);
}

int state11_sampler_expected_d3d9(const state11_sampler_t *s, state11_rs_t *out, int max)
{
	int n = 0;
#define ADD(i, v) do { if (n < max) { out[n].id = (i); out[n].value = (uint32_t)(v); out[n].mask = 0xffffffffu; n++; } } while (0)
	ADD(6, s->min);
	ADD(5, s->mag);
	ADD(7, s->mip);
	ADD(1, s->addr_u);
	ADD(2, s->addr_v);
	ADD(3, s->addr_w);
	ADD(11, s->srgb);
	if (s->aniso >= 2)
		ADD(10, s->aniso);
#undef ADD
	return n;
}
