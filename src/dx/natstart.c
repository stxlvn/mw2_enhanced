#include "natstart.h"

#include <math.h>
#include <string.h>

#define AT(p, off) ((uint8_t *)(p) + (off))
#define W32(p, off, v) (*(uint32_t *)AT(p, off) = (uint32_t)(v))
#define W8(p, off, v) (*(uint8_t *)AT(p, off) = (uint8_t)(v))
#define RF(p, off) (*(float *)AT(p, off))
#define WF(p, off, v) (*(float *)AT(p, off) = (float)(v))
#define RP(p, off) (*(void **)AT(p, off))

// rva of the globals (Ghidra address 0x1441xxxxx - 0x140000000)
enum {
	G_UNITS = 0x4113748, G_MAXTEX = 0x4113744, G_DYNTEX = 0x411374c, G_CLIP = 0x4115b00, G_ANISO_MIN = 0x4115af8, G_ANISO_MAG = 0x4115af9, G_MAX_ANISO = 0x4115afc, G_SLOPE_BIAS = 0x4115b15, G_MIP_CUBE = 0x4115b16,
	G_SSAA = 0x4115b17, G_SHADOW_OK = 0x4115b04, G_SHADOW_DEPTH = 0x4115b08, G_SHADOW_RT = 0x4115b0c, G_SHADOW_A = 0x4115b10, G_SHADOW_B = 0x4115b14,
	G_DEPTH_FMT = 0x410fd40, G_MON_VALID = 0x410fd2c, G_MON_W = 0x410fd30, G_MON_H = 0x410fd34, G_DESK_W = 0x410fd38, G_DESK_H = 0x410fd3c,
	G_WIN_W = 0x4113720, G_WIN_H = 0x4113724, G_BB_W = 0x4113728, G_BB_H = 0x411372c, G_WINDOWED = 0x4113734, G_ASPECT = 0x4113738, G_PIX_X = 0x411373c, G_PIX_Y = 0x4113740,
	G_ASPECT_DVAR = 0x41164b8, G_ASPECT_FLAG_DVAR = 0x1325c38,
	C_K16 = 0x385ae0, C_HALF = 0x385a24, C_ASPECT_1610 = 0x385a80, C_ASPECT_169 = 0x385a88, C_ASPECT_43 = 0x385a68, C_ONE = 0x385a54,
};

void ngs_caps_globals(ngs *s, const nat_caps *c)
{
	uint8_t *e = s->exe;
	W32(e, G_UNITS, c->max_units > 16 ? 16 : c->max_units);
	W32(e, G_MAXTEX, c->max_texture);
	W8(e, G_DYNTEX, c->dynamic_textures);
	W32(e, G_CLIP, c->clip_planes > 6 ? 6 : c->clip_planes);
	W8(e, G_ANISO_MIN, c->aniso_min_mag);
	W8(e, G_ANISO_MAG, c->aniso_min_mag);
	W32(e, G_MAX_ANISO, c->max_aniso);
	W8(e, G_SLOPE_BIAS, c->slope_scale_bias);
	W8(e, G_MIP_CUBE, c->mip_cube);
	W8(e, G_SSAA, 0);
	W32(e, G_DEPTH_FMT, 0x4b); // D24S8: Direct3D 11 has no float-depth variant
	if (c->hw_shadow_depth) { // the success case of the depth / render-target format search: hardware depth textures
		W8(e, G_SHADOW_OK, 1);
		W32(e, G_SHADOW_DEPTH, 0x4b);       // D24S8: the first candidate pair of the table at rva 0x385c70 is (D24S8, "NULL")
		W32(e, G_SHADOW_RT, 0x4c4c554e);    // "NULL" colour target
		W32(e, G_SHADOW_A, 2);
		W8(e, G_SHADOW_B, 0x62);
	} else {
		W8(e, G_SHADOW_OK, 0);
		W32(e, G_SHADOW_DEPTH, 0x72);
		W32(e, G_SHADOW_RT, 0x4d);
		W32(e, G_SHADOW_A, 3);
		W8(e, G_SHADOW_B, 0x61);
	}
}

void ngs_display_globals(ngs *s, const nat_caps *c, int windowed, unsigned width, unsigned height, unsigned win_w, unsigned win_h)
{
	uint8_t *e = s->exe;
	W32(e, G_MON_W, c->desktop_w);
	W32(e, G_MON_H, c->desktop_h);
	W8(e, G_MON_VALID, c->desktop_w > 0 && c->desktop_h > 0);
	W32(e, G_DESK_W, c->desktop_w); // the current display mode of the adapter
	W32(e, G_DESK_H, c->desktop_h);
	W32(e, G_WIN_W, win_w);
	W32(e, G_WIN_H, win_h);
	W32(e, G_BB_W, width);
	W32(e, G_BB_H, height);
	W32(e, G_WINDOWED, windowed);
	const int mode = *(int *)AT(RP(e, G_ASPECT_DVAR), 0x10);
	int changed = 1;
	if (mode == 0) {
		int w = (int)width, h = (int)height;
		if (windowed == 0 && *AT(e, G_MON_VALID)) {
			w = (int)c->desktop_w;
			h = (int)c->desktop_h;
		}
		const int r = (int)floorf(((float)h * RF(e, C_K16)) / (float)w + RF(e, C_HALF)); // 16 * h / w: 10 = 16:10, below it 16:9, above 4:3
		WF(e, G_ASPECT, r == 10 ? RF(e, C_ASPECT_1610) : r < 10 ? RF(e, C_ASPECT_169) : RF(e, C_ASPECT_43));
	} else if (mode == 1) {
		WF(e, G_ASPECT, RF(e, C_ASPECT_43));
	} else if (mode == 2) {
		WF(e, G_ASPECT, RF(e, C_ASPECT_1610));
	} else if (mode == 3) {
		WF(e, G_ASPECT, RF(e, C_ASPECT_169));
	} else {
		changed = RF(e, G_ASPECT) != RF(e, C_ASPECT_43); // an unknown mode keeps the aspect; the flag says whether it differs from 4:3
	}
	if (s->set_aspect_dvar)
		s->set_aspect_dvar(RP(e, G_ASPECT_FLAG_DVAR), changed);
	WF(e, G_PIX_X, (float)win_h * RF(e, G_ASPECT) / (float)win_w);
	WF(e, G_PIX_Y, windowed == 0 ? (float)c->desktop_h * RF(e, G_ASPECT) / (float)c->desktop_w : RF(e, C_ONE));
}

void ngs_memory_setup(ngs *s, unsigned vram_mb, unsigned sys_mb, uint64_t total_virtual_mb, void (*set_dvar)(void *dvar, unsigned value))
{
	enum { G_VRAM_MAX = 0x48c2c94, G_Q1 = 0x481b818, G_Q2 = 0x481b81c, G_Q3 = 0x481b820, G_T1 = 0x4115ae4, G_T2 = 0x4115ae8, G_DVAR_Q1 = 0x4116288, G_DVAR_Q2 = 0x4116330, G_DVAR_Q3 = 0x41162b0, G_OVERRIDE = 0x41163b8, G_NOLIMIT = 0x4116358 };
	uint8_t *e = s->exe;
	uint32_t vram = *(uint32_t *)AT(e, G_VRAM_MAX);
	if (vram < vram_mb)
		vram = vram_mb;
	*(uint32_t *)AT(e, G_VRAM_MAX) = vram;
	uint32_t q1, q2, q3;
	if (*AT(RP(e, G_OVERRIDE), 0x10) != 0) { // the quality settings are given by the player's dvars
		q1 = *(uint32_t *)AT(RP(e, G_DVAR_Q1), 0x10);
		q2 = *(uint32_t *)AT(RP(e, G_DVAR_Q2), 0x10);
		q3 = *(uint32_t *)AT(RP(e, G_DVAR_Q3), 0x10);
	} else {
		q1 = 0;
		q3 = 0;
		if (vram < *(uint32_t *)AT(e, G_T1)) {
			q1 = 0;
			q3 = 1;
			if (vram < *(uint32_t *)AT(e, G_T2))
				q1 = 1;
		}
		if (total_virtual_mb < 3000) { // a 32-bit sized address space: at least level 1 for the third setting
			if (q3 == 0)
				q3 = 1;
		}
		unsigned lvl = 0;
		if (sys_mb < 0x401)
			lvl = 2;
		else if (sys_mb < 0x579)
			lvl = 1;
		if (lvl) {
			if (q1 < lvl)
				q1 = lvl;
			if (q3 < lvl)
				q3 = lvl;
		}
		q2 = q3;
		if (set_dvar) {
			set_dvar(RP(e, G_DVAR_Q1), q1);
			set_dvar(RP(e, G_DVAR_Q2), q2);
			set_dvar(RP(e, G_DVAR_Q3), q3);
		}
	}
	*(uint32_t *)AT(e, G_Q1) = q1;
	*(uint32_t *)AT(e, G_Q2) = q2;
	if (*AT(RP(e, G_NOLIMIT), 0x10) == 0)
		q3 = 3;
	*(uint32_t *)AT(e, G_Q3) = q3;
}
