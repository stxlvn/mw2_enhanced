#pragma once
// The globals the game's device start (0x10550, 0x10260, 0xef10) writes, filled from the native device instead of from Direct3D 9 caps (docs/native-dx-plan.md, "Device creation and caps").
// The orchestration of the start itself (creating the device, the fixed resources, the default images, the render-target table) is the next slice; this file is its table of facts.
#include <stdint.h>

#include "nat11.h"

typedef struct {
	uint8_t *exe;                                  // the game's image base
	void (*set_aspect_dvar)(void *dvar, int flag); // 0x26d800 (the game's own function, called with the dvar at rva 0x1325c38)
} ngs;

// 0xef10: the capability globals and the depth / render-target format decision (the success case: hardware depth textures)
void ngs_caps_globals(ngs *s, const nat_caps *c);
// 0x10260 / 0x10550: the monitor / desktop sizes and the display parameters, with the aspect choice. `windowed` is the game's flag at params + 0xc (1 windowed, 0 full screen); width / height the
// back buffer size; win_w / win_h the values the game keeps at params + 0x18 / + 0x1c.
// 0x242a0: the video-memory estimate and the quality settings derived from it. vram_mb: the adapter's dedicated memory, sys_mb: the "sys_sysMB" dvar, total_virtual_mb: the process's address space
// (GlobalMemoryStatusEx); set_dvar is the game's own 0x26e020 (dvar, value). The three quality dvars are at rva 0x4116288, 0x4116330, 0x41162b0.
void ngs_memory_setup(ngs *s, unsigned vram_mb, unsigned sys_mb, uint64_t total_virtual_mb, void (*set_dvar)(void *dvar, unsigned value));
void ngs_display_globals(ngs *s, const nat_caps *c, int windowed, unsigned width, unsigned height, unsigned win_w, unsigned win_h);
