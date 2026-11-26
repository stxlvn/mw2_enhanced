#pragma once
#include <stdint.h>

// UPnP port forward of the game's UDP port while this PC hosts (upnp.c).
void upnp_install(void);
// Control thread: 1 while this PC hosts a co-op lobby or game.
void upnp_want(int hosting);
// This PC's IPv4 address on its internet route (the network the game talks on); 0 if none.
int upnp_own_address(uint8_t out[4]);
// The Steam lobby this PC's party has made or joined (64-bit build); 0 if none.
int upnp_steam_lobby(uint64_t *id);
