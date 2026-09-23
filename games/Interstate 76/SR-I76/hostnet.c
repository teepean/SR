/**
 *
 *  Host UDP sockets for the Winsock emulation (winsock.c): BSD sockets on Linux, Winsock 2 on Windows.
 *
 */

#include <string.h>
#include "hostnet.h"

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#ifdef __cplusplus
extern "C" {
#endif

static int last_error;

int hn_init(void)
{
    static int initialized;
    WSADATA wsa;
    if (initialized) return 1;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
    initialized = 1;
    return 1;
}

hn_socket hn_udp_socket(void)
{
    SOCKET s;
    DWORD off = 0, bytes;

    if (!hn_init()) return HN_INVALID;
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
    {
        last_error = WSAGetLastError();
        return HN_INVALID;
    }
    // Windows 2000+: an ICMP "port unreachable" makes the next recvfrom fail with WSAECONNRESET (Win95 didn't)
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0, &bytes, NULL, NULL);
    return (hn_socket)s;
}

void hn_close(hn_socket s) { closesocket((SOCKET)s); }

static int check(int r)
{
    if (r == SOCKET_ERROR) last_error = WSAGetLastError();
    return r;
}

int hn_set_broadcast(hn_socket s, int on)
{
    BOOL v = on ? TRUE : FALSE;
    return check(setsockopt((SOCKET)s, SOL_SOCKET, SO_BROADCAST, (const char *)&v, sizeof(v)));
}

int hn_set_nonblocking(hn_socket s, int on)
{
    u_long v = on ? 1 : 0;
    return check(ioctlsocket((SOCKET)s, FIONBIO, &v));
}

int hn_set_buffer(hn_socket s, int send, int size)
{
    return check(setsockopt((SOCKET)s, SOL_SOCKET, send ? SO_SNDBUF : SO_RCVBUF, (const char *)&size, sizeof(size)));
}

int hn_bind(hn_socket s, uint32_t ip, uint16_t port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip;
    a.sin_port = port;
    return check(bind((SOCKET)s, (struct sockaddr *)&a, sizeof(a)));
}

int hn_sendto(hn_socket s, const void *buf, int len, uint32_t ip, uint16_t port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip;
    a.sin_port = port;
    return check(sendto((SOCKET)s, (const char *)buf, len, 0, (struct sockaddr *)&a, sizeof(a)));
}

int hn_recvfrom(hn_socket s, void *buf, int len, uint32_t *ip, uint16_t *port)
{
    struct sockaddr_in a;
    int alen = sizeof(a), r;
    memset(&a, 0, sizeof(a));
    r = check(recvfrom((SOCKET)s, (char *)buf, len, 0, (struct sockaddr *)&a, &alen));
    if (r >= 0)
    {
        *ip = a.sin_addr.s_addr;
        *port = a.sin_port;
    }
    return r;
}

int hn_last_error(void) { return last_error; }

int hn_resolve(const char *name, uint32_t *ip)
{
    struct addrinfo hints, *res = NULL;
    if (!hn_init()) return 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if ((getaddrinfo(name, NULL, &hints, &res) != 0) || (res == NULL))
    {
        last_error = HN_WSAHOST_NOT_FOUND;
        return 0;
    }
    *ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return 1;
}

uint32_t hn_local_ip(void)
{
    SOCKET s;
    struct sockaddr_in a;
    int alen = sizeof(a);
    uint32_t ip = htonl(INADDR_LOOPBACK);

    if (!hn_init()) return ip;
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return ip;
    // connecting a UDP socket sends nothing; it only selects the route (192.0.2.1 = TEST-NET-1)
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0xC0000201);
    a.sin_port = htons(9);
    if ((connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) && (getsockname(s, (struct sockaddr *)&a, &alen) == 0) && (a.sin_addr.s_addr != 0)) ip = a.sin_addr.s_addr;
    closesocket(s);
    return ip;
}

#ifdef __cplusplus
}
#endif

#else

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

static int last_error;

static int map_errno(int e)
{
    switch (e)
    {
        case EAGAIN: return HN_WSAEWOULDBLOCK;
        case EMSGSIZE: return HN_WSAEMSGSIZE;
        case EADDRINUSE: return HN_WSAEADDRINUSE;
        case ENETUNREACH: return HN_WSAENETUNREACH;
        case ECONNREFUSED: return HN_WSAECONNRESET;
        default: return HN_WSAEINVAL;
    }
}

static int check(int r)
{
    if (r < 0) last_error = map_errno(errno);
    return r;
}

int hn_init(void) { return 1; }

hn_socket hn_udp_socket(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0)
    {
        last_error = map_errno(errno);
        return HN_INVALID;
    }
    return (hn_socket)s;
}

void hn_close(hn_socket s) { close((int)s); }

int hn_set_broadcast(hn_socket s, int on)
{
    int v = on ? 1 : 0;
    return check(setsockopt((int)s, SOL_SOCKET, SO_BROADCAST, &v, sizeof(v)));
}

int hn_set_nonblocking(hn_socket s, int on)
{
    int flags = fcntl((int)s, F_GETFL, 0);
    if (flags < 0) return check(-1);
    return check(fcntl((int)s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)));
}

int hn_set_buffer(hn_socket s, int send, int size)
{
    return check(setsockopt((int)s, SOL_SOCKET, send ? SO_SNDBUF : SO_RCVBUF, &size, sizeof(size)));
}

int hn_bind(hn_socket s, uint32_t ip, uint16_t port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip;
    a.sin_port = port;
    return check(bind((int)s, (struct sockaddr *)&a, sizeof(a)));
}

int hn_sendto(hn_socket s, const void *buf, int len, uint32_t ip, uint16_t port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip;
    a.sin_port = port;
    return check((int)sendto((int)s, buf, len, 0, (struct sockaddr *)&a, sizeof(a)));
}

int hn_recvfrom(hn_socket s, void *buf, int len, uint32_t *ip, uint16_t *port)
{
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    int r;
    memset(&a, 0, sizeof(a));
    r = check((int)recvfrom((int)s, buf, len, 0, (struct sockaddr *)&a, &alen));
    if (r >= 0)
    {
        *ip = a.sin_addr.s_addr;
        *port = a.sin_port;
    }
    return r;
}

int hn_last_error(void) { return last_error; }

int hn_resolve(const char *name, uint32_t *ip)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if ((getaddrinfo(name, NULL, &hints, &res) != 0) || (res == NULL))
    {
        last_error = HN_WSAHOST_NOT_FOUND;
        return 0;
    }
    *ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return 1;
}

uint32_t hn_local_ip(void)
{
    int s;
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    uint32_t ip = htonl(INADDR_LOOPBACK);

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return ip;
    // connecting a UDP socket sends nothing; it only selects the route (192.0.2.1 = TEST-NET-1)
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0xC0000201);
    a.sin_port = htons(9);
    if ((connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) && (getsockname(s, (struct sockaddr *)&a, &alen) == 0) && (a.sin_addr.s_addr != 0)) ip = a.sin_addr.s_addr;
    close(s);
    return ip;
}

#ifdef __cplusplus
}
#endif

#endif
