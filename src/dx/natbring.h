#pragma once
// The bring-up chain of the native renderer: the replacement for the game's device creation entry 0x10de0 and what it calls (0xef10 caps, 0xfeb0 mode lists, 0x10a90 display parameters, 0x10260 /
// 0x10550 device start, 0xf760 fixed resources, the render-target table 0x39420). No Direct3D 9 factory or device is created; the globals that held them (rva 0x410fd18, 0x410fd20) are left empty
// or hold the native device as a sentinel. Everything the game's own code does around it (dvars, windows, memory) is called through function pointers into the exe.
#include <stddef.h>
#include <stdint.h>

#include "natdrawfn.h"
#include "natframe.h"
#include "natres.h"
#include "natstart.h"

typedef struct {
	int (*dvar_int)(const char *name);                                                           // 0x26c010
	int (*dvar_bool)(const char *name);                                                          // 0x26bf90
	void (*dvar_set)(void *dvar, int value);                                                     // 0x26e020
	const char *(*dvar_string)(void *dvar);                                                      // 0x26bd80
	int (*scan)(const char *text, const char *fmt, ...);                                          // 0x0ece0
	int (*print)(char *buf, const char *fmt, ...);                                                // 0x0ec80
	void *(*reg_enum)(const char *name, const char **strings, int def, unsigned flags, const char *desc); // 0x26cb10
	void (*dvar_reset_domain)(void *dvar);                                                       // 0x26c8d0
	void (*dvar_set_enum)(void *dvar, const char **strings);                                      // 0x26ed50
	void (*qsort_)(void *base, size_t n, size_t size, void *cmp);                                 // 0x2f6ab0
	void (*com_error)(const char *fmt, ...);                                                     // 0x273440
	void (*quit)(void);                                                                          // 0x290630
	void (*clear_modified)(void *dvar);                                                          // 0x26bcf0
	void (*mem_zero)(void *p, int v, size_t n);                                                  // 0x2e42d0 (memset)
	void *(*alloc)(size_t size);                                                                 // 0x2699a0
	void *(*alloc_named)(size_t size, const char *name);                                         // 0x269b60
	void (*register_callback)(void (*cb)(void));                                                 // 0x203b80
	void (*after_window)(void);                                                                  // 0x2940c0
	void (*lost_resources)(void);                                                                // 0x0f3f0 (device-lost resource list; not needed natively, kept for symmetry)
	void (*fn_4e390)(void *a, void *b, int c);                                                   // 0x4e390
	void (*fn_14bee0)(void);                                                                     // 0x0bee0
	void (*fn_28690)(int a);                                                                     // 0x28690
	void (*fn_4e940)(void);                                                                      // 0x4e940 (default sampler / render states: native version resets the shadow)
	void (*fn_4f760)(void);                                                                      // 0x4f760
	void (*fn_dvar_set_int)(void *dvar, int value);                                               // 0x26e020 (sets a dvar's value)
	void (*fn_f3f0)(void);                                                                       // 0x0f3f0 (requests the common zones: DB_LoadXAssets of the gfx zones, $white ...)
	void (*fn_f570)(void);                                                                       // 0x0f570 (default images, materials)
	void (*fn_e340)(void *dvar);                                                                 // 0x26e340
	void (*fn_203b00)(int v);                                                                    // 0x203b00
	void *enum_monitors_cb;                                                                      // exe + 0x103f0 (MONITORENUMPROC)
} nbr_fn;

typedef struct {
	uint8_t *exe;
	nbr_fn fn;
	ngs ngs_;
	nat_device *dev;
	nd *d;
	ngf f;
	ng_state g;
	ng_ops gops;
	ngr r;
	ngfr pace;
	ngfr_frame frame;
	int created;
	void *sentinel; // what the game's device globals hold: the tripwire object of natboot
} nbr;

// binds the function pointers to the exe (base + rva)
void nbr_bind(nbr *n, uint8_t *exe);
// 0xfeb0: lists the display modes into the game's table (rva 0x410fd44 count, 0x410fd48 entries of {w, h, hz, 0x16}) and registers / refreshes the r_mode and r_displayRefresh dvars
void nbr_list_modes(nbr *n);
// 0x10a90: the display parameters record (hwnd at +0, refresh +8, display mode +0xc, x/y +0x10/+0x14, window size +0x18/+0x1c, size +0x20/+0x24, samples +0x28)
void nbr_display_params(nbr *n, uint8_t *params);
// 0x10550 (with 0x10260, 0xef10, 0xf760): creates the native device and everything the game's start makes; returns 1 on success
int nbr_start(nbr *n, uint8_t *params);
// 0x10de0: the device creation entry: monitor choice, caps, modes, parameters, the window, the start, with the game's fallback to smaller settings when it fails
void nbr_init(nbr *n);

// 0x39420 (with 0x391d0, 0x39110, 0x39310): the render-target table at rva 0x4359000: the saved-screen image, the back buffer entries, the scene-size targets, the shadow maps; entries hold native images
void nbr_targets(nbr *n);
