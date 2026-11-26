// Packet logging for QA: iw4sp.exe imports sendto (#20) and recvfrom (#17) from WSOCK32.dll by
// ordinal; their IAT slots are pointed at wrappers that log the game's UDP traffic.
// They also carry mw2coop's own packets: the lobby's ready state (menus.c), and the start point,
// sent by the host just before each coopLaunch it sends (see progress.c).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "access.h"
#include "game.h"
#include "log.h"
#include "net.h"
#include "netwatch.h"
#include "nick.h"
#include "netguard.h"
#include "settings.h"
#include "menus.h"
#include "progress.h"
#include "rejoin.h"
#include "report.h"
#include "voice.h"
#include "chat.h"
#include "saves.h"
#include "steamnet.h"

typedef int(WINAPI *sendto_t)(SOCKET, const char *, int, int, const struct sockaddr *, int);
typedef int(WINAPI *recvfrom_t)(SOCKET, char *, int, int, struct sockaddr *, int *);

static sendto_t real_sendto;
static recvfrom_t real_recvfrom;
static volatile LONG budget = 150; // max logged packets per run

// The co-op partner: the host learns it from the client's coopConnect, the client from its own.
// mw2coop's lobby packets (net_send_mod) go there on the game's socket.
static SOCKET peer_socket = INVALID_SOCKET;
static struct sockaddr_in peer;
static CRITICAL_SECTION peer_lock;
// the game's UDP socket (the one its out-of-band packets use), for broadcasts
static volatile SOCKET game_socket = INVALID_SOCKET;

static void log_packet(const char *dir, const struct sockaddr *sa, const char *buf, int len)
{
	// skip DemonWare traffic (not 0xffffffff out-of-band) and the lobby bandwidth test ("bw")
	if (len < 6 || *(const unsigned int *)buf != 0xffffffffu || (buf[4] == 'b' && buf[5] == 'w'))
		return;
	if (len >= 17 && memcmp(buf + 4, "mw2coop_ready", 13) == 0) // menus.c's, once a second
		return;
	if (InterlockedDecrement(&budget) < 0)
		return;
	char addr[64] = "?";
	if (sa && sa->sa_family == AF_INET) {
		const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
		const unsigned char *ip = (const unsigned char *)&in->sin_addr;
		snprintf(addr, sizeof(addr), "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], ntohs(in->sin_port));
	}
	char text[200];
	int n = 0;
	for (int i = 0; i < len && n < (int)sizeof(text) - 5; i++) {
		unsigned char c = (unsigned char)buf[i];
		if (c >= 0x20 && c < 0x7f && c != '\\')
			text[n++] = (char)c;
		else
			n += snprintf(text + n, sizeof(text) - n, "\\x%02x", c);
	}
	text[n] = 0;
	log_printf("net: %s %s len %d: %s", dir, addr, len, text);
}

// a co-op connect frame: "ÿÿÿÿ" 'R' 0 <sequence byte> "coopConnect" (the command at its start, as netguard.c reads co-op frames)
static int is_coop_connect(const char *buf, int len)
{
	static const char word[] = "coopConnect";
	return len >= 7 + (int)sizeof(word) - 1 && *(const unsigned int *)buf == 0xffffffffu && buf[4] == 'R' && buf[5] == 0 && memcmp(buf + 7, word, sizeof(word) - 1) == 0;
}

static int contains(const char *buf, int len, const char *word)
{
	int wl = (int)strlen(word);
	for (int i = 0; i + wl <= len; i++)
		if (memcmp(buf + i, word, wl) == 0)
			return 1;
	return 0;
}

// the text word followed by its NUL (a whole command: "coopAccept" is not found in "coopAcceptAck")
static int contains_word(const char *buf, int len, const char *word)
{
	int wl = (int)strlen(word) + 1;
	for (int i = 0; i + wl <= len; i++)
		if (memcmp(buf + i, word, wl) == 0)
			return 1;
	return 0;
}

// QA (2026-11-21, issue 27: "out of sync" over Steam while both games look the same): an internet link between two test copies on one PC. The environment
// variables MW2COOP_QALAG (one-way delay in ms), MW2COOP_QAJITTER (up to that many ms more for each packet, the order kept) and MW2COOP_QALOSS (packets lost per
// 1000) hold this copy's packets to IPv4 addresses other than loopback in a queue that a thread sends when they are due. Unset (players): nothing changes.
typedef struct {
	LONGLONG due;
	SOCKET s;
	int len, flags, tolen;
	struct sockaddr_storage to;
	char data[1500];
} lag_packet_t;
#define LAG_Q 512
static lag_packet_t *lag_q;
static int lag_head, lag_count, qa_lag, qa_jitter, qa_loss, lag_on;
static LONGLONG lag_last_due;
static long lag_lost, lag_sent;
static CRITICAL_SECTION lag_lock;
static LONGLONG lag_now_us(void)
{
	static LARGE_INTEGER hz;
	LARGE_INTEGER c;
	if (!hz.QuadPart)
		QueryPerformanceFrequency(&hz);
	QueryPerformanceCounter(&c);
	return c.QuadPart * 1000000 / hz.QuadPart;
}
static int env_int(const char *name)
{
	char v[32];
	const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
	return n && n < sizeof(v) ? atoi(v) : 0;
}
static DWORD WINAPI lag_thread(LPVOID unused)
{
	(void)unused;
	static lag_packet_t p;
	DWORD said = GetTickCount();
	for (;;) {
		Sleep(1);
		for (;;) {
			int have = 0;
			EnterCriticalSection(&lag_lock);
			if (lag_count && lag_now_us() >= lag_q[lag_head].due) {
				p = lag_q[lag_head];
				lag_head = (lag_head + 1) % LAG_Q;
				lag_count--;
				have = 1;
			}
			LeaveCriticalSection(&lag_lock);
			if (!have)
				break;
			real_sendto(p.s, p.data, p.len, p.flags, (const struct sockaddr *)&p.to, p.tolen);
			lag_sent++;
		}
		if (GetTickCount() - said >= 10000) {
			said = GetTickCount();
			log_printf("net: QA link: %ld packets sent late, %ld lost in the last 10 s", lag_sent, lag_lost);
			lag_sent = lag_lost = 0;
		}
	}
	return 0;
}
static void lag_install(void)
{
	qa_lag = env_int("MW2COOP_QALAG"), qa_jitter = env_int("MW2COOP_QAJITTER"), qa_loss = env_int("MW2COOP_QALOSS");
	if (qa_lag <= 0 && qa_jitter <= 0 && qa_loss <= 0)
		return;
	lag_q = (lag_packet_t *)calloc(LAG_Q, sizeof(lag_packet_t));
	if (!lag_q)
		return;
	InitializeCriticalSection(&lag_lock);
	CreateThread(NULL, 0, lag_thread, NULL, 0, NULL);
	lag_on = 1;
	log_printf("net: QA link: this copy's packets %d ms late, up to %d ms more, %d of 1000 lost", qa_lag, qa_jitter, qa_loss);
}
static int lag_send(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen) // 1: queued or lost (the caller returns len)
{
	if (!lag_on || !to || to->sa_family != AF_INET || ((const struct sockaddr_in *)to)->sin_addr.S_un.S_un_b.s_b1 == 127 || len <= 0 || len > 1500 ||
	    tolen > (int)sizeof(struct sockaddr_storage))
		return 0;
	if (qa_loss > 0 && rand() % 1000 < qa_loss) {
		lag_lost++;
		return 1;
	}
	LONGLONG due = lag_now_us() + (LONGLONG)qa_lag * 1000 + (qa_jitter > 0 ? (LONGLONG)(rand() % (qa_jitter + 1)) * 1000 : 0);
	EnterCriticalSection(&lag_lock);
	if (due < lag_last_due)
		due = lag_last_due; // (the order kept)
	if (lag_count == LAG_Q) { // full: this one goes now
		LeaveCriticalSection(&lag_lock);
		return 0;
	}
	lag_packet_t *p = &lag_q[(lag_head + lag_count) % LAG_Q];
	p->due = due, p->s = s, p->len = len, p->flags = flags, p->tolen = tolen;
	memcpy(&p->to, to, (size_t)tolen);
	memcpy(p->data, buf, (size_t)len);
	lag_count++;
	lag_last_due = due;
	LeaveCriticalSection(&lag_lock);
	return 1;
}

// QA (2026-11-21): the real internet between two test copies on one PC, through a relay on a server (tools/qa_relay.py). MW2COOP_QARELAY=<ip>:<port>: every game packet to a
// private (LAN) address and a game port (28900-29099) goes to the relay as "MWR1" + destination ip (4) + port (2) + the packet; the relay sends it to the copy that registered
// that port, as "MWR1" + the sender's ip and port + the packet, and the receive below gives the game the packet as from that address. Each copy registers its game sockets
// ("MWRH" + its port) every second from those sockets, so only outgoing packets leave this PC: the router lets the answers in, nothing to forward on it.
static int relay_on;
static struct sockaddr_in relay_addr;
static SOCKET relay_sockets[4];
static uint16_t relay_ports[4];
static volatile LONG relay_out, relay_in, relay_acks;
static int private_ipv4(const uint8_t *a)
{
	return a[0] == 10 || (a[0] == 172 && (a[1] & 0xf0) == 16) || (a[0] == 192 && a[1] == 168);
}
static void relay_note_socket(SOCKET s)
{
	for (int i = 0; i < 4; i++) {
		if (relay_sockets[i] == s)
			return;
		if (!relay_sockets[i]) {
			struct sockaddr_in me;
			int ml = sizeof(me);
			if (getsockname(s, (struct sockaddr *)&me, &ml) != 0 || me.sin_family != AF_INET)
				return;
			relay_ports[i] = ntohs(me.sin_port);
			relay_sockets[i] = s;
			log_printf("net: QA relay: game socket on port %u registers at the relay", (unsigned)relay_ports[i]);
			return;
		}
	}
}
static DWORD WINAPI relay_thread(LPVOID unused)
{
	(void)unused;
	DWORD said = GetTickCount();
	for (;;) {
		Sleep(1000);
		for (int i = 0; i < 4; i++)
			if (relay_sockets[i]) {
				char hello[6] = {'M', 'W', 'R', 'H', (char)(relay_ports[i] >> 8), (char)(relay_ports[i] & 0xff)};
				real_sendto(relay_sockets[i], hello, sizeof(hello), 0, (const struct sockaddr *)&relay_addr, sizeof(relay_addr));
			}
		if (GetTickCount() - said >= 10000) {
			said = GetTickCount();
			log_printf("net: QA relay: %ld packets out, %ld in, %ld answers to the registrations, in the last 10 s", (long)InterlockedExchange(&relay_out, 0), (long)InterlockedExchange(&relay_in, 0),
				   (long)InterlockedExchange(&relay_acks, 0));
		}
	}
	return 0;
}
static void relay_install(void)
{
	char v[64];
	const DWORD n = GetEnvironmentVariableA("MW2COOP_QARELAY", v, sizeof(v));
	if (!n || n >= sizeof(v))
		return;
	char *colon = strchr(v, ':');
	unsigned a[4];
	if (!colon || sscanf(v, "%u.%u.%u.%u", &a[0], &a[1], &a[2], &a[3]) != 4 || a[0] > 255 || a[1] > 255 || a[2] > 255 || a[3] > 255)
		return;
	memset(&relay_addr, 0, sizeof(relay_addr));
	relay_addr.sin_family = AF_INET;
	relay_addr.sin_port = htons((u_short)atoi(colon + 1));
	uint8_t *ip = (uint8_t *)&relay_addr.sin_addr;
	ip[0] = (uint8_t)a[0], ip[1] = (uint8_t)a[1], ip[2] = (uint8_t)a[2], ip[3] = (uint8_t)a[3];
	relay_on = 1;
	CreateThread(NULL, 0, relay_thread, NULL, 0, NULL);
	log_printf("net: QA relay: game packets to LAN addresses go through the relay at port %u", (unsigned)ntohs(relay_addr.sin_port));
}
static int relay_send(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to) // 1: sent through the relay
{
	if (!relay_on || !to || to->sa_family != AF_INET || len <= 0 || len > 1490)
		return 0;
	const struct sockaddr_in *in = (const struct sockaddr_in *)to;
	const uint16_t port = ntohs(in->sin_port);
	if (!private_ipv4((const uint8_t *)&in->sin_addr) || port < 28900 || port > 29099)
		return 0;
	relay_note_socket(s);
	char pkt[1500];
	memcpy(pkt, "MWR1", 4);
	memcpy(pkt + 4, &in->sin_addr, 4);
	pkt[8] = (char)(port >> 8), pkt[9] = (char)(port & 0xff);
	memcpy(pkt + 10, buf, (size_t)len);
	InterlockedIncrement(&relay_out);
	if (lag_on && lag_send(s, pkt, len + 10, flags, (const struct sockaddr *)&relay_addr, sizeof(relay_addr)))
		return 1;
	real_sendto(s, pkt, len + 10, flags, (const struct sockaddr *)&relay_addr, sizeof(relay_addr));
	return 1;
}
static int relay_receive(char *buf, int r, struct sockaddr *from, int *fromlen) // the packet's length for the game: unwrapped when it came through the relay; 0 a relay's own packet
{
	if (!relay_on || r < 6 || !from || from->sa_family != AF_INET)
		return r;
	struct sockaddr_in *in = (struct sockaddr_in *)from;
	if (in->sin_addr.s_addr != relay_addr.sin_addr.s_addr || in->sin_port != relay_addr.sin_port)
		return r;
	if (memcmp(buf, "MWRA", 4) == 0) { // the relay's answer to a registration: the way in works
		InterlockedIncrement(&relay_acks);
		return 0;
	}
	if (r < 10 || memcmp(buf, "MWR1", 4) != 0)
		return r;
	memcpy(&in->sin_addr, buf + 4, 4);
	in->sin_port = htons((u_short)(((uint8_t)buf[8] << 8) | (uint8_t)buf[9]));
	if (fromlen)
		*fromlen = sizeof(struct sockaddr_in);
	memmove(buf, buf + 10, (size_t)(r - 10));
	InterlockedIncrement(&relay_in);
	return r - 10;
}

// every send of the game and of mw2coop: a Steam peer's virtual address goes through Steam
static int send_to(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen)
{
	if (steamnet_send(buf, len, to))
		return len;
	if (relay_on && relay_send(s, buf, len, flags, to))
		return len;
	if (lag_on && lag_send(s, buf, len, flags, to, tolen))
		return len;
	return real_sendto(s, buf, len, flags, to, tolen);
}

static void hold_push(const struct sockaddr *from, int fromlen, const char *data, int len);

// rejoin.c: the engine's coopDis, either way ("\xff\xff\xff\xff" "R" <2 bytes> "coopDis\0" <reason> "\0"); the reason is a word (COOP_CLIENTDROP, ..._END_GAME) or,
// from a client that timed out, the error's text in the game's language
static void note_coopdis(int sent, const char *buf, int len)
{
	static const char word[] = "coopDis"; // (compared with its 0)
	if (len < 16 || *(const unsigned int *)buf != 0xffffffffu)
		return;
	for (int i = 4; i < 12 && i + (int)sizeof(word) < len; i++)
		if (memcmp(buf + i, word, sizeof(word)) == 0) {
			char reason[48];
			int k = len - i - (int)sizeof(word);
			if (k > (int)sizeof(reason) - 1)
				k = (int)sizeof(reason) - 1;
			memcpy(reason, buf + i + sizeof(word), k);
			reason[k] = 0;
			rejoin_on_coopdis(sent, reason);
			return;
		}
}

static void remember_peer(SOCKET s, const struct sockaddr *sa)
{
	if (!sa || sa->sa_family != AF_INET)
		return;
	EnterCriticalSection(&peer_lock);
	if (peer_socket != s || memcmp(&peer, sa, sizeof(peer)) != 0) {
		peer_socket = s;
		memcpy(&peer, sa, sizeof(peer));
		const unsigned char *ip = (const unsigned char *)&peer.sin_addr;
		log_printf("net: co-op partner %u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], ntohs(peer.sin_port));
	}
	LeaveCriticalSection(&peer_lock);
}

void net_log_budget(int packets)
{
	InterlockedExchange(&budget, packets);
	log_printf("net: logging the next %d packets", packets);
}

int net_send_mod_to(const uint8_t ip[4], uint16_t port, const char *text)
{
	SOCKET s = game_socket;
	char pkt[256];
	int n = snprintf(pkt, sizeof(pkt), "\xff\xff\xff\xff%s", text);
	if (s == INVALID_SOCKET || !real_sendto || n <= 0 || n >= (int)sizeof(pkt))
		return 0;
	struct sockaddr_in to = {0};
	to.sin_family = AF_INET;
	to.sin_port = htons(port);
	memcpy(&to.sin_addr, ip, 4);
	return send_to(s, pkt, n, 0, (struct sockaddr *)&to, sizeof(to)) == n;
}

int net_send_mod(const char *text)
{
	char pkt[256];
	int n = snprintf(pkt, sizeof(pkt), "\xff\xff\xff\xff%s", text);
	if (n <= 0 || n >= (int)sizeof(pkt) || !real_sendto)
		return 0;
	EnterCriticalSection(&peer_lock);
	// the game's own socket as it is now (the one the game sent its last out-of-band packet from) is the one its receiver listens with; the one noted at the join may be gone
	SOCKET use = game_socket != INVALID_SOCKET ? game_socket : peer_socket;
	int sent_n = use != INVALID_SOCKET && peer_socket != INVALID_SOCKET ? send_to(use, pkt, n, 0, (const struct sockaddr *)&peer, sizeof(peer)) : -2;
	int ok = sent_n == n;
	LeaveCriticalSection(&peer_lock);
	return ok;
}

// a packet of the mod's that is not text (its "\xff\xff\xff\xff" included) or longer than net_send_mod takes: the partner's voice (voice.c), the pieces of his log
// for a bug report (report.c); up to 1400 bytes
int net_send_mod_bin(const void *data, int len)
{
	if (len <= 4 || len > 1400 || !real_sendto)
		return 0;
	EnterCriticalSection(&peer_lock);
	SOCKET use = game_socket != INVALID_SOCKET ? game_socket : peer_socket;
	int sent_n = use != INVALID_SOCKET && peer_socket != INVALID_SOCKET ? send_to(use, (const char *)data, len, 0, (const struct sockaddr *)&peer, sizeof(peer)) : -2;
	LeaveCriticalSection(&peer_lock);
	return sent_n == len;
}

// Every IPv4 network the PC is on (LAN, Wi-Fi, Radmin VPN, Hamachi...) gets the packet at its
// broadcast address, plus 255.255.255.255; port in host order.
int net_broadcast_mod(const char *text, uint16_t port)
{
	SOCKET s = game_socket;
	if (s == INVALID_SOCKET || !real_sendto)
		return 0;
	char pkt[256];
	int n = snprintf(pkt, sizeof(pkt), "\xff\xff\xff\xff%s", text);
	if (n <= 0 || n >= (int)sizeof(pkt))
		return 0;
	BOOL on = TRUE;
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof(on));
	struct sockaddr_in to = {0};
	to.sin_family = AF_INET;
	to.sin_port = htons(port);
	to.sin_addr.s_addr = INADDR_BROADCAST;
	int sent = real_sendto(s, pkt, n, 0, (struct sockaddr *)&to, sizeof(to)) == n;

	ULONG size = 16384;
	IP_ADAPTER_ADDRESSES *list = HeapAlloc(GetProcessHeap(), 0, size);
	if (list && GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
					 NULL, list, &size) == NO_ERROR) {
		for (IP_ADAPTER_ADDRESSES *a = list; a; a = a->Next) {
			if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
				continue;
			for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
				if (u->Address.lpSockaddr->sa_family != AF_INET || u->OnLinkPrefixLength >= 32)
					continue;
				ULONG ip = ntohl(((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr);
				ULONG mask = u->OnLinkPrefixLength ? 0xffffffffu << (32 - u->OnLinkPrefixLength) : 0;
				to.sin_addr.s_addr = htonl(ip | ~mask);
				sent += real_sendto(s, pkt, n, 0, (struct sockaddr *)&to, sizeof(to)) == n;
			}
		}
	}
	if (list)
		HeapFree(GetProcessHeap(), 0, list);
	return sent;
}

// mw2coop's start packet (the host's session dvars and difficulty) and ready packet (the partner's id and
// readiness) mean something only from the other player of this session. Anybody who knew the port could send
// them, so they are acted on only when they come from the address the game talks to (remember_peer; by IP: the
// port a NAT gives may differ). The lobby's own packets (access, list of games) are answered as before.
static volatile LONG ignored_packets;

static const char desync_prefix[] = "\xff\xff\xff\xffmw2coop_desync "; // netwatch.c: the partner's PC found the games apart
static const char nw_prefix[] = "\xff\xff\xff\xffmw2coop_nw ";	  // netwatch.c: the rest of its messages
static const char resync_prefix[] = "\xff\xff\xff\xffmw2coop_resync "; // saves.c: the host loads the common checkpoint
static const char nick_prefix[] = "\xff\xff\xff\xffmw2coop_nick ";	      // nick.c: the partner's name in the lobby
static int is_session_packet(const char *buf, int len)
{
	if ((len >= (int)sizeof(desync_prefix) - 1 && memcmp(buf, desync_prefix, sizeof(desync_prefix) - 1) == 0) ||
	    (len >= (int)sizeof(nw_prefix) - 1 && memcmp(buf, nw_prefix, sizeof(nw_prefix) - 1) == 0) ||
	    (len >= (int)sizeof(nick_prefix) - 1 && memcmp(buf, nick_prefix, sizeof(nick_prefix) - 1) == 0) ||
	    (len >= (int)sizeof(resync_prefix) - 1 && memcmp(buf, resync_prefix, sizeof(resync_prefix) - 1) == 0))
		return 1;
	static const char report[] = "\xff\xff\xff\xffmw2coop_report_"; // report.c: the partner's log asked for and sent
	if (len >= (int)sizeof(report) - 1 && memcmp(buf, report, sizeof(report) - 1) == 0)
		return 1;
	static const char start[] = "\xff\xff\xff\xffmw2coop_start";
	static const char ready[] = "\xff\xff\xff\xffmw2coop_ready";
	static const char brief[] = "\xff\xff\xff\xffmw2coop_briefdone";
	static const char cp[] = "\xff\xff\xff\xffmw2coop_cp ", retry[] = "\xff\xff\xff\xffmw2coop_retry "; // saves.c: the checkpoint each PC holds
	return (len >= (int)sizeof(start) - 1 && memcmp(buf, start, sizeof(start) - 1) == 0) ||
	       (len >= (int)sizeof(ready) - 1 && memcmp(buf, ready, sizeof(ready) - 1) == 0) ||
	       (len >= (int)sizeof(brief) - 1 && memcmp(buf, brief, sizeof(brief) - 1) == 0) ||
	       (len >= (int)sizeof(cp) - 1 && memcmp(buf, cp, sizeof(cp) - 1) == 0) ||
	       (len >= (int)sizeof(retry) - 1 && memcmp(buf, retry, sizeof(retry) - 1) == 0);
}

static int from_partner(const struct sockaddr *from)
{
	if (!from || from->sa_family != AF_INET)
		return 0;
	EnterCriticalSection(&peer_lock);
	int same = peer_socket != INVALID_SOCKET &&
		   ((const struct sockaddr_in *)from)->sin_addr.s_addr == peer.sin_addr.s_addr;
	LeaveCriticalSection(&peer_lock);
	return same;
}

static void hs_sent(const char *buf, int len, const struct sockaddr *to);

static int WINAPI my_sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen)
{
	if (len > 4 && *(const unsigned int *)buf == 0xffffffffu)
		game_socket = s;
	if (len > 4 && *(const unsigned int *)buf == 0xffffffffu && !game_peek(RVA_PARTY_IS_HOST, 1) &&
	    contains(buf, len, "coopConnect"))
		remember_peer(s, to);
	// The host's partner is the one its game accepts (audit 2026-11-18, NET-A: a later connect let through by access.c, a Steam friend pressing Join during a
	// mission or one forged packet in open mode, made its sender the partner, and the real partner's commands were then dropped as a stranger's).
	if (len > 4 && *(const unsigned int *)buf == 0xffffffffu && game_peek(RVA_PARTY_IS_HOST, 1) && contains_word(buf, len, "coopAccept"))
		remember_peer(s, to);
	// The party resends coopLaunch until the client acks it; the start packet goes along each time.
	if (len > 4 && *(const unsigned int *)buf == 0xffffffffu && game_peek(RVA_PARTY_IS_HOST, 1) &&
	    contains(buf, len, "coopLaunch")) {
		char pkt[512];
		int n = progress_launch_packet(pkt, sizeof(pkt));
		log_packet("send", to, pkt, n);
		send_to(s, pkt, n, flags, to, tolen);
	}
	if (netwatch_send_paused() && to && to->sa_family == AF_INET && from_partner(to))
		return len; // QA (mw2coop_netpause): as if sent
	note_coopdis(1, buf, len);
	// rejoin.c: the partner's game says goodbye to its host after a break ("R" <sequence, 2 bytes> "coopDis" ..., resent every 300 ms until the host answers
	// "RA <sequence>"). A host whose game crashed never answers: the goodbye went on about 2.5 minutes, and no join of the partner's got through meanwhile (QA
	// 2026-11-23: the host's game killed and started again, the partner back only at his 21st attempt). Once the break is known here, the mod answers it in
	// the host's place; a host that is still there answers too, and the second answer is one the game has no use for.
	if (len > 8 && to && to->sa_family == AF_INET && *(const unsigned int *)buf == 0xffffffffu && buf[4] == 'R' && !memcmp(buf + 7, "coopDis", 8) &&
	    rejoin_state() == REJOIN_CLIENT_JOIN && from_partner(to)) {
		char ack[24];
		const unsigned seq = (unsigned)(uint8_t)buf[5] << 8 | (uint8_t)buf[6];
		const int n = snprintf(ack, sizeof(ack), "\xff\xff\xff\xffRA %u", seq) + 1; // (with its 0, as the game sends it)
		hold_push(to, tolen < (int)sizeof(struct sockaddr_storage) ? tolen : (int)sizeof(struct sockaddr_storage), ack, n);
		static volatile LONG told;
		if (!InterlockedExchange(&told, 1))
			log_printf("net: the partner's goodbye to the host after the break is answered in the host's place (RA %u)", seq);
	}
	log_packet("send", to, buf, len);
	hs_sent(buf, len, to);
	return send_to(s, buf, len, flags, to, tolen);
}


// While a briefing waits (menus.c) the engine's server frame does not read its socket, so a partner's "my briefing is over" would stay in the socket until the wait is over. A thread reads
// the socket meanwhile (recv_filtered: our packets are acted on there) and keeps what is the game's in a queue, which the game's own reads empty first: nothing is lost. Packets of the
// connection (not out of band) are kept only the last 64: the netchan resends what matters.
typedef struct {
	int len, fromlen;
	struct sockaddr_storage from;
	char data[1500];
} held_packet_t;
#define HOLD_Q 128
static held_packet_t hold_q[HOLD_Q];
static int hold_head, hold_count;
static CRITICAL_SECTION hold_lock;

static int recv_filtered(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen);

// Diagnostics of a briefing hold (the iw4x overflow hunt, 2026-11-24): the partner's game packets (not out of band) kept and dropped while the briefing waits, this
// PC's game packets to the partner meanwhile, and the sizes of the game packets read from and sent to the partner in the 2 s after the hold.
static struct {
	LONG kept, dropped, kept_bytes, kept_max, sent, sent_bytes, sent_max;
	DWORD released;
	char read_after[400], sent_after[400];
} hs;
static CRITICAL_SECTION hs_lock;

static void hs_add(char *list, int len)
{
	size_t n = strlen(list);
	if (n < 380)
		snprintf(list + n, 400 - n, "%s%d", n ? " " : "", len);
}

static void hs_sent(const char *buf, int len, const struct sockaddr *to)
{
	if (len < 4 || *(const unsigned int *)buf == 0xffffffffu || !from_partner(to))
		return;
	EnterCriticalSection(&hs_lock);
	if (menus_briefing_holds()) {
		hs.sent++;
		hs.sent_bytes += len;
		if (len > hs.sent_max)
			hs.sent_max = len;
	} else if (hs.released && GetTickCount() - hs.released < 2000)
		hs_add(hs.sent_after, len);
	LeaveCriticalSection(&hs_lock);
}

static void hs_read(const char *buf, int len, const struct sockaddr *from)
{
	if (len < 4 || *(const unsigned int *)buf == 0xffffffffu || !from_partner(from))
		return;
	EnterCriticalSection(&hs_lock);
	if (hs.released && GetTickCount() - hs.released < 2000)
		hs_add(hs.read_after, len);
	LeaveCriticalSection(&hs_lock);
}

// QA: this PC's reliable command ring (the client's commands to its server, resent in every packet until the server has run them): how many wait and the newest texts
static void hs_ring_log(DWORD now)
{
	static DWORD last;
	static int last_pending = -1;
	uint8_t *const base = (uint8_t *)GetModuleHandleA(NULL); // (the game image, as menus.c takes it)
	if (now - last < 1000 || !base)
		return;
	last = now;
#ifdef _WIN64
	const uint8_t *ring = base + XR(0x545844); // (seq, acknowledged, 128 texts of 0x40 bytes)
#else
	const uint8_t *ring = base + XR(0x48c330) + 4;
#endif
	const int seq = *(volatile const int *)ring, ack = *(volatile const int *)(ring + 4), pending = seq - ack;
	if (pending == last_pending || (pending < 4 && last_pending < 4)) {
		last_pending = pending;
		return;
	}
	last_pending = pending;
	char t[3][48] = {"", "", ""};
	for (int i = 0; i < 3 && i < pending; i++) {
		const char *s = (const char *)ring + 8 + ((seq - i) & 127) * 0x40;
		int k = 0;
		for (; k < 47 && s[k]; k++)
			t[i][k] = s[k] >= 0x20 && s[k] < 0x7f ? s[k] : '?';
		t[i][k] = 0;
	}
	log_printf("net: reliable commands waiting for the server: %d (sequence %d), newest '%s' '%s' '%s'", pending, seq, t[0], t[1], t[2]);
}

static void hs_tick(int holding)
{
	static int was;
	DWORD now = GetTickCount();
	hs_ring_log(now);
	EnterCriticalSection(&hs_lock);
	if (was && !holding) {
		log_printf("net: briefing hold over: partner's game packets kept %ld (%ld bytes, largest %ld), dropped %ld; this PC's game packets to the partner %ld (%ld bytes, largest %ld)",
			   hs.kept, hs.kept_bytes, hs.kept_max, hs.dropped, hs.sent, hs.sent_bytes, hs.sent_max);
		hs.released = now ? now : 1;
		hs.read_after[0] = hs.sent_after[0] = 0;
	}
	if (holding && !was)
		hs.kept = hs.dropped = hs.kept_bytes = hs.kept_max = hs.sent = hs.sent_bytes = hs.sent_max = 0;
	if (hs.released && now - hs.released >= 2000) {
		log_printf("net: game packets from the partner read in the 2 s after the hold: %s", hs.read_after[0] ? hs.read_after : "none");
		log_printf("net: game packets to the partner sent in the 2 s after the hold: %s", hs.sent_after[0] ? hs.sent_after : "none");
		hs.released = 0;
	}
	was = holding;
	LeaveCriticalSection(&hs_lock);
}

// a packet the game reads next as if it had come from <from> (my_sendto's answer in the host's place)
static void hold_push(const struct sockaddr *from, int fromlen, const char *data, int len)
{
	if (len <= 0 || len > 1500 || fromlen <= 0)
		return;
	EnterCriticalSection(&hold_lock);
	if (hold_count == HOLD_Q) {
		hold_head = (hold_head + 1) % HOLD_Q;
		hold_count--;
	}
	held_packet_t *h = &hold_q[(hold_head + hold_count) % HOLD_Q];
	h->len = len;
	h->fromlen = fromlen;
	memcpy(&h->from, from, fromlen);
	memcpy(h->data, data, len);
	hold_count++;
	LeaveCriticalSection(&hold_lock);
}

// The game's UDP socket, learnt from its own polling: the game reads its socket every frame (NET_GetPacket), with or without a packet waiting. Until then
// game_socket was only learnt from a 0xffffffff packet sent or received, and a PC that had neither (the 32-bit game in its menus: no Steam traffic before a join)
// sent nothing when its Join button searched the networks (net_broadcast_mod returned 0; issue 30's check, 2026-11-26).
static void learn_polled_socket(SOCKET s)
{
	struct sockaddr_in own;
	int own_len = sizeof(own), type = 0, type_len = sizeof(type);
	if (getsockname(s, (struct sockaddr *)&own, &own_len) == 0 && own.sin_family == AF_INET &&
	    getsockopt(s, SOL_SOCKET, SO_TYPE, (char *)&type, &type_len) == 0 && type == SOCK_DGRAM &&
	    InterlockedCompareExchangePointer((PVOID volatile *)&game_socket, (PVOID)s, (PVOID)INVALID_SOCKET) == (PVOID)INVALID_SOCKET)
		log_printf("net: the game's socket (port %u) learnt from its polling", (unsigned)ntohs(own.sin_port));
}

static int WINAPI my_recvfrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen)
{
	if (game_socket == INVALID_SOCKET)
		learn_polled_socket(s);
	if (hold_count && !(flags & MSG_PEEK)) {
		EnterCriticalSection(&hold_lock);
		if (hold_count) {
			held_packet_t *h = &hold_q[hold_head];
			int n = h->len < len ? h->len : len;
			memcpy(buf, h->data, n);
			if (from && fromlen && *fromlen >= h->fromlen) {
				memcpy(from, &h->from, h->fromlen);
				*fromlen = h->fromlen;
			}
			hold_head = (hold_head + 1) % HOLD_Q;
			hold_count--;
			LeaveCriticalSection(&hold_lock);
			hs_read(buf, n, from);
			return n;
		}
		LeaveCriticalSection(&hold_lock);
	}
	int r = recv_filtered(s, buf, len, flags, from, fromlen);
	if (r > 0 && !(flags & MSG_PEEK))
		hs_read(buf, r, from);
	return r;
}

static DWORD WINAPI hold_drain_thread(LPVOID unused)
{
	(void)unused;
	for (;;) {
		Sleep(5);
		const int holding = menus_briefing_holds();
		hs_tick(holding);
		if (!holding)
			continue;
		EnterCriticalSection(&peer_lock);
		SOCKET s = peer_socket;
		LeaveCriticalSection(&peer_lock);
		if (s == INVALID_SOCKET)
			continue;
		for (int i = 0; i < 64; i++) {
			char buf[1500];
			struct sockaddr_storage from;
			int fl = sizeof(from);
			int r = recv_filtered(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
			if (r <= 0)
				break;
			EnterCriticalSection(&hold_lock);
			int oob = r >= 4 && *(const unsigned int *)buf == 0xffffffffu;
			if (hold_count == HOLD_Q || (!oob && hold_count >= 64)) { // drop the oldest
				hold_head = (hold_head + 1) % HOLD_Q;
				hold_count--;
				if (!oob)
					InterlockedIncrement(&hs.dropped);
			}
			if (!oob) {
				InterlockedIncrement(&hs.kept);
				InterlockedExchangeAdd(&hs.kept_bytes, r);
				if (r > hs.kept_max)
					hs.kept_max = r;
			}
			held_packet_t *h = &hold_q[(hold_head + hold_count) % HOLD_Q];
			h->len = r;
			h->fromlen = fl;
			memcpy(&h->from, &from, fl);
			memcpy(h->data, buf, r);
			hold_count++;
			LeaveCriticalSection(&hold_lock);
		}
	}
	return 0;
}

static int recv_filtered(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen)
{
	int r = steamnet_receive(buf, len, from, fromlen);
	if (r <= 0)
		r = real_recvfrom(s, buf, len, flags, from, fromlen);
	if (relay_on) {
		relay_note_socket(s); // (the game polls its sockets every frame: they register at the relay before anything is sent or received)
		if (r > 0 && (r = relay_receive(buf, r, from, fromlen)) == 0) {
			WSASetLastError(WSAEWOULDBLOCK); // (the relay's own answer: nothing for the game)
			return SOCKET_ERROR;
		}
	}
	{ // rejoin.c: after a break the host that waits and the partner who looks for it send each other this, so that their routers let the other's packets in; the partner joins once he hears it
		static const char hello[] = "\xff\xff\xff\xffmw2coop_rejoin_hello";
		if (r >= (int)sizeof(hello) - 1 && memcmp(buf, hello, sizeof(hello) - 1) == 0) {
			rejoin_note_hello(); // (the partner joins only once he hears the host: a join that waits in the game's queue for a host that is gone spoils the next ones on the 32-bit game)
			WSASetLastError(WSAEWOULDBLOCK);
			return SOCKET_ERROR;
		}
	}
	if (r > 0 && settings_netguard()) {
		// packets no co-op session sends and the game would act on (netguard.c): the game never sees them
		EnterCriticalSection(&peer_lock);
		int known = peer_socket != INVALID_SOCKET;
		LeaveCriticalSection(&peer_lock);
		const char *why = netguard_check(buf, r, known, from_partner(from));
		if (why) {
			static volatile LONG dropped;
			if (InterlockedIncrement(&dropped) <= 20)
				log_packet("dropped", from, buf, r);
			if (dropped <= 20)
				log_printf("net: guard dropped a packet: %s", why);
			WSASetLastError(WSAEWOULDBLOCK);
			return SOCKET_ERROR;
		}
	}
	if (r > 0) {
		log_packet("recv", from, buf, r);
		if (from_partner(from)) {
			static const char chat[] = "\xff\xff\xff\xffmw2coop_t";
			if (r > (int)sizeof(chat) - 1 && memcmp(buf, chat, sizeof(chat) - 1) == 0) { // (chat.c: a chat line)
				chat_on_packet(buf, r);
				WSASetLastError(WSAEWOULDBLOCK);
				return SOCKET_ERROR;
			}
			static const char voice[] = "\xff\xff\xff\xffmw2coop_v";
			if (r > (int)sizeof(voice) - 1 && memcmp(buf, voice, sizeof(voice) - 1) == 0) { // (voice.c: his voice comes from a thread of its own, so it is not a sign of his game's life)
				voice_on_packet(buf, r);
				WSASetLastError(WSAEWOULDBLOCK);
				return SOCKET_ERROR;
			}
			netwatch_partner_packet(); // any packet of his: the lockstep frames, his usercmds, the mod's own
			note_coopdis(0, buf, r);
		}
		// The partner is learnt from his connect only once access.c has let it through (night audit 2026-11-17, NET-1: any packet with the text "coopConnect" anywhere, a
		// refused connect included, made its sender the partner, and the real partner's start / ready packets were then ignored as a stranger's).
		const int connect = game_peek(RVA_PARTY_IS_HOST, 1) && is_coop_connect(buf, r);
		char reply[128];
		int reply_len = 0, lobby = 0;
		if (r > 4 && *(const unsigned int *)buf == 0xffffffffu && from && from->sa_family == AF_INET) {
			const struct sockaddr_in *in = (const struct sockaddr_in *)from;
			game_socket = s;
			lobby = access_on_packet(buf, r, (const uint8_t *)&in->sin_addr, ntohs(in->sin_port), reply,
						 sizeof(reply), &reply_len) ||
				menus_on_lobby_packet(buf, r, (const uint8_t *)&in->sin_addr, ntohs(in->sin_port), reply,
						      sizeof(reply), &reply_len);
			if (reply_len > 0 && reply_len < (int)sizeof(reply))
				send_to(s, reply, reply_len, 0, from, sizeof(struct sockaddr_in));
		}
		// (a refused connect is answered by access.c and consumed: lobby != 0). Once a partner is known, only the host's own coopAccept changes it (my_sendto).
		if (connect && !lobby) {
			EnterCriticalSection(&peer_lock);
			const int known = peer_socket != INVALID_SOCKET;
			LeaveCriticalSection(&peer_lock);
			if (!known)
				remember_peer(s, from);
		}
		int ours = lobby;
		if (!ours && is_session_packet(buf, r)) {
			// consumed either way: the game has nothing to do with it
			if (from_partner(from)) {
				if (r >= (int)sizeof(desync_prefix) - 1 && memcmp(buf, desync_prefix, sizeof(desync_prefix) - 1) == 0) {
					char t[40] = "", *e = NULL; // "<level time> <server id>" (an older build: the time only)
					const int k = r - (int)(sizeof(desync_prefix) - 1);
					memcpy(t, buf + sizeof(desync_prefix) - 1, k < 39 ? k : 39);
					const long time = strtol(t, &e, 10);
					netwatch_on_desync_packet((int)time, e ? (int)strtol(e, NULL, 10) : 0);
				} else if (r >= (int)sizeof(nw_prefix) - 1 && memcmp(buf, nw_prefix, sizeof(nw_prefix) - 1) == 0) {
					char t[32] = "";
					const int k = r - (int)(sizeof(nw_prefix) - 1);
					memcpy(t, buf + sizeof(nw_prefix) - 1, k < 31 ? k : 31);
					netwatch_on_packet(t);
				} else if (r >= (int)sizeof(nick_prefix) - 1 && memcmp(buf, nick_prefix, sizeof(nick_prefix) - 1) == 0) {
					char t[64] = "";
					const int k = r - (int)(sizeof(nick_prefix) - 1);
					memcpy(t, buf + sizeof(nick_prefix) - 1, k < 63 ? k : 63);
					nick_on_packet(t);
				} else if (!report_on_packet(buf, r) && !progress_on_packet(buf, r) && !saves_on_packet(buf, r))
					menus_on_packet(buf, r);
			} else if (InterlockedIncrement(&ignored_packets) <= 5)
				log_printf("net: a start or ready packet from an address that is not the partner's was ignored");
			ours = 1;
		}
		if (ours) {
			WSASetLastError(WSAEWOULDBLOCK); // ours: the game sees "no packet this time"
			return SOCKET_ERROR;
		}
	}
	return r;
}

// the game's imports are patched in its own image: 64- or 32-bit PE
#ifdef _WIN64
typedef IMAGE_NT_HEADERS64 NT_HEADERS;
typedef IMAGE_THUNK_DATA64 THUNK_DATA;
#define SNAP_BY_ORDINAL IMAGE_SNAP_BY_ORDINAL64
#define ORDINAL_OF IMAGE_ORDINAL64
#else
typedef IMAGE_NT_HEADERS32 NT_HEADERS;
typedef IMAGE_THUNK_DATA32 THUNK_DATA;
#define SNAP_BY_ORDINAL IMAGE_SNAP_BY_ORDINAL32
#define ORDINAL_OF IMAGE_ORDINAL32
#endif

int net_partner_known(void)
{
	EnterCriticalSection(&peer_lock);
	const int known = peer_socket != INVALID_SOCKET;
	LeaveCriticalSection(&peer_lock);
	return known;
}

int net_partner_address(uint8_t ip[4], uint16_t *port)
{
	EnterCriticalSection(&peer_lock);
	const int known = peer_socket != INVALID_SOCKET;
	if (known) {
		memcpy(ip, &peer.sin_addr, 4);
		*port = ntohs(peer.sin_port);
	}
	LeaveCriticalSection(&peer_lock);
	return known;
}

void net_install(void)
{
	InitializeCriticalSection(&peer_lock);
	InitializeCriticalSection(&hold_lock);
	InitializeCriticalSection(&hs_lock);
	CreateThread(NULL, 0, hold_drain_thread, NULL, 0, NULL);
	lag_install(); // (QA: MW2COOP_QALAG and the others; nothing when unset)
	relay_install(); // (QA: MW2COOP_QARELAY; nothing when unset)
	BYTE *base = (BYTE *)GetModuleHandleA(NULL);
	NT_HEADERS *nt = (NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
		if (lstrcmpiA((const char *)(base + d->Name), "WSOCK32.dll") != 0)
			continue;
		THUNK_DATA *names = (THUNK_DATA *)(base + d->OriginalFirstThunk);
		THUNK_DATA *iat = (THUNK_DATA *)(base + d->FirstThunk);
		for (; names->u1.Ordinal; names++, iat++) {
			if (!SNAP_BY_ORDINAL(names->u1.Ordinal))
				continue;
			WORD ord = (WORD)ORDINAL_OF(names->u1.Ordinal);
			void *wrapper = NULL;
			if (ord == 20) {
				real_sendto = (sendto_t)iat->u1.Function;
				wrapper = (void *)my_sendto;
			} else if (ord == 17) {
				real_recvfrom = (recvfrom_t)iat->u1.Function;
				wrapper = (void *)my_recvfrom;
			}
			if (!wrapper)
				continue;
			DWORD old;
			VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
			iat->u1.Function = (ULONG_PTR)wrapper;
			VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
			log_printf("net: hooked WSOCK32 #%u", ord);
		}
	}
}
