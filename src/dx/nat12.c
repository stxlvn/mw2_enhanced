// The native Direct3D 12 device layer (the same API as nat11.c, see nat11.h; the private interface to natdraw12.c is nat12x.h). No Direct3D 9 or 11 object. Windows, 64 bit.
// Design: one direct queue and one command list recorded on the render thread; images stay in COMMON (plain textures: promoted implicitly by the first read) except render targets and depth
// images, whose states are tracked; uploads to textures go through a copy queue, synchronously, from whatever thread asks (the asset loader); buffers live in upload heaps (persistently mapped,
// versions rotate on DISCARD); constants, descriptors and samplers are allocated from per-frame rings.
#include "nat11.h"

#ifdef _WIN32
#include "nat12x.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sm3.h"

#ifndef D3DCOMPILE_ENABLE_STRICTNESS
#define D3DCOMPILE_ENABLE_STRICTNESS (1 << 11)
#endif

#define REL(x) \
	do { \
		if (x) { \
			IUnknown_Release((IUnknown *)(x)); \
			(x) = NULL; \
		} \
	} while (0)

typedef struct ID3DBlob ID3DBlob_;

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ the compiler
typedef struct ID3D10Blob_s ID3D10Blob_s;
static HMODULE h_compiler;
static HRESULT(WINAPI *p_compile)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT, UINT, IUnknown **, IUnknown **);
// ID3DBlob has the vtable of IUnknown plus GetBufferPointer / GetBufferSize
typedef struct {
	const struct {
		HRESULT(STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);
		ULONG(STDMETHODCALLTYPE *AddRef)(void *);
		ULONG(STDMETHODCALLTYPE *Release)(void *);
		void *(STDMETHODCALLTYPE *GetBufferPointer)(void *);
		SIZE_T(STDMETHODCALLTYPE *GetBufferSize)(void *);
	} *vt;
} blob_t;
static void *blob_ptr(IUnknown *b) { return ((blob_t *)b)->vt->GetBufferPointer(b); }
static SIZE_T blob_size(IUnknown *b) { return ((blob_t *)b)->vt->GetBufferSize(b); }

static int load_compiler(void)
{
	if (p_compile)
		return 1;
	h_compiler = LoadLibraryA("d3dcompiler_47.dll");
	if (h_compiler)
		p_compile = (void *)GetProcAddress(h_compiler, "D3DCompile");
	return p_compile != NULL;
}

static void set_err(char *err, size_t n, const char *fmt, unsigned long a)
{
	if (err && n)
		snprintf(err, n, fmt, a);
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ descriptor heaps
typedef struct {
	ID3D12DescriptorHeap *heap;
	D3D12_CPU_DESCRIPTOR_HANDLE base;
	D3D12_GPU_DESCRIPTOR_HANDLE gbase;
	UINT inc, cap, next, nfree;
	UINT *freel;
} dheap;

static int dheap_init(ID3D12Device *dev, dheap *h, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT cap, int visible)
{
	D3D12_DESCRIPTOR_HEAP_DESC dd = {type, cap, visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
	if (FAILED(ID3D12Device_CreateDescriptorHeap(dev, &dd, &IID_ID3D12DescriptorHeap, (void **)&h->heap)))
		return 0;
	h->base = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(h->heap);
	if (visible)
		h->gbase = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(h->heap);
	h->inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, type);
	h->cap = cap;
	h->freel = calloc(cap, sizeof(UINT));
	return h->freel != NULL;
}
static UINT dheap_alloc(dheap *h) // index + 1, 0 = full
{
	if (h->nfree)
		return h->freel[--h->nfree] + 1;
	if (h->next < h->cap)
		return ++h->next;
	return 0;
}
static void dheap_free(dheap *h, UINT idx1)
{
	if (idx1)
		h->freel[h->nfree++] = idx1 - 1;
}
static D3D12_CPU_DESCRIPTOR_HANDLE dheap_cpu(const dheap *h, UINT idx1)
{
	D3D12_CPU_DESCRIPTOR_HANDLE c = {h->base.ptr + (SIZE_T)(idx1 - 1) * h->inc};
	return c;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ the device
typedef struct {
	ID3D12Resource *res;
	UINT64 value;
} garbage_t;

// static vertex / index buffers are cut from big upload buffers (a committed buffer costs a 64 KB heap each, and a level has tens of thousands of them)
typedef struct chunk {
	ID3D12Resource *res;
	uint8_t *map;
	UINT64 size, used;
	int live, full;
} chunk_t;

struct nat_device {
	ID3D12Device *dev;
	ID3D12CommandQueue *queue, *cqueue;
	ID3D12Fence *fence, *cfence;
	HANDLE fevent, cevent;
	UINT64 pending; // the value the next submission signals
	UINT64 cvalue;
	IDXGIFactory4 *factory;
	IDXGISwapChain3 *swap;
	ID3D12Resource *back[NAT12_FRAMES];
	UINT nback;
	HWND hwnd;
	nat_params p;
	nat_caps caps;
	unsigned samples;
	int fullscreen_set;
	// commands
	ID3D12CommandAllocator *alloc[NAT12_FRAMES];
	UINT64 alloc_value[NAT12_FRAMES];
	unsigned fidx;
	ID3D12GraphicsCommandList *list;
	unsigned list_gen;
	int heaps_set;
	int cmds; // commands recorded since the list was reset
	// copy queue (texture uploads)
	ID3D12CommandAllocator *calloc_;
	ID3D12GraphicsCommandList *clist;
	CRITICAL_SECTION ccs, lock;
	// descriptors
	dheap rtv, dsv, srv, smp;
	dheap gsrv, gsmp; // shader-visible
	UINT gsrv_seg_size, gsrv_off;
	UINT gsmp_next;
	struct {
		UINT64 key[16];
		UINT idx;
	} stbl[96];
	UINT n_stbl;
	// constants ring
	ID3D12Resource *ring;
	uint8_t *ring_map;
	UINT64 ring_size, ring_seg, ring_off;
	// scene
	nat_image *scene, *depth, *resolved;
	ID3D12Resource *null_dummy;
	UINT null_srv;
	// deferred releases
	garbage_t *gc;
	size_t gc_n, gc_cap;
	chunk_t *cur_chunk;
	// occlusion queries
	ID3D12QueryHeap *qheap;
	ID3D12Resource *qread;
	uint8_t *qread_map;
	UINT qnext;
	struct nat_query **qended;
	unsigned qended_n, qended_cap;
};

ID3D12Device *nat12_device(nat_device *d) { return d->dev; }
ID3D12GraphicsCommandList *nat12_list(nat_device *d) { return d->list; }
unsigned nat12_list_generation(const nat_device *d) { return d->list_gen; }
unsigned long long nat12_pending_value(const nat_device *d) { return d->pending; }
unsigned long long nat12_completed_value(nat_device *d) { return ID3D12Fence_GetCompletedValue(d->fence); }
unsigned nat12_scene_samples(const nat_device *d) { return d->samples; }

static void wait_value(nat_device *d, UINT64 v)
{
	if (ID3D12Fence_GetCompletedValue(d->fence) >= v)
		return;
	ID3D12Fence_SetEventOnCompletion(d->fence, v, d->fevent);
	WaitForSingleObject(d->fevent, INFINITE);
}

static void gc_add(nat_device *d, ID3D12Resource *r)
{
	if (!r)
		return;
	EnterCriticalSection(&d->lock);
	if (d->gc_n == d->gc_cap) {
		d->gc_cap = d->gc_cap ? d->gc_cap * 2 : 256;
		d->gc = realloc(d->gc, d->gc_cap * sizeof(*d->gc));
	}
	d->gc[d->gc_n].res = r;
	d->gc[d->gc_n].value = d->pending;
	d->gc_n++;
	LeaveCriticalSection(&d->lock);
}
static void gc_collect(nat_device *d, int all)
{
	const UINT64 done = ID3D12Fence_GetCompletedValue(d->fence);
	EnterCriticalSection(&d->lock);
	size_t k = 0;
	for (size_t i = 0; i < d->gc_n; i++) {
		if (all || d->gc[i].value <= done)
			ID3D12Resource_Release(d->gc[i].res);
		else
			d->gc[k++] = d->gc[i];
	}
	d->gc_n = k;
	LeaveCriticalSection(&d->lock);
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ command list and rings
static void begin_list(nat_device *d)
{
	ID3D12CommandAllocator_Reset(d->alloc[d->fidx]);
	ID3D12GraphicsCommandList_Reset(d->list, d->alloc[d->fidx], NULL);
	d->list_gen++;
	d->cmds = 0;
	d->heaps_set = 0;
	d->ring_off = 0;
	d->ring_seg = (UINT64)d->fidx * (d->ring_size / NAT12_FRAMES);
	d->gsrv_off = 0;
}

void nat12_set_heaps(nat_device *d)
{
	if (d->heaps_set)
		return;
	ID3D12DescriptorHeap *h[2] = {d->gsrv.heap, d->gsmp.heap};
	ID3D12GraphicsCommandList_SetDescriptorHeaps(d->list, 2, h);
	d->heaps_set = 1;
}

static void submit(nat_device *d, int wait_idle)
{
	// the occlusion queries ended in this list: their results go to the readback buffer
	for (unsigned i = 0; i < d->qended_n; i++) {
		struct nat_query *q = d->qended[i];
		extern void nat12_query_resolve(nat_device *, struct nat_query *);
		nat12_query_resolve(d, q);
	}
	d->qended_n = 0;
	ID3D12GraphicsCommandList_Close(d->list);
	ID3D12CommandList *l[1] = {(ID3D12CommandList *)d->list};
	ID3D12CommandQueue_ExecuteCommandLists(d->queue, 1, l);
	ID3D12CommandQueue_Signal(d->queue, d->fence, d->pending);
	d->alloc_value[d->fidx] = d->pending;
	d->pending++;
	if (wait_idle)
		wait_value(d, d->pending - 1);
}

void nat12_note_commands(nat_device *d) { d->cmds = 1; }

void nat12_flush(nat_device *d)
{
	submit(d, 1);
	gc_collect(d, 0);
	begin_list(d);
}

void *nat12_upload_alloc(nat_device *d, unsigned size, D3D12_GPU_VIRTUAL_ADDRESS *gpu)
{
	const UINT64 seg = d->ring_size / NAT12_FRAMES;
	const UINT64 need = ((UINT64)size + 255) & ~255ull;
	if (d->ring_off + need > seg)
		return NULL;
	void *p = d->ring_map + d->ring_seg + d->ring_off;
	*gpu = ID3D12Resource_GetGPUVirtualAddress(d->ring) + d->ring_seg + d->ring_off;
	d->ring_off += need;
	return p;
}

int nat12_srv_alloc(nat_device *d, unsigned count, D3D12_CPU_DESCRIPTOR_HANDLE *cpu, D3D12_GPU_DESCRIPTOR_HANDLE *gpu)
{
	if (d->gsrv_off + count > d->gsrv_seg_size)
		return 0;
	const UINT idx = d->fidx * d->gsrv_seg_size + d->gsrv_off;
	cpu->ptr = d->gsrv.base.ptr + (SIZE_T)idx * d->gsrv.inc;
	gpu->ptr = d->gsrv.gbase.ptr + (UINT64)idx * d->gsrv.inc;
	d->gsrv_off += count;
	return 1;
}

int nat12_sampler_create(nat_device *d, const D3D12_SAMPLER_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE *cpu)
{
	EnterCriticalSection(&d->lock);
	const UINT i = dheap_alloc(&d->smp);
	LeaveCriticalSection(&d->lock);
	if (!i)
		return 0;
	*cpu = dheap_cpu(&d->smp, i);
	ID3D12Device_CreateSampler(d->dev, desc, *cpu);
	return 1;
}

// a table of 16 sampler descriptors in the shader-visible heap, made once for each distinct set of 16 (the heap holds 2048 descriptors)
int nat12_sampler_table(nat_device *d, const D3D12_CPU_DESCRIPTOR_HANDLE cpu[16], D3D12_GPU_DESCRIPTOR_HANDLE *gpu)
{
	UINT64 key[16];
	for (int i = 0; i < 16; i++)
		key[i] = cpu[i].ptr;
	for (UINT t = 0; t < d->n_stbl; t++)
		if (!memcmp(d->stbl[t].key, key, sizeof(key))) {
			gpu->ptr = d->gsmp.gbase.ptr + (UINT64)d->stbl[t].idx * d->gsmp.inc;
			return 1;
		}
	if (d->n_stbl >= sizeof(d->stbl) / sizeof(d->stbl[0]) || d->gsmp_next + 16 > d->gsmp.cap) {
		// the table cache is full: nothing recorded so far may keep using the heap's tables, so finish the frame's commands first
		nat12_flush(d);
		d->n_stbl = 0;
		d->gsmp_next = 0;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE dst = {d->gsmp.base.ptr + (SIZE_T)d->gsmp_next * d->gsmp.inc};
	for (int i = 0; i < 16; i++) {
		D3D12_CPU_DESCRIPTOR_HANDLE one = {dst.ptr + (SIZE_T)i * d->gsmp.inc};
		ID3D12Device_CopyDescriptorsSimple(d->dev, 1, one, cpu[i], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
	}
	memcpy(d->stbl[d->n_stbl].key, key, sizeof(key));
	d->stbl[d->n_stbl].idx = d->gsmp_next;
	gpu->ptr = d->gsmp.gbase.ptr + (UINT64)d->gsmp_next * d->gsmp.inc;
	d->n_stbl++;
	d->gsmp_next += 16;
	return 1;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ resources
static ID3D12Resource *make_buffer(nat_device *d, UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
{
	D3D12_HEAP_PROPERTIES hp = {heap, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
	D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, size ? size : 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
	ID3D12Resource *r = NULL;
	if (FAILED(ID3D12Device_CreateCommittedResource(d->dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL, &IID_ID3D12Resource, (void **)&r)))
		return NULL;
	return r;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ images
typedef struct {
	DXGI_FORMAT dxgi, srv, srgb;
	int expand;
	int block;
	int bpp;
	int depth;
} fmt_t;

static int fmt_info(unsigned f, fmt_t *o)
{
	memset(o, 0, sizeof(*o));
	switch (f) {
	case 21: o->dxgi = DXGI_FORMAT_B8G8R8A8_TYPELESS; o->srv = DXGI_FORMAT_B8G8R8A8_UNORM; o->srgb = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; o->bpp = 4; return 1;
	case 22: o->dxgi = DXGI_FORMAT_B8G8R8X8_TYPELESS; o->srv = DXGI_FORMAT_B8G8R8X8_UNORM; o->srgb = DXGI_FORMAT_B8G8R8X8_UNORM_SRGB; o->bpp = 4; return 1;
	case 0x31545844: o->dxgi = DXGI_FORMAT_BC1_TYPELESS; o->srv = DXGI_FORMAT_BC1_UNORM; o->srgb = DXGI_FORMAT_BC1_UNORM_SRGB; o->block = 8; return 1;
	case 0x33545844: o->dxgi = DXGI_FORMAT_BC2_TYPELESS; o->srv = DXGI_FORMAT_BC2_UNORM; o->srgb = DXGI_FORMAT_BC2_UNORM_SRGB; o->block = 16; return 1;
	case 0x35545844: o->dxgi = DXGI_FORMAT_BC3_TYPELESS; o->srv = DXGI_FORMAT_BC3_UNORM; o->srgb = DXGI_FORMAT_BC3_UNORM_SRGB; o->block = 16; return 1;
	case 28: o->dxgi = o->srv = DXGI_FORMAT_A8_UNORM; o->bpp = 1; return 1;
	case 50: o->dxgi = DXGI_FORMAT_B8G8R8A8_TYPELESS; o->srv = DXGI_FORMAT_B8G8R8A8_UNORM; o->expand = 1; o->bpp = 1; return 1;
	case 51: o->dxgi = DXGI_FORMAT_B8G8R8A8_TYPELESS; o->srv = DXGI_FORMAT_B8G8R8A8_UNORM; o->expand = 2; o->bpp = 2; return 1;
	case 113: o->dxgi = o->srv = DXGI_FORMAT_R16G16B16A16_FLOAT; o->bpp = 8; return 1;
	case 116: o->dxgi = o->srv = DXGI_FORMAT_R32G32B32A32_FLOAT; o->bpp = 16; return 1;
	case 111: o->dxgi = o->srv = DXGI_FORMAT_R16_FLOAT; o->bpp = 2; return 1;
	case 112: o->dxgi = o->srv = DXGI_FORMAT_R16G16_FLOAT; o->bpp = 4; return 1;
	case 114: o->dxgi = o->srv = DXGI_FORMAT_R32_FLOAT; o->bpp = 4; return 1;
	case 115: o->dxgi = o->srv = DXGI_FORMAT_R32G32_FLOAT; o->bpp = 8; return 1;
	case 34: o->dxgi = o->srv = DXGI_FORMAT_R16G16_UNORM; o->bpp = 4; return 1;
	case 36: o->dxgi = o->srv = DXGI_FORMAT_R16G16B16A16_UNORM; o->bpp = 8; return 1;
	case 31: o->dxgi = o->srv = DXGI_FORMAT_R10G10B10A2_UNORM; o->bpp = 4; return 1;
	case 0x5a544e49: case 75: case 77: o->dxgi = DXGI_FORMAT_R24G8_TYPELESS; o->srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; o->bpp = 4; o->depth = 1; return 1;
	case 80: case 70: o->dxgi = DXGI_FORMAT_R16_TYPELESS; o->srv = DXGI_FORMAT_R16_UNORM; o->bpp = 2; o->depth = 1; return 1;
	case 0x4c4c554e: o->dxgi = DXGI_FORMAT_B8G8R8A8_TYPELESS; o->srv = DXGI_FORMAT_B8G8R8A8_UNORM; o->bpp = 4; return 1;
	}
	return 0;
}

// An image starts with a Direct3D 9 style vtable (Release +0x10, AddRef +0x8) for the engine's unload code that releases the stored pointer itself.
static void *image_vt[4];
static long image_nop(void *self)
{
	(void)self;
	return 0;
}
static long image_release(void *self);
static long image_addref(void *self);

struct nat_image {
	void **vt;
	int refs;
	nat_device *dev;
	nat_image_desc desc;
	fmt_t f;
	ID3D12Resource *res;
	UINT srv, srv_srgb, rtv, rtv_srgb, dsv; // descriptor indices + 1 (0 = none)
	D3D12_RESOURCE_STATES state;
	int tracked; // a render target or depth image: its state is transitioned explicitly
	unsigned levels, samples;
	DXGI_FORMAT dsv_fmt;
};

static void image_views(nat_device *d, nat_image *im)
{
	const nat_image_desc *desc = &im->desc;
	const int rt = (desc->flags & NAT_RENDER_TARGET) != 0, ds = (desc->flags & NAT_DEPTH) != 0 && desc->kind == NAT_IMAGE_2D;
	if (im->samples == 1) {
		D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
		sd.Format = im->f.srv;
		sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		if (desc->kind == NAT_IMAGE_CUBE) {
			sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
			sd.TextureCube.MipLevels = im->levels;
		} else if (desc->kind == NAT_IMAGE_VOLUME) {
			sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
			sd.Texture3D.MipLevels = im->levels;
		} else {
			sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			sd.Texture2D.MipLevels = im->levels;
		}
		EnterCriticalSection(&d->lock);
		im->srv = dheap_alloc(&d->srv);
		if (im->f.srgb)
			im->srv_srgb = dheap_alloc(&d->srv);
		LeaveCriticalSection(&d->lock);
		if (im->srv)
			ID3D12Device_CreateShaderResourceView(d->dev, im->res, &sd, dheap_cpu(&d->srv, im->srv));
		if (im->srv_srgb) {
			sd.Format = im->f.srgb;
			ID3D12Device_CreateShaderResourceView(d->dev, im->res, &sd, dheap_cpu(&d->srv, im->srv_srgb));
		}
	}
	if (rt) {
		D3D12_RENDER_TARGET_VIEW_DESC rd = {0};
		rd.Format = im->f.srv;
		rd.ViewDimension = im->samples > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
		EnterCriticalSection(&d->lock);
		im->rtv = dheap_alloc(&d->rtv);
		if (im->f.srgb)
			im->rtv_srgb = dheap_alloc(&d->rtv);
		LeaveCriticalSection(&d->lock);
		if (im->rtv)
			ID3D12Device_CreateRenderTargetView(d->dev, im->res, &rd, dheap_cpu(&d->rtv, im->rtv));
		if (im->rtv_srgb) {
			rd.Format = im->f.srgb;
			ID3D12Device_CreateRenderTargetView(d->dev, im->res, &rd, dheap_cpu(&d->rtv, im->rtv_srgb));
		}
	}
	if (ds) {
		D3D12_DEPTH_STENCIL_VIEW_DESC dd = {0};
		dd.Format = im->dsv_fmt;
		dd.ViewDimension = im->samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
		EnterCriticalSection(&d->lock);
		im->dsv = dheap_alloc(&d->dsv);
		LeaveCriticalSection(&d->lock);
		if (im->dsv)
			ID3D12Device_CreateDepthStencilView(d->dev, im->res, &dd, dheap_cpu(&d->dsv, im->dsv));
	}
}

static unsigned mips_of(unsigned w, unsigned h, unsigned d)
{
	unsigned n = 1;
	while (w > 1 || h > 1 || d > 1) {
		w = w > 1 ? w >> 1 : 1;
		h = h > 1 ? h >> 1 : 1;
		d = d > 1 ? d >> 1 : 1;
		n++;
	}
	return n;
}

nat_image *nat_image_create(nat_device *d, const nat_image_desc *desc)
{
	fmt_t f;
	if (!fmt_info(desc->d3d9_format, &f))
		return NULL;
	nat_image *im = calloc(1, sizeof(*im));
	if (!image_vt[0]) {
		image_vt[0] = image_vt[3] = (void *)image_nop;
		image_vt[1] = (void *)image_addref;
		image_vt[2] = (void *)image_release;
	}
	im->vt = image_vt;
	im->refs = 1;
	im->dev = d;
	im->desc = *desc;
	im->f = f;
	const int rt = (desc->flags & NAT_RENDER_TARGET) != 0, ds = (desc->flags & NAT_DEPTH) != 0 && desc->kind == NAT_IMAGE_2D;
	im->levels = desc->levels ? desc->levels : mips_of(desc->w, desc->h, desc->kind == NAT_IMAGE_VOLUME ? desc->d : 1);
	im->samples = rt || ds ? (unsigned)(desc->msaa > 1 ? desc->msaa : 1) : 1;
	im->tracked = rt || ds;
	im->dsv_fmt = f.dxgi == DXGI_FORMAT_R16_TYPELESS ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D24_UNORM_S8_UINT;
	D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
	D3D12_RESOURCE_DESC rd = {0};
	rd.Dimension = desc->kind == NAT_IMAGE_VOLUME ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rd.Width = desc->w;
	rd.Height = desc->kind == NAT_IMAGE_CUBE ? desc->w : desc->h;
	rd.DepthOrArraySize = (UINT16)(desc->kind == NAT_IMAGE_CUBE ? 6 : desc->kind == NAT_IMAGE_VOLUME ? (desc->d ? desc->d : 1) : 1);
	rd.MipLevels = (UINT16)im->levels;
	rd.Format = f.dxgi;
	rd.SampleDesc.Count = im->samples;
	rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	if (rt)
		rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	if (ds)
		rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	im->state = D3D12_RESOURCE_STATE_COMMON;
	if (FAILED(ID3D12Device_CreateCommittedResource(d->dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&im->res))) {
		free(im);
		return NULL;
	}
	image_views(d, im);
	return im;
}

void nat_image_addref(nat_image *i)
{
	if (i)
		i->refs++;
}

void nat_image_destroy(nat_image *i)
{
	if (!i)
		return;
	if (--i->refs > 0)
		return;
	nat_device *d = i->dev;
	EnterCriticalSection(&d->lock);
	dheap_free(&d->srv, i->srv);
	dheap_free(&d->srv, i->srv_srgb);
	dheap_free(&d->rtv, i->rtv);
	dheap_free(&d->rtv, i->rtv_srgb);
	dheap_free(&d->dsv, i->dsv);
	LeaveCriticalSection(&d->lock);
	gc_add(d, i->res);
	free(i);
}
static long image_release(void *self)
{
	nat_image_destroy((nat_image *)self);
	return 0;
}
static long image_addref(void *self)
{
	nat_image_addref((nat_image *)self);
	return 0;
}

// -- uploads: a copy queue, synchronously
static int copy_queue_init(nat_device *d)
{
	D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_COPY, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
	if (FAILED(ID3D12Device_CreateCommandQueue(d->dev, &qd, &IID_ID3D12CommandQueue, (void **)&d->cqueue)))
		return 0;
	if (FAILED(ID3D12Device_CreateCommandAllocator(d->dev, D3D12_COMMAND_LIST_TYPE_COPY, &IID_ID3D12CommandAllocator, (void **)&d->calloc_)))
		return 0;
	if (FAILED(ID3D12Device_CreateCommandList(d->dev, 0, D3D12_COMMAND_LIST_TYPE_COPY, d->calloc_, NULL, &IID_ID3D12GraphicsCommandList, (void **)&d->clist)))
		return 0;
	ID3D12GraphicsCommandList_Close(d->clist);
	if (FAILED(ID3D12Device_CreateFence(d->dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&d->cfence)))
		return 0;
	d->cevent = CreateEventA(NULL, FALSE, FALSE, NULL);
	return d->cevent != NULL;
}

int nat_image_upload(nat_image *i, unsigned face, unsigned level, const void *data, unsigned row_pitch, unsigned slice_pitch)
{
	if (!i || level >= i->levels || (i->desc.kind == NAT_IMAGE_CUBE && face > 5))
		return 0;
	nat_device *d = i->dev;
	const unsigned w = i->desc.w >> level ? i->desc.w >> level : 1, h = i->desc.h >> level ? i->desc.h >> level : 1;
	const unsigned dl = i->desc.kind == NAT_IMAGE_VOLUME ? (i->desc.d >> level ? i->desc.d >> level : 1) : 1;
	const UINT sub = (UINT)(i->desc.kind == NAT_IMAGE_CUBE ? level + face * i->levels : level);
	uint32_t *tmp = NULL;
	if (i->f.expand) {
		tmp = malloc((size_t)w * h * dl * 4);
		const uint8_t *src = data;
		const unsigned unit = i->f.expand == 1 ? 1 : 2;
		for (unsigned z = 0; z < dl; z++)
			for (unsigned y = 0; y < h; y++)
				for (unsigned x = 0; x < w; x++) {
					const uint8_t *s = src + (size_t)z * slice_pitch + (size_t)y * row_pitch + (size_t)x * unit;
					const uint32_t lum = s[0], a = unit == 1 ? 255 : s[1];
					tmp[((size_t)z * h + y) * w + x] = a << 24 | lum << 16 | lum << 8 | lum;
				}
		data = tmp;
		row_pitch = w * 4;
		slice_pitch = w * h * 4;
	}
	D3D12_RESOURCE_DESC rd = {0};
	rd = ID3D12Resource_GetDesc(i->res);
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
	UINT rows = 0;
	UINT64 rowsize = 0, total = 0;
	ID3D12Device_GetCopyableFootprints(d->dev, &rd, sub, 1, 0, &fp, &rows, &rowsize, &total);
	ID3D12Resource *up = make_buffer(d, total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
	if (!up) {
		free(tmp);
		return 0;
	}
	uint8_t *map = NULL;
	D3D12_RANGE none = {0, 0};
	ID3D12Resource_Map(up, 0, &none, (void **)&map);
	const uint8_t *src = data;
	for (unsigned z = 0; z < fp.Footprint.Depth; z++)
		for (unsigned r = 0; r < rows; r++)
			memcpy(map + (size_t)z * fp.Footprint.RowPitch * rows + (size_t)r * fp.Footprint.RowPitch, src + (size_t)z * slice_pitch + (size_t)r * row_pitch, (size_t)rowsize);
	ID3D12Resource_Unmap(up, 0, NULL);
	EnterCriticalSection(&d->ccs);
	ID3D12CommandAllocator_Reset(d->calloc_);
	ID3D12GraphicsCommandList_Reset(d->clist, d->calloc_, NULL);
	D3D12_TEXTURE_COPY_LOCATION dst = {0}, sl = {0};
	dst.pResource = i->res;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.SubresourceIndex = sub;
	sl.pResource = up;
	sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	sl.PlacedFootprint = fp;
	ID3D12GraphicsCommandList_CopyTextureRegion(d->clist, &dst, 0, 0, 0, &sl, NULL);
	ID3D12GraphicsCommandList_Close(d->clist);
	ID3D12CommandList *l[1] = {(ID3D12CommandList *)d->clist};
	ID3D12CommandQueue_ExecuteCommandLists(d->cqueue, 1, l);
	const UINT64 v = ++d->cvalue;
	ID3D12CommandQueue_Signal(d->cqueue, d->cfence, v);
	if (ID3D12Fence_GetCompletedValue(d->cfence) < v) {
		ID3D12Fence_SetEventOnCompletion(d->cfence, v, d->cevent);
		WaitForSingleObject(d->cevent, INFINITE);
	}
	LeaveCriticalSection(&d->ccs);
	ID3D12Resource_Release(up);
	free(tmp);
	return 1;
}

int nat_image_is_depth(const nat_image *i) { return i && (i->desc.flags & NAT_DEPTH) != 0; }
void *nat_image_srv(nat_image *i, int srgb) { (void)i; (void)srgb; return NULL; }
void *nat_image_rtv(nat_image *i, int srgb) { (void)i; (void)srgb; return NULL; }
void *nat_image_dsv(nat_image *i) { (void)i; return NULL; }
void *nat_image_resource(nat_image *i) { return i->res; }
void nat_image_size(const nat_image *i, unsigned *w, unsigned *h, unsigned *samples)
{
	*w = i->desc.w;
	*h = i->desc.h;
	*samples = i->samples;
}
unsigned nat_image_dxgi_format(const nat_image *i) { return (unsigned)i->f.dxgi; }
unsigned nat_image_levels(const nat_image *i) { return i->levels; }

D3D12_CPU_DESCRIPTOR_HANDLE nat12_image_srv(nat_image *i, int srgb)
{
	return dheap_cpu(&i->dev->srv, srgb && i->srv_srgb ? i->srv_srgb : i->srv);
}
D3D12_CPU_DESCRIPTOR_HANDLE nat12_image_rtv(nat_image *i, int srgb) { return dheap_cpu(&i->dev->rtv, srgb && i->rtv_srgb ? i->rtv_srgb : i->rtv); }
D3D12_CPU_DESCRIPTOR_HANDLE nat12_image_dsv(nat_image *i) { return dheap_cpu(&i->dev->dsv, i->dsv); }
int nat12_image_has_rtv(const nat_image *i) { return i && i->rtv != 0; }
int nat12_image_has_dsv(const nat_image *i) { return i && i->dsv != 0; }
int nat12_image_has_srv(const nat_image *i) { return i && i->srv != 0; }
DXGI_FORMAT nat12_image_rtv_format(const nat_image *i, int srgb) { return srgb && i->rtv_srgb ? i->f.srgb : i->f.srv; }
DXGI_FORMAT nat12_image_dsv_format(const nat_image *i) { return i->dsv_fmt; }
DXGI_FORMAT nat12_image_resource_format(const nat_image *i) { return i->f.dxgi; }
ID3D12Resource *nat12_image_res(nat_image *i) { return i->res; }
int nat12_image_kind(const nat_image *i) { return i->desc.kind; }

void nat12_image_transition(nat_device *d, nat_image *i, D3D12_RESOURCE_STATES state)
{
	if (!i || !i->tracked || i->state == state)
		return;
	D3D12_RESOURCE_BARRIER b = {0};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = i->res;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = i->state;
	b.Transition.StateAfter = state;
	ID3D12GraphicsCommandList_ResourceBarrier(d->list, 1, &b);
	d->cmds = 1;
	i->state = state;
}

D3D12_CPU_DESCRIPTOR_HANDLE nat12_null_srv(nat_device *d) { return dheap_cpu(&d->srv, d->null_srv); }

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ the scene targets
nat_image *nat12_scene_image(nat_device *d) { return d->scene; }
nat_image *nat12_scene_depth_image(nat_device *d) { return d->depth; }
nat_image *nat12_scene_resolved_image(nat_device *d) { return d->resolved; }
void *nat_scene_resource(nat_device *d) { return d->scene ? d->scene->res : NULL; }
unsigned nat_scene_samples(const nat_device *d) { return d->samples; }
void *nat_scene_rtv(nat_device *d) { (void)d; return NULL; }
void *nat_scene_rtv_srgb(nat_device *d) { (void)d; return NULL; }
void *nat_scene_dsv(nat_device *d) { (void)d; return NULL; }
void *nat_scene_srv(nat_device *d) { (void)d; return NULL; }

static void release_scene(nat_device *d)
{
	nat_image_destroy(d->scene);
	nat_image_destroy(d->depth);
	nat_image_destroy(d->resolved);
	d->scene = d->depth = d->resolved = NULL;
	for (UINT i = 0; i < NAT12_FRAMES; i++)
		REL(d->back[i]);
}

static int build_scene(nat_device *d, char *err, size_t err_size)
{
	d->nback = NAT12_FRAMES;
	for (UINT i = 0; i < NAT12_FRAMES; i++) {
		HRESULT hr = IDXGISwapChain3_GetBuffer(d->swap, i, &IID_ID3D12Resource, (void **)&d->back[i]);
		if (FAILED(hr)) {
			set_err(err, err_size, "swap chain buffer: %lx", (unsigned long)hr);
			return 0;
		}
	}
	nat_image_desc sd = {NAT_IMAGE_2D, d->p.width, d->p.height, 1, 1, 21, NAT_RENDER_TARGET, (int)d->samples};
	d->scene = nat_image_create(d, &sd);
	nat_image_desc dd = {NAT_IMAGE_2D, d->p.width, d->p.height, 1, 1, 75, NAT_DEPTH, (int)d->samples};
	d->depth = nat_image_create(d, &dd);
	if (d->samples > 1) {
		nat_image_desc rr = {NAT_IMAGE_2D, d->p.width, d->p.height, 1, 1, 21, NAT_RENDER_TARGET, 0};
		d->resolved = nat_image_create(d, &rr);
	}
	if (!d->scene || !d->depth || (d->samples > 1 && !d->resolved)) {
		set_err(err, err_size, "scene targets %lu", 0);
		return 0;
	}
	return 1;
}

static unsigned pick_samples(nat_device *d, int wanted)
{
	for (unsigned s = wanted >= 8 ? 8 : wanted >= 4 ? 4 : wanted >= 2 ? 2 : 1; s > 1; s >>= 1) {
		D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q = {DXGI_FORMAT_B8G8R8A8_UNORM, s, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
		if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(d->dev, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q))) && q.NumQualityLevels > 0)
			return s;
	}
	return 1;
}

static void fill_caps(nat_device *d)
{
	nat_caps *c = &d->caps;
	memset(c, 0, sizeof(*c));
	c->dynamic_textures = 1;
	c->aniso_min_mag = 1;
	c->max_aniso = 16;
	c->max_texture = 16384;
	c->max_units = 16;
	c->clip_planes = 6;
	c->slope_scale_bias = 1;
	c->mip_cube = 1;
	c->hw_shadow_depth = 1;
	c->msaa_max = (int)pick_samples(d, 8);
	c->desktop_w = (unsigned)GetSystemMetrics(SM_CXSCREEN);
	c->desktop_h = (unsigned)GetSystemMetrics(SM_CYSCREEN);
	IDXGIAdapter1 *ad = NULL;
	if (SUCCEEDED(IDXGIFactory4_EnumAdapters1(d->factory, 0, &ad)) && ad) {
		DXGI_ADAPTER_DESC1 desc;
		if (SUCCEEDED(IDXGIAdapter1_GetDesc1(ad, &desc))) {
			WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, c->adapter, sizeof(c->adapter), NULL, NULL);
			c->vram_mb = (unsigned)(desc.DedicatedVideoMemory >> 20);
		}
		IDXGIOutput *out = NULL;
		if (SUCCEEDED(IDXGIAdapter1_EnumOutputs(ad, 0, &out)) && out) {
			DXGI_OUTPUT_DESC od;
			if (SUCCEEDED(IDXGIOutput_GetDesc(out, &od))) {
				c->desktop_w = (unsigned)(od.DesktopCoordinates.right - od.DesktopCoordinates.left);
				c->desktop_h = (unsigned)(od.DesktopCoordinates.bottom - od.DesktopCoordinates.top);
			}
			IDXGIOutput_Release(out);
		}
		IDXGIAdapter1_Release(ad);
	}
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ the device
static int create_swap(nat_device *d, char *err, size_t err_size)
{
	DXGI_SWAP_CHAIN_DESC1 sc = {0};
	sc.Width = d->p.width;
	sc.Height = d->p.height;
	sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sc.SampleDesc.Count = 1;
	sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sc.BufferCount = NAT12_FRAMES;
	sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	IDXGISwapChain1 *s1 = NULL;
	HRESULT hr = IDXGIFactory4_CreateSwapChainForHwnd(d->factory, (IUnknown *)d->queue, d->hwnd, &sc, NULL, NULL, &s1);
	if (FAILED(hr)) {
		set_err(err, err_size, "CreateSwapChainForHwnd: %lx", (unsigned long)hr);
		return 0;
	}
	hr = IDXGISwapChain1_QueryInterface(s1, &IID_IDXGISwapChain3, (void **)&d->swap);
	IDXGISwapChain1_Release(s1);
	if (FAILED(hr)) {
		set_err(err, err_size, "IDXGISwapChain3: %lx", (unsigned long)hr);
		return 0;
	}
	IDXGIFactory4_MakeWindowAssociation(d->factory, d->hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
	return 1;
}

int nat_create(const nat_params *p, nat_device **out, char *err, size_t err_size)
{
	*out = NULL;
	nat_device *d = calloc(1, sizeof(*d));
	d->p = *p;
	d->hwnd = (HWND)p->hwnd;
	InitializeCriticalSection(&d->ccs);
	InitializeCriticalSection(&d->lock);
	if (!d->p.width || !d->p.height) {
		set_err(err, err_size, "empty size %lu", 0);
		free(d);
		return 0;
	}
	if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&d->factory))) {
		set_err(err, err_size, "DXGI factory %lu", 0);
		free(d);
		return 0;
	}
	HRESULT hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&d->dev);
	if (FAILED(hr)) {
		set_err(err, err_size, "D3D12CreateDevice: %lx", (unsigned long)hr);
		nat_destroy(d);
		return 0;
	}
	D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
	if (FAILED(ID3D12Device_CreateCommandQueue(d->dev, &qd, &IID_ID3D12CommandQueue, (void **)&d->queue)) || FAILED(ID3D12Device_CreateFence(d->dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&d->fence))) {
		set_err(err, err_size, "queue / fence %lu", 0);
		nat_destroy(d);
		return 0;
	}
	d->fevent = CreateEventA(NULL, FALSE, FALSE, NULL);
	d->pending = 1;
	for (int i = 0; i < NAT12_FRAMES; i++)
		if (FAILED(ID3D12Device_CreateCommandAllocator(d->dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&d->alloc[i]))) {
			set_err(err, err_size, "command allocator %lu", 0);
			nat_destroy(d);
			return 0;
		}
	if (FAILED(ID3D12Device_CreateCommandList(d->dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, d->alloc[0], NULL, &IID_ID3D12GraphicsCommandList, (void **)&d->list))) {
		set_err(err, err_size, "command list %lu", 0);
		nat_destroy(d);
		return 0;
	}
	ID3D12GraphicsCommandList_Close(d->list); // (begin_list resets it with the frame's allocator)
	d->list_gen = 1;
	if (!copy_queue_init(d) || !dheap_init(d->dev, &d->rtv, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1024, 0) || !dheap_init(d->dev, &d->dsv, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 256, 0) ||
		!dheap_init(d->dev, &d->srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 65536, 0) || !dheap_init(d->dev, &d->smp, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048, 0) ||
		!dheap_init(d->dev, &d->gsrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1000000 / NAT12_FRAMES * NAT12_FRAMES, 1) || !dheap_init(d->dev, &d->gsmp, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048, 1)) {
		set_err(err, err_size, "descriptor heaps %lu", 0);
		nat_destroy(d);
		return 0;
	}
	d->gsrv_seg_size = d->gsrv.cap / NAT12_FRAMES;
	// the constants ring: 96 MB in three frame segments
	d->ring_size = 96ull << 20;
	d->ring = make_buffer(d, d->ring_size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
	if (!d->ring) {
		set_err(err, err_size, "constants ring %lu", 0);
		nat_destroy(d);
		return 0;
	}
	{
		D3D12_RANGE none = {0, 0};
		ID3D12Resource_Map(d->ring, 0, &none, (void **)&d->ring_map);
	}
	// occlusion queries: a heap of 4096 and a readback buffer
	{
		D3D12_QUERY_HEAP_DESC qh = {D3D12_QUERY_HEAP_TYPE_OCCLUSION, 4096, 0};
		ID3D12Device_CreateQueryHeap(d->dev, &qh, &IID_ID3D12QueryHeap, (void **)&d->qheap);
		d->qread = make_buffer(d, 4096 * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
		if (d->qread) {
			D3D12_RANGE all = {0, 4096 * 8};
			ID3D12Resource_Map(d->qread, 0, &all, (void **)&d->qread_map);
		}
	}
	// a null shader resource view for the slots no image is bound to
	d->null_srv = dheap_alloc(&d->srv);
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
		sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		sd.Texture2D.MipLevels = 1;
		ID3D12Device_CreateShaderResourceView(d->dev, NULL, &sd, dheap_cpu(&d->srv, d->null_srv));
	}
	d->samples = pick_samples(d, p->msaa);
	if (!create_swap(d, err, err_size) || !build_scene(d, err, err_size)) {
		nat_destroy(d);
		return 0;
	}
	fill_caps(d);
	begin_list(d);
	if (nat_log_hook)
		nat_log_hook("native: Direct3D 12 device (%u samples, %u back buffers)", d->samples, (unsigned)NAT12_FRAMES);
	if (d->p.fullscreen) {
		DXGI_MODE_DESC m = {0};
		m.Width = d->p.width;
		m.Height = d->p.height;
		m.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		m.RefreshRate.Numerator = d->p.refresh;
		m.RefreshRate.Denominator = d->p.refresh ? 1 : 0;
		IDXGISwapChain3_ResizeTarget(d->swap, &m);
		if (SUCCEEDED(IDXGISwapChain3_SetFullscreenState(d->swap, TRUE, NULL)))
			d->fullscreen_set = 1;
	}
	*out = d;
	return 1;
}

void nat_destroy(nat_device *d)
{
	if (!d)
		return;
	if (d->swap && d->fullscreen_set)
		IDXGISwapChain3_SetFullscreenState(d->swap, FALSE, NULL);
	if (d->queue && d->list && d->fence) {
		ID3D12GraphicsCommandList_Close(d->list);
		ID3D12CommandQueue_Signal(d->queue, d->fence, d->pending);
		wait_value(d, d->pending);
	}
	release_scene(d);
	gc_collect(d, 1);
	REL(d->swap);
	REL(d->factory);
	REL(d->list);
	for (int i = 0; i < NAT12_FRAMES; i++)
		REL(d->alloc[i]);
	REL(d->clist);
	REL(d->calloc_);
	REL(d->queue);
	REL(d->cqueue);
	REL(d->fence);
	REL(d->cfence);
	REL(d->ring);
	REL(d->qheap);
	REL(d->qread);
	REL(d->dev);
	free(d);
}

const nat_caps *nat_get_caps(const nat_device *d) { return &d->caps; }
const char *nat_backend_name(void) { return "Direct3D 12"; }

int nat_reset(nat_device *d, const nat_params *p, char *err, size_t err_size)
{
	nat12_flush(d);
	release_scene(d);
	gc_collect(d, 1);
	d->p = *p;
	d->samples = pick_samples(d, p->msaa);
	if (d->fullscreen_set && !p->fullscreen) {
		IDXGISwapChain3_SetFullscreenState(d->swap, FALSE, NULL);
		d->fullscreen_set = 0;
	}
	DXGI_MODE_DESC m = {0};
	m.Width = p->width;
	m.Height = p->height;
	m.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	IDXGISwapChain3_ResizeTarget(d->swap, &m);
	if (p->fullscreen && !d->fullscreen_set && SUCCEEDED(IDXGISwapChain3_SetFullscreenState(d->swap, TRUE, NULL)))
		d->fullscreen_set = 1;
	HRESULT hr = IDXGISwapChain3_ResizeBuffers(d->swap, NAT12_FRAMES, p->width, p->height, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
	if (FAILED(hr)) {
		set_err(err, err_size, "ResizeBuffers: %lx", (unsigned long)hr);
		return 0;
	}
	return build_scene(d, err, err_size);
}

void nat_get_size(const nat_device *d, unsigned *w, unsigned *h)
{
	*w = d->p.width;
	*h = d->p.height;
}
void *nat_d3d_device(nat_device *d) { return d->dev; }
void *nat_d3d_context(nat_device *d) { return d->list; }

void nat_scene_resolve(nat_device *d)
{
	if (d->samples <= 1 || !d->resolved)
		return;
	nat12_image_transition(d, d->scene, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
	nat12_image_transition(d, d->resolved, D3D12_RESOURCE_STATE_RESOLVE_DEST);
	ID3D12GraphicsCommandList_ResolveSubresource(d->list, d->resolved->res, 0, d->scene->res, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
	nat12_image_transition(d, d->resolved, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

static void barrier_res(nat_device *d, ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
	D3D12_RESOURCE_BARRIER rb = {0};
	rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	rb.Transition.pResource = r;
	rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	rb.Transition.StateBefore = a;
	rb.Transition.StateAfter = b;
	ID3D12GraphicsCommandList_ResourceBarrier(d->list, 1, &rb);
}

int nat_present(nat_device *d)
{
	const UINT bi = IDXGISwapChain3_GetCurrentBackBufferIndex(d->swap);
	ID3D12Resource *back = d->back[bi];
	if (d->samples > 1) {
		nat12_image_transition(d, d->scene, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
		barrier_res(d, back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RESOLVE_DEST);
		ID3D12GraphicsCommandList_ResolveSubresource(d->list, back, 0, d->scene->res, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
		barrier_res(d, back, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_PRESENT);
	} else {
		nat12_image_transition(d, d->scene, D3D12_RESOURCE_STATE_COPY_SOURCE);
		barrier_res(d, back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
		ID3D12GraphicsCommandList_CopyResource(d->list, back, d->scene->res);
		barrier_res(d, back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
	}
	submit(d, 0);
	HRESULT hr = IDXGISwapChain3_Present(d->swap, d->p.vsync ? 1 : 0, 0);
	d->fidx = (d->fidx + 1) % NAT12_FRAMES;
	wait_value(d, d->alloc_value[d->fidx]);
	gc_collect(d, 0);
	begin_list(d);
	if (hr == DXGI_STATUS_OCCLUDED)
		return 1;
	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
		return -1;
	return FAILED(hr) ? -1 : 0;
}

int nat_set_gamma(nat_device *d, const uint16_t ramp[3][256])
{
	(void)d;
	(void)ramp; // the flip model swap chain has no gamma control
	return 0;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ queries
struct nat_query {
	void **vt;
	nat_device *dev;
	int occlusion;
	UINT slot; // occlusion: the index in the query heap (+ 1)
	UINT64 value; // the fence value that proves the commands up to its end are done (0 = never ended)
};

static void *query_vt[16];
static long query_nop(void *self)
{
	(void)self;
	return 0;
}
static long query_release(void *self)
{
	nat_query_destroy((nat_query *)self);
	return 0;
}
void nat_query_begin(nat_query *q);
static long query_issue(void *self, unsigned flags)
{
	nat_query *q = (nat_query *)self;
	if (flags & 2) {
		if (q->occlusion)
			nat_query_begin(q);
	} else {
		nat_query_end(q);
	}
	return 0;
}
static long query_getdata(void *self, void *data, unsigned size, unsigned flags)
{
	(void)flags;
	nat_query *q = (nat_query *)self;
	unsigned v = 0;
	const int r = nat_query_get(q, &v);
	if (r == 1)
		return 1;
	if (r < 0)
		return (long)0x88760868;
	if (data && size >= 4)
		*(unsigned *)data = v;
	return 0;
}

nat_query *nat_query_create(nat_device *d, int occlusion)
{
	nat_query *q = calloc(1, sizeof(*q));
	if (!query_vt[0]) {
		for (int i = 0; i < 16; i++)
			query_vt[i] = (void *)query_nop;
		query_vt[2] = (void *)query_release;
		query_vt[6] = (void *)query_issue;
		query_vt[7] = (void *)query_getdata;
	}
	q->vt = query_vt;
	q->dev = d;
	q->occlusion = occlusion;
	if (occlusion) {
		EnterCriticalSection(&d->lock);
		q->slot = d->qnext < 4096 ? ++d->qnext : 0;
		LeaveCriticalSection(&d->lock);
	}
	return q;
}

void nat_query_destroy(nat_query *q) { free(q); }

void nat_query_begin(nat_query *q)
{
	if (q->occlusion && q->slot)
		ID3D12GraphicsCommandList_BeginQuery(q->dev->list, q->dev->qheap, D3D12_QUERY_TYPE_OCCLUSION, q->slot - 1);
}

void nat_query_end(nat_query *q)
{
	nat_device *d = q->dev;
	if (q->occlusion && q->slot) {
		ID3D12GraphicsCommandList_EndQuery(d->list, d->qheap, D3D12_QUERY_TYPE_OCCLUSION, q->slot - 1);
		if (d->qended_n == d->qended_cap) {
			d->qended_cap = d->qended_cap ? d->qended_cap * 2 : 64;
			d->qended = realloc(d->qended, d->qended_cap * sizeof(*d->qended));
		}
		d->qended[d->qended_n++] = q;
	}
	// an event: everything before it is done when the submission of the list the commands are in is; with nothing recorded since the last submission that is the last one
	q->value = (d->cmds || q->occlusion) ? d->pending : d->pending - 1;
}

void nat12_query_resolve(nat_device *d, nat_query *q)
{
	if (q->occlusion && q->slot)
		ID3D12GraphicsCommandList_ResolveQueryData(d->list, d->qheap, D3D12_QUERY_TYPE_OCCLUSION, q->slot - 1, 1, d->qread, (UINT64)(q->slot - 1) * 8);
}

int nat_query_get(nat_query *q, unsigned *value)
{
	nat_device *d = q->dev;
	if (!q->value) {
		if (value)
			*value = 0;
		return 0;
	}
	if (q->value >= d->pending && d->cmds)
		nat12_flush(d); // the query belongs to commands not submitted yet: nothing would ever finish it
	if (ID3D12Fence_GetCompletedValue(d->fence) < q->value)
		return 1;
	if (value) {
		unsigned v = 1;
		if (q->occlusion && q->slot && d->qread_map) {
			const UINT64 s = *(const UINT64 *)(d->qread_map + (size_t)(q->slot - 1) * 8);
			v = s > 0xffffffffu ? 0xffffffffu : (unsigned)s;
		}
		*value = v;
	}
	return 0;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ buffers
typedef struct {
	ID3D12Resource *res;
	uint8_t *map;
	UINT64 value; // the last submission that read this version
	UINT64 off;   // the offset in res (a piece of a chunk), 0 for an own resource
	chunk_t *chunk;
} bver_t;

#define CHUNK_SIZE (32ull << 20)

static int sub_alloc(nat_device *d, UINT64 size, bver_t *v)
{
	size = (size + 255) & ~255ull;
	if (size > CHUNK_SIZE / 4)
		return 0;
	EnterCriticalSection(&d->lock);
	chunk_t *c = d->cur_chunk;
	if (!c || c->used + size > c->size) {
		if (c)
			c->full = 1;
		chunk_t *n = calloc(1, sizeof(*n));
		n->size = CHUNK_SIZE;
		n->res = make_buffer(d, CHUNK_SIZE, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
		if (!n->res) {
			free(n);
			if (c && c->live == 0) {
				gc_add(d, c->res);
				free(c);
			}
			d->cur_chunk = NULL;
			LeaveCriticalSection(&d->lock);
			return 0;
		}
		D3D12_RANGE none = {0, 0};
		ID3D12Resource_Map(n->res, 0, &none, (void **)&n->map);
		if (c && c->live == 0) {
			gc_add(d, c->res);
			free(c);
		}
		d->cur_chunk = c = n;
	}
	v->res = c->res;
	v->map = c->map + c->used;
	v->off = c->used;
	v->chunk = c;
	c->used += size;
	c->live++;
	LeaveCriticalSection(&d->lock);
	return 1;
}

static void version_free(nat_device *d, bver_t *v)
{
	if (!v->res)
		return;
	if (!v->chunk) {
		gc_add(d, v->res);
		return;
	}
	EnterCriticalSection(&d->lock);
	chunk_t *c = v->chunk;
	c->live--;
	if (c->live == 0 && c->full) {
		gc_add(d, c->res);
		free(c);
	}
	LeaveCriticalSection(&d->lock);
}

struct nat_buffer {
	void **vt;
	nat_device *dev;
	bver_t ver[8];
	int nver, cur;
	int dynamic;
	int is_index;
	unsigned size;
	uint8_t *cpu;
	uint8_t *stage;
	unsigned synced;
	const volatile int *used;
};

static long buffer_nop(void *self)
{
	(void)self;
	return 0;
}
static long buffer_addref(void *self)
{
	(void)self;
	return 1;
}
static long buffer_release(void *self);
static long buffer_lock(void *self, unsigned off, unsigned size, void **out, unsigned flags);
static long buffer_unlock(void *self);
static void *buffer_vt[32];
static void buffer_vt_init(void)
{
	if (buffer_vt[0])
		return;
	for (int i = 0; i < 32; i++)
		buffer_vt[i] = (void *)buffer_nop;
	buffer_vt[1] = (void *)buffer_addref;
	buffer_vt[2] = (void *)buffer_release;
	buffer_vt[11] = (void *)buffer_lock;
	buffer_vt[12] = (void *)buffer_unlock;
}

void (*nat_log_hook)(const char *fmt, ...);

static int add_version(nat_buffer *b, int piece)
{
	if (b->nver >= 8)
		return -1;
	bver_t *v = &b->ver[b->nver];
	memset(v, 0, sizeof(*v));
	if (piece && sub_alloc(b->dev, b->size, v))
		return b->nver++;
	v->res = make_buffer(b->dev, b->size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
	if (!v->res)
		return -1;
	D3D12_RANGE none = {0, 0};
	ID3D12Resource_Map(v->res, 0, &none, (void **)&v->map);
	return b->nver++;
}

// a fresh memory for a DISCARD: a version the GPU is done with, or a new one
static uint8_t *next_version(nat_buffer *b)
{
	nat_device *d = b->dev;
	const UINT64 done = ID3D12Fence_GetCompletedValue(d->fence);
	for (int k = 1; k <= b->nver; k++) {
		const int i = (b->cur + k) % b->nver;
		if (b->ver[i].value <= done) {
			b->cur = i;
			return b->ver[i].map;
		}
	}
	const int i = add_version(b, 0);
	if (i >= 0) {
		b->cur = i;
		return b->ver[i].map;
	}
	// all eight versions are in flight: finish the frame's commands and reuse the oldest
	nat12_flush(d);
	b->cur = (b->cur + 1) % b->nver;
	return b->ver[b->cur].map;
}

nat_buffer *nat_buffer_create(nat_device *d, unsigned size, int is_index, int dynamic, const void *initial)
{
	nat_buffer *b = calloc(1, sizeof(*b));
	buffer_vt_init();
	b->vt = buffer_vt;
	b->dev = d;
	b->dynamic = dynamic;
	b->is_index = is_index;
	b->size = size;
	if (add_version(b, !dynamic) < 0) {
		free(b);
		return NULL;
	}
	if (initial)
		memcpy(b->ver[0].map, initial, size);
	return b;
}

void nat_buffer_destroy(nat_buffer *b)
{
	if (!b)
		return;
	for (int i = 0; i < b->nver; i++)
		version_free(b->dev, &b->ver[i]);
	free(b->cpu);
	free(b->stage);
	free(b);
}

nat_buffer *nat_buffer_create_static(nat_device *d, unsigned size, int is_index, void **block)
{
	nat_buffer *b = calloc(1, sizeof(*b));
	buffer_vt_init();
	b->vt = buffer_vt;
	b->dev = d;
	b->is_index = is_index;
	b->size = size;
	b->cpu = calloc(1, size ? size : 1);
	if (!b->cpu) {
		free(b);
		return NULL;
	}
	*block = b->cpu;
	return b;
}

nat_buffer *nat_buffer_create_staged(nat_device *d, unsigned size, const volatile int *used)
{
	nat_buffer *b = nat_buffer_create(d, size, 0, 1, NULL); // (own upload resources, rotated by Lock: the ring is big and rewritten every frame)
	if (!b)
		return NULL;
	b->dynamic = 0;
	b->stage = calloc(1, size);
	if (!b->stage) {
		nat_buffer_destroy(b);
		return NULL;
	}
	b->used = used;
	return b;
}

int nat_buffer_is_staged(const nat_buffer *b) { return b && b->stage != NULL; }

void nat_buffer_sync(nat_buffer *b)
{
	if (!b || !b->stage || !b->used)
		return;
	int u = *b->used;
	if (u < 0)
		return;
	if ((unsigned)u > b->size)
		u = (int)b->size;
	if ((unsigned)u <= b->synced)
		return;
	memcpy(b->ver[b->cur].map + b->synced, b->stage + b->synced, (size_t)u - b->synced);
	b->synced = (unsigned)u;
}

void *nat_buffer_map(nat_buffer *b, int mode)
{
	if (!b->dynamic || b->nver < 1)
		return NULL;
	if (mode == NAT_MAP_DISCARD)
		return next_version(b);
	return b->ver[b->cur].map;
}
void nat_buffer_unmap(nat_buffer *b) { (void)b; }
void *nat_buffer_resource(nat_buffer *b) { return b->ver[b->cur].res; }

D3D12_GPU_VIRTUAL_ADDRESS nat12_buffer_address(nat_buffer *b)
{
	if (!b || b->nver < 1)
		return 0;
	return ID3D12Resource_GetGPUVirtualAddress(b->ver[b->cur].res) + b->ver[b->cur].off;
}
unsigned nat12_buffer_size(const nat_buffer *b) { return b ? b->size : 0; }
void nat12_buffer_used(nat_device *d, nat_buffer *b)
{
	if (b && b->nver > 0)
		b->ver[b->cur].value = d->pending;
}

static long buffer_release(void *self)
{
	nat_buffer_destroy((nat_buffer *)self);
	return 0;
}

static long buffer_lock(void *self, unsigned off, unsigned size, void **out, unsigned flags)
{
	nat_buffer *b = (nat_buffer *)self;
	(void)size;
	(void)flags;
	if (off > b->size)
		return -1;
	if (b->stage) { // a staged ring: a new frame (Lock with DISCARD) is a fresh version and the whole stage is free again
		next_version(b);
		b->synced = 0;
		*out = b->stage + off;
		return 0;
	}
	if (b->dynamic) {
		uint8_t *p = (flags & 0x1000) ? b->ver[b->cur].map : next_version(b); // D3DLOCK_NOOVERWRITE = 0x1000
		if (!p)
			return -1;
		*out = p + off;
		return 0;
	}
	if (!b->cpu)
		b->cpu = calloc(1, b->size ? b->size : 1);
	if (!b->cpu)
		return -1;
	*out = b->cpu + off;
	return 0;
}

static long buffer_unlock(void *self)
{
	nat_buffer *b = (nat_buffer *)self;
	if (b->dynamic || b->stage || !b->cpu)
		return 0;
	if (add_version(b, 1) >= 0) {
		b->cur = b->nver - 1;
		memcpy(b->ver[b->cur].map, b->cpu, b->size);
	}
	free(b->cpu);
	b->cpu = NULL;
	return 0;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ shaders
struct nat_shader {
	IUnknown *blob;
	int is_vs;
	unsigned nconst;
};

nat_shader *nat_shader_from_hlsl(nat_device *d, const char *hlsl, int is_vs, char *err, size_t err_size)
{
	(void)d;
	if (!load_compiler()) {
		set_err(err, err_size, "d3dcompiler_47.dll %lu", 0);
		return NULL;
	}
	IUnknown *code = NULL, *msgs = NULL;
	HRESULT hr = p_compile(hlsl, strlen(hlsl), "nat", NULL, NULL, "main", is_vs ? "vs_5_0" : "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS | (1 << 14), 0, &code, &msgs);
	if (FAILED(hr) || !code) {
		if (err && err_size)
			snprintf(err, err_size, "compile: %lx %s", (unsigned long)hr, msgs ? (const char *)blob_ptr(msgs) : "");
		if (msgs)
			IUnknown_Release(msgs);
		return NULL;
	}
	if (msgs)
		IUnknown_Release(msgs);
	nat_shader *s = calloc(1, sizeof(*s));
	s->is_vs = is_vs;
	s->blob = code;
	return s;
}

nat_shader *nat_shader_from_sm3(nat_device *d, const uint32_t *code, size_t dwords, int alpha_test, const void *int_inputs, int nint_inputs, char *err, size_t err_size)
{
	char *hlsl = NULL, why[160] = "";
	int is_vs = 0;
	// (alpha_test: bit 0 = the alpha test, bits 8..23 = SM3_SHADOW_MASK; the pixel shaders' constant buffers move to b3.. for the root signature)
	if (!sm3_translate_vs(code, dwords, (alpha_test & 1 ? SM3_ALPHA_TEST : 0) | (alpha_test & 0xffff00) | SM3_D3D12, (const sm3_int_input *)int_inputs, nint_inputs, &hlsl, &is_vs, why, sizeof(why))) {
		if (err && err_size)
			snprintf(err, err_size, "translate: %s", why);
		return NULL;
	}
	const unsigned nconst = (unsigned)sm3_last_const_count();
	nat_shader *s = nat_shader_from_hlsl(d, hlsl, is_vs, err, err_size);
	free(hlsl);
	if (s)
		s->nconst = nconst;
	return s;
}

void nat_shader_destroy(nat_shader *s)
{
	if (!s)
		return;
	if (s->blob)
		IUnknown_Release(s->blob);
	free(s);
}
void *nat_shader_object(nat_shader *s) { (void)s; return NULL; }
void *nat_shader_blob(nat_shader *s, size_t *size)
{
	if (size)
		*size = blob_size(s->blob);
	return blob_ptr(s->blob);
}
void *nat12_shader_blob(nat_shader *s, size_t *size) { return nat_shader_blob(s, size); }
int nat_shader_is_vs(const nat_shader *s) { return s->is_vs; }
unsigned nat12_shader_nconst(const nat_shader *s) { return s->nconst ? s->nconst : 256; }

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ outputs, modes, screenshots
int nat_list_modes(int output, nat_mode *out, int max)
{
	IDXGIFactory *fac = NULL;
	IDXGIAdapter *ad = NULL;
	IDXGIOutput *o = NULL;
	int n = 0;
	if (FAILED(CreateDXGIFactory(&IID_IDXGIFactory, (void **)&fac)) || FAILED(IDXGIFactory_EnumAdapters(fac, 0, &ad)) || FAILED(IDXGIAdapter_EnumOutputs(ad, (UINT)output, &o)))
		goto done;
	{
		UINT count = 0;
		IDXGIOutput_GetDisplayModeList(o, DXGI_FORMAT_B8G8R8A8_UNORM, 0, &count, NULL);
		DXGI_MODE_DESC *m = calloc(count ? count : 1, sizeof(*m));
		if (m && SUCCEEDED(IDXGIOutput_GetDisplayModeList(o, DXGI_FORMAT_B8G8R8A8_UNORM, 0, &count, m))) {
			for (UINT i = 0; i < count && n < max; i++) {
				out[n].w = m[i].Width;
				out[n].h = m[i].Height;
				out[n].hz = m[i].RefreshRate.Denominator ? (m[i].RefreshRate.Numerator + m[i].RefreshRate.Denominator / 2) / m[i].RefreshRate.Denominator : 0;
				n++;
			}
		}
		free(m);
	}
done:
	if (o)
		IDXGIOutput_Release(o);
	if (ad)
		IDXGIAdapter_Release(ad);
	if (fac)
		IDXGIFactory_Release(fac);
	return n;
}

void *nat_output_monitor(int output)
{
	IDXGIFactory *fac = NULL;
	IDXGIAdapter *ad = NULL;
	IDXGIOutput *o = NULL;
	void *mon = NULL;
	if (SUCCEEDED(CreateDXGIFactory(&IID_IDXGIFactory, (void **)&fac)) && SUCCEEDED(IDXGIFactory_EnumAdapters(fac, 0, &ad)) && SUCCEEDED(IDXGIAdapter_EnumOutputs(ad, (UINT)output, &o))) {
		DXGI_OUTPUT_DESC od;
		if (SUCCEEDED(IDXGIOutput_GetDesc(o, &od)))
			mon = od.Monitor;
	}
	if (o)
		IDXGIOutput_Release(o);
	if (ad)
		IDXGIAdapter_Release(ad);
	if (fac)
		IDXGIFactory_Release(fac);
	return mon;
}

int nat_output_count(void)
{
	IDXGIFactory *fac = NULL;
	IDXGIAdapter *ad = NULL;
	int n = 0;
	if (SUCCEEDED(CreateDXGIFactory(&IID_IDXGIFactory, (void **)&fac)) && SUCCEEDED(IDXGIFactory_EnumAdapters(fac, 0, &ad))) {
		IDXGIOutput *o = NULL;
		while (SUCCEEDED(IDXGIAdapter_EnumOutputs(ad, (UINT)n, &o))) {
			IDXGIOutput_Release(o);
			n++;
		}
	}
	if (ad)
		IDXGIAdapter_Release(ad);
	if (fac)
		IDXGIFactory_Release(fac);
	return n;
}

int nat_read_pixel(nat_device *d, nat_image *im, unsigned x, unsigned y, uint32_t *out)
{
	nat_image *src = im ? im : d->scene;
	if (!im && d->samples > 1 && d->resolved) {
		nat_scene_resolve(d);
		src = d->resolved;
	}
	if (x >= src->desc.w || y >= src->desc.h)
		return 0;
	nat12_image_transition(d, src, D3D12_RESOURCE_STATE_COPY_SOURCE);
	D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(src->res);
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
	UINT rows;
	UINT64 rowsize, total;
	ID3D12Device_GetCopyableFootprints(d->dev, &rd, 0, 1, 0, &fp, &rows, &rowsize, &total);
	ID3D12Resource *rb = make_buffer(d, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
	if (!rb)
		return 0;
	D3D12_TEXTURE_COPY_LOCATION dst = {0}, sl = {0};
	dst.pResource = rb;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint = fp;
	sl.pResource = src->res;
	sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &dst, 0, 0, 0, &sl, NULL);
	nat12_flush(d);
	int ok = 0;
	uint8_t *map = NULL;
	D3D12_RANGE all = {0, (SIZE_T)total};
	if (SUCCEEDED(ID3D12Resource_Map(rb, 0, &all, (void **)&map))) {
		*out = ((uint32_t *)(map + (size_t)y * fp.Footprint.RowPitch))[x];
		D3D12_RANGE none = {0, 0};
		ID3D12Resource_Unmap(rb, 0, &none);
		ok = 1;
	}
	ID3D12Resource_Release(rb);
	return ok;
}

int nat_save_scene_bmp(nat_device *d, const char *path)
{
	const unsigned w = d->p.width, h = d->p.height;
	nat_image *src = d->scene;
	if (d->samples > 1 && d->resolved) {
		nat_scene_resolve(d);
		src = d->resolved;
	}
	nat12_image_transition(d, src, D3D12_RESOURCE_STATE_COPY_SOURCE);
	D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(src->res);
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
	UINT rows;
	UINT64 rowsize, total;
	ID3D12Device_GetCopyableFootprints(d->dev, &rd, 0, 1, 0, &fp, &rows, &rowsize, &total);
	ID3D12Resource *rb = make_buffer(d, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
	if (!rb)
		return 0;
	D3D12_TEXTURE_COPY_LOCATION dst = {0}, sl = {0};
	dst.pResource = rb;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint = fp;
	sl.pResource = src->res;
	sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	sl.SubresourceIndex = 0;
	ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &dst, 0, 0, 0, &sl, NULL);
	nat12_flush(d);
	int ok = 0;
	uint8_t *map = NULL;
	D3D12_RANGE all = {0, (SIZE_T)total};
	if (SUCCEEDED(ID3D12Resource_Map(rb, 0, &all, (void **)&map))) {
		FILE *f = fopen(path, "wb");
		if (f) {
			uint8_t hdr[54] = {'B', 'M'};
			const uint32_t size = 54 + w * h * 4, off = 54, dib = 40, planes_bpp = 1 | 32u << 16;
			memcpy(hdr + 2, &size, 4);
			memcpy(hdr + 10, &off, 4);
			memcpy(hdr + 14, &dib, 4);
			memcpy(hdr + 18, &w, 4);
			const int32_t nh = -(int32_t)h;
			memcpy(hdr + 22, &nh, 4);
			memcpy(hdr + 26, &planes_bpp, 4);
			fwrite(hdr, 1, 54, f);
			for (unsigned y = 0; y < h; y++)
				fwrite(map + (size_t)y * fp.Footprint.RowPitch, 4, w, f);
			fclose(f);
			ok = 1;
		}
		D3D12_RANGE none = {0, 0};
		ID3D12Resource_Unmap(rb, 0, &none);
	}
	ID3D12Resource_Release(rb);
	return ok;
}
#endif
