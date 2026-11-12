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
