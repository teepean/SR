/**
 *
 *  WSOCK32 (Winsock 1.1) for the recompiled WINET.DLL (ANETDLL's TCP/IP transport): UDP sockets only.
 *  Game-visible structures keep their 32-bit Windows layouts; host sockets are in hostnet.c.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "ptr32.h"
#include "winapi.h"
#include "hostnet.h"
#include "config.h"

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

EXTERN_C_BEGIN

#define WS_INVALID_SOCKET 0xFFFFFFFFu
#define WS_SOCKET_ERROR 0xFFFFFFFFu
#define WS_AF_INET 2
#define WS_SOCK_DGRAM 2
#define WS_SOL_SOCKET 0xFFFF
#define WS_SO_REUSEADDR 0x0004
#define WS_SO_BROADCAST 0x0020
#define WS_SO_SNDBUF 0x1001
#define WS_SO_RCVBUF 0x1002
#define WS_FIONBIO 0x8004667Eu

typedef struct {
    uint16_t sin_family;
    uint16_t sin_port;      // network byte order
    uint32_t sin_addr;      // network byte order
    uint8_t sin_zero[8];
} ws_sockaddr_in;

typedef struct {
    uint16_t wVersion;
    uint16_t wHighVersion;
    char szDescription[257];
    char szSystemStatus[129];
    uint16_t iMaxSockets;
    uint16_t iMaxUdpDg;
    PTR32(char) lpVendorInfo;
} ws_wsadata;               // 400 bytes

typedef struct {
    PTR32(char) h_name;
    PTR32(PTR32(char)) h_aliases;
    int16_t h_addrtype;
    int16_t h_length;
    PTR32(PTR32(char)) h_addr_list;
} ws_hostent;

// Windows delivers a socket's own broadcasts to itself (WINET finds out its own address that way: it
// broadcasts to its port and waits for the packet); Linux may not (firewalls drop them too), so own
// broadcasts are also queued locally, and copies the host delivers anyway are dropped.
#define LOOP_PACKETS 8
#define LOOP_SIZE 2048
typedef struct {
    int len;
    uint32_t time;
    uint8_t data[LOOP_SIZE];
} loop_packet;

// sockets: handle = index + 1 (Windows SOCKET values are small integers too)
#define MAX_SOCKETS 16
static struct {
    int used;
    hn_socket s;
    uint16_t port;                      // bound port (network byte order)
    loop_packet queue[LOOP_PACKETS];    // own broadcasts waiting to be received
    int queue_head, queue_count;
    loop_packet sent[LOOP_PACKETS];     // recently sent own broadcasts (to drop host-delivered copies)
    int sent_pos;
} sockets[MAX_SOCKETS];

static uint32_t local_ip;

static int is_broadcast(uint32_t ip)
{
    return (ip == 0xFFFFFFFFu) || ((ip >> 24) == 0xFF);     // limited or (for /24 networks) directed broadcast
}

static uint32_t wsa_error;

uint32_t CCALL ws2_32_inet_addr_c(const char *cp);

static int socket_index(uint32_t handle)
{
    if ((handle == 0) || (handle > MAX_SOCKETS) || !sockets[handle - 1].used) return -1;
    return (int)handle - 1;
}

static uint32_t fail(uint32_t error)
{
    wsa_error = error;
    return WS_SOCKET_ERROR;
}

uint32_t CCALL ws2_32_WSAStartup_c(uint32_t wVersionRequested, ws_wsadata *lpWSAData)
{
    if (winapi_debug) eprintf("WSAStartup: version 0x%x\n", wVersionRequested);
    if (!hn_init()) return 10091;   // WSASYSNOTREADY
    if (lpWSAData != NULL)
    {
        memset(lpWSAData, 0, sizeof(*lpWSAData));
        lpWSAData->wVersion = 0x0101;
        lpWSAData->wHighVersion = 0x0101;
        strcpy(lpWSAData->szDescription, "SR-I76 Winsock emulation");
        strcpy(lpWSAData->szSystemStatus, "Running");
        lpWSAData->iMaxSockets = MAX_SOCKETS;
        lpWSAData->iMaxUdpDg = 65467;
    }
    return 0;
}

uint32_t CCALL ws2_32_WSACleanup_c(void)
{
    return 0;
}

uint32_t CCALL ws2_32_WSAGetLastError_c(void)
{
    return wsa_error;
}

uint32_t CCALL ws2_32_socket_c(int32_t af, int32_t type, int32_t protocol)
{
    int i;
    hn_socket s;

    if ((af != WS_AF_INET) || (type != WS_SOCK_DGRAM))
    {
        if (winapi_debug) eprintf("socket: af %d type %d protocol %d not supported\n", af, type, protocol);
        wsa_error = 10044;  // WSAESOCKTNOSUPPORT
        return WS_INVALID_SOCKET;
    }
    for (i = 0; i < MAX_SOCKETS; i++)
    {
        if (!sockets[i].used) break;
    }
    if (i == MAX_SOCKETS)
    {
        wsa_error = 10024;  // WSAEMFILE
        return WS_INVALID_SOCKET;
    }
    s = hn_udp_socket();
    if (s == HN_INVALID)
    {
        wsa_error = hn_last_error();
        return WS_INVALID_SOCKET;
    }
    memset(&sockets[i], 0, sizeof(sockets[i]));
    sockets[i].used = 1;
    sockets[i].s = s;
    if (local_ip == 0) local_ip = hn_local_ip();
    if (winapi_debug) eprintf("socket: UDP -> %d\n", i + 1);
    return (uint32_t)(i + 1);
}

uint32_t CCALL ws2_32_closesocket_c(uint32_t s)
{
    int i = socket_index(s);
    if (i < 0) return fail(10038);  // WSAENOTSOCK
    hn_close(sockets[i].s);
    sockets[i].used = 0;
    return 0;
}

uint32_t CCALL ws2_32_bind_c(uint32_t s, const ws_sockaddr_in *name, int32_t namelen)
{
    int i = socket_index(s);
    if (i < 0) return fail(10038);
    if ((name == NULL) || (namelen < (int32_t)sizeof(ws_sockaddr_in))) return fail(10014);    // WSAEFAULT
    // SR-I76.cfg net_bind_ip: bind to one local address instead of all (several instances on one machine:
    // 127.0.0.1, 127.0.0.2, ...; or a specific network interface)
    if (name->sin_addr == 0)
    {
        const char *bind_ip = config_get("net_bind_ip");
        uint32_t ip = (bind_ip != NULL) ? ws2_32_inet_addr_c(bind_ip) : 0xFFFFFFFFu;
        if ((ip != 0xFFFFFFFFu) && (ip != 0))
        {
            if (hn_bind(sockets[i].s, ip, name->sin_port) < 0) return fail(hn_last_error());
            local_ip = ip;
            sockets[i].port = name->sin_port;
            if (winapi_debug) eprintf("bind: %d to %s port %u\n", s, bind_ip, (unsigned)((name->sin_port >> 8) | ((name->sin_port & 0xFF) << 8)));
            return 0;
        }
    }
    if (hn_bind(sockets[i].s, name->sin_addr, name->sin_port) < 0)
    {
        if (winapi_debug) eprintf("bind: port %u failed (%d)\n", (unsigned)((name->sin_port >> 8) | ((name->sin_port & 0xFF) << 8)), hn_last_error());
        return fail(hn_last_error());
    }
    sockets[i].port = name->sin_port;
    if (winapi_debug) eprintf("bind: %d to port %u\n", s, (unsigned)((name->sin_port >> 8) | ((name->sin_port & 0xFF) << 8)));
    return 0;
}

uint32_t CCALL setsockopt_c(uint32_t s, int32_t level, int32_t optname, const char *optval, int32_t optlen)
{
    int i = socket_index(s);
    int32_t v;
    if (i < 0) return fail(10038);
    if ((optval == NULL) || (optlen < 1)) return fail(10014);
    v = (optlen >= 4) ? *(const int32_t *)optval : (uint8_t)optval[0];
    if (level == WS_SOL_SOCKET)
    {
        switch (optname)
        {
            case WS_SO_BROADCAST:
                if (hn_set_broadcast(sockets[i].s, v != 0) < 0) return fail(hn_last_error());
                return 0;
            case WS_SO_SNDBUF:
            case WS_SO_RCVBUF:
                if (hn_set_buffer(sockets[i].s, optname == WS_SO_SNDBUF, v) < 0) return fail(hn_last_error());
                return 0;
            case WS_SO_REUSEADDR:
                return 0;
        }
    }
    if (winapi_debug) eprintf("setsockopt: level 0x%x option 0x%x ignored\n", level, optname);
    return 0;
}

uint32_t CCALL ws2_32_ioctlsocket_c(uint32_t s, uint32_t cmd, uint32_t *argp)
{
    int i = socket_index(s);
    if (i < 0) return fail(10038);
    if (cmd == WS_FIONBIO)
    {
        if (hn_set_nonblocking(sockets[i].s, (argp != NULL) && (*argp != 0)) < 0) return fail(hn_last_error());
        return 0;
    }
    if (winapi_debug) eprintf("ioctlsocket: command 0x%x not supported\n", cmd);
    return fail(10045);     // WSAEOPNOTSUPP
}

uint32_t CCALL ws2_32_sendto_c(uint32_t s, const char *buf, int32_t len, int32_t flags, const ws_sockaddr_in *to, int32_t tolen)
{
    int i = socket_index(s);
    int r;
    if (i < 0) return fail(10038);
    if ((to == NULL) || (tolen < (int32_t)sizeof(ws_sockaddr_in))) return fail(10014);
    r = hn_sendto(sockets[i].s, buf, len, to->sin_addr, to->sin_port);
    if (winapi_debug >= 2) eprintf("sendto: %d bytes to %u.%u.%u.%u:%u -> %d\n", len, to->sin_addr & 0xFF, (to->sin_addr >> 8) & 0xFF, (to->sin_addr >> 16) & 0xFF, to->sin_addr >> 24, (unsigned)((to->sin_port >> 8) | ((to->sin_port & 0xFF) << 8)), r);
    if (r < 0) return fail(hn_last_error());
    if (is_broadcast(to->sin_addr) && (to->sin_port == sockets[i].port) && (sockets[i].port != 0) && (len <= LOOP_SIZE))
    {
        loop_packet *p;
        uint32_t now = winapi_get_ticks();
        if (sockets[i].queue_count < LOOP_PACKETS)
        {
            p = &sockets[i].queue[(sockets[i].queue_head + sockets[i].queue_count) % LOOP_PACKETS];
            p->len = len;
            p->time = now;
            memcpy(p->data, buf, len);
            sockets[i].queue_count++;
        }
        p = &sockets[i].sent[sockets[i].sent_pos];
        sockets[i].sent_pos = (sockets[i].sent_pos + 1) % LOOP_PACKETS;
        p->len = len;
        p->time = now;
        memcpy(p->data, buf, len);
    }
    return (uint32_t)r;
}

// a host-delivered copy of an own broadcast that was already queued?
static int is_own_copy(int i, const char *buf, int len, uint32_t ip, uint16_t port)
{
    int k;
    uint32_t now;
    if ((port != sockets[i].port) || ((ip != local_ip) && (ip != 0x0100007Fu))) return 0;
    now = winapi_get_ticks();
    for (k = 0; k < LOOP_PACKETS; k++)
    {
        loop_packet *p = &sockets[i].sent[k];
        if ((p->len == len) && (now - p->time < 2000) && (memcmp(p->data, buf, len) == 0))
        {
            p->len = -1;
            return 1;
        }
    }
    return 0;
}

uint32_t CCALL recvfrom_c(uint32_t s, char *buf, int32_t len, int32_t flags, ws_sockaddr_in *from, int32_t *fromlen)
{
    int i = socket_index(s);
    uint32_t ip;
    uint16_t port;
    int r;
    if (i < 0) return fail(10038);
    if (sockets[i].queue_count > 0)
    {
        loop_packet *p = &sockets[i].queue[sockets[i].queue_head];
        sockets[i].queue_head = (sockets[i].queue_head + 1) % LOOP_PACKETS;
        sockets[i].queue_count--;
        r = (p->len < len) ? p->len : len;
        memcpy(buf, p->data, r);
        ip = local_ip;
        port = sockets[i].port;
    }
    else
    {
        do
        {
            r = hn_recvfrom(sockets[i].s, buf, len, &ip, &port);
            if (r < 0) return fail(hn_last_error());
        } while (is_own_copy(i, buf, r, ip, port));
    }
    if ((from != NULL) && (fromlen != NULL) && (*fromlen >= (int32_t)sizeof(ws_sockaddr_in)))
    {
        memset(from, 0, sizeof(*from));
        from->sin_family = WS_AF_INET;
        from->sin_port = port;
        from->sin_addr = ip;
        *fromlen = sizeof(ws_sockaddr_in);
    }
    if (winapi_debug >= 2) eprintf("recvfrom: %d bytes from %u.%u.%u.%u:%u\n", r, ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24, (unsigned)((port >> 8) | ((port & 0xFF) << 8)));
    return (uint32_t)r;
}

uint32_t CCALL ws2_32_htons_c(uint32_t hostshort)
{
    return (uint16_t)(((hostshort & 0xFF) << 8) | ((hostshort >> 8) & 0xFF));
}

// dotted decimal -> address in network byte order; INADDR_NONE on error
uint32_t CCALL ws2_32_inet_addr_c(const char *cp)
{
    uint32_t parts[4], n = 0, v;
    const char *p = cp;

    if (cp == NULL) return 0xFFFFFFFFu;
    for (;;)
    {
        if ((*p < '0') || (*p > '9')) return 0xFFFFFFFFu;
        v = 0;
        while ((*p >= '0') && (*p <= '9'))
        {
            v = v * 10 + (*p - '0');
            if (v > 255) return 0xFFFFFFFFu;
            p++;
        }
        parts[n++] = v;
        if (*p == '.' && n < 4) { p++; continue; }
        break;
    }
    while ((*p == ' ') || (*p == '\t')) p++;
    if ((*p != 0) || (n != 4)) return 0xFFFFFFFFu;
    return parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
}

char * CCALL ws2_32_inet_ntoa_c(uint32_t in)
{
    static char buf[16];    // static: below 2 GB like all runtime globals
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", in & 0xFF, (in >> 8) & 0xFF, (in >> 16) & 0xFF, in >> 24);
    return buf;
}

ws_hostent * CCALL ws2_32_gethostbyname_c(const char *name)
{
    static char h_name[256];
    static uint32_t address;
    static PTR32(char) addr_list[2];
    static PTR32(char) aliases[1];
    static ws_hostent h;
    uint32_t ip;

    if ((name == NULL) || !hn_resolve(name, &ip))
    {
        if (winapi_debug) eprintf("gethostbyname: %s not found\n", (name != NULL) ? name : "(null)");
        wsa_error = 11001;  // WSAHOST_NOT_FOUND
        return NULL;
    }
    snprintf(h_name, sizeof(h_name), "%s", name);
    address = ip;
    addr_list[0] = (char *)&address;
    addr_list[1] = NULL;
    aliases[0] = NULL;
    h.h_name = h_name;
    h.h_aliases = aliases;
    h.h_addrtype = WS_AF_INET;
    h.h_length = 4;
    h.h_addr_list = addr_list;
    if (winapi_debug) eprintf("gethostbyname: %s -> %u.%u.%u.%u\n", name, ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24);
    return &h;
}

EXTERN_C_END
