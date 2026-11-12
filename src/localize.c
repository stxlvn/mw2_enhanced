// The mod's own localized strings (&"MW2COOP_..." in scripts). The game draws Cyrillic only from
// its localized strings, which this build keeps in Windows-1251 (COOP_WAITINGFORPLAYER reads
// ce e6 e8 e4 ...): a plain script string with Cyrillic shows as dots. So
// DB_FindXAssetHeader(ASSET_TYPE_LOCALIZE_ENTRY, "MW2COOP_...") is answered here, in Russian
// when the game's loc_language dvar is 6, else in English.
//
// DB_FindXAssetHeader (rva 0x0ef5d0) starts with three 5-byte moves that use no RIP-relative
// address (mov [rsp+8],rbx / mov [rsp+10h],rbp / mov [rsp+18h],rsi); those 15 bytes are run from
// a trampoline that then jumps back to +15.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "game.h"
#include "fonts.h"
#include "localize.h"
#include "ctexts.h"
#include "scripts.h"
#include "transplant.h"
#include "hook.h"
#include "log.h"

#define ASSET_TYPE_LOCALIZE_ENTRY 27

typedef struct {
	const char *value;
	const char *name;
} LocalizeEntry;

typedef void *(*db_find_header_t)(int type, const char *name);

#define ASSET_TYPE_FONT 24
static db_find_header_t original;
static volatile LONG russian = -1;	// -1 unknown; a negative result is rechecked while assets initialize
static volatile LONG russian_probe_tick;

typedef struct {
	const char *name;
	const char *ru;	// Windows-1251
	const char *en;
} ModString;

static const ModString strings[] = {
	// Вы оглушены. Ожидайте второго игрока
	{"MW2COOP_STUNNED", "\xc2\xfb \xee\xe3\xeb\xf3\xf8\xe5\xed\xfb. \xce\xe6\xe8\xe4\xe0\xe9\xf2\xe5 \xe2\xf2\xee\xf0\xee\xe3\xee \xe8\xe3\xf0\xee\xea\xe0",
	 "You are stunned. Wait for the other player"},
	// Удерживайте [{+activate}], чтобы сесть за пулемёт (Team Player: the passenger takes the turret)
	{"MW2COOP_SWAP_SEATS", "\xd3\xe4\xe5\xf0\xe6\xe8\xe2\xe0\xe9\xf2\xe5 [{+activate}], \xf7\xf2\xee\xe1\xfb \xf1\xe5\xf1\xf2\xfc \xe7\xe0 \xef\xf3\xeb\xe5\xec\xb8\xf2",
	 "Hold [{+activate}] to take the turret"},
	// Огонь по своим: вкл / выкл (the lobby row: can the players hurt each other)
	{"MW2COOP_FF_ON", "\xce\xe3\xee\xed\xfc \xef\xee \xf1\xe2\xee\xe8\xec: \xe2\xea\xeb",
	 "Friendly fire: on"},
	{"MW2COOP_FF_OFF", "\xce\xe3\xee\xed\xfc \xef\xee \xf1\xe2\xee\xe8\xec: \xe2\xfb\xea\xeb",
	 "Friendly fire: off"},
	// Готов
	{"MW2COOP_READY_SET", "\xc3\xee\xf2\xee\xe2",
	 "Ready"},
	// Не готов
	{"MW2COOP_READY_UNSET", "\xcd\xe5 \xe3\xee\xf2\xee\xe2",
	 "Not ready"},
	// Старт
	{"MW2COOP_START_GO", "\xd1\xf2\xe0\xf0\xf2",
	 "Start"},
	// Старт: нужен второй игрок
	{"MW2COOP_START_NEED_PARTNER", "\xd1\xf2\xe0\xf0\xf2: \xed\xf3\xe6\xe5\xed \xe2\xf2\xee\xf0\xee\xe9 \xe8\xe3\xf0\xee\xea",
	 "Start: a second player is needed"},
	// Старт: вы не готовы
	{"MW2COOP_START_WAIT_YOU", "\xd1\xf2\xe0\xf0\xf2: \xe2\xfb \xed\xe5 \xe3\xee\xf2\xee\xe2\xfb",
	 "Start: you are not ready"},
	// Старт: напарник не готов
	{"MW2COOP_START_WAIT_PARTNER", "\xd1\xf2\xe0\xf0\xf2: \xed\xe0\xef\xe0\xf0\xed\xe8\xea \xed\xe5 \xe3\xee\xf2\xee\xe2",
	 "Start: your partner is not ready"},
	// Старт: ждём готовности
	{"MW2COOP_START_WAIT_BOTH", "\xd1\xf2\xe0\xf0\xf2: \xe6\xe4\xb8\xec \xe3\xee\xf2\xee\xe2\xed\xee\xf1\xf2\xe8",
	 "Start: waiting for both players"},
	// Ждём старта от хоста
	{"MW2COOP_START_HOST", "\xc6\xe4\xb8\xec \xf1\xf2\xe0\xf0\xf2\xe0 \xee\xf2 \xf5\xee\xf1\xf2\xe0",
	 "Waiting for the host to start"},
	// ПРИСОЕДИНИТЬСЯ К ИГРЕ
	{"MW2COOP_JOIN_IDLE", "\xcf\xd0\xc8\xd1\xce\xc5\xc4\xc8\xcd\xc8\xd2\xdc\xd1\xdf \xca \xc8\xc3\xd0\xc5",
	 "JOIN A GAME"},
	// ПОИСК ИГР В СЕТИ...
	{"MW2COOP_JOIN_SEARCHING", "\xcf\xce\xc8\xd1\xca \xc8\xc3\xd0 \xc2 \xd1\xc5\xd2\xc8...",
	 "SEARCHING THE NETWORK..."},
	// ИГР В СЕТИ НЕ НАЙДЕНО
	{"MW2COOP_JOIN_NONE", "\xc8\xc3\xd0 \xc2 \xd1\xc5\xd2\xc8 \xcd\xc5 \xcd\xc0\xc9\xc4\xc5\xcd\xce",
	 "NO GAMES FOUND"},
	// ПОДКЛЮЧЕНИЕ...
	{"MW2COOP_JOIN_CONNECTING", "\xcf\xce\xc4\xca\xcb\xde\xd7\xc5\xcd\xc8\xc5...",
	 "CONNECTING..."},
	// Игры в сети
	{"MW2COOP_LOBBIES", "\xc8\xe3\xf0\xfb \xe2 \xf1\xe5\xf2\xe8",
	 "Games on the network"},
	// Доступ: свободный
	{"MW2COOP_ACCESS_OPEN", "\xc4\xee\xf1\xf2\xf3\xef: \xf1\xe2\xee\xe1\xee\xe4\xed\xfb\xe9",
	 "Access: open"},
	// Доступ: по паролю
	{"MW2COOP_ACCESS_PASSWORD", "\xc4\xee\xf1\xf2\xf3\xef: \xef\xee \xef\xe0\xf0\xee\xeb\xfe",
	 "Access: password"},
	// Доступ: только друзья Steam
	{"MW2COOP_ACCESS_STEAM", "\xc4\xee\xf1\xf2\xf3\xef: \xf2\xee\xeb\xfc\xea\xee \xe4\xf0\xf3\xe7\xfc\xff Steam",
	 "Access: Steam friends only"},
	// Пароль: не задан / задан (the lobby row under Friendly fire, in password mode)
	{"MW2COOP_PW_UNSET", "\xcf\xe0\xf0\xee\xeb\xfc: \xed\xe5 \xe7\xe0\xe4\xe0\xed",
	 "Password: not set"},
	{"MW2COOP_PW_SET", "\xcf\xe0\xf0\xee\xeb\xfc: \xe7\xe0\xe4\xe0\xed",
	 "Password: set"},
	// Пароль сессии
	{"MW2COOP_EDIT_PASSWORD", "\xcf\xe0\xf0\xee\xeb\xfc \xf1\xe5\xf1\xf1\xe8\xe8",
	 "Session password"},
	// Адрес хоста
	{"MW2COOP_EDIT_ADDRESS", "\xc0\xe4\xf0\xe5\xf1 \xf5\xee\xf1\xf2\xe0",
	 "Host address"},
	// Пароль:
	{"MW2COOP_LABEL_PASSWORD", "\xcf\xe0\xf0\xee\xeb\xfc:",
	 "Password:"},
	// Адрес:
	{"MW2COOP_LABEL_ADDRESS", "\xc0\xe4\xf0\xe5\xf1:",
	 "Address:"},
	// Ввести адрес вручную...
	{"MW2COOP_BY_ADDRESS", "\xc2\xe2\xe5\xf1\xf2\xe8 \xe0\xe4\xf0\xe5\xf1 \xe2\xf0\xf3\xf7\xed\xf3\xfe...",
	 "Enter an address..."},
	// (пароль)
	{"MW2COOP_LOCKED", "(\xef\xe0\xf0\xee\xeb\xfc)",
	 "(password)"},
	// Неверный пароль
	{"MW2COOP_BAD_PASSWORD", "\xcd\xe5\xe2\xe5\xf0\xed\xfb\xe9 \xef\xe0\xf0\xee\xeb\xfc",
	 "Wrong password"},
	// Хост пускает только друзей Steam
	{"MW2COOP_FRIENDS_ONLY", "\xd5\xee\xf1\xf2 \xef\xf3\xf1\xea\xe0\xe5\xf2 \xf2\xee\xeb\xfc\xea\xee \xe4\xf0\xf3\xe7\xe5\xe9 Steam",
	 "The host only lets Steam friends in"},
	// РАЗДЕЛЕНИЕ ЭКРАНА (the console game's own wording of this button, MENU_SPLIT_SCREEN_CAPS; suggested in issue 2)
	{"MW2COOP_SPLITSCREEN", "\xd0\xc0\xc7\xc4\xc5\xcb\xc5\xcd\xc8\xc5 \xdd\xca\xd0\xc0\xcd\xc0",
	 "SPLIT SCREEN"},
	// Разделение экрана: the split screen lobby's title, after "Campaign :"
	{"MW2COOP_SPLIT_TITLE", "\xd0\xe0\xe7\xe4\xe5\xeb\xe5\xed\xe8\xe5 \xfd\xea\xf0\xe0\xed\xe0", "Split Screen"},
	// МУЛЬТИПЛЕЕР / НАСТРОЙКИ: the main menu's multiplayer entry (co-op) and its settings entry (the game's Options)
	{"MW2COOP_MULTIPLAYER", "\xcc\xd3\xcb\xdc\xd2\xc8\xcf\xcb\xc5\xc5\xd0", "MULTIPLAYER"},
	{"MW2COOP_SETTINGS", "\xcd\xc0\xd1\xd2\xd0\xce\xc9\xca\xc8", "SETTINGS"},
	// О МОДЕ: the About page of the main menu and its four links (opened by mw2coop_open_url, menus.c)
	{"MW2COOP_ABOUT", "\xce\x20\xcc\xce\xc4\xc5", "ABOUT THE MOD"},
	// the About page: who made the mod (nicknames as on GitHub; the mod's name in brackets for the authors of the work this one stands on)
	{"MW2COOP_CREDIT_0", "\xd0\xc0\xc7\xd0\xc0\xc1\xce\xd2\xd7\xc8\xca\xc8", "DEVELOPERS"},
	{"MW2COOP_CREDIT_1", "stxlvn", "stxlvn"},
	{"MW2COOP_CREDIT_2", "VladWinner", "VladWinner"},
	{"MW2COOP_CREDIT_3", "\xce\xd1\xce\xc1\xc0\xdf\x20\xc1\xcb\xc0\xc3\xce\xc4\xc0\xd0\xcd\xce\xd1\xd2\xdc", "SPECIAL THANKS"},
	{"MW2COOP_CREDIT_4", "alicealys (h2-mod)", "alicealys (h2-mod)"},
	{"MW2COOP_CREDIT_5", "momo5502 (COD-MOD)", "momo5502 (COD-MOD)"},
	{"MW2COOP_CREDIT_6", "AlterWare (iw4x-sp)", "AlterWare (iw4x-sp)"},
	{"MW2COOP_CREDIT_7", "FreeTheTech101 (iw4lan, IW4-Dump-Files)", "FreeTheTech101 (iw4lan, IW4-Dump-Files)"},
	{"MW2COOP_CREDIT_8", "cranebeaker (Co-op Anything)", "cranebeaker (Co-op Anything)"},
	// Альфа / Браво: the split screen lobby's player cards (call signs, owner 2026-10-28)
	{"MW2COOP_CALLSIGN_A", "\xc0\xeb\xfc\xf4\xe0", "Alpha"},
	{"MW2COOP_CALLSIGN_B", "\xc1\xf0\xe0\xe2\xee", "Bravo"},
	// (Back: отменить) / (Backspace: отменить): how a player takes his device choice back
	{"MW2COOP_SPLIT_CANCEL_PAD", " (Back: \xee\xf2\xec\xe5\xed\xe8\xf2\xfc)", " (Back: cancel)"},
	{"MW2COOP_SPLIT_CANCEL_KB", " (Backspace: \xee\xf2\xec\xe5\xed\xe8\xf2\xfc)", " (Backspace: cancel)"},
	// Геймпад: Альфа / Геймпад: Браво: the gamepad options pages under Options > Controls (padopts.c) and their rows
	{"MW2COOP_PAD_MENU_1", "\xc3\xe5\xe9\xec\xef\xe0\xe4: \xc0\xeb\xfc\xf4\xe0", "Gamepad: Alpha"},
	{"MW2COOP_PAD_MENU_2", "\xc3\xe5\xe9\xec\xef\xe0\xe4: \xc1\xf0\xe0\xe2\xee", "Gamepad: Bravo"},
	// Чувствительность обзора
	{"MW2COOP_PAD_SENS", "\xd7\xf3\xe2\xf1\xf2\xe2\xe8\xf2\xe5\xeb\xfc\xed\xee\xf1\xf2\xfc \xee\xe1\xe7\xee\xf0\xe0", "Look sensitivity"},
	// Инверсия обзора
	{"MW2COOP_PAD_INVERT", "\xc8\xed\xe2\xe5\xf0\xf1\xe8\xff \xee\xe1\xe7\xee\xf0\xe0", "Invert look"},
	// Помощь в прицеливании
	{"MW2COOP_PAD_ASSIST", "\xcf\xee\xec\xee\xf9\xfc \xe2 \xef\xf0\xe8\xf6\xe5\xeb\xe8\xe2\xe0\xed\xe8\xe8", "Aim assist"},
	// Стики местами (левша)
	{"MW2COOP_PAD_SOUTHPAW", "\xd1\xf2\xe8\xea\xe8 \xec\xe5\xf1\xf2\xe0\xec\xe8 (\xeb\xe5\xe2\xf8\xe0)", "Swap sticks (southpaw)"},
	// Тактическая раскладка (присесть на правый стик)
	{"MW2COOP_PAD_TACTICAL",
	 "\xd2\xe0\xea\xf2\xe8\xf7\xe5\xf1\xea\xe0\xff \xf0\xe0\xf1\xea\xeb\xe0\xe4\xea\xe0 (\xef\xf0\xe8\xf1\xe5\xf1\xf2\xfc \xed\xe0 \xef\xf0\xe0\xe2\xfb\xe9 \xf1\xf2\xe8\xea)",
	 "Tactical layout (crouch on the right stick)"},
	// Вибрация
	{"MW2COOP_PAD_RUMBLE", "\xc2\xe8\xe1\xf0\xe0\xf6\xe8\xff", "Vibration"},
	// the Enhanced options page (enhopts.c): its title, rows and the restart pop-up
	{"MW2COOP_ENH_MENU", "Enhanced", "Enhanced"},
	{"MW2COOP_ENH_FOV", "\xd3\xe3\xee\xeb \xee\xe1\xe7\xee\xf0\xe0", "Field of view"},
	{"MW2COOP_ENH_MAXFPS", "\xce\xe3\xf0\xe0\xed\xe8\xf7\xe5\xed\xe8\xe5 \xf7\xe0\xf1\xf2\xee\xf2\xfb \xea\xe0\xe4\xf0\xee\xe2", "Frame rate limit"},
	{"MW2COOP_ENH_SHARP", "\xd0\xe5\xe7\xea\xee\xf1\xf2\xfc FSR 3 / DLSS", "FSR 3 / DLSS sharpening"},
	{"MW2COOP_ENH_HUDTEXT", "\xd0\xe0\xe7\xec\xe5\xf0 \xef\xee\xe4\xef\xe8\xf1\xe5\xe9 \xf6\xe5\xeb\xe5\xe9", "Objective label size"},
	{"MW2COOP_ENH_NAMETAG", "\xd0\xe0\xe7\xec\xe5\xf0 \xe8\xec\xb8\xed \xe8\xe3\xf0\xee\xea\xee\xe2", "Name tag size"},
	{"MW2COOP_ENH_UPSCALER", "\xc0\xef\xf1\xea\xe5\xe9\xeb\xe5\xf0", "Upscaler"},
	{"MW2COOP_ENH_OFF", "\xc2\xfb\xea\xeb", "Off"},
	{"MW2COOP_ENH_FALLDMG", "\xd3\xf0\xee\xed \xee\xf2 \xef\xe0\xe4\xe5\xed\xe8\xff \xe2 \xea\xee\xee\xef\xe5\xf0\xe0\xf2\xe8\xe2\xe5", "Fall damage in co-op"},
	{"MW2COOP_ENH_BORDERLESS", "\xcf\xee\xeb\xed\xfb\xe9 \xfd\xea\xf0\xe0\xed \xe1\xe5\xe7 \xf0\xe0\xec\xea\xe8", "Borderless full screen"},
	{"MW2COOP_ENH_DARKTITLE", "\xd2\xb8\xec\xed\xfb\xe9 \xe7\xe0\xe3\xee\xeb\xee\xe2\xee\xea \xee\xea\xed\xe0", "Dark title bar"},
	{"MW2COOP_ENH_DIRECTX", "\xc2\xe5\xf0\xf1\xe8\xff DirectX", "DirectX version"},
	{"MW2COOP_ENH_RESTART_TITLE", "\xcd\xf3\xe6\xe5\xed \xef\xe5\xf0\xe5\xe7\xe0\xef\xf3\xf1\xea", "Restart needed"},
	{"MW2COOP_ENH_RESTART_TEXT", "\xdd\xf2\xe0 \xed\xe0\xf1\xf2\xf0\xee\xe9\xea\xe0 \xed\xe0\xf7\xed\xb8\xf2 \xe4\xe5\xe9\xf1\xf2\xe2\xee\xe2\xe0\xf2\xfc\n\xef\xee\xf1\xeb\xe5 \xef\xe5\xf0\xe5\xe7\xe0\xef\xf3\xf1\xea\xe0 \xe8\xe3\xf0\xfb.\n\xc2\xfb\xe9\xf2\xe8 \xe8\xe7 \xe8\xe3\xf0\xfb \xf1\xe5\xe9\xf7\xe0\xf1?", "This setting takes effect after\nthe game is restarted.\nQuit the game now?"},
	{"MW2COOP_ENH_G_PICTURE", "\xc8\xe7\xee\xe1\xf0\xe0\xe6\xe5\xed\xe8\xe5", "Picture"},
	{"MW2COOP_ENH_G_WINDOW", "\xce\xea\xed\xee \xe8 \xee\xf2\xf0\xe8\xf1\xee\xe2\xea\xe0", "Window and renderer"},
	{"MW2COOP_ENH_G_UPSCALING", "\xc0\xef\xf1\xea\xe5\xe9\xeb\xe8\xed\xe3", "Upscaling"},
	{"MW2COOP_ENH_G_HUD", "\xc8\xed\xf2\xe5\xf0\xf4\xe5\xe9\xf1", "HUD"},
	{"MW2COOP_ENH_G_CONTROLS", "\xd3\xef\xf0\xe0\xe2\xeb\xe5\xed\xe8\xe5", "Controls"},
	{"MW2COOP_ENH_RAWMOUSE", "\xcd\xe5\xee\xe1\xf0\xe0\xe1\xee\xf2\xe0\xed\xed\xfb\xe9 \xe2\xe2\xee\xe4 \xec\xfb\xf8\xe8", "Raw mouse input"},
	{"MW2COOP_ENH_G_COOP", "\xca\xee\xee\xef\xe5\xf0\xe0\xf2\xe8\xe2", "Co-op"},
	// Игрок 1: / Игрок 2: (the split screen lobby's hint line: each player presses a button on his device)
	{"MW2COOP_SPLIT_P1", "\xc8\xe3\xf0\xee\xea 1: ", "Player 1: "},
	{"MW2COOP_SPLIT_P2", "\xc8\xe3\xf0\xee\xea 2: ", "Player 2: "},
	// нажмите кнопку на своём устройстве / (клавиатура или геймпад), on two lines
	{"MW2COOP_SPLIT_PRESS", "\xed\xe0\xe6\xec\xe8\xf2\xe5 \xea\xed\xee\xef\xea\xf3 \xed\xe0 \xf1\xe2\xee\xb8\xec \xf3\xf1\xf2\xf0\xee\xe9\xf1\xf2\xe2\xe5\n(\xea\xeb\xe0\xe2\xe8\xe0\xf2\xf3\xf0\xe0 \xe8\xeb\xe8 \xe3\xe5\xe9\xec\xef\xe0\xe4)",
	 "press a button on your device\n(keyboard or gamepad)"},
	// клавиатура и мышь
	{"MW2COOP_SPLIT_KEYBOARD", "\xea\xeb\xe0\xe2\xe8\xe0\xf2\xf3\xf0\xe0 \xe8 \xec\xfb\xf8\xfc", "keyboard and mouse"},
	// геймпад %d
	{"MW2COOP_SPLIT_PAD", "\xe3\xe5\xe9\xec\xef\xe0\xe4 %d", "gamepad %d"},
	// Старт: игроки выбирают устройства
	{"MW2COOP_START_NEED_DEVICES", "\xd1\xf2\xe0\xf0\xf2: \xe8\xe3\xf0\xee\xea\xe8 \xe2\xfb\xe1\xe8\xf0\xe0\xfe\xf2 \xf3\xf1\xf2\xf0\xee\xe9\xf1\xf2\xe2\xe0",
	 "Start: the players choose their devices"},
	// КАМПАНИЯ
	{"MW2COOP_CAMPAIGN", "\xca\xc0\xcc\xcf\xc0\xcd\xc8\xdf",
	 "CAMPAIGN"},
	// Брандмауэр Windows
	{"MW2COOP_FIREWALL_TITLE", "\xc1\xf0\xe0\xed\xe4\xec\xe0\xf3\xfd\xf0 Windows",
	 "Windows Firewall"},
	// Второй игрок не сможет войти. / Разрешить доступ?
	{"MW2COOP_FIREWALL_TEXT", "\xc2\xf2\xee\xf0\xee\xe9 \xe8\xe3\xf0\xee\xea \xed\xe5 \xf1\xec\xee\xe6\xe5\xf2 \xe2\xee\xe9\xf2\xe8.\n\xd0\xe0\xe7\xf0\xe5\xf8\xe8\xf2\xfc \xe4\xee\xf1\xf2\xf3\xef?",
	 "The other player cannot get in.\nAllow access?"},
	// Другой напарник
	{"MW2COOP_PARTNER_TITLE", "\xc4\xf0\xf3\xe3\xee\xe9 \xed\xe0\xef\xe0\xf0\xed\xe8\xea",
	 "Different partner"},
	// Кампания «%s» сохранена с: %s. / Продолжить с %s?
	{"MW2COOP_PARTNER_FMT", "\xca\xe0\xec\xef\xe0\xed\xe8\xff \xab%s\xbb \xf1\xee\xf5\xf0\xe0\xed\xe5\xed\xe0 \xf1: %s.\n\xcf\xf0\xee\xe4\xee\xeb\xe6\xe8\xf2\xfc \xf1 %s?",
	 "The campaign \"%s\" was saved with: %s.\nContinue with %s?"},
	// Участники прохождения
	{"MW2COOP_MEMBER_TITLE", "\xd3\xf7\xe0\xf1\xf2\xed\xe8\xea\xe8 \xef\xf0\xee\xf5\xee\xe6\xe4\xe5\xed\xe8\xff",
	 "Playthrough members"},
	// Добавить %s в участники / этого прохождения?
	{"MW2COOP_MEMBER_FMT", "\xc4\xee\xe1\xe0\xe2\xe8\xf2\xfc %s \xe2 \xf3\xf7\xe0\xf1\xf2\xed\xe8\xea\xe8\n\xfd\xf2\xee\xe3\xee \xef\xf0\xee\xf5\xee\xe6\xe4\xe5\xed\xe8\xff?",
	 "Add %s to the members\nof this playthrough?"},
	// Новая игра
	{"MW2COOP_NEWGAME_TITLE", "\xcd\xee\xe2\xe0\xff \xe8\xe3\xf0\xe0",
	 "New game"},
	// Начать новую игру с %s / с самого начала?
	{"MW2COOP_NEWGAME_FMT", "\xcd\xe0\xf7\xe0\xf2\xfc \xed\xee\xe2\xf3\xfe \xe8\xe3\xf0\xf3 \xf1 %s\n\xf1 \xf1\xe0\xec\xee\xe3\xee \xed\xe0\xf7\xe0\xeb\xe0?",
	 "Start a new game with %s\nfrom the very beginning?"},
	// ВЫБРАТЬ СОХРАНЕНИЕ
	{"MW2COOP_PICK_SAVE", "\xc2\xdb\xc1\xd0\xc0\xd2\xdc \xd1\xce\xd5\xd0\xc0\xcd\xc5\xcd\xc8\xc5",
	 "PICK A SAVE"},
	// Сохранения
	{"MW2COOP_SAVES", "\xd1\xee\xf5\xf0\xe0\xed\xe5\xed\xe8\xff",
	 "Saves"},
	// без напарника
	{"MW2COOP_SAVE_NO_PARTNER", "\xe1\xe5\xe7 \xed\xe0\xef\xe0\xf0\xed\xe8\xea\xe0",
	 "no partner"},
	// ПРОДОЛЖИТЬ: 
	{"MW2COOP_CONTINUE_PREFIX", "\xcf\xd0\xce\xc4\xce\xcb\xc6\xc8\xd2\xdc: ",
	 "CONTINUE: "},
	// НОВАЯ КАМПАНИЯ
	{"MW2COOP_NEW_CAMPAIGN", "\xcd\xce\xc2\xc0\xdf \xca\xc0\xcc\xcf\xc0\xcd\xc8\xdf",
	 "NEW CAMPAIGN"},
	// Музей: bonus level description shown in the campaign co-op lobby
	{"MW2COOP_MUSEUM_DESC",
	 "\xc1\xee\xed\xf3\xf1\xed\xfb\xe9 \xf3\xf0\xee\xe2\xe5\xed\xfc: \xf1\xe2\xee\xe1\xee\xe4\xed\xee\xe5 \xe8\xf1\xf1\xeb\xe5\xe4\xee\xe2\xe0\xed\xe8\xe5 \xec\xf3\xe7\xe5\xff \xf1 \xfd\xea\xf1\xef\xee\xed\xe0\xf2\xe0\xec\xe8 \xea\xe0\xec\xef\xe0\xed\xe8\xe8.",
	 "Bonus level: freely explore the museum and its campaign exhibits."},
};
#define STRING_COUNT (int)(sizeof(strings) / sizeof(strings[0]))

// Names the menus show whose text the mod switches at run time: the lobby's ready and start
// buttons and the campaign menu's join button (localize_select: one of strings[]), the rows of
// the list of games on the network (localize_set_text: any text, Windows-1251). The UI looks the
// entry up each frame, so a switch shows at once.
typedef struct {
	const char *name;
	const char *first; // the string shown until a switch, NULL for text
	volatile LONG shown; // index into strings[], -1 for text, -2 until <first> is looked up
	char text[128];
	LocalizeEntry entry;
} Alias;

static Alias aliases[] = {
	{"MW2COOP_READY", "MW2COOP_READY_SET", -2, "", {NULL, NULL}},
	{"MW2COOP_START", "MW2COOP_START_GO", -2, "", {NULL, NULL}},
	{"MW2COOP_JOIN", "MW2COOP_JOIN_IDLE", -2, "", {NULL, NULL}},
	{"MW2COOP_ACCESS", "MW2COOP_ACCESS_OPEN", -2, "", {NULL, NULL}},
	{"MW2COOP_FF", "MW2COOP_FF_ON", -2, "", {NULL, NULL}},
	{"MW2COOP_PW", "MW2COOP_PW_UNSET", -2, "", {NULL, NULL}},
	{"MW2COOP_EDIT_TITLE", "MW2COOP_EDIT_PASSWORD", -2, "", {NULL, NULL}},
	{"MW2COOP_EDIT_LABEL", "MW2COOP_LABEL_PASSWORD", -2, "", {NULL, NULL}},
	{"MW2COOP_CONTINUE", NULL, -1, "", {NULL, NULL}},
	{"MW2COOP_LOBBY_1", NULL, -1, "", {NULL, NULL}},
	{"MW2COOP_SAVE_1", NULL, -1, "", {NULL, NULL}},
	{"MW2COOP_PARTNER_Q", NULL, -1, "", {NULL, NULL}},
	{"MW2COOP_MEMBER_Q", NULL, -1, "", {NULL, NULL}},
	{"MW2COOP_NEWGAME_Q", NULL, -1, "", {NULL, NULL}},
	{"MW2COOP_SAVE_2", NULL, -1, "", {NULL, NULL}},
	{"MW2COOP_SAVE_3", NULL, -1, "", {NULL, NULL}},
	{"MW2COOP_SAVE_4", NULL, -1, "", {NULL, NULL}},
	{"MW2COOP_LOBBY_2", NULL, -1, "", {NULL, NULL}},
	{"MW2COOP_LOBBY_3", NULL, -1, "", {NULL, NULL}},
};
#define ALIAS_COUNT (int)(sizeof(aliases) / sizeof(aliases[0]))

static LocalizeEntry entries[sizeof(strings) / sizeof(strings[0])];

// The game's own strings that read wrong in the Russian build. The text is Windows-1251 like the rest.
typedef struct {
	const char *name;
	const char *ru;
	LocalizeEntry entry;
} FixedString;

static FixedString fixed[] = {
	// The stock text names Price, who has not been introduced yet at this point of the story
	// (issue #6): "Доберитесь с капитаном Мактавишем и отрядом до зоны посадки."
	{"FAVELA_ESCAPE_OBJ_FOLLOW_SOAP", "\xc4\xee\xe1\xe5\xf0\xe8\xf2\xe5\xf1\xfc \xf1 \xea\xe0\xef\xe8\xf2\xe0\xed\xee\xec \xcc\xe0\xea\xf2\xe0\xe2\xe8\xf8\xe5\xec \xe8 \xee\xf2\xf0\xff\xe4\xee\xec \xe4\xee \xe7\xee\xed\xfb \xef\xee\xf1\xe0\xe4\xea\xe8.", {NULL, NULL}},
};
#define FIXED_COUNT (int)(sizeof(fixed) / sizeof(fixed[0]))

// ---------------------------------------------------------------------------------------------
// Texts from files: every *.str in <game>\mw2coop\localizedstrings is read at start (the .str format the Call of Duty
// engines and IW4M / iw4x mods use for localized strings: VERSION "1", then REFERENCE <NAME> with LANG_ENGLISH and
// LANG_RUSSIAN lines, // comments). A text found there replaces the built-in one of the same name; content/release/
// localizedstrings/mw2coop.str holds all of them and is made from the table above by tools/gen_texts_str.py.
static char *over_en[sizeof(strings) / sizeof(strings[0])];
static char *over_ru[sizeof(strings) / sizeof(strings[0])];
static char *over_fixed[sizeof(fixed) / sizeof(fixed[0])];

// <text> (UTF-8, <len> bytes) as Windows-1251, in a heap block; NULL if it is empty or does not fit
static char *utf8_to_1251(const char *text, int len)
{
	WCHAR wide[512];
	char *out;
	int n = MultiByteToWideChar(CP_UTF8, 0, text, len, wide, 512);
	if (n <= 0)
		return NULL;
	int bytes = WideCharToMultiByte(1251, 0, wide, n, NULL, 0, NULL, NULL);
	if (bytes <= 0 || !(out = HeapAlloc(GetProcessHeap(), 0, bytes + 1)))
		return NULL;
	WideCharToMultiByte(1251, 0, wide, n, out, bytes, NULL, NULL);
	out[bytes] = 0;
	return out;
}

// The quoted text at <p> (after LANG_xxx): \n and \" and \\ are escapes. Returns the converted text, or NULL.
static char *str_quoted(const char *p, const char *end)
{
	char raw[1024];
	int n = 0;
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;
	if (p >= end || *p != '"')
		return NULL;
	for (p++; p < end && *p != '"' && n < (int)sizeof(raw) - 1; p++) {
		if (*p == '\\' && p + 1 < end) {
			p++;
			raw[n++] = *p == 'n' ? '\n' : *p;
		} else
			raw[n++] = *p;
	}
	if (!n)
		return NULL;
	char *out = HeapAlloc(GetProcessHeap(), 0, (size_t)n + 1);
	if (out) {
		memcpy(out, raw, (size_t)n);
		out[n] = 0;
	}
	return out;
}

static int str_name_index(const char *name, int *fixed_index)
{
	*fixed_index = -1;
	for (int i = 0; i < STRING_COUNT; i++)
		if (strcmp(name, strings[i].name) == 0)
			return i;
	for (int i = 0; i < FIXED_COUNT; i++)
		if (strcmp(name, fixed[i].name) == 0) {
			*fixed_index = i;
			return -1;
		}
	return -1;
}

// en / ru are UTF-8 here (or NULL): the texts of this file's own table are kept as Windows-1251, the console's as UTF-8
static char *to_1251(const char *utf8)
{
	return utf8 ? utf8_to_1251(utf8, (int)strlen(utf8)) : NULL;
}

static void str_apply(const char *name, char *en, char *ru)
{
	int f;
	int i = str_name_index(name, &f);
	if (i >= 0) {
		if (en)
			over_en[i] = to_1251(en);
		if (ru)
			over_ru[i] = to_1251(ru);
	} else if (f >= 0) {
		if (ru)
			over_fixed[f] = to_1251(ru);
	} else if (!ctexts_set(name, en, ru))
		log_printf("localize: %s is not a text of the mod, ignored", name);
}

static int str_load_file(const char *path)
{
	HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return 0;
	DWORD size = GetFileSize(h, NULL), got = 0;
	char *buf = size && size < (1u << 20) ? HeapAlloc(GetProcessHeap(), 0, size + 1) : NULL;
	if (!buf || !ReadFile(h, buf, size, &got, NULL)) {
		CloseHandle(h);
		return 0;
	}
	CloseHandle(h);
	buf[got] = 0;
	char name[96] = "";
	char *en = NULL, *ru = NULL;
	int texts = 0;
	const char *p = buf, *stop = buf + got;
	if (got >= 3 && (unsigned char)p[0] == 0xef && (unsigned char)p[1] == 0xbb && (unsigned char)p[2] == 0xbf)
		p += 3;
	while (p < stop) {
		const char *eol = p;
		while (eol < stop && *eol != '\n')
			eol++;
		const char *e = eol;
		while (e > p && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
			e--;
		const char *q = p;
		while (q < e && (*q == ' ' || *q == '\t'))
			q++;
		if (q < e && !(q[0] == '/' && q + 1 < e && q[1] == '/')) {
			const char *w = q;
			while (w < e && *w != ' ' && *w != '\t')
				w++;
			size_t wl = (size_t)(w - q);
			if (wl == 9 && !strncmp(q, "REFERENCE", 9)) {
				if (name[0])
					str_apply(name, en, ru);
				name[0] = 0;
				en = ru = NULL;
				while (w < e && (*w == ' ' || *w == '\t'))
					w++;
				if (w < e && *w == '"')
					w++;
				size_t n = 0;
				while (w < e && *w != '"' && n < sizeof(name) - 1)
					name[n++] = *w++;
				name[n] = 0;
				texts++;
			} else if (name[0] && wl == 12 && !strncmp(q, "LANG_ENGLISH", 12))
				en = str_quoted(w, e);
			else if (name[0] && wl == 12 && !strncmp(q, "LANG_RUSSIAN", 12)) {
				if (e - w >= 5 && !strncmp(e - 5, "#same", 5))
					ru = en;
				else
					ru = str_quoted(w, e);
			}
		}
		p = eol + 1;
	}
	if (name[0])
		str_apply(name, en, ru);
	HeapFree(GetProcessHeap(), 0, buf);
	return texts;
}

static void load_str_files(void)
{
	char dir[MAX_PATH], pattern[MAX_PATH], path[MAX_PATH];
	DWORD length = GetModuleFileNameA(NULL, dir, sizeof(dir));
	char *slash = length && length < sizeof(dir) ? strrchr(dir, '\\') : NULL;
	if (!slash)
		return;
	slash[1] = 0;
	snprintf(pattern, sizeof(pattern), "%smw2coop\\localizedstrings\\*.str", dir);
	WIN32_FIND_DATAA fd;
	HANDLE find = FindFirstFileA(pattern, &fd);
	if (find == INVALID_HANDLE_VALUE)
		return;
	do {
		snprintf(path, sizeof(path), "%smw2coop\\localizedstrings\\%s", dir, fd.cFileName);
		int texts = str_load_file(path);
		log_printf("localize: %s: %d texts read", fd.cFileName, texts);
	} while (FindNextFileA(find, &fd));
	FindClose(find);
}

static int probe_game_language(int retry_negative)
{
	// DB_FindXAssetHeader calls this on the asset-loading path. Reading a cached language
	// value must not issue a locked read-modify-write on every asset lookup.
	LONG state = __atomic_load_n(&russian, __ATOMIC_ACQUIRE);
	if (state == 1 || (state == 0 && !retry_negative))
		return state; // once Russian is present, or a caller only needs the cached answer

	// loc_language is the game's language selector (6 = Russian). It may not be registered
	// at the first asset lookup, so leave the result unknown and retry from later asset lookups.
	// Keep Dvar_FindVar off the render path and throttle probes to once per second.
	DWORD now = GetTickCount();
	LONG last = __atomic_load_n(&russian_probe_tick, __ATOMIC_RELAXED);
	if ((DWORD)(now - (DWORD)last) < 1000)
		return state > 0;
	if (InterlockedCompareExchange(&russian_probe_tick, (LONG)now, last) != last)
		return state > 0;

	dvar_t *language = game_find_dvar("loc_language");
	if (!language || (language->type != DVAR_TYPE_INT && language->type != DVAR_TYPE_ENUM))
		return state > 0; // the dvar is not ready (or has an unexpected type); retry later

	int value = language->current.integer;
	int detected = value == 6;
	LONG previous = InterlockedExchange(&russian, detected);
	if (previous != detected)
		log_printf("localize: game language %s (loc_language=%d)%s",
			   detected ? "Russian" : "not Russian", value,
			   detected ? "" : "; will recheck while assets load");
	return detected;
}

static int game_is_russian(void)
{
	return probe_game_language(0);
}

int localize_is_russian(void)
{
	return game_is_russian();
}

int localize_retry_language(void)
{
	return probe_game_language(1);
}

static const char *text_of(int i)
{
	if (game_is_russian())
		return over_ru[i] ? over_ru[i] : strings[i].ru;
	return over_en[i] ? over_en[i] : strings[i].en;
}

static int string_index(const char *name)
{
	for (int i = 0; i < STRING_COUNT; i++)
		if (strcmp(name, strings[i].name) == 0)
			return i;
	return -1;
}

static void *find_header(int type, const char *name)
{
	if (type == ASSET_TYPE_LOCALIZE_ENTRY && name && strncmp(name, "MW2COOP_", 8) == 0) {
		for (int i = 0; i < ALIAS_COUNT; i++) {
			if (strcmp(name, aliases[i].name) != 0)
				continue;
			if (aliases[i].shown == -2)
				InterlockedCompareExchange(&aliases[i].shown, string_index(aliases[i].first), -2);
			aliases[i].entry.name = aliases[i].name;
			aliases[i].entry.value = aliases[i].shown < 0 ? aliases[i].text : text_of(aliases[i].shown);
			return &aliases[i].entry;
		}
		for (int i = 0; i < STRING_COUNT; i++) {
			if (strcmp(name, strings[i].name) != 0)
				continue;
			entries[i].name = strings[i].name;
			entries[i].value = text_of(i);
			return &entries[i];
		}
	}
	if (type == ASSET_TYPE_LOCALIZE_ENTRY && name) {
		for (int i = 0; i < FIXED_COUNT; i++) {
			if (strcmp(name, fixed[i].name) != 0 || !game_is_russian())
				continue;
			if (!fixed[i].entry.name) {
				LocalizeEntry *stock = original(type, name);
				log_printf("localize: %s replaced (stock text: %s)", name, stock && stock->value ? stock->value : "?");
			}
			fixed[i].entry.name = fixed[i].name;
			fixed[i].entry.value = over_fixed[i] ? over_fixed[i] : fixed[i].ru;
			return &fixed[i].entry;
		}
	}
#ifndef _WIN64
	if (game_is_iw4x() && type == 0x24) { // a script of ours: the iw4x client reads it as a rawfile (scripts.c)
		void *raw = scripts_raw_override(name);
		if (raw)
			return raw;
	}
#endif
	if (type == 4 && name && name[0] == 'm') {
		void *clone = transplant_clone(name, original);
		if (clone)
			return clone;
	}
	static signed char dblog = -1; // QA: MW2COOP_DBLOG=1 logs the first 700 asset lookups (type, name) before the call, and the result after (native DirectX bring-up: what the engine asks for, in what order)
	static volatile LONG dbn;
	if (dblog < 0)
		dblog = getenv("MW2COOP_DBLOG") != NULL;
	const LONG dbk = dblog ? InterlockedIncrement(&dbn) : 0;
	if (dblog && dbk <= 700)
		log_printf("dblog: %ld > type %d '%s'", (long)dbk, type, name ? name : "?");
	if (dblog && dbk <= 3) { // who asks: the return addresses (exe rva)
		void *bt[24];
		const USHORT nb = CaptureStackBackTrace(0, 24, bt, NULL);
		char line[400];
		int ln = 0;
		const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
		for (USHORT k = 0; k < nb && ln < (int)sizeof(line) - 20; k++)
			if ((uintptr_t)bt[k] > base && (uintptr_t)bt[k] < base + 0x4900000)
				ln += snprintf(line + ln, sizeof(line) - ln, " exe+%llx", (unsigned long long)((uintptr_t)bt[k] - base));
		log_printf("dblog: %ld stack:%s", (long)dbk, line);
	}
	void *found = original(type, name);
	if (dblog && dbk <= 700)
		log_printf("dblog: %ld < %p", (long)dbk, found);
	localize_retry_language(); // refresh an early fallback result from the asset-loading path, not the render thread
	if (type == ASSET_TYPE_FONT)
		fonts_seen(found, name, original);
	else
		fonts_retry(original); // catch localization/assets that become ready after the first font lookup
	return found;
}

void localize_select(const char *alias, const char *name)
{
	for (int a = 0; a < ALIAS_COUNT; a++) {
		if (strcmp(alias, aliases[a].name) != 0)
			continue;
		int i = string_index(name);
		if (i >= 0)
			InterlockedExchange(&aliases[a].shown, i);
	}
}

void localize_set_text(const char *alias, const char *text)
{
	for (int a = 0; a < ALIAS_COUNT; a++) {
		if (strcmp(alias, aliases[a].name) != 0)
			continue;
		InterlockedExchange(&aliases[a].shown, -1);
		lstrcpynA(aliases[a].text, text, sizeof(aliases[a].text));
	}
}

// Another text for one of the game's own strings (NULL puts the game's text back). Main thread only.
static LocalizeEntry *override_entry;
static const char *override_orig;
static char override_text[256];

void localize_override(const char *name, const char *text)
{
	if (!original)
		return; // (the hook is not in)
	LocalizeEntry *e = find_header(ASSET_TYPE_LOCALIZE_ENTRY, name);
	if (!e || !e->value || !e->name || strcmp(e->name, name) != 0)
		return;
	if (text) {
		if (e != override_entry) {
			override_entry = e;
			override_orig = e->value;
		}
		lstrcpynA(override_text, text, sizeof(override_text));
		e->value = override_text;
	} else if (override_entry == e) {
		e->value = override_orig;
		override_entry = NULL;
	}
}

const char *localize_text(const char *name)
{
	if (!original) // the hook is not in (the build was not recognised, or its bytes differ): the game's own function is not ours to call
		return NULL;
	LocalizeEntry *e = find_header(ASSET_TYPE_LOCALIZE_ENTRY, name);
	return e && e->value && e->name && strcmp(e->name, name) == 0 ? e->value : NULL;
}

#ifdef _WIN64
static const uint8_t expected[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18};
#else
// 32-bit DB_FindXAssetHeader (0x4f6f10): push ecx; push ebx; mov ebx, [0x69116c] (an absolute address, copied as it is; in the iw4x-sp client's exe it is 0x69415c: filled in at install)
static uint8_t expected[] = {0x51, 0x53, 0x8b, 0x1d, 0x6c, 0x11, 0x69, 0x00};
#endif

void localize_install(void)
{
#ifndef _WIN64
	{
		uint32_t at = XV(0x69116c); // (the same number on the official build)
		memcpy(expected + 4, &at, 4);
	}
#endif
	uint8_t *target = (uint8_t *)GetModuleHandleA(NULL) + RVA_DB_FIND_HEADER;
	if (memcmp(target, expected, sizeof(expected)) != 0) {
		log_printf("localize: unexpected bytes at DB_FindXAssetHeader, not hooking");
		return;
	}
	if (!hook_function(target, expected, sizeof(expected), find_header, (void **)&original))
		return;
	log_printf("localize: DB_FindXAssetHeader hooked for MW2COOP_ strings");
	load_str_files();
}
