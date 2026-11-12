// The bring-up chain of the native renderer (see natbring.h).
#include "natbring.h"

#include <stdio.h>
#include <string.h>

#include "../log.h"

#ifdef _WIN32
#include <windows.h>
#endif

#define AT(p, off) ((uint8_t *)(p) + (off))
#define RD_PTR(p, off) (*(void **)AT(p, off))
#define RD_U8(p, off) (*(uint8_t *)AT(p, off))
#define RD_U16(p, off) (*(uint16_t *)AT(p, off))
#define RD_I32(p, off) (*(int32_t *)AT(p, off))
#define RD_U32(p, off) (*(uint32_t *)AT(p, off))

enum {
	G_MODE_COUNT = 0x410fd44, G_MODE_TABLE = 0x410fd48, G_ADAPTER = 0x410fd28, G_DEVICE = 0x410fd20, G_FACTORY = 0x410fd18, G_DEPTH_FMT = 0x410fd40,
	G_RES_STRINGS = 0x4110d48, G_REFRESH_STRINGS = 0x4111550, G_STRING_BUFFER = 0x4111d58,
	D_MODE = 0x41164b0, D_REFRESH = 0x41164e8, D_XPOS = 0x41164f8, D_YPOS = 0x4116490, D_WINDOW_RES = 0x4116498, D_SAMPLES = 0x4116500, D_VSYNC = 0x41164d0, D_MONITOR = 0x41164c8, D_DEFAULT_RES = 0x4115ae0,
	G_WINDOW_COUNT = 0x4113278, G_WINDOWS = 0x4113280, G_CURRENT_WINDOW = 0x4113274, G_DEVICE_LOST = 0x4113270, G_STARTED = 0x4113750, G_NO_RENDER = 0x411376c,
};

void nbr_bind(nbr *n, uint8_t *exe)
{
	n->exe = exe;
#define BIND(field, rva) n->fn.field = (void *)(exe + (rva))
	BIND(dvar_int, 0x26c010);
	BIND(dvar_bool, 0x26bf90);
	BIND(dvar_set, 0x26e020);
	BIND(dvar_string, 0x26bd80);
	BIND(scan, 0xece0);
	BIND(print, 0xec80);
	BIND(reg_enum, 0x26cb10);
	BIND(dvar_reset_domain, 0x26c8d0);
	BIND(dvar_set_enum, 0x26ed50);
	BIND(qsort_, 0x2f6ab0);
	BIND(com_error, 0x273440);
	BIND(quit, 0x290630);
	BIND(clear_modified, 0x26bcf0);
	BIND(mem_zero, 0x2e42d0);
	BIND(alloc, 0x2699a0);
	BIND(alloc_named, 0x269b60);
	BIND(register_callback, 0x203b80);
	BIND(after_window, 0x2940c0);
	BIND(fn_4e390, 0x4e390);
	BIND(fn_14bee0, 0xbee0);
	BIND(fn_28690, 0x28690);
	BIND(fn_4e940, 0x4e940);
	BIND(fn_4f760, 0x4f760);
	BIND(fn_f3f0, 0xf3f0);
	BIND(fn_dvar_set_int, 0x26e020);
	BIND(fn_f570, 0xf570);
	BIND(fn_e340, 0x26e340);
	BIND(fn_203b00, 0x203b00);
	n->fn.enum_monitors_cb = exe + 0x103f0;
#undef BIND
	n->ngs_.exe = exe;
	n->ngs_.set_aspect_dvar = (void *)(exe + 0x26d800);
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ 0xfeb0
static int cmp_u32(const void *a, const void *b)
{
	const int32_t *x = a, *y = b;
	return x[0] != y[0] ? (x[0] < y[0] ? -1 : 1) : x[1] != y[1] ? (x[1] < y[1] ? -1 : 1) : x[2] != y[2] ? (x[2] < y[2] ? -1 : 1) : 0;
}

void nbr_list_modes(nbr *n)
{
	uint8_t *e = n->exe;
	nat_mode m[256];
	const int count = nat_list_modes(0, m, 256);
	int32_t *table = (int32_t *)AT(e, G_MODE_TABLE);
	int cnt = 0;
	for (int i = 0; i < count; i++) {
		table[cnt * 4 + 0] = (int32_t)m[i].w;
		table[cnt * 4 + 1] = (int32_t)m[i].h;
		table[cnt * 4 + 2] = m[i].hz ? (int32_t)m[i].hz : 0x3c;
		table[cnt * 4 + 3] = 0x16; // X8R8G8B8
		cnt++;
	}
	RD_I32(e, G_MODE_COUNT) = cnt;
	qsort(table, (size_t)cnt, 16, cmp_u32);
	// the distinct resolutions of 640x480 and above, and the distinct refresh rates (ascending)
	int32_t res[256][2];
	int nres = 0, hz[64], nhz = 0;
	for (int i = 0; i < cnt; i++) {
		const int w = table[i * 4], h = table[i * 4 + 1], r = table[i * 4 + 2];
		if ((nres == 0 || res[nres - 1][0] != w || res[nres - 1][1] != h) && w >= 640 && h >= 480) {
			res[nres][0] = w;
			res[nres][1] = h;
			nres++;
		}
		int dup = 0;
		for (int k = 0; k < nhz; k++)
			dup |= hz[k] == r;
		if (!dup && nhz < 64) {
			int pos = nhz++;
			while (pos > 0 && hz[pos - 1] > r) {
				hz[pos] = hz[pos - 1];
				pos--;
			}
			hz[pos] = r;
		}
	}
	if (nres == 0) {
		n->fn.com_error("No valid resolutions of %i x %i or above found", 640, 480);
		n->fn.quit();
	}
	char *buf = (char *)AT(e, G_STRING_BUFFER);
	const char **rs = (const char **)AT(e, G_RES_STRINGS), **hs = (const char **)AT(e, G_REFRESH_STRINGS);
	int def_res = 0, def_hz = 0;
	for (int i = 0; i < nres; i++) {
		rs[i] = buf;
		buf += snprintf(buf, 32, "%ix%i", res[i][0], res[i][1]) + 1;
		if (i == RD_I32(e, D_DEFAULT_RES))
			def_res = i;
	}
	rs[nres] = NULL;
	for (int i = 0; i < nhz; i++) {
		hs[i] = buf;
		buf += snprintf(buf, 32, "%i Hz", hz[i]) + 1;
		if (hz[i] < 60)
			def_hz = i;
	}
	hs[nhz] = NULL;
	void **dm = (void **)AT(e, D_MODE), **dr = (void **)AT(e, D_REFRESH);
	if (!*dm) {
		*dm = n->fn.reg_enum("r_mode", rs, def_res, 3, "Direct X resolution mode");
	} else {
		n->fn.dvar_reset_domain(*dm);
		n->fn.dvar_set_enum(*dm, rs);
	}
	if (!*dr) {
		*dr = n->fn.reg_enum("r_displayRefresh", hs, def_hz, 0x4003, "Refresh rate");
	} else {
		n->fn.dvar_reset_domain(*dr);
		n->fn.dvar_set_enum(*dr, hs);
	}
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ 0x10a90
void nbr_display_params(nbr *n, uint8_t *p)
{
	uint8_t *e = n->exe;
	const int mode = n->fn.dvar_int("r_displayMode");
	RD_I32(p, 0xc) = mode;
	int parsed = 0;
	if (mode == 1) {
		POINT pt;
		pt.x = n->fn.dvar_int("vid_xpos");
		pt.y = n->fn.dvar_int("vid_ypos");
		HMONITOR mon = MonitorFromPoint(pt, 0);
		if (!mon) {
			n->fn.dvar_set(RD_PTR(e, D_XPOS), 0);
			n->fn.dvar_set(RD_PTR(e, D_YPOS), 0);
			POINT zero = {0, 0};
			MonitorFromPoint(zero, 1);
		} else {
			MONITORINFO mi;
			memset(&mi, 0, sizeof(mi));
			mi.cbSize = 0x28;
			if (GetMonitorInfoA(mon, &mi)) {
				RD_I32(p, 0x10) = mi.rcMonitor.left;
				RD_I32(p, 0x14) = mi.rcMonitor.top;
				n->fn.dvar_set(RD_PTR(e, D_XPOS), mi.rcMonitor.left);
				n->fn.dvar_set(RD_PTR(e, D_YPOS), RD_I32(p, 0x14));
				const int w = mi.rcMonitor.right - mi.rcMonitor.left, h = mi.rcMonitor.bottom - mi.rcMonitor.top;
				RD_I32(p, 0x20) = w < 0 ? -w : w;
				RD_I32(p, 0x24) = h < 0 ? -h : h;
				parsed = 1;
			}
		}
		if (!parsed) {
			const char *s = n->fn.dvar_string(RD_PTR(e, D_MODE));
			if (n->fn.scan(s ? s : "INVALID_VALUE", "%ix%i", p + 0x20, p + 0x24) == 2) {
				parsed = 1;
			} else {
				n->fn.dvar_set(RD_PTR(e, D_MODE), 0);
				n->fn.scan(n->fn.dvar_string(RD_PTR(e, D_MODE)), "%ix%i", p + 0x20, p + 0x24);
				parsed = 1;
			}
		}
	} else {
		if (mode != 0) {
			const char *s = *(const char **)AT(RD_PTR(e, D_WINDOW_RES), 0x10);
			if (n->fn.scan(s, "%ix%i", p + 0x20, p + 0x24) == 2) {
				MONITORINFO mi;
				memset(&mi, 0, sizeof(mi));
				mi.cbSize = 0x28;
				int mw, mh;
				if (GetMonitorInfoA((HMONITOR)nat_output_monitor(0), &mi)) {
					mw = mi.rcMonitor.right - mi.rcMonitor.left;
					mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
				} else {
					mw = GetSystemMetrics(0);
					mh = GetSystemMetrics(1);
				}
				if (mw < 1 || mh < 1 || (RD_I32(p, 0x20) <= mw && RD_I32(p, 0x24) <= mh))
					parsed = 1;
			}
		}
		if (!parsed)
			n->fn.scan(n->fn.dvar_string(RD_PTR(e, D_MODE)), "%ix%i", p + 0x20, p + 0x24);
	}
	RD_I32(p, 0x18) = RD_I32(p, 0x20);
	RD_I32(p, 0x1c) = RD_I32(p, 0x24);
	int hz = 0x3c;
	if (RD_I32(p, 0xc) == 0) {
		n->fn.scan(n->fn.dvar_string(RD_PTR(e, D_REFRESH)), "%i Hz", &hz);
		const int32_t *t = (const int32_t *)AT(e, G_MODE_TABLE);
		const int cnt = RD_I32(e, G_MODE_COUNT), w = RD_I32(p, 0x20), h = RD_I32(p, 0x24);
		int lo = 0, hi = cnt - 1, found = 0;
		while (lo <= hi) {
			const int mid = (hi + lo) / 2;
			int c = t[mid * 4] - w;
			if (c == 0)
				c = t[mid * 4 + 1] - h;
			if (c == 0)
				c = t[mid * 4 + 2] - hz;
			if (c == 0) {
				found = 1;
				break;
			}
			if (c >= 0)
				hi = mid - 1;
			else
				lo = mid + 1;
		}
		if (!found && cnt > 0) {
			int k = hi < 0 ? 0 : hi;
			if (t[k * 4] != w || t[k * 4 + 1] != h)
				k = lo >= cnt ? cnt - 1 : lo;
			hz = t[k * 4 + 2];
		}
	}
	RD_I32(p, 8) = hz;
	RD_I32(p, 0x10) = n->fn.dvar_int("vid_xpos");
	RD_I32(p, 0x14) = n->fn.dvar_int("vid_ypos");
	RD_PTR(p, 0) = NULL;
	RD_I32(p, 0x28) = RD_I32(RD_PTR(e, D_SAMPLES), 0x10);
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ 0x10550
static const char *dev_error;
static uint8_t *g_exe;
static nd_shader *res_shader(void *p) { return p; }
static nd_decl *res_decl(void *p) { return p; }
static nat_image *res_image(void *p) { return p ? *(nat_image **)p : NULL; }
static void res_poly(int mode, float *bias, float *slope) { ngf_poly_offset(g_exe, mode, bias, slope); }
static const nd_ops n_ops = {res_shader, res_decl, res_image, res_poly};

static void nbr_bee0(nbr *n);

// QA: where the other threads are (an asset loader that stopped making progress): each thread is stopped for a moment and its instruction pointer and the code addresses on the top of
// its stack are logged as module+offset (exe addresses as rva).
#include <tlhelp32.h>
void nbr_dump_threads(uint8_t *exe)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return;
	THREADENTRY32 te;
	te.dwSize = sizeof(te);
	const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
	for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
		if (te.th32OwnerProcessID != pid || te.th32ThreadID == me)
			continue;
		HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
		if (!th)
			continue;
		if (SuspendThread(th) != (DWORD)-1) {
			CONTEXT c;
			memset(&c, 0, sizeof(c));
			c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
			if (GetThreadContext(th, &c)) {
				char line[700];
				int n = snprintf(line, sizeof(line), "native11: thread %lu rip %llx", (unsigned long)te.th32ThreadID, (unsigned long long)c.Rip);
				const uintptr_t exe_lo = (uintptr_t)exe, exe_hi = exe_lo + 0x4900000;
				HMODULE self = NULL;
				GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)nbr_dump_threads, &self);
				const uintptr_t mw_lo = (uintptr_t)self, mw_hi = mw_lo + 0x400000;
				HMODULE m;
				if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)c.Rip, &m)) {
					char nm[MAX_PATH];
					GetModuleFileNameA(m, nm, MAX_PATH);
					const char *base = strrchr(nm, 92) ? strrchr(nm, 92) + 1 : nm; // 92 = backslash
					n += snprintf(line + n, sizeof(line) - n, " (%s+0x%llx)", base, (unsigned long long)(c.Rip - (uintptr_t)m));
				}
				const uintptr_t *sp = (const uintptr_t *)c.Rsp;
				int shown = 0;
				n += snprintf(line + n, sizeof(line) - n, " stack:");
				for (int k = 0; k < 400 && shown < 18 && n < (int)sizeof(line) - 24; k++) {
					uintptr_t v;
					if (!ReadProcessMemory(GetCurrentProcess(), sp + k, &v, sizeof(v), NULL))
						break;
					if (v > exe_lo && v < exe_hi) {
						n += snprintf(line + n, sizeof(line) - n, " exe+%llx", (unsigned long long)(v - exe_lo));
						shown++;
					} else if (v > mw_lo && v < mw_hi) { // this DLL (winmm.dll): module offsets
						n += snprintf(line + n, sizeof(line) - n, " mw+%llx", (unsigned long long)(v - mw_lo));
						shown++;
					}
				}
				log_printf("%s", line);
			}
			ResumeThread(th);
		}
		CloseHandle(th);
	}
	CloseHandle(snap);
}

int nbr_start(nbr *n, uint8_t *p)
{
	uint8_t *e = n->exe;
	g_exe = e;
	nat_params np;
	memset(&np, 0, sizeof(np));
	np.hwnd = RD_PTR(p, 0);
	np.width = (unsigned)RD_I32(p, 0x20);
	np.height = (unsigned)RD_I32(p, 0x24);
	np.refresh = (unsigned)RD_I32(p, 8);
	np.fullscreen = RD_I32(p, 0xc) == 0;
	np.vsync = RD_U8(RD_PTR(e, D_VSYNC), 0x10) != 0;
	np.msaa = RD_I32(p, 0x28);
	char err[160] = "";
	RD_I32(e, G_DEPTH_FMT) = 0x4b;
	log_printf("native11: nbr_start: window %p, %ux%u, mode %d, refresh %u, samples %d", np.hwnd, np.width, np.height, RD_I32(p, 0xc), np.refresh, np.msaa);
	if (!nat_create(&np, &n->dev, err, sizeof(err))) {
		log_printf("native11: nat_create failed: %s", err);
		dev_error = "nat_create failed";
		RD_U8(e, G_STARTED) = 1;
		return 0;
	}
	RD_U8(e, G_STARTED) = 1;
	RD_U8(e, G_DEVICE_LOST) = 0;
	RD_PTR(e, G_DEVICE) = n->sentinel; // a non-NULL sentinel: the game tests it to mean "there is a device"
	const nat_caps *caps = nat_get_caps(n->dev);
	ngs_caps_globals(&n->ngs_, caps);
	RD_I32(e, 0x4113730) = RD_I32(p, 8);
	ngs_display_globals(&n->ngs_, caps, RD_I32(p, 0xc) != 0, np.width, np.height, (unsigned)RD_I32(p, 0x18), (unsigned)RD_I32(p, 0x1c));
	RD_I32(e, 0x4358e80) = (int32_t)np.width;
	RD_I32(e, 0x4358e84) = (int32_t)np.height;
	RD_PTR(e, 0x4358e78) = NULL;
	// the native back end objects
	memset(&n->gops, 0, sizeof(n->gops));
	n->gops.derived = (const float *(*)(void *, unsigned, unsigned))(e + 0x502f0);
	n->gops.filter_table = (const uint32_t *)(e + 0x4585290);
	n->gops.write_game_state = 1;
	ng_init(&n->g);
	memset(n->f.tex_thunk, 0, sizeof(n->f.tex_thunk)); // (the function pointers of n->f were set when the hooks were installed: natboot.c; only the device-bound state is reset)
	n->f.g = &n->g;
	n->f.ops = &n->gops;
	n->f.exe = e;
	n->r.dev = n->dev;
	n->r.mip_cube = caps->mip_cube;
	n->r.fn_48bd0 = (void *)(e + 0x48bd0);
	{ // the film planes (0x2b7980) upload only when the decoder says rectangles changed: BinkGetRects from the game's Bink DLL
		HMODULE bink = GetModuleHandleA("bink2w64.dll");
		FARPROC rects = bink ? GetProcAddress(bink, "BinkGetRects") : NULL;
		if (!rects && bink)
			rects = GetProcAddress(bink, "_BinkGetRects@8");
		n->r.fn_bink_rects = (int (*)(void *, int))rects;
		log_printf("native11: BinkGetRects %s", rects ? "found" : "NOT found (film planes are never uploaded)");
	}
	n->r.exe = e;
	n->pace.dev = n->dev;
	n->pace.exe = e;
	n->d = nd_create(n->dev, &n_ops);
	n->f.d = n->d;
	n->r.d = n->d;
	n->r.fn_rand = (int (*)(void))(e + 0x2f715c);
	n->r.fn_qsort = (void (*)(void *, size_t, size_t, void *))(e + 0x2f6ab0);
	n->r.fn_1fa40 = (void (*)(const uint8_t *, uint8_t *))(e + 0x1fa40);
	n->created = 1;
	log_printf("native11: native device created on %s (%u MB)", caps->adapter, caps->vram_mb);
	// The original start (0x10550) requests the common zones right after the device and the display mode exist (0xf3f0: DB_LoadXAssets of the gfx zones, which hold $white and the other
	// default images); the loader thread creates their images through the native image creation. Without it 0xf570 found no default image (QA 2026-11-12).
	RD_I32(e, 0x4113270) = 0;
	// 0xef10's last act on the dvar: r_rendererInUse = 0 (shader model 3.0), the version the game's shaders are made for (the shaders of another version are skipped)
	n->fn.fn_dvar_set_int(RD_PTR(e, 0x4116198), 0);
	n->fn.fn_f3f0();
	// the fixed resources of 0xf760: the render-target table first, then the CPU-side tables, the model lighting volume, the dynamic vertex buffer, rings, queries
	nbr_targets(n);
	if (RD_U8(e, G_NO_RENDER) == 0) {
		typedef int (*create_t)(uint8_t *, unsigned, unsigned, unsigned, unsigned, int);
		const create_t create = (create_t)(e + 0x492d0);
		nbr_bee0(n);
		const int alt = n->fn.dvar_bool("r_altModelLightingUpdate");
		uint8_t *vol0 = e + 0x481b9c0, *vol1 = e + 0x481b9e8;
		RD_PTR(vol0, 0x20) = RD_PTR(e, 0x36fa80);
		RD_PTR(e, 0x488a2e8) = vol0;
		RD_U16(vol0, 9) = 0x401;
		const unsigned vol_h = (unsigned)RD_I32(e, 0x488a2d0);
		if (!alt) {
			create(vol0, 0x100, vol_h, 4, 0x20002, 0x15);
			RD_PTR(e, 0x488a2f0) = NULL;
		} else {
			create(vol0, 0x100, vol_h, 4, 0x1020002, 0x15);
			RD_PTR(vol1, 0x20) = RD_PTR(e, 0x36fa88);
			RD_PTR(e, 0x488a2f0) = vol1;
			RD_U16(vol1, 9) = 0x401;
			create(vol1, 0x100, vol_h, 4, 0x4020002, 0x15);
		}
		RD_PTR(e, 0x488a2f8) = RD_PTR(e, 0x488a2e8);
		if (RD_U8(RD_PTR(e, 0x41161a0), 0x10) != 0)
			RD_PTR(e, 0x4116520) = nat_buffer_create(n->dev, 0xc00000, 0, 1, NULL);
		n->fn.fn_28690(0);
	}
	ngr_create_rings(&n->r);
	for (int i = 0; i < 2; i++) // "temp skin" memory for the two skinning buffers
		RD_PTR(e, 0x45d14d0 + (size_t)i * 0x27ae0 * 8) = n->fn.alloc_named(0x480000, "temp skin");
	RD_PTR(e, 0x4113298) = nat_query_create(n->dev, 0);
	for (int i = 0; i < 32; i++) {
		nat_query *q = nat_query_create(n->dev, 0);
		if (!q)
			return 0;
		((nat_query **)AT(e, 0x4113158))[i] = q;
	}
	RD_I32(e, 0x4113258) = 0;
	RD_I32(e, 0x41132a0) = 0;
	if (RD_U8(e, G_NO_RENDER) == 0) {
		const unsigned slots = RD_U32(e, 0x4115a88);
		for (unsigned i = 0; i < slots; i++)
			for (int k = 0; k < 2; k++) {
				((nat_query **)AT(e, 0x45851d0))[(size_t)i * 7 + (size_t)k] = nat_query_create(n->dev, 1);
				RD_U8(e, 0x45851c9 + (size_t)i * 0x38 + (size_t)k) = 0;
			}
		RD_PTR(e, 0x4115b28) = nat_query_create(n->dev, 1);
		ngr_particle_buffers(&n->r);
	}
	// the game's own start-up work that does no Direct3D
	n->fn.mem_zero(e + 0x4585490, 0, 0x8d0);
	RD_I32(e, 0x4585b60) = -1;
	RD_I32(e, 0x45859d0) = 0;
	RD_I32(e, 0x45859cc) = 1;
	RD_I32(e, 0x45859c8) = 1;
	RD_PTR(e, 0x4585d50) = n->fn.alloc(0x1900000);
	if (RD_U8(e, 0x4585488) == 0) {
		RD_U8(e, 0x4585488) = 1;
		n->fn.register_callback((void (*)(void))(e + 0xb1a0));
	}
	n->fn.mem_zero(e + 0x4356600, 0, 0x5c0);
	n->fn.fn_4e390(e + 0x4357760, e + 0x4356600, 0);
	RD_PTR(e, 0x4356c90) = n->sentinel;
	if (n->fn.fn_4f760)
		n->fn.fn_4f760();
	if (n->fn.fn_4e940)
		n->fn.fn_4e940();
	// the scene-size target is the current one; the tables of the CPU-side geometry pools
	RD_I32(e, 0x4358e80) = RD_I32(e, 0x4113728);
	RD_I32(e, 0x4358e84) = RD_I32(e, 0x411372c);
	RD_I32(e, 0x4358e90) = RD_I32(e, 0x4359038);
	RD_I32(e, 0x4358e94) = RD_I32(e, 0x435903c);
	RD_PTR(e, 0x4358e78) = NULL;
	RD_I32(e, 0x4358e8c) = 0;
	{
		void *ctx[2] = {RD_PTR(e, 0x36fc70), RD_PTR(e, 0x36fc78)};
		((void (*)(void *, int))(e + 0x50ec0))(ctx, 1);
	}
	if (n->fn.fn_e340)
		n->fn.fn_e340(RD_PTR(e, 0x41164e0));
	static const uint32_t pools[10] = {0x408f580, 0x409f580, 0x40a3580, 0x408f480, 0x40b3580, 0x40bb580, 0x40cb580, 0x40cd180, 0x40ced80, 0x40d0980};
	for (int i = 0; i < 10; i++)
		RD_PTR(e, 0x40d2950 + (size_t)i * 8) = e + pools[i];
	RD_I32(e, 0x40d2900) = 0x2000;
	RD_I32(e, 0x40d2904) = 0x800;
	RD_I32(e, 0x40d2908) = 0x2000;
	RD_I32(e, 0x40d290c) = 0x20;
	RD_I32(e, 0x40d2910) = 0x1000;
	RD_I32(e, 0x40d2914) = 0x2000;
	memcpy(e + 0x40d2918, e + 0x385c90, 16);
	// The asset loader thread creates the zone's images (through the native image creation) once the device exists; the original start reaches 0xf570 about half a second after
	// the device, by when the default images ($white ...) are registered. 0xef500 is the lookup without the default fallback: wait for it (QA 2026-11-12: the first native boot
	// asked 16 ms after the device and the engine stopped with "Could not load default asset $white").
	{
		const DWORD t0 = GetTickCount();
		uint8_t *(*raw_find)(int, const char *) = (uint8_t *(*)(int, const char *))(e + 0xef500);
		int dumped = 0;
		while (!raw_find(10, "$white") && GetTickCount() - t0 < 20000) {
			Sleep(5);
			if (!dumped && GetTickCount() - t0 > 3000) {
				dumped = 1;
				nbr_dump_threads(e);
			}
		}
		log_printf("native11: $white %s after %lu ms", raw_find(10, "$white") ? "is registered" : "was NOT registered", (unsigned long)(GetTickCount() - t0));
	}
	n->fn.fn_f570(); // default images and materials
	for (unsigned i = 0; i < 2; i++) {
		void *dv = RD_PTR(e, 0x41164a0 + (size_t)i * 8);
		n->fn.clear_modified(dv);
		if (RD_U8(dv, 0x10) != 0)
			n->fn.fn_203b00((int)i + 2);
	}
	return 1;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ 0xbee0
// 0xbee0: the vertex rings of the CPU-built geometry (particles, text, the film plane and the cinematic quads). 0x39830 locks a Direct3D 9 buffer to write four vertices; here it writes into
// a scratch block through a fake lock and the block is copied into the native buffer.
static uint8_t quad_scratch[0x100];
static int quad_lock(void *self, unsigned off, unsigned size, void **out, unsigned flags)
{
	(void)self; (void)off; (void)size; (void)flags;
	*out = quad_scratch;
	return 0;
}
static int quad_unlock(void *self)
{
	(void)self;
	return 0;
}
static void *quad_vtable[32];
static void *quad_object[2] = {quad_vtable, NULL};

static void quad_fill(nbr *n, uint8_t *q, float x, float y, float w, float h, float u0, float v0, float u1, float v1)
{
	quad_vtable[0x58 / 8] = (void *)quad_lock;
	quad_vtable[0x60 / 8] = (void *)quad_unlock;
	void *real = RD_PTR(q, 24);
	RD_PTR(q, 24) = quad_object;
	memset(quad_scratch, 0, sizeof(quad_scratch));
	((void (*)(void *, float, float, float, float, float, float, float, float))(n->exe + 0x39830))(q, x, y, w, h, u0, v0, u1, v1);
	RD_PTR(q, 24) = real;
	void *dst = nat_buffer_map((nat_buffer *)real, NAT_MAP_DISCARD);
	if (dst)
		memcpy(dst, quad_scratch, 0x80);
	nat_buffer_unmap((nat_buffer *)real);
}

static void ring_block(nbr *n, uint8_t *base, int cpu_size, int cap, int ring_size)
{
	// base = the address of the first int: [-4] zero, [0] capacity, [4] CPU block, [12] the ring, [36] stride
	void *(*alloc_)(size_t) = (void *(*)(size_t))(n->exe + 0x269a70);
	void *blk = alloc_((size_t)cpu_size);
	memcpy(base + 4, &blk, 8);
	RD_I32(base, 0) = cap;
	RD_I32(base, -4) = 0;
	ngr_ring_init(&n->r, base + 12, (unsigned)ring_size);
}

static void nbr_bee0(nbr *n)
{
	uint8_t *e = n->exe;
	void *(*alloc_)(size_t) = (void *(*)(size_t))(e + 0x269a70);
	uint8_t *p = e + 0x4591174;
	for (int it = 0; it < 2; it++, p += 0x4f5c0 * 4) {
		void *blk;
		ring_block(n, p, 0xc000, 0x6000, 0x80000);
		RD_I32(p, 36) = 0x20;
		blk = alloc_(0x1e00);
		memcpy(p - 44, &blk, 8);
		RD_I32(p, -48) = 0xf00;
		RD_I32(p, -52) = 0;
		RD_I32(p, -12) = 0x20;
		ngr_ring_init(&n->r, p - 36, 0x14000);
		blk = alloc_(0x9000);
		memcpy(p + 52, &blk, 8);
		RD_I32(p, 48) = 0x4800;
		RD_I32(p, 44) = 0;
		RD_I32(p, 84) = 0x20;
		ngr_ring_init(&n->r, p + 60, 0x42000);
		blk = alloc_(0x4800);
		memcpy(p + 100, &blk, 8);
		RD_I32(p, 96) = 0x2400;
		RD_I32(p, 92) = 0;
		RD_I32(p, 132) = 0x2c;
		ngr_ring_init(&n->r, p + 108, 0x42000);
		memset(p + 148, 0, 8);
		memset(p + 140, 0, 8);
		RD_I32(p, 180) = 0x14;
		ngr_ring_init(&n->r, p + 156, 0xb4000);
	}
	const float one = *(float *)(e + 0x385a54);
	const float w = (float)(unsigned)RD_I32(e, 0x4113720), h = (float)(unsigned)RD_I32(e, 0x4113724);
	p = e + 0x4359194;
	for (int it = 0; it < 4; it++, p += 0x40) {
		void *blk = alloc_(0xc);
		memcpy(p + 4, &blk, 8);
		RD_I32(p, 0) = 6;
		RD_I32(p, -4) = 0;
		RD_I32(p, 36) = 0x20;
		ngr_ring_init(&n->r, p + 12, 0x80);
		memset(p - 20, 0, 8);
		*(float *)(p - 8) = h;
		*(float *)(p - 12) = w;
		quad_fill(n, p - 4, 0.0f, 0.0f, w, h, 0.0f, 0.0f, one, one);
	}
	const float step_a = *(float *)(e + 0x385b50);
	uint8_t *q = e + 0x4359280;
	float y = 0.0f;
	p = e + 0x4359284;
	for (int it = 0; it < 4; it++, q += 0x30, p += 0x30, y += step_a) {
		void *blk = alloc_(0xc);
		memcpy(p + 4, &blk, 8);
		RD_I32(p, 0) = 6;
		RD_I32(p, -4) = 0;
		RD_I32(p, 36) = 0x20;
		ngr_ring_init(&n->r, p + 12, 0x80);
		quad_fill(n, q, 0.0f, y, step_a, step_a, 0.0f, 0.0f, one, one);
	}
	const float step_b = *(float *)(e + 0x385b60);
	q = e + 0x4359340;
	p = e + 0x4359344;
	y = 0.0f;
	for (int it = 0; it < 2; it++, q += 0x30, p += 0x30, y += step_b) {
		void *blk = alloc_(0xc);
		memcpy(p + 4, &blk, 8);
		RD_I32(p, 0) = 6;
		RD_I32(p, -4) = 0;
		RD_I32(p, 36) = 0x20;
		ngr_ring_init(&n->r, p + 12, 0x80);
		quad_fill(n, q, 0.0f, y, step_b, step_b, 0.0f, 0.0f, one, one);
	}
	RD_I32(e, 0x4116510) = 0;
	RD_I32(e, 0x4116514) = 0x180000;
	RD_PTR(e, 0x4116518) = alloc_(0x300000);
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ 0x10de0
void nbr_init(nbr *n)
{
	uint8_t *e = n->exe;
	log_printf("native11: device creation entry (0x10de0)");
	if (RD_PTR(e, G_DEVICE)) {
		n->fn.fn_f570();
		return;
	}
	// monitor choice, then the adapter (the native device uses the first adapter), caps and mode lists
	RD_I32(e, G_ADAPTER) = 0;
	nbr_list_modes(n);
	for (;;) {
		uint8_t params[0x50];
		memset(params, 0, sizeof(params));
		nbr_display_params(n, params);
		const int dm = RD_I32(params, 0xc);
		DWORD ex_style = 0, style = 0;
		if (dm == 0) {
			ex_style = 8;
			style = 0x80000000u;
		} else if (dm == 1) {
			style = 0x80000000u;
		} else if (dm == 2) {
			style = 0xc80000;
		}
		RECT rc = {0, 0, RD_I32(params, 0x20), RD_I32(params, 0x24)};
		AdjustWindowRectEx(&rc, style, 0, ex_style);
		HWND hwnd = CreateWindowExA(ex_style, "IW4", "Modern Warfare 2", style, RD_I32(params, 0x10), RD_I32(params, 0x14), rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, GetModuleHandleA(NULL), NULL);
		RD_PTR(params, 0) = hwnd;
		if (hwnd && nbr_start(n, params)) {
			RD_I32(e, G_CURRENT_WINDOW) = 0;
			// the game remembers its windows (0x10550's tail)
			RD_PTR(e, G_WINDOWS + (size_t)RD_I32(e, G_WINDOW_COUNT) * 24) = hwnd;
			RD_I32(e, G_WINDOWS + 0x10 + (size_t)RD_I32(e, G_WINDOW_COUNT) * 24) = RD_I32(params, 0x20);
			RD_I32(e, G_WINDOWS + 0x14 + (size_t)RD_I32(e, G_WINDOW_COUNT) * 24) = RD_I32(params, 0x24);
			RD_I32(e, G_WINDOW_COUNT)++;
			ShowWindow(hwnd, 5);
			n->fn.after_window();
			return;
		}
		// the start failed: fall back to a smaller setting, one dvar at a time (samples, refresh, resolution)
		void *d_samples = RD_PTR(e, D_SAMPLES), *d_refresh = RD_PTR(e, D_REFRESH), *d_mode = RD_PTR(e, D_MODE);
		if (RD_I32(d_samples, 0x10) < 2) {
			if (RD_I32(d_refresh, 0x10) < 1 || RD_I32(e, 0x4113730) < 0x3d) {
				if (RD_I32(d_mode, 0x10) < 1 || (RD_I32(e, 0x4113728) < 0x281 && RD_I32(e, 0x411372c) < 0x1e1))
					n->fn.quit();
				else
					n->fn.dvar_set(d_mode, RD_I32(d_mode, 0x10) - 1);
			} else {
				n->fn.dvar_set(d_refresh, RD_I32(d_refresh, 0x10) - 1);
			}
		} else {
			n->fn.dvar_set(d_samples, RD_I32(d_samples, 0x10) - 1);
		}
	}
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ 0x39420
// a target record {image struct, colour target, depth target, w, h} (32 bytes at rva 0x4359000 + 0x20 * n and the scratch records next to them)
static void make_record(nbr *n, int idx, int size_mode, unsigned shift, int fmt, int is_color, uint8_t *rec)
{
	uint8_t *e = n->exe;
	unsigned w = (unsigned)RD_I32(e, size_mode == 0 ? 0x4113728 : 0x4113720) >> shift, h = (unsigned)RD_I32(e, size_mode == 2 ? 0x4113724 : 0x411372c) >> shift;
	w = w < 2 ? 1 : w;
	h = h < 2 ? 1 : h;
	uint8_t *img = e + 0x481b830 + (size_t)idx * 0x28;
	*(uint16_t *)AT(img, 9) = 0x600;
	RD_PTR(img, 0x20) = RD_PTR(e, 0x36fa30 + (size_t)idx * 8);
	RD_PTR(rec, 0) = img;
	ngr_image_create(&n->r, img, w, h, 1, 0x2000003, fmt);
	nat_image *color = RD_PTR(img, 0);
	RD_U32(rec, 0x18) = w;
	RD_U32(rec, 0x1c) = h;
	RD_PTR(rec, is_color ? 8 : 0x10) = color;
}

// the shared single-sample depth image of the offscreen targets
static nat_image *shared_depth(nbr *n)
{
	uint8_t *e = n->exe;
	if (!RD_PTR(e, 0x4113268))
		RD_PTR(e, 0x4113268) = ngr_depth_create(&n->r, (unsigned)RD_I32(e, 0x4113728), (unsigned)RD_I32(e, 0x411372c), 1);
	return RD_PTR(e, 0x4113268);
}

// 0x391d0: an offscreen colour target with the shared depth
static void color_target(nbr *n, int idx, int size_mode, unsigned shift, int fmt, uint8_t *rec)
{
	make_record(n, idx, size_mode, shift, fmt, 1, rec);
	RD_PTR(rec, 0x10) = shared_depth(n);
}

// 0x39310: a shadow map: a depth texture (hardware path, with a colour target of the "NULL" format) or a colour texture with a separate depth image
static void shadow_map(nbr *n, int idx, unsigned w16, unsigned mult, uint8_t *rec)
{
	uint8_t *e = n->exe;
	const unsigned w = w16 & 0xffff, h = (unsigned)(int16_t)((int16_t)w16 * (int16_t)mult) & 0xffff;
	const int hw = RD_I32(e, 0x4115b10) == 2;
	uint8_t *img = e + 0x481b830 + (size_t)idx * 0x28;
	*(uint16_t *)AT(img, 9) = 0x600;
	RD_PTR(img, 0x20) = RD_PTR(e, 0x36fa30 + (size_t)idx * 8);
	RD_PTR(rec, 0) = img;
	ngr_image_create(&n->r, img, w, h, 1, 0x2000003, RD_I32(e, 0x4115b08));
	RD_U32(rec, 0x18) = w;
	RD_U32(rec, 0x1c) = h;
	nat_image *tex = RD_PTR(img, 0);
	nat_image_desc d = {NAT_IMAGE_2D, w, h, 1, 1, (unsigned)RD_I32(e, 0x4115b0c), hw ? NAT_RENDER_TARGET : NAT_DEPTH, 0};
	nat_image *other = nat_image_create(n->dev, &d);
	if (hw) {
		RD_PTR(rec, 0x10) = tex; // the depth texture is the depth target
		RD_PTR(rec, 8) = other;  // a dummy colour target of the "NULL" format
	} else {
		RD_PTR(rec, 8) = tex;
		RD_PTR(rec, 0x10) = other;
	}
}

void nbr_targets(nbr *n)
{
	uint8_t *e = n->exe;
	uint8_t *t = e + 0x4359000;
	const int no_render = RD_U8(e, G_NO_RENDER) != 0;
	RD_I32(e, 0x4359038) = RD_I32(e, 0x4113728);
	RD_I32(e, 0x435903c) = RD_I32(e, 0x411372c);
	// entry 1: the back buffer (the scene target and its depth); entry 2 is a copy
	RD_PTR(t + 0x20, 0) = NULL;
	RD_PTR(t + 0x20, 8) = NULL;
	RD_PTR(t + 0x20, 0x10) = no_render ? ND_NO_DEPTH : NULL;
	memcpy(t + 0x40, t + 0x20, 0x20);
	if (no_render)
		return;
	const int fmt = 0x15; // the back buffer's A8R8G8B8
	color_target(n, 7, 2, 0, fmt, t + 0x80);
	if (RD_U8(RD_PTR(e, 0x41162f8), 0x10))
		color_target(n, 2, 2, 0, 0x72, t + 0xa0);
	memcpy(t + 0x60, t + 0x80, 0x20);
	// entry 0: the saved screen (a copy of the scene's colour)
	unsigned w = (unsigned)RD_I32(e, 0x4113728), h = (unsigned)RD_I32(e, 0x411372c);
	w = w < 2 ? 1 : w;
	h = h < 2 ? 1 : h;
	uint8_t *saved = e + 0x481b970;
	RD_PTR(saved, 0x20) = RD_PTR(e, 0x36fa70);
	*(uint16_t *)AT(saved, 9) = 0x600;
	ngr_image_create(&n->r, saved, w, h, 1, 0x2000003, fmt);
	RD_PTR(t, 0) = saved;
	RD_PTR(t, 8) = RD_PTR(saved, 0);
	RD_U32(t, 0x18) = w;
	RD_U32(t, 0x1c) = h;
	shadow_map(n, 0, 0x400, 2, t + 0x140);
	shadow_map(n, 1, 0x200, 4, t + 0x160);
	color_target(n, 3, 2, 2, fmt, t + 0x100);
	color_target(n, 4, 2, 2, fmt, t + 0x120);
	color_target(n, 5, 2, 2, fmt, t + 0xc0);
	color_target(n, 6, 2, 2, fmt, t + 0xe0);
}
