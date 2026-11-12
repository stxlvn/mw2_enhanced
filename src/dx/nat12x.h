#pragma once
// Private interface between nat12.c (the Direct3D 12 device layer) and natdraw12.c (its flush half): descriptors, resource states, the command list and the upload rings. Direct3D 12 headers needed.
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "nat11.h"

#define NAT12_FRAMES 3

// the device's command list of the frame being recorded (render thread only) and what a draw needs from the device
ID3D12Device *nat12_device(nat_device *d);
ID3D12GraphicsCommandList *nat12_list(nat_device *d);
// bumps when the list was reset (a new frame or a flush): everything bound in a list must be bound again
unsigned nat12_list_generation(const nat_device *d);
// the fence value the next submission will signal (a resource used by commands recorded now is free when the fence has reached it)
unsigned long long nat12_pending_value(const nat_device *d);
unsigned long long nat12_completed_value(nat_device *d);
// submits what is recorded and waits until the GPU is done with it (readbacks, a full upload ring, a resize); the list is open again afterwards
void nat12_flush(nat_device *d);
// commands were recorded in the list (an event query issued before any is complete with the submissions already made)
void nat12_note_commands(nat_device *d);
// the frame's upload memory for constants and the like: 256-byte aligned, valid until the GPU is done with the frame; NULL when the frame's ring is full (nat12_flush and ask again)
void *nat12_upload_alloc(nat_device *d, unsigned size, D3D12_GPU_VIRTUAL_ADDRESS *gpu);
// shader-visible descriptor rings of the frame (count descriptors, contiguous): NULL when full. The heaps are set on the list by nat12_set_heaps.
int nat12_srv_alloc(nat_device *d, unsigned count, D3D12_CPU_DESCRIPTOR_HANDLE *cpu, D3D12_GPU_DESCRIPTOR_HANDLE *gpu);
void nat12_set_heaps(nat_device *d);

// images: the scene's own, CPU descriptors (copy them into the shader-visible ring), resource state tracking (only render targets and depth images are transitioned: other textures stay in COMMON
// and are promoted implicitly by the first read)
nat_image *nat12_scene_image(nat_device *d);
nat_image *nat12_scene_depth_image(nat_device *d);
nat_image *nat12_scene_resolved_image(nat_device *d); // single-sample copy of the scene for sampling (NULL when the scene is not multisampled: the scene itself)
D3D12_CPU_DESCRIPTOR_HANDLE nat12_image_srv(nat_image *i, int srgb);
D3D12_CPU_DESCRIPTOR_HANDLE nat12_image_rtv(nat_image *i, int srgb);
D3D12_CPU_DESCRIPTOR_HANDLE nat12_image_dsv(nat_image *i);
int nat12_image_has_rtv(const nat_image *i);
int nat12_image_has_dsv(const nat_image *i);
int nat12_image_has_srv(const nat_image *i);
DXGI_FORMAT nat12_image_rtv_format(const nat_image *i, int srgb);
DXGI_FORMAT nat12_image_dsv_format(const nat_image *i);
DXGI_FORMAT nat12_image_resource_format(const nat_image *i);
ID3D12Resource *nat12_image_res(nat_image *i);
int nat12_image_kind(const nat_image *i);
// moves the resource to `state` (a no-op for an image that is not tracked: plain textures)
void nat12_image_transition(nat_device *d, nat_image *i, D3D12_RESOURCE_STATES state);
D3D12_CPU_DESCRIPTOR_HANDLE nat12_null_srv(nat_device *d);

// buffers: the current GPU address of the version a draw reads (and the size left after `offset`)
D3D12_GPU_VIRTUAL_ADDRESS nat12_buffer_address(nat_buffer *b);
unsigned nat12_buffer_size(const nat_buffer *b);
void nat12_buffer_used(nat_device *d, nat_buffer *b); // the frame's commands read this buffer version
// shader blobs (DXBC) for the pipeline states
void *nat12_shader_blob(nat_shader *s, size_t *size);
unsigned nat12_shader_nconst(const nat_shader *s); // constant registers (float4) the shader reads: the size of the constant buffer it needs is 16 times this

// the CPU descriptor of a sampler made from a description (cached by the caller), and a shader-visible table of 16 of them
int nat12_sampler_create(nat_device *d, const D3D12_SAMPLER_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE *cpu);
int nat12_sampler_table(nat_device *d, const D3D12_CPU_DESCRIPTOR_HANDLE cpu[16], D3D12_GPU_DESCRIPTOR_HANDLE *gpu);

// the scene's swap: the present of the frame (called by nat_present only), the render target's width and height
unsigned nat12_scene_samples(const nat_device *d);
