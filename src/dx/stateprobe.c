#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hook.h"
#include "../log.h"
#include "state11.h"
#include "stateprobe.h"

#ifdef _WIN64
#define VT_SET_RENDER_STATE 57

typedef HRESULT (STDMETHODCALLTYPE *set_rs_t)(void *dev, DWORD state, DWORD value);
typedef void (*pass_t)(void *ctx, unsigned pass);

static set_rs_t set_rs_orig;
static pass_t pass_orig;
static uint32_t shadow[256];
static uint8_t seen[256];
static volatile LONG calls, compared, mismatches, unknown;
static int installed_pass;

#define MAX_REPORTED 40
static struct { int id; uint32_t expected, actual, w0, w1; } reported[MAX_REPORTED];
static int n_reported;

static HRESULT STDMETHODCALLTYPE set_rs_hook(void *dev, DWORD state, DWORD value)
{
	if (state < 256) {
		shadow[state] = value;
		seen[state] = 1;
	}
	return set_rs_orig(dev, state, value);
}

// the distinct pairs of state words seen (a small open-addressing table): a clean run over few variants proves little
#define SEEN_SLOTS 8192
static uint64_t seen_pairs[SEEN_SLOTS];
static int n_pairs;
static void note_pair(uint32_t w0, uint32_t w1)
{
	const uint64_t key = ((uint64_t)w0 << 32 | w1) + 1;
	uint32_t h = (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 51);
	for (int i = 0; i < SEEN_SLOTS; i++, h = (h + 1) % SEEN_SLOTS) {
		if (seen_pairs[h] == key)
			return;
		if (!seen_pairs[h]) {
			seen_pairs[h] = key;
			n_pairs++;
			return;
		}
	}
}

static void check(uint32_t w0, uint32_t w1)
{
	note_pair(w0, w1);
	state11_t s;
	state11_rs_t e[48];
	state11_decode(w0, w1, &s);
	const int n = state11_expected_d3d9(&s, e, 48);
	for (int i = 0; i < n; i++) {
		if (e[i].id >= 256 || !seen[e[i].id]) {
			InterlockedIncrement(&unknown);
			continue;
		}
		InterlockedIncrement(&compared);
		const uint32_t actual = shadow[e[i].id];
		if ((actual & e[i].mask) == (e[i].value & e[i].mask))
			continue;
		InterlockedIncrement(&mismatches);
		int known = 0;
		for (int k = 0; k < n_reported; k++)
			if (reported[k].id == e[i].id && reported[k].expected == e[i].value && reported[k].actual == actual)
				known = 1;
		if (!known && n_reported < MAX_REPORTED) {
			reported[n_reported].id = e[i].id;
			reported[n_reported].expected = e[i].value;
			reported[n_reported].actual = actual;
			reported[n_reported].w0 = w0;
			reported[n_reported].w1 = w1;
			n_reported++;
			log_printf("stateprobe: MISMATCH render state %d: predicted %u, device has %u (state words %08x %08x)", e[i].id, e[i].value, actual, w0, w1);
		}
	}
}

static void pass_hook(void *ctx, unsigned pass)
{
	pass_orig(ctx, pass);
	const uint8_t *state = *(uint8_t **)((uint8_t *)ctx + 8);
	check(*(uint32_t *)(state + 0xb54), *(uint32_t *)(state + 0xb58));
	const LONG c = InterlockedIncrement(&calls);
	if (c % 1000000 == 0)
		log_printf("stateprobe: %ld pass set-ups, %d distinct state-word pairs, %ld states compared, %ld mismatches (%d distinct), %ld not seen yet", (long)c, n_pairs, (long)compared, (long)mismatches, n_reported, (long)unknown);
}

static void install_pass_hook(void)
{
	if (installed_pass)
		return;
	installed_pass = 1;
	uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
	IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	if (nt->OptionalHeader.SizeOfImage < 0x54a40) {
		log_printf("stateprobe: not the retail exe, no probe");
		return;
	}
	static const uint8_t head[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20}; // four register saves, 20 bytes
	if (hook_function(base + 0x54a10, head, sizeof(head), (void *)pass_hook, (void **)&pass_orig))
		log_printf("stateprobe: pass set-up hooked, comparing the device's render states with src/dx/state11.c");
	else
		log_printf("stateprobe: the bytes at rva 0x54a10 are not the expected ones, no probe");
}

void stateprobe_hook_device(struct IDirect3DDevice9 *dev)
{
	if (!getenv("MW2COOP_STATEPROBE"))
		return;
	void **vt = *(void ***)dev;
	if (vt[VT_SET_RENDER_STATE] != (void *)set_rs_hook) {
		DWORD old;
		if (VirtualProtect(&vt[VT_SET_RENDER_STATE], sizeof(void *), PAGE_READWRITE, &old)) {
			set_rs_orig = (set_rs_t)vt[VT_SET_RENDER_STATE];
			vt[VT_SET_RENDER_STATE] = (void *)set_rs_hook;
			VirtualProtect(&vt[VT_SET_RENDER_STATE], sizeof(void *), old, &old);
		}
	}
	install_pass_hook();
}
#else
void stateprobe_hook_device(struct IDirect3DDevice9 *dev)
{
	(void)dev;
}
#endif
