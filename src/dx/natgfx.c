// The apply half of the new back end (see natgfx.h). Pure C, no Direct3D.
#include <math.h>
#include "natgfx.h"

#include <string.h>

#include "state11.h"

#define AT(p, off) ((uint8_t *)(p) + (off))
#define RD_PTR(p, off) (*(void **)AT(p, off))
#define RD_U8(p, off) (*(uint8_t *)AT(p, off))
#define RD_U16(p, off) (*(uint16_t *)AT(p, off))
#define RD_U32(p, off) (*(uint32_t *)AT(p, off))
#define RD_I32(p, off) (*(int32_t *)AT(p, off))

// argument types of a shader argument (16 bytes: +0 type, +2 destination register, +8.. by type)
enum { A_MAT_VC = 0, A_LIT_VC = 1, A_MAT_SAMPLER = 2, A_CODE_VC = 3, A_CODE_SAMPLER = 4, A_CODE_PC = 5, A_MAT_PC = 6, A_LIT_PC = 7 };

void ng_init(ng_state *s)
{
	memset(s, 0, sizeof(*s));
}

float (*ng_weapon_k)(void);
static float wf_orig[16]; // the game's view-projection matrix while the weapon's scaled one is in the registers
static int wf_scaled;

static int wf_perspective(const float *m) // clip w comes from depth: row 3 has no constant w part (the 2D passes in the same depth range are orthographic and stay as they are)
{
	return fabsf(m[15]) < 1e-4f && (fabsf(m[12]) + fabsf(m[13]) + fabsf(m[14])) > 1e-4f;
}

static void wf_mark_dirty(ng_state *s)
{
	s->vs_dirty[0] |= 15;
	s->n_vs_writes++;
}

void ng_weapon_viewport(ng_state *s, float min_z)
{
	if (!ng_weapon_k)
		return;
	const int weapon = min_z > 0.0f;
	if (weapon && !wf_scaled) {
		const float k = ng_weapon_k();
		if (k > 0 && wf_perspective(&s->vs_c[0][0])) {
			memcpy(wf_orig, s->vs_c, sizeof(wf_orig));
			for (int i = 0; i < 8; i++)
				(&s->vs_c[0][0])[i] *= k;
			wf_mark_dirty(s);
			wf_scaled = 1;
		}
	} else if (!weapon && wf_scaled) {
		memcpy(s->vs_c, wf_orig, sizeof(wf_orig));
		wf_mark_dirty(s);
		wf_scaled = 0;
	}
}

static void put_const(ng_state *s, int pixel, unsigned reg, const float *src, unsigned rows)
{
	float(*file)[4] = pixel ? s->ps_c : s->vs_c;
	uint32_t *dirty = pixel ? s->ps_dirty : s->vs_dirty;
	if (!src || reg >= 256)
		return;
	if (reg + rows > 256)
		rows = 256 - reg;
	if (!pixel && wf_scaled) {
		if (reg == 0 && rows >= 4 && !wf_perspective(src)) {
			wf_scaled = 0; // an orthographic matrix: not the weapon any more, left as the game sends it
		} else if (reg < 4) {
			const float k = ng_weapon_k ? ng_weapon_k() : 0;
			if (k > 0) {
				for (unsigned r = reg; r < reg + rows && r < 4; r++)
					memcpy(wf_orig + r * 4, src + (r - reg) * 4, 16); // what the game holds in the register
				memcpy(file[reg], src, (size_t)rows * 16);
				for (unsigned r = reg; r < reg + rows && r < 2; r++)
					for (int j = 0; j < 4; j++)
						file[r][j] *= k;
				for (unsigned r = reg; r < reg + rows; r++)
					dirty[r >> 5] |= 1u << (r & 31);
				s->n_vs_writes++;
				return;
			}
		}
	}
	memcpy(file[reg], src, (size_t)rows * 16);
	for (unsigned r = reg; r < reg + rows; r++)
		dirty[r >> 5] |= 1u << (r & 31);
	if (pixel)
		s->n_ps_writes++;
	else
		s->n_vs_writes++;
}

void ng_bind_sampler(ng_state *s, const ng_ops *ops, unsigned slot, uint8_t sampler_byte, void *image)
{
	if (slot >= 16)
		return;
	s->tex[slot] = image;
	s->sampler_byte[slot] = sampler_byte;
	const int srgb = image ? (RD_U8(image, 0xb) != 0) : 0;
	s->sampler_word[slot] = state11_sampler_word(sampler_byte, srgb, ops->filter_table);
	s->n_samplers++;
}

static const float *code_const(const ng_ops *ops, void *input, unsigned index, unsigned first_row)
{
	if (index < 0x4c)
		return (const float *)AT(input, NG_IN_CODE_CONST + index * 16);
	return ops->derived ? ops->derived(input, index, first_row) : NULL;
}

// one list of arguments, only the types in `mask` (the game's functions each handle a run of types of the sorted list and stop at the first other one)
static void apply_args(ng_state *s, const ng_ops *ops, void *input, void *state, const uint8_t *args, unsigned count, unsigned mask)
{
	void *mat = RD_PTR(state, NG_ST_MATERIAL);
	for (unsigned i = 0; i < count; i++, args += 16) {
		const unsigned type = RD_U16(args, 0), dst = RD_U16(args, 2);
		if (type > 7 || !(mask >> type & 1))
			continue;
		switch (type) {
		case A_MAT_VC:
		case A_MAT_PC: {
			const uint8_t *e = RD_PTR(mat, NG_MAT_CONSTS);
			const int hash = RD_I32(args, 8);
			unsigned guard = 0;
			while (e && RD_I32(e, 0) != hash && guard++ < 4096)
				e += 32;
			if (!e || guard >= 4096) {
				if (ops->warn)
					ops->warn("material constant not found", (unsigned)hash);
				break;
			}
			put_const(s, type == A_MAT_PC, dst, (const float *)(e + 16), 1);
			break;
		}
		case A_LIT_VC:
		case A_LIT_PC:
			put_const(s, type == A_LIT_PC, dst, (const float *)RD_PTR(args, 8), 1);
			break;
		case A_MAT_SAMPLER: {
			const uint8_t *e = RD_PTR(mat, NG_MAT_TEXTURES);
			const int hash = RD_I32(args, 8);
			unsigned guard = 0;
			while (e && RD_I32(e, 0) != hash && guard++ < 4096)
				e += 16;
			if (!e || guard >= 4096) {
				if (ops->warn)
					ops->warn("material sampler not found", (unsigned)hash);
				break;
			}
			void *image = ops->material_image ? ops->material_image(input, e) : RD_PTR(e, 8);
			ng_bind_sampler(s, ops, dst, RD_U8(e, 6), image);
			break;
		}
		case A_CODE_VC:
		case A_CODE_PC: {
			const unsigned index = RD_U16(args, 8), first = RD_U8(args, 10), rows = RD_U8(args, 11);
			put_const(s, type == A_CODE_PC, dst, code_const(ops, input, index, first), rows);
			break;
		}
		case A_CODE_SAMPLER: {
			const unsigned k = RD_U32(args, 8);
			void *image = RD_PTR(input, NG_IN_CODE_SAMPLER + k * 8);
			if (!image && ops->warn)
				ops->warn("code sampler without an image", k);
			ng_bind_sampler(s, ops, dst, RD_U8(input, NG_IN_CODE_SAMPLER_BYTE + k), image);
			break;
		}
		}
	}
}

void ng_select_pass(ng_state *s, const ng_ops *ops, void *input, void *state, unsigned pass_index)
{
	uint8_t *tech = RD_PTR(state, NG_ST_TECH);
	uint8_t *pass = tech + ((size_t)pass_index * 5 + 2) * 8;
	void *mat = RD_PTR(state, NG_ST_MATERIAL);
	// (the game's own functions read these two fields right afterwards, so they are always written)
	RD_PTR(state, NG_ST_PASS) = pass;
	RD_U32(state, NG_ST_PASS_INDEX) = pass_index;
	const uint8_t *tbl = RD_PTR(mat, NG_MAT_STATEBITS);
	const unsigned first = RD_U8(mat, NG_MAT_FIRST_STATE + (unsigned)RD_I32(state, NG_ST_TECH_TYPE)) + pass_index;
	const uint32_t w0 = RD_U32(tbl, first * 8), w1 = RD_U32(tbl, first * 8 + 4);
	if (!s->words_valid || w0 != s->word0 || w1 != s->word1) {
		s->word0 = w0;
		s->word1 = w1;
		s->words_valid = 1;
		s->n_state_changes++;
	}
	if (ops->write_game_state) {
		RD_U32(state, NG_ST_WORD0) = w0;
		RD_U32(state, NG_ST_WORD1) = w1;
	}
	void *ps_field = RD_PTR(pass, 0x10);
	if (s->cur_ps_pass_field != ps_field || !s->ps) {
		s->cur_ps_pass_field = ps_field;
		s->ps = ps_field ? RD_PTR(ps_field, 8) : NULL;
		s->n_shader_changes++;
		if (ops->write_game_state)
			RD_PTR(state, NG_ST_PS_CACHE) = ps_field;
	}
	const unsigned n_prim = RD_U8(pass, 0x18), n_obj = RD_U8(pass, 0x19), n_stable = RD_U8(pass, 0x1a);
	if (n_stable) {
		const uint8_t *args = RD_PTR(pass, 0x20);
		apply_args(s, ops, input, state, args + (size_t)(n_prim + n_obj) * 16, n_stable, 0x1f); // types 0-4
	}
}

void ng_apply_decl_shader(ng_state *s, const ng_ops *ops, void *state)
{
	uint8_t *pass = RD_PTR(state, NG_ST_PASS);
	uint8_t *decl_set = RD_PTR(pass, 0);
	uint8_t *vs_field = RD_PTR(pass, 8);
	const int vtype = RD_I32(state, NG_ST_VTYPE);
	void *decl = decl_set ? RD_PTR(decl_set, 0x30 + (size_t)vtype * 8) : NULL;
	if (s->cur_decl != decl || !s->decl) {
		s->cur_decl = decl;
		s->decl = decl;
		if (ops->write_game_state)
			RD_PTR(state, NG_ST_DECL_CACHE) = decl;
	}
	if (!decl && ops->warn)
		ops->warn("vertex type without a declaration for this shader", (unsigned)vtype);
	if (s->cur_vs_pass_field != vs_field || !s->vs) {
		s->cur_vs_pass_field = vs_field;
		s->vs = vs_field ? RD_PTR(vs_field, 8) : NULL;
		s->n_shader_changes++;
		if (ops->write_game_state)
			RD_PTR(state, NG_ST_VS_CACHE) = vs_field;
	}
}

void ng_apply_ps_stable(ng_state *s, const ng_ops *ops, void *input, void *state)
{
	uint8_t *pass = RD_PTR(state, NG_ST_PASS);
	const unsigned n_prim = RD_U8(pass, 0x18), n_obj = RD_U8(pass, 0x19), n_stable = RD_U8(pass, 0x1a);
	if (n_stable)
		apply_args(s, ops, input, state, (uint8_t *)RD_PTR(pass, 0x20) + (size_t)(n_prim + n_obj) * 16, n_stable, 0xe0); // types 5-7
}

void ng_apply_object(ng_state *s, const ng_ops *ops, void *input, void *state)
{
	uint8_t *pass = RD_PTR(state, NG_ST_PASS);
	const unsigned n_prim = RD_U8(pass, 0x18), n_obj = RD_U8(pass, 0x19);
	if (n_obj)
		apply_args(s, ops, input, state, (uint8_t *)RD_PTR(pass, 0x20) + (size_t)n_prim * 16, n_obj, (1u << A_CODE_VC) | (1u << A_CODE_SAMPLER));
}

void ng_apply_prim(ng_state *s, const ng_ops *ops, void *input, void *state)
{
	uint8_t *pass = RD_PTR(state, NG_ST_PASS);
	const unsigned n_prim = RD_U8(pass, 0x18);
	if (n_prim)
		apply_args(s, ops, input, state, RD_PTR(pass, 0x20), n_prim, 1u << A_CODE_VC);
}

void ng_apply_ps_args(ng_state *s, const ng_ops *ops, void *input, void *state, const void *args, unsigned count)
{
	apply_args(s, ops, input, state, args, count, 0xe0);
}

void ng_apply_object_args(ng_state *s, const ng_ops *ops, void *input, void *state, const void *args, unsigned count)
{
	apply_args(s, ops, input, state, args, count, (1u << A_CODE_VC) | (1u << A_CODE_SAMPLER));
}

void ng_set_sampler_state(ng_state *s, const ng_ops *ops, unsigned slot, uint8_t sampler_byte)
{
	if (slot >= 16)
		return;
	s->sampler_byte[slot] = sampler_byte;
	s->sampler_word[slot] = state11_sampler_word(sampler_byte, 0, ops->filter_table);
	s->n_samplers++;
}

void ng_apply_code_vc_args(ng_state *s, const ng_ops *ops, void *input, void *state, const void *args, unsigned count)
{
	apply_args(s, ops, input, state, args, count, 1u << A_CODE_VC);
}

void ng_apply_vs_stable_args(ng_state *s, const ng_ops *ops, void *input, void *state, const void *args, unsigned count)
{
	apply_args(s, ops, input, state, args, count, 0x1f);
}
