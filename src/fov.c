// A field of view only one player should get. The Predator's UAV view (maps\_remotemissile)
// sets cg_fov with SetSavedDvar, which reaches both players' screens; setClientDvar cannot set
// cg_fov (the engine refuses it and drops the co-op session). The script names the pilot in
// mw2coop_fov_pilot ("1" host, "2" client, "" when the view ends; maps\_mw2coop::fov_pilot), and
// on the other machine this thread puts its own player's field of view back, every 10 ms, to
// what it was before the pilot's view began.
#include <windows.h>

#include "fov.h"
#include "game.h"
#include "log.h"

static DWORD WINAPI fov_thread(LPVOID unused)
{
	(void)unused;
	dvar_t *fov = NULL, *pilot = NULL;
	float normal = 0;
	int held = 0;
	for (;;) {
		if (!fov)
			fov = game_find_dvar("cg_fov");
		if (!pilot)
			pilot = game_find_dvar("mw2coop_fov_pilot");
		if (!fov || fov->type != DVAR_TYPE_FLOAT) {
			Sleep(pilot ? 100 : 250); // the dvar registry is searched less often while scripts have not registered the pilot marker
			continue;
		}
		char who = 0;
		if (pilot && pilot->type == DVAR_TYPE_STRING && pilot->current.string)
			who = pilot->current.string[0];
		if (who != '1' && who != '2') {
			normal = fov->current.value; // the player's own field of view
			if (held) {
				log_printf("fov: pilot view over");
				held = 0;
			}
			Sleep(pilot ? 100 : 250); // idle in ordinary play; wake faster only while a script marker may appear
			continue;
		}
		char me = game_peek(RVA_PARTY_IS_HOST, 1) ? '1' : '2';
		if (who == me || normal <= 0) {
			Sleep(100);
			continue;
		}
		if (fov->current.value != normal) {
			fov->current.value = normal;
			if (!held)
				log_printf("fov: player %c flies the Predator; this player keeps cg_fov %g", who, normal);
			held = 1;
		}
		Sleep(10); // keep the active Predator view correction responsive; the 100 Hz poll is not needed otherwise
	}
	return 0;
}

void fov_install(void)
{
	HANDLE t = CreateThread(NULL, 0, fov_thread, NULL, 0, NULL);
	if (t)
		CloseHandle(t);
}
