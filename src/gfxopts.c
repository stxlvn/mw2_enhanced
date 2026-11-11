// Graphics options. The game's Options pages are menus the UI has loaded (menuDef_t); an item bound to a dvar carries its name, and a slider its range in the item's
// editFieldDef (minVal, maxVal, defVal, range as floats). Written after the MW2 Recompiled project's graphics options (a field of view up to 120, anisotropic filtering up to 16x,
// antialiasing up to 8x, a frame rate limit above the console's): the PC game has most of them in its own menus, so this module looks at what the pages offer (the log) and raises a
// slider that stops short of what the engine allows.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gfxopts.h"
#include "log.h"
#include "game.h"
#include "menus_ui.h"

#define ITEM_TYPE_SLIDER 10
#define MENU_NAME_MAX 640

#ifdef _WIN64
#define RVA_UI XR(0x1a3ab00)
#define UI_MENUS_AT 0x38
#define UI_COUNT_AT 0x1438
#else
#define RVA_UI XR(0x151cef0)
#define UI_MENUS_AT 0x38
#define UI_COUNT_AT 0xa38
#endif

static int slider_range(uint8_t *item, float *lo, float *hi)
{
	if (*(int *)(item + ITEM_TYPE) != ITEM_TYPE_SLIDER)
		return 0;
	const float *def = *(const float **)(item + ITEM_TYPE_DATA);
	if (!def)
		return 0;
	*lo = def[0];
	*hi = def[1];
	return 1;
}

static int is_option_menu(const char *name)
{
	static const char *const words[] = {"option", "video", "graphic", "display", "sound", "control"};
	char low[96];
	int n = 0;
	for (; name[n] && n < (int)sizeof(low) - 1; n++)
		low[n] = (char)((name[n] >= 'A' && name[n] <= 'Z') ? name[n] + 32 : name[n]);
	low[n] = 0;
	for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
		if (strstr(low, words[i]))
			return 1;
	return 0;
}

// every item of a page: its place, type, text and dvar (to put a new row where there is room)
static void dump_layout(const char *name)
{
	uint8_t *ui = (uint8_t *)GetModuleHandleA(NULL) + RVA_UI;
	int count = *(int *)(ui + UI_COUNT_AT);
	uint8_t **menus = (uint8_t **)(ui + UI_MENUS_AT);
	for (int m = 0; m < count && m < MENU_NAME_MAX; m++) {
		const char *mn = menus[m] ? *(const char **)menus[m] : NULL;
		if (!mn || strcmp(mn, name))
			continue;
		int n = *(int *)(menus[m] + MENU_ITEM_COUNT);
		uint8_t **items = *(uint8_t ***)(menus[m] + MENU_ITEMS);
		const float *mr = (const float *)(menus[m] + 8);
		log_printf("gfxopts: layout of %s: window %g %g %g %g", name, mr[0], mr[1], mr[2], mr[3]);
		for (int i = 0; items && i < n; i++) {
			const float *r = (const float *)(items[i] + ITEM_RECT);
			const char *text = *(const char **)(items[i] + ITEM_TEXT), *dvar = *(const char **)(items[i] + ITEM_DVAR);
			log_printf("gfxopts:   [%d] type %d rect %g %g %g %g align %d %d text '%s' dvar '%s'", i, *(int *)(items[i] + ITEM_TYPE), r[0], r[1], r[2], r[3],
				   *(char *)(items[i] + ITEM_RECT_ALIGN_H), *(char *)(items[i] + ITEM_RECT_ALIGN_V), text ? text : "", dvar ? dvar : "");
		}
	}
}

static void dump_options(void)
{
	dump_layout("pc_options_advanced_video");
	dump_layout("main_options");
	dump_layout("pc_options_look");
	uint8_t *ui = (uint8_t *)GetModuleHandleA(NULL) + RVA_UI;
	int count = *(int *)(ui + UI_COUNT_AT);
	uint8_t **menus = (uint8_t **)(ui + UI_MENUS_AT);
	for (int m = 0; m < count && m < MENU_NAME_MAX; m++) {
		const char *name = menus[m] ? *(const char **)menus[m] : NULL;
		if (!name || !is_option_menu(name))
			continue;
		int n = *(int *)(menus[m] + MENU_ITEM_COUNT);
		uint8_t **items = *(uint8_t ***)(menus[m] + MENU_ITEMS);
		log_printf("gfxopts: menu %s, %d items", name, n);
		for (int i = 0; items && i < n && i < 200; i++) {
			const char *dvar = *(const char **)(items[i] + ITEM_DVAR);
			if (!dvar || !dvar[0])
				continue;
			float lo, hi;
			if (slider_range(items[i], &lo, &hi))
				log_printf("gfxopts:   %s: slider %g .. %g", dvar, lo, hi);
			else
				log_printf("gfxopts:   %s: item type %d", dvar, *(int *)(items[i] + ITEM_TYPE));
		}
	}
}

void gfxopts_tick(void)
{
	static int dumped;
	if (dumped)
		return;
	uint8_t *ui = (uint8_t *)GetModuleHandleA(NULL) + RVA_UI;
	if (*(int *)(ui + UI_COUNT_AT) <= 0)
		return;
	dumped = 1;
	dump_options();
}
