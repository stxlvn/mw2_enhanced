#pragma once
// QA, MW2COOP_STATEPROBE=1 (64-bit retail exe only): checks src/dx/state11.c against the game. The probe watches every SetRenderState the game makes on its Direct3D 9 device and, after each
// pass set-up (rva 0x54a10), compares what the device has with what state11 predicts from the pass's two state words. The totals go to the log.
struct IDirect3DDevice9;
void stateprobe_hook_device(struct IDirect3DDevice9 *dev);
