#pragma once
// The native Direct3D 11 device layer of the game's new renderer back end (docs/native-dx-plan.md, M5; "the owner's rule": with DirectX 11 selected no Direct3D 9 object exists).
// It owns the device, the swap chain, the scene targets, the caps the game reads at start (docs/native-dx-plan.md, "Device creation and caps"), images (2D, cube, volume; the game's own
// D3DFORMAT numbers), buffers and shaders (Direct3D 9 shader model 3 bytecode through src/dx/sm3.c). It knows nothing about Direct3D 9 and does not include its headers. The game's back end
// functions (the pass-apply and draw functions) are rewritten on top of it; the Direct3D 12 version is the same header with another .c file.
#include <stddef.h>
#include <stdint.h>

typedef struct nat_device nat_device;

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
} nat_caps;

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ device
int nat_create(const nat_params *p, nat_device **out, char *err, size_t err_size);
void nat_destroy(nat_device *d);
const nat_caps *nat_get_caps(const nat_device *d);
// vid_restart / a changed resolution or mode; the resources made by the caller stay valid (the scene targets are rebuilt by the device)
int nat_reset(nat_device *d, const nat_params *p, char *err, size_t err_size);
// resolves the scene to the back buffer and presents; 0 = shown, 1 = occluded (nothing to do), -1 = device lost / removed
int nat_present(nat_device *d);
void nat_get_size(const nat_device *d, unsigned *w, unsigned *h);

// the raw interfaces, for the back end's own draw code (void * so that this header stays free of the Direct3D headers): ID3D11Device, ID3D11DeviceContext, the scene's color target view
// (multisampled when msaa > 1), the scene's depth view (D24S8, same size and sample count)
void *nat_d3d_device(nat_device *d);
void *nat_d3d_context(nat_device *d);
void *nat_scene_rtv(nat_device *d);
void *nat_scene_dsv(nat_device *d);
void *nat_scene_srv(nat_device *d); // the scene color as a texture (single sample: resolved first when multisampled)
void nat_scene_resolve(nat_device *d); // makes nat_scene_srv() current (the game's StretchRect of the scene)

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
typedef struct nat_image nat_image;
nat_image *nat_image_create(nat_device *d, const nat_image_desc *desc);
void nat_image_destroy(nat_image *i);
// one level (and face for cubes); data is the level's pixels in the game's layout: rows of (width pixels or 4x4 blocks), slices of volumes one after the other. L8 / A8L8 are widened to 8888.
int nat_image_upload(nat_image *i, unsigned face, unsigned level, const void *data, unsigned row_pitch, unsigned slice_pitch);
void *nat_image_srv(nat_image *i, int srgb);
void *nat_image_rtv(nat_image *i, int srgb);
void *nat_image_dsv(nat_image *i);
unsigned nat_image_levels(const nat_image *i);

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ buffers
typedef struct nat_buffer nat_buffer;
// vertex (is_index 0) or index buffers; dynamic ones are written with nat_buffer_map (a ring: NAT_MAP_NOOVERWRITE appends, NAT_MAP_DISCARD restarts)
nat_buffer *nat_buffer_create(nat_device *d, unsigned size, int is_index, int dynamic, const void *initial);
void nat_buffer_destroy(nat_buffer *b);
enum { NAT_MAP_DISCARD = 1, NAT_MAP_NOOVERWRITE = 2 };
void *nat_buffer_map(nat_buffer *b, int mode);
void nat_buffer_unmap(nat_buffer *b);
void *nat_buffer_resource(nat_buffer *b); // ID3D11Buffer

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ shaders
typedef struct nat_shader nat_shader;
// Direct3D 9 shader model 3 bytecode (the game's own); pixel shaders get the alpha test constant buffer (b2) when alpha_test is set. int_inputs: see sm3_int_input (vertex shaders).
nat_shader *nat_shader_from_sm3(nat_device *d, const uint32_t *code, size_t dwords, int alpha_test, const void *int_inputs, int nint_inputs, char *err, size_t err_size);
nat_shader *nat_shader_from_hlsl(nat_device *d, const char *hlsl, int is_vs, char *err, size_t err_size);
void nat_shader_destroy(nat_shader *s);
void *nat_shader_object(nat_shader *s); // ID3D11VertexShader / ID3D11PixelShader
void *nat_shader_blob(nat_shader *s, size_t *size); // the compiled bytecode (for input layouts)
int nat_shader_is_vs(const nat_shader *s);
