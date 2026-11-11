# Native DirectX 9Ex / 11 / 12 for the game (plan and survey, 2026-11-11)

Owner's wish (2026-11-11): the DirectX choice on Settings > Enhanced should be DirectX 9 (original), 9Ex, 11 and 12, with the game itself drawing through each, **no compatibility layer** between the game and the API (no DXVK, no Windows' `d3d9on12`). Vlad's list was the same four.

## What exists today

| Choice | State |
|---|---|
| DirectX 9 | the game's own |
| DirectX 9Ex | `[display] d3d9ex` (src/d3dcap.c): Direct3DCreate9Ex and CreateDeviceEx, managed pool turned into default/dynamic. Alt+Tab works. **A `vid_restart` stalls** (the new device is made, the menus never come back); nobody found why. Not in the 32-bit build's tests. |
| DirectX 12 | only Windows' `d3d9on12` layer (`[display] d3d9on12`), which is a compatibility layer; FSR 3 and DLSS sit on it (src/fsr3.c) |
| DirectX 11 | nothing (the d3d9on12 layer can be taken to D3D11On12, never done) |

## What the renderer asks of Direct3D 9 (64-bit exe, Ghidra, `tools/ghidra/RendererSurvey.java`, `VcallSites.java`)

The renderer is not scattered over the exe: the device is a global (rva 0x410fd20) read in 39 functions, and the calls through the device's vtable are made in a few dozen functions. Call sites by method (all code of the exe; a few may be other objects' calls with the same slot offset):

- Draw: `DrawIndexedPrimitive` 33 sites in 24 functions, `DrawPrimitiveUP` 5 sites in 1 function (rva 0x28ce0), `DrawPrimitive` none.
- State: `SetRenderState` 71 sites in 23 functions, `SetSamplerState` 16 in 2 (rva 0x4e940, 0x4f380: a state cache), `SetTexture` 11, `SetScissorRect` 9, `SetViewport` 3, `Clear` 1, `SetRenderTarget` 1, `SetDepthStencilSurface` 1, `StretchRect` 4.
- Shaders and geometry: `SetVertexShader` 3, `SetPixelShader` 3, `SetVertexShaderConstantF` 6 (4 functions), `SetPixelShaderConstantF` 3, `SetStreamSource` 18, `SetIndices` 13, `SetVertexDeclaration` 7, `CreateVertexShader` 2, `CreatePixelShader` 2.
- Resources: `CreateTexture` 7, `CreateVertexBuffer` 5, `CreateIndexBuffer` 5, `CreateQuery` 3, `CreateDepthStencilSurface` 1, `CreateCubeTexture` 1.
- Device life: `Reset` 2, `Present` 2, `TestCooperativeLevel` 5, `GetBackBuffer` 2.

So the surface is narrow: roughly 25 distinct device methods, a few hundred call sites, concentrated in the renderer's command-buffer code (rva 0x4e000-0x66000) and its device set-up (rva 0x23000-0x2a000, 0xf000-0x12000). Textures, vertex buffers and index buffers are used through their own interface methods (Lock, Unlock, GetSurfaceLevel, ...), not counted above.

## What a native backend has to do

1. **Shaders.** The game ships Direct3D 9 shader model 3 bytecode (vs_3_0 / ps_3_0; the materials of the fastfiles hold it, the HLSL is not available). D3D11 and D3D12 cannot load it. A translator is unavoidable: SM3 tokens -> HLSL text -> `D3DCompile` (d3dcompiler_47.dll, which the mod already uses in src/fsr3.c) -> DXBC (shader model 5.0 for D3D11, 5.1 for D3D12). About 40 opcodes, the register files (c, i, b, v, o, t, s, r, a0, aL), the dcl semantics. Constants: float constant registers c0-c255 map to one constant buffer per stage that `SetVertexShaderConstantF` / `SetPixelShaderConstantF` fill.
2. **State model.** D3D9's loose render states and sampler states become D3D11 state objects (created on demand, cached by a hash of the state words) or a D3D12 pipeline state object per (shaders, vertex layout, blend, depth, raster, render target formats). The renderer's own state cache (0x4e940) is the natural place to hook, so the translation sees one coherent state at each draw.
3. **Resources.** Formats (DXT1/3/5, A8R8G8B8, A16B16G16R16F, R32F, depth formats sampled as textures: D3D9 depth "INTZ" and shadow-map PCF hacks), dynamic buffers (D3D9 Lock with DISCARD/NOOVERWRITE -> D3D11 map flags or D3D12 upload ring), vertex declarations -> input layouts, render-target textures and multisample resolve (`StretchRect`), occlusion queries.
4. **Swap chain.** DXGI flip model, windowed/borderless, `Present`, resize; the game's own `vid_restart` / `Reset` paths.
5. **Everything the mod does on the D3D9 device today.** The native split screen's compositor, the console overlay, screenshots (src/shot.c), the sunwait/aspect hooks, FSR 3 / DLSS, font atlas filtering: all of it uses `IDirect3DDevice9` calls on the game's device (native.c, console.c, shot.c, fsr3.c, d3dcap.c). A native D3D11/12 backend has to give these a device to talk to, or they must be rewritten against the new backend. This is the biggest hidden cost: native.c alone is about 9000 lines.

## Honest options

- **A. A device object that the game's renderer calls (the backend implements the ~25 methods, resources and the shader translator).** Technically this is what DXVK or d3d9on12 are, made part of the mod and fed by the call sites we know; "native" only in the sense that nothing outside the mod sits between the game and Direct3D 11/12. It keeps the mod's own D3D9-using features working if the backend also exposes the methods they use. Estimate: D3D11 first (state objects, simple resources): several weeks of focused work for a first image, months for fidelity (shadows, sunlit effects, split screen, FSR 3); D3D12 on top of the same translator: weeks more (PSO cache, descriptors, barriers, upload ring).
- **B. Rewrite the renderer's back end (the command-buffer functions at 0x4e000-0x66000) against D3D11/12 without any D3D9-shaped object.** Cleaner and faster in the end, but every game structure the back end reads (GfxCmdBufState, material and technique data, GfxBuffers) has to be reverse engineered, and the mod's features still need their own path. Months.
- **C. Keep Windows' layer for DirectX 12 and add D3D11On12 for 11** (hours to days each): works today, but it is a compatibility layer, which the owner does not want.

## Order of work

1. **DirectX 9Ex first** (native in the sense of the owner's wish, nothing new to write beyond a bug): find why a `vid_restart` stalls on an Ex device and fix it; then put it on the Settings > Enhanced row as an option (not before: it stalls the game when the video options are applied). Leads: the game calls `TestCooperativeLevel` (5 sites) and `Reset` (2 sites); on an Ex device `TestCooperativeLevel` never says `DEVICENOTRESET` and `Reset` is not the Ex call (`ResetEx`); a new device made while the old one is still referenced (our d3dcap keeps the device pointer, the console overlay and native.c hold state blocks and textures on it).
2. **A shader translator (SM3 -> HLSL) with a test that translates the shaders of every material of a loaded level and compiles them** (no game run needed, only the zone data in memory). This is the piece every D3D11/12 path needs, and it can be validated offline.
3. **A D3D11 backend for the world pass** behind a switch, first on the Trainer, then the mod's features.
4. **D3D12** on the same translator.

## 9Ex: what was found on 2026-11-11 (the `vid_restart` stall)

Reproduced in a test copy (`d3d9ex=1`, console `vid_restart` in the main menu, 1280x720 windowed). The game makes the new device (`CreateDeviceEx` succeeds, the render states are set), the window's dark title bar is applied, and then nothing: no Present, no log lines from the menus or the console, no font re-import. Without 9Ex the same `vid_restart` goes on to re-import the fonts and the menus come back within a second. Sometimes the process ends by itself a few seconds later (exit code 0).

- The main thread is inside the device set-up function (rva 0x10550, the one that sets the render states) -> 0xf3f0, which calls the zone loader `DB_LoadXAssets` (rva 0xf0ae0, the call is the sync one) and sits in its `Sleep(1)` loop waiting for the loader's flags (0x140a8a830 / 0x140a8a834) to clear. So the zone reload after the restart never finishes.
- A thread inside `d3d9.dll+0x2c43b` (called from the game's thread start code, no game frames above it) waits in the runtime; the window procedure thread, the overlay thread (gameoverlayrenderer64) and others are idle.
- Not yet known: where the loader's thread is. `crash.c`'s stack dump now prints `module+offset` for frames in other modules (d3d9.dll, the driver), so the next dump shows what each thread waits in. Suspects: a `CreateTexture` / `CreateVertexBuffer` of the loader going through our default-pool conversion and waiting on the device lock held by a Present of the new (hidden) window; or a state the Ex runtime keeps for the old device.

### 9Ex update (later on 2026-11-11): the first cause is found and fixed, the next failure is one step further

- **Found:** the stall was the game's own fatal error. The loader's thread called `Create2DTexture` (rva 0x23950) and the exit stack of the process (exe+0x23a6d -> 0x1fb74a, the error function -> exit) shows it failed. After a `vid_restart` the new Ex device's CreateTexture / CreateVertexBuffer / ... vtable slots were the **original** d3d9.dll entries, not our hooks (the log line added in `hook_resource_creation`: slot 00007FFDE244E310 against our hook 00007FFD599C3350): a managed-pool texture on an Ex device fails. `hook_resource_creation` only ever ran for the first device. **Fix:** it now checks every new device and hooks again when the slot is not ours (src/d3dcap.c, uncommitted). With it the zone reload after the restart goes on (the log reaches the 1000th converted texture).
- **Next failure:** the game still ends at the first or second restart, black: the engine's `Failed init after: Tried to use 'feedbackSampler' when it isn't valid` (reported by the owner, 2026-11-11). The message is printed by the sampler binding code (rva 0x53d00) when the image behind a code sampler is null; `feedbackSampler` is code sampler 8 (the table is set up in rva 0x45930; its image pointer starts at 0 and is set when the feedback effect runs). Why a material that needs it is drawn with it null after a restart on an Ex device is not known: leads are a race with the render thread during the re-init (Present on an Ex device does not block the way a plain device's does) and an image whose creation was skipped.
- Tools added: `tools/ghidra/FindString.java` (a string and its referencing functions), a failed-creation log in the d3d9ex hooks, `module+offset` frames in the stack dump.

### 9Ex status at the end of 2026-11-11

A `vid_restart` on 9Ex works once, like the plain device. A second restart ends the game with the `feedbackSampler` error on both (checked with the mod's restart preparations switched off, `MW2COOP_VIDPLAIN=1`): the engine's own failure; the error is raised on the render thread and on the main thread while the renderer is set up again, in the sampler binding (rva 0x53d00) for a material that needs the feedback image (code sampler 8, its image pointer is never set after the table is cleared in rva 0x45930). The DirectX row of Settings > Enhanced offers DirectX 9 / 9Ex / 12.

### Step 2 started: the shader translator (`tools/sm3_to_hlsl.py`)

- **Dump:** `MW2COOP_DUMPSHADERS=1` makes the game's device write every vertex and pixel shader it is given (the bytecode of the materials) to `<game dir>\mw2coop\shaderss_<hash>.bin` / `ps_<hash>.bin` (src/d3dcap.c, QA only; the files are the game's data and stay in the test copy). The main menu and the Trainer give 2730 distinct shaders (244 vertex, 2486 pixel, 11 MB).
- **Translator:** a Python module and CLI: version token, DCL / DEF / DEFI / DEFB, about 50 opcodes (ALU, texld / texldl / texldd with projection and bias, cmp, lit, sincos, dsx / dsy, texkill, if / ifc / else / rep / loop / break, relative constant addressing, the matrix ops), source and result modifiers, vPos / vFace; the output is HLSL for shader model 5. Not translated: call / label subroutines, predication (they raise `Unsupported` and are counted); none occurs in the 2730.
- **Compile check:** `python -I tools/sm3_to_hlsl.py --check <dir>` runs each translation through `D3DCompile` (d3dcompiler_47.dll): **2730 of 2730 compile** (ps_5_0 / vs_5_0). That shows the translations are valid HLSL, not that they compute the same as Direct3D 9.
- **Not done:** the semantic check (run a shader on a Direct3D 9 device and the translation on a Direct3D 11 WARP device with the same constants and compare the numbers; the legacy rules that differ: `mul` 0 * inf = 0, `rcp` / `rsq` of 0, `pow`, `lit`, `exp` / `log`), the shadow-map samplers (a comparison sampler depends on the bound texture), the constant table names (the CTAB comments give them), shaders of more levels (the other zones may use call / label or predication), and the cbuffer layout the backend fills.

### Step 2: the semantic check (`tools/sm3_semantic_check.py`, `tools/sm3check.c`)

For each pixel shader the harness runs the original bytecode on a Direct3D 9 device (the machine's GPU) and the translation on a Direct3D 11 WARP device (software) with the same pixel-shader constants (seeded random; the shader's own `def` constants win), the same interpolants (a vertex shader writes seeded constants to every interpolant), the same textures (2D, cube and volume by the shader's declarations, linear, clamped) and the same render targets (A32B32G32R32F, up to four), and compares the pixel it writes.

- **Interface lesson:** Direct3D 11 links the stages by register; the pixel shader's input structure now always starts with `SV_Position` and the vertex shader's output structure starts with it too. Without that the interpolants were shifted by one (a pixel shader read the position where it wanted a colour).
- **Result on the 2485 pixel shaders of the main menu and the Trainer** (one depth-output shader skipped), noise textures: 1571 match, 506 both write zero, 408 differ; of those 306 by less than 0.02, 76 by 0.02-0.2, 26 more. The differences follow the filtering of the noise (cube face seams are filtered across faces by Direct3D 11 and not by this Direct3D 9 GPU, 8-bit filter weights) and the GPU's approximations of exp / log / pow.
- **Result with `SM3FLAT=1` (every texel of a texture the same, so only the arithmetic is compared): all 2485 agree** (1987 match, 498 both write zero, 0 differ). So the translation computes what Direct3D 9 computes for these shaders: the opcodes, the modifiers, the sampler dispatch (2D, cube, volume, projected, biased, lod), the flow control, the constants (def, relative addressing).
- **Vertex shaders** (`SM3_VS=1 python -I tools/sm3_semantic_check.py <folder>`, `sm3check --vs`): Direct3D 9 runs the original bytecode with `ProcessVertices` on a device with software vertex processing (the runtime's own CPU implementation, exact arithmetic), Direct3D 11 WARP runs the translation with stream output (the vertex shader's bytecode as the geometry shader of a stream-output stage); three vertices of seeded values, the same constants, every output component compared. **All 244 vertex shaders agree.** The check was shown to see errors: with the translation deliberately broken (every ` + ` turned into ` - `) all 30 tried shaders differ.
- **Eight levels** (the Trainer, Roadkill, Cliffhanger, Airport, Favela, Oil Rig, Invasion, Estate and the Endgame wake-up; the plain game, each for about a minute, 2026-11-11): **4120 distinct shaders** (285 vertex, 3835 pixel). All translate and compile; **all 3834 pixel shaders (flat textures: 3178 match, 656 write zeros on both sides) and all 285 vertex shaders agree with Direct3D 9**, none of them uses call / label subroutines or predication, none reads a texture in the vertex stage, one writes depth.
- **Not covered:** the shadow-map comparison samplers (a property of the bound texture), the one depth-output shader, the shaders of the rest of the campaign (the dump is one environment variable away).

Steps 3-4 are not started.

## Decision (2026-11-11, the owner): option B

Rewrite the renderer's back end directly against Direct3D 11 / 12, with no Direct3D 9 shaped object in between. Option A (a fake `IDirect3DDevice9`) and option C (Windows' layers) are off the table.

### The back end, as read from the 64-bit exe (Ghidra, `local/scratch/ghidra/backend1.c`; all numbers are rvas unless stated, **observations**, not yet checked in a running game)

- Every draw function takes a context `{input*, state*}`. `state` is the command-buffer state (GfxCmdBufState): `+0xd0` the device, `+0xd8` the index-buffer cache, `+0xe8` / `+0xf0` / `+0xf8` the stream-source cache (stride, buffer, offset; `SetStreamSource` is skipped when they match), `+0xe0` a mode flag, `+0x120` the material, `+0x128` the technique index, `+0x130` the technique, `+0x138` the current pass.
- A pass (MaterialPass): `+0x18` per-primitive argument count, `+0x19` per-object argument count, `+0x1a` stable argument count, `+0x20` the argument array (16 bytes each, in that order). A draw is always the same sequence: select the pass (`0x54a10`), apply render state (`0x54940`), stable arguments (`0x54070`), vertex stream, per-object arguments including the samplers (`0x53d00`), per-draw constants (`0x53f10`), then `DrawIndexedPrimitive(TRIANGLELIST, 0, 0, numVerts, startIndex, primCount)`. The loop over passes is `0x26840`; the other draw paths (`0x399f0`, `0x5c960`, `0x5cc10`, `0x5e950`, `0x5f220`, `0x5fe50`) are the same sequence for sprites, lights, particles, the sky and so on; `0x50a20`, `0x50ec0`, `0x543b0`, `0x54220`, `0x28ce0` and `0x252e0` set up state and targets.
- So the game already has a **small, regular back end**: a few dozen functions that turn "material + technique + pass + argument list" into device calls. That is what is replaced. The front end (visibility, sorting, building the draw lists, the zone data, the fastfile loader) stays untouched.
- The code samplers (`feedbackSampler` and its siblings, table at input `+0x12c0`) are bound through `0x53d00`; the error "Tried to use 'feedbackSampler' when it isn't valid" comes from that binding when the image pointer of code sampler 8 is NULL (found while looking at the second `vid_restart` after a 9Ex or plain restart).

### What the new back end is made of

1. **Shaders:** `src/dx/sm3.c` (done, below) turns each material's shader bytecode into HLSL; `D3DCompile` makes DXBC; the compiled blobs are cached on disk by a hash of the bytecode so the compile cost is paid once.
2. **Argument binding:** the pass's argument array already says, for every shader constant, where its value comes from (a code constant, a material constant, a sampler). The new back end evaluates the same list but writes into a per-stage constant buffer image (`float4 c[256]`, one `Map(DISCARD)` / upload per draw or per changed range) instead of calling `SetVertexShaderConstantF`.
3. **State:** the render-state words of a pass (blend, depth, cull, alpha test, colour write mask, stencil) become cached D3D11 state objects, or pipeline states on D3D12, keyed by the words.
4. **Resources:** the game creates textures, vertex and index buffers through its own wrappers (`Create2DTexture` 0x23950 and the buffer creators); they are replaced by the new API's resources. Image data reaches them from the loader, not from `Lock`.
5. **Frame:** swap chain (flip model), render targets and the depth buffer, the resolve and the post-effects, then the mod's own overlays (console, split-screen compositor, FSR 3 / DLSS) rewritten against the new device.

### How it is validated: the mirror, then the switch

A rewrite of this size cannot be judged by playing. Each milestone has a check that runs without a human:

- **M1 (done):** shader translator in C, byte-for-byte the same compiled shaders as the Python reference on all 4120 shaders of eight levels (`tools/sm3c_test.c` + `tools/sm3c_compare.py`). The Python version was validated against Direct3D 9 itself (`tools/sm3_semantic_check.py`: 3834 pixel and 285 vertex shaders agree).
- **M2 (mirror):** while the game draws through Direct3D 9 as today, a second D3D11 device receives the same draws, fed from the back-end functions' own data (material, pass, arguments), not from the D3D9 call stream. Check: the D3D11 frame against the D3D9 frame, pixel by pixel, on the QA levels (Trainer first). The mirror is scaffolding; it is deleted at M5.
- **M3 (UI and sprites), M4 (world, lights, particles, sky):** each draw path switched over one at a time, the same frame comparison after each.
- **M5 (switch):** the D3D9 device is no longer created; the mod's overlays move to the new device; the Enhanced page's DirectX row gets the real "DirectX 11" entry.
- **M6:** Direct3D 12 on the same translator, state cache and argument binding (pipeline states, descriptor heaps, an upload ring, barriers).

Not decided yet: whether the 32-bit exe follows (its back end has other rvas; the address resolver of `docs` would need a section for them) and how the split screen's second view is drawn (done in `native.c` on the D3D9 device today; not read for this plan yet).

### The structures the back end reads (64-bit exe; read from the decompiled draw path `0x26840` / `0x54a10` / `0x54940` / `0x54070` / `0x53d00` / `0x53f10` / `0x543b0` / `0x50a20`; field names are ours, taken from what each field does)

**Observed** (every offset below is read by the code above, and the vtable slots match `IDirect3DDevice9`):

- **Context:** `{input*, state*}` (16 bytes, passed by value or pointer). `input` = per-frame data (code constants, code samplers), `state` = GfxCmdBufState.
- **input:** `+0xe00 + i*16` code constant `i` for `i < 0x4c` (a float4 each); derived constants (`i >= 0x4c`) are computed on demand by `0x502f0`; `+0x156c + i*2` / `+0x1674 + ((i-0x4c)>>2)*2` a 16-bit version counter per constant (a constant is re-sent only when the version in the cached `(version << 32 | index)` word of the register differs); `+0x12c0 + k*8` the image for code sampler `k` (NULL means "Tried to use '<name>' when it isn't valid", names at 0x144113618 + k*8), `+0x1398 + k` its sampler-state byte; `+0x16e0` a target mode.
- **state:** `+0x10 + s*4` sampler-state word of sampler `s` (16 samplers), `+0x00 + s` its filter byte, `+0x50 + s*8` the bound image of sampler `s`; `+0x150 + r*8` the cached `(version, index)` word of vertex-shader constant register `r`; `+0x350 + r*8` the same for pixel-shader constants; `+0xd0` device; `+0xd8` index buffer; `+0xe0` vertex type (index into a decl's per-type table); `+0xe8/+0xf0/+0xf8` stream 0 (stride, buffer, offset), `+0x100/+0x108/+0x110` stream 1; `+0x118` current vertex declaration; `+0x120` material; `+0x128` technique type; `+0x130` technique; `+0x138` current pass; `+0x140` pass index; `+0xb54/+0xb58` the two render-state bit words in effect; `+0xb68` current pixel shader; `+0xb70` current vertex shader; `+0xb98` current render target / depth set.
- **Material:** `+0x00` name; `+0x20 + techniqueType` one byte = index of the first state-bits pair for that technique; `+0x58 -> +0x10 -> +0x38` the technique used by the full-screen paths; `+0x60` texture table (16-byte entries: `+0` name hash, `+6` sampler-state byte, `+7` semantic, `+8` image); `+0x68` constant table (32-byte entries: `+0` name hash, `+16` four floats); `+0x70` state-bits table (8 bytes = two 32-bit words, the render-state description of one pass).
- **Technique:** `+0x0a` pass count (16-bit), `+0x10 + p*0x28` pass `p`.
- **Pass (0x28 bytes):** `+0x00` vertex declaration set (`+0x30 + vertexType*8` the Direct3D vertex declaration, 0 = "Vertex type %i doesn't have the information used by shader %s in material %s"), `+0x08` vertex shader (`+0` name, `+8` the Direct3D shader object), `+0x10` pixel shader (`+8` the object), `+0x18` per-primitive argument count, `+0x19` per-object argument count, `+0x1a` stable argument count, `+0x20` the argument array, in that order (per-primitive, per-object, stable).
- **Shader argument (16 bytes):** `+0` type (16-bit), `+2` destination register, `+4..` by type. Types and the code that handles them: 0 material vertex constant (name hash at +8, looked up in the material's constant table, float4 at entry+16, `SetVertexShaderConstantF`), 1 literal vertex constant (pointer at +8), 2 material sampler (name hash at +8, looked up in the texture table; semantic 11 = water, 2/3/5/8 = the "default" images when `DAT_1441135c8` is set), 3 code vertex constant (`+8` index 16-bit, `+10` first row, `+11` row count), 4 code sampler (`+8` index into `input+0x12c0`), 5 code pixel constant (same layout as 3), 6 material pixel constant, 7 literal pixel constant. Only the pixel-constant types are in the stable list; `0x54a10` applies the pixel stable arguments, `0x54070` the vertex stable ones, `0x53d00` the per-object ones, `0x53f10` the per-primitive code vertex constants.
- **Draw:** vertex data goes in through `SetStreamSource` (slot 100 = `0x320/8`), indices through `SetIndices` (`0x340`) from a ring (`0x54820` copies `count*3` 16-bit indices into a dynamic index buffer, `0x54b30` the same for 32-byte vertices), `DrawIndexedPrimitive(TRIANGLELIST, 0, 0, numVerts, startIndex, primCount)`.

**Not yet known:** the render-state bit words' layout (`0x505a0` and `0x50810` turn the two words into `SetRenderState` calls and `0x4f380` turns a sampler word into `SetSamplerState`), the derived constants (`0x502f0`), the render-target / viewport set-up (`0x50ec0`, `0x4e940`), what the vertex types are, and where the images get their Direct3D textures (the loader). Those are the next slices.

**Consequence for the plan:** the data the new back end needs is already in these tables. The translation of one pass is: bind the pass's two shaders, fill two constant buffers from the argument list (the same cached-version logic can stay), bind images for the sampler arguments, derive blend / depth / raster state from the two state-bit words, set the vertex layout from the declaration set for the current vertex type, and draw. The mirror (M2) can hook the same function that applies a pass (`0x54a10` and its four helpers) and issue the Direct3D 11 version of exactly that.
