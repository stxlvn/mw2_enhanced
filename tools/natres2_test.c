// Test of src/dx/natres2.c: shader, declaration, ring and particle buffer creation on a real Direct3D 11 device with synthetic game memory.
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/dx/natres.h"

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
#define WP(p, off, v) (*(void **)((uint8_t *)(p) + (off)) = (void *)(v))
#define RP(p, off) (*(void **)((uint8_t *)(p) + (off)))

static unsigned rand_state = 1;
static int fake_rand(void)
{
	rand_state = rand_state * 1103515245u + 12345u;
	return (int)((rand_state >> 16) & 0x7fff);
}
static unsigned qsort_calls;
static void fake_qsort(void *base, size_t num, size_t size, void *cmp)
{
	(void)base;
	(void)cmp;
	qsort_calls++;
	CHECK(num == 0x400 && (size == 0x50 || size == 0xa0), "qsort arguments");
}

static const uint32_t PS3[] = {0xffff0300, 0x02000001, 0x800f0800, 0xa0e40000, 0x0000ffff};

int main(void)
{
	WNDCLASSA wc = {0};
	wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.lpszClassName = "natres2test";
	RegisterClassA(&wc);
	HWND hwnd = CreateWindowA("natres2test", "natres2 test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 50, 50, 200, 200, NULL, NULL, wc.hInstance, NULL);
	char err[256] = "";
	nat_params p = {hwnd, 64, 64, 0, 0, 0, 0};
	nat_device *dev = NULL;
	if (!nat_create(&p, &dev, err, sizeof(err))) {
		printf("FAIL create: %s\n", err);
		return 1;
	}
	uint8_t *exe = calloc(1, 0x4900000);
	static uint8_t dvar_shaders[0x20], dvar_version[0x20];
	WP(exe, 0x41161a0, dvar_shaders);
	WP(exe, 0x4116198, dvar_version);
	dvar_shaders[0x10] = 1;
	*(uint32_t *)(dvar_version + 0x10) = 3;
	nd_ops ops = {NULL, NULL, NULL, NULL};
	nd *d = nd_create(dev, &ops);
	ngr r = {0};
	r.dev = dev;
	r.d = d;
	r.exe = exe;
	r.fn_rand = fake_rand;
	r.fn_qsort = fake_qsort;
	*(float *)(exe + 0x385b80) = 32768.0f;
	*(float *)(exe + 0x385a54) = 1.0f;
	*(float *)(exe + 0x385a18) = 0.25f;
	*(float *)(exe + 0x385a10) = 0.125f;

	// shaders: a source with bytecode and the right version stamp; wrong stamp or missing code give NULL
	static uint8_t src[0x20], obj[0x20];
	WP(src, 0, PS3);
	*(uint16_t *)(src + 10) = 3;
	ngr_create_shader(&r, src, obj);
	CHECK(RP(obj, 8) != NULL, "shader made");
	nd_shader_destroy(RP(obj, 8));
	*(uint16_t *)(src + 10) = 4;
	ngr_create_shader(&r, src, obj);
	CHECK(RP(obj, 8) == NULL, "stale shader version gives NULL");
	*(uint16_t *)(src + 10) = 3;
	dvar_shaders[0x10] = 0;
	ngr_create_shader(&r, src, obj);
	CHECK(RP(obj, 8) == NULL, "shaders off gives NULL");
	dvar_shaders[0x10] = 1;

	// declarations: table 1 (3 bytes per entry: stream, offset, type; sets of 9 per vertex type), table 2 (usage, index)
	uint8_t *t1 = exe + 0x36fa90, *t2 = exe + 0x36fc40;
	memset(t1, 0xff, 9 * 16 * 3); // everything invalid first
	for (unsigned s = 0; s < 16; s++) { // element kind 0: position float3 at offset 0 stream 0; kind 1: texcoord float2 at 12, stream 0 (invalid in set 5)
		uint8_t *e0 = t1 + (0 + s * 9) * 3, *e1 = t1 + (1 + s * 9) * 3;
		e0[0] = 0; e0[1] = 0; e0[2] = 2;
		if (s != 5) {
			e1[0] = 0; e1[1] = 12; e1[2] = 1;
		}
	}
	t2[0] = 0; t2[1] = 0; // usage position index 0
	t2[2] = 5; t2[3] = 0; // usage texcoord index 0
	static uint8_t set[0xc0];
	static const uint8_t elems[4] = {0, 0, 1, 1};
	ngr_make_decls(&r, set, elems, 2);
	CHECK(RP(set, 0x20) != NULL && RP(set, 0x20 + 5 * 8) == NULL && RP(set, 0x20 + 15 * 8) != NULL, "declarations: set 5 is invalid");
	dvar_shaders[0x10] = 0;
	ngr_make_decls(&r, set, elems, 2);
	CHECK(RP(set, 0x20) == NULL, "declarations cleared when shaders are off");
	dvar_shaders[0x10] = 1;

	// rings
	ngr_create_rings(&r);
	{
		int32_t *vr = (int32_t *)(exe + 0x41165c0);
		CHECK(vr[0] == 0 && vr[1] == 0x100000 && RP(vr, 8) != NULL && RP(exe, 0x41165d8) == vr, "main vertex ring");
		int32_t *ir = (int32_t *)(exe + 0x4116568);
		CHECK(ir[1] == 0x100000 && RP(ir, 8) != NULL && RP(exe, 0x4116580) == ir, "index ring 0");
		int32_t *v1 = (int32_t *)(exe + 0x4116588), *i1 = (int32_t *)(exe + 0x4116528);
		CHECK(v1[1] == 0x480000 && RP(v1, 8) != NULL && i1[1] == 0x100000 && RP(i1, 8) != NULL && RP(exe, 0x4116558) == i1, "other rings");
		nat_buffer *b = RP(vr, 8);
		void *m = nat_buffer_map(b, NAT_MAP_DISCARD);
		CHECK(m != NULL, "ring buffer maps");
		if (m)
			nat_buffer_unmap(b);
	}

	// staged static buffer
	ngr_staged *st = ngr_stage(64, 0);
	memset(st->mem, 7, 64);
	nat_buffer *sb = ngr_commit(&r, st);
	CHECK(sb != NULL, "static buffer from a staged block");

	// particle buffers
	ngr_particle_buffers(&r);
	CHECK(RP(exe, 0x4359468) && RP(exe, 0x4359470) && RP(exe, 0x4359478) && RP(exe, 0x4359480) && qsort_calls == 2, "particle buffers made and sorted (%u sorts)", qsort_calls);
	printf(fails ? "%d checks FAILED\n" : "all checks passed\n", fails);
	return fails ? 1 : 0;
}
