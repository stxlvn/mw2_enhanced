// Co-op in the game's own menus (no .bat files). This build has no menu text parser, so the
// zone's menus (ASSET_TYPE_MENU 26, as the UI has loaded them) are changed in memory:
//
// - Campaign main menu (main_text): a "TWO PLAYERS ONLINE" button under Mission Select, cloned
//   from Mission Select. It opens the game's own Spec Ops co-op lobby (coop_lobby: Steam friend
//   invites, ready state, Start) with a campaign mission: where the players stopped last
//   (mw2coop\progress.txt), else The Pit (trainer).
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

#include "game.h"
#include "access.h"
#include "firewall.h"
#include "hook.h"
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
		if (n && lstrcmpiA(n, name) == 0)
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
		if (nm && lstrcmpiA(nm, name) == 0)
			out[n++] = menus[i];
	}
	return n;
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
	return;
#endif
	for (int i = 0; set && i < set->count && i < 16; i++) {
		MenuEventHandler *h = set->handlers[i];
		if (h && h->type == 0 && h->data)
			log_printf("menus:   script: %.400s", (const char *)h->data);
		else if (h)
			log_printf("menus:   handler type %d", h->type);
	}
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
		log_printf("menus: instance %d of %s at %p: %d items, window flags %x", m, name, (void *)menus[m], ic, *(uint32_t *)(menus[m] + 0x50));
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
		log_printf("menus: %s[%d] %s '%s' x %g y %g w %g h %g align %d/%d", menu_name, i, name ? name : "-",
			   text ? text : "", r[0], r[1], r[2], r[3], it[ITEM_RECT_ALIGN_H], it[ITEM_RECT_ALIGN_V]);
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
			int fcount = *(int *)(it + ITEM_FLOAT_EXPS - 8);
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

static uint8_t **main_items; // our items array for main_text, NULL until patched

// Buttons added under Mission Select, in the order of the itemDefs of menuDef "main_text" in menus.menu (src/menutable.c)
static int main_button_count(void)
{
	int n = 0;
	while (menutable_item("main_text", n))
		n++;
	return n;
}

// The look of the main menu: the game's logo, with the mod's name in it (logo.c), sits at the top of the left panel
// with its right edge on the right edge of the list; the dark panel behind the list and the line in it end in a hard edge on the right, so they are drawn mirrored (a negative
// width draws the picture mirrored over the same place) with the fade-in material of the line, which fades out there. The other pages are the game's own.
// The title line ("For the record") is emptied: the name is in the logo.
static void *fade_material;  // the Material * of "gradient_fadein"

static const char *item_material_name(const uint8_t *it)
{
	const char **mat = *(const char ***)(it + ITEM_BACKGROUND_PTR);
	return mat && !IsBadReadPtr(mat, sizeof(*mat)) && *mat && !IsBadStringPtrA(*mat, 16) ? *mat : "";
}

static const char split_specops_action[] = "\"play\" \"mouse_click\" ; \"exec\" \"mw2coop_split_specops\" ; ";

static void place_logo(uint8_t *it)
{
	float *rect = (float *)(it + ITEM_RECT), *client = (float *)(it + ITEM_RECT_CLIENT_Y - 4); // rect and rectClient: x, y, width, height
	rect[0] = client[0] = -44, rect[1] = client[1] = 4, rect[2] = client[2] = 216, rect[3] = client[3] = 54; // its right edge on the right edge of the buttons' text
	it[ITEM_RECT_ALIGN_H] = 1, it[ITEM_RECT_ALIGN_V] = 1;
}

static void style_items(uint8_t **items, int count)
{
	for (int i = 0; i < count; i++) // the fade material first: the line of the main menu has it
		if (!fade_material && !strcmp(item_material_name(items[i]), "gradient_fadein"))
			fade_material = *(void **)(items[i] + ITEM_BACKGROUND_PTR);
	for (int i = 0; i < count; i++) {
		uint8_t *it = items[i];
		const char *text = *(const char **)(it + ITEM_TEXT), *mat_name = item_material_name(it);
		float *rect = (float *)(it + ITEM_RECT), *client = (float *)(it + ITEM_RECT_CLIENT_Y - 4);
		if (!strcmp(mat_name, "logo_cod2")) {
			logo_install(*(void **)(it + ITEM_BACKGROUND_PTR), getenv("MW2COOP_LOGODUMP") != NULL);
			place_logo(it);
		} else if (!strcmp(mat_name, "gradient_fadein_fadebottom") || !strcmp(mat_name, "gradient_fadein")) {
			if (fade_material && strcmp(mat_name, "gradient_fadein"))
				*(void **)(it + ITEM_BACKGROUND_PTR) = fade_material;
			if (rect[2] > 0)
				rect[2] = client[2] = -rect[2];
		} else if (text && !strcmp(text, "@MENU_SP_FOR_THE_RECORD")) {
			*(const char **)(it + ITEM_TEXT) = ""; // the mod's name is in the logo
			*(void **)(it + ITEM_TEXT_EXP) = NULL;
			rect[1] = client[1] = -6;
		}
	}
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
static void append_on_open(uint8_t *menu, const char *script);
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
		if (!strcmp(mat, "logo_cod2")) {
			logo_item = pi[i];
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
	*(MenuEventHandlerSet **)(back + ITEM_ACTION) = ui_script_set("\"play\" \"mouse_click\" ; \"close\" \"self\" ; ");
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

// The game's own Multiplayer entry: the action of the "game_mp" item of its selection menu (main_selection). The mod's Multiplayer button runs that, so it does what the game's
// button does; the mod's own modes (co-op, split screen) are on the Campaign page only. NULL if the item is not there.
static void *original_multiplayer_action(void)
{
	uint8_t *selection = ui_find_menu("main_selection");
	if (!selection)
		return NULL;
	uint8_t **si = *(uint8_t ***)(selection + MENU_ITEMS);
	for (int i = 0; i < *(int *)(selection + MENU_ITEM_COUNT); i++) {
		const char *name = *(const char **)si[i];
		if (name && !strcmp(name, "game_mp"))
			return *(void **)(si[i] + ITEM_ACTION);
	}
	return NULL;
}

static int name_in(const char *name, const char *const *list)
{
	for (; *list; list++)
		if (name && !strcmp(name, *list))
			return 1;
	return 0;
}

// The main menu as six entries: CAMPAIGN, SPEC OPS, MULTIPLAYER (menus.menu, menuDef "main_text"), then Settings, Credits and Quit. The game's Resume, New game and Mission select
// move to a page of their own, "mw2coop_campaign" (a copy of the main menu with Stats but without Options, Credits, the Main menu button and Quit, with a Back button);
// the game's Main menu button (the Campaign / Spec Ops chooser) goes, as SPEC OPS opens the Spec Ops menu itself.
static void patch_main_menu(void)
{
	uint8_t *menu = ui_find_menu("main_text");
	if (!menu)
		return;
	uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
	if (items == main_items)
		return;
	int count = *(int *)(menu + MENU_ITEM_COUNT);
	int at = ui_item_index(menu, "levelselect");
	const int nbuttons = main_button_count();
	if (at < 0 || !nbuttons || ui_item_index(menu, menutable_item("main_text", 0)->name) >= 0) {
		log_printf("menus: main_text has no levelselect button, no co-op buttons added");
		main_items = items;
		return;
	}
	static const char *const top_gone[] = {"resume", "newgame", "newgame_warning", "levelselect", "game_selection", NULL};
	static const char *const campaign_gone[] = {"options", "credits", "game_selection", "button_quit", NULL};
	static const char *const top_gone_more[] = {"stats", NULL}; // Stats moves to the campaign page
	uint8_t *src = items[at]; // Mission Select: the template of the new buttons

	// the campaign page
	if (!ui_find_menu("mw2coop_campaign")) {
		uint8_t *camp = ui_clone_menu(menu, "mw2coop_campaign");
		uint8_t **ci = *(uint8_t ***)(camp + MENU_ITEMS);
		uint8_t **cl = HeapAlloc(GetProcessHeap(), 0, (count + 1) * sizeof(*cl));
		int n = 0;
		uint8_t *level = NULL;
		int extra = 0; // the mod's buttons of the page (menus.menu, menuDef "mw2coop_campaign"): Co-op and Split screen, under Mission Select
		while (menutable_item("mw2coop_campaign", extra))
			extra++;
		cl = HeapAlloc(GetProcessHeap(), 0, (count + extra + 1) * sizeof(*cl));
		float level_y = 0;
		for (int i = 0; i < count; i++) {
			const char *name = *(const char **)ci[i];
			if (name_in(name, campaign_gone) || !strcmp(item_material_name(ci[i]), "logo_cod2")) // (the logo is the main menu's only)
				continue;
			if (level && *(float *)(ci[i] + ITEM_RECT_Y) >= level_y + BUTTON_HEIGHT - 0.5f && *(float *)(ci[i] + ITEM_RECT_Y) < 1000) { // below Mission Select: moves down
				*(float *)(ci[i] + ITEM_RECT_Y) += extra * BUTTON_HEIGHT;
				*(float *)(ci[i] + ITEM_RECT_CLIENT_Y) += extra * BUTTON_HEIGHT;
			}
			cl[n++] = ci[i];
			if (name && !strcmp(name, "levelselect")) {
				level = ci[i];
				level_y = *(float *)(level + ITEM_RECT_Y);
				for (int b = 0; b < extra; b++) {
					const MenuItem *def = menutable_item("mw2coop_campaign", b);
					uint8_t *item = ui_clone_item(level, camp);
					float y = level_y + (b + 1) * BUTTON_HEIGHT;
					*(const char **)item = def->name;
					*(const char **)(item + ITEM_TEXT) = def->text;
					*(MenuEventHandlerSet **)(item + ITEM_ACTION) = ui_script_set(def->action);
					*(float *)(item + ITEM_RECT_Y) = y;
					*(float *)(item + ITEM_RECT_CLIENT_Y) = y;
					cl[n++] = item;
				}
			}
		}
		if (level) {
			uint8_t *back = ui_clone_item(level, camp);
			float y = *(float *)(level + ITEM_RECT_Y) + BUTTON_HEIGHT;
			for (int k = 0; k < n; k++) { // Back goes under the last button (Stats)
				const char *kn = *(const char **)cl[k];
				if (kn && !strcmp(kn, "stats"))
					y = *(float *)(cl[k] + ITEM_RECT_Y) + BUTTON_HEIGHT;
			}
			*(const char **)back = "mw2coop_campaign_back";
			*(const char **)(back + ITEM_TEXT) = "@PLATFORM_BACK_CAPS";
			*(MenuEventHandlerSet **)(back + ITEM_ACTION) = ui_script_set("\"play\" \"mouse_click\" ; \"close\" \"self\" ; ");
			*(float *)(back + ITEM_RECT_Y) = y;
			*(float *)(back + ITEM_RECT_CLIENT_Y) = y;
			cl[n++] = back;
		}
		*(uint8_t ***)(camp + MENU_ITEMS) = cl;
		*(int *)(camp + MENU_ITEM_COUNT) = n;
		set_page_esc(camp);
		if (ui_register_menu(camp))
			log_printf("menus: campaign page added (%d items)", n);
	}

	add_about_page(menu, count, src);

	// the look (style_items): the logo at the top of the left panel, the panel and its line fading out to the right; the buttons go down to make room
	const float header_dy = 10.0f;
	style_items(items, count);
	// the top level: the game's four buttons are replaced by the table's
	uint8_t **list = HeapAlloc(GetProcessHeap(), 0, (count + nbuttons + 1) * sizeof(*list));
	int n = 0, inserted = 0;
	const MenuItem *about = menutable_item("main_text_about", 0); // the About button goes under the last Credits item
	int last_credits = -1;
	for (int i = 0; i < count; i++)
		if (!strcmp(*(const char **)items[i] ? *(const char **)items[i] : "", "credits"))
			last_credits = i;
	for (int i = 0; i < count; i++) {
		const char *name = *(const char **)items[i];
		if (name_in(name, top_gone_more))
			continue;
		if (name_in(name, top_gone)) {
			if (inserted)
				continue;
			inserted = 1;
			float y0 = *(float *)(items[i] + ITEM_RECT_Y);
			for (int b = 0; b < nbuttons; b++) {
				uint8_t *item = HeapAlloc(GetProcessHeap(), 0, ITEM_SIZE);
				memcpy(item, src, ITEM_SIZE);
				float y = y0 + header_dy + b * BUTTON_HEIGHT;
				const MenuItem *def = menutable_item("main_text", b);
				*(const char **)item = def->name;
				*(const char **)(item + ITEM_TEXT) = def->text;
				*(MenuEventHandlerSet **)(item + ITEM_ACTION) = ui_script_set(def->action);
				if (!strcmp(def->name, "mw2coop_multiplayer_btn")) {
					void *original = original_multiplayer_action();
					if (original)
						*(void **)(item + ITEM_ACTION) = original;
					log_printf("menus: the Multiplayer button runs %s", original ? "the game's own action" : "the fallback of menus.menu");
				}
				*(uint32_t *)(item + ITEM_DYNAMIC_FLAGS) &= ~WINDOW_HASFOCUS;
				*(float *)(item + ITEM_RECT_Y) = y;
				*(float *)(item + ITEM_RECT_CLIENT_Y) = y;
				list[n++] = item;
			}
			continue;
		}
		if (name && !strcmp(name, "options")) // Options is called Settings
			*(const char **)(items[i] + ITEM_TEXT) = "@MW2COOP_SETTINGS";
		// Stats and the Main menu button are gone: Credits moves up one row, Quit two
		if (name && (!strcmp(name, "credits") || !strcmp(name, "button_quit"))) {
			float up = BUTTON_HEIGHT * (!strcmp(name, "credits") ? 1 : about ? 1 : 2);
			*(float *)(items[i] + ITEM_RECT_Y) -= up;
			*(float *)(items[i] + ITEM_RECT_CLIENT_Y) -= up;
		}
		if (inserted) { // the rows below the three entries (the line, Settings, Credits, Quit) move down with them
			*(float *)(items[i] + ITEM_RECT_Y) += header_dy;
			*(float *)(items[i] + ITEM_RECT_CLIENT_Y) += header_dy;
		}
		list[n++] = items[i];
		if (about && i == last_credits) {
			uint8_t *item = HeapAlloc(GetProcessHeap(), 0, ITEM_SIZE);
			memcpy(item, src, ITEM_SIZE);
			float y = *(float *)(items[i] + ITEM_RECT_Y) + BUTTON_HEIGHT;
			*(const char **)item = about->name;
			*(const char **)(item + ITEM_TEXT) = about->text;
			*(MenuEventHandlerSet **)(item + ITEM_ACTION) = ui_script_set(about->action);
			*(uint32_t *)(item + ITEM_DYNAMIC_FLAGS) &= ~WINDOW_HASFOCUS;
			*(float *)(item + ITEM_RECT_Y) = y;
			*(float *)(item + ITEM_RECT_CLIENT_Y) = y;
			list[n++] = item;
		}
	}
	// the old items stay valid, so the UI may read the new array before the new count
	main_items = list;
	*(uint8_t ***)(menu + MENU_ITEMS) = list;
	*(int *)(menu + MENU_ITEM_COUNT) = n;
	// the game's onOpen ends with "focusfirst", which no longer finds the Resume button it was written for: the first entry gets the focus by name (a key or a pad starts there)
	append_on_open(menu, "\"setfocus\" \"mw2coop_campaign_btn\" ; ");
	log_printf("menus: main_text: Campaign, Spec Ops and Multiplayer entries (%d items)", n);
}

// Esc on a page of the mod closes it (the pages are copies of the main menu, whose Esc would ask whether to quit). onESC follows onClose in the menu (onOpen, onCloseRequest, onClose, onESC).
static void set_page_esc(uint8_t *page)
{
	*(void **)(page + MENU_ON_CLOSE + sizeof(void *)) = ui_script_set("\"play\" \"mouse_click\" ; \"close\" \"self\" ; ");
}

// One more handler (a menu script) at the end of a menu's onOpen.
static void append_on_open(uint8_t *menu, const char *script)
{
	const MenuEventHandlerSet *old = *(MenuEventHandlerSet **)(menu + MENU_ON_OPEN);
	int n = old && old->count > 0 && old->count < 64 ? old->count : 0;
	MenuEventHandlerSet *set = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*set));
	MenuEventHandler **list = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (n + 1) * sizeof(*list));
	for (int i = 0; i < n; i++)
		list[i] = old->handlers[i];
	MenuEventHandler *h = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*h));
	h->data = (void *)script;
	h->type = 0;
	list[n] = h;
	set->count = n + 1;
	set->handlers = list;
	*(MenuEventHandlerSet **)(menu + MENU_ON_OPEN) = set;
}

// The game starts on "main_selection", its picture cards Campaign / Spec Ops / Multiplayer. The mod's main menu is the list in main_text (patch_main_menu), so the
// selection menu opens it when it opens itself: one more handler at the end of its onOpen.
static uint8_t *selection_patched; // the menu object whose onOpen was extended
// The Spec Ops page's way back: its Main menu button opens the game's own Campaign / Spec Ops picture chooser ("main_selection"); the mod's main menu is main_text, so
// the scripts of the page's items open that instead. The scripts are replaced once per menu object (a zone reload makes a new one).
static int specops_replace_set(MenuEventHandlerSet *set, int depth, const char *from, const char *to)
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
#ifdef _WIN64
		} else if (h->type == 1 && h->data && game_mem_readable(h->data, 16)) { // if (expression, set): a readable set at either place is followed
			done += specops_replace_set(*(MenuEventHandlerSet **)h->data, depth + 1, from, to);
			done += specops_replace_set(*(MenuEventHandlerSet **)((uint8_t *)h->data + 8), depth + 1, from, to);
		} else if (h->type == 2 && h->data) {
			done += specops_replace_set((MenuEventHandlerSet *)h->data, depth + 1, from, to);
#endif
		}
	}
	return done;
}

static uint8_t *specops_seen;
static void patch_specops_return(void)
{
	uint8_t *menu = ui_find_menu("main_specops");
	if (!menu || menu == specops_seen)
		return;
	specops_seen = menu;
	uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
	int count = *(int *)(menu + MENU_ITEM_COUNT), done = 0;
	for (int i = 0; i < count; i++)
		done += specops_replace_set(*(MenuEventHandlerSet **)(items[i] + ITEM_ACTION), 0, "\"main_selection\"", "\"main_text\"");
	set_page_esc(menu); // Esc closes the page, which was opened over the main menu (the game's own Esc sent it to the chooser)
	log_printf("menus: main_specops: %d script(s) now open main_text (the Main menu button); Esc closes the page", done);
}

static void patch_selection_menu(void)
{

	uint8_t *sel = ui_find_menu("main_selection");
	if (!sel || sel == selection_patched || !ui_find_menu("main_text"))
		return;
	selection_patched = sel;
	const MenuEventHandlerSet *old = *(MenuEventHandlerSet **)(sel + MENU_ON_OPEN);
	int n = old && old->count > 0 && old->count < 64 ? old->count : 0;
	MenuEventHandlerSet *set = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*set));
	MenuEventHandler **list = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (n + 1) * sizeof(*list));
	for (int i = 0; i < n; i++)
		list[i] = old->handlers[i];
	MenuEventHandler *h = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*h));
	h->data = (void *)"\"open\" \"main_text\" ; ";
	h->type = 0;
	list[n] = h;
	set->count = n + 1;
	set->handlers = list;
	*(MenuEventHandlerSet **)(sel + MENU_ON_OPEN) = set;
	log_printf("menus: main_selection opens main_text (onOpen had %d handlers)", n);
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
			ui_script_set("\"play\" \"mouse_over\" ; \"setItemColor\" \"self\" \"backcolor\" 0 0 0 1 ; ");
		*(void **)(it + ITEM_LEAVE_FOCUS) =
			ui_script_set("\"setItemColor\" \"self\" \"backcolor\" 0 0 0 \"0.0\" ; ");
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
		uint8_t *main = ui_find_menu("main_text");
		wanted = main ? *(const char **)(main + MENU_SOUND_LOOP) : NULL;
		if (!wanted)
			wanted = "music_mainmenu";
	} else if (!lobby_music_saved) {
		return;
	}
	if (*loop != wanted) {
		*loop = wanted;
		log_printf("menus: coop_lobby music set to %s", campaign ? "the single-player menu theme" : "the native Special Ops theme");
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
	const int split_now = split_pending != 0;
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
	if (!so_name(map))
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

// Leaving the campaign lobby: coop_lobby is the Special Ops lobby, and its "leave" popup (leave_lobby_warning) sends the player on to main_specops. Where that is done by a menu script
// ("open" "main_specops"), the script is changed here for as long as the lobby holds a campaign mission, so the player lands on main_text (the engine's other way out, by the UI
// state, is caught in menus_poll). The native scripts are put back for a Spec Ops lobby. The first time the popup is seen its items are logged, to see what the game really runs.
typedef struct {
	MenuEventHandler *handler;
	void *native;
} LeaveSwap;
static LeaveSwap leave_swaps[24];
static int leave_swap_count, leave_swapped, leave_logged;
static uint8_t *leave_menu_seen;

// a copy of the script with every "main_specops" (quoted) replaced by "main_text"; NULL when it has none
static char *leave_script_copy(const char *src)
{
	static const char from[] = "\"main_specops\"", to[] = "\"main_text\"";
	if (!strstr(src, from))
		return NULL;
	size_t cap = strlen(src) + 1;
	char *out = HeapAlloc(GetProcessHeap(), 0, cap);
	if (!out)
		return NULL;
	size_t n = 0;
	while (*src) {
		if (!strncmp(src, from, sizeof(from) - 1)) {
			memcpy(out + n, to, sizeof(to) - 1);
			n += sizeof(to) - 1;
			src += sizeof(from) - 1;
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
	log_printf("menus: leave_lobby_warning: %d script(s) now open main_text instead of main_specops", leave_swap_count);
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
	static const char *const online_rows[] = {"invitefriend", "mw2coop_access", "mw2coop_password", "nattype"};
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
#ifdef _WIN64
#define ITEM_STATIC_FLAGS 0x4c
#else
#define ITEM_STATIC_FLAGS 0x44
#endif
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
static volatile LONG my_ready, partner_ready;
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

static void send_ready(void)
{
	char text[96];
	snprintf(text, sizeof(text), READY_TAG "%ld %s %016llx", my_ready, settings_player_id(),
		 (unsigned long long)*(uint64_t *)(base + RVA_XUID_TABLE));
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
	char rest[96] = "", id[33] = "";
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

static void update_texts(void)
{
	int host = base[RVA_PARTY_IS_HOST];
	// a split screen lobby has no Ready button: the second player is on this PC, his device is all that is needed
	if (split_mode)
		localize_set_text("MW2COOP_READY", "");
	else
		localize_select("MW2COOP_READY", my_ready ? "MW2COOP_READY_UNSET" : "MW2COOP_READY_SET");
	show_row(ui_find_menu("coop_lobby"), "dont_selectdiff", !split_mode || spec_split_lobby()); // (Special Ops keeps its difficulty row)
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
static void start_launch(void);

// devices.c: a player's device changed
void menus_split_changed(void)
{
	update_texts();
}

// The split screen's Start (src/native.c): the second player is on this PC, in this process. The lobby's party is let go, the game becomes a two local players game and the
// mission the lobby shows starts as a normal level.
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
	// the second player is a member of the save like a partner who joins it (a Special Ops op has no save)
	if (!specops)
		progress_begin(continuing, "split-screen", 0x53504c4954ULL, "Player 2", 1);
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
	char cmd[160];
	if (!specops)
		campaign_loading_begin(); // (a Special Ops op keeps its own loading screen: specialops stays 1)
	snprintf(cmd, sizeof(cmd), "stopListening\nset mw2coop_split_view 0\n%smap %s\n", specops ? "set specialops 1\n" : "", map);
	game_cbuf_addtext(cmd);
	InterlockedExchange(&native_started, (LONG)GetTickCount() | 1);
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
	if (continuing && !partner_confirmed && progress_partner_is_new(partner_id, partner_xuid)) {
		log_printf("menus: Start: the partner is not a member of this save, asking");
		partner_questions();
		game_cbuf_addtext("openmenu mw2coop_partner\n");
		return;
	}
	progress_begin(continuing, partner_id, partner_xuid, (const char *)base + RVA_PARTY_MEMBER_NAMES + 0x38, // slot 1
		       add_member);
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
	if ((fmt = localize_text("MW2COOP_PARTNER_FMT"))) {
		snprintf(text, sizeof(text), fmt, mission ? mission : map, members, partner);
		localize_set_text("MW2COOP_PARTNER_Q", text);
	}
	if ((fmt = localize_text("MW2COOP_MEMBER_FMT"))) {
		snprintf(text, sizeof(text), fmt, partner);
		localize_set_text("MW2COOP_MEMBER_Q", text);
	}
	if ((fmt = localize_text("MW2COOP_NEWGAME_FMT"))) {
		snprintf(text, sizeof(text), fmt, partner);
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

// Start with a partner who is not a member of the save: continue with him? Then: add him to this
// playthrough's members? No to that: start a new game from the very beginning? No anywhere closes
// the popup and nothing starts.
static void add_partner_popups(void)
{
	yes_no_popup("mw2coop_partner");
	yes_no_popup("mw2coop_member");
	yes_no_popup("mw2coop_newgame");
}

// mw2coop_online: run by the campaign menu's co-op button before "listen" opens the lobby
static void online_f(void)
{
	lobby_music_patch(1); // second chance before listen activates coop_lobby
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
	snprintf(cmd, sizeof(cmd), "set coop_mapName %s\nset start \"%s\"\nset mw2coop_cp_start \"%s\"\nset mw2coop_cp_pos \"%s\"\n", map,
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

// Player 1 works the lobby; on a pad, his pad does it: a thread reads it 60 times a second and queues the keys a menu knows (mw2coop_menukey: the engine's key event,
// rva 0xd0f80, on the main thread). D-pad / left stick: arrows (a held direction again after 400 ms, then every 120 ms), A: Enter, B: Escape. Back is the lobby's
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
	DWORD(WINAPI * get)(DWORD, xs_t *) = NULL;
	const char *dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
	for (int k = 0; k < 3 && !get; k++) {
		HMODULE h = LoadLibraryA(dlls[k]);
		if (h)
			get = (DWORD(WINAPI *)(DWORD, xs_t *))GetProcAddress(h, "XInputGetState");
	}
	if (!get)
		return 0;
	int seen = -1;
	WORD prev = 0;
	DWORD repeat_at = 0;
	for (;;) {
		Sleep(16);
		int pad = devices_menu_pad();
		if (!split_mode || native_split_active() || pad < 0) {
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

static cmd_function_t commands[] = {
	{NULL, "mw2coop_online", NULL, NULL, online_f CMD_NODE_FLAG},
	{NULL, "mw2coop_host_menu", NULL, NULL, host_menu_f CMD_NODE_FLAG},
	{NULL, "mw2coop_split_menu", NULL, NULL, split_menu_f CMD_NODE_FLAG},
	{NULL, "mw2coop_open_url", NULL, NULL, open_url_f CMD_NODE_FLAG},
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
	{NULL, "mw2coop_find", NULL, NULL, find_f CMD_NODE_FLAG},
	{NULL, "mw2coop_join_found", NULL, NULL, join_found_f CMD_NODE_FLAG},
};

// The menu work runs on the game's main thread, between frames: the control thread only queues
// "mw2coop_menus_tick" (every 250 ms, never while a level runs or loads). The UI zone may be
// reloaded (back from a level), which brings fresh menus without our changes.
static volatile LONG tick_queued;
static volatile DWORD tick_queued_at;

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
static volatile LONG campaign_main_route_tick; // when main_text was last asked for, 0 when not waiting for it
static volatile LONG campaign_main_route_tries;
static void campaign_loading_begin(void)
{
	dvar_t *d = game_find_dvar("specialops");
	if (!d)
		return;
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
#ifdef _WIN64
	hook_jump(base + RVA_COOP_WAIT, 16, (void *)coop_wait_hook, 0xcc);
#else
	// the original touches only AL and the flags, and its callers (whole-program optimized) keep values in ECX and EDX across it: the stub saves them
	hook_jump(base + RVA_COOP_WAIT, 16, (void *)coop_wait_stub, 0xcc);
#endif
	log_printf("menus: the briefing of a network co-op waits for the film");
}


void menus_poll(void)
{
	if (!base)
		return;
	char v[8] = "", coop[8] = "";
	game_dvar_raw("sv_running", v, sizeof(v));
	game_dvar_raw("coop", coop, sizeof(coop));
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
		if (ui != last_ui || cls != last_cls) {
			log_printf("menus: ui state %d, client state %d", ui, cls);
			if (ui == 6)
				InterlockedExchange(&brief_partner_done, 0);
			last_ui = ui, last_cls = cls;
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
		// The engine can still be opening main_specops when main_text is asked for, and then main_specops ends up on top: while the UI state stays on it, ask once more.
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
	{ // the loading screen's look: the campaign's while a campaign co-op or split screen game is being set up or played (Spec Ops' lobby and levels keep theirs)
		static int sticky;
		char mapn[64] = "";
		game_dvar_raw("coop_mapName", mapn, sizeof(mapn));
		int campaign = base[RVA_PARTY_ACTIVE] && mapn[0] && strncmp(mapn, "so_", 3) != 0;
		int split_so = so_name(split_return_map) && (native_split_active() || split_lobby_returning); // a split screen Special Ops op: its own loading screen, no campaign briefing
		int lobby_ui = *(volatile int *)(base + RVA_UI_STATE);
		if (split_so)
			campaign = 0;
		if (!split_so && (campaign || native_split_active() || split_lobby_returning || (specialops_forced && v[0] == '1')))
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
	if (v[0] == '1')
		return;
	// the queue is cleared by a map load; a tick queued before it never runs
	if (tick_queued && GetTickCount() - tick_queued_at < 3000)
		return;
	tick_queued_at = GetTickCount();
	InterlockedExchange(&tick_queued, 1);
	game_cbuf_addtext("mw2coop_menus_tick\n");
}

static void tick_f(void)
{
	InterlockedExchange(&tick_queued, 0);
	if (!base)
		return;
	native_session_tick();
	patch_main_menu();
	patch_selection_menu();
	patch_specops_return();
	patch_act1_menu();
	patch_specops_menu();
	add_lobbies_menu();
	add_host_menu();
	add_saves_menu();
	add_partner_popups();
	padopts_tick();
	gfxopts_tick();
	enhopts_tick();
	picking_tick();
	unlock_missions_tick();
	find_tick();
	int campaign = campaign_lobby();
	int spec_split = spec_split_lobby();
	int split_lobby = campaign || spec_split; // the lobby of a split screen game of ours: the return to it is looked after for a Special Ops op too
	int lobby_ui = *(volatile int *)(base + RVA_UI_STATE);
	patch_lobby(campaign, spec_split);
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
	if (InterlockedExchange(&campaign_main_return_pending, 0)) {
		game_cbuf_addtext("openmenu main_text\n");
		LONG tries = InterlockedIncrement(&campaign_main_route_tries);
		InterlockedExchange(&campaign_main_route_tick, (LONG)GetTickCount() | 1);
		log_printf("menus: campaign lobby exit routed to main_text (ui state %d, try %ld)", *(volatile int *)(base + RVA_UI_STATE), (long)tries);
	}
	if (InterlockedExchange(&campaign_lobby_return_pending, 0)) {
		if (campaign && *(volatile int *)(base + RVA_UI_STATE) == 0x0e) {
			lobby_music_patch(1);
			game_cbuf_addtext("openmenu coop_lobby\n");
			log_printf("menus: campaign party returned to main_specops; reopening coop_lobby");
		} else
			InterlockedExchange(&campaign_lobby_return_queued, 0);
	}
	access_tick(campaign, base[RVA_PARTY_IS_HOST]);
	firewall_tick(campaign && base[RVA_PARTY_IS_HOST] && !split_mode); // the second player is on this PC: nothing to let in
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
				snprintf(dev, sizeof(dev), f ? f : "pad %d", d + 1);
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
