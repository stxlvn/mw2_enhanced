#pragma once

// The mod's own options page in the game's menu, Settings > Enhanced (enhopts.c): the field of view, the frame rate limit, the upscalers and their sharpening, HUD text sizes,
// window options, fall damage. Menu tick (main thread): registers the dvars once, adds the page and its button, writes changed rows to settings.ini and applies what can follow at once.
void enhopts_tick(void);
// Called while a level runs too (control.c's poll): the page's field of view is held against the level's scripts.
void enhopts_level_tick(void);
// The sharpening the page holds (0-100), -1 before the page's dvars exist.
int enhopts_sharpness(void);
