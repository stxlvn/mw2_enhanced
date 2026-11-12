#pragma once
// QA, MW2COOP_NATCHECK=1: natgfx (the new back end's apply half) next to the game's own pass-apply functions, compared at every draw (see natcheck.c).
struct IDirect3DDevice9;
void natcheck_hook_device(struct IDirect3DDevice9 *dev);
