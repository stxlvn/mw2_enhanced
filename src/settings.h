#pragma once
#include <stdint.h>

// <game dir>\mw2coop\settings.ini; call from DllMain, before the game starts.
void settings_load(void);
// The game's network port (default 28960).
uint16_t settings_port(void);
// settings.ini [network] steam_relay (default 0).
int settings_steam_relay(void);
// settings.ini [display] fonts (default 1): the Russian game gets the mod's fonts (fonts.c); 0 keeps the game's own.
int settings_fonts(void);
// settings.ini [display] fontmip (default 1): the mod's font atlas is filtered between its mip levels (trilinear), so small text is smooth; 0 keeps the game's plain bilinear.
int settings_fontmip(void);
// settings.ini [display] darktitle (default 1): the game window gets the dark title bar and square corners of Windows 10/11 (window.c); 0 leaves it as the game makes it.
int settings_darktitle(void);
// settings.ini [display] safearea_auto (default 1): on a screen wider than 16:9 the dvar safeArea_horizontal is lowered by 16:9 / the screen's ratio, so the menus keep the 16:9 margins (window.c); 0 leaves it as it is.
int settings_safearea(void);
// settings.ini [display] hwgamma (default 1): the game sets the display's hardware gamma ramp from r_gamma (default 0.8, a darker picture than the windowed and borderless modes, which have no ramp) in exclusive full screen; 0 sets r_ignorehwgamma so that full screen looks like them (guards.c).
int settings_hwgamma(void);
// settings.ini [native] unlinkguard (default 1): the iw4x-sp client only, see guards.c.
int settings_unlinkguard(void);
// settings.ini [native] hotguards (default 1): 0 leaves the game's per-entity weapon definition and entity table lookups unguarded (QA: tells their cost from the game's own).
int settings_hotguards(void);
// settings.ini [display] hudtextscale (default 50, percent, 20-200): the size of the objective pointer labels ("Follow", "Protect", the distance such as "2.0m") relative to the game's own (guards.c).
int settings_hudtextscale(void);
void settings_set_hudtextscale(int percent);
// settings.ini [display] nametagscale (default 70, percent, 20-200): the same for the names over other players.
int settings_nametagscale(void);
void settings_set_nametagscale(int percent);
// The full path of settings.ini (after settings_load); the options page (enhopts.c) writes the player's choices there.
const char *settings_ini_path(void);
// settings.ini [display] d3d9ex (default 0 until tested, then 1): the game's Direct3D 9 is the extended one (Direct3DCreate9Ex, src/d3dcap.c): a window that loses the focus (Alt+Tab) does not lose the device,
// so the render thread does not stop and a co-op partner is not left waiting. Not in the iw4x-sp profile (it has its own r_useD3D9Ex).
int settings_d3d9ex(void);
// settings.ini [display] directx11 (default 0, experimental): the game's own Direct3D 9 back end is replaced by the mod's native Direct3D 11 one (src/dx/natboot.c); no Direct3D 9 device
// exists then, so what needs one (the mod's console overlay, screenshots, FSR 1 / FSR 3 / DLSS, the split screen compositor) is off. MW2COOP_NATIVE11=1 does the same for a test.
int settings_directx11(void);
// settings.ini [network] guard (default 1): packets no co-op session sends (joinParty, rcon, a co-op command from a stranger, a map name with a command separator) are dropped before the game sees them (netguard.c); 0: the game sees everything.
int settings_netguard(void);
// settings.ini [display] d3d9on12 (default 0, experimental): the game's Direct3D 9 runs on Windows' Direct3D 12 (d3d9.dll Direct3DCreate9On12), the way to DX12 and, through D3D11On12, DX11 (d3dcap.c).
int settings_d3d9on12(void);
// settings.ini [display] fsr3 (default 0, experimental): FSR 3 on the game's frame (fsr3.c; switches d3d9on12 on); fsr3_dir: the folder with AMD's amd_fidelityfx_loader_dx12.dll.
// 64-bit build only: always 0 in the 32-bit and iw4x builds (AMD's DLLs are 64-bit only).
int settings_fsr3(void);
const char *settings_fsr3_dir(void);
// settings.ini [display] dlss (default 0, experimental): NVIDIA's DLSS Super Resolution instead of FSR 3 on the same frame path (it also makes settings_fsr3() 1); dlss_dir: the folder with
// mw2coop_dlss.dll (src/dlss, the bridge to NVIDIA's SDK) and nvngx_dlss.dll. 64-bit build only.
int settings_dlss(void);
const char *settings_dlss_dir(void);
// dlss_preset (empty by default: NVIDIA's own choice, the transformer model K for the quality mode): the DLSS model, a letter J, K, L or M.
const char *settings_dlss_preset(void);
// fsr3_sharpness (0-100, default 60): the sharpening FSR 3 applies to its output (0 = off).
int settings_fsr3_sharpness(void);
void settings_set_fsr3_sharpness(int percent);
// fsr3_refine (default 1): how FSR 3 gets motion for what moves on its own (soldiers, the weapon): 0 = the camera's motion only, 1 = strict image-based correction (near, clearly
// matching blocks only), 2 = the classic correction (every block; guesses motion from the jitter's aliasing on distant fine detail).
int settings_fsr3_refine(void);
// fsr3_lodbias (empty by default: AMD's log2(render / output) - 1): the texture LOD bias of the world pass with FSR 3; 1 and the value when set.
int settings_fsr3_lodbias(float *bias);
// settings.ini [display] upscale (default 0): the game renders at the size of its own video settings and is upscaled to upscale_width x upscale_height (FSR 1, upscale.c);
// upscale_sharpness: the sharpening in hundredths of a stop of reduction (default 25; 0 = the strongest).
int settings_upscale(void);
int settings_upscale_width(void);
int settings_upscale_height(void);
int settings_upscale_sharpness(void);
// settings.ini [display] d3d9on12_adapter (default -1: the adapter with the primary display attached): the graphics adapter index the Direct3D 12 device is made on.
int settings_d3d9on12_adapter(void);
// settings.ini [display] borderless (default 1): the game's exclusive full screen is made a window of the whole screen without a border (d3dcap.c).
int settings_borderless(void);
// settings.ini [native] keepads (default 1): the aim bit stays in the player's command while the Cliffhanger climb reads it (climbads.c).
int settings_keepads(void);
// settings.ini [gameplay] fall_damage (0 or 1, -1: the game's own), jump_height (1-1000, 0: the game's own), gravity (1-5000, 0: the game's own): set at start as the session dvars
// mw2coop_fall_damage, mw2coop_jump_height, mw2coop_gravity (maps\_mw2coop_options).
int settings_gameplay_fall_damage(void);
int settings_gameplay_jump_height(void);
int settings_gameplay_gravity(void);
// settings.ini [discord] client_id: the id of a Discord application (digits); empty (the default): no Discord presence (discord.c).
const char *settings_discord_id(void);
// The picture of the Discord card: the address of a file of the mod's repository (fixed, not a setting).
const char *settings_discord_image(void);
// mw2coop\player.ini [player] id: 32 lowercase hex digits, made up once (main thread only).
const char *settings_player_id(void);
