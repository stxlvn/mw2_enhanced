#pragma once
// Frame pacing and presenting of the native back end (docs/native-dx-plan.md, M5): the event-query ring of the game (rva 0x480a900 .. 0x480a920) and the functions that use it (0xc4c0's wait loop,
// 0x10410, 0x2f800, 0x2f860, 0x45010, 0x45060's present-and-issue, 0x4d100's drain). The queries are nat_query objects in the game's own arrays (the 32 pooled event queries at rva 0x4113158, the
// ring of the last 8 issued at rva 0x480a920).
#include <stdint.h>

#include "nat11.h"

typedef struct {
	nat_device *dev;
	uint8_t *exe;
	void (*sleep_ms)(unsigned ms); // Sleep (a test hook; NULL = Sleep(1) of the platform)
} ngfr;

// the game's globals as offsets (rva)
enum { NGFR_FRAME = 0x480a900, NGFR_ISSUED = 0x480a908, NGFR_RETIRED = 0x480a910, NGFR_RING = 0x480a920, NGFR_POOL = 0x4113158, NGFR_POOL_INDEX = 0x4113258, NGFR_LAST_EVENT = 0x41132d0 };

// 0x10410: retires completed queries from the ring (stops at the first one that is still pending)
void ngfr_retire(ngfr *r);
// 0xc4c0's first half: retires, then waits while 3 or more frames are in flight, then counts this frame (0x480a900)
void ngfr_wait_frames(ngfr *r);
// 0x45060: presents (nat_present), issues the next pooled event query at the end of the frame and puts it in the ring; resets the index ring offset the game keeps at rva 0x4116580's target
int ngfr_present_and_issue(ngfr *r);
// 0x5b7f0 / 0x45060's issue part: takes the next pooled query, issues it, stores it at *slot (the scene data's +0x419c8 field, or the "last event" global); returns the query
nat_query *ngfr_issue_next(ngfr *r, void **slot);
// 0x2f800 / 0x45010: is the query in *slot still pending? (0x2f800: pending = true; 0x45010 wants "done": !pending, a lost device counts as done)
int ngfr_pending(const ngfr *r, void *slot_value);
// 0x4d100: waits until the query is done (the first frame after a restart)
void ngfr_drain(ngfr *r, nat_query *q);

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ the frame start / end
#include "natdrawfn.h"
#include "natres.h"

// the game's own CPU functions the two frame functions call, and the native parts they use (every pointer may be NULL in a test: the step is skipped)
typedef struct {
	ngf *f;
	ngr *r;
	void (*fn_clear_modified)(void *dvar);   // 0x26bcf0
	void (*fn_default_states)(void);         // 0x4f760
	int (*fn_21570)(void *piVar8);           // 0x21570
	void (*fn_211c0)(void *a, void *b);      // 0x211c0
	void (*fn_29070)(void);                  // 0x29070
	void (*fn_b6e0)(int arg);                // 0xb6e0
	void (*fn_5c010)(void *view);            // 0x5c010
	void (*fn_484c0)(void *view);            // 0x484c0 (native: ngfr_484c0)
	void (*fn_4e390)(void *a, void *b, int c); // 0x4e390
	void (*fn_4f070)(void *a, void *b, void *c); // 0x4f070
	void (*fn_4ec20)(void *a);               // 0x4ec20
	void (*fn_45320)(void *a);               // 0x45320
	void (*fn_26a60)(void);                  // 0x26a60
} ngfr_frame;

// 0x44d00: the start of a frame's drawing: the dynamic vertex upload into the ring, the lighting volume, the frame state copies
void ngfr_44d00(ngfr_frame *x, uint8_t *scene);
// 0x453c0: the end of the frame's view loops (each view's composite and its post step), the reset of the 2D state and the index / stream caches; there is no EndScene
void ngfr_453c0(ngfr_frame *x);

// the one Direct3D step of 0x484c0, 0x41160 and 0x5c240: StretchRect of the current render target (table at rva 0x4359000, entry state+0xb98, field +8) into the saved-screen image `dst_image`
// (a game image, +0 the native image), linear filtered; 0x484c0 does it once per frame while the cache at rva 0x4358a70 is empty and then stores `dst_image` there
void ngfr_stretch_current_to_saved(ngfr_frame *x, void *state, uint8_t *dst_image);
