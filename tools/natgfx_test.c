// Test of src/dx/natgfx.c against synthetic game tables (material, technique, pass, argument lists, input, state), built with the layouts of docs/native-dx-plan.md.
//   natgfx_test   (exit code 0 = all checks passed)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/dx/natgfx.h"
#include "../src/dx/state11.h"

static int fails;
#define CHECK(c, ...) \
	do { \
		if (!(c)) { \
			fails++; \
			printf("FAIL line %d: ", __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} while (0)

#define W16(p, off, v) (*(uint16_t *)((uint8_t *)(p) + (off)) = (uint16_t)(v))
#define W8(p, off, v) (*(uint8_t *)((uint8_t *)(p) + (off)) = (uint8_t)(v))
#define W32(p, off, v) (*(uint32_t *)((uint8_t *)(p) + (off)) = (uint32_t)(v))
#define WP(p, off, v) (*(void **)((uint8_t *)(p) + (off)) = (void *)(v))

static float derived_rows[4][4];
static unsigned derived_calls;
static const float *derived(void *input, unsigned index, unsigned first_row)
{
	(void)input;
	derived_calls++;
	derived_rows[0][0] = (float)index;
	derived_rows[0][1] = (float)first_row;
	return &derived_rows[0][0];
}
static unsigned warnings;
static void warn(const char *what, unsigned arg)
{
	(void)what;
	(void)arg;
	warnings++;
}

static void arg(uint8_t *a, unsigned type, unsigned dst)
{
	memset(a, 0, 16);
	W16(a, 0, type);
	W16(a, 2, dst);
}

int main(void)
{
	uint32_t table[32];
	for (int i = 0; i < 32; i++)
		table[i] = 0x2200u | (uint32_t)i << 0; // filters: min 2 (bits 8-11), others 0 here; only used to see that the table is read
	ng_ops ops = {derived, NULL, warn, table, 1};
	ng_state ng;
	ng_init(&ng);

	uint8_t *input = calloc(1, 0x2000), *state = calloc(1, 0x1000);
	// code constants 0..: float4 i = {i, i+0.5, 0, 1}
	for (int i = 0; i < 0x4c; i++) {
		float *c = (float *)(input + NG_IN_CODE_CONST + i * 16);
		c[0] = (float)i;
		c[1] = i + 0.5f;
		c[2] = 0;
		c[3] = 1;
	}
	static int dummy_image_a[8], dummy_image_b[8];
	WP(input, NG_IN_CODE_SAMPLER + 3 * 8, dummy_image_a);
	W8(input, NG_IN_CODE_SAMPLER_BYTE + 3, 0x21);
	W8((uint8_t *)dummy_image_a, 0xb, 1); // sRGB image

	// material: a constant table (two entries, hashes 0x1111, 0x2222), a texture table (hash 0x3333), state bits table
	uint8_t *mat = calloc(1, 0x200), *consts = calloc(4, 32), *textures = calloc(4, 16), *statebits = calloc(8, 8);
	W32(consts, 0, 0x1111);
	((float *)(consts + 16))[0] = 11.0f;
	W32(consts, 32, 0x2222);
	((float *)(consts + 32 + 16))[0] = 22.0f;
	((float *)(consts + 32 + 16))[3] = 2.0f;
	W32(textures, 0, 0x3333);
	W8(textures, 6, 0x05);
	WP(textures, 8, dummy_image_b);
	WP(mat, NG_MAT_CONSTS, consts);
	WP(mat, NG_MAT_TEXTURES, textures);
	WP(mat, NG_MAT_STATEBITS, statebits);
	W8(mat, NG_MAT_FIRST_STATE + 4, 2); // technique type 4: first state pair 2
	W32(statebits, 2 * 8, 0x11111111);
	W32(statebits, 2 * 8 + 4, 0x22222222);
	W32(statebits, 3 * 8, 0x33333333);
	W32(statebits, 3 * 8 + 4, 0x44444444);

	// technique with two passes (pass at tech + (index * 5 + 2) * 8)
	uint8_t *tech = calloc(1, 0x200);
	uint8_t *vs_obj = calloc(1, 0x20), *ps_obj = calloc(1, 0x20), *decl_set = calloc(1, 0x80);
	static int vs_d3d, ps_d3d, decl_d3d;
	WP(vs_obj, 8, &vs_d3d);
	WP(ps_obj, 8, &ps_d3d);
	WP(decl_set, 0x30 + 2 * 8, &decl_d3d); // vertex type 2 has a declaration
	uint8_t *args = calloc(16, 16);
	uint8_t *pass0 = tech + 2 * 8;
	WP(pass0, 0, decl_set);
	WP(pass0, 8, vs_obj);
	WP(pass0, 0x10, ps_obj);
	W8(pass0, 0x18, 1); // per-primitive: 1 (code vertex constant)
	W8(pass0, 0x19, 2); // per-object: 2 (code vertex constant + code sampler)
	W8(pass0, 0x1a, 7); // stable: 7
	WP(pass0, 0x20, args);
	// per-primitive: code vertex constant 5, 2 rows, into register 10
	arg(args + 0, 3, 10);
	W16(args + 0, 8, 5);
	W8(args + 0, 10, 0);
	W8(args + 0, 11, 2);
	// per-object: derived constant 0x50 (first row 1) into register 20, 4 rows; code sampler 3 into sampler 7
	arg(args + 16, 3, 20);
	W16(args + 16, 8, 0x50);
	W8(args + 16, 10, 1);
	W8(args + 16, 11, 4);
	arg(args + 32, 4, 7);
	W32(args + 32, 8, 3);
	// stable: material vc 0x2222 -> vs c4; literal vc -> vs c5; material sampler 0x3333 -> sampler 1; code pc 7 -> ps c2; material pc 0x1111 -> ps c3; literal pc -> ps c6; code sampler 9 (no image) -> sampler 2
	static const float lit_v[4] = {7, 8, 9, 10}, lit_p[4] = {70, 80, 90, 100};
	arg(args + 48, 0, 4);
	W32(args + 48, 8, 0x2222);
	arg(args + 64, 1, 5);
	WP(args + 64, 8, lit_v);
	arg(args + 80, 2, 1);
	W32(args + 80, 8, 0x3333);
	arg(args + 96, 5, 2);
	W16(args + 96, 8, 7);
	W8(args + 96, 10, 0);
	W8(args + 96, 11, 1);
	arg(args + 112, 6, 3);
	W32(args + 112, 8, 0x1111);
	arg(args + 128, 7, 6);
	WP(args + 128, 8, lit_p);
	arg(args + 144, 4, 2);
	W32(args + 144, 8, 9);

	WP(state, NG_ST_MATERIAL, mat);
	W32(state, NG_ST_TECH_TYPE, 4);
	WP(state, NG_ST_TECH, tech);
	W32(state, NG_ST_VTYPE, 2);

	ng_select_pass(&ng, &ops, input, state, 0);
	CHECK(*(void **)(state + NG_ST_PASS) == pass0 && *(uint32_t *)(state + NG_ST_PASS_INDEX) == 0, "pass fields");
	CHECK(ng.word0 == 0x11111111u && ng.word1 == 0x22222222u && ng.words_valid, "state words %08x %08x", ng.word0, ng.word1);
	CHECK(*(uint32_t *)(state + NG_ST_WORD0) == 0x11111111u && *(uint32_t *)(state + NG_ST_WORD1) == 0x22222222u, "game cache of the words");
	CHECK(ng.ps == &ps_d3d && *(void **)(state + NG_ST_PS_CACHE) == ps_obj, "pixel shader");
	CHECK(ng.vs_c[4][0] == 22.0f && ng.vs_c[4][3] == 2.0f, "material vertex constant %g", ng.vs_c[4][0]);
	CHECK(ng.vs_c[5][0] == 7 && ng.vs_c[5][3] == 10, "literal vertex constant");
	CHECK(ng.tex[1] == dummy_image_b && ng.sampler_byte[1] == 5, "material sampler");
	CHECK(ng.tex[2] == NULL && warnings == 1, "code sampler without an image (warnings %u)", warnings);
	CHECK(ng.ps_c[2][0] == 0 && ng.ps_c[3][0] == 0, "pixel constants wait for ng_apply_ps_stable");
	CHECK((ng.vs_dirty[0] & (1u << 4)) && (ng.vs_dirty[0] & (1u << 5)) && !(ng.vs_dirty[0] & (1u << 6)), "dirty bits");

	ng_apply_decl_shader(&ng, &ops, state);
	CHECK(ng.decl == &decl_d3d && ng.vs == &vs_d3d, "declaration and vertex shader");
	CHECK(*(void **)(state + NG_ST_DECL_CACHE) == &decl_d3d && *(void **)(state + NG_ST_VS_CACHE) == vs_obj, "game caches of decl / vs");

	ng_apply_ps_stable(&ng, &ops, input, state);
	CHECK(ng.ps_c[2][0] == 7.0f && ng.ps_c[2][1] == 7.5f, "code pixel constant");
	CHECK(ng.ps_c[3][0] == 11.0f, "material pixel constant");
	CHECK(ng.ps_c[6][0] == 70 && ng.ps_c[6][3] == 100, "literal pixel constant");

	ng_apply_object(&ng, &ops, input, state);
	CHECK(derived_calls == 1 && ng.vs_c[20][0] == (float)0x50 && ng.vs_c[20][1] == 1.0f, "derived constant (calls %u)", derived_calls);
	CHECK(ng.tex[7] == dummy_image_a && ng.sampler_byte[7] == 0x21, "code sampler");
	state11_sampler_t sd;
	state11_sampler_decode(ng.sampler_word[7], &sd);
	CHECK(sd.srgb == 1, "sRGB flag of the image reaches the sampler word");

	ng_apply_prim(&ng, &ops, input, state);
	CHECK(ng.vs_c[10][0] == 5.0f && ng.vs_c[11][0] == 6.0f && ng.vs_c[11][1] == 6.5f, "per-primitive constants (2 rows)");

	// the same pass again changes nothing in the words; pass 1 has its own words and no pixel shader
	const unsigned changes = ng.n_state_changes;
	ng_select_pass(&ng, &ops, input, state, 0);
	CHECK(ng.n_state_changes == changes, "same words counted as a change");
	ng_select_pass(&ng, &ops, input, state, 1);
	CHECK(ng.word0 == 0x33333333u && ng.word1 == 0x44444444u && ng.n_state_changes == changes + 1, "second pass words");
	CHECK(ng.ps == NULL, "pass 1 has no pixel shader");

	// a game that mirrors another path must not have its caches touched
	ops.write_game_state = 0;
	ng_state ng2;
	ng_init(&ng2);
	*(void **)(state + NG_ST_PS_CACHE) = NULL;
	ng_select_pass(&ng2, &ops, input, state, 0);
	CHECK(*(void **)(state + NG_ST_PS_CACHE) == NULL && ng2.ps == &ps_d3d, "write_game_state = 0 leaves the game's cache alone");

	printf(fails ? "%d checks FAILED\n" : "all checks passed\n", fails);
	return fails ? 1 : 0;
}
