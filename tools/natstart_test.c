// Test of src/dx/natstart.c on synthetic game memory.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/dx/natstart.h"

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
#define U32(e, rva) (*(uint32_t *)((e) + (rva)))
#define F32(e, rva) (*(float *)((e) + (rva)))

static int dvar_calls, dvar_flag;
static void on_dvar(void *dvar, int flag)
{
	(void)dvar;
	dvar_calls++;
	dvar_flag = flag;
}

static int mem_calls;
static void on_mem(void *dvar, unsigned value)
{
	(void)dvar;
	(void)value;
	mem_calls++;
}

int main(void)
{
	uint8_t *exe = calloc(1, 0x4900000);
	ngs s = {exe, on_dvar};
	nat_caps c;
	memset(&c, 0, sizeof(c));
	c.dynamic_textures = 1;
	c.aniso_min_mag = 1;
	c.max_aniso = 16;
	c.max_texture = 16384;
	c.max_units = 32; // above the game's 16
	c.clip_planes = 8; // above the game's 6
	c.slope_scale_bias = 1;
	c.mip_cube = 1;
	c.hw_shadow_depth = 1;
	c.desktop_w = 1920;
	c.desktop_h = 1080;
	ngs_caps_globals(&s, &c);
	CHECK(U32(exe, 0x113748) == 16 && U32(exe, 0x115b00) == 6 && U32(exe, 0x113744) == 16384 && U32(exe, 0x115afc) == 16, "capped caps");
	CHECK(exe[0x11374c] == 1 && exe[0x115af8] == 1 && exe[0x115af9] == 1 && exe[0x115b15] == 1 && exe[0x115b16] == 1 && exe[0x115b17] == 0, "flags");
	CHECK(exe[0x115b04] == 1 && U32(exe, 0x115b08) == 0x4b && U32(exe, 0x115b0c) == 0x4c4c554e && U32(exe, 0x115b10) == 2 && exe[0x115b14] == 0x62 && U32(exe, 0x10fd40) == 0x4b, "shadow format decision, success case");
	c.hw_shadow_depth = 0;
	ngs_caps_globals(&s, &c);
	CHECK(exe[0x115b04] == 0 && U32(exe, 0x115b0c) == 0x4d && U32(exe, 0x115b08) == 0x72 && U32(exe, 0x115b10) == 3 && exe[0x115b14] == 0x61, "shadow format decision, fallback");

	// display globals: the aspect dvar struct (value at +0x10) and the float constants
	static int dvar_aspect[8];
	*(void **)(exe + 0x41164b8) = dvar_aspect;
	*(void **)(exe + 0x1325c38) = exe;
	F32(exe, 0x385ae0) = 16.0f;
	F32(exe, 0x385a24) = 0.5f;
	F32(exe, 0x385a80) = 1.6f;
	F32(exe, 0x385a88) = 1.7777f;
	F32(exe, 0x385a68) = 1.3333f;
	F32(exe, 0x385a54) = 1.0f;
	dvar_aspect[4] = 0; // auto
	ngs_display_globals(&s, &c, 1, 1280, 720, 1280, 720);
	CHECK(F32(exe, 0x113738) == 1.7777f && U32(exe, 0x113728) == 1280 && U32(exe, 0x11372c) == 720 && U32(exe, 0x113734) == 1, "auto aspect 16:9 (%g)", F32(exe, 0x113738));
	CHECK(F32(exe, 0x113740) == 1.0f && dvar_calls == 1, "windowed pixel ratio / dvar call");
	CHECK(U32(exe, 0x10fd30) == 1920 && U32(exe, 0x10fd34) == 1080 && exe[0x10fd2c] == 1, "monitor size");
	ngs_display_globals(&s, &c, 1, 1280, 800, 1280, 800);
	CHECK(F32(exe, 0x113738) == 1.6f, "auto aspect 16:10");
	ngs_display_globals(&s, &c, 1, 1024, 768, 1024, 768);
	CHECK(F32(exe, 0x113738) == 1.3333f, "auto aspect 4:3");
	ngs_display_globals(&s, &c, 0, 1024, 768, 1024, 768); // full screen takes the monitor's ratio (16:9)
	CHECK(F32(exe, 0x113738) == 1.7777f && F32(exe, 0x113740) > 0.99f && F32(exe, 0x113740) < 1.01f, "full screen: aspect of the monitor (%g, %g)", F32(exe, 0x113738), F32(exe, 0x113740));
	dvar_aspect[4] = 2;
	ngs_display_globals(&s, &c, 1, 1280, 720, 1280, 720);
	CHECK(F32(exe, 0x113738) == 1.6f, "forced 16:10");
	// memory setup: a card above both thresholds on a roomy machine keeps level 0; a small one raises the levels
	{
		static uint8_t dv[5][0x20];
		*(void **)(exe + 0x41163b8) = dv[0]; // override off
		*(void **)(exe + 0x4116288) = dv[1];
		*(void **)(exe + 0x4116330) = dv[2];
		*(void **)(exe + 0x41162b0) = dv[3];
		*(void **)(exe + 0x4116358) = dv[4];
		dv[4][0x10] = 1; // no limit flag set
		U32(exe, 0x115ae4) = 1024;
		U32(exe, 0x115ae8) = 512;
		ngs_memory_setup(&s, 6144, 16384, 8192, on_mem);
		CHECK(U32(exe, 0x481b818) == 0 && U32(exe, 0x481b81c) == 0 && U32(exe, 0x481b820) == 0 && U32(exe, 0x48c2c94) == 6144 && mem_calls == 3, "roomy machine: levels %u %u %u", U32(exe, 0x481b818), U32(exe, 0x481b81c), U32(exe, 0x481b820));
		ngs_memory_setup(&s, 256, 16384, 8192, on_mem); // the maximum only grows: 6144 stays
		CHECK(U32(exe, 0x48c2c94) == 6144, "video memory maximum only grows");
		U32(exe, 0x48c2c94) = 0;
		ngs_memory_setup(&s, 256, 16384, 8192, on_mem); // below both thresholds
		CHECK(U32(exe, 0x481b818) == 1 && U32(exe, 0x481b820) == 1, "small card: levels %u %u", U32(exe, 0x481b818), U32(exe, 0x481b820));
		ngs_memory_setup(&s, 6144, 1024, 8192, on_mem); // little system memory: level 2
		CHECK(U32(exe, 0x481b818) == 2 && U32(exe, 0x481b820) == 2, "little system memory: %u %u", U32(exe, 0x481b818), U32(exe, 0x481b820));
		dv[4][0x10] = 0; // limit flag clear: the third level is forced to 3
		ngs_memory_setup(&s, 6144, 16384, 8192, on_mem);
		CHECK(U32(exe, 0x481b820) == 3, "forced third level");
	}
	(void)dvar_flag;
	printf(fails ? "%d checks FAILED\n" : "all checks passed\n", fails);
	return fails ? 1 : 0;
}
