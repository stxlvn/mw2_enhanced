# Settings > Enhanced (src/enhopts.c)

The mod's own options page in the game's Settings menu: rows grouped by what they apply to (Picture, Window and renderer, Upscaling, HUD, Controls, Co-op), numbers beside the sliders, a pop-up that suggests a restart when the page is left after a change that waits for the next start.

## Where it can be opened

From Settings, in the main menu and in a level's pause menu: the Enhanced button is in the left column of every options page (Video, Audio, Controls, Voice, Chat), after Chat and before Optimal settings. In a level the pause menu's Settings opens the Video page alone (no main_options under it) and a level's zone brings its own copies of the game's menus, so the button is put into every instance of those pages (`patch_options`, run from the control thread every 250 ms by `enhopts_level_tick`, which also makes the page again after a level's menu list was rebuilt). The button is a plain `open`: the page goes over the options page, Esc comes back to it, Esc again leaves Settings. (The game's own column buttons close their sibling pages first; copying that left the page with nothing under it and Esc ended in the game: the first version, seen 2026-11-11.) The button is placed in the focus order right after Chat, so the keyboard and the gamepad reach it (appended at the end of the items it came after Optimal settings and could not be reached with Down). Changes apply at once or at the next start, in a level too (the HUD text sizes, the sharpening, the field of view, fall damage).

## Rows

See `src/enhopts.c` (`opts[]`) and the changelog. A row that mirrors a `settings.ini` key writes it when it changes; the DirectX row and the Upscaler row follow each other (FSR 3 and DLSS need DirectX 12).

## Layout notes

- Rows are 18 units apart, 6 more before a group name (at 20 the last row ran into Back).
- The number beside a slider is a text item without a text bound to the slider's dvar; its x is the bar's x plus 96 (the drawn bar is about that wide, the item's rectangle is wider).
- Menu items are copied from the game's own Look page and advanced video page, so the look matches; `SLIDER_BAR_W`, `ROW_H`, `GROUP_GAP` are the measured numbers.
