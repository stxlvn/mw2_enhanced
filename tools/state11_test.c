// Unit test of src/dx/state11.c: hand-worked state words (the bit layout of docs/native-dx-plan.md) against the Direct3D 9 values they stand for.
//   clang -I src -o state11_test tools/state11_test.c src/dx/state11.c && ./state11_test
#include <stdio.h>
#include "../src/dx/state11.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static int expect(const state11_rs_t *e, int n, int id, uint32_t value)
{
	for (int i = 0; i < n; i++)
		if (e[i].id == id)
			return (e[i].value & e[i].mask) == (value & e[i].mask);
	return 0;
}
static int listed(const state11_rs_t *e, int n, int id)
{
	for (int i = 0; i < n; i++)
		if (e[i].id == id)
			return 1;
	return 0;
}

int main(void)
{
	state11_t s;
	state11_rs_t e[48];
	int n;

	// opaque: blending off, alpha test off (bit 11), back faces culled, colour and alpha written; depth test less-equal, depth write on
	state11_decode(0x18008800, 0x0000000d, &s);
	CHECK(!s.blend_enable && !s.alpha_test_enable && s.cull == 3 && s.write_mask == 15 && s.fill == 3 && !s.srgb_write);
	CHECK(s.z_enable && s.z_write && s.z_func == 4 && !s.stencil_enable);
	n = state11_expected_d3d9(&s, e, 48);
	CHECK(expect(e, n, 27, 0) && expect(e, n, 168, 15) && expect(e, n, 22, 3) && expect(e, n, 7, 1) && expect(e, n, 23, 4) && expect(e, n, 14, 1));
	CHECK(!listed(e, n, 19) && !listed(e, n, 25) && !listed(e, n, 55)); // factors, alpha function, stencil ops keep their old values

	// additive: source alpha, destination one, add; the alpha channel blends the same (no separate alpha bits); depth test off (bit 1), no write
	state11_decode(0x18000925, 0x00000002, &s);
	CHECK(s.blend_enable && s.src == 5 && s.dst == 2 && s.op == 1);
	CHECK(s.src_a == 5 && s.dst_a == 2 && s.op_a == 1);
	CHECK(!s.z_enable && !s.z_write);
	n = state11_expected_d3d9(&s, e, 48);
	CHECK(expect(e, n, 27, 1) && expect(e, n, 19, 5) && expect(e, n, 20, 2) && expect(e, n, 171, 1) && expect(e, n, 207, 5) && expect(e, n, 208, 2) && expect(e, n, 209, 1));
	CHECK(expect(e, n, 7, 0) && !listed(e, n, 23)); // depth function untouched while the test is off

	// separate alpha: colour = src alpha / inv src alpha / add, alpha = one / zero / max
	state11_decode(0x800 | (1u << 8) | 5 | (6u << 4) | (2u << 16) | (1u << 20) | (5u << 24), 0, &s);
	CHECK(s.blend_enable && s.src == 5 && s.dst == 6 && s.op == 1);
	CHECK(s.src_a == 2 && s.dst_a == 1 && s.op_a == 5);

	// alpha test: bits 12-13 = 1 -> greater than 0, 2 -> less than 128, 3 and 0 -> greater or equal 128
	state11_decode(0x1000, 0, &s);
	CHECK(s.alpha_test_enable && s.alpha_func == 5 && s.alpha_ref == 0);
	state11_decode(0x2000, 0, &s);
	CHECK(s.alpha_func == 2 && s.alpha_ref == 0x80);
	state11_decode(0x3000, 0, &s);
	CHECK(s.alpha_func == 7 && s.alpha_ref == 0x80);

	// cull table: 1 none, 2 back (counter-clockwise), 3 front (clockwise); wireframe and sRGB write bits
	state11_decode(0x4000, 0, &s);
	CHECK(s.cull == 1);
	state11_decode(0xc000, 0, &s);
	CHECK(s.cull == 2);
	state11_decode(0x80000000u | 0x40000000u, 0, &s);
	CHECK(s.fill == 2 && s.srgb_write == 1);

	// stencil, one-sided: enable, front pass = replace (2 -> D3DSTENCILOP 3), fail keep, zfail keep, function always (7 -> 8); the back face takes the front face's bits
	state11_decode(0, 0x40 | (2u << 8) | (7u << 17), &s);
	CHECK(s.stencil_enable && !s.stencil_two_sided);
	CHECK(s.front.pass == 3 && s.front.fail == 1 && s.front.zfail == 1 && s.front.func == 8);
	CHECK(s.back.pass == 3 && s.back.fail == 1 && s.back.zfail == 1 && s.back.func == 8);
	n = state11_expected_d3d9(&s, e, 48);
	CHECK(expect(e, n, 52, 1) && expect(e, n, 55, 3) && expect(e, n, 56, 8) && expect(e, n, 188, 3) && expect(e, n, 189, 8));

	// stencil, two-sided: back face has its own bits (pass incr = 6 -> 7, function equal 2 -> 3)
	state11_decode(0, 0x40 | 0x80 | (2u << 8) | (7u << 17) | (6u << 20) | (2u << 29), &s);
	CHECK(s.stencil_two_sided && s.front.pass == 3 && s.back.pass == 7 && s.back.func == 3 && s.back.fail == 1);

	// sampler word: filter bits from the table, wrap (1) or clamp (3) per axis from bits 5-7 of the byte, sRGB from the image
	{
		uint32_t table[32];
		for (int i = 0; i < 32; i++)
			table[i] = (uint32_t)(i & 3) << 8 | (uint32_t)(i & 3) << 12 | (uint32_t)((i >> 2) & 3) << 16;
		CHECK(state11_sampler_word(0x00, 0, table) == (0x1500000u | table[0]));
		CHECK(state11_sampler_word(0x20, 0, table) == (0x1700000u | table[0])); // clamp U: U field 3
		CHECK(state11_sampler_word(0x40, 0, table) == (0x1d00000u | table[0])); // clamp V
		CHECK(state11_sampler_word(0x80, 1, table) == (0x3500000u | 0x4000000u | table[0])); // clamp W, sRGB
		state11_sampler_t t;
		state11_sampler_decode(state11_sampler_word(0xe3, 1, table), &t);
		CHECK(t.addr_u == 3 && t.addr_v == 3 && t.addr_w == 3 && t.srgb == 1 && t.min == 3 && t.mag == 3 && t.mip == 0 && t.aniso == 0);
		n = state11_sampler_expected_d3d9(&t, e, 48);
		CHECK(n == 7 && expect(e, n, 6, 3) && expect(e, n, 1, 3) && expect(e, n, 11, 1));
		t.aniso = 8;
		n = state11_sampler_expected_d3d9(&t, e, 48);
		CHECK(n == 8 && expect(e, n, 10, 8));
	}

	printf(failures ? "%d failures\n" : "all checks passed\n", failures);
	return failures != 0;
}
