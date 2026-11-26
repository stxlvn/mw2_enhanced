// Console commands added by mw2coop, run on the game's main thread by Cbuf like any command.
//
// mw2coop_join <ip> [port]: join a host by address, without a Steam lobby. The normal client
// path joins the host's Steam lobby first (rva 0x28f1b0 -> 0x22dda0 -> PartyClient_Join 0xd7cb0),
// which puts the host's address info (64 bytes, from the Steam lobby) into the client's party
// and activates it before "connect ip port". This does what PartyClient_Join does with that info
// filled from the address, then connects. The party must know the host's address: the host's
// lobby heartbeat ("hb", rva 0xd68b0) is only accepted from it, and a client that never answers
// with "hba" is dropped by the host after coop_lobbyTimeout (12 s) and resets its own party.
// Host info as Lobby_Join copies it (lobby +0x14) and rva 0x293cb0 reads it: +0 xuid,
// +8 internal IPv4, +0xc external IPv4, +0x10 internal port, +0x12 external port (host order);
// the internal pair is used when both PCs share the external address.
//
// mw2coop_party_join [ip [port]]: the same party join without the connect (the v0.2.0 join
// launcher runs it before its own connect; with no address the host info stays empty, which
// only holds for an immediate launch).
//
// mw2coop_launch_map: start coop_mapName on this machine the way the lobby launch does
// (rva 0x22e1a0). Used for the next mission: in a level the party is inactive (rva 0x5dd951
// is 0, so the party frame never acts on coopLaunch), and a client that has launched once
// (slot state 3) ignores further coopLaunch messages. Both machines see the same level-end
// request, so each DLL runs this itself; the rest of the session state is unchanged.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "log.h"
#include "settings.h"
#include "party.h"
#include "nick.h"
#include "rejoin.h"
#include "upnp.h"

static uint8_t *base;
#define FN(rva, type) ((type)(base + (rva)))

static int parse_address(const char *ip, const char *port_text, uint8_t addr[4], uint16_t *port)
{
	unsigned a, b, c, d;
	char tail;
	if (sscanf(ip, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
		return 0;
	addr[0] = (uint8_t)a, addr[1] = (uint8_t)b, addr[2] = (uint8_t)c, addr[3] = (uint8_t)d;
	unsigned p = port_text[0] ? (unsigned)atoi(port_text) : settings_port();
	if (p == 0 || p > 65535) {
		log_printf("party: bad port '%s' (settings port %u)", port_text, (unsigned)settings_port());
		return 0;
	}
	*port = (uint16_t)p;
	return 1;
}

// returns 1 if the party was joined
static int party_join(const uint8_t *addr, uint16_t port)
{
	uint8_t *lobby = *(uint8_t **)(base + RVA_PARTY_LOBBY);
	if (!lobby) {
		log_printf("party: no lobby object");
		return 0;
	}
	uint64_t host_info[8] = {0};
#ifndef _WIN64
	// The 32-bit game has the whole client join in one function (PartyClient_Join, 0x42fc40: Party_Reset, the lobby join,
	// the address translation, Party_Activate); the host info is laid out as in 64 bits (see the comment at the top).
	if (addr) {
		uint8_t *hi = (uint8_t *)host_info;
		memcpy(hi + 8, addr, 4);
		memcpy(hi + 0xc, addr, 4);
		memcpy(hi + 0x10, &port, 2);
		memcpy(hi + 0x12, &port, 2);
	}
	uint32_t out_ip = 0;
	uint16_t out_port = 0;
	char ok = FN(RVA_PARTY_JOIN, char (*)(int, void *, uint32_t *, uint16_t *))(0, host_info, &out_ip, &out_port);
	log_printf("party: client party joined (%d, active %d, host %u.%u.%u.%u:%u)", ok, base[RVA_PARTY_ACTIVE],
		   addr ? addr[0] : 0, addr ? addr[1] : 0, addr ? addr[2] : 0, addr ? addr[3] : 0, addr ? port : 0);
	return ok != 0;
#else
	if (addr) {
		uint8_t *hi = (uint8_t *)host_info;
		memcpy(hi + 8, addr, 4);
		memcpy(hi + 0xc, addr, 4);
		memcpy(hi + 0x10, &port, 2);
		memcpy(hi + 0x12, &port, 2);
	}
	FN(RVA_PARTY_RESET, void (*)(void))();
	FN(RVA_PARTY_CLEAR_MEMBERS, void (*)(int))(0);
	base[RVA_PARTY_IS_HOST] = 0;
	char ok = FN(RVA_LOBBY_JOIN, char (*)(void *, uint64_t, uint32_t, uint64_t *, uint64_t, uint32_t, uint32_t))(
		lobby, 0, 0x322, host_info, 0, 2, 0);
	*(int32_t *)(base + RVA_PARTY_START_TIME) = FN(RVA_SYS_MILLISECONDS, int (*)(void))();
	FN(RVA_PARTY_ACTIVATE, void (*)(void))();
	FN(RVA_PARTY_AFTER_JOIN, void (*)(void))();
	log_printf("party: client party joined (lobby join %d, active %d, lobby+0x10 %d, host %u.%u.%u.%u:%u)", ok,
		   base[RVA_PARTY_ACTIVE], lobby[0x10], addr ? addr[0] : 0, addr ? addr[1] : 0, addr ? addr[2] : 0,
		   addr ? addr[3] : 0, addr ? port : 0);
	return ok != 0;
#endif
}

static void party_join_f(void)
{
	uint8_t addr[4];
	uint16_t port;
	const char *ip = game_cmd_argv(1);
	if (!ip[0]) {
		party_join(NULL, 0);
		return;
	}
	if (!parse_address(ip, game_cmd_argv(2), addr, &port)) {
		log_printf("party: bad address '%s %s'", ip, game_cmd_argv(2));
		return;
	}
	party_join(addr, port);
}

static void join_f(void)
{
	uint8_t addr[4];
	uint16_t port;
	if (!parse_address(game_cmd_argv(1), game_cmd_argv(2), addr, &port)) {
		log_printf("party: usage: mw2coop_join <ip> [port] (got '%s' '%s')", game_cmd_argv(1), game_cmd_argv(2));
		return;
	}
	// A loopback address makes both parties' member addresses loopback (netadr type 2): the
	// game then hands the lobby heartbeat ("hb", rva 0xd7ee0) to itself instead of the socket,
	// the client never answers and is dropped after coop_lobbyTimeout (12 s). Two copies on one PC
	// (QA) join this PC's own address instead.
	uint8_t own[4];
	if (addr[0] == 127 && upnp_own_address(own)) {
		log_printf("party: %u.%u.%u.%u is loopback, joining this PC's %u.%u.%u.%u", addr[0], addr[1], addr[2], addr[3],
			   own[0], own[1], own[2], own[3]);
		memcpy(addr, own, 4);
	}
	if (!party_join(addr, port))
		return;
	if (rejoin_state() == REJOIN_CLIENT_JOIN)
		nick_rejoined_by_address(); // back into the session that broke: it keeps its kind (a session made through Steam keeps the Steam names)
	else if (GetEnvironmentVariableA("MW2COOP_QA_STEAMJOIN", NULL, 0)) // (QA: a pair on one PC joins by address; this join counts as one made through Steam)
		log_printf("party: QA: this join by address counts as one made through Steam (names)");
	else
		nick_joined_by_address(); // a LAN session: "Player 2" unless a nickname is set (nick.c); before the connect, whose coopConnect carries the name
	// connect stores its port argument as-is into a network-order field
	char cmd[64];
	if (game_is_iw4x()) // the iw4x-sp client replaces `connect` by one that takes the plain port, swaps it and runs the game's own as `connect_coop`
		snprintf(cmd, sizeof(cmd), "connect %u.%u.%u.%u %u\n", addr[0], addr[1], addr[2], addr[3], (unsigned)port);
	else
		snprintf(cmd, sizeof(cmd), "connect %u.%u.%u.%u %u\n", addr[0], addr[1], addr[2], addr[3], (unsigned)(uint16_t)((port >> 8) | (port << 8)));
	game_cbuf_addtext(cmd);
}

static void launch_map_f(void)
{
	char map[64] = "";
	game_dvar_raw("coop_mapName", map, sizeof(map));
	log_printf("party: launching '%s' (slot state %d, party active %d)", map,
		   *(int32_t *)(base + RVA_COOP_SLOT_STATE), base[RVA_PARTY_ACTIVE]);
	if (map[0])
		FN(RVA_COOP_LAUNCH_MAP, void (*)(int))(0);
}

// QA: the party's state (the host's heartbeat loop at rva 0xd7ee0 and timeout loop at 0xd74a0
// read the same fields)
void party_log_state(void)
{
	uint8_t *lobby = *(uint8_t **)(base + RVA_PARTY_LOBBY);
#ifndef _WIN64
	// 32-bit: the flags the party frame (0x4a4be0) and the launch (0x57c800) read: lobby +8 active, bytes +0x68 / +0xa0 mark
	// the two members present (0x464040 counts them)
	log_printf("party: active %d host %d launch %d lobby %p (+8 %d) members %d %d", base[RVA_PARTY_ACTIVE], base[RVA_PARTY_IS_HOST],
		   base[RVA_PARTY_LAUNCH], (void *)lobby, lobby ? lobby[8] : -1, lobby ? lobby[0x68] : -1, lobby ? lobby[0xa0] : -1);
	return;
#else
	log_printf("party: active %d host %d launch %d lobby %p (+0x10 %d)", base[RVA_PARTY_ACTIVE], base[RVA_PARTY_IS_HOST],
		   base[RVA_PARTY_LAUNCH], (void *)lobby, lobby ? lobby[0x10] : -1);
	if (!lobby)
		return;
	int now = FN(RVA_SYS_MILLISECONDS, int (*)(void))();
	for (int i = 0; i < 2; i++) {
		char used = FN(RVA_LOBBY_SLOT_USED, char (*)(void *, int))(lobby, i);
		uint64_t xuid = FN(XR(0x236b30), uint64_t (*)(void *, int))(lobby, i);
		char local = FN(XR(0x28fdf0), char (*)(uint64_t))(xuid);
		uint8_t addr[24] = {0};
		FN(XR(0x2367f0), void *(*)(void *, void *, int))(addr, lobby, i);
		int heard = *(int *)(base + XR(0x5dd984) + i * 0x38), sent = *(int *)(base + XR(0x5dd980) + i * 0x38);
		log_printf("party: slot %d used %d xuid %016llx local %d heard %d ms ago, sent %d ms ago, addr type %d %u.%u.%u.%u:%u",
			   i, used, (unsigned long long)xuid, local, now - heard, now - sent, *(int *)addr, addr[4], addr[5],
			   addr[6], addr[7], (unsigned)((addr[8] << 8) | addr[9]));
	}
#endif
}

static cmd_function_t commands[] = {
	{NULL, "mw2coop_party_join", NULL, NULL, party_join_f CMD_NODE_FLAG},
	{NULL, "mw2coop_join", NULL, NULL, join_f CMD_NODE_FLAG},
	{NULL, "mw2coop_launch_map", NULL, NULL, launch_map_f CMD_NODE_FLAG},
};

void party_install(void)
{
	base = (uint8_t *)GetModuleHandleA(NULL);
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		game_add_command(&commands[i]);
		log_printf("party: added console command %s", commands[i].name);
	}
}
