// Co-op in the game's own menus (no .bat files). This build has no menu text parser, so the
// zone's menus (ASSET_TYPE_MENU 26, as the UI has loaded them) are changed in memory:
//
// - The game's original main_selection remains the front door: its Campaign / Spec Ops /
//   Multiplayer picture cards, background, selection previews and logo rectangle are kept.
//   Settings, Game Credits, About and the native Quit button form one centered bottom row, all
//   using the original Quit button's styling. Each loaded main_selection instance is patched.
// - Campaign opens a clone of the game's native main_text page (Resume, New game, Mission Select,
//   a new divider, co-op / split-screen entries, the original divider, Statistics, Settings and Back).
//   It opens the game's own Spec Ops co-op lobby (coop_lobby: Steam friend invites, ready state,
//   Start) with a campaign mission: where the players stopped last (mw2coop\progress.txt), else
//   The Pit (trainer).
// - In that lobby "Change Op" opens the campaign mission list (levels) instead of the Spec Ops
//   list. The mission buttons end in the difficulty popup's "devmap <map>"; in the campaign lobby
//   the devmap command is caught and sets the lobby's mission instead.
//
// Layout of this build's structures (64-bit, found with memdump.c, see NOTES.md):
//   menuDef_t:  name +0, itemCount int +0xbc, onOpen +0xe0, onClose +0xf0, onESC +0xf8,
//               itemDef_t **items +0x168
//   itemDef_t (0x1e8 bytes): name +0, rect {x,y,w,h} +8, rectClient +0x1c, group +0x30,
//               dynamicFlags +0x50 (2 = has focus, 4 = visible), text +0xf0, parent menu +0x100,
//               action +0x128, onFocus +0x138, leaveFocus +0x140, floatExpressions +0x198, disabledExp +0x1a8
// (the 32-bit values are in menus_ui.h)
//   MenuEventHandlerSet {int count; MenuEventHandler **handlers}
//   MenuEventHandler {void *data; char type}  type 0: data is a menu script string
#include <windows.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fonts.h"
#include "scrdebug.h"
#include "game.h"
#include "pads.h"
#include "access.h"
#include "firewall.h"
#include "update.h"
#include "hook.h"
#include "ctexts.h"
#include "localize.h"
#include "devices.h"
#include "native.h"
#include "padicon.h"
#include "padopts.h"
#include "gfxopts.h"
#include "enhopts.h"
#include "log.h"
#include "missions.h"
#include "menutable.h"
#include "logo.h"
#include "settings.h"
#include "upnp.h"
#include "menus.h"
#include "menus_ui.h"
#include "net.h"
#include "progress.h"
#include "saves.h"
#include "ping.h"
#include "rejoin.h"
#include "report.h"
#include "weaponfov.h"
#include "voice.h"
#include "chat.h"
#include "steamnet.h"

// itemDef_t::textRect follows windowDef_t. The cached width/height must be invalidated
// when a cloned button is moved, otherwise its label keeps the source item's old layout.
#ifdef _WIN64
#define ITEM_TEXT_RECT 0xb0
#else
#define ITEM_TEXT_RECT 0xa4
#endif

#ifdef _WIN64
#define ITEM_STATIC_FLAGS 0x4c
#else
#define ITEM_STATIC_FLAGS 0x44
#endif

#define WINDOW_FLAG_DECORATION 0x00100000u
// rectClient has its own alignment bytes after its four floats. The UI's layout pass
// consults both sets; changing only rect.horzAlign is overwritten when the menu opens.
#define ITEM_RECT_CLIENT_ALIGN_H (ITEM_RECT_CLIENT_Y + 0x0c)
#define ITEM_RECT_CLIENT_ALIGN_V (ITEM_RECT_CLIENT_Y + 0x0d)

#ifdef _WIN64
#define RVA_UI_CONTEXT XR(0x1a3ab00) // uiContext_t (Menus_OpenByName's first argument, "openmenu" @0x24ca10)
#define UI_MENUS 0x38		 // menuDef_t *menus[]
#define UI_MENU_COUNT 0x1438	 // int
#define UI_MAX_MENUS 640	 // (0x1438 - 0x38) / 8

typedef struct {
	void *data;
	char type;
	char pad[7];
} MenuEventHandler;

typedef struct {
	int count;
	int pad;
	MenuEventHandler **handlers;
} MenuEventHandlerSet;
#else
// 32-bit: the uiContext_t at 0x191cef0 ("openmenu", 0x61ed60, passes it to Menus_OpenByName 0x465010); menus[640] at +0x38 and
// the count after them at +0xa38; handlers have 4-byte pointers (no padding)
#define RVA_UI_CONTEXT XRA(0x151cef0)
#define UI_MENUS 0x38
#define UI_MENU_COUNT 0xa38
#define UI_MAX_MENUS 640

typedef struct {
	void *data;
	char type;
	char pad[3];
} MenuEventHandler;

typedef struct {
	int count;
	MenuEventHandler **handlers;
} MenuEventHandlerSet;
#endif

static void campaign_loading_begin(void);
static void briefskip_f(void);
static uint8_t *base;
#define FN_SLOT_USED(lobby, i) ((char (*)(void *, int))(base + RVA_LOBBY_SLOT_USED))(lobby, i)

// The menus the UI has loaded (the list Menus_FindByName, rva 0x259d30, searches): no asset
// database call, which blocks while a zone loads (a control-thread lookup during a level load
// hung both games after the briefing).
uint8_t *ui_find_menu(const char *name)
{
	uint8_t *ui = base + RVA_UI_CONTEXT;
	int count = *(int *)(ui + UI_MENU_COUNT);
	uint8_t **menus = (uint8_t **)(ui + UI_MENUS);
	for (int i = 0; i < count && i < UI_MAX_MENUS; i++) {
		const char *n = menus[i] ? *(const char **)menus[i] : NULL;
		if (n && _stricmp(n, name) == 0) // (ASCII names: _stricmp, not the locale-aware lstrcmpiA that converts to UTF-16 on every call, audit 2026-11-18, menus F9)
			return menus[i];
	}
	return NULL;
}

// Every instance of the menu <name> in the list (a level's zone brings its own copy of the game's menus), up to <max>; the count.
int ui_menu_instances(const char *name, uint8_t **out, int max)
{
	uint8_t *ui = base + RVA_UI_CONTEXT;
	int count = *(int *)(ui + UI_MENU_COUNT);
	uint8_t **menus = (uint8_t **)(ui + UI_MENUS);
	int n = 0;
	for (int i = 0; i < count && i < UI_MAX_MENUS && n < max; i++) {
		const char *nm = menus[i] ? *(const char **)menus[i] : NULL;
		if (nm && _stricmp(nm, name) == 0)
			out[n++] = menus[i];
	}
	return n;
}

// The mod's own popups wait while it is open: the firewall question once opened on top of "the connection to the other player was lost" (2026-11-21, a client
// whose host had crashed: the client became the host of its own lobby, and the question covered the error's text).
int ui_error_popup_open(void)
{
	static const char *const names[] = {"error_popmenu", "error_popmenu_lobby", "error_popmenu_submenu"};
	for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
		uint8_t *inst[8];
		const int n = ui_menu_instances(names[k], inst, 8);
		for (int i = 0; i < n; i++)
			if (*(const uint32_t *)(inst[i] + ITEM_DYNAMIC_FLAGS) & WINDOW_VISIBLE)
				return 1;
	}
	return 0;
}

int ui_item_index(uint8_t *menu, const char *name)
{
	int count = *(int *)(menu + MENU_ITEM_COUNT);
	uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
	for (int i = 0; i < count; i++) {
		const char *n = *(const char **)items[i];
		if (n && strcmp(n, name) == 0)
			return i;
	}
	return -1;
}

// QA: logs what a menu item runs ("!menuaction <menu> <item>" in commands.txt): its action
// handlers, a script for type 0, else the handler type.
#ifdef _WIN64
static void log_statement(const char *label, const uint8_t *st);
// QA: a handler set with its conditions and nested sets (type 0 script, 1 if (expression + set), 2 else (set), others by type only)
static void log_handler_set(const char *label, const MenuEventHandlerSet *set, int depth)
{
	if (!set || IsBadReadPtr(set, sizeof(*set)) || depth > 6 || set->count < 1 || set->count > 24 || IsBadReadPtr(set->handlers, set->count * sizeof(void *)))
		return;
	for (int i = 0; i < set->count; i++) {
		MenuEventHandler *h = set->handlers[i];
		if (!h || IsBadReadPtr(h, 16))
			continue;
		if (h->type == 0 && (!h->data || IsBadStringPtrA((const char *)h->data, 8)))
			continue;
		if (h->type == 0 && h->data)
			log_printf("menus:   %*s%s script: %.1500s", depth * 2, "", label, (const char *)h->data);
		else if (h->type == 1 && h->data && !IsBadReadPtr(h->data, 16)) {
			log_printf("menus:   %*s%s if:", depth * 2, "", label);
			// (the layout of the conditional is not known: try the handler set at either place; log_handler_set only follows a readable one with a plausible count)
			log_handler_set(label, *(MenuEventHandlerSet **)h->data, depth + 1);
			log_handler_set(label, *(MenuEventHandlerSet **)((uint8_t *)h->data + 8), depth + 1);
		} else if (h->type == 2 && h->data) {
			log_printf("menus:   %*s%s else:", depth * 2, "", label);
			log_handler_set(label, (MenuEventHandlerSet *)h->data, depth + 1);
		} else if (h->type >= 3 && h->type <= 6 && h->data && !IsBadReadPtr(h->data, 16)) { // set local var: name, expression
			const char *nm = *(const char **)h->data;
			log_printf("menus:   %*s%s setLocalVar type %d '%s'", depth * 2, "", label, h->type, nm && !IsBadStringPtrA(nm, 40) ? nm : "?");
			const uint8_t *st = *(uint8_t **)((uint8_t *)h->data + 8);
			if (st && !IsBadReadPtr(st, 0x18))
				log_statement("value", st);
		} else
			log_printf("menus:   %*s%s handler type %d", depth * 2, "", label, h->type);
	}
}
#endif

#ifndef _WIN64
// QA (32 bits): a handler set's scripts, into its if (ConditionalScript: the set, then the expression) and else sets
static void log_action_set32(const MenuEventHandlerSet *set, int depth)
{
	if (!set || !game_mem_readable(set, sizeof(*set)) || depth > 4 || set->count < 1 || set->count > 24 ||
	    !game_mem_readable(set->handlers, set->count * sizeof(void *)))
		return;
	for (int i = 0; i < set->count; i++) {
		MenuEventHandler *h = set->handlers[i];
		if (!h || !game_mem_readable(h, sizeof(*h)))
			continue;
		if (h->type == 0 && h->data && game_string_readable((const char *)h->data, 8))
			log_printf("menus:   %*sscript: %.400s", depth * 2, "", (const char *)h->data);
		else if (h->type == 1 && h->data && game_mem_readable(h->data, 2 * sizeof(void *))) {
			log_printf("menus:   %*sif:", depth * 2, "");
			log_action_set32(*(MenuEventHandlerSet **)h->data, depth + 1);
		} else if (h->type == 2 && h->data) {
			log_printf("menus:   %*selse:", depth * 2, "");
			log_action_set32((MenuEventHandlerSet *)h->data, depth + 1);
		} else
			log_printf("menus:   %*shandler type %d", depth * 2, "", h->type);
	}
}
#endif

void menus_log_action(const char *menu_name, const char *item_name)
{
	uint8_t *menu = ui_find_menu(menu_name);
	int at = menu ? ui_item_index(menu, item_name) : -1;
	if (at < 0) {
		log_printf("menus: no item '%s' in menu '%s'", item_name, menu_name);
		return;
	}
	uint8_t *item = (*(uint8_t ***)(menu + MENU_ITEMS))[at];
	MenuEventHandlerSet *set = *(MenuEventHandlerSet **)(item + ITEM_ACTION);
	log_printf("menus: %s/%s action: %d handler(s)", menu_name, item_name, set ? set->count : 0);
#ifdef _WIN64
	log_handler_set("action", set, 0);
	log_handler_set("onFocus", *(MenuEventHandlerSet **)(item + ITEM_ON_FOCUS), 0);
	log_handler_set("leaveFocus", *(MenuEventHandlerSet **)(item + ITEM_LEAVE_FOCUS), 0);
#else
	log_action_set32(set, 0);
#endif
}

// QA: logs the names of the menus the UI has loaded ("!menus").
void menus_log_names(void)
{
	uint8_t *ui = base + RVA_UI_CONTEXT;
	int count = *(int *)(ui + UI_MENU_COUNT);
	uint8_t **menus = (uint8_t **)(ui + UI_MENUS);
	char line[600] = "";
	for (int i = 0; i < count && i < UI_MAX_MENUS; i++) {
		const char *n = menus[i] ? *(const char **)menus[i] : NULL;
		snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s ", n ? n : "-");
		if (strlen(line) > 480 || i == count - 1) {
			log_printf("menus: loaded: %s", line);
			line[0] = 0;
		}
	}
}

// QA: logs a menu's items ("!menuitems <menu>"): name, text, rect and its alignment.
#ifdef _WIN64
static void log_statement(const char *label, const uint8_t *st)
{
	int n = st ? *(const int *)st : 0;
	const uint8_t *ent = st ? *(uint8_t *const *)(st + 8) : NULL;
	if (!ent || n <= 0 || n > 64)
		return;
	char line[700];
	int len = snprintf(line, sizeof(line), "menus:     %s exp:", label);
	for (int t = 0; t < n && len < 640; t++) {
		const uint8_t *e = ent + 0x18 * t;
		if (*(const int *)e == 0)
			len += snprintf(line + len, sizeof(line) - len, " op%d", *(const int *)(e + 8));
		else {
			int dt = *(const int *)(e + 8);
			const char *str = *(const char *const *)(e + 0x10);
			if (dt == 2 && str && !IsBadStringPtrA(str, 40))
				len += snprintf(line + len, sizeof(line) - len, " \"%.40s\"", str);
			else if (dt == 0)
				len += snprintf(line + len, sizeof(line) - len, " %d", *(const int *)(e + 0x10));
			else if (dt == 1)
				len += snprintf(line + len, sizeof(line) - len, " %g", *(const float *)(e + 0x10));
			else
				len += snprintf(line + len, sizeof(line) - len, " <%d>", dt);
		}
	}
	{ // the static dvars the expression refers to (operator 24 takes the index of one): their names
		const uint8_t *sd = *(uint8_t *const *)(st + 0x10);
		if (sd && !IsBadReadPtr(sd, 0x20)) {
			int nd = *(const int *)(sd + 0x10);
			uint8_t *const *list = *(uint8_t *const *const *)(sd + 0x18);
			for (int t = 0; t + 1 < n && list && nd > 0 && nd < 4096; t++) {
				const uint8_t *a = ent + 0x18 * t, *b = a + 0x18;
				if (*(const int *)a == 0 && *(const int *)(a + 8) == 24 && *(const int *)b == 1 && *(const int *)(b + 8) == 0) {
					int idx = *(const int *)(b + 0x10);
					const char *dn = idx >= 0 && idx < nd && list[idx] && !IsBadReadPtr(list[idx], 16) ? *(const char *const *)(list[idx] + 8) : NULL;
					len += snprintf(line + len, sizeof(line) - len, "  [dvar %d = %s]", idx, dn && !IsBadStringPtrA(dn, 40) ? dn : "?");
				}
			}
		}
	}
	log_printf("%s", line);
}
#endif

// QA: every instance of the menu <name> in the list (zone reloads leave several) with the state of its items that have the material <mat>
void menus_log_instances(const char *name, const char *mat)
{
	uint8_t *ui = base + RVA_UI_CONTEXT;
	int count = *(int *)(ui + UI_MENU_COUNT);
	uint8_t **menus = (uint8_t **)(ui + UI_MENUS);
	for (int m = 0; m < count && m < UI_MAX_MENUS; m++) {
		const char *n = menus[m] ? *(const char **)menus[m] : NULL;
		if (!n || lstrcmpiA(n, name))
			continue;
		int ic = *(int *)(menus[m] + MENU_ITEM_COUNT);
		uint8_t **items = *(uint8_t ***)(menus[m] + MENU_ITEMS);
		log_printf("menus: instance %d of %s at %p: %d items, window flags %x", m, name, (void *)menus[m], ic, *(uint32_t *)(menus[m] + ITEM_DYNAMIC_FLAGS));
		for (int i = 0; i < ic; i++) {
			uint8_t *mt = *(uint8_t **)(items[i] + ITEM_BACKGROUND_PTR);
			const char *mn = mt && !IsBadReadPtr(mt, 8) ? *(const char **)mt : NULL;
			if (mn && !IsBadStringPtrA(mn, 40) && !strcmp(mn, mat))
				log_printf("menus:   [%d] %p flags %x visible exp %p rect %g %g %g %g", i, (void *)items[i], *(uint32_t *)(items[i] + ITEM_DYNAMIC_FLAGS), *(void **)(items[i] + (ITEM_FLOAT_EXPS + sizeof(void *))),
					   *(float *)(items[i] + ITEM_RECT), *(float *)(items[i] + ITEM_RECT + 4), *(float *)(items[i] + ITEM_RECT + 8), *(float *)(items[i] + ITEM_RECT + 12));
		}
	}
}

static int ci_has(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	for (; *hay; hay++)
		if (!_strnicmp(hay, needle, n))
			return 1;
	return 0;
}

// QA: every item of every loaded menu whose text or name contains `needle` (case blind)
void menus_find_text(const char *needle)
{
	uint8_t *ui = base + RVA_UI_CONTEXT;
	int count = *(int *)(ui + UI_MENU_COUNT);
	uint8_t **menus = (uint8_t **)(ui + UI_MENUS);
	for (int m = 0; m < count && m < UI_MAX_MENUS; m++) {
		if (!menus[m])
			continue;
		const char *mn = *(const char **)menus[m];
		int ic = *(int *)(menus[m] + MENU_ITEM_COUNT);
		uint8_t **items = *(uint8_t ***)(menus[m] + MENU_ITEMS);
		for (int i = 0; i < ic; i++) {
			const char *nm = *(const char **)items[i], *tx = *(const char **)(items[i] + ITEM_TEXT);
			uint8_t *mat = *(uint8_t **)(items[i] + ITEM_BACKGROUND_PTR);
			const char *mtn = mat && !IsBadReadPtr(mat, 8) ? *(const char **)mat : NULL;
			if (mtn && IsBadStringPtrA(mtn, 40))
				mtn = NULL;
			if ((nm && ci_has(nm, needle)) || (tx && ci_has(tx, needle)) || (mtn && ci_has(mtn, needle)))
				log_printf("menus: found in %s[%d]: name '%s' text '%s' material '%s'", mn ? mn : "?", i, nm ? nm : "-", tx ? tx : "-", mtn ? mtn : "-");
		}
	}
}

void menus_log_items(const char *menu_name)
{
	uint8_t *menu = ui_find_menu(menu_name);
	if (!menu) {
		log_printf("menus: no menu '%s'", menu_name);
		return;
	}
	static const int events[] = {MENU_ON_OPEN, MENU_ON_CLOSE};
	for (int e = 0; e < 2; e++) {
		MenuEventHandlerSet *set = *(MenuEventHandlerSet **)(menu + events[e]);
		for (int i = 0; set && i < set->count && i < 16; i++) {
			MenuEventHandler *h = set->handlers[i];
			if (h && h->type == 0 && h->data)
				log_printf("menus: %s %s script: %.400s", menu_name, e ? "onClose" : "onOpen", (const char *)h->data);
			else if (h)
				log_printf("menus: %s %s handler type %d", menu_name, e ? "onClose" : "onOpen", h->type);
		}
	}
	int count = *(int *)(menu + MENU_ITEM_COUNT);
	uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
	for (int i = 0; i < count; i++) {
		uint8_t *it = items[i];
		const char *name = *(const char **)it, *text = *(const char **)(it + ITEM_TEXT);
		float *r = (float *)(it + ITEM_RECT);
		float *client = (float *)(it + ITEM_RECT_CLIENT_Y - 4);
		float *text_rect = (float *)(it + ITEM_TEXT_RECT);
		log_printf("menus: %s[%d] %s '%s' rect %g %g %g %g client %g %g %g %g textRect %g %g %g %g align %d/%d clientAlign %d/%d",
			   menu_name, i, name ? name : "-", text ? text : "",
			   r[0], r[1], r[2], r[3], client[0], client[1], client[2], client[3],
			   text_rect[0], text_rect[1], text_rect[2], text_rect[3],
			   it[ITEM_RECT_ALIGN_H], it[ITEM_RECT_ALIGN_V],
			   it[ITEM_RECT_CLIENT_ALIGN_H], it[ITEM_RECT_CLIENT_ALIGN_V]);
		// what can lock or hide it, and what it runs (the campaign's mission buttons)
		MenuEventHandlerSet *act = *(MenuEventHandlerSet **)(it + ITEM_ACTION);
		const char *script = act && act->count > 0 && act->handlers[0] && act->handlers[0]->type == 0
					     ? (const char *)act->handlers[0]->data
					     : NULL;
		void *fexp = *(void **)(it + ITEM_FLOAT_EXPS);
		if (fexp || *(void **)(it + ITEM_DISABLED_EXP) || script)
			log_printf("menus:   floatExps %p disabledExp %p flags %x action %.200s", fexp,
				   *(void **)(it + ITEM_DISABLED_EXP), *(uint32_t *)(it + ITEM_DYNAMIC_FLAGS), script ? script : "-");
#ifdef _WIN64
		{ // QA: the text made by an expression ("exp text"), e.g. a popup title taken from a string of the game
			const uint8_t *te = *(uint8_t **)(it + ITEM_TEXT_EXP);
			if (te && !IsBadReadPtr(te, 0x18))
				log_statement("textExp", te);
		}
		{ // QA: the item's type and its background material
			uint8_t *mat = *(uint8_t **)(it + ITEM_BACKGROUND_PTR);
			const char *mn = mat && !IsBadReadPtr(mat, 8) ? *(const char **)mat : NULL;
			log_printf("menus:     type %d data %d material %s", *(int *)(it + ITEM_TYPE), *(int *)(it + ITEM_DATA_TYPE), mn && !IsBadStringPtrA(mn, 40) ? mn : "-");
			{
				const char *dv = *(const char **)(it + ITEM_DVAR);
				log_printf("menus:     dvar '%s'", dv && !IsBadStringPtrA(dv, 60) ? dv : "-");
				log_statement("text", *(uint8_t **)(it + ITEM_TEXT_EXP));
				log_statement("material", *(uint8_t **)(it + ITEM_MATERIAL_EXP_PTR));
			}
		}
#endif
#ifdef _WIN64
		{ // QA: the expressions of the item (the tokens: operand kind/value or operator code), to find what shows or hides it
			int fcount = *(int *)(it + ITEM_FLOAT_EXPS - 4); // (floatExpressionCount, right before the pointer: rva 0x254981 compares +0x194)
			log_statement("visible", *(uint8_t **)(it + ITEM_FLOAT_EXPS + 8));
			for (int k = 0; fexp && k < fcount && k < 12; k++)
				log_statement("float", *(uint8_t **)((uint8_t *)fexp + 16 * k + 8));
		}
#endif
	}
}

// A handler set running one menu script, e.g. "\"open\" \"levels\" ;" (the parsed form the
// zone stores: quoted tokens, commands ended by ';').
void *ui_script_set(const char *script)
{
	MenuEventHandlerSet *set = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*set));
	MenuEventHandler **list = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*list));
	MenuEventHandler *h = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*h));
	h->data = (void *)script;
	h->type = 0;
	list[0] = h;
	set->count = 1;
	set->handlers = list;
	return set;
}

void *ui_script_replace(const void *from_set, const char *from, const char *to)
{
	const MenuEventHandlerSet *src = from_set;
	if (!src || src->count <= 0)
		return NULL;
	MenuEventHandlerSet *set = HeapAlloc(GetProcessHeap(), 0, sizeof(*set));
	MenuEventHandler **list = HeapAlloc(GetProcessHeap(), 0, src->count * sizeof(*list));
	*set = *src;
	int replaced = 0;
	const size_t from_len = strlen(from), to_len = strlen(to);
	for (int i = 0; i < src->count; i++) {
		MenuEventHandler *h = HeapAlloc(GetProcessHeap(), 0, sizeof(*h));
		*h = *src->handlers[i];
		const char *s = h->type == 0 ? (const char *)h->data : NULL;
		if (s && strstr(s, from)) {
			size_t n = 0;
			for (const char *p = s; (p = strstr(p, from)); p += from_len)
				n++;
			char *out = HeapAlloc(GetProcessHeap(), 0, strlen(s) + n * to_len + 1), *o = out;
			for (const char *p = s, *hit; *p; p = hit + from_len) {
				hit = strstr(p, from);
				if (!hit) {
					strcpy(o, p);
					o += strlen(p);
					break;
				}
				memcpy(o, p, hit - p);
				o += hit - p;
				memcpy(o, to, to_len);
				o += to_len;
			}
			*o = 0;
			h->data = out;
			replaced++;
		}
		list[i] = h;
	}
	set->handlers = list;
	return replaced ? set : NULL;
}

uint8_t *ui_clone_item(uint8_t *from, uint8_t *parent_menu)
{
	uint8_t *it = HeapAlloc(GetProcessHeap(), 0, ITEM_SIZE);
	memcpy(it, from, ITEM_SIZE);
	*(uint8_t **)(it + ITEM_PARENT) = parent_menu;
	*(uint32_t *)(it + ITEM_DYNAMIC_FLAGS) &= ~WINDOW_HASFOCUS;
	return it;
}

uint8_t *ui_clone_menu(uint8_t *from, const char *name)
{
	uint8_t *menu = HeapAlloc(GetProcessHeap(), 0, MENU_SIZE);
	memcpy(menu, from, MENU_SIZE);
	*(const char **)menu = name;
	int count = *(int *)(from + MENU_ITEM_COUNT);
	uint8_t **src = *(uint8_t ***)(from + MENU_ITEMS);
	uint8_t **items = HeapAlloc(GetProcessHeap(), 0, count * sizeof(*items));
	for (int i = 0; i < count; i++)
		items[i] = ui_clone_item(src[i], menu);
	*(uint8_t ***)(menu + MENU_ITEMS) = items;
	return menu;
}

int ui_register_menu(uint8_t *menu)
{
	uint8_t *ui = base + RVA_UI_CONTEXT;
	int count = *(int *)(ui + UI_MENU_COUNT);
	if (count >= UI_MAX_MENUS)
		return 0;
	((uint8_t **)(ui + UI_MENUS))[count] = menu;
	*(int *)(ui + UI_MENU_COUNT) = count + 1;
	return 1;
}

// ---------------------------------------------------------------- campaign main menu button

static const char split_specops_action[] = "\"play\" \"mouse_click\" ; \"exec\" \"mw2coop_split_specops\" ; ";

static const char *item_material_name(const uint8_t *it)
{
	const char **mat = *(const char ***)(it + ITEM_BACKGROUND_PTR);
	return mat && !IsBadReadPtr(mat, sizeof(*mat)) && *mat && !IsBadStringPtrA(*mat, 16) ? *mat : "";
}

static int is_selection_logo(const char *name)
{
	return name && (!strcmp(name, "logo_cod2") || !strcmp(name, "mw2coop_logo_original") ||
		!strcmp(name, "mw2coop_logo_enhanced"));
}

// The About page (menus.menu, menuDef "mw2coop_about"): a page of its own, made from a copy of the game's main menu. From the top: the logo centered in the header, the credits
// (the itemDefs without an action: lines of text, centered), the four source links as icons (a strip of four pictures, mw2coop\about-icons.tga, in the place of the picture of one
// of the game's menu cards; the itemDefs with an action are the clickable places over the icons) and Back at the bottom. The icons are a copy of the logo's
// material with a texture of their own (icons_material), so nothing the game draws elsewhere changes; without them (ABOUT_ICONS 0, or the picture missing) the links are text buttons. Over the whole page lies the dark panel of the main menu.
#ifdef _WIN64
#define ITEM_FONT 0xd0 // int fontEnum, then textAlignMode (int), textalignx and textaligny (float), textscale (float)
#else
#define ITEM_FONT 0xc4
#endif
#define ITEM_TEXT_ALIGN (ITEM_FONT + 4)
#define ITEM_TEXT_ALIGN_X (ITEM_FONT + 8)
#define ITEM_TEXT_SCALE (ITEM_FONT + 16)
static void set_page_esc(uint8_t *page);
#define ALIGN_MIDDLE_CENTER 5
#define ITEM_TEXT_STYLE (ITEM_FONT + 0x14) // int textStyle after textscale: 3 = shadowed
#define TEXT_STYLE_SHADOWED 3
#define ABOUT_ICONS 1
#define TEXT_DX 44.0f // a centered text is drawn this many units left of where a picture with the same rectangle is (measured on 1920 x 1080), so its rectangle goes right by that

static void set_rect(uint8_t *it, float x, float y, float w, float h, int align_h, int align_v)
{
	float *rect = (float *)(it + ITEM_RECT), *client = (float *)(it + ITEM_RECT_CLIENT_Y - 4);
	rect[0] = x, rect[1] = y, rect[2] = client[2] = w, rect[3] = client[3] = h;
	client[0] = x + (align_h == 2 ? 320.0f : 0.0f); // (the engine puts a centered item's client rectangle at the center of the 640 x 480 screen)
	client[1] = y + (align_v == 2 ? 240.0f : 0.0f);
	it[ITEM_RECT_ALIGN_H] = (uint8_t)align_h, it[ITEM_RECT_ALIGN_V] = (uint8_t)align_v;
}

// Find the thin horizontal divider already used by the native main_text menu. The copy is
// placed between Mission Select and the mod's co-op entries, while the original divider remains
// after Split Screen and before Settings.
static uint8_t *find_campaign_divider(uint8_t **items, int count, float after_y, float before_y)
{
	uint8_t *best = NULL;
	float best_y = -100000.0f;
	for (int i = 0; i < count; i++) {
		uint8_t *it = items[i];
		const char *mat = item_material_name(it);
		const char *text = *(const char **)(it + ITEM_TEXT);
		float *r = (float *)(it + ITEM_RECT);
		float w = r[2] < 0.0f ? -r[2] : r[2];
		float h = r[3] < 0.0f ? -r[3] : r[3];
		int horizontal_line = (mat && (strstr(mat, "line") || strstr(mat, "gradient_fadein"))) ||
			(w >= 100.0f && h > 0.0f && w >= h * 8.0f);
		if (!horizontal_line || w < 100.0f || h <= 0.0f || h > 32.0f ||
		    r[1] < after_y - 0.5f || r[1] > before_y + 0.5f ||
		    *(MenuEventHandlerSet **)(it + ITEM_ACTION) || (text && text[0]))
			continue;
		// The divider nearest Settings is the one that currently separates the co-op group
		// from the lower native menu entries.
		if (r[1] > best_y) {
			best = it;
			best_y = r[1];
		}
	}
	return best;
}

static uint8_t *clone_campaign_divider(uint8_t *source, uint8_t *parent, float y)
{
	if (!source)
		return NULL;
	uint8_t *it = ui_clone_item(source, parent);
	float *r = (float *)(source + ITEM_RECT);
	float x = r[0], w = r[2], h = r[3] < 0.0f ? -r[3] : r[3];
	int align_h = source[ITEM_RECT_ALIGN_H], align_v = source[ITEM_RECT_ALIGN_V];
	*(MenuEventHandlerSet **)(it + ITEM_ACTION) = NULL;
	*(void **)(it + ITEM_ON_FOCUS) = NULL;
	*(void **)(it + ITEM_LEAVE_FOCUS) = NULL;
	*(const char **)(it + ITEM_TEXT) = "";
	*(void **)(it + ITEM_TEXT_EXP) = NULL;
	*(int *)(it + ITEM_FLOAT_EXPS - sizeof(void *)) = 0;
	*(void **)(it + ITEM_FLOAT_EXPS) = NULL;
	set_rect(it, x, y, w, h, align_h, align_v);
	return it;
}

static int name_in(const char *name, const char *const *list);
static void add_about_page(uint8_t *main_menu, int count, uint8_t *button)
{
	static const char *const gone[] = {"resume", "newgame", "newgame_warning", "levelselect", "options", "stats", "credits", "game_selection", "button_quit", NULL};
	if (ui_find_menu("mw2coop_about"))
		return;
	uint8_t *page = ui_clone_menu(main_menu, "mw2coop_about");
	uint8_t **pi = *(uint8_t ***)(page + MENU_ITEMS);
	int lines = 0, links = 0;
	for (int i = 0;; i++) {
		const MenuItem *def = menutable_item("mw2coop_about", i);
		if (!def)
			break;
		if (def->action[0])
			links++;
		else
			lines++;
	}
	uint8_t **list = HeapAlloc(GetProcessHeap(), 0, (count + lines + links + 4) * sizeof(*list));
	int n = 0;
	uint8_t *title_item = NULL, *logo_item = NULL;
	for (int i = 0; i < count; i++) {
		const char *name = *(const char **)pi[i], *text = *(const char **)(pi[i] + ITEM_TEXT), *mat = item_material_name(pi[i]);
		if (name_in(name, gone) || !strcmp(mat, "gradient_fadein"))
			continue;
		if (!strcmp(mat, "gradient_fadein_fadebottom")) // the dark panel over the whole page
			set_rect(pi[i], -427, -240, 854, 480, 2, 2);
		if (is_selection_logo(mat)) {
			logo_item = pi[i];
			void *variant = logo_material_variant(*(void **)(pi[i] + ITEM_BACKGROUND_PTR), 1);
			if (variant)
				*(void **)(pi[i] + ITEM_BACKGROUND_PTR) = variant;
			set_rect(pi[i], -144, 6, 288, 72, 2, 1);
		}
		if (text && !strcmp(text, "@MENU_SP_FOR_THE_RECORD")) {
			title_item = pi[i];
			*(const char **)(pi[i] + ITEM_TEXT) = ""; // (it is the template of the credits lines)
			*(void **)(pi[i] + ITEM_TEXT_EXP) = NULL;
		}
		list[n++] = pi[i];
	}
	// the credits: lines of text, centered, one under the other
	float y = 92;
	int k = 0;
	for (int i = 0;; i++) {
		const MenuItem *def = menutable_item("mw2coop_about", i);
		if (!def)
			break;
		if (def->action[0] || !title_item)
			continue;
		uint8_t *line = ui_clone_item(title_item, page);
		*(const char **)line = def->name;
		*(const char **)(line + ITEM_TEXT) = def->text;
		*(void **)(line + ITEM_TEXT_EXP) = NULL;
		const int heading = strstr(def->name, "head") != NULL;
		if (heading && k > 0)
			y += 8; // a little air before the second heading
		set_rect(line, -300 + TEXT_DX, y, 600, 16, 2, 1);
		*(int *)(line + ITEM_FONT) = heading ? 9 : 3; // a heading in the title font, the lines in the body font of the buttons
		*(int *)(line + ITEM_TEXT_ALIGN) = ALIGN_MIDDLE_CENTER;
		*(float *)(line + ITEM_TEXT_ALIGN_X) = 0;
		*(float *)(line + ITEM_TEXT_SCALE) = heading ? 0.45f : 0.3f;
		*(int *)(line + ITEM_TEXT_STYLE) = TEXT_STYLE_SHADOWED; // readable over the picture without a box behind
		y += heading ? 24 : 17;
		k++;
		list[n++] = line;
	}
	// the icons: one picture of four, 40 x 40 units each, with a button over each
	float icons_y = y + 63; // under the credits
	void *icons = ABOUT_ICONS && logo_item ? icons_material(*(void **)(logo_item + ITEM_BACKGROUND_PTR)) : NULL; // a copy of the logo's material with the icons as its picture
	if (icons) {
		uint8_t *strip = ui_clone_item(logo_item, page);
		*(const char **)strip = "mw2coop_about_icons";
		*(void **)(strip + ITEM_BACKGROUND_PTR) = icons;
		set_rect(strip, -80, icons_y, 160, 40, 2, 1); // four icons of 40 x 40 units
		list[n++] = strip;
	}
	int b = 0;
	for (int i = 0;; i++) {
		const MenuItem *def = menutable_item("mw2coop_about", i);
		if (!def)
			break;
		if (!def->action[0])
			continue;
		uint8_t *hit = ui_clone_item(button, page);
		*(const char **)hit = def->name;
		*(const char **)(hit + ITEM_TEXT) = icons ? "" : def->text;
		*(void **)(hit + ITEM_TEXT_EXP) = NULL;
		*(MenuEventHandlerSet **)(hit + ITEM_ACTION) = ui_script_set(def->action);
		if (icons) {
			*(void **)(hit + ITEM_BACKGROUND_PTR) = NULL; // (a button's selection bar, and the black fill its focus script sets, would be drawn in the wrong place over an icon)
			*(void **)(hit + ITEM_ON_FOCUS) = NULL;
			*(void **)(hit + ITEM_LEAVE_FOCUS) = NULL;
			set_rect(hit, -80 + 40 * b++, icons_y, 40, 40, 2, 1);
		}
		else {
			set_rect(hit, -240 + TEXT_DX + 120 * b++, icons_y, 120, 20, 2, 1);
			*(int *)(hit + ITEM_TEXT_ALIGN) = ALIGN_MIDDLE_CENTER;
			*(float *)(hit + ITEM_TEXT_ALIGN_X) = 0;
		}
		list[n++] = hit;
	}
	// Back at the bottom
	uint8_t *back = ui_clone_item(button, page);
	*(const char **)back = "mw2coop_about_back";
	*(const char **)(back + ITEM_TEXT) = "@PLATFORM_BACK_CAPS";
	*(void **)(back + ITEM_TEXT_EXP) = NULL;
	*(MenuEventHandlerSet **)(back + ITEM_ACTION) = ui_script_set(UI_S_CLICK_CLOSE);
	set_rect(back, -100 + TEXT_DX, 420, 200, 20, 2, 1);
	*(int *)(back + ITEM_TEXT_ALIGN) = ALIGN_MIDDLE_CENTER;
	*(float *)(back + ITEM_TEXT_ALIGN_X) = 0;
	list[n++] = back;
	*(uint8_t ***)(page + MENU_ITEMS) = list;
	*(int *)(page + MENU_ITEM_COUNT) = n;
	set_page_esc(page);
	if (ui_register_menu(page))
		log_printf("menus: About page added (%d items, icons %s)", n, icons ? "on" : "no picture");
}

// The Multiplayer button on the iw4x-sp client (owner, 2026-11-20: the official game leads to the official multiplayer, iw4x-sp to iw4x's). The game's own action starts
// the official iw4mp.exe, which an iw4x-sp install does not play: its multiplayer is IW4x (github.com/iw4x), started by IW4x's launcher next to the game
// (iw4x-launcher.exe: it updates IW4x and asks which client the first time) or, in an older install, iw4x.exe. The first of them that is there is started from the game's
// folder and this game quits, as the game's own button does. Without either the button keeps the game's own action.
static const char *const iw4x_mp_exes[] = {"iw4x-launcher.exe", "iw4x.exe"};
static int iw4x_mp_path(char *out, size_t size)
{
	char dir[MAX_PATH];
	DWORD n = GetModuleFileNameA(NULL, dir, MAX_PATH);
	while (n > 0 && dir[n - 1] != '\\')
		n--;
	dir[n] = 0;
	for (size_t i = 0; i < sizeof(iw4x_mp_exes) / sizeof(iw4x_mp_exes[0]); i++) {
		snprintf(out, size, "%s%s", dir, iw4x_mp_exes[i]);
		if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES)
			return 1;
	}
	return 0;
}

static void iw4x_mp_f(void)
{
	char exe[MAX_PATH], dir[MAX_PATH], cmd[MAX_PATH + 2];
	if (!game_is_iw4x() || !iw4x_mp_path(exe, sizeof(exe))) {
		log_printf("menus: no IW4x launcher next to the game: Multiplayer does nothing here");
		return;
	}
	lstrcpynA(dir, exe, sizeof(dir));
	char *slash = strrchr(dir, '\\');
	if (slash)
		*slash = 0;
	snprintf(cmd, sizeof(cmd), "\"%s\"", exe);
	STARTUPINFOA si = {0};
	PROCESS_INFORMATION pi = {0};
	si.cb = sizeof(si);
	if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
		log_printf("menus: IW4x's multiplayer did not start (%s, error %lu)", exe, (unsigned long)GetLastError());
		return;
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	log_printf("menus: IW4x's multiplayer started (%s); this game quits", exe);
	game_cbuf_addtext("quit\n");
}

static int name_in(const char *name, const char *const *list)
{
	for (; *list; list++)
		if (name && !strcmp(name, *list))
			return 1;
	return 0;
}

// Build the mod's Campaign page by cloning the game's native main_text menu. The native menu
// remains untouched and is never used as the mod's front door; main_selection is the only main menu.
static void patch_main_menu(void)
{
	uint8_t *menu = ui_find_menu("main_text");
	if (!menu || ui_find_menu("mw2coop_campaign"))
		return;
	int count = *(int *)(menu + MENU_ITEM_COUNT);
	uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
	int at = ui_item_index(menu, "levelselect");
	int extra = 0;
	while (menutable_item("mw2coop_campaign", extra))
		extra++;
	if (!items || count <= 0 || count > 512 || at < 0 || !extra) {
		log_printf("menus: native main_text has no Mission Select template; Campaign page not added");
		return;
	}
	uint8_t *level = items[at];
	float level_y = *(float *)(level + ITEM_RECT_Y), options_y = 100000.0f;
	for (int i = 0; i < count; i++)
		if (!strcmp(*(const char **)items[i] ? *(const char **)items[i] : "", "options"))
			options_y = *(float *)(items[i] + ITEM_RECT_Y);
	uint8_t *camp = ui_clone_menu(menu, "mw2coop_campaign");
	if (!camp)
		return;
	uint8_t **ci = *(uint8_t ***)(camp + MENU_ITEMS);
	static const char *const campaign_gone[] = {"credits", "game_selection", "button_quit", NULL};
	const float campaign_group_gap = 8.0f;
	uint8_t *divider_source = find_campaign_divider(ci, count, level_y + BUTTON_HEIGHT, options_y);
	uint8_t *divider = divider_source ? clone_campaign_divider(divider_source, camp, level_y + BUTTON_HEIGHT + 3.0f) : NULL;
	uint8_t **cl = HeapAlloc(GetProcessHeap(), 0, (count + extra + 2) * sizeof(*cl));
	if (!cl)
		return;
	int n = 0;
	for (int i = 0; i < count; i++) {
		const char *name = *(const char **)ci[i];
		if (name_in(name, campaign_gone) || is_selection_logo(item_material_name(ci[i])))
			continue;
		if (name && !strcmp(name, "options"))
			*(const char **)(ci[i] + ITEM_TEXT) = "@MW2COOP_SETTINGS";
		if (*(float *)(ci[i] + ITEM_RECT_Y) >= level_y + BUTTON_HEIGHT - 0.5f && *(float *)(ci[i] + ITEM_RECT_Y) < 1000) {
			*(float *)(ci[i] + ITEM_RECT_Y) += extra * BUTTON_HEIGHT + campaign_group_gap;
			*(float *)(ci[i] + ITEM_RECT_CLIENT_Y) += extra * BUTTON_HEIGHT + campaign_group_gap;
		}
		cl[n++] = ci[i];
		if (name && !strcmp(name, "levelselect")) {
			if (divider)
				cl[n++] = divider;
			for (int b = 0; b < extra; b++) {
				const MenuItem *def = menutable_item("mw2coop_campaign", b);
				uint8_t *item = ui_clone_item(level, camp);
				float y = level_y + (b + 1) * BUTTON_HEIGHT + campaign_group_gap;
				*(const char **)item = def->name;
				*(const char **)(item + ITEM_TEXT) = def->text;
				*(MenuEventHandlerSet **)(item + ITEM_ACTION) = ui_script_set(def->action);
				*(float *)(item + ITEM_RECT_Y) = y;
				*(float *)(item + ITEM_RECT_CLIENT_Y) = y;
				cl[n++] = item;
			}
		}
	}
	uint8_t *back = ui_clone_item(level, camp);
	float back_y = level_y + BUTTON_HEIGHT;
	for (int k = 0; k < n; k++) {
		const char *name = *(const char **)cl[k];
		if (name && (!strcmp(name, "stats") || !strcmp(name, "options"))) {
			float next_y = *(float *)(cl[k] + ITEM_RECT_Y) + BUTTON_HEIGHT;
			if (next_y > back_y)
				back_y = next_y;
		}
	}
	*(const char **)back = "mw2coop_campaign_back";
	*(const char **)(back + ITEM_TEXT) = "@PLATFORM_BACK_CAPS";
	*(MenuEventHandlerSet **)(back + ITEM_ACTION) = ui_script_set(UI_S_CLICK_CLOSE);
	*(float *)(back + ITEM_RECT_Y) = back_y;
	*(float *)(back + ITEM_RECT_CLIENT_Y) = back_y;
	cl[n++] = back;
	*(uint8_t ***)(camp + MENU_ITEMS) = cl;
	*(int *)(camp + MENU_ITEM_COUNT) = n;
	set_page_esc(camp);
	if (ui_register_menu(camp)) {
		add_about_page(menu, count, level);
		log_printf("menus: Campaign page cloned from native main_text (%d items, divider %s); native main_text left untouched", n, divider ? "added" : "not found");
	}
}

// Esc on a page of the mod closes it (the pages are copies of the main menu, whose Esc would ask whether to quit). onESC follows onClose in the menu (onOpen, onCloseRequest, onClose, onESC).
static void set_page_esc(uint8_t *page)
{
	*(void **)(page + MENU_ON_CLOSE + sizeof(void *)) = ui_script_set(UI_S_CLICK_CLOSE);
}

// Append one menu script to an event handler set while preserving handlers already supplied by the game.
static void append_menu_event(uint8_t *menu, size_t event_offset, const char *script)
{
	const MenuEventHandlerSet *old = *(MenuEventHandlerSet **)(menu + event_offset);
	int n = old && game_mem_readable(old, sizeof(*old)) && old->count > 0 && old->count < 64 &&
		old->handlers && game_mem_readable(old->handlers, (size_t)old->count * sizeof(void *)) ? old->count : 0;
	MenuEventHandlerSet *set = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*set));
	MenuEventHandler **list = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (n + 1) * sizeof(*list));
	if (!set || !list) {
		if (set)
			HeapFree(GetProcessHeap(), 0, set);
		if (list)
			HeapFree(GetProcessHeap(), 0, list);
		return;
	}
	for (int i = 0; i < n; i++)
		list[i] = old->handlers[i];
	MenuEventHandler *h = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*h));
	if (!h) {
		HeapFree(GetProcessHeap(), 0, list);
		HeapFree(GetProcessHeap(), 0, set);
		return;
	}
	h->data = (void *)script;
	h->type = 0;
	list[n] = h;
	set->count = n + 1;
	set->handlers = list;
	*(MenuEventHandlerSet **)(menu + event_offset) = set;
}

static void append_on_esc(uint8_t *menu, const char *script)
{
	append_menu_event(menu, MENU_ON_CLOSE + sizeof(void *), script);
}

// Replace a quoted menu name in a menu's script handlers. Used both for the Spec Ops Escape
// route and for the single-player pause menus whose stock scripts still target main_text.
static int menu_script_replace_target(MenuEventHandlerSet *set, int depth, const char *from, const char *to)
{
	const size_t from_len = strlen(from), to_len = strlen(to);
	if (!set || !game_mem_readable(set, sizeof(*set)) || depth > 4 || set->count < 1 || set->count > 24 || !game_mem_readable(set->handlers, set->count * sizeof(void *)))
		return 0;
	int done = 0;
	for (int i = 0; i < set->count; i++) {
		MenuEventHandler *h = set->handlers[i];
		if (!h || !game_mem_readable(h, sizeof(*h)))
			continue;
		if (h->type == 0) {
			if (!h->data || !game_string_readable((const char *)h->data, 8) || !strstr((const char *)h->data, from))
				continue;
			const char *src = h->data;
			char *out = HeapAlloc(GetProcessHeap(), 0, strlen(src) * (to_len > from_len ? to_len : 1) + 1);
			size_t n = 0;
			while (*src) {
				if (!strncmp(src, from, from_len)) {
					memcpy(out + n, to, to_len);
					n += to_len;
					src += from_len;
				} else
					out[n++] = *src++;
			}
			out[n] = 0;
			h->data = out;
			done++;
		} else if (h->type == 1 && h->data && game_mem_readable(h->data, 2 * sizeof(void *))) {
			// EVENT_IF: ConditionalScript {MenuEventHandlerSet *, Statement_s *} (a readable set at either place is followed). Both builds: the 32-bit build skipped
			// these, and its main_selection's Campaign card (an if script) kept opening the game's main_text instead of the mod's Campaign page (owner, 2026-11-24)
			done += menu_script_replace_target(*(MenuEventHandlerSet **)h->data, depth + 1, from, to);
			done += menu_script_replace_target(*(MenuEventHandlerSet **)((uint8_t *)h->data + sizeof(void *)), depth + 1, from, to);
		} else if (h->type == 2 && h->data) { // EVENT_ELSE: the else set itself
			done += menu_script_replace_target((MenuEventHandlerSet *)h->data, depth + 1, from, to);
		}
	}
	return done;
}

static int menu_script_has_target(MenuEventHandlerSet *set, int depth, const char *needle)
{
	if (!set || !game_mem_readable(set, sizeof(*set)) || depth > 4 || set->count < 1 || set->count > 24 ||
	    !game_mem_readable(set->handlers, set->count * sizeof(void *)))
		return 0;
	for (int i = 0; i < set->count; i++) {
		MenuEventHandler *h = set->handlers[i];
		if (!h || !game_mem_readable(h, sizeof(*h)))
			continue;
		if (h->type == 0) {
			const char *script = (const char *)h->data;
			if (script && game_string_readable(script, 8) && strstr(script, needle))
				return 1;
		}
		else if (h->type == 1 && h->data && game_mem_readable(h->data, 2 * sizeof(void *))) { // (EVENT_IF and EVENT_ELSE as in menu_script_replace_target)
			if (menu_script_has_target(*(MenuEventHandlerSet **)h->data, depth + 1, needle) ||
			    menu_script_has_target(*(MenuEventHandlerSet **)((uint8_t *)h->data + sizeof(void *)), depth + 1, needle))
				return 1;
		} else if (h->type == 2 && h->data && menu_script_has_target((MenuEventHandlerSet *)h->data, depth + 1, needle))
			return 1;
	}
	return 0;
}

static int menu_event_has_script(uint8_t *menu, size_t event_offset, const char *needle)
{
	const MenuEventHandlerSet *set = *(MenuEventHandlerSet **)(menu + event_offset);
	if (!set || !game_mem_readable(set, sizeof(*set)) || set->count < 1 || set->count > 64 ||
	    !set->handlers || !game_mem_readable(set->handlers, (size_t)set->count * sizeof(void *)))
		return 0;
	for (int i = 0; i < set->count; i++) {
		MenuEventHandler *h = set->handlers[i];
		if (!h || !game_mem_readable(h, sizeof(*h)) || h->type != 0 || !h->data)
			continue;
		const char *script = (const char *)h->data;
		if (!IsBadStringPtrA(script, 128) && strstr(script, needle))
			return 1;
	}
	return 0;
}

static void patch_specops_return(void)
{
	static const char *const esc_script =
		"\"play\" \"mouse_click\" ; \"close\" \"self\" ; \"open\" \"main_selection\" ; ";
	uint8_t *menus[64];
	int count = ui_menu_instances("main_specops", menus, 64);
	for (int i = 0; i < count; i++) {
		uint8_t *menu = menus[i];
		if (!menu || menu_event_has_script(menu, MENU_ON_CLOSE + sizeof(void *), "\"open\" \"main_selection\""))
			continue;
		// Some game builds open main_specops as a full-screen menu rather than a closeable overlay.
		// Append a direct Escape route so both cases land on the original card screen.
		append_on_esc(menu, esc_script);
		log_printf("menus: main_specops: appended Esc return to main_selection (instance %d/%d)", i + 1, count);
	}
}

// Retarget the two pause/quit warning menus to the original main_selection card screen. They are
// proactively patched on the game thread during the active level, then retried while paused if a
// particular UI zone loads either menu lazily.
static int pause_menu_main_selection_retarget(uint8_t *menu, int *has_route)
{
	static const size_t menu_events[] = {
		MENU_ON_OPEN,
		MENU_ON_OPEN + sizeof(void *), // onCloseRequest
		MENU_ON_CLOSE,
		MENU_ON_CLOSE + sizeof(void *) // onEsc
	};
	static const size_t item_events[] = {ITEM_ACTION, ITEM_ACCEPT, ITEM_ON_FOCUS, ITEM_LEAVE_FOCUS};
	int replaced = 0, route = 0;
	for (size_t i = 0; i < sizeof(menu_events) / sizeof(menu_events[0]); i++) {
		MenuEventHandlerSet *set = *(MenuEventHandlerSet **)(menu + menu_events[i]);
		replaced += menu_script_replace_target(set, 0, "\"main_text\"", "\"main_selection\"");
		replaced += menu_script_replace_target(set, 0, "openmenu main_text", "openmenu main_selection");
		if (i >= 2 && menu_script_has_target(set, 0, "main_selection"))
			route = 1; // only close / Esc handlers count as the menu's direct return route
	}
	int count = *(int *)(menu + MENU_ITEM_COUNT);
	uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
	if (count < 0 || count > 512 || !items || !game_mem_readable(items, (size_t)count * sizeof(*items))) {
		if (has_route)
			*has_route = route;
		return replaced;
	}
	for (int i = 0; i < count; i++) {
		uint8_t *item = items[i];
		if (!item || !game_mem_readable(item, ITEM_SIZE))
			continue;
		for (size_t e = 0; e < sizeof(item_events) / sizeof(item_events[0]); e++) {
			MenuEventHandlerSet *set = *(MenuEventHandlerSet **)(item + item_events[e]);
			replaced += menu_script_replace_target(set, 0, "\"main_text\"", "\"main_selection\"");
			replaced += menu_script_replace_target(set, 0, "openmenu main_text", "openmenu main_selection");
			if (e < 2 && menu_script_has_target(set, 0, "main_selection"))
				route = 1; // ACTION / ACCEPT only; focus and leave-focus are not exit routes
		}
	}
	if (has_route)
		*has_route = route;
	return replaced;
}

static int patch_pause_menu_main_selection_return(int *visible_route_mask)
{
	// Pause menus belong to the level UI zone, while main_selection belongs to the front-end
	// zone and is not necessarily registered yet. Script targets can still be redirected now;
	// requiring main_selection to be loaded here forced every return through the delayed fallback.
	static const char *const names[] = {"savegame_warning", "pausedmenu"};
	uint8_t *menus[64];
	int ready_mask = 0, visible_mask = 0;
	for (size_t m = 0; m < sizeof(names) / sizeof(names[0]); m++) {
		int count = ui_menu_instances(names[m], menus, 64);
		for (int i = 0; i < count; i++) {
			if (!menus[i])
				continue;
			int has_route = 0;
			int changed = pause_menu_main_selection_retarget(menus[i], &has_route);
			if (changed)
				log_printf("menus: %s: %d script target(s) now open main_selection", names[m], changed);
			if (has_route) {
				ready_mask |= 1 << m;
				if (*(uint32_t *)(menus[i] + ITEM_DYNAMIC_FLAGS) & WINDOW_VISIBLE)
					visible_mask |= 1 << m;
			}
		}
	}
	if (visible_route_mask)
		*visible_route_mask = visible_mask;
	return ready_mask; // bit 0: savegame_warning; bit 1: pausedmenu
}

// The background and client hitbox use the same centered geometry. Text is drawn by a separate
// decoration item at that same position, so it cannot become a second focus target or stretch
// independently of the native button.
static int selection_button_position(uint8_t *item, float screen_left, float width)
{
	const float draw_x = screen_left - 320.0f;
	const float client_x = draw_x;
	float *rect = (float *)(item + ITEM_RECT);
	float *client = (float *)(item + ITEM_RECT_CLIENT_Y - 4);
	float *text_rect = (float *)(item + ITEM_TEXT_RECT);
	int changed = rect[0] != draw_x || client[0] != client_x || rect[2] != width || client[2] != width ||
		item[ITEM_RECT_ALIGN_H] != 2 || item[ITEM_RECT_CLIENT_ALIGN_H] != 2 ||
		item[ITEM_RECT_ALIGN_V] != 3 || item[ITEM_RECT_CLIENT_ALIGN_V] != 3;
	if (!changed)
		return 0;
	rect[0] = draw_x;
	client[0] = client_x;
	rect[2] = client[2] = width;
	item[ITEM_RECT_ALIGN_H] = 2; // normal centered layout for text; no fullscreen stretch
	item[ITEM_RECT_CLIENT_ALIGN_H] = 2;
	item[ITEM_RECT_ALIGN_V] = 3; // VERTICAL_ALIGN_BOTTOM, as the native Quit row
	item[ITEM_RECT_CLIENT_ALIGN_V] = 3;
	// The background button intentionally has no text. Keep the cache invalid in case a native
	// menu reload left an old Quit label layout attached to this item.
	text_rect[0] = text_rect[1] = text_rect[2] = text_rect[3] = 0.0f;
	return 1;
}

static int selection_button_label_position(uint8_t *item, float screen_left, float width)
{
	const float draw_x = screen_left - 320.0f;
	const float client_x = draw_x; // both rectangles use HORIZONTAL_ALIGN_CENTER, so both X values are center-relative
	float *rect = (float *)(item + ITEM_RECT);
	float *client = (float *)(item + ITEM_RECT_CLIENT_Y - 4);
	float *text_rect = (float *)(item + ITEM_TEXT_RECT);
	int changed = rect[0] != draw_x || client[0] != client_x || rect[1] != 0.0f || client[1] != 0.0f ||
		rect[2] != width || client[2] != width || rect[3] != BUTTON_HEIGHT || client[3] != BUTTON_HEIGHT ||
		item[ITEM_RECT_ALIGN_H] != 2 || item[ITEM_RECT_CLIENT_ALIGN_H] != 2 ||
		item[ITEM_RECT_ALIGN_V] != 3 || item[ITEM_RECT_CLIENT_ALIGN_V] != 3;
	if (!changed)
		return 0;
	rect[0] = draw_x;
	client[0] = client_x;
	rect[1] = client[1] = 0.0f;
	rect[2] = client[2] = width;
	rect[3] = client[3] = BUTTON_HEIGHT;
	item[ITEM_RECT_ALIGN_H] = 2; // normal centered layout for text; no fullscreen stretch
	item[ITEM_RECT_CLIENT_ALIGN_H] = 2;
	item[ITEM_RECT_ALIGN_V] = 3;
	item[ITEM_RECT_CLIENT_ALIGN_V] = 3;
	text_rect[0] = text_rect[1] = text_rect[2] = text_rect[3] = 0.0f;
	return 1;
}

static void selection_button_clear_text(uint8_t *item)
{
	*(const char **)(item + ITEM_TEXT) = "";
	*(void **)(item + ITEM_TEXT_EXP) = NULL;
	float *text_rect = (float *)(item + ITEM_TEXT_RECT);
	text_rect[0] = text_rect[1] = text_rect[2] = text_rect[3] = 0.0f;
}

// Clone the native Quit item as a background-only clickable button. Its own text is moved to a
// companion decoration item at the same centered position, leaving all native hover/focus colors
// and click styling on the clickable background.
static uint8_t *selection_menu_button(uint8_t *quit_style, uint8_t *action_source, uint8_t *parent,
				      const char *name, const char *fallback_action,
				      float screen_left, float width)
{
	uint8_t *item = ui_clone_item(quit_style, parent);
	if (!item)
		return NULL;
	*(const char **)item = name;
	selection_button_clear_text(item);
	MenuEventHandlerSet *action = action_source ? *(MenuEventHandlerSet **)(action_source + ITEM_ACTION) : NULL;
	MenuEventHandlerSet *accept = action_source ? *(MenuEventHandlerSet **)(action_source + ITEM_ACCEPT) : NULL;
	if (!action)
		action = ui_script_set(fallback_action);
	if (!accept)
		accept = ui_script_set(fallback_action);
	*(MenuEventHandlerSet **)(item + ITEM_ACTION) = action;
	*(MenuEventHandlerSet **)(item + ITEM_ACCEPT) = accept;
	selection_button_position(item, screen_left, width);
	return item;
}

// A text-only overlay cloned from Quit: preserve its font, text scale, horizontal text alignment,
// foreground color and style, but no background or input handlers. Its draw and hit rectangles
// share the same centered geometry as the background button.
static uint8_t *selection_menu_button_label(uint8_t *quit_style, uint8_t *parent, const char *name,
					    const char *text, float screen_left, float width)
{
	uint8_t *item = ui_clone_item(quit_style, parent);
	if (!item)
		return NULL;
	*(const char **)item = name;
	*(const char **)(item + ITEM_TEXT) = text;
	*(void **)(item + ITEM_TEXT_EXP) = NULL;
	*(void **)(item + ITEM_BACKGROUND_PTR) = NULL;
	*(void **)(item + ITEM_MATERIAL_EXP_PTR) = NULL;
	*(void **)(item + ITEM_FLOAT_EXPS) = NULL;
	*(void **)(item + ITEM_DISABLED_EXP) = NULL;
	*(void **)(item + ITEM_DVAR) = NULL;
	*(void **)(item + ITEM_TYPE_DATA) = NULL;
	*(MenuEventHandlerSet **)(item + ITEM_ACTION) = NULL;
	*(MenuEventHandlerSet **)(item + ITEM_ACCEPT) = NULL;
	*(MenuEventHandlerSet **)(item + ITEM_ON_FOCUS) = NULL;
	*(MenuEventHandlerSet **)(item + ITEM_LEAVE_FOCUS) = NULL;
	*(int *)(item + ITEM_TYPE) = 0; // ordinary text/decoration item, not a second interactive button
	*(int *)(item + ITEM_DATA_TYPE) = 0;
	*(uint32_t *)(item + ITEM_DYNAMIC_FLAGS) = WINDOW_VISIBLE;
	*(uint32_t *)(item + ITEM_STATIC_FLAGS) |= WINDOW_FLAG_DECORATION;
	selection_button_label_position(item, screen_left, width);
	return item;
}

// Reassert the label's decoration-only state only when a newly loaded UI zone or another UI
// patch actually changed it. This runs from the 250 ms menu tick, so writing the same fields
// on every pass would add needless shared-memory traffic.
static int selection_button_label_state(uint8_t *item, const char *text)
{
	int changed = 0;
	if (*(const char **)(item + ITEM_TEXT) != text)
		*(const char **)(item + ITEM_TEXT) = text, changed = 1;
#define CLEAR_ITEM_POINTER(offset) do { \
	void **slot = (void **)(item + (offset)); \
	if (*slot) *slot = NULL, changed = 1; \
} while (0)
	CLEAR_ITEM_POINTER(ITEM_TEXT_EXP);
	CLEAR_ITEM_POINTER(ITEM_BACKGROUND_PTR);
	CLEAR_ITEM_POINTER(ITEM_MATERIAL_EXP_PTR);
	CLEAR_ITEM_POINTER(ITEM_FLOAT_EXPS);
	CLEAR_ITEM_POINTER(ITEM_DISABLED_EXP);
	CLEAR_ITEM_POINTER(ITEM_DVAR);
	CLEAR_ITEM_POINTER(ITEM_TYPE_DATA);
	CLEAR_ITEM_POINTER(ITEM_ACTION);
	CLEAR_ITEM_POINTER(ITEM_ACCEPT);
	CLEAR_ITEM_POINTER(ITEM_ON_FOCUS);
	CLEAR_ITEM_POINTER(ITEM_LEAVE_FOCUS);
#undef CLEAR_ITEM_POINTER
	if (*(int *)(item + ITEM_TYPE) != 0)
		*(int *)(item + ITEM_TYPE) = 0, changed = 1;
	if (*(int *)(item + ITEM_DATA_TYPE) != 0)
		*(int *)(item + ITEM_DATA_TYPE) = 0, changed = 1;
	if (*(uint32_t *)(item + ITEM_DYNAMIC_FLAGS) != WINDOW_VISIBLE)
		*(uint32_t *)(item + ITEM_DYNAMIC_FLAGS) = WINDOW_VISIBLE, changed = 1;
	uint32_t *static_flags = (uint32_t *)(item + ITEM_STATIC_FLAGS);
	if (!(*static_flags & WINDOW_FLAG_DECORATION))
		*static_flags |= WINDOW_FLAG_DECORATION, changed = 1;
	return changed;
}

// main_selection uses menu-local ui_highlighted_game, not a global dvar. Mirror the card focus
// into a mod-owned dvar and request the logo swap directly from the game's menu thread.
static int patch_selection_focus_handlers(uint8_t *sel)
{
	static const char *const cards[3] = {"game_so", "game_sp", "game_mp"};
	static const char *const scripts[3] = {
		"\"setdvar\" \"mw2coop_highlighted_game\" 1 ; ",
		"\"setdvar\" \"mw2coop_highlighted_game\" 2 ; ",
		"\"setdvar\" \"mw2coop_highlighted_game\" 3 ; "
	};
	int added = 0;
	for (int i = 0; i < 3; i++) {
		int at = ui_item_index(sel, cards[i]);
		if (at < 0)
			continue;
		uint8_t **items = *(uint8_t ***)(sel + MENU_ITEMS);
		uint8_t *item = items ? items[at] : NULL;
		if (item && !menu_event_has_script(item, ITEM_ON_FOCUS, "mw2coop_highlighted_game")) {
			append_menu_event(item, ITEM_ON_FOCUS, scripts[i]);
			added++;
		}
	}
	return added;
}

static void patch_selection_instance(uint8_t *sel, uint8_t *main)
{
	if (!sel)
		return;
	int count = *(int *)(sel + MENU_ITEM_COUNT);
	uint8_t **items = *(uint8_t ***)(sel + MENU_ITEMS);
	if (count <= 0 || count > 512 || !items)
		return;

	int sp_at = ui_item_index(sel, "game_sp");
	int quit_at = ui_item_index(sel, "quit");
	if (quit_at < 0)
		return;

	// Four equal 144-unit slots with 8-unit gaps: [20,164], [172,316], [324,468], [476,620].
	// The 600-unit group is centered in the 640-unit logical menu; backgrounds and labels use
	// matching centered rectangles, with labels marked as decoration-only.
	static const float button_width = 144.0f;
	static const float button_left[4] = {20.0f, 172.0f, 324.0f, 476.0f};
	static const char *const button_names[4] = {
		"mw2coop_selection_settings", "mw2coop_selection_credits", "mw2coop_selection_about", "quit"
	};
	static const char *const label_names[4] = {
		"mw2coop_selection_settings_label", "mw2coop_selection_credits_label",
		"mw2coop_selection_about_label", "mw2coop_selection_quit_label"
	};
	static const char *const label_texts[4] = {
		"@MENU_OPTIONS", "@MENU_CREDITS", "@EXE_SV_INFO_MOD", "@MENU_QUIT"
	};
	static const char *const fallback_actions[3] = {
		"\"play\" \"mouse_click\" ; \"open\" \"options\" ; ",
		"\"play\" \"mouse_click\" ; \"open\" \"credits\" ; ",
		"\"play\" \"mouse_click\" ; \"open\" \"mw2coop_about\" ; "
	};

	int campaign_route = 0;
	if (sp_at >= 0) {
		MenuEventHandlerSet *action = *(MenuEventHandlerSet **)(items[sp_at] + ITEM_ACTION);
		campaign_route = menu_script_replace_target(action, 0, "\"main_text\"", "\"mw2coop_campaign\"");
	}

	int found[3], missing = 0;
	for (int i = 0; i < 3; i++) {
		found[i] = ui_item_index(sel, button_names[i]);
		if (found[i] < 0)
			missing++;
	}

	// Use native Settings / Credits / About actions when the stock main_text page is loaded;
	// retain safe script fallbacks when it is not.
	uint8_t *sources[3] = {NULL, NULL, NULL};
	if (main) {
		int main_count = *(int *)(main + MENU_ITEM_COUNT);
		uint8_t **main_items = *(uint8_t ***)(main + MENU_ITEMS);
		if (main_count > 0 && main_count <= 512 && main_items) {
			static const char *const source_names[3] = {"options", "credits", "mw2coop_about_btn"};
			for (int i = 0; i < 3; i++) {
				int at = ui_item_index(main, source_names[i]);
				if (at >= 0)
					sources[i] = main_items[at];
			}
		}
	}

	int added = 0;
	if (missing) {
		uint8_t **list = HeapAlloc(GetProcessHeap(), 0, (count + missing) * sizeof(*list));
		if (list) {
			memcpy(list, items, count * sizeof(*list));
			int n = count;
			for (int i = 0; i < 3; i++) {
				if (found[i] >= 0)
					continue;
				uint8_t *button = selection_menu_button(items[quit_at], sources[i], sel, button_names[i],
									fallback_actions[i], button_left[i], button_width);
				if (button) {
					list[n++] = button;
					added++;
				}
			}
			if (added) {
				*(uint8_t ***)(sel + MENU_ITEMS) = list;
				*(int *)(sel + MENU_ITEM_COUNT) = n;
				items = list;
				count = n;
			} else
				HeapFree(GetProcessHeap(), 0, list);
		}
	}

	// The native Quit and the three added buttons are background/hitbox-only items. Their labels
	// are four independent text-only decorations, drawn after the backgrounds with normal scale.
	int layout_changed = 0;
	for (int i = 0; i < 4; i++) {
		int at = ui_item_index(sel, button_names[i]);
		if (at < 0)
			continue;
		items = *(uint8_t ***)(sel + MENU_ITEMS);
		uint8_t *button = items[at];
		const char *button_text = *(const char **)(button + ITEM_TEXT);
		if (!button_text || button_text[0]) {
			selection_button_clear_text(button);
			layout_changed = 1;
		}
		layout_changed |= selection_button_position(button, button_left[i], button_width);
	}

	int label_found[4], labels_missing = 0;
	for (int i = 0; i < 4; i++) {
		label_found[i] = ui_item_index(sel, label_names[i]);
		if (label_found[i] < 0)
			labels_missing++;
	}
	if (labels_missing) {
		items = *(uint8_t ***)(sel + MENU_ITEMS);
		count = *(int *)(sel + MENU_ITEM_COUNT);
		uint8_t **list = HeapAlloc(GetProcessHeap(), 0, (count + labels_missing) * sizeof(*list));
		if (list) {
			memcpy(list, items, count * sizeof(*list));
			int n = count;
			uint8_t *quit_style = items[ui_item_index(sel, "quit")];
			for (int i = 0; i < 4; i++) {
				if (label_found[i] >= 0)
					continue;
				uint8_t *label = selection_menu_button_label(quit_style, sel, label_names[i], label_texts[i],
										 button_left[i], button_width);
				if (label) {
					list[n++] = label;
					label_found[i] = n - 1;
					added++;
				}
			}
			if (n > count) {
				*(uint8_t ***)(sel + MENU_ITEMS) = list;
				*(int *)(sel + MENU_ITEM_COUNT) = n;
				items = list;
				count = n;
			} else
				HeapFree(GetProcessHeap(), 0, list);
		}
	}

	for (int i = 0; i < 4; i++) {
		int at = ui_item_index(sel, label_names[i]);
		if (at < 0)
			continue;
		items = *(uint8_t ***)(sel + MENU_ITEMS);
		uint8_t *label = items[at];
		layout_changed |= selection_button_label_state(label, label_texts[i]);
		layout_changed |= selection_button_label_position(label, button_left[i], button_width);
	}

	int focus_hooks = patch_selection_focus_handlers(sel);

	// Ensure both logo variants exist for this UI-zone texture. selection_logo_tick swaps the
	// selected material on the main thread, without changing the game's original texture.
	items = *(uint8_t ***)(sel + MENU_ITEMS);
	count = *(int *)(sel + MENU_ITEM_COUNT);
	for (int i = 0; i < count; i++) {
		uint8_t *item = items[i];
		if (is_selection_logo(item_material_name(item))) {
			void *material = *(void **)(item + ITEM_BACKGROUND_PTR);
			if (getenv("MW2COOP_LOGODUMP"))
				logo_install(material, 1);
			(void)logo_material_variant(material, 1);
			break;
		}
	}

	static uint8_t *logged_menus[64];
	static int logged_count;
	int first_visit = 1;
	for (int i = 0; i < logged_count; i++)
		if (logged_menus[i] == sel) {
			first_visit = 0;
			break;
		}
	if (first_visit && logged_count < 64)
		logged_menus[logged_count++] = sel;
	if (first_visit || layout_changed || added || focus_hooks || campaign_route) {
		int q = ui_item_index(sel, "quit");
		float q_draw = 0.0f, q_hit = 0.0f, q_width = 0.0f;
		int q_draw_align = -1, q_hit_align = -1;
		if (q >= 0) {
			items = *(uint8_t ***)(sel + MENU_ITEMS);
			float *r = (float *)(items[q] + ITEM_RECT);
			float *c = (float *)(items[q] + ITEM_RECT_CLIENT_Y - 4);
			q_draw = r[0]; q_hit = c[0]; q_width = r[2];
			q_draw_align = items[q][ITEM_RECT_ALIGN_H];
			q_hit_align = items[q][ITEM_RECT_CLIENT_ALIGN_H];
		}
		float draw_x[3] = {0.0f, 0.0f, 0.0f}, hit_x[3] = {0.0f, 0.0f, 0.0f};
		float label_x[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		int present[3] = {0, 0, 0}, draw_align[3] = {-1, -1, -1}, hit_align[3] = {-1, -1, -1};
		for (int i = 0; i < 3; i++) {
			int at = ui_item_index(sel, button_names[i]);
			if (at >= 0) {
				items = *(uint8_t ***)(sel + MENU_ITEMS);
				uint8_t *item = items[at];
				present[i] = 1;
				draw_x[i] = *(float *)(item + ITEM_RECT);
				hit_x[i] = *(float *)(item + ITEM_RECT_CLIENT_Y - 4);
				draw_align[i] = item[ITEM_RECT_ALIGN_H];
				hit_align[i] = item[ITEM_RECT_CLIENT_ALIGN_H];
			}
		}
		for (int i = 0; i < 4; i++) {
			int at = ui_item_index(sel, label_names[i]);
			if (at >= 0) {
				items = *(uint8_t ***)(sel + MENU_ITEMS);
				label_x[i] = *(float *)(items[at] + ITEM_RECT);
			}
		}
		int visible = (*(uint32_t *)(sel + ITEM_DYNAMIC_FLAGS) & WINDOW_VISIBLE) != 0;
		log_printf("menus: main_selection row instance %p visible %d items %d; Quit draw %.1f hit %.1f w %.1f align %d/%d; buttons present %d/%d/%d drawX %.1f/%.1f/%.1f hitX %.1f/%.1f/%.1f align %d/%d/%d client %d/%d/%d; labels %.1f/%.1f/%.1f/%.1f textAlign 2/2; changed %d added %d focusHooks %d campaign %d",
			   sel, visible, *(int *)(sel + MENU_ITEM_COUNT), q_draw, q_hit, q_width, q_draw_align, q_hit_align,
			   present[0], present[1], present[2], draw_x[0], draw_x[1], draw_x[2], hit_x[0], hit_x[1], hit_x[2],
			   draw_align[0], draw_align[1], draw_align[2], hit_align[0], hit_align[1], hit_align[2],
			   label_x[0], label_x[1], label_x[2], label_x[3], layout_changed, added, focus_hooks, campaign_route);
	}
}

static void patch_selection_menu(void)
{
	// Do not gate positioning on the generated Campaign/About pages: after a UI-zone transition
	// those pages may not have been registered yet, while main_selection is already visible.
	uint8_t *main = ui_find_menu("main_text");
	uint8_t *menus[64];
	int count = ui_menu_instances("main_selection", menus, 64);
	for (int i = 0; i < count; i++)
		patch_selection_instance(menus[i], main);
}

// The menu's ui_highlighted_game is a local variable, not a global dvar. Card onFocus handlers
// mirror their selected index to mw2coop_highlighted_game; the focused item is also a direct fallback.
static int selection_focused_card(uint8_t *menu)
{
	static const char *const names[3] = {"game_so", "game_sp", "game_mp"};
	int count = *(int *)(menu + MENU_ITEM_COUNT);
	uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
	if (count <= 0 || count > 512 || !items)
		return 0;
	for (int i = 0; i < 3; i++) {
		int at = ui_item_index(menu, names[i]);
		if (at < 0)
			continue;
		uint32_t flags = *(uint32_t *)(items[at] + ITEM_DYNAMIC_FLAGS);
		if ((flags & (WINDOW_VISIBLE | WINDOW_HASFOCUS)) == (WINDOW_VISIBLE | WINDOW_HASFOCUS))
			return i + 1;
	}
	return 0;
}

static void selection_logo_tick(void)
{
	char highlighted[16] = "";
	game_dvar_raw("mw2coop_highlighted_game", highlighted, sizeof(highlighted));
	int selected = highlighted[0] >= '1' && highlighted[0] <= '3' && !highlighted[1] ? highlighted[0] - '0' : 0;
	uint8_t *menus[64];
	int count = ui_menu_instances("main_selection", menus, 64);
	for (int i = 0; i < count; i++) {
		uint8_t *menu = menus[i];
		if (!menu || !(*(uint32_t *)(menu + ITEM_DYNAMIC_FLAGS) & WINDOW_VISIBLE))
			continue;
		int focused = selection_focused_card(menu);
		if (focused)
			selected = focused; // actual UI focus wins if a stale dvar survived a menu-zone change
	}
	static int last_selected = 2; // the default front-end focus is Campaign
	if (selected >= 1 && selected <= 3)
		last_selected = selected;
	static int last_logged_selection;
	if (last_selected != last_logged_selection) {
		static const char *const labels[4] = {"none", "Spec Ops", "Campaign", "Multiplayer"};
		log_printf("menus: main_selection focus is %s (local mirror dvar '%s')", labels[last_selected], highlighted);
		last_logged_selection = last_selected;
	}
	// the Enhanced logo on Campaign and Spec Ops, the game's original on Multiplayer (the mod is not in the multiplayer game; owner, 2026-11-24: kept as Vlad made it)
	const int enhanced = last_selected != 3;
	for (int i = 0; i < count; i++) {
		uint8_t *menu = menus[i];
		if (!menu || !(*(uint32_t *)(menu + ITEM_DYNAMIC_FLAGS) & WINDOW_VISIBLE))
			continue;
		int n = *(int *)(menu + MENU_ITEM_COUNT);
		uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
		if (n <= 0 || n > 512 || !items)
			continue;
		for (int j = 0; j < n; j++) {
			uint8_t *item = items[j];
			if (!item || !is_selection_logo(item_material_name(item)))
				continue;
			void *current = *(void **)(item + ITEM_BACKGROUND_PTR);
			void *variant = logo_material_variant(current, enhanced);
			if (variant && variant != current) {
				*(void **)(item + ITEM_BACKGROUND_PTR) = variant;
				log_printf("menus: main_selection logo switched to %s variant (selected card %d, dvar '%s')",
					enhanced ? "Enhanced" : "original", last_selected, highlighted);
			}
		}
	}
}

// Special Ops' menu (main_specops): a split screen button under its "two players online" one (the button is found by its text, so a game build that words it otherwise gets none: the log
// says so). It opens the same lobby as that button does, with the second player on this PC (mw2coop_split_specops).
static uint8_t **specops_items; // our items array for main_specops
static void patch_specops_menu(void)
{
	uint8_t *menu = ui_find_menu("main_specops");
	if (!menu)
		return;
	uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
	if (items == specops_items)
		return;
	int count = *(int *)(menu + MENU_ITEM_COUNT), at = -1;
	for (int i = 0; i < count; i++) {
		const char *name = *(const char **)items[i];
		const char *text = *(const char **)(items[i] + ITEM_TEXT);
		if (name && !strcmp(name, "mw2coop_specops_split"))
			return;
		if (at < 0 && ((text && strstr(text, "TWO_PLAYER_ONLINE")) || (name && strstr(name, "TWO_PLAYER_ONLINE"))))
			at = i;
	}
	specops_items = items;
	if (at < 0) {
		log_printf("menus: main_specops has no two players online button, no split screen button added");
		return;
	}
	uint8_t *src = items[at];
	float y = *(float *)(src + ITEM_RECT_Y) + BUTTON_HEIGHT;
	uint8_t **list = HeapAlloc(GetProcessHeap(), 0, (count + 1) * sizeof(*list));
	memcpy(list, items, (at + 1) * sizeof(*list));
	uint8_t *item = ui_clone_item(src, menu);
	*(const char **)item = "mw2coop_specops_split";
	*(const char **)(item + ITEM_TEXT) = "@MW2COOP_SPLITSCREEN";
	*(MenuEventHandlerSet **)(item + ITEM_ACTION) = ui_script_set(split_specops_action);
	*(uint32_t *)(item + ITEM_DYNAMIC_FLAGS) &= ~WINDOW_HASFOCUS;
	*(float *)(item + ITEM_RECT_Y) = y;
	*(float *)(item + ITEM_RECT_CLIENT_Y) = y;
	list[at + 1] = item;
	memcpy(list + at + 2, items + at + 1, (count - at - 1) * sizeof(*list));
	for (int i = at + 2; i < count + 1; i++) { // everything below moves down
		if (*(float *)(list[i] + ITEM_RECT_Y) >= y - 0.5f) {
			*(float *)(list[i] + ITEM_RECT_Y) += BUTTON_HEIGHT;
			*(float *)(list[i] + ITEM_RECT_CLIENT_Y) += BUTTON_HEIGHT;
		}
	}
	specops_items = list;
	*(uint8_t ***)(menu + MENU_ITEMS) = list;
	*(int *)(menu + MENU_ITEM_COUNT) = count + 1;
	log_printf("menus: main_specops: split screen button added (%d items)", count + 1);
}

// No Russian in Mission Select (owner, 2026-11-04): the PC game's Act I list holds Trainer, Roadkill, Cliffhanger and Favela but not the airport mission. A button for it is cloned from
// Cliffhanger's, put after it (the ones below move down), always selectable (it takes Trainer's visibility expression), and starts the level at once with the profile's difficulty
// (devmap airport; in a co-op pick the devmap hook above takes it into the lobby as for the other missions). Its picture and text come from the airport's own strings.
static uint8_t **act1_items; // our items array for popmenu_act1
static void patch_act1_menu(void)
{
	uint8_t *menu = ui_find_menu("popmenu_act1");
	if (!menu)
		return;
	uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
	if (items == act1_items)
		return;
	act1_items = items;
	int count = *(int *)(menu + MENU_ITEM_COUNT);
	int after = ui_item_index(menu, "@MENU_SP_SP_CLIFFHANGER"), first = ui_item_index(menu, "@MENU_SP_SP_TRAINER");
	if (after < 0 || first < 0 || ui_item_index(menu, "@MENU_SP_SP_AIRPORT") >= 0) {
		log_printf("menus: popmenu_act1 has no Cliffhanger button (or already has No Russian), none added");
		return;
	}
	uint8_t *src = items[after];
	float y = *(float *)(src + ITEM_RECT_Y) + BUTTON_HEIGHT;
	uint8_t **list = HeapAlloc(GetProcessHeap(), 0, (count + 1) * sizeof(*list));
	memcpy(list, items, (after + 1) * sizeof(*list));
	uint8_t *item = ui_clone_item(src, menu);
	*(const char **)item = "@MENU_SP_SP_AIRPORT";
	*(const char **)(item + ITEM_TEXT) = "@MENU_SP_SP_AIRPORT";
	*(void **)(item + ITEM_FLOAT_EXPS) = *(void **)(items[first] + ITEM_FLOAT_EXPS);
	*(float *)(item + ITEM_RECT_Y) = y;
	*(float *)(item + ITEM_RECT_CLIENT_Y) = y;
	*(MenuEventHandlerSet **)(item + ITEM_ACTION) =
		ui_script_set("\"play\" \"mouse_click\" ; \"setdvar\" \"credits\" 0 ; \"setdvar\" \"credits_active\" 0 ; \"exec\" \"devmap airport\" ; ");
	*(MenuEventHandlerSet **)(item + ITEM_ON_FOCUS) =
		ui_script_set("\"play\" \"mouse_over\" ; \"setItemColor\" \"self\" \"backcolor\" 0 0 0 1 ; \"setLocalVarBool\" \"ui_menuAButton\" 1 ; "
			      "\"setLocalVarString\" \"ui_level_name\" \"@MENU_SP_SP_AIRPORT\" ; \"setLocalVarString\" \"ui_level_levelshot\" \"levelshot_mw2_airport\" ; "
			      "\"setLocalVarString\" \"ui_level_desc\" \"@MENU_SP_SP_AIRPORT_DESC\" ; \"setLocalVarString\" \"ui_choicegroup\" \"CHOICE_GROUP\" ; ");
	list[after + 1] = item;
	memcpy(list + after + 2, items + after + 1, (count - after - 1) * sizeof(*list));
	for (int i = after + 2; i < count + 1; i++) { // the missions below move down
		const char *nm = *(const char **)list[i];
		if (nm && strncmp(nm, "@MENU_SP_SP_", 12) == 0 && *(float *)(list[i] + ITEM_RECT_Y) >= y - 0.5f) {
			*(float *)(list[i] + ITEM_RECT_Y) += BUTTON_HEIGHT;
			*(float *)(list[i] + ITEM_RECT_CLIENT_Y) += BUTTON_HEIGHT;
		}
	}
	act1_items = list;
	*(uint8_t ***)(menu + MENU_ITEMS) = list;
	*(int *)(menu + MENU_ITEM_COUNT) = count + 1;
	log_printf("menus: popmenu_act1: No Russian added after Cliffhanger (%d items)", count + 1);
}

// ---------------------------------------------------------------- joining a game on the network

// mw2coop_find: the campaign menu's join button. Asks every network the PC is on (LAN, Radmin
// VPN and the like: each adapter's broadcast address, net.c) for campaign lobbies for 2 s, then
// lists the hosts that answered in a popup ("mw2coop_lobbies", built from the difficulty popup);
// the player picks one (mw2coop_join_found <n>; a locked lobby asks for its password, access.c)
// or enters an address. A host answers while it holds a campaign lobby with a free slot that
// is open to the network: "mw2coop_here <host xuid> <p|-> <map> <host name>" (p: password; a
// host on several networks answers on each, the XUID keeps it to one row).
#define FIND_TAG "mw2coop_find"
#define HERE_TAG "mw2coop_here "
#define FIND_MS 2000
#define LIST_ROWS 4
#define MAX_LOBBIES (LIST_ROWS - 1) // the last row enters an address

typedef struct {
	uint8_t ip[4];
	uint16_t port;
	uint64_t xuid;
	int locked;
	char map[32];
	char name[48];
} Lobby;

static void show_row(uint8_t *menu, const char *row, int visible);
static CRITICAL_SECTION lobby_lock; // the packet thread adds, the main thread reads
static Lobby lobbies[MAX_LOBBIES];
static int lobby_count;
static volatile LONG finding;
static volatile DWORD find_started, find_sent, join_text_until;
static int find_target; // asking one address (typed in) instead of every network
static uint8_t target_ip[4];
static uint16_t target_port;

static void find_f(void)
{
	if (finding)
		return;
	EnterCriticalSection(&lobby_lock);
	lobby_count = 0;
	LeaveCriticalSection(&lobby_lock);
	find_started = GetTickCount();
	find_sent = 0;
	find_target = 0;
	InterlockedExchange(&finding, 1);
	localize_select("MW2COOP_JOIN", "MW2COOP_JOIN_SEARCHING");
	log_printf("menus: looking for campaign lobbies on the network");
}

// "Name - Mission" in the game's language (Windows-1251)
static void lobby_row(const Lobby *l, char *out, int size)
{
	char key[64] = "MENU_SP_SP_";
	int n = (int)strlen(key);
	for (const char *m = l->map; *m && n < (int)sizeof(key) - 1; m++)
		key[n++] = (char)((*m >= 'a' && *m <= 'z') ? *m - 32 : *m);
	key[n] = 0;
	const char *mission = localize_text(key);
	const char *lock = l->locked ? localize_text("MW2COOP_LOCKED") : NULL;
	snprintf(out, size, "%s - %s%s%s", l->name[0] ? l->name : "?", mission ? mission : l->map, lock ? " " : "",
		 lock ? lock : "");
}

void menus_find_at(const uint8_t ip[4], uint16_t port)
{
	find_f();
	memcpy(target_ip, ip, 4);
	target_port = port;
	find_target = 1;
}

static void find_tick(void)
{
	DWORD now = GetTickCount();
	if (finding) {
		if (now - find_started < FIND_MS) {
			if (!find_sent || now - find_sent >= 700) {
				find_sent = now;
				if (find_target)
					net_send_mod_to(target_ip, target_port, FIND_TAG);
				else
					net_broadcast_mod(FIND_TAG, settings_port());
			}
			return;
		}
		InterlockedExchange(&finding, 0);
		if (find_target) {
			// the address typed in: join it whether or not it answered (an answer tells if it is locked)
			EnterCriticalSection(&lobby_lock);
			int locked = lobby_count ? lobbies[0].locked : 0;
			LeaveCriticalSection(&lobby_lock);
			localize_select("MW2COOP_JOIN", "MW2COOP_JOIN_IDLE");
			access_join(target_ip, target_port, locked);
			return;
		}
		EnterCriticalSection(&lobby_lock);
		int count = lobby_count;
		for (int i = 0; i < MAX_LOBBIES; i++) {
			char alias[32], row[128] = "";
			snprintf(alias, sizeof(alias), "MW2COOP_LOBBY_%d", i + 1);
			if (i < count)
				lobby_row(&lobbies[i], row, sizeof(row));
			localize_set_text(alias, row);
		}
		LeaveCriticalSection(&lobby_lock);
		log_printf("menus: %d campaign lobb%s answered", count, count == 1 ? "y" : "ies");
		// rows without a lobby are hidden, so the focus starts on the first lobby or the address row
		uint8_t *list = ui_find_menu("mw2coop_lobbies");
		for (int i = 0; i < MAX_LOBBIES; i++) {
			char row[24];
			snprintf(row, sizeof(row), "mw2coop_lobby_%d", i + 1);
			show_row(list, row, i < count);
		}
		// the list opens even when nothing answered: its last row enters an address
		localize_select("MW2COOP_JOIN", count ? "MW2COOP_JOIN_IDLE" : "MW2COOP_JOIN_NONE");
		if (!count)
			join_text_until = now + 4000;
		game_cbuf_addtext("openmenu mw2coop_lobbies\n");
		return;
	}
	if (join_text_until && (int)(now - join_text_until) > 0) {
		join_text_until = 0;
		localize_select("MW2COOP_JOIN", "MW2COOP_JOIN_IDLE");
	}
}

// mw2coop_join_found <n>: a row of the list (1-based)
static void join_found_f(void)
{
	int n = atoi(game_cmd_argv(1)) - 1;
	Lobby l;
	EnterCriticalSection(&lobby_lock);
	int ok = n >= 0 && n < lobby_count;
	if (ok)
		l = lobbies[n];
	LeaveCriticalSection(&lobby_lock);
	if (!ok)
		return;
	if (!l.locked) {
		localize_select("MW2COOP_JOIN", "MW2COOP_JOIN_CONNECTING");
		join_text_until = GetTickCount() + 6000;
	}
	access_join(l.ip, l.port, l.locked);
}

// The list popup: a copy of the difficulty popup (title, four buttons, the description lines)
// with our texts and actions. It is added to the UI's menu list, which a UI zone reload rebuilds.

// A popup with a title and four rows, copied from the difficulty popup (its rows are the four
// difficulty buttons; the descriptions and the checkpoint warning are blanked).
static const char *const popup_rows[LIST_ROWS] = {"newgame_easy", "newgame_regular", "newgame_hardened", "newgame_veteran"};

static uint8_t *rows_popup(const char *name, const char *title, const char *const row_names[LIST_ROWS],
			   const char *const row_texts[LIST_ROWS], const char *const row_actions[LIST_ROWS])
{
	uint8_t *src = ui_find_menu("popmenu_difficulty");
	int first = src ? ui_item_index(src, popup_rows[0]) : -1;
	if (first < 0)
		return NULL;
	uint8_t *menu = ui_clone_menu(src, name);
	*(void **)(menu + MENU_ON_OPEN) = ui_script_set("\"focusfirst\" ; ");
	int count = *(int *)(menu + MENU_ITEM_COUNT);
	uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
	for (int i = 0; i < count; i++) {
		uint8_t *it = items[i];
		const char *text = *(const char **)(it + ITEM_TEXT);
		if (text && strcmp(text, "@MENU_SELECT_DIFFICULTY") == 0)
			*(const char **)(it + ITEM_TEXT) = title;
		else if (i > first - 6 && i < first) {
			*(const char **)(it + ITEM_TEXT) = "";
			*(void **)(it + ITEM_TEXT_EXP) = NULL;
		}
	}
	for (int r = 0; r < LIST_ROWS; r++) {
		int i = ui_item_index(src, popup_rows[r]);
		if (i < 0)
			continue;
		uint8_t *it = items[i];
		*(const char **)it = row_names[r];
		*(const char **)(it + ITEM_TEXT) = row_texts[r];
		*(void **)(it + ITEM_ACTION) = ui_script_set(row_actions[r]);
		*(void **)(it + ITEM_ON_FOCUS) =
			ui_script_set(UI_S_HOVER);
		*(void **)(it + ITEM_LEAVE_FOCUS) =
			ui_script_set(UI_S_LEAVE);
	}
	return ui_register_menu(menu) ? menu : NULL;
}

// A popup whose heading and four rows come from menus.menu (menutable.c)
static uint8_t *table_popup(const char *name)
{
	const char *names[LIST_ROWS], *texts[LIST_ROWS], *actions[LIST_ROWS];
	for (int r = 0; r < LIST_ROWS; r++) {
		const MenuItem *it = menutable_item(name, r);
		if (!it) {
			log_printf("menus: menus.menu has no item %d for the popup %s", r + 1, name);
			return NULL;
		}
		names[r] = it->name;
		texts[r] = it->text;
		actions[r] = it->action;
	}
	const char *title = menutable_title(name);
	return rows_popup(name, title ? title : "", names, texts, actions);
}

// show or hide a popup row (WINDOW_VISIBLE in dynamicFlags; a hidden row cannot have the focus)
static void show_row(uint8_t *menu, const char *row, int visible)
{
	int at = menu ? ui_item_index(menu, row) : -1;
	if (at < 0)
		return;
	uint32_t *flags = (uint32_t *)((*(uint8_t ***)(menu + MENU_ITEMS))[at] + ITEM_DYNAMIC_FLAGS);
	if (visible)
		*flags |= WINDOW_VISIBLE;
	else
		*flags &= ~(WINDOW_VISIBLE | WINDOW_HASFOCUS);
}

// mw2coop_open_url <1-4>: opens one of the About page's pages in the browser (1 GitHub, 2 Nexus Mods, 3 PlayGround, 4 Discord). The pages are fixed here (what a menus.menu can ask for is a number, never an address).
static void open_url_f(void)
{
	static const char *const urls[] = {"https://github.com/stxlvn/mw2_enhanced", "https://www.nexusmods.com/codmw2/mods/99",
					   "https://www.playground.ru/call_of_duty_modern_warfare_2/file/call_of_duty_modern_warfare_2_mw2_coop_kooperativnaya_kampaniya_0_5_0_beta-1883975",
					   "https://discord.gg/q882kMMEY"};
	int k = game_cmd_argc() > 1 ? atoi(game_cmd_argv(1)) : 0;
	if (k < 1 || k > 4) {
		log_printf("menus: mw2coop_open_url %d: no such page", k);
		return;
	}
	log_printf("menus: opening %s", urls[k - 1]);
	ShellExecuteA(NULL, "open", urls[k - 1], NULL, NULL, SW_SHOWNORMAL);
}

// the split screen lobby's difficulty popups, one for each player (rows: the game's Recruit, Regular, Hardened, Veteran)
static void add_skill_menus(void)
{
	if (ui_find_menu("mw2coop_skill_p1"))
		return;
	if (table_popup("mw2coop_skill_p1") && table_popup("mw2coop_skill_p2"))
		log_printf("menus: split screen difficulty popups added");
}

static void add_lobbies_menu(void)
{
	if (ui_find_menu("mw2coop_lobbies"))
		return;
	// rows 1..3: the lobbies found; row 4: enter an address (menus.menu)
	if (table_popup("mw2coop_lobbies"))
		log_printf("menus: list of games on the network added");
}

// ---------------------------------------------------------------- hosting: continue, new, pick

// The campaign menu's "two players online" opens a popup: continue where the players stopped
// (progress.txt; the row is hidden without progress), a new campaign (the game's difficulty
// popup, then The Pit), or a mission from the campaign list (then its difficulty popup). The
// pick ends in the game's own "devmap <map>", which the devmap hook turns into the lobby.
static volatile LONG picking; // a new campaign or a mission is being picked for the lobby
static DWORD picking_seen;    // last tick one of the picking menus was open

static void add_host_menu(void)
{
	if (ui_find_menu("mw2coop_host"))
		return;
	uint8_t *menu = table_popup("mw2coop_host");
	if (menu) {
		show_row(menu, "mw2coop_host_4", 0);
		log_printf("menus: host popup added");
	}
}

static int so_name(const char *map)
{
	return map && strncmp(map, "so_", 3) == 0;
}

static volatile LONG split_pending; // the lobby about to be hosted is a split screen one
static volatile LONG split_mode;    // the lobby being hosted is a split screen one (the second player is on this PC, his process starts at Start)
static volatile LONG split_lobby_returning; // the local mission ended; listen is reopening its lobby
static volatile LONG split_lobby_return_tick;
static volatile LONG split_lobby_return_attempts;
static volatile LONG split_return_devices_valid;
static int split_return_devices[2] = {DEVICE_NONE, DEVICE_NONE};
static char split_return_map[64];

// coop_lobby is reused from Special Ops. Give campaign sessions the same loop as the
// single-player main menu, before listen activates the menu and starts its soundLoop.
static uint8_t *lobby_music_seen;
static const char *lobby_music_specops;
static int lobby_music_saved;
static void lobby_music_patch(int campaign)
{
	uint8_t *lobby = ui_find_menu("coop_lobby");
	if (!lobby)
		return;
	const char **loop = (const char **)(lobby + MENU_SOUND_LOOP);
	if (lobby != lobby_music_seen) {
		lobby_music_seen = lobby;
		lobby_music_specops = *loop;
		lobby_music_saved = 1;
	}
	const char *wanted = lobby_music_specops;
	if (campaign) {
		uint8_t *main = ui_find_menu("main_selection");
		wanted = main ? *(const char **)(main + MENU_SOUND_LOOP) : NULL;
		if (!wanted)
			wanted = "music_mainmenu";
	} else if (!lobby_music_saved) {
		return;
	}
	if (*loop != wanted) {
		*loop = wanted;
		log_printf("menus: coop_lobby music set to %s", campaign ? "the native main-selection theme" : "the native Special Ops theme");
	}
}

// mw2coop_host_menu: the campaign menu's button (split: the split screen button's, same popup)
static void host_menu_open(int split)
{
	InterlockedExchange(&split_pending, split);
	uint8_t *menu = ui_find_menu("mw2coop_host");
	char map[64], start[64];
	int have = progress_last(map, sizeof(map), start, sizeof(start));
	if (have) {
		char key[80] = "MENU_SP_SP_", text[160];
		int n = (int)strlen(key);
		for (const char *m = map; *m && n < (int)sizeof(key) - 1; m++)
			key[n++] = (char)((*m >= 'a' && *m <= 'z') ? *m - 32 : *m);
		key[n] = 0;
		const char *prefix = localize_text("MW2COOP_CONTINUE_PREFIX"), *mission = localize_text(key);
		snprintf(text, sizeof(text), "%s%s", prefix ? prefix : "", mission ? mission : map);
		localize_set_text("MW2COOP_CONTINUE", text);
	}
	show_row(menu, "mw2coop_continue", have);
	show_row(menu, "mw2coop_host_4", have);
	game_cbuf_addtext("openmenu mw2coop_host\n");
}

static void host_menu_f(void)
{
	host_menu_open(0);
}

static int save_picked; // the host picked a save in the menu (the lobby stays on it)
static void host_lobby(const char *map);

// The host's saves (progress.c): the newest four, "Mission - members".
static void add_saves_menu(void)
{
	if (ui_find_menu("mw2coop_saves"))
		return;
	if (table_popup("mw2coop_saves"))
		log_printf("menus: saves popup added");
}

// mw2coop_saves_menu: the host popup's "pick a save"
static void saves_menu_f(void)
{
	static const char *const rows[LIST_ROWS] = {"mw2coop_save_1", "mw2coop_save_2", "mw2coop_save_3", "mw2coop_save_4"};
	static const char *const keys[LIST_ROWS] = {"MW2COOP_SAVE_1", "MW2COOP_SAVE_2", "MW2COOP_SAVE_3", "MW2COOP_SAVE_4"};
	uint8_t *menu = ui_find_menu("mw2coop_saves");
	for (int r = 0; r < LIST_ROWS; r++) {
		char map[64], name[96], key[80] = "MENU_SP_SP_", text[192];
		int have = progress_save_info(r, map, sizeof(map), name, sizeof(name));
		if (have) {
			int n = (int)strlen(key);
			for (const char *m = map; *m && n < (int)sizeof(key) - 1; m++)
				key[n++] = (char)((*m >= 'a' && *m <= 'z') ? *m - 32 : *m);
			key[n] = 0;
			const char *mission = localize_text(key);
			const char *nobody = localize_text("MW2COOP_SAVE_NO_PARTNER");
			snprintf(text, sizeof(text), "%s - %s", mission ? mission : map, name[0] ? name : (nobody ? nobody : "?"));
			localize_set_text(keys[r], text);
		}
		show_row(menu, rows[r], have);
	}
	game_cbuf_addtext("openmenu mw2coop_saves\n");
}

// mw2coop_save_pick <1..4>: host a lobby on that save
static void save_pick_f(void)
{
	const char *arg = game_cmd_argv(1);
	if (arg[0] < '1' || arg[0] > '0' + LIST_ROWS || arg[1])
		return;
	progress_choose(arg[0] - '1');
	save_picked = 1;
	host_lobby(NULL);
}

// A split screen lobby needs no Steam lobby, but "listen" makes the game ask Steam for one, and when Steam answers with anything but OK (offline mode, no connection, too many lobbies
// made in a row: issue 11, "Steam connect failed" in the Campaign split screen lobby) the game's console command steamlobbycreatefail shows the error and drops the lobby. Its command
// node is looked at once the game has registered it (at the Steam start-up) and the function in it is put behind ours: a split screen lobby ignores the failure, a network one still shows it.
#ifdef _WIN64
#define STEAM_LOBBY_FAIL_NODE XR(0x1a2f758) // the cmd_function node of steamlobbycreatefail (function at 0x241120)
#else
#define STEAM_LOBBY_FAIL_NODE XR(0x17eb4e4) // VA 0x1beb4e4 (function at VA 0x66b0c0)
#endif
static void (*steam_lobby_fail_original)(void);
static int iwnet_would_pass(void);
static void steam_lobby_fail_hook(void)
{
	if (InterlockedCompareExchange(&split_mode, 0, 0)) {
		log_printf("menus: Steam did not make a lobby; a split screen lobby does not need one, the error is not shown");
		return;
	}
	if (!iwnet_would_pass()) { // no Steam or no IWNet: a LAN / direct IP lobby needs no Steam lobby either (only invites do)
		log_printf("menus: Steam did not make a lobby and there is no connection to it; the lobby stays (LAN / direct IP), the error is not shown");
		return;
	}
	if (steam_lobby_fail_original)
		steam_lobby_fail_original();
}

static void steam_lobby_fail_install(void)
{
	if (!STEAM_LOBBY_FAIL_NODE)
		return; // (the iw4x-sp profile: not mapped)
	cmd_function_t *node = (cmd_function_t *)(base + STEAM_LOBBY_FAIL_NODE);
	if (node->function == steam_lobby_fail_hook)
		return;
	if (!node->name || strcmp(node->name, "steamlobbycreatefail") || !node->function) {
		static int logged;
		if (!logged++)
			log_printf("menus: the steamlobbycreatefail command is not where it was expected: a Steam lobby failure is shown in a split screen lobby too");
		return;
	}
	DWORD old;
	if (!VirtualProtect(&node->function, sizeof(node->function), PAGE_READWRITE, &old))
		return;
	steam_lobby_fail_original = node->function;
	node->function = steam_lobby_fail_hook;
	VirtualProtect(&node->function, sizeof(node->function), old, &old);
	log_printf("menus: a failed Steam lobby no longer ends a split screen lobby (steamlobbycreatefail)");
}

static int continuing, add_member;
static volatile LONG partner_confirmed;

// Host a lobby: what the Spec Ops "two players online" button runs, then mw2coop_online [map] (or, for a Spec Ops op, the op).
// The game's own `listen` (hosting) and `connect` (joining) both start with a check that the game is signed in to IWNet and that Steam is up (64-bit 0x28fc00; offline it opens
// "Connecting to IWNet..." or "Steam is offline" and does nothing else: issue 21, a PC without internet; the 32-bit game's check, 0x416590 (VA), prints a line and stops). The mod's
// co-op needs neither: a split screen lobby, and a LAN or direct IP session (`mw2coop_join <address>`, no Steam lobby) have no use for IWNet. So when the game's own answer would be "no",
// the check inside `listen` and `connect` says yes (these two commands are only used by the mod's co-op in the campaign); an answer "yes" is left to the game, and a split screen lobby
// is always passed. A Steam lobby that cannot be made is ignored in the same situation (steam_lobby_fail_hook).
#ifdef _WIN64
#define LISTEN_SIGNIN_CALL XR(0xd25f6) // the call in listen (function at 0xd25e0)
#define CONNECT_SIGNIN_CALL XR(0xd23c9) // the call in connect (function at 0xd23c0)
#else
#define LISTEN_SIGNIN_CALL XR(0x17a886) // VA 0x57a886 in listen (function at VA 0x57a860)
#define CONNECT_SIGNIN_CALL XR(0x17a5b9) // VA 0x57a5b9 in connect (function at VA 0x57a5b0)
#endif
// what the game's own check answers, without its side effects (a pop-up, a message): Steam is up and the connection to IWNet stands
static int iwnet_would_pass(void)
{
#ifdef _WIN64
	if (!((unsigned char (*)(void))(base + XR(0x240f00)))()) // Steam available
		return 0;
	return base[XR(0x1bbab29)] != 0 && ((unsigned char (*)(void))(base + XR(0x1bab60)))() != 0;
#else
	if (!(((unsigned (*)(void))(base + XR(0x8b8c0)))() & 0xff)) // Steam: logged on
		return 0;
	return base[XR(0x15fbfe4)] != 0;
#endif
}

static int (*listen_signin_original)(int);
static int signin_hook(int local)
{
	if (InterlockedCompareExchange(&split_mode, 0, 0) || InterlockedCompareExchange(&split_pending, 0, 0)) {
		static int logged;
		if (!logged++)
			log_printf("menus: a split screen lobby needs no IWNet: the sign-in check of listen / connect is passed");
		return 1;
	}
	if (!iwnet_would_pass()) {
		static int logged;
		if (!logged++)
			log_printf("menus: no connection to IWNet or Steam (a LAN or direct IP session): the sign-in check of listen / connect is passed");
		return 1;
	}
	return listen_signin_original ? listen_signin_original(local) : 0;
}

static void signin_install(void)
{
	static int done;
	if (done)
		return;
	done = 1;
	const struct {
		uint32_t site;
		const char *what;
	} sites[] = {{LISTEN_SIGNIN_CALL, "listen"}, {CONNECT_SIGNIN_CALL, "connect"}};
	for (int i = 0; i < 2; i++) {
		void *original = NULL;
		if (hook_call(base + sites[i].site, (void *)signin_hook, &original)) {
			listen_signin_original = (int (*)(int))original;
			log_printf("menus: the IWNet sign-in check inside %s is behind ours (co-op works without internet)", sites[i].what);
		} else
			log_printf("menus: the sign-in call inside %s is not where it was expected: co-op needs IWNet there", sites[i].what);
	}
}

// QA only (MW2COOP_FAKE_IWNET_OFF=1, 64-bit): the game believes it is not connected to IWNet, as on a PC without internet (issue 21: "Connecting to IWNet..." forever in the split
// screen lobby). The game's "connected to IWNet" query (0x1bab60) is replaced by a function that says no.
#ifdef _WIN64
static int fake_iwnet_off_query(void)
{
	return 0;
}

static void fake_iwnet_off_install(void)
{
	static int done;
	if (done || !getenv("MW2COOP_FAKE_IWNET_OFF"))
		return;
	done = 1;
	hook_jump(base + XR(0x1bab60), 14, (const void *)fake_iwnet_off_query, 0xcc);
	log_printf("menus: QA: the game is told it is not connected to IWNet (MW2COOP_FAKE_IWNET_OFF)");
}
#else
static void fake_iwnet_off_install(void)
{
}
#endif

static void host_lobby(const char *map)
{
	steam_lobby_fail_install();
	int specops = so_name(map); // a Special Ops op: the lobby is the native one, only the second player is on this PC
	lobby_music_patch(!specops); // listen activates coop_lobby immediately after mw2coop_online
	char cmd[512];
	char online[160];
	if (specops) {
		continuing = 0;
		add_member = 0;
		InterlockedExchange(&partner_confirmed, 0);
		snprintf(online, sizeof(online), "set specialops 1\nset coop_mapName %s\n", map);
	} else
		snprintf(online, sizeof(online), "mw2coop_online %s\n", map ? map : "");
	// xrequirelivesignin: the game wants to be signed in to IWNet, and without a connection to it (a PC without internet) it opens "Connecting to IWNet..." and waits for ever (issue 21).
	// A split screen lobby needs no IWNet (nor Steam lobby, see above), so it does not ask for it; a network lobby keeps the game's own check.
	// Without a connection to IWNet or Steam (a LAN or direct IP lobby, issue 23: the "Connecting to IWNet..." pop-up of this command stayed over the lobby on a PC behind a VPN) the command
	// is not sent either: listen and connect pass their own check by the hooks above, so nothing else asks for the sign-in. With the connection the game's own check runs as before.
	const int split_now = split_pending != 0 || !iwnet_would_pass();
	snprintf(cmd, sizeof(cmd),
		 "set arcademode 0\nset arcademode_full 0\nset solo_play 0\nset ui_skip_level_select 1\n"
		 "set so_ophud_player0 0\nset so_ophud_player1 0\n%s"
		 "stopListening\nset arcademode_lives 2\nset arcademode_combined_score 0\nset mblur_enable 0\n"
		 "%slisten\n",
		 split_now ? "" : "xrequirelivesignin\n", online);
	game_cbuf_addtext(cmd);
	if (InterlockedExchange(&split_pending, 0)) { // a split screen lobby: the second player presses a button on his device first
		InterlockedExchange(&split_mode, 1);
		// On an automatic return, wait until listen has actually restored coop_lobby before
		// restarting the device assignment thread. Starting it while stopListening/listen
		// are still changing the UI can lose one player's device card.
		if (InterlockedCompareExchange(&split_lobby_returning, 0, 0)) {
			// The saved assignments remain valid until tick_f confirms the lobby is live.
		} else if (InterlockedExchange(&split_return_devices_valid, 0))
			devices_begin_assigned(split_return_devices[0], split_return_devices[1]);
		else
			devices_begin();
	} else {
		InterlockedExchange(&split_mode, 0);
		devices_end();
	}
}

// the Split screen button: the host popup of the split screen lobby (the screen does not change: both players are in this one window)
static void split_menu_f(void)
{
	host_menu_open(1);
}

// mw2coop_split_specops: the split screen button of Special Ops' menu. The lobby is Spec Ops' own (Change Op, difficulty), with the second player on this PC.
static void split_specops_f(void)
{
	char map[64] = "";
	game_dvar_raw("coop_mapName", map, sizeof(map));
	// (the name goes into the command buffer: coop_mapName can come from an earlier host's coopAccept, so a plain name only, audit 2026-11-18, menus F12)
	if (!so_name(map) || map[strspn(map, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")])
		lstrcpynA(map, "so_killspree_trainer", sizeof(map)); // the lobby shows an op; the stale campaign mission of an earlier lobby is not one
	InterlockedExchange(&split_pending, 1);
	log_printf("menus: Special Ops split screen lobby on '%s'", map);
	host_lobby(map);
}

static void host_go_f(void)
{
	progress_choose(-1);
	save_picked = 0;
	host_lobby(NULL);
}

static void host_new_f(void)
{
	InterlockedExchange(&picking, 1);
	picking_seen = GetTickCount();
	game_cbuf_addtext("set ui_load_level trainer\nopenmenu popmenu_difficulty\n");
}

static void host_pick_f(void)
{
	InterlockedExchange(&picking, 1);
	picking_seen = GetTickCount();
	game_cbuf_addtext("setdvar arcademode 0\nopenmenu levels\n");
}

// Picking ends when its menus are all closed without a devmap (Esc): menuDef dynamicFlags
// (+0x50) has WINDOW_VISIBLE while a menu is open.
static void picking_tick(void)
{
	if (!picking)
		return;
	static const char *const menus[] = {"levels", "popmenu_act1", "popmenu_act2", "popmenu_act3", "popmenu_difficulty"};
	for (size_t i = 0; i < sizeof(menus) / sizeof(menus[0]); i++) {
		uint8_t *m = ui_find_menu(menus[i]);
		if (m && (*(uint32_t *)(m + ITEM_DYNAMIC_FLAGS) & WINDOW_VISIBLE))
			picking_seen = GetTickCount();
	}
	if (GetTickCount() - picking_seen > 1500) {
		InterlockedExchange(&picking, 0);
		log_printf("menus: mission pick for the lobby cancelled");
	}
}

// Unlock all missions for the co-op host's pick (issue #3): the game's mission list gates on the dvar mis_cheat
// (the cheat that opens every mission; the profile is untouched). It is 1 only while one of the list's menus is
// open in a co-op pick or lobby, and put back as it was afterwards, so the player's own Mission Select and saves
// keep their rules.
static int cheat_on;
static char cheat_old[16];

static void unlock_missions_tick(void);

// ---------------------------------------------------------------- the lobby with a campaign mission

// Both PCs: the party is up, no level runs, and the lobby's mission is not a Spec Ops one
// (the client learns coop_mapName from the host's coopAccept).
static int campaign_lobby(void)
{
	if (!base[RVA_PARTY_ACTIVE])
		return 0;
	char v[64] = "";
	game_dvar_raw("sv_running", v, sizeof(v));
	if (v[0] == '1')
		return 0;
	game_dvar_raw("coop_mapName", v, sizeof(v));
	return v[0] && strncmp(v, "so_", 3) != 0;
}

// Both PCs' view of a split screen lobby holding a Special Ops op (the lobby is the native one; only its Start is ours, the second player being on this PC)
static int spec_split_lobby(void)
{
	if (!base[RVA_PARTY_ACTIVE] || !(split_mode || split_lobby_returning))
		return 0;
	char v[64] = "";
	game_dvar_raw("sv_running", v, sizeof(v));
	if (v[0] == '1')
		return 0;
	game_dvar_raw("coop_mapName", v, sizeof(v));
	return so_name(v);
}

// Leaving the campaign lobby: coop_lobby is the Special Ops lobby, and its "leave" popup
// (leave_lobby_warning) sends the player on to main_specops. While it holds a campaign mission,
// those script targets are changed to main_selection. The native scripts are put back for a
// Spec Ops lobby. The first time the popup is seen its items are logged, to see what the game runs.
typedef struct {
	MenuEventHandler *handler;
	void *native;
} LeaveSwap;
static LeaveSwap leave_swaps[24];
static int leave_swap_count, leave_swapped, leave_logged;
static uint8_t *leave_menu_seen;

// A copy of the script with every "main_specops" (quoted) replaced by "main_selection".
// The latter is longer, so size the allocation for every replacement before copying.
static char *leave_script_copy(const char *src)
{
	static const char from[] = "\"main_specops\"", to[] = "\"main_selection\"";
	const size_t from_len = sizeof(from) - 1, to_len = sizeof(to) - 1;
	size_t occurrences = 0;
	for (const char *p = src; (p = strstr(p, from)); p += from_len)
		occurrences++;
	if (!occurrences)
		return NULL;
	size_t cap = strlen(src) + occurrences * (to_len - from_len) + 1;
	char *out = HeapAlloc(GetProcessHeap(), 0, cap);
	if (!out)
		return NULL;
	size_t n = 0;
	while (*src) {
		if (!strncmp(src, from, from_len)) {
			memcpy(out + n, to, to_len);
			n += to_len;
			src += from_len;
		} else
			out[n++] = *src++;
	}
	out[n] = 0;
	return out;
}

static void leave_walk(MenuEventHandlerSet *set, int depth)
{
	if (!set || IsBadReadPtr(set, sizeof(*set)) || depth > 4 || set->count < 1 || set->count > 24 || IsBadReadPtr(set->handlers, set->count * sizeof(void *)))
		return;
	for (int i = 0; i < set->count; i++) {
		MenuEventHandler *h = set->handlers[i];
		if (!h || IsBadReadPtr(h, sizeof(*h)))
			continue;
		if (h->type == 0) {
			if (!h->data || IsBadStringPtrA((const char *)h->data, 8) || leave_swap_count >= (int)(sizeof(leave_swaps) / sizeof(leave_swaps[0])))
				continue;
			char *copy = leave_script_copy((const char *)h->data);
			if (!copy)
				continue;
			leave_swaps[leave_swap_count].handler = h;
			leave_swaps[leave_swap_count].native = h->data;
			leave_swap_count++;
			h->data = copy;
#ifdef _WIN64
		} else if (h->type == 1 && h->data && !IsBadReadPtr(h->data, 16)) { // if (expression, set): the layout is not known, a readable set at either place is followed
			leave_walk(*(MenuEventHandlerSet **)h->data, depth + 1);
			leave_walk(*(MenuEventHandlerSet **)((uint8_t *)h->data + 8), depth + 1);
		} else if (h->type == 2 && h->data) {
			leave_walk((MenuEventHandlerSet *)h->data, depth + 1);
#endif
		}
	}
}

static void leave_warning_patch(int campaign)
{
	uint8_t *menu = ui_find_menu("leave_lobby_warning");
	if (menu != leave_menu_seen) { // a new menu (the zone was reloaded): its scripts are the native ones
		leave_menu_seen = menu;
		leave_swap_count = 0;
		leave_swapped = 0;
	}
	if (!menu)
		return;
	if (!campaign && leave_swapped) {
		for (int i = 0; i < leave_swap_count; i++)
			if (!IsBadWritePtr(leave_swaps[i].handler, sizeof(MenuEventHandler)))
				leave_swaps[i].handler->data = leave_swaps[i].native;
		leave_swap_count = 0;
		leave_swapped = 0;
		log_printf("menus: leave_lobby_warning: the native scripts are back");
		return;
	}
	if (!campaign || leave_swapped)
		return;
	if (!leave_logged) {
		leave_logged = 1;
		int count = *(int *)(menu + MENU_ITEM_COUNT);
		uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
		log_printf("menus: leave_lobby_warning has %d item(s)", count);
		for (int i = 0; i < count && i < 24; i++) {
			const char *name = *(const char **)items[i];
			log_printf("menus: leave_lobby_warning[%d] %s", i, name && !IsBadStringPtrA(name, 40) ? name : "-");
#ifdef _WIN64
			log_handler_set("action", *(MenuEventHandlerSet **)(items[i] + ITEM_ACTION), 0);
#endif
		}
	}
	for (int e = 0; e < 4; e++) // onOpen, onCloseRequest, onClose, onESC: four pointers in a row in both layouts
		leave_walk(*(MenuEventHandlerSet **)(menu + MENU_ON_OPEN + e * (int)sizeof(void *)), 0);
	{
		int count = *(int *)(menu + MENU_ITEM_COUNT);
		uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
		for (int i = 0; i < count && i < 24; i++)
			leave_walk(*(MenuEventHandlerSet **)(items[i] + ITEM_ACTION), 0);
	}
	leave_swapped = 1;
	log_printf("menus: leave_lobby_warning: %d script(s) now open main_selection instead of main_specops", leave_swap_count);
}

// Items changed while the lobby holds a campaign mission; the native values are kept to switch
// back for Spec Ops.
typedef struct {
	const char *item;   // name, "name+1" for the unnamed item right after it, or "@TEXT" for the item showing it
	int offset;
	const char *script; // replacement action ...
	const void *value;  // ... or replacement pointer (NULL clears a statement)
	uint8_t *seen;	    // the item the native value was read from (zone reloads bring new ones)
	void *native;
	void *campaign;
} LobbySwap;

static LobbySwap lobby_swaps[] = {
	// the title: "SPECIAL OPS : Two players online" -> "CAMPAIGN : ..."
	{"@MENU_SPECIAL_OPS", ITEM_TEXT, NULL, "@MW2COOP_CAMPAIGN", NULL, NULL, NULL},
	// Change Op: the campaign missions
	{"selectmap", ITEM_ACTION, "\"play\" \"mouse_click\" ; \"open\" \"levels\" ; ", NULL, NULL, NULL, NULL},
	// Start: the native button needs coopready(), which counts the Steam lobby's members; a
	// partner who joined by address has none. mw2coop_start checks the party and the ready state.
	{"startmatch", ITEM_ACTION, "\"play\" \"mouse_click\" ; \"exec\" \"mw2coop_start\" ; ", NULL, NULL, NULL, NULL},
	{"startmatch", ITEM_FLOAT_EXPS, NULL, NULL, NULL, NULL, NULL}, // disabled when ( !gamehost() || !coopready() )
	{"startmatch", ITEM_DISABLED_EXP, NULL, NULL, NULL, NULL, NULL},
	{"startmatch", ITEM_TEXT, NULL, "@MW2COOP_START", NULL, NULL, NULL},
	// Ready: the Spec Ops difficulty line, shown (greyed) for missions without Spec Ops difficulty
	{"dont_selectdiff", ITEM_ACTION, "\"play\" \"mouse_click\" ; \"exec\" \"mw2coop_ready\" ; ", NULL, NULL, NULL, NULL},
	{"dont_selectdiff", ITEM_FLOAT_EXPS, NULL, NULL, NULL, NULL, NULL}, // disabled when the op has no difficulty
	{"dont_selectdiff", ITEM_TEXT, NULL, "@MW2COOP_READY", NULL, NULL, NULL},
	// its focus hint read "not available for this op"
	{"dont_selectdiff", ITEM_ON_FOCUS, "\"play\" \"mouse_over\" ; \"setItemColor\" \"self\" \"backcolor\" 0 0 0 1 ; ", NULL, NULL, NULL, NULL},
	// the greyed "Select difficulty" text drawn on the same line
	{"dont_selectdiff+1", ITEM_TEXT, NULL, "", NULL, NULL, NULL},
};

static int lobby_campaign_set;
static void patch_lobby_mission(uint8_t *menu, int campaign);

// the item showing <text> natively (or our text, once swapped: the swap remembers its item)
static int item_by_text(uint8_t *menu, const char *text, const LobbySwap *w)
{
	int count = *(int *)(menu + MENU_ITEM_COUNT);
	uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
	for (int i = 0; i < count; i++) {
		const char *t = *(const char **)(items[i] + ITEM_TEXT);
		if (items[i] == w->seen || (t && strcmp(t, text) == 0))
			return i;
	}
	return -1;
}

// The native split screen lobby: both players are on this PC. The title says split screen instead of online, the rows that only an online lobby needs are hidden
// (invite a friend, access, password, the network type), and each player's card shows the picture of his device (keyboard and mouse, a gamepad, none before he has
// pressed). Everything goes back when the lobby is an online one again. The cards' pictures are the two 64 x 64 items at x -176 (y 70: player 1, y 148: player 2), whose
// window background (Material *, ITEM_BACKGROUND_PTR) is the game's "pc_keyboard_mouse".
static void patch_split_lobby(uint8_t *menu, int spec_split)
{
	static uint8_t *kb_material;
	static uint8_t *cards[2], *title_item;
	static void *title_native;
	static int split_was;
	int split = split_mode;
	int count = *(int *)(menu + MENU_ITEM_COUNT);
	uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
	// a new copy of the lobby menu (the UI's zone came back from a level): the items found in the old one are not written any more (they were, every
	// tick, into memory the old zone gave back; audit 2026-11-18, menus F5)
	static uint8_t *seen_menu;
	static uint8_t **seen_items;
	if (menu != seen_menu || items != seen_items) {
		seen_menu = menu;
		seen_items = items;
		cards[0] = cards[1] = title_item = NULL;
		title_native = NULL;
		kb_material = NULL;
	}
	if (!cards[0] || !cards[1] || !title_item) {
		for (int i = 0; i < count; i++) {
			uint8_t *it = items[i];
			const float *rc = (const float *)(it + ITEM_RECT);
			if (rc[0] == -176.0f && rc[2] == 64.0f && rc[3] == 64.0f && (rc[1] == 70.0f || rc[1] == 148.0f))
				cards[rc[1] == 70.0f ? 0 : 1] = it;
			const char *text = *(const char **)(it + ITEM_TEXT);
			if (text && !strcmp(text, "@MENU_SP_TWO_PLAYER_ONLINE")) {
				title_item = it;
				title_native = *(void **)(it + ITEM_TEXT);
			}
		}
		if (cards[0] && !kb_material)
			kb_material = *(uint8_t **)(cards[0] + ITEM_BACKGROUND_PTR);
#ifndef _WIN64
		// 32 bits: the card's own material is an opaque placeholder (a black box behind a picture drawn on it); the engine's "white" material blends its alpha, as the hints' pictures do
		if (cards[0] && cards[1]) {
			uint8_t *w = ((uint8_t * (*)(int, const char *))(base + RVA_DB_FIND_HEADER))(5, "white");
			if (w)
				kb_material = w;
		}
#endif
	}
	if (title_item)
		*(const char **)(title_item + ITEM_TEXT) = split ? "@MW2COOP_SPLIT_TITLE" : (const char *)title_native;
	static const char *const online_rows[] = {"invitefriend", "mw2coop_access", "mw2coop_password", "nattype", "mw2coop_nick"};
	if (split || split_was) {
		for (size_t r = 0; r < sizeof(online_rows) / sizeof(online_rows[0]); r++)
			for (int i = 0; i < count; i++) {
				const char *name = *(const char **)items[i];
				if (name && !strcmp(name, online_rows[r])) {
					uint32_t *flags = (uint32_t *)(items[i] + ITEM_DYNAMIC_FLAGS);
					if (split)
						*flags &= ~(WINDOW_VISIBLE | WINDOW_HASFOCUS);
					else
						*flags |= WINDOW_VISIBLE;
				}
			}
	}
	for (int pl = 0; pl < 2 && kb_material; pl++) {
		if (!cards[pl])
			continue;
		uint8_t *want = kb_material;
		int shown = 1;
		if (split) {
			int d = devices_get(pl);
			shown = d != DEVICE_NONE; // no picture before the player has pressed a button on his device
			want = d >= 0 ? (uint8_t *)padicon_material(kb_material) : kb_material;
#ifndef _WIN64
			if (d < 0) // 32 bits: the game has no keyboard and mouse picture for the card (an empty box): it is drawn here
				want = (uint8_t *)padicon_keyboard_material(kb_material);
#endif
			if (!want)
				want = kb_material; // (the gamepad picture is made once the keyboard's texture is there)
		}
#ifndef _WIN64
		else { // the online lobby too: the game's own picture is the same empty placeholder there, a white box on the iw4x-sp client (player report 2026-11-20)
			want = (uint8_t *)padicon_keyboard_material(kb_material);
			if (!want)
				want = kb_material;
		}
#endif
		*(uint8_t **)(cards[pl] + ITEM_BACKGROUND_PTR) = want;
		uint32_t *flags = (uint32_t *)(cards[pl] + ITEM_DYNAMIC_FLAGS);
		if (shown)
			*flags |= WINDOW_VISIBLE;
		else
			*flags &= ~WINDOW_VISIBLE;
	}
	// the friendly fire row moves up into the Ready row's place, right under the operation (Ready, Invite and Access are hidden), and
	// back. It is a copy of the invite row, with its rules for greying out (disabled offline, as the split screen lobby is): those go
	// in the split screen lobby (the row's command checks that this is the host) and come back for an online one
	{
		static uint8_t *seen; // the row as first seen (access.c makes it again when the UI is reloaded)
		static float ff_y, ff_cy;
		static void *ff_float_exps, *ff_disabled_exp;
		uint8_t *ff = NULL, *ready = NULL;
		for (int i = 0; i < count && (!ff || !ready); i++) {
			const char *name = *(const char **)items[i];
			if (name && !strcmp(name, "mw2coop_ff") && !ff)
				ff = items[i];
			if (name && !strcmp(name, "dont_selectdiff") && !ready)
				ready = items[i];
		}
		if (ff && ff != seen) {
			seen = ff, ff_y = *(float *)(ff + ITEM_RECT_Y), ff_cy = *(float *)(ff + ITEM_RECT_CLIENT_Y);
			ff_float_exps = *(void **)(ff + ITEM_FLOAT_EXPS), ff_disabled_exp = *(void **)(ff + ITEM_DISABLED_EXP);
		}
		if (ff && ready) {
			float dy = split && !spec_split ? *(float *)(ready + ITEM_RECT_Y) - ff_y : 0; // (Special Ops: its difficulty row stays where it is)
			*(float *)(ff + ITEM_RECT_Y) = ff_y + dy;
			*(float *)(ff + ITEM_RECT_CLIENT_Y) = ff_cy + dy;
			*(void **)(ff + ITEM_FLOAT_EXPS) = split && !spec_split ? NULL : ff_float_exps;
			*(void **)(ff + ITEM_DISABLED_EXP) = split && !spec_split ? NULL : ff_disabled_exp;
		}
	}
	// the two difficulty rows of the console's split screen lobby (its menu_splitscreen_setup has a difficulty button for each player, each opening its own popup): in a campaign
	// mission's lobby under the friendly fire row; in a Special Ops op's lobby in the place of the game's one difficulty row and the (hidden) invite row under it, the game's
	// row hidden (Vlad 2026-11-21: one row set player 1's difficulty only, and the lobby said the players had different ones; the console's lobby has one popup for each,
	// popmenu_specialops_splitscreen_difficulty_p1 / _p2)
	{
		uint8_t *ff = NULL, *diff = NULL;
		for (int i = 0; i < count; i++) {
			const char *name = *(const char **)items[i];
			if (name && !strcmp(name, "mw2coop_ff") && !ff)
				ff = items[i];
			if (name && !strcmp(name, "selectdiff") && !diff)
				diff = items[i];
			if (name && (!strcmp(name, "selectdiff") || !strcmp(name, "dont_selectdiff")) && split && spec_split)
				*(uint32_t *)(items[i] + ITEM_DYNAMIC_FLAGS) &= ~(WINDOW_VISIBLE | WINDOW_HASFOCUS);
		}
		if (split && spec_split)
			ff = NULL; // (placed from the difficulty row below)
		for (int pl = 0; pl < 2; pl++) {
			uint8_t *row = NULL;
			char want[24];
			snprintf(want, sizeof(want), "mw2coop_skill%d", pl + 1);
			for (int i = 0; i < count && !row; i++) {
				const char *name = *(const char **)items[i];
				if (name && !strcmp(name, want))
					row = items[i];
			}
			if (!row)
				continue;
			uint32_t *flags = (uint32_t *)(row + ITEM_DYNAMIC_FLAGS);
			uint8_t *above = spec_split ? diff : ff; // the row this one is placed from
			const float step = (float)(spec_split ? pl : pl + 1) * BUTTON_HEIGHT;
			if (split && above) {
				const float y = *(float *)(above + ITEM_RECT_Y) + step, cy = *(float *)(above + ITEM_RECT_CLIENT_Y) + step;
				*(float *)(row + ITEM_RECT_Y) = y;
				*(float *)(row + ITEM_RECT_CLIENT_Y) = cy;
				*(void **)(row + ITEM_FLOAT_EXPS) = NULL; // (a copy of the invite row, which is greyed out offline: this one is always on)
				*(void **)(row + ITEM_DISABLED_EXP) = NULL;
				*flags |= WINDOW_VISIBLE;
			} else
				*flags &= ~(WINDOW_VISIBLE | WINDOW_HASFOCUS);
		}
	}
	split_was = split;
}

static void patch_lobby(int campaign, int spec_split)
{
	uint8_t *menu = ui_find_menu("coop_lobby");
	if (!menu)
		return;
	lobby_music_patch(campaign);
	int changed = 0;
	for (size_t k = 0; k < sizeof(lobby_swaps) / sizeof(lobby_swaps[0]); k++) {
		LobbySwap *w = &lobby_swaps[k];
		char name[64];
		lstrcpynA(name, w->item, sizeof(name));
		char *plus = strchr(name, '+');
		if (plus)
			*plus = 0;
		int i = name[0] == '@' ? item_by_text(menu, name, w) : ui_item_index(menu, name);
		if (i < 0)
			continue;
		if (plus && ++i >= *(int *)(menu + MENU_ITEM_COUNT))
			continue;
		uint8_t *item = (*(uint8_t ***)(menu + MENU_ITEMS))[i];
		void **field = (void **)(item + w->offset);
		if (item != w->seen) {
			w->seen = item;
			w->native = *field;
		}
		if (!w->campaign)
			w->campaign = w->script ? ui_script_set(w->script) : (void *)w->value;
		int is_start = strncmp(w->item, "startmatch", 10) == 0;
		void *want = (campaign || (spec_split && is_start)) ? w->campaign : w->native; // a split Special Ops lobby: native op and difficulty, our Start
		if (*field != want) {
			*field = want;
			changed = 1;
		}
	}
	patch_lobby_mission(menu, campaign);
	patch_split_lobby(menu, spec_split);
	if (changed || campaign * 2 + spec_split != lobby_campaign_set)
		log_printf("menus: coop_lobby set for %s", campaign ? "a campaign mission" : spec_split ? "a split screen Spec Ops op" : "Spec Ops");
	lobby_campaign_set = campaign * 2 + spec_split;
}

// The lobby's mission picture and name: Spec Ops ops get them from sp/specOpsTable.csv (the
// picture's materialExp, the name's textExp); a campaign mission has no row there and showed a
// checkerboard. With a campaign mission the expressions are switched off and the item shows the
// mission list's own: material "levelshot_mw2_<map>", text "@MENU_SP_SP_<MAP>".
#define ASSET_TYPE_MATERIAL 5
#define ITEM_BACKGROUND ITEM_BACKGROUND_PTR
#define ITEM_MATERIAL_EXP ITEM_MATERIAL_EXP_PTR
#define WINDOW_AUTOWRAPPED 0x00800000
static uint8_t *pic_item, *name_item;
static void *pic_native_material, *pic_native_exp, *name_native_exp;
static const char *name_native_text;
static char shown_mission[128];
static const char *shown_description;

// Item 22 in coop_lobby is the native contextual-hint text item above the mission title and
// picture. In the campaign lobby it becomes a multi-line mission-description panel; its original
// text, expressions, geometry and flags return for Spec Ops.
typedef struct {
	uint8_t *item;
	const char *text;
	void *text_exp;
	void *float_exps;
	void *disabled_exp;
	uint32_t visible_flags;
	uint32_t static_flags;
	float rect[4];
	float client_rect[4];
} LobbyDescriptionState;
static LobbyDescriptionState lobby_description_state;

static void patch_lobby_description(uint8_t *menu, int campaign, const char *description)
{
	int at = ui_item_index(menu, "description");
	if (at < 0)
		return;
	uint8_t *item = (*(uint8_t ***)(menu + MENU_ITEMS))[at];
	LobbyDescriptionState *state = &lobby_description_state;
	if (item != state->item) {
		state->item = item;
		state->text = *(const char **)(item + ITEM_TEXT);
		state->text_exp = *(void **)(item + ITEM_TEXT_EXP);
		state->float_exps = *(void **)(item + ITEM_FLOAT_EXPS);
		state->disabled_exp = *(void **)(item + ITEM_DISABLED_EXP);
		state->visible_flags = *(uint32_t *)(item + ITEM_DYNAMIC_FLAGS) & (WINDOW_VISIBLE | WINDOW_HASFOCUS);
		state->static_flags = *(uint32_t *)(item + ITEM_STATIC_FLAGS);
		memcpy(state->rect, item + ITEM_RECT, sizeof(state->rect));
		memcpy(state->client_rect, item + ITEM_RECT_CLIENT_Y - 4, sizeof(state->client_rect));
	}
	if (!campaign) {
		*(const char **)(item + ITEM_TEXT) = state->text;
		*(void **)(item + ITEM_TEXT_EXP) = state->text_exp;
		*(void **)(item + ITEM_FLOAT_EXPS) = state->float_exps;
		*(void **)(item + ITEM_DISABLED_EXP) = state->disabled_exp;
		memcpy(item + ITEM_RECT, state->rect, sizeof(state->rect));
		memcpy(item + ITEM_RECT_CLIENT_Y - 4, state->client_rect, sizeof(state->client_rect));
		uint32_t *flags = (uint32_t *)(item + ITEM_DYNAMIC_FLAGS);
		*flags = (*flags & ~(WINDOW_VISIBLE | WINDOW_HASFOCUS)) | state->visible_flags;
		*(uint32_t *)(item + ITEM_STATIC_FLAGS) = state->static_flags;
		return;
	}
	if (!description || !description[0]) {
		*(const char **)(item + ITEM_TEXT) = "";
		*(void **)(item + ITEM_TEXT_EXP) = NULL;
		*(void **)(item + ITEM_FLOAT_EXPS) = NULL;
		*(void **)(item + ITEM_DISABLED_EXP) = NULL;
		*(uint32_t *)(item + ITEM_DYNAMIC_FLAGS) &= ~(WINDOW_VISIBLE | WINDOW_HASFOCUS);
		*(uint32_t *)(item + ITEM_STATIC_FLAGS) &= ~WINDOW_AUTOWRAPPED;
		return;
	}
	*(const char **)(item + ITEM_TEXT) = description; // already localized by localize_text()
	*(void **)(item + ITEM_TEXT_EXP) = NULL;
	*(void **)(item + ITEM_FLOAT_EXPS) = NULL; // do not gate the description on ui_hint_focus/startmatch
	*(void **)(item + ITEM_DISABLED_EXP) = NULL;
	*(uint32_t *)(item + ITEM_STATIC_FLAGS) |= WINDOW_AUTOWRAPPED;
	float *rect = (float *)(item + ITEM_RECT);
	float *client = (float *)(item + ITEM_RECT_CLIENT_Y - 4);
	// Keep the bottom edge at the existing description slot; grow upward into the free area
	// immediately above the mission name (20 px) and image, with the stock autowrap enabled.
	rect[1] = state->rect[1] - 52.0f;
	rect[3] = 72.0f;
	client[1] = state->client_rect[1] - 52.0f;
	client[3] = 72.0f;
	*(uint32_t *)(item + ITEM_DYNAMIC_FLAGS) |= WINDOW_VISIBLE;
}

static void find_mission_items(uint8_t *menu)
{
	int count = *(int *)(menu + MENU_ITEM_COUNT);
	uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
	uint8_t *pic = NULL, *name = NULL;
	for (int i = 0; i < count; i++) {
		float *r = (float *)(items[i] + ITEM_RECT);
		if (r[2] == 216 && r[3] == 120 && *(void **)(items[i] + ITEM_MATERIAL_EXP))
			pic = items[i];
	}
	for (int i = 0; pic && i < count; i++) {
		float *r = (float *)(items[i] + ITEM_RECT), *pr = (float *)(pic + ITEM_RECT);
		if (r[2] == 216 && r[3] == 20 && r[1] == pr[1] - 20 && *(void **)(items[i] + ITEM_TEXT_EXP))
			name = items[i];
	}
	if (!pic || !name)
		return;
	if (pic != pic_item) {
		pic_item = pic;
		pic_native_material = *(void **)(pic + ITEM_BACKGROUND);
		pic_native_exp = *(void **)(pic + ITEM_MATERIAL_EXP);
		shown_mission[0] = 0;
		shown_description = NULL;
	}
	if (name != name_item) {
		name_item = name;
		name_native_exp = *(void **)(name + ITEM_TEXT_EXP);
		name_native_text = *(const char **)(name + ITEM_TEXT);
		shown_mission[0] = 0;
		shown_description = NULL;
	}
}

// The campaign missions' pictures (levelshot_mw2_*) are square art: the item is 216 x 120 for the Spec Ops 16:9 pictures, so a campaign mission gets a square one in the middle
// of that space (v0.6.0 report), and the Spec Ops ops the original rectangle back.
static void lobby_picture_shape(int square)
{
	float *r = (float *)(pic_item + ITEM_RECT), *rc = (float *)(pic_item + ITEM_RECT_CLIENT_Y - 4); // (the menu draws from rectClient, worked out when the menu opened)
	static float x0, w0;
	if (!w0 && r[2] == 216)
		x0 = r[0], w0 = r[2];
	if (!w0)
		return;
	static int logged;
	if (!logged++)
		log_printf("menus: lobby picture rect %.0f %.0f %.0f %.0f client %.0f %.0f %.0f %.0f align %d %d", r[0], r[1], r[2], r[3], rc[0], rc[1], rc[2], rc[3], pic_item[ITEM_RECT_ALIGN_H], pic_item[ITEM_RECT_ALIGN_V]);
	float w = square ? r[3] : w0, dx = square && pic_item[ITEM_RECT_ALIGN_H] == 1 ? (w0 - r[3]) / 2 : 0; // (left aligned: moved in by half the difference)
	rc[0] += x0 + dx - r[0];
	rc[2] = w;
	r[0] = x0 + dx;
	r[2] = w;
}

static void patch_lobby_mission(uint8_t *menu, int campaign)
{
	find_mission_items(menu);
	if (!pic_item || !name_item) {
		patch_lobby_description(menu, campaign, NULL);
		return;
	}
	lobby_picture_shape(campaign);
	if (!campaign) {
		*(void **)(pic_item + ITEM_MATERIAL_EXP) = pic_native_exp;
		*(void **)(pic_item + ITEM_BACKGROUND) = pic_native_material;
		*(void **)(name_item + ITEM_TEXT_EXP) = name_native_exp;
		*(const char **)(name_item + ITEM_TEXT) = name_native_text;
		patch_lobby_description(menu, 0, NULL);
		shown_mission[0] = 0;
		shown_description = NULL;
		return;
	}

	char map[64] = "", museum_mode[32] = "";
	game_dvar_raw("coop_mapName", map, sizeof(map));
	if (!map[0]) {
		patch_lobby_description(menu, 1, NULL);
		shown_mission[0] = 0;
		shown_description = NULL;
		return;
	}
	if (strcmp(map, "ending") == 0)
		game_dvar_raw("ui_char_museum_mode", museum_mode, sizeof(museum_mode));

	// Mission Select's MUSEUM entry uses the 'ending' map with ui_char_museum_mode=free.
	// The selectable Endgame mission continues to use the original 'ending' -> 'af_chase'
	// fallback; all other campaign mission mappings remain unchanged.
	int museum = strcmp(map, "ending") == 0 && strcmp(museum_mode, "free") == 0;
	{
		// the game's mode dvar does not survive the way to the level (it reads empty there, issue 24: the Museum started the knife fight): the choice is kept in a dvar of the mod,
		// which maps\_mw2coop_ending reads (and clears)
		// Only the host's choice counts: the client takes it from the start packet (progress.c session_dvars). Each PC wrote its own, and in a network game
		// the client played the knife fight's credits while the host walked the Museum (audit 2026-11-18, GSC F2).
		char kept[8] = "";
		game_dvar_raw("mw2coop_museum", kept, sizeof(kept));
		if (base[RVA_PARTY_IS_HOST] && (kept[0] == '1') != (museum != 0))
			game_cbuf_addtext(museum ? "set mw2coop_museum 1\n" : "set mw2coop_museum 0\n");
	}
	const char *shown = museum ? "museum" : (strcmp(map, "ending") == 0 ? "af_chase" : map);
	char identity[128];
	snprintf(identity, sizeof(identity), "%s|%s", map, museum_mode);
	if (strcmp(identity, shown_mission) == 0) {
		// Menu setup/onOpen can overwrite the description item after the first lobby patch.
		// Reapply the cached text on every tick instead of waiting for the mission to change.
		patch_lobby_description(menu, 1, shown_description);
		return;
	}
	lstrcpynA(shown_mission, identity, sizeof(shown_mission));

	// Only campaign Mission Select entries have a known levelshot. The Museum material was
	// confirmed in levels.menu as levelshot_mw2_museum; never look up levelshot_mw2_ending.
	const Mission *listed = missions_find(shown); // content/release/missions.csv: "picture" is 1 for the missions of Mission Select
	int in_list = listed && listed->picture;
	if (!in_list) {
		shown_description = NULL;
		patch_lobby_description(menu, 1, NULL);
		log_printf("menus: no picture for '%s'", map);
		return;
	}

	static char material[96], text[96], desc_key[112];
	snprintf(material, sizeof(material), "levelshot_mw2_%s", shown);
	if (museum) {
		lstrcpynA(text, "MUSEUM", sizeof(text));
		lstrcpynA(desc_key, "MW2COOP_MUSEUM_DESC", sizeof(desc_key));
	} else {
		int n = snprintf(text, sizeof(text), "@MENU_SP_SP_");
		for (const char *m = shown; *m && n < (int)sizeof(text) - 1; m++)
			text[n++] = (char)((*m >= 'a' && *m <= 'z') ? *m - 32 : *m);
		text[n] = 0;

		int d = snprintf(desc_key, sizeof(desc_key), "MENU_SP_SP_");
		for (const char *m = shown; *m && d < (int)sizeof(desc_key) - 6; m++)
			desc_key[d++] = (char)((*m >= 'a' && *m <= 'z') ? *m - 32 : *m);
		snprintf(desc_key + d, sizeof(desc_key) - d, "_DESC");
	}

	const char *description = localize_text(desc_key);
	shown_description = description;
	patch_lobby_description(menu, 1, description);
	typedef void *(*find_t)(int, const char *);
	void *material_ptr = ((find_t)(base + RVA_DB_FIND_HEADER))(ASSET_TYPE_MATERIAL, material);
	*(void **)(pic_item + ITEM_MATERIAL_EXP) = NULL;
	*(void **)(pic_item + ITEM_BACKGROUND) = material_ptr;
	*(void **)(name_item + ITEM_TEXT_EXP) = NULL;
	*(const char **)(name_item + ITEM_TEXT) = text;
	log_printf("menus: lobby shows %s (%s), description %s", material, text,
		description ? "loaded" : "not available");
}

// Ready state: each PC sends its own to the partner every second while the lobby is open
// ("\xff\xff\xff\xffmw2coop_ready 0|1 <player id> <Steam ID>", net.c); a partner not heard from
// for 3 s is not there. The player id (settings.ini) and the Steam ID tell the host who the
// partner is: continuing saved progress with someone else asks first (mw2coop_partner popup).
#define READY_TAG "mw2coop_ready "
static volatile LONG my_ready, partner_ready, partner_saves;
static volatile DWORD partner_heard, ready_sent;
static char partner_id[33];
static uint64_t partner_xuid;
static int continuing;		// the host's lobby opened on its saved progress
static volatile LONG partner_new;	// a partner id arrived: look up the pair's save (tick)
static volatile LONG partner_confirmed;	// the host answered both partner popups
static int add_member;		// ...and said yes to adding the partner to the save's members

static int partner_present(void)
{
	return partner_heard && GetTickCount() - partner_heard < 3000;
}

int menus_partner_saves(void)
{
	return partner_saves != 0;
}

static int party_full(void)
{
	uint8_t *lobby = *(uint8_t **)(base + RVA_PARTY_LOBBY);
	if (!lobby)
		return 0;
	int members = 0;
	for (int i = 0; i < 2; i++)
		members += FN_SLOT_USED(lobby, i) != 0;
	return members == 2;
}

// The snapshot of the playthrough the host continues (saves.c, progress.c): the host's ready packets name its key ("snapkey=<key>"), the client's answer with the
// tag of its own copy under that key ("snaptag=<key>:<tag>", "-" for none). The host starts the snapshot only when the tags are the same.
static char partner_snapkey[48];     // (client) the key the host's lobby continues
static char partner_snaptag[120];    // (host) the client's "<key>:<tag>"

static void send_ready(void)
{
	char text[200], snap[128] = "";
	if (base[RVA_PARTY_IS_HOST]) {
		char key[48], tag[64];
		if (continuing && progress_chosen_snapshot(key, sizeof(key), tag, sizeof(tag)))
			snprintf(snap, sizeof(snap), " snapkey=%s", key);
	} else if (partner_snapkey[0]) {
		char tag[64];
		snprintf(snap, sizeof(snap), " snaptag=%s:%s", partner_snapkey, progress_snapshot_tag(partner_snapkey, tag, sizeof(tag)) ? tag : "-");
	}
	// (the host: "full=1" while its party holds both players: a partner who joins it again after a break knows he is in, rejoin.c)
	snprintf(text, sizeof(text), READY_TAG "%ld %s %016llx saves=%d%s%s", my_ready, settings_player_id(),
		 (unsigned long long)*(uint64_t *)(base + RVA_XUID_TABLE), saves_supported(), snap, base[RVA_PARTY_IS_HOST] && party_full() ? " full=1" : "");
	net_send_mod(text);
	ready_sent = GetTickCount();
}

// net.c: a lobby query from the network, or a host's answer to ours. Returns 1 if it was one.
int menus_on_lobby_packet(const char *buf, int len, const uint8_t *from_ip, uint16_t from_port, char *reply,
			  int reply_size, int *reply_len)
{
	*reply_len = 0;
	static const char find[] = "\xff\xff\xff\xff" FIND_TAG;
	static const char here[] = "\xff\xff\xff\xff" HERE_TAG;
	if (len == (int)sizeof(find) - 1 && memcmp(buf, find, len) == 0) {
		// answer only while hosting a campaign lobby that still has room
		char lock = access_lobby_flag();
		if (base[RVA_PARTY_IS_HOST] && lobby_campaign_set >= 2 && !party_full() && lock) {
			char map[64] = "", name[33] = "";
			game_dvar_raw("coop_mapName", map, sizeof(map));
			// the host's name as its party has it (slot 0, rva 0x5dd960, set by the party join 0xd7220)
			lstrcpynA(name, (const char *)base + RVA_PARTY_MEMBER_NAMES, sizeof(name));
			uint64_t xuid = *(uint64_t *)(base + RVA_XUID_TABLE);
			*reply_len = snprintf(reply, reply_size, "%s%016llx %c %s %s", here, (unsigned long long)xuid, lock, map, name);
			log_printf("menus: answered a lobby query from %u.%u.%u.%u:%u", from_ip[0], from_ip[1], from_ip[2],
				   from_ip[3], from_port);
		}
		return 1;
	}
	if (len >= (int)sizeof(here) - 1 && memcmp(buf, here, sizeof(here) - 1) == 0) {
		if (!finding)
			return 1;
		// "<xuid> <p|-> <map> <host name>"
		char text[128];
		int n = len - ((int)sizeof(here) - 1);
		n = n < (int)sizeof(text) - 1 ? n : (int)sizeof(text) - 1;
		memcpy(text, buf + sizeof(here) - 1, n);
		text[n] = 0;
		char *flag = strchr(text, ' ');
		if (!flag || flag[1] == 0 || flag[2] != ' ')
			return 1;
		*flag++ = 0;
		char *map = flag + 2;
		char *name = strchr(map, ' ');
		if (name)
			*name++ = 0;
		uint64_t xuid = strtoull(text, NULL, 16);
		// Anybody on the network can answer a search, and the list shows these texts: the map is a plain name, and the host name loses control bytes and every
		// '^' that is not a colour (the engine reads "^\x01" plus a few bytes as a picture and takes a material pointer from the text: audit 2026-11-18, menus F3).
		for (char *s = map, *d = map; ; s++) {
			if (!*s) {
				*d = 0;
				break;
			}
			if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_')
				*d++ = *s;
		}
		if (name)
			for (char *s = name, *d = name; ; s++) {
				if (!*s) {
					*d = 0;
					break;
				}
				if ((unsigned char)*s < 0x20 || (*s == '^' && !(s[1] >= '0' && s[1] <= '9')))
					continue;
				*d++ = *s;
			}
		EnterCriticalSection(&lobby_lock);
		int known = 0;
		for (int i = 0; i < lobby_count; i++)
			known |= lobbies[i].xuid == xuid;
		if (!known && lobby_count < MAX_LOBBIES) {
			Lobby *l = &lobbies[lobby_count++];
			memcpy(l->ip, from_ip, 4);
			l->port = from_port;
			l->xuid = xuid;
			l->locked = *flag == 'p';
			lstrcpynA(l->map, map, sizeof(l->map));
			lstrcpynA(l->name, name ? name : "", sizeof(l->name));
			log_printf("menus: lobby at %u.%u.%u.%u:%u: %s, host '%s'%s", from_ip[0], from_ip[1], from_ip[2],
				   from_ip[3], from_port, l->map, l->name, l->locked ? ", password" : "");
		}
		LeaveCriticalSection(&lobby_lock);
		return 1;
	}
	return 0;
}

static volatile LONG brief_partner_done; // the partner's briefing film is over or skipped (the packet mw2coop_briefdone)
static volatile DWORD brief_notify_until; // the partner is told "my briefing is over" at every poll until then (a skip on this PC, any way it happens; UDP may lose one)
static volatile LONG host_full;		  // (client) the host's last ready packet said its party holds both players
int menus_on_packet(const char *buf, int len)
{
	static const char brief[] = "\xff\xff\xff\xffmw2coop_briefdone";
	if (len >= (int)sizeof(brief) - 1 && memcmp(buf, brief, sizeof(brief) - 1) == 0) {
		if (!InterlockedExchange(&brief_partner_done, 1))
			log_printf("menus: the partner's briefing film is over or skipped");
		return 1;
	}
	static const char tag[] = "\xff\xff\xff\xff" READY_TAG;
	int tl = (int)sizeof(tag) - 1;
	if (len < tl + 1 || memcmp(buf, tag, tl) != 0)
		return 0;
	LONG ready = buf[tl] == '1';
	char rest[200] = "", id[33] = "";
	int n = len - tl - 1 < (int)sizeof(rest) - 1 ? len - tl - 1 : (int)sizeof(rest) - 1;
	if (n > 0)
		memcpy(rest, buf + tl + 1, n);
	unsigned long long xuid = 0;
	if (sscanf(rest, " %32s %llx", id, &xuid) >= 1 && strcmp(id, partner_id) != 0) {
		lstrcpynA(partner_id, id, sizeof(partner_id));
		partner_xuid = xuid;
		log_printf("menus: partner id %s, Steam ID %016llx", partner_id, xuid);
		InterlockedExchange(&partner_new, 1);
	}
	// the snapshot the host continues, and the client's copy of it
	const char *sk = strstr(rest, "snapkey="), *st = strstr(rest, "snaptag=");
	char word[120] = "";
	if (sk && sscanf(sk + 8, "%47s", word) == 1 && strcmp(word, partner_snapkey) != 0) {
		lstrcpynA(partner_snapkey, word, sizeof(partner_snapkey));
		log_printf("menus: the host continues the playthrough %s", partner_snapkey);
	} else if (!sk)
		partner_snapkey[0] = 0;
	if (st && sscanf(st + 8, "%119s", word) == 1 && strcmp(word, partner_snaptag) != 0) {
		lstrcpynA(partner_snaptag, word, sizeof(partner_snaptag));
		log_printf("menus: the partner's checkpoint of this playthrough: %s", partner_snaptag);
	}
	// the partner's build loads the game's checkpoints ("saves=1"; an older mod sends nothing): the host turns them on for the session only when both do (progress.c)
	LONG saves = strstr(rest, "saves=1") != NULL;
	if (InterlockedExchange(&partner_saves, saves) != saves)
		log_printf("menus: the partner's build %s the game's checkpoints", saves ? "loads" : "does not load");
	InterlockedExchange(&host_full, strstr(rest, " full=1") != NULL);
	if (InterlockedExchange(&partner_ready, ready) != ready || !partner_present())
		log_printf("menus: partner %s", ready ? "ready" : "not ready");
	partner_heard = GetTickCount();
	return 1;
}

// the native split screen lobby: both players pressed a button on their devices
static int split_devices_chosen(void)
{
	return devices_get(0) != DEVICE_NONE && devices_get(1) != DEVICE_NONE;
}

// The split screen lobby's difficulty of each player (the console game has one popup for each: popmenu_specialops_splitscreen_difficulty_p1 / _p2, g_gameskill_player_1 / _2):
// 0 Recruit .. 3 Veteran, -1 until the lobby sets it from the profile's. The level scripts take it from the flagged dvars mw2coop_skill_p1 / p2 (maps\_mw2coop::player_gameskill).
static volatile LONG split_skill[2] = {-1, -1};

// mw2coop_split_skill <player 1|2> <0-3>: a popup row
static void split_skill_f(void)
{
	const int pl = game_cmd_argc() > 1 ? atoi(game_cmd_argv(1)) : 0, v = game_cmd_argc() > 2 ? atoi(game_cmd_argv(2)) : -1;
	if (pl < 1 || pl > 2 || v < 0 || v > 3 || !split_mode)
		return;
	InterlockedExchange(&split_skill[pl - 1], v);
	char cmd[48];
	snprintf(cmd, sizeof(cmd), "set mw2coop_skill_p%d %d\n", pl, v);
	game_cbuf_addtext(cmd);
	log_printf("menus: split screen difficulty of player %d: %d", pl, v);
}

// The rows' texts ("Difficulty, Alpha: Regular") and the dvars the scripts read; the defaults are the profile's difficulty
static void split_skill_texts(void)
{
	static const char *const names[4] = {"MENU_RECRUIT", "MENU_REGULAR", "MENU_HARDENED", "MENU_VETERAN"};
	if (!split_mode) {
		for (int pl = 0; pl < 2; pl++)
			if (split_skill[pl] >= 0) {
				InterlockedExchange(&split_skill[pl], -1);
				game_cbuf_addtext(pl ? "set mw2coop_skill_p2 \"\"\n" : "set mw2coop_skill_p1 \"\"\n"); // an online or a later session: the host's difficulty again
			}
		return;
	}
	const char *label = localize_text("MW2COOP_SKILL_LABEL");
	for (int pl = 0; pl < 2; pl++) {
		if (split_skill[pl] < 0) {
			dvar_t *g = game_find_dvar("g_gameskill");
			int v = g && g->type == DVAR_TYPE_INT && g->current.integer >= 0 && g->current.integer <= 3 ? g->current.integer : 1;
			InterlockedExchange(&split_skill[pl], v);
			char cmd[48];
			snprintf(cmd, sizeof(cmd), "set mw2coop_skill_p%d %d\n", pl + 1, v);
			game_cbuf_addtext(cmd);
		}
		const char *sign = localize_text(pl ? "MW2COOP_CALLSIGN_B" : "MW2COOP_CALLSIGN_A"), *lv = localize_text(names[split_skill[pl]]);
		char line[128];
		snprintf(line, sizeof(line), "%s, %s: %s", label ? label : "Difficulty", sign ? sign : pl ? "Bravo" : "Alpha", lv ? lv : "");
		localize_set_text(pl ? "MW2COOP_SKILL2" : "MW2COOP_SKILL1", line);
	}
}

static void update_texts(void)
{
	int host = base[RVA_PARTY_IS_HOST];
	split_skill_texts();
	// a split screen lobby has no Ready button: the second player is on this PC, his device is all that is needed
	if (split_mode)
		localize_set_text("MW2COOP_READY", "");
	else
		localize_select("MW2COOP_READY", my_ready ? "MW2COOP_READY_UNSET" : "MW2COOP_READY_SET");
	if (!split_mode)
		show_row(ui_find_menu("coop_lobby"), "dont_selectdiff", 1); // (a split screen lobby: patch_split_lobby hides it; each player has a difficulty row there)
	const char *start;
	if (split_mode)
		start = split_devices_chosen() ? "MW2COOP_START_GO" : "MW2COOP_START_NEED_DEVICES";
	else if (!partner_present() || (host && !party_full()))
		start = "MW2COOP_START_NEED_PARTNER";
	else if (my_ready && partner_ready)
		start = host ? "MW2COOP_START_GO" : "MW2COOP_START_HOST";
	else if (my_ready)
		start = "MW2COOP_START_WAIT_PARTNER";
	else if (partner_ready)
		start = "MW2COOP_START_WAIT_YOU";
	else
		start = "MW2COOP_START_WAIT_BOTH";
	localize_select("MW2COOP_START", start);
}

// mw2coop_ready: the lobby's ready button (both PCs)
static void ready_f(void)
{
	if (split_mode)
		return; // no Ready button in a split screen lobby
	InterlockedExchange(&my_ready, !my_ready);
	log_printf("menus: %s", my_ready ? "ready" : "not ready");
	send_ready();
	update_texts();
}

static void partner_questions(void);
static void snapshot_question(void);
static void start_launch(void);

// devices.c: a player's device changed
// (devices.c, its own thread) a press changed who plays on what: the texts are made again by the menu tick on the main thread. Made here, they looked
// up assets and changed the lobby's items from the devices thread, racing the UI (audit 2026-11-18, menus F7).
static volatile LONG split_texts_wanted;
void menus_split_changed(void)
{
	InterlockedExchange(&split_texts_wanted, 1);
}

// The split screen's Start (src/native.c): the second player is on this PC, in this process. The lobby's party is let go, the game becomes a two local players game and the
// mission the lobby shows starts as a normal level.
// The Special Ops ops with two roles, one player in the air and one on the ground (Overwatch: the AC-130; Big Brother: the helicopter's gun; specOpsTable.csv column 11 is 2).
// The game's lobby Start opens its popup so_character_select_coop ("Select the AC130 gunship" / the helicopter, rows player1 and player2 with the players' names); a row runs
// `setdvar coop_start so_char_host` (player 1 in the air) or `so_char_client` (player 2), then coopLaunch, and the level reads coop_start (with nothing there Overwatch put player
// 1 and Big Brother player 2 in the air). The split screen lobby's Start is ours: it opens the same popup, whose rows run mw2coop_split_role instead there (patch_role_popup),
// which sets coop_start and starts the split screen session (Vlad, 2026-11-21: "you cannot choose who flies the AC-130 and who runs"; owner: as the game does it).
static int split_role = -1; // 0 Alpha (player 1) in the air, 1 Bravo; -1 not asked yet for this start
static int two_role_op(const char *map)
{
	return !strcmp(map, "so_ac130_co_hunted") || !strcmp(map, "so_chopper_invasion");
}

// The rows of the game's role popup: in a split screen lobby they start our session (coopLaunch is the online launch); the game's own actions everywhere else
static void patch_role_popup(int split)
{
	static const char *const rows[2] = {"player1", "player2"};
	static const char *const scripts[2] = {"\"play\" \"mouse_click\" ; \"close\" \"self\" ; \"exec\" \"mw2coop_split_role 1\" ; ",
					       "\"play\" \"mouse_click\" ; \"close\" \"self\" ; \"exec\" \"mw2coop_split_role 2\" ; "};
	static uint8_t *seen[2];
	static void *native[2], *ours[2];
	uint8_t *menu = ui_find_menu("so_character_select_coop");
	if (!menu)
		return;
	for (int r = 0; r < 2; r++) {
		int at = ui_item_index(menu, rows[r]);
		if (at < 0)
			continue;
		uint8_t *item = (*(uint8_t ***)(menu + MENU_ITEMS))[at];
		void **field = (void **)(item + ITEM_ACTION);
		if (item != seen[r]) { // first look, or the UI zone was reloaded
			seen[r] = item;
			native[r] = *field;
		}
		if (!ours[r])
			ours[r] = ui_script_set(scripts[r]);
		void *want = split ? ours[r] : native[r];
		if (want && *field != want)
			*field = want;
	}
}

static volatile LONG native_started; // tick count of the launch, for tick_f's check that the session ends
static volatile LONG native_level_seen; // set by menus_poll while the server runs; tick_f is not queued during a level
static void native_launch(void)
{
	if (native_split_active()) // a second press of Start while the mission loads
		return;
	if (!split_devices_chosen()) {
		log_printf("menus: Start: the players have not both pressed a button on their devices yet");
		return;
	}
	char map[64] = "";
	game_dvar_raw("coop_mapName", map, sizeof(map));
	if (!map[0] || strpbrk(map, " \t;\"\n"))
		lstrcpyA(map, "trainer");
	int specops = so_name(map);
	if (specops && two_role_op(map) && split_role < 0) { // who is in the air: the game's own popup asks first (its row runs mw2coop_split_role here, which starts again)
		patch_role_popup(1);
		game_cbuf_addtext("openmenu so_character_select_coop\n");
		log_printf("menus: Start: '%s' has two roles, asking who is in the air", map);
		return;
	}
	// the second player is a member of the save like a partner who joins it (a Special Ops op has no save)
	if (!specops) {
		progress_begin(continuing, "split-screen", 0x53504c4954ULL, "Player 2", 1);
		// a playthrough last played in split screen goes on from its checkpoint (saves.c); one played over the network starts at its start point here
		char key[48], tag[64];
		const size_t n = continuing && progress_chosen_snapshot(key, sizeof(key), tag, sizeof(tag)) ? strlen(tag) : 0;
		const int load = n > 6 && strcmp(tag + n - 6, "_split") == 0;
		if (n)
			log_printf("menus: Start: the playthrough's checkpoint %s %s", tag, load ? "is loaded" : "was not made in split screen: the start point");
		game_cbuf_addtext(load ? "set mw2coop_snapload 1\n" : "set mw2coop_snapload 0\n");
	}
	int dev0 = devices_get(0), dev1 = devices_get(1);
	if (!native_split_start2(dev0, dev1)) {
		log_printf("menus: Start: the split screen could not be started");
		return;
	}
	InterlockedExchange(&native_level_seen, 0);
	log_printf("menus: Start: native split screen, mission '%s', player 1 on %d, player 2 on %d (-1 keyboard)", map, dev0, dev1);
	lstrcpynA(split_return_map, map, sizeof(split_return_map));
	split_return_devices[0] = dev0;
	split_return_devices[1] = dev1;
	InterlockedExchange(&split_return_devices_valid, 1);
	devices_end();
	InterlockedExchange(&split_mode, 0);
	((void (*)(void))(base + RVA_PARTY_RESET))();
	{ // each player's difficulty for the level (progress_split_skill): each player's row of the lobby (a campaign mission and a Special Ops op alike)
		const int lobby = progress_skill_lobby();
		const int p1 = split_skill[0] >= 0 ? (int)split_skill[0] : lobby, p2 = split_skill[1] >= 0 ? (int)split_skill[1] : lobby;
		if (p1 >= 0 && p2 >= 0)
			progress_split_skill(p1, p2);
	}
	char cmd[224], role[48] = "";
	if (!specops)
		campaign_loading_begin(); // (a Special Ops op keeps its own loading screen: specialops stays 1)
	if (specops && two_role_op(map)) { // the scripts read the role from coop_start: so_char_host puts player 1 in the air, so_char_client player 2
		snprintf(role, sizeof(role), "set coop_start %s\n", split_role == 1 ? "so_char_client" : "so_char_host");
		log_printf("menus: Start: %s is in the air", split_role == 1 ? "player 2" : "player 1");
	}
	split_role = -1; // (asked again at the next start)
	snprintf(cmd, sizeof(cmd), "stopListening\nset mw2coop_split_view 0\n%s%smap %s\n", role, specops ? "set specialops 1\n" : "", map);
	game_cbuf_addtext(cmd);
	InterlockedExchange(&native_started, (LONG)GetTickCount() | 1);
}

// mw2coop_split_role <1|2>: the role popup's row (the player in the air), then the Start goes on
static void split_role_f(void)
{
	const int pl = game_cmd_argc() > 1 ? atoi(game_cmd_argv(1)) : 0;
	if (pl < 1 || pl > 2 || !split_mode)
		return;
	split_role = pl - 1;
	native_launch();
}

// tick_f: the session ends once the game is back in the menus (or when the level never started)
static void native_session_tick(void)
{
	if (!native_split_active() || !native_started) { // (a session the settings file started is not ours to end)
		InterlockedExchange(&native_level_seen, 0);
		return;
	}
	char v[16] = "";
	game_dvar_raw("sv_running", v, sizeof(v));
	int running = v[0] == '1';
	if (running)
		InterlockedExchange(&native_level_seen, 1); // fallback if a menu tick happens during level startup
	int level_seen = InterlockedCompareExchange(&native_level_seen, 0, 0) != 0;
	if ((level_seen && !running) || (!level_seen && GetTickCount() - (DWORD)native_started > 30000)) {
		int completed = level_seen && !running;
		InterlockedExchange(&native_level_seen, 0);
		InterlockedExchange(&native_started, 0);
		native_split_stop();
		if (completed && split_return_map[0]) {
			InterlockedExchange(&split_lobby_return_tick, (LONG)GetTickCount());
			InterlockedExchange(&split_lobby_return_attempts, 1);
			InterlockedExchange(&split_lobby_returning, 1);
			InterlockedExchange(&split_pending, 1);
			host_lobby(split_return_map);
			log_printf("menus: %s ended; returning split screen to coop_lobby on '%s'", so_name(split_return_map) ? "Special Ops op" : "campaign mission", split_return_map);
		} else {
			InterlockedExchange(&split_return_devices_valid, 0);
			split_return_map[0] = 0;
		}
	}
}

// mw2coop_start: the lobby's Start in campaign mode. The party frame launches once both slots
// are taken and the bandwidth test is done.
static void start_f(void)
{
	if (split_mode) { // the second player is on this PC: no party, the host check does not apply
		native_launch();
		return;
	}
	if (!base[RVA_PARTY_IS_HOST]) {
		log_printf("menus: Start: only the host starts");
		return;
	}
	if (!access_can_start())
		return; // password mode without a password: the popup asks for it
	if (!party_full() || !partner_present()) {
		log_printf("menus: Start: waiting for the second player");
		return;
	}
	if (!my_ready || !partner_ready) {
		log_printf("menus: Start: not everyone is ready (host %ld, partner %ld)", my_ready, partner_ready);
		return;
	}
	start_launch();
}

// the launch itself, once everyone is in and ready
static void start_launch(void)
{
	// A save with a snapshot (the game's checkpoint of its last session, saves.c) goes on from that checkpoint, on both PCs, so the partner must hold the same
	// one: the partner it was played with (owner, 2026-11-17: "then just block the save from starting if the player is not the same"). Anyone else is offered a
	// new game. A save from before snapshots asks about the members as before.
	char key[48], tag[64], want[120];
	int load = 0;
	if (continuing && progress_chosen_snapshot(key, sizeof(key), tag, sizeof(tag))) {
		snprintf(want, sizeof(want), "%s:%s", key, tag);
		const size_t n = strlen(tag);
		if (n > 4 && strcmp(tag + n - 4, "_net") == 0 && strcmp(partner_snaptag, want) == 0) {
			load = 1;
			log_printf("menus: Start: the partner holds this playthrough's checkpoint %s: both go on from it", want);
		} else if (!partner_confirmed) {
			log_printf("menus: Start: the partner does not hold this playthrough's checkpoint %s (his: '%s'): the save does not start", want,
				   partner_snaptag[0] ? partner_snaptag : "none");
			snapshot_question();
			game_cbuf_addtext("openmenu mw2coop_newgame\n");
			return;
		}
	} else if (continuing && !partner_confirmed && progress_partner_is_new(partner_id, partner_xuid)) {
		log_printf("menus: Start: the partner is not a member of this save, asking");
		partner_questions();
		game_cbuf_addtext("openmenu mw2coop_partner\n");
		return;
	}
	progress_begin(continuing, partner_id, partner_xuid, (const char *)base + RVA_PARTY_MEMBER_NAMES + 0x38, // slot 1
		       add_member);
	rejoin_session_begin(1, (const char *)base + RVA_PARTY_MEMBER_NAMES + 0x38, partner_xuid, partner_id); // (if it breaks off, both go on from its last checkpoint)
	game_cbuf_addtext(load ? "set mw2coop_snapload 1\n" : "set mw2coop_snapload 0\n"); // (a session dvar: the start packet takes it to the client)
	log_printf("menus: Start: coopLaunch");
	campaign_loading_begin();
	game_cbuf_addtext("coopLaunch\n");
}

// The popups' questions with the names in them, in the game's language.
static void partner_questions(void)
{
	char map[64] = "", members[96] = "", key[80] = "MENU_SP_SP_", text[256];
	const char *partner = (const char *)base + RVA_PARTY_MEMBER_NAMES + 0x38; // slot 1
	progress_chosen_info(map, sizeof(map), members, sizeof(members));
	int n = (int)strlen(key);
	for (const char *m = map; *m && n < (int)sizeof(key) - 1; m++)
		key[n++] = (char)((*m >= 'a' && *m <= 'z') ? *m - 32 : *m);
	key[n] = 0;
	const char *mission = localize_text(key), *fmt;
	// (the formats come from the .str files a translation may replace: one whose conversions differ is not used, audit 2026-11-18, menus F10)
	if ((fmt = localize_text("MW2COOP_PARTNER_FMT")) && ctexts_fmt_ok(fmt, "sss")) {
		snprintf(text, sizeof(text), fmt, mission ? mission : map, members, partner);
		localize_set_text("MW2COOP_PARTNER_Q", text);
	}
	if ((fmt = localize_text("MW2COOP_MEMBER_FMT")) && ctexts_fmt_ok(fmt, "s")) {
		snprintf(text, sizeof(text), fmt, partner);
		localize_set_text("MW2COOP_MEMBER_Q", text);
	}
	if ((fmt = localize_text("MW2COOP_NEWGAME_FMT")) && ctexts_fmt_ok(fmt, "s")) {
		snprintf(text, sizeof(text), fmt, partner);
		localize_set_text("MW2COOP_NEWGAME_Q", text);
	}
}

// The new game popup's question when the partner does not hold the save's checkpoint
static void snapshot_question(void)
{
	char text[320];
	const char *partner = (const char *)base + RVA_PARTY_MEMBER_NAMES + 0x38; // slot 1
	const char *fmt = localize_text("MW2COOP_SNAPBLOCK_FMT");
	if (fmt && ctexts_fmt_ok(fmt, "ss")) {
		snprintf(text, sizeof(text), fmt, partner, partner);
		localize_set_text("MW2COOP_NEWGAME_Q", text);
	}
}

// mw2coop_partner_yes: the first popup's Yes (continue with this partner): ask about the members
static void partner_yes_f(void)
{
	game_cbuf_addtext("openmenu mw2coop_member\n");
}

// mw2coop_member_yes: the second popup's Yes, the partner joins this save's members
static void member_yes_f(void)
{
	add_member = 1;
	log_printf("menus: the partner joins this save's members");
	InterlockedExchange(&partner_confirmed, 1);
	start_f();
}

// mw2coop_member_no: the second popup's No, ask about a new game
static void member_no_f(void)
{
	game_cbuf_addtext("openmenu mw2coop_newgame\n");
}

// mw2coop_new_game: the third popup's Yes, a new game from the start (The Pit, a new save); the
// old save stays as it was
static void new_game_f(void)
{
	add_member = 0;
	continuing = 0;
	game_cbuf_addtext("set coop_mapName trainer\nset start \"\"\n"); // before start_f's coopLaunch
	log_printf("menus: a new game from the start");
	InterlockedExchange(&partner_confirmed, 1);
	start_f();
}

// A yes/no question (a copy of the quit popup, as firewall.c does).
static void yes_no_popup(const char *name)
{
	if (ui_find_menu(name))
		return;
	// the heading, the question and the Yes / No rows come from menus.menu
	const MenuItem *yes = menutable_item(name, 0), *no = menutable_item(name, 1);
	const char *title = menutable_title(name), *question = menutable_question(name);
	if (!yes || !no || !title || !question) {
		log_printf("menus: menus.menu has no complete yes/no popup %s", name);
		return;
	}
	// From the difficulty popup (rows_popup), not the quit popup: the difficulty popup is opened
	// from the lobby in Spec Ops. Its own text items stay hidden whatever they are given, so the
	// question (up to three lines) is a copy of the title item one line under it; rows: Yes, No,
	// lowered under the question.
	const char *const names[LIST_ROWS] = {yes->name, no->name, "mw2coop_row_3", "mw2coop_row_4"};
	const char *const texts[LIST_ROWS] = {yes->text, no->text, "", ""};
	const char *const actions[LIST_ROWS] = {yes->action, no->action, "", ""};
	uint8_t *menu = rows_popup(name, title, names, texts, actions);
	if (!menu)
		return;
	show_row(menu, "mw2coop_row_3", 0);
	show_row(menu, "mw2coop_row_4", 0);
	int count = *(int *)(menu + MENU_ITEM_COUNT);
	uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
	int at = -1;
	for (int i = 0; i < count; i++)
		if (*(const char **)(items[i] + ITEM_TEXT) == title)
			at = i;
	if (at < 0)
		return;
	uint8_t *text = ui_clone_item(items[at], menu); // by a difference: the two rects hold different origins
	*(const char **)(text + ITEM_TEXT) = question;
	*(float *)(text + ITEM_RECT_Y) += 26;
	*(float *)(text + ITEM_RECT_CLIENT_Y) += 26;
	uint8_t **more = HeapAlloc(GetProcessHeap(), 0, (count + 1) * sizeof(*more));
	memcpy(more, items, count * sizeof(*more));
	more[count] = text;
	*(uint8_t ***)(menu + MENU_ITEMS) = more;
	*(int *)(menu + MENU_ITEM_COUNT) = count + 1;
	int rows[2] = {ui_item_index(menu, yes->name), ui_item_index(menu, no->name)};
	for (int i = 0; i < 2; i++) {
		*(float *)(more[rows[i]] + ITEM_RECT_Y) += 72;
		*(float *)(more[rows[i]] + ITEM_RECT_CLIENT_Y) += 72;
	}
	log_printf("menus: popup %s added", name);
}

void menus_yes_no_popup(const char *name)
{
	yes_no_popup(name);
}

void menus_show_row(uint8_t *menu, const char *row, int visible)
{
	show_row(menu, row, visible);
}

// Start with a partner who is not a member of the save: continue with him? Then: add him to this
// playthrough's members? No to that: start a new game from the very beginning? No anywhere closes
// the popup and nothing starts.
static void add_partner_popups(void)
{
	yes_no_popup("mw2coop_partner");
	yes_no_popup("mw2coop_member");
	yes_no_popup("mw2coop_newgame");
	yes_no_popup("mw2coop_rejoin");
}

// mw2coop_online: run by the campaign menu's co-op button before "listen" opens the lobby
static void online_f(void)
{
	lobby_music_patch(1); // second chance before listen activates coop_lobby
	if (rejoin_state() == REJOIN_CLIENT_JOIN)
		rejoin_end("the player hosts a lobby of his own");
	char map[64] = "trainer", start[64] = "";
	const char *chosen = game_cmd_argv(1);
	continuing = 0;
	add_member = 0;
	InterlockedExchange(&partner_confirmed, 0);
	if (chosen[0] && !strpbrk(chosen, " \t;\"\n")) {
		lstrcpynA(map, chosen, sizeof(map));
		log_printf("menus: campaign lobby with '%s' from the start", map);
	} else if (progress_chosen(map, sizeof(map), start, sizeof(start))) {
		log_printf("menus: campaign lobby, continuing at '%s' start '%s'", map, start);
		continuing = 1;
	}
	else
		log_printf("menus: campaign lobby, no progress yet: '%s'", map);
	char cmd[320], spot[64] = "";
	if (continuing)
		progress_chosen_spot(spot, sizeof(spot));
	snprintf(cmd, sizeof(cmd), "set coop_mapName %s\nset start \"%s\"\nset mw2coop_cp_start \"%s\"\nset mw2coop_cp_pos \"%s\"\nset mw2coop_snapload 0\n", map,
		 start, spot[0] ? start : "", spot);
	game_cbuf_addtext(cmd);
}

// devmap in the host's lobby: run by the campaign mission list (The Pit directly, the other
// missions from the difficulty popup); it sets the lobby's mission instead of starting solo.
static void (*devmap_native)(void);

// The list shows "cheats enabled" while mis_cheat is 1: the item's text is an empty string during the co-op pick.
static void hide_cheat_text(int hide)
{
	static const char *const menus[] = {"levels", "popmenu_act1", "popmenu_act2", "popmenu_act3"};
	static const char *saved[4];
	static const char empty[] = "";
	for (size_t i = 0; i < sizeof(menus) / sizeof(menus[0]); i++) {
		uint8_t *m = ui_find_menu(menus[i]);
		int at = m ? ui_item_index(m, "cheat_indicator") : -1;
		if (at < 0)
			continue;
		const char **text = (const char **)((*(uint8_t ***)(m + MENU_ITEMS))[at] + ITEM_TEXT);
		if (hide && *text != empty) {
			saved[i] = *text;
			*text = empty;
		} else if (!hide && *text == empty) {
			*text = saved[i];
		}
	}
}

static void unlock_missions_tick(void)
{
	int want = 0;
	if (picking || campaign_lobby()) {
		static const char *const menus[] = {"levels", "popmenu_act1", "popmenu_act2", "popmenu_act3"};
		for (size_t i = 0; i < sizeof(menus) / sizeof(menus[0]); i++) {
			uint8_t *m = ui_find_menu(menus[i]);
			if (m && (*(uint32_t *)(m + ITEM_DYNAMIC_FLAGS) & WINDOW_VISIBLE))
				want = 1;
		}
	}
	if (cheat_on)
		hide_cheat_text(want);
	if (want && !cheat_on) {
		char buf[32];
		game_dvar_string("mis_cheat", buf, sizeof(buf));
		snprintf(cheat_old, sizeof(cheat_old), "%s",
			 (strcmp(buf, "<missing>") == 0 || strcmp(buf, "false") == 0) ? "0" : (strcmp(buf, "true") == 0 ? "1" : buf));
		game_cbuf_addtext("set mis_cheat 1\n");
		cheat_on = 1;
		log_printf("menus: mission list open for the lobby: mis_cheat 1 (was %s), all missions selectable", cheat_old);
	} else if (!want && cheat_on) {
		char cmd[48];
		snprintf(cmd, sizeof(cmd), "set mis_cheat %s\n", cheat_old);
		game_cbuf_addtext(cmd);
		cheat_on = 0;
		log_printf("menus: mission list closed: mis_cheat back to %s", cheat_old);
	}
}

static void devmap_f(void)
{
	char map[64];
	lstrcpynA(map, game_sv_cmd_argv(1), sizeof(map));
	if (picking && map[0] && !strpbrk(map, " \t;\"\n")) {
		InterlockedExchange(&picking, 0);
		game_cbuf_addtext("closemenu popmenu_act1\nclosemenu popmenu_act2\nclosemenu popmenu_act3\nclosemenu levels\n");
		log_printf("menus: hosting '%s' (picked from the list)", map);
		host_lobby(map);
		return;
	}
	if (!campaign_lobby() || !base[RVA_PARTY_IS_HOST] || !map[0] || strpbrk(map, " \t;\"\n")) {
		devmap_native();
		return;
	}
	char cmd[256];
	snprintf(cmd, sizeof(cmd),
		 "set coop_mapName %s\nset start \"\"\n"
		 "closemenu popmenu_act1\nclosemenu popmenu_act2\nclosemenu popmenu_act3\nclosemenu levels\n",
		 map);
	game_cbuf_addtext(cmd);
	log_printf("menus: campaign lobby mission -> '%s'", map);
}

static void tick_f(void);
static void rejoin_yes_f(void);
static void rejoin_no_f(void);

// ---- the native split screen lobby: both players' cards, player 1's pad in the menus
// The lobby's player cards come from the menu expression coopPlayerInfo(n, field) (rva 0x2442b0: local client, its arguments (count at [0x28], n as int / float / string at
// [0]/[2], the field via rva 0x2447c0 on [4]), the result {type at [0] (0 a number, 2 a string), value at [2]}). In a split screen lobby both players are there from the
// start, by their call signs (owner, 2026-10-28): Alpha (player 1, the top half) and Bravo (player 2); Start still waits for each to press a button on his device.
#ifdef _WIN64
static void (*player_info_original)(int, int *, int *);
static const uint8_t player_info_bytes[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89,
					    0x74, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xec, 0x30};
#define PI_COUNT 0x28 // the arguments: count, then {type, value} of 16 bytes
#define PI_STRIDE 4	  // (ints)
#define PI_VALUE 2
#else
// 32 bits (docs/32bit-native-map/inputui.md): usercall, ESI = the out struct, stack (lc, args); the arguments are {type, value} of 8 bytes, the count at +0x50; the result {type, value}
static void *player_info_target;
static const uint8_t player_info_bytes[] = {0x83, 0xec, 0x08, 0x53, 0x33, 0xdb};
#define PI_COUNT 0x14
#define PI_STRIDE 2
#define PI_VALUE 1
static void player_info_original(int lc, int *args, int *out)
{
	__asm__ volatile("pushl %1\n\tpushl %2\n\tcall *%3\n\taddl $8, %%esp" : "+S"(out) : "r"(args), "r"(lc), "m"(player_info_target) : "eax", "ecx", "edx", "memory", "cc");
}
#endif
static void player_info_hook(int lc, int *args, int *out)
{
	player_info_original(lc, args, out);
	if (!split_mode || args[PI_COUNT] != 2)
		return;
	int n = args[0] == 1 ? (int)*(const float *)(args + PI_VALUE) : args[0] == 0 ? args[PI_VALUE] : atoi(*(const char **)(args + PI_VALUE));
	if (n < 0 || n > 1)
		return;
#ifdef _WIN64
	const char *field = ((const char *(*)(int *))(base + XR(0x2447c0)))(args + 4);
#else
	const char *field = args[PI_STRIDE] == 2 ? *(const char **)(args + PI_STRIDE + 1) : NULL; // the field name is a string argument (the engine's text function returns it as it is)
#endif
	if (!field)
		return;
	if (!strcmp(field, "exists")) {
		out[0] = 0;
		out[PI_VALUE] = 1;
	} else if (!strcmp(field, "skill") && split_skill[n] >= 0) { // each player's own pick (the lobby's "different difficulties" note and stars compare these)
		out[0] = 0;
		out[PI_VALUE] = (int)split_skill[n];
	} else if (n == 1 && (!strcmp(field, "percentcompleteso") || !strcmp(field, "missionsohighestdifficulty"))) {
		// both players play from this PC's profile: player 2's Spec Ops progress and stars are player 1's (the party's second slot held nothing, and the lobby said "this op is
		// locked for your partner, he will not earn stars" for every op past the first ones)
		int copy[PI_COUNT + 1];
		memcpy(copy, args, sizeof(copy));
		copy[0] = 0;
		copy[PI_VALUE] = 0;
		player_info_original(lc, copy, out);
	} else if (!strcmp(field, "name")) {
		static char names[2][32];
		const char *t = localize_text(n ? "MW2COOP_CALLSIGN_B" : "MW2COOP_CALLSIGN_A");
		snprintf(names[n], sizeof(names[n]), "%s", t ? t : n ? "Bravo" : "Alpha");
		out[0] = 2;
		*(const char **)(out + PI_VALUE) = names[n];
	}
}
#ifndef _WIN64
void n32_player_info_hook(int lc, int *args, int *out);
void n32_player_info_hook(int lc, int *args, int *out)
{
	player_info_hook(lc, args, out);
}
__asm__(".text\n"
	".globl _n32_player_info_stub\n"
	"_n32_player_info_stub:\n"
	"	pushl %esi\n"
	"	pushl 12(%esp)\n"
	"	pushl 12(%esp)\n"
	"	call _n32_player_info_hook\n"
	"	addl $12, %esp\n"
	"	ret\n");
extern char n32_player_info_stub[];
#endif

// Player 1 works the split-screen lobby; on a pad, his pad does it: while that lobby is active a thread reads it 60 times a second and queues the keys a menu knows
// (mw2coop_menukey: the engine's key event, rva 0xd0f80, on the main thread). Outside that lobby it sleeps between 100 ms checks. D-pad / left stick: arrows (a held
// direction again after 400 ms, then every 120 ms), A: Enter, B: Escape. Back is the lobby's
// "give my device back" (instance.c).
static void menukey_f(void)
{
	int key = atoi(game_cmd_argv(1));
	if (key <= 0 || key > 255 || !base)
		return;
	native_menu_key(key);
}
static DWORD WINAPI lobby_pad_thread(LPVOID unused)
{
	(void)unused;
	typedef struct {
		DWORD packet;
		WORD buttons;
		BYTE lt, rt;
		SHORT lx, ly, rx, ry;
	} xs_t;
	DWORD(WINAPI * get)(DWORD, xs_t *) = (DWORD(WINAPI *)(DWORD, xs_t *))pads_xinput_proc("XInputGetState"); // (XInput and PlayStation pads, pads.c)
	int seen = -1;
	WORD prev = 0;
	DWORD repeat_at = 0;
	for (;;) {
		// This reader only drives the split-screen lobby. In an ordinary session (and once
		// the split-screen game has started) avoid querying the device manager 60 times a second.
		if (!InterlockedCompareExchange(&split_mode, 0, 0) || native_split_active()) {
			seen = -1;
			Sleep(100);
			continue;
		}
		Sleep(16);
		int pad = devices_menu_pad();
		if (pad < 0) {
			seen = -1;
			continue;
		}
		xs_t st;
		memset(&st, 0, sizeof(st));
		if (get((DWORD)pad, &st) != ERROR_SUCCESS)
			continue;
		WORD b = st.buttons;
		if (st.ly > 20000)
			b |= 0x0001;
		if (st.ly < -20000)
			b |= 0x0002;
		if (st.lx < -20000)
			b |= 0x0004;
		if (st.lx > 20000)
			b |= 0x0008;
		if (seen != pad) { // the press that made the pad his is not a menu key
			seen = pad;
			prev = b;
			continue;
		}
		static const struct {
			WORD mask;
			int key;
		} keys[] = {{0x0001, 154}, {0x0002, 155}, {0x0004, 156}, {0x0008, 157}, {0x1000, 13}, {0x2000, 27}};
		DWORD now = GetTickCount();
		for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
			int was = (prev & keys[i].mask) != 0, is = (b & keys[i].mask) != 0;
			int send = is && !was;
			if (is && was && i < 4 && (int)(now - repeat_at) >= 0)
				send = 1;
			if (!send)
				continue;
			char cmd[32];
			snprintf(cmd, sizeof(cmd), "mw2coop_menukey %d\n", keys[i].key);
			game_cbuf_addtext(cmd);
			if (i < 4)
				repeat_at = now + (was ? 120 : 400);
		}
		prev = b;
	}
	return 0;
}

static void pause_routes_tick_f(void);
static void logo_tick_f(void);

static cmd_function_t commands[] = {
	{NULL, "mw2coop_online", NULL, NULL, online_f CMD_NODE_FLAG},
	{NULL, "mw2coop_host_menu", NULL, NULL, host_menu_f CMD_NODE_FLAG},
	{NULL, "mw2coop_split_menu", NULL, NULL, split_menu_f CMD_NODE_FLAG},
	{NULL, "mw2coop_split_skill", NULL, NULL, split_skill_f CMD_NODE_FLAG},
	{NULL, "mw2coop_split_role", NULL, NULL, split_role_f CMD_NODE_FLAG},
	{NULL, "mw2coop_open_url", NULL, NULL, open_url_f CMD_NODE_FLAG},
	{NULL, "mw2coop_iw4x_mp", NULL, NULL, iw4x_mp_f CMD_NODE_FLAG},
	{NULL, "mw2coop_split_specops", NULL, NULL, split_specops_f CMD_NODE_FLAG},
	{NULL, "mw2coop_host_go", NULL, NULL, host_go_f CMD_NODE_FLAG},
	{NULL, "mw2coop_host_new", NULL, NULL, host_new_f CMD_NODE_FLAG},
	{NULL, "mw2coop_host_pick", NULL, NULL, host_pick_f CMD_NODE_FLAG},
	{NULL, "mw2coop_start", NULL, NULL, start_f CMD_NODE_FLAG},
	{NULL, "mw2coop_menukey", NULL, NULL, menukey_f CMD_NODE_FLAG},
	{NULL, "mw2coop_briefskip", NULL, NULL, briefskip_f CMD_NODE_FLAG},
	{NULL, "mw2coop_ready", NULL, NULL, ready_f CMD_NODE_FLAG},
	{NULL, "mw2coop_partner_yes", NULL, NULL, partner_yes_f CMD_NODE_FLAG},
	{NULL, "mw2coop_member_yes", NULL, NULL, member_yes_f CMD_NODE_FLAG},
	{NULL, "mw2coop_member_no", NULL, NULL, member_no_f CMD_NODE_FLAG},
	{NULL, "mw2coop_new_game", NULL, NULL, new_game_f CMD_NODE_FLAG},
	{NULL, "mw2coop_saves_menu", NULL, NULL, saves_menu_f CMD_NODE_FLAG},
	{NULL, "mw2coop_save_pick", NULL, NULL, save_pick_f CMD_NODE_FLAG},
	{NULL, "mw2coop_menus_tick", NULL, NULL, tick_f CMD_NODE_FLAG},
	{NULL, "mw2coop_pause_routes_tick", NULL, NULL, pause_routes_tick_f CMD_NODE_FLAG},
	{NULL, "mw2coop_logo_tick", NULL, NULL, logo_tick_f CMD_NODE_FLAG},
	{NULL, "mw2coop_find", NULL, NULL, find_f CMD_NODE_FLAG},
	{NULL, "mw2coop_join_found", NULL, NULL, join_found_f CMD_NODE_FLAG},
	{NULL, "mw2coop_rejoin_yes", NULL, NULL, rejoin_yes_f CMD_NODE_FLAG},
	{NULL, "mw2coop_rejoin_no", NULL, NULL, rejoin_no_f CMD_NODE_FLAG},
};

// The menu work runs on the game's main thread, between frames: the control thread only queues
// "mw2coop_menus_tick" (every 250 ms, never while a level runs or loads). The UI zone may be
// reloaded (back from a level), which brings fresh menus without our changes.
static volatile LONG tick_queued;
static volatile DWORD tick_queued_at;
static volatile LONG pause_routes_tick_queued;
static volatile DWORD pause_routes_tick_queued_at;
static volatile DWORD pause_routes_last_queue_at;
static volatile LONG pause_menu_route_mask; // bit 0: savegame_warning; bit 1: pausedmenu
static volatile LONG pause_direct_route_ready; // set when the currently visible pause menu has a direct return
static volatile LONG logo_tick_queued; // queue the main-thread logo material swap / menu layout refresh
static volatile DWORD logo_tick_queued_at;

static void queue_logo_tick(DWORD now)
{
	LONG queued = InterlockedCompareExchange(&logo_tick_queued, 0, 0);
	if (queued && now - logo_tick_queued_at > 1500)
		InterlockedExchange(&logo_tick_queued, 0); // recover if a map/UI transition cleared the queued command
	if (InterlockedCompareExchange(&logo_tick_queued, 1, 0) == 0) {
		logo_tick_queued_at = now;
		game_cbuf_addtext("mw2coop_logo_tick\n");
	}
}

// The loading screen. The Spec Ops lobby sets the dvar "specialops" to 1, and the loading screen follows it: with 1 it is Spec Ops' (another version of the mission's film with a
// black panel and the studio logo, the bar under the level's text, "waiting for the other player"), with 0 the campaign's (the film over the whole picture, the bar at the bottom in the
// middle, nothing to wait for). Our campaign co-op and split screen use that lobby, so they would load with Spec Ops' screen: the dvar is 0 from the launch to the end of the level
// (checked: a split screen mission plays the same with 0), and Spec Ops itself is untouched (its lobby sets it again). The scripts do not look at it for the co-op: is_specialop() is
// patched (content/patches/maps/_utility.gsc.patch).
static int specialops_saved = -1;
static volatile LONG campaign_session; // set by menus_poll while a campaign co-op or split screen game is being set up or played

// the dvar is a bool in this build (only its first byte is the value); an int would be read the same way through game_dvar_raw
static int specialops_read(dvar_t *d)
{
	char v[16] = "";
	game_dvar_raw("specialops", v, sizeof(v));
	(void)d;
	return v[0] && v[0] != '0';
}

static void specialops_write(int v)
{
	dvar_t *d = game_find_dvar("specialops");
	if (!d)
		return;
	if (d->type == 0)
		*(uint8_t *)&d->current = (uint8_t)v;
	else
		d->current.integer = v;
}

// at the launch (the host's Start, a client's launch from the party, the split screen): called by the code that launches, and by the thread below for the launch that is the engine's
static volatile LONG specialops_forced; // from the launch to the end of the session the dvar is kept at 0 (the engine's co-op code may set it again)
static volatile LONG campaign_lobby_return_queued;
static volatile LONG campaign_lobby_return_pending;
static volatile LONG campaign_mission_seen;
static volatile LONG campaign_lobby_seen;
static volatile LONG campaign_main_return_pending;
static volatile LONG campaign_main_route_tick; // when main_selection was last asked for, 0 when not waiting for it
static volatile LONG campaign_main_route_tries;
static void campaign_loading_begin(void)
{
	dvar_t *d = game_find_dvar("specialops");
	if (!d)
		return;
	// Not for a Special Ops op: the client calls this when the host's start packet comes, whatever the mission. With the dvar at 0 the client's scripts took the campaign's path
	// (_loadout's is_specialop() false: no listing for an so_ level, the default loadout with weapons its zone lacks: no weapon and "no viewmodel set ... hand", the engine's
	// error a player reported for 1.0.1 as "a bad hand" in Spec Ops), while the host's took Spec Ops' (traced in a pair on so_killspree_trainer).
	{
		char map[64] = "";
		game_dvar_raw("coop_mapName", map, sizeof(map));
		if (!map[0])
			game_dvar_raw("mapname", map, sizeof(map));
		if (so_name(map)) {
			log_printf("menus: loading screen: '%s' is a Special Ops op, specialops stays as it is", map);
			return;
		}
	}
	InterlockedExchange(&specialops_forced, 1);
	InterlockedExchange(&campaign_session, 1);
	if (!specialops_read(d))
		return;
	if (specialops_saved < 0)
		specialops_saved = 1;
	specialops_write(0);
	log_printf("menus: loading screen: the campaign's (specialops 1 -> 0)");
}

void menus_campaign_loading_begin(void) // progress.c: the host's start packet has come (the client)
{
	campaign_loading_begin();
}

static void campaign_loading_end(void)
{
	if (specialops_saved < 0)
		return;
	InterlockedExchange(&specialops_forced, 0);
	specialops_write(specialops_saved);
	log_printf("menus: loading screen: specialops back to %d", specialops_saved);
	specialops_saved = -1;
}

// The engine's own launch of a client (the party frame: the coop slot state goes to 3, then "map"): this thread sees it within a few milliseconds
static DWORD WINAPI loading_thread(LPVOID unused)
{
	(void)unused;
	for (;;) {
		Sleep(15);
		if (!base)
			continue;
		if (*(volatile int32_t *)(base + RVA_COOP_SLOT_STATE) == 3 && !specialops_forced) {
			char mapn[64] = "";
			game_dvar_raw("coop_mapName", mapn, sizeof(mapn));
			if (mapn[0] && strncmp(mapn, "so_", 3) != 0)
				campaign_loading_begin();
		}
		if (specialops_forced) { // enforced: nothing may turn it back on while the campaign co-op loads or runs
			dvar_t *d = game_find_dvar("specialops");
			if (d && specialops_read(d))
				specialops_write(0);
		}
	}
	return 0;
}

// The briefing of a network co-op mission. In single player the level waits (the server frame and the UI state "pregame", ui state 3) until the mission's film is over or the player
// skips it; in the engine's co-op the same wait is the function at rva 0x22e170 ("somebody is not ready": the server frame driver and the UI frame ask it), which lets the level go on
// as soon as both PCs have loaded, so the film was cut off. Here the function also says "wait" in a campaign co-op while the film plays, and lets go when the film is over or skipped
// on either PC: each PC tells the other when its own is (the packet mw2coop_briefdone), so a skip on one PC is a skip on both. The split screen has one PC and the single player
// wait already (engine co-op byte 0).
#ifdef _WIN64
#define REF_COOP_WAIT 0x22e170
#define RVA_COOP_WAIT XR(REF_COOP_WAIT)
#define REF_COOP_BYTE_E 0x52fb5c
#define RVA_COOP_BYTE_E XR(REF_COOP_BYTE_E)	  // the engine's co-op byte
#define REF_UI_STATE 0x1a49dc4
#define RVA_UI_STATE XR(REF_UI_STATE)
#define REF_CLIENT_STATE 0x533cb8
#define RVA_CLIENT_STATE XR(REF_CLIENT_STATE)
#define REF_FILM_ON 0x45859c4
#define RVA_FILM_ON XR(REF_FILM_ON)		  // the cinematic state the single player test reads: on, and two counters that differ when it is over
#define REF_FILM_A 0x45859cc
#define RVA_FILM_A XR(REF_FILM_A)
#define REF_FILM_B 0x45859d0
#define RVA_FILM_B XR(REF_FILM_B)
#define REF_ENGINE_WAIT_A 0x192d7e3
#define RVA_ENGINE_WAIT_A XR(REF_ENGINE_WAIT_A)
#define REF_ENGINE_WAIT_B 0x192d8f4
#define RVA_ENGINE_WAIT_B XR(REF_ENGINE_WAIT_B)
#else
// the 32-bit build (the same code at VA 0x40af30; the UI frame is 0x4b5c50, its state variable 0x1926b84)
#define REF_COOP_WAIT 0xaf30
#define RVA_COOP_WAIT XR(REF_COOP_WAIT)
#define REF_COOP_BYTE_E 0x48897c
#define RVA_COOP_BYTE_E XR(REF_COOP_BYTE_E)
#define REF_UI_STATE 0x1526b84
#define RVA_UI_STATE XR(REF_UI_STATE)
#define REF_CLIENT_STATE 0x525bc0
#define RVA_CLIENT_STATE XR(REF_CLIENT_STATE)
#define REF_FILM_ON 0x1887bc0
#define RVA_FILM_ON XR(REF_FILM_ON)
#define REF_FILM_A 0x1887bc8
#define RVA_FILM_A XR(REF_FILM_A)
#define REF_FILM_B 0x1887bcc
#define RVA_FILM_B XR(REF_FILM_B)
#define REF_ENGINE_WAIT_A 0x166b0c2
#define RVA_ENGINE_WAIT_A XR(REF_ENGINE_WAIT_A)
#define REF_ENGINE_WAIT_B 0x166acf1
#define RVA_ENGINE_WAIT_B XR(REF_ENGINE_WAIT_B)
#endif
// A build whose addresses moved (addr.c): an address that was not found stands in as a block of zeros. The film flags and the UI state are such globals, and a wait that reads zeros for them
// would never see the film end: the briefing wait is off when any of the addresses it reads is not a real one of this build.
static int brief_addresses_ok(void)
{
	static int ok = -1;
	if (ok < 0)
		ok = addr_resolved(REF_COOP_BYTE_E) && addr_resolved(REF_UI_STATE) && addr_resolved(REF_CLIENT_STATE) && addr_resolved(REF_FILM_ON) && addr_resolved(REF_FILM_A) &&
		     addr_resolved(REF_FILM_B) && addr_resolved(REF_ENGINE_WAIT_A) && addr_resolved(REF_ENGINE_WAIT_B);
	if (!ok) {
		static int logged;
		if (!logged++)
			log_printf("menus: an address the briefing wait needs was not found in this build: no briefing wait");
	}
	return ok;
}
#define BRIEF_MAX_MS 240000
static DWORD brief_started, brief_last_call, brief_last_sent;
static int brief_released;

static int film_over(void)
{
	return base[RVA_FILM_ON] && *(volatile int *)(base + RVA_FILM_B) != *(volatile int *)(base + RVA_FILM_A);
}

// the logic of the wait: 1 = keep waiting. It runs whatever the engine's own flags say (a client waits for the host's release by them, but its own skip has to reach the host)
static int brief_wait(void)
{
	{ // QA runs (mw2coop_qa set) do not wait for the film, unless mw2coop_qa_brief is 1 (the test of the wait itself)
		char q[32] = "", b[8] = "";
		game_dvar_raw("mw2coop_qa", q, sizeof(q));
		game_dvar_raw("mw2coop_qa_brief", b, sizeof(b));
		if (q[0] && b[0] != '1')
			return 0;
	}
	DWORD now = GetTickCount();
	if (now - brief_last_call > 3000) { // a new briefing
		brief_started = now;
		brief_released = 0;
		brief_last_sent = 0;
	}
	brief_last_call = now;
	if (brief_released)
		return 0;
	{ // QA: mw2coop_qa_briefskip "<host|client|both> <ms>": that PC's film is made to look over that long after the wait began (a skip, without a mouse)
		char t[40] = "", who[12];
		unsigned ms = 0;
		game_dvar_raw("mw2coop_qa_briefskip", t, sizeof(t));
		if (t[0] && sscanf(t, "%11s %u", who, &ms) == 2 && now - brief_started >= ms && !film_over() &&
		    (!strcmp(who, "both") || (!strcmp(who, "host") && base[RVA_PARTY_IS_HOST]) || (!strcmp(who, "client") && !base[RVA_PARTY_IS_HOST]))) {
			base[RVA_FILM_ON] = 1;
			*(volatile int *)(base + RVA_FILM_A) = *(volatile int *)(base + RVA_FILM_B) + 2;
			log_printf("menus: QA: the briefing film is made to look over (%s, %u ms)", who, ms);
		}
	}
	int over = film_over();
	{ // QA: the film state while waiting
		static int la = -1, lb = -1, lon = -1;
		int a = *(volatile int *)(base + RVA_FILM_A), b = *(volatile int *)(base + RVA_FILM_B), on = base[RVA_FILM_ON];
		if (a != la || b != lb || on != lon) {
			log_printf("menus: briefing wait: film flag %d, counters %d %d (%u ms)", on, a, b, (unsigned)(now - brief_started));
			la = a, lb = b, lon = on;
		}
	}
	int why = over ? 1 : brief_partner_done ? 2 : now - brief_started > BRIEF_MAX_MS ? 3 : 0;
	if (over && now - brief_last_sent > 200) { // the partner learns that the film is over here (a few times: UDP)
		brief_last_sent = now;
		int sent = net_send_mod("mw2coop_briefdone");
		static int logged;
		if (logged++ < 3)
			log_printf("menus: briefing: told the partner the film is over (%s)", sent ? "sent" : "NOT sent: no partner address");
	}
	if (!why)
		return 1;
	brief_released = 1;
	log_printf("menus: briefing released (%s, %u ms)", why == 1 ? "the film is over or was skipped here" : why == 2 ? "the partner's film is over or skipped" : "time", (unsigned)(now - brief_started));
	net_send_mod("mw2coop_briefdone");
	return 0;
}

// net.c: true while this PC waits in a briefing (the hook was asked a moment ago and has not let go)
int menus_briefing_holds(void)
{
	return campaign_session && !brief_released && GetTickCount() - brief_last_call < 1500 && brief_last_call != 0;
}

static unsigned char coop_wait_hook(void)
{
	unsigned char engine = base[RVA_ENGINE_WAIT_B] ? 1 : base[RVA_ENGINE_WAIT_A];
	if (!campaign_session || !brief_addresses_ok() || !base[RVA_COOP_BYTE_E] || *(volatile int *)(base + RVA_UI_STATE) != 3)
		return engine;
	return brief_wait() ? 1 : engine;
}

// QA: mw2coop_briefskip makes the film look over on this PC (as it does when it ends or is skipped), to test the shared skip without a mouse
static void briefskip_f(void)
{
	const char *who = game_cmd_argv(1); // "client": only a PC that is not the host (the commands file of a pair is the same for both)
	if (!strcmp(who, "client") && base[RVA_PARTY_IS_HOST])
		return;
	if (!strcmp(who, "host") && !base[RVA_PARTY_IS_HOST])
		return;
	base[RVA_FILM_ON] = 1;
	*(volatile int *)(base + RVA_FILM_A) = *(volatile int *)(base + RVA_FILM_B) + 2;
	log_printf("menus: QA: the briefing film is made to look over");
}
// "coopLaunch" (the host's launch, whoever types it: the Start button, a saved game, QA): the loading screen is the campaign's before the engine launches
static void (*coop_launch_original)(void);
static void coop_launch_wrapper(void)
{
	char mapn[64] = "";
	game_dvar_raw("coop_mapName", mapn, sizeof(mapn));
	if (mapn[0] && strncmp(mapn, "so_", 3) != 0)
		campaign_loading_begin();
	if (coop_launch_original)
		coop_launch_original();
}

static void coop_launch_hook(void)
{
	for (cmd_function_t *c = *(cmd_function_t **)(base + RVA_CMD_FUNCTIONS); c; c = c->next) {
		if (c->name && !strcmp(c->name, "coopLaunch") && c->function && c->function != coop_launch_wrapper) {
			coop_launch_original = c->function;
			c->function = coop_launch_wrapper;
			log_printf("menus: coopLaunch is watched for the loading screen");
			return;
		}
	}
	log_printf("menus: no coopLaunch command found (the loading screen look comes from the other launch paths only)");
}

#ifndef _WIN64
unsigned char coop_wait_hook32(void);
unsigned char coop_wait_hook32(void)
{
	return coop_wait_hook();
}
__asm__(".text\n"
	".globl _coop_wait_stub\n"
	"_coop_wait_stub:\n"
	"\tpushl %ecx\n"
	"\tpushl %edx\n"
	"\tcall _coop_wait_hook32\n"
	"\tpopl %edx\n"
	"\tpopl %ecx\n"
	"\tret\n");
extern char coop_wait_stub[];
#endif

static void brief_install(void)
{
	if (!addr_resolved(REF_COOP_WAIT)) {
		// (a build whose addresses moved: the jump would land in the block that stands in for every function that was not found, and all of them would run this hook;
		// the iw4x-sp exe through the signature path crashed so, 2026-11-17)
		log_printf("menus: the co-op briefing wait function was not found in this build: no briefing wait");
		return;
	}
#ifdef _WIN64
	hook_jump(base + RVA_COOP_WAIT, 16, (void *)coop_wait_hook, 0xcc);
#else
	// the original touches only AL and the flags, and its callers (whole-program optimized) keep values in ECX and EDX across it: the stub saves them
	hook_jump(base + RVA_COOP_WAIT, 16, (void *)coop_wait_stub, 0xcc);
#endif
	log_printf("menus: the briefing of a network co-op waits for the film");
}

#ifndef _WIN64
// The 32-bit game (the official exe and iw4x-sp's 1.0.159) tells its host about every console command a network co-op client runs: Cmd_ExecuteString (VA 0x4da360)
// calls VA 0x4a2950, which queues "nt <command>" (or "n <index>" for one a script waits for with NotifyOnPlayerCommand) as a reliable client command, except for what
// this test (VA 0x44b6e0, its one caller VA 0x4a2999) leaves out: "profile_*", "updategamerprofile" and "UpdateDvarsFromProfile". The 64-bit exe sends only "n <index>".
// The mod's own work runs as console commands on the main thread (mw2coop_enh_tick 4 times a second, mw2coop_logo_tick, mw2coop_pause_routes_tick,
// mw2coop_report_menu, ...), so a client queued about 4 reliable commands a second for its host. While a briefing waits the host runs no server frame and acknowledges
// none of them: a 28 s wait passed the ring's 128 (EXE_ERR_CLIENT_CMD_OVERFLOW on the client), and after a 10 s wait the client's first messages carried about 50
// (1.8 KB each), more than the host's input buffer of one game frame (4096 bytes, VA 0x431300) holds when three land in one frame: "Packet is 3290 bytes, unable to add
// 1649 more" (VA 0x65b397). iw4x-sp sessions broke so right after the briefing, most often after a rejoin (QA rj10/rj11, 2026-11-24). The mod's commands are left out too.
#define REF_CMD_NOTIFY_TEST 0x4b6e0
static const uint8_t cmd_notify_test_bytes[] = {0x56, 0x8b, 0x74, 0x24, 0x08, 0x6a, 0x08, 0x68}; // push esi; mov esi, [esp+8]; push 8; push "profile_"
static unsigned char cmd_notify_test(const char *name)
{
	if (!name || !_strnicmp(name, "profile_", 8) || !_stricmp(name, "updategamerprofile") || !_stricmp(name, "UpdateDvarsFromProfile"))
		return 0;
	if (*name == '+' || *name == '-') // (a held key's pair: +mw2coop_talk, -mw2coop_ping)
		name++;
	return _strnicmp(name, "mw2coop_", 8) != 0;
}

static void cmd_notify_install(void)
{
	uint8_t *p = base + XR(REF_CMD_NOTIFY_TEST);
	if (!addr_resolved(REF_CMD_NOTIFY_TEST) || memcmp(p, cmd_notify_test_bytes, sizeof(cmd_notify_test_bytes))) {
		log_printf("menus: the test of which commands a co-op client tells its host about was not found: the mod's commands go to the host too");
		return;
	}
	// (the one caller pushes the name, tests AL and keeps nothing in ECX or EDX across the call: the original calls two string compares)
	hook_jump(p, 16, (void *)cmd_notify_test, 0xcc);
	log_printf("menus: a co-op client keeps the mod's own commands from its host");
}
#endif


void menus_poll(void)
{
	if (!base)
		return;
	char v[8] = "", coop[8] = "";
	game_dvar_raw("sv_running", v, sizeof(v));
	game_dvar_raw("coop", coop, sizeof(coop));
	rejoin_poll(*(volatile int *)(base + RVA_UI_STATE), *(volatile int *)(base + RVA_CLIENT_STATE), v[0] == '1'); // (a network session's break: rejoin.c)
	// menus_poll runs on the control thread during a level; tick_f deliberately does not.
	// Record the local mission here so tick_f can detect its end as soon as the menus return.
	if (v[0] == '1' && InterlockedCompareExchange(&native_started, 0, 0) &&
	    InterlockedExchange(&native_level_seen, 1) == 0)
		log_printf("menus: local split mission server observed; return detection armed");
	// hosting: the host's party is up (lobby), or a co-op level runs on the host
	{ // QA: the UI state of local client 0 (1 main menu, 2 paused, 3 pregame, 6 briefing = the loading screen, 7 victory) and the client state: when does the briefing end?
		static int last_ui = -99, last_cls = -99;
		int ui = *(volatile int *)(base + RVA_UI_STATE), cls = *(volatile int *)(base + RVA_CLIENT_STATE);
		if (ui == 3) { // QA: the film state in the pregame (the single player wait reads it)
			static int la = -1, lb = -1, lon = -1;
			int a = *(volatile int *)(base + RVA_FILM_A), b = *(volatile int *)(base + RVA_FILM_B), on = base[RVA_FILM_ON];
			if (a != la || b != lb || on != lon) {
				log_printf("menus: pregame film state: flag %d, counters %d %d", on, a, b);
				la = a, lb = b, lon = on;
			}
		}
		{ // The level's own sound (its ambient track and music, started when the level's scripts start) is silent while the loading screen and the briefing are up, as in single player (the
		  // engine fades all audio out for the load and in at the level's start, `snd_fade`, snd_levelFadeTime); in the engine's co-op nothing did, so the mission's music played under
		  // the briefing film (issue 20, owner 2026-11-10). The film's own sound has its own volume (snd_cinematicVolumeScale). Faded back in when the UI leaves the loading and pregame states.
		  // QA runs (mw2coop_qa set) leave the sound alone.
			static int faded;
			char q[32] = "";
			game_dvar_raw("mw2coop_qa", q, sizeof(q));
			int want = campaign_session && base[RVA_COOP_BYTE_E] && (ui == 6 || ui == 3) && !q[0];
			if (want && !faded) {
				faded = 1;
				game_cbuf_addtext("snd_fade 0 0\n");
				log_printf("menus: the level's sound is faded out for the loading screen and the briefing");
			} else if (!want && faded) {
				faded = 0;
				game_cbuf_addtext("snd_fade 1 1000\n");
				log_printf("menus: the level's sound is faded back in");
			}
		}
		if (last_ui == 3 && ui != 3 && campaign_session && base[RVA_COOP_BYTE_E]) { // the pregame is over on this PC: the film ended, or the player skipped it
			brief_notify_until = GetTickCount() + 2500;
			log_printf("menus: the briefing is over on this PC (the partner is told)");
		}
		if (brief_notify_until && GetTickCount() < brief_notify_until)
			net_send_mod("mw2coop_briefdone");
		int ui_changed = ui != last_ui;
		if (ui_changed || cls != last_cls) {
			log_printf("menus: ui state %d, client state %d", ui, cls);
			if (ui == 6) {
				InterlockedExchange(&brief_partner_done, 0);
				InterlockedExchange(&pause_menu_route_mask, 0);
				InterlockedExchange(&pause_direct_route_ready, 0);
			}
			last_ui = ui, last_cls = cls;
		}
		DWORD now = GetTickCount();
		// Card onFocus mirrors the menu-local selection into mw2coop_highlighted_game.
		// Register the mod-owned dvar before the first hover handler can try to update it.
		static int logo_dvar_initialized;
		static char last_highlighted[16] = "";
		char highlighted[16] = "";
		if (!game_dvar_raw("mw2coop_highlighted_game", highlighted, sizeof(highlighted))) {
			if (!logo_dvar_initialized) {
				logo_dvar_initialized = 1;
				game_cbuf_addtext("set mw2coop_highlighted_game 2\n");
				strcpy(highlighted, "2");
			}
		} else {
			logo_dvar_initialized = 1;
		}
		if (ui_changed || strcmp(last_highlighted, highlighted)) {
			strncpy(last_highlighted, highlighted, sizeof(last_highlighted) - 1);
			last_highlighted[sizeof(last_highlighted) - 1] = 0;
			queue_logo_tick(now);
		}
		// The engine lays out native menu items again during open/hover transitions. Reassert
		// the four-button row at a low rate while the front end is visible, after that layout pass.
		static DWORD last_selection_refresh;
		if (ui == 1 && now - last_selection_refresh >= 500) {
			last_selection_refresh = now;
			queue_logo_tick(now);
		}
		// Keep a fallback for builds whose pause menus expose no script to retarget. When the direct
		// menu route was found, do not reopen the front-end after the UI has already transitioned.
		static int pause_return_armed;
		static DWORD pause_return_armed_at;
		if (ui == 2) {
			if (!pause_return_armed)
				InterlockedExchange(&pause_direct_route_ready, 0); // wait for the currently visible pause menu's route
			pause_return_armed = 1;
			pause_return_armed_at = now;
		} else if (pause_return_armed && ui == 1) {
			pause_return_armed = 0;
			if (InterlockedExchange(&pause_direct_route_ready, 0)) {
				log_printf("menus: pause exit used the retargeted menu script; delayed fallback skipped");
			} else {
				game_cbuf_addtext("openmenu main_selection\nmw2coop_menus_tick\n");
				log_printf("menus: pause exit fallback forced openmenu main_selection; menu refresh queued");
			}
		} else if (pause_return_armed && ui == 0 && v[0] == '1') {
			// The player resumed the level instead of returning to the front-end.
			pause_return_armed = 0;
			InterlockedExchange(&pause_direct_route_ready, 0);
		} else if (pause_return_armed && now - pause_return_armed_at > 15000) {
			pause_return_armed = 0;
			InterlockedExchange(&pause_direct_route_ready, 0);
		}
	}
	{ // campaign co-op mission exit: the engine may choose main_specops because coop_lobby is shared with Special Ops
		char mapn[64] = "";
		game_dvar_raw("coop_mapName", mapn, sizeof(mapn));
		int campaign = base[RVA_PARTY_ACTIVE] && mapn[0] && strncmp(mapn, "so_", 3) != 0;
		int ui = *(volatile int *)(base + RVA_UI_STATE);
		if (campaign && ui == 0x0b)
			InterlockedExchange(&campaign_lobby_seen, 1);
		if (campaign && v[0] == '1')
			InterlockedExchange(&campaign_mission_seen, 1);
		if (campaign && campaign_mission_seen && v[0] != '1' && ui == 0x0e) {
			if (InterlockedCompareExchange(&campaign_lobby_return_queued, 1, 0) == 0)
				InterlockedExchange(&campaign_lobby_return_pending, 1);
		} else if (!campaign || ui != 0x0e || v[0] == '1') {
			InterlockedExchange(&campaign_lobby_return_queued, 0);
			InterlockedExchange(&campaign_lobby_return_pending, 0);
		}
		// Leaving the campaign lobby through leave_lobby_warning takes the engine's
		// shared Special Ops exit path. If no campaign level just ended, that path
		// must land on the single-player main menu instead of main_specops.
		if (campaign_lobby_seen && !campaign_mission_seen && v[0] != '1' && ui == 0x0e) {
			InterlockedExchange(&campaign_main_return_pending, 1);
			InterlockedExchange(&campaign_lobby_seen, 0);
		} else if (ui == 1) {
			InterlockedExchange(&campaign_lobby_seen, 0);
		}
		// The engine can still be opening main_specops when main_selection is asked for; while the UI state stays on it, ask once more.
		{
			LONG asked = InterlockedCompareExchange(&campaign_main_route_tick, 0, 0);
			if (asked && ui != 0x0e) {
				InterlockedExchange(&campaign_main_route_tick, 0);
				InterlockedExchange(&campaign_main_route_tries, 0);
			} else if (asked && v[0] != '1' && GetTickCount() - (DWORD)asked > 1500 && InterlockedCompareExchange(&campaign_main_route_tries, 0, 0) < 2) {
				InterlockedExchange(&campaign_main_return_pending, 1);
				InterlockedExchange(&campaign_main_route_tick, (LONG)GetTickCount());
			} else if (asked && GetTickCount() - (DWORD)asked > 6000) {
				InterlockedExchange(&campaign_main_route_tick, 0);
				InterlockedExchange(&campaign_main_route_tries, 0);
			}
		}
		if (!campaign || ui == 0x0b)
			InterlockedExchange(&campaign_mission_seen, 0);
	}
	upnp_want(base[RVA_PARTY_IS_HOST] && (base[RVA_PARTY_ACTIVE] || (v[0] == '1' && coop[0] == '1')));
	native_poll_players();
	{ // the loading screen's look: the campaign's while a campaign co-op or split screen game is being set up or played (Spec Ops' lobby and levels keep theirs)
		static int sticky;
		char mapn[64] = "";
		game_dvar_raw("coop_mapName", mapn, sizeof(mapn));
		int campaign = base[RVA_PARTY_ACTIVE] && mapn[0] && strncmp(mapn, "so_", 3) != 0;
		int split_so = so_name(split_return_map) && (native_split_active() || split_lobby_returning); // a split screen Special Ops op: its own loading screen, no campaign briefing
		int lobby_ui = *(volatile int *)(base + RVA_UI_STATE);
		if (split_so)
			campaign = 0;
		// (a client: its party is not the active one and its level server starts after the host's start packet (sv_running 0 for about a second): without the engine's co-op dvar
		// here the dvar went back to 1 15 ms after the launch had set it to 0, and the client played the whole level with specialops 1 while the host had 0. A script that reads it
		// (Cliffhanger's climb at the pull-up) took another path on each PC: the games went apart at the same spot after every checkpoint load (issue 27; found with the hash records
		// of netwatch.c: GetDvar("specialops") "0" on the host, "1" on the client).)
		if (!split_so && (campaign || native_split_active() || split_lobby_returning || (specialops_forced && (v[0] == '1' || coop[0] == '1'))))
			sticky = 1;
		else
			sticky = 0; // a Spec Ops lobby, or the player has left the campaign lobby
		if ((!(campaign || split_so) || lobby_ui != 0x0b) && split_lobby_returning &&
			   GetTickCount() - (DWORD)InterlockedCompareExchange(&split_lobby_return_tick, 0, 0) > 20000) {
			InterlockedExchange(&split_lobby_returning, 0); // don't keep the campaign dvar forced if listen failed
			InterlockedExchange(&split_lobby_return_tick, 0);
			InterlockedExchange(&split_lobby_return_attempts, 0);
			InterlockedExchange(&split_return_devices_valid, 0);
			InterlockedExchange(&split_mode, 0);
			InterlockedExchange(&split_pending, 0);
			devices_end();
			split_return_map[0] = 0;
			log_printf("menus: split campaign lobby return timed out; leaving the menu state as the game chose it");
		}
		InterlockedExchange(&campaign_session, sticky);
		if (!sticky)
			campaign_loading_end();
	}
	report_poll(v[0] == '1'); // (the pause menu's "Report a bug": the menu tick below does not run while a level does)
	if (v[0] == '1') {
		// Prepatch both known pause menus on the game's main thread during active gameplay.
		// A UI zone may register only one; when paused, only the currently visible menu's route
		// arms the direct return. Retry while paused if that menu was loaded lazily.
		int ui = *(volatile int *)(base + RVA_UI_STATE);
		DWORD now = GetTickCount();
		LONG mask = InterlockedCompareExchange(&pause_menu_route_mask, 0, 0);
		if (ui == 3 || ui == 6) {
			InterlockedExchange(&pause_menu_route_mask, 0);
			InterlockedExchange(&pause_direct_route_ready, 0);
			mask = 0;
		}
		int due = (ui == 2) || (ui == 0 && mask != 3 && now - pause_routes_last_queue_at >= 750);
		LONG queued = InterlockedCompareExchange(&pause_routes_tick_queued, 0, 0);
		if (queued && now - pause_routes_tick_queued_at > 1500)
			InterlockedExchange(&pause_routes_tick_queued, 0); // recover if a map transition cleared the queued command
		if (due && InterlockedCompareExchange(&pause_routes_tick_queued, 1, 0) == 0) {
			pause_routes_tick_queued_at = now;
			pause_routes_last_queue_at = now;
			game_cbuf_addtext("mw2coop_pause_routes_tick\n");
		}
		return;
	}
	InterlockedExchange(&pause_routes_tick_queued, 0);
	// the queue is cleared by a map load; a tick queued before it never runs
	if (tick_queued && GetTickCount() - tick_queued_at < 3000)
		return;
	tick_queued_at = GetTickCount();
	InterlockedExchange(&tick_queued, 1);
	game_cbuf_addtext("mw2coop_menus_tick\n");
}

// This narrow command is queued by menus_poll while a level runs. It executes on the game's main
// thread (not the control thread) and only patches the two pause-menu return scripts.
static void logo_tick_f(void)
{
	InterlockedExchange(&logo_tick_queued, 0);
	if (base) {
		patch_selection_menu(); // also refreshes a texture replaced by a vid_restart
		selection_logo_tick();
	}
}

static void pause_routes_tick_f(void)
{
	InterlockedExchange(&pause_routes_tick_queued, 0);
	if (base) {
		int visible = 0;
		int found = patch_pause_menu_main_selection_return(&visible);
		if (found) {
			LONG old_mask, new_mask;
			do {
				old_mask = InterlockedCompareExchange(&pause_menu_route_mask, 0, 0);
				new_mask = old_mask | found;
			} while (InterlockedCompareExchange(&pause_menu_route_mask, new_mask, old_mask) != old_mask);
			if (new_mask != old_mask)
				log_printf("menus: pause return scripts prepatched (route mask 0x%x)", (unsigned)new_mask);
			if (*(volatile int *)(base + RVA_UI_STATE) == 2 && (visible & new_mask) != 0 &&
			    !InterlockedExchange(&pause_direct_route_ready, 1))
				log_printf("menus: visible pause menu has a direct main_selection return; delayed fallback disabled (mask 0x%x)", (unsigned)(visible & new_mask));
		}
	}
}

// ---- going on with a network session that broke off (rejoin.c has the record and finds the break; owner 2026-11-23) ----

// the lobby's hint line (bottom right, where the game says "double-click a friend's name for voice chat"); a split screen lobby keeps it for its devices
static int rejoin_hint_on;
static void rejoin_hint(const char *text)
{
	if (split_mode)
		return;
	if (text) {
		localize_override("MENU_SP_STEAM_CHAT_HINT", text);
		rejoin_hint_on = 1;
	} else if (rejoin_hint_on) {
		localize_override("MENU_SP_STEAM_CHAT_HINT", NULL);
		rejoin_hint_on = 0;
	}
}

static void rejoin_hint_fmt(const char *name, const char *want, const char *a, int n)
{
	const char *fmt = localize_text(name);
	char text[256];
	if (!fmt || !ctexts_fmt_ok(fmt, want))
		return;
	if (!strcmp(want, "s"))
		snprintf(text, sizeof(text), fmt, a);
	else if (!strcmp(want, "sd"))
		snprintf(text, sizeof(text), fmt, a, n);
	else
		lstrcpynA(text, fmt, sizeof(text));
	rejoin_hint(text);
}

// the partner of the session that broke off: by his player id (the mod's, the same after a restart; QA 2026-11-23: the iw4x-sp host's XUID was another after its
// restart, and its partner never took the lobby for his), else by his XUID
static int rejoin_same_partner(const rejoin_info_t *r)
{
	if (r->pid[0] && partner_id[0])
		return strcmp(partner_id, r->pid) == 0;
	return !r->xuid || partner_xuid == r->xuid;
}

// the partner's address to join or to send to: a Steam relay peer's virtual address is made again from his Steam ID (it is numbered per run of the game)
static int rejoin_address(const rejoin_info_t *r, uint8_t ip[4], uint16_t *port)
{
	memcpy(ip, r->ip, 4);
	*port = r->port;
	if (ip[0] == 240 && ip[1] == 10 && !steamnet_address_of(r->xuid, ip))
		return 0;
	return ip[0] != 0 && *port != 0;
}

// The host: its lobby continues the playthrough of the session that broke off (its save, its mission and checkpoint, as Continue does), lets the partner in
// whatever the access, and is ready; the partner's game joins again by itself.
static void rejoin_arrange_host(const rejoin_info_t *r)
{
	char map[64], start[64], spot[64] = "", cmd[320];
	if (r->save[0] && progress_choose_file(r->save))
		save_picked = 1; // (the lobby stays on this save when the partner is known, not on his newest)
	if (progress_chosen(map, sizeof(map), start, sizeof(start))) {
		continuing = 1;
		add_member = 0;
		progress_chosen_spot(spot, sizeof(spot));
		snprintf(cmd, sizeof(cmd), "set coop_mapName %s\nset start \"%s\"\nset mw2coop_cp_start \"%s\"\nset mw2coop_cp_pos \"%s\"\nset mw2coop_snapload 0\n", map, start,
			 spot[0] ? start : "", spot);
		game_cbuf_addtext(cmd);
		log_printf("menus: rejoin: the lobby continues %s ('%s' start '%s')", r->save, map, start);
	} else
		log_printf("menus: rejoin: no save of the playthrough to continue: the lobby keeps its mission");
	access_rejoin_partner(r->xuid);
	InterlockedExchange(&my_ready, 1);
	send_ready();
	update_texts();
}

static void rejoin_lobby_tick(int campaign, int ui)
{
	static int arranged, counting, joined, attempts, reopens;
	static DWORD count_from, hello_next, next_try, join_since, wait_since, reopen_at;
	rejoin_info_t r;
	// a session of an earlier run of the game that broke off: asked once, at the main menu
	if (ui == 1 && rejoin_offer_take(&r)) {
		const char *fmt = localize_text(r.host ? "MW2COOP_REJOIN_HOST_FMT" : "MW2COOP_REJOIN_CLIENT_FMT");
		char text[256];
		if (fmt && ctexts_fmt_ok(fmt, "s")) {
			snprintf(text, sizeof(text), fmt, r.name[0] ? r.name : "?");
			localize_set_text("MW2COOP_REJOIN_Q", text);
			game_cbuf_addtext("openmenu mw2coop_rejoin\n");
			log_printf("menus: rejoin: asking whether to go on with the %s session that broke off (partner '%s')", r.host ? "hosted" : "joined", r.name);
		}
	}
	const int st = rejoin_state();
	if (st != REJOIN_HOST_WAIT)
		arranged = 0, counting = 0, reopens = 0, wait_since = 0;
	if (st != REJOIN_CLIENT_JOIN)
		joined = 0, attempts = 0, next_try = 0, join_since = 0;
	if (st != REJOIN_HOST_WAIT && st != REJOIN_CLIENT_JOIN) {
		rejoin_hint(NULL);
		return;
	}
	rejoin_info(&r);
	const DWORD now = GetTickCount();
	const char *name = r.name[0] ? r.name : "?";
	if (st == REJOIN_HOST_WAIT) {
		const int in_lobby = campaign && ui == 0x0b && base[RVA_PARTY_ACTIVE] && base[RVA_PARTY_IS_HOST];
		if (!wait_since)
			wait_since = now;
		if (!in_lobby) {
			if (arranged && ui != 0x0b && ui != 0) { // the host left the lobby (Back): no more waiting
				rejoin_end("the host left the lobby");
				rejoin_hint(NULL);
			} else if (!arranged && (ui == 1 || ui == 14) && now - wait_since > 2000 && (int)(now - reopen_at) >= 0 && reopens < 3) {
				// the level ended outside the host's lobby: the iw4x-sp client's host stops with an engine error ("Packet is ... bytes, unable to add ... more")
				// when the partner goes silent and lands in Special Ops' menu (QA 2026-11-23): the lobby is opened again on the playthrough
				reopens++;
				reopen_at = now + 6000;
				if (r.save[0] && progress_choose_file(r.save))
					save_picked = 1;
				log_printf("menus: rejoin: the host is out of its lobby (ui state %d): the lobby is opened again (%d)", ui, reopens);
				host_lobby(NULL);
			}
			return;
		}
		if (!arranged) {
			arranged = 1;
			rejoin_arrange_host(&r);
		}
		const int back = party_full() && partner_present();
		const int same = back && rejoin_same_partner(&r); // (by the player id: the iw4x-sp client's XUID is not the same after a restart)
		char key[48], tag[64], want[120] = "";
		const int snap = continuing && progress_chosen_snapshot(key, sizeof(key), tag, sizeof(tag));
		if (snap)
			snprintf(want, sizeof(want), "%s:%s", key, tag);
		const int match = !snap || strcmp(partner_snaptag, want) == 0;
		if (same && match && my_ready && partner_ready) {
			if (!counting) {
				counting = 1;
				count_from = now;
				log_printf("menus: rejoin: the partner is back and ready (checkpoint %s): the lobby starts in 5 s", snap ? want : "none, start point");
			}
			const int left = 5 - (int)((now - count_from) / 1000);
			if (left <= 0) {
				counting = 0;
				InterlockedExchange(&partner_confirmed, 1); // (the playthrough's own partner: no questions about members)
				rejoin_hint(NULL);
				log_printf("menus: rejoin: going on");
				start_launch();
				return;
			}
			rejoin_hint_fmt("MW2COOP_REJOIN_GO_FMT", "sd", name, left);
			return;
		}
		if (counting)
			log_printf("menus: rejoin: the count stopped (partner %s, ready %ld / %ld)", back ? "here" : "gone", my_ready, partner_ready);
		counting = 0;
		if (back && !same)
			rejoin_hint_fmt("MW2COOP_REJOIN_OTHER", "", NULL, 0);
		else if (same && !match)
			rejoin_hint_fmt("MW2COOP_REJOIN_NOSNAP_FMT", "s", name, 0);
		else
			rejoin_hint_fmt("MW2COOP_REJOIN_WAIT_FMT", "s", name, 0);
		// a router in front of the host lets the partner's join in once the host has sent to him: a packet every 2 s while he is away
		uint8_t ip[4];
		uint16_t port;
		if (!back && (int)(now - hello_next) >= 0 && rejoin_address(&r, ip, &port)) {
			hello_next = now + 2000;
			net_send_mod_to(ip, port, "mw2coop_rejoin_hello");
		}
		return;
	}
	// the partner: joins the host's address again until the host's party holds him (its ready packets say "full"), then is ready
	const int in = base[RVA_PARTY_ACTIVE] && !base[RVA_PARTY_IS_HOST] && partner_present() && host_full && rejoin_same_partner(&r);
	if (in) {
		if (!joined) {
			joined = 1;
			log_printf("menus: rejoin: back in the host's lobby after %d attempt(s): ready", attempts);
			InterlockedExchange(&my_ready, 1);
			send_ready();
			update_texts();
		}
		rejoin_hint_fmt("MW2COOP_REJOIN_BACK_FMT", "s", name, 0);
		return;
	}
	if (joined && !base[RVA_PARTY_ACTIVE]) { // he was back and left the lobby himself
		rejoin_end("the partner left the host's lobby");
		rejoin_hint(NULL);
		return;
	}
	if (ui != 0x0b && ui != 1 && ui != 14)
		return; // (a popup of the game's, a load: later)
	if (!join_since)
		join_since = now;
	if (now - join_since > 10 * 60 * 1000) {
		rejoin_end("the host's game did not take the partner back in 10 minutes");
		rejoin_hint(NULL);
		return;
	}
	uint8_t ip[4];
	uint16_t port;
	if (!rejoin_address(&r, ip, &port)) {
		rejoin_end("the host's address is not known");
		return;
	}
	// the partner's own small packet to the host every 2 s: its router keeps the way open for the host's, and the host's game sees it is looked for
	static DWORD chello_next;
	if ((int)(now - chello_next) >= 0) {
		chello_next = now + 2000;
		net_send_mod_to(ip, port, "mw2coop_rejoin_hello");
	}
	// A join is tried only when the host is heard (its waiting lobby's packets, or its lobby's ready packets): a join sent to a host that is gone waits in the
	// game's queue of reliable packets, and on the 32-bit game (iw4x-sp) a join queued so kept every later one from getting through after the host had started
	// again (QA 2026-11-23: the partner never got back; one join sent while the host was down was enough).
	const int host_heard = rejoin_hello_age() < 4000 || partner_present();
	if ((attempts && (int)(now - next_try) < 0) || !host_heard) {
		if (attempts)
			rejoin_hint_fmt("MW2COOP_REJOIN_JOIN_FMT", "sd", name, attempts);
		else
			rejoin_hint_fmt("MW2COOP_REJOIN_WAITHOST_FMT", "s", name, 0);
		return;
	}
	next_try = now + 8000; // (a join takes a few seconds to show in the host's packets: the next one only after that)
	attempts++;
	char cmd[64];
	snprintf(cmd, sizeof(cmd), "mw2coop_join %u.%u.%u.%u %u\n", ip[0], ip[1], ip[2], ip[3], (unsigned)port);
	game_cbuf_addtext(cmd);
	log_printf("menus: rejoin: joining the host at %u.%u.%u.%u:%u again (attempt %d, the host is heard)", ip[0], ip[1], ip[2], ip[3], (unsigned)port, attempts);
	rejoin_hint_fmt("MW2COOP_REJOIN_JOIN_FMT", "sd", name, attempts);
}

// the main menu's question after a restart: Yes (the host opens its lobby on the playthrough again; the partner joins the host again), No
static void rejoin_yes_f(void)
{
	rejoin_info_t r;
	rejoin_info(&r);
	if (r.host) {
		rejoin_set_state(REJOIN_HOST_WAIT, "the host opens the lobby again");
		if (r.save[0] && progress_choose_file(r.save))
			save_picked = 1;
		host_lobby(NULL); // (mw2coop_online: the lobby continues the picked save)
	} else
		rejoin_set_state(REJOIN_CLIENT_JOIN, "the partner joins the host again");
}

static void rejoin_no_f(void)
{
	rejoin_end("the player said no");
}

// the client: a network session launches (the host's start packet, progress.c)
void menus_rejoin_client_begins(void)
{
	rejoin_session_begin(0, (const char *)base + RVA_PARTY_MEMBER_NAMES, partner_xuid, partner_id); // (slot 0: the host)
}

static void tick_f(void)
{
	InterlockedExchange(&tick_queued, 0);
	if (!base)
		return;
	if (InterlockedExchange(&split_texts_wanted, 0))
		update_texts();
	addr_scratch_check("between two menu ticks"); // (a build whose addresses moved: the block for code that was not found must stay `ret`)
	native_session_tick();
	patch_main_menu(); // clone the native Campaign page without modifying the native main_text menu
	patch_selection_menu();
	selection_logo_tick();
	patch_specops_return();
	patch_pause_menu_main_selection_return(NULL);
	patch_act1_menu();
	patch_specops_menu();
	add_lobbies_menu();
	add_skill_menus();
	add_host_menu();
	add_saves_menu();
	add_partner_popups();
	padopts_tick();
	fonts_retry_tick(); // (the fonts' late retry: here, at the top of the main thread, not inside an asset lookup)
	gfxopts_tick();
	enhopts_tick();
	ping_menu_tick(); // (binds [input] ping_key to the co-op markers once)
	picking_tick();
	unlock_missions_tick();
	find_tick();
	addr_scratch_check("in the menu tick's patches");
	int campaign = campaign_lobby();
	int spec_split = spec_split_lobby();
	int split_lobby = campaign || spec_split; // the lobby of a split screen game of ours: the return to it is looked after for a Special Ops op too
	int lobby_ui = *(volatile int *)(base + RVA_UI_STATE);
	rejoin_lobby_tick(campaign, lobby_ui); // a network session that broke off: the host waits, the partner joins again
	report_menu_tick();		       // the pause menu's "Report a bug" (report.c)
	weaponfov_tick();		       // cg_weaponFov once the game's dvars are there (weaponfov.c)
	voice_menu_tick();		       // the push-to-talk key bound once (voice.c)
	chat_menu_tick();		       // the chat key bound once (chat.c)
	patch_lobby(campaign, spec_split);
	patch_role_popup(InterlockedCompareExchange(&split_mode, 0, 0) != 0); // (the game's role popup starts our session in a split screen lobby)
	leave_warning_patch(campaign);
	if (split_lobby && lobby_ui == 0x0b && InterlockedCompareExchange(&split_lobby_returning, 0, 0)) {
		// The lobby is now live: restore both device cards once, against the final menu.
		if (InterlockedExchange(&split_return_devices_valid, 0))
			devices_begin_assigned(split_return_devices[0], split_return_devices[1]);
		InterlockedExchange(&split_lobby_returning, 0);
		InterlockedExchange(&split_lobby_return_tick, 0);
		InterlockedExchange(&split_lobby_return_attempts, 0);
		log_printf("menus: split campaign lobby restored with the original device assignments");
	} else if ((!split_lobby || lobby_ui != 0x0b) && InterlockedCompareExchange(&split_lobby_returning, 0, 0) &&
		   GetTickCount() - (DWORD)InterlockedCompareExchange(&split_lobby_return_tick, 0, 0) > 1200 &&
		   InterlockedCompareExchange(&split_lobby_return_attempts, 0, 0) < 3) {
		LONG attempt = InterlockedIncrement(&split_lobby_return_attempts);
		InterlockedExchange(&split_pending, 1);
		host_lobby(split_return_map);
		InterlockedExchange(&split_lobby_return_tick, (LONG)GetTickCount());
		log_printf("menus: split campaign lobby listen retry %ld/3", attempt);
	}
	{
		// Leaving a split screen lobby (Back, Esc) ended nothing of it: a network lobby joined later as the client had no Ready, took the split lobby's own
		// difficulties and rows, and the device thread kept sending pad keys (audit 2026-11-18, menus F1). The party gone for 3 s with no split screen session
		// starting or returning: the split lobby is over (split_skill_texts then empties its difficulties).
		static DWORD party_gone_since;
		if (InterlockedCompareExchange(&split_mode, 0, 0) && !base[RVA_PARTY_ACTIVE] && !native_split_active() &&
		    !InterlockedCompareExchange(&split_lobby_returning, 0, 0) && !InterlockedCompareExchange(&split_pending, 0, 0)) {
			if (!party_gone_since)
				party_gone_since = GetTickCount() | 1;
			else if (GetTickCount() - party_gone_since > 3000) {
				party_gone_since = 0;
				InterlockedExchange(&split_mode, 0);
				devices_end();
				update_texts();
				log_printf("menus: the split screen lobby was left: its devices and difficulties are dropped");
			}
		} else
			party_gone_since = 0;
	}
	if (InterlockedExchange(&campaign_main_return_pending, 0)) {
		game_cbuf_addtext("openmenu main_selection\n");
		LONG tries = InterlockedIncrement(&campaign_main_route_tries);
		InterlockedExchange(&campaign_main_route_tick, (LONG)GetTickCount() | 1);
		log_printf("menus: campaign lobby exit routed to main_selection (ui state %d, try %ld)", *(volatile int *)(base + RVA_UI_STATE), (long)tries);
	}
	if (InterlockedExchange(&campaign_lobby_return_pending, 0)) {
		if (campaign && *(volatile int *)(base + RVA_UI_STATE) == 0x0e) {
			lobby_music_patch(1);
			game_cbuf_addtext("openmenu coop_lobby\n");
			log_printf("menus: campaign party returned to main_specops; reopening coop_lobby");
		} else
			InterlockedExchange(&campaign_lobby_return_queued, 0);
	}
	access_tick(campaign, base[RVA_PARTY_IS_HOST], spec_split);
	firewall_tick(campaign && base[RVA_PARTY_IS_HOST] && !split_mode); // the second player is on this PC: nothing to let in
	update_tick(lobby_ui == 1 && !campaign && !spec_split && !split_mode && !base[RVA_PARTY_ACTIVE]); // the updater's popups: at the main menu only
	if (!campaign) {
		InterlockedExchange(&my_ready, 0);
		InterlockedExchange(&partner_ready, 0);
		partner_heard = 0;
		partner_id[0] = 0;
		partner_xuid = 0;
		if (spec_split)
			update_texts(); // the Start button's text and the device cards of a split screen Special Ops lobby
		return;
	}
	if (GetTickCount() - ready_sent >= 1000)
		send_ready();
	// the host continuing ("Continue"): once the partner is known, the lobby moves to his newest save
	if (InterlockedExchange(&partner_new, 0) && base[RVA_PARTY_IS_HOST] && continuing && !save_picked) {
		char map[64], start[64];
		if (progress_find_with(partner_id, partner_xuid, map, sizeof(map), start, sizeof(start))) {
			char cmd[320], spot[64] = "";
			progress_chosen_spot(spot, sizeof(spot));
			snprintf(cmd, sizeof(cmd), "set coop_mapName %s\nset start \"%s\"\nset mw2coop_cp_start \"%s\"\nset mw2coop_cp_pos \"%s\"\n",
				 map, start, spot[0] ? start : "", spot);
			game_cbuf_addtext(cmd);
			log_printf("menus: continuing this partner's save: '%s' start '%s'", map, start);
		} else
			log_printf("menus: no save with this partner yet");
	}
	update_texts();
	// a split screen lobby: until the second player has pressed a button on his device, the lobby's own hint line (bottom right, where the game says
	// "double-click a friend's name for voice chat") tells him what to do
	static int hint_shown;
	if (split_mode) { // each player presses a button on his device: the line says who plays on what, and who is still to press
		char line[256] = "", dev[64];
		for (int pl = 0; pl < 2; pl++) {
			int d = devices_get(pl);
			char who[48]; // the player's call sign, as on his card
			const char *sign = localize_text(pl ? "MW2COOP_CALLSIGN_B" : "MW2COOP_CALLSIGN_A");
			snprintf(who, sizeof(who), "%s: ", sign ? sign : pl ? "Bravo" : "Alpha");
			const char *what;
			if (d == DEVICE_NONE)
				what = localize_text("MW2COOP_SPLIT_PRESS");
			else if (d == DEVICE_KEYBOARD)
				what = localize_text("MW2COOP_SPLIT_KEYBOARD");
			else {
				const char *f = localize_text("MW2COOP_SPLIT_PAD");
				snprintf(dev, sizeof(dev), f && ctexts_fmt_ok(f, "d") ? f : "pad %d", d + 1);
				what = dev;
			}
			const char *cancel = d == DEVICE_NONE ? "" : localize_text(d == DEVICE_KEYBOARD ? "MW2COOP_SPLIT_CANCEL_KB" : "MW2COOP_SPLIT_CANCEL_PAD");
			size_t n = strlen(line);
			snprintf(line + n, sizeof(line) - n, "%s%s%s%s", pl ? "\n" : "", who, what ? what : "", cancel ? cancel : ""); // a line for each player
			if (d == DEVICE_NONE)
				break; // player 2 is asked once player 1 has his device
		}
		char icontest[8] = "";
		if (game_dvar_raw("mw2coop_icontest", icontest, sizeof(icontest)) && icontest[0] == '1') { // QA: does the text draw take an inline material ("^\x01" w h length name)?
			size_t n = strlen(line);
			snprintf(line + n, sizeof(line) - n, " [^\x01\x20\x20\x11pc_keyboard_mouse]");
			static int logged;
			if (!logged++)
				log_printf("menus: icon test hint (%zu chars): %s", strlen(line), line);
		}
		localize_override("MENU_SP_STEAM_CHAT_HINT", line);
		hint_shown = 1;
	} else if (hint_shown) {
		localize_override("MENU_SP_STEAM_CHAT_HINT", NULL);
		hint_shown = 0;
	}
}

void menus_install(void)
{
	InitializeCriticalSection(&lobby_lock);
	base = (uint8_t *)GetModuleHandleA(NULL);
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
		game_add_command(&commands[i]);
#ifdef _WIN64
	if (memcmp(base + XR(0x2442b0), player_info_bytes, sizeof(player_info_bytes)) ||
	    !hook_function(base + XR(0x2442b0), player_info_bytes, sizeof(player_info_bytes), player_info_hook, (void **)&player_info_original))
		log_printf("menus: coopPlayerInfo not hooked (bytes differ)");
#else
	if (memcmp(base + XR(0x21c430), player_info_bytes, sizeof(player_info_bytes)) ||
	    !hook_function(base + XR(0x21c430), player_info_bytes, sizeof(player_info_bytes), n32_player_info_stub, &player_info_target))
		log_printf("menus: coopPlayerInfo not hooked (bytes differ)");
#endif
	CreateThread(NULL, 0, lobby_pad_thread, NULL, 0, NULL);
	CreateThread(NULL, 0, loading_thread, NULL, 0, NULL);
	coop_launch_hook();
	brief_install();
#ifndef _WIN64
	cmd_notify_install();
#endif
	// devmap is a server command: the console node's function is empty and the command goes on
	// to this node (Cmd_AddServerCommand in SV_AddOperatorCommands, rva 0x22bef0)
	cmd_function_t *sv_devmap = (cmd_function_t *)(base + RVA_SV_CMD_DEVMAP);
	if (sv_devmap->name && strcmp(sv_devmap->name, "devmap") == 0 && sv_devmap->function) {
		devmap_native = sv_devmap->function;
		sv_devmap->function = devmap_f;
	}
	fake_iwnet_off_install();
	signin_install();
	log_printf("menus: installed (devmap %s)", devmap_native ? "hooked" : "NOT FOUND");
	if (!devmap_native)
		base = NULL;
}
