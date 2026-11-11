#pragma once
// QA, MW2COOP_D3D11MIRROR=1 (64-bit retail exe only): replays the game's Direct3D 9 draws on a Direct3D 11 device and compares the pictures (docs/native-dx-plan.md, M2).
struct IDirect3DDevice9;
void mirror11_hook_device(struct IDirect3DDevice9 *dev);
