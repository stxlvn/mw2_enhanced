// Who may join a campaign lobby, and how a player gets into a locked one.
//
// Host: the lobby's "Access" row (a copy of Invite Friend on the free line under it) cycles
//   open -> Steam friends only -> password -> open. In password mode a "Password" row under
//   Friendly fire opens the text popup that sets it (owner, 2026-10-23: switching to password
//   mode must not ask for it at once; a separate button, and no start until it is set). Until a
//   password is set the lobby is locked for everyone but Steam friends and Start asks for it.
// - A Steam friend of the host always gets in: friends join through Steam invites. The XUID in a
//   coopConnect is the SteamID; ISteamFriends013::GetFriendRelationship (vtable slot 5) == 3.
// - open: anyone who finds the lobby on the network or knows the address.
// - password: others must send "mw2coop_auth <password>" from the address they connect from
//   first; the host keeps that address for 60 s.
// - Steam friends only: the lobby does not answer network queries and others are refused.
// A refused coopConnect never reaches the game; the host answers "coopReject <reason>".
//
// Host: the "Friendly fire" row under it (owner, 2026-10-22) toggles whether the players can hurt
//   each other; on by default (the game's own co-op lets them). The setting is the dvar
//   mw2coop_ff ("1"/"0"), sent to the client with coopLaunch (src/progress.c) and applied by
//   maps\_mw2coop::friendly_fire_setting() on both machines.
//
// Client: a locked lobby from the list, or an address typed in, opens a text popup
// ("mw2coop_edit", a copy of the create-profile popup with its edit field bound to the dvar
// mw2coop_edit). The same popup sets the host's password.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "access.h"
#include "game.h"
#include "localize.h"
#include "log.h"
#include "settings.h"
#include "steamnet.h"
#include "menus.h"
#include "menus_ui.h"
#include "net.h"
#include "nick.h"

enum { ACCESS_OPEN, ACCESS_PASSWORD, ACCESS_STEAM };
enum { EDIT_NONE, EDIT_HOST_PASSWORD, EDIT_JOIN_PASSWORD, EDIT_ADDRESS, EDIT_NICK };

static uint8_t *base;
static volatile LONG access_mode = ACCESS_OPEN;
static char password[33];
static volatile LONG hosting; // this PC holds a campaign lobby (set by the menu tick)
static volatile LONG friendly_fire = 1;

static volatile uint64_t rejoin_partner_xuid; // rejoin.c: the partner of a session that broke off, let in whatever the access (0: none)

void access_rejoin_partner(uint64_t xuid)
{
	rejoin_partner_xuid = xuid;
}

static int edit_mode;
static uint8_t edit_ip[4];
static uint16_t edit_port;

#define MAX_AUTH 8
typedef struct {
	uint8_t ip[4];
	uint16_t port;
	DWORD at;
} Auth;
static Auth auths[MAX_AUTH];
static CRITICAL_SECTION auth_lock;

// ---------------------------------------------------------------- Steam

// steam_friend(): steamnet.c (night audit 2026-11-17, NET-2: this file had its own copy that looked for steam_api64.dll only and called the method without the 32-bit
// thiscall convention, so in the 32-bit build "Steam friends only" refused every friend)

// ---------------------------------------------------------------- the text popup

static void open_edit(int mode, const char *title, const char *label)
{
	edit_mode = mode;
	localize_select("MW2COOP_EDIT_TITLE", title);
	localize_select("MW2COOP_EDIT_LABEL", label);
	game_cbuf_addtext("set mw2coop_edit \"\"\nopenmenu mw2coop_edit\n");
}

static void add_edit_menu(void)
{
	if (ui_find_menu("mw2coop_edit"))
		return;
	uint8_t *src = ui_find_menu("profile_create_popmenu");
	if (!src || ui_item_index(src, "createprofile") < 0 || ui_item_index(src, "ok") < 0)
		return;
	uint8_t *menu = ui_clone_menu(src, "mw2coop_edit");
	*(void **)(menu + MENU_ON_OPEN) = ui_script_set("\"setfocus\" \"createprofile\" ; ");
	*(void **)(menu + MENU_ON_CLOSE) = NULL;
	int count = *(int *)(menu + MENU_ITEM_COUNT);
	uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
	static const char ok[] = "\"play\" \"mouse_click\" ; \"close\" \"self\" ; \"exec\" \"mw2coop_edit_ok\" ; ";
	for (int i = 0; i < count; i++) {
		uint8_t *it = items[i];
		const char *name = *(const char **)it;
		const char *text = *(const char **)(it + ITEM_TEXT);
		if (text && strcmp(text, "@MENU_CREATE_PLAYER_PROFILE") == 0)
			*(const char **)(it + ITEM_TEXT) = "@MW2COOP_EDIT_TITLE";
		if (!name)
			continue;
		if (strcmp(name, "createprofile") == 0) {
			// a plain edit field (the profile name field is a file-name field, 16, which drops
			// '.' and ':') holding up to 32 characters, 22 shown at a time (editFieldDef +0x10 maxChars,
			// +0x18 maxPaintChars: the field scrolls with the cursor)
			*(int *)(it + ITEM_TYPE) = ITEM_TYPE_EDITFIELD;
			*(int *)(it + ITEM_DATA_TYPE) = ITEM_TYPE_EDITFIELD;
			uint8_t *field = *(uint8_t **)(it + ITEM_TYPE_DATA);
			if (field) {
				uint8_t *copy = HeapAlloc(GetProcessHeap(), 0, 0x20);
				memcpy(copy, field, 0x20);
				*(int *)(copy + 0x10) = 32; // maxChars
				*(int *)(copy + 0x18) = 22; // maxPaintChars: what fits in the box; longer text scrolls
				*(uint8_t **)(it + ITEM_TYPE_DATA) = copy;
			}
			*(const char **)(it + ITEM_DVAR) = "mw2coop_edit";
			*(const char **)(it + ITEM_TEXT) = "@MW2COOP_EDIT_LABEL";
			*(void **)(it + ITEM_ACCEPT) = ui_script_set(ok); // Enter in the field
		} else if (strcmp(name, "ok") == 0) {
			*(void **)(it + ITEM_ACTION) = ui_script_set(ok);
		} else if (strcmp(name, "cancel") == 0) {
			*(void **)(it + ITEM_ACTION) = ui_script_set(UI_S_CLICK_CLOSE);
		}
	}
	if (ui_register_menu(menu))
		log_printf("access: text popup added");
}

static int parse_address(const char *text, uint8_t ip[4], uint16_t *port)
{
	unsigned a, b, c, d, p = settings_port();
	char tail;
	int n = sscanf(text, " %u.%u.%u.%u:%u %c", &a, &b, &c, &d, &p, &tail);
	if ((n != 4 && n != 5) || a > 255 || b > 255 || c > 255 || d > 255 || p == 0 || p > 65535)
		return 0;
	ip[0] = (uint8_t)a, ip[1] = (uint8_t)b, ip[2] = (uint8_t)c, ip[3] = (uint8_t)d;
	*port = (uint16_t)p;
	return 1;
}

static void join_now(const uint8_t ip[4], uint16_t port)
{
	char cmd[64];
	snprintf(cmd, sizeof(cmd), "mw2coop_join %u.%u.%u.%u %u\n", ip[0], ip[1], ip[2], ip[3], port);
	log_printf("access: joining %u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], port);
	localize_select("MW2COOP_JOIN", "MW2COOP_JOIN_CONNECTING");
	game_cbuf_addtext(cmd);
}

// mw2coop_edit_ok: the popup's OK (or Enter)
static void edit_ok_f(void)
{
	char text[64] = "";
	game_dvar_raw("mw2coop_edit", text, sizeof(text));
	int mode = edit_mode;
	edit_mode = EDIT_NONE;
	game_cbuf_addtext("set mw2coop_edit \"\"\n"); // a password does not stay in a dvar
	if (mode == EDIT_HOST_PASSWORD) {
		if (!text[0])
			return; // no password given: the last one stays
		lstrcpynA(password, text, sizeof(password));
		log_printf("access: lobby password set");
	} else if (mode == EDIT_JOIN_PASSWORD) {
		char auth[64];
		snprintf(auth, sizeof(auth), "mw2coop_auth %s", text);
		net_send_mod_to(edit_ip, edit_port, auth); // before the join's coopConnect (next frame)
		join_now(edit_ip, edit_port);
	} else if (mode == EDIT_NICK) {
		nick_set(text); // ("" goes back to the default name)
	} else if (mode == EDIT_ADDRESS) {
		uint8_t ip[4];
		uint16_t port;
		if (!parse_address(text, ip, &port)) {
			log_printf("access: '%s' is not an address", text);
			return;
		}
		menus_find_at(ip, port); // learns whether it is locked, then access_join
	}
}

// mw2coop_nick_edit: the lobby's Nickname row (host and client, campaign and Spec Ops lobbies): the popup opens with the current nickname in its field
static void nick_edit_f(void)
{
	char now[40], cmd[96];
	nick_clean(nick_current(), now, sizeof(now));
	edit_mode = EDIT_NICK;
	localize_select("MW2COOP_EDIT_TITLE", "MW2COOP_EDIT_NICK");
	localize_select("MW2COOP_EDIT_LABEL", "MW2COOP_LABEL_NICK");
	snprintf(cmd, sizeof(cmd), "set mw2coop_edit \"%s\"\nopenmenu mw2coop_edit\n", now);
	game_cbuf_addtext(cmd);
}

// mw2coop_join_address: the list's last row
static void join_address_f(void)
{
	open_edit(EDIT_ADDRESS, "MW2COOP_EDIT_ADDRESS", "MW2COOP_LABEL_ADDRESS");
}

void access_join(const uint8_t ip[4], uint16_t port, int locked)
{
	if (!locked) {
		join_now(ip, port);
		return;
	}
	memcpy(edit_ip, ip, 4);
	edit_port = port;
	open_edit(EDIT_JOIN_PASSWORD, "MW2COOP_EDIT_PASSWORD", "MW2COOP_LABEL_PASSWORD");
}

// ---------------------------------------------------------------- the host's lobby

// mw2coop_access: the lobby's Access row
static void access_f(void)
{
	if (!base[RVA_PARTY_IS_HOST])
		return;
	switch (access_mode) {
	case ACCESS_OPEN:
		InterlockedExchange(&access_mode, ACCESS_STEAM);
		log_printf("access: lobby for Steam friends only");
		break;
	case ACCESS_STEAM:
		InterlockedExchange(&access_mode, ACCESS_PASSWORD);
		log_printf("access: lobby by password (%s)", password[0] ? "set" : "not set yet");
		break;
	default:
		InterlockedExchange(&access_mode, ACCESS_OPEN);
		password[0] = 0;
		log_printf("access: lobby open");
		break;
	}
}

// mw2coop_password: the lobby's Password row (shown in password mode only)
static void password_f(void)
{
	if (!base[RVA_PARTY_IS_HOST] || access_mode != ACCESS_PASSWORD)
		return;
	open_edit(EDIT_HOST_PASSWORD, "MW2COOP_EDIT_PASSWORD", "MW2COOP_LABEL_PASSWORD");
}

// Start in the host's lobby: not in password mode without a password (the popup opens instead).
int access_can_start(void)
{
	if (access_mode != ACCESS_PASSWORD || password[0])
		return 1;
	log_printf("access: Start: set the password first");
	password_f();
	return 0;
}

// mw2coop_ff: the lobby's Friendly fire row
static void ff_f(void)
{
	if (!base[RVA_PARTY_IS_HOST])
		return;
	InterlockedExchange(&friendly_fire, !friendly_fire);
	game_cbuf_addtext(friendly_fire ? "set mw2coop_ff 1\n" : "set mw2coop_ff 0\n");
	log_printf("access: friendly fire between players %s", friendly_fire ? "on" : "off");
}

static uint8_t *lobby_row(uint8_t *from, uint8_t *menu, const char *name, const char *text, const char *action, float y)
{
	uint8_t *row = ui_clone_item(from, menu);
	*(const char **)row = name;
	*(const char **)(row + ITEM_TEXT) = text;
	*(void **)(row + ITEM_ACTION) = ui_script_set(action);
	*(void **)(row + ITEM_ON_FOCUS) =
		ui_script_set(UI_S_HOVER);
	*(void **)(row + ITEM_LEAVE_FOCUS) = ui_script_set(UI_S_LEAVE);
	*(float *)(row + ITEM_RECT_Y) = y;
	*(float *)(row + ITEM_RECT_CLIENT_Y) = y;
	return row;
}

// The Nickname, Access and Friendly fire rows: inserted into coop_lobby's items. The Nickname row (nick.c; owner, 2026-11-20) is in both of its lobbies, the campaign's
// and Spec Ops', for the host and the client alike, on the free line under Invite a friend; in the campaign lobby the host's rows follow it.
#define LOBBY_ROWS 6 // nickname, access, friendly fire, password, and the split screen lobby's two difficulty rows (shown by menus.c)
#define LOBBY_SHIFT_ROWS 4 // the lines the rows below move down (the first four rows; the difficulty rows only exist in the split screen lobby, which lays them out itself)
static uint8_t *lobby_menu, **native_items, **campaign_items, **specops_items, **spec_split_items, *password_row;

// The campaign lobby's texts beside the operation panel: right of the operation's name row (x 0..216) and of its picture, from the name's line down,
// left-aligned in a column of their own (aligned to the bottom like the panel). The player list on the right begins 280 units from the right edge.
#define SIDE_X 224.0f
#define SIDE_W 200.0f
static void side_panel(uint8_t *it, float y, float h, int wrap)
{
	float *rect = (float *)(it + ITEM_RECT), *client = (float *)(it + ITEM_RECT_CLIENT_Y - 4), *text_rect = (float *)(it + ITEM_TEXT_RECT);
	rect[0] = client[0] = SIDE_X;
	rect[1] = client[1] = y;
	rect[2] = client[2] = SIDE_W;
	rect[3] = client[3] = h;
	*(int *)(it + ITEM_TEXT_ALIGN) &= ~3; // left (the vertical part of the mode stays)
	*(float *)(it + ITEM_TEXT_ALIGN_X) = 0.0f;
	if (wrap)
		*(uint32_t *)(it + ITEM_STATIC_FLAGS) |= WINDOW_AUTOWRAPPED;
	text_rect[0] = text_rect[1] = text_rect[2] = text_rect[3] = 0.0f; // (the cached layout of the place it came from)
}

static int native_count;

static void patch_lobby_row(int campaign, int spec_split)
{
	uint8_t *menu = ui_find_menu("coop_lobby");
	if (!menu)
		return;
	uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
	if (menu != lobby_menu || (items != native_items && items != campaign_items && items != specops_items && items != spec_split_items)) {
		// first look, or the UI zone was reloaded
		int at = ui_item_index(menu, "invitefriend");
		if (at < 0)
			return;
		lobby_menu = menu;
		native_items = items;
		native_count = *(int *)(menu + MENU_ITEM_COUNT);
		float y = *(float *)(items[at] + ITEM_RECT_Y) + BUTTON_HEIGHT;
		uint8_t *nick = lobby_row(items[at], menu, "mw2coop_nick", "@MW2COOP_NICK",
					  "\"play\" \"mouse_click\" ; \"exec\" \"mw2coop_nick_edit\" ; ", y);
		*(void **)(nick + ITEM_FLOAT_EXPS) = NULL; // (a copy of the invite row, which is greyed out offline: this one is always on)
		*(void **)(nick + ITEM_DISABLED_EXP) = NULL;
		uint8_t *access = lobby_row(items[at], menu, "mw2coop_access", "@MW2COOP_ACCESS",
					    "\"play\" \"mouse_click\" ; \"exec\" \"mw2coop_access\" ; ", y + BUTTON_HEIGHT);
		uint8_t *ff = lobby_row(items[at], menu, "mw2coop_ff", "@MW2COOP_FF",
					"\"play\" \"mouse_click\" ; \"exec\" \"mw2coop_ff\" ; ", y + 2 * BUTTON_HEIGHT);
		password_row = lobby_row(items[at], menu, "mw2coop_password", "@MW2COOP_PW",
					 "\"play\" \"mouse_click\" ; \"exec\" \"mw2coop_password\" ; ", y + 3 * BUTTON_HEIGHT);
		uint8_t *skill1 = lobby_row(items[at], menu, "mw2coop_skill1", "@MW2COOP_SKILL1",
					    "\"play\" \"mouse_click\" ; \"open\" \"mw2coop_skill_p1\" ; ", y + 4 * BUTTON_HEIGHT);
		uint8_t *skill2 = lobby_row(items[at], menu, "mw2coop_skill2", "@MW2COOP_SKILL2",
					    "\"play\" \"mouse_click\" ; \"open\" \"mw2coop_skill_p2\" ; ", y + 5 * BUTTON_HEIGHT);
		campaign_items = HeapAlloc(GetProcessHeap(), 0, (native_count + LOBBY_ROWS) * sizeof(*campaign_items));
		memcpy(campaign_items, native_items, (at + 1) * sizeof(*campaign_items));
		campaign_items[at + 1] = nick;
		campaign_items[at + 2] = access;
		campaign_items[at + 3] = ff;
		campaign_items[at + 4] = password_row;
		campaign_items[at + 5] = skill1;
		campaign_items[at + 6] = skill2;
		memcpy(campaign_items + at + 7, native_items + at + 1, (native_count - at - 1) * sizeof(*campaign_items));
		// Spec Ops: the game's items with the Nickname row on the free line, nothing moves
		specops_items = HeapAlloc(GetProcessHeap(), 0, (native_count + 1) * sizeof(*specops_items));
		memcpy(specops_items, native_items, (at + 1) * sizeof(*specops_items));
		specops_items[at + 1] = nick;
		memcpy(specops_items + at + 2, native_items + at + 1, (native_count - at - 1) * sizeof(*specops_items));
		// a split screen Spec Ops lobby: also each player's difficulty row, after the game's difficulty rows in the list (menus.c puts them in that row's place and hides it)
		int diff_at = -1;
		for (int i = 0; i < native_count; i++) {
			const char *name = *(const char **)native_items[i];
			if (name && (!strcmp(name, "selectdiff") || !strcmp(name, "dont_selectdiff")))
				diff_at = i;
		}
		spec_split_items = HeapAlloc(GetProcessHeap(), 0, (native_count + 3) * sizeof(*spec_split_items));
		for (int i = 0, k = 0; i < native_count; i++) {
			spec_split_items[k++] = native_items[i];
			if (i == (diff_at >= 0 ? diff_at : at)) {
				spec_split_items[k++] = skill1;
				spec_split_items[k++] = skill2;
			}
			if (i == at)
				spec_split_items[k++] = nick;
		}
		// the Access, Friendly fire and Password rows take the line of the lobby's NAT type text
		// ("nattype", y 126) and the next two: the left column's items from that line down (aligned
		// top-left like the buttons) move three lines lower. The texts the lobby writes over the operation panel's top (aligned to the bottom: the focused
		// button's hint lines "description" -262 and "description2" -242, Start's status lines at -262) do not fit there any more:
		// moved down with the rows they lay on the operation's name and picture, and the mission's description (menus.c) on the Friendly fire and NAT type
		// rows (a player's report and the QA frames, 2026-11-26, 1920 x 1080). They go beside the operation panel, right of its name and picture (side_panel).
		// Copies, so the Spec Ops lobby keeps its layout.
		for (int i = at + LOBBY_ROWS + 1; i < native_count + LOBBY_ROWS; i++) {
			uint8_t *it = campaign_items[i];
			float iy = *(float *)(it + ITEM_RECT_Y);
			const char *name = *(const char **)it;
			int below = it[ITEM_RECT_ALIGN_V] == 1 && iy >= y + BUTTON_HEIGHT / 2 && iy <= 400;
			int hint = it[ITEM_RECT_ALIGN_V] == 3 && iy < -230;
			// the start's status lines ("needs one more player", "start the operation", "waiting for the host"): unnamed, at the hint's line with a text
			// of their own, shown while Start is focused in the description's place
			int status = hint && !name && *(void **)(it + ITEM_TEXT_EXP);
			if (it[ITEM_RECT_ALIGN_H] != 1 || !(below || hint))
				continue;
			uint8_t *moved = ui_clone_item(campaign_items[i], menu);
			if (status || (hint && name && !strcmp(name, "description")))
				side_panel(moved, -182.0f, 92.0f, 1);
			else if (hint && name && !strcmp(name, "description2"))
				side_panel(moved, -86.0f, 44.0f, 1);
			else {
				*(float *)(moved + ITEM_RECT_Y) += (LOBBY_SHIFT_ROWS - 1) * BUTTON_HEIGHT;
				*(float *)(moved + ITEM_RECT_CLIENT_Y) += (LOBBY_SHIFT_ROWS - 1) * BUTTON_HEIGHT;
			}
			campaign_items[i] = moved;
		}
	}
	uint8_t **want = campaign ? campaign_items : spec_split ? spec_split_items : specops_items;
	const int want_count = campaign ? native_count + LOBBY_ROWS : spec_split ? native_count + 3 : native_count + 1;
	if (items != want) {
		// shrink the count before the array goes back, grow it after the new one is in
		if (want_count < *(int *)(menu + MENU_ITEM_COUNT))
			*(int *)(menu + MENU_ITEM_COUNT) = want_count;
		*(uint8_t ***)(menu + MENU_ITEMS) = want;
		*(int *)(menu + MENU_ITEM_COUNT) = want_count;
	}
}

void access_tick(int campaign_lobby, int host, int spec_split)
{
	if (!base)
		return;
	add_edit_menu();
	patch_lobby_row(campaign_lobby, spec_split);
	nick_tick();
	{ // the Nickname row: "Nickname: <the name this PC gives>"
		const char *fmt = localize_text("MW2COOP_NICK_FMT");
		char row[96];
		snprintf(row, sizeof(row), fmt ? fmt : "Nickname: %s", nick_current());
		localize_set_text("MW2COOP_NICK", row);
	}
	InterlockedExchange(&hosting, campaign_lobby && host);
	if (!campaign_lobby) {
		// a new lobby starts open; the password is not kept
		if (access_mode != ACCESS_OPEN && !host) {
			InterlockedExchange(&access_mode, ACCESS_OPEN);
			password[0] = 0;
		}
		return;
	}
	// the Password row: shown to the host in password mode (dynamicFlags 4 = visible, 2 = focus)
	int show_password = host && access_mode == ACCESS_PASSWORD;
	if (password_row) {
		uint32_t *flags = (uint32_t *)(password_row + ITEM_DYNAMIC_FLAGS);
		*flags = show_password ? (*flags | 4) : (*flags & ~6u);
	}
	if (!host) {
		localize_set_text("MW2COOP_ACCESS", ""); // the host's settings are not ours to show
		localize_set_text("MW2COOP_FF", "");
		localize_set_text("MW2COOP_PW", "");
		return;
	}
	localize_select("MW2COOP_PW", password[0] ? "MW2COOP_PW_SET" : "MW2COOP_PW_UNSET");
	localize_select("MW2COOP_FF", friendly_fire ? "MW2COOP_FF_ON" : "MW2COOP_FF_OFF");
	char cur[8] = "";
	if (!game_dvar_raw("mw2coop_ff", cur, sizeof(cur)) || cur[0] != (friendly_fire ? '1' : '0'))
		game_cbuf_addtext(friendly_fire ? "set mw2coop_ff 1\n" : "set mw2coop_ff 0\n");
	static const char *const texts[] = {"MW2COOP_ACCESS_OPEN", "MW2COOP_ACCESS_PASSWORD", "MW2COOP_ACCESS_STEAM"};
	localize_select("MW2COOP_ACCESS", texts[access_mode]);
}

char access_lobby_flag(void)
{
	if (access_mode == ACCESS_STEAM)
		return 0;
	return access_mode == ACCESS_PASSWORD ? 'p' : '-';
}

// ---------------------------------------------------------------- packets (game's network thread)

static int authorized(const uint8_t ip[4], uint16_t port)
{
	int ok = 0;
	EnterCriticalSection(&auth_lock);
	for (int i = 0; i < MAX_AUTH; i++)
		if (auths[i].port && GetTickCount() - auths[i].at < 60000 && memcmp(auths[i].ip, ip, 4) == 0 &&
		    auths[i].port == port)
			ok = 1;
	LeaveCriticalSection(&auth_lock);
	return ok;
}

static void authorize(const uint8_t ip[4], uint16_t port)
{
	EnterCriticalSection(&auth_lock);
	// a free slot, else the oldest
	int slot = 0;
	for (int i = 0; i < MAX_AUTH; i++) {
		if (!auths[i].port) {
			slot = i;
			break;
		}
		if (GetTickCount() - auths[i].at > GetTickCount() - auths[slot].at)
			slot = i;
	}
	memcpy(auths[slot].ip, ip, 4);
	auths[slot].port = port;
	auths[slot].at = GetTickCount();
	LeaveCriticalSection(&auth_lock);
}

int access_on_packet(const char *buf, int len, const uint8_t ip[4], uint16_t port, char *reply, int reply_size,
		     int *reply_len)
{
	static const char auth[] = "\xff\xff\xff\xffmw2coop_auth ";
	int al = (int)sizeof(auth) - 1;
	if (len >= al && memcmp(buf, auth, al) == 0) {
		if (hosting && access_mode == ACCESS_PASSWORD) {
			// Guesses are limited (audit 2026-11-18, ACC-2: any number of guesses a second, each written to the log on the thread that reads the game's packets):
			// after 5 wrong ones in a minute an address is not answered for a minute; the log says it once.
			static struct {
				uint8_t ip[4];
				DWORD since;
				int wrong;
			} tries[8];
			static int next_try;
			const DWORD now = GetTickCount();
			int t = -1;
			for (int i = 0; i < 8; i++)
				if (memcmp(tries[i].ip, ip, 4) == 0 && tries[i].since)
					t = i;
			if (t >= 0 && now - tries[t].since > 60000)
				tries[t].since = 0, tries[t].wrong = 0, t = -1;
			if (t >= 0 && tries[t].wrong >= 5)
				return 1; // locked out for the rest of the minute
			int n = len - al;
			int ok = password[0] && n == (int)strlen(password) && memcmp(buf + al, password, n) == 0;
			if (ok)
				authorize(ip, port);
			else {
				if (t < 0) {
					t = next_try;
					next_try = (next_try + 1) % 8;
					memcpy(tries[t].ip, ip, 4);
					tries[t].since = now ? now : 1;
					tries[t].wrong = 0;
				}
				tries[t].wrong++;
			}
			if (ok || tries[t].wrong <= 5)
				log_printf("access: %s password from %u.%u.%u.%u:%u%s", ok ? "right" : "wrong", ip[0], ip[1], ip[2], ip[3], port,
					   !ok && tries[t].wrong == 5 ? " (5 wrong in a minute: not answered for a minute)" : "");
		}
		return 1;
	}
	if (!hosting || access_mode == ACCESS_OPEN)
		return 0;
	// "\xff\xff\xff\xff" "R" <2 bytes> "coopConnect\0" <protocol byte> <xuid, 8 bytes> ...
	static const char connect[] = "coopConnect";
	const char *at = NULL;
	for (int i = 4; i + (int)sizeof(connect) + 9 <= len && i < 16; i++)
		if (memcmp(buf + i, connect, sizeof(connect)) == 0)
			at = buf + i + sizeof(connect);
	if (!at)
		return 0;
	uint64_t xuid;
	memcpy(&xuid, at + 1, 8);
	if (steam_friend(xuid) || (access_mode == ACCESS_PASSWORD && authorized(ip, port)))
		return 0;
	if (rejoin_partner_xuid && xuid == rejoin_partner_xuid) { // the partner of the session that broke off comes back (rejoin.c): he got in once already
		static DWORD logged;
		if (GetTickCount() - logged > 5000) {
			logged = GetTickCount();
			log_printf("access: %u.%u.%u.%u:%u is the partner of the broken session (xuid %016llx): let in", ip[0], ip[1], ip[2], ip[3], port,
				   (unsigned long long)xuid);
		}
		return 0;
	}
	*reply_len = snprintf(reply, reply_size, "\xff\xff\xff\xff" "coopReject %s",
			      access_mode == ACCESS_STEAM ? "MW2COOP_FRIENDS_ONLY" : "MW2COOP_BAD_PASSWORD");
	static DWORD last_log;
	if (GetTickCount() - last_log > 2000) {
		last_log = GetTickCount();
		log_printf("access: refused %u.%u.%u.%u:%u (xuid %016llx, %s)", ip[0], ip[1], ip[2], ip[3], port,
			   (unsigned long long)xuid, access_mode == ACCESS_STEAM ? "not a Steam friend" : "no password");
	}
	return 1;
}

static cmd_function_t commands[] = {
	{NULL, "mw2coop_edit_ok", NULL, NULL, edit_ok_f CMD_NODE_FLAG},
	{NULL, "mw2coop_password", NULL, NULL, password_f CMD_NODE_FLAG},
	{NULL, "mw2coop_join_address", NULL, NULL, join_address_f CMD_NODE_FLAG},
	{NULL, "mw2coop_access", NULL, NULL, access_f CMD_NODE_FLAG},
	{NULL, "mw2coop_ff", NULL, NULL, ff_f CMD_NODE_FLAG},
	{NULL, "mw2coop_nick_edit", NULL, NULL, nick_edit_f CMD_NODE_FLAG},
};

void access_install(void)
{
	InitializeCriticalSection(&auth_lock);
	base = (uint8_t *)GetModuleHandleA(NULL);
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
		game_add_command(&commands[i]);
	game_cbuf_addtext("set mw2coop_edit \"\"\n");
}
