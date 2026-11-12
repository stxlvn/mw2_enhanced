#pragma once
// The "apply" half of the game's new back end (docs/native-dx-plan.md, M3): what the pass-apply functions of the exe (0x54a10, 0x54940, 0x54070, 0x543b0, 0x53d00, 0x53f10, 0x50a20) do, rewritten
// without any Direct3D call. They read the game's own tables (material, technique, pass, argument lists; layouts in the plan) and write a plain shadow of the device state: the two constant
// register files, the bound shaders / declaration, the image and sampler word of each of the 16 samplers and the two render-state words. A flush step (next slice) turns the shadow into
// Direct3D 11 / 12 objects and calls. This file has no Direct3D header; tools/natgfx_test.c checks it against synthetic game tables.
#include <stddef.h>
#include <stdint.h>

typedef struct ng_ops {
	// the derived constant row(s) of code constant `index` (>= 0x4c): the game's own 0x502f0 (input, index, first_row), returns a pointer to float4s
	const float *(*derived)(void *input, unsigned index, unsigned first_row);
	// the image a material sampler entry (texture table entry: +0 name hash, +6 sampler byte, +7 semantic, +8 image) really binds: the game's rules for the water, default and debug images
	void *(*material_image)(void *input, const uint8_t *entry);
	// something went wrong in the game's data (a code sampler without an image, a vertex type without a declaration); `what` is a short text
	void (*warn)(const char *what, unsigned arg);
	const uint32_t *filter_table; // the 32-entry sampler filter table (rva 0x4585290, filled at run time)
	int write_game_state;         // 1: the real back end (the game's cache fields are kept up to date); 0: a test or the mirror next to the Direct3D 9 path (they are left alone)
} ng_ops;

typedef struct ng_state {
	float vs_c[256][4], ps_c[256][4];
	uint32_t vs_dirty[8], ps_dirty[8]; // one bit per register: written since the last flush
	void *vs, *ps, *decl;              // the Direct3D object fields of the pass's shaders and the declaration for the current vertex type (0 = none)
	void *tex[16];                     // the image bound to each sampler (the game's image pointer)
	uint8_t sampler_byte[16];
	uint32_t sampler_word[16];
	uint32_t word0, word1;             // the render-state words in effect
	int words_valid;
	void *cur_ps_pass_field, *cur_vs_pass_field; // our own copy of the game's "current shader" caches (state +0xb68 / +0xb70)
	void *cur_decl;
	// counters (tests, statistics)
	unsigned n_vs_writes, n_ps_writes, n_samplers, n_state_changes, n_shader_changes;
} ng_state;

// offsets in the game's structures (64-bit exe), all read by the code above
enum {
	NG_ST_VTYPE = 0xe0, NG_ST_MATERIAL = 0x120, NG_ST_TECH_TYPE = 0x128, NG_ST_TECH = 0x130, NG_ST_PASS = 0x138, NG_ST_PASS_INDEX = 0x140, NG_ST_WORD0 = 0xb54, NG_ST_WORD1 = 0xb58,
	NG_ST_PS_CACHE = 0xb68, NG_ST_VS_CACHE = 0xb70, NG_ST_DECL_CACHE = 0x118,
	NG_MAT_TEXTURES = 0x60, NG_MAT_CONSTS = 0x68, NG_MAT_STATEBITS = 0x70, NG_MAT_FIRST_STATE = 0x20,
	NG_IN_CODE_CONST = 0xe00, NG_IN_CODE_SAMPLER = 0x12c0, NG_IN_CODE_SAMPLER_BYTE = 0x1398,
};

void ng_init(ng_state *s);
// 0x54a10: selects pass `pass_index` of the current technique (updates state +0x138 / +0x140), applies the render-state words, the pixel shader and the pass's stable arguments (vertex side
// and samplers; the pixel constants of the stable list are ng_apply_ps_stable's)
void ng_select_pass(ng_state *s, const ng_ops *ops, void *input, void *state, unsigned pass_index);
// 0x54940: the vertex declaration for the current vertex type and the vertex shader of the current pass
void ng_apply_decl_shader(ng_state *s, const ng_ops *ops, void *state);
// 0x54070: the pixel constants of the current pass's stable arguments
void ng_apply_ps_stable(ng_state *s, const ng_ops *ops, void *input, void *state);
// the same three with the list the caller passes (the game's functions are called with a count and a pointer; they equal the pass's own lists on every path read so far)
void ng_apply_ps_args(ng_state *s, const ng_ops *ops, void *input, void *state, const void *args, unsigned count);
void ng_apply_object_args(ng_state *s, const ng_ops *ops, void *input, void *state, const void *args, unsigned count);
// 0x54220: only the code vertex constants (type 3) of a list (the sort-key draws call it with the pass's stable list when the key changed)
void ng_apply_code_vc_args(ng_state *s, const ng_ops *ops, void *input, void *state, const void *args, unsigned count);
// 0x543b0: the vertex-side stable arguments (types 0-4) of a list the caller passes
void ng_apply_vs_stable_args(ng_state *s, const ng_ops *ops, void *input, void *state, const void *args, unsigned count);
// 0x53d00: the per-object arguments of the current pass (code vertex constants and code samplers)
void ng_apply_object(ng_state *s, const ng_ops *ops, void *input, void *state);
// 0x53f10: the per-primitive arguments of the current pass (code vertex constants)
void ng_apply_prim(ng_state *s, const ng_ops *ops, void *input, void *state);
// 0x50b10: sets the sampler byte and word of sampler `slot` without an image (the caller binds the texture itself); forgets the image of the slot
void ng_set_sampler_state(ng_state *s, const ng_ops *ops, unsigned slot, uint8_t sampler_byte);
// 0x50a20: binds image `image` (the game's image pointer, may be 0) to sampler `slot` with the material's sampler byte
void ng_bind_sampler(ng_state *s, const ng_ops *ops, unsigned slot, uint8_t sampler_byte, void *image);
