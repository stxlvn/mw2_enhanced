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
