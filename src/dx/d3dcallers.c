// QA, MW2COOP_D3DCALLERS=1 (64-bit only): which code of the game calls which Direct3D 9 method. Every method of the device, of the factory (IDirect3D9) and of the resource classes (texture, cube,
// volume, surface, vertex and index buffer, query, shaders, declaration, swap chain) gets a small stub in its vtable that records (class, slot, return address) and jumps on. The table is written to
// <game>\mw2coop\d3dcallers.txt every few seconds. tools/ghidra/FunctionOf.java turns the return addresses into the functions that make the calls: the exact list of what a native Direct3D 11 / 12
// back end has to take over (docs/native-dx-plan.md, "The owner's rule").
#include "d3dcallers.h"

#ifdef _WIN64
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../log.h"

enum { C_DEVICE, C_D3D9, C_TEXTURE, C_CUBE, C_VOLUME, C_SURFACE, C_VB, C_IB, C_QUERY, C_VS, C_PS, C_DECL, C_SWAPCHAIN, C_COUNT };
static const char *const CLASS_NAMES[C_COUNT] = {"Device", "D3D9", "Texture", "Cube", "Volume", "Surface", "VertexBuffer", "IndexBuffer", "Query", "VertexShader", "PixelShader", "Declaration", "SwapChain"};
static const int CLASS_SLOTS[C_COUNT] = {119, 17, 22, 22, 22, 17, 14, 14, 8, 5, 5, 5, 10};

#define TABLE_SIZE 65536
static struct {
	volatile LONG64 key; // (class << 56 | slot << 48 | return address) + 1; 0 = free
	volatile LONG count;
} table[TABLE_SIZE];

static void record(unsigned id, uintptr_t ret)
{
	const LONG64 key = (LONG64)(((uint64_t)id << 48) | (ret & 0xffffffffffffull)) + 1;
	size_t h = (size_t)((uint64_t)key * 0x9e3779b97f4a7c15ull >> 48) & (TABLE_SIZE - 1);
	for (int n = 0; n < TABLE_SIZE; n++, h = (h + 1) & (TABLE_SIZE - 1)) {
		LONG64 k = table[h].key;
		if (k == key) {
			InterlockedIncrement(&table[h].count);
			return;
		}
		if (k == 0) {
			k = InterlockedCompareExchange64(&table[h].key, key, 0);
			if (k == 0 || k == key) {
				InterlockedIncrement(&table[h].count);
				return;
			}
		}
	}
}

static uint8_t *stub_page;
static size_t stub_used;

static void *make_stub(unsigned id, void *orig)
{
	static const uint8_t pro[] = {0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xec, 0x28};
	if (!stub_page) {
		stub_page = VirtualAlloc(NULL, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (!stub_page)
			return NULL;
	}
	uint8_t *p = stub_page + stub_used, *q = p;
	memcpy(q, pro, sizeof(pro));
	q += sizeof(pro);
	*q++ = 0xb9; // mov ecx, id
	memcpy(q, &id, 4);
	q += 4;
	*q++ = 0x48; *q++ = 0x8b; *q++ = 0x54; *q++ = 0x24; *q++ = 0x48; // mov rdx, [rsp+48h]
	*q++ = 0x48; *q++ = 0xb8; // mov rax, record
	void *rec = (void *)record;
	memcpy(q, &rec, 8);
	q += 8;
	*q++ = 0xff; *q++ = 0xd0; // call rax
	*q++ = 0x48; *q++ = 0x83; *q++ = 0xc4; *q++ = 0x28; // add rsp, 28h
	*q++ = 0x41; *q++ = 0x59; // pop r9
	*q++ = 0x41; *q++ = 0x58; // pop r8
	*q++ = 0x5a;              // pop rdx
	*q++ = 0x59;              // pop rcx
	*q++ = 0x48; *q++ = 0xb8; // mov rax, orig
	memcpy(q, &orig, 8);
	q += 8;
	*q++ = 0xff; *q++ = 0xe0; // jmp rax
	stub_used += (size_t)(q - p + 15) & ~15u;
	return p;
}

static void *hooked[C_COUNT];

static unsigned class_mask = 0xffffffffu;

static void hook_class(int cls, void *object)
{
	if (!object || !(class_mask & (1u << cls)))
		return;
	void **vt = *(void ***)object;
	if (hooked[cls] == vt)
		return;
	hooked[cls] = vt;
	for (int s = 0; s < CLASS_SLOTS[cls]; s++) {
		void *stub = make_stub((unsigned)(cls << 8 | s), vt[s]);
		DWORD old;
		if (stub && VirtualProtect(&vt[s], sizeof(void *), PAGE_READWRITE, &old)) {
			vt[s] = stub;
			VirtualProtect(&vt[s], sizeof(void *), old, &old);
		}
	}
}

static DWORD WINAPI writer(LPVOID arg)
{
	(void)arg;
	char path[MAX_PATH];
	DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
	while (n > 0 && path[n - 1] != '\\')
		n--;
	path[n] = 0;
	lstrcatA(path, "mw2coop\\d3dcallers.txt");
	for (;;) {
		Sleep(3000);
		FILE *f = fopen(path, "w");
		if (!f)
			continue;
		const uint8_t *exe = (const uint8_t *)GetModuleHandleA(NULL);
		for (int i = 0; i < TABLE_SIZE; i++) {
			const LONG64 k = table[i].key;
			if (!k)
				continue;
			const uint64_t v = (uint64_t)(k - 1);
			const unsigned id = (unsigned)(v >> 48), cls = id >> 8, slot = id & 255;
			const uintptr_t ret = (uintptr_t)(v & 0xffffffffffffull);
			HMODULE m = NULL;
			char mod[64] = "?";
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ret, &m) && m) {
				char full[MAX_PATH];
				GetModuleFileNameA(m, full, MAX_PATH);
				const char *base = strrchr(full, '\\');
				lstrcpynA(mod, base ? base + 1 : full, sizeof(mod));
			}
			fprintf(f, "%s %u %s %llx %ld\n", cls < C_COUNT ? CLASS_NAMES[cls] : "?", slot, mod, (unsigned long long)(ret - (uintptr_t)(m ? (const uint8_t *)m : exe)), (long)table[i].count);
		}
		fclose(f);
	}
	return 0;
}

void d3dcallers_hook_device(struct IDirect3DDevice9 *d)
{
	static int started;
	if (!getenv("MW2COOP_D3DCALLERS") || started || !d)
		return;
	started = 1;
	if (getenv("MW2COOP_D3DCALLERS")[0] && getenv("MW2COOP_D3DCALLERS")[0] != '1')
		class_mask = (unsigned)strtoul(getenv("MW2COOP_D3DCALLERS"), NULL, 16); // a hex mask of the classes to count (bit 0 device, 1 factory, 2 texture ...), for finding a class that cannot be hooked
	IDirect3DDevice9 *dev = (IDirect3DDevice9 *)d;
	// sample objects of every class (the vtables are the classes', shared with the game's objects); made before the device is hooked so that they are not counted
	IDirect3DTexture9 *tex = NULL;
	IDirect3DCubeTexture9 *cube = NULL;
	IDirect3DVolumeTexture9 *vol = NULL;
	IDirect3DSurface9 *surf = NULL;
	IDirect3DVertexBuffer9 *vb = NULL;
	IDirect3DIndexBuffer9 *ib = NULL;
	IDirect3DQuery9 *query = NULL;
	IDirect3DVertexShader9 *vs = NULL;
	IDirect3DPixelShader9 *ps = NULL;
	IDirect3DVertexDeclaration9 *decl = NULL;
	IDirect3DSwapChain9 *sc = NULL;
	IDirect3D9 *d3d = NULL;
	IDirect3DDevice9_CreateTexture(dev, 16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL);
	IDirect3DDevice9_CreateCubeTexture(dev, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, NULL);
	IDirect3DDevice9_CreateVolumeTexture(dev, 8, 8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &vol, NULL);
	IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 16, 16, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &surf, NULL);
	IDirect3DDevice9_CreateVertexBuffer(dev, 64, 0, 0, D3DPOOL_MANAGED, &vb, NULL);
	IDirect3DDevice9_CreateIndexBuffer(dev, 64, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL);
	IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_EVENT, &query);
	{
		static const DWORD vsc[] = {0xfffe0101, 0x02000001, 0xc00f0000, 0x90e40000, 0x0000ffff};
		static const DWORD psc[] = {0xffff0101, 0x02000001, 0x800f0000, 0xa0e40000, 0x0000ffff};
		IDirect3DDevice9_CreateVertexShader(dev, vsc, &vs);
		IDirect3DDevice9_CreatePixelShader(dev, psc, &ps);
		static const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
		IDirect3DDevice9_CreateVertexDeclaration(dev, el, &decl);
	}
	IDirect3DDevice9_GetSwapChain(dev, 0, &sc);
	IDirect3DDevice9_GetDirect3D(dev, &d3d);
	hook_class(C_TEXTURE, tex);
	hook_class(C_CUBE, cube);
	hook_class(C_VOLUME, vol);
	hook_class(C_SURFACE, surf);
	hook_class(C_VB, vb);
	hook_class(C_IB, ib);
	hook_class(C_QUERY, query);
	hook_class(C_VS, vs);
	hook_class(C_PS, ps);
	hook_class(C_DECL, decl);
	hook_class(C_SWAPCHAIN, sc);
	hook_class(C_D3D9, d3d);
	hook_class(C_DEVICE, dev);
	CloseHandle(CreateThread(NULL, 0, writer, NULL, 0, NULL));
	log_printf("d3dcallers: every Direct3D 9 method is counted by caller (written to mw2coop\\d3dcallers.txt)");
}
#else
void d3dcallers_hook_device(struct IDirect3DDevice9 *d)
{
	(void)d;
}
#endif
