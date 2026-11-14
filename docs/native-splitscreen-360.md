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
