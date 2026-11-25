// UPnP port forwarding for the host: while this PC hosts a co-op lobby or game, the router (an
// Internet Gateway Device) is asked to forward the game's UDP port to it, so a partner on the
// internet can reach it without a manual port forward. Removed when hosting ends; the forward is
// leased for an hour and renewed, so a crashed game leaves nothing behind for long.
//
// Plain UPnP over winsock on its own thread (it may take a few seconds):
//   1. SSDP M-SEARCH to 239.255.255.250:1900 (from the address of the internet route) and to
//      the router on that route, for an InternetGatewayDevice -> LOCATION url
//   2. GET the device description -> controlURL of WANIPConnection or WANPPPConnection
//   3. SOAP AddPortMapping (UDP <port> -> this PC's address on the router's network)
//   4. SOAP GetExternalIPAddress: a private address there means the provider's own NAT (CGNAT),
//      which no port forward on the home router can get through.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "log.h"
#include "settings.h"
#include "upnp.h"

#define DESCRIPTION "Call of Duty MW2 co-op (mw2coop)"

static volatile LONG wanted;  // the menu tick: 1 while hosting
static volatile LONG mapped;  // the worker: 1 while a mapping is in place
static volatile DWORD unwanted_at; // the menu tick: when hosting last stopped
static HANDLE wake;
static char control_host[64], control_path[256], service_type[96];
static uint16_t control_port;
static char local_ip[16];

// ---------------------------------------------------------------- tiny HTTP

static int http_request(const char *host, uint16_t port, const char *request, char *reply, int reply_size)
{
	if (reply_size > 0)
		reply[0] = 0; // a failed request leaves an empty reply, never the caller's uninitialised buffer (audit 2026-11-18)
	struct addrinfo hints = {0}, *res = NULL;
	char port_text[8];
	snprintf(port_text, sizeof(port_text), "%u", port);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port_text, &hints, &res) != 0 || !res)
		return -1;
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	DWORD timeout = 3000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
	int n = -1;
	if (connect(s, res->ai_addr, (int)res->ai_addrlen) == 0) {
		// the local address the router sees us at
		struct sockaddr_in me;
		int len = sizeof(me);
		if (getsockname(s, (struct sockaddr *)&me, &len) == 0)
			inet_ntop(AF_INET, &me.sin_addr, local_ip, sizeof(local_ip));
		if (send(s, request, (int)strlen(request), 0) > 0) {
			n = 0;
			int r;
			while (n < reply_size - 1 && (r = recv(s, reply + n, reply_size - 1 - n, 0)) > 0)
				n += r;
			reply[n] = 0;
		}
	}
	closesocket(s);
	freeaddrinfo(res);
	return n;
}

// "http://host:port/path" -> parts
static int parse_url(const char *url, char *host, int host_size, uint16_t *port, char *path, int path_size)
{
	if (strncmp(url, "http://", 7) != 0)
		return 0;
	const char *h = url + 7, *slash = strchr(h, '/'), *colon = strchr(h, ':');
	if (!slash)
		slash = h + strlen(h);
	if (colon && colon < slash) {
		lstrcpynA(host, h, (int)(colon - h + 1) < host_size ? (int)(colon - h + 1) : host_size);
		*port = (uint16_t)atoi(colon + 1);
	} else {
		lstrcpynA(host, h, (int)(slash - h + 1) < host_size ? (int)(slash - h + 1) : host_size);
		*port = 80;
	}
	lstrcpynA(path, *slash ? slash : "/", path_size);
	return 1;
}

static const char *find_tag(const char *xml, const char *tag, char *out, int size)
{
	char open[64];
	snprintf(open, sizeof(open), "<%s>", tag);
	const char *a = strstr(xml, open);
	if (!a)
		return NULL;
	a += strlen(open);
	const char *b = strstr(a, "</");
	if (!b)
		return NULL;
	int n = (int)(b - a) < size - 1 ? (int)(b - a) : size - 1;
	memcpy(out, a, n);
	out[n] = 0;
	return b;
}

// ---------------------------------------------------------------- discovery

// The address this PC reaches the internet from (a UDP connect sends nothing) and the router on
// that route. With more than one network adapter Windows may send multicast out of another one,
// and the router never hears the search (owner's PC, 2026-10-22: no answer until the search left
// from the LAN address).
static int internet_route(struct in_addr *self, struct in_addr *gateway)
{
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct sockaddr_in to = {0}, me = {0};
	to.sin_family = AF_INET;
	to.sin_port = htons(53);
	inet_pton(AF_INET, "8.8.8.8", &to.sin_addr);
	int len = sizeof(me);
	int ok = connect(s, (struct sockaddr *)&to, sizeof(to)) == 0 && getsockname(s, (struct sockaddr *)&me, &len) == 0;
	closesocket(s);
	if (!ok)
		return 0;
	*self = me.sin_addr;
	MIB_IPFORWARDROW row;
	gateway->s_addr = GetBestRoute(to.sin_addr.s_addr, me.sin_addr.s_addr, &row) == NO_ERROR ? row.dwForwardNextHop : 0;
	return 1;
}

int upnp_own_address(uint8_t out[4])
{
	struct in_addr self = {0}, gateway = {0};
	if (!internet_route(&self, &gateway) || !self.s_addr)
		return 0;
	memcpy(out, &self, 4);
	return 1;
}

static int discover(void)
{
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	DWORD timeout = 2500;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
	struct in_addr self = {0}, gateway = {0};
	char self_text[16] = "?", gateway_text[16] = "?";
	if (internet_route(&self, &gateway)) {
		struct sockaddr_in from = {0};
		from.sin_family = AF_INET;
		from.sin_addr = self;
		bind(s, (struct sockaddr *)&from, sizeof(from));
		setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&self, sizeof(self));
		inet_ntop(AF_INET, &self, self_text, sizeof(self_text));
		inet_ntop(AF_INET, &gateway, gateway_text, sizeof(gateway_text));
	}
	log_printf("upnp: searching from %s (router %s)", self_text, gateway_text);
	struct sockaddr_in to = {0};
	to.sin_family = AF_INET;
	to.sin_port = htons(1900);
	inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
	static const char search[] = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
				     "MX: 2\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
	sendto(s, search, sizeof(search) - 1, 0, (struct sockaddr *)&to, sizeof(to));
	// and to the router itself: routers answer a search sent straight to them as well
	if (gateway.s_addr) {
		to.sin_addr = gateway;
		sendto(s, search, sizeof(search) - 1, 0, (struct sockaddr *)&to, sizeof(to));
	}
	char location[256] = "", buf[1536];
	int n;
	while (!location[0] && (n = recv(s, buf, sizeof(buf) - 1, 0)) > 0) {
		buf[n] = 0;
		for (char *line = buf; line && *line; line = strstr(line, "\r\n") ? strstr(line, "\r\n") + 2 : NULL) {
			if (_strnicmp(line, "LOCATION:", 9) == 0) {
				char *v = line + 9;
				while (*v == ' ')
					v++;
				int len = (int)strcspn(v, "\r\n");
				lstrcpynA(location, v, len + 1 < (int)sizeof(location) ? len + 1 : (int)sizeof(location));
			}
		}
	}
	closesocket(s);
	if (!location[0]) {
		log_printf("upnp: no router answered (UPnP off on the router, or no router)");
		return 0;
	}
	char host[64], path[256];
	uint16_t port;
	if (!parse_url(location, host, sizeof(host), &port, path, sizeof(path)))
		return 0;
	char request[512];
	snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n\r\n", path, host, port);
	static char desc[32768];
	if (http_request(host, port, request, desc, sizeof(desc)) <= 0)
		return 0;
	static const char *const services[] = {"urn:schemas-upnp-org:service:WANIPConnection:1",
					       "urn:schemas-upnp-org:service:WANIPConnection:2",
					       "urn:schemas-upnp-org:service:WANPPPConnection:1"};
	for (size_t i = 0; i < sizeof(services) / sizeof(services[0]); i++) {
		const char *at = strstr(desc, services[i]);
		char control[256];
		if (!at || !find_tag(at, "controlURL", control, sizeof(control)))
			continue;
		lstrcpynA(service_type, services[i], sizeof(service_type));
		lstrcpynA(control_host, host, sizeof(control_host));
		control_port = port;
		if (strncmp(control, "http://", 7) == 0)
			parse_url(control, control_host, sizeof(control_host), &control_port, control_path, sizeof(control_path));
		else
			snprintf(control_path, sizeof(control_path), "%s%s", control[0] == '/' ? "" : "/", control);
		log_printf("upnp: router %s:%u, %s", control_host, control_port, service_type);
		return 1;
	}
	log_printf("upnp: the router offers no WAN connection service");
	return 0;
}

static int soap(const char *action, const char *args, char *reply, int reply_size)
{
	static char body[2048], request[3072];
	snprintf(body, sizeof(body),
		 "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
		 "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s>"
		 "</s:Body></s:Envelope>\r\n",
		 action, service_type, args, action);
	snprintf(request, sizeof(request),
		 "POST %s HTTP/1.1\r\nHost: %s:%u\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
		 "SOAPAction: \"%s#%s\"\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
		 control_path, control_host, control_port, service_type, action, (int)strlen(body), body);
	int n = http_request(control_host, control_port, request, reply, reply_size);
	return n > 12 && strncmp(reply + 9, "200", 3) == 0;
}

static int private_address(const char *ip)
{
	unsigned a = 0, b = 0;
	sscanf(ip, "%u.%u", &a, &b);
	return a == 10 || (a == 172 && b >= 16 && b < 32) || (a == 192 && b == 168) || (a == 100 && b >= 64 && b < 128);
}

#define LEASE_S 3600
static DWORD mapped_at;

static int request_mapping(int lease, char *reply, int reply_size)
{
	char args[768];
	uint16_t port = settings_port();
	snprintf(args, sizeof(args),
		 "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>"
		 "<NewInternalPort>%u</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
		 "<NewPortMappingDescription>" DESCRIPTION "</NewPortMappingDescription><NewLeaseDuration>%d</NewLeaseDuration>",
		 port, port, local_ip, lease);
	return soap("AddPortMapping", args, reply, reply_size);
}

// ---------------------------------------------------------------- the port in the Steam invite
// A Steam invite gives the partner the address the host's game published in its Steam lobby, and the game takes the internet port of it from the IWNet
// "ipdetect" reply: the port the ipdetect server saw the query come from. A router or a proxy that sends the query from another port puts a port in the
// invite that nothing forwards: a player's router answered DNS with fake-IP addresses (198.18.0.0/15, a proxy on the router), the query went out from
// port 47224, and the partner's coopConnect went to <host IP>:47224 for good while UPnP forwarded 28960 (2026-11-25; the same evening a join by the
// lobby's rejoin, which uses the game's port, worked). With the game's port forwarded by UPnP and the ipdetect address the same as the router's, the
// forwarded port is published instead, as the game does after its own UPnP (64-bit build, Ghidra):
//   - the address record (rva 0x1bbf038; 0x28fe20 returns it): +0 this PC's address, +4 the internet address the ipdetect reply gave (0x28fca0, called
//     by the reply handler 0x1b8c70), +8 the game's port, +0xa the internet port (host order);
//   - the int at rva 0x3c6800: the internet port is confirmed (0x28fbf0); a later ipdetect reply then keeps it (0x28fca0). The game's own UPnP (0x1bccf0)
//     sets the port (0x28fd20) and this flag;
//   - LobbyCreated_t (0x240310) copies the record once into the IWSteamClient object (its pointer at rva 0x1a2d700: +0xd0 the lobby's id, +0xe0 copied,
//     +0xe4 the copy, its internet port at +0xee) and sets the lobby data addrLoc, portLoc, addr and port (ISteamMatchmaking::SetLobbyData, vtable +0xa0).
//     A lobby made before the forward was in place gets its "port" set again here.
// Not on the 32-bit builds yet.
#ifdef _WIN64
#define XN_RECORD XR(0x1bbf038)
#define XN_PORT_CONFIRMED XR(0x3c6800)
#define STEAM_CLIENT_PTR XR(0x1a2d700)
static void publish_forwarded_port(uint16_t port, uint32_t router_ip)
{
	uint8_t *exe = (uint8_t *)GetModuleHandleA(NULL);
	if (!addr_resolved(0x1bbf038) || !addr_resolved(0x3c6800) || !addr_resolved(0x1a2d700)) {
		log_printf("upnp: the game's address record was not found in this build: the invite keeps the port the ipdetect server saw");
		return;
	}
	volatile uint32_t *ip = (volatile uint32_t *)(exe + XN_RECORD);	// [0] this PC, [1] the internet address
	volatile uint16_t *ports = (volatile uint16_t *)(exe + XN_RECORD + 8); // [0] the game's port, [1] the internet port
	for (int i = 0; i < 60 && !ip[1]; i++)					// the ipdetect reply comes a few seconds after the game starts
		Sleep(250);
	struct in_addr seen = {.s_addr = ip[1]}, router = {.s_addr = router_ip};
	char seen_text[16] = "", router_text[16] = "";
	inet_ntop(AF_INET, &seen, seen_text, sizeof(seen_text));
	inet_ntop(AF_INET, &router, router_text, sizeof(router_text));
	if (!ip[1]) {
		log_printf("upnp: no ipdetect reply yet: the invite keeps the game's own port");
		return;
	}
	if (ip[1] == ip[0])
		return; // no NAT: the game publishes its own port
	if (ip[1] != router_ip) {
		log_printf("upnp: the ipdetect server saw %s, the router says %s: the invite keeps the port the server saw (%u)", seen_text, router_text, ports[1]);
		return;
	}
	const uint16_t before = ports[1];
	ports[1] = port;
	*(volatile int32_t *)(exe + XN_PORT_CONFIRMED) = 1;
	if (before != port)
		log_printf("upnp: the invite's port is %u now (the ipdetect server saw %s:%u; the router forwards %u)", port, seen_text, before, port);
	uint8_t *client = *(uint8_t *volatile *)(exe + STEAM_CLIENT_PTR);
	if (!client || !*(volatile uint8_t *)(client + 0xe0) || !*(volatile uint64_t *)(client + 0xd0))
		return; // no Steam lobby yet: it takes the record when it is made
	if (*(volatile uint16_t *)(client + 0xee) == port)
		return;
	*(volatile uint16_t *)(client + 0xee) = port;
	HMODULE steam = GetModuleHandleA("steam_api64.dll");
	void *(*matchmaking)(void) = steam ? (void *(*)(void))GetProcAddress(steam, "SteamMatchmaking") : NULL;
	void *mm = matchmaking ? matchmaking() : NULL;
	char value[8];
	snprintf(value, sizeof(value), "%u", port);
	const int ok = mm && ((_Bool (*)(void *, uint64_t, const char *, const char *))(*(void ***)mm)[0xa0 / 8])(mm, *(volatile uint64_t *)(client + 0xd0), "port", value);
	log_printf("upnp: the Steam lobby's port %s %u", ok ? "set again to" : "could NOT be set to", port);
}
#else
static void publish_forwarded_port(uint16_t port, uint32_t router_ip)
{
	(void)port, (void)router_ip;
}
#endif

static void add_mapping(void)
{
	char reply[4096];
	uint16_t port = settings_port();
	int ok = request_mapping(LEASE_S, reply, sizeof(reply));
	if (!ok) {
		char code[16] = "";
		find_tag(reply, "errorCode", code, sizeof(code));
		if (strcmp(code, "725") == 0) // OnlyPermanentLeasesSupported
			ok = request_mapping(0, reply, sizeof(reply));
	}
	if (!ok) {
		char code[16] = "?";
		find_tag(reply, "errorCode", code, sizeof(code));
		log_printf("upnp: the router refused to forward UDP %u (error %s)", port, code);
		return;
	}
	if (mapped)
		return; // a renewal
	mapped_at = GetTickCount();
	InterlockedExchange(&mapped, 1);
	char ext[32] = "";
	if (soap("GetExternalIPAddress", "", reply, sizeof(reply)))
		find_tag(reply, "NewExternalIPAddress", ext, sizeof(ext));
	log_printf("upnp: UDP %u forwarded to %s; the router's internet address is %s%s", port, local_ip,
		   ext[0] ? ext : "unknown",
		   ext[0] && private_address(ext) ? " (a provider's private address: players on the internet cannot reach it, use Steam or a VPN)" : "");
	if (ext[0] && !private_address(ext))
		publish_forwarded_port(port, inet_addr(ext));
}

static void delete_mapping(void)
{
	char args[256], reply[2048];
	snprintf(args, sizeof(args),
		 "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>",
		 settings_port());
	soap("DeletePortMapping", args, reply, sizeof(reply));
	InterlockedExchange(&mapped, 0);
	log_printf("upnp: forward of UDP %u removed", settings_port());
}

static DWORD WINAPI worker(LPVOID unused)
{
	(void)unused;
	int found = 0, tried = 0;
	for (;;) {
		WaitForSingleObject(wake, 5000);
		if (wanted && !mapped) {
			if (!found && !tried) {
				tried = 1; // one search per run: a router without UPnP stays without it
				found = discover();
			}
			if (found)
				add_mapping();
			if (!mapped)
				InterlockedExchange(&wanted, 0); // do not retry every few seconds
		} else if (!wanted && mapped) {
			// starting a level drops "hosting" for a moment (the lobby's party goes before the
			// level is up: removed and added again, QA 2026-10-22); only a lasting stop counts
			if (GetTickCount() - unwanted_at >= 10000)
				delete_mapping();
		} else if (wanted && mapped && GetTickCount() - mapped_at > (LEASE_S / 2) * 1000u) {
			mapped_at = GetTickCount();
			add_mapping(); // renew the lease
		}
	}
	return 0;
}

void upnp_want(int hosting)
{
	if (!wake)
		return;
	static int last = -1;
	if (hosting == last)
		return;
	last = hosting;
	if (!hosting)
		unwanted_at = GetTickCount();
	InterlockedExchange(&wanted, hosting);
	SetEvent(wake);
}

#ifdef _WIN64
static uint16_t qa_invite_port;
static DWORD WINAPI qa_invite_worker(LPVOID unused)
{
	(void)unused;
	while (!wanted)
		WaitForSingleObject(wake, 5000);
	uint8_t *exe = (uint8_t *)GetModuleHandleA(NULL);
	for (int i = 0; i < 60 && addr_resolved(0x1bbf038) && !((volatile uint32_t *)(exe + XN_RECORD))[1]; i++)
		Sleep(250);
	publish_forwarded_port(qa_invite_port, addr_resolved(0x1bbf038) ? ((volatile uint32_t *)(exe + XN_RECORD))[1] : 0);
	return 0;
}
#endif

void upnp_install(void)
{
	// QA: test copies never change the router (MW2COOP_NOUPNP, set by tools/qa_pair.sh): a pair run on one PC needs no forward, and a real
	// one is an outward-facing change on the owner's network
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa); // (kept either way: other parts may count on Winsock being up)
	if (GetEnvironmentVariableA("MW2COOP_NOUPNP", NULL, 0)) {
		log_printf("upnp: off (MW2COOP_NOUPNP)");
#ifdef _WIN64
		// QA (MW2COOP_QA_INVITEPORT=<port>): no router; once this PC hosts, the invite's port is published as after a forward of <port> by a router
		// whose internet address is the one the ipdetect reply gave
		char qa[16];
		if (GetEnvironmentVariableA("MW2COOP_QA_INVITEPORT", qa, sizeof(qa)) && atoi(qa) > 0 && atoi(qa) < 65536) {
			qa_invite_port = (uint16_t)atoi(qa);
			wake = CreateEventA(NULL, FALSE, FALSE, NULL);
			CloseHandle(CreateThread(NULL, 0, qa_invite_worker, NULL, 0, NULL));
			log_printf("upnp: QA: the invite's port is published as if the router forwarded %u", qa_invite_port);
		}
#endif
		return;
	}
	wake = CreateEventA(NULL, FALSE, FALSE, NULL);
	CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
}

