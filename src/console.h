#pragma once

// The MW2 Enhanced developer console (console.c). The retail build has console dvars but no console UI.
// ` / ~ opens an almost-square Enhanced window on the right; use the Enhanced/Legacy title-bar button to switch layouts, drag the title bar to move it, double-click the title bar to expand/restore, and use the bottom-right grip to resize; Shift+~ expands to fullscreen. Input maps letters through the US layout while Shift+punctuation follows the active keyboard layout. Console UI and localized messages follow the game language by default; the RU/EN title-bar toggle can override this choice.
// Ctrl+A/C/X/V edit the input; Ctrl+W or Ctrl+Backspace deletes a word; Ctrl+U/K delete to the start/end; Ctrl+L clears scrollback.
// Output and input text can be selected with the mouse and copied; double-click an input word, triple-click an output row, or use Copy Line in the output context menu. Autocomplete appears in a content-sized dropdown; `*term`, `term*`, `*term*`, and `?term` search names. Ctrl+F searches output; F3 / Shift+F3 move through matches. A typed prefix filters Up/Down history. QoL commands: click the pin glyph beside a dvar autocomplete entry to pin/unpin it; watch/unwatch/watchlist and dvarcompare <name> are also available.
// The software arrow is a separate D3D sprite, scaled from client to backbuffer coordinates; it follows the actual Win32 cursor in menus and raw relative movement while gameplay has captured it. The hardware/game cursor is suppressed with WM_SETCURSOR while the console is open and restored through the original game procedure on close.
// Drag the title bar to move the window; double-click it to expand/restore; drag the bottom-right grip to resize. The leftmost title-bar control toggles the persistent Pause mode (grey/off by default). Enhanced starts almost square on the right; Legacy starts half-width near the left. Both can be moved and resized within the overlay area. Right-click opens an editor-style menu with selection, clipboard and output actions.
// The selected layout, UI language and Pause mode are persisted in mw2coop\settings.ini as [console] layout=enhanced|legacy, language=auto|en|ru and pause_mode=0|1. Pause mode is off by default; when enabled, opening the console pauses with cl_paused 1 and closing it returns to 0 only if the game was not already paused. While open, the console captures mouse input so menu cursors do not move behind the overlay.
// When Pause mode itself pauses a previously running game, paint a large subdued "|| Game pause activated" watermark behind the output. Favorite dvar values use the shared typed formatter, including FLOAT2/FLOAT3/FLOAT4 and colour vectors.
// Bare dvar writes use `seta`; successful changes stay silent, and failed or deferred changes show a warning.
// sv_cheats is a changeable dvar that is never saved to the config (the game's INIT/ROM and the seta ARCHIVE flags are taken off it), whether or not [console] sv_cheats_default switches it on at the start; the direct BOOL sv_cheats command is retained.
void console_install(void);
// A line of output for the panel (any thread). Engine output is added the same way once the print function is hooked.
void console_print(const char *text);
// native.c's Present hooks call this after the final back-buffer composition (render thread).
// The panel is drawn only for a frame that will actually be presented.
void console_present(int via, void *device);
// The native renderer's frame hook (src/dx/natboot.c, once per frame on the render thread, before the present): when the console is open, repaints the panel bitmap if it changed and hands it to
// `show` (a 32-bit B8G8R8A8 bitmap of w x h pixels, `repainted` = it differs from the last call), which draws it over the whole back buffer. Returns 1 when the panel is to be drawn.
void console_qa_open(int open); // QA: opens / closes the panel without the key
int console_native_frame(unsigned back_width, unsigned back_height, void (*show)(const uint32_t *bits, int w, int h, int repainted));
