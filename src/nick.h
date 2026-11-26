#pragma once
// The player's nickname in the co-op lobby (nick.c).
#include <stddef.h>

// the name this PC gives now (the nickname, the Steam name, or "Player 1" / "Player 2")
const char *nick_current(void);
// the name this PC has in a running session where it is the host (1) or the client (0): in a level the party is not active, and nick_current's default
// then says "Player 1" on both PCs (chat.c)
const char *nick_in_session(int host);
// a new nickname from the lobby's popup ("" goes back to the default); saved in player.ini and told to the partner
void nick_set(const char *text);
void nick_clean(const char *in, char *out, size_t size);
// party.c: this PC joins a lobby by address (a LAN session), before its connect
void nick_joined_by_address(void);
// party.c: the same for the rejoin after a break (rejoin.c): a session made through Steam stays one, with its names
void nick_rejoined_by_address(void);
// net.c: the partner's "mw2coop_nick <L|S> <name>"
void nick_on_packet(const char *text);
// the menus' tick (main thread)
void nick_tick(void);
