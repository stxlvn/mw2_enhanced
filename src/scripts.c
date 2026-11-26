// GSC loading: Scr_AddSourceBuffer is replaced so that mw2coop can
//   - add or fully replace a script:  <game dir>\mw2coop\scripts\<name>   (our own code)
//   - patch a game script:            <game dir>\mw2coop\patches\<name>.patch
// Patches are applied to the zone's own text at load time, so the mod never ships game code.
//
// Patch format (text; '\r' is ignored everywhere):
//   @@@ replace          exact text that must occur exactly once in the script
//   ...
//   @@@ with             what to put there instead
//   ...
//   @@@ end
//   @@@ append           text added at the end of the script (new functions)
//   ...
//   @@@ end
//   @@@ subst <function> text replaced at every whole-word occurrence inside one function
//   ...                  of the script (from its header to its closing brace); at least one
//   @@@ to               occurrence must exist. Large reworks (say, level.player -> self in a
//   ...                  function) need no game code in the patch this way.
//   @@@ end
// Lines outside blocks are comments.
//
// The mod's scripts and patches are for a co-op session only (the dvar "coop" is 1: split screen or network, campaign or Spec Ops). In single player (one player: the campaign or a
// Spec Ops mission played alone) no co-op script of ours is loaded and no co-op patch is applied: the game runs its own scripts as shipped. The dvar is read at every script load, so a
// session of one after a session of two (and the other way round) gets the right text.
// The exception is the fixes of the game's own bugs that have nothing to do with a second player (the Museum civilians): those are patch files named <name>.fix.patch and scripts
// named maps\_mw2fix*.gsc, and they are loaded in every session, single player too. Nothing in them may refer to maps\_mw2coop*.
//
// Co-op runs in lockstep (both PCs run the level scripts), so every machine must end up with
// the same text. The log prints an FNV-1a hash of each changed script to compare host/client.
//
// Original (rva 0x217950), fastfile-only in this build:
//   raw = DB_FindXAssetHeader(ASSET_TYPE_RAWFILE, ext);
//   if (DB_IsXAssetDefault(ASSET_TYPE_RAWFILE, ext)) return NULL;
//   len = DB_GetRawFileLen(raw); buf = Hunk_AllocateTempMemoryHigh(len);
//   DB_GetRawBuffer(raw, buf, len); return buf;
// The replacement does the same through the game's own functions, so no trampoline is needed.
//
// Another mod may replace the function the same way (a 14-byte "jmp [rip+0]; dq target" at its
// start), e.g. a fix mod loaded under another DLL name. Whichever installs second calls the
// first one for the script text and patches what it returns, so both work in either order.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN64
#include "game.h"
#endif
#include "hook.h"
#include "log.h"
#include "scrdebug.h"
#include "scripts.h"

#define ASSET_TYPE_RAWFILE 0x24
#ifdef _WIN64
#define JUMP_SIZE_FOR_SCRIPTS 14
#else
#define JUMP_SIZE_FOR_SCRIPTS 5
#endif

static uint8_t *base;
static char mod_dir[MAX_PATH];
typedef char *(*add_source_buffer_t)(const char *filename, const char *ext, const char *code_pos, int archive);
static add_source_buffer_t chained;	// another mod's replacement installed before ours

#define FN(rva, type) ((type)(base + (rva)))
int game_dvar_raw(const char *name, char *buf, int size); // game.c (not included here: the 32-bit build has its own RVA macros above)

// true when the level being loaded is a co-op session (see the top of the file)
static int coop_session(void)
{
	char v[8] = "";
	return game_dvar_raw("coop", v, sizeof(v)) && v[0] == '1';
}

// A script of ours that is loaded in every session: the fixes that are no part of the co-op (maps/_mw2fix*.gsc)
static int fix_script(const char *name)
{
	const char *base_name = name;
	for (const char *p = name; *p; p++)
		if (*p == '/' || *p == '\\')
			base_name = p + 1;
	return strncmp(base_name, "_mw2fix", 7) == 0;
}
typedef void *(*db_find_header_t)(int type, const char *name);
typedef int (*db_is_default_t)(int type, const char *name);
typedef int (*db_rawfile_len_t)(void *raw);
typedef void (*db_raw_buffer_t)(void *raw, char *buf, int size);
#ifdef _WIN64
typedef char *(*hunk_temp_high_t)(int64_t size);
#else
// The 32-bit official Steam build (code SHA-256 FC9E4843...52D7, image base 0x400000), docs/32bit-plan.md.
// All of these are cdecl; the wrapper takes the same four arguments as the 64-bit function.
typedef char *(*hunk_temp_high_t)(int size);
#include "game.h" // the RVA_ macros (through addr_xr: the iw4x-sp client's 1.0.159 exe has its own addresses)
#endif

// Reads <mod_dir>\<sub>\<ext><suffix> into a malloc'd, NUL-terminated buffer with '\r' removed.
static char *read_mod_file(const char *sub, const char *ext, const char *suffix, int *size);

// The mod's QA scenarios (maps/_mw2coop_qa.gsc, about 270 KB, and maps/_mw2coop_qa_bind.gsc, which only it calls) in a session without one (the dvar mw2coop_qa
// empty: every player's session): a stub with the entry points the other scripts call. The scenarios' text made thousands of script strings on every co-op level,
// and Just Like Old Times (af_caves) needs nearly the whole script string table of the 32-bit game: after 16 levels in one process its load ran out ("exceeded
// maximum number of script strings"; the error box then failed on its own string, again and again, until the stack overflowed: QA level chains 2026-11-17).
static int is_name(const char *a, const char *b) // (a script name, either slash)
{
	for (; *a && *b; a++, b++)
		if (!((*a == '/' || *a == '\\') && (*b == '/' || *b == '\\')) && (*a | 0x20) != (*b | 0x20))
			return 0;
	return !*a && !*b;
}
static char *read_mod_script(const char *ext, int *size)
{
	char qa[64] = "";
	const int have_qa = game_dvar_raw("mw2coop_qa", qa, sizeof(qa)) && qa[0];
	// (QA "hoplite": the stub too, with one thing of its own: each mission is completed 15 s after it starts, a level chain as a player has it, without the scenarios)
	if (is_name(ext, "maps/_mw2coop_qa.gsc") && (!have_qa || !strcmp(qa, "hoplite"))) {
		static const char stub[] = "// the QA scenarios are left out of a session without one (src/scripts.c): their entry points\n"
					   "main()\n{\n\tif ( GetDvar( \"mw2coop_qa\" ) == \"hoplite\" )\n\t\tlevel thread hoplite();\n}\n"
					   "hoplite()\n{\n\twait 15;\n\tmaps\\_mw2coop::mission_complete();\n}\n"
					   "link_probe()\n{\n}\nmuseum_anim_probe()\n{\n}\nclimbsolo()\n{\n}\nbbcheck()\n{\n}\nracestart()\n{\n}\nhintkeys_precache()\n{\n}\ncheck( ok, text )\n{\n}\nreport( text )\n{\n}\n"
					   "qa_shot( label )\n{\n}\npress( player, button, ms )\n{\n}\nwait_intro()\n{\n}\nplayer_name( player )\n{\n\treturn \"\";\n}\n";
		char *buf = malloc(sizeof(stub));
		if (!buf)
			return NULL;
		memcpy(buf, stub, sizeof(stub));
		*size = (int)sizeof(stub) - 1;
		static int told;
		if (!told++)
			log_printf("scripts: no QA scenario in this session: maps/_mw2coop_qa.gsc is a stub of its entry points (the scenarios' strings stay out of the game's string table)");
		return buf;
	}
	return read_mod_file("scripts", ext, "", size);
}

static char *read_mod_file(const char *sub, const char *ext, const char *suffix, int *size)
{
	char path[MAX_PATH];
	int n = snprintf(path, sizeof(path), "%s\\%s\\%s%s", mod_dir, sub, ext, suffix);
	if (n <= 0 || n >= (int)sizeof(path))
		return NULL;
	for (char *p = path; *p; p++)
		if (*p == '/')
			*p = '\\';
	HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return NULL;
	LARGE_INTEGER len;
	GetFileSizeEx(f, &len);
	if (len.QuadPart <= 0 || len.QuadPart > 16 * 1024 * 1024) {
		CloseHandle(f);
		return NULL;
	}
	char *buf = malloc(len.QuadPart + 1);
	DWORD got = 0;
	ReadFile(f, buf, (DWORD)len.QuadPart, &got, NULL);
	CloseHandle(f);
	int w = 0;
	for (DWORD i = 0; i < got; i++)
		if (buf[i] != '\r')
			buf[w++] = buf[i];
	buf[w] = 0;
	*size = w;
	return buf;
}

static uint32_t fnv1a(const char *s, int len)
{
	uint32_t h = 2166136261u;
	for (int i = 0; i < len; i++)
		h = (h ^ (uint8_t)s[i]) * 16777619u;
	return h;
}

static int count_occurrences(const char *hay, const char *needle, const char **first)
{
	int n = 0;
	size_t nl = strlen(needle);
	*first = NULL;
	for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle)) {
		if (!n)
			*first = p;
		n++;
		if (nl == 0)
			break;
	}
	return n;
}

// Takes the next "@@@ <word>" block body from *cursor. Returns the marker word in kind.
static char *next_block(char **cursor, char *kind, int kind_size)
{
	char *p = strstr(*cursor, "@@@ ");
	if (!p)
		return NULL;
	char *eol = strchr(p, '\n');
	if (!eol)
		return NULL;
	int k = (int)(eol - (p + 4));
	if (k >= kind_size)
		k = kind_size - 1;
	memcpy(kind, p + 4, k);
	kind[k] = 0;
	char *body = eol + 1;
	if (strncmp(body, "@@@ ", 4) == 0) { // empty body, the next marker follows right away
		*eol = 0;
		*cursor = body;
		return eol;
	}
	char *end = strstr(body, "\n@@@ ");
	if (!end)
		return NULL;
	*end = 0; // body ends before the newline that precedes the next marker
	*cursor = end + 1;
	return body;
}

static int is_ident_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// The top-level function `name` in src: [*start, *end) from the start of its header line to its
// closing brace. A header starts a line; strings and comments are skipped when matching braces.
static int find_function(const char *src, const char *name, const char **start, const char **end)
{
	size_t nl = strlen(name);
	for (const char *p = strstr(src, name); p; p = strstr(p + 1, name)) {
		if ((p != src && p[-1] != '\n') || is_ident_char(p[nl]))
			continue;
		const char *q = p + nl;
		while (*q == ' ' || *q == '\t')
			q++;
		if (*q != '(')
			continue;
		const char *c = strchr(q, '{');
		int depth = 0;
		for (; c && *c; c++) {
			if (c[0] == '/' && c[1] == '/') {
				while (*c && *c != '\n')
					c++;
				if (!*c)
					break;
			} else if (c[0] == '/' && c[1] == '*') {
				c = strstr(c + 2, "*/");
				if (!c)
					return 0;
				c++;
			} else if (*c == '"') {
				for (c++; *c && *c != '"'; c++)
					if (*c == '\\' && c[1])
						c++;
				if (!*c)
					break;
			} else if (*c == '{') {
				depth++;
			} else if (*c == '}' && --depth == 0) {
				*start = p;
				*end = c + 1;
				return 1;
			}
		}
		return 0;
	}
	return 0;
}

// Replaces every whole-word `from` with `to` inside function `name`. Returns the new malloc'd
// text (src freed) and the number of replacements in *count (src unchanged when 0).
static char *subst_in_function(char *src, int *len, const char *name, const char *from, const char *to, int *count)
{
	const char *fs, *fe;
	*count = 0;
	size_t fl = strlen(from), tl = strlen(to);
	if (!fl || !find_function(src, name, &fs, &fe))
		return src;
	int lead = is_ident_char(from[0]), trail = is_ident_char(from[fl - 1]);
	size_t cap = (size_t)*len + 1, n = 0;
	char *out = malloc(cap);
	size_t pre = fs - src;
	memcpy(out, src, pre);
	n = pre;
	for (const char *p = fs; p < fe;) {
		if ((size_t)(fe - p) >= fl && memcmp(p, from, fl) == 0 && !(lead && p > src && is_ident_char(p[-1])) &&
		    !(trail && is_ident_char(p[fl]))) {
			if (n + tl + (size_t)(*len - (p - src)) + 1 > cap) {
				cap = (n + tl + (size_t)(*len - (p - src)) + 1) * 2;
				out = realloc(out, cap);
			}
			memcpy(out + n, to, tl);
			n += tl;
			p += fl;
			(*count)++;
		} else {
			out[n++] = *p++;
		}
	}
	size_t rest = (size_t)*len - (fe - src);
	if (n + rest + 1 > cap)
		out = realloc(out, n + rest + 1);
	memcpy(out + n, fe, rest + 1);
	n += rest;
	if (!*count) {
		free(out);
		return src;
	}
	free(src);
	*len = (int)n;
	return out;
}

// Applies <ext><suffix> (".patch", then ".cp.patch": the checkpoint hooks, written by their own
// generator) to src (malloc'd). Returns the new malloc'd text, or src unchanged; adds to *applied
// and *total.
static char *apply_patches(const char *ext, const char *suffix, char *src, int *len, int *applied, int *total)
{
	int psize;
	char *patch = read_mod_file("patches", ext, suffix, &psize);
	if (!patch)
		return src;
	char *cursor = patch, kind[128];
	char *body;
	while ((body = next_block(&cursor, kind, sizeof(kind))) != NULL) {
		if (strcmp(kind, "replace") == 0) {
			char kind2[32];
			char *with = next_block(&cursor, kind2, sizeof(kind2));
			if (!with || strcmp(kind2, "with") != 0) {
				log_printf("scripts: %s: malformed patch (replace without with)", ext);
				break;
			}
			(*total)++;
			const char *at;
			int n = count_occurrences(src, body, &at);
			if (n != 1) {
				log_printf("scripts: %s: patch %d FAILED, text found %d times: %.60s", ext, *total, n, body);
				continue;
			}
			size_t bl = strlen(body), wl = strlen(with), off = at - src;
			char *out = malloc(*len - bl + wl + 1);
			memcpy(out, src, off);
			memcpy(out + off, with, wl);
			memcpy(out + off + wl, at + bl, *len - off - bl + 1);
			*len = (int)(*len - bl + wl);
			free(src);
			src = out;
			(*applied)++;
		} else if (strcmp(kind, "append") == 0) {
			(*total)++;
			size_t bl = strlen(body);
			char *out = malloc(*len + bl + 3);
			memcpy(out, src, *len);
			out[*len] = '\n';
			memcpy(out + *len + 1, body, bl);
			out[*len + 1 + bl] = '\n';
			out[*len + 2 + bl] = 0;
			*len += (int)bl + 2;
			free(src);
			src = out;
			(*applied)++;
		} else if (strncmp(kind, "subst ", 6) == 0) {
			char kind2[32];
			char *to = next_block(&cursor, kind2, sizeof(kind2));
			if (!to || strcmp(kind2, "to") != 0) {
				log_printf("scripts: %s: malformed patch (subst without to)", ext);
				break;
			}
			(*total)++;
			int count;
			src = subst_in_function(src, len, kind + 6, body, to, &count);
			if (!count)
				log_printf("scripts: %s: patch %d FAILED, '%.40s' not found in %s", ext, *total, body, kind + 6);
			else
				(*applied)++;
		} else if (strcmp(kind, "end") != 0 && strcmp(kind, "with") != 0 && strcmp(kind, "to") != 0) {
			log_printf("scripts: %s: unknown patch block '%s'", ext, kind);
		}
	}
	free(patch);
	return src;
}

static char *to_hunk(const char *text, int len)
{
	char *buf = FN(RVA_HUNK_TEMP_HIGH, hunk_temp_high_t)(len + 1);
	memcpy(buf, text, len);
	buf[len] = 0;
	return buf;
}

// Debugging: with MW2COOP_SCRIPTDUMP=<part of a name> in the environment, the zone's own text of every script whose name has that part
// (any case) is written to mw2coop\scriptdump\<name, '/' as '_'>, before any patch: which version of a level's script a client's zone
// holds (the Museum's animations were undefined on iw4x-sp only, 2026-11-19).
static void dump_zone_script(const char *ext, const char *buf, int len)
{
	char want[64];
	DWORD w = GetEnvironmentVariableA("MW2COOP_SCRIPTDUMP", want, sizeof(want));
	if (!w || w >= sizeof(want) || !buf || len <= 0)
		return;
	int found = 0;
	for (const char *s = ext; *s && !found; s++) {
		size_t i = 0;
		while (want[i] && s[i] && (s[i] | 0x20) == (want[i] | 0x20))
			i++;
		found = !want[i];
	}
	if (!found)
		return;
	char path[MAX_PATH];
	int n = snprintf(path, sizeof(path), "%s\\scriptdump", mod_dir);
	if (n <= 0 || n >= (int)sizeof(path))
		return;
	CreateDirectoryA(path, NULL);
	n = snprintf(path, sizeof(path), "%s\\scriptdump\\%s", mod_dir, ext);
	if (n <= 0 || n >= (int)sizeof(path))
		return;
	for (char *p = path + strlen(mod_dir) + 12; *p; p++) // (past "\scriptdump\")
		if (*p == '/' || *p == '\\')
			*p = '_';
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fwrite(buf, 1, (size_t)len, f);
	fclose(f);
	log_printf("scripts: %s: the zone's text (%d bytes) written to %s", ext, len, path);
}

// The zone text of `ext` (buf, len) with the patch files applied: a malloc'd copy and its length in *n, or NULL when the
// script has no patch file. Logs the result (the FNV hash is what host and client compare).
static char *patched_text(const char *ext, const char *buf, int len, int *n, int coop)
{
	// Patch a working copy; the zone text keeps its CR, which the compiler ignores.
	int plen;
	char *probe = coop ? read_mod_file("patches", ext, ".patch", &plen) : NULL;
	if (!probe && coop)
		probe = read_mod_file("patches", ext, ".cp.patch", &plen);
	if (!probe)
		probe = read_mod_file("patches", ext, ".fix.patch", &plen); // (every session)
	if (!probe)
		return NULL;
	free(probe);
	*n = 0;
	char *work = malloc(len + 1);
	for (int i = 0; i < len && buf[i]; i++)
		if (buf[i] != '\r')
			work[(*n)++] = buf[i];
	work[*n] = 0;
	int applied = 0, total = 0;
	if (coop) {
		work = apply_patches(ext, ".patch", work, n, &applied, &total);
		work = apply_patches(ext, ".cp.patch", work, n, &applied, &total);
	}
	work = apply_patches(ext, ".fix.patch", work, n, &applied, &total);
	scrdebug_note_source(ext, work, *n);
	log_printf("scripts: %s <- zone + %d/%d patches (%d bytes, hash %08x)%s", ext, applied, total, *n,
		   fnv1a(work, *n), applied == total ? "" : "  !!! PATCH FAILURES");
	return work;
}

static char *add_source_buffer(const char *filename, const char *ext, const char *code_pos, int archive)
{
	int size;
	int coop = coop_session();
	static int last_coop = -1;
	if (coop != last_coop) {
		last_coop = coop;
		log_printf("scripts: %s", coop ? "co-op session: the mod's scripts and patches are loaded" : "single player: the game's own scripts (only the fixes that are no part of the co-op: *.fix.patch, _mw2fix*)");
	}
	char *own = coop || fix_script(ext) ? read_mod_script(ext, &size) : NULL;
	if (own) {
		char *buf = to_hunk(own, size);
		log_printf("scripts: %s <- mw2coop (%d bytes, hash %08x)", ext, size, fnv1a(own, size));
		scrdebug_note_source(ext, own, size);
		free(own);
		return buf;
	}
	char *buf;
	int len;
	if (chained) {
		buf = chained(filename, ext, code_pos, archive);
		if (!buf)
			return NULL;
		len = (int)strlen(buf);
	} else {
		void *raw = FN(RVA_DB_FIND_HEADER, db_find_header_t)(ASSET_TYPE_RAWFILE, ext);
		if (FN(RVA_DB_IS_DEFAULT, db_is_default_t)(ASSET_TYPE_RAWFILE, ext))
			return NULL;
		len = FN(RVA_DB_RAWFILE_LEN, db_rawfile_len_t)(raw);
		buf = FN(RVA_HUNK_TEMP_HIGH, hunk_temp_high_t)(len);
		FN(RVA_DB_RAW_BUFFER, db_raw_buffer_t)(raw, buf, len);
	}
	dump_zone_script(ext, buf, len);

	int n;
	char *work = patched_text(ext, buf, len, &n, coop);
	if (!work) {
		scrdebug_note_source(ext, buf, len);
		return buf;
	}
	char *out = to_hunk(work, n);
	free(work);
	return out;
}

#ifdef _WIN64
// Expected first bytes of the original, to make sure we patch the right code.
static const uint8_t expected[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x56, 0x48, 0x83, 0xec, 0x20, 0xb9, 0x24, 0x00, 0x00, 0x00};
#else
// The first two instructions and more of the cdecl wrapper at 0x47f8e0 (it forwards to a usercall body).
static const uint8_t expected[] = {0x8b, 0x44, 0x24, 0x10, 0x8b, 0x4c, 0x24, 0x04, 0x53, 0x8b, 0x5c, 0x24, 0x10, 0x56, 0x8b, 0x74, 0x24, 0x10, 0x50, 0x51};
#endif
#ifndef _WIN64
// The iw4x-sp client loads scripts with its own loader (it replaces the calls of Scr_AddSourceBuffer, so the hook above is not reached):
// it asks DB_FindXAssetHeader / DB_IsXAssetDefault for the rawfile "<script>.gsc", allocates DB_GetRawFileLen bytes and fills them
// with DB_GetRawBuffer. So here those two are hooked and give the mod's text: a script of ours the zone does not have gets a made-up
// rawfile (scripts_raw_override / scripts_is_override, called from the hooks of DB_FindXAssetHeader and DB_IsXAssetDefault), a patched
// zone script is built from the real rawfile when its length is first asked.
typedef struct {
	const char *name;
	int compressed_len;
	int len;
	const char *buffer;
} raw_file_t;
typedef struct {
	char name[96];
	char *text; // the mod's text, NULL: the zone's own
	int n;
	int own; // the text is a file of ours (no zone rawfile behind it)
	int coop; // made for a co-op session (the other kind has the fixes only)
	raw_file_t fake;
} script_entry_t;
// Every script the client compiles has an entry (one with no text says "the zone's own"), and a campaign played level after level in one process asks for
// more than the 320 the table first held: past them a patched script loaded unpatched and a module of ours was not found (audit 2026-11-18, glue F1).
// Each entry is allocated on its own, so its address (handed to the game as a rawfile) never moves.
#define MAX_ENTRIES 8192
static script_entry_t *entries[MAX_ENTRIES]; // (a script asked in both kinds of session has two)
static int entry_count;
static CRITICAL_SECTION entries_lock;
static volatile LONG lock_ready;
static int (*len_original)(raw_file_t *raw);
static void (*buffer_original)(raw_file_t *raw, char *buf, int size);

static int is_script_name(const char *name)
{
	size_t n = name ? strlen(name) : 0;
	return n > 4 && n < 96 && strcmp(name + n - 4, ".gsc") == 0;
}

// The entry of a script name (entries_lock held), made on the first ask: a file of ours, else (when the caller has the zone's
// rawfile) the zone text with the patch files applied, else an entry with no text (the zone's own is used). NULL: no entry yet and
// no rawfile to make one from.
static script_entry_t *script_entry(const char *name, raw_file_t *raw)
{
	int coop = coop_session(); // single player: the zone's own text and the fixes that are no part of the co-op (an entry is kept for its kind of session)
	for (int i = 0; i < entry_count; i++)
		if (entries[i]->coop == coop && strcmp(entries[i]->name, name) == 0)
			return entries[i];
	int size;
	char *own = coop || fix_script(name) ? read_mod_script(name, &size) : NULL;
	if (!own && !raw)
		return NULL;
	script_entry_t *e = entry_count < MAX_ENTRIES ? calloc(1, sizeof(script_entry_t)) : NULL;
	if (!e) {
		static int said;
		if (!said++)
			log_printf("scripts: no room for the entry of %s (%d entries): the zone's own script is used", name, entry_count);
		free(own);
		return NULL;
	}
	lstrcpynA(e->name, name, sizeof(e->name));
	e->coop = coop;
	e->text = NULL;
	e->own = 0;
	if (own) {
		e->text = own;
		e->n = size;
		e->own = 1;
		log_printf("scripts: %s <- mw2coop (%d bytes, hash %08x)", name, size, fnv1a(own, size));
		scrdebug_note_source(name, own, size);
	} else {
		int len = len_original(raw);
		char *buf = malloc(len + 1);
		buffer_original(raw, buf, len);
		buf[len] = 0;
		dump_zone_script(name, buf, len);
		e->text = patched_text(name, buf, len, &e->n, coop);
		if (!e->text)
			scrdebug_note_source(name, buf, len); // (the zone's own script: its function names are for the call stacks too)
		free(buf);
	}
	e->fake.name = e->name;
	e->fake.len = e->n;
	e->fake.buffer = e->text;
	entries[entry_count++] = e;
	if (entry_count % 100 == 0)
		log_printf("scripts: %d script entries (iw4x client)", entry_count);
	return e;
}

// The made-up rawfile of a script of ours (the zone has none, or ours replaces it), else NULL.
void *scripts_raw_override(const char *name)
{
	if (!lock_ready || !is_script_name(name))
		return NULL;
	EnterCriticalSection(&entries_lock);
	script_entry_t *e = script_entry(name, NULL);
	void *r = e && e->own ? (void *)&e->fake : NULL;
	LeaveCriticalSection(&entries_lock);
	return r;
}

int scripts_is_override(const char *name)
{
	return scripts_raw_override(name) != NULL;
}

static int len_hook(raw_file_t *raw)
{
	if (raw && is_script_name(raw->name)) {
		EnterCriticalSection(&entries_lock);
		script_entry_t *e = script_entry(raw->name, raw);
		int n = e && e->text ? e->n + 1 : -1;
		LeaveCriticalSection(&entries_lock);
		if (n >= 0)
			return n;
	}
	return len_original(raw);
}

static void buffer_hook(raw_file_t *raw, char *buf, int size)
{
	if (raw && is_script_name(raw->name)) {
		EnterCriticalSection(&entries_lock);
		script_entry_t *e = script_entry(raw->name, raw);
		if (e && e->text && size > 0) {
			int n = e->n < size - 1 ? e->n : size - 1;
			memcpy(buf, e->text, n);
			buf[n] = 0;
			LeaveCriticalSection(&entries_lock);
			return;
		}
		LeaveCriticalSection(&entries_lock);
	}
	buffer_original(raw, buf, size);
}

static void install_iw4x(void)
{
	// DB_GetRawFileLen: mov eax,[esp+4]; mov eax,[eax+8]; (add eax,1; ret follow). DB_GetRawBuffer: sub esp,3Ch; push ebx/ebp/esi/edi
	static const uint8_t len_bytes[] = {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x40, 0x08};
	static const uint8_t buffer_bytes[] = {0x83, 0xec, 0x3c, 0x53, 0x55, 0x56, 0x57};
	InitializeCriticalSection(&entries_lock);
	lock_ready = 1;
	if (!hook_function(base + RVA_DB_RAWFILE_LEN, len_bytes, sizeof(len_bytes), len_hook, (void **)&len_original) ||
	    !hook_function(base + RVA_DB_RAW_BUFFER, buffer_bytes, sizeof(buffer_bytes), buffer_hook, (void **)&buffer_original)) {
		log_printf("scripts: unexpected bytes at DB_GetRawFileLen / DB_GetRawBuffer, not patching (iw4x client)");
		return;
	}
	log_printf("scripts: iw4x client: script text served through DB_GetRawFileLen / DB_GetRawBuffer, mod files from %s", mod_dir);
}
#endif

void scripts_install(void)
{
	base = (uint8_t *)GetModuleHandleA(NULL);
	DWORD n = GetModuleFileNameA(NULL, mod_dir, MAX_PATH);
	while (n > 0 && mod_dir[n - 1] != '\\')
		n--;
	mod_dir[n] = 0;
	lstrcatA(mod_dir, "mw2coop");

#ifndef _WIN64
	if (game_is_iw4x()) {
		install_iw4x();
		return;
	}
#endif
	uint8_t *target = base + RVA_SCR_ADD_SOURCE_BUFFER;
#ifdef _WIN64
	static const uint8_t jmp_rip[6] = {0xff, 0x25, 0, 0, 0, 0};
	if (memcmp(target, jmp_rip, sizeof(jmp_rip)) == 0) {
		memcpy(&chained, target + 6, sizeof(chained));
		log_printf("scripts: Scr_AddSourceBuffer already replaced by another mod (%p): chained", (void *)chained);
	} else
#endif
	if (memcmp(target, expected, sizeof(expected)) != 0) {
		log_printf("scripts: unexpected bytes at Scr_AddSourceBuffer, not patching");
		return;
	}
	hook_jump(target, JUMP_SIZE_FOR_SCRIPTS, add_source_buffer, 0x90);
	log_printf("scripts: Scr_AddSourceBuffer replaced, mod files from %s", mod_dir);
}
