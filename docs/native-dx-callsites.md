# Who calls Direct3D 9 (measured, 64-bit retail exe)

The work list for the real Direct3D 11 / 12 back end (docs/native-dx-plan.md, "The owner's rule"): every exe function that called a Direct3D 9 method in the plain game on Trainer, Roadkill and Airport (menu, loading, level),
found with `MW2COOP_D3DCALLERS=1` (`src/dx/d3dcallers.c`: every method of the device, the factory and the resource classes gets a stub that records the return address; the swap-chain class is not counted, hooking its vtable
crashes the game within a second, cause unknown) and resolved to functions with `tools/ghidra/FunctionOf.java`; the table is made by `tools/d3dcallers_report.py`. 2026-11-12, one run per level, ~100 s each.

**Observed:** 89 functions of the exe (200 distinct call sites) call 49 different Direct3D 9 methods. A function not reached in these runs (a level feature, a video, the video options, vid_restart, a shutdown path) is missing:
more levels and the options menu have to be run through the same probe before the list is final. The calls counted are the real ones in the run (millions of `SetStreamSource` in the draw loops, one-off
creations in the loaders).

Other modules call the same interface and are part of the problem too: the Steam overlay (`gameoverlayrenderer64.dll`, 39 sites, hooks the device; with a Direct3D 11 game it uses its own Direct3D 11 hook), `DiscordHook64.dll`
(6 sites), d3d9.dll itself (41 internal calls, not ours to replace) and this mod's `WINMM.dll` (33 sites: font atlas and texture locks, `UpdateTexture`, `GetDeviceCaps`, `GetSwapChain`, `GetViewport`, `GetTexture`, the
console / split screen draws). Those 33 are the mod's own users of the device that have to move to the new back end.

| function (rva) | Direct3D 9 methods called (calls counted in the runs) |
|---|---|
| 0x4710 | Query.GetData x11095 |
| 0x8f60 | Device.CreatePixelShader x4706 |
| 0x9070 | Device.CreateVertexShader x1491 |
| 0xc4c0 | Query.GetData x4686 |
| 0xf760 | Device.CreateQuery x36, Device.CreateVertexBuffer x1 |
| 0x10260 | D3D9.GetAdapterDisplayMode x1 |
| 0x10410 | Query.GetData x101619 |
| 0x10550 | Device.SetRenderState x3 |
| 0x11140 | Device.TestCooperativeLevel x2 |
| 0x12180 | Device.TestCooperativeLevel x26809 |
| 0x23920 | Texture.Release x83 |
| 0x23950 | Device.CreateTexture x1605 |
| 0x23a90 | Device.CreateVolumeTexture x3 |
| 0x23be0 | Device.CreateCubeTexture x6 |
| 0x242a0 | Device.GetAvailableTextureMem x2 |
| 0x24d00 | Device.CreateVertexBuffer x7 |
| 0x24df0 | Device.CreateIndexBuffer x3 |
| 0x24f00 | Device.CreateVertexBuffer x21 |
| 0x24fa0 | Device.CreateIndexBuffer x3, Device.CreateVertexBuffer x2 |
| 0x252e0 | Device.CreateIndexBuffer x2 |
| 0x258a0 | Device.CreateVertexDeclaration x220 |
| 0x266a0 | Device.SetTexture x177576 |
| 0x26730 | Device.SetStreamSource x16797433 |
| 0x267b0 | Device.SetPixelShader x2 |
| 0x267e0 | Device.SetVertexShader x2 |
| 0x26810 | Device.SetVertexDeclaration x2 |
| 0x26840 | Device.SetStreamSource x184374, Device.DrawIndexedPrimitive x1319 |
| 0x2a0a0 | Volume.AddDirtyBox x266330, Volume.LockBox x11096, Volume.UnlockBox x11096 |
| 0x2f800 | Query.GetData x784598 |
| 0x2f860 | Query.GetData x11095 |
| 0x39110 | Texture.GetSurfaceLevel x8 |
| 0x391d0 | Device.CreateDepthStencilSurface x1 |
| 0x39310 | Device.CreateRenderTarget x2 |
| 0x39420 | Texture.GetSurfaceLevel x1, Device.GetSwapChain x1, Device.GetBackBuffer x1, Device.CreateDepthStencilSurface x1 |
| 0x3cc00 | Device.SetScissorRect x22191, Device.SetViewport x22191 |
| 0x3ec20 | Device.SetIndices x99859 |
| 0x40750 | Device.SetScissorRect x400638 |
| 0x41160 | Device.StretchRect x806, Texture.GetSurfaceLevel x806 |
| 0x44d00 | Device.BeginScene x12784 |
| 0x45010 | Query.GetData x21199 |
| 0x45060 | Query.Issue x12783, Device.SetGammaRamp x1 |
| 0x453c0 | Device.SetIndices x12782, Device.EndScene x88 |
| 0x484c0 | Texture.GetSurfaceLevel x11096, Device.StretchRect x11096 |
| 0x48fa0 | Volume.UnlockBox x1, Volume.LockBox x1 |
| 0x49170 | Texture.LockRect x12430, Texture.UnlockRect x12430, Cube.LockRect x180, Cube.UnlockRect x180 |
| 0x4d100 | Query.Issue x4, Query.GetData x4, Device.BeginScene x2, Device.EndScene x1 |
| 0x4e940 | Device.SetSamplerState x56, Device.SetRenderState x2 |
| 0x4f380 | Device.SetSamplerState x6805692 |
| 0x4f510 | Device.SetRenderState x1898312 |
| 0x4f660 | Device.SetRenderState x2 |
| 0x4f6e0 | Device.SetRenderState x2 |
| 0x504d0 | Device.SetStreamSource x12783 |
| 0x505a0 | Device.SetRenderState x3104694 |
| 0x50810 | Device.SetRenderState x244109 |
| 0x50a20 | Device.SetTexture x17210504 |
| 0x50c60 | Device.SetScissorRect x22190, Device.SetViewport x22190 |
| 0x50ec0 | Device.SetRenderTarget x77670, Device.SetDepthStencilSurface x55478, Device.SetRenderState x33288, Device.SetTexture x920 |
| 0x51070 | Device.Clear x33702 |
| 0x511d0 | Device.SetRenderState x44383 |
| 0x51280 | Device.SetRenderState x14 |
| 0x515a0 | Device.SetScissorRect x55479 |
| 0x53d00 | Device.SetVertexShaderConstantF x158881 |
| 0x53f10 | Device.SetVertexShaderConstantF x28358219 |
| 0x54070 | Device.SetPixelShaderConstantF x11343013 |
| 0x54220 | Device.SetVertexShaderConstantF x34368 |
| 0x543b0 | Device.SetVertexShaderConstantF x1025933 |
| 0x54820 | Device.SetIndices x109091 |
| 0x54940 | Device.SetVertexShader x2932880, Device.SetVertexDeclaration x1486227 |
| 0x54a10 | Device.SetPixelShader x3258476 |
| 0x5b7f0 | Query.Issue x11096 |
| 0x5c240 | Texture.GetSurfaceLevel x1233, Device.StretchRect x1233 |
| 0x5cc10 | Device.SetStreamSource x16564, Device.SetIndices x7544 |
| 0x5da10 | Device.SetIndices x5090 |
| 0x5dde0 | Device.SetIndices x30 |
| 0x5f220 | Device.SetIndices x144235 |
| 0x5fe50 | Device.SetTexture x5758824, Device.SetStreamSource x99864 |
| 0x62890 | Device.SetRenderState x22190 |
| 0x62c70 | Device.SetRenderState x22192 |
| 0x62e80 | Device.SetRenderState x22192 |
| 0x63090 | Device.SetRenderState x22192 |
| 0x63540 | Device.SetRenderState x22192 |
| 0x63850 | Device.SetIndices x66576 |
| 0x63f50 | Device.SetStreamSource x3178932, Device.SetIndices x77650 |
| 0x642e0 | Device.SetTexture x336163, Device.SetIndices x77655 |
| 0x64980 | Device.SetIndices x66566 |
| 0x64e80 | Device.SetIndices x55476 |
| 0x659a0 | Device.SetRenderState x44380, Device.SetScissorRect x22190 |
| 0x2b7840 | Texture.LockRect x2142 |
| 0x2b7980 | Texture.UnlockRect x2142, Texture.AddDirtyRect x1611 |

89 functions of the exe call Direct3D 9

| method | functions |
|---|---|
| Device.SetRenderState | 16 |
| Device.SetIndices | 12 |
| Query.GetData | 7 |
| Device.SetStreamSource | 6 |
| Device.SetTexture | 5 |
| Texture.GetSurfaceLevel | 5 |
| Device.SetScissorRect | 5 |
| Device.CreateVertexBuffer | 4 |
| Device.SetVertexShaderConstantF | 4 |
| Query.Issue | 3 |
| Device.CreateIndexBuffer | 3 |
| Device.StretchRect | 3 |
| Device.CreateDepthStencilSurface | 2 |
| Device.SetSamplerState | 2 |
| Volume.UnlockBox | 2 |
| Volume.LockBox | 2 |
| Device.SetPixelShader | 2 |
| Texture.UnlockRect | 2 |
| Device.EndScene | 2 |
| Device.BeginScene | 2 |
| Texture.LockRect | 2 |
| Device.SetVertexShader | 2 |
| Device.TestCooperativeLevel | 2 |
| Device.SetViewport | 2 |
| Device.SetVertexDeclaration | 2 |
| Device.GetSwapChain | 1 |
| Device.GetBackBuffer | 1 |
| Device.Clear | 1 |
| Texture.AddDirtyRect | 1 |
| Device.SetGammaRamp | 1 |
| Device.CreateQuery | 1 |
| Device.CreateCubeTexture | 1 |
| Device.CreateVolumeTexture | 1 |
| Device.CreateVertexDeclaration | 1 |
| Cube.LockRect | 1 |
| Cube.UnlockRect | 1 |
| Device.SetRenderTarget | 1 |
| Device.SetDepthStencilSurface | 1 |
| Device.CreateTexture | 1 |
| Device.CreateVertexShader | 1 |
| Device.CreateRenderTarget | 1 |
| Device.SetPixelShaderConstantF | 1 |
| D3D9.GetAdapterDisplayMode | 1 |
| Volume.AddDirtyBox | 1 |
| Device.CreatePixelShader | 1 |
| Texture.Release | 1 |
| Device.GetAvailableTextureMem | 1 |
| Device.DrawIndexedPrimitive | 1 |

other modules (call sites by method)
- DiscordHook64.dll: 6 sites: Device.CreateVertexBuffer 3, Device.Release 1, Device.CreateTexture 1, Device.QueryInterface 1
- d3d9.dll: 41 sites: Device.AddRef 10, Device.Release 5, D3D9.CheckDeviceFormat 3, Device.QueryInterface 2, Texture.AddRef 1, Device.SetNPatchMode 1, Device.SetVertexShader 1, Device.SetPixelShaderConstantB 1, Device.SetPixelShader 1, Device.SetVertexShaderConstantF 1, Device.SetPixelShaderConstantI 1, Device.GetNPatchMode 1, Device.SetIndices 1, Device.SetVertexShaderConstantI 1, Device.SetStreamSourceFreq 1, Device.SetTexture 1, Device.SetVertexDeclaration 1, Device.SetStreamSource 1, Device.SetPixelShaderConstantF 1, Device.SetClipPlane 1, D3D9.AddRef 1, Device.SetVertexShaderConstantB 1, D3D9.CheckDeviceMultiSampleType 1, Query.GetDataSize 1, Texture.Release 1
- gameoverlayrenderer64.dll: 39 sites: Device.SetTextureStageState 10, Device.SetViewport 2, Device.SetRenderTarget 2, Texture.Release 2, Device.CreateTexture 2, Device.SetNPatchMode 1, Texture.UnlockRect 1, Device.CreateStateBlock 1, Device.AddRef 1, Device.TestCooperativeLevel 1, Device.EndScene 1, Device.BeginScene 1, Texture.LockRect 1, Device.SetPixelShader 1, Device.SetFVF 1, Device.CreateVertexBuffer 1, Device.SetVertexShader 1, Device.GetDirect3D 1, Device.DrawPrimitive 1, Device.GetViewport 1, D3D9.Release 1, Device.SetStreamSource 1, Device.UpdateTexture 1, D3D9.QueryInterface 1, Device.SetTexture 1, Device.GetRenderTarget 1
- WINMM.dll: 33 sites: Texture.UnlockRect 5, Texture.LockRect 5, Texture.GetLevelDesc 3, Texture.GetDevice 2, Texture.GetType 2, Device.Release 2, Device.CreateTexture 2, Device.GetTexture 1, Texture.GetLevelCount 1, Device.GetViewport 1, Texture.Release 1, Device.QueryInterface 1, Device.UpdateTexture 1, Device.GetVertexShader 1, Device.GetSwapChain 1, Device.DrawIndexedPrimitive 1, Device.GetPixelShader 1, Device.GetDeviceCaps 1, Device.EndScene 1

## The functions by job (what a back end has to provide, from the table above)

- **Device, caps, frame:** `0x10260` (CreateDevice and the display mode), `0x10550` (first render states), `0xef10` (GetDeviceCaps, found by reading, not reached by the probe), `0x11140` / `0x12180` (`TestCooperativeLevel`, the device-lost loop), `0x242a0`
  (`GetAvailableTextureMem`), `0x39420` (swap chain, back buffer, first depth buffer), `0x44d00` (`BeginScene` and the end-of-frame upload), `0x453c0` (`EndScene`, `SetIndices`), `0x45060` (`SetGammaRamp`), `0x4d100`.
- **Creation of resources:** images `0x23950` (2D), `0x23be0` (cube), `0x23a90` (volume), release `0x23920`; vertex and index buffers `0x24d00`, `0x24df0`, `0x24f00`, `0x24fa0`, `0x252e0`, `0xf760`; vertex declarations `0x258a0`; shaders
  `0x8f60` (pixel, 4706 per run), `0x9070` (vertex); render targets and depth `0x391d0`, `0x39310`, `0x39110`.
- **Data into resources:** textures and cubes `0x49170` (`LockRect` / `UnlockRect`, 12430 per run), volumes `0x48fa0` and `0x2a0a0` (the light-probe volume, `AddDirtyBox` 266330 times: updated every frame), the film textures `0x2b7840` /
  `0x2b7980`. Vertex and index data is written into the locked ring buffers of `0x44d00`, `0x54820`, `0x54b30` (see the plan).
- **GPU queries (event / occlusion), used for frame pacing and visibility:** `0x4710`, `0xc4c0`, `0x10410`, `0x2f800`, `0x2f860`, `0x45010`, `0x45060`, `0x4d100`, `0x5b7f0`. Direct3D 11: `ID3D11Query`, same idea.
- **State, constants, binding, draw:** the pass functions (`0x54a10`, `0x54940`, `0x54070`, `0x53d00`, `0x53f10`, `0x543b0`, `0x54220`, `0x50a20`, `0x505a0`, `0x50810`, `0x4f380`, `0x4f510`, `0x4f660`, `0x4f6e0`, `0x511d0`, `0x51280`,
  `0x4e940`) and the draw paths (`0x26730`, `0x26840`, `0x267b0`, `0x267e0`, `0x26810`, `0x266a0`, `0x3ec20`, `0x504d0`, `0x5cc10`, `0x5da10`, `0x5dde0`, `0x5f220`, `0x5fe50`, `0x62890`..`0x63540`, `0x63850`, `0x63f50`, `0x642e0`,
  `0x64980`, `0x64e80`, `0x659a0`).
- **Targets, viewport, scissor, clear, copies:** `0x3cc00`, `0x40750`, `0x50c60`, `0x515a0`, `0x50ec0` (render target switch), `0x51070` (clear), `StretchRect` in `0x41160`, `0x484c0`, `0x5c240`.
