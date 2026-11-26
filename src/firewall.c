// Windows Firewall, the in-game way: the first time this PC hosts in a run, a hidden PowerShell
// checks whether the game's exe may receive traffic (mw2coop\mw2coop-firewall.ps1 -Check: inbound
// rules for that exe allow UDP on every network profile, Radmin VPN and the like being Public, and
// none blocks it; Windows adds block rules when its own "Allow access?" prompt was cancelled). If
// not, the game shows a yes/no popup (a copy of the quit popup); "Yes" runs the script with
// administrator rights (one UAC prompt) to remove the block rules and add (or repair) the mod's
// allow rule for this exe. What the script found (mw2coop\firewall-check.txt, firewall-apply.txt)
// goes into mw2coop.log.
#include <windows.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "firewall.h"
#include "game.h"
#include "log.h"
#include "menus_ui.h"

static volatile LONG check_state; // 0 not yet, 1 running, 2 fine, 3 blocked
static volatile LONG popup_wanted;
static char game_dir[MAX_PATH], exe_path[MAX_PATH];

static void paths(void)
{
	if (exe_path[0])
		return;
	GetModuleFileNameA(NULL, exe_path, MAX_PATH);
	lstrcpynA(game_dir, exe_path, MAX_PATH);
	char *slash = strrchr(game_dir, '\\');
	if (slash)
		slash[1] = 0;
}

static DWORD run_script(const char *mode, int elevated)
{
	char args[2 * MAX_PATH + 160];
	snprintf(args, sizeof(args),
		 "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"%smw2coop\\mw2coop-firewall.ps1\" -Exe \"%s\" %s",
		 game_dir, exe_path, mode);
	SHELLEXECUTEINFOA info = {0};
	info.cbSize = sizeof(info);
	info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
	info.lpVerb = elevated ? "runas" : "open";
	info.lpFile = "powershell.exe";
	info.lpParameters = args;
	info.nShow = SW_HIDE;
	if (!ShellExecuteExA(&info) || !info.hProcess)
		return (DWORD)-1; // also: UAC declined
	WaitForSingleObject(info.hProcess, 60000);
	DWORD code = (DWORD)-1;
	GetExitCodeProcess(info.hProcess, &code);
	CloseHandle(info.hProcess);
	return code;
}

static void report_path(char *path, size_t size, const char *file)
{
	snprintf(path, size, "%smw2coop\\%s", game_dir, file);
}

// an old report is never logged as this run's
static void drop_report(const char *file)
{
	char path[MAX_PATH + 64];
	report_path(path, sizeof(path), file);
	DeleteFileA(path);
}

// the script's report (one finding a line) into the log
static void log_report(const char *file)
{
	char path[MAX_PATH + 64];
	report_path(path, sizeof(path), file);
	FILE *f = fopen(path, "rb");
	if (!f)
		return;
	char line[400];
	for (int n = 0; n < 40 && fgets(line, sizeof(line), f); n++) {
		size_t len = strlen(line);
		while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = 0;
		if (len)
			log_printf("firewall:   %s", line);
	}
	fclose(f);
}

static DWORD WINAPI check_thread(LPVOID fix)
{
	if (fix) {
		drop_report("firewall-apply.txt");
		DWORD r = run_script("-Apply", 1);
		log_printf("firewall: fix %s", r == 0 ? "done" : r == (DWORD)-1 ? "not done (administrator rights not given?)" : "not done (the rule could not be made)");
		if (r != (DWORD)-1)
			log_report("firewall-apply.txt");
	}
	drop_report("firewall-check.txt");
	DWORD code = run_script("-Check", 0);
	InterlockedExchange(&check_state, code == 0 ? 2 : 3);
	log_printf("firewall: %s", code == 0 ? "the game may receive co-op traffic"
				   : code == 2 ? "the firewall could not be checked"
					       : "the game is blocked or has no rule for every network");
	log_report("firewall-check.txt");
	if (code == 1 && !fix) // the popup only when the fix can help: not when the rules could not be read
		InterlockedExchange(&popup_wanted, 1);
	return 0;
}

// mw2coop_firewall_fix: the popup's Yes
static void fix_f(void)
{
	paths();
	CloseHandle(CreateThread(NULL, 0, check_thread, (LPVOID)1, 0, NULL));
}

static void add_popup(void)
{
	if (ui_find_menu("mw2coop_firewall"))
		return;
	uint8_t *src = ui_find_menu("quit_popmenu");
	int yes = src ? ui_item_index(src, "yes") : -1;
	if (yes < 1 || ui_item_index(src, "no") < 0)
		return;
	uint8_t *menu = ui_clone_menu(src, "mw2coop_firewall");
	uint8_t **items = *(uint8_t ***)(menu + MENU_ITEMS);
	int count = *(int *)(menu + MENU_ITEM_COUNT);
	for (int i = 0; i < count; i++) {
		const char *text = *(const char **)(items[i] + ITEM_TEXT);
		if (text && strcmp(text, "@MENU_NOTICE") == 0)
			*(const char **)(items[i] + ITEM_TEXT) = "@MW2COOP_FIREWALL_TITLE";
	}
	// the question, just above Yes
	*(const char **)(items[yes - 1] + ITEM_TEXT) = "@MW2COOP_FIREWALL_TEXT";
	*(void **)(items[yes - 1] + ITEM_TEXT_EXP) = NULL;
	ui_popup_question_lines(menu, yes - 1, "MW2COOP_FIREWALL_TEXT", 2);
	*(void **)(items[yes] + ITEM_ACTION) =
		ui_script_set("\"play\" \"mouse_click\" ; \"close\" \"self\" ; \"exec\" \"mw2coop_firewall_fix\" ; ");
	*(void **)(items[ui_item_index(src, "no")] + ITEM_ACTION) = ui_script_set(UI_S_CLICK_CLOSE);
	if (ui_register_menu(menu))
		log_printf("firewall: popup added");
}

void firewall_tick(int hosting)
{
	add_popup();
	if (hosting && check_state == 0) {
		paths();
		InterlockedExchange(&check_state, 1);
		CloseHandle(CreateThread(NULL, 0, check_thread, NULL, 0, NULL));
	}
	if (hosting && popup_wanted && !ui_error_popup_open()) { // (after an error popup is closed, not on top of it)
		InterlockedExchange(&popup_wanted, 0);
		game_cbuf_addtext("openmenu mw2coop_firewall\n");
	}
}

static cmd_function_t commands[] = {
	{NULL, "mw2coop_firewall_fix", NULL, NULL, fix_f CMD_NODE_FLAG},
};

void firewall_install(void)
{
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
		game_add_command(&commands[i]);
}
