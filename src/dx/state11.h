#pragma once
// The renderer's pass state words (docs/native-dx-plan.md, "The render-state words") decoded into plain data: what blend, depth, stencil and rasterizer state a pass asks for.
// No Direct3D header is needed here; src/dx/stateprobe.c checks the decoding against what the game really sets on its Direct3D 9 device, and the Direct3D 11 back end will build its state
// objects from this struct. The numbers inside are Direct3D 9's (D3DBLEND, D3DCMP, D3DSTENCILOP ... have the same values as the D3D11 enums except where noted in the plan).
#include <stdint.h>

typedef struct {
	int func, pass, fail, zfail; // D3DCMP (1 never ... 8 always), D3DSTENCILOP (1 keep ... 8 decr)
} state11_stencil_t;

typedef struct {
	// word 0
	int blend_enable;
	int src, dst, op;         // D3DBLEND, D3DBLENDOP
	int src_a, dst_a, op_a;   // the alpha channel's, equal to the colour's when the word has none
	int write_mask;           // bit 0 R, 1 G, 2 B, 3 A
	int alpha_test_enable;
	int alpha_func, alpha_ref; // D3DCMP, 0..255
	int cull;                 // D3DCULL: 1 none, 2 clockwise, 3 counter-clockwise
	int fill;                 // D3DFILL: 2 wireframe, 3 solid
	int srgb_write;
	// word 1
	int z_write, z_enable;
	int z_func;               // D3DCMP
	int polygon_offset;       // 0 none, 1, 2, 3 (a pair of dvars each; the numbers are not decoded here)
	int stencil_enable, stencil_two_sided;
	state11_stencil_t front, back;
} state11_t;

// word0 / word1: the two 32-bit words of a pass's state-bits pair (the game keeps the requested ones at state + 0xb54 / + 0xb58)
void state11_decode(uint32_t word0, uint32_t word1, state11_t *out);

// The split screen's 2D layers (src/native.c, mode 10) are drawn into a render target cleared to transparent and laid over the half's picture afterwards: the layer's alpha must hold the coverage.
// "Over" blends accumulate it (alpha: one, one minus source alpha), additive glows and multiplies leave it alone (alpha: zero, one); a draw that blends nothing or writes no colour keeps its own.
void state11_layer_fix(state11_t *s);

// What Direct3D 9 state the game's own code leaves set after applying these words: only the states that the words decide (a state whose switch is off, such as the blend factors with
// blending off, keeps its earlier value and is not listed). `mask` is the part of the value to compare. Returns the count (at most max).
typedef struct {
	int id;           // D3DRENDERSTATETYPE
	uint32_t value;
	uint32_t mask;
} state11_rs_t;
int state11_expected_d3d9(const state11_t *s, state11_rs_t *out, int max);

// The sampler word (docs/native-dx-plan.md, "Sampler word"): one 32-bit word per sampler, built by the game from the material's sampler byte and the image's sRGB flag.
typedef struct {
	int aniso;                  // 0..: Direct3D 9 MAXANISOTROPY, only used when 2 or more
	int min, mag, mip;          // D3DTEXTUREFILTERTYPE: 0 none, 1 point, 2 linear, 3 anisotropic
	int addr_u, addr_v, addr_w; // D3DTADDRESS: 1 wrap, 2 mirror, 3 clamp, 4 border
	int srgb;
} state11_sampler_t;
void state11_sampler_decode(uint32_t word, state11_sampler_t *out);
// The word the game derives from the sampler byte (bits 0-4 the filter index into the 32-entry table at rva 0x4585290, filled at run time; bits 5, 6, 7 clamp U, V, W instead of wrap) and the image's
// sRGB flag. `table` is that table.
uint32_t state11_sampler_word(uint8_t byte, int srgb_image, const uint32_t table[32]);
// The Direct3D 9 sampler states (D3DSAMP_*) the word decides; id = the D3DSAMP number, `mask` as above. MAXANISOTROPY is listed only when the word's level is 2 or more.
int state11_sampler_expected_d3d9(const state11_sampler_t *s, state11_rs_t *out, int max);
