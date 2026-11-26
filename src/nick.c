// The player's nickname (owner, 2026-11-20): a Nickname row in the co-op lobby, on every build of the mod (64 bits, the official 32-bit exe, the iw4x-sp client).
//
// The name a player has in the lobby is the one his own PC gives. The client's coopConnect carries it to the host next to the XUID, and the host's party writes it into
// the member names (RVA_PARTY_MEMBER_NAMES, char[32] a member, stride 0x38), which the lobby's cards show. Where the game takes it:
//   - the Steam builds: ISteamFriends::GetPersonaName (1.0.159: VA 0x492ef0 calls SteamFriends() through its import and the interface's first method). The method is
//     replaced on the interface's table.
//   - the iw4x-sp client: its own name function (it replaces the game's) gives the dvar "name", "Unknown Soldier" unless the player set it; the dvar is kept to the name.
// The name (owner: "joined through Steam: the Steam name by default; LAN: Player 1 / Player 2, changeable"):
//   - the nickname set in the lobby (player.ini, [player] name);
//   - else, in a session made through Steam, the Steam name;
//   - else "Player 1" (the host, or outside a lobby) or "Player 2" (a player in someone else's lobby). A LAN session: a join by address or from the list of games on the
//     network (mw2coop_join), the partner's word for it on the host, and always on the iw4x-sp client (no Steam lobby there).
// The lobby is kept to it as well: this PC's own slot gets the name, and the partner gets it (mw2coop_nick <L|S> <name>) whenever it changes; his name goes into his slot
// here. (The game sends the name only once, in the client's coopConnect.)
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "game.h"
#include "log.h"
#include "net.h"
#include "nick.h"
#include "settings.h"

#ifdef _WIN64
#define THISCALL
#define STEAM_DLL "steam_api64.dll"
#define NICK_CLS_STATE XR(0x533cb8) // int: the client's connection state (native.c RVA_CLS_STATE)
#else
#define THISCALL __attribute__((thiscall))
#define STEAM_DLL "steam_api.dll"
#define NICK_CLS_STATE XR(0x525bc0)
#endif

#define NAME_LEN 32 // the party's char[32] (31 characters and the end)
#define SLOT_STRIDE 0x38
static const char unknown_soldier[] = "Unknown Soldier"; // the dvar "name"'s default (1.0.159 VA 0x47514c registers it)

static uint8_t *base;
static char nickname[NAME_LEN]; // player.ini [player] name, "" for the default
static int loaded;
static char partner_name[NAME_LEN]; // what the partner said his name is, "" until he does
static int lan_client;		    // this PC joined its lobby by address (mw2coop_join)
static int lan_partner;		    // the partner said he joined by address
static char last_sent[NAME_LEN];    // the name the partner was told in this session
static int last_lan;		    // the last session that ended was a LAN one (kept for a rejoin of it)
static const char *(THISCALL *persona_original)(void *self);

// keeps what a name may hold: no control characters, and none of " \ ; that the console and the packets read as syntax
void nick_clean(const char *in, char *out, size_t size)
{
	size_t n = 0;
	while (*in == ' ')
		in++;
	for (; *in && n + 1 < size; in++) {
		const unsigned char c = (unsigned char)*in;
		if (c < 0x20 || c == '"' || c == '\\' || c == ';')
			continue;
		out[n++] = (char)c;
	}
	while (n > 0 && out[n - 1] == ' ')
		n--;
	out[n] = 0;
}

static void load(void)
{
	if (loaded)
		return;
	loaded = 1;
	char raw[128] = "";
	settings_player_name(raw, sizeof(raw));
	nick_clean(raw, nickname, sizeof(nickname));
	if (nickname[0])
		log_printf("nick: the nickname from player.ini is \"%s\"", nickname);
}

static int in_party(void)
{
	return base && base[RVA_PARTY_ACTIVE];
}
static int is_host(void)
{
	return base && base[RVA_PARTY_IS_HOST];
}
static int lan_session(void)
{
	return game_is_iw4x() || lan_client || lan_partner;
}

// the name this PC gives; own: what the game's interface answers (the Steam name); host: 1 / 0 the PC's part in a session, -1 the party's word for it
static const char *name_with(const char *own, int host)
{
	static char out[NAME_LEN];
	load();
	if (nickname[0])
		lstrcpynA(out, nickname, sizeof(out));
	else if (!lan_session() && own && own[0] && strcmp(own, unknown_soldier) != 0)
		nick_clean(own, out, sizeof(out));
	else
		lstrcpynA(out, (host < 0 ? in_party() && !is_host() : !host) ? "Player 2" : "Player 1", sizeof(out));
	return out;
}

static const char *name_for(const char *own)
{
	return name_with(own, -1);
}

static const char *THISCALL persona_hook(void *self)
{
	return name_for(persona_original ? persona_original(self) : NULL);
}

// The interface: on the iw4x-sp client the client's own, through the game's import of SteamFriends (1.0.159: the import slot VA 0x694528, the one the name function
// 0x492ef0 calls; the client fills it with its own export, and GetProcAddress cannot find that one: the headers at the image base are the game's, first test 2026-11-20),
// else steam_api(64).dll's.
#define IW4X_IMPORT_STEAMFRIENDS 0x694528u
static int executable(const void *p)
{
	MEMORY_BASIC_INFORMATION mi;
	return p && VirtualQuery(p, &mi, sizeof(mi)) && mi.State == MEM_COMMIT &&
	       (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}
static void *steam_friends(void)
{
	static void *(*get)(void);
	if (!get && game_is_iw4x()) {
		void *slot = *(void *volatile *)(uintptr_t)IW4X_IMPORT_STEAMFRIENDS;
		if (executable(slot))
			get = (void *(*)(void))slot;
	}
	if (!get) {
		HMODULE h = GetModuleHandleA(STEAM_DLL);
		get = h ? (void *(*)(void))GetProcAddress(h, "SteamFriends") : NULL;
	}
	return get ? get() : NULL;
}

static int install(void)
{
	static int done, tries;
	if (done)
		return 1;
	if (tries > 400) // (about 100 s of menu ticks: no Steam interface on this build; the lobby still keeps the names)
		return 0;
	if (tries++ == 400) {
		log_printf("nick: no SteamFriends interface: the game's own name stays (the lobby rows still use the nickname)");
		return 0;
	}
	void *friends = steam_friends();
	if (!friends)
		return 0;
	void **vt = *(void ***)friends;
	DWORD old;
	if (!vt || !VirtualProtect(&vt[0], sizeof(void *), PAGE_READWRITE, &old))
		return 0;
	if (vt[0] != (void *)persona_hook) {
		persona_original = (const char *(THISCALL *)(void *))vt[0];
		vt[0] = (void *)persona_hook;
	}
	VirtualProtect(&vt[0], sizeof(void *), old, &old);
	done = 1;
	log_printf("nick: the game's name comes from the mod (now \"%s\")", name_for(persona_original(friends)));
	return 1;
}

const char *nick_current(void)
{
	return name_for(persona_original ? persona_original(steam_friends()) : NULL);
}

const char *nick_in_session(int host)
{
	return name_with(persona_original ? persona_original(steam_friends()) : NULL, host ? 1 : 0);
}

static void write_slot(int slot, const char *name)
{
	char *at = (char *)base + RVA_PARTY_MEMBER_NAMES + slot * SLOT_STRIDE;
	if (at[0] && strncmp(at, name, NAME_LEN - 1) != 0)
		lstrcpynA(at, name, NAME_LEN);
}

// the iw4x-sp client's name function reads the dvar "name": kept to the name (the coopConnect a join sends carries it)
static void sync_name_dvar(const char *name)
{
	if (!game_is_iw4x())
		return;
	char now[64] = "", cmd[96];
	game_dvar_raw("name", now, sizeof(now));
	if (!strcmp(now, name))
		return;
	snprintf(cmd, sizeof(cmd), "set name \"%s\"\n", name);
	game_cbuf_addtext(cmd);
}

static void send_to_partner(void)
{
	char msg[64];
	const char *name = nick_current();
	snprintf(msg, sizeof(msg), "mw2coop_nick %c %s", lan_client ? 'L' : 'S', name);
	net_send_mod(msg);
	lstrcpynA(last_sent, name, sizeof(last_sent));
}

void nick_set(const char *text)
{
	load();
	char clean[NAME_LEN];
	nick_clean(text ? text : "", clean, sizeof(clean));
	lstrcpynA(nickname, clean, sizeof(nickname));
	settings_set_player_name(nickname);
	log_printf("nick: nickname %s%s%s", nickname[0] ? "\"" : "", nickname[0] ? nickname : "cleared (the default name)", nickname[0] ? "\"" : "");
	sync_name_dvar(nick_current());
	if (in_party()) {
		write_slot(is_host() ? 0 : 1, nick_current());
		if (net_partner_known())
			send_to_partner();
	}
}

// party.c: this PC joins a lobby by address (a LAN session): before its connect, so the coopConnect carries the name of a LAN player
void nick_joined_by_address(void)
{
	lan_client = 1;
	log_printf("nick: joined by address: a LAN session (the default name is \"%s\")", nick_current());
	sync_name_dvar(nick_current());
}

// The rejoin after a break (menus.c, rejoin.c) joins the host by address as well, but it goes back into the session that broke: one made through Steam keeps the
// Steam names (owner's log 2026-11-26, Exodus: after the rejoin the partner's card and name tag said "Player 2" instead of his Steam name).
void nick_rejoined_by_address(void)
{
	if (lan_client || last_lan) {
		nick_joined_by_address();
		return;
	}
	log_printf("nick: back by address in a session made through Steam: the names stay (\"%s\")", nick_current());
}

// net.c: "mw2coop_nick <L|S> <name>" from the partner
void nick_on_packet(const char *text)
{
	if (!text[0] || text[1] != ' ')
		return;
	if (text[0] == 'L' && is_host() && !lan_partner) {
		lan_partner = 1;
		log_printf("nick: the partner joined by address: a LAN session (this PC's default name is \"%s\")", nick_current());
	}
	char clean[NAME_LEN];
	nick_clean(text + 2, clean, sizeof(clean));
	if (!clean[0])
		return;
	if (strcmp(clean, partner_name) != 0)
		log_printf("nick: the partner's name is \"%s\"", clean);
	lstrcpynA(partner_name, clean, sizeof(partner_name));
	if (in_party())
		write_slot(is_host() ? 1 : 0, partner_name);
}

// The menus' tick (main thread, every 250 ms while the menus are up)
void nick_tick(void)
{
	if (!base)
		base = (uint8_t *)GetModuleHandleA(NULL);
	install();
	static int adopted;
	if (!adopted && game_is_iw4x()) { // a name the player gave the iw4x-sp client himself (`name <nick>`, as INSTALL-x86.txt said until 2026-11-20) becomes the nickname
		char was[64] = "", clean[NAME_LEN];
		adopted = 1;
		load();
		game_dvar_raw("name", was, sizeof(was));
		nick_clean(was, clean, sizeof(clean));
		if (!nickname[0] && clean[0] && strcmp(clean, unknown_soldier) && strcmp(clean, "Player 1") && strcmp(clean, "Player 2")) {
			lstrcpynA(nickname, clean, sizeof(nickname));
			settings_set_player_name(nickname);
			log_printf("nick: the client's name \"%s\" is the nickname now", nickname);
		}
	}
	static DWORD idle_since; // out of every lobby and level since then: the session is over
	if (!in_party()) {
		if (*(volatile int *)(base + NICK_CLS_STATE) >= 3) // (a level: the party is not active there)
			idle_since = 0;
		else if (!idle_since)
			idle_since = GetTickCount() | 1;
		else if (GetTickCount() - idle_since > 2000 && (lan_client || lan_partner || partner_name[0] || last_sent[0])) {
			last_lan = lan_client || lan_partner;
			lan_client = lan_partner = 0;
			partner_name[0] = last_sent[0] = 0;
			log_printf("nick: the session is over (the default name is \"%s\" again)", nick_current());
		}
		sync_name_dvar(nick_current());
		return;
	}
	idle_since = 0;
	last_lan = lan_client || lan_partner; // (the kind of the session this PC is in, for a rejoin after a break)
	const char *mine = nick_current();
	sync_name_dvar(mine);
	write_slot(is_host() ? 0 : 1, mine);
	if (partner_name[0])
		write_slot(is_host() ? 1 : 0, partner_name);
	if (net_partner_known() && strcmp(mine, last_sent) != 0)
		send_to_partner();
}
