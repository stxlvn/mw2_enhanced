#pragma once
// MW2COOP_NATIVE11=1: the native renderer in place of the game's Direct3D 9 back end (see natboot.c).
void natboot_install(void);
void *natboot_trap_device(void);
// 1 when the native renderer is on (MW2COOP_NATIVE11): the mod's own users of the game's Direct3D 9 textures and device (fonts atlas, screenshots, ...) must stay off then
#ifdef _WIN64
int natboot_active(void);
// QA / the console command !shot: the next presented frame goes to <game>\mw2coop\shots\<name>.bmp (name already cleaned)
void natboot_shot(const char *name);
#else
static inline int natboot_active(void) { return 0; } // the native renderer is 64-bit only
static inline void natboot_shot(const char *name) { (void)name; }
#endif
