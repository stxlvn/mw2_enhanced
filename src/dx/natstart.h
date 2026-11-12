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
void ngs_display_globals(ngs *s, const nat_caps *c, int windowed, unsigned width, unsigned height, unsigned win_w, unsigned win_h);
