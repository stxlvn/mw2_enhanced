# Finding the game's addresses by signature

The mod patches `iw4sp.exe` at addresses it learned from one build of the game (the *reference build*: the official 32-bit Steam build, the
64-bit Steam build). A build with other addresses used to mean "unknown build: nothing is patched". This page is about how the mod now looks
the addresses up in a build it has not seen: by byte signatures of the code around them.

## What is there

| Piece | Where | What it does |
|---|---|---|
| Signature files | `mw2coop/addresses-x86.txt`, `addresses-x64.txt` (`content/release/`) | for every address the mod uses, byte patterns of the code around it, `??` for bytes that depend on where things are |
| Generator | `tools/addrsig.py gen` | makes a signature file from the reference build (an image dump or the exe) |
| Python check | `tools/addrsig.py verify` | resolves a signature file against another build, optionally against a table of known answers |
| Resolver | `src/addr.c`, `src/addr.h` | the same rules in C, run by the mod at start on a build it does not know; a hash index of the code makes a full run take 0.1 to 0.4 s |
| Test program | `tools/test_addr.c`, `tools/test_addr.sh` | runs the resolver on a game exe file |

Known builds are not affected: the resolver runs only when the code hash (32-bit) or the PE timestamp (64-bit) is not a known one.

## The signature file

One signature per line (the header of `tools/addrsig.py` has the details):

```
CODE 0x00cf10 off=16 sig=8B 44 24 04 ?? ?? ?? ?? 56 ...     code point at reference rva 0xcf10, 16 bytes into the pattern
DATA 0x33ba88 abs cap=4 sig=A1 ?? ?? ?? ?? ...              a global: the 4 bytes at offset 4 are its absolute address
DATA 0x1b21e90 rip cap=3 tail=0 sig=48 8D 0D ?? ?? ?? ?? ... a global: a rip-relative displacement at offset 3 (64-bit)
DATA 0x2e9fd0 abs cap=8 delta=4 sig=...                      the code reaches the global through a field 4 bytes into it
DATA 0x331aec derive                                         no signature: taken from its neighbours
```

A signature is made only if it matches exactly one place in the reference build. Several signatures (up to 24 for code, 16 for data) are made per
address from windows of different size and position around it.

## The rules (the Python check and the C resolver are the same)

1. A signature counts if it matches exactly **one** place in the executable sections of the running build.
2. An address is accepted if two signatures agree on it, or, for a code address, one signature with 40 or more fixed bytes; and no other
   candidate has as many votes. Anything else stays unresolved. **The mod must not use an unresolved address.**
3. A global that no signature found is derived from the two nearest accepted globals below it and the two nearest above it (within 16 MB), if all of them moved by the same amount.
   On the 1.0.159 build this added 3 right answers and no wrong one; on the 64-bit build it takes 74 of 120 to 115.
4. The window for finding a global through the code that uses a field of it is limited to -0x20 .. +0x100 bytes. A wider window found 3 wrong
   addresses in a test (the code referenced another entry of the same table in the other build), so it is not used.

## Same layout

If every address found is the reference address itself, at least 90% of the code addresses (and 40) and at least half of the data addresses
were found, the build has the reference build's layout: the same addresses with some bytes changed. The mod then runs with the known addresses
(`addr_layout_same()`); each hook still checks the bytes it patches and refuses if they differ.

## Measured

| Test | Result |
|---|---|
| 32-bit reference (official) -> the 1.0.159 exe of the iw4x client; the 241 addresses with known answers (`src/addr159.c`) | **215 right, 0 wrong**, 26 not resolved |
| 2026-11-10, the signature file after the guards, climbads, gfxopts and level-start hooks were added (312 addresses), offline on four exes | official image: 305 found, all identity (the 7 not found are the builtin entries, which `game_builtin_entry` finds by name, and 0x29116c); the second official-layout exe: 304 of 312; the iw4x-sp 1.0.159 exe: 272 of 312, **219 right, 0 wrong** against the answers known by hand; the COD-MOD exe (3514008 bytes): 272 of 312 (no known answers to compare) |
| 2026-11-11, in the game: a 64-bit pair (Trainer `bay`, then the Endgame boat in the real briefing mode) with `MW2COOP_FORCE_UNKNOWN=1 MW2COOP_MOVED=1 MW2COOP_FORCE_RESOLVED=1` (the resolver's tables in use, 14 addresses on the zero block) | no crash on either copy, `bay` PASS 5 on both; rumble builtins found by name (3 of 6), the briefing wait switched itself off (film flags not found), the level started 4 s after loading |
| the same in C (`test_addr.exe`) | identical to the Python result, 0.26 s |
| 32-bit reference -> a second official-layout exe (D:\ install of the owner, 33 patched places, 174 differing bytes; the layout is identical, so every answer must be identity) | **235 of 241 found, all identity**; in the game the mod recognised the layout, installed all its modules and started split screen with the same hooks as on the official exe |
| 64-bit reference -> itself, inside the running game (forced) | 115 of 120 found in 110 ms (code 55 of 55, data 60 of 65), all identity; this is the "same layout" case, so a 64-bit exe with the reference layout now runs |

## A build whose addresses moved (experimental)

`[native] movedbuilds=1` (or `MW2COOP_MOVED=1`) lets the mod, 32-bit or 64-bit, run on an exe whose addresses it had to look up (85% or more found). The code asks `addr_xr(reference rva)`:
the same number on a known build, the resolved one on this build, and for an address that was not found the offset of a private block (a block of `ret` bytes for code, zeros for
data; `# code <rva> <rva>` lines of the signature file give the reference build's code ranges) so a hook sees the wrong bytes and refuses, a read gives zeros and a call returns.
The player-method table of mirror.c is found by the method names instead of by address. Tried on the iw4x-sp 1.0.159 exe: 213 of 241 found, no crash, most hooks refused (that exe's
code differs from the official one: the iw4x profile build is for it). The 64-bit code goes through `XR()` the same way since 2026-11-10 (the `RVA_` macros, every `base + XR(0x...)`, the tables of ranges, call sites and hook sites; 295 addresses in
`addresses-x64.txt`, 281 of them found with signatures on the reference exe, all themselves; the other 14 are renderer and dvar globals that no signature reaches, the neighbour
rule may place them). Not covered: diagnostics that are off by default (the `scenescan`, `npcsnap` and other test switches read raw addresses) and `crash.c`'s check of one place of
the known build. **The 64-bit moved-build path has only been proven on the reference exe** (every address resolves to itself; the compiled code of the 64-bit DLL was byte-identical
before `XR()` became the resolver): no second 64-bit build exists here to run it on.

## What is not done

* A 64-bit build whose addresses moved runs only with `movedbuilds=1` and only if 85% of the addresses are found; it has never been run (no such build to try). A 64-bit
  build with the reference layout runs as before.
* A 32-bit build whose addresses moved is not run yet either. The native code already goes through `XR()` / `XV()` (the iw4x-sp profile maps them
  through `addr159()`), but the generic hooks use `RVA_` macros and inline addresses (scrdebug32.c, climbads.c), and every consumer has to survive an
  address that stays unresolved (`addr_xr()` gives 0). Routing half of them would be worse than none: a moved build runs only when all of them are routed.
* 6 globals of the 32-bit build (the rumble script function table and three others) are never found: no code reaches them near enough.

## Regenerating

```sh
python -I tools/addrsig.py gen x86 <image32.bin or iw4sp.exe> content/release/addresses-x86.txt
python -I tools/addrsig.py gen x64 <64-bit iw4sp.exe>        content/release/addresses-x64.txt
python -I tools/addrsig.py verify content/release/addresses-x86.txt <other build> [--truth table]
sh tools/test_addr.sh && build/test_addr.exe content/release/addresses-x86.txt <other build>
```

The 32-bit reference is a memory dump of the official build (`mw2coop\dump_image`, docs/32bit-plan.md); the 64-bit generator needs
`llvm-objdump` for instruction boundaries. QA: `MW2COOP_FORCE_UNKNOWN=1` takes the unknown-build path on a known build.
