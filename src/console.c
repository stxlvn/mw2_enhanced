// MW2 Enhanced developer console: a movable Enhanced/Legacy window, autocomplete dropdown and editor-style context menu over the game picture,
// drawn by a Present hook of the game's Direct3D 9 device (GDI paints text into a bitmap which is uploaded to a texture).
// It uses the game's window procedure for keyboard/mouse input. The same code serves the
// 64-bit, the 32-bit and the iw4x-sp builds: it needs only the device and the game's command buffer.
#include <windows.h>
#include <d3d9.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "dx/natboot.h"
#include "game.h"
#include "log.h"
#include "native.h"
#include "shot.h"
#include "localize.h"
#include "ctexts.h"
#ifndef _WIN64
#include "d3dcap.h"
#endif

#define MAX_LINES 1000
#define LINE_LEN 4096
#define MAX_DVAR_STRING_OUTPUT (1024u * 1024u)
#define MAX_SOURCE_BUFFER_BYTES (8u * 1024u * 1024u)
#define INPUT_LEN 190
#define HISTORY 24
#define MAX_COMPLETIONS 4096
#define MAX_COMPLETION_CATALOG 16384
#define COMPLETION_CATALOG_TTL_MS 5000u
#define COMPLETION_NAME_LEN 64
#define COMPLETION_VALUE_LEN 256
#define CONSOLE_LINE_HEIGHT 19
#define SCAN_TILDE 0x29 // the key left of 1, whatever the layout puts on it
#define CONSOLE_DVAR_INIT_FLAG 0x00000800u
#define DVAR_ARCHIVE_FLAG          0x00000001u
#define DVAR_LATCH_FLAG            0x00000002u
#define DVAR_CHEAT_FLAG            0x00000004u
#define DVAR_USER_CREATED_FLAG     0x00000100u
#define DVAR_SAVED_FLAG            0x00000200u
#define DVAR_SERVERINFO_FLAG       0x00000400u
#define DVAR_INIT_FLAG             0x00000800u
#define DVAR_CHANGEABLE_RESET_FLAG 0x00001000u
#define DVAR_ROM_FLAG              0x00002000u
#define DVAR_EXTERNAL_FLAG         0x00004000u
#define DVAR_AUTOEXEC_FLAG         0x00008000u
#define AUTO_CHEATS_PENDING        1
#define AUTO_CHEATS_USER_OVERRIDE  2
#define AUTO_CHEATS_APPLIED        3
#define WM_CONSOLE_CURSOR_VISIBILITY (WM_APP + 0x3D1)
#define CONSOLE_CONTEXT_INPUT  1
#define CONSOLE_CONTEXT_OUTPUT 2
#define CONSOLE_CONTEXT_ITEM_H 23
#define CONSOLE_CONTEXT_OUTPUT_WIDTH  248
#define CONSOLE_CONTEXT_INPUT_WIDTH   260
#define INPUT_UNDO_DEPTH 64
#define OUTPUT_SEARCH_LEN 128
#define CONSOLE_WATCH_COLUMNS 4
#define CONSOLE_WATCH_ROW_HEIGHT 22
#define CONSOLE_TITLE_CLICK_TIMER 0x4D574532u
#define CONSOLE_TITLE_CLICK_DELAY_MS 220u
#define INPUT_TEXT_OFFSET 62
#define CONSOLE_STATUS_HEIGHT 22
#define CONSOLE_BOTTOM_PAD 7
#define CONSOLE_INPUT_PAD 7

enum {
	CONSOLE_CURSOR_ARROW, CONSOLE_CURSOR_MOVE, CONSOLE_CURSOR_SIZE_WE,
	CONSOLE_CURSOR_SIZE_NS, CONSOLE_CURSOR_SIZE_NWSE, CONSOLE_CURSOR_SIZE_NESW,
	CONSOLE_CURSOR_HAND
};

static CRITICAL_SECTION lock;
static char lines[MAX_LINES][LINE_LEN];
static UINT line_codepage[MAX_LINES];
static char *source_lines[MAX_LINES];
static size_t source_line_lengths[MAX_LINES];
static UINT source_codepage[MAX_LINES];
static size_t source_bytes;
static int source_count, source_next;
static int console_reflowing;
static int line_count, line_next; // a ring: line_next is where the next line goes
static char input[INPUT_LEN + 1];
static int input_len, cursor, selection_anchor = -1;
static int input_selecting;
static DWORD input_last_click_tick;
static LONG input_last_click_x, input_last_click_y;
static char history[HISTORY][INPUT_LEN + 1];
static char history_draft[INPUT_LEN + 1];
static int history_count, history_pos, history_draft_valid;
static int history_filter_active, history_filter_cursor;
static char history_filter_prefix[INPUT_LEN + 1];
static char history_filter_draft[INPUT_LEN + 1];
static int output_search_mode;
static char output_search_query[OUTPUT_SEARCH_LEN];
static char output_search_saved_input[INPUT_LEN + 1];
static int output_search_saved_cursor, output_search_saved_selection;
static int output_search_match_seq = -1;
static DWORD output_last_click_tick;
static int output_last_click_seq = -1, output_click_count;
static LONG output_last_click_x, output_last_click_y;
static int output_context_line = -1;
static volatile LONG console_watch_count;
static char (*console_watch_names)[COMPLETION_NAME_LEN];
static size_t console_watch_capacity;
typedef struct {
	char text[INPUT_LEN + 1];
	int length, cursor, selection_anchor;
} input_edit_state_t;
static input_edit_state_t input_undo_stack[INPUT_UNDO_DEPTH];
static input_edit_state_t input_redo_stack[INPUT_UNDO_DEPTH];
static int input_undo_count, input_redo_count;
static int input_undo_last_group;
static DWORD input_undo_last_tick;
static int output_selecting, output_sel_a_line = -1, output_sel_a_col, output_sel_b_line = -1, output_sel_b_col;
static int scroll;
static int output_wheel_remainder;
static int completion_wheel_remainder;
typedef struct {
	char name[COMPLETION_NAME_LEN];
	char value[COMPLETION_VALUE_LEN];
	char default_value[COMPLETION_VALUE_LEN];
	uint8_t is_dvar;
} completion_item_t;

static completion_item_t completion_items[MAX_COMPLETIONS];
typedef struct {
	char name[COMPLETION_NAME_LEN];
	dvar_t *dvar;
	uint8_t is_dvar;
} completion_source_t;
static completion_source_t completion_catalog[MAX_COMPLETION_CATALOG];
static int completion_catalog_count;
static int completion_catalog_truncated;
static DWORD completion_catalog_tick;
static int completion_catalog_ready;
static int completion_count, completion_total, completion_scroll, completion_selected = -1;
static RECT completion_rect;
static RECT completion_pin_rects[MAX_COMPLETIONS];
static volatile LONG console_watch_hover_index = -1;
static RECT output_scroll_track, output_scroll_thumb;
static RECT completion_scroll_track, completion_scroll_thumb;
static int output_scroll_max, output_scroll_page;
static int completion_scroll_max, completion_scroll_page;
static volatile LONG console_scrollbar_drag_mode; /* 1 = output, 2 = autocomplete */
static LONG console_scrollbar_drag_y, console_scrollbar_drag_start;
static int completion_top, completion_header_height, completion_rows, completion_columns, completion_cell_width;
static volatile LONG is_open;
static volatile LONG console_pause_mode;
static volatile LONG console_pause_session_known;
static volatile LONG console_pause_initially_paused;
static volatile LONG console_pause_command_active;
static volatile LONG console_pause_capture_owned;
static volatile LONG console_pause_capture_release_pending;
static volatile LONG console_pause_button_pressed;
static volatile LONG console_button_pressed;
static HWND console_pause_previous_capture;
static volatile LONG present_bootstrap_request;
static volatile LONG full_console;
static volatile LONG console_legacy_layout; /* 0 = Enhanced layout, 1 = Legacy layout */
static volatile LONG console_ui_language = -1; /* -1 = follow game language, 0 = EN, 1 = RU */
static volatile LONG auto_cheats_queued;
static volatile LONG dirty = 1;
static volatile LONG ignore_tilde;
static volatile LONG console_wrap_columns = 110;
static volatile LONG console_font_cell_width = 9;
static volatile LONG console_input_left_offset = INPUT_TEXT_OFFSET;
static volatile LONG console_panel_height;
static volatile LONG virtual_mouse_x = -1, virtual_mouse_y = -1;
static volatile LONG virtual_mouse_raw_seen;
static volatile LONG virtual_mouse_relative_mode;
static volatile LONG console_capture_owned;
static HWND console_previous_capture;
static volatile LONG console_native_cursor_hidden;
static volatile LONG console_last_cursor_hidden = -1;
static volatile LONG console_last_native_cursor_kind = -1;
static volatile LONG console_status_text_width;
#define OUTPUT_TOAST_MAX_LINES 4
#define OUTPUT_TOAST_CAPTURE_MS 3500u
#define OUTPUT_TOAST_DISPLAY_MS 2600u
static char output_toast_lines[OUTPUT_TOAST_MAX_LINES][320];
static UINT output_toast_codepages[OUTPUT_TOAST_MAX_LINES];
static int output_toast_count;
static DWORD output_toast_capture_until, output_toast_expires;
static RECT output_toast_rect, output_toast_close_rect;
static volatile LONG console_cursor_kind;
static LONG console_cursor_bitmap_kind = -1;
static LONG console_cursor_texture_kind = -1;
static volatile LONG console_current_dpi = 96;
static volatile LONG console_version_line_printed;
static char console_version_status[COMPLETION_VALUE_LEN + 16];
static volatile LONG console_pending_title_click;
static DWORD console_pending_title_click_tick;
static LONG console_pending_title_click_x, console_pending_title_click_y;
static volatile LONG console_window_positioned;
static volatile LONG console_window_sized;
static volatile LONG console_saved_geometry_valid;
static LONG console_saved_left_pct, console_saved_top_pct;
static LONG console_saved_width_pct, console_saved_height_pct;
static volatile LONG console_window_left;
static volatile LONG console_window_top;
static volatile LONG console_window_width;
static volatile LONG console_window_height;
static volatile LONG console_window_drag_mode; /* 1 = move, 2 = left, 3 = right, 4 = bottom, 5/6 = bottom corners, 7 = pending title click */
static volatile LONG console_window_collapsed;
static volatile LONG console_min_restore_valid;
static volatile LONG console_min_restore_left, console_min_restore_top;
static volatile LONG console_min_restore_width, console_min_restore_height;
static volatile LONG console_header_hover_button;
static DWORD console_header_hover_tick;
static volatile LONG console_window_active;
static volatile LONG context_menu_open;
static volatile LONG context_menu_mode;
static volatile LONG context_menu_selected = -1;
static RECT context_menu_rect;
static LONG console_drag_anchor_x, console_drag_anchor_y;
static LONG console_drag_start_left, console_drag_start_top;
static LONG console_drag_start_width, console_drag_start_height;
static uint32_t *console_cursor_pixels;
static int console_cursor_hotspot_x, console_cursor_hotspot_y;
static int console_cursor_width = 32, console_cursor_height = 32;
static IDirect3DTexture9 *console_cursor_texture;
static IDirect3DDevice9 *console_cursor_texture_device;
static volatile LONG console_backbuffer_width;
static volatile LONG console_backbuffer_height;
static volatile LONG panel_draw_busy;
static volatile LONG console_cp = -1;
static char dump_name[64]; // QA: a frame with the panel on it is saved under this name

static UINT console_window_dpi(HWND h)
{
	typedef UINT (WINAPI *get_dpi_for_window_fn)(HWND);
	static get_dpi_for_window_fn get_dpi_for_window;
	static volatile LONG resolved;
	UINT dpi = 0;
	if (InterlockedCompareExchange(&resolved, 0, 0) == 0) {
		HMODULE user32 = GetModuleHandleA("user32.dll");
		if (user32)
			get_dpi_for_window = (get_dpi_for_window_fn)GetProcAddress(user32, "GetDpiForWindow");
		InterlockedExchange(&resolved, 1);
	}
	if (get_dpi_for_window && h)
		dpi = get_dpi_for_window(h);
	if (!dpi) {
		HDC dc = h ? GetDC(h) : GetDC(NULL);
		if (dc) {
			int device_dpi = GetDeviceCaps(dc, LOGPIXELSX);
			if (device_dpi >= 96 && device_dpi <= 768) dpi = (UINT)device_dpi;
			if (h) ReleaseDC(h, dc); else ReleaseDC(NULL, dc);
		}
	}
	if (dpi < 96 || dpi > 768) dpi = 96;
	return dpi;
}

static int console_dpi_logical(int physical, UINT dpi)
{
	if (physical <= 0) return 0;
	if (dpi < 96) dpi = 96;
	return (int)(((long long)physical * 96 + dpi / 2) / dpi);
}

static UINT console_codepage(void);
static void console_print_localized(const char *english, const char *russian_utf8);
static void console_print_text(const char *name);
static int console_ui_is_russian(void);
static void console_ui_text_out(HDC dc, int x, int y, const char *utf8);
static int console_ui_text_width(HDC dc, const char *utf8, SIZE *size);
static int console_text_width(HDC dc, const char *text, int length, SIZE *size);
static int line_visible_length_cp(const char *text, UINT codepage);
static size_t copy_visible_range_cp(const char *source, int from, int to, char *destination, size_t capacity, UINT codepage);
static int visible_byte_offset_cp(const char *text, int columns, UINT codepage);
static int console_header_close_hit(int x, int y);
static int console_header_pause_hit(int x, int y);
static void console_pause_button_rect(const RECT *window_rect, RECT *pause_rect);
static int console_header_min_hit(int x, int y);
static int console_header_max_hit(int x, int y);
static int console_header_layout_hit(int x, int y);
static int console_header_language_hit(int x, int y);
static int console_header_button_at(int x, int y);
static void console_header_button_activate(int button);
static void console_window_minimize_toggle(void);
static void console_window_maximize_toggle(void);
static int console_pointer_over_ui(HWND h, int x, int y);
static void console_update_pointer_cursor(HWND h, int x, int y);
static void console_init_virtual_mouse(HWND h);
static void console_update_pointer_cursor_from_os(HWND h);
static int panel_height(int h);
static int console_input_hit(int x, int y, int *index);
static int console_input_index_at(int x, int y);
static void console_window_geometry(int width, int panel_h, RECT *rect);
static void console_set_native_cursor_hidden(HWND h, int hidden);
static void console_apply_native_cursor_state(HWND h, int hidden);
static void console_load_cursor_bitmap(HCURSOR cursor);
static int console_input_line_count(const RECT *rect);
static void console_focus_changed(HWND h, int active);
static int console_context_menu_hit(int x, int y, int *index);
static void console_context_menu_open_at(HWND h, int x, int y);
static void console_context_menu_execute(int mode, int index);
static void console_load_layout(void);
static void console_save_layout(void);
static void console_save_window_geometry(void);
static void console_clear_window_geometry(void);
static void console_save_language(void);
static void console_save_pause_mode(void);
static void console_pause_mode_toggle(void);
static void input_undo_push(int group);
static void input_undo_push_snapshot(const input_edit_state_t *state, int group);
static int input_state_equal(const input_edit_state_t *a, const input_edit_state_t *b);
static void input_state_capture(input_edit_state_t *state);
static int input_undo_apply_locked(int redo);
static void input_undo_apply(int redo);
static void console_print_version_once(void);
static void console_load_qol_lists(void);
static int console_watch_count_read(void);
static int console_watch_rows(void);
static int console_bottom_reserved_height(void);
static int console_watch_chip_hit(int x, int y, int *index);
static int console_watch_unpin_hit(int x, int y, int *index);
static int console_watch_remove_at(int x, int y);
static int console_save_qol_lists(void);
static void console_output_search_step(int direction);
static void console_output_search_cancel(void);
static void run_line(char *cmd);

// ---------------------------------------------------------------- the text

/* Historical output remains one viewport; new lines use a separate toast. */

static void console_output_toast_add_locked(const char *text, size_t length, UINT codepage)
{
	DWORD now = GetTickCount();
	size_t i, out = 0;
	if (!output_toast_capture_until || (LONG)(now - output_toast_capture_until) >= 0 || scroll <= 0 || console_reflowing)
		return;
	if (length > sizeof(output_toast_lines[0]) - 1)
		length = sizeof(output_toast_lines[0]) - 1;
	if (output_toast_count >= OUTPUT_TOAST_MAX_LINES) {
		memmove(output_toast_lines, output_toast_lines + 1,
			(sizeof(output_toast_lines) / sizeof(output_toast_lines[0]) - 1) * sizeof(output_toast_lines[0]));
		memmove(output_toast_codepages, output_toast_codepages + 1,
			(sizeof(output_toast_codepages) / sizeof(output_toast_codepages[0]) - 1) * sizeof(output_toast_codepages[0]));
		output_toast_count--;
	}
	for (i = 0; i < length && out + 1 < sizeof(output_toast_lines[0]); i++) {
		if (text[i] == '^' && i + 1 < length && text[i + 1] >= '0' && text[i + 1] <= '9') {
			i++;
			continue;
		}
		if (text[i] != '\r')
			output_toast_lines[output_toast_count][out++] = text[i];
	}
	output_toast_lines[output_toast_count][out] = 0;
	output_toast_codepages[output_toast_count] = codepage ? codepage : console_codepage();
	output_toast_count++;
	output_toast_expires = now + OUTPUT_TOAST_DISPLAY_MS;
	InterlockedExchange(&dirty, 1);
}

static void console_store_line(const char *text, size_t length, UINT codepage)
{
	/* Keep the user's historical viewport anchored while new lines arrive. */
	if (scroll > 0) {
		if (scroll >= line_count)
			scroll = line_count > 0 ? line_count - 1 : 0;
		if (scroll > 0 && scroll < MAX_LINES - 1)
			scroll++;
	}
	if (length >= LINE_LEN)
		length = LINE_LEN - 1;
	if (line_count == MAX_LINES) {
		if (output_search_match_seq >= 0) {
			if (output_search_match_seq == 0)
				output_search_match_seq = -1;
			else
				output_search_match_seq--;
		}
		if (output_context_line >= 0) {
			if (output_context_line == 0)
				output_context_line = -1;
			else
				output_context_line--;
		}
	}
	memcpy(lines[line_next], text, length);
	lines[line_next][length] = 0;
	line_codepage[line_next] = codepage;
	line_next = (line_next + 1) % MAX_LINES;
	if (line_count < MAX_LINES)
		line_count++;
	console_output_toast_add_locked(text, length, codepage);
}

static int console_char_bytes(const char *text, const char *end, UINT codepage)
{
	unsigned char ch;
	if (text >= end)
		return 0;
	ch = (unsigned char)*text;
	return IsDBCSLeadByteEx(codepage, ch) && text + 1 < end ? 2 : 1;
}

static void console_store_wrapped_line(const char *begin, const char *end, int columns, UINT codepage)
{
	const char *start = begin;
	char active_color = 0;
	if (!console_reflowing) {
		int slot = source_next;
		size_t source_length = (size_t)(end - begin);
		char *copy;
		/* Dvar detail lines can be much longer than a visual output row. Keep the
		   complete logical line up to the dvar-output limit, within a bounded ring. */
		if (source_length > MAX_DVAR_STRING_OUTPUT)
			source_length = MAX_DVAR_STRING_OUTPUT;
		if (source_lines[slot]) {
			source_bytes -= source_line_lengths[slot] + 1;
			free(source_lines[slot]);
			source_lines[slot] = NULL;
			source_line_lengths[slot] = 0;
		}
		while (source_count > 0 && source_bytes > MAX_SOURCE_BUFFER_BYTES - (source_length + 1)) {
			int oldest = (source_next - source_count + MAX_LINES * 2) % MAX_LINES;
			if (source_lines[oldest]) {
				source_bytes -= source_line_lengths[oldest] + 1;
				free(source_lines[oldest]);
				source_lines[oldest] = NULL;
				source_line_lengths[oldest] = 0;
			}
			source_codepage[oldest] = 0;
			source_count--;
		}
		copy = (char *)malloc(source_length + 1);
		if (copy) {
			memcpy(copy, begin, source_length);
			copy[source_length] = 0;
			source_lines[slot] = copy;
			source_line_lengths[slot] = source_length;
			source_bytes += source_length + 1;
		}
		source_codepage[slot] = codepage;
		source_next = (source_next + 1) % MAX_LINES;
		if (source_count < MAX_LINES) source_count++;
	}
	if (start == end) {
		console_store_line("", 0, codepage);
		return;
	}
	while (start < end) {
		const char *p = start, *last_space = NULL, *cut, *next;
		int visible = 0;
		while (p < end && visible < columns) {
			if (*p == '\r') {
				p++;
				continue;
			}
			if (*p == '^' && p + 1 < end && p[1] >= '0' && p[1] <= '9') {
				p += 2;
				continue;
			}
			if (*p == ' ' || *p == '\t')
				last_space = p;
			p += console_char_bytes(p, end, codepage);
			visible++;
		}
		cut = next = p;
		if (p < end && last_space && last_space > start) {
			cut = last_space;
			next = last_space + 1;
			while (next < end && (*next == ' ' || *next == '\t'))
				next++;
		}
		if (cut == start) {
			// A long unbroken token has no word boundary; hard-wrap it instead.
			if (p == start) {
				p += console_char_bytes(p, end, codepage);
				cut = next = p;
			} else {
				cut = next = p;
			}
		}
		{
			char line[LINE_LEN];
			size_t used = 0;
			if (active_color) {
				line[used++] = '^';
				line[used++] = active_color;
			}
			for (const char *c = start; c < cut && used < LINE_LEN - 1; c++) {
				if (*c == '\r')
					continue;
				if (*c == '^' && c + 1 < cut && c[1] >= '0' && c[1] <= '9')
					active_color = c[1];
				line[used++] = *c;
			}
			console_store_line(line, used, codepage);
		}
		/* Whitespace skipped at a wrap boundary may contain a color change. */
		for (const char *c = cut; c < next; c++) {
			if (*c == '^' && c + 1 < next && c[1] >= '0' && c[1] <= '9')
				active_color = c[1];
		}
		start = next;
	}
}

/* Rebuild the visible scrollback from unwrapped output when the window width changes. */
static void console_reflow_output_locked(int columns)
{
	int old_scroll = scroll;
	output_search_match_seq = -1;
	output_context_line = -1;
	int first = (source_next - source_count + MAX_LINES * 2) % MAX_LINES;
	line_count = 0;
	line_next = 0;
	scroll = 0;
	console_reflowing = 1;
	for (int i = 0; i < source_count; i++) {
		int index = (first + i) % MAX_LINES;
		const char *text = source_lines[index];
		if (text)
			console_store_wrapped_line(text, text + source_line_lengths[index], columns,
				source_codepage[index] ? source_codepage[index] : console_codepage());
	}
	console_reflowing = 0;
	if (old_scroll > line_count - 1) old_scroll = line_count > 0 ? line_count - 1 : 0;
	scroll = old_scroll;
	output_sel_a_line = output_sel_b_line = -1;
	output_selecting = 0;
}

static void console_print_codepage(const char *text, UINT codepage)
{
	if (!text)
		return;
	EnterCriticalSection(&lock);
	{
		const char *p = text;
		int columns = (int)InterlockedCompareExchange(&console_wrap_columns, 0, 0);
		if (columns < 20) columns = 20;
		if (columns > 1500) columns = 1500;
		while (*p) {
			const char *e = p;
			while (*e && *e != '\n')
				e++;
			console_store_wrapped_line(p, e, columns, codepage);
			p = *e ? e + 1 : e;
		}
	}
	InterlockedExchange(&dirty, 1);
	LeaveCriticalSection(&lock);
}

void console_print(const char *text)
{
	console_print_codepage(text, console_codepage());
}
static void printf_line(const char *fmt, ...)
{
	char buf[LINE_LEN * 2];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	console_print(buf);
}

// ---------------------------------------------------------------- the lists (what the original console printed with cmdlist, dvarlist, dvardump and find)

// a filter matches by wildcard (* and ?) when it has one, else as a part of the name; case does not matter
static int glob_match(const char *pattern, const char *text)
{
	const char *p = pattern, *s = text;
	const char *star = NULL, *retry = NULL;
	while (*s) {
		if (*p == '?' || (*p && tolower((unsigned char)*p) == tolower((unsigned char)*s))) {
			p++;
			s++;
		} else if (*p == '*') {
			while (p[1] == '*')
				p++;
			star = p++;
			retry = s;
		} else if (star) {
			p = star + 1;
			s = ++retry;
		} else {
			return 0;
		}
	}
	while (*p == '*')
		p++;
	return *p == 0;
}

static int name_matches(const char *filter, const char *name)
{
	if (!filter || !*filter)
		return 1;
	if (strchr(filter, '*') || strchr(filter, '?'))
		return glob_match(filter, name);
	char a[96], b[96];
	int i;
	for (i = 0; filter[i] && i < 95; i++)
		a[i] = (char)tolower((unsigned char)filter[i]);
	a[i] = 0;
	for (i = 0; name[i] && i < 95; i++)
		b[i] = (char)tolower((unsigned char)name[i]);
	b[i] = 0;
	return strstr(b, a) != NULL;
}

/* Value searches use an unbounded, case-insensitive substring match by default;
   '*' and '?' opt into the same glob syntax used for dvar-name searches. */
static int completion_value_matches(const char *filter, const char *value)
{
	const char *start;
	if (!filter || !*filter || !value)
		return 0;
	if (strchr(filter, '*') || strchr(filter, '?'))
		return glob_match(filter, value);
	for (start = value; *start; start++) {
		const char *a = filter, *b = start;
		while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
			a++;
			b++;
		}
		if (!*a)
			return 1;
	}
	return 0;
}

typedef struct {
	const char *filter;
	int n, cap, truncated;
	const char **names;
} list_ctx;

typedef struct {
	const char *name;
	const dvar_t *dvar;
} dvar_list_entry;

static dvar_list_entry dvar_list_entries[16384];

static int collect_command(const char *name, void *ctx)
{
	list_ctx *l = (list_ctx *)ctx;
	if (_stricmp(name, "mw2coop_dvar_verify") == 0 || _stricmp(name, "mw2coop_sv_cheats_default") == 0 || !name_matches(l->filter, name))
		return 0;
	if (l->n >= l->cap) {
		l->truncated = 1;
		return 1;
	}
	l->names[l->n++] = name;
	return 0;
}

typedef struct {
	const char *filter;
	int count, truncated;
} dvar_list_ctx;

static int collect_dvar(dvar_t *d, void *ctx)
{
	dvar_list_ctx *list = (dvar_list_ctx *)ctx;
	if (!d || !d->name || !name_matches(list->filter, d->name))
		return 0;
	if (list->count >= (int)(sizeof(dvar_list_entries) / sizeof(dvar_list_entries[0]))) {
		list->truncated = 1;
		return 1;
	}
	dvar_list_entries[list->count].name = d->name;
	dvar_list_entries[list->count].dvar = d;
	list->count++;
	return 0;
}

static int by_name(const void *a, const void *b)
{
	return _stricmp(*(const char *const *)a, *(const char *const *)b);
}

static int by_dvar_list_entry(const void *a, const void *b)
{
	const dvar_list_entry *left = (const dvar_list_entry *)a;
	const dvar_list_entry *right = (const dvar_list_entry *)b;
	return _stricmp(left->name, right->name);
}

static const char *dvar_type_name(const dvar_t *d)
{
	static const char *const names[] = {
		"BOOL", "FLOAT", "FLOAT2", "FLOAT3", "FLOAT4", "INT", "ENUM", "STRING", "COLOR", "FLOAT3_COLOR"
	};
	return d && d->type < sizeof(names) / sizeof(names[0]) ? names[d->type] : "UNKNOWN";
}

static void dvar_flags_text(const dvar_t *d, char *buf, size_t size)
{
	static const struct { uint32_t bit; const char *name; } flags[] = {
		{DVAR_ARCHIVE_FLAG, "ARCHIVE"}, {DVAR_LATCH_FLAG, "LATCH"}, {DVAR_CHEAT_FLAG, "CHEAT"},
		{DVAR_USER_CREATED_FLAG, "USER_CREATED"}, {DVAR_SAVED_FLAG, "SAVED"},
		{DVAR_SERVERINFO_FLAG, "SERVERINFO"}, {DVAR_INIT_FLAG, "INIT"},
		{DVAR_CHANGEABLE_RESET_FLAG, "CHANGEABLE_RESET"}, {DVAR_ROM_FLAG, "ROM"},
		{DVAR_EXTERNAL_FLAG, "EXTERNAL"}, {DVAR_AUTOEXEC_FLAG, "AUTOEXEC"}
	};
	size_t used = 0;
	uint32_t known = 0;
	if (!buf || !size)
		return;
	buf[0] = 0;
	for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
		known |= flags[i].bit;
		if (!(d->flags & flags[i].bit))
			continue;
		int n = snprintf(buf + used, size > used ? size - used : 0, "%s%s", used ? " | " : "", flags[i].name);
		if (n > 0)
			used += (size_t)n < size - used ? (size_t)n : size - used;
	}
	if (d->flags & ~known) {
		int n = snprintf(buf + used, size > used ? size - used : 0, "%sUNKNOWN_0x%08x", used ? " | " : "", d->flags & ~known);
		if (n > 0)
			used += (size_t)n < size - used ? (size_t)n : size - used;
	}
	if (!used)
		snprintf(buf, size, "NONE");
}

static int dvar_values_differ(const dvar_t *d, const dvar_value_t *a, const dvar_value_t *b)
{
	if (!d || !a || !b)
		return 0;
	switch (d->type) {
	case DVAR_TYPE_BOOL: return a->enabled != b->enabled;
	case DVAR_TYPE_INT:
	case DVAR_TYPE_ENUM: return a->integer != b->integer;
	case DVAR_TYPE_FLOAT: return a->value != b->value;
	case DVAR_TYPE_FLOAT_2: return a->vector[0] != b->vector[0] || a->vector[1] != b->vector[1];
	case DVAR_TYPE_FLOAT_3:
	case DVAR_TYPE_FLOAT_3_COLOR:
		return a->vector[0] != b->vector[0] || a->vector[1] != b->vector[1] || a->vector[2] != b->vector[2];
	case DVAR_TYPE_FLOAT_4:
	case DVAR_TYPE_COLOR:
		return a->vector[0] != b->vector[0] || a->vector[1] != b->vector[1] || a->vector[2] != b->vector[2] || a->vector[3] != b->vector[3];
	case DVAR_TYPE_STRING:
		if (!a->string || !b->string) return a->string != b->string;
		return strcmp(a->string, b->string) != 0;
	default: return 0;
	}
}

static int dvar_cheats_enabled(void)
{
	dvar_t *cheats = game_find_dvar("sv_cheats");
	return cheats && cheats->type == DVAR_TYPE_BOOL && cheats->current.enabled != 0;
}

static char *dvar_value_dup(const dvar_t *d, const dvar_value_t *value)
{
	if (d && d->type == DVAR_TYPE_STRING) {
		const char *source = value->string;
		size_t length = 0;
		char *copy;
		if (!source || IsBadStringPtrA(source, 1))
			source = "";
		while (length < MAX_DVAR_STRING_OUTPUT && source[length])
			length++;
		copy = (char *)malloc(length + 1);
		if (!copy)
			return NULL;
		memcpy(copy, source, length);
		copy[length] = 0;
		return copy;
	}
	{
		char temporary[256];
		char *copy;
		size_t length;
		game_dvar_value_text(d, value, temporary, sizeof(temporary));
		length = strlen(temporary);
		copy = (char *)malloc(length + 1);
		if (copy)
			memcpy(copy, temporary, length + 1);
		return copy;
	}
}

static void print_dvar_line(const dvar_t *d, int with_default)
{
	char flags[320], meta[448];
	char *value = dvar_value_dup(d, &d->current);
	char *def = NULL, *latched = NULL, *differences = NULL, *line = NULL;
	const char *type = dvar_type_name(d);
	size_t size, meta_used = 0;
	int current_default_equal = 1, current_latched_equal = 1, default_latched_equal = 1;
	if (!value) {
		console_print("Unable to allocate dvar output buffer");
		return;
	}

	dvar_flags_text(d, flags, sizeof(flags));
	if (with_default) {
		def = dvar_value_dup(d, &d->reset);
		latched = dvar_value_dup(d, &d->latched);
		if (!def || !latched) {
			free(value);
			free(def);
			free(latched);
			console_print("Unable to allocate dvar output buffer");
			return;
		}
		current_default_equal = !dvar_values_differ(d, &d->current, &d->reset);
		current_latched_equal = !dvar_values_differ(d, &d->current, &d->latched);
		default_latched_equal = !dvar_values_differ(d, &d->reset, &d->latched);

		const char *def_display = def[0] ? def : "\"\"";
		const char *latched_display = latched[0] ? latched : "\"\"";
		size_t def_len = strlen(def_display), latched_len = strlen(latched_display);
		size_t differences_size = def_len + latched_len + 64;
		differences = (char *)malloc(differences_size);
		if (!differences) {
			free(value);
			free(def);
			free(latched);
			console_print("Unable to allocate dvar output buffer");
			return;
		}
		differences[0] = 0;
		if (!current_default_equal || !current_latched_equal) {
			if (default_latched_equal) {
				snprintf(differences, differences_size, " (default/latched: %s)", def_display);
			} else if (!current_default_equal && !current_latched_equal) {
				snprintf(differences, differences_size, " (default: %s, latched: %s)", def_display, latched_display);
			} else if (!current_default_equal) {
				snprintf(differences, differences_size, " (default: %s)", def_display);
			} else {
				snprintf(differences, differences_size, " (latched: %s)", latched_display);
			}
		}
	}

	/* Put the live value immediately after the dvar name. Type and flags are secondary metadata. */
	meta_used += (size_t)snprintf(meta + meta_used, sizeof(meta) - meta_used, "%s", type);
	if (strcmp(flags, "NONE") && meta_used < sizeof(meta))
		meta_used += (size_t)snprintf(meta + meta_used, sizeof(meta) - meta_used, " | %s", flags);
	if ((d->flags & DVAR_CHEAT_FLAG) && meta_used < sizeof(meta))
		meta_used += (size_t)snprintf(meta + meta_used, sizeof(meta) - meta_used, " | sv_cheats %s",
			dvar_cheats_enabled() ? "enabled" : "disabled");
	if ((d->flags & DVAR_LATCH_FLAG) && !current_latched_equal && meta_used < sizeof(meta))
		meta_used += (size_t)snprintf(meta + meta_used, sizeof(meta) - meta_used, " | pending restart/reinit");

	size = strlen(d->name) + strlen(value) + (differences ? strlen(differences) : 0) + strlen(meta) + 32;
	line = (char *)malloc(size);
	if (line)
		snprintf(line, size, "%s %s%s  ^3[%s]^7", d->name, value, differences ? differences : "", meta);
	if (line) {
		console_print(line);
		free(line);
	} else {
		console_print("Unable to allocate dvar output buffer");
	}
	free(value);
	free(def);
	free(latched);
	free(differences);
}

static int parse_number_list(const char *text, double *values, int count)
{
	const char *p = text;
	for (int i = 0; i < count; i++) {
		while (*p == ' ' || *p == '\t' || *p == ',') p++;
		if (!*p)
			return 0;
		char *end = NULL;
		double value = strtod(p, &end);
		if (end == p)
			return 0;
		values[i] = value;
		p = end;
	}
	while (*p == ' ' || *p == '\t' || *p == ',') p++;
	return *p == 0;
}

static int dvar_value_matches_text(const dvar_t *d, const dvar_value_t *value, const char *expected)
{
	char *end = NULL;
	if (!d || !value || !expected)
		return -1;
	switch (d->type) {
	case DVAR_TYPE_BOOL:
		if (!_stricmp(expected, "1") || !_stricmp(expected, "true")) return value->enabled != 0;
		if (!_stricmp(expected, "0") || !_stricmp(expected, "false")) return value->enabled == 0;
		return -1;
	case DVAR_TYPE_INT:
	case DVAR_TYPE_ENUM: {
		long n = strtol(expected, &end, 10);
		if (end == expected) return -1;
		while (*end == ' ' || *end == '\t') end++;
		return *end ? -1 : value->integer == (int32_t)n;
	}
	case DVAR_TYPE_FLOAT: {
		double n = strtod(expected, &end);
		if (end == expected) return -1;
		while (*end == ' ' || *end == '\t') end++;
		if (*end) return -1;
		double diff = (double)value->value - n;
		if (diff < 0) diff = -diff;
		return diff <= 0.00001 * (1.0 + (n < 0 ? -n : n));
	}
	case DVAR_TYPE_FLOAT_2:
	case DVAR_TYPE_FLOAT_3:
	case DVAR_TYPE_FLOAT_3_COLOR:
	case DVAR_TYPE_FLOAT_4:
	case DVAR_TYPE_COLOR: {
		double n[4] = {0, 0, 0, 0};
		int count = d->type == DVAR_TYPE_FLOAT_2 ? 2 :
			(d->type == DVAR_TYPE_FLOAT_3 || d->type == DVAR_TYPE_FLOAT_3_COLOR) ? 3 : 4;
		if (!parse_number_list(expected, n, count)) return -1;
		for (int i = 0; i < count; i++) {
			double diff = (double)value->vector[i] - n[i];
			if (diff < 0) diff = -diff;
			if (diff > 0.00001 * (1.0 + (n[i] < 0 ? -n[i] : n[i]))) return 0;
		}
		return 1;
	}
	case DVAR_TYPE_STRING:
		return value->string && strcmp(value->string, expected) == 0;
	default:
		return -1;
	}
}

static void print_dvar_warning(const char *name, const char *text_name)
{
	char en[LINE_LEN], ru[LINE_LEN];
	snprintf(en, sizeof(en), ctexts_get(text_name, 0), name ? name : "<unknown>");
	snprintf(ru, sizeof(ru), ctexts_get(text_name, 1), name ? name : "<unknown>");
	console_print_localized(en, ru);
}

static void print_dvar_verification(dvar_t *d, const char *expected)
{
	int current_match, latched_match;
	if (!expected) {
		print_dvar_line(d, 1);
		return;
	}
	current_match = dvar_value_matches_text(d, &d->current, expected);
	latched_match = dvar_value_matches_text(d, &d->latched, expected);
	if (current_match == 1)
		return;
	if (latched_match == 1 && (d->flags & DVAR_LATCH_FLAG)) {
		print_dvar_warning(d->name, "CON_WARN_PENDING");
	} else if ((d->flags & DVAR_CHEAT_FLAG) && !dvar_cheats_enabled()) {
		print_dvar_warning(d->name, "CON_WARN_CHEATS_OFF");
	} else if (d->flags & DVAR_ROM_FLAG) {
		print_dvar_warning(d->name, "CON_WARN_READ_ONLY");
	} else if (d->flags & DVAR_INIT_FLAG) {
		print_dvar_warning(d->name, "CON_WARN_INIT_ONLY");
	} else if (current_match < 0 || latched_match < 0) {
		print_dvar_warning(d->name, "CON_WARN_VERIFY_FAILED");
	} else {
		print_dvar_warning(d->name, "CON_WARN_REJECTED");
	}
}

// The name (console.str) of the word "command" for <count>, in the plural form of the language
static const char *command_noun_name(int count, int russian)
{
	if (russian) {
		int last_two = count % 100;
		int last_one = count % 10;
		if (last_two >= 11 && last_two <= 14) return "CON_NOUN_COMMAND_MANY";
		if (last_one == 1) return "CON_NOUN_COMMAND_ONE";
		if (last_one >= 2 && last_one <= 4) return "CON_NOUN_COMMAND_FEW";
		return "CON_NOUN_COMMAND_MANY";
	}
	return count == 1 ? "CON_NOUN_COMMAND_ONE" : "CON_NOUN_COMMAND_MANY";
}

static void format_command_count(char *out, size_t capacity, int count, int russian)
{
	if (!out || capacity == 0) return;
	if (count < 0) count = 0;
	snprintf(out, capacity, "%d %s", count, ctexts_get(command_noun_name(count, russian), russian));
}

static void list_commands(const char *filter)
{
	static const char *names[4096];
	list_ctx l = {filter, 0, 4096, 0, names};
	game_for_each_command(collect_command, &l);
	qsort(names, (size_t)l.n, sizeof(names[0]), by_name);
	for (int i = 0; i < l.n; i++)
		console_print(names[i]);
	{
		int ru = console_ui_is_russian();
		char count_label[48];
		format_command_count(count_label, sizeof(count_label), l.n, ru);
		printf_line("%s%s", count_label, l.truncated ? ctexts_get("CON_LIST_TRUNCATED", ru) : "");
	}
}

static void list_dvars(const char *filter, int with_default)
{
	dvar_list_ctx list = {filter, 0, 0};
	game_for_each_dvar(collect_dvar, &list);
	qsort(dvar_list_entries, (size_t)list.count, sizeof(dvar_list_entries[0]), by_dvar_list_entry);
	for (int i = 0; i < list.count; i++)
		print_dvar_line(dvar_list_entries[i].dvar, with_default);
	printf_line("%d dvars%s  (types: BOOL/FLOAT/FLOAT2/FLOAT3/FLOAT4/INT/ENUM/STRING/COLOR/FLOAT3_COLOR; CHEAT=0x4, LATCH=0x2, INIT=0x800, ROM=0x2000)",
		   list.count, list.truncated ? " (truncated at 16384)" : "");
}

static void find_all(const char *text)
{
	if (!*text) {
		console_print_text("CON_FIND_USAGE");
		return;
	}
	list_commands(text);
	list_dvars(text, 0);
}

static void dump_console(const char *name)
{
	char path[MAX_PATH], safe_name[64];
	DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
	if (!n || n >= MAX_PATH) {
		console_print("^1condump: cannot resolve the game folder");
		return;
	}
	while (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/')
		n--;
	if (!n) {
		console_print("^1condump: cannot resolve the game folder");
		return;
	}
	path[n] = 0;
	const char *source_name = name ? name : "";
	size_t used = 0;
	while (*source_name && used < sizeof(safe_name) - 1) {
		unsigned char ch = (unsigned char)*source_name++;
		if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
			(ch >= '0' && ch <= '9') || ch == '_' || ch == '-')
			safe_name[used++] = (char)ch;
		else if (ch == ' ' || ch == '.')
			safe_name[used++] = '_';
	}
	if (!used)
		memcpy(safe_name, "console_dump", used = sizeof("console_dump") - 1);
	safe_name[used] = 0;
	char file[96];
	int file_len = snprintf(file, sizeof(file), "mw2coop\\%s.txt", safe_name);
	if (file_len < 0 || (size_t)file_len >= sizeof(file) || (size_t)n + (size_t)file_len >= sizeof(path)) {
		console_print("^1condump: output path is too long");
		return;
	}
	memcpy(path + n, file, (size_t)file_len + 1);
	int count;
	char *snapshot;
	EnterCriticalSection(&lock);
	count = line_count;
	snapshot = count ? (char *)malloc((size_t)count * LINE_LEN) : NULL;
	if (snapshot) {
		for (int i = 0; i < count; i++) {
			int idx = (line_next - count + i + MAX_LINES * 2) % MAX_LINES;
			memcpy(snapshot + (size_t)i * LINE_LEN, lines[idx], LINE_LEN);
		}
	}
	LeaveCriticalSection(&lock);
	if (count && !snapshot) {
		console_print("^1condump: not enough memory to save the output");
		return;
	}
	FILE *f = fopen(path, "w");
	if (!f) {
		free(snapshot);
		printf_line("^1condump: cannot write %s", file);
		return;
	}
	int ok = 1;
	for (int i = 0; i < count; i++) {
		const char *line = snapshot + (size_t)i * LINE_LEN;
		size_t length = strlen(line);
		if (fwrite(line, 1, length, f) != length || fwrite("\n", 1, 1, f) != 1) {
			ok = 0;
			break;
		}
	}
	free(snapshot);
	if (fclose(f) != 0)
		ok = 0;
	if (!ok) {
		printf_line("^1condump: failed while writing %s", file);
		return;
	}
	printf_line("console written to %s", file);
}

// Tab completion uses a short-lived registry-name snapshot. Rewalking all engine hash
// buckets on every keystroke was unnecessary; only values for matching dvars are refreshed.
typedef struct {
	const char *filter;
	size_t len;
	int wildcard;
	int value_search;
	int n, stored;
	char common[96];
} comp_ctx;

static void hide_completion(void)
{
	completion_count = completion_total = completion_scroll = 0;
	completion_selected = -1;
	completion_rows = completion_columns = completion_cell_width = 0;
	SetRectEmpty(&completion_rect);
	for (int i = 0; i < MAX_COMPLETIONS; i++) SetRectEmpty(&completion_pin_rects[i]);
}

static void completion_catalog_add(const char *name, int is_dvar, dvar_t *dvar)
{
	completion_source_t *item;
	if (!name || !*name)
		return;
	if (completion_catalog_count >= MAX_COMPLETION_CATALOG) {
		completion_catalog_truncated = 1;
		return;
	}
	item = &completion_catalog[completion_catalog_count++];
	lstrcpynA(item->name, name, sizeof(item->name));
	item->dvar = dvar;
	item->is_dvar = (uint8_t)!!is_dvar;
}

static int completion_catalog_command(const char *name, void *unused)
{
	(void)unused;
	if (_stricmp(name, "mw2coop_dvar_verify") != 0 && _stricmp(name, "mw2coop_sv_cheats_default") != 0)
		completion_catalog_add(name, 0, NULL);
	return 0;
}

static int completion_catalog_dvar(dvar_t *d, void *unused)
{
	(void)unused;
	if (d && d->name)
		completion_catalog_add(d->name, 1, d);
	return 0;
}

static int by_completion_source(const void *a, const void *b)
{
	const completion_source_t *left = (const completion_source_t *)a;
	const completion_source_t *right = (const completion_source_t *)b;
	return _stricmp(left->name, right->name);
}

static void rebuild_completion_catalog(void)
{
	static const char *const own[] = {
		"cmdlist", "commands", "dvarlist", "dvardump", "find", "echo", "condump", "clear", "cls", "help",
		"watch", "unwatch", "watchlist", "dvarcompare"
	};
	int unique = 0;
	completion_catalog_count = 0;
	completion_catalog_truncated = 0;
	for (size_t i = 0; i < sizeof(own) / sizeof(own[0]); i++)
		completion_catalog_add(own[i], 0, NULL);
	game_for_each_command(completion_catalog_command, NULL);
	game_for_each_dvar(completion_catalog_dvar, NULL);
	qsort(completion_catalog, (size_t)completion_catalog_count, sizeof(completion_catalog[0]), by_completion_source);
	for (int i = 0; i < completion_catalog_count; i++) {
		if (unique && !_stricmp(completion_catalog[unique - 1].name, completion_catalog[i].name)) {
			if (completion_catalog[i].is_dvar && !completion_catalog[unique - 1].is_dvar)
				completion_catalog[unique - 1] = completion_catalog[i];
			continue;
		}
		if (unique != i)
			completion_catalog[unique] = completion_catalog[i];
		unique++;
	}
	completion_catalog_count = unique;
	completion_catalog_tick = GetTickCount();
	completion_catalog_ready = 1;
}

static void completion_add_match(comp_ctx *c, const completion_source_t *source)
{
	completion_item_t *item;
	const char *name = source->name;
	if (c->value_search) {
		char current[COMPLETION_VALUE_LEN];
		if (!source->is_dvar || !source->dvar)
			return;
		game_dvar_value_text(source->dvar, &source->dvar->current, current, sizeof(current));
		if (!completion_value_matches(c->filter, current))
			return;
	} else if (c->wildcard ? !name_matches(c->filter, name) : _strnicmp(name, c->filter, c->len) != 0) {
		return;
	}
	if (c->n == 0) {
		lstrcpynA(c->common, name, sizeof(c->common));
	} else {
		size_t k = 0;
		while (c->common[k] && name[k] && tolower((unsigned char)c->common[k]) == tolower((unsigned char)name[k]))
			k++;
		c->common[k] = 0;
	}
	if (c->stored < MAX_COMPLETIONS) {
		item = &completion_items[c->stored++];
		memset(item, 0, sizeof(*item));
		lstrcpynA(item->name, name, sizeof(item->name));
		item->is_dvar = source->is_dvar;
		if (source->is_dvar && source->dvar) {
			game_dvar_value_text(source->dvar, &source->dvar->current, item->value, sizeof(item->value));
			game_dvar_value_text(source->dvar, &source->dvar->reset, item->default_value, sizeof(item->default_value));
		}
	}
	c->n++;
}

/* Called with lock held by the input/window thread; never from the renderer. */
static void refresh_completions(int apply_common)
{
	char word[INPUT_LEN + 1];
	char *value_filter = NULL;
	int value_search = 0;
	int n = 0;
	comp_ctx c;
	if (output_search_mode) {
		hide_completion();
		return;
	}

	/* `*=%text%` searches the current value of every registered dvar. Keep the
	   complete text after `*=` so vector/color values containing spaces work too. */
	if (input_len >= 2 && input[0] == '*' && input[1] == '=') {
		int used = input_len;
		value_search = 1;
		if (used > INPUT_LEN) used = INPUT_LEN;
		memcpy(word, input, (size_t)used);
		word[used] = 0;
		value_filter = word + 2;
		while (*value_filter == ' ' || *value_filter == '\t') value_filter++;
		if (*value_filter == '%') value_filter++;
		while (used > 2 && (word[used - 1] == ' ' || word[used - 1] == '\t'))
			word[--used] = 0;
		if (used > 2 && word[used - 1] == '%')
			word[--used] = 0;
		/* Re-trim after removing the optional closing percent delimiter. */
		while (used > 2 && (word[used - 1] == ' ' || word[used - 1] == '\t'))
			word[--used] = 0;
		if (value_filter != word + 2)
			memmove(word + 2, value_filter, strlen(value_filter) + 1);
		value_filter = word + 2;
		if (!*value_filter || cursor < 2) {
			hide_completion();
			return;
		}
		n = (int)strlen(word);
	} else {
		while (n < input_len && input[n] != ' ' && n < (int)sizeof(word) - 1)
			word[n] = input[n], n++;
		word[n] = 0;
		if (!n || cursor > n) {
			hide_completion();
			return;
		}
	}

	if (!completion_catalog_ready || (DWORD)(GetTickCount() - completion_catalog_tick) >= COMPLETION_CATALOG_TTL_MS)
		rebuild_completion_catalog();

	memset(&c, 0, sizeof(c));
	c.filter = value_search ? value_filter : word;
	c.len = strlen(c.filter);
	c.value_search = value_search;
	c.wildcard = value_search || strchr(word, '*') != NULL || strchr(word, '?') != NULL;
	if (value_search || c.wildcard) {
		/* Value matching and wildcard name matching both require a full cached scan. */
		for (int i = 0; i < completion_catalog_count; i++)
			completion_add_match(&c, &completion_catalog[i]);
	} else {
		/* The catalogue is sorted; find the first possible prefix match in O(log N). */
		int low = 0, high = completion_catalog_count;
		while (low < high) {
			int mid = low + (high - low) / 2;
			if (_strnicmp(completion_catalog[mid].name, word, (size_t)n) < 0)
				low = mid + 1;
			else
				high = mid;
		}
		for (int i = low; i < completion_catalog_count; i++) {
			if (_strnicmp(completion_catalog[i].name, word, (size_t)n) != 0)
				break;
			completion_add_match(&c, &completion_catalog[i]);
		}
	}

	completion_total = c.n;
	completion_count = c.stored;
	completion_scroll = 0;
	completion_selected = -1;
	completion_rows = completion_columns = completion_cell_width = 0;
	SetRectEmpty(&completion_rect);
	if (!c.n)
		return;

	if (value_search) {
		/* Do not rewrite the search expression with a common dvar-name prefix. */
		if (completion_count > 0) completion_selected = 0;
	} else if (apply_common) {
		int add = (int)strlen(c.common);
		/* A wildcard query is a search, not a partially typed prefix. Do not replace it
		   with a common prefix when several names match; a unique match may be accepted. */
		if ((!c.wildcard || c.n == 1) && add > 0 && input_len - n + add <= INPUT_LEN) {
			memmove(input + add, input + n, (size_t)(input_len - n + 1));
			memcpy(input, c.common, (size_t)add);
			input_len += add - n;
			cursor = add;
			selection_anchor = -1;
			if (c.n == 1 && input[add] != ' ' && input_len < INPUT_LEN) {
				memmove(input + add + 1, input + add, (size_t)(input_len - add + 1));
				input[add] = ' ';
				input_len++;
				cursor = add + 1;
				hide_completion();
				return;
			}
		}
		if (completion_count > 1 || (c.wildcard && completion_count == 1))
			completion_selected = 0;
	}
}
static void tab_complete(void)
{
	input_edit_state_t before, after;
	input_state_capture(&before);
	refresh_completions(1);
	input_state_capture(&after);
	if (!input_state_equal(&before, &after))
		input_undo_push_snapshot(&before, 0);
}

static int completion_fit_value(HDC dc, const char *source, int max_width, char *out, size_t capacity, SIZE *result)
{
	char original[COMPLETION_VALUE_LEN];
	int offsets[COMPLETION_VALUE_LEN];
	const UINT codepage = console_codepage();
	const char *p, *end;
	size_t used = 0;
	int visible = 0;
	SIZE size = {0, 0};
	if (!out || capacity < 4)
		return 0;

	/* Suggestions are single-line text: remove CoD colours and normalize controls first. */
	for (p = source ? source : "", end = p + strlen(p); p < end && used < sizeof(original) - 1;) {
		int bytes;
		unsigned char ch;
		if (*p == '^' && p + 1 < end && p[1] >= '0' && p[1] <= '9') {
			p += 2;
			continue;
		}
		bytes = console_char_bytes(p, end, codepage);
		ch = (unsigned char)*p;
		if (bytes == 1 && (ch == '\r' || ch == '\n' || ch == '\t')) {
			original[used++] = ' ';
		} else if (bytes == 1 && (ch < 32 || ch == 127)) {
			/* Ignore non-printing control bytes in a one-line value preview. */
		} else {
			if (used + (size_t)bytes >= sizeof(original))
				break;
			memcpy(original + used, p, (size_t)bytes);
			used += (size_t)bytes;
		}
		p += bytes;
	}
	original[used] = 0;
	end = original + used;
	offsets[0] = 0;
	for (p = original; p < end;) {
		p += console_char_bytes(p, end, codepage);
		if (visible < COMPLETION_VALUE_LEN - 1)
			offsets[++visible] = (int)(p - original);
	}
	if (!visible) {
		out[0] = 0;
		if (result) result->cx = result->cy = 0;
		return 0;
	}

	/* Prefer the full value; otherwise binary-search the longest prefix plus an ellipsis. */
	if (console_text_width(dc, original, (int)used, &size) && size.cx <= max_width) {
		lstrcpynA(out, original, (int)capacity);
		if (result) *result = size;
		return (int)strlen(out);
	}
	{
		int low = 0, high = visible - 1;
		int best = -1;
		while (low <= high) {
			int mid = low + (high - low) / 2;
			int bytes = offsets[mid];
			char candidate[COMPLETION_VALUE_LEN];
			if ((size_t)bytes + 4 > sizeof(candidate)) {
				high = mid - 1;
				continue;
			}
			memcpy(candidate, original, (size_t)bytes);
			memcpy(candidate + bytes, "...", 4);
			if (console_text_width(dc, candidate, bytes + 3, &size) && size.cx <= max_width) {
				best = mid;
				low = mid + 1;
			} else {
				high = mid - 1;
			}
		}
		if (best < 0) {
			out[0] = 0;
			if (result) result->cx = result->cy = 0;
			return 0;
		}
		{
			int bytes = offsets[best];
			if ((size_t)bytes + 4 > capacity)
				bytes = (int)capacity - 4;
			memcpy(out, original, (size_t)bytes);
			memcpy(out + bytes, "...", 4);
			console_text_width(dc, out, bytes + 3, &size);
			if (result) *result = size;
			return bytes + 3;
		}
	}
}

static void input_state_capture(input_edit_state_t *state)
{
	if (!state) return;
	memcpy(state->text, input, sizeof(state->text));
	state->length = input_len;
	state->cursor = cursor;
	state->selection_anchor = selection_anchor;
}

static int input_state_equal(const input_edit_state_t *a, const input_edit_state_t *b)
{
	return a && b && a->length == b->length && a->cursor == b->cursor &&
		a->selection_anchor == b->selection_anchor &&
		memcmp(a->text, b->text, sizeof(a->text)) == 0;
}

static void input_undo_push_snapshot(const input_edit_state_t *state, int group)
{
	DWORD now = GetTickCount();
	if (!state || output_search_mode) return;
	if (group && input_undo_last_group == group &&
		(DWORD)(now - input_undo_last_tick) <= 850u) {
		input_undo_last_tick = now;
		return;
	}
	if (input_undo_count >= INPUT_UNDO_DEPTH) {
		memmove(input_undo_stack, input_undo_stack + 1,
			(INPUT_UNDO_DEPTH - 1) * sizeof(input_undo_stack[0]));
		input_undo_count = INPUT_UNDO_DEPTH - 1;
	}
	input_undo_stack[input_undo_count++] = *state;
	input_redo_count = 0;
	input_undo_last_group = group;
	input_undo_last_tick = now;
}

static void input_undo_push(int group)
{
	input_edit_state_t state;
	input_state_capture(&state);
	input_undo_push_snapshot(&state, group);
}

static int input_undo_apply_locked(int redo)
{
	input_edit_state_t current, next;
	if (output_search_mode) return 0;
	input_edit_state_t *source = redo ? input_redo_stack : input_undo_stack;
	input_edit_state_t *destination = redo ? input_undo_stack : input_redo_stack;
	int *source_count = redo ? &input_redo_count : &input_undo_count;
	int *destination_count = redo ? &input_undo_count : &input_redo_count;
	if (*source_count <= 0) return 0;
	input_state_capture(&current);
	if (*destination_count >= INPUT_UNDO_DEPTH) {
		memmove(destination, destination + 1,
			(INPUT_UNDO_DEPTH - 1) * sizeof(destination[0]));
		*destination_count = INPUT_UNDO_DEPTH - 1;
	}
	destination[(*destination_count)++] = current;
	next = source[--(*source_count)];
	memcpy(input, next.text, sizeof(input));
	input[INPUT_LEN] = 0;
	input_len = next.length;
	if (input_len < 0) input_len = 0;
	if (input_len > INPUT_LEN) input_len = INPUT_LEN;
	cursor = next.cursor;
	if (cursor < 0) cursor = 0;
	if (cursor > input_len) cursor = input_len;
	selection_anchor = next.selection_anchor;
	if (selection_anchor < -1) selection_anchor = -1;
	if (selection_anchor > input_len) selection_anchor = input_len;
	input_undo_last_group = 0;
	input_undo_last_tick = 0;
	refresh_completions(0);
	InterlockedExchange(&dirty, 1);
	return 1;
}

static void input_undo_apply(int redo)
{
	EnterCriticalSection(&lock);
	InterlockedExchange(&context_menu_open, 0);
	InterlockedExchange(&context_menu_selected, -1);
	(void)input_undo_apply_locked(redo);
	LeaveCriticalSection(&lock);
}

static void completion_choose(int index)
{
	if (index < 0 || index >= completion_count)
		return;
	input_undo_push(0);
	lstrcpynA(input, completion_items[index].name, sizeof(input));
	input_len = cursor = (int)strlen(input);
	if (input_len < INPUT_LEN) {
		input[input_len++] = ' ';
		input[input_len] = 0;
		cursor = input_len;
	}
	selection_anchor = -1;
	hide_completion();
	InterlockedExchange(&dirty, 1);
}

// ---------------------------------------------------------------- the commands

// sv_cheats is ROM/INIT-protected in this build; only this BOOL dvar gets the dedicated override.
static int set_sv_cheats_value(dvar_t *d, int value)
{
	LONG old_flags;
	int changed;
	if (!d || d->type != DVAR_TYPE_BOOL)
		return 0;
	old_flags = InterlockedAnd((volatile LONG *)&d->flags, ~(LONG)(CONSOLE_DVAR_INIT_FLAG | DVAR_ROM_FLAG));
	changed = d->current.enabled != !!value || d->reset.enabled != !!value || d->latched.enabled != !!value;
	d->reset.enabled = (uint8_t)!!value;
	d->latched.enabled = (uint8_t)!!value;
	d->current.enabled = (uint8_t)!!value;
	if (changed || (old_flags & (LONG)CONSOLE_DVAR_INIT_FLAG))
		d->modified = 1;
	return 1;
}

static void sv_cheats_f(void)
{
	dvar_t *d = game_find_dvar("sv_cheats");
	const char *arg = game_cmd_argv(1);
	const char *extra = game_cmd_argv(2);
	if (!d) {
		console_print_text("CON_SVCHEATS_NOT_REGISTERED");
		return;
	}
	if (d->type != DVAR_TYPE_BOOL) {
		console_print_text("CON_SVCHEATS_BAD_TYPE");
		return;
	}
	if (!arg || !*arg) {
		print_dvar_line(d, 1);
		return;
	}
	if ((strcmp(arg, "0") && strcmp(arg, "1")) || (extra && *extra)) {
		console_print_text("CON_SVCHEATS_USAGE");
		return;
	}
	{
		int wanted = arg[0] == '1';
		if (!set_sv_cheats_value(d, wanted) || d->current.enabled != wanted ||
			d->reset.enabled != wanted || d->latched.enabled != wanted) {
			console_print_text("CON_SVCHEATS_REJECTED");
		}
		InterlockedExchange(&auto_cheats_queued, AUTO_CHEATS_USER_OVERRIDE);
		log_printf("console: sv_cheats update %s (flags=0x%08x)",
			d->current.enabled == wanted && d->reset.enabled == wanted && d->latched.enabled == wanted ? "verified" : "not verified",
			d->flags);
	}
}

static void sv_cheats_default_f(void)
{
	dvar_t *d;
	LONG state = InterlockedCompareExchange(&auto_cheats_queued, 0, 0);
	/* An explicit user command always takes precedence over startup retries. */
	if (state == AUTO_CHEATS_USER_OVERRIDE)
		return;
	d = game_find_dvar("sv_cheats");
	if (!set_sv_cheats_value(d, 1) || !d->current.enabled || !d->reset.enabled || !d->latched.enabled) {
		InterlockedCompareExchange(&auto_cheats_queued, 0, AUTO_CHEATS_PENDING);
		return;
	}
	InterlockedExchange(&auto_cheats_queued, AUTO_CHEATS_APPLIED);
	/* No output or success claim: the game can recreate the dvar after startup. */
}

static cmd_function_t sv_cheats_cmd = {NULL, "sv_cheats", NULL, NULL, sv_cheats_f CMD_NODE_FLAG};
static cmd_function_t sv_cheats_default_cmd = {NULL, "mw2coop_sv_cheats_default", NULL, NULL, sv_cheats_default_f CMD_NODE_FLAG};
// The dvar write is processed by the engine before this command in the same command
// buffer. Read back the real current/latched values instead of claiming that a queued
// request was accepted.
static void dvar_verify_f(void)
{
	const char *name = game_cmd_argv(1);
	dvar_t *d = name && *name ? game_find_dvar(name) : NULL;
	char expected[INPUT_LEN + 1];
	int argc = game_cmd_argc(), used = 0;
	if (!d) {
		print_dvar_warning(name && *name ? name : "<missing name>", "CON_WARN_NOT_REGISTERED");
		return;
	}
	expected[0] = 0;
	for (int i = 2; i < argc && used < INPUT_LEN; i++) {
		const char *arg = game_cmd_argv(i);
		int n = (int)strlen(arg);
		if (used && used < INPUT_LEN) expected[used++] = ' ';
		if (n > INPUT_LEN - used) n = INPUT_LEN - used;
		if (n > 0) { memcpy(expected + used, arg, (size_t)n); used += n; }
	}
	expected[used] = 0;
	print_dvar_verification(d, argc > 2 ? expected : NULL);
}

static cmd_function_t dvar_verify_cmd = {NULL, "mw2coop_dvar_verify", NULL, NULL, dvar_verify_f CMD_NODE_FLAG};

static int console_name_in_list(char (*names)[COMPLETION_NAME_LEN], int count, const char *name)
{
	for (int i = 0; i < count; i++)
		if (!_stricmp(names[i], name)) return i;
	return -1;
}

static void console_watch_print(void)
{
	char (*names)[COMPLETION_NAME_LEN] = NULL;
	int count;
	EnterCriticalSection(&lock);
	count = console_watch_count_read();
	if (count > 0) {
		names = (char (*)[COMPLETION_NAME_LEN])malloc((size_t)count * sizeof(*names));
		if (names) memcpy(names, console_watch_names, (size_t)count * sizeof(*names));
	}
	LeaveCriticalSection(&lock);
	if (!count) {
		console_print_text("CON_FAV_NONE");
		return;
	}
	if (!names) {
		console_print_text("CON_FAV_ALLOC_FAILED");
		return;
	}
	console_print_text("CON_FAV_HEADER");
	for (int i = 0; i < count; i++) {
		char value[COMPLETION_VALUE_LEN];
		game_dvar_string(names[i], value, sizeof(value));
		printf_line("%s = ^2%s", names[i], value);
	}
	free(names);
}

static int console_watch_reserve_locked(int needed)
{
	size_t next;
	char (*grown)[COMPLETION_NAME_LEN];
	if (needed <= 0) return 1;
	if ((size_t)needed <= console_watch_capacity) return 1;
	next = console_watch_capacity ? console_watch_capacity : 8;
	while (next < (size_t)needed) {
		if (next > (size_t)-1 / 2) return 0;
		next *= 2;
	}
	if (next > (size_t)-1 / sizeof(*console_watch_names)) return 0;
	grown = (char (*)[COMPLETION_NAME_LEN])realloc(console_watch_names, next * sizeof(*console_watch_names));
	if (!grown) return 0;
	memset(grown + console_watch_capacity, 0,
		(next - console_watch_capacity) * sizeof(*console_watch_names));
	console_watch_names = grown;
	console_watch_capacity = next;
	return 1;
}

static int console_watch_change(const char *name, int remove)
{
	int index, count;
	if (!name || !*name || strlen(name) >= COMPLETION_NAME_LEN)
		return 0;
	if (!remove && !game_find_dvar(name)) {
		printf_line("^1Cannot pin %s:^7 dvar is not registered", name);
		return 0;
	}
	EnterCriticalSection(&lock);
	count = console_watch_count_read();
	index = console_name_in_list(console_watch_names, count, name);
	if (remove) {
		if (index >= 0) {
			for (int i = index; i + 1 < count; i++)
				memcpy(console_watch_names[i], console_watch_names[i + 1], sizeof(console_watch_names[i]));
			if (count > 0) console_watch_names[count - 1][0] = 0;
			InterlockedExchange(&console_watch_count, count - 1);
			LeaveCriticalSection(&lock);
			InterlockedExchange(&console_watch_hover_index, -1);
			console_save_qol_lists();
			InterlockedExchange(&dirty, 1);
			return 1;
		}
		LeaveCriticalSection(&lock);
		return 0;
	}
	if (index >= 0) {
		LeaveCriticalSection(&lock);
		return 1;
	}
	if (count == 0x7fffffff || !console_watch_reserve_locked(count + 1)) {
		LeaveCriticalSection(&lock);
		console_print_text("CON_FAV_GROW_FAILED");
		return 0;
	}
	lstrcpynA(console_watch_names[count], name, COMPLETION_NAME_LEN);
	InterlockedExchange(&console_watch_count, count + 1);
	LeaveCriticalSection(&lock);
	InterlockedExchange(&console_watch_hover_index, -1);
	console_save_qol_lists();
	InterlockedExchange(&dirty, 1);
	return 1;
}


static void console_dvar_compare(const char *name)
{
	dvar_t *d = name && *name ? game_find_dvar(name) : NULL;
	char *current, *def, *latched;
	if (!d) {
		console_print_text("CON_DVARCOMPARE_USAGE");
		return;
	}
	current = dvar_value_dup(d, &d->current);
	def = dvar_value_dup(d, &d->reset);
	latched = dvar_value_dup(d, &d->latched);
	if (!current || !def || !latched) {
		free(current); free(def); free(latched);
		console_print("Unable to allocate dvar comparison buffer");
		return;
	}
	printf_line("^5%s^7 (%s)", d->name, dvar_type_name(d));
	printf_line("^2CURRENT^7  %s", current);
	printf_line("%s DEFAULT^7  %s", dvar_values_differ(d, &d->current, &d->reset) ? "^3DIFF" : "^8SAME", def);
	printf_line("%s LATCHED^7  %s", dvar_values_differ(d, &d->current, &d->latched) ? "^3DIFF" : "^8SAME", latched);
	free(current); free(def); free(latched);
}

static void run_line(char *cmd)
{
	while (*cmd == ' ')
		cmd++;
	char *end = cmd + strlen(cmd);
	while (end > cmd && end[-1] == ' ')
		*--end = 0;
	if (!*cmd)
		return;
	EnterCriticalSection(&lock);
	if (scroll > 0 && line_count > 0) {
		output_toast_count = 0;
		output_toast_capture_until = GetTickCount() + OUTPUT_TOAST_CAPTURE_MS;
		output_toast_expires = 0;
		SetRectEmpty(&output_toast_rect);
		SetRectEmpty(&output_toast_close_rect);
	} else {
		output_toast_capture_until = 0;
		output_toast_count = 0;
		SetRectEmpty(&output_toast_rect);
		SetRectEmpty(&output_toast_close_rect);
	}
	LeaveCriticalSection(&lock);
	printf_line("^3] ^7%s", cmd);
	EnterCriticalSection(&lock);
	if (history_count == 0 || strcmp(history[(history_count - 1) % HISTORY], cmd))
		lstrcpynA(history[history_count++ % HISTORY], cmd, INPUT_LEN + 1);
	history_pos = history_count;
	history_draft_valid = 0;
	LeaveCriticalSection(&lock);
	if (!_stricmp(cmd, "clear") || !_stricmp(cmd, "cls")) {
		EnterCriticalSection(&lock);
		line_count = line_next = 0;
		for (int i = 0; i < MAX_LINES; i++) {
			free(source_lines[i]);
			source_lines[i] = NULL;
			source_line_lengths[i] = 0;
			source_codepage[i] = 0;
		}
		source_bytes = 0;
		source_count = source_next = 0;
		scroll = 0;
		output_search_match_seq = -1;
		output_context_line = -1;
		output_sel_a_line = output_sel_b_line = -1;
		output_selecting = 0;
		output_toast_count = 0;
		output_toast_capture_until = output_toast_expires = 0;
		SetRectEmpty(&output_toast_rect);
		SetRectEmpty(&output_toast_close_rect);
		LeaveCriticalSection(&lock);
		return;
	}
	if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
		console_print_text("CON_HELP_01");
		console_print_text("CON_HELP_02");
		console_print_text("CON_HELP_03");
		console_print_text("CON_HELP_04");
		console_print_text("CON_HELP_05");
		console_print_text("CON_HELP_06");
		console_print_text("CON_HELP_07");
		console_print_text("CON_HELP_08");
		console_print_text("CON_HELP_09");
		console_print_text("CON_HELP_10");
		console_print_text("CON_HELP_11");
		console_print_text("CON_HELP_12");
		console_print_text("CON_HELP_13");
		console_print_text("CON_HELP_14");
		console_print_text("CON_HELP_15");
		console_print_text("CON_HELP_16");
		console_print_text("CON_HELP_17");
		console_print_text("CON_HELP_18");
		console_print_text("CON_HELP_19");
		console_print_text("CON_HELP_20");
		console_print_text("CON_HELP_21");
		console_print_text("CON_HELP_22");
		return;
	}
	char name[64];
	int n = 0;
	while (cmd[n] && cmd[n] != ' ' && n < (int)sizeof(name) - 1)
		name[n] = cmd[n], n++;
	name[n] = 0;
	const char *rest = cmd + n;
	while (*rest == ' ')
		rest++;
	// the console's own commands (the game's versions print through a function this build no longer has)
	if (!_stricmp(name, "cmdlist") || !_stricmp(name, "commands")) {
		list_commands(rest);
		return;
	}
	if (!_stricmp(name, "dvarlist")) {
		list_dvars(rest, 0);
		return;
	}
	if (!_stricmp(name, "dvardump")) {
		list_dvars(rest, 1);
		return;
	}
	if (!_stricmp(name, "find")) {
		find_all(rest);
		return;
	}
	if (!_stricmp(name, "echo")) {
		console_print(rest);
		return;
	}
	if (!_stricmp(name, "condump")) {
		dump_console(rest);
		return;
	}
	if (!_stricmp(name, "watch")) {
		(void)console_watch_change(rest, 0);
		return;
	}
	if (!_stricmp(name, "unwatch")) {
		(void)console_watch_change(rest, 1);
		return;
	}
	if (!_stricmp(name, "watchlist")) {
		console_watch_print();
		return;
	}
	if (!_stricmp(name, "dvarcompare")) {
		console_dvar_compare(rest);
		return;
	}
	if (game_find_dvar(name) && !game_command_exists(name)) {
		if (!*rest) {
			print_dvar_line(game_find_dvar(name), 1);
			return;
		}
		char set[INPUT_LEN * 2 + 128];
		snprintf(set, sizeof(set), "seta %s %s\nmw2coop_dvar_verify %s %s\n", name, rest, name, rest);
		game_cbuf_addtext(set);
		return;
	}
	if (!game_command_exists(name) && strcmp(name, "set") && strcmp(name, "seta") && strcmp(name, "exec") && strcmp(name, "wait"))
		printf_line("^1unknown command or dvar \"%s\" (sent to the game anyway)", name);
	char cb[INPUT_LEN + 2];
	snprintf(cb, sizeof(cb), "%s\n", cmd);
	game_cbuf_addtext(cb);
}

// ---------------------------------------------------------------- the panel

static HWND game_window;
static WNDPROC game_proc;
static char console_settings_file[MAX_PATH];
static HWND raw_mouse_target;
static int raw_mouse_registration_owned;
static int raw_mouse_target_tracks_game_window;
static int raw_mouse_registration_seen;
static HWND raw_input_relay_window;
static WNDPROC raw_input_relay_original_proc;
static volatile LONG raw_mouse_packets;
static volatile LONG raw_mouse_first_packet_logged;

// Cursor capture uses a virtual pointer in relative mouse mode; the physical cursor is never warped.

static int console_settings_path_init(void)
{
	char exe_path[MAX_PATH];
	DWORD length;
	char *slash;
	if (console_settings_file[0])
		return 1;
	length = GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
	if (!length || length >= sizeof(exe_path))
		return 0;
	slash = strrchr(exe_path, '\\');
	if (!slash)
		slash = strrchr(exe_path, '/');
	if (!slash)
		return 0;
	*slash = 0;
	if (snprintf(console_settings_file, sizeof(console_settings_file), "%s\\mw2coop\\settings.ini", exe_path) >= (int)sizeof(console_settings_file)) {
		console_settings_file[0] = 0;
		return 0;
	}
	return 1;
}

static int console_watch_count_read(void)
{
	return (int)InterlockedCompareExchange(&console_watch_count, 0, 0);
}

static int console_watch_rows(void)
{
	int count = console_watch_count_read();
	return count > 0 ? count / CONSOLE_WATCH_COLUMNS + (count % CONSOLE_WATCH_COLUMNS != 0) : 0;
}

static int console_bottom_reserved_height(void)
{
	return CONSOLE_STATUS_HEIGHT + console_watch_rows() * CONSOLE_WATCH_ROW_HEIGHT;
}

static void console_load_qol_lists(void)
{
	char key[32];
	char value[COMPLETION_NAME_LEN];
	int configured = GetPrivateProfileIntA("console", "pinned_count", -1, console_settings_file);
	int legacy_format = configured < 0;
	int count = legacy_format ? 4 : configured;
	int seeded_defaults = 0;
	if (count < 0) count = 0;
	EnterCriticalSection(&lock);
	console_watch_count = 0;
	/* Read and grow incrementally instead of trusting pinned_count as an
	   allocation size. This has no fixed favorites limit and tolerates a
	   truncated/corrupt INI without attempting a huge up-front allocation. */
	for (int i = 0; i < count; i++) {
		int current;
		snprintf(key, sizeof(key), legacy_format ? "watch_%d" : "pinned_%d", i);
		GetPrivateProfileStringA("console", key, "", value, sizeof(value), console_settings_file);
		if (!value[0]) {
			if (legacy_format) continue;
			break; /* v4.39+ writes a contiguous pinned_0...pinned_N sequence. */
		}
		current = console_watch_count_read();
		if (console_name_in_list(console_watch_names, current, value) >= 0)
			continue;
		if (current == 0x7fffffff || !console_watch_reserve_locked(current + 1))
			break;
		lstrcpynA(console_watch_names[current], value, COMPLETION_NAME_LEN);
		InterlockedExchange(&console_watch_count, current + 1);
	}
	LeaveCriticalSection(&lock);
	/* Seed the two useful map/location dvars once, including when upgrading a
	   v4.39 profile that already persisted pinned_count=0. Preserve all existing
	   favorites and never re-add an item after the user explicitly unpins it. */
	if (GetPrivateProfileIntA("console", "favorites_defaults_initialized", 0, console_settings_file) != 1) {
		static const char *const defaults[] = {"mapname", "viewposNow"};
		int initialized = 1;
		EnterCriticalSection(&lock);
		for (int i = 0; i < (int)(sizeof(defaults) / sizeof(defaults[0])); i++) {
			int current = console_watch_count_read();
			if (console_name_in_list(console_watch_names, current, defaults[i]) >= 0)
				continue;
			if (current == 0x7fffffff || !console_watch_reserve_locked(current + 1)) {
				initialized = 0;
				break;
			}
			lstrcpynA(console_watch_names[current], defaults[i], COMPLETION_NAME_LEN);
			InterlockedExchange(&console_watch_count, current + 1);
			seeded_defaults = 1;
		}
		LeaveCriticalSection(&lock);
		if (seeded_defaults && !console_save_qol_lists())
			initialized = 0;
		if (initialized)
			(void)WritePrivateProfileStringA("console", "favorites_defaults_initialized", "1", console_settings_file);
	}
}

static int console_save_qol_lists(void)
{
	char key[32], count_text[32];
	char (*watches)[COMPLETION_NAME_LEN] = NULL;
	int watch_count, ok = 1;
	if (!console_settings_path_init()) return 0;
	EnterCriticalSection(&lock);
	watch_count = console_watch_count_read();
	if (watch_count > 0) {
		watches = (char (*)[COMPLETION_NAME_LEN])malloc((size_t)watch_count * sizeof(*watches));
		if (watches) memcpy(watches, console_watch_names, (size_t)watch_count * sizeof(*watches));
	}
	LeaveCriticalSection(&lock);
	if (watch_count > 0 && !watches) return 0;
	snprintf(count_text, sizeof(count_text), "%d", watch_count);
	if (!WritePrivateProfileStringA("console", "pinned_count", count_text, console_settings_file)) ok = 0;
	for (int i = 0; i < watch_count; i++) {
		snprintf(key, sizeof(key), "pinned_%d", i);
		if (!WritePrivateProfileStringA("console", key, watches[i], console_settings_file)) ok = 0;
	}
	/* Keep the original four keys mirrored so older builds can still read the first row. */
	for (int i = 0; i < 4; i++) {
		snprintf(key, sizeof(key), "watch_%d", i);
		if (!WritePrivateProfileStringA("console", key, i < watch_count ? watches[i] : "", console_settings_file)) ok = 0;
	}
	free(watches);
	if (!WritePrivateProfileStringA(NULL, NULL, NULL, console_settings_file)) ok = 0;
	return ok;
}


static void console_load_layout(void)
{
	char value[32] = "enhanced";
	char language[16] = "auto";
	LONG legacy, ui_language = -1;
	InterlockedExchange(&console_pause_mode, 0);
	if (!console_settings_path_init()) {
		InterlockedExchange(&console_legacy_layout, 0);
		InterlockedExchange(&console_ui_language, -1);
		log_printf("console: settings path unavailable; using game language, Enhanced layout and Pause mode off");
		return;
	}
	GetPrivateProfileStringA("console", "layout", "enhanced", value, sizeof(value), console_settings_file);
	legacy = !_stricmp(value, "legacy");
	InterlockedExchange(&console_legacy_layout, legacy);
	GetPrivateProfileStringA("console", "language", "auto", language, sizeof(language), console_settings_file);
	if (!_stricmp(language, "ru")) ui_language = 1;
	else if (!_stricmp(language, "en")) ui_language = 0;
	InterlockedExchange(&console_ui_language, ui_language);
	InterlockedExchange(&console_pause_mode,
		GetPrivateProfileIntA("console", "pause_mode", 0, console_settings_file) == 1);
	console_load_qol_lists();
	if (GetPrivateProfileIntA("console", "window_custom", 0, console_settings_file) == 1) {
		int left = GetPrivateProfileIntA("console", "window_left_pct", -1, console_settings_file);
		int top = GetPrivateProfileIntA("console", "window_top_pct", -1, console_settings_file);
		int width = GetPrivateProfileIntA("console", "window_width_pct", -1, console_settings_file);
		int height = GetPrivateProfileIntA("console", "window_height_pct", -1, console_settings_file);
		if (left >= 0 && left <= 10000 && top >= 0 && top <= 10000 &&
			width > 0 && width <= 10000 && height > 0 && height <= 10000) {
			console_saved_left_pct = left;
			console_saved_top_pct = top;
			console_saved_width_pct = width;
			console_saved_height_pct = height;
			InterlockedExchange(&console_saved_geometry_valid, 1);
		}
	}
	log_printf("console: loaded %s layout and %s UI language from settings.ini",
		legacy ? "Legacy" : "Enhanced", ui_language < 0 ? "game" : ui_language ? "Russian" : "English");
}

static void console_save_layout(void)
{
	const char *value = InterlockedCompareExchange(&console_legacy_layout, 0, 0) ? "legacy" : "enhanced";
	if (!console_settings_path_init() || !WritePrivateProfileStringA("console", "layout", value, console_settings_file)) {
		log_printf("console: failed to save layout setting (error %lu)", (unsigned long)GetLastError());
		return;
	}
	WritePrivateProfileStringA(NULL, NULL, NULL, console_settings_file); // (the flush returns 0 when it works: not an error, and the old error code is stale)
	log_printf("console: saved %s layout to settings.ini", value);
}

static void console_save_language(void)
{
	const char *value = console_ui_is_russian() ? "ru" : "en";
	if (!console_settings_path_init() || !WritePrivateProfileStringA("console", "language", value, console_settings_file)) {
		log_printf("console: failed to save UI language (error %lu)", (unsigned long)GetLastError());
		return;
	}
	WritePrivateProfileStringA(NULL, NULL, NULL, console_settings_file); // (the flush returns 0 when it works: not an error, and the old error code is stale)
	log_printf("console: saved %s UI language to settings.ini", value);
}

static void console_save_pause_mode(void)
{
	const char *value = InterlockedCompareExchange(&console_pause_mode, 0, 0) ? "1" : "0";
	if (!console_settings_path_init() || !WritePrivateProfileStringA("console", "pause_mode", value, console_settings_file)) {
		log_printf("console: failed to save Pause mode (error %lu)", (unsigned long)GetLastError());
		return;
	}
	WritePrivateProfileStringA(NULL, NULL, NULL, console_settings_file); // (the flush returns 0 when it works: not an error, and the old error code is stale)
	log_printf("console: saved Pause mode %s to settings.ini", value);
}

static void console_save_window_geometry(void)
{
	RECT rect;
	char value[16];
	int width = (int)InterlockedCompareExchange(&console_backbuffer_width, 0, 0);
	int height = (int)InterlockedCompareExchange(&console_panel_height, 0, 0);
	int left_pct, top_pct, width_pct, height_pct;
	if (width <= 0 || height <= 0 || full_console || console_window_collapsed || !console_settings_path_init())
		return;
	console_window_geometry(width, height, &rect);
	left_pct = (int)(((long long)rect.left * 10000 + width / 2) / width);
	top_pct = (int)(((long long)rect.top * 10000 + height / 2) / height);
	width_pct = (int)(((long long)(rect.right - rect.left) * 10000 + width / 2) / width);
	height_pct = (int)(((long long)(rect.bottom - rect.top) * 10000 + height / 2) / height);
	if (left_pct < 0) left_pct = 0;
	if (left_pct > 10000) left_pct = 10000;
	if (top_pct < 0) top_pct = 0;
	if (top_pct > 10000) top_pct = 10000;
	if (width_pct < 1) width_pct = 1;
	if (width_pct > 10000) width_pct = 10000;
	if (height_pct < 1) height_pct = 1;
	if (height_pct > 10000) height_pct = 10000;
#define WRITE_GEOMETRY_VALUE(key, number) do { \
		snprintf(value, sizeof(value), "%d", (number)); \
		if (!WritePrivateProfileStringA("console", (key), value, console_settings_file)) goto failed; \
	} while (0)
	if (!WritePrivateProfileStringA("console", "window_custom", "0", console_settings_file))
		goto failed;
	WRITE_GEOMETRY_VALUE("window_left_pct", left_pct);
	WRITE_GEOMETRY_VALUE("window_top_pct", top_pct);
	WRITE_GEOMETRY_VALUE("window_width_pct", width_pct);
	WRITE_GEOMETRY_VALUE("window_height_pct", height_pct);
	WRITE_GEOMETRY_VALUE("window_custom", 1);
#undef WRITE_GEOMETRY_VALUE
	if (!WritePrivateProfileStringA(NULL, NULL, NULL, console_settings_file))
		goto failed;
	return;
failed:
	(void)WritePrivateProfileStringA("console", "window_custom", "0", console_settings_file);
	log_printf("console: failed to save window geometry (error %lu)", (unsigned long)GetLastError());
}

static void console_clear_window_geometry(void)
{
	InterlockedExchange(&console_saved_geometry_valid, 0);
	if (console_settings_path_init())
		(void)WritePrivateProfileStringA("console", "window_custom", "0", console_settings_file);
}


/* Cursor capture is handled without moving the physical pointer. */
static int console_mouse_is_clipped(HWND h)
{
	RECT client, clip;
	POINT top_left = {0, 0}, bottom_right;
	if (!h || !GetClientRect(h, &client))
		return 0;
	bottom_right.x = client.right;
	bottom_right.y = client.bottom;
	if (!ClientToScreen(h, &top_left) || !ClientToScreen(h, &bottom_right) || !GetClipCursor(&clip))
		return 0;
	/* Treat a clip rectangle matching the game client as relative/captured mode.
	   Use a 2-pixel tolerance for games that inset the clip by one pixel. */
	return abs(clip.left - top_left.x) <= 2 && abs(clip.top - top_left.y) <= 2 &&
		abs(clip.right - bottom_right.x) <= 2 && abs(clip.bottom - bottom_right.y) <= 2;
}

static void console_restore_capture(void)
{
	HWND previous;
	if (!InterlockedExchange(&console_capture_owned, 0))
		return;
	previous = console_previous_capture;
	console_previous_capture = NULL;
	if (GetCapture() != game_window)
		return;
	if (previous && IsWindow(previous))
		SetCapture(previous);
	if (GetCapture() == game_window)
		ReleaseCapture();
}

static int console_pause_capture_interaction_active(void)
{
	return InterlockedCompareExchange(&console_pause_button_pressed, 0, 0) != 0 ||
		InterlockedCompareExchange(&console_window_drag_mode, 0, 0) != 0 ||
		InterlockedCompareExchange(&console_scrollbar_drag_mode, 0, 0) != 0 ||
		output_selecting != 0 || input_selecting != 0;
}

static void console_pause_capture_acquire(HWND h)
{
	HWND previous;
	if (!h || !IsWindow(h) || GetForegroundWindow() != h || GetCapture() == h)
		return;
	previous = GetCapture();
	SetCapture(h);
	if (GetCapture() == h) {
		console_pause_previous_capture = previous;
		InterlockedExchange(&console_pause_capture_release_pending, 0);
		InterlockedExchange(&console_pause_capture_owned, 1);
	}
}

static void console_pause_capture_release(HWND h, int force)
{
	HWND previous;
	if (!InterlockedCompareExchange(&console_pause_capture_owned, 0, 0))
		return;
	if (!force && is_open && console_pause_capture_interaction_active()) {
		InterlockedExchange(&console_pause_capture_release_pending, 1);
		return;
	}
	InterlockedExchange(&console_pause_capture_owned, 0);
	InterlockedExchange(&console_pause_capture_release_pending, 0);
	previous = console_pause_previous_capture;
	console_pause_previous_capture = NULL;
	if (!h || GetCapture() != h)
		return;
	if (previous && IsWindow(previous))
		SetCapture(previous);
	if (GetCapture() == h)
		ReleaseCapture();
}

static int console_pause_read_state(int *paused)
{
	dvar_t *d = game_find_dvar("cl_paused");
	if (!d || !paused)
		return 0;
	if (d->type == DVAR_TYPE_BOOL)
		*paused = d->current.enabled != 0;
	else if (d->type == DVAR_TYPE_INT)
		*paused = d->current.integer != 0;
	else
		return 0;
	return 1;
}

static void console_pause_queue(int paused)
{
	game_cbuf_addtext(paused ? "cl_paused 1\n" : "cl_paused 0\n");
}

// A co-op session (network or split screen) runs on a simulation both players share: cl_paused 1 there freezes the partner or ends in the "no signal" disconnect, so Pause mode
// does nothing while the game's dvar coop is 1.
static int console_pause_blocked_by_coop(void)
{
	char v[8];
	return game_dvar_raw("coop", v, sizeof(v)) && v[0] == '1';
}

static void console_pause_apply_mode(void)
{
	int paused = 0;
	if (!is_open)
		return;
	if (InterlockedCompareExchange(&console_pause_mode, 0, 0) && console_pause_blocked_by_coop()) {
		static LONG told;
		if (!InterlockedExchange(&told, 1))
			console_print_text("CON_PAUSE_OFF_COOP");
		return;
	}
	if (InterlockedCompareExchange(&console_pause_mode, 0, 0)) {
		if (!InterlockedCompareExchange(&console_pause_session_known, 0, 0)) {
			if (!console_pause_read_state(&paused)) {
				log_printf("console: Pause mode could not read cl_paused; leaving game state unchanged");
				return;
			}
			InterlockedExchange(&console_pause_initially_paused, paused);
			InterlockedExchange(&console_pause_session_known, 1);
			if (!paused) {
				console_pause_queue(1);
				InterlockedExchange(&console_pause_command_active, 1);
			}
		} else if (!InterlockedCompareExchange(&console_pause_initially_paused, 0, 0) &&
			!InterlockedCompareExchange(&console_pause_command_active, 0, 0)) {
			console_pause_queue(1);
			InterlockedExchange(&console_pause_command_active, 1);
		}
	} else if (InterlockedCompareExchange(&console_pause_session_known, 0, 0) &&
		!InterlockedCompareExchange(&console_pause_initially_paused, 0, 0) &&
		InterlockedExchange(&console_pause_command_active, 0)) {
		console_pause_queue(0);
	}
}

static void console_pause_end_session(void)
{
	if (InterlockedCompareExchange(&console_pause_session_known, 0, 0) &&
		!InterlockedCompareExchange(&console_pause_initially_paused, 0, 0) &&
		InterlockedExchange(&console_pause_command_active, 0))
		console_pause_queue(0);
	InterlockedExchange(&console_pause_session_known, 0);
	InterlockedExchange(&console_pause_initially_paused, 0);
	InterlockedExchange(&console_pause_command_active, 0);
}

static void console_pause_mode_toggle(void)
{
	LONG enabled = InterlockedCompareExchange(&console_pause_mode, 0, 0) ? 0 : 1;
	InterlockedExchange(&console_pause_mode, enabled);
	console_save_pause_mode();
	if (is_open) {
		console_pause_apply_mode();
		if (enabled)
			console_pause_capture_acquire(game_window);
		else
			console_pause_capture_release(game_window, 0);
	}
	InterlockedExchange(&dirty, 1);
}

static void console_print_version_once(void)
{
	dvar_t *version;
	char value[COMPLETION_VALUE_LEN];
	if (InterlockedCompareExchange(&console_version_line_printed, 0, 0)) return;
	version = game_find_dvar("version");
	if (!version || InterlockedCompareExchange(&console_version_line_printed, 1, 0) != 0) return;
	value[0] = 0;
	game_dvar_value_text(version, &version->current, value, sizeof(value));
	if (!value[0]) {
		InterlockedExchange(&console_version_line_printed, 0);
		return;
	}
	snprintf(console_version_status, sizeof(console_version_status), "%s", value);
	InterlockedExchange(&dirty, 1);
}

static void set_open(int on)
{
	if (on == (int)is_open)
		return;
	InterlockedExchange(&present_bootstrap_request, on ? 1 : 0);
	if (on) {
		int active = game_window && GetForegroundWindow() == game_window;
		InterlockedExchange(&console_window_active, active);
		InterlockedExchange(&is_open, 1);
		console_pause_apply_mode();
		if (active) {
			console_init_virtual_mouse(game_window);
			if (InterlockedCompareExchange(&console_pause_mode, 0, 0))
				console_pause_capture_acquire(game_window);
		} else {
			InterlockedExchange(&virtual_mouse_relative_mode, 0);
			InterlockedExchange(&virtual_mouse_raw_seen, 0);
		}
		console_set_native_cursor_hidden(game_window, active && console_mouse_is_clipped(game_window));
		if (!console_window_collapsed)
			console_print_version_once();
	} else {
		console_pause_end_session();
		InterlockedExchange(&console_pause_button_pressed, 0);
		InterlockedExchange(&console_button_pressed, 0);
		console_pause_capture_release(game_window, 1);
		InterlockedExchange(&is_open, 0);
		console_set_native_cursor_hidden(game_window, 0);
	}
	if (!on) {
		if (game_window) {
			KillTimer(game_window, CONSOLE_TITLE_CLICK_TIMER);
			InterlockedExchange(&console_pending_title_click, 0);
		}
		InterlockedExchange(&virtual_mouse_raw_seen, 0);
		InterlockedExchange(&virtual_mouse_relative_mode, 0);
		InterlockedExchange(&console_scrollbar_drag_mode, 0);
		InterlockedExchange(&console_header_hover_button, 0);
		InterlockedExchange(&console_button_pressed, 0);
		if (InterlockedExchange(&console_window_drag_mode, 0))
			console_save_window_geometry();
		console_restore_capture();
		InterlockedExchange(&full_console, 0);
		EnterCriticalSection(&lock);
		output_selecting = 0;
		input_selecting = 0;
		output_wheel_remainder = completion_wheel_remainder = 0;
		output_toast_count = 0;
		output_toast_capture_until = output_toast_expires = 0;
		SetRectEmpty(&output_toast_rect);
		hide_completion();
		InterlockedExchange(&context_menu_open, 0);
		LeaveCriticalSection(&lock);
	}
	InterlockedExchange(&dirty, 1);
	log_printf("console: %s", on ? "open" : "closed");
}


static int has_selection(void)
{
	return selection_anchor >= 0 && selection_anchor != cursor;
}

static void clear_selection(void)
{
	selection_anchor = -1;
}

static void delete_selection(void)
{
	if (!has_selection()) {
		clear_selection();
		return;
	}
	int first = selection_anchor < cursor ? selection_anchor : cursor;
	int last = selection_anchor > cursor ? selection_anchor : cursor;
	memmove(input + first, input + last, (size_t)(input_len - last + 1));
	input_len -= last - first;
	cursor = first;
	clear_selection();
}

static int output_has_selection(void)
{
	return output_sel_a_line >= 0 && output_sel_b_line >= 0 &&
		(output_sel_a_line != output_sel_b_line || output_sel_a_col != output_sel_b_col);
}

static int clipboard_copy_output(void)
{
	if (!output_has_selection()) return 0;
	int a_line = output_sel_a_line, a_col = output_sel_a_col;
	int b_line = output_sel_b_line, b_col = output_sel_b_col;
	if (a_line > b_line || (a_line == b_line && a_col > b_col)) {
		int t; t=a_line; a_line=b_line; b_line=t; t=a_col; a_col=b_col; b_col=t;
	}
	size_t capacity = (size_t)MAX_LINES * LINE_LEN + 1;
	WCHAR *wide = (WCHAR *)malloc((capacity + 1) * sizeof(WCHAR));
	size_t wide_used = 0;
	if (!wide) return 0;
	for (int line = a_line; line <= b_line && wide_used + LINE_LEN < capacity; line++) {
		int physical = (line_next - line_count + line + MAX_LINES * 2) % MAX_LINES;
		const char *src = lines[physical];
		UINT cp = line_codepage[physical] ? line_codepage[physical] : console_codepage();
		int len = line_visible_length_cp(src, cp), from = line == a_line ? a_col : 0, to = line == b_line ? b_col : len;
		char segment[LINE_LEN];
		size_t bytes;
		int chars;
		if (from < 0) from = 0; if (from > len) from = len;
		if (to < from) to = from; if (to > len) to = len;
		bytes = copy_visible_range_cp(src, from, to, segment, sizeof(segment) - 1, cp);
		if (line != a_line && wide_used + 2 < capacity) {
			wide[wide_used++] = L'\r';
			wide[wide_used++] = L'\n';
		}
		chars = MultiByteToWideChar(cp, 0, segment, (int)bytes, wide + wide_used, (int)(capacity - wide_used));
		if (chars > 0)
			wide_used += (size_t)chars;
	}
	if (!wide_used || !OpenClipboard(game_window)) { free(wide); return 0; }
	wide[wide_used] = 0;
	HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, (wide_used + 1) * sizeof(WCHAR));
	int ok = 0;
	if (data) {
		WCHAR *dst = (WCHAR *)GlobalLock(data);
		if (dst) {
			memcpy(dst, wide, wide_used * sizeof(WCHAR));
			dst[wide_used] = 0;
			GlobalUnlock(data);
			EmptyClipboard();
			if (SetClipboardData(CF_UNICODETEXT, data)) ok = 1;
			else GlobalFree(data);
		} else GlobalFree(data);
	}
	free(wide);
	CloseClipboard();
	return ok;
}

static int clipboard_copy_selection(void)
{
	if (!has_selection())
		return 0;
	int first = selection_anchor < cursor ? selection_anchor : cursor;
	int count = (selection_anchor > cursor ? selection_anchor : cursor) - first;
	wchar_t text[INPUT_LEN + 1];
	int chars = MultiByteToWideChar(console_codepage(), 0, input + first, count, text, INPUT_LEN);
	if (chars <= 0 || !OpenClipboard(game_window))
		return 0;
	int copied = 0;
	HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(chars + 1) * sizeof(wchar_t));
	if (data) {
		wchar_t *dst = (wchar_t *)GlobalLock(data);
		if (dst) {
			memcpy(dst, text, (size_t)chars * sizeof(wchar_t));
			dst[chars] = 0;
			GlobalUnlock(data);
			EmptyClipboard();
			if (SetClipboardData(CF_UNICODETEXT, data))
				copied = 1;
			else
				GlobalFree(data);
		} else
			GlobalFree(data);
	}
	CloseClipboard();
	return copied;
}

static int clipboard_read(char *out, int size)
{
	int written = 0;
	if (size <= 1 || !OpenClipboard(game_window))
		return 0;
	HANDLE data = GetClipboardData(CF_UNICODETEXT);
	if (data) {
		const wchar_t *src = (const wchar_t *)GlobalLock(data);
		if (src) {
			char tmp[INPUT_LEN * 4 + 1];
			int n = WideCharToMultiByte(console_codepage(), 0, src, -1, tmp, sizeof(tmp), NULL, NULL);
			if (n > 0) {
				for (int i = 0; tmp[i] && written < size - 1; i++) {
					unsigned char ch = (unsigned char)tmp[i];
					if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
					if (ch >= 32)
						out[written++] = (char)ch;
				}
			}
			GlobalUnlock(data);
		}
	} else {
		data = GetClipboardData(CF_TEXT);
		if (data) {
			const char *src = (const char *)GlobalLock(data);
			if (src) {
				for (int i = 0; src[i] && written < size - 1; i++) {
					unsigned char ch = (unsigned char)src[i];
					if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
					if (ch >= 32)
						out[written++] = (char)ch;
				}
				GlobalUnlock(data);
			}
		}
	}
	CloseClipboard();
	out[written] = 0;
	return written;
}


static int context_menu_count(int mode)
{
	return mode == CONSOLE_CONTEXT_INPUT ? 7 : mode == CONSOLE_CONTEXT_OUTPUT ? 4 : 0;
}

static const char *context_menu_label(int mode, int index)
{
	static const char *const input_items[] = {"CON_MENU_UNDO", "CON_MENU_REDO", "CON_MENU_CUT", "CON_MENU_COPY", "CON_MENU_PASTE", "CON_MENU_DELETE", "CON_MENU_SELECT_ALL"};
	static const char *const output_items[] = {"CON_MENU_COPY", "CON_MENU_COPY_LINE", "CON_MENU_SELECT_ALL", "CON_MENU_CLEAR_OUTPUT"};
	int russian = console_ui_is_russian();
	if (mode == CONSOLE_CONTEXT_INPUT && index >= 0 && index < 7)
		return ctexts_get(input_items[index], russian);
	if (mode == CONSOLE_CONTEXT_OUTPUT && index >= 0 && index < 4)
		return ctexts_get(output_items[index], russian);
	return "";
}

static const char *context_menu_shortcut(int mode, int index)
{
	static const char *const input_keys[] = {"", "", "Ctrl+X", "Ctrl+C", "Ctrl+V", "Del", "Ctrl+A"};
	static const char *const output_keys[] = {"Ctrl+C", "", "Ctrl+A", "Ctrl+L"};
	if (mode == CONSOLE_CONTEXT_INPUT && index >= 0 && index < 7)
		return input_keys[index];
	if (mode == CONSOLE_CONTEXT_OUTPUT && index >= 0 && index < 4)
		return output_keys[index];
	return "";
}

static int context_menu_item_enabled(int mode, int index)
{
	if (mode == CONSOLE_CONTEXT_INPUT) {
		switch (index) {
		case 0: return input_undo_count > 0;
		case 1: return input_redo_count > 0;
		case 2: return has_selection();
		case 3: return has_selection();
		case 4: return IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT);
		case 5: return has_selection() || cursor < input_len;
		case 6: return input_len > 0;
		}
	} else if (mode == CONSOLE_CONTEXT_OUTPUT) {
		switch (index) {
		case 0: return output_has_selection();
		case 1: return output_context_line >= 0 && output_context_line < line_count;
		case 2: return line_count > 0;
		case 3: return line_count > 0;
		}
	}
	return 0;
}

static void output_select_all(void)
{
	if (!line_count) {
		output_sel_a_line = output_sel_b_line = -1;
		output_sel_a_col = output_sel_b_col = 0;
		output_selecting = 0;
		return;
	}
	output_sel_a_line = 0;
	output_sel_a_col = 0;
	output_sel_b_line = line_count - 1;
	int last_physical = (line_next - 1 + MAX_LINES) % MAX_LINES;
	output_sel_b_col = line_visible_length_cp(lines[last_physical],
		line_codepage[last_physical] ? line_codepage[last_physical] : console_codepage());
	output_selecting = 0;
}

static void console_context_menu_execute(int mode, int index)
{
	if (index < 0 || index >= context_menu_count(mode))
		return;
	EnterCriticalSection(&lock);
	InterlockedExchange(&context_menu_open, 0);
	InterlockedExchange(&context_menu_selected, -1);
	if (mode == CONSOLE_CONTEXT_INPUT) {
		switch (index) {
		case 0: /* Undo */
			(void)input_undo_apply_locked(0);
			break;
		case 1: /* Redo */
			(void)input_undo_apply_locked(1);
			break;
		case 2: /* Cut */
			if (has_selection() && clipboard_copy_selection()) {
				input_undo_push(0);
				delete_selection();
			}
			break;
		case 3: /* Copy */
			if (has_selection()) (void)clipboard_copy_selection();
			break;
		case 4: { /* Paste */
			char paste[INPUT_LEN + 1];
			int n = clipboard_read(paste, sizeof(paste));
			if (n > 0) {
				int limit = output_search_mode ? OUTPUT_SEARCH_LEN - 1 : INPUT_LEN;
				int selected = has_selection() ? abs(selection_anchor - cursor) : 0;
				int room = limit - (input_len - selected);
				if (room < 0) room = 0;
				if (n > room) n = room;
				if (n > 0 || selected) {
					input_undo_push(0);
					delete_selection();
					memmove(input + cursor + n, input + cursor, (size_t)(input_len - cursor + 1));
					memcpy(input + cursor, paste, (size_t)n);
					cursor += n;
					input_len += n;
				}
			}
			break;
		}
		case 5: /* Delete */
			if (has_selection() || cursor < input_len) {
				input_undo_push(0);
				if (has_selection())
					delete_selection();
				else {
					memmove(input + cursor, input + cursor + 1, (size_t)(input_len - cursor));
					input_len--;
				}
			}
			clear_selection();
			break;
		case 6: /* Select All */
			input_undo_last_group = 0;
			selection_anchor = 0;
			cursor = input_len;
			break;
		}
		refresh_completions(0);
	} else if (mode == CONSOLE_CONTEXT_OUTPUT) {
		switch (index) {
		case 0:
			if (output_has_selection()) (void)clipboard_copy_output();
			break;
		case 1:
			if (output_context_line >= 0 && output_context_line < line_count) {
				int physical = (line_next - line_count + output_context_line + MAX_LINES * 2) % MAX_LINES;
				output_sel_a_line = output_sel_b_line = output_context_line;
				output_sel_a_col = 0;
				output_sel_b_col = line_visible_length_cp(lines[physical],
					line_codepage[physical] ? line_codepage[physical] : console_codepage());
				(void)clipboard_copy_output();
				output_sel_a_line = output_sel_b_line = -1;
			}
			break;
		case 2:
			output_select_all();
			break;
		case 3:
			line_count = line_next = 0;
			scroll = 0;
			output_search_match_seq = -1;
			output_context_line = -1;
			output_sel_a_line = output_sel_b_line = -1;
			output_selecting = 0;
			output_toast_count = 0;
			output_toast_capture_until = output_toast_expires = 0;
			SetRectEmpty(&output_toast_rect);
			SetRectEmpty(&output_toast_close_rect);
			break;
		}
	}
	InterlockedExchange(&dirty, 1);
	LeaveCriticalSection(&lock);
}

static int console_history_filter_step(int direction)
{
	int oldest = history_count > HISTORY ? history_count - HISTORY : 0;
	int prefix_length;
	if (!history_filter_active) {
		if (!input_len) return 0;
		lstrcpynA(history_filter_prefix, input, sizeof(history_filter_prefix));
		lstrcpynA(history_filter_draft, input, sizeof(history_filter_draft));
		history_filter_cursor = history_count;
		history_filter_active = 1;
	}
	prefix_length = (int)strlen(history_filter_prefix);
	if (direction < 0) {
		for (int i = history_filter_cursor - 1; i >= oldest; i--) {
			const char *candidate = history[i % HISTORY];
			if (!_strnicmp(candidate, history_filter_prefix, (size_t)prefix_length)) {
				history_filter_cursor = i;
				input_undo_count = input_redo_count = 0;
				input_undo_last_group = 0;
				lstrcpynA(input, candidate, INPUT_LEN + 1);
				input_len = cursor = (int)strlen(input);
				clear_selection(); refresh_completions(0);
				return 1;
			}
		}
		return 1; /* A non-empty prefix must not fall back to unrelated history. */
	}
	for (int i = history_filter_cursor + 1; i < history_count; i++) {
		const char *candidate = history[i % HISTORY];
		if (!_strnicmp(candidate, history_filter_prefix, (size_t)prefix_length)) {
			history_filter_cursor = i;
			input_undo_count = input_redo_count = 0;
			input_undo_last_group = 0;
			lstrcpynA(input, candidate, INPUT_LEN + 1);
			input_len = cursor = (int)strlen(input);
			clear_selection(); refresh_completions(0);
			return 1;
		}
	}
	input_undo_count = input_redo_count = 0;
	input_undo_last_group = 0;
	lstrcpynA(input, history_filter_draft, INPUT_LEN + 1);
	input_len = cursor = (int)strlen(input);
	history_pos = history_count;
	history_filter_active = 0;
	clear_selection(); refresh_completions(0);
	return 1;
}

static void console_output_search_cancel(void)
{
	EnterCriticalSection(&lock);
	if (output_search_mode) {
		output_search_mode = 0;
		lstrcpynA(input, output_search_saved_input, INPUT_LEN + 1);
		input_len = (int)strlen(input);
		cursor = output_search_saved_cursor;
		if (cursor < 0) cursor = 0;
		if (cursor > input_len) cursor = input_len;
		selection_anchor = output_search_saved_selection;
		if (selection_anchor < -1 || selection_anchor > input_len) selection_anchor = -1;
		refresh_completions(0);
		InterlockedExchange(&dirty, 1);
	}
	LeaveCriticalSection(&lock);
}

static void edit_key(WPARAM vk)
{
	int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
	int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	EnterCriticalSection(&lock);
	InterlockedExchange(&context_menu_open, 0);
	if (output_search_mode && vk == VK_RETURN) {
		lstrcpynA(output_search_query, input, sizeof(output_search_query));
		output_search_mode = 0;
		lstrcpynA(input, output_search_saved_input, INPUT_LEN + 1);
		input_len = (int)strlen(input);
		cursor = output_search_saved_cursor;
		if (cursor < 0) cursor = 0;
		if (cursor > input_len) cursor = input_len;
		selection_anchor = output_search_saved_selection;
		if (selection_anchor < -1 || selection_anchor > input_len) selection_anchor = -1;
		output_search_match_seq = -1;
		refresh_completions(0);
		LeaveCriticalSection(&lock);
		console_output_search_step(1);
		InterlockedExchange(&dirty, 1);
		return;
	}
	if (output_search_mode && vk == VK_ESCAPE) {
		output_search_mode = 0;
		lstrcpynA(input, output_search_saved_input, INPUT_LEN + 1);
		input_len = (int)strlen(input);
		cursor = output_search_saved_cursor;
		if (cursor < 0) cursor = 0;
		if (cursor > input_len) cursor = input_len;
		selection_anchor = output_search_saved_selection;
		if (selection_anchor < -1 || selection_anchor > input_len) selection_anchor = -1;
		refresh_completions(0);
		InterlockedExchange(&dirty, 1);
		LeaveCriticalSection(&lock);
		return;
	}
	if (history_filter_active && vk != VK_UP && vk != VK_DOWN)
		history_filter_active = 0;
	if (ctrl) {
		switch (vk) {
		case 'Z':
			if (shift) (void)input_undo_apply_locked(1);
			else (void)input_undo_apply_locked(0);
			break;
		case 'Y':
			(void)input_undo_apply_locked(1);
			break;
		case 'A':
			input_undo_last_group = 0;
			selection_anchor = 0;
			cursor = input_len;
			hide_completion();
			break;
		case 'C':
			if (output_has_selection()) (void)clipboard_copy_output();
			else (void)clipboard_copy_selection();
			break;
		case 'F':
			if (!output_search_mode) {
				output_search_mode = 1;
				lstrcpynA(output_search_saved_input, input, sizeof(output_search_saved_input));
				output_search_saved_cursor = cursor;
				output_search_saved_selection = selection_anchor;
				input[0] = 0; input_len = cursor = 0; selection_anchor = -1;
				hide_completion();
			}
			break;
		case 'L':
			line_count = line_next = 0;
			scroll = 0;
			output_search_match_seq = -1;
			output_context_line = -1;
			output_sel_a_line = output_sel_b_line = -1;
			output_selecting = 0;
			output_toast_count = 0;
			output_toast_capture_until = output_toast_expires = 0;
			SetRectEmpty(&output_toast_rect);
			SetRectEmpty(&output_toast_close_rect);
			hide_completion();
			break;
		case 'U': /* Delete from the cursor to the beginning of the input. */
			if (has_selection() || cursor > 0) input_undo_push(0);
			if (has_selection())
				delete_selection();
			else if (cursor > 0) {
				memmove(input, input + cursor, (size_t)(input_len - cursor + 1));
				input_len -= cursor;
				cursor = 0;
			}
			clear_selection();
			refresh_completions(0);
			break;
		case 'K': /* Delete from the cursor to the end of the input. */
			if (has_selection() || cursor < input_len) input_undo_push(0);
			if (has_selection())
				delete_selection();
			else if (cursor < input_len) {
				input_len = cursor;
				input[input_len] = 0;
			}
			clear_selection();
			refresh_completions(0);
			break;
		case 'X':
			if (clipboard_copy_selection()) {
				input_undo_push(0);
				delete_selection();
				refresh_completions(0);
			}
			break;
		case 'W':
		case VK_BACK: {
			if (has_selection() || cursor > 0) input_undo_push(2);
			if (has_selection()) delete_selection();
			else if (cursor > 0) {
				int first = cursor;
				while (first > 0 && isspace((unsigned char)input[first - 1])) first--;
				while (first > 0 && !isspace((unsigned char)input[first - 1])) first--;
				memmove(input + first, input + cursor, (size_t)(input_len - cursor + 1));
				input_len -= cursor - first; cursor = first;
			}
			clear_selection(); refresh_completions(0);
			break;
		}
		case 'V': {
			char paste[INPUT_LEN + 1];
			int n = clipboard_read(paste, sizeof(paste));
			if (n > 0) {
				int limit = output_search_mode ? OUTPUT_SEARCH_LEN - 1 : INPUT_LEN;
				int selected = has_selection() ? abs(selection_anchor - cursor) : 0;
				int room = limit - (input_len - selected);
				if (room < 0) room = 0;
				if (n > room) n = room;
				if (n > 0 || selected) {
					input_undo_push(0);
					delete_selection();
					memmove(input + cursor + n, input + cursor, (size_t)(input_len - cursor + 1));
					memcpy(input + cursor, paste, (size_t)n);
					cursor += n;
					input_len += n;
				}
			}
			refresh_completions(0);
			break;
		}
		case VK_HOME:
			scroll = line_count > 0 ? line_count - 1 : 0;
			break;
		case VK_END:
			scroll = 0;
			break;
		default:
			LeaveCriticalSection(&lock);
			return;
		}
		InterlockedExchange(&dirty, 1);
		LeaveCriticalSection(&lock);
		return;
	}
	switch (vk) {
	case VK_RETURN: {
		if (completion_selected >= 0 && completion_count > 0 &&
			(completion_count > 1 || (input_len >= 2 && input[0] == '*' && input[1] == '='))) {
			completion_choose(completion_selected);
			break;
		}
		char cmd[INPUT_LEN + 1];
		lstrcpynA(cmd, input, sizeof(cmd));
		if (input_len > 0) input_undo_push(0);
		input[0] = 0, input_len = cursor = 0;
		history_pos = history_count;
		history_draft_valid = 0;
		clear_selection();
		hide_completion();
		LeaveCriticalSection(&lock);
		run_line(cmd);
		InterlockedExchange(&dirty, 1);
		return;
	}
	case VK_BACK:
		if (has_selection() || cursor > 0) input_undo_push(2);
		if (has_selection())
			delete_selection();
		else if (cursor > 0) {
			memmove(input + cursor - 1, input + cursor, (size_t)(input_len - cursor + 1));
			cursor--, input_len--;
		}
		clear_selection(); refresh_completions(0);
		break;
	case VK_DELETE:
		if (has_selection() || cursor < input_len) input_undo_push(3);
		if (has_selection())
			delete_selection();
		else if (cursor < input_len) {
			memmove(input + cursor, input + cursor + 1, (size_t)(input_len - cursor));
			input_len--;
		}
		clear_selection(); refresh_completions(0);
		break;
	case VK_LEFT:
		input_undo_last_group = 0;
		if (completion_count > 1) {
			if (completion_selected > 0) completion_selected--;
		} else if (has_selection() && !shift) {
			cursor = selection_anchor < cursor ? selection_anchor : cursor;
			clear_selection();
		} else {
			if (shift && selection_anchor < 0) selection_anchor = cursor;
			if (cursor > 0) cursor--;
			if (!shift) clear_selection();
		}
		if (completion_count <= 1) refresh_completions(0);
		break;
	case VK_RIGHT:
		input_undo_last_group = 0;
		if (completion_count > 1) {
			if (completion_selected + 1 < completion_count) completion_selected++;
		} else if (has_selection() && !shift) {
			cursor = selection_anchor > cursor ? selection_anchor : cursor;
			clear_selection();
		} else {
			if (shift && selection_anchor < 0) selection_anchor = cursor;
			if (cursor < input_len) cursor++;
			if (!shift) clear_selection();
		}
		if (completion_count <= 1) refresh_completions(0);
		break;
	case VK_HOME:
		input_undo_last_group = 0;
		if (shift && selection_anchor < 0) selection_anchor = cursor;
		cursor = 0;
		if (!shift) clear_selection();
		refresh_completions(0);
		break;
	case VK_END:
		input_undo_last_group = 0;
		if (shift && selection_anchor < 0) selection_anchor = cursor;
		cursor = input_len;
		if (!shift) clear_selection();
		refresh_completions(0);
		break;
	case VK_UP:
		input_undo_last_group = 0;
		if (output_search_mode) break;
		if (completion_count > 1) {
			int step = completion_columns > 0 ? completion_columns : 1;
			if (completion_selected < 0) completion_selected = 0;
			else completion_selected -= step;
			if (completion_selected < 0) completion_selected = 0;
		} else if (input_len > 0 || history_filter_active) {
			(void)console_history_filter_step(-1);
		} else {
			int oldest = history_count > HISTORY ? history_count - HISTORY : 0;
			if (history_pos == history_count && history_count > 0) {
				lstrcpynA(history_draft, input, sizeof(history_draft));
				history_draft_valid = 1;
			}
			if (history_pos > oldest) {
				history_pos--;
				input_undo_count = input_redo_count = 0;
				input_undo_last_group = 0;
				lstrcpynA(input, history[history_pos % HISTORY], INPUT_LEN + 1);
				input_len = cursor = (int)strlen(input);
				clear_selection();
				refresh_completions(0);
			}
		}
		break;
	case VK_DOWN:
		input_undo_last_group = 0;
		if (output_search_mode) break;
		if (completion_count > 1) {
			int step = completion_columns > 0 ? completion_columns : 1;
			if (completion_selected < 0) completion_selected = 0;
			else completion_selected += step;
			if (completion_selected >= completion_count) completion_selected = completion_count - 1;
		} else if (history_filter_active) {
			(void)console_history_filter_step(1);
		} else if (history_pos < history_count) {
			history_pos++;
			input_undo_count = input_redo_count = 0;
			input_undo_last_group = 0;
			if (history_pos < history_count)
				lstrcpynA(input, history[history_pos % HISTORY], INPUT_LEN + 1);
			else if (history_draft_valid)
				lstrcpynA(input, history_draft, INPUT_LEN + 1);
			else
				input[0] = 0;
			if (history_pos == history_count)
				history_draft_valid = 0;
			input_len = cursor = (int)strlen(input);
			clear_selection();
			refresh_completions(0);
		}
		break;
	case VK_TAB:
		tab_complete();
		break;
	case VK_PRIOR:
		scroll += 8;
		if (scroll > (line_count > 0 ? line_count - 1 : 0))
			scroll = line_count > 0 ? line_count - 1 : 0;
		break;
	case VK_NEXT:
		scroll -= 8;
		if (scroll < 0)
			scroll = 0;
		break;
	}
	InterlockedExchange(&dirty, 1);
	LeaveCriticalSection(&lock);
}

static HKL english_layout;
static int current_layout_shift_symbol(LPARAM key_lparam, char *out)
{
	BYTE state[256];
	WCHAR wide[4];
	WORD char_type = 0;
	char encoded[4];
	BOOL used_default = FALSE;
	UINT scan = (UINT)((key_lparam >> 16) & 0xff);
	UINT vk;
	HKL layout = GetKeyboardLayout(0);
	int n, bytes;
	if (!scan || !layout)
		return 0;
	GetKeyboardState(state);
	if (!(state[VK_SHIFT] & 0x80))
		return 0;
	vk = MapVirtualKeyExA(scan, MAPVK_VSC_TO_VK_EX, layout);
	n = ToUnicodeEx(vk, scan, state, wide, 4, 0, layout);
	if (n != 1 || wide[0] < 32)
		return 0;
	if (GetStringTypeW(CT_CTYPE1, wide, 1, &char_type) && (char_type & C1_ALPHA))
		return 0; /* Shift+letter still follows the English command layout. */
	bytes = WideCharToMultiByte(console_codepage(), 0, wide, 1, encoded, sizeof(encoded), NULL, &used_default);
	if (bytes != 1 || used_default)
		return 0; /* Keep the input byte-indexed; only insert symbols representable as one code-page byte. */
	*out = encoded[0];
	return 1;
}

static int english_layout_char(WPARAM ch, LPARAM key_lparam, char *out)
{
	BYTE state[256]; WCHAR wide[4];
	(void)ch;
	UINT scan = (UINT)((key_lparam >> 16) & 0xff);
	UINT vk;
	int n;
	if (!english_layout) english_layout = LoadKeyboardLayoutA("00000409", KLF_NOTELLSHELL);
	if (!english_layout || !scan) return 0;
	GetKeyboardState(state);
	vk = MapVirtualKeyExA(scan, MAPVK_VSC_TO_VK_EX, english_layout);
	n = ToUnicodeEx(vk, scan, state, wide, 4, 0, english_layout);
	if (n != 1 || wide[0] < 32 || wide[0] > 126) return 0;
	*out = (char)wide[0];
	return 1;
}

static void edit_char(WPARAM ch)
{
	if (ch < 32 || ch > 255 || ch == 127)
		return;
	EnterCriticalSection(&lock);
	InterlockedExchange(&context_menu_open, 0);
	history_filter_active = 0;
	if (has_selection() || input_len < (output_search_mode ? OUTPUT_SEARCH_LEN - 1 : INPUT_LEN)) input_undo_push(1);
	if (has_selection())
		delete_selection();
	if (input_len < (output_search_mode ? OUTPUT_SEARCH_LEN - 1 : INPUT_LEN)) {
		memmove(input + cursor + 1, input + cursor, (size_t)(input_len - cursor + 1));
		input[cursor++] = (char)ch;
		input_len++;
	}
	clear_selection();
	refresh_completions(0);
	InterlockedExchange(&dirty, 1);
	LeaveCriticalSection(&lock);
}

static int completion_hit(int x, int y, int *index)
{
	if (completion_count <= 0 || !PtInRect(&completion_rect, (POINT){x, y}) ||
		PtInRect(&completion_scroll_track, (POINT){x, y}) || completion_cell_width <= 0 || completion_rows <= 0)
		return 0;
	int rel_y = y - completion_top - completion_header_height;
	if (rel_y < 0)
		return 0;
	int row = rel_y / CONSOLE_LINE_HEIGHT;
	int col = (x - completion_rect.left - 8) / completion_cell_width;
	if (completion_count == 1 && completion_items[0].is_dvar && row == 1 && completion_columns == 1) {
		if (index) *index = 0;
		return 1;
	}
	if (row < 0 || row >= completion_rows || col < 0 || col >= completion_columns)
		return 0;
	int idx = (completion_scroll + row) * completion_columns + col;
	if (idx < 0 || idx >= completion_count)
		return 0;
	if (index) *index = idx;
	return 1;
}

static int completion_pin_hit(int x, int y, int *index)
{
	POINT pt = {x, y};
	if (completion_count <= 0) return 0;
	for (int i = 0; i < completion_count; i++) {
		if (completion_pin_rects[i].right > completion_pin_rects[i].left &&
			completion_pin_rects[i].bottom > completion_pin_rects[i].top &&
			PtInRect(&completion_pin_rects[i], pt)) {
			if (index) *index = i;
			return 1;
		}
	}
	return 0;
}


static int console_watch_item_rect(int index, RECT *item_rect)
{
	RECT window_rect;
	int width = (int)InterlockedCompareExchange(&console_backbuffer_width, 0, 0);
	int panel_h = (int)InterlockedCompareExchange(&console_panel_height, 0, 0);
	int count = console_watch_count_read();
	int rows = count > 0 ? count / CONSOLE_WATCH_COLUMNS +
		(count % CONSOLE_WATCH_COLUMNS != 0) : 0;
	int status_top, row, col, row_top, row_bottom, row_columns;
	int usable_left, usable_right, gap = 5, available_width;
	if (!item_rect || !rows || index < 0 || index >= count || width <= 0 || panel_h <= 0 ||
		console_window_collapsed)
		return 0;
	console_window_geometry(width, panel_h, &window_rect);
	status_top = window_rect.bottom - CONSOLE_STATUS_HEIGHT - rows * CONSOLE_WATCH_ROW_HEIGHT;
	row = index / CONSOLE_WATCH_COLUMNS;
	col = index % CONSOLE_WATCH_COLUMNS;
	row_columns = min(CONSOLE_WATCH_COLUMNS, count - row * CONSOLE_WATCH_COLUMNS);
	if (row_columns <= 0 || col >= row_columns)
		return 0;
	row_top = status_top + CONSOLE_STATUS_HEIGHT + row * CONSOLE_WATCH_ROW_HEIGHT;
	row_bottom = min(row_top + CONSOLE_WATCH_ROW_HEIGHT, window_rect.bottom - 1);
	usable_left = window_rect.left + 1 + 8;
	usable_right = window_rect.right - 1 - 8;
	available_width = usable_right - usable_left - (row_columns - 1) * gap;
	if (available_width / row_columns < 16 || row_bottom <= row_top + 4)
		return 0;
	/* A partially filled final row shares the full console width instead of
	   leaving its dvars constrained to the first few fixed grid columns. */
	item_rect->left = usable_left + col * gap + (available_width * col) / row_columns;
	item_rect->right = usable_left + col * gap + (available_width * (col + 1)) / row_columns;
	item_rect->top = row_top + 2;
	item_rect->bottom = row_bottom - 2;
	return item_rect->right > item_rect->left && item_rect->bottom > item_rect->top;
}

static int console_watch_unpin_rect(int index, RECT *unpin_rect)
{
	RECT item_rect;
	int height, width = 18;
	if (!unpin_rect || !console_watch_item_rect(index, &item_rect))
		return 0;
	height = item_rect.bottom - item_rect.top;
	if (height < 14)
		return 0;
	unpin_rect->right = item_rect.right - 1;
	unpin_rect->left = unpin_rect->right - width;
	unpin_rect->top = item_rect.top + (height - 18) / 2;
	unpin_rect->bottom = unpin_rect->top + 18;
	if (unpin_rect->top < item_rect.top) {
		unpin_rect->top = item_rect.top;
		unpin_rect->bottom = item_rect.bottom;
	}
	return unpin_rect->left >= item_rect.left && unpin_rect->right <= item_rect.right;
}

static int console_watch_chip_hit(int x, int y, int *index)
{
	RECT window_rect, item_rect;
	int width = (int)InterlockedCompareExchange(&console_backbuffer_width, 0, 0);
	int panel_h = (int)InterlockedCompareExchange(&console_panel_height, 0, 0);
	int count = console_watch_count_read();
	int rows = count > 0 ? count / CONSOLE_WATCH_COLUMNS +
		(count % CONSOLE_WATCH_COLUMNS != 0) : 0;
	int status_top, row;
	POINT pt = {x, y};
	if (!rows || width <= 0 || panel_h <= 0 || console_window_collapsed)
		return 0;
	console_window_geometry(width, panel_h, &window_rect);
	status_top = window_rect.bottom - CONSOLE_STATUS_HEIGHT - rows * CONSOLE_WATCH_ROW_HEIGHT;
	if (y < status_top + CONSOLE_STATUS_HEIGHT || y >= window_rect.bottom - 1)
		return 0;
	row = (y - (status_top + CONSOLE_STATUS_HEIGHT)) / CONSOLE_WATCH_ROW_HEIGHT;
	if (row < 0 || row >= rows)
		return 0;
	{
		int col;
		int row_columns = min(CONSOLE_WATCH_COLUMNS, count - row * CONSOLE_WATCH_COLUMNS);
		if (row_columns <= 0)
			return 0;
		for (col = 0; col < row_columns; col++) {
			int item = row * CONSOLE_WATCH_COLUMNS + col;
			if (!console_watch_item_rect(item, &item_rect) || !PtInRect(&item_rect, pt))
				continue;
			if (index) *index = item;
			return 1;
		}
	}
	return 0;
}

static int console_watch_unpin_hit(int x, int y, int *index)
{
	RECT unpin_rect;
	POINT pt = {x, y};
	int item = -1;
	if (!console_watch_chip_hit(x, y, &item) || item < 0 ||
		!console_watch_unpin_rect(item, &unpin_rect) || !PtInRect(&unpin_rect, pt))
		return 0;
	if (index) *index = item;
	return 1;
}

static int console_watch_remove_at(int x, int y)
{
	char name[COMPLETION_NAME_LEN] = "";
	int index = -1, count;
	/* Resolve the visible icon and copy its dvar under one lock so a concurrent
	   watch/unwatch command cannot make a stale cell index remove another item. */
	EnterCriticalSection(&lock);
	if (!console_watch_unpin_hit(x, y, &index)) {
		LeaveCriticalSection(&lock);
		return 0;
	}
	count = console_watch_count_read();
	if (index >= 0 && index < count)
		lstrcpynA(name, console_watch_names[index], sizeof(name));
	LeaveCriticalSection(&lock);
	if (name[0]) {
		(void)console_watch_change(name, 1);
		InterlockedExchange(&console_watch_hover_index, -1);
		InterlockedExchange(&dirty, 1);
		return 1;
	}
	return 0;
}

static int line_visible_length_cp(const char *text, UINT codepage)
{
	const char *p = text, *end = text + strlen(text);
	int columns = 0;
	while (p < end) {
		if (*p == '^' && p + 1 < end && p[1] >= '0' && p[1] <= '9') {
			p += 2;
			continue;
		}
		p += console_char_bytes(p, end, codepage);
		columns++;
	}
	return columns;
}

static int line_visible_text_columns_cp(const char *text, UINT codepage)
{
	const char *p = text, *end = text + strlen(text);
	int columns = 0, last_text_column = 0;
	while (p < end) {
		int bytes;
		if (*p == '^' && p + 1 < end && p[1] >= '0' && p[1] <= '9') {
			p += 2;
			continue;
		}
		bytes = console_char_bytes(p, end, codepage);
		if (!(bytes == 1 && (*p == ' ' || *p == '\t')))
			last_text_column = columns + 1;
		columns++;
		p += bytes;
	}
	return last_text_column;
}

static size_t copy_visible_range_cp(const char *source, int from, int to, char *destination, size_t capacity, UINT codepage)
{
	const char *p = source, *end = source + strlen(source);
	int column = 0;
	size_t written = 0;
	while (p < end && written < capacity) {
		int bytes;
		if (*p == '^' && p + 1 < end && p[1] >= '0' && p[1] <= '9') {
			p += 2;
			continue;
		}
		bytes = console_char_bytes(p, end, codepage);
		if (column >= from && column < to && written + (size_t)bytes <= capacity) {
			memcpy(destination + written, p, (size_t)bytes);
			written += (size_t)bytes;
		}
		column++;
		p += bytes;
	}
	return written;
}

static int visible_byte_offset_cp(const char *text, int columns, UINT codepage)
{
	const char *p = text, *end = text + strlen(text);
	int visible = 0;
	while (p < end && visible < columns) {
		p += console_char_bytes(p, end, codepage);
		visible++;
	}
	return (int)(p - text);
}

static void console_mouse_to_backbuffer(HWND h, int *x, int *y)
{
	RECT rc;
	int width = (int)InterlockedCompareExchange(&console_backbuffer_width, 0, 0);
	int height = (int)InterlockedCompareExchange(&console_backbuffer_height, 0, 0);
	if (!x || !y || !GetClientRect(h, &rc) || rc.right <= 0 || rc.bottom <= 0 || width <= 0 || height <= 0)
		return;
	*x = (int)((long long)*x * width / rc.right);
	*y = (int)((long long)*y * height / rc.bottom);
}

static void console_window_geometry(int width, int panel_h, RECT *rect)
{
	int window_width, window_height, left, top;
	int max_width, max_height, min_width, min_height;
	int legacy_layout = InterlockedCompareExchange(&console_legacy_layout, 0, 0) != 0;
	if (!rect || width <= 0 || panel_h <= 0) {
		if (rect) SetRectEmpty(rect);
		return;
	}
	max_width = width - 32;
	if (max_width < 1) max_width = width;
	max_height = panel_h - (legacy_layout ? 24 : 16);
	if (max_height < 1) max_height = panel_h;
	min_width = legacy_layout ? (max_width < 400 ? max_width : 400) : (max_width < 520 ? max_width : 520);
	min_height = max_height < 220 ? max_height : 220;
	{
		int favorites_min = 36 + CONSOLE_LINE_HEIGHT * 3 + CONSOLE_BOTTOM_PAD +
			CONSOLE_INPUT_PAD + CONSOLE_LINE_HEIGHT + console_bottom_reserved_height() + 16;
		if (favorites_min > min_height) min_height = favorites_min;
		if (min_height > max_height) min_height = max_height;
	}

	if (!full_console && InterlockedCompareExchange(&console_saved_geometry_valid, 0, 0) &&
		!InterlockedCompareExchange(&console_window_positioned, 0, 0) &&
		!InterlockedCompareExchange(&console_window_sized, 0, 0)) {
		int saved_left = (int)(((long long)width * console_saved_left_pct + 5000) / 10000);
		int saved_top = (int)(((long long)panel_h * console_saved_top_pct + 5000) / 10000);
		int saved_width = (int)(((long long)width * console_saved_width_pct + 5000) / 10000);
		int saved_height = (int)(((long long)panel_h * console_saved_height_pct + 5000) / 10000);
		InterlockedExchange(&console_window_left, saved_left);
		InterlockedExchange(&console_window_top, saved_top);
		InterlockedExchange(&console_window_width, saved_width);
		InterlockedExchange(&console_window_height, saved_height);
		InterlockedExchange(&console_window_positioned, 1);
		InterlockedExchange(&console_window_sized, 1);
		InterlockedExchange(&console_saved_geometry_valid, 0);
	}

	if (full_console) {
		window_width = max_width;
		window_height = max_height;
		left = (width - window_width) / 2;
		top = 8;
	} else {
		if (InterlockedCompareExchange(&console_window_sized, 0, 0)) {
			window_width = (int)InterlockedCompareExchange(&console_window_width, 0, 0);
			window_height = (int)InterlockedCompareExchange(&console_window_height, 0, 0);
		} else if (legacy_layout) {
			/* Legacy starts as a half-width, left-aligned console. */
			window_width = width / 2;
			window_height = (panel_h * 42) / 100;
		} else {
			/* Enhanced: an almost-square window centered within the right half. */
			int side = (width * 46) / 100;
			int height_side = (panel_h * 74) / 100;
			int right_half_side = width / 2 - 16;
			int max_side = max_height;
			int min_side;
			if (side > height_side) side = height_side;
			if (side > right_half_side) side = right_half_side;
			if (side > max_side) side = max_side;
			min_side = min_width;
			if (min_side > right_half_side) min_side = right_half_side;
			if (min_side > max_side) min_side = max_side;
			if (side < min_side) side = min_side;
			if (side < 1) side = 1;
			window_width = side;
			window_height = side;
		}
		if (window_width < min_width) window_width = min_width;
		if (window_width > max_width) window_width = max_width;
		if (window_height < min_height) window_height = min_height;
		if (window_height > max_height) window_height = max_height;
		if (InterlockedCompareExchange(&console_window_collapsed, 0, 0))
			window_height = 36;

		if (InterlockedCompareExchange(&console_window_positioned, 0, 0)) {
			left = (int)InterlockedCompareExchange(&console_window_left, 0, 0);
			top = (int)InterlockedCompareExchange(&console_window_top, 0, 0);
		} else if (legacy_layout) {
			left = 16;
			top = 16;
		} else {
			left = width / 2 + (width / 2 - window_width) / 2;
			top = panel_h * 4 / 100;
		}
	}
	if (left < (legacy_layout ? 16 : 8)) left = legacy_layout ? 16 : 8;
	if (left + window_width > width - 8) left = width - 8 - window_width;
	if (left < 0) left = 0;
	if (top < (legacy_layout ? 16 : 8)) top = legacy_layout ? 16 : 8;
	if (top + window_height > panel_h - 8) top = panel_h - 8 - window_height;
	if (top < 0) top = 0;

	rect->left = left;
	rect->top = top;
	rect->right = left + window_width;
	rect->bottom = top + window_height;
}

static int console_window_resize_hit(int x, int y)
{
	RECT rect;
	int width = (int)InterlockedCompareExchange(&console_backbuffer_width, 0, 0);
	int height = (int)InterlockedCompareExchange(&console_panel_height, 0, 0);
	POINT pt = {x, y};
	if (width <= 0 || height <= 0 || full_console ||
		InterlockedCompareExchange(&console_window_collapsed, 0, 0)) return 0;
	console_window_geometry(width, height, &rect);
	/* Resize zones are strictly inside the window perimeter. The popup/context
	   surfaces are tested first so their edges never inherit a resize cursor. */
	if (!PtInRect(&rect, pt))
		return 0;
	if (!TryEnterCriticalSection(&lock))
		return 0;
	{
		int popup_hit = PtInRect(&completion_rect, pt) || PtInRect(&output_toast_rect, pt) ||
			console_watch_chip_hit(x, y, NULL) ||
			(InterlockedCompareExchange(&context_menu_open, 0, 0) && PtInRect(&context_menu_rect, pt));
		LeaveCriticalSection(&lock);
		if (popup_hit)
			return 0;
	}
	if (y >= rect.bottom - 8 && y < rect.bottom) {
		if (x < rect.left + 14) return 5;
		if (x >= rect.right - 14) return 6;
		if (x >= rect.left + 8 && x < rect.right - 8) return 4;
	}
	if (y >= rect.top + 8 && y < rect.bottom - 8) {
		if (x < rect.left + 8) return 2;
		if (x >= rect.right - 8) return 3;
	}
	return 0;
}

static int console_window_title_hit(int x, int y)
{
	RECT rect;
	int width = (int)InterlockedCompareExchange(&console_backbuffer_width, 0, 0);
	int height = (int)InterlockedCompareExchange(&console_panel_height, 0, 0);
	if (width <= 0 || height <= 0) return 0;
	console_window_geometry(width, height, &rect);
	/* All non-button parts of the title bar support click-to-collapse and drag. */
	return y >= rect.top && y < rect.top + 36 &&
		x >= rect.left + 8 && x < rect.right - 8;
}

static void console_window_minimize_toggle(void)
{
	RECT current;
	int width = (int)InterlockedCompareExchange(&console_backbuffer_width, 0, 0);
	int panel_h = (int)InterlockedCompareExchange(&console_panel_height, 0, 0);
	if (width <= 0 || panel_h <= 0)
		return;
	if (full_console)
		InterlockedExchange(&full_console, 0);
	if (InterlockedCompareExchange(&console_window_collapsed, 0, 0)) {
		if (InterlockedCompareExchange(&console_min_restore_valid, 0, 0)) {
			InterlockedExchange(&console_window_left, InterlockedCompareExchange(&console_min_restore_left, 0, 0));
			InterlockedExchange(&console_window_top, InterlockedCompareExchange(&console_min_restore_top, 0, 0));
			InterlockedExchange(&console_window_width, InterlockedCompareExchange(&console_min_restore_width, 0, 0));
			InterlockedExchange(&console_window_height, InterlockedCompareExchange(&console_min_restore_height, 0, 0));
			InterlockedExchange(&console_window_positioned, 1);
			InterlockedExchange(&console_window_sized, 1);
		}
		InterlockedExchange(&console_window_collapsed, 0);
		InterlockedExchange(&console_min_restore_valid, 0);
		console_print_version_once();
	} else {
		console_window_geometry(width, panel_h, &current);
		InterlockedExchange(&console_min_restore_left, current.left);
		InterlockedExchange(&console_min_restore_top, current.top);
		InterlockedExchange(&console_min_restore_width, current.right - current.left);
		InterlockedExchange(&console_min_restore_height, current.bottom - current.top);
		InterlockedExchange(&console_min_restore_valid, 1);
		InterlockedExchange(&console_window_left, current.left);
		InterlockedExchange(&console_window_top, current.top);
		InterlockedExchange(&console_window_width, current.right - current.left);
		InterlockedExchange(&console_window_height, current.bottom - current.top);
		InterlockedExchange(&console_window_positioned, 1);
		InterlockedExchange(&console_window_sized, 1);
		InterlockedExchange(&console_window_collapsed, 1);
	}
	InterlockedExchange(&dirty, 1);
}

static void console_window_maximize_toggle(void)
{
	if (full_console) {
		InterlockedExchange(&full_console, 0);
		console_save_window_geometry();
	} else {
		InterlockedExchange(&console_window_collapsed, 0);
		InterlockedExchange(&console_min_restore_valid, 0);
		InterlockedExchange(&full_console, 1);
		set_open(1);
		console_print_version_once();
	}
	InterlockedExchange(&dirty, 1);
}

static void console_apply_native_cursor_state(HWND h, int hidden)
{
	LONG previous_hidden = InterlockedExchange(&console_last_cursor_hidden, hidden != 0);
	if (hidden) {
		if (previous_hidden != 1)
			SetCursor(NULL);
		return;
	}
	if (previous_hidden == 1)
		InterlockedExchange(&console_last_native_cursor_kind, -1);
	console_update_pointer_cursor_from_os(h);
}

static void console_set_native_cursor_hidden(HWND h, int hidden)
{
	DWORD window_thread;
	if (natboot_active()) // the native renderer has no sprite for the console's own cursor: the system cursor stays
		hidden = 0;
	InterlockedExchange(&console_native_cursor_hidden, hidden != 0);
	if (!h || !IsWindow(h))
		return;
	window_thread = GetWindowThreadProcessId(h, NULL);
	if (window_thread != GetCurrentThreadId()) {
		PostMessageA(h, WM_CONSOLE_CURSOR_VISIBILITY, 0, 0);
		return;
	}
	console_apply_native_cursor_state(h, hidden);
}

static int output_hit(HWND h, int x, int y, int *line, int *column)
{
	(void)h;
	int height = (int)InterlockedCompareExchange(&console_panel_height, 0, 0);
	int backbuffer_height = (int)InterlockedCompareExchange(&console_backbuffer_height, 0, 0);
	int backbuffer_width = (int)InterlockedCompareExchange(&console_backbuffer_width, 0, 0);
	if (height <= 0 || (backbuffer_height > 0 && height > backbuffer_height))
		height = backbuffer_height;
	if (height <= 0 || backbuffer_width <= 0)
		return 0;
	RECT window_rect;
	console_window_geometry(backbuffer_width, height, &window_rect);
	const int header_bottom = window_rect.top + 36;
	const int text_left = window_rect.left + 16;
	const int input_y = window_rect.bottom - console_bottom_reserved_height() - CONSOLE_BOTTOM_PAD - CONSOLE_INPUT_PAD - CONSOLE_LINE_HEIGHT;
	const int input_top_y = input_y - (console_input_line_count(&window_rect) - 1) * CONSOLE_LINE_HEIGHT;
	const int output_gap = 8;
	const int output_bottom = input_top_y - output_gap;
	const int output_top = header_bottom + 9;
	int rows = (output_bottom - output_top) / CONSOLE_LINE_HEIGHT;
	if (y < output_top || y >= output_bottom || x < text_left || rows <= 0 || x >= window_rect.right - 12)
		return 0;
	/* Output row zero is the newest line immediately above the input strip. */
	int row = (output_bottom - 1 - y) / CONSOLE_LINE_HEIGHT;
	if (row < 0 || row >= rows)
		return 0;
	EnterCriticalSection(&lock);
	{
		int offset = scroll + row;
		int seq = line_count - 1 - offset;
		if (seq < 0 || seq >= line_count) {
			LeaveCriticalSection(&lock);
			return 0;
		}
		int physical = (line_next - 1 - offset + MAX_LINES * 2) % MAX_LINES;
		int len = line_visible_length_cp(lines[physical], line_codepage[physical] ? line_codepage[physical] : console_codepage());
		int cell_width = (int)InterlockedCompareExchange(&console_font_cell_width, 0, 0);
		int col;
		if (cell_width < 1) cell_width = 9;
		col = (x - text_left) / cell_width;
		if (col < 0) col = 0;
		if (col > len) col = len;
		LeaveCriticalSection(&lock);
		*line = seq;
		*column = col;
		return 1;
	}
}


static int console_window_drag_blank_hit(int x, int y)
{
	RECT rect;
	int width = (int)InterlockedCompareExchange(&console_backbuffer_width, 0, 0);
	int panel_h = (int)InterlockedCompareExchange(&console_panel_height, 0, 0);
	int input_y, input_top_y, output_top, output_bottom;
	int line = -1, column = 0;
	if (width <= 0 || panel_h <= 0 || console_window_collapsed || full_console)
		return 0;
	if (!TryEnterCriticalSection(&lock))
		return 0;
	{
		POINT pt = {x, y};
		int popup_hit = PtInRect(&completion_rect, pt) || PtInRect(&output_toast_rect, pt) ||
			(InterlockedCompareExchange(&context_menu_open, 0, 0) && PtInRect(&context_menu_rect, pt));
		LeaveCriticalSection(&lock);
		if (popup_hit) return 0;
	}
	console_window_geometry(width, panel_h, &rect);
	input_y = rect.bottom - console_bottom_reserved_height() - CONSOLE_BOTTOM_PAD - CONSOLE_INPUT_PAD - CONSOLE_LINE_HEIGHT;
	input_top_y = input_y - (console_input_line_count(&rect) - 1) * CONSOLE_LINE_HEIGHT;
	output_top = rect.top + 36 + 9;
	output_bottom = input_top_y - 8;
	if (y >= output_top && y < output_bottom && x >= rect.left + 8 && x < rect.right - 8) {
		if (!output_hit(game_window, x, y, &line, &column))
			return 1;
		EnterCriticalSection(&lock);
		if (line >= 0 && line < line_count) {
			int physical = (line_next - line_count + line + MAX_LINES * 2) % MAX_LINES;
			int length = line_visible_text_columns_cp(lines[physical], line_codepage[physical] ? line_codepage[physical] : console_codepage());
			int blank = column >= length;
			LeaveCriticalSection(&lock);
			return blank;
		}
		LeaveCriticalSection(&lock);
		return 1;
	}
	/* Only the real status strip is a drag handle; favorites stay informational except for their hover-revealed unpin glyph. */
	{
		int status_top = rect.bottom - console_bottom_reserved_height();
		if (y >= status_top && y < status_top + CONSOLE_STATUS_HEIGHT &&
			x >= rect.left + 2 && x < rect.right - 2) {
			int text_width = (int)InterlockedCompareExchange(&console_status_text_width, 0, 0);
			int text_left = rect.left + 12;
			return x < text_left || x >= text_left + text_width;
		}
	}
	return 0;
}


static int console_context_menu_hit(int x, int y, int *index)
{
	POINT pt = {x, y};
	int mode, row;
	if (!InterlockedCompareExchange(&context_menu_open, 0, 0) || !PtInRect(&context_menu_rect, pt))
		return 0;
	mode = (int)InterlockedCompareExchange(&context_menu_mode, 0, 0);
	row = (y - context_menu_rect.top - 4) / CONSOLE_CONTEXT_ITEM_H;
	if (row < 0 || row >= context_menu_count(mode))
		return 0;
	if (index) *index = row;
	return 1;
}

static void console_context_menu_open_at(HWND h, int x, int y)
{
	int width = (int)InterlockedCompareExchange(&console_backbuffer_width, 0, 0);
	int panel_h = (int)InterlockedCompareExchange(&console_panel_height, 0, 0);
	int mode = CONSOLE_CONTEXT_INPUT;
	int input_index = 0, line = 0, column = 0;
	RECT rect;
	if (width <= 0 || panel_h <= 0)
		return;
	if (console_input_hit(x, y, &input_index)) {
		SetFocus(h);
		EnterCriticalSection(&lock);
		output_context_line = -1;
		if (!has_selection() || input_index < (selection_anchor < cursor ? selection_anchor : cursor) ||
			input_index > (selection_anchor > cursor ? selection_anchor : cursor)) {
			cursor = input_index;
			clear_selection();
		}
		LeaveCriticalSection(&lock);
	} else if (output_hit(h, x, y, &line, &column)) {
		mode = CONSOLE_CONTEXT_OUTPUT;
		EnterCriticalSection(&lock);
		output_context_line = line;
		LeaveCriticalSection(&lock);
	} else {
		return;
	}
	{
		int preferred_width = mode == CONSOLE_CONTEXT_INPUT ? CONSOLE_CONTEXT_INPUT_WIDTH : CONSOLE_CONTEXT_OUTPUT_WIDTH;
		int menu_width = width > 8 ? (width - 8 < preferred_width ? width - 8 : preferred_width) : width;
		if (menu_width < 1) return;
		rect.left = x;
		rect.top = y;
		rect.right = x + menu_width;
		rect.bottom = y + 8 + context_menu_count(mode) * CONSOLE_CONTEXT_ITEM_H;
		if (rect.right > width - 4) {
			rect.left = width - 4 - menu_width;
			rect.right = width - 4;
		}
		if (rect.left < 4) {
			rect.left = 4;
			rect.right = rect.left + menu_width;
		}
	}
	{
		int menu_height = 8 + context_menu_count(mode) * CONSOLE_CONTEXT_ITEM_H;
		int max_top = panel_h - 4 - menu_height;
		if (max_top < 4) max_top = 4;
		if (rect.top > max_top) rect.top = max_top;
		if (rect.top < 4) rect.top = 4;
		rect.bottom = rect.top + menu_height;
	}
	EnterCriticalSection(&lock);
	context_menu_rect = rect;
	InterlockedExchange(&context_menu_mode, mode);
	InterlockedExchange(&context_menu_selected, -1);
	InterlockedExchange(&context_menu_open, 1);
	InterlockedExchange(&dirty, 1);
	LeaveCriticalSection(&lock);
}

static void console_virtual_mouse_position(HWND h, int *x, int *y)
{
	RECT rc;
	int mx = (int)InterlockedCompareExchange(&virtual_mouse_x, 0, 0);
	int my = (int)InterlockedCompareExchange(&virtual_mouse_y, 0, 0);
	if (mx < 0 || my < 0) {
		if (GetClientRect(h, &rc) && rc.right > 0 && rc.bottom > 0) {
			mx = rc.right / 2;
			my = rc.bottom / 2;
		} else {
			mx = my = 0;
		}
		InterlockedExchange(&virtual_mouse_x, mx);
		InterlockedExchange(&virtual_mouse_y, my);
	}
	if (x) *x = mx;
	if (y) *y = my;
}

static void console_virtual_mouse_move(HWND h, int dx, int dy, int absolute, int abs_x, int abs_y, int virtual_desktop)
{
	RECT rc;
	int x, y;
	if (!GetClientRect(h, &rc) || rc.right <= 0 || rc.bottom <= 0)
		return;
	console_virtual_mouse_position(h, &x, &y);
	if (absolute) {
		int sx = virtual_desktop ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0;
		int sy = virtual_desktop ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0;
		int sw = virtual_desktop ? GetSystemMetrics(SM_CXVIRTUALSCREEN) : GetSystemMetrics(SM_CXSCREEN);
		int sh = virtual_desktop ? GetSystemMetrics(SM_CYVIRTUALSCREEN) : GetSystemMetrics(SM_CYSCREEN);
		POINT pt;
		pt.x = sx + (int)((long long)abs_x * (sw > 1 ? sw - 1 : 1) / 65535);
		pt.y = sy + (int)((long long)abs_y * (sh > 1 ? sh - 1 : 1) / 65535);
		if (ScreenToClient(h, &pt)) {
			x = (int)pt.x;
			y = (int)pt.y;
		}
	} else {
		int64_t next_x = (int64_t)x + dx;
		int64_t next_y = (int64_t)y + dy;
		x = next_x < INT32_MIN ? INT32_MIN : next_x > INT32_MAX ? INT32_MAX : (int)next_x;
		y = next_y < INT32_MIN ? INT32_MIN : next_y > INT32_MAX ? INT32_MAX : (int)next_y;
	}
	/* The software pointer tracks the whole game client, not just the console or dropdown. */
	if (x < 0) x = 0;
	if (x >= rc.right) x = rc.right - 1;
	if (y < 0) y = 0;
	if (y >= rc.bottom) y = rc.bottom - 1;
	InterlockedExchange(&virtual_mouse_x, x);
	InterlockedExchange(&virtual_mouse_y, y);
	InterlockedExchange(&virtual_mouse_raw_seen, 1);
}
static int console_input_columns(const RECT *rect)
{
	int cell_width = (int)InterlockedCompareExchange(&console_font_cell_width, 0, 0);
	int available;
	if (cell_width < 1) cell_width = 9;
	available = rect->right - rect->left - (int)InterlockedCompareExchange(&console_input_left_offset, 0, 0) - 18;
	if (available < cell_width) available = cell_width;
	return max(1, available / cell_width);
}

static int console_input_line_count(const RECT *rect)
{
	int columns = console_input_columns(rect);
	return max(1, ((int)InterlockedCompareExchange((volatile LONG *)&input_len, 0, 0) + columns - 1) / columns);
}

static int console_input_index_at(int x, int y)
{
	RECT rect;
	int width = (int)InterlockedCompareExchange(&console_backbuffer_width, 0, 0);
	int height = (int)InterlockedCompareExchange(&console_panel_height, 0, 0);
	int columns, cell_width, input_y, row, col, index, length, lines;
	if (width <= 0 || height <= 0) return 0;
	console_window_geometry(width, height, &rect);
	columns = console_input_columns(&rect);
	cell_width = (int)InterlockedCompareExchange(&console_font_cell_width, 0, 0);
	if (cell_width < 1) cell_width = 9;
	input_y = rect.bottom - console_bottom_reserved_height() - CONSOLE_BOTTOM_PAD - CONSOLE_INPUT_PAD - CONSOLE_LINE_HEIGHT;
	EnterCriticalSection(&lock);
	length = input_len;
	lines = max(1, (length + columns - 1) / columns);
	LeaveCriticalSection(&lock);
	row = (y - (input_y - (lines - 1) * CONSOLE_LINE_HEIGHT)) / CONSOLE_LINE_HEIGHT;
	col = (x - (rect.left + (int)InterlockedCompareExchange(&console_input_left_offset, 0, 0)) + cell_width / 2) / cell_width;
	if (row < 0) row = 0;
	if (row >= lines) row = lines - 1;
	if (col < 0) col = 0;
	if (col > columns) col = columns;
	index = row * columns + col;
	if (index > length) index = length;
	return index;
}

static int console_input_hit(int x, int y, int *index)
{
	RECT rect;
	int width = (int)InterlockedCompareExchange(&console_backbuffer_width, 0, 0);
	int height = (int)InterlockedCompareExchange(&console_panel_height, 0, 0);
	int input_y, lines, top_y;
	if (width <= 0 || height <= 0) return 0;
	console_window_geometry(width, height, &rect);
	lines = console_input_line_count(&rect);
	input_y = rect.bottom - console_bottom_reserved_height() - CONSOLE_BOTTOM_PAD - CONSOLE_INPUT_PAD - CONSOLE_LINE_HEIGHT;
	top_y = input_y - (lines - 1) * CONSOLE_LINE_HEIGHT;
	if (y < top_y - 5 || y >= input_y + CONSOLE_LINE_HEIGHT + 5 ||
		x < rect.left + 40 || x >= rect.right - 10 || console_header_pause_hit(x, y))
		return 0;
	if (index) *index = console_input_index_at(x, y);
	return 1;
}

static void console_window_drag_update(int x, int y)
{
	LONG mode = InterlockedCompareExchange(&console_window_drag_mode, 0, 0);
	int width = (int)InterlockedCompareExchange(&console_backbuffer_width, 0, 0);
	int panel_h = (int)InterlockedCompareExchange(&console_panel_height, 0, 0);
	int dx, dy, left, top, new_width, new_height;
	int legacy = InterlockedCompareExchange(&console_legacy_layout, 0, 0) != 0;
	int min_width, min_height, max_width, max_height, fixed_right;
	if (!mode || mode == 7 || width <= 0 || panel_h <= 0) return;
	dx = x - console_drag_anchor_x;
	dy = y - console_drag_anchor_y;
	left = console_drag_start_left;
	top = console_drag_start_top;
	new_width = console_drag_start_width;
	new_height = console_drag_start_height;
	if (mode == 1) {
		left += dx;
		top += dy;
		if (left < (legacy ? 16 : 8)) left = legacy ? 16 : 8;
		if (left + new_width > width - 8) left = width - 8 - new_width;
		if (top < 8) top = 8;
		if (top + new_height > panel_h - 8) top = panel_h - 8 - new_height;
		if (left < 0) left = 0;
		if (top < 0) top = 0;
		InterlockedExchange(&console_window_left, left);
		InterlockedExchange(&console_window_top, top);
		if (console_window_collapsed) {
			InterlockedExchange(&console_min_restore_left, left);
			InterlockedExchange(&console_min_restore_top, top);
		}
		InterlockedExchange(&console_window_positioned, 1);
	} else {
		fixed_right = console_drag_start_left + console_drag_start_width;
		if (mode == 2 || mode == 5) { left += dx; new_width -= dx; }
		if (mode == 3 || mode == 6) new_width += dx;
		if (mode == 4 || mode == 5 || mode == 6) new_height += dy;
		min_width = legacy ? 400 : 520;
		min_height = 220;
		if (min_width > width - 16) min_width = width - 16;
		if (min_height > panel_h - 16) min_height = panel_h - 16;
		if (min_width < 1) min_width = 1;
		if (min_height < 1) min_height = 1;
		if (mode == 2 || mode == 5) {
			int min_left = legacy ? 16 : 8;
			max_width = fixed_right - min_left;
		} else {
			max_width = width - 8 - left;
		}
		max_height = panel_h - 8 - top;
		if (max_width < min_width) max_width = min_width;
		if (max_height < min_height) max_height = min_height;
		if (new_width < min_width) new_width = min_width;
		if (new_width > max_width) new_width = max_width;
		if (new_height < min_height) new_height = min_height;
		if (new_height > max_height) new_height = max_height;
		if (mode == 2 || mode == 5) left = fixed_right - new_width;
		if (mode == 2 || mode == 5) {
			int min_left = legacy ? 16 : 8;
			if (left < min_left) { left = min_left; new_width = fixed_right - left; }
		}
		InterlockedExchange(&console_window_left, left);
		InterlockedExchange(&console_window_top, top);
		InterlockedExchange(&console_window_width, new_width);
		InterlockedExchange(&console_window_height, new_height);
		InterlockedExchange(&console_window_positioned, 1);
		InterlockedExchange(&console_window_sized, 1);
	}
	InterlockedExchange(&dirty, 1);
}


static void console_mouse_move_at(HWND h, int x, int y)
{
	int idx = -1, selecting, input_dragging;
	int hover_button = console_header_button_at(x, y);
	if (hover_button != (int)InterlockedCompareExchange(&console_header_hover_button, 0, 0)) {
		InterlockedExchange(&console_header_hover_button, hover_button);
		console_header_hover_tick = GetTickCount();
		InterlockedExchange(&dirty, 1);
	}
	if (console_scrollbar_drag_mode) {
		int track_height, thumb_height, travel, max_scroll;
		LONG mode = InterlockedCompareExchange(&console_scrollbar_drag_mode, 0, 0);
		EnterCriticalSection(&lock);
		if (mode == 1) {
			track_height = output_scroll_track.bottom - output_scroll_track.top;
			thumb_height = output_scroll_thumb.bottom - output_scroll_thumb.top;
			travel = track_height - thumb_height;
			max_scroll = output_scroll_max;
			if (travel > 0 && max_scroll > 0) {
				int next = console_scrollbar_drag_start -
					(int)((long long)(y - console_scrollbar_drag_y) * max_scroll / travel);
				if (next < 0) next = 0;
				if (next > max_scroll) next = max_scroll;
				if (scroll != next) { scroll = next; InterlockedExchange(&dirty, 1); }
			}
		} else if (mode == 2) {
			track_height = completion_scroll_track.bottom - completion_scroll_track.top;
			thumb_height = completion_scroll_thumb.bottom - completion_scroll_thumb.top;
			travel = track_height - thumb_height;
			max_scroll = completion_scroll_max;
			if (travel > 0 && max_scroll > 0) {
				int next = console_scrollbar_drag_start +
					(int)((long long)(y - console_scrollbar_drag_y) * max_scroll / travel);
				if (next < 0) next = 0;
				if (next > max_scroll) next = max_scroll;
				if (completion_scroll != next) { completion_scroll = next; InterlockedExchange(&dirty, 1); }
			}
		}
		LeaveCriticalSection(&lock);
		return;
	}
	if (InterlockedCompareExchange(&context_menu_open, 0, 0)) {
		int hover = console_context_menu_hit(x, y, &idx) ? idx : -1;
		EnterCriticalSection(&lock);
		if (context_menu_open && context_menu_selected != hover) {
			InterlockedExchange(&context_menu_selected, hover);
			InterlockedExchange(&dirty, 1);
		}
		LeaveCriticalSection(&lock);
		return;
	}
	{
		LONG drag_mode = InterlockedCompareExchange(&console_window_drag_mode, 0, 0);
		if (drag_mode == 7 && (abs(x - console_drag_anchor_x) > max(8, max(GetSystemMetrics(SM_CXDRAG), GetSystemMetrics(SM_CYDRAG))) || abs(y - console_drag_anchor_y) > max(8, max(GetSystemMetrics(SM_CXDRAG), GetSystemMetrics(SM_CYDRAG))))) {
			if (!full_console) {
				InterlockedExchange(&console_window_drag_mode, 1);
				drag_mode = 1;
			} else {
				/* A drag on a maximized title is not a click-to-collapse. */
				InterlockedExchange(&console_window_drag_mode, 0);
				return;
			}
		}
		if (drag_mode) {
			if (drag_mode != 7) console_window_drag_update(x, y);
			return;
		}
	}
	EnterCriticalSection(&lock);
	selecting = output_selecting;
	input_dragging = input_selecting;
	if (!input_dragging && completion_count > 1 && completion_hit(x, y, &idx) && idx != completion_selected) {
		completion_selected = idx;
		InterlockedExchange(&dirty, 1);
	}
	LeaveCriticalSection(&lock);
	if (input_dragging) {
		int index = console_input_index_at(x, y);
		EnterCriticalSection(&lock);
		if (input_selecting && cursor != index) {
			cursor = index;
			InterlockedExchange(&dirty, 1);
		}
		LeaveCriticalSection(&lock);
		return;
	}
	if (selecting) {
		int line, col;
		if (output_hit(h, x, y, &line, &col)) {
			EnterCriticalSection(&lock);
			if (output_selecting && (output_sel_b_line != line || output_sel_b_col != col)) {
				output_sel_b_line = line;
				output_sel_b_col = col;
				InterlockedExchange(&dirty, 1);
			}
			LeaveCriticalSection(&lock);
		}
	}
}

static int console_input_word_char(unsigned char c)
{
	return isalnum(c) || c == '_' || c >= 0x80;
}

static void console_input_select_word(int index)
{
	int first, last;
	if (input_len <= 0) return;
	if (index >= input_len) index = input_len - 1;
	if (index < 0) index = 0;
	if (!console_input_word_char((unsigned char)input[index]) && index > 0 &&
		console_input_word_char((unsigned char)input[index - 1]))
		index--;
	if (!console_input_word_char((unsigned char)input[index])) {
		first = index;
		last = index + 1;
	} else {
		first = index;
		last = index + 1;
		while (first > 0 && console_input_word_char((unsigned char)input[first - 1])) first--;
		while (last < input_len && console_input_word_char((unsigned char)input[last])) last++;
	}
	selection_anchor = first;
	cursor = last;
	input_selecting = 0;
	input_undo_last_group = 0;
	InterlockedExchange(&dirty, 1);
}

static void console_input_double_click_at(int x, int y)
{
	int index;
	if (!console_input_hit(x, y, &index)) return;
	EnterCriticalSection(&lock);
	console_input_select_word(index);
	input_last_click_tick = 0;
	LeaveCriticalSection(&lock);
}

static void console_mouse_down_at(HWND h, int x, int y, int capture_legacy)
{
	int idx = -1, completion;
	RECT window_rect;
	if (InterlockedExchange(&console_pending_title_click, 0)) {
		DWORD elapsed = GetTickCount() - console_pending_title_click_tick;
		int dx = x - console_pending_title_click_x, dy = y - console_pending_title_click_y;
		int on_header_button = console_header_button_at(x, y);
		int title_body = console_window_title_hit(x, y) && !on_header_button;
		if (elapsed <= CONSOLE_TITLE_CLICK_DELAY_MS && abs(dx) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
			abs(dy) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2 && title_body) {
			if (h) KillTimer(h, CONSOLE_TITLE_CLICK_TIMER);
			console_window_maximize_toggle();
			return;
		}
		if (h) KillTimer(h, CONSOLE_TITLE_CLICK_TIMER);
		if (!on_header_button)
			console_window_minimize_toggle();
	}
	{
		POINT toast_pt = {x, y};
		if (PtInRect(&output_toast_close_rect, toast_pt) || PtInRect(&output_toast_rect, toast_pt)) {
			EnterCriticalSection(&lock);
			if (PtInRect(&output_toast_close_rect, toast_pt)) {
				output_toast_count = 0;
				output_toast_expires = output_toast_capture_until = 0;
			} else {
				scroll = 0;
				output_toast_count = 0;
				output_toast_expires = output_toast_capture_until = 0;
			}
			SetRectEmpty(&output_toast_rect);
			SetRectEmpty(&output_toast_close_rect);
			InterlockedExchange(&dirty, 1);
			LeaveCriticalSection(&lock);
			return;
		}
	}
	if (InterlockedCompareExchange(&context_menu_open, 0, 0)) {
		int mode = (int)InterlockedCompareExchange(&context_menu_mode, 0, 0);
		if (console_context_menu_hit(x, y, &idx)) {
			console_context_menu_execute(mode, idx);
			return;
		}
		EnterCriticalSection(&lock);
		InterlockedExchange(&context_menu_open, 0);
		InterlockedExchange(&context_menu_selected, -1);
		InterlockedExchange(&dirty, 1);
		LeaveCriticalSection(&lock);
	}
	int backbuffer_width = (int)InterlockedCompareExchange(&console_backbuffer_width, 0, 0);
	int panel_h = (int)InterlockedCompareExchange(&console_panel_height, 0, 0);
	int resize_hit;
	EnterCriticalSection(&lock);
	if (PtInRect(&output_scroll_track, (POINT){x, y}) && output_scroll_max > 0) {
		int travel = (output_scroll_track.bottom - output_scroll_track.top) -
			(output_scroll_thumb.bottom - output_scroll_thumb.top);
		InterlockedExchange(&console_scrollbar_drag_mode, 1);
		console_scrollbar_drag_y = y;
		console_scrollbar_drag_start = scroll;
		if (!PtInRect(&output_scroll_thumb, (POINT){x, y}) && travel > 0) {
			int page = output_scroll_page > 1 ? output_scroll_page - 1 : 1;
			scroll += y < output_scroll_thumb.top ? page : -page;
			if (scroll < 0) scroll = 0;
			if (scroll > output_scroll_max) scroll = output_scroll_max;
			console_scrollbar_drag_start = scroll;
		}
		InterlockedExchange(&dirty, 1);
		LeaveCriticalSection(&lock);
		if (capture_legacy && GetCapture() != h) {
			HWND previous = GetCapture(); SetCapture(h);
			if (GetCapture() == h) { console_previous_capture = previous; InterlockedExchange(&console_capture_owned, 1); }
		}
		return;
	}
	if (PtInRect(&completion_scroll_track, (POINT){x, y}) && completion_scroll_max > 0) {
		int travel = (completion_scroll_track.bottom - completion_scroll_track.top) -
			(completion_scroll_thumb.bottom - completion_scroll_thumb.top);
		InterlockedExchange(&console_scrollbar_drag_mode, 2);
		console_scrollbar_drag_y = y;
		console_scrollbar_drag_start = completion_scroll;
		if (!PtInRect(&completion_scroll_thumb, (POINT){x, y}) && travel > 0) {
			int page = completion_scroll_page > 1 ? completion_scroll_page - 1 : 1;
			completion_scroll += y < completion_scroll_thumb.top ? -page : page;
			if (completion_scroll < 0) completion_scroll = 0;
			if (completion_scroll > completion_scroll_max) completion_scroll = completion_scroll_max;
			console_scrollbar_drag_start = completion_scroll;
		}
		InterlockedExchange(&dirty, 1);
		LeaveCriticalSection(&lock);
		if (capture_legacy && GetCapture() != h) {
			HWND previous = GetCapture(); SetCapture(h);
			if (GetCapture() == h) { console_previous_capture = previous; InterlockedExchange(&console_capture_owned, 1); }
		}
		return;
	}
	LeaveCriticalSection(&lock);
	{
		char pin_name[COMPLETION_NAME_LEN] = "";
		int pinned = 0, pin_index = -1;
		EnterCriticalSection(&lock);
		if (completion_pin_hit(x, y, &pin_index) && pin_index >= 0 && pin_index < completion_count &&
			completion_items[pin_index].is_dvar) {
			lstrcpynA(pin_name, completion_items[pin_index].name, sizeof(pin_name));
			pinned = console_name_in_list(console_watch_names, console_watch_count_read(), pin_name) >= 0;
		}
		LeaveCriticalSection(&lock);
		if (pin_name[0]) {
			(void)console_watch_change(pin_name, pinned);
			InterlockedExchange(&dirty, 1);
			return;
		}
	}
	if (console_watch_unpin_hit(x, y, NULL)) {
		(void)console_watch_remove_at(x, y);
		return;
	}
	if (console_watch_chip_hit(x, y, NULL))
		return; /* Favorite labels are informational; only the hover-revealed unpin icon is clickable. */
	if (console_header_pause_hit(x, y)) {
		InterlockedExchange(&console_button_pressed, 4);
		InterlockedExchange(&console_pause_button_pressed, 1);
		InterlockedExchange(&dirty, 1);
		return;
	}
	if (console_header_close_hit(x, y)) {
		InterlockedExchange(&console_button_pressed, 3);
		InterlockedExchange(&dirty, 1);
		return;
	}
	if (console_header_min_hit(x, y)) {
		InterlockedExchange(&console_button_pressed, 1);
		InterlockedExchange(&dirty, 1);
		return;
	}
	if (console_header_max_hit(x, y)) {
		InterlockedExchange(&console_button_pressed, 2);
		InterlockedExchange(&dirty, 1);
		return;
	}
	if (console_header_language_hit(x, y)) {
		InterlockedExchange(&console_button_pressed, 6);
		InterlockedExchange(&dirty, 1);
		return;
	}
	if (console_header_layout_hit(x, y)) {
		InterlockedExchange(&console_button_pressed, 5);
		InterlockedExchange(&dirty, 1);
		return;
	}
	resize_hit = console_window_resize_hit(x, y);
	{
		int title_hit = console_window_title_hit(x, y);
		int blank_drag_hit = !resize_hit && !title_hit && console_window_drag_blank_hit(x, y);
		if (backbuffer_width > 0 && panel_h > 0 && (resize_hit || title_hit || blank_drag_hit)) {
			console_window_geometry(backbuffer_width, panel_h, &window_rect);
		console_drag_anchor_x = x;
		console_drag_anchor_y = y;
		console_drag_start_left = window_rect.left;
		console_drag_start_top = window_rect.top;
		console_drag_start_width = window_rect.right - window_rect.left;
		console_drag_start_height = window_rect.bottom - window_rect.top;
		if (!full_console) {
			InterlockedExchange(&console_window_left, window_rect.left);
			InterlockedExchange(&console_window_top, window_rect.top);
			InterlockedExchange(&console_window_width, console_drag_start_width);
			InterlockedExchange(&console_window_height, console_drag_start_height);
			InterlockedExchange(&console_window_positioned, 1);
			InterlockedExchange(&console_window_sized, 1);
		}
		InterlockedExchange(&console_window_drag_mode, resize_hit ? resize_hit : (blank_drag_hit ? 1 : 7));
		if (capture_legacy && GetCapture() != h) {
			HWND previous = GetCapture();
			SetCapture(h);
			if (GetCapture() == h) {
				console_previous_capture = previous;
				InterlockedExchange(&console_capture_owned, 1);
			}
		}
		InterlockedExchange(&dirty, 1);
		return;
		}
	}
	EnterCriticalSection(&lock);
	completion = completion_hit(x, y, &idx);
	if (completion)
		completion_choose(idx);
	LeaveCriticalSection(&lock);
	if (!completion) {
		int input_index, line, col;
		if (console_input_hit(x, y, &input_index)) {
			DWORD now = GetTickCount();
			int double_click = input_last_click_tick &&
				(DWORD)(now - input_last_click_tick) <= GetDoubleClickTime() &&
				abs(x - input_last_click_x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
				abs(y - input_last_click_y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
			SetFocus(h);
			if (capture_legacy && GetCapture() != h) {
				HWND previous = GetCapture();
				SetCapture(h);
				if (GetCapture() == h) {
					console_previous_capture = previous;
					InterlockedExchange(&console_capture_owned, 1);
				}
			}
			EnterCriticalSection(&lock);
			output_sel_a_line = output_sel_b_line = -1;
			output_selecting = 0;
			if (double_click) {
				console_input_select_word(input_index);
				input_last_click_tick = 0;
			} else {
				selection_anchor = cursor = input_index;
				input_undo_last_group = 0;
				input_selecting = 1;
				input_last_click_tick = now;
				input_last_click_x = x;
				input_last_click_y = y;
			}
			hide_completion();
			InterlockedExchange(&dirty, 1);
			LeaveCriticalSection(&lock);
		} else if (output_hit(h, x, y, &line, &col)) {
			DWORD now = GetTickCount();
			int triple = output_last_click_tick && output_last_click_seq == line &&
				(DWORD)(now - output_last_click_tick) <= GetDoubleClickTime() &&
				abs(x - output_last_click_x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
				abs(y - output_last_click_y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2 &&
				output_click_count >= 2;
			if (output_last_click_tick && output_last_click_seq == line &&
				(DWORD)(now - output_last_click_tick) <= GetDoubleClickTime() &&
				abs(x - output_last_click_x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
				abs(y - output_last_click_y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2)
				output_click_count++;
			else output_click_count = 1;
			output_last_click_tick = now;
			output_last_click_seq = line;
			output_last_click_x = x; output_last_click_y = y;
			SetFocus(h);
			if (capture_legacy && GetCapture() != h) {
				HWND previous = GetCapture();
				SetCapture(h);
				if (GetCapture() == h) {
					console_previous_capture = previous;
					InterlockedExchange(&console_capture_owned, 1);
				}
			}
			EnterCriticalSection(&lock);
			clear_selection();
			input_selecting = 0;
			if (triple) {
				int physical = (line_next - line_count + line + MAX_LINES * 2) % MAX_LINES;
				output_selecting = 0;
				output_sel_a_line = output_sel_b_line = line;
				output_sel_a_col = 0;
				output_sel_b_col = line_visible_length_cp(lines[physical],
					line_codepage[physical] ? line_codepage[physical] : console_codepage());
				output_click_count = 0;
			} else {
				output_selecting = 1;
				output_sel_a_line = output_sel_b_line = line;
				output_sel_a_col = output_sel_b_col = col;
			}
			InterlockedExchange(&dirty, 1);
			LeaveCriticalSection(&lock);
		} else {
			EnterCriticalSection(&lock);
			output_sel_a_line = output_sel_b_line = -1;
			output_selecting = 0;
			input_selecting = 0;
			clear_selection();
			InterlockedExchange(&dirty, 1);
			LeaveCriticalSection(&lock);
		}
	}
}

static void console_mouse_up(void)
{
	LONG pressed_button = InterlockedCompareExchange(&console_button_pressed, 0, 0);
	int release_button = 0;
	int release_x = (int)InterlockedCompareExchange(&virtual_mouse_x, 0, 0);
	int release_y = (int)InterlockedCompareExchange(&virtual_mouse_y, 0, 0);
	LONG drag_mode = InterlockedExchange(&console_window_drag_mode, 0);
	if (pressed_button > 0 && game_window && release_x >= 0 && release_y >= 0) {
		console_mouse_to_backbuffer(game_window, &release_x, &release_y);
		release_button = console_header_button_at(release_x, release_y);
	}
	InterlockedExchange(&console_pause_button_pressed, 0);
	InterlockedExchange(&console_button_pressed, 0);
	InterlockedExchange(&console_scrollbar_drag_mode, 0);
	InterlockedExchange(&dirty, 1);
	if (drag_mode == 7) {
		console_pending_title_click_tick = GetTickCount();
		console_pending_title_click_x = console_drag_anchor_x;
		console_pending_title_click_y = console_drag_anchor_y;
		InterlockedExchange(&console_pending_title_click, 1);
		if (!game_window || !SetTimer(game_window, CONSOLE_TITLE_CLICK_TIMER, CONSOLE_TITLE_CLICK_DELAY_MS, NULL)) {
			InterlockedExchange(&console_pending_title_click, 0);
			console_window_minimize_toggle();
		}
	} else if (drag_mode >= 1 && drag_mode <= 6)
		console_save_window_geometry();
	EnterCriticalSection(&lock);
	output_selecting = 0;
	input_selecting = 0;
	LeaveCriticalSection(&lock);
	console_restore_capture();
	if (pressed_button > 0 && pressed_button == release_button)
		console_header_button_activate((int)pressed_button);
	if (InterlockedCompareExchange(&console_pause_capture_release_pending, 0, 0))
		console_pause_capture_release(game_window, 1);
	else if (is_open && InterlockedCompareExchange(&console_pause_mode, 0, 0) &&
		InterlockedCompareExchange(&console_window_active, 0, 0))
		console_pause_capture_acquire(game_window);
}

static int console_output_page_lines(void)
{
	RECT rect;
	int width = (int)InterlockedCompareExchange(&console_backbuffer_width, 0, 0);
	int panel_h = (int)InterlockedCompareExchange(&console_panel_height, 0, 0);
	int input_y, output_top, rows, input_top_y;
	if (width <= 0 || panel_h <= 0)
		return 8;
	console_window_geometry(width, panel_h, &rect);
	input_y = rect.bottom - console_bottom_reserved_height() - CONSOLE_BOTTOM_PAD - CONSOLE_INPUT_PAD - CONSOLE_LINE_HEIGHT;
	input_top_y = input_y - (console_input_line_count(&rect) - 1) * CONSOLE_LINE_HEIGHT;
	output_top = rect.top + 36 + 9;
	rows = (input_top_y - 8 - output_top) / CONSOLE_LINE_HEIGHT;
	if (rows < 1) rows = 1;
	return rows;
}

static int console_output_find_in_line(const char *source, const char *query, int *match_at)
{
	char plain[LINE_LEN];
	int used = 0;
	if (!source || !query || !*query) return 0;
	for (const char *p = source; *p && used < LINE_LEN - 1; p++) {
		if (*p == '^' && p[1] >= '0' && p[1] <= '9') { p++; continue; }
		plain[used++] = *p;
	}
	plain[used] = 0;
	{
		int qn = (int)strlen(query);
		for (int i = 0; i + qn <= used; i++) {
			if (!_strnicmp(plain + i, query, (size_t)qn)) {
				if (match_at) *match_at = i;
				return qn;
			}
		}
	}
	return 0;
}

static void console_output_search_step(int direction)
{
	int count, found = -1, start, rows;
	char query[OUTPUT_SEARCH_LEN];
	if (direction != -1) direction = 1;
	EnterCriticalSection(&lock);
	lstrcpynA(query, output_search_query, sizeof(query));
	count = line_count;
	if (!query[0] || count <= 0) {
		output_search_match_seq = -1;
		LeaveCriticalSection(&lock);
		MessageBeep(MB_ICONEXCLAMATION);
		return;
	}
	if (output_search_match_seq >= 0)
		start = output_search_match_seq + direction;
	else
		start = direction > 0 ? 0 : count - 1;
	for (int i = 0; i < count; i++) {
		int seq = start + direction * i;
		while (seq < 0) seq += count;
		while (seq >= count) seq -= count;
		int physical = (line_next - line_count + seq + MAX_LINES * 2) % MAX_LINES;
		if (console_output_find_in_line(lines[physical], query, NULL)) {
			found = seq;
			break;
		}
	}
	if (found >= 0) {
		output_search_match_seq = found;
		rows = console_output_page_lines();
		{
			int offset = line_count - 1 - found;
			int next_scroll = offset - rows / 2;
			int max_scroll = line_count > rows ? line_count - rows : 0;
			if (next_scroll < 0) next_scroll = 0;
			if (next_scroll > max_scroll) next_scroll = max_scroll;
			scroll = next_scroll;
		}
	}
	LeaveCriticalSection(&lock);
	if (found < 0)
		MessageBeep(MB_ICONEXCLAMATION);
	InterlockedExchange(&dirty, 1);
}

static void console_mouse_wheel_at(int x, int y, int delta)
{
	POINT pt = {x, y};
	EnterCriticalSection(&lock);
	if ((PtInRect(&completion_rect, pt) && completion_count > 1) ||
		(context_menu_open && PtInRect(&context_menu_rect, pt))) {
		int notches;
		completion_wheel_remainder += delta;
		notches = completion_wheel_remainder / WHEEL_DELTA;
		completion_wheel_remainder -= notches * WHEEL_DELTA;
		if (notches) {
			int previous_scroll = completion_scroll;
			completion_scroll -= notches * 3;
			if (completion_scroll < 0) completion_scroll = 0;
			if (completion_scroll != previous_scroll)
				InterlockedExchange(&dirty, 1);
		}
	} else {
		int notches;
		UINT lines = 3;
		long long next_scroll;
		output_wheel_remainder += delta;
		notches = output_wheel_remainder / WHEEL_DELTA;
		output_wheel_remainder -= notches * WHEEL_DELTA;
		if (notches) {
			if (SystemParametersInfoA(SPI_GETWHEELSCROLLLINES, 0, &lines, 0)) {
				if (lines == WHEEL_PAGESCROLL)
					lines = (UINT)console_output_page_lines();
				else if (lines > 1000)
					lines = 1000;
			}
			next_scroll = (long long)scroll + (long long)notches * lines;
			if (next_scroll < 0) next_scroll = 0;
			{
				int page = console_output_page_lines();
				int max_scroll = line_count > page ? line_count - page : 0;
				if (next_scroll > max_scroll) next_scroll = max_scroll;
			}
			if ((int)next_scroll != scroll) {
				scroll = (int)next_scroll;
				InterlockedExchange(&dirty, 1);
			}
		}
	}
	LeaveCriticalSection(&lock);
}

static int console_sync_virtual_mouse_from_os(HWND h)
{
	POINT pt;
	RECT rc;
	if (!h || !GetCursorPos(&pt) || !ScreenToClient(h, &pt) || !GetClientRect(h, &rc) || rc.right <= 0 || rc.bottom <= 0)
		return 0;
	if (pt.x < 0) pt.x = 0;
	if (pt.y < 0) pt.y = 0;
	if (pt.x >= rc.right) pt.x = rc.right - 1;
	if (pt.y >= rc.bottom) pt.y = rc.bottom - 1;
	InterlockedExchange(&virtual_mouse_x, pt.x);
	InterlockedExchange(&virtual_mouse_y, pt.y);
	/* No Raw Input packet has been routed for this new focus session yet;
	   leave legacy mouse messages available as a fallback until the first one. */
	InterlockedExchange(&virtual_mouse_raw_seen, 0);
	return 1;
}


static void console_focus_changed(HWND h, int active)
{
	LONG was_active = InterlockedExchange(&console_window_active, active != 0);
	if (active && h && IsWindow(h)) {
		if (is_open) {
			/* Opening the console initializes the virtual pointer once in set_open().
			   Focus notifications must preserve that virtual position: GetCursorPos may
			   return the game's fixed ClipCursor lock point rather than the user pointer. */
			if (!was_active) {
				/* Preserve the current virtual position across focus transitions. A
				   physical GetCursorPos sample may be the game's ClipCursor lock point. */
				InterlockedExchange(&virtual_mouse_relative_mode, console_mouse_is_clipped(h) ? 1 : 0);
				InterlockedExchange(&virtual_mouse_raw_seen, 0);
			}
			if (InterlockedCompareExchange(&console_pause_mode, 0, 0))
				console_pause_capture_acquire(h);
		} else {
			InterlockedExchange(&virtual_mouse_relative_mode, 0);
			InterlockedExchange(&virtual_mouse_raw_seen, 0);
			(void)console_sync_virtual_mouse_from_os(h);
		}
		console_set_native_cursor_hidden(h, is_open && console_mouse_is_clipped(h));
	} else {
		InterlockedExchange(&virtual_mouse_relative_mode, 0);
		InterlockedExchange(&virtual_mouse_raw_seen, 0);
		if (h) KillTimer(h, CONSOLE_TITLE_CLICK_TIMER);
		InterlockedExchange(&console_pending_title_click, 0);
		InterlockedExchange(&console_scrollbar_drag_mode, 0);
		InterlockedExchange(&console_header_hover_button, 0);
		InterlockedExchange(&console_button_pressed, 0);
		if (InterlockedExchange(&console_window_drag_mode, 0)) console_save_window_geometry();
		InterlockedExchange(&console_pause_button_pressed, 0);
		console_pause_capture_release(game_window, 1);
		console_restore_capture();
		EnterCriticalSection(&lock);
		input_selecting = 0;
		output_selecting = 0;
		InterlockedExchange(&context_menu_open, 0);
		LeaveCriticalSection(&lock);
		console_set_native_cursor_hidden(h, 0);
	}
	InterlockedExchange(&dirty, 1);
}


static int console_header_button_hit(int x, int y, int left_offset, int right_offset)
{
	RECT rect;
	int width = (int)InterlockedCompareExchange(&console_backbuffer_width, 0, 0);
	int height = (int)InterlockedCompareExchange(&console_panel_height, 0, 0);
	if (width <= 0 || height <= 0) return 0;
	console_window_geometry(width, height, &rect);
	return y >= rect.top + 5 && y < rect.top + 31 &&
		x >= rect.right - right_offset && x < rect.right - left_offset;
}

static int console_header_close_hit(int x, int y) { return console_header_button_hit(x, y, 8, 32); }
static int console_header_max_hit(int x, int y) { return console_header_button_hit(x, y, 34, 58); }
static int console_header_min_hit(int x, int y) { return console_header_button_hit(x, y, 60, 84); }

static void console_pause_button_rect(const RECT *window_rect, RECT *pause_rect)
{
	int input_y, input_top_y, lines, top, height;
	if (!window_rect || !pause_rect) return;
	lines = console_input_line_count(window_rect);
	input_y = window_rect->bottom - console_bottom_reserved_height() - CONSOLE_BOTTOM_PAD - CONSOLE_INPUT_PAD - CONSOLE_LINE_HEIGHT;
	input_top_y = input_y - (lines - 1) * CONSOLE_LINE_HEIGHT;
	height = input_y + CONSOLE_LINE_HEIGHT + 5 - (input_top_y - 5);
	top = input_top_y - 5 + (height - 24) / 2;
	pause_rect->left = window_rect->left + 10;
	pause_rect->right = window_rect->left + 34;
	pause_rect->top = top;
	pause_rect->bottom = top + 24;
}

static int console_header_pause_hit(int x, int y)
{
	RECT rect, pause_rect;
	int width = (int)InterlockedCompareExchange(&console_backbuffer_width, 0, 0);
	int height = (int)InterlockedCompareExchange(&console_panel_height, 0, 0);
	if (width <= 0 || height <= 0 || console_window_collapsed) return 0;
	console_window_geometry(width, height, &rect);
	console_pause_button_rect(&rect, &pause_rect);
	return PtInRect(&pause_rect, (POINT){x, y});
}

static int console_header_layout_hit(int x, int y)
{
	RECT rect;
	int width = (int)InterlockedCompareExchange(&console_backbuffer_width, 0, 0);
	int height = (int)InterlockedCompareExchange(&console_panel_height, 0, 0);
	if (width <= 0 || height <= 0) return 0;
	console_window_geometry(width, height, &rect);
	return y >= rect.top + 5 && y < rect.top + 31 && x >= rect.right - 182 && x < rect.right - 88;
}

static int console_header_language_hit(int x, int y)
{
	RECT rect;
	int width = (int)InterlockedCompareExchange(&console_backbuffer_width, 0, 0);
	int height = (int)InterlockedCompareExchange(&console_panel_height, 0, 0);
	if (width <= 0 || height <= 0) return 0;
	console_window_geometry(width, height, &rect);
	return y >= rect.top + 5 && y < rect.top + 31 && x >= rect.right - 236 && x < rect.right - 186;
}

static int console_header_button_at(int x, int y)
{
	if (console_header_min_hit(x, y)) return 1;
	if (console_header_max_hit(x, y)) return 2;
	if (console_header_close_hit(x, y)) return 3;
	if (console_header_pause_hit(x, y)) return 4;
	if (console_header_layout_hit(x, y)) return 5;
	if (console_header_language_hit(x, y)) return 6;
	return 0;
}

static void console_header_button_activate(int button)
{
	switch (button) {
	case 1:
		console_window_minimize_toggle();
		if (!console_window_collapsed)
			console_save_window_geometry();
		break;
	case 2:
		console_window_maximize_toggle();
		break;
	case 3:
		set_open(0);
		break;
	case 4:
		console_pause_mode_toggle();
		break;
	case 5: {
		LONG layout = InterlockedCompareExchange(&console_legacy_layout, 0, 0);
		InterlockedExchange(&console_legacy_layout, layout ? 0 : 1);
		console_save_layout();
		console_clear_window_geometry();
		InterlockedExchange(&console_window_positioned, 0);
		InterlockedExchange(&console_window_sized, 0);
		InterlockedExchange(&console_window_collapsed, 0);
		InterlockedExchange(&console_min_restore_valid, 0);
		InterlockedExchange(&console_wrap_columns, 0); /* Force a full output reflow for the new layout. */
		break;
	}
	case 6:
		InterlockedExchange(&console_ui_language, console_ui_is_russian() ? 0 : 1);
		console_save_language();
		break;
	default:
		break;
	}
	InterlockedExchange(&dirty, 1);
}

static int console_pointer_over_ui(HWND h, int x, int y)
{
	RECT window_rect;
	int width = (int)InterlockedCompareExchange(&console_backbuffer_width, 0, 0);
	int panel_h = (int)InterlockedCompareExchange(&console_panel_height, 0, 0);
	int active_drag = InterlockedCompareExchange(&console_pause_button_pressed, 0, 0) != 0 ||
		InterlockedCompareExchange(&console_button_pressed, 0, 0) != 0 ||
		InterlockedCompareExchange(&console_window_drag_mode, 0, 0) != 0 ||
		InterlockedCompareExchange(&console_scrollbar_drag_mode, 0, 0) != 0 ||
		InterlockedCompareExchange((volatile LONG *)&output_selecting, 0, 0) != 0 ||
		InterlockedCompareExchange((volatile LONG *)&input_selecting, 0, 0) != 0;
	POINT pt = {x, y};
	if ((width <= 0 || panel_h <= 0) && h) {
		RECT client;
		if (GetClientRect(h, &client) && client.right > 0 && client.bottom > 0) {
			width = client.right;
			panel_h = panel_height(client.bottom);
		}
	}
	if (active_drag)
		return 1;
	if (width > 0 && panel_h > 0) {
		console_window_geometry(width, panel_h, &window_rect);
		if (PtInRect(&window_rect, pt))
			return 1;
	}
	if (!TryEnterCriticalSection(&lock))
		return 1; /* Prefer consuming one ambiguous pointer event over leaking a UI click to the game. */
	{
		int inside = PtInRect(&completion_rect, pt) || PtInRect(&output_toast_rect, pt) ||
			(context_menu_open && PtInRect(&context_menu_rect, pt));
		LeaveCriticalSection(&lock);
		if (inside) return 1;
	}
	return 0;
}

static LPCSTR console_cursor_resource(LONG kind)
{
	switch (kind) {
	case CONSOLE_CURSOR_MOVE: return IDC_SIZEALL;
	case CONSOLE_CURSOR_SIZE_WE: return IDC_SIZEWE;
	case CONSOLE_CURSOR_SIZE_NS: return IDC_SIZENS;
	case CONSOLE_CURSOR_SIZE_NWSE: return IDC_SIZENWSE;
	case CONSOLE_CURSOR_SIZE_NESW: return IDC_SIZENESW;
	case CONSOLE_CURSOR_HAND: return IDC_HAND;
	default: return IDC_ARROW;
	}
}

static void console_update_pointer_cursor(HWND h, int x, int y)
{
	LONG kind = CONSOLE_CURSOR_ARROW;
	LONG drag_mode = InterlockedCompareExchange(&console_window_drag_mode, 0, 0);
	int popup_surface = 0, popup_item = 0;
	int resize_mode = 0, watch_index = -1;
	int watch_hover = console_watch_chip_hit(x, y, &watch_index);
	LONG previous_watch_hover = InterlockedExchange(&console_watch_hover_index, watch_hover ? watch_index : -1);
	POINT pt = {x, y};
	if (previous_watch_hover != (watch_hover ? watch_index : -1))
		InterlockedExchange(&dirty, 1);
	if (TryEnterCriticalSection(&lock)) {
		if (PtInRect(&completion_rect, pt)) {
			popup_surface = 1;
			if (completion_hit(x, y, NULL)) popup_item = 1;
		} else if (PtInRect(&output_toast_rect, pt)) {
			popup_surface = 1;
			popup_item = 1;
		} else if (context_menu_open && PtInRect(&context_menu_rect, pt)) {
			popup_surface = 1;
		}
		LeaveCriticalSection(&lock);
	} else {
		/* An ambiguous popup frame must not briefly expose a resize cursor. */
		popup_surface = 1;
	}
	if (drag_mode >= 2 && drag_mode <= 6) {
		kind = drag_mode == 2 || drag_mode == 3 ? CONSOLE_CURSOR_SIZE_WE :
			drag_mode == 4 ? CONSOLE_CURSOR_SIZE_NS :
			drag_mode == 5 ? CONSOLE_CURSOR_SIZE_NESW : CONSOLE_CURSOR_SIZE_NWSE;
	} else if (drag_mode == 1 || drag_mode == 7) {
		kind = CONSOLE_CURSOR_MOVE;
	} else if (popup_surface) {
		kind = popup_item ? CONSOLE_CURSOR_HAND : CONSOLE_CURSOR_ARROW;
	} else if (console_watch_unpin_hit(x, y, NULL)) {
		kind = CONSOLE_CURSOR_HAND;
	} else {
		resize_mode = console_window_resize_hit(x, y);
		if (resize_mode) {
			kind = resize_mode == 2 || resize_mode == 3 ? CONSOLE_CURSOR_SIZE_WE :
				resize_mode == 4 ? CONSOLE_CURSOR_SIZE_NS :
				resize_mode == 5 ? CONSOLE_CURSOR_SIZE_NESW : CONSOLE_CURSOR_SIZE_NWSE;
		} else if (TryEnterCriticalSection(&lock)) {
			int item_index = -1;
			if (completion_hit(x, y, &item_index)) kind = CONSOLE_CURSOR_HAND;
			LeaveCriticalSection(&lock);
		}
	}
	InterlockedExchange(&console_cursor_kind, kind);
	if (InterlockedCompareExchange(&console_native_cursor_hidden, 0, 0)) {
		if (InterlockedExchange(&console_last_cursor_hidden, 1) != 1) {
			InterlockedExchange(&console_last_native_cursor_kind, -1);
			SetCursor(NULL);
		}
	} else {
		if (InterlockedExchange(&console_last_cursor_hidden, 0) == 1)
			InterlockedExchange(&console_last_native_cursor_kind, -1);
		if (InterlockedExchange(&console_last_native_cursor_kind, kind) != kind)
			SetCursor(LoadCursorA(NULL, console_cursor_resource(kind)));
	}
	(void)h;
}

static void console_update_pointer_cursor_from_os(HWND h)
{
	POINT pt;
	int virtual_x, virtual_y;
	if (h && InterlockedCompareExchange(&virtual_mouse_relative_mode, 0, 0)) {
		console_virtual_mouse_position(h, &virtual_x, &virtual_y);
		pt.x = (LONG)virtual_x;
		pt.y = (LONG)virtual_y;
	} else if (!h || !GetCursorPos(&pt) || !ScreenToClient(h, &pt)) {
		if (InterlockedCompareExchange(&console_native_cursor_hidden, 0, 0)) {
			if (InterlockedExchange(&console_last_cursor_hidden, 1) != 1)
				SetCursor(NULL);
			InterlockedExchange(&console_last_native_cursor_kind, -1);
		} else {
			if (InterlockedExchange(&console_last_cursor_hidden, 0) == 1)
				InterlockedExchange(&console_last_native_cursor_kind, -1);
			if (InterlockedExchange(&console_last_native_cursor_kind, CONSOLE_CURSOR_ARROW) != CONSOLE_CURSOR_ARROW)
				SetCursor(LoadCursorA(NULL, IDC_ARROW));
		}
		return;
	}
	{
		int x = (int)pt.x, y = (int)pt.y;
		console_mouse_to_backbuffer(h, &x, &y);
		console_update_pointer_cursor(h, x, y);
	}
}

static void console_init_virtual_mouse(HWND h)
{
	RECT client;
	POINT pt;
	int x, y, relative;
	if (!h || !GetClientRect(h, &client) || client.right <= 0 || client.bottom <= 0) return;
	relative = console_mouse_is_clipped(h);
	if (relative) {
		/* The game may already have clipped/warped the physical cursor to its
		   lock point. Reopening the console must resume the last virtual position. */
		x = (int)InterlockedCompareExchange(&virtual_mouse_x, -1, -1);
		y = (int)InterlockedCompareExchange(&virtual_mouse_y, -1, -1);
		if (x < 0 || y < 0) {
			if (GetCursorPos(&pt) && ScreenToClient(h, &pt)) {
				x = (int)pt.x;
				y = (int)pt.y;
			} else {
				x = client.right / 2;
				y = client.bottom / 2;
			}
		}
	} else if (GetCursorPos(&pt) && ScreenToClient(h, &pt)) {
		x = (int)pt.x;
		y = (int)pt.y;
	} else {
		x = (int)InterlockedCompareExchange(&virtual_mouse_x, -1, -1);
		y = (int)InterlockedCompareExchange(&virtual_mouse_y, -1, -1);
		if (x < 0 || y < 0) {
			x = client.right / 2;
			y = client.bottom / 2;
		}
	}
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x >= client.right) x = client.right - 1;
	if (y >= client.bottom) y = client.bottom - 1;
	InterlockedExchange(&virtual_mouse_x, x);
	InterlockedExchange(&virtual_mouse_y, y);
	InterlockedExchange(&virtual_mouse_relative_mode, relative);
	/* In relative mode, ignore the synthetic WM_MOUSEMOVE that may follow focus
	   or capture changes; the first Raw Input delta continues from this position. */
	InterlockedExchange(&virtual_mouse_raw_seen, 0);
	InterlockedExchange(&dirty, 1);
}


static int console_handle_raw_input(HWND h, HRAWINPUT raw_handle)
{
	RAWINPUT raw;
	UINT size = sizeof(raw);
	UINT received;
	if (!is_open)
		return 0;
	received = GetRawInputData(raw_handle, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER));
	if (received == (UINT)-1 || received < sizeof(RAWINPUTHEADER))
		return 1; /* Do not let an unparseable packet reach the game's input path while open. */
	if (raw.header.dwType == RIM_TYPEKEYBOARD)
		return 1; /* WM_KEYDOWN/WM_CHAR already feed the console; do not move the game. */
	if (raw.header.dwType != RIM_TYPEMOUSE)
		return 0; /* Preserve unrelated HID devices. */
	if (received < sizeof(RAWINPUT))
		return 1;
	InterlockedIncrement(&raw_mouse_packets);
	if (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
		InterlockedExchange(&virtual_mouse_relative_mode, 0);
		console_virtual_mouse_move(h, 0, 0, 1, raw.data.mouse.lLastX, raw.data.mouse.lLastY,
			(raw.data.mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0);
	} else {
		/* While the console is open, all mouse deltas belong to its virtual pointer.
		   Never forward them to the game's camera/input path. */
		InterlockedExchange(&virtual_mouse_relative_mode, 1);
		console_virtual_mouse_move(h, raw.data.mouse.lLastX, raw.data.mouse.lLastY, 0, 0, 0, 0);
	}
	{
		int x, y;
		int active_interaction = InterlockedCompareExchange(&console_pause_button_pressed, 0, 0) != 0 ||
			InterlockedCompareExchange(&console_window_drag_mode, 0, 0) != 0 ||
			InterlockedCompareExchange(&console_scrollbar_drag_mode, 0, 0) != 0 ||
			InterlockedCompareExchange((volatile LONG *)&output_selecting, 0, 0) != 0 ||
			InterlockedCompareExchange((volatile LONG *)&input_selecting, 0, 0) != 0;
		console_virtual_mouse_position(h, &x, &y);
		console_mouse_to_backbuffer(h, &x, &y);
		console_update_pointer_cursor(h, x, y);
		if (active_interaction || console_pointer_over_ui(h, x, y))
			console_mouse_move_at(h, x, y);
		else if (InterlockedExchange(&console_header_hover_button, 0))
			InterlockedExchange(&dirty, 1);
		if (raw.data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) {
			if (console_pointer_over_ui(h, x, y)) console_mouse_down_at(h, x, y, 0);
		}
		if (raw.data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)
			console_mouse_up();
		if (raw.data.mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) {
			if (!console_watch_chip_hit(x, y, NULL) && console_pointer_over_ui(h, x, y))
				console_context_menu_open_at(h, x, y);
		}
		if (raw.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) {
			if (console_pointer_over_ui(h, x, y))
				console_mouse_wheel_at(x, y, (SHORT)raw.data.mouse.usButtonData);
		}
	}
	return 1;
}

static LRESULT CALLBACK raw_input_relay_proc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
	if (msg == WM_INPUT && is_open && game_window && console_handle_raw_input(game_window, (HRAWINPUT)l)) {
		/* The game registered input on this auxiliary HWND. Consume mouse input
		   after routing it to the overlay; keyboard input is already handled by
		   WM_KEYDOWN/WM_CHAR, while unrelated HID devices stay with the game. */
		return DefWindowProcA(h, msg, w, l);
	}
	{
		WNDPROC previous = (WNDPROC)InterlockedCompareExchangePointer(
			(PVOID volatile *)&raw_input_relay_original_proc, NULL, NULL);
		return previous ? CallWindowProcA(previous, h, msg, w, l) : DefWindowProcA(h, msg, w, l);
	}
}

static LRESULT CALLBACK console_proc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
	int scan = (int)((l >> 16) & 0xff);
	if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && (scan == SCAN_TILDE || w == VK_OEM_3) && !(l & 0x40000000)) {
		if (GetKeyState(VK_SHIFT) & 0x8000) {
			if (is_open && full_console)
				console_window_maximize_toggle();
			else {
				InterlockedExchange(&console_window_collapsed, 0);
				console_window_maximize_toggle();
				set_open(1);
			}
		} else {
			if (is_open && full_console)
				InterlockedExchange(&full_console, 0);
			set_open(!is_open);
		}
		InterlockedExchange(&ignore_tilde, 1);
		InterlockedExchange(&dirty, 1);
		return 0;
	}
	if (msg == WM_CONSOLE_CURSOR_VISIBILITY) {
		/* Apply the requested native cursor state on the game UI thread. */
		console_apply_native_cursor_state(h, InterlockedCompareExchange(&console_native_cursor_hidden, 0, 0) != 0);
		return 0;
	}
	if (msg == WM_TIMER && w == CONSOLE_TITLE_CLICK_TIMER) {
		KillTimer(h, CONSOLE_TITLE_CLICK_TIMER);
		if (InterlockedExchange(&console_pending_title_click, 0)) {
			console_window_minimize_toggle();
			if (!console_window_collapsed) console_save_window_geometry();
		}
		return 0;
	}
	if (msg == WM_ACTIVATE || msg == WM_ACTIVATEAPP) {
		LRESULT result = CallWindowProcA(game_proc, h, msg, w, l);
		int active = msg == WM_ACTIVATE ? LOWORD(w) != WA_INACTIVE : (w != FALSE && GetForegroundWindow() == h);
		console_focus_changed(h, active);
		return result;
	}
	if (msg == WM_SIZE) {
		LRESULT result;
		if (w == SIZE_MINIMIZED)
			console_focus_changed(h, 0);
		result = CallWindowProcA(game_proc, h, msg, w, l);
		if (w != SIZE_MINIMIZED)
			console_focus_changed(h, GetForegroundWindow() == h);
		return result;
	}
	if (msg == WM_KILLFOCUS) {
		LRESULT result = CallWindowProcA(game_proc, h, msg, w, l);
		console_focus_changed(h, 0);
		return result;
	}
	if (msg == WM_SETFOCUS) {
		LRESULT result = CallWindowProcA(game_proc, h, msg, w, l);
		console_focus_changed(h, GetForegroundWindow() == h);
		return result;
	}
	if (is_open) {
		if (msg == WM_SYSKEYDOWN && (GetKeyState(VK_MENU) & 0x8000) &&
			(w == VK_LEFT || w == VK_RIGHT)) {
			input_undo_apply(w == VK_RIGHT);
			return 0;
		}
		switch (msg) {
		case WM_SETCURSOR:
			console_update_pointer_cursor_from_os(h);
			return TRUE;
		case WM_KEYDOWN:
			if (w == VK_F3 && output_search_query[0] && !output_search_mode && !context_menu_open) {
				console_output_search_step((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
				return 0;
			}
			if (context_menu_open) {
				int mode = (int)InterlockedCompareExchange(&context_menu_mode, 0, 0);
				int count = context_menu_count(mode), selected = (int)InterlockedCompareExchange(&context_menu_selected, 0, 0);
				if (w == VK_ESCAPE) {
					InterlockedExchange(&context_menu_open, 0);
					InterlockedExchange(&context_menu_selected, -1);
					InterlockedExchange(&dirty, 1);
				} else if (w == VK_UP || w == VK_DOWN) {
					int direction = w == VK_UP ? -1 : 1;
					int next = selected;
					for (int tries = 0; tries < count; tries++) {
						next = next < 0 ? (direction > 0 ? 0 : count - 1) : (next + direction + count) % count;
						if (context_menu_item_enabled(mode, next)) break;
					}
					if (count > 0 && context_menu_item_enabled(mode, next))
						InterlockedExchange(&context_menu_selected, next);
					InterlockedExchange(&dirty, 1);
				} else if (w == VK_RETURN && selected >= 0) {
					console_context_menu_execute(mode, selected);
				} else if (w == VK_HOME || w == VK_END) {
					int next = w == VK_HOME ? 0 : count - 1;
					int direction = w == VK_HOME ? 1 : -1;
					while (next >= 0 && next < count && !context_menu_item_enabled(mode, next)) next += direction;
					InterlockedExchange(&context_menu_selected, next >= 0 && next < count ? next : -1);
					InterlockedExchange(&dirty, 1);
				} else {
					/* Keep the usual Ctrl+C/X/V/A/L and Delete shortcuts usable while
					   the context menu is open; the edit handler dismisses the menu. */
					InterlockedExchange(&context_menu_open, 0);
					InterlockedExchange(&context_menu_selected, -1);
					edit_key(w);
				}
			} else if (w == VK_ESCAPE && output_search_mode) {
				console_output_search_cancel();
			} else if (w == VK_ESCAPE) {
				int close_console = 0;
				EnterCriticalSection(&lock);
				if (completion_count > 0)
					hide_completion();
				else
					close_console = 1;
				LeaveCriticalSection(&lock);
				if (close_console)
					set_open(0);
			} else
				edit_key(w);
			return 0;
		case WM_SYSKEYDOWN:
		case WM_SYSCHAR:
			return 0;
		case WM_CHAR: {
			char translated;
			if (ignore_tilde) {
				InterlockedExchange(&ignore_tilde, 0);
				return 0;
			}
			if (!(GetKeyState(VK_CONTROL) & 0x8000)) {
				if (current_layout_shift_symbol(l, &translated))
					edit_char((unsigned char)translated);
				else if (english_layout_char(w, l, &translated))
					edit_char((unsigned char)translated);
			}
			return 0;
		}
		case WM_MOUSEMOVE: {
			int x = (short)LOWORD(l), y = (short)HIWORD(l);
			int active_interaction = InterlockedCompareExchange(&console_pause_button_pressed, 0, 0) != 0 ||
				InterlockedCompareExchange(&console_window_drag_mode, 0, 0) != 0 ||
				InterlockedCompareExchange(&console_scrollbar_drag_mode, 0, 0) != 0 ||
				InterlockedCompareExchange((volatile LONG *)&output_selecting, 0, 0) != 0 ||
				InterlockedCompareExchange((volatile LONG *)&input_selecting, 0, 0) != 0;
			if (!InterlockedCompareExchange(&virtual_mouse_relative_mode, 0, 0)) {
				InterlockedExchange(&virtual_mouse_x, x);
				InterlockedExchange(&virtual_mouse_y, y);
			} else {
				/* In clipped/relative mode, WM_MOUSEMOVE often reports the game's
				   lock point (usually the client centre), not the user's virtual
				   cursor. Keep the cached pointer until Raw Input moves it. */
				console_virtual_mouse_position(h, &x, &y);
			}
			console_mouse_to_backbuffer(h, &x, &y);
			{
				int over_ui = console_pointer_over_ui(h, x, y);
				console_update_pointer_cursor(h, x, y);
				if (active_interaction || over_ui)
					console_mouse_move_at(h, x, y);
				else if (InterlockedExchange(&console_header_hover_button, 0))
					InterlockedExchange(&dirty, 1);
			}
			return 0;
		}

		case WM_LBUTTONDOWN: {
			int x, y;
			if (InterlockedCompareExchange(&virtual_mouse_raw_seen, 0, 0))
				return 0; /* WM_INPUT already delivered this click. */
			if (InterlockedCompareExchange(&virtual_mouse_relative_mode, 0, 0)) {
				/* ClipCursor makes lParam report the fixed lock point (often screen
				   centre). Clicks must use the virtual Raw Input pointer instead. */
				console_virtual_mouse_position(h, &x, &y);
			} else {
				x = (short)LOWORD(l); y = (short)HIWORD(l);
				InterlockedExchange(&virtual_mouse_x, x);
				InterlockedExchange(&virtual_mouse_y, y);
			}
			console_mouse_to_backbuffer(h, &x, &y);
			if (!console_pointer_over_ui(h, x, y))
				return 0;
			console_mouse_down_at(h, x, y, 1);
			console_update_pointer_cursor(h, x, y);
			return 0;
		}
		case WM_LBUTTONUP: {
			int x, y;
			if (InterlockedCompareExchange(&virtual_mouse_raw_seen, 0, 0)) return 0;
			if (InterlockedCompareExchange(&virtual_mouse_relative_mode, 0, 0))
				console_virtual_mouse_position(h, &x, &y);
			else {
				x = (short)LOWORD(l); y = (short)HIWORD(l);
				InterlockedExchange(&virtual_mouse_x, x);
				InterlockedExchange(&virtual_mouse_y, y);
			}
			int active_interaction = InterlockedCompareExchange(&console_pause_button_pressed, 0, 0) != 0 ||
				InterlockedCompareExchange(&console_window_drag_mode, 0, 0) != 0 ||
				InterlockedCompareExchange(&console_scrollbar_drag_mode, 0, 0) != 0 ||
				InterlockedCompareExchange((volatile LONG *)&output_selecting, 0, 0) != 0 ||
				InterlockedCompareExchange((volatile LONG *)&input_selecting, 0, 0) != 0;
			console_mouse_to_backbuffer(h, &x, &y);
			if (active_interaction || console_pointer_over_ui(h, x, y))
				console_mouse_up();
			console_update_pointer_cursor(h, x, y);
			return 0;
		}
		case WM_LBUTTONDBLCLK: {
			int x, y;
			if (InterlockedCompareExchange(&virtual_mouse_raw_seen, 0, 0))
				return 0; /* Raw input already handled the second click. */
			if (InterlockedCompareExchange(&virtual_mouse_relative_mode, 0, 0))
				console_virtual_mouse_position(h, &x, &y);
			else {
				x = (short)LOWORD(l); y = (short)HIWORD(l);
				InterlockedExchange(&virtual_mouse_x, x);
				InterlockedExchange(&virtual_mouse_y, y);
			}
			console_mouse_to_backbuffer(h, &x, &y);
			if (console_header_button_at(x, y)) {
				console_mouse_down_at(h, x, y, 1);
				return 0;
			}
			{
				int output_line, output_column;
				if (output_hit(h, x, y, &output_line, &output_column)) {
					output_last_click_tick = GetTickCount();
					output_last_click_seq = output_line;
					output_last_click_x = x;
					output_last_click_y = y;
					output_click_count = 2;
					return 0;
				}
			}
			if (console_window_title_hit(x, y)) {
				if (InterlockedExchange(&console_pending_title_click, 0)) {
					KillTimer(h, CONSOLE_TITLE_CLICK_TIMER);
					console_window_maximize_toggle();
				} else if (console_window_collapsed) {
					console_window_minimize_toggle();
				} else {
					console_window_maximize_toggle();
				}
			} else {
				console_input_double_click_at(x, y);
			}
			return 0;
		}

		case WM_RBUTTONDOWN: {
			if (InterlockedCompareExchange(&virtual_mouse_raw_seen, 0, 0)) return 0;
			int x, y;
			if (InterlockedCompareExchange(&virtual_mouse_relative_mode, 0, 0))
				console_virtual_mouse_position(h, &x, &y);
			else {
				x = (short)LOWORD(l); y = (short)HIWORD(l);
				InterlockedExchange(&virtual_mouse_x, x);
				InterlockedExchange(&virtual_mouse_y, y);
			}
			console_mouse_to_backbuffer(h, &x, &y);
			if (!console_watch_chip_hit(x, y, NULL) && console_pointer_over_ui(h, x, y))
				console_context_menu_open_at(h, x, y);
			return 0;
		}
		case WM_RBUTTONUP:
			return 0;
		case WM_CAPTURECHANGED:
			InterlockedExchange(&console_capture_owned, 0);
			InterlockedExchange(&console_pause_capture_owned, 0);
			InterlockedExchange(&console_pause_capture_release_pending, 0);
			InterlockedExchange(&console_pause_button_pressed, 0);
			console_pause_previous_capture = NULL;
			InterlockedExchange(&console_scrollbar_drag_mode, 0);
			if (InterlockedExchange(&console_window_drag_mode, 0))
				console_save_window_geometry();
			console_previous_capture = NULL;
			EnterCriticalSection(&lock);
			output_selecting = 0;
			input_selecting = 0;
			LeaveCriticalSection(&lock);
			return 0;
		case WM_MBUTTONDOWN:
		case WM_MBUTTONUP:
		case WM_XBUTTONDOWN:
		case WM_XBUTTONUP:
			return 0;
		case WM_MOUSEWHEEL: {
			POINT pt = {(short)LOWORD(l), (short)HIWORD(l)};
			int x, y;
			ScreenToClient(h, &pt);
			int already_routed = InterlockedCompareExchange(&virtual_mouse_raw_seen, 0, 0) != 0;
			if (InterlockedCompareExchange(&virtual_mouse_relative_mode, 0, 0))
				console_virtual_mouse_position(h, &x, &y);
			else {
				x = (int)pt.x; y = (int)pt.y;
			}
			console_mouse_to_backbuffer(h, &x, &y);
			if (!console_pointer_over_ui(h, x, y))
				return 0;
			if (!already_routed) {
				if (!InterlockedCompareExchange(&virtual_mouse_relative_mode, 0, 0)) {
					InterlockedExchange(&virtual_mouse_x, (LONG)pt.x);
					InterlockedExchange(&virtual_mouse_y, (LONG)pt.y);
				}
				console_mouse_wheel_at(x, y, (short)HIWORD(w));
			}
			return 0;
		}
		case WM_INPUT: // Route raw mouse to the virtual cursor; keep keyboard input in WM_KEYDOWN/WM_CHAR.
			if (console_handle_raw_input(h, (HRAWINPUT)l))
				return DefWindowProcA(h, msg, w, l);
			return CallWindowProcA(game_proc, h, msg, w, l);
		case WM_MOUSEHWHEEL:
		case WM_NCMOUSEMOVE:
		case WM_NCLBUTTONDOWN:
		case WM_NCLBUTTONUP:
		case WM_NCRBUTTONDOWN:
		case WM_NCRBUTTONUP:
			return 0;
		}
	} else if (msg == WM_CHAR && ignore_tilde) {
		InterlockedExchange(&ignore_tilde, 0);
		return 0;
	}
	return CallWindowProcA(game_proc, h, msg, w, l);
}

// The game stores Russian localized output as Windows-1251, independently of the Windows ACP.
static UINT console_codepage(void)
{
	LONG value = InterlockedCompareExchange(&console_cp, -1, -1);
	if (value < 0) {
		LONG detected = localize_is_russian() ? 1251 : CP_ACP;
		InterlockedCompareExchange(&console_cp, detected, -1);
		value = InterlockedCompareExchange(&console_cp, -1, -1);
	}
	return (UINT)value;
}

static int console_ui_is_russian(void)
{
	LONG language = InterlockedCompareExchange(&console_ui_language, -1, -1);
	return language < 0 ? localize_is_russian() : language != 0;
}

static void console_print_localized(const char *english, const char *russian_utf8)
{
	WCHAR wide[LINE_LEN];
	char encoded[LINE_LEN];
	int wide_len, bytes;
	if (!console_ui_is_russian()) {
		console_print(english);
		return;
	}
	wide_len = MultiByteToWideChar(CP_UTF8, 0, russian_utf8, -1, wide, (int)(sizeof(wide) / sizeof(wide[0])));
	if (wide_len <= 0) {
		console_print(english);
		return;
	}
	bytes = WideCharToMultiByte(1251, 0, wide, -1, encoded, sizeof(encoded), NULL, NULL);
	if (bytes <= 0) {
		console_print(english);
		return;
	}
	console_print_codepage(encoded, 1251);
}

// Prints the text of <name> (console.str) in the console's language.
static void console_print_text(const char *name)
{
	console_print_localized(ctexts_get(name, 0), ctexts_get(name, 1));
}

static void console_ui_text_out(HDC dc, int x, int y, const char *utf8)
{
	WCHAR wide[LINE_LEN * 2];
	int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, (int)(sizeof(wide) / sizeof(wide[0])));
	if (n > 1)
		TextOutW(dc, x, y, wide, n - 1);
}

static int console_ui_text_width(HDC dc, const char *utf8, SIZE *size)
{
	WCHAR wide[LINE_LEN * 2];
	int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, (int)(sizeof(wide) / sizeof(wide[0])));
	return n > 1 && GetTextExtentPoint32W(dc, wide, n - 1, size);
}

/* Forward declaration: centered output helpers are defined before the
   underlying codepage-aware text renderer below. */
static void console_text_out(HDC dc, int x, int y, const char *text, int length);

static int console_text_vertical_origin(HDC dc, int top, int height)
{
	TEXTMETRICW metrics;
	int text_height = 16;
	if (GetTextMetricsW(dc, &metrics) && metrics.tmHeight > 0)
		text_height = metrics.tmHeight;
	if (text_height > height) text_height = height;
	return top + (height - text_height) / 2;
}

static void console_text_out_centered(HDC dc, int x, int top, int height,
	const char *text, int length)
{
	console_text_out(dc, x, console_text_vertical_origin(dc, top, height), text, length);
}

static void console_ui_text_out_centered(HDC dc, int x, int top, int height, const char *utf8)
{
	console_ui_text_out(dc, x, console_text_vertical_origin(dc, top, height), utf8);
}

static void console_text_out_cp(HDC dc, int x, int y, const char *text, int length, UINT codepage)
{
	WCHAR wide[LINE_LEN * 2];
	int n;
	if (length < 0) length = (int)strlen(text);
	if (length > LINE_LEN - 1) length = LINE_LEN - 1;
	n = MultiByteToWideChar(codepage, 0, text, length, wide, (int)(sizeof(wide) / sizeof(wide[0])));
	if (n > 0) TextOutW(dc, x, y, wide, n);
}

static void console_text_out(HDC dc, int x, int y, const char *text, int length)
{
	console_text_out_cp(dc, x, y, text, length, console_codepage());
}

static int console_text_width_cp(HDC dc, const char *text, int length, SIZE *size, UINT codepage)
{
	WCHAR wide[LINE_LEN * 2];
	int n;
	if (length < 0) length = (int)strlen(text);
	if (length > LINE_LEN - 1) length = LINE_LEN - 1;
	n = MultiByteToWideChar(codepage, 0, text, length, wide, (int)(sizeof(wide) / sizeof(wide[0])));
	return n > 0 && GetTextExtentPoint32W(dc, wide, n, size);
}

static int console_text_width(HDC dc, const char *text, int length, SIZE *size)
{
	return console_text_width_cp(dc, text, length, size, console_codepage());
}

// ---------------------------------------------------------------- drawing

static HDC mem_dc;
static HBITMAP dib;
static uint32_t *dib_bits;
static HFONT font;
static int dib_w, dib_h, dib_panel_h;
static IDirect3DTexture9 *tex;
static IDirect3DDevice9 *tex_device;
static int tex_w, tex_h;
static IDirect3DStateBlock9 *panel_state_block;
static IDirect3DDevice9 *panel_state_device;

enum {
	PANEL_BRUSH_BACKGROUND,
	PANEL_BRUSH_EDGE,
	PANEL_BRUSH_SELECTION,
	PANEL_BRUSH_POPUP,
	PANEL_BRUSH_SELECTED,
	PANEL_BRUSH_CARET,
	PANEL_BRUSH_TITLE,
	PANEL_BRUSH_INPUT,
	PANEL_BRUSH_ACCENT,
	PANEL_BRUSH_SHADOW,
	PANEL_BRUSH_PAUSE_INACTIVE,
	PANEL_BRUSH_PAUSE_ACTIVE,
	PANEL_BRUSH_PAUSE_INACTIVE_ICON,
	PANEL_BRUSH_PAUSE_ACTIVE_ICON,
	PANEL_BRUSH_BUTTON_HOVER,
	PANEL_BRUSH_BUTTON_PRESSED,
	PANEL_BRUSH_TOOLTIP,
	PANEL_BRUSH_COUNT
};
static HBRUSH panel_brushes[PANEL_BRUSH_COUNT];

static HBRUSH panel_brush(int index, COLORREF color)
{
	if (index < 0 || index >= PANEL_BRUSH_COUNT)
		return NULL;
	if (!panel_brushes[index])
		panel_brushes[index] = CreateSolidBrush(color);
	return panel_brushes[index];
}

static void console_fill_button(HDC dc, const RECT *rect, int button_id, HBRUSH normal)
{
	LONG pressed = InterlockedCompareExchange(&console_button_pressed, 0, 0);
	LONG hover = InterlockedCompareExchange(&console_header_hover_button, 0, 0);
	HBRUSH brush = normal;
	if (pressed == button_id)
		brush = panel_brush(PANEL_BRUSH_BUTTON_PRESSED, RGB(91, 68, 43));
	else if (hover == button_id)
		brush = panel_brush(PANEL_BRUSH_BUTTON_HOVER, RGB(65, 57, 46));
	if (dc && rect && brush)
		FillRect(dc, rect, brush);
}

static void console_draw_aa_line(int x0, int y0, int x1, int y1, float thickness, COLORREF color)
{
	const int samples = 16;
	float vx = (float)(x1 - x0), vy = (float)(y1 - y0);
	float len2 = vx * vx + vy * vy;
	float radius = thickness * 0.5f;
	int left = (x0 < x1 ? x0 : x1) - 2;
	int right = (x0 > x1 ? x0 : x1) + 2;
	int top = (y0 < y1 ? y0 : y1) - 2;
	int bottom = (y0 > y1 ? y0 : y1) + 2;
	unsigned cr = GetRValue(color), cg = GetGValue(color), cb = GetBValue(color);
	if (!dib_bits || dib_w <= 0 || dib_h <= 0) return;
	GdiFlush(); /* Commit preceding GDI drawing before direct DIB pixel blending. */
	if (left < 0) left = 0;
	if (top < 0) top = 0;
	if (right >= dib_w) right = dib_w - 1;
	if (bottom >= dib_h) bottom = dib_h - 1;
	for (int y = top; y <= bottom; y++) {
		for (int x = left; x <= right; x++) {
			int covered = 0;
			for (int sy = 0; sy < samples; sy++) {
				for (int sx = 0; sx < samples; sx++) {
					float px = (float)x + ((float)sx + 0.5f) / samples;
					float py = (float)y + ((float)sy + 0.5f) / samples;
					float t = len2 > 0.0f ? ((px - x0) * vx + (py - y0) * vy) / len2 : 0.0f;
					float qx, qy, dx, dy;
					if (t < 0.0f) t = 0.0f;
					if (t > 1.0f) t = 1.0f;
					qx = (float)x0 + t * vx;
					qy = (float)y0 + t * vy;
					dx = px - qx; dy = py - qy;
					if (dx * dx + dy * dy <= radius * radius) covered++;
				}
			}
			if (covered) {
				uint32_t *pixel = &dib_bits[(size_t)y * (size_t)dib_w + (size_t)x];
				unsigned a = (unsigned)(covered * 255 / (samples * samples));
				unsigned old = *pixel;
				unsigned r = (old >> 16) & 255u, g = (old >> 8) & 255u, b = old & 255u;
				r = (r * (255u - a) + cr * a + 127u) / 255u;
				g = (g * (255u - a) + cg * a + 127u) / 255u;
				b = (b * (255u - a) + cb * a + 127u) / 255u;
				*pixel = (old & 0xff000000u) | (r << 16) | (g << 8) | b;
			}
		}
	}
}

static int panel_height(int h)
{
	/* The floating window is allowed anywhere in the game client, including
	   the bottom edge; the transparent overlay therefore covers the full client. */
	return h;
}

static int console_format_local_time(const SYSTEMTIME *local_time, char *out, int capacity)
{
	char format[80];
	char with_seconds[96];
	char separator[8];
	char *minute_token, *insert_at;
	int length, separator_length;
	if (!local_time || !out || capacity <= 0) return 0;
	if (!GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_STIMEFORMAT, format, (int)sizeof(format)))
		strcpy(format, "HH:mm:ss");
	if (!strchr(format, 's') && !strchr(format, 'S')) {
		minute_token = strchr(format, 'm');
		if (!minute_token) minute_token = strchr(format, 'M');
		if (minute_token) {
			insert_at = minute_token + 1;
			while (*insert_at == 'm' || *insert_at == 'M') insert_at++;
			if (!GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_STIME, separator, (int)sizeof(separator)))
				strcpy(separator, ":");
			separator_length = (int)strlen(separator);
			length = (int)strlen(format);
			if (separator_length > 0 && length + separator_length + 2 < (int)sizeof(with_seconds)) {
				size_t prefix = (size_t)(insert_at - format);
				memcpy(with_seconds, format, prefix);
				memcpy(with_seconds + prefix, separator, (size_t)separator_length);
				memcpy(with_seconds + prefix + separator_length, "ss", 2);
				strcpy(with_seconds + prefix + separator_length + 2, insert_at);
			} else {
				strcpy(with_seconds, "HH:mm:ss");
			}
		} else {
			strcpy(with_seconds, "HH:mm:ss");
		}
	} else {
		strcpy(with_seconds, format);
	}
	return GetTimeFormatA(LOCALE_USER_DEFAULT, 0, local_time, with_seconds, out, capacity) != 0;
}

static int paint(int w, int panel_h)
{
	const int lh = CONSOLE_LINE_HEIGHT;
	const int header_h = 36;
	const int bottom_pad = CONSOLE_BOTTOM_PAD;
	const int input_pad = CONSOLE_INPUT_PAD;
	const int popup_gap = 14;
	const int output_gap = 8;
	int window_left, window_right, window_bottom, window_top, input_y, input_top_y, input_lines, input_columns, output_top;
	int rows, show_single_default = 0, cellw = 1, cols = 1, total_rows = 0;
	int visible = 0, popup_h = 0, popup_top = panel_h + popup_gap, popup_width = 0;
	int available_width;
	int popup_side = 0; /* 1 = left of console, 2 = right of console */
	int placement = 0; /* 1 = below, 2 = above, 3 = inside output, 4 = beside */
	int bitmap_h = panel_h;
	RECT window_rect, title_rect, input_rect, popup_rect, pause_rect, button_tooltip_rect;
	char button_tooltip_text[128] = "";
	HBRUSH back, edge, title_brush, input_brush, accent_brush, popup_brush, shadow_brush;
	static const COLORREF palette[10] = {
		RGB(0, 0, 0), RGB(196, 111, 96), RGB(126, 151, 112), RGB(232, 190, 106),
		RGB(153, 154, 151), RGB(158, 173, 163), RGB(180, 143, 119), RGB(224, 221, 213),
		RGB(224, 221, 213), RGB(153, 140, 140)
	};
	console_window_geometry(w, panel_h, &window_rect);
	SetRectEmpty(&button_tooltip_rect);
	SetRectEmpty(&pause_rect);
	window_left = window_rect.left;
	window_right = window_rect.right;
	window_top = window_rect.top;
	window_bottom = window_rect.bottom;
	available_width = window_right - window_left - 20;
	if (window_right <= window_left + 120 ||
		(!console_window_collapsed && window_bottom <= window_top + header_h + lh * 3) ||
		(console_window_collapsed && window_bottom <= window_top + header_h - 1))
		return 0;
	InterlockedExchange(&console_panel_height, panel_h);

	if (!TryEnterCriticalSection(&lock))
		return 0; /* Never stall the render thread behind input or output updates. */
	SetRectEmpty(&output_scroll_track);
	SetRectEmpty(&output_scroll_thumb);
	SetRectEmpty(&completion_scroll_track);
	SetRectEmpty(&completion_scroll_thumb);
	SetRectEmpty(&output_toast_rect);
	SetRectEmpty(&output_toast_close_rect);
	output_scroll_max = output_scroll_page = 0;
	completion_scroll_max = completion_scroll_page = 0;

	if (!font) {
		/* Keep grayscale antialiasing for this per-pixel-alpha DIB. ClearType's
		   RGB subpixel fringes are not alpha-safe for a composited D3D overlay. */
		HFONT created = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
		HGDIOBJ previous;
		if (!created) {
			LeaveCriticalSection(&lock);
			return 0;
		}
		previous = SelectObject(mem_dc, created);
		if (!previous || previous == HGDI_ERROR) {
			DeleteObject(created);
			LeaveCriticalSection(&lock);
			return 0;
		}
		font = created;
		SetBkMode(mem_dc, TRANSPARENT);
	}

	{
		SIZE input_cell = {0, 0};
		WCHAR sample = L'M';
		if (GetTextExtentPoint32W(mem_dc, &sample, 1, &input_cell) && input_cell.cx > 0)
			InterlockedExchange(&console_font_cell_width, input_cell.cx);
	}
	{
		int editor_offset = INPUT_TEXT_OFFSET;
		if (output_search_mode) {
			SIZE prefix_size = {0, 0};
			const char *prefix = ctexts_get("CON_FIND_PREFIX", console_ui_is_russian());
			if (console_ui_text_width(mem_dc, prefix, &prefix_size)) {
				int required = 44 + prefix_size.cx + 12;
				if (required > editor_offset) editor_offset = required;
			}
		}
		InterlockedExchange(&console_input_left_offset, editor_offset);
	}
	input_columns = console_input_columns(&window_rect);
	input_lines = max(1, (input_len + input_columns - 1) / input_columns);
	input_y = window_bottom - console_bottom_reserved_height() - bottom_pad - input_pad - lh;
	input_top_y = input_y - (input_lines - 1) * lh;

	if (completion_count > 0) {
		int max_pair_width = 0;
		int below_space, above_space, inside_space;
		int below_rows, above_rows, inside_rows;
		int max_visible;
		int input_line = input_top_y;
		show_single_default = completion_count == 1 && completion_items[0].is_dvar;
		for (int i = 0; i < completion_count; i++) {
			SIZE z = {0, 0};
			console_text_width(mem_dc, completion_items[i].name, (int)strlen(completion_items[i].name), &z);
			{
				int pair_width = z.cx;
				if (completion_items[i].is_dvar && completion_items[i].value[0]) {
					SIZE value_size = {0, 0};
					console_text_width(mem_dc, completion_items[i].value, (int)strlen(completion_items[i].value), &value_size);
					pair_width += 24 + value_size.cx;
				}
				if (show_single_default && completion_items[i].default_value[0]) {
					SIZE default_size = {0, 0};
					console_text_width(mem_dc, completion_items[i].default_value, (int)strlen(completion_items[i].default_value), &default_size);
					if (default_size.cx + 24 > pair_width) pair_width = default_size.cx + 24;
				}
				if (pair_width > max_pair_width) max_pair_width = pair_width;
			}
		}
		total_rows = show_single_default ? 2 : completion_count;

		/* The panel texture always stays at the fixed compact/fullscreen height.
		   Pick the roomiest valid dropdown location: below, above, or inside the
		   output area. A long match list is clipped to this bounded viewport and
		   navigated with the existing completion scroll; it can never resize the
		   panel texture or make the main console disappear. */
		below_space = panel_h - (window_bottom + popup_gap) - 8;
		above_space = window_top - popup_gap - 8;
		inside_space = input_line - popup_gap - (window_top + header_h + 8);
		below_rows = below_space > header_h ? (below_space - header_h) / lh : 0;
		above_rows = above_space > header_h ? (above_space - header_h) / lh : 0;
		inside_rows = inside_space > header_h ? (inside_space - header_h) / lh : 0;
		if (!InterlockedCompareExchange(&console_legacy_layout, 0, 0) && inside_rows > 0) {
			/* Enhanced is designed around an in-window autocomplete surface. Keep it
			   there by default and scroll long lists instead of moving them outside. */
			max_visible = inside_rows;
			placement = 3;
		} else if (InterlockedCompareExchange(&console_legacy_layout, 0, 0)) {
			/* Legacy strongly prefers a dropdown below the window. Even a short
			   bottom viewport is better than a side panel; it remains scrollable. */
			if (below_rows > 0) {
				max_visible = below_rows;
				placement = 1;
			} else {
				max_visible = 0;
				placement = 1;
				if (above_rows > max_visible) { max_visible = above_rows; placement = 2; }
				if (inside_rows > max_visible) { max_visible = inside_rows; placement = 3; }
				/* A side panel is now a last resort: use it only if no vertical
				   placement can display even a single suggestion row. */
				if (max_visible == 0) {
					int left_space = window_left - 16;
					int right_space = w - window_right - 16;
					int side_space = left_space >= right_space ? left_space : right_space;
					int side_rows = (panel_h - 16 - header_h) / lh;
					if (side_rows > 48) side_rows = 48;
					if (side_space >= 176 && side_rows > 0) {
						placement = 4;
						popup_side = left_space >= right_space ? 1 : 2;
						max_visible = side_rows;
						available_width = side_space;
					}
				}
			}
		} else {
			max_visible = below_rows;
			placement = 1;
			if (above_rows > max_visible) { max_visible = above_rows; placement = 2; }
			if (inside_rows > max_visible) { max_visible = inside_rows; placement = 3; }
		}
		if (max_visible > 48) max_visible = 48;

		/* Use columns only when all rows cannot fit. If the list still exceeds
		   four columns, retain a scrollable subset rather than growing off-screen. */
		cols = 1;
		if (!show_single_default && max_visible > 0 && total_rows > max_visible) {
			int desired_cols = (total_rows + max_visible - 1) / max_visible;
			int min_cell_width = max_pair_width + 48;
			int max_cols;
			if (min_cell_width < 320) min_cell_width = 320;
			if (min_cell_width > 640) min_cell_width = 640;
			max_cols = (available_width - 16) / min_cell_width;
			if (max_cols < 1) max_cols = 1;
			if (max_cols > 4) max_cols = 4;
			cols = desired_cols < max_cols ? desired_cols : max_cols;
		}
		cellw = (available_width - 16) / cols;
		if (cellw < 1) cellw = 1;
		/* A one-column list should hug its content. It only reaches the full
		   console width when long names/values actually need that much room. */
		if (cols == 1) {
			int natural_width = max_pair_width + 40;
			if (natural_width < 280) natural_width = 280;
			popup_width = natural_width < available_width ? natural_width : available_width;
			cellw = popup_width - 16;
		} else {
			popup_width = available_width;
		}
		if (cellw < 1) cellw = 1;
		if (cols > 1)
			total_rows = (completion_count + cols - 1) / cols;
		visible = total_rows < max_visible ? total_rows : max_visible;
		if (total_rows > visible && cellw > 32)
			cellw -= 10;
		if (visible > 0) {
			popup_h = header_h + visible * lh;
			if (placement == 2) {
				popup_top = window_top - popup_gap - popup_h;
			} else if (placement == 3) {
				popup_top = input_line - popup_h - popup_gap;
			} else if (placement == 4) {
				popup_top = (panel_h - popup_h) / 2;
				if (popup_top < 8) popup_top = 8;
				if (popup_top + popup_h > panel_h - 8) popup_top = panel_h - 8 - popup_h;
			} else {
				popup_top = window_bottom + popup_gap;
			}
		}
	}

	/* The DIB and D3D texture never grow with suggestions or context menus. */
	bitmap_h = panel_h;
	if (w <= 0 || bitmap_h <= 0 || bitmap_h > (int)InterlockedCompareExchange(&console_backbuffer_height, 0, 0)) {
		LeaveCriticalSection(&lock);
		return 0;
	}
	if ((size_t)w > ((size_t)-1) / (size_t)bitmap_h || (size_t)w * (size_t)bitmap_h > 67108864u) {
		LeaveCriticalSection(&lock);
		return 0;
	}
	if (dib_w != w || dib_h != bitmap_h) {
		BITMAPINFO bi;
		void *new_bits = NULL;
		HBITMAP new_dib;
		HGDIOBJ previous;
		memset(&bi, 0, sizeof(bi));
		bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
		bi.bmiHeader.biWidth = w;
		bi.bmiHeader.biHeight = -bitmap_h;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		new_dib = CreateDIBSection(mem_dc, &bi, DIB_RGB_COLORS, &new_bits, NULL, 0);
		if (!new_dib || !new_bits) {
			if (new_dib) DeleteObject(new_dib);
			LeaveCriticalSection(&lock);
			return 0;
		}
		previous = SelectObject(mem_dc, new_dib);
		if (!previous || previous == HGDI_ERROR) {
			DeleteObject(new_dib);
			LeaveCriticalSection(&lock);
			return 0;
		}
		if (dib) DeleteObject(dib);
		dib = new_dib;
		dib_bits = (uint32_t *)new_bits;
		dib_w = w;
		dib_h = bitmap_h;
	}
	if (!dib_bits) {
		LeaveCriticalSection(&lock);
		return 0;
	}
	{
		SIZE cell = {0, 0};
		WCHAR sample = L'M';
		int columns = 110;
		if (GetTextExtentPoint32W(mem_dc, &sample, 1, &cell) && cell.cx > 0) {
			columns = (window_right - window_left - 32) / cell.cx;
			InterlockedExchange(&console_font_cell_width, cell.cx);
		}
		if (columns < 20) columns = 20;
		if (columns > 1500) columns = 1500;
		if (columns != (int)InterlockedCompareExchange(&console_wrap_columns, 0, 0)) {
			console_reflow_output_locked(columns);
			InterlockedExchange(&dirty, 1);
		}
		InterlockedExchange(&console_wrap_columns, columns);
	}

	/* A single DIB contains two independent surfaces: the floating console window
	   and, below it, a detached autocomplete dropdown. The gap stays transparent. */
	memset(dib_bits, 0, (size_t)w * (size_t)bitmap_h * sizeof(*dib_bits));
	title_rect = window_rect;
	title_rect.bottom = window_top + header_h;
	input_y = window_bottom - console_bottom_reserved_height() - bottom_pad - input_pad - lh;
	input_top_y = input_y - (input_lines - 1) * lh;
	output_top = title_rect.bottom + 9;
	rows = (input_top_y - output_gap - output_top) / lh;
	if (rows < 0 || console_window_collapsed) rows = 0;
	if (rows > 0 && scroll > line_count - rows)
		scroll = line_count > rows ? line_count - rows : 0;
	input_rect.left = window_left + 40;
	input_rect.right = window_right - 10;
	input_rect.top = input_top_y - 5;
	input_rect.bottom = input_y + lh + 5;
	console_pause_button_rect(&window_rect, &pause_rect);

	back = panel_brush(PANEL_BRUSH_BACKGROUND, RGB(37, 40, 42));
	edge = panel_brush(PANEL_BRUSH_EDGE, RGB(85, 90, 92));
	title_brush = panel_brush(PANEL_BRUSH_TITLE, RGB(48, 52, 54));
	input_brush = panel_brush(PANEL_BRUSH_INPUT, RGB(27, 30, 32));
	accent_brush = panel_brush(PANEL_BRUSH_ACCENT, RGB(199, 146, 72));
	popup_brush = panel_brush(PANEL_BRUSH_POPUP, RGB(32, 36, 38));
	shadow_brush = panel_brush(PANEL_BRUSH_SHADOW, RGB(0, 0, 0));
	if (back) FillRect(mem_dc, &window_rect, back);
	if (shadow_brush) {
		RECT shadow = {window_rect.left + 3, window_rect.bottom, window_rect.right + 3, window_rect.bottom + 4};
		if (shadow.right > w) shadow.right = w;
		if (shadow.bottom > bitmap_h) shadow.bottom = bitmap_h;
		if (shadow.right > shadow.left && shadow.bottom > shadow.top) FillRect(mem_dc, &shadow, shadow_brush);
	}
	if (title_brush) FillRect(mem_dc, &title_rect, title_brush);
	if (edge) {
		RECT r = {window_rect.left, window_rect.top, window_rect.right, window_rect.top + 1}; FillRect(mem_dc, &r, edge);
		r.top = window_rect.bottom - 1; r.bottom = window_rect.bottom; FillRect(mem_dc, &r, edge);
		r.left = window_rect.left; r.right = window_rect.left + 1; r.top = window_rect.top; r.bottom = window_rect.bottom; FillRect(mem_dc, &r, edge);
		r.left = window_rect.right - 1; r.right = window_rect.right; FillRect(mem_dc, &r, edge);
	}
	if (accent_brush) {
		RECT accent = {window_rect.left + 1, window_rect.top + 1, window_rect.left + 4, title_rect.bottom};
		FillRect(mem_dc, &accent, accent_brush);
	}
	/* A low-opacity watermark explains why gameplay is stopped by this console.
	   It belongs to the output surface, behind scrollback/input and other UI. */
	if (!console_window_collapsed &&
		InterlockedCompareExchange(&is_open, 0, 0) &&
		InterlockedCompareExchange(&console_pause_mode, 0, 0) &&
		InterlockedCompareExchange(&console_pause_command_active, 0, 0)) {
		int paused = 0;
		if (console_pause_read_state(&paused) && paused) {
			static HFONT watermark_fonts[4];
			static const int watermark_heights[4] = {34, 30, 26, 22};
			WCHAR watermark_text[64];
			int watermark_len = MultiByteToWideChar(CP_UTF8, 0,
				ctexts_get("CON_PAUSE_WATERMARK", console_ui_is_russian()),
				-1, watermark_text, (int)(sizeof(watermark_text) / sizeof(watermark_text[0])));
			const int watermark_top = output_top + 12;
			const int watermark_bottom = input_top_y - output_gap - 12;
			const int watermark_width = window_right - window_left - 32;
			if (watermark_len > 1 && watermark_bottom > watermark_top + 24 && watermark_width > 100) {
				int dc_state = SaveDC(mem_dc);
				if (dc_state) {
					HFONT selected_font = NULL;
					SIZE text_size = {0, 0};
					for (int i = 0; i < 4; i++) {
						if (!watermark_fonts[i])
							watermark_fonts[i] = CreateFontW(-watermark_heights[i], 0, 0, 0,
								FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
								CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS,
								L"Segoe UI");
						if (!watermark_fonts[i])
							continue;
						SelectObject(mem_dc, watermark_fonts[i]);
						if (GetTextExtentPoint32W(mem_dc, watermark_text, watermark_len - 1, &text_size) &&
							text_size.cx <= watermark_width && text_size.cy > 0 &&
							text_size.cy <= watermark_bottom - watermark_top) {
							selected_font = watermark_fonts[i];
							break;
						}
					}
					if (selected_font && text_size.cx > 0 && text_size.cy > 0) {
						int text_x = window_left + (window_right - window_left - text_size.cx) / 2;
						int text_y = watermark_top + (watermark_bottom - watermark_top - text_size.cy) / 2;
						size_t pixel_count = (size_t)text_size.cx * (size_t)text_size.cy;
						uint32_t *background_pixels = pixel_count <= (size_t)-1 / sizeof(uint32_t)
							? (uint32_t *)malloc(pixel_count * sizeof(uint32_t)) : NULL;
						if (background_pixels) {
							for (int y = 0; y < text_size.cy; y++)
								memcpy(background_pixels + (size_t)y * text_size.cx,
									dib_bits + (size_t)(text_y + y) * w + text_x,
									(size_t)text_size.cx * sizeof(uint32_t));
							SetBkMode(mem_dc, TRANSPARENT);
							SetTextColor(mem_dc, RGB(188, 172, 143));
							TextOutW(mem_dc, text_x, text_y, watermark_text, watermark_len - 1);
							GdiFlush();
							/* GDI already antialiases glyph coverage. Blend its result back
							   toward the panel pixels to make the watermark genuinely subdued. */
							for (int y = 0; y < text_size.cy; y++) {
								uint32_t *destination = dib_bits + (size_t)(text_y + y) * w + text_x;
								const uint32_t *background = background_pixels + (size_t)y * text_size.cx;
								for (int x = 0; x < text_size.cx; x++) {
									uint32_t before = background[x], after = destination[x];
									unsigned r0 = (before >> 16) & 255u, g0 = (before >> 8) & 255u, b0 = before & 255u;
									unsigned r1 = (after >> 16) & 255u, g1 = (after >> 8) & 255u, b1 = after & 255u;
									/* 30% of the antialiased glyph colour over the original panel. */
									unsigned r = (r0 * 178u + r1 * 77u + 127u) / 255u;
									unsigned g = (g0 * 178u + g1 * 77u + 127u) / 255u;
									unsigned b = (b0 * 178u + b1 * 77u + 127u) / 255u;
									destination[x] = (after & 0xff000000u) | (r << 16) | (g << 8) | b;
								}
							}
							free(background_pixels);
						} else {
							/* Allocation failure still leaves a low-contrast fallback. */
							SetBkMode(mem_dc, TRANSPARENT);
							SetTextColor(mem_dc, RGB(76, 77, 72));
							TextOutW(mem_dc, text_x, text_y, watermark_text, watermark_len - 1);
						}
					}
				RestoreDC(mem_dc, dc_state);
			}
		}
	}
	}
	/* Subtle bottom-right grip: use the same subpixel rasterizer as every glyph. */
	if (!console_window_collapsed) {
		console_draw_aa_line(window_right - 17, window_bottom - 5,
			window_right - 5, window_bottom - 17, 1.5f, RGB(132, 119, 98));
		console_draw_aa_line(window_right - 11, window_bottom - 5,
			window_right - 5, window_bottom - 11, 1.5f, RGB(132, 119, 98));
	}
	if (!console_window_collapsed) {
		HBRUSH pause_normal = panel_brush(
			InterlockedCompareExchange(&console_pause_mode, 0, 0) ? PANEL_BRUSH_PAUSE_ACTIVE : PANEL_BRUSH_PAUSE_INACTIVE,
			InterlockedCompareExchange(&console_pause_mode, 0, 0) ? RGB(75, 59, 39) : RGB(43, 46, 47));
		HBRUSH pause_icon = panel_brush(
			InterlockedCompareExchange(&console_pause_mode, 0, 0) ? PANEL_BRUSH_PAUSE_ACTIVE_ICON : PANEL_BRUSH_PAUSE_INACTIVE_ICON,
			InterlockedCompareExchange(&console_pause_mode, 0, 0) ? RGB(229, 169, 91) : RGB(136, 139, 139));
		if (input_brush) FillRect(mem_dc, &input_rect, input_brush);
		if (accent_brush) {
			RECT input_accent = {input_rect.left, input_rect.top, input_rect.left + 2, input_rect.bottom};
			FillRect(mem_dc, &input_accent, accent_brush);
		}
		console_fill_button(mem_dc, &pause_rect, 4, pause_normal);
		if (edge) {
			RECT r = pause_rect; r.bottom = r.top + 1; FillRect(mem_dc, &r, edge);
			r = pause_rect; r.top = r.bottom - 1; FillRect(mem_dc, &r, edge);
			r = pause_rect; r.right = r.left + 1; FillRect(mem_dc, &r, edge);
			r = pause_rect; r.left = r.right - 1; FillRect(mem_dc, &r, edge);
		}
		if (pause_icon) {
			RECT bar = {pause_rect.left + 7, pause_rect.top + 7, pause_rect.left + 10, pause_rect.bottom - 7};
			FillRect(mem_dc, &bar, pause_icon);
			bar.left += 7; bar.right += 7;
			FillRect(mem_dc, &bar, pause_icon);
		}
	}

	if (!console_window_collapsed) {
		int watch_count = console_watch_count_read();
		int watch_rows = console_watch_rows();
		RECT status_rect = {window_left + 1, window_bottom - console_bottom_reserved_height(),
			window_right - 1, window_bottom - watch_rows * CONSOLE_WATCH_ROW_HEIGHT};
		HBRUSH status_brush = panel_brush(PANEL_BRUSH_INPUT, RGB(29, 32, 34));
		if (status_brush) FillRect(mem_dc, &status_rect, status_brush);
		if (edge) {
			RECT separator = {status_rect.left, status_rect.top, status_rect.right, status_rect.top + 1};
			FillRect(mem_dc, &separator, edge);
		}
		{
			int status_text_width = 0;
			if (InterlockedCompareExchange(&console_version_line_printed, 0, 0) && console_version_status[0]) {
				char status_display[COMPLETION_VALUE_LEN];
				SIZE status_size = {0, 0};
				int status_room = status_rect.right - (status_rect.left + 12) - 12;
				status_display[0] = 0;
				SetTextColor(mem_dc, RGB(145, 142, 134));
				if (status_room > 0 && completion_fit_value(mem_dc, console_version_status,
					status_room, status_display, sizeof(status_display), &status_size) > 0) {
					status_text_width = status_size.cx;
					console_text_out_centered(mem_dc, status_rect.left + 12, status_rect.top,
						status_rect.bottom - status_rect.top, status_display, (int)strlen(status_display));
				}
			}
			InterlockedExchange(&console_status_text_width, status_text_width);
		}
		/* Favorites are plain, unframed text. Hover reveals a small unpin glyph at
		   the right edge of that item; only the glyph is an interactive target. */
		for (int row = 0; row < watch_rows; row++) {
			RECT row_rect = {window_left + 1,
				status_rect.bottom + row * CONSOLE_WATCH_ROW_HEIGHT,
				window_right - 1,
				min(status_rect.bottom + (row + 1) * CONSOLE_WATCH_ROW_HEIGHT, window_bottom - 1)};
			HBRUSH row_brush = panel_brush(PANEL_BRUSH_INPUT, RGB(27, 30, 32));
			if (row_brush) FillRect(mem_dc, &row_rect, row_brush);
			if (edge) {
				RECT separator = {row_rect.left, row_rect.top, row_rect.right, row_rect.top + 1};
				FillRect(mem_dc, &separator, edge);
			}
			int row_columns = min(CONSOLE_WATCH_COLUMNS, watch_count - row * CONSOLE_WATCH_COLUMNS);
			for (int col = 0; col < row_columns; col++) {
				int idx = row * CONSOLE_WATCH_COLUMNS + col;
				RECT item_rect, unpin_rect;
				char value[COMPLETION_VALUE_LEN], label[COMPLETION_NAME_LEN + COMPLETION_VALUE_LEN + 4];
				char shown[COMPLETION_NAME_LEN + COMPLETION_VALUE_LEN + 8];
				SIZE text_size = {0, 0};
				int hovered;
				if (idx >= watch_count || !console_watch_item_rect(idx, &item_rect)) continue;
				hovered = idx == (int)InterlockedCompareExchange(&console_watch_hover_index, 0, 0);
				value[0] = 0;
				game_dvar_string(console_watch_names[idx], value, sizeof(value));
				snprintf(label, sizeof(label), "%s = %s", console_watch_names[idx], value[0] ? value : "?");
				/* Reserve icon space even before hover, preventing label jitter. */
				if (completion_fit_value(mem_dc, label,
					(item_rect.right - item_rect.left) - 26, shown, sizeof(shown), &text_size) > 0) {
					SetTextColor(mem_dc, RGB(216, 187, 143));
					console_text_out_centered(mem_dc, item_rect.left + 2, item_rect.top,
						item_rect.bottom - item_rect.top, shown, (int)strlen(shown));
				}
				if (hovered && console_watch_unpin_rect(idx, &unpin_rect)) {
					int cx = (unpin_rect.left + unpin_rect.right) / 2;
					int cy = (unpin_rect.top + unpin_rect.bottom) / 2;
					COLORREF glyph = RGB(222, 163, 91);
					/* Thumbtack with a short cut through its head: an explicit unpin affordance. */
					console_draw_aa_line(cx - 4, cy - 5, cx + 4, cy - 5, 1.5f, glyph);
					console_draw_aa_line(cx - 4, cy - 5, cx, cy - 1, 1.4f, glyph);
					console_draw_aa_line(cx + 4, cy - 5, cx, cy - 1, 1.4f, glyph);
					console_draw_aa_line(cx, cy - 1, cx, cy + 5, 1.4f, glyph);
					console_draw_aa_line(cx - 5, cy + 5, cx + 5, cy + 5, 1.2f, glyph);
					console_draw_aa_line(cx - 6, cy + 6, cx + 6, cy - 6, 1.3f, glyph);
				}
			}
		}
	}

	{
		int brand_left = window_left + 18;
		if (console_window_collapsed) {
			console_draw_aa_line(window_left + 7, window_top + 12,
				window_left + 14, window_top + 21, 1.8f, RGB(199, 146, 72));
			console_draw_aa_line(window_left + 14, window_top + 21,
				window_left + 21, window_top + 12, 1.8f, RGB(199, 146, 72));
			brand_left += 10;
		}
		SetTextColor(mem_dc, RGB(199, 146, 72));
		console_text_out_centered(mem_dc, brand_left, window_top, header_h, "MW2", 3);
		{
			SIZE brand_size = {0, 0};
			console_text_width(mem_dc, "MW2", 3, &brand_size);
			SetTextColor(mem_dc, RGB(226, 225, 219));
			console_text_out_centered(mem_dc, brand_left + brand_size.cx + 7, window_top, header_h,
				"ENHANCED", 8);
		}
	}
	{
		const char *layout_label = InterlockedCompareExchange(&console_legacy_layout, 0, 0) ? "Legacy" : "Enhanced";
		const char *language_label = console_ui_is_russian() ? "RU" : "EN";
		char time_text[64] = "";
		SYSTEMTIME local_time;
		RECT layout_rect = {window_right - 182, window_top + 6, window_right - 88, window_top + 30};
		RECT language_rect = {window_right - 236, window_top + 6, window_right - 186, window_top + 30};
		RECT time_rect = {window_left + 164, window_top + 6, window_right - 244, window_top + 30};
		RECT buttons[3] = {
			{window_right - 84, window_top + 5, window_right - 60, window_top + 31},
			{window_right - 58, window_top + 5, window_right - 34, window_top + 31},
			{window_right - 32, window_top + 5, window_right - 8, window_top + 31}
		};
		SIZE layout_size = {0, 0}, language_size = {0, 0}, time_size = {0, 0};
		HBRUSH button_brush = panel_brush(PANEL_BRUSH_INPUT, RGB(27, 30, 32));
		GetLocalTime(&local_time);
		if (!console_format_local_time(&local_time, time_text, sizeof(time_text)))
			time_text[0] = 0;
		console_text_width(mem_dc, layout_label, (int)strlen(layout_label), &layout_size);
		console_text_width(mem_dc, language_label, (int)strlen(language_label), &language_size);
		console_text_width(mem_dc, time_text, (int)strlen(time_text), &time_size);
		if (time_rect.left < window_left + 164) time_rect.left = window_left + 164;
		if (time_size.cx + 8 > time_rect.right - time_rect.left) {
			time_text[0] = 0;
			time_size.cx = 0;
		}
		console_fill_button(mem_dc, &language_rect, 6, button_brush);
		console_fill_button(mem_dc, &layout_rect, 5, button_brush);
		for (int i = 0; i < 3; i++) console_fill_button(mem_dc, &buttons[i], i + 1, button_brush);
		if (edge) {
			RECT r = language_rect;
			r.bottom = r.top + 1; FillRect(mem_dc, &r, edge);
			r = language_rect; r.top = r.bottom - 1; FillRect(mem_dc, &r, edge);
			r = language_rect; r.right = r.left + 1; FillRect(mem_dc, &r, edge);
			r = language_rect; r.left = r.right - 1; FillRect(mem_dc, &r, edge);
			r = layout_rect; r.bottom = r.top + 1; FillRect(mem_dc, &r, edge);
			r = layout_rect; r.top = r.bottom - 1; FillRect(mem_dc, &r, edge);
			r = layout_rect; r.right = r.left + 1; FillRect(mem_dc, &r, edge);
			r = layout_rect; r.left = r.right - 1; FillRect(mem_dc, &r, edge);
			for (int i = 0; i < 3; i++) {
				r = buttons[i]; r.bottom = r.top + 1; FillRect(mem_dc, &r, edge);
				r = buttons[i]; r.top = r.bottom - 1; FillRect(mem_dc, &r, edge);
				r = buttons[i]; r.right = r.left + 1; FillRect(mem_dc, &r, edge);
				r = buttons[i]; r.left = r.right - 1; FillRect(mem_dc, &r, edge);
			}
		}
		SetTextColor(mem_dc, RGB(229, 169, 91));
		console_text_out_centered(mem_dc, language_rect.left + (language_rect.right - language_rect.left - language_size.cx) / 2,
			language_rect.top, language_rect.bottom - language_rect.top, language_label, (int)strlen(language_label));
		console_text_out_centered(mem_dc, layout_rect.left + (layout_rect.right - layout_rect.left - layout_size.cx) / 2,
			layout_rect.top, layout_rect.bottom - layout_rect.top, layout_label, (int)strlen(layout_label));
		SetTextColor(mem_dc, RGB(166, 160, 149));
		console_text_out_centered(mem_dc, time_rect.left + (time_rect.right - time_rect.left - time_size.cx) / 2,
			time_rect.top, time_rect.bottom - time_rect.top, time_text, (int)strlen(time_text));
		/* Minimize and maximize/restore glyphs share the 16×16 coverage rasterizer
		   used by the close and collapsed chevron icons. */
		console_draw_aa_line(buttons[0].left + 6, buttons[0].top + 18,
			buttons[0].right - 6, buttons[0].top + 18, 1.7f, RGB(202, 164, 105));
		if (full_console) {
			/* Restore glyph: two offset, unfilled window outlines. */
			console_draw_aa_line(buttons[1].left + 10, buttons[1].top + 7,
				buttons[1].right - 5, buttons[1].top + 7, 1.5f, RGB(202, 164, 105));
			console_draw_aa_line(buttons[1].right - 5, buttons[1].top + 7,
				buttons[1].right - 5, buttons[1].bottom - 9, 1.5f, RGB(202, 164, 105));
			console_draw_aa_line(buttons[1].left + 7, buttons[1].top + 10,
				buttons[1].left + 7, buttons[1].bottom - 7, 1.5f, RGB(202, 164, 105));
			console_draw_aa_line(buttons[1].left + 7, buttons[1].bottom - 7,
				buttons[1].right - 8, buttons[1].bottom - 7, 1.5f, RGB(202, 164, 105));
			console_draw_aa_line(buttons[1].right - 8, buttons[1].bottom - 7,
				buttons[1].right - 8, buttons[1].top + 10, 1.5f, RGB(202, 164, 105));
			console_draw_aa_line(buttons[1].right - 8, buttons[1].top + 10,
				buttons[1].left + 7, buttons[1].top + 10, 1.5f, RGB(202, 164, 105));
		} else {
			console_draw_aa_line(buttons[1].left + 7, buttons[1].top + 7,
				buttons[1].right - 7, buttons[1].top + 7, 1.5f, RGB(202, 164, 105));
			console_draw_aa_line(buttons[1].right - 7, buttons[1].top + 7,
				buttons[1].right - 7, buttons[1].bottom - 7, 1.5f, RGB(202, 164, 105));
			console_draw_aa_line(buttons[1].right - 7, buttons[1].bottom - 7,
				buttons[1].left + 7, buttons[1].bottom - 7, 1.5f, RGB(202, 164, 105));
			console_draw_aa_line(buttons[1].left + 7, buttons[1].bottom - 7,
				buttons[1].left + 7, buttons[1].top + 7, 1.5f, RGB(202, 164, 105));
		}
		console_draw_aa_line(buttons[2].left + 5, buttons[2].top + 6,
			buttons[2].right - 5, buttons[2].bottom - 6, 1.8f, RGB(202, 164, 105));
		console_draw_aa_line(buttons[2].right - 5, buttons[2].top + 6,
			buttons[2].left + 5, buttons[2].bottom - 6, 1.8f, RGB(202, 164, 105));
		{
			int hover = (int)InterlockedCompareExchange(&console_header_hover_button, 0, 0);
			if (hover && !console_window_collapsed && (DWORD)(GetTickCount() - console_header_hover_tick) >= 550) {
				int pause_enabled = InterlockedCompareExchange(&console_pause_mode, 0, 0) != 0;
				const char *tip_name = hover == 1 ? "CON_TIP_MINIMIZE" : hover == 2 ?
					(full_console ? "CON_TIP_RESTORE" : "CON_TIP_MAXIMIZE") : hover == 3 ? "CON_TIP_CLOSE" :
					hover == 4 ? (pause_enabled ? "CON_TIP_PAUSE_ON" : "CON_TIP_PAUSE_OFF") :
					hover == 5 ? "CON_TIP_LAYOUT" : "CON_TIP_LANGUAGE";
				const char *tip = ctexts_get(tip_name, console_ui_is_russian());
				SIZE tip_size = {0, 0};
				RECT tip_rect;
				int tip_width = 0;
				console_ui_text_width(mem_dc, tip, &tip_size);
				{
					tip_width = tip_size.cx + 16;
					int max_tip_width = window_right - window_left - 16;
					int anchor = hover == 4 ? pause_rect.right : hover == 5 ? layout_rect.right :
						hover == 6 ? language_rect.right : buttons[hover - 1].right;
					if (tip_width > max_tip_width) tip_width = max_tip_width;
					if (tip_width < 24) tip_width = 24;
					tip_rect.left = anchor - tip_width;
					tip_rect.right = anchor;
					if (tip_rect.left < window_left + 8) {
						tip_rect.left = window_left + 8;
						tip_rect.right = tip_rect.left + tip_width;
					}
					if (tip_rect.right > window_right - 8) {
						tip_rect.right = window_right - 8;
						tip_rect.left = tip_rect.right - tip_width;
					}
				}
				if (hover == 4) {
					tip_rect.left = pause_rect.left;
					tip_rect.right = tip_rect.left + tip_width;
					if (tip_rect.right > window_right - 8) {
						tip_rect.right = window_right - 8;
						tip_rect.left = tip_rect.right - tip_width;
					}
					tip_rect.top = pause_rect.top - 28;
				} else {
					tip_rect.top = window_top + header_h + 4;
				}
				tip_rect.bottom = tip_rect.top + 24;
				/* Remember the tooltip here, but draw it after output, autocomplete and
				   context menus so later text can never paint over its surface. */
				button_tooltip_rect = tip_rect;
				lstrcpynA(button_tooltip_text, tip, sizeof(button_tooltip_text));
			}
		}
	}
	if (edge) {
		RECT sep = {window_rect.left + 1, title_rect.bottom, window_rect.right - 1, title_rect.bottom + 1};
		FillRect(mem_dc, &sep, edge);
	}
	if (console_window_collapsed) {
		SetRectEmpty(&completion_rect);
		SetRectEmpty(&output_scroll_track);
		SetRectEmpty(&output_scroll_thumb);
		SetRectEmpty(&completion_scroll_track);
		SetRectEmpty(&completion_scroll_thumb);
		GdiFlush();
		for (int y = window_rect.top; y < window_rect.bottom; y++) {
			uint32_t *row = dib_bits + (size_t)y * (size_t)w;
			for (int x = window_rect.left; x < window_rect.right; x++)
				row[x] = (row[x] & 0x00ffffffu) | 0xE8000000u;
		}
		dib_panel_h = panel_h;
		LeaveCriticalSection(&lock);
		return 1;
	}

	{
		int output_dc_state = SaveDC(mem_dc);
		IntersectClipRect(mem_dc, window_left + 8, output_top, window_right - 14, input_top_y - output_gap);
		if (button_tooltip_rect.right > button_tooltip_rect.left &&
			button_tooltip_rect.bottom > button_tooltip_rect.top)
			ExcludeClipRect(mem_dc, button_tooltip_rect.left, button_tooltip_rect.top,
				button_tooltip_rect.right, button_tooltip_rect.bottom);
		for (int k = 0; k < rows; k++) {
		int output_offset = scroll + k;
		int seq = line_count - 1 - output_offset;
		if (seq < 0 || seq >= line_count) continue;
		int idx = (line_next - 1 - output_offset + MAX_LINES * 2) % MAX_LINES;
		const char *source = lines[idx];
		UINT text_codepage = line_codepage[idx] ? line_codepage[idx] : console_codepage();
		char text[LINE_LEN];
		int n = 0;
		for (const char *c = source; *c && n < LINE_LEN - 1; c++) {
			if (*c == '^' && c[1] >= '0' && c[1] <= '9') {
				c++;
				continue;
			}
			text[n++] = *c;
		}
		text[n] = 0;
		int y = input_top_y - lh - output_gap - lh * k;
		if (output_has_selection()) {
			int a_line = output_sel_a_line, a_col = output_sel_a_col;
			int b_line = output_sel_b_line, b_col = output_sel_b_col;
			if (a_line > b_line || (a_line == b_line && a_col > b_col)) {
				int t = a_line; a_line = b_line; b_line = t;
				t = a_col; a_col = b_col; b_col = t;
			}
			if (seq >= a_line && seq <= b_line) {
				int from = seq == a_line ? a_col : 0;
				int to = seq == b_line ? b_col : n;
				if (to > from && from < n) {
					SIZE aa = {0, 0}, bb = {0, 0};
					console_text_width_cp(mem_dc, text, visible_byte_offset_cp(text, from, text_codepage), &aa, text_codepage);
					console_text_width_cp(mem_dc, text, visible_byte_offset_cp(text, to, text_codepage), &bb, text_codepage);
					RECT selected = {window_left + 16 + aa.cx, y, window_right - 12, y + lh};
					selected.right = window_left + 16 + bb.cx;
					HBRUSH selection = panel_brush(PANEL_BRUSH_SELECTION, RGB(75, 64, 48));
					if (selection) FillRect(mem_dc, &selected, selection);
				}
			}
		}
		if (output_search_match_seq == seq && output_search_query[0]) {
			int match_at = -1;
			int match_len = console_output_find_in_line(text, output_search_query, &match_at);
			if (match_len > 0 && match_at >= 0) {
				SIZE prefix_size = {0, 0}, match_size = {0, 0};
				console_text_width_cp(mem_dc, text, match_at, &prefix_size, text_codepage);
				console_text_width_cp(mem_dc, text + match_at, match_len, &match_size, text_codepage);
				{
					RECT mark = {window_left + 15 + prefix_size.cx, y + 1,
						window_left + 16 + prefix_size.cx + match_size.cx, y + lh - 1};
					HBRUSH search_brush = panel_brush(PANEL_BRUSH_SELECTED, RGB(111, 82, 43));
					if (search_brush) FillRect(mem_dc, &mark, search_brush);
				}
			}
		}
		{
			int source_pos = 0, run_start = 0;
			int draw_x = window_left + 16;
			COLORREF run_color = palette[7];
			for (const char *c = source; *c && source_pos < n; c++) {
				if (*c == '^' && c[1] >= '0' && c[1] <= '9') {
					if (source_pos > run_start) {
						SIZE run_size = {0, 0};
						SetTextColor(mem_dc, run_color);
						console_text_out_cp(mem_dc, draw_x, y, text + run_start, source_pos - run_start, text_codepage);
						console_text_width_cp(mem_dc, text + run_start, source_pos - run_start, &run_size, text_codepage);
						draw_x += run_size.cx;
					}
					run_color = palette[c[1] - '0'];
					run_start = source_pos;
					c++;
				} else {
					source_pos++;
				}
			}
			if (n > run_start) {
				SetTextColor(mem_dc, run_color);
				console_text_out_cp(mem_dc, draw_x, y, text + run_start, n - run_start, text_codepage);
			}
		}
		}
		if (output_dc_state) RestoreDC(mem_dc, output_dc_state);
	}
	/* Output scrollbar: its thumb tracks the visible portion of the scrollback. */
	if (line_count > rows && rows > 0) {
		int track_height;
		int thumb_height;
		int travel;
		output_scroll_track.left = window_right - 12;
		output_scroll_track.right = window_right - 4;
		output_scroll_track.top = output_top + 1;
		output_scroll_track.bottom = input_top_y - output_gap - 1;
		track_height = output_scroll_track.bottom - output_scroll_track.top;
		output_scroll_max = line_count - rows;
		output_scroll_page = rows;
		thumb_height = (int)((long long)track_height * rows / line_count);
		if (thumb_height < 18) thumb_height = 18;
		if (thumb_height > track_height) thumb_height = track_height;
		travel = track_height - thumb_height;
		output_scroll_thumb.left = output_scroll_track.left;
		output_scroll_thumb.right = output_scroll_track.right;
		output_scroll_thumb.bottom = output_scroll_track.bottom -
			(output_scroll_max > 0 ? (int)((long long)travel * scroll / output_scroll_max) : 0);
		output_scroll_thumb.top = output_scroll_thumb.bottom - thumb_height;
		{
			HBRUSH track = panel_brush(PANEL_BRUSH_POPUP, RGB(24, 27, 29));
			HBRUSH thumb = panel_brush(PANEL_BRUSH_ACCENT, RGB(155, 119, 72));
			if (track) FillRect(mem_dc, &output_scroll_track, track);
			if (thumb) FillRect(mem_dc, &output_scroll_thumb, thumb);
		}
	}

	{
		int text_left = window_left + (int)InterlockedCompareExchange(&console_input_left_offset, 0, 0);
		for (int row = 0; row < input_lines; row++) {
			int first = row * input_columns;
			int count = input_len - first;
			int line_y = input_top_y + row * lh;
			if (count > input_columns) count = input_columns;
			if (count < 0) count = 0;
			if (has_selection()) {
				int sel_first = selection_anchor < cursor ? selection_anchor : cursor;
				int sel_last = selection_anchor > cursor ? selection_anchor : cursor;
				int from = sel_first > first ? sel_first : first;
				int to = sel_last < first + count ? sel_last : first + count;
				if (to > from) {
					SIZE a = {0, 0}, b = {0, 0};
					console_text_width(mem_dc, input + first, from - first, &a);
					console_text_width(mem_dc, input + first, to - first, &b);
					RECT selected = {text_left + a.cx, line_y + 1, text_left + b.cx, line_y + lh - 1};
					HBRUSH selection = panel_brush(PANEL_BRUSH_SELECTION, RGB(75, 64, 48));
					if (selection) FillRect(mem_dc, &selected, selection);
				}
			}
			if (count > 0)
				console_text_out(mem_dc, text_left, line_y, input + first, count);
		}
	}
	SetRectEmpty(&completion_rect);
	completion_rows = completion_columns = completion_cell_width = 0;
	SetTextColor(mem_dc, RGB(229, 169, 91));
	if (output_search_mode)
		console_ui_text_out_centered(mem_dc, window_left + 44, input_top_y, lh,
			ctexts_get("CON_FIND_PREFIX", console_ui_is_russian()));
	else
		console_text_out(mem_dc, window_left + 44, input_top_y, "> ", 2);
	SetTextColor(mem_dc, RGB(229, 225, 216));
	if (InterlockedCompareExchange(&console_window_active, 0, 0) && ((GetTickCount() / 500) & 1)) {
		int caret_row = cursor / input_columns;
		int caret_col = cursor % input_columns;
		int line_start, line_length;
		SIZE caret = {0, 0};
		if (caret_row >= input_lines) { caret_row = input_lines - 1; caret_col = input_columns; }
		line_start = caret_row * input_columns;
		line_length = input_len - line_start;
		if (line_length < 0) line_length = 0;
		if (line_length > input_columns) line_length = input_columns;
		if (caret_col > line_length) caret_col = line_length;
		console_text_width(mem_dc, input + line_start, caret_col, &caret);
		{
			int caret_y = input_top_y + caret_row * lh;
			int editor_left = window_left + (int)InterlockedCompareExchange(&console_input_left_offset, 0, 0);
			RECT caret_rect = {editor_left + caret.cx, caret_y + 2,
				editor_left + caret.cx + 2, caret_y + lh - 2};
			HBRUSH caret_brush = panel_brush(PANEL_BRUSH_CARET, RGB(225, 177, 95));
			if (caret_brush) FillRect(mem_dc, &caret_rect, caret_brush);
		}
	}

	if (completion_count > 0 && visible > 0 && popup_h > 0) {
		int max_scroll = total_rows - visible;
		if (completion_selected >= 0) {
			int selected_row = completion_selected / cols;
			if (selected_row < completion_scroll) completion_scroll = selected_row;
			else if (selected_row >= completion_scroll + visible) completion_scroll = selected_row - visible + 1;
		}
		if (completion_scroll > max_scroll) completion_scroll = max_scroll;
		if (completion_scroll < 0) completion_scroll = 0;
		if (placement == 4 && popup_side == 1) {
			popup_rect.right = window_left - 8;
			popup_rect.left = popup_rect.right - popup_width;
		} else if (placement == 4 && popup_side == 2) {
			popup_rect.left = window_right + 8;
			popup_rect.right = popup_rect.left + popup_width;
		} else {
			popup_rect.left = window_left + 10;
			popup_rect.right = popup_rect.left + popup_width;
			if (popup_rect.right > window_right - 10) popup_rect.right = window_right - 10;
		}
		popup_rect.top = popup_top;
		popup_rect.bottom = popup_top + popup_h;
		completion_rect = popup_rect;
		for (int pin_i = 0; pin_i < MAX_COMPLETIONS; pin_i++) SetRectEmpty(&completion_pin_rects[pin_i]);
		completion_top = popup_top;
		completion_header_height = header_h;
		completion_rows = visible;
		completion_columns = cols;
		completion_cell_width = cellw;
		if (shadow_brush) {
			RECT shadow = {popup_rect.left + 3, popup_rect.top + 3, popup_rect.right + 3, popup_rect.bottom + 3};
			if (shadow.right > w) shadow.right = w;
			if (shadow.bottom > bitmap_h) shadow.bottom = bitmap_h;
			if (shadow.right > shadow.left && shadow.bottom > shadow.top) FillRect(mem_dc, &shadow, shadow_brush);
		}
		if (popup_brush) FillRect(mem_dc, &popup_rect, popup_brush);
		if (edge) {
			RECT line = {popup_rect.left, popup_rect.top, popup_rect.right, popup_rect.top + 1};
			FillRect(mem_dc, &line, edge);
		}
		if (accent_brush) {
			RECT accent = {popup_rect.left, popup_rect.top, popup_rect.left + 3, popup_rect.bottom};
			FillRect(mem_dc, &accent, accent_brush);
		}
		SetTextColor(mem_dc, RGB(166, 160, 149));
		{
			char label[192];
			{
				int ru = console_ui_is_russian();
				char count_label[48];
				format_command_count(count_label, sizeof(count_label), completion_total, ru);
				if (completion_catalog_truncated)
					snprintf(label, sizeof(label), ctexts_get("CON_LIST_INCOMPLETE", ru), completion_total, ctexts_get(command_noun_name(completion_total, ru), ru));
				else if (completion_total > completion_count)
					snprintf(label, sizeof(label), ctexts_get("CON_LIST_SHOWING", ru), count_label, completion_count);
				else
					snprintf(label, sizeof(label), "%s", count_label);
			}
			{
				int label_dc_state = SaveDC(mem_dc);
				if (label_dc_state)
					IntersectClipRect(mem_dc, popup_rect.left + 8, popup_top, popup_rect.right - 8, popup_top + header_h);
				console_ui_text_out_centered(mem_dc, popup_rect.left + 12, popup_top, header_h, label);
				if (label_dc_state) RestoreDC(mem_dc, label_dc_state);
			}
		}
		for (int r = 0; r < visible; r++) {
			int y = popup_top + header_h + r * lh;
			if (show_single_default && r == 1) {
				completion_item_t *item = &completion_items[0];
				char shown[COMPLETION_VALUE_LEN];
				SIZE value_size = {0, 0};
				int room = cellw - 68;
				SetTextColor(mem_dc, RGB(153, 140, 140));
				console_ui_text_out_centered(mem_dc, popup_rect.left + 12, y, lh, ctexts_get("CON_DEFAULT_LABEL", console_ui_is_russian()));
				if (room > 20 && completion_fit_value(mem_dc, item->default_value, room, shown, sizeof(shown), &value_size) > 0) {
					int value_x = popup_rect.left + cellw - 8 - value_size.cx;
					SetTextColor(mem_dc, RGB(188, 173, 147));
					console_text_out_centered(mem_dc, value_x, y, lh, shown, (int)strlen(shown));
				}
				continue;
			}
			for (int col = 0; col < cols; col++) {
				int idx = (completion_scroll + r) * cols + col;
				int x = popup_rect.left + 8 + col * cellw;
				completion_item_t *item;
				SIZE name_size = {0, 0};
				if (idx >= completion_count) break;
				item = &completion_items[idx];
				{
					int cell_dc_state = SaveDC(mem_dc);
					int cell_right = min(x + cellw, popup_rect.right - 8);
					int text_right = cell_right - (item->is_dvar ? 24 : 0);
					if (cell_dc_state) IntersectClipRect(mem_dc, x, y, cell_right, y + lh);
					if (idx == completion_selected) {
						RECT selected = {x, y, cell_right, y + lh};
						HBRUSH selected_brush = panel_brush(PANEL_BRUSH_SELECTED, RGB(81, 67, 47));
						if (selected_brush) FillRect(mem_dc, &selected, selected_brush);
					}
					{
						int text_dc_state = SaveDC(mem_dc);
						if (text_dc_state) IntersectClipRect(mem_dc, x, y, text_right, y + lh);
						console_text_width(mem_dc, item->name, (int)strlen(item->name), &name_size);
						SetTextColor(mem_dc, idx == completion_selected ? RGB(255, 246, 229) : RGB(218, 214, 204));
						console_text_out_centered(mem_dc, x + 8, y, lh, item->name, (int)strlen(item->name));
						if (item->is_dvar && item->value[0]) {
							char shown[COMPLETION_VALUE_LEN];
							SIZE value_size = {0, 0};
							int value_right = cell_right - 24;
							int room = value_right - (x + 8 + name_size.cx + 8);
							if (room > 20 && completion_fit_value(mem_dc, item->value, room, shown, sizeof(shown), &value_size) > 0) {
								int value_x = value_right - value_size.cx;
								if (value_x > x + 8 + name_size.cx + 8) {
									SetTextColor(mem_dc, idx == completion_selected ? RGB(247, 220, 178) : RGB(201, 159, 104));
									console_text_out_centered(mem_dc, value_x, y, lh, shown, (int)strlen(shown));
								}
							}
						}
						if (text_dc_state) RestoreDC(mem_dc, text_dc_state);
					}
					if (item->is_dvar && cell_right - x >= 42) {
						RECT pin = {cell_right - 21, y + 2, cell_right - 3, y + lh - 2};
						int is_pinned = console_name_in_list(console_watch_names, console_watch_count_read(), item->name) >= 0;
						COLORREF pin_color = is_pinned ? RGB(220, 165, 91) : RGB(133, 139, 137);
						completion_pin_rects[idx] = pin;
						/* Small angled pin: head, neck, shaft and point, all coverage-AA. */
						console_draw_aa_line(pin.left + 5, pin.top + 3, pin.left + 11, pin.top + 3, 1.6f, pin_color);
						console_draw_aa_line(pin.left + 5, pin.top + 3, pin.left + 8, pin.top + 7, 1.5f, pin_color);
						console_draw_aa_line(pin.left + 11, pin.top + 3, pin.left + 8, pin.top + 7, 1.5f, pin_color);
						console_draw_aa_line(pin.left + 8, pin.top + 7, pin.left + 8, pin.top + 12, 1.4f, pin_color);
						console_draw_aa_line(pin.left + 5, pin.top + 12, pin.left + 11, pin.top + 12, 1.2f, pin_color);
					}
					if (cell_dc_state) RestoreDC(mem_dc, cell_dc_state);
				}
			}
		}
		completion_scroll_max = total_rows - visible;
		completion_scroll_page = visible;
		if (completion_scroll_max > 0 && visible > 0) {
			int track_height, thumb_height, travel;
			completion_scroll_track.left = popup_rect.right - 12;
			completion_scroll_track.right = popup_rect.right - 4;
			completion_scroll_track.top = popup_rect.top + header_h + 2;
			completion_scroll_track.bottom = popup_rect.bottom - 2;
			track_height = completion_scroll_track.bottom - completion_scroll_track.top;
			thumb_height = (int)((long long)track_height * visible / total_rows);
			if (thumb_height < 16) thumb_height = 16;
			if (thumb_height > track_height) thumb_height = track_height;
			travel = track_height - thumb_height;
			completion_scroll_thumb.left = completion_scroll_track.left;
			completion_scroll_thumb.right = completion_scroll_track.right;
			completion_scroll_thumb.top = completion_scroll_track.top +
				(int)((long long)travel * completion_scroll / completion_scroll_max);
			completion_scroll_thumb.bottom = completion_scroll_thumb.top + thumb_height;
			{
				HBRUSH track = panel_brush(PANEL_BRUSH_POPUP, RGB(24, 27, 29));
				HBRUSH thumb = panel_brush(PANEL_BRUSH_ACCENT, RGB(155, 119, 72));
				if (track) FillRect(mem_dc, &completion_scroll_track, track);
				if (thumb) FillRect(mem_dc, &completion_scroll_thumb, thumb);
			}
		}
	}

	/* New command output is shown as a detached toast while the user keeps a
	   historical scroll position. The scrollback itself remains a single view. */
	if (output_toast_count > 0 && (LONG)(GetTickCount() - output_toast_expires) < 0) {
		int toast_width = 300;
		int toast_height = 30 + output_toast_count * (lh + 2);
		int toast_left, toast_top, available_below;
		for (int i = 0; i < output_toast_count; i++) {
			SIZE z = {0, 0};
			console_text_width_cp(mem_dc, output_toast_lines[i], (int)strlen(output_toast_lines[i]), &z,
				output_toast_codepages[i] ? output_toast_codepages[i] : console_codepage());
			if (z.cx + 28 > toast_width) toast_width = z.cx + 28;
		}
		if (toast_width > 420) toast_width = 420;
		if (toast_width > w - 16) toast_width = w - 16;
		if (toast_width < 140) toast_width = 140;
		toast_left = window_right - toast_width;
		if (toast_left < 8) toast_left = 8;
		available_below = panel_h - 8 - (window_bottom + 8);
		if (available_below < toast_height) {
			/* If the console is close to the bottom edge, use a clear side strip
			   rather than covering its input/status controls. */
			if (window_right + 8 + toast_width <= w - 8) {
				toast_left = window_right + 8;
				toast_top = window_bottom - toast_height;
			} else if (window_left >= toast_width + 16) {
				toast_left = window_left - toast_width - 8;
				toast_top = window_bottom - toast_height;
			} else {
				toast_height = available_below > 20 ? available_below : 0;
				toast_top = window_bottom + 8;
			}
		} else {
			toast_top = window_bottom + 8;
		}
		if (toast_height > 20 && toast_left >= 4 && toast_left + toast_width <= w - 4 &&
			toast_top >= 4 && toast_top + toast_height <= panel_h - 4) {
			HBRUSH toast_back = panel_brush(PANEL_BRUSH_POPUP, RGB(28, 32, 34));
			HBRUSH toast_edge = panel_brush(PANEL_BRUSH_EDGE, RGB(91, 96, 98));
			HBRUSH toast_accent = panel_brush(PANEL_BRUSH_ACCENT, RGB(199, 146, 72));
			RECT r;
			output_toast_rect.left = toast_left;
			output_toast_rect.top = toast_top;
			output_toast_rect.right = toast_left + toast_width;
			output_toast_rect.bottom = toast_top + toast_height;
			if (shadow_brush) {
				r = output_toast_rect; OffsetRect(&r, 3, 3); FillRect(mem_dc, &r, shadow_brush);
			}
			if (toast_back) FillRect(mem_dc, &output_toast_rect, toast_back);
			if (toast_edge) {
				r = output_toast_rect; r.bottom = r.top + 1; FillRect(mem_dc, &r, toast_edge);
				r = output_toast_rect; r.top = r.bottom - 1; FillRect(mem_dc, &r, toast_edge);
				r = output_toast_rect; r.right = r.left + 1; FillRect(mem_dc, &r, toast_edge);
				r = output_toast_rect; r.left = r.right - 1; FillRect(mem_dc, &r, toast_edge);
			}
			if (toast_accent) {
				r = output_toast_rect; r.right = r.left + 3; FillRect(mem_dc, &r, toast_accent);
			}
			{
				RECT header = {toast_left + 10, toast_top + 2, toast_left + toast_width - 26, toast_top + 20};
				SetTextColor(mem_dc, RGB(199, 146, 72));
				console_ui_text_out_centered(mem_dc, header.left, header.top, header.bottom - header.top,
					ctexts_get("CON_NEW_OUTPUT", console_ui_is_russian()));
				output_toast_close_rect.left = toast_left + toast_width - 22;
				output_toast_close_rect.top = toast_top + 2;
				output_toast_close_rect.right = toast_left + toast_width - 5;
				output_toast_close_rect.bottom = toast_top + 19;
				console_draw_aa_line(output_toast_close_rect.left + 5, output_toast_close_rect.top + 4,
					output_toast_close_rect.right - 5, output_toast_close_rect.bottom - 4, 1.4f, RGB(205, 195, 178));
				console_draw_aa_line(output_toast_close_rect.right - 5, output_toast_close_rect.top + 4,
					output_toast_close_rect.left + 5, output_toast_close_rect.bottom - 4, 1.4f, RGB(205, 195, 178));
			}
			SetTextColor(mem_dc, RGB(225, 220, 209));
			{
				int saved = SaveDC(mem_dc);
				if (saved) IntersectClipRect(mem_dc, toast_left + 12, toast_top + 22,
					toast_left + toast_width - 8, toast_top + toast_height - 4);
				for (int i = 0; i < output_toast_count; i++) {
					int y = toast_top + 23 + i * (lh + 2);
					console_text_out_cp(mem_dc, toast_left + 12, console_text_vertical_origin(mem_dc, y, lh),
						output_toast_lines[i], (int)strlen(output_toast_lines[i]),
						output_toast_codepages[i] ? output_toast_codepages[i] : console_codepage());
				}
				if (saved) RestoreDC(mem_dc, saved);
			}
		}
	} else {
		output_toast_count = 0;
		output_toast_capture_until = 0;
		output_toast_expires = 0;
	}

	if (context_menu_open && context_menu_rect.right > context_menu_rect.left &&
		context_menu_rect.bottom > context_menu_rect.top) {
		int mode = (int)InterlockedCompareExchange(&context_menu_mode, 0, 0);
		int selected = (int)InterlockedCompareExchange(&context_menu_selected, 0, 0);
		HBRUSH menu_brush = panel_brush(PANEL_BRUSH_POPUP, RGB(40, 44, 46));
		HBRUSH menu_edge = panel_brush(PANEL_BRUSH_EDGE, RGB(85, 90, 92));
		HBRUSH menu_selection = panel_brush(PANEL_BRUSH_SELECTED, RGB(81, 67, 47));
		if (shadow_brush) {
			RECT shadow = {context_menu_rect.left + 3, context_menu_rect.top + 3,
				context_menu_rect.right + 3, context_menu_rect.bottom + 3};
			if (shadow.right > w) shadow.right = w;
			if (shadow.bottom > bitmap_h) shadow.bottom = bitmap_h;
			if (shadow.right > shadow.left && shadow.bottom > shadow.top) FillRect(mem_dc, &shadow, shadow_brush);
		}
		if (menu_brush) FillRect(mem_dc, &context_menu_rect, menu_brush);
		if (menu_edge) {
			RECT border = context_menu_rect;
			border.bottom = border.top + 1; FillRect(mem_dc, &border, menu_edge);
			border = context_menu_rect; border.top = border.bottom - 1; FillRect(mem_dc, &border, menu_edge);
			border = context_menu_rect; border.right = border.left + 1; FillRect(mem_dc, &border, menu_edge);
			border = context_menu_rect; border.left = border.right - 1; FillRect(mem_dc, &border, menu_edge);
		}
		for (int i = 0; i < context_menu_count(mode); i++) {
			int y = context_menu_rect.top + 4 + i * CONSOLE_CONTEXT_ITEM_H;
			RECT item_rect = {context_menu_rect.left + 2, y, context_menu_rect.right - 2, y + CONSOLE_CONTEXT_ITEM_H};
			int enabled = context_menu_item_enabled(mode, i);
			if (i == selected && menu_selection) FillRect(mem_dc, &item_rect, menu_selection);
			SetTextColor(mem_dc, enabled ? (i == selected ? RGB(255, 245, 228) : RGB(220, 215, 203)) : RGB(112, 108, 100));
			{
				const char *label = context_menu_label(mode, i);
				const char *shortcut = context_menu_shortcut(mode, i);
				SIZE shortcut_size = {0, 0};
				console_ui_text_out(mem_dc, context_menu_rect.left + 12, y + 3, label);
				if (shortcut[0]) {
					console_text_width(mem_dc, shortcut, (int)strlen(shortcut), &shortcut_size);
					SetTextColor(mem_dc, enabled ? RGB(145, 137, 122) : RGB(91, 87, 80));
					console_text_out(mem_dc, context_menu_rect.right - 12 - shortcut_size.cx, y + 3,
						shortcut, (int)strlen(shortcut));
				}
			}
		}
	}

	/* Draw button tooltips last: output rows and detached popups must not bleed
	   through the tooltip surface or cover its label. The final alpha pass below
	   makes this DIB region fully opaque over the game image as well. */
	if (button_tooltip_text[0] && button_tooltip_rect.right > button_tooltip_rect.left &&
		button_tooltip_rect.bottom > button_tooltip_rect.top) {
		HBRUSH tip_back = panel_brush(PANEL_BRUSH_TOOLTIP, RGB(16, 19, 21));
		HBRUSH tip_edge = panel_brush(PANEL_BRUSH_EDGE, RGB(112, 105, 92));
		RECT r = button_tooltip_rect;
		if (tip_back) FillRect(mem_dc, &button_tooltip_rect, tip_back);
		if (tip_edge) {
			r = button_tooltip_rect; r.bottom = r.top + 1; FillRect(mem_dc, &r, tip_edge);
			r = button_tooltip_rect; r.top = r.bottom - 1; FillRect(mem_dc, &r, tip_edge);
			r = button_tooltip_rect; r.right = r.left + 1; FillRect(mem_dc, &r, tip_edge);
			r = button_tooltip_rect; r.left = r.right - 1; FillRect(mem_dc, &r, tip_edge);
		}
		{
			int old_bk_mode = SetBkMode(mem_dc, OPAQUE);
			COLORREF old_bk_color = SetBkColor(mem_dc, RGB(16, 19, 21));
			SetTextColor(mem_dc, RGB(239, 232, 218));
			console_ui_text_out_centered(mem_dc, button_tooltip_rect.left + 8, button_tooltip_rect.top,
				button_tooltip_rect.bottom - button_tooltip_rect.top, button_tooltip_text);
			SetBkColor(mem_dc, old_bk_color);
			SetBkMode(mem_dc, old_bk_mode);
		}
	}

	/* The native system cursor remains visible; no second D3D cursor is drawn. */

	GdiFlush();
	/* Alpha is applied only to the actual window and detached dropdown. Pixels in
	   the gap and around the floating window remain transparent over the game. */
	for (int y = window_rect.top; y < window_rect.bottom; y++) {
		uint32_t *row = dib_bits + (size_t)y * (size_t)w;
		for (int x = window_rect.left; x < window_rect.right; x++)
			row[x] = (row[x] & 0x00ffffffu) | 0xE8000000u;
	}
	/* The short shadow below the floating window is intentionally translucent too. */
	for (int y = window_rect.bottom; y < window_rect.bottom + 4 && y < bitmap_h; y++) {
		uint32_t *row = dib_bits + (size_t)y * (size_t)w;
		for (int x = window_rect.left + 3; x < window_rect.right && x < w; x++)
			if (row[x] & 0x00ffffffu) row[x] = (row[x] & 0x00ffffffu) | 0x78000000u;
	}
	if (output_toast_rect.right > output_toast_rect.left && output_toast_rect.bottom > output_toast_rect.top) {
		for (int y = output_toast_rect.top; y < output_toast_rect.bottom && y < bitmap_h; y++) {
			uint32_t *row = dib_bits + (size_t)y * (size_t)w;
			for (int x = output_toast_rect.left; x < output_toast_rect.right && x < w; x++)
				row[x] = (row[x] & 0x00ffffffu) | 0xC0000000u;
		}
	}
	if (completion_rect.right > completion_rect.left && completion_rect.bottom > completion_rect.top) {
		for (int y = completion_rect.top; y < completion_rect.bottom && y < bitmap_h; y++) {
			uint32_t *row = dib_bits + (size_t)y * (size_t)w;
			for (int x = completion_rect.left; x < completion_rect.right && x < w; x++)
				row[x] = (row[x] & 0x00ffffffu) | 0xE8000000u;
		}
	}
	if (context_menu_open && context_menu_rect.right > context_menu_rect.left && context_menu_rect.bottom > context_menu_rect.top) {
		for (int y = context_menu_rect.top; y < context_menu_rect.bottom && y < bitmap_h; y++) {
			uint32_t *row = dib_bits + (size_t)y * (size_t)w;
			for (int x = context_menu_rect.left; x < context_menu_rect.right && x < w; x++)
				row[x] = (row[x] & 0x00ffffffu) | 0xE8000000u;
		}
	}
	/* Apply tooltip opacity last so even overlapping dropdowns/toasts cannot
	   dilute the tooltip's solid background alpha. */
	if (button_tooltip_rect.right > button_tooltip_rect.left && button_tooltip_rect.bottom > button_tooltip_rect.top) {
		for (int y = button_tooltip_rect.top; y < button_tooltip_rect.bottom && y < bitmap_h; y++) {
			uint32_t *row = dib_bits + (size_t)y * (size_t)w;
			for (int x = button_tooltip_rect.left; x < button_tooltip_rect.right && x < w; x++)
				row[x] = (row[x] & 0x00ffffffu) | 0xFF000000u;
		}
	}
	dib_panel_h = panel_h;
	LeaveCriticalSection(&lock);
	return 1;
}

typedef struct {
	float x, y, z, rhw, u, v;
} quad_vertex;

static int console_prepare_cursor_texture(IDirect3DDevice9 *dev)
{
	D3DLOCKED_RECT locked;
	HRESULT hr;
	LONG kind = InterlockedCompareExchange(&console_cursor_kind, 0, 0);
	if (!dev) return 0;
	if (console_cursor_bitmap_kind != kind) {
		HCURSOR cursor = LoadCursorA(NULL, console_cursor_resource(kind));
		if (cursor) {
			console_load_cursor_bitmap(cursor);
			console_cursor_bitmap_kind = kind;
		}
		if (console_cursor_texture) {
			IDirect3DTexture9_Release(console_cursor_texture);
			console_cursor_texture = NULL;
			console_cursor_texture_device = NULL;
		}
		console_cursor_texture_kind = -1;
	}
	if (!console_cursor_pixels || console_cursor_width <= 0 || console_cursor_height <= 0) return 0;
	if (console_cursor_texture && (console_cursor_texture_device != dev || console_cursor_texture_kind != kind)) {
		IDirect3DTexture9_Release(console_cursor_texture);
		console_cursor_texture = NULL;
		console_cursor_texture_device = NULL;
		console_cursor_texture_kind = -1;
	}
	if (!console_cursor_texture) {
		hr = IDirect3DDevice9_CreateTexture(dev, (UINT)console_cursor_width, (UINT)console_cursor_height, 1, 0,
			D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &console_cursor_texture, NULL);
		if (FAILED(hr) || !console_cursor_texture) {
			console_cursor_texture = NULL;
			return 0;
		}
		console_cursor_texture_device = dev;
		hr = IDirect3DTexture9_LockRect(console_cursor_texture, 0, &locked, NULL, 0);
		if (FAILED(hr)) {
			IDirect3DTexture9_Release(console_cursor_texture);
			console_cursor_texture = NULL;
			console_cursor_texture_device = NULL;
			return 0;
		}
		for (int y = 0; y < console_cursor_height; y++)
			memcpy((uint8_t *)locked.pBits + (size_t)y * (size_t)locked.Pitch,
				console_cursor_pixels + (size_t)y * (size_t)console_cursor_width,
				(size_t)console_cursor_width * sizeof(*console_cursor_pixels));
		hr = IDirect3DTexture9_UnlockRect(console_cursor_texture, 0);
		if (FAILED(hr)) {
			IDirect3DTexture9_Release(console_cursor_texture);
			console_cursor_texture = NULL;
			console_cursor_texture_device = NULL;
			return 0;
		}
		console_cursor_texture_kind = kind;
	}
	return 1;
}


static HRESULT console_upload_panel_texture(IDirect3DTexture9 *target, const uint32_t *pixels, int width, int height)
{
	D3DLOCKED_RECT locked;
	HRESULT hr;
	if (!target || !pixels || width <= 0 || height <= 0)
		return E_INVALIDARG;
	hr = IDirect3DTexture9_LockRect(target, 0, &locked, NULL, 0);
	if (FAILED(hr))
		return hr;
	for (int y = 0; y < height; y++)
		memcpy((uint8_t *)locked.pBits + (size_t)y * (size_t)locked.Pitch,
			pixels + (size_t)y * (size_t)width, (size_t)width * sizeof(*pixels));
	return IDirect3DTexture9_UnlockRect(target, 0);
}

static void draw_panel(IDirect3DDevice9 *dev, int trace)
{
	IDirect3DSurface9 *bb = NULL;
	D3DSURFACE_DESC desc;
	int w, panel_h, bitmap_h;
	int repaint, draw_cursor;
	UINT dpi;
	if (trace)
		log_printf("console: panel draw begin");
	if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) {
		log_printf("console: no back buffer");
		return;
	}
	IDirect3DSurface9_GetDesc(bb, &desc);
	IDirect3DSurface9_Release(bb);
	if (trace)
		log_printf("console: panel back buffer %ux%u", desc.Width, desc.Height);
	dpi = console_window_dpi(game_window);
	{
		LONG previous_dpi = InterlockedExchange(&console_current_dpi, (LONG)dpi);
		if (previous_dpi != (LONG)dpi) InterlockedExchange(&dirty, 1);
	}
	w = console_dpi_logical((int)desc.Width, dpi);
	panel_h = console_dpi_logical(panel_height((int)desc.Height), dpi);
	InterlockedExchange(&console_backbuffer_width, w);
	InterlockedExchange(&console_backbuffer_height, console_dpi_logical((int)desc.Height, dpi));
	if (trace)
		log_printf("console: drawing %dx%d logical UI at %u DPI over %ux%u backbuffer", w, panel_h, dpi, desc.Width, desc.Height);
	if (w < 64 || panel_h < 64)
		return;
	if (!mem_dc) {
		if (trace) log_printf("console: creating panel memory DC");
		mem_dc = CreateCompatibleDC(NULL);
		if (!mem_dc) {
			log_printf("console: cannot create the panel memory DC");
			return;
		}
	}
	if (is_open) {
		static DWORD clock_tick;
		DWORD now = GetTickCount();
		DWORD repaint_interval = console_watch_count_read() > 0 ? 250u : 1000u;
		if ((DWORD)(now - clock_tick) >= repaint_interval) {
			clock_tick = now;
			InterlockedExchange(&dirty, 1);
		}
		if (InterlockedCompareExchange(&console_window_active, 0, 0) && ((now / 500) & 1))
			InterlockedExchange(&dirty, 1);
	}
	repaint = InterlockedExchange(&dirty, 0) || !dib_bits || dib_w != w || dib_panel_h != panel_h || dib_h != panel_h;
	if (repaint) {
		if (!paint(w, panel_h)) {
			InterlockedExchange(&dirty, 1);
			/* Input/autocomplete updates can briefly own the shared lock. Keep drawing
			   the last complete panel instead of dropping the entire console frame. */
			if (!dib_bits || dib_w != w || dib_h != panel_h)
				return;
			repaint = 0;
		}
		if (!dib_bits || dib_w != w || dib_h != panel_h) {
			InterlockedExchange(&dirty, 1);
			log_printf("console: panel bitmap is not ready");
			return;
		}
	}
	bitmap_h = dib_h;
	if (bitmap_h > (int)desc.Height)
		bitmap_h = (int)desc.Height;
	if (bitmap_h < panel_h)
		return;
	if (tex_device != dev) {
		if (tex) IDirect3DTexture9_Release(tex);
		tex = NULL;
		tex_device = dev;
		tex_w = tex_h = 0;
	}
	if (!tex || tex_w != w || tex_h != bitmap_h) {
		IDirect3DTexture9 *replacement = NULL;
		HRESULT create_result;
		if (trace)
			log_printf("console: creating overlay texture %dx%d", w, bitmap_h);
		create_result = IDirect3DDevice9_CreateTexture(dev, (UINT)w, (UINT)bitmap_h, 1, 0,
			D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &replacement, NULL);
		if (SUCCEEDED(create_result) && replacement) {
			HRESULT upload_result = console_upload_panel_texture(replacement, dib_bits, w, bitmap_h);
			if (SUCCEEDED(upload_result)) {
				/* Swap only after the new surface is fully allocated and uploaded. */
				IDirect3DTexture9 *previous = tex;
				tex = replacement;
				tex_w = w;
				tex_h = bitmap_h;
				replacement = NULL;
				if (previous) IDirect3DTexture9_Release(previous);
				repaint = 0;
				if (trace) log_printf("console: replacement overlay texture committed");
			} else {
				log_printf("console: replacement overlay texture upload failed (%08lx)", (unsigned long)upload_result);
				InterlockedExchange(&dirty, 1);
			}
			if (replacement) IDirect3DTexture9_Release(replacement);
		} else {
			static LONG logged_create;
			if (replacement) IDirect3DTexture9_Release(replacement);
			if (InterlockedCompareExchange(&logged_create, 1, 0) == 0)
				log_printf("console: cannot create replacement overlay texture (%dx%d, %08lx)",
					w, bitmap_h, (unsigned long)create_result);
			InterlockedExchange(&dirty, 1);
		}
		if (tex && tex_device == dev) {
			/* If a tall suggestion list cannot be allocated, keep drawing the last
			   complete frame. The console stays visible; only the new popup is clipped. */
			w = tex_w;
			bitmap_h = tex_h;
			repaint = 0;
		} else {
			return;
		}
	} else if (repaint) {
		HRESULT upload_result = console_upload_panel_texture(tex, dib_bits, w, bitmap_h);
		if (FAILED(upload_result)) {
			static LONG logged_upload;
			if (InterlockedCompareExchange(&logged_upload, 1, 0) == 0)
				log_printf("console: overlay texture update failed (%08lx); retaining the last frame", (unsigned long)upload_result);
			InterlockedExchange(&dirty, 1);
		}
	}
	if (panel_state_device != dev) {
		if (panel_state_block) IDirect3DStateBlock9_Release(panel_state_block);
		panel_state_block = NULL;
		panel_state_device = dev;
	}
	if (!panel_state_block) {
		if (trace) log_printf("console: creating cached D3D state block");
		if (FAILED(IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &panel_state_block)) || !panel_state_block) {
			static LONG logged_create;
			panel_state_block = NULL;
			if (InterlockedCompareExchange(&logged_create, 1, 0) == 0)
				log_printf("console: cannot create D3D state block; will retry");
			return;
		}
	}
	if (FAILED(IDirect3DStateBlock9_Capture(panel_state_block))) {
		static LONG logged_capture;
		IDirect3DStateBlock9_Release(panel_state_block);
		panel_state_block = NULL;
		if (InterlockedCompareExchange(&logged_capture, 1, 0) == 0)
			log_printf("console: cannot capture cached D3D state block; will recreate it");
		return;
	}
	draw_cursor = 0;
	if (is_open && console_window_active && game_window &&
		InterlockedCompareExchange(&console_native_cursor_hidden, 0, 0) &&
		console_prepare_cursor_texture(dev))
		draw_cursor = 1;
	if (trace) log_printf("console: BeginScene");
	HRESULT begin_result = IDirect3DDevice9_BeginScene(dev);
	if (FAILED(begin_result)) {
		static LONG logged_begin;
		if (InterlockedCompareExchange(&logged_begin, 1, 0) == 0)
			log_printf("console: BeginScene failed (%08lx)", (unsigned long)begin_result);
		return;
	}
	if (trace) log_printf("console: BeginScene succeeded");
	D3DVIEWPORT9 vp = {0, 0, desc.Width, desc.Height, 0.0f, 1.0f};
	IDirect3DDevice9_SetViewport(dev, &vp);
	IDirect3DDevice9_SetPixelShader(dev, NULL);
	IDirect3DDevice9_SetVertexShader(dev, NULL);
	IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
	IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex);
	IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
	IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
	IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
	IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
	IDirect3DDevice9_SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
	IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
	IDirect3DDevice9_SetRenderState(dev, D3DRS_FILLMODE, D3DFILL_SOLID);
	IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
	IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
	IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xf);
	IDirect3DDevice9_SetRenderState(dev, D3DRS_SRGBWRITEENABLE, FALSE);
	IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
	IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	float x1 = (float)desc.Width - 0.5f, y1 = (float)desc.Height - 0.5f;
	quad_vertex q[4] = {
		{-0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f}, {x1, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f},
		{-0.5f, y1, 0.0f, 1.0f, 0.0f, 1.0f}, {x1, y1, 0.0f, 1.0f, 1.0f, 1.0f}
	};
	HRESULT dr = IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(quad_vertex));
	if (trace) log_printf("console: overlay quad drawn, result %lx", (unsigned long)dr);
	if (draw_cursor) {
		RECT client;
		int mx, my, cursor_width = console_cursor_width, cursor_height = console_cursor_height;
		int hotspot_x = console_cursor_hotspot_x, hotspot_y = console_cursor_hotspot_y;
		console_virtual_mouse_position(game_window, &mx, &my);
		if (GetClientRect(game_window, &client) && client.right > 0 && client.bottom > 0) {
			/* Scale the whole cursor, not just its anchor: client and D3D backbuffer
			   sizes can differ under DPI scaling, fullscreen scaling or render scaling. */
			mx = (int)((long long)mx * desc.Width / client.right);
			my = (int)((long long)my * desc.Height / client.bottom);
			cursor_width = (int)(((long long)console_cursor_width * desc.Width + client.right / 2) / client.right);
			cursor_height = (int)(((long long)console_cursor_height * desc.Height + client.bottom / 2) / client.bottom);
			hotspot_x = (int)(((long long)console_cursor_hotspot_x * desc.Width + client.right / 2) / client.right);
			hotspot_y = (int)(((long long)console_cursor_hotspot_y * desc.Height + client.bottom / 2) / client.bottom);
			if (cursor_width < 1) cursor_width = 1;
			if (cursor_height < 1) cursor_height = 1;
		} else {
			console_mouse_to_backbuffer(game_window, &mx, &my);
		}
		if (mx >= 0 && my >= 0 && mx < (int)desc.Width && my < (int)desc.Height) {
			float left = (float)(mx - hotspot_x) - 0.5f;
			float top = (float)(my - hotspot_y) - 0.5f;
			float right = left + (float)cursor_width;
			float bottom = top + (float)cursor_height;
			quad_vertex cursor_quad[4] = {
				{left, top, 0.0f, 1.0f, 0.0f, 0.0f}, {right, top, 0.0f, 1.0f, 1.0f, 0.0f},
				{left, bottom, 0.0f, 1.0f, 0.0f, 1.0f}, {right, bottom, 0.0f, 1.0f, 1.0f, 1.0f}
			};
			IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)console_cursor_texture);
			HRESULT cursor_result = IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, cursor_quad, sizeof(quad_vertex));
			if (trace) log_printf("console: cursor sprite drawn at %d,%d (%08lx)", mx, my, (unsigned long)cursor_result);
		}
	}
	HRESULT end_result = IDirect3DDevice9_EndScene(dev);
	if (trace) log_printf("console: EndScene result %08lx", (unsigned long)end_result);
	HRESULT apply_result = IDirect3DStateBlock9_Apply(panel_state_block);
	if (FAILED(apply_result)) {
		static LONG logged_apply;
		if (InterlockedCompareExchange(&logged_apply, 1, 0) == 0)
			log_printf("console: restoring D3D state failed (%08lx); recreating state block", (unsigned long)apply_result);
		IDirect3DStateBlock9_Release(panel_state_block);
		panel_state_block = NULL;
	}
	if (trace && SUCCEEDED(apply_result)) log_printf("console: panel D3D state restored");
	if (trace) log_printf("console: panel draw complete");
}

int console_native_frame(unsigned back_width, unsigned back_height, void (*show)(const uint32_t *bits, int w, int h, int repainted))
{
	if (!is_open || !show)
		return 0;
	if (InterlockedCompareExchange(&panel_draw_busy, 1, 0) != 0)
		return 0;
	int drawn = 0;
	UINT dpi = console_window_dpi(game_window);
	{
		LONG previous_dpi = InterlockedExchange(&console_current_dpi, (LONG)dpi);
		if (previous_dpi != (LONG)dpi)
			InterlockedExchange(&dirty, 1);
	}
	const int w = console_dpi_logical((int)back_width, dpi);
	const int panel_h = console_dpi_logical(panel_height((int)back_height), dpi);
	InterlockedExchange(&console_backbuffer_width, w);
	InterlockedExchange(&console_backbuffer_height, console_dpi_logical((int)back_height, dpi));
	if (w < 64 || panel_h < 64)
		goto out;
	if (!mem_dc) {
		mem_dc = CreateCompatibleDC(NULL);
		if (!mem_dc) {
			log_printf("console: cannot create the panel memory DC");
			goto out;
		}
	}
	{
		static DWORD clock_tick;
		DWORD now = GetTickCount();
		DWORD repaint_interval = console_watch_count_read() > 0 ? 250u : 1000u;
		if ((DWORD)(now - clock_tick) >= repaint_interval) {
			clock_tick = now;
			InterlockedExchange(&dirty, 1);
		}
		if (InterlockedCompareExchange(&console_window_active, 0, 0) && ((now / 500) & 1))
			InterlockedExchange(&dirty, 1);
	}
	int repaint = InterlockedExchange(&dirty, 0) || !dib_bits || dib_w != w || dib_panel_h != panel_h || dib_h != panel_h;
	if (repaint) {
		if (!paint(w, panel_h)) {
			InterlockedExchange(&dirty, 1);
			if (!dib_bits || dib_w != w || dib_h != panel_h)
				goto out;
			repaint = 0;
		}
		if (!dib_bits || dib_w != w || dib_h != panel_h) {
			InterlockedExchange(&dirty, 1);
			goto out;
		}
	}
	{
		static int dib_known_w, dib_known_h;
		static const uint32_t *dib_known;
		if (dib_known != dib_bits || dib_known_w != w || dib_known_h != dib_h) { // a new bitmap: the caller uploads it
			dib_known = dib_bits;
			dib_known_w = w;
			dib_known_h = dib_h;
			repaint = 1;
		}
	}
	show(dib_bits, w, dib_h, repaint);
	drawn = 1;
out:
	InterlockedExchange(&panel_draw_busy, 0);
	return drawn;
}

// Called by native.c's Present hooks after native frame composition: once per presented frame, on the render thread.
// Which of the three hooks the game really goes through is the first one that calls.
void console_present(int via, void *devp)
{
	static int draw_via = -1;
	static LONG traced;
	if (draw_via < 0)
		draw_via = via;
	if (via != draw_via || !is_open)
		return;
	if (InterlockedCompareExchange(&panel_draw_busy, 1, 0) != 0)
		return;
	IDirect3DDevice9 *dev = (IDirect3DDevice9 *)devp;
	int trace = InterlockedCompareExchange(&traced, 1, 0) == 0;
	if (trace)
		log_printf("console: first panel Present via %d on thread %lu", via, (unsigned long)GetCurrentThreadId());
	draw_panel(dev, trace);
	if (trace)
		log_printf("console: panel Present returned");
	if (dump_name[0]) { // QA: a frame with the panel on it is saved under this name
		char name[64];
		lstrcpynA(name, dump_name, sizeof(name));
		dump_name[0] = 0;
		shot_save_now(dev, name);
	}
	InterlockedExchange(&panel_draw_busy, 0);
}

// ---------------------------------------------------------------- the setup

static BOOL CALLBACK find_window_cb(HWND h, LPARAM out)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(h, &pid);
	if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER))
		return TRUE;
	RECT r;
	GetClientRect(h, &r);
	HWND *best = (HWND *)out;
	RECT b;
	if (r.right < 320 || r.bottom < 200)
		return TRUE;
	if (*best) {
		GetClientRect(*best, &b);
		if ((long)r.right * r.bottom <= (long)b.right * b.bottom)
			return TRUE;
	}
	*best = h;
	return TRUE;
}

static void *game_device(void)
{
#ifdef _WIN64
	uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
	return *(void **)(base + XR(0x410fd20)); // the 64-bit game keeps its device here (native.c RVA_D3D_DEVICE)
#else
	return d3dcap_device();
#endif
}

// sv_cheats is switched on at the start only when settings.ini has [console] sv_cheats_default=1: it is not a default of the game, and cheats change what a campaign counts (review of the
// console pull request, 2026-11-06: it was on for everybody).
static int console_sv_cheats_default_wanted(void)
{
	static LONG decided = -1;
	LONG v = InterlockedCompareExchange(&decided, -1, -1);
	if (v == -1) {
		v = console_settings_path_init() && GetPrivateProfileIntA("console", "sv_cheats_default", 0, console_settings_file) == 1;
		InterlockedExchange(&decided, v);
	}
	return v != 0;
}

// sv_cheats is a dvar the player may change (`sv_cheats 1`, `set sv_cheats 1`, the dvar list): the game registers it INIT/ROM (read-only after the start) and `seta` adds ARCHIVE, which would write
// the value into the config and bring cheats back at the next start. The flags are taken off whenever the dvar is found with them (the game can register it again), whatever
// [console] sv_cheats_default says; the value itself is not touched here.
static void console_sv_cheats_unprotect(void)
{
	const LONG mask = (LONG)(CONSOLE_DVAR_INIT_FLAG | DVAR_ROM_FLAG | DVAR_ARCHIVE_FLAG);
	dvar_t *d = game_find_dvar("sv_cheats");
	if (!d || d->type != DVAR_TYPE_BOOL || !(d->flags & (uint32_t)mask))
		return;
	LONG old_flags = InterlockedAnd((volatile LONG *)&d->flags, ~mask);
	log_printf("console: sv_cheats is a changeable, unsaved dvar (flags 0x%08x -> 0x%08x)", (unsigned)old_flags, (unsigned)d->flags);
}

static void queue_default_sv_cheats(void)
{
	dvar_t *d;
	console_sv_cheats_unprotect();
	if (!console_sv_cheats_default_wanted())
		return;
	LONG state = InterlockedCompareExchange(&auto_cheats_queued, 0, 0);
	if (state == AUTO_CHEATS_USER_OVERRIDE || state == AUTO_CHEATS_PENDING)
		return;
	d = game_find_dvar("sv_cheats");
	if (!d || d->type != DVAR_TYPE_BOOL) {
		if (state == AUTO_CHEATS_APPLIED)
			InterlockedCompareExchange(&auto_cheats_queued, 0, AUTO_CHEATS_APPLIED);
		return; // setup_thread will check again once the dvar registry is ready
	}
	if (d->current.enabled && d->latched.enabled && d->reset.enabled) {
		InterlockedExchange(&auto_cheats_queued, AUTO_CHEATS_APPLIED);
		return;
	}
	/* The engine can recreate/reset this dvar after our first attempt. Re-queue only
	   after read-back shows that it no longer holds 1; explicit user changes still win. */
	if (state == AUTO_CHEATS_APPLIED &&
	    InterlockedCompareExchange(&auto_cheats_queued, 0, AUTO_CHEATS_APPLIED) != AUTO_CHEATS_APPLIED)
		return;
	if (InterlockedCompareExchange(&auto_cheats_queued, AUTO_CHEATS_PENDING, 0) != 0)
		return;
	game_cbuf_addtext("mw2coop_sv_cheats_default\n");
	log_printf("console: queued silent startup sv_cheats attempt");
}

static void attach_raw_input_relay(HWND target)
{
	DWORD pid = 0;
	WNDPROC previous;
	if (!target || target == game_window || !IsWindow(target))
		return; /* console_proc already receives WM_INPUT on the game window */
	GetWindowThreadProcessId(target, &pid);
	if (pid != GetCurrentProcessId())
		return;
	if (raw_input_relay_window == target &&
		(WNDPROC)GetWindowLongPtrA(target, GWLP_WNDPROC) == raw_input_relay_proc)
		return;
	if (raw_input_relay_window && raw_input_relay_window != target) {
		HWND old_window = raw_input_relay_window;
		if (!IsWindow(old_window)) {
			raw_input_relay_window = NULL;
			InterlockedExchangePointer((PVOID volatile *)&raw_input_relay_original_proc, NULL);
		} else if ((WNDPROC)GetWindowLongPtrA(old_window, GWLP_WNDPROC) == raw_input_relay_proc) {
			WNDPROC old_proc = (WNDPROC)InterlockedCompareExchangePointer(
				(PVOID volatile *)&raw_input_relay_original_proc, NULL, NULL);
			if (!old_proc || !SetWindowLongPtrA(old_window, GWLP_WNDPROC, (LONG_PTR)old_proc))
				return;
			raw_input_relay_window = NULL;
			InterlockedExchangePointer((PVOID volatile *)&raw_input_relay_original_proc, NULL);
		} else {
			return; /* Another subclass is chained above us; do not break its procedure chain. */
		}
	}
	previous = (WNDPROC)GetWindowLongPtrA(target, GWLP_WNDPROC);
	if (!previous || previous == raw_input_relay_proc) {
		log_printf("console: auxiliary raw mouse window %p has no usable window procedure", (void *)target);
		return;
	}
	InterlockedExchangePointer((PVOID volatile *)&raw_input_relay_original_proc, previous);
	SetLastError(0);
	if (!SetWindowLongPtrA(target, GWLP_WNDPROC, (LONG_PTR)raw_input_relay_proc) && GetLastError()) {
		InterlockedExchangePointer((PVOID volatile *)&raw_input_relay_original_proc, NULL);
		log_printf("console: cannot subclass auxiliary raw mouse window %p (error %lu)", (void *)target, (unsigned long)GetLastError());
		return;
	}
	raw_input_relay_window = target;
	if ((WNDPROC)GetWindowLongPtrA(target, GWLP_WNDPROC) != raw_input_relay_proc) {
		/* Another same-process subclass may have chained over us immediately.
		   Keep the saved original alive in case that procedure forwards to us. */
		log_printf("console: auxiliary raw mouse subclass changed during installation for %p", (void *)target);
		return;
	}
	log_printf("console: raw mouse input relayed from auxiliary game window %p", (void *)target);
}

static void register_console_raw_mouse(HWND h)
{
	UINT count = 0;
	RAWINPUTDEVICE *devices = NULL;
	if (!h)
		return;
	if (GetRegisteredRawInputDevices(NULL, &count, sizeof(RAWINPUTDEVICE)) == (UINT)-1) {
		log_printf("console: cannot inspect raw mouse registration (error %lu)", (unsigned long)GetLastError());
		return;
	}
	if (count) {
		devices = (RAWINPUTDEVICE *)malloc((size_t)count * sizeof(*devices));
		if (!devices)
			return;
		if (GetRegisteredRawInputDevices(devices, &count, sizeof(RAWINPUTDEVICE)) == (UINT)-1) {
			log_printf("console: cannot read raw mouse registrations (error %lu)", (unsigned long)GetLastError());
			free(devices);
			return;
		}
		for (UINT i = 0; i < count; i++) {
			HWND target;
			int changed;
			DWORD pid = 0;
			if (devices[i].usUsagePage != 0x01 || devices[i].usUsage != 0x02)
				continue;
			target = devices[i].hwndTarget;
			changed = !raw_mouse_registration_seen || target != raw_mouse_target;

			/* Only retarget registrations created by this console or registrations
			   originally attached to the game's own HWND. Preserve all existing flags. */
			if ((raw_mouse_registration_owned || raw_mouse_target_tracks_game_window) &&
				target == raw_mouse_target && target && target != h) {
				RAWINPUTDEVICE updated = devices[i];
				updated.hwndTarget = h;
				if (RegisterRawInputDevices(&updated, 1, sizeof(updated))) {
					raw_mouse_target = h;
					raw_mouse_target_tracks_game_window = 1;
					raw_mouse_registration_seen = 1;
					log_printf("console: retargeted raw mouse registration to the recreated game window");
				} else {
					log_printf("console: could not retarget raw mouse registration (error %lu)", (unsigned long)GetLastError());
				}
			} else if (!target || target == h) {
				if (changed)
					log_printf("console: using existing raw mouse registration (flags 0x%08lx, target %p)", (unsigned long)devices[i].dwFlags, (void *)target);
				if (changed)
					raw_mouse_registration_owned = 0;
				raw_mouse_target = target;
				raw_mouse_target_tracks_game_window = target == h;
				raw_mouse_registration_seen = 1;
			} else {
				if (!IsWindow(target)) {
					RAWINPUTDEVICE updated = devices[i];
					updated.hwndTarget = h;
					if (RegisterRawInputDevices(&updated, 1, sizeof(updated))) {
						raw_mouse_target = h;
						raw_mouse_registration_owned = 0;
						raw_mouse_target_tracks_game_window = 1;
						raw_mouse_registration_seen = 1;
						log_printf("console: retargeted stale raw mouse registration to the current game window");
					} else {
						log_printf("console: could not replace stale raw mouse target (error %lu)", (unsigned long)GetLastError());
					}
					free(devices);
					return;
				}
				GetWindowThreadProcessId(target, &pid);
				if (changed) {
					raw_mouse_target = target;
					raw_mouse_registration_owned = 0;
					raw_mouse_target_tracks_game_window = 0;
					raw_mouse_registration_seen = 1;
					if (pid == GetCurrentProcessId()) {
						attach_raw_input_relay(target);
						log_printf("console: preserving existing raw mouse registration on %p (flags 0x%08lx)", (void *)target, (unsigned long)devices[i].dwFlags);
					} else {
						log_printf("console: raw mouse registration targets %p outside this process; legacy mouse fallback remains enabled", (void *)target);
					}
				} else if (pid == GetCurrentProcessId()) {
					attach_raw_input_relay(target);
				}
			}
			free(devices);
			return;
		}
		free(devices);
	}
	{
		RAWINPUTDEVICE mouse = {0x01, 0x02, 0, h};
		if (RegisterRawInputDevices(&mouse, 1, sizeof(mouse))) {
			raw_mouse_target = h;
			raw_mouse_registration_owned = 1;
			raw_mouse_target_tracks_game_window = 1;
			raw_mouse_registration_seen = 1;
			log_printf("console: registered foreground raw mouse input for the overlay");
		} else {
			log_printf("console: raw mouse registration failed (error %lu)", (unsigned long)GetLastError());
		}
	}
}


static DWORD WINAPI setup_thread(LPVOID unused)
{
	(void)unused;
	IDirect3DDevice9 *dev = NULL;
	int present_bootstrap_armed = 0;
	DWORD raw_mouse_check_tick = 0;
	for (int i = 0; i < 6000 && !dev; i++) {
		Sleep(100);
		queue_default_sv_cheats();
		dev = (IDirect3DDevice9 *)game_device();
		if (dev) {
			HWND first = NULL;
			EnumWindows(find_window_cb, (LPARAM)&first);
			if (!first)
				dev = NULL;
		}
	}
	if (!dev) {
		log_printf("console: the game's device or window never appeared");
		return 0;
	}
	console_load_layout();
	console_print_text("CON_BANNER");
	// the game may make its window again (a video restart): the watcher puts the console's procedure on whichever window is the game's
	for (;;) {
		HWND now = NULL;
		native_present_poll();
		if (!present_bootstrap_armed && InterlockedCompareExchange(&present_bootstrap_request, 0, 0)) {
			present_bootstrap_armed = native_present_hooks();
			if (present_bootstrap_armed) {
				InterlockedExchange(&present_bootstrap_request, 0);
				log_printf("console: D3D hook request serviced by setup thread (render thread will complete installation)");
			}
		}
		queue_default_sv_cheats();
		if (is_open && game_window) {
			LONG next_mode = console_mouse_is_clipped(game_window);
			LONG old_mode = InterlockedExchange(&virtual_mouse_relative_mode, next_mode);
			if (old_mode != next_mode) {
				InterlockedExchange(&virtual_mouse_raw_seen, 0);
				if (!next_mode) (void)console_sync_virtual_mouse_from_os(game_window);
				console_set_native_cursor_hidden(game_window, next_mode);
				InterlockedExchange(&dirty, 1);
			}
		}
		if (!InterlockedCompareExchange(&raw_mouse_first_packet_logged, 0, 0) &&
			InterlockedCompareExchange(&raw_mouse_packets, 0, 0) > 0) {
			InterlockedExchange(&raw_mouse_first_packet_logged, 1);
			log_printf("console: raw mouse packets are reaching the overlay");
		}
		EnumWindows(find_window_cb, (LPARAM)&now);
		if (now && (now != game_window || (WNDPROC)GetWindowLongPtrA(now, GWLP_WNDPROC) != (WNDPROC)console_proc)) {
			if ((WNDPROC)GetWindowLongPtrA(now, GWLP_WNDPROC) != (WNDPROC)console_proc) {
				game_proc = (WNDPROC)SetWindowLongPtrA(now, GWLP_WNDPROC, (LONG_PTR)console_proc);
				log_printf("console: ready (device %p, window %p was %p, ` opens it)", (void *)dev, (void *)now, (void *)game_proc);
			}
			if (now != game_window && is_open)
				set_open(0);
			game_window = now;
		}
		if (now && (DWORD)(GetTickCount() - raw_mouse_check_tick) >= 1000) {
			raw_mouse_check_tick = GetTickCount();
			register_console_raw_mouse(now);
		}
		Sleep(250);
	}
	return 0;
}

void console_qa_open(int open)
{
	set_open(open);
}

static void condump_f(void)
{
	lstrcpynA(dump_name, game_cmd_argv(1), sizeof(dump_name));
}
static cmd_function_t condump_cmd = {NULL, "mw2coop_condump", NULL, NULL, condump_f CMD_NODE_FLAG};

typedef struct {
	BITMAPINFOHEADER header;
	RGBQUAD colors[2];
} console_mono_bitmap_info_t;

static int console_mask_bit(const uint8_t *bits, int stride, int height, int x, int y)
{
	int row;
	if (!bits || x < 0 || y < 0 || y >= height)
		return 0;
	row = height - 1 - y; /* GetDIBits with positive biHeight returns bottom-up rows. */
	return (bits[(size_t)row * (size_t)stride + (size_t)(x >> 3)] & (0x80u >> (x & 7))) != 0;
}

static void console_load_cursor_bitmap(HCURSOR cursor)
{
	ICONINFO info;
	BITMAP color_bm, mask_bm;
	HDC screen_dc = NULL;
	uint32_t *color_bits = NULL, *pixels = NULL;
	uint8_t *mask_bits = NULL;
	int width = 0, height = 0, mask_height = 0, mask_stride = 0;
	int color_ok = 0, alpha_present = 0, mask_ok = 0;
	if (!cursor || !GetIconInfo(cursor, &info))
		return;
	memset(&color_bm, 0, sizeof(color_bm));
	memset(&mask_bm, 0, sizeof(mask_bm));
	if (info.hbmColor && GetObjectA(info.hbmColor, (int)sizeof(color_bm), &color_bm) == (int)sizeof(color_bm)) {
		width = color_bm.bmWidth;
		height = color_bm.bmHeight;
	} else if (info.hbmMask && GetObjectA(info.hbmMask, (int)sizeof(mask_bm), &mask_bm) == (int)sizeof(mask_bm)) {
		width = mask_bm.bmWidth;
		height = mask_bm.bmHeight / 2;
		if (height <= 0) height = GetSystemMetrics(SM_CYCURSOR);
	}
	if (width <= 0) width = GetSystemMetrics(SM_CXCURSOR);
	if (height <= 0) height = GetSystemMetrics(SM_CYCURSOR);
	if (width > 128 || height > 128 || width <= 0 || height <= 0)
		goto done;
	console_cursor_hotspot_x = (int)info.xHotspot;
	console_cursor_hotspot_y = (int)info.yHotspot;
	console_cursor_width = width;
	console_cursor_height = height;
	screen_dc = GetDC(NULL);
	if (!screen_dc)
		goto done;
	if (info.hbmMask && GetObjectA(info.hbmMask, (int)sizeof(mask_bm), &mask_bm) == (int)sizeof(mask_bm)) {
		mask_height = mask_bm.bmHeight;
		if (mask_height < height) mask_height = height;
		mask_stride = ((width + 31) / 32) * 4;
		mask_bits = (uint8_t *)calloc((size_t)mask_stride, (size_t)mask_height);
		if (mask_bits) {
			console_mono_bitmap_info_t bi;
			memset(&bi, 0, sizeof(bi));
			bi.header.biSize = sizeof(bi.header);
			bi.header.biWidth = width;
			bi.header.biHeight = mask_height;
			bi.header.biPlanes = 1;
			bi.header.biBitCount = 1;
			bi.header.biCompression = BI_RGB;
			bi.header.biClrUsed = 2;
			bi.colors[0].rgbRed = bi.colors[0].rgbGreen = bi.colors[0].rgbBlue = 0;
			bi.colors[1].rgbRed = bi.colors[1].rgbGreen = bi.colors[1].rgbBlue = 255;
			mask_ok = GetDIBits(screen_dc, info.hbmMask, 0, (UINT)mask_height, mask_bits, (BITMAPINFO *)&bi, DIB_RGB_COLORS) == mask_height;
		}
	}
	if (info.hbmColor) {
		BITMAPINFO bi;
		color_bits = (uint32_t *)calloc((size_t)width * (size_t)height, sizeof(*color_bits));
		if (color_bits) {
			memset(&bi, 0, sizeof(bi));
			bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
			bi.bmiHeader.biWidth = width;
			bi.bmiHeader.biHeight = height;
			bi.bmiHeader.biPlanes = 1;
			bi.bmiHeader.biBitCount = 32;
			bi.bmiHeader.biCompression = BI_RGB;
			color_ok = GetDIBits(screen_dc, info.hbmColor, 0, (UINT)height, color_bits, &bi, DIB_RGB_COLORS) == height;
		}
	}
	pixels = (uint32_t *)calloc((size_t)width * (size_t)height, sizeof(*pixels));
	if (!pixels)
		goto done;
	if (color_ok) {
		for (int i = 0; i < width * height; i++)
			if (color_bits[i] & 0xff000000u) { alpha_present = 1; break; }
		for (int y = 0; y < height; y++) {
			int src_y = height - 1 - y;
			for (int x = 0; x < width; x++) {
				uint32_t pixel = color_bits[(size_t)src_y * (size_t)width + (size_t)x];
				if (!alpha_present) {
					int transparent = mask_ok && console_mask_bit(mask_bits, mask_stride, mask_height, x, y);
					pixel = (pixel & 0x00ffffffu) | (transparent ? 0u : 0xff000000u);
				}
				pixels[(size_t)y * (size_t)width + (size_t)x] = pixel;
			}
		}
	} else if (mask_ok) {
		for (int y = 0; y < height; y++) {
			for (int x = 0; x < width; x++) {
				int and_bit = console_mask_bit(mask_bits, mask_stride, mask_height, x, y);
				int xor_bit = mask_height >= height * 2 ? console_mask_bit(mask_bits, mask_stride, mask_height, x, y + height) : 0;
				uint32_t pixel = and_bit && !xor_bit ? 0u : (xor_bit ? 0xffffffffu : 0xff000000u);
				pixels[(size_t)y * (size_t)width + (size_t)x] = pixel;
			}
		}
	}
	if (pixels && (color_ok || mask_ok)) {
		free(console_cursor_pixels);
		console_cursor_pixels = pixels;
		pixels = NULL;
	}
done:
	if (screen_dc) ReleaseDC(NULL, screen_dc);
	free(color_bits);
	free(mask_bits);
	free(pixels);
	if (info.hbmColor) DeleteObject(info.hbmColor);
	if (info.hbmMask) DeleteObject(info.hbmMask);
}

void console_install(void)
{
	InterlockedExchange(&console_cursor_kind, CONSOLE_CURSOR_ARROW);
	InterlockedExchange(&console_last_cursor_hidden, -1);
	InterlockedExchange(&console_last_native_cursor_kind, -1);
	console_cursor_bitmap_kind = -1;
	console_cursor_texture_kind = -1;
	log_printf("console: native cursor is used in menus; captured mode uses a non-teleporting software cursor");
	(void)console_codepage(); // resolve the game's language before the Present/render thread uses the text helpers
	InitializeCriticalSection(&lock);
	game_add_command(&condump_cmd);
	game_add_command(&dvar_verify_cmd);
	game_add_command(&sv_cheats_default_cmd);
	{
		int had_stock_command = game_command_exists("sv_cheats");
		game_add_command(&sv_cheats_cmd); // prepend: the direct BOOL setter takes precedence over a stock no-op node
		log_printf("console: registered direct sv_cheats BOOL setter (stock node existed: %d)", had_stock_command);
	}
	queue_default_sv_cheats();
	CloseHandle(CreateThread(NULL, 0, setup_thread, NULL, 0, NULL));
}
