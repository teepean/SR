/**
 *
 *  Host UDP sockets for the Winsock emulation (winsock.c): BSD sockets on Linux, Winsock 2 on Windows.
 *  Addresses and ports are in network byte order (as in struct sockaddr_in).
 *
 */

#if !defined(_HOSTNET_H_INCLUDED_)
#define _HOSTNET_H_INCLUDED_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef intptr_t hn_socket;
#define HN_INVALID ((hn_socket)-1)

// Winsock error codes returned by hn_last_error
#define HN_WSAEWOULDBLOCK 10035
#define HN_WSAEMSGSIZE 10040
#define HN_WSAEADDRINUSE 10048
#define HN_WSAENETUNREACH 10051
#define HN_WSAECONNRESET 10054
#define HN_WSAHOST_NOT_FOUND 11001
#define HN_WSAEINVAL 10022

int hn_init(void);
hn_socket hn_udp_socket(void);
void hn_close(hn_socket s);
int hn_set_broadcast(hn_socket s, int on);
int hn_set_nonblocking(hn_socket s, int on);
int hn_set_buffer(hn_socket s, int send, int size);
int hn_bind(hn_socket s, uint32_t ip, uint16_t port);
int hn_sendto(hn_socket s, const void *buf, int len, uint32_t ip, uint16_t port);
int hn_recvfrom(hn_socket s, void *buf, int len, uint32_t *ip, uint16_t *port);
int hn_last_error(void);
// name -> IPv4 address; 0 on failure
int hn_resolve(const char *name, uint32_t *ip);
// the address of the interface used for outgoing traffic (127.0.0.1 if there is none)
uint32_t hn_local_ip(void);

#ifdef __cplusplus
}
#endif

#endif /* _HOSTNET_H_INCLUDED_ */
