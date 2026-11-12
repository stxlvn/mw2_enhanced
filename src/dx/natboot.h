#pragma once
// MW2COOP_NATIVE11=1: the native renderer in place of the game's Direct3D 9 back end (see natboot.c).
void natboot_install(void);
void *natboot_trap_device(void);
// 1 when the native renderer is on (MW2COOP_NATIVE11): the mod's own users of the game's Direct3D 9 textures and device (fonts atlas, screenshots, ...) must stay off then
#ifdef _WIN64
int natboot_active(void);
#else
static inline int natboot_active(void) { return 0; } // the native renderer is 64-bit only
#endif
