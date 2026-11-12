// The game's Direct3D 9 device without knowing where the game stores it, and the extended Direct3D 9 for it ([display] d3d9ex).
// 32 bits: DllMain runs while the exe's imports are loaded (d3d9.dll is one of them, docs/32bit-plan.md), before the game's code runs: the first bytes of d3d9.dll's Direct3DCreate9 get a jump
// to our function, which calls the real one and replaces the CreateDevice entry of the IDirect3D9 object's vtable with ours; ours calls the real CreateDevice and keeps the device it returns.
// A hook installed later (the Steam overlay's) finds our entries where the originals were and calls them.
// The extended interface (Direct3D 9Ex): with it a window that loses the focus (Alt+Tab) does not lose its device, so nothing has to be reset and the render thread never waits for the window
// to come back. Without it, a game window in the background in exclusive fullscreen left the render thread blocked inside d3d9.dll: in co-op the partner got "no signal from the other
// participant for 10 s" and the game that had been left crashed afterwards, and in split screen the Reset on the way back ended in a stack overflow inside the Steam overlay (v0.6.0 reports,
// 2026-11-05). Our function then makes the object with Direct3DCreate9Ex (an IDirect3D9Ex is an IDirect3D9 with more methods) and the device with CreateDeviceEx; Direct3D 9Ex has no
// managed pool, so the vtable entries that create textures and buffers turn D3DPOOL_MANAGED into the default pool with dynamic usage (the same way the iw4x client does it, which has
// this as its option r_useD3D9Ex). Known limits (2026-11-05): a vid_restart with it on stalls (the new device is made, the menus never come back; the default pool resources of the old
// device are not the reason: the game makes a new device), a texture whose format the driver will not make dynamic is made without it and cannot be locked (not seen in any test run),
// and the 32-bit build is not tested. 64 bits: the exe's import table entry for Direct3DCreate9 is pointed at our function (the same code from there on).
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <dxgi.h>
#include <d3d12.h>
#include <stdint.h>
#include <string.h>
#include <tlhelp32.h>

#include "d3dcap.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "settings.h"
#include "upscale.h"
#include "dx/stateprobe.h"
#include "dx/mirror11.h"
#include "dx/d3dcallers.h"
#include "dx/natcheck.h"

#define VT_CREATE_DEVICE         16 // IDirect3D9::CreateDevice
#define VT_CREATE_TEXTURE        23 // IDirect3DDevice9::CreateTexture ... CreateIndexBuffer
#define VT_CREATE_VOLUME_TEXTURE 24
#define VT_CREATE_CUBE_TEXTURE   25
#define VT_CREATE_VERTEX_BUFFER  26
#define VT_CREATE_INDEX_BUFFER   27

typedef IDirect3D9 *(WINAPI *create9_t)(UINT);
typedef HRESULT(WINAPI *create9ex_t)(UINT, IDirect3D9Ex **);
typedef FARPROC(WINAPI *get_proc_address_t)(HMODULE, LPCSTR);
typedef HRESULT(STDMETHODCALLTYPE *create_device_ex_t)(IDirect3D9Ex *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, D3DDISPLAYMODEEX *, IDirect3DDevice9Ex **);
typedef HRESULT(STDMETHODCALLTYPE *create_device_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef HRESULT(STDMETHODCALLTYPE *create_texture_t)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *);
typedef HRESULT(STDMETHODCALLTYPE *create_volume_t)(IDirect3DDevice9 *, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9 **, HANDLE *);
typedef HRESULT(STDMETHODCALLTYPE *create_cube_t)(IDirect3DDevice9 *, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9 **, HANDLE *);
typedef HRESULT(STDMETHODCALLTYPE *create_vb_t)(IDirect3DDevice9 *, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9 **, HANDLE *);
typedef HRESULT(STDMETHODCALLTYPE *create_ib_t)(IDirect3DDevice9 *, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9 **, HANDLE *);

static create9_t create9_orig;
static create9ex_t create9ex_orig;
static get_proc_address_t getproc_orig;
static create_device_t create_device_orig;
static create_device_ex_t create_device_ex_orig;
static void *volatile device;
static IUnknown *volatile g_dev12;   // the Direct3D 12 device Direct3D 9 runs on (d3d9on12)
static IUnknown *volatile g_queue12; // the direct command queue made for it: the upscalers wait on it, and Direct3D 9 resources are unwrapped against it
static int use_ex;
static UINT ex_version;

static create_texture_t create_texture_orig;
static create_volume_t create_volume_orig;
static create_cube_t create_cube_orig;
static create_vb_t create_vb_orig;
static create_ib_t create_ib_orig;
static LONG converted[5]; // managed resources turned into default-pool ones: textures, volumes, cubes, vertex buffers, index buffers

void *d3dcap_device(void)
{
	return InterlockedCompareExchangePointer((PVOID volatile *)&device, NULL, NULL);
}

void *d3dcap_d3d12_device(void)
{
	return g_dev12;
}

void *d3dcap_d3d12_queue(void)
{
	return g_queue12;
}

// Proof of the way to the upscalers (DLSS, FSR 3 need Direct3D 11 / 12 resources): the game's Direct3D 9 device on Direct3D 12 (IDirect3DDevice9On12) hands out the Direct3D 12 resource under a
// Direct3D 9 surface. Logged once, at a Present: the back buffer and the depth buffer, if one is set then, as Direct3D 12 resources.
typedef struct {
	HRESULT(STDMETHODCALLTYPE *qi)(void *, const GUID *, void **);
	ULONG(STDMETHODCALLTYPE *addref)(void *);
	ULONG(STDMETHODCALLTYPE *release)(void *);
	HRESULT(STDMETHODCALLTYPE *get_device)(void *, const GUID *, void **);
	HRESULT(STDMETHODCALLTYPE *unwrap)(void *, IDirect3DResource9 *, IUnknown *, const GUID *, void **);
	HRESULT(STDMETHODCALLTYPE *give_back)(void *, IDirect3DResource9 *, UINT, UINT64 *, void **);
} on12_vtbl_t;

static void probe_surface(void *on12, const char *what, IDirect3DSurface9 *surf)
{
	static const GUID iid_resource = {0x696442be, 0xa72e, 0x4059, {0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad}};
	on12_vtbl_t *vt = *(on12_vtbl_t **)on12;
	D3DSURFACE_DESC sd;
	if (!surf || FAILED(IDirect3DSurface9_GetDesc(surf, &sd))) {
		log_printf("d3d9on12 probe: %s: none set", what);
		return;
	}
	void *res = NULL;
	HRESULT hr = vt->unwrap(on12, (IDirect3DResource9 *)surf, g_queue12, &iid_resource, &res);
	if (SUCCEEDED(hr) && res) {
		D3D12_RESOURCE_DESC rd;
		((D3D12_RESOURCE_DESC * (STDMETHODCALLTYPE *)(void *, D3D12_RESOURCE_DESC *))(*(void ***)res)[10])(res, &rd); // ID3D12Resource::GetDesc, the struct through a hidden pointer
		log_printf("d3d9on12 probe: %s: Direct3D 9 surface %ux%u format %d -> Direct3D 12 resource %llux%u format %d, %u samples, flags %x", what, sd.Width, sd.Height, (int)sd.Format,
			   (unsigned long long)rd.Width, rd.Height, (int)rd.Format, rd.SampleDesc.Count, (unsigned)rd.Flags);
		vt->give_back(on12, (IDirect3DResource9 *)surf, 0, NULL, NULL);
		ID3D12Resource_Release((ID3D12Resource *)res);
	} else {
		log_printf("d3d9on12 probe: %s: %ux%u format %d: UnwrapUnderlyingResource hr %lx", what, sd.Width, sd.Height, (int)sd.Format, (unsigned long)hr);
	}
}

void d3dcap_on12_probe(void *dev9)
{
	static const GUID iid_on12 = {0xe7fda234, 0xb589, 0x4049, {0x94, 0x0d, 0x88, 0x78, 0x97, 0x75, 0x31, 0xc8}};
	static LONG done;
	// before_present calls this every frame: once the probe has run, avoid a locked RMW on every Present.
	if (!dev9 || !g_queue12 || __atomic_load_n(&done, __ATOMIC_RELAXED))
		return;
	if (InterlockedCompareExchange(&done, 1, 0))
		return;
	void *on12 = NULL;
	if (FAILED(IDirect3DDevice9_QueryInterface((IDirect3DDevice9 *)dev9, &iid_on12, &on12)) || !on12) {
		log_printf("d3d9on12 probe: the device has no IDirect3DDevice9On12 (not running on Direct3D 12)");
		return;
	}
	on12_vtbl_t *vt = *(on12_vtbl_t **)on12;
	void *dev12 = NULL;
	static const GUID iid_device = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
	HRESULT hr = vt->get_device(on12, &iid_device, &dev12);
	log_printf("d3d9on12 probe: GetD3D12Device hr %lx device %p (ours %p)", (unsigned long)hr, dev12, (void *)g_dev12);
	if (dev12)
		IUnknown_Release((IUnknown *)dev12);
	IDirect3DSurface9 *bb = NULL, *depth = NULL;
	if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer((IDirect3DDevice9 *)dev9, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
		probe_surface(on12, "back buffer", bb);
		IDirect3DSurface9_Release(bb);
	}
	if (SUCCEEDED(IDirect3DDevice9_GetDepthStencilSurface((IDirect3DDevice9 *)dev9, &depth)) && depth) {
		probe_surface(on12, "depth buffer", depth);
		IDirect3DSurface9_Release(depth);
	} else {
		log_printf("d3d9on12 probe: depth buffer: none set at Present");
	}
	vt->release(on12);
}

static void convert_logged(int kind, const char *what, unsigned a, unsigned b, unsigned fmt)
{
	LONG n = InterlockedIncrement(&converted[kind]);
	if (n <= 2 || n == 1000 || n == 10000)
		log_printf("d3d9ex: managed %s (%u, %u, format %x) made a default-pool dynamic one (the %ld. of its kind)", what, a, b, fmt, (long)n);
}

static void creation_failed(const char *what, HRESULT hr, unsigned a, unsigned b, unsigned c, unsigned d, unsigned e)
{
	static LONG n;
	if (InterlockedIncrement(&n) <= 30)
		log_printf("d3d9ex: creating a %s failed: hr %lx (%u, %u, %u, usage/format/pool %x %u %u)", what, (unsigned long)hr, a, b, c, d, e, 0u);
}

static HRESULT STDMETHODCALLTYPE create_texture_hook(IDirect3DDevice9 *dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *shared)
{
	if (pool != D3DPOOL_MANAGED) {
		HRESULT hr0 = create_texture_orig(dev, w, h, levels, usage, fmt, pool, out, shared);
		if (FAILED(hr0))
			creation_failed("texture (not managed)", hr0, w, h, levels, (unsigned)usage, (unsigned)fmt);
		return hr0;
	}
	HRESULT hr = create_texture_orig(dev, w, h, levels, usage | D3DUSAGE_DYNAMIC, fmt, D3DPOOL_DEFAULT, out, shared);
	if (FAILED(hr)) { // a compressed format that this driver does not make dynamic: still a texture (without the lock it would need)
		static LONG logged;
		if (InterlockedIncrement(&logged) <= 3)
			log_printf("d3d9ex: texture %ux%u levels %u format %x: dynamic default pool failed (hr %lx), trying without dynamic", w, h, levels, (unsigned)fmt, (unsigned long)hr);
		hr = create_texture_orig(dev, w, h, levels, usage, fmt, D3DPOOL_DEFAULT, out, shared);
	}
	if (SUCCEEDED(hr))
		convert_logged(0, "texture", w, h, (unsigned)fmt);
	else
		creation_failed("texture (managed)", hr, w, h, levels, (unsigned)usage, (unsigned)fmt);
	return hr;
}

static HRESULT STDMETHODCALLTYPE create_volume_hook(IDirect3DDevice9 *dev, UINT w, UINT h, UINT depth, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DVolumeTexture9 **out, HANDLE *shared)
{
	if (pool == D3DPOOL_MANAGED) {
		usage |= D3DUSAGE_DYNAMIC;
		pool = D3DPOOL_DEFAULT;
		convert_logged(1, "volume texture", w, h, (unsigned)fmt);
	}
	return create_volume_orig(dev, w, h, depth, levels, usage, fmt, pool, out, shared);
}

static HRESULT STDMETHODCALLTYPE create_cube_hook(IDirect3DDevice9 *dev, UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DCubeTexture9 **out, HANDLE *shared)
{
	if (pool == D3DPOOL_MANAGED) {
		usage |= D3DUSAGE_DYNAMIC;
		pool = D3DPOOL_DEFAULT;
		convert_logged(2, "cube texture", edge, levels, (unsigned)fmt);
	}
	return create_cube_orig(dev, edge, levels, usage, fmt, pool, out, shared);
}

static HRESULT STDMETHODCALLTYPE create_vb_hook(IDirect3DDevice9 *dev, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9 **out, HANDLE *shared)
{
	if (pool == D3DPOOL_MANAGED) {
		usage |= D3DUSAGE_DYNAMIC;
		pool = D3DPOOL_DEFAULT;
		convert_logged(3, "vertex buffer", length, 0, (unsigned)fvf);
	}
	return create_vb_orig(dev, length, usage, fvf, pool, out, shared);
}

static HRESULT STDMETHODCALLTYPE create_ib_hook(IDirect3DDevice9 *dev, UINT length, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9 **out, HANDLE *shared)
{
	if (pool == D3DPOOL_MANAGED) {
		usage |= D3DUSAGE_DYNAMIC;
		pool = D3DPOOL_DEFAULT;
		convert_logged(4, "index buffer", length, 0, (unsigned)fmt);
	}
	return create_ib_orig(dev, length, usage, fmt, pool, out, shared);
}

static void patch_vtable(void **vt, int index, void *hook, void **orig)
{
	DWORD old;
	if (!VirtualProtect(&vt[index], sizeof(void *), PAGE_READWRITE, &old))
		return;
	*orig = vt[index];
	vt[index] = hook;
	VirtualProtect(&vt[index], sizeof(void *), old, &old);
}

// QA, MW2COOP_DUMPSHADERS=1: every vertex and pixel shader the game makes (Direct3D 9 shader model 3 bytecode, from the materials of the zones) is written to
// <game dir>\mw2coop\shaders\vs_<hash>.bin / ps_<hash>.bin, once each. For the shader translator that a native Direct3D 11 / 12 backend needs (docs/native-dx-plan.md): its tests
// run on these files. They are the game's data: they stay in the test copy, never in the repository.
#define VT_CREATE_VERTEX_SHADER 91
#define VT_CREATE_PIXEL_SHADER  106
typedef HRESULT(STDMETHODCALLTYPE *create_shader_t)(IDirect3DDevice9 *, const DWORD *, void **);
static create_shader_t create_vs_orig, create_ps_orig;
static char shader_dir[MAX_PATH];

// the length in bytes of a shader's token stream: version token, instructions and comments up to the end token
static size_t shader_size(const DWORD *t)
{
	size_t n = 1;
	for (; n < (1u << 20); ) {
		const DWORD tok = t[n];
		if ((tok & 0xffff) == 0xfffe) { // a comment (the constant table): the length in dwords is in the high word
			n += 1 + ((tok >> 16) & 0x7fff);
			continue;
		}
		if ((tok & 0xffff) == 0xffff)
			return (n + 1) * sizeof(DWORD);
		n += 1 + ((tok >> 24) & 0xf);
	}
	return 0;
}

static void shader_dump(const char *kind, const DWORD *code)
{
	if (!code || !shader_dir[0])
		return;
	const size_t size = shader_size(code);
	if (!size)
		return;
	uint32_t h = 2166136261u; // FNV-1a
	const uint8_t *b = (const uint8_t *)code;
	for (size_t i = 0; i < size; i++)
		h = (h ^ b[i]) * 16777619u;
	char path[MAX_PATH];
	wsprintfA(path, "%s%s_%08lx.bin", shader_dir, kind, (unsigned long)h);
	HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL); // an existing file: the same shader again
	if (f == INVALID_HANDLE_VALUE)
		return;
	DWORD wrote;
	WriteFile(f, code, (DWORD)size, &wrote, NULL);
	CloseHandle(f);
}

static HRESULT STDMETHODCALLTYPE create_vs_dump(IDirect3DDevice9 *dev, const DWORD *code, void **out)
{
	shader_dump("vs", code);
	return create_vs_orig(dev, code, out);
}

static HRESULT STDMETHODCALLTYPE create_ps_dump(IDirect3DDevice9 *dev, const DWORD *code, void **out)
{
	shader_dump("ps", code);
	return create_ps_orig(dev, code, out);
}

static void hook_shader_dump(IDirect3DDevice9 *dev)
{
	d3dcallers_hook_device(dev); // QA, MW2COOP_D3DCALLERS=1
	stateprobe_hook_device(dev); // QA, MW2COOP_STATEPROBE=1
	mirror11_hook_device(dev);   // QA, MW2COOP_D3D11MIRROR=1
	natcheck_hook_device(dev);  // QA, MW2COOP_NATCHECK=1
	if (!getenv("MW2COOP_DUMPSHADERS"))
		return;
	void **vt = *(void ***)dev;
	if (vt[VT_CREATE_VERTEX_SHADER] == (void *)create_vs_dump)
		return;
	if (!shader_dir[0]) {
		DWORD n = GetModuleFileNameA(NULL, shader_dir, MAX_PATH);
		while (n > 0 && shader_dir[n - 1] != '\\')
			n--;
		shader_dir[n] = 0;
		lstrcatA(shader_dir, "mw2coop\\shaders\\");
		CreateDirectoryA(shader_dir, NULL);
	}
	patch_vtable(vt, VT_CREATE_VERTEX_SHADER, (void *)create_vs_dump, (void **)&create_vs_orig);
	patch_vtable(vt, VT_CREATE_PIXEL_SHADER, (void *)create_ps_dump, (void **)&create_ps_orig);
	log_printf("d3dcap: shaders are dumped to %s (MW2COOP_DUMPSHADERS)", shader_dir);
}

// The device's vtable is the class's: every device of this process makes its resources through these entries from now on
static void hook_resource_creation(IDirect3DDevice9 *dev)
{
	void **vt = *(void ***)dev;
	if (create_texture_orig) { // made before: a device made later (a vid_restart) must still go through the hooks; a vtable that is not the hooked one is hooked again
		log_printf("d3d9ex: the new device's CreateTexture slot is %p (the hook %p), CreateVertexBuffer %p (%p)", vt[VT_CREATE_TEXTURE], (void *)create_texture_hook, vt[VT_CREATE_VERTEX_BUFFER], (void *)create_vb_hook);
		if (vt[VT_CREATE_TEXTURE] != (void *)create_texture_hook) {
			void *keep_t = (void *)create_texture_orig, *keep_v = (void *)create_volume_orig, *keep_c = (void *)create_cube_orig, *keep_b = (void *)create_vb_orig, *keep_i = (void *)create_ib_orig;
			patch_vtable(vt, VT_CREATE_TEXTURE, (void *)create_texture_hook, (void **)&create_texture_orig);
			patch_vtable(vt, VT_CREATE_VOLUME_TEXTURE, (void *)create_volume_hook, (void **)&create_volume_orig);
			patch_vtable(vt, VT_CREATE_CUBE_TEXTURE, (void *)create_cube_hook, (void **)&create_cube_orig);
			patch_vtable(vt, VT_CREATE_VERTEX_BUFFER, (void *)create_vb_hook, (void **)&create_vb_orig);
			patch_vtable(vt, VT_CREATE_INDEX_BUFFER, (void *)create_ib_hook, (void **)&create_ib_orig);
			log_printf("d3d9ex: hooked again (the originals were %p %p %p %p %p, are %p %p %p %p %p)", keep_t, keep_v, keep_c, keep_b, keep_i, (void *)create_texture_orig, (void *)create_volume_orig, (void *)create_cube_orig, (void *)create_vb_orig, (void *)create_ib_orig);
		}
		return;
	}
	patch_vtable(vt, VT_CREATE_TEXTURE, (void *)create_texture_hook, (void **)&create_texture_orig);
	patch_vtable(vt, VT_CREATE_VOLUME_TEXTURE, (void *)create_volume_hook, (void **)&create_volume_orig);
	patch_vtable(vt, VT_CREATE_CUBE_TEXTURE, (void *)create_cube_hook, (void **)&create_cube_orig);
	patch_vtable(vt, VT_CREATE_VERTEX_BUFFER, (void *)create_vb_hook, (void **)&create_vb_orig);
	patch_vtable(vt, VT_CREATE_INDEX_BUFFER, (void *)create_ib_hook, (void **)&create_ib_orig);
	log_printf("d3d9ex: texture and buffer creation hooked (managed pool -> default pool, dynamic)");
}

static IDirect3D9 *WINAPI create9_hook(UINT version);
static HRESULT WINAPI create9ex_hook(UINT version, IDirect3D9Ex **out);
static FARPROC create9_hook_proc(void)
{
	create9_t typed = create9_hook;
	FARPROC generic = NULL;
	_Static_assert(sizeof(generic) == sizeof(typed), "Win32 function pointer sizes differ");
	memcpy(&generic, &typed, sizeof(generic));
	return generic;
}
static FARPROC create9ex_hook_proc(void)
{
	create9ex_t typed = create9ex_hook;
	FARPROC generic = NULL;
	_Static_assert(sizeof(generic) == sizeof(typed), "Win32 function pointer sizes differ");
	memcpy(&generic, &typed, sizeof(generic));
	return generic;
}

// IID_IDirect3D9Ex (the game makes more than one Direct3D object: the device is made through whichever of them it likes, so the object is asked, not compared)
static const GUID iid_d3d9ex = {0x02177241, 0x69fc, 0x400c, {0x8f, 0xf1, 0x93, 0xa4, 0x4d, 0xf6, 0x86, 0x1d}};

static create9_t plain_create9_factory(void)
{
	create9_t make = create9_orig;
	if (!make) {
		HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
		FARPROC proc = d3d9 ? (getproc_orig ? getproc_orig(d3d9, "Direct3DCreate9") : GetProcAddress(d3d9, "Direct3DCreate9")) : NULL;
		if (proc)
			memcpy(&make, &proc, sizeof(make));
		if (make)
			(void)InterlockedCompareExchangePointer((PVOID volatile *)&create9_orig, (PVOID)make, NULL);
	}
	return create9_orig ? create9_orig : make;
}

// The game makes more than one Direct3D object, and an object's vtable is not necessarily the table of the first one: each patched slot keeps the function it had, and the hook calls the
// original of the table of the object it was called on (a single global original, taken from the first object, was called with the second object: CreateDevice failed with
// E_OUTOFMEMORY or the game crashed, 2026-11-07).
#define FACTORY_ORIGINALS 16
static struct {
	void **vt;
	int index;
	void *orig;
} factory_originals[FACTORY_ORIGINALS];
static volatile LONG factory_original_count;

static void *factory_original(void *object, int index, void *fallback)
{
	void **vt = object ? *(void ***)object : NULL;
	LONG n = factory_original_count;
	if (n > FACTORY_ORIGINALS)
		n = FACTORY_ORIGINALS;
	for (LONG i = 0; vt && i < n; i++)
		if (factory_originals[i].vt == vt && factory_originals[i].index == index)
			return factory_originals[i].orig;
	return fallback;
}

// Which graphics card the plain Direct3D 9 renders on (a laptop with two: on the owner's one the Direct3D 12 layer, which picks the NVIDIA card itself, ran at 15-18 ms a frame and the
// plain Direct3D 9 at 28-43 ms, 2026-11-08). The description Direct3D 9 gives for the adapter the game opened is logged once per device.
static void log_adapter(IDirect3D9 *d3d, UINT adapter)
{
	D3DADAPTER_IDENTIFIER9 id;
	memset(&id, 0, sizeof(id));
	if (d3d && SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, adapter, 0, &id)))
		log_printf("d3dcap: adapter %u is '%s' (vendor %04lx device %04lx, driver %s)", adapter, id.Description, (unsigned long)id.VendorId, (unsigned long)id.DeviceId, id.Driver);
}

static HRESULT STDMETHODCALLTYPE create_device_hook_outer(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags, D3DPRESENT_PARAMETERS *pp,
						     IDirect3DDevice9 **out)
{
	HRESULT hr;
	int ex = 0;
	IDirect3D9Ex *obj = NULL;
	if (pp && (pp->hDeviceWindow || window)) {
		HWND w = pp->hDeviceWindow ? pp->hDeviceWindow : window;
		char cls[64] = "";
		RECT wr, cr;
		GetClassNameA(w, cls, sizeof(cls));
		GetWindowRect(w, &wr);
		GetClientRect(w, &cr);
		log_printf("d3dcap: window %p class '%s' style %lx exstyle %lx window rect %ld,%ld %ldx%ld client %ldx%ld visible %d focus window %p thread %lu (this thread %lu)", (void *)w, cls,
			   (unsigned long)GetWindowLongPtrA(w, GWL_STYLE), (unsigned long)GetWindowLongPtrA(w, GWL_EXSTYLE), wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, cr.right, cr.bottom,
			   IsWindowVisible(w), (void *)window, (unsigned long)GetWindowThreadProcessId(w, NULL), (unsigned long)GetCurrentThreadId());
	}
	if (pp)
		log_printf("d3dcap: CreateDevice parameters: %ux%u format %d count %u multisample %d/%lu swap effect %d window %p windowed %d depth %d format %d flags %lx hz %u interval %u", pp->BackBufferWidth,
			   pp->BackBufferHeight, (int)pp->BackBufferFormat, pp->BackBufferCount, (int)pp->MultiSampleType, (unsigned long)pp->MultiSampleQuality, (int)pp->SwapEffect, (void *)pp->hDeviceWindow,
			   (int)pp->Windowed, (int)pp->EnableAutoDepthStencil, (int)pp->AutoDepthStencilFormat, (unsigned long)pp->Flags, pp->FullScreen_RefreshRateInHz, pp->PresentationInterval);
	if (pp && pp->Windowed && settings_d3d9on12()) {
		// A windowed swap chain of Direct3D 9 on Direct3D 12 shows a white or black picture above about 800x600 on the tested laptop (NVIDIA chip rendering, the other chip showing the
		// display), whatever the window; the flip model of Direct3D 9Ex (FLIPEX, two back buffers) presents correctly (tools/on12test, 2026-11-07). A flip model back buffer cannot be
		// multisampled: the game's 2x multisampling of the back buffer goes with it.
		pp->SwapEffect = D3DSWAPEFFECT_FLIPEX;
		if (pp->BackBufferCount < 2)
			pp->BackBufferCount = 2;
		pp->MultiSampleType = D3DMULTISAMPLE_NONE;
		pp->MultiSampleQuality = 0;
		log_printf("d3d9on12: windowed: flip model swap chain (FLIPEX, %u back buffers, not multisampled)", pp->BackBufferCount);
	}
	if (pp)
		upscale_adjust_params(pp);
	if (settings_borderless() && pp && !pp->Windowed) { // exclusive full screen: a device that can be lost, with the render thread stuck in d3d9.dll and a Reset on the way back when the window loses the focus
		pp->Windowed = TRUE; // (the window the game made for it is a popup of the whole screen already)
		pp->FullScreen_RefreshRateInHz = 0;
		static LONG told;
		if (!InterlockedExchange(&told, 1))
			log_printf("d3dcap: [display] borderless: the full screen device is made a windowed one (%ux%u)", pp->BackBufferWidth, pp->BackBufferHeight);
	}
	if (use_ex && pp && out && SUCCEEDED(IDirect3D9_QueryInterface(d3d, &iid_d3d9ex, (void **)&obj)) && obj) {
		D3DPRESENT_PARAMETERS asked = *pp; // a failed CreateDeviceEx can leave the parameters rewritten: the plain attempt gets what the game asked for
		D3DDISPLAYMODEEX mode, *pmode = NULL;
		if (!pp->Windowed) { // exclusive fullscreen: the mode the game asked for
			memset(&mode, 0, sizeof(mode));
			mode.Size = sizeof(mode);
			mode.Width = pp->BackBufferWidth;
			mode.Height = pp->BackBufferHeight;
			mode.RefreshRate = pp->FullScreen_RefreshRateInHz;
			mode.Format = pp->BackBufferFormat;
			mode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
			pmode = &mode;
		}
		IDirect3DDevice9Ex *dx = NULL;
		hr = IDirect3D9Ex_CreateDeviceEx(obj, adapter, type, window, flags, pp, pmode, &dx);
		IDirect3D9Ex_Release(obj);
		if (SUCCEEDED(hr) && dx) {
			*out = (IDirect3DDevice9 *)dx;
			ex = 1;
		} else {
			log_printf("d3d9ex: CreateDeviceEx failed (hr %lx, %s, %ux%u at %u Hz format %d), the plain Direct3D 9 is used instead", (unsigned long)hr, pp->Windowed ? "windowed" : "fullscreen",
				   pp->BackBufferWidth, pp->BackBufferHeight, pp->FullScreen_RefreshRateInHz, (int)pp->BackBufferFormat);
			create9_t make_plain = plain_create9_factory();
			IDirect3D9 *plain = make_plain ? make_plain(ex_version) : NULL;
			*pp = asked;
			hr = plain ? IDirect3D9_CreateDevice(plain, adapter, type, window, flags, pp, out) : D3DERR_NOTAVAILABLE;
			if (plain)
				IDirect3D9_Release(plain); // a device keeps the object alive
		}
	} else {
		create_device_t real = (create_device_t)factory_original(d3d, VT_CREATE_DEVICE, (void *)create_device_orig);
		hr = real ? real(d3d, adapter, type, window, flags, pp, out) : D3DERR_NOTAVAILABLE;
	}
	if (SUCCEEDED(hr) && out && *out && pp && pp->Windowed && upscale_enabled())
		upscale_device_made(pp->hDeviceWindow ? pp->hDeviceWindow : window);
	if (SUCCEEDED(hr) && out && *out) {
		InterlockedExchangePointer((PVOID volatile *)&device, *out);
		log_printf("d3dcap: CreateDevice(adapter %u, type %d, window %p, flags %lx) -> device %p, vtable %p%s", adapter, (int)type, (void *)window,
			   (unsigned long)flags, (void *)*out, *(void **)*out, ex ? " (Direct3D 9Ex)" : "");
		log_adapter(d3d, adapter);
		hook_shader_dump(*out);
		if (ex)
			hook_resource_creation(*out);
	} else {
		log_printf("d3dcap: CreateDevice failed, hr %lx", (unsigned long)hr);
	}
	return hr;
}

static __thread int in_create_device;

static HRESULT STDMETHODCALLTYPE create_device_hook(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags, D3DPRESENT_PARAMETERS *pp,
						     IDirect3DDevice9 **out)
{
	if (in_create_device) { // d3d9 calls a patched slot again from inside CreateDevice (another object's table): pass it on, the outer call does the work
		create_device_t inner = (create_device_t)factory_original(d3d, VT_CREATE_DEVICE, (void *)create_device_orig);
		return inner ? inner(d3d, adapter, type, window, flags, pp, out) : D3DERR_NOTAVAILABLE;
	}
	in_create_device = 1;
	HRESULT hr = create_device_hook_outer(d3d, adapter, type, window, flags, pp, out);
	in_create_device = 0;
	return hr;
}

static void patch_factory_slot(void **vt, int index, void *hook, void **original)
{
	DWORD old, ignored;
	if (!vt || !hook || !original || !VirtualProtect(&vt[index], sizeof(void *), PAGE_READWRITE, &old))
		return;
	void *current = InterlockedCompareExchangePointer((PVOID volatile *)&vt[index], NULL, NULL);
	if (current != hook) {
		LONG slot = InterlockedIncrement(&factory_original_count) - 1;
		if (slot < FACTORY_ORIGINALS) {
			factory_originals[slot].vt = vt;
			factory_originals[slot].index = index;
			factory_originals[slot].orig = current;
		}
		if (!*original)
			*original = current;
		InterlockedExchangePointer((PVOID volatile *)&vt[index], hook);
	}
	VirtualProtect(&vt[index], sizeof(void *), old, &ignored);
}

static HRESULT STDMETHODCALLTYPE create_device_ex_hook(IDirect3D9Ex *d3d, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
							D3DPRESENT_PARAMETERS *pp, D3DDISPLAYMODEEX *mode, IDirect3DDevice9Ex **out)
{
	create_device_ex_t real = (create_device_ex_t)factory_original(d3d, 20, (void *)create_device_ex_orig);
	HRESULT hr = real ? real(d3d, adapter, type, window, flags, pp, mode, out) : D3DERR_INVALIDCALL;
	if (SUCCEEDED(hr) && out && *out) {
		InterlockedExchangePointer((PVOID volatile *)&device, *out);
		hook_shader_dump((IDirect3DDevice9 *)*out);
		if (use_ex)
			hook_resource_creation((IDirect3DDevice9 *)*out);
		log_printf("d3dcap: CreateDeviceEx(adapter %u, type %d, window %p, flags %lx) -> device %p, vtable %p",
			   adapter, (int)type, (void *)window, (unsigned long)flags, (void *)*out, *(void **)*out);
		log_adapter((IDirect3D9 *)d3d, adapter);
	} else {
		log_printf("d3dcap: CreateDeviceEx failed, hr %lx", (unsigned long)hr);
	}
	return hr;
}

static void hook_factory(IDirect3D9 *d3d)
{
	if (!d3d || !*(void ***)d3d)
		return;
	void **vt = *(void ***)d3d;
	patch_factory_slot(vt, VT_CREATE_DEVICE, (void *)create_device_hook, (void **)&create_device_orig);
	IDirect3D9Ex *ex = NULL;
	if (SUCCEEDED(IDirect3D9_QueryInterface(d3d, &iid_d3d9ex, (void **)&ex)) && ex) {
		void **exvt = *(void ***)ex;
		patch_factory_slot(exvt, 20, (void *)create_device_ex_hook, (void **)&create_device_ex_orig); // IDirect3D9Ex::CreateDeviceEx
		IDirect3D9Ex_Release(ex);
	}
}

// d3d9on12.h (the Windows SDK): the arguments of Direct3DCreate9On12
typedef struct {
	BOOL Enable9On12;
	IUnknown *pD3D12Device;
	IUnknown *pD3D12Queues[2];
	UINT NumQueues;
	UINT NodeMask;
} d3d9on12_args_t;
typedef IDirect3D9 *(WINAPI *create9on12_t)(UINT, d3d9on12_args_t *, UINT);
typedef HRESULT(WINAPI *create9on12ex_t)(UINT, d3d9on12_args_t *, UINT, IDirect3D9Ex **);

// The Direct3D 12 device Direct3D 9 runs on. Left to itself Direct3DCreate9On12 takes the first adapter, which on a laptop with two graphics chips is the NVIDIA one while the display is
// wired to the other: a window above about 800x600 then shows a white or black picture (found with tools/on12test, 2026-11-07). The device is made on the adapter that has the
// primary display attached (the first with an output), or [display] d3d9on12_adapter=<index>.
static IUnknown *make_d3d12_device_for_display(void)
{
	typedef HRESULT(WINAPI * mkfac_t)(const GUID *, void **);
	typedef HRESULT(WINAPI * mkdev_t)(IUnknown *, int, const GUID *, void **);
	static const GUID iid_factory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
	static const GUID iid_device = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
	HMODULE dxgi = LoadLibraryA("dxgi.dll"), d12 = LoadLibraryA("d3d12.dll");
	mkfac_t mkfac = dxgi ? (mkfac_t)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
	mkdev_t mkdev = d12 ? (mkdev_t)(void *)GetProcAddress(d12, "D3D12CreateDevice") : NULL;
	IDXGIFactory1 *fac = NULL;
	IUnknown *dev12 = NULL;
	if (!mkfac || !mkdev || FAILED(mkfac(&iid_factory1, (void **)&fac)) || !fac)
		return NULL;
	int want = settings_d3d9on12_adapter();
	IDXGIAdapter1 *ad = NULL;
	for (UINT i = 0; !dev12 && SUCCEEDED(IDXGIFactory1_EnumAdapters1(fac, i, &ad)); i++) {
		DXGI_ADAPTER_DESC1 desc;
		IDXGIAdapter1_GetDesc1(ad, &desc);
		// does one of its outputs show the primary monitor (the one at 0,0, where the game's window opens)?
		HMONITOR primary = MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
		int has_primary = 0, outputs = 0;
		IDXGIOutput *out = NULL;
		for (UINT o = 0; SUCCEEDED(IDXGIAdapter1_EnumOutputs(ad, o, &out)) && out; o++) {
			DXGI_OUTPUT_DESC od;
			if (SUCCEEDED(IDXGIOutput_GetDesc(out, &od))) {
				outputs++;
				if (od.AttachedToDesktop && od.Monitor == primary)
					has_primary = 1;
				log_printf("d3d9on12:   adapter %u output %u: %ls attached %d primary %d", i, o, od.DeviceName, (int)od.AttachedToDesktop, od.Monitor == primary);
			}
			IDXGIOutput_Release(out);
		}
		int pick = want >= 0 ? (int)i == want : (has_primary && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE));
		log_printf("d3d9on12: adapter %u: %ls, %d outputs, primary monitor %d%s", i, desc.Description, outputs, has_primary, pick ? " <- used" : "");
		if (pick && FAILED(mkdev((IUnknown *)ad, 0xb000 /* D3D_FEATURE_LEVEL_11_0 */, &iid_device, (void **)&dev12)))
			dev12 = NULL;
		if (pick && dev12) {
			static const GUID iid_queue = {0x0ec870a6, 0x5d7e, 0x4c22, {0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed}};
			D3D12_COMMAND_QUEUE_DESC qd;
			memset(&qd, 0, sizeof(qd));
			qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			ID3D12CommandQueue *q = NULL;
			if (SUCCEEDED(ID3D12Device_CreateCommandQueue((ID3D12Device *)dev12, &qd, &iid_queue, (void **)&q)) && q)
				g_queue12 = (IUnknown *)q;
		}
		IDXGIAdapter1_Release(ad);
	}
	IDXGIFactory1_Release(fac);
	return dev12;
}

static IDirect3D9 *WINAPI create9_hook(UINT version)
{
	IDirect3D9 *d3d = NULL;
	if (!use_ex && settings_d3d9on12()) { // experimental, the plain interface: the game's Direct3D 9 on top of Direct3D 12
		HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
		create9on12_t make = d3d9 ? (create9on12_t)(void *)GetProcAddress(d3d9, "Direct3DCreate9On12") : NULL;
		d3d9on12_args_t args;
		memset(&args, 0, sizeof(args));
		args.Enable9On12 = TRUE;
		args.pD3D12Device = g_dev12 = make_d3d12_device_for_display();
		if (g_queue12) {
			args.pD3D12Queues[0] = g_queue12;
			args.NumQueues = 1;
		}
		d3d = make ? make(version, &args, 1) : NULL;
		log_printf("d3d9on12: Direct3DCreate9On12(%u) -> %p%s", version, (void *)d3d, make ? "" : " (not exported by this d3d9.dll)");
	}
	if (use_ex) {
		HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
		create9ex_t make = create9ex_orig;
		if (!make && d3d9) {
			FARPROC proc = getproc_orig ? getproc_orig(d3d9, "Direct3DCreate9Ex") : GetProcAddress(d3d9, "Direct3DCreate9Ex");
			if (proc)
				memcpy(&make, &proc, sizeof(make));
			if (make)
				InterlockedCompareExchangePointer((PVOID volatile *)&create9ex_orig, (PVOID)make, NULL);
		}
		IDirect3D9Ex *ex = NULL;
		if (settings_d3d9on12()) { // Direct3D 9Ex on Direct3D 12
			create9on12ex_t make12 = d3d9 ? (create9on12ex_t)(void *)GetProcAddress(d3d9, "Direct3DCreate9On12Ex") : NULL;
			d3d9on12_args_t args;
			memset(&args, 0, sizeof(args));
			args.Enable9On12 = TRUE;
			args.pD3D12Device = g_dev12 = make_d3d12_device_for_display();
			if (g_queue12) {
				args.pD3D12Queues[0] = g_queue12;
				args.NumQueues = 1;
			}
			if (make12 && SUCCEEDED(make12(version, &args, 1, &ex)) && ex)
				log_printf("d3d9on12: Direct3DCreate9On12Ex(%u) -> %p", version, (void *)ex);
			else
				ex = NULL;
		}
		if (ex) {
			d3d = (IDirect3D9 *)ex;
			ex_version = version;
		} else if (make && SUCCEEDED(make(version, &ex)) && ex) {
			d3d = (IDirect3D9 *)ex;
			ex_version = version;
			log_printf("d3d9ex: Direct3DCreate9Ex(%u) -> %p", version, (void *)ex);
		} else {
			log_printf("d3d9ex: Direct3DCreate9Ex is not available, the plain Direct3D 9 is used");
		}
	}
	if (!d3d && create9_orig)
		d3d = create9_orig(version);
	if (d3d) {
		hook_factory(d3d);
		log_printf("d3dcap: Direct3DCreate9(%u) -> %p, CreateDevice%s hooked", version, (void *)d3d,
			   create_device_ex_orig ? "/CreateDeviceEx" : "");
	}
	return d3d;
}

static HRESULT WINAPI create9ex_hook(UINT version, IDirect3D9Ex **out)
{
	HRESULT hr = create9ex_orig ? create9ex_orig(version, out) : D3DERR_NOTAVAILABLE;
	if (SUCCEEDED(hr) && out && *out) {
		hook_factory((IDirect3D9 *)*out);
		log_printf("d3dcap: Direct3DCreate9Ex(%u) -> %p, CreateDeviceEx hooked", version, (void *)*out);
	}
	return hr;
}

static FARPROC WINAPI getproc_hook(HMODULE module, LPCSTR name)
{
	FARPROC result = getproc_orig ? getproc_orig(module, name) : NULL;
	if (!result || !name || (uintptr_t)name <= 0xffff || module != GetModuleHandleA("d3d9.dll"))
		return result;
	if (!lstrcmpA(name, "Direct3DCreate9")) {
		FARPROC wrapper = create9_hook_proc();
		if (result != wrapper) {
			(void)InterlockedCompareExchangePointer((PVOID volatile *)&create9_orig, (PVOID)result, NULL);
			return wrapper;
		}
	} else if (!lstrcmpA(name, "Direct3DCreate9Ex")) {
		FARPROC wrapper = create9ex_hook_proc();
		if (result != wrapper) {
			(void)InterlockedCompareExchangePointer((PVOID volatile *)&create9ex_orig, (PVOID)result, NULL);
			return wrapper;
		}
	}
	return result;
}

#ifdef _WIN64
// Find a named import in the host PE without relying on the executable's layout.
static void **find_import_slot(HMODULE image, const char *dll, const char *func)
{
	if (!image || !dll || !func)
		return NULL;
	uint8_t *base = (uint8_t *)image;
	IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
		return NULL;
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return NULL;
	IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir->VirtualAddress || !dir->Size)
		return NULL;
	for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); d->Name; d++) {
		if (lstrcmpiA((const char *)(base + d->Name), dll) || !d->OriginalFirstThunk)
			continue;
		IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk);
		IMAGE_THUNK_DATA *thunk = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
		for (; names->u1.AddressOfData; names++, thunk++) {
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
				continue;
			IMAGE_IMPORT_BY_NAME *by_name = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
			if (!lstrcmpA((const char *)by_name->Name, func))
				return (void **)&thunk->u1.Function;
		}
	}
	return NULL;
}
#endif

static int d3dcap_prefix_equal_i(const char *value, const char *prefix)
{
	if (!value || !prefix)
		return 0;
	while (*prefix) {
		unsigned char a = (unsigned char)*value++, b = (unsigned char)*prefix++;
		if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
		if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
		if (a != b)
			return 0;
	}
	return 1;
}

static int patch_import_slot(void **slot, void *replacement, void **original)
{
	DWORD old, ignored;
	if (!slot || !replacement || !original || !VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
		return 0;
	void *current = InterlockedCompareExchangePointer((PVOID volatile *)slot, NULL, NULL);
	int patched = 0;
	if (current && current != replacement) {
		(void)InterlockedCompareExchangePointer((PVOID volatile *)original, current, NULL);
		InterlockedExchangePointer((PVOID volatile *)slot, replacement);
		patched = 1;
	}
	VirtualProtect(slot, sizeof(void *), old, &ignored);
	return patched;
}

static int patch_module_imports_unchecked(HMODULE image)
{
	if (!image)
		return 0;
	uint8_t *base = (uint8_t *)image;
	IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
		return 0;
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return 0;
	IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir->VirtualAddress || !dir->Size)
		return 0;
	int patched = 0;
	for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); imp->Name; imp++) {
		const char *dll = (const char *)(base + imp->Name);
		if (!lstrcmpiA(dll, "d3d9.dll")) {
			if (!imp->OriginalFirstThunk)
				continue; // no import names: do not infer an ordinal from its current pointer
			IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
			IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
			for (; iat->u1.Function && names->u1.AddressOfData; iat++, names++) {
				if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
					continue;
				IMAGE_IMPORT_BY_NAME *by_name = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
				const char *name = (const char *)by_name->Name;
				if (!lstrcmpA(name, "Direct3DCreate9"))
					patched += patch_import_slot((void **)&iat->u1.Function, (void *)create9_hook_proc(), (void **)&create9_orig);
				else if (!lstrcmpA(name, "Direct3DCreate9Ex"))
					patched += patch_import_slot((void **)&iat->u1.Function, (void *)create9ex_hook_proc(), (void **)&create9ex_orig);
			}
		} else if (!lstrcmpiA(dll, "kernel32.dll") || !lstrcmpiA(dll, "kernelbase.dll") ||
			   d3dcap_prefix_equal_i(dll, "api-ms-win-core-libraryloader-")) {
			if (!imp->OriginalFirstThunk)
				continue;
			IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
			IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
			for (; iat->u1.Function && names->u1.AddressOfData; iat++, names++) {
				if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
					continue;
				IMAGE_IMPORT_BY_NAME *by_name = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
				if (!lstrcmpA((const char *)by_name->Name, "GetProcAddress"))
					patched += patch_import_slot((void **)&iat->u1.Function, (void *)getproc_hook, (void **)&getproc_orig);
			}
		}
	}
	return patched;
}

// The module list changes under the scanner: a module can unload between the snapshot and the read, or a region is not readable (an access violation in the scanner thread was seen with
// d3d9ex=1, 2026-11-07). The module is pinned with a reference count while its imports are read, and the header is read only where the memory is committed and readable.
static int patch_module_imports(HMODULE image)
{
	MEMORY_BASIC_INFORMATION mbi;
	HMODULE pinned = NULL;
	if (!image || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)image, &pinned) || !pinned)
		return 0;
	int patched = 0;
	if (VirtualQuery(pinned, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
		patched = patch_module_imports_unchecked(pinned);
	FreeLibrary(pinned);
	return patched;
}

static int patch_loaded_module_imports(void)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
	if (snap == INVALID_HANDLE_VALUE)
		return 0;
	MODULEENTRY32 me;
	memset(&me, 0, sizeof(me));
	me.dwSize = sizeof(me);
	int patched = 0;
	if (Module32First(snap, &me)) {
		do {
			patched += patch_module_imports(me.hModule);
		} while (Module32Next(snap, &me));
	}
	CloseHandle(snap);
	return patched;
}

#ifndef _WIN64
static void hook_create9(HMODULE d3d9);
#endif

static DWORD WINAPI wait_for_d3d9_capture(LPVOID unused)
{
	(void)unused;
	for (int i = 0; i < 1200 && !d3dcap_device(); i++) {
		(void)patch_loaded_module_imports();
#ifndef _WIN64
		HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
		if (d3d9 && !create9_orig)
			hook_create9(d3d9); // preserve the known x86 export-detour fallback for late-loaded IW4x
#endif
		Sleep(100);
	}
	if (!d3dcap_device())
		log_printf("d3dcap: timed out waiting for Direct3DCreate9/Ex device capture");
	return 0;
}

#ifndef _WIN64
static void hook_create9(HMODULE d3d9)
{
	static LONG attempted;
	if (InterlockedCompareExchange(&attempted, 1, 0) != 0)
		return;
	FARPROC proc = getproc_orig ? getproc_orig(d3d9, "Direct3DCreate9") : GetProcAddress(d3d9, "Direct3DCreate9");
	uint8_t *fn = (uint8_t *)proc;
	if (!fn) {
		log_printf("d3dcap: no Direct3DCreate9 in d3d9.dll");
		return;
	}
	static const uint8_t prolog[5] = {0x8b, 0xff, 0x55, 0x8b, 0xec}; // mov edi,edi; push ebp; mov ebp,esp
	if (!hook_function(fn, prolog, sizeof(prolog), (void *)create9_hook, (void **)&create9_orig))
		log_printf("d3dcap: Direct3DCreate9 starts with %02x %02x %02x %02x %02x, not the expected prologue", fn[0], fn[1], fn[2], fn[3], fn[4]);
	else
		log_printf("d3dcap: Direct3DCreate9 hooked at %p", (void *)fn);
}
#endif

void d3dcap_install(void)
{
	// (iw4x-sp keeps control of r_useD3D9Ex: we observe its factory without changing that setting)
	use_ex = game_is_iw4x() ? 0 : (settings_d3d9ex() || settings_d3d9on12()); // Direct3D 9 on Direct3D 12 is made through the Ex interface (the flip model swap chain needs it)
	HMODULE host = GetModuleHandleA(NULL);
	int host_patched = patch_module_imports(host);
	log_printf("d3dcap: host imports captured for cinematics: %d", host_patched);
#ifdef _WIN64
	// The original x64 path only installed this hook when d3d9ex was enabled. Cinematics
	// also need the device in ordinary D3D9 mode, so the same wrapper now captures both.
	void **slot = find_import_slot(host, "d3d9.dll", "Direct3DCreate9");
	if (slot && !create9_orig) {
		DWORD old;
		if (VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
			create9_orig = (create9_t)*slot;
			*slot = (void *)create9_hook_proc();
			DWORD ignored;
			VirtualProtect(slot, sizeof(void *), old, &ignored);
		}
	}
#else
	HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
	if (d3d9 && !create9_orig)
		hook_create9(d3d9);
#endif
	HANDLE thread = CreateThread(NULL, 0, wait_for_d3d9_capture, NULL, 0, NULL);
	if (thread)
		CloseHandle(thread);
	else
		log_printf("d3dcap: could not start the device-capture worker");
}
