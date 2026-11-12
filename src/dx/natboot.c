// MW2COOP_NATIVE11=1 (64-bit retail exe): installs the native renderer in place of the game's Direct3D 9 back end (docs/native-dx-plan.md, M5). The rewritten functions are redirected with
// hook_jump (the originals never run); the device the game believes it has is a tripwire object whose every method logs "unrewritten Direct3D 9 method #slot called from rva ..." and returns 0, so
// that a run shows exactly which function still has to be rewritten. No Direct3D 9 object is created.
#include "natboot.h"

#ifdef _WIN64
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "../hook.h"
#include "../log.h"
#include "../settings.h"
#include "../console.h"
#include "natbring.h"

static volatile LONG hits[48];
static const char *const hit_names[48] = {"w_54a10","w_54940","w_54070","w_543b0","w_53d00","w_53f10","w_54220","w_50a20","w_50b10","w_50c60","w_3cc00","w_50ec0","w_515a0","w_26730","w_54820","w_54b30","w_504d0","w_51070","w_26840","w_399f0","w_5c960","w_5cc10","w_5f220","w_5fe50","w_63850","w_3ec20","w_5da10","w_5dde0","w_63f50","w_266a0","w_267b0","w_267e0","w_26810","w_492d0","w_49170","w_23920","w_8f60","w_258a0","w_24f00","w_44d00","w_453c0","w_10410","w_45010","w_2f800","w_10de0","w_4f380","w_39420","w_45060"};
static nbr N;
static uint8_t *exe;

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ the tripwire object
#define TRAP_SLOTS 128
static void *trap_vtable[TRAP_SLOTS];
static void *trap_object[2] = {trap_vtable, NULL};
static struct {
	unsigned slot;
	uintptr_t ret;
	volatile LONG count;
} seen[256];
static volatile LONG n_seen;

static void trap_log(unsigned slot, uintptr_t ret)
{
	const uintptr_t rva = ret - (uintptr_t)exe;
	for (LONG i = 0; i < n_seen && i < 256; i++)
		if (seen[i].slot == slot && seen[i].ret == rva) {
			InterlockedIncrement(&seen[i].count);
			return;
		}
	const LONG k = InterlockedIncrement(&n_seen) - 1;
	if (k < 256) {
		seen[k].slot = slot;
		seen[k].ret = rva;
		seen[k].count = 1;
		log_printf("native11: unrewritten Direct3D 9 method #%u called from rva 0x%llx", slot, (unsigned long long)rva);
	}
}

static void make_traps(void)
{
	static const uint8_t pro[] = {0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xec, 0x28}; // push rcx, rdx, r8, r9; sub rsp, 28h
	uint8_t *page = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	size_t used = 0;
	for (unsigned s = 0; s < TRAP_SLOTS; s++) {
		uint8_t *p = page + used, *q = p;
		memcpy(q, pro, sizeof(pro));
		q += sizeof(pro);
		*q++ = 0xb9; // mov ecx, slot
		memcpy(q, &s, 4);
		q += 4;
		*q++ = 0x48; *q++ = 0x8b; *q++ = 0x54; *q++ = 0x24; *q++ = 0x48; // mov rdx, [rsp+48h]
		*q++ = 0x48; *q++ = 0xb8; // mov rax, trap_log
		void *fn = (void *)trap_log;
		memcpy(q, &fn, 8);
		q += 8;
		*q++ = 0xff; *q++ = 0xd0; // call rax
		*q++ = 0x48; *q++ = 0x83; *q++ = 0xc4; *q++ = 0x28; // add rsp, 28h
		*q++ = 0x41; *q++ = 0x59; *q++ = 0x41; *q++ = 0x58; *q++ = 0x5a; *q++ = 0x59; // pop r9, r8, rdx, rcx
		*q++ = 0x31; *q++ = 0xc0; // xor eax, eax
		*q++ = 0xc3;              // ret
		trap_vtable[s] = p;
		used += (size_t)(q - p + 15) & ~15u;
	}
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ wrappers (the game's calling conventions)
#define CTX(c) ((void **)(c))[0], ((void **)(c))[1]

static void w_54a10(void *ctx, unsigned pass) { InterlockedIncrement(&hits[0]); ng_select_pass(&N.g, &N.gops, ((void **)ctx)[0], ((void **)ctx)[1], pass); }
static void w_54940(void *state) { InterlockedIncrement(&hits[1]); ng_apply_decl_shader(&N.g, &N.gops, state); }
static void w_54070(void *ctx, int count, void *args) { InterlockedIncrement(&hits[2]); ng_apply_ps_args(&N.g, &N.gops, CTX(ctx), args, (unsigned)count); }
static void w_543b0(void *ctx, int count, void *args) { InterlockedIncrement(&hits[3]); ng_apply_vs_stable_args(&N.g, &N.gops, CTX(ctx), args, (unsigned)count); }
static void w_53d00(void *ctx, int count, void *args) { InterlockedIncrement(&hits[4]); ng_apply_object_args(&N.g, &N.gops, CTX(ctx), args, (unsigned)count); }
static void w_53f10(void *ctx) { InterlockedIncrement(&hits[5]); ng_apply_prim(&N.g, &N.gops, CTX(ctx)); }
static void w_54220(void *ctx, int count, void *args) { InterlockedIncrement(&hits[6]); ng_apply_code_vc_args(&N.g, &N.gops, CTX(ctx), args, (unsigned)count); }
static void w_50a20(void *ctx, unsigned slot, uint8_t byte, void *image) { InterlockedIncrement(&hits[7]); ng_bind_sampler(&N.g, &N.gops, slot, byte, image); (void)ctx; }
static void w_50b10(void *state, unsigned slot, uint8_t byte) { InterlockedIncrement(&hits[8]); ng_set_sampler_state(&N.g, &N.gops, slot, byte); (void)state; }
static void w_50c60(void *state, int *vp) { InterlockedIncrement(&hits[9]); if (!N.created) return; ngf_set_viewport(&N.f, state, vp); }
static void w_3cc00(void *state, int flag) { InterlockedIncrement(&hits[10]); if (!N.created) return; ngf_depth_range(&N.f, state, flag); }
static void w_50ec0(void *ctx, int target) { InterlockedIncrement(&hits[11]); if (!N.created) return; ngf_target_switch(&N.f, ((void **)ctx)[0], ((void **)ctx)[1], target); }
static void w_515a0(void *state, int *rect) { InterlockedIncrement(&hits[12]); if (!N.created) return; ngf_set_scissor(&N.f, state, rect); }
static void w_26730(void *dev, void *buf, int off, int stride) { InterlockedIncrement(&hits[13]); if (!N.created) return; ngf_set_streams(&N.f, (uint8_t *)dev - 0xd0, buf, off, stride); }
static int w_54820(void *dev, void *src, int tris) { InterlockedIncrement(&hits[14]); return ngf_ring_indices(&N.f, (uint8_t *)dev - 0xd0, src, tris); }
static int w_54b30(void *a, void *src, int count) { InterlockedIncrement(&hits[15]); (void)a; return ngf_ring_vertices(&N.f, src, count); }
static void w_504d0(void *dev) { InterlockedIncrement(&hits[16]); if (!N.created) return; ngf_clear_streams(&N.f, (uint8_t *)dev - 0xd0); }
static void w_51070(void *dev, uint8_t flags, float *rgba, float depth, uint8_t stencil) { InterlockedIncrement(&hits[17]); if (!N.created) return; (void)dev; ngf_clear(&N.f, flags, rgba, depth, stencil); }
static void w_26840(void *ctx, int *cmd) { InterlockedIncrement(&hits[18]); if (!N.created) return; ngf_draw_26840(&N.f, ctx, cmd); }
static void w_399f0(void *ctx, void *material, void *info) { InterlockedIncrement(&hits[19]); if (!N.created) return; ngf_draw_399f0(&N.f, ctx, material, info); }
static void w_5c960(void *ctx, uintptr_t *cur) { InterlockedIncrement(&hits[20]); if (!N.created) return; ngf_draw_5c960(&N.f, ctx, cur); }
static void w_5cc10(void *ctx, uintptr_t *cur) { InterlockedIncrement(&hits[21]); if (!N.created) return; ngf_draw_5cc10(&N.f, ctx, cur); }
static void w_5f220(void *ctx, uintptr_t *cur) { InterlockedIncrement(&hits[22]); if (!N.created) return; ngf_draw_5f220(&N.f, ctx, cur); }
static void w_5fe50(uintptr_t *cur, void *ctx) { InterlockedIncrement(&hits[23]); if (!N.created) return; ngf_draw_5fe50(&N.f, cur, ctx); }
static void w_63850(void *item, void *ctx) { InterlockedIncrement(&hits[24]); if (!N.created) return; ngf_draw_63850(&N.f, item, ctx); }
static void log_backtrace(const char *what)
{
	void *fr[32];
	const USHORT n = RtlCaptureStackBackTrace(0, 32, fr, NULL);
	char line[600];
	int k = snprintf(line, sizeof(line), "native11: %s, callers:", what);
	for (USHORT i = 0; i < n && k < (int)sizeof(line) - 24; i++) {
		const uintptr_t v = (uintptr_t)fr[i];
		if (v > (uintptr_t)exe && v < (uintptr_t)exe + 0x4900000)
			k += snprintf(line + k, sizeof(line) - k, " exe+%llx", (unsigned long long)(v - (uintptr_t)exe));
		else
			k += snprintf(line + k, sizeof(line) - k, " ?%llx", (unsigned long long)(v & 0xfffff));
	}
	log_printf("%s", line);
}
static void w_3ec20(void *ctx, void *cmd)
{
	const LONG h = InterlockedIncrement(&hits[25]);
	if (!N.created)
		return;
	if (h == 4000000 || h == 4000001)
		log_backtrace("w_3ec20 spinning");
	ngf_draw_3ec20(&N.f, ctx, cmd);
}
static void w_5da10(void *ctx, uintptr_t *cur) { InterlockedIncrement(&hits[26]); if (!N.created) return; ngf_draw_5da10(&N.f, ctx, cur); }
static void w_5dde0(void *ctx, int *cmd) { InterlockedIncrement(&hits[27]); if (!N.created) return; ngf_draw_5dde0(&N.f, ctx, cmd); }
static void w_63f50(uintptr_t *cur, unsigned packed, void *ctx) { InterlockedIncrement(&hits[28]); if (!N.created) return; ngf_draw_63f50(&N.f, cur, packed, ctx); }
static void w_266a0(void) { InterlockedIncrement(&hits[29]); if (!N.created) return; ngf_unbind_textures(&N.f, *(uint32_t *)(exe + 0x4113748)); }
static void w_267b0(void) { InterlockedIncrement(&hits[30]); if (!N.created) return; ngf_unbind_ps(&N.f); }
static void w_267e0(void) { InterlockedIncrement(&hits[31]); if (!N.created) return; ngf_unbind_vs(&N.f); }
static void w_26810(void) { InterlockedIncrement(&hits[32]); if (!N.created) return; ngf_unbind_decl(&N.f); }
static int w_492d0(uint8_t *image, unsigned w, unsigned h, unsigned d, unsigned flags, int fmt) { InterlockedIncrement(&hits[33]); if (!N.created) return 0; return ngr_image_create(&N.r, image, w & 0xffff, h & 0xffff, d & 0xffff, flags, fmt); }
static void w_49170(uint8_t *image, unsigned fmt, unsigned face, int level, const void *data) { InterlockedIncrement(&hits[34]); if (!N.created) return; ngr_image_upload(&N.r, image, (int)fmt, face, (unsigned)level, data); }
static void w_23920(uint8_t *image) { InterlockedIncrement(&hits[35]); ngr_image_release(image); *(uint64_t *)(image + 0x10) = 0; }
static void w_8f60(uint8_t *src, uint8_t *obj)
{
	InterlockedIncrement(&hits[36]);
	if (!N.created)
		return;
	ngr_create_shader(&N.r, src, obj);
	static volatile LONG logged, failed, made;
	if (*(void **)(obj + 8))
		made++;
	else
		failed++;
	if (((made + failed) % 500) == 0)
		log_printf("native11: shaders: %ld created, %ld not (version or none)", (long)made, (long)failed);
	if (!*(void **)(obj + 8) && InterlockedIncrement(&logged) <= 4)
		log_printf("native11: shader not created: code %p, shaders-on dvar value %d, source version %u, dvar version %u", *(void **)src, (int)*(uint8_t *)(*(uint8_t **)(exe + 0x41161a0) + 0x10), (unsigned)*(uint16_t *)(src + 10), (unsigned)*(uint32_t *)(*(uint8_t **)(exe + 0x4116198) + 0x10));
}
static void w_258a0(uint8_t *set, int count) { InterlockedIncrement(&hits[37]); if (!N.created) return; ngr_make_decls(&N.r, set, set, count); }
static void w_24f00(void *ring, unsigned size) { InterlockedIncrement(&hits[38]); if (!N.created) return; ngr_ring_init(&N.r, ring, size); }
static void w_44d00(uint8_t *scene) { InterlockedIncrement(&hits[39]); if (!N.created) return; ngfr_44d00(&N.frame, scene); }
static void w_453c0(void) { InterlockedIncrement(&hits[40]); if (!N.created) return; ngfr_453c0(&N.frame); }
static void w_10410(void) { InterlockedIncrement(&hits[41]); ngfr_retire(&N.pace); }
static int w_45010(void) { InterlockedIncrement(&hits[42]); return !ngfr_pending(&N.pace, *(void **)(exe + NGFR_LAST_EVENT)) ; }
static int w_2f800(void) { InterlockedIncrement(&hits[43]); return ngfr_pending(&N.pace, *(void **)(*(uint8_t **)(exe + 0x410fd00) + 0x419c8)); }
static void w_10de0(void) { InterlockedIncrement(&hits[44]); nbr_init(&N); }
static void w_noop(void) {}
// 0x2b7840 / 0x2b7980: the Bink film planes (lock: the decoder writes into CPU memory; unlock: the changed planes go to the images)
static void w_2b7840(uint8_t *film) { if (N.created) ngr_film_lock(&N.r, film); }
static void w_2b7980(void *dev, uint8_t *film, void *bink) { (void)dev; if (N.created) ngr_film_unlock(&N.r, film, bink); }
// 0x4d100: the start-up occlusion query self-test: it clears, draws a 16 x 16 quad (DAT_140385ae0) between the query's Begin and End and returns the samples that passed (0x100 = 16 x 16 is also
// what the game takes when the query gives no answer). Native queries work, so the answer is the expected one; the result only scales the later visibility tests.
static unsigned w_4d100(void) { return N.created && *(void **)(exe + 0x45851d0) ? 0x100u : 0u; }
// 0x23950 / 0x23a90 / 0x23be0: the image creators (also called by the asset loader 0x23db0 and through a function table, not only by 0x492d0)
static void w_23950(uint8_t *image, unsigned w, unsigned h, unsigned levels, unsigned flags, int fmt) { if (N.created) ngr_creator_2d(&N.r, image, w & 0xffff, h & 0xffff, levels, flags, fmt); }
static void w_23a90(uint8_t *image, unsigned w, unsigned h, unsigned d, unsigned p5, unsigned flags, int fmt) { (void)p5; if (N.created) ngr_creator_volume(&N.r, image, w & 0xffff, h & 0xffff, d & 0xffff, flags, fmt); }
static void w_23be0(uint8_t *image, unsigned w, unsigned nomip, int fmt) { if (N.created) ngr_creator_cube(&N.r, image, w & 0xffff, nomip, fmt); }
// 0x24d00 / 0x24df0: a static vertex / index buffer for the asset loader; *out receives the buffer object, the return value is the block to write (0 when shaders are off: the engine then
// makes no buffers). The loader fills the block and calls Unlock through the object's vtable (nat11.c).
static void *w_24d00(void **out, uint64_t size)
{
	if (!N.created || *(char *)(*(uint8_t **)(exe + 0x41161a0) + 0x10) == 0)
		return NULL;
	void *block = NULL;
	*out = nat_buffer_create_static(N.dev, (unsigned)size, 0, &block);
	return *out ? block : NULL;
}
static void *w_24df0(void **out, uint64_t size)
{
	if (!N.created || *(char *)(*(uint8_t **)(exe + 0x41161a0) + 0x10) == 0)
		return NULL;
	void *block = NULL;
	*out = nat_buffer_create_static(N.dev, (unsigned)size, 1, &block);
	return *out ? block : NULL;
}
static unsigned w_4f380(void *dev, unsigned slot, unsigned word, unsigned old) { InterlockedIncrement(&hits[45]); (void)dev; (void)slot; (void)old; return word; }
static void w_39420(void) { InterlockedIncrement(&hits[46]); if (N.created) nbr_targets(&N); }
// the mod's console panel (src/console.c) over the finished frame: one image, made again when the bitmap's size changes
static nat_image *overlay_image;
static unsigned overlay_w, overlay_h;
static void overlay_show(const uint32_t *bits, int w, int h, int repainted)
{
	if (!overlay_image || overlay_w != (unsigned)w || overlay_h != (unsigned)h) {
		nat_image_destroy(overlay_image);
		nat_image_desc d = {NAT_IMAGE_2D, (unsigned)w, (unsigned)h, 1, 1, 21 /* A8R8G8B8 */, 0, 0};
		overlay_image = nat_image_create(N.dev, &d);
		overlay_w = (unsigned)w;
		overlay_h = (unsigned)h;
		repainted = 1;
	}
	if (!overlay_image)
		return;
	if (repainted)
		nat_image_upload(overlay_image, 0, 0, bits, (unsigned)w * 4, 0);
	nd_overlay(N.d, overlay_image);
}
static char shot_name[96];
void natboot_shot(const char *name)
{
#ifndef NAT_BACKEND_12
	if (settings_directx12()) {
		extern void b12_natboot_shot(const char *);
		b12_natboot_shot(name);
		return;
	}
#endif
	lstrcpynA(shot_name, name, sizeof(shot_name));
}
static volatile DWORD render_tid;
static void w_45060(unsigned flags)
{ InterlockedIncrement(&hits[47]);
	render_tid = GetCurrentThreadId();
	if (!N.created || !(flags & 2))
		return;
	if (getenv("MW2COOP_NATCON")) { // QA: the console is opened at frame 5000 of the run and closed at 5500
		static int f;
		f++;
		if (f == 5000)
			console_qa_open(1);
		if (f == 5100)
			natboot_shot("natcon");
		if (f == 5500)
			console_qa_open(0);
	}
	{ // the console panel, when it is open
		unsigned bw, bh;
		nat_get_size(N.dev, &bw, &bh);
		console_native_frame(bw, bh, overlay_show);
	}
	if (getenv("MW2COOP_FLASHWATCH")) { // QA: a frame that is (nearly) one colour is saved: the full-screen flashes reported with the native renderers
		static int skip, saved;
		if (++skip % 3 == 0) {
			uint32_t px[48];
			if (nat_sample_scene(N.dev, 8, 6, px)) {
				unsigned lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
				for (int i = 0; i < 48; i++)
					for (int c = 0; c < 3; c++) {
						const unsigned v = (px[i] >> (8 * c)) & 255;
						lo[c] = v < lo[c] ? v : lo[c];
						hi[c] = v > hi[c] ? v : hi[c];
					}
				static int in_flash, start_frame, last_frame;
				static unsigned first_col, last_col, spans;
				const int uniform = hi[0] + hi[1] + hi[2] > 60 && hi[0] - lo[0] < 12 && hi[1] - lo[1] < 12 && hi[2] - lo[2] < 12;
				const unsigned col = (px[0] & 0xffffff);
				if (skip > 300 && uniform) {
					if (!in_flash) {
						in_flash = 1;
						start_frame = skip;
						first_col = col;
						if (saved < 6) {
							char nm[48];
							snprintf(nm, sizeof(nm), "flash%d", saved++);
							natboot_shot(nm);
						}
					}
					last_frame = skip;
					last_col = col;
				} else if (in_flash) {
					in_flash = 0;
					spans++;
					log_printf("native11: flashwatch: one-colour frames from frame %d to %d (about %d frames), colour %06x -> %06x, then the picture returned (span %u)", start_frame, last_frame, last_frame - start_frame + 3, first_col, last_col, spans);
				}
			}
		}
	}
	if (shot_name[0]) { // !shot <name>
		char name[96], path[MAX_PATH];
		lstrcpynA(name, shot_name, sizeof(name));
		shot_name[0] = 0;
		DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
		while (n > 0 && path[n - 1] != 92)
			n--;
		path[n] = 0;
		lstrcatA(path, "mw2coop\\shots");
		CreateDirectoryA(path, NULL);
		lstrcatA(path, "\\");
		lstrcatA(path, name);
		lstrcatA(path, ".bmp");
		log_printf("shot: %s (native frame): %s", name, nat_save_scene_bmp(N.dev, path) ? "saved" : "not saved");
	}
	static int frame;
	frame++;
	if (frame == 300 || frame == 900 || frame == 1800 || frame == 3000 || (frame > 3000 && frame <= 24000 && frame % 1500 == 0)) {
		char path[MAX_PATH];
		DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
		while (n > 0 && path[n - 1] != 92)
			n--;
		path[n] = 0;
		lstrcatA(path, "mw2coop");
		CreateDirectoryA(path, NULL);
		lstrcatA(path, "\\native11");
		CreateDirectoryA(path, NULL);
		char name[64];
		snprintf(name, sizeof(name), "\\scene-%d.bmp", frame);
		lstrcatA(path, name);
		log_printf("native11: frame %d, scene saved: %s", frame, nat_save_scene_bmp(N.dev, path) ? "yes" : "no");
	}
	ngfr_present_and_issue(&N.pace);
}
static const char *safe_str(const char *p) { return (p && !IsBadReadPtr(p, 1)) ? p : "?"; }
static void w_sys_error(const char *fmt, const char *a, const char *b, const char *c)
{
	log_printf("native11: ENGINE ERROR: %s | %s | %s | %s", safe_str(fmt), safe_str(a), safe_str(b), safe_str(c));
	ExitProcess(3);
}
static void w_242a0(void)
{
	if (!N.created)
		return;
	MEMORYSTATUSEX ms;
	memset(&ms, 0, sizeof(ms));
	ms.dwLength = sizeof(ms);
	GlobalMemoryStatusEx(&ms);
	ngs_memory_setup(&N.ngs_, nat_get_caps(N.dev)->vram_mb, (unsigned)N.fn.dvar_int("sys_sysMB"), ms.ullTotalVirtual >> 20, (void (*)(void *, unsigned))N.fn.dvar_set);
}
static int w_2a0a0_dummy;

static LONG CALLBACK veh(EXCEPTION_POINTERS *x)
{
	const DWORD c = x->ExceptionRecord->ExceptionCode;
	if (c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_ILLEGAL_INSTRUCTION || c == EXCEPTION_INT_DIVIDE_BY_ZERO || c == EXCEPTION_STACK_OVERFLOW) {
		static LONG n;
		if (InterlockedIncrement(&n) <= 8) {
			const uintptr_t ip = (uintptr_t)x->ExceptionRecord->ExceptionAddress;
			HMODULE m = NULL;
			char name[MAX_PATH] = "?";
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ip, &m) && m)
				GetModuleFileNameA(m, name, MAX_PATH);
			const char *file = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
			log_printf("native11: exception %lx at %s+0x%llx (access address %llx), caller return %llx", (unsigned long)c, file, (unsigned long long)(ip - (uintptr_t)m), (unsigned long long)(x->ExceptionRecord->NumberParameters > 1 ? x->ExceptionRecord->ExceptionInformation[1] : 0),
				   (unsigned long long)(x->ContextRecord->Rsp ? *(uintptr_t *)x->ContextRecord->Rsp - (uintptr_t)exe : 0));
		}
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

// QA: MW2COOP_PROFILE=1: a poor man's profiler. The render thread's instruction pointer is sampled every 2 ms; every 10 s the 12 hottest 64-byte blocks (module + offset) are logged.
static DWORD WINAPI profiler(LPVOID arg)
{
	(void)arg;
	enum { N = 4096 };
	static struct {
		uintptr_t addr;
		unsigned n;
		char mod[24];
	} h[N];
	for (;;) {
		memset(h, 0, sizeof(h));
		unsigned total = 0;
		for (int i = 0; i < 5000; i++) {
			Sleep(2);
			const DWORD tid = render_tid;
			if (!tid)
				continue;
			HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
			if (!th)
				continue;
			if (SuspendThread(th) != (DWORD)-1) {
				CONTEXT c;
				memset(&c, 0, sizeof(c));
				c.ContextFlags = CONTEXT_CONTROL;
				if (GetThreadContext(th, &c)) {
					HMODULE m = NULL;
					char nm[MAX_PATH] = "?";
					uintptr_t off = c.Rip;
					if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)c.Rip, &m) && m) {
						GetModuleFileNameA(m, nm, MAX_PATH);
						off = c.Rip - (uintptr_t)m;
					}
					const char *base = strrchr(nm, 92) ? strrchr(nm, 92) + 1 : nm;
					const uintptr_t blk = off & ~(uintptr_t)63;
					unsigned k = 0;
					while (k < N && h[k].n && !(h[k].addr == blk && !strncmp(h[k].mod, base, sizeof(h[k].mod) - 1)))
						k++;
					if (k < N) {
						if (!h[k].n) {
							h[k].addr = blk;
							lstrcpynA(h[k].mod, base, sizeof(h[k].mod));
						}
						h[k].n++;
					}
					total++;
				}
				ResumeThread(th);
			}
			CloseHandle(th);
		}
		// report the hottest blocks
		for (int r = 0; r < 12; r++) {
			unsigned best = 0, bi = N;
			for (unsigned k = 0; k < N && h[k].n; k++)
				if (h[k].n > best) {
					best = h[k].n;
					bi = k;
				}
			if (bi == N || !best)
				break;
			log_printf("native11: profile: %5.1f%% %s+0x%llx", total ? 100.0 * best / total : 0.0, h[bi].mod, (unsigned long long)h[bi].addr);
			h[bi].n = 0;
		}
		log_printf("native11: profile: %u samples", total);
	}
	return 0;
}

static DWORD WINAPI monitor(LPVOID arg)
{
	(void)arg;
	LONG last[sizeof(hits) / sizeof(hits[0])] = {0};
	for (;;) {
		Sleep(500);
		char line[1024] = "";
		for (size_t i = 0; i < sizeof(hits) / sizeof(hits[0]); i++)
			if (hits[i] != last[i]) {
				snprintf(line + strlen(line), sizeof(line) - strlen(line), " %s=%ld", hit_names[i], (long)hits[i]);
				last[i] = hits[i];
			}
		if (line[0])
			log_printf("native11: calls (totals):%s", line);
		{ // a draw function spinning (a loop that never ends): dump the threads once, after the second such interval
			static LONG o_spin, spins;
			if (hits[25] - o_spin > 3000000 && ++spins == 2)
				nbr_dump_threads(exe);
			o_spin = hits[25];
		}
		if (N.created && N.d) {
			static unsigned o_draws, o_refused;
			unsigned st, ly, sh, up, dr, rf;
			nd_stats(N.d, &st, &ly, &sh, &up, &dr, &rf);
			{ // each new reason a draw was refused for is logged once
				static char seen[8][96];
				static unsigned nseen;
				const char *e = nd_last_error(N.d);
				if (e && e[0]) {
					unsigned k = 0;
					while (k < nseen && strncmp(seen[k], e, 95))
						k++;
					if (k == nseen && nseen < 8) {
						lstrcpynA(seen[nseen++], e, 96);
						log_printf("native11: draw refused: %s", e);
					}
				}
			}
			if (dr != o_draws || rf != o_refused) {
				log_printf("native11: draws %u, refused %u, uploads %u, shaders %u, layouts %u, states %u; last refusal: %s", dr, rf, up, sh, ly, st, nd_last_error(N.d));
				o_draws = dr;
				o_refused = rf;
			}
		}
	}
	return 0;
}

static void install(uint32_t rva, void *fn)
{
	hook_jump(exe + rva, 14, fn, 0xcc);
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ call sites
// The game's own drawing functions that are not rewritten yet (0x60120 and the ones the tripwire names) still call a few Direct3D 9 device methods through the stored device pointer
// (`mov rax,[rcx]; call [rax+slot*8]`). Each such call instruction (6 bytes, ff 90 disp32) is made a `call rel32` to a stub that jumps to the native step with the same arguments: the device
// object stays a tripwire, nothing creates or calls a Direct3D 9 object. A temporary bridge: a function that is rewritten natively stops using its sites (its code is replaced by a jump).
static long s_set_render_state(void *dev, unsigned state, unsigned value)
{
	(void)dev;
	if (!N.created)
		return 0;
	if (state == 174) { // D3DRS_SCISSORTESTENABLE
		nd_set_scissor_enable(N.d, (int)value);
		return 0;
	}
	static volatile LONG seen_state[256];
	if (state < 256 && InterlockedExchange(&seen_state[state], 1) == 0)
		log_printf("native11: SetRenderState %u = %u (not mapped yet)", state, value);
	return 0;
}
static long s_set_texture(void *dev, unsigned stage, void *tex)
{
	(void)dev;
	if (N.created)
		ngf_set_texture_raw(&N.f, stage, tex);
	return 0;
}
static long s_set_scissor_rect(void *dev, const RECT *r)
{
	(void)dev;
	if (N.created && r)
		nd_set_scissor_rect(N.d, r->left, r->top, r->right, r->bottom);
	return 0;
}
static long s_draw_indexed(void *dev, unsigned type, int base_vertex, unsigned min_index, unsigned num_vertices, unsigned start_index, unsigned prim_count)
{
	(void)dev;
	(void)min_index;
	(void)num_vertices;
	if (!N.created)
		return 0;
	if (type != 4) { // D3DPT_TRIANGLELIST
		static volatile LONG warned;
		if (InterlockedExchange(&warned, 1) == 0)
			log_printf("native11: DrawIndexedPrimitive of type %u is not a triangle list", type);
		return 0;
	}
	nd_draw_indexed(N.d, &N.g, base_vertex, start_index, prim_count);
	return 0;
}
static long s_set_indices(void *dev, void *ib)
{
	(void)dev;
	if (N.created)
		nd_set_indices(N.d, (nat_buffer *)ib, 0);
	return 0;
}
// texture->GetSurfaceLevel(level, &surface): a native image is its own surface (with a reference, which the engine's Release gives back)
static long s_get_surface_level(void *tex, unsigned level, void **out)
{
	(void)level;
	if (out) {
		*out = tex;
		nat_image_addref((nat_image *)tex);
	}
	return 0;
}
// device->StretchRect(source surface, source rect, destination surface, destination rect, filter): a NULL surface is the scene target
static long s_stretch_rect(void *dev, void *src, const RECT *sr, void *dst, const RECT *dr, unsigned filter)
{
	(void)dev;
	if (N.created)
		nd_stretch(N.d, (nat_image *)src, (const nd_rect *)sr, (nat_image *)dst, (const nd_rect *)dr, filter == 2);
	return 0;
}
static long s_set_stream(void *dev, unsigned stream, void *vb, unsigned offset, unsigned stride)
{
	(void)dev;
	if (N.created)
		nd_set_stream(N.d, stream, (nat_buffer *)vb, offset, stride);
	return 0;
}

static void patch_sites(void)
{
	static const struct {
		unsigned slot;
		void *fn;
	} map[] = {{57, s_set_render_state}, {65, s_set_texture}, {75, s_set_scissor_rect}, {82, s_draw_indexed}, {100, s_set_stream}, {104, s_set_indices}, {18, s_get_surface_level}, {34, s_stretch_rect}};
	unsigned patched[10] = {0};
	uint8_t *page = hook_page_near(exe);
	if (!page)
		return;
	// one 14-byte absolute-jump stub per method
	for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		hook_jump_bytes(page + i * 16, map[i].fn);
	DWORD old;
	VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &old);
	for (uint32_t rva = 0x40000; rva < 0x66000; rva++) {
		uint8_t *p = exe + rva;
		// `call [reg + disp32]`: ff 90..97 (6 bytes) or 41 ff 90..97 (7 bytes, r8..r15)
		unsigned len = 0;
		if (p[0] == 0xff && (p[1] & 0xf8) == 0x90)
			len = 6;
		else if (p[0] == 0x41 && p[1] == 0xff && (p[2] & 0xf8) == 0x90)
			len = 7;
		if (!len)
			continue;
		int32_t disp;
		memcpy(&disp, p + len - 4, 4);
		if (disp <= 0 || (disp & 7))
			continue;
		const unsigned slot = (unsigned)disp / 8;
		for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
			if (map[i].slot != slot)
				continue;
			const int64_t rel = (int64_t)(page + i * 16) - (int64_t)(p + 5);
			if (rel > INT32_MAX || rel < INT32_MIN)
				break;
			DWORD op;
			VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &op);
			p[0] = 0xe8;
			const int32_t r32 = (int32_t)rel;
			memcpy(p + 1, &r32, 4);
			for (unsigned k = 5; k < len; k++)
				p[k] = 0x90;
			VirtualProtect(p, len, op, &op);
			patched[i]++;
			break;
		}
	}
	FlushInstructionCache(GetCurrentProcess(), exe + 0x40000, 0x26000);
	log_printf("native11: device call sites patched: SetRenderState %u, SetTexture %u, SetScissorRect %u, DrawIndexedPrimitive %u, SetStreamSource %u, SetIndices %u, GetSurfaceLevel %u, StretchRect %u", patched[0], patched[1], patched[2], patched[3], patched[4], patched[5], patched[6], patched[7]);
}

static void install_all(void);

// the exe is packed (SteamStub) when this DLL loads: the code can only be patched after the stub has unpacked it, which is when the first bytes of a known function look right
static DWORD WINAPI waiter(LPVOID arg)
{
	(void)arg;
	static const uint8_t head[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20}; // rva 0x54a10
	for (int i = 0; i < 60000; i++) {
		if (!IsBadReadPtr(exe + 0x54a10, sizeof(head)) && memcmp(exe + 0x54a10, head, sizeof(head)) == 0) {
			install_all();
			return 0;
		}
		Sleep(1);
	}
	log_printf("native11: the exe never looked unpacked, nothing installed");
	return 0;
}

// QA (MW2COOP_WHITEPOLL=1): when does the engine register the default image $white (asset lookup 0xef500 without the default fallback)? Polled every 2 ms from the start.
static DWORD WINAPI whitepoll_thread(LPVOID unused)
{
	(void)unused;
	uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
	uint8_t *(*raw_find)(int, const char *) = (uint8_t *(*)(int, const char *))(base + 0xef500);
	static const uint8_t head[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20};
	while (IsBadReadPtr(base + 0x54a10, sizeof(head)) || memcmp(base + 0x54a10, head, sizeof(head)) != 0) // the exe is SteamStub packed: wait for its unpacked code
		Sleep(1);
	const DWORD t0 = GetTickCount();
	log_printf("native11: whitepoll: the exe is unpacked, polling for $white");
	for (;;) {
		uint8_t *hit = raw_find(10, "$white"); // (a walk over the hash chains: harmless on the empty tables of an early engine)
		if (hit) {
			log_printf("native11: whitepoll: $white registered %lu ms after the poll started (header %p)", (unsigned long)(GetTickCount() - t0), (void *)hit);
			return 0;
		}
		if (GetTickCount() - t0 > 60000)
			return 0;
		Sleep(2);
	}
}

void natboot_install(void)
{
	if (getenv("MW2COOP_WHITEPOLL"))
		CloseHandle(CreateThread(NULL, 0, whitepoll_thread, NULL, 0, NULL));
#ifndef NAT_BACKEND_12
	if (settings_directx12()) { // the Direct3D 12 copy of this file (build.sh: b12_ prefix)
		extern void b12_natboot_install(void);
		b12_natboot_install();
		return;
	}
#endif
	if (!settings_directx11() && !settings_directx12()) // [display] directx11=1 / directx12=1 or MW2COOP_NATIVE11=1 / MW2COOP_NATIVE12=1
		return;
	exe = (uint8_t *)GetModuleHandleA(NULL);
	CloseHandle(CreateThread(NULL, 0, waiter, NULL, 0, NULL));
}

static void install_all(void)
{
	log_printf("native11: the exe looks unpacked, installing");
	if (getenv("MW2COOP_NATIVE11_DRY")) {
		log_printf("native11: dry run, nothing installed");
		return;
	}
	IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
	if (nt->OptionalHeader.SizeOfImage < 0x4585400) {
		log_printf("native11: not the retail exe, off");
		return;
	}
	memset(&N, 0, sizeof(N));
	nbr_bind(&N, exe);
	nat_log_hook = log_printf;
	AddVectoredExceptionHandler(1, veh);
	CloseHandle(CreateThread(NULL, 0, monitor, NULL, 0, NULL));
	if (getenv("MW2COOP_PROFILE"))
		CloseHandle(CreateThread(NULL, 0, profiler, NULL, 0, NULL));
	make_traps();
	N.sentinel = trap_object;
	// the frame helpers' function pointers
	N.frame.f = &N.f;
	N.frame.r = &N.r;
	N.frame.fn_clear_modified = N.fn.clear_modified;
	N.frame.fn_default_states = (void (*)(void))(exe + 0x4f760);
	N.frame.fn_21570 = (int (*)(void *))(exe + 0x21570);
	N.frame.fn_211c0 = (void (*)(void *, void *))(exe + 0x211c0);
	N.frame.fn_29070 = (void (*)(void))(exe + 0x29070);
	N.frame.fn_b6e0 = (void (*)(int))(exe + 0xb6e0);
	N.frame.fn_5c010 = (void (*)(void *))(exe + 0x5c010);
	N.frame.fn_484c0 = (void (*)(void *))(exe + 0x484c0); // the post-process composite (glow, depth of field, fog): the game's own code, its StretchRect / GetSurfaceLevel calls are bridged (patch_sites)
	N.frame.fn_4e390 = (void (*)(void *, void *, int))(exe + 0x4e390);
	N.frame.fn_4f070 = (void (*)(void *, void *, void *))(exe + 0x4f070);
	N.frame.fn_4ec20 = (void (*)(void *))(exe + 0x4ec20);
	N.frame.fn_45320 = (void (*)(void *))(exe + 0x45320);
	N.frame.fn_26a60 = (void (*)(void))(exe + 0x26a60);
	N.f.fn_2d_constants = (void (*)(void *))(exe + 0x4ec20);
	N.f.fn_viewport_rect = (void (*)(void *, int *))(exe + 0x50ba0);
	N.f.fn_projection = (void (*)(void *, int *))(exe + 0x50d70);
	N.f.fn_5c8d0 = (void (*)(void *))(exe + 0x5c8d0);
	N.f.fn_5d650 = (void (*)(void *, void *, int))(exe + 0x5d650);
	N.f.fn_4fbe0 = (void (*)(void *, void *, int, int))(exe + 0x4fbe0);
	N.f.fn_5dd00 = (void (*)(void *, void *, void *))(exe + 0x5dd00);
	N.f.fn_60120 = (void (*)(void *, void *, int))(exe + 0x60120);
	N.f.fn_637a0 = (void (*)(void *, void *))(exe + 0x637a0);
	N.f.fn_efbe0 = (void (*)(uint8_t, void *, void **, int *))(exe + 0xefbe0);
	N.f.fn_efdd0 = (void (*)(uint8_t, void *, void **, int *))(exe + 0xefdd0);
	// pass apply and state
	install(0x54a10, w_54a10);
	install(0x54940, w_54940);
	install(0x54070, w_54070);
	install(0x543b0, w_543b0);
	install(0x53d00, w_53d00);
	install(0x53f10, w_53f10);
	install(0x54220, w_54220);
	install(0x50a20, w_50a20);
	install(0x50b10, w_50b10);
	install(0x50c60, w_50c60);
	install(0x3cc00, w_3cc00);
	install(0x50ec0, w_50ec0);
	install(0x515a0, w_515a0);
	install(0x26730, w_26730);
	install(0x54820, w_54820);
	install(0x54b30, w_54b30);
	install(0x504d0, w_504d0);
	install(0x51070, w_51070);
	// draw loops
	install(0x26840, w_26840);
	install(0x399f0, w_399f0);
	install(0x5c960, w_5c960);
	install(0x5cc10, w_5cc10);
	install(0x5f220, w_5f220);
	install(0x5fe50, w_5fe50);
	install(0x63850, w_63850);
	install(0x3ec20, w_3ec20);
	install(0x5da10, w_5da10);
	install(0x5dde0, w_5dde0);
	install(0x63f50, w_63f50);
	install(0x266a0, w_266a0);
	install(0x267b0, w_267b0);
	install(0x267e0, w_267e0);
	install(0x26810, w_26810);
	// resources
	install(0x492d0, w_492d0);
	install(0x49170, w_49170);
	install(0x23920, w_23920);
	install(0x8f60, w_8f60);
	install(0x9070, w_8f60);
	install(0x258a0, w_258a0);
	install(0x24f00, w_24f00);
	install(0x2b7840, w_2b7840);
	install(0x2b7980, w_2b7980);
	install(0x4d100, w_4d100);
	patch_sites();
	install(0x23950, w_23950);
	install(0x23a90, w_23a90);
	install(0x23be0, w_23be0);
	install(0x24d00, w_24d00);
	install(0x24df0, w_24df0);
	install(0x44d00, w_44d00);
	install(0x453c0, w_453c0);
	install(0x10410, w_10410);
	install(0x45010, w_45010);
	install(0x2f800, w_2f800);
	install(0x10de0, w_10de0);
	install(0x39420, w_39420);
	install(0x45060, w_45060);
	// the loose render / sampler state setters: the native back end derives its state from the words at every draw
	install(0x51280, w_noop);
	install(0x4f660, w_noop);
	install(0x4f6e0, w_noop);
	install(0x4e940, w_noop);
	install(0x4f510, w_noop);
	install(0x505a0, w_noop);
	install(0x50810, w_noop);
	install(0x511d0, w_noop);
	install(0x4f380, w_4f380);
	install(0x2906b0, w_sys_error);
	install(0x242a0, w_242a0);
	(void)w_2a0a0_dummy;
	log_printf("native11: the native renderer is installed (%u functions redirected); the game's Direct3D 9 device is a tripwire", 52u);
}

void *natboot_trap_device(void)
{
	return trap_object;
}
int natboot_active(void)
{
	return settings_directx11() || settings_directx12();
}
#else
void natboot_install(void) {}
void *natboot_trap_device(void) { return 0; }
int natboot_active(void) { return 0; }
#endif
