#!/usr/bin/env python3
# Test tool for winet.c net_nat: run it, start the host with I76_NET_BIND_IP=127.0.0.1 and the joiner with
# I76_NET_BIND_IP=127.0.0.2, and let the joiner connect to 127.0.0.4 (see PORTING_NOTES.md).
# Simulated NAT router for one client: the client talks to INSIDE (looks like the host), the host sees the
# client as OUTSIDE (a different address and port, like a router's public address).
import socket, select, sys, time
CLIENT = ('127.0.0.2', 21155)     # the joiner behind the "router"
INSIDE = ('127.0.0.4', 21155)     # router address the joiner sends to (stands for the host)
OUTSIDE = ('127.0.0.3', 40000)    # router's public address as seen by the host
HOST = ('127.0.0.1', 21155)
duration = float(sys.argv[1]) if len(sys.argv) > 1 else 150
a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); a.bind(INSIDE)
b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); b.bind(OUTSIDE)
n = [0, 0]
end = time.time() + duration
while time.time() < end:
    r, _, _ = select.select([a, b], [], [], 0.5)
    for s in r:
        data, src = s.recvfrom(4096)
        if s is a and src == CLIENT:
            b.sendto(data, HOST); n[0] += 1
        elif s is b and src == HOST:
            a.sendto(data, CLIENT); n[1] += 1
print('natsim: client->host %d, host->client %d' % tuple(n))
