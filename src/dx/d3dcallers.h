#pragma once
// QA, MW2COOP_D3DCALLERS=1: records which code calls which Direct3D 9 method (see d3dcallers.c).
struct IDirect3DDevice9;
void d3dcallers_hook_device(struct IDirect3DDevice9 *dev);
