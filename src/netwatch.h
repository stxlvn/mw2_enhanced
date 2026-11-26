#pragma once
// Network co-op watch (netwatch.c): the partner's silence and the two PCs' games going apart, shown in the game's message window.
void netwatch_install(void);
void netwatch_poll(void);			// control.c, every 250 ms
void netwatch_partner_packet(void);		// net.c: a packet from the partner came in
void netwatch_on_desync_packet(int level_time, int server_id); // net.c: the partner's PC found the games apart (packet mw2coop_desync; server_id 0: an older build sent none)
int netwatch_send_paused(void);			// net.c: QA mw2coop_netpause holds the packets to the partner
int netwatch_recv_paused(void);			// net.c: QA mw2coop_netqa_recvpause drops the partner's game packets (not out of band)
void netwatch_on_packet(const char *text);	// net.c: "mw2coop_nw <word> [n]" from the partner
void netwatch_resync_started(void);		// saves.c: the client got the host's mw2coop_resync
void netwatch_level_switch(void);		// saves.c: a fast_restart or a map command runs here: the level before it ends
unsigned netwatch_level_gen(void);		// saves.c: the number of such switches so far
int netwatch_this_pc_hosts(void);			// 1: this PC hosts the lockstep session (markers.c QA)
void netwatch_say(const char *text);		// report.c: a line in the game's message window while a level runs (main thread; Windows-1251)
