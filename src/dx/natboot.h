#pragma once
// MW2COOP_NATIVE11=1: the native renderer in place of the game's Direct3D 9 back end (see natboot.c).
void natboot_install(void);
void *natboot_trap_device(void);
