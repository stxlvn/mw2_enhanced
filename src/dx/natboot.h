#pragma once
// MW2COOP_NATIVE11=1: the native renderer in place of the game's Direct3D 9 back end (see natboot.c).
void natboot_install(void);
void *natboot_trap_device(void);
// 1 when the native renderer is on (MW2COOP_NATIVE11): the mod's own users of the game's Direct3D 9 textures and device (fonts atlas, screenshots, ...) must stay off then
#ifdef _WIN64
int natboot_active(void);
// QA / the console command !shot: the next presented frame goes to <game>\mw2coop\shots\<name>.bmp (name already cleaned)
void natboot_shot(const char *name);
// the split screen's 2D layers on the native renderers (natboot.c): the images exist (1), the scene's 2D pass is drawn into the layer of a half (begin / end)
void natboot_scene_size(unsigned *w, unsigned *h); // the scene's size (0 before the device exists)
int natboot_comp_make(void);
void natboot_layer_begin(int half, int clear);
void natboot_layer_end(void);
// FSR 1 with a crisp HUD: begin upscales the 3D picture and points the 2D draws at the output-size image (1 when it did), end puts the targets back
int natboot_hud_begin(void);
void natboot_hud_end(void);
// textures of the active native renderer for the mod's own code (src/modtex.c)
void *natboot_tex_create(unsigned w, unsigned h, unsigned levels, unsigned d3dfmt);
int natboot_tex_upload(void *img, unsigned level, const void *data, unsigned pitch);
int natboot_tex_download(void *img, unsigned level, void **out, unsigned *pitch, unsigned *rows);
int natboot_tex_info(void *img, unsigned *w, unsigned *h, unsigned *levels, unsigned *max_size, unsigned *fmt);
void natboot_tex_release(void *img);
#else
static inline int natboot_active(void) { return 0; } // the native renderer is 64-bit only
static inline void natboot_shot(const char *name) { (void)name; }
static inline void natboot_scene_size(unsigned *w, unsigned *h) { *w = *h = 0; }
static inline int natboot_comp_make(void) { return 0; }
static inline void natboot_layer_begin(int half, int clear) { (void)half; (void)clear; }
static inline void natboot_layer_end(void) {}
static inline int natboot_hud_begin(void) { return 0; }
static inline void natboot_hud_end(void) {}
static inline void *natboot_tex_create(unsigned w, unsigned h, unsigned levels, unsigned d3dfmt) { (void)w; (void)h; (void)levels; (void)d3dfmt; return 0; }
static inline int natboot_tex_upload(void *img, unsigned level, const void *data, unsigned pitch) { (void)img; (void)level; (void)data; (void)pitch; return 0; }
static inline int natboot_tex_download(void *img, unsigned level, void **out, unsigned *pitch, unsigned *rows) { (void)img; (void)level; (void)out; (void)pitch; (void)rows; return 0; }
static inline int natboot_tex_info(void *img, unsigned *w, unsigned *h, unsigned *levels, unsigned *max_size, unsigned *fmt) { (void)img; (void)w; (void)h; (void)levels; (void)max_size; (void)fmt; return 0; }
static inline void natboot_tex_release(void *img) { (void)img; }
#endif
