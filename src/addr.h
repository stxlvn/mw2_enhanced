#pragma once
// Finding the game's addresses by byte signatures, for a build of iw4sp.exe the mod was not written against (docs/address-resolution.md).
//
// The signature file (mw2coop\addresses-x86.txt / addresses-x64.txt, made by tools/addrsig.py from the reference build) lists, for each address
// the mod uses (an RVA of the reference build), byte patterns of the code around it. addr_resolve() searches the running build's code for them;
// an address is accepted only when two signatures agree (or one long code signature does), or, for a global that no signature found, when the
// nearest resolved globals on both sides moved by the same amount. What is not accepted stays unresolved; the mod must not use it.
#include <stdint.h>

typedef struct {
	const uint8_t *data; // the section's bytes as they are in memory
	uint32_t rva;        // its RVA
	uint32_t size;
} AddrSection;

typedef struct {
	uint32_t ref;      // an RVA of the reference build
	uint32_t resolved; // the same place in the running build
} AddrPair;

// The core, free of the game: <sigs> is the text of a signature file, <secs> the executable sections of the build, <image_base> the VA that
// absolute 32-bit operands of that build are relative to. Returns the number of addresses accepted; *out holds them (sorted by ref) and the
// ones that were not accepted. Free it with addr_result_free().
typedef struct {
	AddrPair *pairs;
	int n;
	uint32_t *missing;
	int nmissing;
	int code_total, code_found, data_total, data_found; // by kind, over all the addresses of the file
} AddrResult;

int addr_resolve(const char *sigs, const AddrSection *secs, int nsecs, uint64_t image_base, AddrResult *out);
void addr_result_free(AddrResult *r);

// The running game: 0 as long as the build is the reference one (addresses are used as they are). addr_resolve_running() reads the signature file
// next to the game, resolves, switches the mode to 1 and returns the number accepted (*total: of how many); the log lists what was not accepted.
// 1 if the last addr_resolve_running() found the build to have the reference build's layout: every address found is the reference address itself,
// at least 90% of the code addresses (and 40 of them) and at least half of the data addresses were found. The unresolved ones are then taken to be the reference ones too; the code of the build may
// still differ in places, and each hook verifies the bytes it patches.
int addr_layout_same(void);
int addr_mode(void);
int addr_resolve_running(int *total);

// An address of the reference build, translated for the running build: the same number in mode 0, the resolved one in mode 1, 0 if it has none.
unsigned addr_xr(unsigned ref);
// 1 when addr_xr(ref) is a real address of the running build (always on the known build), 0 when the address was not found and a harmless block stands in for it: code that
// reads state through such an address must not wait for or act on it.
int addr_resolved(unsigned ref);
// 32-bit: the running build is the iw4x-sp client's known 1.0.159 exe: addr_xr() reads the table of addr159.c from now on (mode 2).
void addr_use_159(void);
