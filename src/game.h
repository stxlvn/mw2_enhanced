#pragma once
#include <stdint.h>
#include "addr.h"
#include "game.h"

// An address of the reference build, translated for the running build (addr.c): the same number as long as the build is the reference one. The 32-bit official block of RVA_
// macros below goes through it; the iw4x-sp profile has its own values.
#define XRA(r) addr_xr(r)

// 1 when the 32-bit mod runs in the iw4x-sp client (iw4x-sp.exe, the known 1.0.159 iw4sp.exe): its addresses come from the table of addr159.c, and a few things work
// another way (scripts are rawfiles there, the client brings its own watermark and its own `connect`). Never in the 64-bit build.
#ifdef _WIN64
#define game_is_iw4x() 0
#else
int game_is_iw4x(void);
#endif

#ifdef _WIN64
// Addresses in iw4sp.exe of the 64-bit Steam build (buildid 25374375, PE timestamp 0x6aaaa257,
// SHA-256 2F8C9CFD...D805), as RVAs from the image base. Found with Ghidra, see NOTES.md.
#define IW4SP_TIMESTAMP 0x6aaaa257u

#define RVA_CBUF_ADDTEXT   XR(0x1ee890)  // void Cbuf_AddText(int localClientNum, const char *text); locks CS 0x24
#define RVA_DVAR_FINDVAR   XR(0x26bdf0)  // dvar_t *Dvar_FindVar(const char *name); reader-locked, thread safe
#define RVA_DVAR_HASH      XR(0x1b21e90) // dvar_t *table[1024]: the buckets Dvar_FindVar walks (chain: dvar_t.hash_next)
#define RVA_CMD_FUNCTIONS  XR(0x1315308) // cmd_function_t *cmd_functions (list head)
// Console command arguments {int nesting; ...; int argc[8]; char **argv[8]}: console commands
// (connect @0xd23c0) and server commands (devmap @0x22cb70) have one each.
#define RVA_CMD_ARGS         XR(0x13151b0) // nesting; argc +0x44, argv +0x68
#define RVA_SV_CMD_ARGS      XR(0x1315260) // nesting; argc +0x44, argv +0x68
#define RVA_SV_CMD_DEVMAP    XR(0x1904f60) // server cmd_function_t node of "devmap" (handler @0x22cb70)
#define RVA_XUID_TABLE     XR(0x1bbaad8) // uint64 xuid of local client i at +i*0x68 (Live_GetXuid, rva 0x28f8b0)

// Co-op party (lobby) state
#define RVA_PARTY_SKILL          XR(0x5dd988) // int: party member n's difficulty at +n*0x38 (rva 0xd7710 reads it)
#define RVA_PARTY_LOBBY          XR(0x5dd958) // lobby object *: +0x10 active, +0x70/+0xa8 member slots
#define RVA_PARTY_IS_HOST        XR(0x5dd950) // uint8
#define RVA_PARTY_ACTIVE         XR(0x5dd951) // uint8, gates the party frame (0xd74a0)
#define RVA_PARTY_LAUNCH         XR(0x5dd953) // uint8, set by coopLaunch (0xd7a50)
#define RVA_PARTY_MEMBER_NAMES   XR(0x5dd960) // char[32] per member, stride 0x38 (slot 0: the host)
#define RVA_PARTY_START_TIME     XR(0x5dd954) // int32
#define RVA_LOBBY_SLOT_USED      XR(0x236bc0) // char (lobby, slot): slot taken (party state @0xd7ee0)
#define RVA_PARTY_RESET          XR(0x0d7dd0) // void Party_Reset(void)
#define RVA_PARTY_CLEAR_MEMBERS  XR(0x236920) // void (int)
#define RVA_LOBBY_JOIN           XR(0x236d50) // char Lobby_Join(lobby, localClient, 0x322, hostInfo[8], 0, 2, 0)
#define RVA_PARTY_ACTIVATE       XR(0x0d7380) // void Party_Activate(void): active = 1, member timers
#define RVA_PARTY_AFTER_JOIN     XR(0x0d6820) // void (void), called at the end of PartyClient_Join (0xd7cb0)
#define RVA_SYS_MILLISECONDS     XR(0x293dc0) // int Sys_Milliseconds(void)
#define RVA_COOP_LAUNCH_MAP      XR(0x22e1a0) // void (int localClient): coop slot state 3, net rates, "map <coop_mapName>"
#define RVA_COOP_SLOT_STATE      XR(0x533cb8) // int32 per local client (stride 16): 0 idle, 3 launched

// Co-op lobby (see NOTES.md): listen @0xd25e0, connect @0xd23c0, coopLaunch @0xd25d0,
// host-side coopConnect handler @0x22d830. Online gate @0x28fc00 needs Steam logged on and
// the DemonWare state (rva 0x115c07c) == 4.

// Script loading
#define RVA_SCR_ADD_SOURCE_BUFFER XR(0x217950) // char *Scr_AddSourceBuffer(filename, extFilename, codePos, archive)
#define RVA_DB_FIND_HEADER  XR(0x0ef5d0) // void *DB_FindXAssetHeader(int type, const char *name)
#define RVA_DB_IS_DEFAULT   XR(0x0efff0) // int DB_IsXAssetDefault(int type, const char *name)
#define RVA_DB_RAWFILE_LEN  XR(0x0efdc0) // int DB_GetRawFileLen(RawFile *)
#define RVA_DB_RAW_BUFFER   XR(0x0efc70) // void DB_GetRawBuffer(RawFile *, char *buf, int size)
#define RVA_HUNK_TEMP_HIGH  XR(0x2693f0) // void *Hunk_AllocateTempMemoryHigh(int64 size)

// The script builtin table entry of "setdvar" ({name, function, developer}) and Scr_GetString(index)
#define RVA_FN_SETDVAR_ENTRY XR(0x3c1568)
#define RVA_SCR_GET_STRING   XR(0x223440)
#else
// Addresses in iw4sp.exe of the 32-bit official Steam build (linked 2010-02-10, code SHA-256 FC9E4843...52D7), as RVAs
// from the image base 0x400000. Found with Ghidra on the memory dump, see docs/32bit-plan.md. Names as above; a 0 means
// "not found yet" and the module that needs it stays off.
#define IW4SP_TIMESTAMP 0u // the exe on disk is rewritten per account by Steam's wrapper: the build is known by its code hash (main32.c)

#define RVA_CBUF_ADDTEXT   XRA(0x009b40)  // void Cbuf_AddText(int localClientNum, const char *text); cdecl, locks CS 0x21
#define RVA_DVAR_FINDVAR   XRA(0x233480)  // dvar_t *Dvar_FindVar(const char *name): USERCALL, name in EDI
#define RVA_DVAR_HASH      XRA(0x1569a90) // dvar_t *table[1024] (Dvar_FindVar's buckets)
#define RVA_CMD_ADDCOMMAND XRA(0x028090)  // void Cmd_AddCommand(name, function, node, 0); cdecl
#define RVA_CMD_ARGS       XRA(0x104c950) // nesting; argc[nesting] at +0x44, argv[nesting] at +0x64
#define RVA_SV_CMD_ARGS    XRA(0x1057620) // the server commands' arguments, same layout (argc +0x44, argv +0x64); read in the devmap handler 0x659780
#define RVA_SV_CMD_DEVMAP  XRA(0x166ab98) // the server command node of "devmap" (handler 0x659780); the console node is 0x166abb0 (a no-op)
#define RVA_CMD_FUNCTIONS  XRA(0x104c9d8) // cmd_function_t *cmd_functions (list head; Cmd_AddCommand 0x428090 links in front)
#define RVA_XUID_TABLE     XRA(0x15fbf88) // uint64 xuid of local client i at +i*0x68 (read by 0x46f800)

// Co-op party (lobby) state
#define RVA_PARTY_SKILL          XRA(0x534480) // int: party member n's difficulty at +n*0x38 (0x4141b0 reads it)
#define RVA_PARTY_LOBBY          XRA(0x534450) // lobby object * (its first field is the name "gameSession")
#define RVA_PARTY_IS_HOST        XRA(0x534448) // uint8: set by listen (0x57a860), cleared by a client's join
#define RVA_PARTY_ACTIVE         XRA(0x534449) // uint8, gates the party frame (0x4a4be0); lobby +8 must be set too
#define RVA_PARTY_LAUNCH         XRA(0x53444b) // uint8, set by coopLaunch (0x57aa20); the frame (0x57c800) launches with two members
#define RVA_PARTY_MEMBER_NAMES   XRA(0x534458) // char[32] per member, stride 0x38 (timers at +0x20, difficulty at +0x28)
#define RVA_PARTY_START_TIME     XRA(0x53444c) // int32
#define RVA_LOBBY_SLOT_USED      XRA(0x035ff0) // char (lobby, slot): the slot is taken (called in Party_Activate 0x45dde0)
#define RVA_PARTY_RESET          XRA(0x02f0f0) // void Party_Reset(void)
#define RVA_PARTY_CLEAR_MEMBERS  XRA(0x0b82f0) // void (int)
#define RVA_LOBBY_JOIN           XRA(0x092e80) // void Lobby_Join(lobby, xuid, 0x322, hostInfo, 0, 0, 2, 0); cdecl, 8 args
#define RVA_PARTY_JOIN           XRA(0x02fc40) // char PartyClient_Join(int localClient, hostInfo, int *ip, short *port): the whole client join
#define RVA_PARTY_ACTIVATE       XRA(0x05dde0) // void Party_Activate(void)
#define RVA_PARTY_AFTER_JOIN     XRA(0x019810) // void (void)
#define RVA_SYS_MILLISECONDS     XRA(0x04e1b0) // int Sys_Milliseconds(void)
#define RVA_COOP_LAUNCH_MAP      XRA(0x07bd80) // void (int localClient): coop slot state 3, then "map <coop_mapName>"
#define RVA_COOP_SLOT_STATE      XRA(0x525bc0) // int32 per local client (stride 16): 0 idle, 3 launched

// Script loading
#define RVA_SCR_ADD_SOURCE_BUFFER XRA(0x07f8e0) // char *Scr_AddSourceBuffer(filename, extFilename, codePos, archive); cdecl wrapper
#define RVA_DB_FIND_HEADER  XRA(0x0f6f10)
#define RVA_DB_IS_DEFAULT   XRA(0x0c32b0)
#define RVA_DB_RAWFILE_LEN  XRA(0x0e3080)
#define RVA_DB_RAW_BUFFER   XRA(0x0e55f0)
#define RVA_HUNK_TEMP_HIGH  XRA(0x00eed0) // void *Hunk_AllocateTempMemoryHigh(int size)

// The script builtin table entry of "setdvar" ({name, function, developer}) and Scr_GetString(index)
#define RVA_FN_SETDVAR_ENTRY XRA(0x331504)
#define RVA_SCR_GET_STRING   XRA(0x0d5f20)
#endif

typedef enum {
	DVAR_TYPE_BOOL = 0,
	DVAR_TYPE_FLOAT = 1,
	DVAR_TYPE_FLOAT_2 = 2,
	DVAR_TYPE_FLOAT_3 = 3,
	DVAR_TYPE_FLOAT_4 = 4,
	DVAR_TYPE_INT = 5,
	DVAR_TYPE_ENUM = 6,
	DVAR_TYPE_STRING = 7,
	DVAR_TYPE_COLOR = 8,
	DVAR_TYPE_FLOAT_3_COLOR = 9,
	DVAR_TYPE_COUNT = 10,
} dvar_type_t;

typedef union {
	uint8_t enabled;
	int32_t integer;
	float value;
	float vector[4];
	const char *string;
} dvar_value_t;

#ifdef _WIN64
typedef struct dvar_s {
	const char *name;    // 0x00
	uint32_t flags;      // 0x08
	uint8_t type;        // 0x0c
	uint8_t modified;    // 0x0d
	uint8_t pad[2];
	dvar_value_t current; // 0x10
	dvar_value_t latched; // 0x20
	dvar_value_t reset;   // 0x30
	uint8_t domain[0x18]; // 0x40
	struct dvar_s *hash_next; // 0x58
} dvar_t;
#else
// 32-bit: name +0, description +4, flags +8, type +0xc, current +0x10 ... hash chain +0x4c (same offsets as 64 up to 0x40)
typedef struct dvar_s {
	const char *name;    // 0x00
	const char *description; // 0x04
	uint32_t flags;      // 0x08
	uint8_t type;        // 0x0c
	uint8_t modified;    // 0x0d
	uint8_t pad[2];
	dvar_value_t current; // 0x10
	dvar_value_t latched; // 0x20
	dvar_value_t reset;   // 0x30
	uint8_t domain[0x0c]; // 0x40
	struct dvar_s *hash_next; // 0x4c
} dvar_t;
#endif

typedef struct cmd_function_s {
	struct cmd_function_s *next; // 0x00
	const char *name;            // 0x08
	const char *autocomplete_dir; // 0x10
	const char *autocomplete_ext; // 0x18
	void (*function)(void);      // 0x20
#ifndef _WIN64
	// 32-bit: the node is 0x18 bytes ({next, name, dir, ext, function, 1}); Cmd_AddCommand writes all of it
	uint32_t flag;               // 0x14
#endif
} cmd_function_t;
// the tail of a static node initializer: nothing in 64 bits, the 32-bit node's last field
#ifdef _WIN64
#define CMD_NODE_FLAG
#else
#define CMD_NODE_FLAG , 1
#endif

// Returns 1 when the running exe is the build the addresses above belong to.
int game_init(void);
void game_cbuf_addtext(const char *text);
dvar_t *game_find_dvar(const char *name);
// Formats a dvar's current value into buf; returns buf ("<missing>" when not registered).
const char *game_dvar_string(const char *name, char *buf, int size);
// 1 when a console command of that name is registered (the client command list).
int game_command_exists(const char *name);
// Every registered dvar / console command (the developer console's lists and completion). The callback returns nonzero to stop.
void game_for_each_dvar(int (*fn)(dvar_t *dvar, void *ctx), void *ctx);
void game_for_each_command(int (*fn)(const char *name, void *ctx), void *ctx);
// A dvar value as text: bool/int as numbers, enum as its index, float with %g, vectors and colors as lists, strings in quotes.
void game_dvar_value_text(const dvar_t *dvar, const dvar_value_t *value, char *buf, int size);
void game_log_commands(void);
// Copies a dvar's current value into buf (a string, or a bool/int as a number); 0 if missing or another type.
int game_dvar_raw(const char *name, char *buf, int size);
int game_qa_flag(const char *name); // QA bisect: 1 when the file mw2coop\<name> exists next to the exe
// Read the pointer stored at rva, then width bytes at pointer+off (0 if null/unreadable).
unsigned long long game_peek_ptr(unsigned long long rva, unsigned long long off, int width);
// QA only: change local client 0's XUID by delta.
void game_xuid_add(long long delta);
// Registers a console command node ({next, name, ..., function}) with the game: put at the head of the command list
// (64-bit) or through Cmd_AddCommand (32-bit).
void game_add_command(cmd_function_t *node);
// Whether [p, p+size) is committed readable memory / whether the string at s (up to max bytes) is: checked with VirtualQuery, no exception (IsBadReadPtr raises and catches
// one, and the crash logger counts those: crash.c writes only its first three).
int game_mem_readable(const void *p, size_t size);
uint8_t *game_builtin_entry(unsigned rva, const char *name); // the {name, function, developer} entry of a script builtin, by address or else by name
int game_string_readable(const char *s, size_t max);
// Read 1/2/4/8 bytes of the game image at an RVA (0 outside the image).
unsigned long long game_peek(unsigned long long rva, int width);

// Arguments of the console (or server) command being run; call from the command function.
int game_cmd_argc(void);
const char *game_cmd_argv(int i);
const char *game_sv_cmd_argv(int i);

// The 32-bit native code (native.c, rumble.c, padopts.c, padglyphs.c) is written with the official 32-bit build's addresses. In the iw4x-sp profile (1.0.159) XR() turns an
// official RVA into the 159 RVA through a table that tools/gen_addr159.py builds from the source (tools/map159.py maps by byte signatures); XV() does the same for a VA.
#define XR(r) addr_xr(r) // (64-bit too: the number as it is on the reference build, the resolved one on a build whose addresses moved)
// An address of the 64-bit game in code that both builds compile (diagnostics, the 64-bit compositor, hooks the 32-bit build never installs). 64-bit: XR(). 32-bit: such code is never reached, and if
// it ever were, it would read or write a private block of zeros (addr_dead_block) instead of an address far outside the 32-bit image.
#ifdef _WIN64
#define XR64(r) XR(r)
#else
unsigned addr_dead_block(void);
#define XR64(r) ((void)(r), addr_dead_block())
#endif
#ifndef _WIN64
#define XV(va) (0x400000u + addr_xr((va) - 0x400000u))
#else
#define XV(va) (va)
#endif
