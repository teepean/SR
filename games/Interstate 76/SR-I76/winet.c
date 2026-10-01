/**
 *
 *  DLL\WINET.DLL: ANETDLL's TCP/IP transport (the anet "comm" API), reimplemented on hostnet.c.
 *
 *  ANETDLL loads the driver with LoadLibrary and calls its 17 comm* functions through GetProcAddress, each
 *  as int comm*(req *, resp *); the responses are packed structures that start with a status byte.
 *  The driver keeps a table of peer addresses; the table index is the comm handle (0 = broadcast, 1 = self;
 *  ANETDLL's handles -1 = broadcast, -2 = self, -3/-4 = none).
 *
 *  SR-I76.cfg net_nat:
 *    0 = like the original WINET.DLL: an address is the 4-byte IPv4 address, every player uses UDP port 21155,
 *        and a player's address is the one it sees itself (the LAN address behind a NAT router). Compatible
 *        with the original game.
 *    1 = (default) NAT traversal, after Shane Peelar's patched WINET.DLL: an address is 6 bytes (IPv4 address
 *        + port), a player's own address is a placeholder, and the receiver replaces the placeholder in each
 *        packet with the sender's address as it arrives (the router's public address and port). Addresses of
 *        the receiver itself are sent as a second placeholder, which the receiver turns back into its own
 *        placeholder (ANETDLL checks that packets name it). Only the host needs a forwarded port (UDP 21155);
 *        all players must use net_nat = 1.
 *  net_bind_ip: bind to one local address (several instances on one machine: 127.0.0.1, 127.0.0.2, ...).
 *  I76_DEBUG=2 logs the packets, 3 also dumps them.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hostnet.h"
#include "config.h"
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

extern int winapi_debug;

#define WINET_PORT 21155        // 0x52A3
#define MAX_PEERS 100
#define MAX_PACKET 512

// comm status codes (commapi.h)
#define COMM_STATUS_OK 0
#define COMM_STATUS_EMPTY 2
#define COMM_STATUS_FULL 3
#define COMM_STATUS_BAD 5
#define COMM_STATUS_NOT_INIT 13

#define HANDLE_NONE -4
#define HANDLE_BROADCAST -1
#define HANDLE_ME -2

typedef struct {
    uint32_t ip;        // network byte order
    uint16_t port;      // network byte order
} peer;

static int nat_mode;
static int addr_len;
static hn_socket sock = HN_INVALID;
static peer peers[MAX_PEERS];
static int peer_count;
static uint16_t own_port;
static uint32_t own_ip;
static int bound_any;                // bound to all interfaces (own packets may also come from 127.0.0.1)
static uint8_t own_addr[8];         // the address returned by commPlayerInfo(me)

// NAT mode placeholders: the sender itself, and the receiver
static const uint8_t addr_sender[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xAA, 0xAA };
static const uint8_t addr_receiver[6] = { 0xAB, 0xAD, 0xC0, 0xDA, 0xBB, 0xBB };

// comm_driverInfo_t of the original WINET.DLL (signature, version, name "Internet", capabilities, needs)
static const uint8_t driver_info[0x38] = {
    0x41, 0x56, 0x4B, 0x45, 0x47, 0x45, 0x4C, 0x36, 0x00, 0x33, 0x00, 0x00, 0x00, 0x49, 0x6E, 0x74,
    0x65, 0x72, 0x6E, 0x65, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x2D,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static char player_name[32];
static uint8_t info_addr[8];        // address returned by commPlayerInfo (ANETDLL copies it)

// response fields are unaligned (packed structures)
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void *ptr32(uint32_t v) { return (void *)(uintptr_t)v; }
static uint32_t to32(const void *p) { return (uint32_t)(uintptr_t)p; }

static unsigned int port_host(uint16_t port_net) { return ((port_net & 0xFF) << 8) | (port_net >> 8); }

static void format_ip(char *buf, uint32_t ip)
{
    sprintf(buf, "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24);
}

static void peer_to_addr(const peer *p, uint8_t *addr)
{
    memcpy(addr, &p->ip, 4);
    if (nat_mode) memcpy(addr + 4, &p->port, 2);
}

// handle <-> table index
static int handle_to_index(int32_t h)
{
    switch (h)
    {
        case -4: case -3: return -1;
        case HANDLE_ME: return 1;
        case HANDLE_BROADCAST: return 0;
        default: return h;
    }
}

static int32_t index_to_handle(int i)
{
    switch (i)
    {
        case -1: return HANDLE_NONE;
        case 0: return HANDLE_BROADCAST;
        case 1: return HANDLE_ME;
        default: return i;
    }
}

static int find_peer(uint32_t ip, uint16_t port, int add)
{
    int i;
    for (i = 0; i < peer_count; i++)
    {
        // the original compares the IPv4 address only (everybody uses the same port)
        if ((peers[i].ip == ip) && (!nat_mode || (peers[i].port == port))) return i;
    }
    if (!add || (peer_count >= MAX_PEERS)) return -1;
    peers[peer_count].ip = ip;
    peers[peer_count].port = port;
    return peer_count++;
}

static void replace_all(uint8_t *buf, int len, const uint8_t *from, const uint8_t *to, int n)
{
    int i;
    for (i = 0; i + n <= len; i++)
    {
        if ((buf[i] == from[0]) && (memcmp(buf + i, from, n) == 0))
        {
            memcpy(buf + i, to, n);
            i += n - 1;
        }
    }
}

static int is_own_packet(uint32_t ip, uint16_t port)
{
    return (port == own_port) && ((ip == own_ip) || (bound_any && (ip == 0x0100007Fu)));
}

static void dump_packet(const char *what, const uint8_t *buf, int len, uint32_t ip, uint16_t port)
{
    char s[16];
    int k;
    format_ip(s, ip);
    eprintf("winet: %s %d bytes %s %s:%u", what, len, (what[0] == 's') ? "to" : "from", s, port_host(port));
    if (winapi_debug >= 3)
    {
        eprintf(":");
        for (k = 0; (k < len) && (k < 128); k++) eprintf(" %02x", buf[k]);
        if (len > 128) eprintf(" ...");
    }
    eprintf("\n");
}


/* ------------------------------------------------------------------ */
/* comm API                                                            */

int32_t CCALL commNoOp_c(void *req, uint8_t *resp)
{
    if (resp != NULL) resp[0] = COMM_STATUS_OK;
    return 1;
}

int32_t CCALL commInit_c(void *req, uint8_t *resp)
{
    const char *s;
    uint32_t bind_ip = 0;

    nat_mode = config_get_int("net_nat", 1) != 0;
    addr_len = nat_mode ? 6 : 4;
    own_port = (uint16_t)port_host(WINET_PORT);

    if (sock != HN_INVALID) hn_close(sock);
    sock = HN_INVALID;
    if (hn_init()) sock = hn_udp_socket();
    if (sock == HN_INVALID)
    {
        if (resp != NULL) resp[0] = COMM_STATUS_NOT_INIT;
        return 0;
    }

    s = config_get("net_bind_ip");
    if ((s != NULL) && (*s != 0) && !hn_resolve(s, &bind_ip)) bind_ip = 0;
    if (hn_bind(sock, bind_ip, own_port) < 0)
    {
        eprintf("winet: can't bind UDP port %u (%d)\n", WINET_PORT, hn_last_error());
        hn_close(sock);
        sock = HN_INVALID;
        if (resp != NULL) resp[0] = COMM_STATUS_NOT_INIT;
        return 0;
    }
    hn_set_broadcast(sock, 1);
    hn_set_nonblocking(sock, 1);
    own_ip = (bind_ip != 0) ? bind_ip : hn_local_ip();
    bound_any = (bind_ip == 0);

    // 0 = broadcast, 1 = self
    peers[0].ip = 0xFFFFFFFFu;
    peers[0].port = own_port;
    peers[1].ip = own_ip;
    peers[1].port = own_port;
    peer_count = 2;
    if (nat_mode) memcpy(own_addr, addr_sender, 6); else memcpy(own_addr, &own_ip, 4);

    if (winapi_debug)
    {
        char ip[16];
        format_ip(ip, own_ip);
        eprintf("winet: UDP port %u on %s, %s\n", WINET_PORT, ip, nat_mode ? "NAT mode (6-byte addresses)" : "original addresses");
    }
    if (resp != NULL) resp[0] = COMM_STATUS_OK;
    return 1;
}

int32_t CCALL commTerm_c(void *req, uint8_t *resp)
{
    if (sock != HN_INVALID) hn_close(sock);
    sock = HN_INVALID;
    peer_count = 0;
    if (resp != NULL) resp[0] = COMM_STATUS_OK;
    return 1;
}

// resp: status, comm_driverInfo_t *
int32_t CCALL commDriverInfo_c(void *req, uint8_t *resp)
{
    if (resp == NULL) return 0;
    resp[0] = COMM_STATUS_OK;
    put32(resp + 1, to32(driver_info));
    return 1;
}

// req: handle; resp: status, char *name, void *address, u32 address length, u32 0, u32 0
int32_t CCALL commPlayerInfo_c(const uint8_t *req, uint8_t *resp)
{
    uint8_t dummy[24];
    int i;

    if (resp == NULL) resp = dummy;
    put32(resp + 1, to32(player_name));
    i = handle_to_index((req != NULL) ? (int32_t)get32(req) : 0);
    if ((i < 0) || (i >= peer_count))
    {
        resp[0] = COMM_STATUS_BAD;
        return 0;
    }
    if (i == 1) memcpy(info_addr, own_addr, addr_len); else peer_to_addr(&peers[i], info_addr);
    put32(resp + 5, to32(info_addr));
    put32(resp + 9, addr_len);
    put32(resp + 13, 0);
    put32(resp + 17, 0);
    resp[0] = COMM_STATUS_OK;
    return 1;
}

int32_t CCALL commTxFull_c(void *req, uint8_t *resp)
{
    if (resp != NULL) resp[0] = COMM_STATUS_OK;
    return 0;
}

// req: handle, void *buffer, u32 length
int32_t CCALL commTxPkt_c(const uint8_t *req, uint8_t *resp)
{
    uint8_t dummy, buf[MAX_PACKET];
    const uint8_t *data;
    uint32_t len;
    int i, r;

    if (resp == NULL) resp = &dummy;
    if ((req == NULL) || (sock == HN_INVALID))
    {
        resp[0] = COMM_STATUS_BAD;
        return 0;
    }
    i = handle_to_index((int32_t)get32(req));
    data = (const uint8_t *)ptr32(get32(req + 4));
    len = get32(req + 8);
    if ((len == 0) || (len > MAX_PACKET) || (data == NULL) || (i < 0) || (i >= peer_count))
    {
        resp[0] = COMM_STATUS_BAD;
        return 0;
    }
    memcpy(buf, data, len);
    if (nat_mode && (i >= 2))
    {
        // the receiver's address -> "receiver" placeholder
        uint8_t addr[6];
        peer_to_addr(&peers[i], addr);
        replace_all(buf, len, addr, addr_receiver, 6);
    }
    r = hn_sendto(sock, buf, len, peers[i].ip, peers[i].port);
    if (winapi_debug >= 2) dump_packet("sent", buf, len, peers[i].ip, peers[i].port);
    if (r <= 0)
    {
        resp[0] = COMM_STATUS_FULL;
        return 0;
    }
    resp[0] = COMM_STATUS_OK;
    return 1;
}

int32_t CCALL commPeekPkt_c(void *req, uint8_t *resp)
{
    if (resp != NULL) resp[0] = COMM_STATUS_BAD;
    return 0;
}

// req: void *buffer, u32 size; resp: status, handle, u32 length
int32_t CCALL commRxPkt_c(const uint8_t *req, uint8_t *resp)
{
    uint8_t dummy[9], buf[2048];
    uint8_t *data;
    uint32_t size, ip;
    uint16_t port;
    int len, i;

    if (resp == NULL) resp = dummy;
    put32(resp + 1, (uint32_t)HANDLE_NONE);
    put32(resp + 5, 0);
    if ((req == NULL) || (sock == HN_INVALID))
    {
        resp[0] = COMM_STATUS_BAD;
        return 0;
    }
    data = (uint8_t *)ptr32(get32(req));
    size = get32(req + 4);
    if ((data == NULL) || (size == 0))
    {
        resp[0] = COMM_STATUS_BAD;
        return 0;
    }

    for (;;)
    {
        len = hn_recvfrom(sock, buf, sizeof(buf), &ip, &port);
        if (len <= 0)
        {
            resp[0] = COMM_STATUS_EMPTY;
            return 0;
        }
        // own broadcasts (Windows delivers them, the original drops packets from its own address)
        if (is_own_packet(ip, port)) continue;
        break;
    }
    if (winapi_debug >= 2) dump_packet("received", buf, len, ip, port);
    if ((len > MAX_PACKET) || ((uint32_t)len > size))
    {
        resp[0] = COMM_STATUS_BAD;
        return 0;
    }
    if (nat_mode)
    {
        // the sender's placeholder -> its address as seen here; the receiver's placeholder -> our own placeholder
        peer sender;
        uint8_t addr[6];
        sender.ip = ip;
        sender.port = port;
        peer_to_addr(&sender, addr);
        replace_all(buf, len, addr_sender, addr, 6);
        replace_all(buf, len, addr_receiver, addr_sender, 6);
    }
    memcpy(data, buf, len);
    i = find_peer(ip, port, 0);
    if (i == 1)
    {
        resp[0] = COMM_STATUS_EMPTY;
        return 0;
    }
    put32(resp + 1, (uint32_t)index_to_handle(i));
    put32(resp + 5, (uint32_t)len);
    resp[0] = COMM_STATUS_OK;
    return 1;
}

// req: void *address buffer, u32 buffer size, char *text; resp: status, u32 address length
int32_t CCALL commScanAddr_c(const uint8_t *req, uint8_t *resp)
{
    uint8_t dummy[5], addr[6];
    uint8_t *out;
    const char *text;
    char host[256], *colon;
    peer p;

    if (resp == NULL) resp = dummy;
    if ((req == NULL) || (get32(req + 4) < (uint32_t)addr_len))
    {
        resp[0] = COMM_STATUS_FULL;
        return 0;
    }
    out = (uint8_t *)ptr32(get32(req));
    text = (const char *)ptr32(get32(req + 8));
    if (text == NULL)
    {
        resp[0] = COMM_STATUS_BAD;
        return 0;
    }
    snprintf(host, sizeof(host), "%s", text);
    p.port = own_port;
    colon = strrchr(host, ':');
    if (nat_mode && (colon != NULL))
    {
        // host:port
        *colon = 0;
        p.port = (uint16_t)port_host((uint16_t)atoi(colon + 1));
    }
    if (!hn_resolve(host, &p.ip))
    {
        resp[0] = COMM_STATUS_BAD;
        return 0;
    }
    peer_to_addr(&p, addr);
    if (out != NULL) memcpy(out, addr, addr_len);
    resp[0] = COMM_STATUS_OK;
    put32(resp + 1, addr_len);
    if (winapi_debug) eprintf("winet: commScanAddr %s\n", text);
    return 1;
}

// req: char *buffer, u32 buffer size, void *address, u32 address length
int32_t CCALL commPrintAddr_c(const uint8_t *req, uint8_t *resp)
{
    uint8_t dummy;
    const uint8_t *addr;
    char text[32];
    char *out;
    uint32_t ip;

    if (resp == NULL) resp = &dummy;
    resp[0] = COMM_STATUS_OK;
    if ((req == NULL) || (get32(req + 12) != (uint32_t)addr_len) || ((addr = (const uint8_t *)ptr32(get32(req + 8))) == NULL))
    {
        resp[0] = COMM_STATUS_BAD;
        return 0;
    }
    memcpy(&ip, addr, 4);
    format_ip(text, ip);
    if (nat_mode)
    {
        uint16_t port;
        memcpy(&port, addr + 4, 2);
        if (port != own_port) sprintf(text + strlen(text), ":%u", port_host(port));
    }
    out = (char *)ptr32(get32(req));
    if (out != NULL)
    {
        if (strlen(text) + 1 > get32(req + 4))
        {
            resp[0] = COMM_STATUS_FULL;
            return 0;
        }
        strcpy(out, text);
    }
    return 1;
}

int32_t CCALL commGroupAlloc_c(void *req, uint8_t *resp)
{
    if (resp != NULL) resp[0] = COMM_STATUS_OK;
    return 0;
}

int32_t CCALL commGroupFree_c(void *req, uint8_t *resp)
{
    if (resp != NULL) resp[0] = COMM_STATUS_OK;
    return 0;
}

int32_t CCALL commGroupAdd_c(void *req, uint8_t *resp)
{
    if (resp != NULL) resp[0] = COMM_STATUS_OK;
    return 0;
}

int32_t CCALL commSetParam_c(void *req, uint8_t *resp)
{
    if (resp != NULL) resp[0] = COMM_STATUS_OK;
    return 1;
}

// req: void *address, u32 address length; resp: status, handle
int32_t CCALL commSayHi_c(const uint8_t *req, uint8_t *resp)
{
    uint8_t dummy[5];
    const uint8_t *addr;
    uint32_t len;
    peer p;
    int i;

    if (resp == NULL) resp = dummy;
    if ((req == NULL) || ((addr = (const uint8_t *)ptr32(get32(req))) == NULL))
    {
        resp[0] = COMM_STATUS_BAD;
        return 0;
    }
    len = get32(req + 4);
    if (nat_mode && (len == 6) && (memcmp(addr, addr_sender, 6) == 0))
    {
        i = 1;
    }
    else if ((len == (uint32_t)addr_len) || (nat_mode && (len == 4)))
    {
        memcpy(&p.ip, addr, 4);
        p.port = own_port;
        if (len == 6) memcpy(&p.port, addr + 4, 2);
        i = find_peer(p.ip, p.port, 1);
    }
    else
    {
        resp[0] = COMM_STATUS_BAD;
        return 0;
    }
    put32(resp + 1, (uint32_t)index_to_handle(i));
    if (i < 0)
    {
        resp[0] = COMM_STATUS_BAD;
        return 0;
    }
    if (winapi_debug >= 2)
    {
        char s[16];
        format_ip(s, peers[i].ip);
        eprintf("winet: commSayHi %s:%u -> handle %d\n", s, port_host(peers[i].port), index_to_handle(i));
    }
    resp[0] = COMM_STATUS_OK;
    return 1;
}

int32_t CCALL commSayBye_c(void *req, uint8_t *resp)
{
    if (resp != NULL) resp[0] = COMM_STATUS_OK;
    return 1;
}

#ifdef __cplusplus
}
#endif
