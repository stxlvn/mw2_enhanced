// The mod's own options page in the game's menu: Settings > Enhanced (owner, 2026-11-11: "enhance game settings page, add more parameters"). Until now the mod's settings lived in
// settings.ini only; the ones a player changes most often get rows on a page of the game's own look, made from its Look page (pc_options_look) the way the gamepad pages are
// (padopts.c): its slider and its yes / no rows, and a choice row taken from the advanced video page, bound to dvars, so the game draws and changes them like its own options
// (mouse, keyboard or pad).
//
// Three kinds of rows:
//   - a dvar of the game itself (com_maxfps): the row only shows it;
//   - a dvar of this page that the game saves in players\config.cfg (the field of view): applied here while the game runs;
//   - a dvar that mirrors settings.ini: read from the file when the page is made, written back when the player changes it. Some take effect at once (the upscaler's sharpening,
//     the HUD text sizes: settings.c has setters for them); the rest are read while the graphics device is created, so a change of one of them opens a pop-up that suggests
//     restarting the game (owner, 2026-11-11: no "(restart)" in the rows' names, a pop-up instead).
// The upscalers are one row with a choice (owner: "make switchable, Upscaler FSR1 FSR3 DLSS"): Off, FSR 1, FSR 3, DLSS; it writes the three keys upscale / fsr3 / dlss.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "enhopts.h"
#include "game.h"
#include "log.h"
#include "menus_ui.h"
#include "native.h"
#include "settings.h"

#ifdef _WIN64
#define RVA_DVAR_REGISTER_BOOL XR(0x26c930)  // dvar_t *(const char *name, bool value, unsigned flags, const char *description)
#define RVA_DVAR_REGISTER_FLOAT XR(0x26cb90) // dvar_t *(const char *name, float value, float min, float max, unsigned flags, const char *description)
// multiDef_s (the data of a choice row, item type 12): const char *dvarList[32]; const char *dvarStr[32]; float dvarValue[32]; int count; int strDef;
#define MULTI_VALUE 0x200
#define MULTI_COUNT 0x280
#define MULTI_SIZE 0x288
#define UPSCALERS 4 // Off, FSR 1, FSR 3, DLSS
#else
#define RVA_DVAR_REGISTER_BOOL XR(0x866b0)
#define RVA_DVAR_REGISTER_FLOAT XR(0x9f700)
#define MULTI_VALUE 0x100
#define MULTI_COUNT 0x180
#define MULTI_SIZE 0x188
#define UPSCALERS 2 // Off, FSR 1 (FSR 3 and DLSS need the 64-bit build)
#endif
#define DVAR_ARCHIVE 1
#define ITEM_TYPE_SLIDER 10
#define ITEM_TYPE_YESNO 11
#define ITEM_TYPE_MULTI 12

enum { K_SLIDER, K_YESNO, K_UPSCALER };
enum { LIVE_NONE, LIVE_FOV, LIVE_SHARP, LIVE_HUDTEXT, LIVE_NAMETAG, LIVE_FALL };
// What a row applies to; the page lists the rows by these groups (in this order), each under its name, not by how a change takes effect.
enum { GROUP_PICTURE, GROUP_WINDOW, GROUP_UPSCALING, GROUP_HUD, GROUP_COOP, GROUPS };
static const char *const group_names[GROUPS] = {"@MW2COOP_ENH_G_PICTURE", "@MW2COOP_ENH_G_WINDOW", "@MW2COOP_ENH_G_UPSCALING", "@MW2COOP_ENH_G_HUD", "@MW2COOP_ENH_G_COOP"};
enum { OPT_FOV, OPT_MAXFPS, OPT_UPSCALER, OPT_SHARP }; // positions in opts[] (the order in the page is the group's, then this one)

typedef struct {
	const char *dvar;	   // the dvar the row is bound to
	const char *label;	   // the row's text (a localized reference, src/localize.c)
	int kind;
	float lo, hi, def;
	const char *section, *key; // settings.ini: where the value is kept; NULL: the game's config keeps the dvar (K_UPSCALER: three keys, see upscaler_write)
	int own;		   // 1: registered here; 0: the game's own dvar
	int live;		   // what a change does at once
	int restart;		   // it takes effect when the game starts next: a change opens the pop-up
	const char *description;
	int group;		   // GROUP_*: what the row applies to
} opt_t;

static const opt_t opts[] = {
    {"mw2coop_opt_fov", "@MW2COOP_ENH_FOV", K_SLIDER, 65, 120, 65, NULL, NULL, 1, LIVE_FOV, 0, "Enhanced: field of view in degrees (65: the game's own)", GROUP_PICTURE},
    {"com_maxfps", "@MW2COOP_ENH_MAXFPS", K_SLIDER, 0, 300, 0, NULL, NULL, 0, LIVE_NONE, 0, NULL, GROUP_PICTURE},
    {"mw2coop_opt_upscaler", "@MW2COOP_ENH_UPSCALER", K_UPSCALER, 0, UPSCALERS - 1, 0, "display", NULL, 1, LIVE_NONE, 1, "Enhanced: the upscaler, 0 off, 1 FSR 1, 2 FSR 3, 3 DLSS (next start)", GROUP_UPSCALING},
    {"mw2coop_opt_sharp", "@MW2COOP_ENH_SHARP", K_SLIDER, 0, 100, 60, "display", "fsr3_sharpness", 1, LIVE_SHARP, 0, "Enhanced: sharpening of FSR 3 and DLSS, 0-100", GROUP_UPSCALING},
    {"mw2coop_opt_hudtext", "@MW2COOP_ENH_HUDTEXT", K_SLIDER, 20, 200, 50, "display", "hudtextscale", 1, LIVE_HUDTEXT, 0, "Enhanced: size of the objective labels, percent", GROUP_HUD},
    {"mw2coop_opt_nametag", "@MW2COOP_ENH_NAMETAG", K_SLIDER, 20, 200, 70, "display", "nametagscale", 1, LIVE_NAMETAG, 0, "Enhanced: size of the names over players, percent", GROUP_HUD},
    {"mw2coop_opt_falldmg", "@MW2COOP_ENH_FALLDMG", K_YESNO, 0, 1, 0, "gameplay", "fall_damage", 1, LIVE_FALL, 0, "Enhanced: fall damage in a co-op session", GROUP_COOP},
    {"mw2coop_opt_borderless", "@MW2COOP_ENH_BORDERLESS", K_YESNO, 0, 1, 1, "display", "borderless", 1, LIVE_NONE, 1, "Enhanced: full screen as a borderless window (next start)", GROUP_WINDOW},
    {"mw2coop_opt_darktitle", "@MW2COOP_ENH_DARKTITLE", K_YESNO, 0, 1, 1, "display", "darktitle", 1, LIVE_NONE, 1, "Enhanced: dark title bar (next start)", GROUP_WINDOW},
#ifdef _WIN64
    {"mw2coop_opt_d3d12", "@MW2COOP_ENH_D3D12", K_YESNO, 0, 1, 0, "display", "d3d9on12", 1, LIVE_NONE, 1, "Enhanced: Direct3D 9 on the Direct3D 12 layer (next start)", GROUP_WINDOW},
#endif
};
#define OPTS ((int)(sizeof(opts) / sizeof(opts[0])))

static dvar_t *dvars[sizeof(opts) / sizeof(opts[0])];
static float last[sizeof(opts) / sizeof(opts[0])]; // the value written to settings.ini (or applied) last
static int registered;
static int restart_wanted; // a row that waits for the next start was changed since the page's pop-up was last shown
static float started[sizeof(opts) / sizeof(opts[0])]; // each row's value when the game started: what the running game uses

static float value_of(int o)
{
	dvar_t *d = dvars[o];
	if (!d)
		return opts[o].def;
	if (d->type == DVAR_TYPE_BOOL)
		return (float)d->current.enabled;
	if (d->type == DVAR_TYPE_INT)
		return (float)d->current.integer;
	return d->current.value;
}

// the upscaler in settings.ini: three switches (dlss wins over fsr3, fsr3 over upscale, as settings.c reads them)
static int upscaler_read(const char *path)
{
	int v = 0;
	if (GetPrivateProfileIntA("display", "upscale", 0, path))
		v = 1;
	if (UPSCALERS > 2 && GetPrivateProfileIntA("display", "fsr3", 0, path))
		v = 2;
	if (UPSCALERS > 3 && GetPrivateProfileIntA("display", "dlss", 0, path))
		v = 3;
	return v;
}

static void upscaler_write(const char *path, int v)
{
	int ok = WritePrivateProfileStringA("display", "upscale", v == 1 ? "1" : "0", path);
	ok &= WritePrivateProfileStringA("display", "fsr3", v == 2 ? "1" : "0", path);
	ok &= WritePrivateProfileStringA("display", "dlss", v == 3 ? "1" : "0", path);
	if (ok)
		log_printf("enhopts: settings.ini [display] upscale=%d fsr3=%d dlss=%d (the upscaler: %s)", v == 1, v == 2, v == 3, v == 1 ? "FSR 1" : v == 2 ? "FSR 3" : v == 3 ? "DLSS" : "off");
	else
		log_printf("enhopts: settings.ini could not be written (the upscaler, error %lu)", GetLastError());
}

static void register_dvars(void)
{
	if (registered)
		return;
	if (!game_find_dvar("com_maxfps")) // the game's dvar table is not there yet
		return;
	registered = 1;
	uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
	dvar_t *(*reg_bool)(const char *, int, unsigned, const char *) = (void *)(base + RVA_DVAR_REGISTER_BOOL);
	dvar_t *(*reg_float)(const char *, float, float, float, unsigned, const char *) = (void *)(base + RVA_DVAR_REGISTER_FLOAT);
	const char *path = settings_ini_path();
	for (int o = 0; o < OPTS; o++) {
		const opt_t *t = &opts[o];
		if (!t->own) {
			dvars[o] = game_find_dvar(t->dvar);
			last[o] = value_of(o);
			continue;
		}
		float v = t->def;
		if (t->kind == K_UPSCALER)
			v = (float)upscaler_read(path);
		else if (t->section) { // the file's value, else the default
			int iv = (int)GetPrivateProfileIntA(t->section, t->key, (int)t->def, path);
			if ((float)iv >= t->lo && (float)iv <= t->hi)
				v = (float)iv;
		}
		const unsigned flags = t->section ? 0 : DVAR_ARCHIVE; // a settings.ini value is saved there, not in the game's config too
		dvars[o] = t->kind == K_YESNO ? reg_bool(t->dvar, v != 0, flags, t->description) : reg_float(t->dvar, v, t->lo, t->hi, flags, t->description);
		last[o] = value_of(o);
		started[o] = last[o];
	}
	log_printf("enhopts: %d options registered (field of view %.0f, sharpening %.0f)", OPTS, value_of(OPT_FOV), value_of(OPT_SHARP));
}

// The field of view: the game's cg_fov is 65 across a 4:3 picture and its scripts write it (and cg_fovScale, at each level load); cg_fovScale multiplies every view's angle, the
// sights' zoom included, so the wanted angle is held there as want / 65. Not while the picture is split: the split screen has its own scale (native.c).
static void hold_fov(void)
{
	static dvar_t *scale;
	static int logged, held;
	const float want = value_of(OPT_FOV);
	if (native_split_active())
		return;
	if (!scale)
		scale = game_find_dvar("cg_fovScale");
	if (!scale || scale->type != DVAR_TYPE_FLOAT)
		return;
	const float s = want / 65.0f;
	if (want <= 65.5f) { // the game's own: only undo what this page set
		if (held) {
			scale->current.value = 1.0f;
			scale->modified = 1;
			held = 0;
		}
		return;
	}
	if (scale->current.value != s) {
		scale->current.value = s;
		scale->modified = 1;
		held = 1;
		if (logged++ < 4)
			log_printf("enhopts: field of view %.0f degrees (cg_fovScale %.3f)", want, s);
	}
}

// a changed row: its settings.ini key is written and what can follow at once is done; a row that waits for the next start asks for the pop-up
static void follow_changes(void)
{
	char text[32], cmd[64];
	const char *path = settings_ini_path();
	for (int o = 0; o < OPTS; o++) {
		const opt_t *t = &opts[o];
		if (!dvars[o] || !t->own)
			continue;
		float v = value_of(o);
		if (t->kind != K_YESNO)
			v = (float)(int)(v + 0.5f); // whole numbers in the file
		if (v == last[o])
			continue;
		last[o] = v;
		if (t->kind == K_UPSCALER)
			upscaler_write(path, (int)v);
		else if (t->section) {
			snprintf(text, sizeof(text), "%d", (int)v);
			if (WritePrivateProfileStringA(t->section, t->key, text, path))
				log_printf("enhopts: settings.ini [%s] %s=%s", t->section, t->key, text);
			else
				log_printf("enhopts: settings.ini could not be written ([%s] %s=%s, error %lu)", t->section, t->key, text, GetLastError());
		}
		if (t->restart)
			restart_wanted = 1;
		switch (t->live) {
		case LIVE_SHARP:
			settings_set_fsr3_sharpness((int)v);
			break;
		case LIVE_HUDTEXT:
			settings_set_hudtextscale((int)v);
			break;
		case LIVE_NAMETAG:
			settings_set_nametagscale((int)v);
			break;
		case LIVE_FALL: // the session dvar the co-op scripts read (control.c sets it from settings.ini at the start)
			snprintf(cmd, sizeof(cmd), "set mw2coop_fall_damage %d\n", (int)v);
			game_cbuf_addtext(cmd);
			break;
		default:
			break;
		}
	}
}

static uint8_t *item_by_text(uint8_t *menu, const char *text)
{
	int count = *(int *)(menu + MENU_ITEM_COUNT);
	uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
	for (int i = 0; i < count; i++) {
		const char *t = *(const char **)(items[i] + ITEM_TEXT);
		if (t && !strcmp(t, text))
			return items[i];
	}
	return NULL;
}

static uint8_t *item_by_type(uint8_t *menu, int type)
{
	int count = *(int *)(menu + MENU_ITEM_COUNT);
	uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
	for (int i = 0; i < count; i++)
		if (*(int *)(items[i] + ITEM_TYPE) == type)
			return items[i];
	return NULL;
}

static void place(uint8_t *item, float y)
{
	*(float *)(item + ITEM_RECT_Y) = y;
	*(float *)(item + ITEM_RECT_CLIENT_Y) = y;
}

// The upscaler's choice row: a copy of a choice row of the advanced video page (same place and size as the Look page's yes / no rows) with a list of its own
static uint8_t *upscaler_item(uint8_t *menu)
{
	static const char *const names[4] = {"@MW2COOP_ENH_OFF", "FSR 1", "FSR 3", "DLSS"};
	uint8_t *video = ui_find_menu("pc_options_advanced_video");
	uint8_t *src = video ? item_by_type(video, ITEM_TYPE_MULTI) : NULL;
	if (!src || !*(void **)(src + ITEM_TYPE_DATA))
		return NULL;
	uint8_t *it = ui_clone_item(src, menu);
	uint8_t *multi = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, MULTI_SIZE);
	for (int i = 0; i < UPSCALERS; i++) {
		((const char **)multi)[i] = names[i];
		((float *)(multi + MULTI_VALUE))[i] = (float)i;
	}
	*(int *)(multi + MULTI_COUNT) = UPSCALERS;
	*(int *)(multi + MULTI_COUNT + 4) = 0; // strDef: the list holds numbers
	*(void **)(it + ITEM_TYPE_DATA) = multi;
	return it;
}

// A menu's event handler sets (onOpen, onCloseRequest, onClose, onESC; menus.c has the same two types): a count and a list of handlers, each one a menu script.
#ifdef _WIN64
typedef struct {
	void *data;
	char type;
	char pad[7];
} ev_handler_t;
typedef struct {
	int count;
	int pad;
	ev_handler_t **handlers;
} ev_set_t;
#else
typedef struct {
	void *data;
	char type;
	char pad[3];
} ev_handler_t;
typedef struct {
	int count;
	ev_handler_t **handlers;
} ev_set_t;
#endif

// One more script at the end of a menu's onClose; what the menu ran there before stays.
static void append_on_close(uint8_t *menu, const char *script)
{
	const ev_set_t *old = *(ev_set_t **)(menu + MENU_ON_CLOSE);
	const int n = old && old->count > 0 && old->count < 64 ? old->count : 0;
	ev_set_t *set = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*set));
	ev_handler_t **list = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (n + 1) * sizeof(*list));
	for (int i = 0; i < n; i++)
		list[i] = old->handlers[i];
	ev_handler_t *h = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*h));
	h->data = (void *)script;
	h->type = 0;
	list[n] = h;
	set->count = n + 1;
	set->handlers = list;
	*(ev_set_t **)(menu + MENU_ON_CLOSE) = set;
}

// The page: the Look page's backgrounds, title and Back items, then the rows 20 units apart from y 28, in groups by what a row applies to (GROUP_*), each group under its name.
static void add_page(void)
{
	if (ui_find_menu("mw2coop_enhanced"))
		return;
	uint8_t *src = ui_find_menu("pc_options_look");
	if (!src)
		return;
	uint8_t *menu = ui_clone_menu(src, "mw2coop_enhanced");
	append_on_close(menu, "\"exec\" \"set mw2coop_enh_left 1\" ; "); // leaving the page (Back, Esc): tick_f asks the restart pop-up if a row that waits for the next start changed
	int count = *(int *)(menu + MENU_ITEM_COUNT);
	uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
	uint8_t *title = item_by_text(menu, "@MENU_LOOK"), *slider = item_by_type(menu, ITEM_TYPE_SLIDER), *yesno = item_by_type(menu, ITEM_TYPE_YESNO);
	uint8_t *slider_label = item_by_text(menu, "@MENU_MOUSE_SENSITIVITY"), *label = item_by_text(menu, "@MENU_INVERT_MOUSE");
	if (!title || !slider || !yesno || !slider_label || !label) {
		log_printf("enhopts: the Look page is not as expected, no Enhanced page");
		return;
	}
	int title_at = 0;
	while (items[title_at] != title)
		title_at++;
	uint8_t **list = HeapAlloc(GetProcessHeap(), 0, (count + 2 * OPTS + GROUPS + 2) * sizeof(*list));
	int n = 0;
	for (int i = 0; i <= title_at; i++) // the backgrounds and the title
		list[n++] = items[i];
	*(const char **)(title + ITEM_TEXT) = "@MW2COOP_ENH_MENU";
	const float slider_dy = *(float *)(slider + ITEM_RECT_Y) - *(float *)(slider_label + ITEM_RECT_Y);
	void *click = ui_script_set("\"play\" \"mouse_click\" ; ");
	int used_slider = 0, used_yesno = 0, used_label = 0, rows = 0;
	float y = 28.0f; // the next row's top: the rows are 20 units apart, a group starts 8 units lower than the end of the one above
	for (int g = 0; g < GROUPS; g++) {
		int first = 1;
		for (int o = 0; o < OPTS; o++) {
			const opt_t *t = &opts[o];
			if (t->group != g)
				continue;
			uint8_t *control, *text;
			if (t->kind == K_SLIDER) {
				control = used_slider ? ui_clone_item(slider, menu) : slider;
				text = used_slider ? ui_clone_item(slider_label, menu) : slider_label;
				used_slider = 1;
				float *field = HeapAlloc(GetProcessHeap(), 0, 0x20); // the slider's range: editFieldDef {minVal, maxVal, defVal, range, ...}
				memcpy(field, *(void **)(slider + ITEM_TYPE_DATA), 0x20);
				field[0] = t->lo, field[1] = t->hi, field[2] = t->def, field[3] = t->hi - t->lo;
				*(void **)(control + ITEM_TYPE_DATA) = field;
			} else {
				if (t->kind == K_UPSCALER) {
					control = upscaler_item(menu);
					if (!control) {
						log_printf("enhopts: no choice row to copy from the advanced video page: no Upscaler row");
						continue;
					}
				} else {
					control = used_yesno ? ui_clone_item(yesno, menu) : yesno;
					used_yesno = 1;
				}
				text = used_label ? ui_clone_item(label, menu) : label;
				used_label = 1;
				*(void **)(control + ITEM_ACTION) = click; // (the mouse pitch row also ran "uiScript update ui_mousePitch")
			}
			if (first) { // the group's name, a text row in the highlight colour, before its first row
				first = 0;
				if (rows)
					y += 8.0f;
				uint8_t *head = ui_clone_item(label, menu);
				*(const char **)(head + ITEM_TEXT) = group_names[g];
				float *color = (float *)(head + ITEM_DYNAMIC_FLAGS + 8); // windowDef_t: dynamicFlags, nextTime, foreColor[4]
				color[0] = 1.0f, color[1] = 0.82f, color[2] = 0.3f, color[3] = 1.0f;
				place(head, y);
				list[n++] = head;
				y += 20.0f;
			}
			place(control, t->kind == K_SLIDER ? y + slider_dy : y);
			place(text, y);
			*(const char **)(control + ITEM_DVAR) = t->dvar;
			*(const char **)(text + ITEM_TEXT) = t->label;
			list[n++] = control;
			list[n++] = text;
			rows++;
			y += 20.0f;
		}
	}
	for (int i = title_at + 1; i < count; i++) { // the Back items stay
		const char *t = *(const char **)(items[i] + ITEM_TEXT);
		if (t && !strcmp(t, "@PLATFORM_BACK_CAPS"))
			list[n++] = items[i];
	}
	*(uint8_t ***)(menu + MENU_ITEMS) = list;
	*(int *)(menu + MENU_ITEM_COUNT) = n;
	if (ui_register_menu(menu))
		log_printf("enhopts: page mw2coop_enhanced added (%d items, %d rows)", n, rows);
}

// The pop-up a row that waits for the next start opens: the game's quit pop-up with a text of its own. Yes quits the game (the player starts it again), No goes on playing.
static void add_popup(void)
{
	if (ui_find_menu("mw2coop_restart"))
		return;
	uint8_t *src = ui_find_menu("quit_popmenu");
	int yes = src ? ui_item_index(src, "yes") : -1;
	if (yes < 1 || ui_item_index(src, "no") < 0)
		return;
	uint8_t *menu = ui_clone_menu(src, "mw2coop_restart");
	uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
	int count = *(int *)(menu + MENU_ITEM_COUNT);
	for (int i = 0; i < count; i++) {
		const char *text = *(const char **)(items[i] + ITEM_TEXT);
		if (text && strcmp(text, "@MENU_NOTICE") == 0)
			*(const char **)(items[i] + ITEM_TEXT) = "@MW2COOP_ENH_RESTART_TITLE";
	}
	*(const char **)(items[yes - 1] + ITEM_TEXT) = "@MW2COOP_ENH_RESTART_TEXT"; // the question, just above Yes
	*(void **)(items[yes - 1] + ITEM_TEXT_EXP) = NULL;
	*(void **)(items[yes] + ITEM_ACTION) = ui_script_set("\"play\" \"mouse_click\" ; \"close\" \"self\" ; \"exec\" \"quit\" ; ");
	*(void **)(items[ui_item_index(src, "no")] + ITEM_ACTION) = ui_script_set("\"play\" \"mouse_click\" ; \"close\" \"self\" ; ");
	if (ui_register_menu(menu))
		log_printf("enhopts: restart pop-up added");
}

// The page's button in the options menu (main_options: Video, Audio, Controls, Voice, Chat, a line, Optimal settings): a copy of the Controls button right under the last page
// button above the line; the line and what is below it in that column move one row down.
static void patch_options(void)
{
	static int logged;
	uint8_t *menu = ui_find_menu("main_options");
	if (!menu || ui_item_index(menu, "mw2coop_enhanced_btn") >= 0)
		return;
	int count = *(int *)(menu + MENU_ITEM_COUNT);
	uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
	uint8_t *controls = NULL;
	float line_y = -1;
	for (int i = 0; i < count; i++) {
		const char *t = *(const char **)(items[i] + ITEM_TEXT);
		const float *r = (const float *)(items[i] + ITEM_RECT);
		if (t && !strcmp(t, "@MENU_CONTROLS") && *(int *)(items[i] + ITEM_TYPE) == 1)
			controls = items[i];
		if (r[3] == 1.0f && r[1] > 0 && r[1] < 400.0f && line_y < 0) // the separator: a 1 unit high rule
			line_y = r[1];
	}
	if (!controls) {
		if (!logged++)
			log_printf("enhopts: no Controls button in main_options, no Enhanced button");
		return;
	}
	const float *cr = (const float *)(controls + ITEM_RECT);
	float y = line_y;
	if (y < 0) { // no line: under the lowest button of the column
		for (int i = 0; i < count; i++) {
			const float *r = (const float *)(items[i] + ITEM_RECT);
			if (*(int *)(items[i] + ITEM_TYPE) == 1 && r[0] == cr[0] && items[i][ITEM_RECT_ALIGN_V] == controls[ITEM_RECT_ALIGN_V] && r[1] + BUTTON_HEIGHT > y)
				y = r[1] + BUTTON_HEIGHT;
		}
	}
	uint8_t **list = HeapAlloc(GetProcessHeap(), 0, (count + 1) * sizeof(*list));
	for (int i = 0; i < count; i++) { // the line and the rows below it, top-aligned like the buttons: one row down (copies: the old items stay as the UI may still read them)
		uint8_t *it = items[i];
		const float *r = (const float *)(it + ITEM_RECT);
		if (line_y >= 0 && it[ITEM_RECT_ALIGN_V] == controls[ITEM_RECT_ALIGN_V] && r[1] >= line_y - 0.5f && r[1] < 400.0f && r[3] <= cr[3] + 0.5f) {
			uint8_t *moved = ui_clone_item(it, menu);
			place(moved, r[1] + BUTTON_HEIGHT);
			it = moved;
		}
		list[i] = it;
	}
	uint8_t *it = ui_clone_item(controls, menu);
	*(const char **)it = "mw2coop_enhanced_btn";
	*(const char **)(it + ITEM_TEXT) = "@MW2COOP_ENH_MENU";
	*(void **)(it + ITEM_ACTION) = ui_script_set("\"play\" \"mouse_click\" ; \"open\" \"mw2coop_enhanced\" ; ");
	place(it, y);
	list[count] = it;
	*(uint8_t ***)(menu + MENU_ITEMS) = list; // the old items stay valid, so the UI may read the new array before the new count
	*(int *)(menu + MENU_ITEM_COUNT) = count + 1;
	log_printf("enhopts: Enhanced button added to main_options at y %.0f", y);
}

void enhopts_level_tick(void)
{
	if (registered)
		hold_fov();
}

int enhopts_sharpness(void)
{
	return registered && dvars[OPT_SHARP] ? (int)(value_of(OPT_SHARP) + 0.5f) : -1;
}

void enhopts_tick(void)
{
	register_dvars();
	if (!registered)
		return;
	add_page();
	add_popup();
	patch_options();
	follow_changes();
	hold_fov();
	// the page was left (its onClose set mw2coop_enh_left): a row that waits for the next start now differs from what the running game uses -> the pop-up
	char left[8];
	if (game_dvar_raw("mw2coop_enh_left", left, sizeof(left)) && left[0] == '1') {
		game_cbuf_addtext("set mw2coop_enh_left 0\n");
		int differs = 0;
		for (int o = 0; o < OPTS; o++)
			differs |= opts[o].restart && dvars[o] && value_of(o) != started[o];
		if (restart_wanted && differs && ui_find_menu("mw2coop_restart")) {
			game_cbuf_addtext("openmenu mw2coop_restart\n");
			log_printf("enhopts: the page was left after a change that takes effect at the next start: the restart pop-up is opened");
		}
		restart_wanted = 0;
	}
}
