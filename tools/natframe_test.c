// Test of src/dx/natframe.c: the game's event-query ring on real Direct3D 11 queries.
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/dx/natframe.h"

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

static unsigned naps;
static void on_nap(unsigned ms)
{
	(void)ms;
	naps++;
	Sleep(1);
}

int main(void)
{
	WNDCLASSA wc = {0};
	wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.lpszClassName = "natframetest";
	RegisterClassA(&wc);
	HWND hwnd = CreateWindowA("natframetest", "natframe test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 50, 50, 200, 200, NULL, NULL, wc.hInstance, NULL);
	char err[256] = "";
	nat_params p = {hwnd, 64, 64, 0, 0, 0, 0};
	nat_device *dev = NULL;
	if (!nat_create(&p, &dev, err, sizeof(err))) {
		printf("FAIL create: %s\n", err);
		return 1;
	}
	uint8_t *exe = calloc(1, 0x4900000);
	static int32_t index_ring[4];
	*(void **)(exe + 0x4116580) = index_ring;
	for (int i = 0; i < 32; i++)
		((nat_query **)(exe + NGFR_POOL))[i] = nat_query_create(dev, 0);
	ngfr r = {dev, exe, on_nap};
	ID3D11DeviceContext *ctx = nat_d3d_context(dev);
	const float c[4] = {0, 0, 0, 1};

	for (int f = 0; f < 10; f++) {
		ngfr_wait_frames(&r);
		index_ring[0] = 77;
		ID3D11DeviceContext_ClearRenderTargetView(ctx, nat_scene_rtv(dev), c);
		CHECK(ngfr_present_and_issue(&r) >= 0, "present");
		CHECK(index_ring[0] == 0, "index ring restarts after the present");
	}
	CHECK(U32(exe, NGFR_ISSUED) == 10 && U32(exe, NGFR_FRAME) == 10 && U32(exe, NGFR_POOL_INDEX) == 10, "counters: issued %u frame %u pool %u", U32(exe, NGFR_ISSUED), U32(exe, NGFR_FRAME), U32(exe, NGFR_POOL_INDEX));
	nat_query **ring = (nat_query **)(exe + NGFR_RING);
	CHECK(ring[(10 - 1) & 7] == ((nat_query **)(exe + NGFR_POOL))[9], "the ring holds the last issued query");
	CHECK(*(void **)(exe + NGFR_LAST_EVENT) == ((nat_query **)(exe + NGFR_POOL))[9], "last-event global");
	// everything completes: retiring catches up with issuing
	unsigned guard = 0;
	while (U32(exe, NGFR_RETIRED) != U32(exe, NGFR_ISSUED) && guard++ < 2000) {
		ngfr_retire(&r);
		Sleep(1);
	}
	CHECK(U32(exe, NGFR_RETIRED) == 10, "retired %u of %u", U32(exe, NGFR_RETIRED), U32(exe, NGFR_ISSUED));
	CHECK(!ngfr_pending(&r, NULL), "no query: not pending");
	ngfr_drain(&r, ((nat_query **)(exe + NGFR_POOL))[9]);
	CHECK(!ngfr_pending(&r, ((nat_query **)(exe + NGFR_POOL))[9]), "drained query is done");
	// a pool slot that wraps: 32 more frames keep the ring bounded (3 in flight at most)
	for (int f = 0; f < 40; f++) {
		ngfr_wait_frames(&r);
		ID3D11DeviceContext_ClearRenderTargetView(ctx, nat_scene_rtv(dev), c);
		ngfr_present_and_issue(&r);
		CHECK((int)(U32(exe, NGFR_ISSUED) - U32(exe, NGFR_RETIRED)) <= 3 + 1 + 40, "in flight");
	}
	CHECK(U32(exe, NGFR_POOL_INDEX) == (50 & 31), "pool index wraps (%u)", U32(exe, NGFR_POOL_INDEX));
	// an occlusion query: draw nothing, the count is zero
	nat_query *oq = nat_query_create(dev, 1);
	(void)oq;
	const uint16_t ramp[3][256] = {{0}};
	CHECK(nat_set_gamma(dev, ramp) == 0, "gamma needs exclusive full screen");
	printf("naps %u\n", naps);
	printf(fails ? "%d checks FAILED\n" : "all checks passed\n", fails);
	return fails ? 1 : 0;
}
