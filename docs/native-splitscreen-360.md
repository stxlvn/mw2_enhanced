# Split screen the way the Xbox 360 game does it: study and plan (2026-11-14)

Owner's request (2026-11-14): study how the Xbox 360 version does split screen and build the mod's the same way, instead of fixing the current design
problem by problem. Reference: the owner's own disc (Ghidra project `local/ghidra-proj-x360`, read-only; nothing from it goes into the mod), and the PC
exe. Earlier notes: [native-splitscreen-plan.md](native-splitscreen-plan.md) (the current design and its history).

## How the 360 does it (read from the 360 binary and the PC exe)

1. **One cgame state per local player, as arrays.** The 360's cgame state is an array indexed by the local player number: the shellshock update
   (360 `0x8210aff8`, PC rva `0xb7a20`) addresses `cg` as `0x8247a5e8 + localClientNum * 0x2f1e4`. The PC build folded these arrays into single statics
   (`cg` at rva `0x422440`), but kept the local player number as an argument almost everywhere.
2. **Both views are drawn in the same frame.** Each local player's cgame draw makes its own scene with its own viewport (its rectangle of the screen),
   and the renderer has per-view state: on the PC too, the renderer's view array is still indexed by the local player (`0x4812878 + localClient * 0x2b8`).
3. **The engine asks how many local players there are and adapts.** On the PC that question is a stub: rva `0x7ac10` returns 1, and the per-player
   viewport (rva `0xbd520`) returns one static rectangle. 12 places in the engine ask the count, 4 ask the viewport. Example: the shellshock screen effect
   (rva `0xb75b0`) saves the whole screen with one player (`0xe530`) and only that player's section of the screen with two or more (`0xe580`, the render
   command 3, still in the PC renderer). With one shared saved picture and two players saving the whole screen, each view blends the other's frame: the
   "ghost" (owner: "the lower player's screen duplicated in the upper one").
4. **Each player is his own client:** his own entity number, his own sound listener (the PC engine still has two listeners, `SND_SetListener` rva
   `0x23db90`), his own field-of-view scale (`cg_playerFovScale0` / `1`, rva `0x51e1c0` / `0x51e1c8`), his own effects system.

## What the mod does now, and why the bugs keep coming

The current split screen (`src/native.c`, `[native] experiment=10`) draws **one view per engine frame**: player 1's on even frames, player 2's on odd
frames, each into its own texture, and a compositor puts the two halves together at Present. All per-player client state is one copy, swapped in and out
(about 13 memory ranges, `ranges[]`). It was chosen on 2026-10-26 because drawing two scenes in one frame crashed the renderer's back end (experiment 1:
garbage surface streams for view index 1 in the draw loops `0x63f50` / `0x64e80`).

The bugs that come from that design, not from a missing range:
- each view runs at half the engine's frame rate, and its camera and sound are updated every other frame: the camera jitter of both players
  (Vlad, 2026-11-14, Spec Ops snowmobile races), player 2's sound half a second late;
- anything the engine keeps once per frame or once per process for "the" player (the saved-screen picture of shellshock, the shared targets of
  post-processing, the LOD choice, the sun shadow cull, effects spawned by scripts) belongs to whichever view drew last: the ghost hands of Endgame,
  the AC-130's field of view leaking into the other player's view, shells not going where the crosshair points;
- every new scene with a new engine feature finds another such piece of state.

## The plan: two local players in one frame, as the 360 does

Each phase ends with something checkable in the game; the current design stays the fallback until phase 3 passes.

| Phase | Work | Check |
|---|---|---|
| A. Renderer, two views per frame | Find why the back end got garbage for view 1 (experiment 1): compare the PC renderer's per-view paths (scene submit `0x8210`, the view count at frame data +`0x419e8`, the surface streams of the draw loops `0x63f50` / `0x64e80`, the cull workers) with the 360's, find what was stubbed to one view, and restore it | Trainer: two scenes of the same player in one frame, top and bottom, no crash for 10 minutes |
| B. Local player count and viewports | `0x7ac10` returns 2 while split screen runs; `0xbd520` returns each player's rectangle; go through the 12 + 4 callers and check each behaves per player (shellshock sections, 2D placement, ...) | the Endgame ghost gone with no special case; the HUD in each half from the engine's own placement |
| C. Both cgame draws in one frame | The client frame runs the cgame draw for player 1 and player 2 back to back (the client statics still swapped between them, the PC has no arrays), each with its own local player number for the renderer and sound; the frame alternation and the Present compositor go | both halves at the full frame rate; no camera jitter in the snowmobile races; player 2's sound in time |
| D. Per-player engine state | The remaining shared state the 360 keeps per player (field-of-view scale, effects, sound listener, vision, LOD) is mapped from the 360 arrays to PC addresses and made per player | the AC-130 mission (FOV, aim), Endgame, Cliffhanger, the Spec Ops list without crashes |
| E. Clean-up | the old alternation code, its workarounds (ranges no longer needed, the compositor, fxsplit, sunsplit...) removed; 32-bit port | the regression sweep of all levels and Spec Ops missions in split screen |

## Open questions

- Phase A is the risky one (it is where experiment 1 stopped). The 360 binary shows working multi-view code next to the PC's stubs, which is what
  was missing on 2026-10-26.
- Whether the PC renderer's per-view arrays have room for 2 views everywhere (the view array above has 5 entries) or some were cut to 1.

## Phase A notes (2026-11-14)

- Experiment 1 rerun with the current build (`[native] experiment=1`, Trainer, briefing skipped, 105 s): no crash any more, but no two views either: the
  window shows ONE view stretched over the whole height, and the skinned models (soldiers, the weapons in their hands) have long broken triangles.
  Experiment 1 drew both scenes as local client 0.
- The renderer is built for several scenes per frame: `R_RenderScene` (rva `0x8210`, the refdef and a scene number) takes its scene number from a
  per-frame counter (rva `0x4a40`: global at rva `0x41136f0`, also written to frame data `+0x41a30` / `+0x41a34`), appends a view slot (frame data
  `+0x1100 + n * 0x150`, count at `+0x419e8`) and sets the scene up in its own slot (frame data `+0x41a38` array, stride `0x13a0`, rva `0x5160`).
- So the second scene has its own slots; what breaks is shared per-frame data the second scene reuses: first suspect the skinned-model cache.
- The cgame draw (rva `0xbca00`) calls begin-frame (`0xc840`) itself; the PC's screen update (`0xd5890`) calls the draw with a literal local player 0. A second
  draw in the same frame must skip begin-frame (native.c `second_pass`, already there): then the frame holds 2 scenes and 2 views (logged: "scenes 2 views 2").
- The back end draws every view of the frame whose type (`+0x1380` in the scene slot, set to 2 by `0x5160`) is 2 (`0x453c0`, ported as `ngfr_453c0`). Both
  views were drawn, but over the whole screen: the view's rectangle comes from the per-player view rectangle (`0xbd520` -> rva `0x3bd808`, normalised
  x, y, w, h = 0, 0, 1, 1), not from `r_subwindow`, which only moved the projection. With that rectangle set to the top half for the first draw and the bottom
  half for the second (`[native] experiment=1`, `exp1rect=1`), the renderer draws two real views per frame, and crashes the way the October experiment did:
  view index 1, in the cached static model loop (`0x63f50`, at `0x6400c`) after ~20 s; with `r_smc_enable 0` in the model instance loop (`0x63850`, at `0x63888`);
  with the local player count at 2 (`[native] lccount=2`, the immediate of `0x7ac10`) still at `0x6400c`, after ~70 s. The bad addresses are small offsets:
  a per-view list (the static model surfaces of the view) that is built for view 0 only.
- Next: the front end's per-view static model data (visibility and cache), compared with the 360's (where each local player's view has its own).
- **Cause found and worked around: the static model draw data of the second view overflowed its region.** The four static model list builders
  (`0x3ad40`, `0x3b810`, `0x3be80`, `0x3c440`, called from the scene set-up `0x5160`) take their output chunks from two frame data regions by atomic bumps of
  two counters that begin-frame resets: `+0xb7f80 + [+0x41964]` (0xa000 a chunk) and `+0xd2f80 + [+0x41968]` (0x4000). The first region has 0x1b000 bytes before
  the second starts; one view uses about half of it, two views used 0x1c000-0x24000 (logged) and wrote over the second region. The second region is not free
  either (other data up to `+0x32318`, probe `fdprobe=1`). Fix (`split_smc_redirect`, `[native] exp1smc=1`): before the second view's draw the two counters are
  set so that the bumps land in two 1 MB buffers of the mod, one pair per frame data set, allocated above the frame data (the counter is an unsigned 32-bit
  offset from it). Result: two views per frame on Trainer, top and bottom half, no crash in 150 s (the 10 minute run of the phase A check follows).

## Phase C started: both players in every frame (`[native] experiment=11`, 2026-11-14)

- `client_frame_inner` (mode 11): player 2's client logic runs first every frame, with his own screen update suppressed through the screen update's busy flag
  (rva `0x5db410`, checked by `0xd5b00`); then player 1's frame runs, and its cgame draw (`cg_draw_hook_inner`, mode 11) draws player 1, swaps to context 1 and
  draws player 2 (second draw: begin-frame skipped, static model chunks redirected, player 2's 2D off for now).
- **The view rectangle is the cgame's** (pixels, rva `0x43c630`: x, y, w, h; copied into the refdef by `0xbe710`; inside the context-swapped range, so one per
  player): player 1 (0, 0, W, H/2), player 2 (0, H/2, W, H/2). The normalised per-player rectangle `0x3bd808` (what `0xbd520` returns) is only used by the
  shellshock's screen sections; it is set per view too.
- **The back end drew the 3D of one scene only** (`0x45690` calls the 3D pass `0x5c620` for the frame data's "current scene" `+0x41a30` = the last one submitted).
  The call at `0x456ee` now runs the pass for every scene of the frame, with `+0x41a30` set to the pass's scene (the pass reads it at `0x4df70` / `0x4e030`).
- **The pass clears after its target switch with the whole target as viewport** (`0x5c484`): only the first pass clears now (the whole picture).
- Result (Trainer, D3D9 and native D3D11): the screen is split by the engine itself, player 2's bottom view is right; player 1's top view is drawn but lit
  wrongly (white / black). Cause (next): the scene set-up writes per-scene lighting into fields of the frame data that exist once (the sun at `+0x41a48..+0x41a54`,
  the sun shadow flag `+0x4195c`, `+0x41958`, the light list `+0x421e0` with entries at `+0x423b0` stride 0x1e0 and their shadow bits `+0x41970`): the last scene's
  win. Next: keep them per scene (saved after each scene's set-up, put back before its 3D pass), checked against the 360's scene set-up.
- Tried (off by default, `[native] scenekeep=1`, `scenekeepparts=<bits>`): keeping the frame data block `+0x41940..+0x62980` per scene (saved after each scene's draw,
  after the front end's workers finished: `ctx_wait_workers`) and putting it back before each 3D pass. Putting back the light list part (`+0x421e0..`) crashes at once
  in the light loop (`0x64ac4`); without it there is no crash but player 1's view stays wrong (white / black). So the top view's problem is not (only) those fields.
  Open: what else of the frame is built for one scene (the sun shadow cascades `0x659a0` per pass, the post-processing per view `0x484c0`, the 2D of player 1), to be
  found in the 360's back end (its frame loop over scenes) instead of by trial.

## The 360's renderer front end, read (2026-11-14 evening)

Read from the 360 image (`default_sp.pe`, Ghidra project `mw2x360`; the 360's register save helpers `0x823ddf40..0x823de0e0` had to be taken out of
"no return" first, they cut every function short):
- **The 360's `CG_DrawActiveFrame` is `0x82113110` (PC `0xbca00`).** It stores its local player number in a renderer global, **360 `0x8244b424`**
  (PC rva **`0x51deb8`**, written by `0xbca00` from its argument), and has `lc == 0` / `else` branches that the PC kept (PC `if (param_1 == 0)` in `0xbca00`).
  The per-player begin function `0x821130a8` (PC `0xbd610`) sets the same global.
- **The 360's `R_RenderScene` is `0x8238b158` and its scene set-up `0x8238a288` (PC `0x8210` and `0x5160`).** Same steps in the same order: the
  shellshock for the player (`0x82112298(lc)`, PC `0xbd3d0`), the effects of the refdef's player only (`if (lc == refdef.lc)`, PC `0x20a2c0`), the
  effects' time step for player 0 only (`0x82141e70(lc)` = "lc == 0", PC `0xdb4d0` -> `0x208a10`), the worker waits by number (PC "wait %s").
- **The renderer keeps view data per local player**, indexed by that global: the 360 `0x83dc2080 + lc * 0x2b8` (3 frusta of `0xe8`: view and sun shadow
  cull planes), the PC **`0x4812a40 + lc * 0x2b8`, with room for 4 players** (the next globals start at `0x4813528`). Users on the PC: `0x5160`,
  `0x4780` (shadowed spot lights), `0x17740`, `0x18b60`, `0x19000`, `0x1b9b0`, `0x1bee0`, `0x38cc0`; all run inside `R_RenderScene`.
- **What mode 11 did wrong:** player 2's draw runs through the cgame as player 0 (the PC's client arrays have one element: experiment 3), so his scene
  used player 0's renderer slot too, and the effects' time step ran twice a frame.

### Change (`[native] exp11lc=1`, default on in mode 11)

- The call of `R_RenderScene` (`0xca523`) is hooked: player 2's runs with the renderer global at 1 (as on the 360), then the front end's workers are waited
  for and it goes back to 0. The hook passes every register argument on (`R_RenderScene(refdef, scene)`: a one-argument hook crashed the back end's light
  loop at `0x3f0d9` in every run, with or without the change).
- Readers of the global that index cgame data of one element get 0: the call sites `0x82dd` (shellshock `0xbd3d0`) and `0x6356` (`0x93ac0`,
  `0x51e330 + lc * 0x2cd0`); the per-entity "culled in" byte of `cpose_t` (`+3 + lc`, one byte on the PC: `0xb20d0`, `0xb20e0`, `0x9e547`); the entity
  lookup at `0xa4cbf`; the local entity callbacks of the workers `0x934b0` and `0x94bc0` (with 1: engine error "CG_FreeLocalEntity: not active").
- Result (Trainer, D3D9, 60 s, frames saved with `[native] shot11=<present>`): no crash and no engine error, both views every frame (161 fps).
  **Player 1's top view is still wrong** (black / white) and the skinned models still have long triangles, so the per-player slot was not the cause of those.
  Player 2's effects: the refdef carries local player 0, so `if (lc == refdef.lc)` skips his effects update now (to check).
- Not read yet: the 360's back end frame (the PC `0x45690` -> `0x5c620` / `0x5c240`), which decides how each scene's 3D pass finds its own sun, shadow and
  light state. Next step.

## Why the evening runs crashed: the scene's 2D command list was never finished (2026-11-14, later)

`scr/pc_f1.txt` and its neighbours end the same way: `CRASH: access violation (read 0x10) at rva 0x645bd`, 21-30 s in, right after "split view on". The fault
is in the per-scene 2D command loop (function `0x642e0`, entered from the dispatcher `0x5e620` at `0x5e801` when the command word's type is not 9). It reads
`RBX = *(RSI + 0x20)` (the command's stream context) and then `*(RBX + 0x10)` with `RBX` = 0. `RBX` is the *last `SetStreamSource` target* of the context
(`state + 0x58`) and it is NULL when a `DrawIndexedPrimitive` comes with no stream bound: the command still claims vertices, and the loop crashes fetching them.

Why no stream is bound: the renderer's command list is drawn by **two different functions, chosen by the previous command's type** (`CMP EDI/R12D,9`):
- type 9 (the 3D scene) -> `0x5dde0` (or `0x5da10`), **rewritten** by the native renderer (`ngf_draw_*`);
- type 0..8 (the 2D layer: HUD, console, debug draw, which run **after** every 3D scene of a frame, `0x5e620`'s non-9 branch) -> `0x642e0` / `0x64980`, **not
  rewritten**, so its `SetStreamSource` / `SetIndices` / `DrawIndexedPrimitive` still go through the device pointer at `0x410fd20`, which the native renderer
  leaves empty (`patch_sites()` bridges them to native steps, `w_4f380` acknowledges the sampler states). The `w_4f380` stub is what kept the crash from being
  the first pointer: the stream itself is fine, the vertices are not.

What changed in mode 11 that exposed it:
1. `exp11frames=1` (the last uncommitted change): with one renderer frame per player, `0x453c0` runs **once per player** and resets the 2D state (`0x4e390`)
   and the stream cache twice; player 2's `SetStreamSource` was in the *other* frame's context.
2. `exp11lc=1`: player 2's `R_RenderScene` runs with the renderer's local-player global (`0x51deb8`) at 1 while the dispatcher and the command loops run in
   the client frame; the 3D was ported with that number, the 2D was not.

Before touching any of the two: the 2D command loop has to run without the game's Direct3D 9 device whether the frame is split or not, because the native
renderer has no device at all. Check to run first (D3D11, `experiment=10`, one player view): if the console (`MW2COOP_NATCON=1`) or any 2D debug draw after the
3D scene is drawn at all in a long run, the 2D path is fine and the crash is mode 11's frame split. If it crashes the same way, the 2D draw loop is the bug.

## Checked in the game (2026-11-14 late, 64-bit, D:\coop-test\mw2-64, 1280x720)

- **The committed mode 11 does not crash the way the evening's uncommitted change did.** With `exp11frames` stashed (HEAD `d6e43e9`), `[native] experiment=11`,
  Trainer: two runs of 90 s and 300 s with no exception, "scenes 2 views 2" every frame, both players stepping. So the crash in `scr/pc_f1.txt` and its neighbours
  (rva `0x645bd`, 21-30 s) comes from the last uncommitted change (one renderer frame per player), not from what is committed.
- **The 2D command list of a frame never reaches the engine function in either mode on the native renderer.** With `[native] tracescene=1` (a QA trace of
  `ngfr_453c0` and of `native_final2d_entry`) neither mode 11 nor mode 10 logs a single line, while the split screen runs (mode 10 composes, mode 11 draws both
  views). So the renderer being copied by the native one is not entered the way this doc assumed: the functions of the Direct3D 9 back end that the native
  renderer replaced are not all on the live path, and the 2D the player sees goes somewhere this trace does not cover. Next: find the caller of the 3D view
  setup (`fn_484c0` / `fn_5c010`) and of the 2D dispatch table (rva `0x36f768`, no code reference to it) in the *running* process (a debugger breakpoint on
  `ngfr_453c0` and on `0x5c010` shows who calls them), instead of reading static references.
- Frame capture for QA of mode 11 on the game's own Direct3D 9: `[native] exp11raw=1;exp11rawat=<n>;exp11rawstep=<k>` saves `raw1-<n>.bmp` after player 1's draw.
  It uses `shot_save_now` (the device's back buffer) and does not write a file yet: the "not saved" reason is not logged for it.

## Which renderer the QA runs use (2026-11-15)

Every run in the 2026-11-14 late session and tonight went through `tools/qa_native.sh`, which writes `[native] experiment=<n>` and nothing else: no `[display] directx11=1`,
so the game's own **Direct3D 9** renderer is in use and `natboot` (the mod's rewritten back end) is not installed at all. Consequences:
- The traces added tonight to `ngfr_453c0` / `native_final2d_entry` are silent in these runs because those functions are never called, not because the 2D path is broken. That
  correction is the reason mode 11 "never reached the 2D list": the 2D list in a plain Direct3D 9 run is the engine's own, called by the engine's `0x453c0`, which the mod only
  hooks to get `scene2d_hook` (`0xca523`-style hook set in `native.c`), not to replace.
- Mode 10 (the compositor) and mode 11 in a plain Direct3D 9 run both show real, moving pictures (mode 10 composed both halves tonight, `scratch/native-last.png`), so the engine's
  own frame path is intact in both.
- To test the native renderers (the path where `patch_sites`, `ngf_*` and `exp11lc` matter), `MW2COOP_DISPLAY=directx11` must be added to the QA run: `qa_native.sh` has no switch
  for `[display]` yet, and the last session's "mode 11 on the native renderer" claims were about `[native] experiment=11` with `MW2COOP_NATIVE11=1` in the environment.

The next check is therefore: mode 11 with `directx11=1` (native), which is where player 1's top view was white/black and where the crash in `0x645bd` was found.

## Mode 11 on the native Direct3D 11 renderer, measured (2026-11-15, `MW2COOP_DISPLAY="directx11=1"`)

The QA runner now takes `MW2COOP_DISPLAY` (e.g. `directx11=1`) and writes the `[display]` section. With it, mode 11 in the Trainer:

- **No crash in 100 s** (the whole run, the copy was killed by its timeout). The uncommitted `exp11frames` variant is still the one that died, not this.
- **The back end is entered once per frame after the split starts** (`w_44d00`, `w_453c0`, `w_45060` move together, one per frame). So in mode 11 the engine does **not** run
  the two-view loop of `0xB12` any more: the `end_frame_hook(flags=1)` between the two players' draws ends the first player's frame, `0x453c0` runs, and then the second player's
  draw begins a frame of its own. Two whole renderer frames per engine frame, which is what `exp11frames` was reaching for and what the commit at `d6e43e9` does not do.
- **Player 1's view (top half) is black** in the raw capture taken between the two players' draws (`shots/raw1-300.bmp`): the black covers the top half exactly and the picture
  beneath it is the alpha-numeric overlay of that frame. Player 2's view (bottom) draws correctly. This is the "black/white top view" of the earlier notes, now with a picture of it.
- Before the level, the same run renders the mission's briefing film and text correctly on Direct3D 11 (`native11/scene-9000.bmp`: the AC-130 city map, the Russian subtitles),
  so the native renderer itself is sound; the fault is specific to the split.

Open question that the picture narrows: the top half is not a wrongly-lit view, it is a **cleared** region — the black is laid over the frame the 2D layer is drawn on, so the first
player's 3D pass, its post-process composite (`fn_484c0`) or the frame's clear writes black over exactly that rectangle. Next: print the view rectangle and the clear rectangle the
frame's 3D pass uses for each of the two scenes (the pass's clear is already given the whole target for the first view, `split_smc_redirect`'s neighbour `native_scene_clear`).

## The black top half: what is ruled out, with the logs (2026-11-15, native Direct3D 11, mode 11)

Measured in the game (`clearlog=1`, `rslog=1`, `exp11raw=1`, 1280x720, Trainer):

- **Both players' scenes are submitted**: `rslog after p1: lc 0 ... scenes 1 views 1`, then `rslog after p2: lc 1 second 1 ... scenes 2 views 2`. The per-player renderer
  slot (`exp11lc`) is in effect, and the second scene is in the frame.
- **The view rectangles are set as intended**: at the draw, `view rect 0.000 0.000 1.000 0.500 | cg view 0 0 1280 360 | picture 1280 720` (player 1's top half), and the cgame's
  rectangle is the same.
- **The clear the mod hooks (`0x5c484 -> 0x51070`) is not the 3D pass's clear.** Only 24 such clears happen in a run, all on the cgame draw thread, all with the same signature
  (`flags 0x7 rgba 0 0 0 0 depth 1`, `view rect 0 0 1 1`, `cg view 0 0 0 0`) and always with `scene3d_pass` 0 or -1 — i.e. before the split's rectangles are in place, and never
  from the front end's worker thread that runs the frame's 3D passes (`0x45690`/`0x456ee`, a data reference, so the call is indirect). The log in `php` from an earlier session
  ("the 3D pass, `0x5c484`") names the wrong call: `0x5c620`, the function the `p1`/`p2` logs come from, is the one whose pieces the 3D pass runs, and its clear is reached
  indirectly.
- **Where the black is written is therefore still open**, but the candidates are now narrow: the front end's own frame clear (the `real clear` of the scene target, inside the
  worker's pass), the post-process composite (`fn_484c0`), or the scene target's first switch by `reset_2d` (`ngfr_453c0`). The next instrument is inside `ngfr_453c0` and the
  worker pass: log every `nd_clear` (the native device's clear) with its target and rectangle for two frames of the split, and compare with `raw1-*.bmp`.
