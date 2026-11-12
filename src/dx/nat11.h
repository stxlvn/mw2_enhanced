#pragma once
// The native Direct3D 11 device layer of the game's new renderer back end (docs/native-dx-plan.md, M5; "the owner's rule": with DirectX 11 selected no Direct3D 9 object exists).
// It owns the device, the swap chain, the scene targets, the caps the game reads at start (docs/native-dx-plan.md, "Device creation and caps"), images (2D, cube, volume; the game's own
// D3DFORMAT numbers), buffers and shaders (Direct3D 9 shader model 3 bytecode through src/dx/sm3.c). It knows nothing about Direct3D 9 and does not include its headers. The game's back end
// functions (the pass-apply and draw functions) are rewritten on top of it; the Direct3D 12 version is the same header with another .c file.
#include <stddef.h>
#include <stdint.h>

typedef struct nat_device nat_device;
typedef struct nat_image nat_image;

typedef struct {
	void *hwnd;
	unsigned width, height;
	unsigned refresh; // fullscreen only, 0 = the desktop's
	int fullscreen;
	int vsync;
	int msaa; // samples: 0 / 1 none, 2, 4, 8 (lowered to what the adapter supports)
} nat_params;

// what the game reads from GetDeviceCaps / CheckDeviceFormat at start (0xef10, 0x10550) and the sizes it takes from the monitor (0x10260)
typedef struct {
	int dynamic_textures; // Caps2 DYNAMICTEXTURES
	int aniso_min_mag; // TextureFilterCaps anisotropic
	int max_aniso;
	unsigned max_texture;
	int max_units; // MaxSimultaneousTextures, at most 16
	int clip_planes; // at most 6 (emulated with clip distances)
	int slope_scale_bias;
	int mip_cube;
	int hw_shadow_depth; // depth textures with a comparison sampler: the success case of the depth / render-target format search
	int msaa_max;
	unsigned desktop_w, desktop_h; // the monitor
	char adapter[128];
	unsigned vram_mb; // dedicated video memory of the adapter
} nat_caps;

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ before a device exists
typedef struct {
	unsigned w, h, hz;
} nat_mode;
// the display modes (32-bit colour, as the game lists them) of output `output` of the first adapter, unsorted, refresh rate in whole Hz (0 when the driver gives none); returns the count (at most max)
int nat_list_modes(int output, nat_mode *out, int max);
// the HMONITOR of that output (NULL when it does not exist) and the number of outputs
void *nat_output_monitor(int output);
int nat_output_count(void);

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ device
int nat_create(const nat_params *p, nat_device **out, char *err, size_t err_size);
void nat_destroy(nat_device *d);
const nat_caps *nat_get_caps(const nat_device *d);
const char *nat_backend_name(void); // "Direct3D 11" / "Direct3D 12"
// vid_restart / a changed resolution or mode; the resources made by the caller stay valid (the scene targets are rebuilt by the device)
int nat_reset(nat_device *d, const nat_params *p, char *err, size_t err_size);
// resolves the scene to the back buffer and presents; 0 = shown, 1 = occluded (nothing to do), -1 = device lost / removed
int nat_present(nat_device *d);
void nat_get_size(const nat_device *d, unsigned *w, unsigned *h);
// QA / tests: one pixel (B8G8R8A8) of an image (NULL = the scene, resolved when multisampled); 1 on success
int nat_read_pixel(nat_device *d, nat_image *im, unsigned x, unsigned y, uint32_t *out);
// QA: the scene sampled on a grid of cols x rows points (B8G8R8A8 each, out[row * cols + col]); 1 on success
int nat_sample_scene(nat_device *d, unsigned cols, unsigned rows, uint32_t *out);
// QA: the scene as a BMP file (resolved when multisampled); 1 on success
int nat_save_scene_bmp(nat_device *d, const char *path);

// the raw interfaces, for the back end's own draw code (void * so that this header stays free of the Direct3D headers): ID3D11Device, ID3D11DeviceContext, the scene's color target view
// (multisampled when msaa > 1), the scene's depth view (D24S8, same size and sample count)
void *nat_d3d_device(nat_device *d);
void *nat_d3d_context(nat_device *d);
void *nat_scene_rtv(nat_device *d);
void *nat_scene_rtv_srgb(nat_device *d); // the same target seen as sRGB (the game's SRGBWRITEENABLE)
void *nat_scene_dsv(nat_device *d);
void *nat_scene_srv(nat_device *d); // the scene color as a texture (single sample: resolved first when multisampled)
void nat_scene_resolve(nat_device *d); // makes nat_scene_srv() current (the game's StretchRect of the scene)

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ queries
// event queries (frame pacing: "has the GPU finished everything issued before") and occlusion queries
typedef struct nat_query nat_query;
nat_query *nat_query_create(nat_device *d, int occlusion);
void nat_query_destroy(nat_query *q);
void nat_query_end(nat_query *q); // Direct3D 9 Issue(D3DISSUE_END)
// 0 = the data is ready (*value filled for occlusion queries: the samples that passed), 1 = not ready yet, -1 = the device is lost
int nat_query_get(nat_query *q, unsigned *value);
// gamma ramp (3 x 256 words, 0..65535) through the output's gamma control: works in exclusive full screen only; returns 0 when it cannot be applied
int nat_set_gamma(nat_device *d, const uint16_t ramp[3][256]);

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ images
// d3d9_format: the game's own D3DFORMAT numbers (21 A8R8G8B8, 22 X8R8G8B8, 28 A8, 50 L8, 51 A8L8, DXT1 / 3 / 5 fourcc, 111..116 floats, 'INTZ', D16 / D24S8 as depth ...)
enum { NAT_IMAGE_2D = 1, NAT_IMAGE_CUBE = 2, NAT_IMAGE_VOLUME = 3 };
enum { NAT_RENDER_TARGET = 1, NAT_DEPTH = 2, NAT_DYNAMIC = 4 };
typedef struct {
	int kind;
	unsigned w, h, d, levels; // levels 0 = the whole chain
	unsigned d3d9_format;
	unsigned flags;
	int msaa; // render targets only
} nat_image_desc;
nat_image *nat_image_create(nat_device *d, const nat_image_desc *desc);
void nat_image_destroy(nat_image *i);
// one level (and face for cubes); data is the level's pixels in the game's layout: rows of (width pixels or 4x4 blocks), slices of volumes one after the other. L8 / A8L8 are widened to 8888.
int nat_image_upload(nat_image *i, unsigned face, unsigned level, const void *data, unsigned row_pitch, unsigned slice_pitch);
void *nat_image_srv(nat_image *i, int srgb);
void nat_image_addref(nat_image *i); // one more owner (the image is freed with the last nat_image_destroy)
int nat_image_is_depth(const nat_image *i); // a depth texture (Direct3D 9: a hardware shadow map when sampled by a shader)
void *nat_image_rtv(nat_image *i, int srgb);
void *nat_image_dsv(nat_image *i);
void *nat_image_resource(nat_image *i);
void nat_image_size(const nat_image *i, unsigned *w, unsigned *h, unsigned *samples);
unsigned nat_image_dxgi_format(const nat_image *i); // the DXGI_FORMAT of the resource (typeless where the game needs two views)
void *nat_scene_resource(nat_device *d);
unsigned nat_scene_samples(const nat_device *d); // the ID3D11Resource (readbacks, copies)
unsigned nat_image_levels(const nat_image *i);

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ buffers
typedef struct nat_buffer nat_buffer;
// vertex (is_index 0) or index buffers; dynamic ones are written with nat_buffer_map (a ring: NAT_MAP_NOOVERWRITE appends, NAT_MAP_DISCARD restarts)
nat_buffer *nat_buffer_create(nat_device *d, unsigned size, int is_index, int dynamic, const void *initial);
void nat_buffer_destroy(nat_buffer *b);
// A static buffer written by the engine after creating it (0x24d00 / 0x24df0): *block is the memory to write; the Direct3D 11 buffer is made at the engine's Unlock call (through the
// Direct3D 9 style vtable every nat_buffer starts with: Lock +0x58, Unlock +0x60, Release +0x10)
nat_buffer *nat_buffer_create_static(nat_device *d, unsigned size, int is_index, void **block);
// A vertex buffer locked by the engine for a whole frame (Direct3D 9 Lock through the vtable) and written by its worker threads: *used is the engine's counter of the reserved bytes;
// nat_buffer_sync copies the reserved part to the GPU before a draw reads it (nat_buffer_is_staged tells which buffers need it)
extern void (*nat_log_hook)(const char *fmt, ...); // diagnostics: set by the mod, NULL in the tests
nat_buffer *nat_buffer_create_staged(nat_device *d, unsigned size, const volatile int *used);
int nat_buffer_is_staged(const nat_buffer *b);
void nat_buffer_sync(nat_buffer *b);
enum { NAT_MAP_DISCARD = 1, NAT_MAP_NOOVERWRITE = 2 };
void *nat_buffer_map(nat_buffer *b, int mode);
void nat_buffer_unmap(nat_buffer *b);
void *nat_buffer_resource(nat_buffer *b); // ID3D11Buffer

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ shaders
typedef struct nat_shader nat_shader;
// Direct3D 9 shader model 3 bytecode (the game's own); pixel shaders get the alpha test constant buffer (b2) when alpha_test is set. int_inputs: see sm3_int_input (vertex shaders).
// queues the shader (SM3 code) for a background compile of its common variant into the disk cache
void nat_shader_prewarm(const uint32_t *code, size_t dwords);
nat_shader *nat_shader_from_sm3(nat_device *d, const uint32_t *code, size_t dwords, int alpha_test, const void *int_inputs, int nint_inputs, char *err, size_t err_size);
nat_shader *nat_shader_from_hlsl(nat_device *d, const char *hlsl, int is_vs, char *err, size_t err_size);
void nat_shader_destroy(nat_shader *s);
void *nat_shader_object(nat_shader *s); // ID3D11VertexShader / ID3D11PixelShader
void *nat_shader_blob(nat_shader *s, size_t *size); // the compiled bytecode (for input layouts)
int nat_shader_is_vs(const nat_shader *s);
