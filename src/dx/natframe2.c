// The frame start and end functions of the native back end (0x44d00, 0x453c0), see natframe.h.
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "natframe.h"
#include "nat11.h"
#include "../log.h"

int ngfr_trace_scene; // QA: the frame end's preconditions are logged while set

// QA (tracescene): the back end's frame end and the frame's 2D list are entered at all, and from where: every call to ngfr_453c0, its first 400; the stack's
// return addresses into the game's image name the caller
static void trace_entry(const char *what, unsigned n, uint8_t *exe)
{
	if (n > 400 && n % 400 != 0)
		return;
	void *fr[6];
	const USHORT c = RtlCaptureStackBackTrace(1, 6, fr, NULL);
	char line[200];
	int len = snprintf(line, sizeof(line), "native: trace %s #%u from", what, n);
	for (USHORT i = 0; i < c && len < (int)sizeof(line) - 12; i++) {
		const uintptr_t off = (uintptr_t)fr[i] - (uintptr_t)exe;
		len += snprintf(line + len, sizeof(line) - len, " %lx", (unsigned long)(off < 0x4000000 ? off : 0));
	}
	if (nat_log_hook)
		nat_log_hook("%s", line);
}

#define AT(p, off) ((uint8_t *)(p) + (off))
#define RD_PTR(p, off) (*(void **)AT(p, off))
#define RD_U8(p, off) (*(uint8_t *)AT(p, off))
#define RD_U16(p, off) (*(uint16_t *)AT(p, off))
#define RD_I32(p, off) (*(int32_t *)AT(p, off))
#define RD_U32(p, off) (*(uint32_t *)AT(p, off))
#define RD_F32(p, off) (*(float *)AT(p, off))

enum {
	G_SCENE = 0x4358f58, G_SCENE_MODE = 0x4358f60, G_STATE_OBJ = 0x4357760, G_CTX_PTRS = 0x36fc70, G_FRAME_COUNT = 0x411375c, G_DEVICE_LOST = 0x4113270, G_IN_SCENE = 0x4113271, G_VB_RING_BUFFER = 0x4116520,
	G_TABLE_STRIDE = 0x370400, G_TABLE_VTX = 0x4826683, G_FRAME_STATE_COPY = 0x481b750, G_LIGHT_SRC = 0x481b770, G_LIGHT_FLAG = 0x481b76c, G_PARTICLES = 0x4115920, G_MODE_DVAR = 0x4116308,
	G_LIGHT_VOLUME = 0x488a2e8,
};

// the dvars whose modified flag (+0xd) makes 0x44d00 set the defaults again
static const unsigned MODIFIED_DVARS[5] = {0x41163f8, 0x41163e8, 0x4116310, 0x41162a0, 0x4116378};

void ngfr_44d00(ngfr_frame *x, uint8_t *scene)
{
	uint8_t *e = x->f->exe;
	RD_PTR(e, G_SCENE) = scene;
	if (!(RD_U8(scene, 0x41a60) & 1))
		return;
	RD_I32(e, G_FRAME_COUNT)++;
	if (RD_U8(e, G_DEVICE_LOST) == 0) {
		for (int i = 0; i < 5; i++) {
			void *dvar = RD_PTR(e, MODIFIED_DVARS[i]);
			if (RD_U8(dvar, 0xd)) { // the modified one is acknowledged, then the defaults are set again
				if (x->fn_clear_modified)
					x->fn_clear_modified(dvar);
				if (x->fn_default_states)
					x->fn_default_states();
				break;
			}
		}
		// (alpha to coverage: the state 181 of the Direct3D 9 path is a driver hack; the native device reports no support, so nothing is set)
		RD_F32(e, 0x4359460) = RD_F32(RD_PTR(e, G_MODE_DVAR), 0x10);
	}
	// the frame's dynamic vertices go into the big vertex ring in pieces (no-overwrite)
	const unsigned n = RD_U32(scene, 0x41930);
	if (n) {
		nat_buffer *vb = RD_PTR(e, G_VB_RING_BUFFER);
		uint8_t *base = nat_buffer_map(vb, NAT_MAP_NOOVERWRITE);
		unsigned cursor = 0;
		for (unsigned i = 0; i < n; i++) {
			const uint16_t idx = (uint16_t)(RD_U16(scene, 0x41730 + i * 2) - 1);
			const unsigned type = RD_U8(e, G_TABLE_VTX + (size_t)idx * 8);
			const unsigned count = RD_U32(e, G_TABLE_STRIDE + (size_t)type * 4);
			const unsigned off = RD_U32(e, G_TABLE_VTX + 1 + (size_t)idx * 8) << 5, size = count << 5;
			if (base)
				memcpy(base + off, scene + 0x1730 + (size_t)cursor * 0x20, size);
			cursor += count;
		}
		if (base)
			nat_buffer_unmap(vb);
	}
	uint8_t *p = scene + 0x4193c;
	if (x->fn_21570 && x->fn_21570(p)) {
		memcpy(AT(e, G_FRAME_STATE_COPY), p, 0x1c);
		if (RD_I32(p, 0) == 0) {
			uint8_t *dst = (uint8_t *)RD_PTR(RD_PTR(e, G_PARTICLES), 0x158) + 0xa8;
			memcpy(dst, AT(e, G_LIGHT_SRC), 0xa8);
			RD_I32(e, G_LIGHT_FLAG) = 1;
		} else if (x->fn_211c0) {
			x->fn_211c0(p + 4, p + 0x10);
		}
	}
	if (x->r)
		ngr_model_lighting(x->r, RD_PTR(e, G_LIGHT_VOLUME), scene + 0x1096a8, RD_U32(scene, 0x41938));
	RD_U8(e, G_IN_SCENE) = 1; // (BeginScene)
	if (x->fn_29070)
		x->fn_29070();
	if (x->fn_b6e0)
		x->fn_b6e0(0);
	*(uint64_t *)AT(e, 0x4584e48) = 0;
}

// the common tail of the view loops: the 2D state object is re-initialised and the target switched to the scene
static void reset_2d(ngfr_frame *x, uint8_t *scene)
{
	uint8_t *e = x->f->exe;
	void *ctx[2] = {RD_PTR(e, G_CTX_PTRS), RD_PTR(e, G_CTX_PTRS + 8)};
	RD_I32(e, 0x4358e90) = RD_I32(e, 0x4359038);
	RD_I32(e, 0x4358e8c) = 0;
	RD_I32(e, 0x4358e94) = RD_I32(e, 0x435903c);
	ngf_target_switch(x->f, ctx[0], ctx[1], 1);
	(void)scene;
}

void ngfr_453c0(ngfr_frame *x)
{
	{ static unsigned n; if (ngfr_trace_scene) trace_entry("453c0", n++, x->f->exe); }
	{ if (ngfr_trace_scene) { uint8_t *sc = RD_PTR(x->f->exe, G_SCENE); log_printf("native: 453c0 flags 0x%x scenes %u views %u mode %d viewsz %u", sc ? RD_U8(sc, 0x41a60) : 0, sc ? RD_U32(sc, 0x41a34) : 0, sc ? RD_U32(sc, 0x41a34) : 0, RD_I32(x->f->exe, G_SCENE_MODE), sc ? RD_U32(sc, 0x419e8) : 0); } }

	uint8_t *e = x->f->exe;
	uint8_t *scene = RD_PTR(e, G_SCENE);
	if (!(RD_U8(scene, 0x41a60) & 2))
		return;
	const unsigned views = RD_U32(scene, 0x41a34);
	const int mode = RD_I32(e, G_SCENE_MODE);
	if (views != 0 && mode != 0) {
		uint8_t *list = RD_PTR(scene, 0x41a38);
		if (mode == 1 || mode == 2) {
			for (unsigned i = 0; i < views; i++) {
				uint8_t *view = list + (size_t)i * 0x13a0;
				RD_PTR(view, 0x1378) = scene;
				if (x->fn_5c010)
					x->fn_5c010(view);
			}
		} else {
			for (unsigned i = 0; i < views; i++) {
				uint8_t *view = list + (size_t)i * 0x13a0;
				RD_PTR(view, 0x1378) = scene;
				if (RD_I32(view, 0x1380) == 2) {
					{ extern int nd_trace; if (nd_trace) log_printf("trace: ==== view %u of %u begin", i, views); }
					if (x->fn_484c0)
						x->fn_484c0(view);
					if (x->fn_5c010)
						x->fn_5c010(view);
				}
			}
			scene = RD_PTR(e, G_SCENE);
			uint8_t *first = RD_PTR(scene, 0x41a38);
			if (x->fn_4e390)
				x->fn_4e390(AT(e, G_STATE_OBJ), first + 0xdc0, 0);
			reset_2d(x, scene);
			if (x->fn_4f070)
				x->fn_4f070(AT(e, G_STATE_OBJ), first + 0x180, first);
			RD_I32(e, 0x4358e40) = 0;
			RD_U8(e, 0x4358e98) = 1;
			memcpy(AT(e, 0x4358e78), first + 0x160, 16);
			if (x->fn_4ec20)
				x->fn_4ec20(AT(e, G_STATE_OBJ));
		}
	}
	scene = RD_PTR(e, G_SCENE);
	if (x->fn_4e390)
		x->fn_4e390(AT(e, G_STATE_OBJ), AT(e, 0x4356600), 0);
	RD_PTR(e, 0x4358b18) = scene;
	memset(AT(e, 0x4356d10), 0, 0xa00);
	reset_2d(x, scene);
	RD_I32(e, 0x4358e80) = RD_I32(e, 0x4113728);
	RD_I32(e, 0x4358e84) = RD_I32(e, 0x411372c);
	RD_PTR(e, 0x4358e78) = NULL;
	void *cb = RD_PTR(scene, 0x41a40);
	if (cb && x->fn_45320)
		x->fn_45320(cb);
	if (RD_I32(e, 0x4584e4c) != 0 && x->fn_26a60)
		x->fn_26a60();
	if (RD_PTR(e, 0x4356c98) != NULL) { // the warm-up state's index buffer cache: cleared (SetIndices(NULL))
		RD_PTR(e, 0x4356c98) = NULL;
		nd_set_indices(x->f->d, NULL, 0);
	}
	ngf_clear_streams(x->f, AT(e, 0x4356c90 - 0xd0));
	RD_U8(e, G_IN_SCENE) = 0; // (EndScene)
}

void ngfr_stretch_current_to_saved(ngfr_frame *x, void *state, uint8_t *dst_image)
{
	uint8_t *e = x->f->exe;
	const int cur = RD_I32(state, 0xb98);
	nat_image *src = cur >= 0 ? (nat_image *)RD_PTR(AT(e, 0x4359000 + (size_t)cur * 32), 8) : NULL;
	nd_stretch(x->f->d, src, NULL, (nat_image *)RD_PTR(dst_image, 0), NULL, 1);
}
