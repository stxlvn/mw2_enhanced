# Settings > Enhanced (src/enhopts.c)

The mod's own options page in the game's Settings menu: rows grouped by what they apply to (Picture, Window and renderer, Upscaling, HUD, Controls, Co-op), numbers beside the sliders, a pop-up that suggests a restart when the page is left after a change that waits for the next start.

## Where it can be opened

From the main menu only (Settings > Enhanced). The pause menu of a level has a Settings entry too, but it opens a copy of the options menu that the page's button is not in (checked 2026-11-11: the pause menu's Settings shows Video, Audio, Controls, Voice, Chat, Optimal settings; the mod's list of menus holds one `main_options` that carries the button, and patching every instance of that name, and making the page again after the level's menu list was rebuilt, did not change what the pause menu shows). So the game keeps another copy of the menus for the pause menu (another UI context or a menu read straight from the zone); finding it is not done. Changes made in the main menu apply at once or at the next start, in a level too (`enhopts_level_tick`, called by the control thread every 250 ms: the HUD text sizes, the sharpening, the field of view, fall damage).

## Rows

See `src/enhopts.c` (`opts[]`) and the changelog. A row that mirrors a `settings.ini` key writes it when it changes; the DirectX row and the Upscaler row follow each other (FSR 3 and DLSS need DirectX 12).

## Layout notes

- Rows are 18 units apart, 6 more before a group name (at 20 the last row ran into Back).
- The number beside a slider is a text item without a text bound to the slider's dvar; its x is the bar's x plus 96 (the drawn bar is about that wide, the item's rectangle is wider).
- Menu items are copied from the game's own Look page and advanced video page, so the look matches; `SLIDER_BAR_W`, `ROW_H`, `GROUP_GAP` are the measured numbers.
