// Address resolution by byte signatures (see addr.h). The format of the signature file is described in tools/addrsig.py.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "addr.h"

#ifndef ADDR_TEST
#include <windows.h>
#include "log.h"
#endif

#define KIND_CODE 0
#define KIND_DATA 1
#define STRONG_FIXED 40    // fixed bytes of one code signature that are enough on their own
#define DERIVE_REACH 0x1000000 // a derived global lies this close to the resolved ones on both sides
#define MAX_CAND 4

typedef struct {
	int kind;      // KIND_CODE / KIND_DATA
	int derive;    // no signature: derive from neighbours
	int rip;       // DATA: rip-relative operand (else absolute)
	uint32_t rva;
	int off, cap, tail, delta;
	int len;
	uint8_t *bytes, *fixed; // the pattern; fixed[i] == 0 is a wildcard
	int nfixed;
} Sig;

typedef struct {
	uint32_t rva;
	int kind;
	struct {
		uint32_t cand;
		int votes, maxfixed;
	} c[MAX_CAND];
	int ncand;
	int has_code_sig;
	uint32_t result;
	int ok;
} Entry;

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int parse_int_opt(const char *line, const char *key, int def)
{
	const char *p = strstr(line, key);
	return p ? atoi(p + strlen(key)) : def;
}

// Parses one line; returns 1 if it holds a signature or a derive entry (appended to *sigs).
static int parse_line(char *line, Sig **sigs, int *n, int *cap)
{
	Sig s;
	memset(&s, 0, sizeof(s));
	while (*line == ' ' || *line == '\t')
		line++;
	if (!*line || *line == '#')
		return 0;
	if (strncmp(line, "CODE ", 5) == 0)
		s.kind = KIND_CODE, line += 5;
	else if (strncmp(line, "DATA ", 5) == 0)
		s.kind = KIND_DATA, line += 5;
	else
		return 0;
	while (*line == ' ')
		line++;
	s.rva = (uint32_t)strtoul(line, &line, 16);
	if (strstr(line, "derive") && !strstr(line, "sig=")) {
		s.derive = 1;
	} else {
		char *sig = strstr(line, "sig=");
		if (!sig)
			return 0;
		*sig = 0;
		s.off = parse_int_opt(line, "off=", 0);
		s.cap = parse_int_opt(line, "cap=", 0);
		s.tail = parse_int_opt(line, "tail=", 0);
		s.delta = parse_int_opt(line, "delta=", 0);
		s.rip = strstr(line, " rip") != NULL;
		sig += 4;
		int count = 0;
		for (char *p = sig; *p; p++)
			if (*p == ' ')
				count++;
		s.bytes = malloc((size_t)count + 2);
		s.fixed = malloc((size_t)count + 2);
		if (!s.bytes || !s.fixed)
			return 0;
		for (char *p = sig; *p;) {
			while (*p == ' ')
				p++;
			if (!*p)
				break;
			if (p[0] == '?') {
				s.bytes[s.len] = 0;
				s.fixed[s.len++] = 0;
				p += p[1] == '?' ? 2 : 1;
			} else {
				int hi = hexval(p[0]), lo = hexval(p[1]);
				if (hi < 0 || lo < 0)
					break;
				s.bytes[s.len] = (uint8_t)(hi * 16 + lo);
				s.fixed[s.len++] = 1;
				s.nfixed++;
				p += 2;
			}
		}
		if (s.len < 4)
			return 0;
	}
	if (*n == *cap) {
		*cap = *cap ? *cap * 2 : 1024;
		*sigs = realloc(*sigs, (size_t)*cap * sizeof(Sig));
		if (!*sigs)
			return 0;
	}
	(*sigs)[(*n)++] = s;
	return 1;
}

// ---------------------------------------------------------------------------------------------------------------------------- the index of the code

typedef struct {
	const AddrSection *secs;
	int nsecs;
	uint32_t *start; // the flat position at which each section starts
	uint32_t total;
	const uint8_t **flat_sec;
	int32_t *head;
	int32_t *next;
	uint8_t *flat;
} Index;

#define HASH_BITS 20
static uint32_t hash4(const uint8_t *p)
{
	uint32_t x = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
	return (x * 2654435761u) >> (32 - HASH_BITS);
}

static int index_build(Index *ix, const AddrSection *secs, int nsecs)
{
	memset(ix, 0, sizeof(*ix));
	ix->secs = secs;
	ix->nsecs = nsecs;
	ix->start = calloc((size_t)nsecs + 1, sizeof(uint32_t));
	for (int i = 0; i < nsecs; i++) {
		ix->start[i] = ix->total;
		ix->total += secs[i].size + 8; // a gap so that no window runs from one section into the next
	}
	ix->start[nsecs] = ix->total;
	ix->flat = calloc(ix->total + 8, 1);
	ix->head = malloc(((size_t)1 << HASH_BITS) * sizeof(int32_t));
	ix->next = malloc((size_t)ix->total * sizeof(int32_t));
	if (!ix->start || !ix->flat || !ix->head || !ix->next)
		return 0;
	memset(ix->head, 0xff, ((size_t)1 << HASH_BITS) * sizeof(int32_t));
	for (int i = 0; i < nsecs; i++) {
		memcpy(ix->flat + ix->start[i], secs[i].data, secs[i].size);
		for (uint32_t k = 0; k + 4 <= secs[i].size; k++) {
			uint32_t pos = ix->start[i] + k;
			uint32_t h = hash4(ix->flat + pos);
			ix->next[pos] = ix->head[h];
			ix->head[h] = (int32_t)pos;
		}
	}
	return 1;
}

static void index_free(Index *ix)
{
	free(ix->start);
	free(ix->flat);
	free(ix->head);
	free(ix->next);
}

static int section_of(const Index *ix, uint32_t pos)
{
	int lo = 0, hi = ix->nsecs - 1;
	while (lo < hi) {
		int mid = (lo + hi + 1) / 2;
		if (ix->start[mid] <= pos)
			lo = mid;
		else
			hi = mid - 1;
	}
	return lo;
}

// The matches of a signature (up to 2): flat positions of its first byte. Returns how many.
static int index_find(const Index *ix, const Sig *s, uint32_t hits[2])
{
	// the anchor: the four fixed bytes in a row whose hash chain is shortest
	int best_at = -1;
	int best_len = 1 << 30;
	for (int i = 0; i + 4 <= s->len; i++) {
		if (!(s->fixed[i] && s->fixed[i + 1] && s->fixed[i + 2] && s->fixed[i + 3]))
			continue;
		int len = 0;
		for (int32_t p = ix->head[hash4(s->bytes + i)]; p >= 0 && len < best_len; p = ix->next[p])
			len++;
		if (len < best_len) {
			best_len = len;
			best_at = i;
		}
	}
	if (best_at < 0)
		return 0;
	int n = 0;
	for (int32_t p = ix->head[hash4(s->bytes + best_at)]; p >= 0; p = ix->next[p]) {
		int64_t start = (int64_t)p - best_at;
		if (start < 0 || start + s->len > ix->total)
			continue;
		int sec = section_of(ix, (uint32_t)p);
		if (start < (int64_t)ix->start[sec] || start + s->len > (int64_t)ix->start[sec] + ix->secs[sec].size)
			continue;
		const uint8_t *t = ix->flat + start;
		int ok = 1;
		for (int k = 0; k < s->len; k++)
			if (s->fixed[k] && t[k] != s->bytes[k]) {
				ok = 0;
				break;
			}
		if (!ok)
			continue;
		if (n < 2)
			hits[n] = (uint32_t)start;
		if (++n >= 2)
			break;
	}
	return n;
}

// ---------------------------------------------------------------------------------------------------------------------------- the resolver

static int by_rva(const void *a, const void *b)
{
	uint32_t x = ((const Entry *)a)->rva, y = ((const Entry *)b)->rva;
	return x < y ? -1 : x > y;
}

int addr_resolve(const char *sigs_text, const AddrSection *secs, int nsecs, uint64_t image_base, AddrResult *res)
{
	memset(res, 0, sizeof(*res));
	size_t text_len = strlen(sigs_text);
	char *copy = malloc(text_len + 1);
	if (!copy)
		return 0;
	memcpy(copy, sigs_text, text_len + 1);
	Sig *sigs = NULL;
	int nsig = 0, cap = 0;
	for (char *line = strtok(copy, "\n"); line; line = strtok(NULL, "\n")) {
		size_t l = strlen(line);
		while (l && (line[l - 1] == '\r' || line[l - 1] == ' '))
			line[--l] = 0;
		parse_line(line, &sigs, &nsig, &cap);
	}
	free(copy);

	// one Entry per address
	Entry *ent = calloc((size_t)nsig + 1, sizeof(Entry));
	int nent = 0;
	for (int i = 0; i < nsig; i++) {
		int e;
		for (e = 0; e < nent; e++)
			if (ent[e].rva == sigs[i].rva)
				break;
		if (e == nent) {
			ent[nent].rva = sigs[i].rva;
			ent[nent].kind = sigs[i].kind;
			nent++;
		}
		if (sigs[i].kind == KIND_CODE && !sigs[i].derive)
			ent[e].has_code_sig = 1;
	}
	qsort(ent, (size_t)nent, sizeof(Entry), by_rva);

	Index ix;
	int built = index_build(&ix, secs, nsecs);
	for (int i = 0; built && i < nsig; i++) {
		const Sig *s = &sigs[i];
		if (s->derive)
			continue;
		uint32_t hits[2];
		if (index_find(&ix, s, hits) != 1)
			continue;
		int sec = section_of(&ix, hits[0]);
		uint32_t at = hits[0] - ix.start[sec]; // offset in the section
		uint32_t rva_at = secs[sec].rva + at;
		uint32_t cand;
		if (s->kind == KIND_CODE) {
			cand = rva_at + (uint32_t)s->off;
		} else if (!s->rip) {
			uint32_t v;
			memcpy(&v, ix.flat + hits[0] + s->cap, 4);
			cand = (uint32_t)(v - (uint32_t)image_base);
		} else {
			int32_t disp;
			memcpy(&disp, ix.flat + hits[0] + s->cap, 4);
			cand = rva_at + (uint32_t)s->cap + 4 + (uint32_t)s->tail + (uint32_t)disp;
		}
		cand -= (uint32_t)s->delta; // the signature's code reached the global through a field s->delta bytes into it
		Entry key = {s->rva, 0, {{0, 0, 0}}, 0, 0, 0, 0};
		Entry *e = bsearch(&key, ent, (size_t)nent, sizeof(Entry), by_rva);
		int c;
		for (c = 0; c < e->ncand; c++)
			if (e->c[c].cand == cand)
				break;
		if (c == e->ncand) {
			if (c >= MAX_CAND)
				continue;
			e->c[c].cand = cand;
			e->ncand++;
		}
		e->c[c].votes++;
		if (s->nfixed > e->c[c].maxfixed)
			e->c[c].maxfixed = s->nfixed;
	}
	// accept: two signatures agree, or one long code signature; and nothing else has as many votes
	for (int i = 0; i < nent; i++) {
		Entry *e = &ent[i];
		int top = -1;
		for (int c = 0; c < e->ncand; c++)
			if (top < 0 || e->c[c].votes > e->c[top].votes || (e->c[c].votes == e->c[top].votes && e->c[c].maxfixed > e->c[top].maxfixed))
				top = c;
		if (top < 0)
			continue;
		int strong = e->c[top].votes >= 2 || (e->kind == KIND_CODE && e->c[top].maxfixed >= STRONG_FIXED);
		int tie = 0;
		for (int c = 0; c < e->ncand; c++)
			if (c != top && e->c[c].votes >= e->c[top].votes)
				tie = 1;
		if (strong && !tie) {
			e->ok = 1;
			e->result = e->c[top].cand;
		}
	}
	// a global that no signature found: the globals around it moved as one block, so it moved by the same amount as the two nearest
	// resolved ones below and the two nearest above it, if all of those agree
	for (int i = 0; i < nent; i++) {
		Entry *e = &ent[i];
		if (e->ok || e->kind != KIND_DATA)
			continue;
		int bl[2] = {-1, -1}, ab[2] = {-1, -1}; // nearest first
		for (int j = 0; j < nent; j++) {
			if (ent[j].ok != 1 || ent[j].kind != KIND_DATA)
				continue;
			if (ent[j].rva < e->rva && e->rva - ent[j].rva <= DERIVE_REACH) {
				if (bl[0] < 0 || ent[j].rva > ent[bl[0]].rva) {
					bl[1] = bl[0];
					bl[0] = j;
				} else if (bl[1] < 0 || ent[j].rva > ent[bl[1]].rva)
					bl[1] = j;
			}
			if (ent[j].rva > e->rva && ent[j].rva - e->rva <= DERIVE_REACH) {
				if (ab[0] < 0 || ent[j].rva < ent[ab[0]].rva) {
					ab[1] = ab[0];
					ab[0] = j;
				} else if (ab[1] < 0 || ent[j].rva < ent[ab[1]].rva)
					ab[1] = j;
			}
		}
		if (bl[0] < 0 || ab[0] < 0)
			continue;
		int32_t delta = (int32_t)(ent[bl[0]].result - ent[bl[0]].rva);
		int same = 1;
		int others[3] = {bl[1], ab[0], ab[1]};
		for (int k = 0; k < 3; k++)
			if (others[k] >= 0 && (int32_t)(ent[others[k]].result - ent[others[k]].rva) != delta)
				same = 0;
		if (same) {
			e->ok = 2;
			e->result = e->rva + (uint32_t)delta;
		}
	}
	index_free(&ix);

	AddrPair *out = malloc(((size_t)nent + 1) * sizeof(AddrPair));
	uint32_t *miss = malloc(((size_t)nent + 1) * sizeof(uint32_t));
	int nout = 0, nmiss = 0;
	for (int i = 0; i < nent; i++) {
		if (ent[i].ok) {
			out[nout].ref = ent[i].rva;
			out[nout].resolved = ent[i].result;
			nout++;
		} else {
			miss[nmiss++] = ent[i].rva;
		}
	}
	for (int i = 0; i < nent; i++) {
		if (ent[i].kind == KIND_CODE) {
			res->code_total++;
			res->code_found += ent[i].ok != 0;
		} else {
			res->data_total++;
			res->data_found += ent[i].ok != 0;
		}
	}
	res->pairs = out;
	res->n = nout;
	res->missing = miss;
	res->nmissing = nmiss;
	for (int i = 0; i < nsig; i++) {
		free(sigs[i].bytes);
		free(sigs[i].fixed);
	}
	free(sigs);
	free(ent);
	return nout;
}

void addr_result_free(AddrResult *r)
{
	free(r->pairs);
	free(r->missing);
	memset(r, 0, sizeof(*r));
}

#ifndef ADDR_TEST
// ---------------------------------------------------------------------------------------------------------------------------- the running game

static int mode;
static int layout_same;
static AddrPair *table;
static int table_n;

int addr_mode(void)
{
	return mode;
}

int addr_layout_same(void)
{
	return layout_same;
}

// What addr_xr() hands out for an address that was not found in a build whose addresses moved: a place in a private block of memory instead of 0 (which would be the exe's own
// header). A hook compares the bytes it is about to patch and finds the wrong ones; a read gives zeros; a write lands in the block; and a call returns at once (the code block
// is all `ret`). So a feature whose addresses are missing does nothing, and nothing that is not missing is touched. Which kind an address is, the signature file says
// ("# code <rva> <rva>": the reference build's code); an address the file does not list is code if it lies in those ranges.
#define SCRATCH_SIZE 0x10000
static struct {
	unsigned lo, hi;
} ref_code[8];
static int ref_code_n;
static uint8_t *scratch[2]; // [0] data (zeros), [1] code (ret)
static int scratch_tried;
static int scratch_hits;

static int ref_is_code(unsigned ref)
{
	for (int i = 0; i < ref_code_n; i++)
		if (ref >= ref_code[i].lo && ref < ref_code[i].hi)
			return 1;
	return 0;
}

static void scratch_make(void)
{
	scratch_tried = 1;
	const uint8_t *base = (const uint8_t *)GetModuleHandleW(NULL);
	for (int k = 0; k < 2; k++) {
		uint8_t *p = NULL;
#ifdef _WIN64
		// the RVA must fit in 32 bits: a block within 4 GB above the image
		for (uintptr_t at = (uintptr_t)base + 0x20000000u; !p && at < (uintptr_t)base + 0xf0000000u; at += 0x01000000u)
			p = VirtualAlloc((void *)at, SCRATCH_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
		p = VirtualAlloc(NULL, SCRATCH_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#endif
		if (p && k == 1)
			memset(p, 0xc3, SCRATCH_SIZE);
		scratch[k] = p;
	}
}

static unsigned scratch_rva(unsigned ref)
{
	if (!scratch_tried)
		scratch_make();
	int code = ref_is_code(ref);
	const uint8_t *base = (const uint8_t *)GetModuleHandleW(NULL);
	if (!scratch[code])
		return 0;
	static unsigned logged[64];
	int seen = 0;
	for (int i = 0; i < scratch_hits && i < 64; i++)
		if (logged[i] == ref)
			seen = 1;
	if (!seen && scratch_hits < 64) {
		logged[scratch_hits++] = ref;
		log_printf("addr: reference rva 0x%x has no address in this build: a harmless %s block is used for it", ref, code ? "code (ret)" : "data (zeros)");
	}
	return (unsigned)(scratch[code] + 0x100 - base); // (a little in: an offset below the address stays inside the block)
}

#ifndef _WIN64
unsigned addr159(unsigned rva); // addr159.c: the official 32-bit RVA -> the RVA in the iw4x-sp client's 1.0.159 exe
void addr_use_159(void)
{
	mode = 2;
}
#endif

unsigned addr_xr(unsigned ref)
{
#ifndef _WIN64
	if (mode == 2)
		return ref ? addr159(ref) : 0;
#endif
	if (!mode || !ref)
		return ref;
	int lo = 0, hi = table_n - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (table[mid].ref == ref)
			return table[mid].resolved;
		if (table[mid].ref < ref)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return scratch_rva(ref);
}

int addr_resolved(unsigned ref)
{
#ifndef _WIN64
	if (mode == 2)
		return ref && addr159(ref) != 0;
#endif
	if (!mode || !ref)
		return 1;
	int lo = 0, hi = table_n - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (table[mid].ref == ref)
			return 1;
		if (table[mid].ref < ref)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return 0;
}

int addr_resolve_running(int *total)
{
	char path[MAX_PATH];
	DWORD length = GetModuleFileNameA(NULL, path, sizeof(path));
	char *slash = length && length < sizeof(path) ? strrchr(path, '\\') : NULL;
	if (!slash)
		return 0;
	slash[1] = 0;
#ifdef _WIN64
	strncat(path, "mw2coop\\addresses-x64.txt", sizeof(path) - strlen(path) - 1);
#else
	strncat(path, "mw2coop\\addresses-x86.txt", sizeof(path) - strlen(path) - 1);
#endif
	HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		log_printf("addr: no signature file %s: the addresses of this build cannot be looked up", path);
		return 0;
	}
	DWORD size = GetFileSize(f, NULL), got = 0;
	char *text = size && size < (64u << 20) ? malloc(size + 1) : NULL;
	if (!text || !ReadFile(f, text, size, &got, NULL)) {
		CloseHandle(f);
		free(text);
		return 0;
	}
	CloseHandle(f);
	text[got] = 0;

	const uint8_t *base = (const uint8_t *)GetModuleHandleW(NULL);
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
	const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
	const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
	AddrSection secs[16];
	int nsecs = 0;
	for (int i = 0; i < nt->FileHeader.NumberOfSections && nsecs < 16; i++) {
		if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
			continue;
		secs[nsecs].data = base + sec[i].VirtualAddress;
		secs[nsecs].rva = sec[i].VirtualAddress;
		secs[nsecs].size = sec[i].Misc.VirtualSize;
		nsecs++;
	}
	ref_code_n = 0;
	for (const char *l = text; (l = strstr(l, "\n# code 0x")) && ref_code_n < 8;) {
		unsigned lo = 0, hi = 0;
		if (sscanf(l + 1, "# code 0x%x 0x%x", &lo, &hi) == 2) {
			ref_code[ref_code_n].lo = lo;
			ref_code[ref_code_n].hi = hi;
			ref_code_n++;
		}
		l += 2;
	}
	DWORD t0 = GetTickCount();
	AddrResult res;
	int n = addr_resolve(text, secs, nsecs, (uint64_t)(uintptr_t)base, &res);
	free(text);
	*total = n + res.nmissing;
	log_printf("addr: %d of %d addresses of this build found by their signatures in %lu ms (%s)", n, *total, (unsigned long)(GetTickCount() - t0), path);
	log_printf("addr: code %d of %d, data %d of %d", res.code_found, res.code_total, res.data_found, res.data_total);
	for (int i = 0; i < res.nmissing && i < 40; i++)
		log_printf("addr: not found: reference rva 0x%x", (unsigned)res.missing[i]);
	int identical = 0;
	for (int i = 0; i < res.n; i++)
		if (res.pairs[i].ref == res.pairs[i].resolved)
			identical++;
	layout_same = identical == n && res.code_total >= 40 && res.code_found * 10 >= res.code_total * 9 &&
		      res.data_total > 0 && res.data_found * 2 >= res.data_total;
	if (layout_same)
		log_printf("addr: every one of the %d addresses found is the reference build's own: this build has its layout (the code differs in places)", n);
	free(table);
	table = res.pairs;
	table_n = res.n;
	res.pairs = NULL;
	mode = (layout_same && !getenv("MW2COOP_FORCE_RESOLVED")) ? 0 : 1; // the same layout: the addresses are used as they are (QA: MW2COOP_FORCE_RESOLVED=1 runs the table lookups on the known layout)
	addr_result_free(&res);
	return n;
}
#endif
