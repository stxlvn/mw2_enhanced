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
	G_UNITS = 0x113748, G_MAXTEX = 0x113744, G_DYNTEX = 0x11374c, G_CLIP = 0x115b00, G_ANISO_MIN = 0x115af8, G_ANISO_MAG = 0x115af9, G_MAX_ANISO = 0x115afc, G_SLOPE_BIAS = 0x115b15, G_MIP_CUBE = 0x115b16,
	G_SSAA = 0x115b17, G_SHADOW_OK = 0x115b04, G_SHADOW_DEPTH = 0x115b08, G_SHADOW_RT = 0x115b0c, G_SHADOW_A = 0x115b10, G_SHADOW_B = 0x115b14,
	G_DEPTH_FMT = 0x10fd40, G_MON_VALID = 0x10fd2c, G_MON_W = 0x10fd30, G_MON_H = 0x10fd34, G_DESK_W = 0x10fd38, G_DESK_H = 0x10fd3c,
	G_WIN_W = 0x113720, G_WIN_H = 0x113724, G_BB_W = 0x113728, G_BB_H = 0x11372c, G_WINDOWED = 0x113734, G_ASPECT = 0x113738, G_PIX_X = 0x11373c, G_PIX_Y = 0x113740,
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
