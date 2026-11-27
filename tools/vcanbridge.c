/*
 * vcanbridge: connects a qvcan bus to UDP, so that a program on another
 * machine (here: the QEMU host, scripts/qv_host.py) is one more node.
 *
 *   vcanbridge DEV LOCAL_PORT REMOTE_IP:REMOTE_PORT [-v]
 *
 * Every frame on DEV goes to the remote end; every frame from the remote end
 * is sent on DEV. One thread, select() on the bus and the socket (the bus
 * side is devvcan's io_notify).
 *
 * Datagram: "QVB1", count (uint16, big endian), 0 (uint16), then count
 * records of 16 bytes: id with flags (uint32, big endian), len, 0, 0, 0,
 * data[8].
 * SPDX-License-Identifier: Apache-2.0
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <devctl.h>

#include "vcan.h"

#define MAXREC 64

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

static void die(const char *what)
{
    fprintf(stderr, "vcanbridge: %s: %s\n", what, strerror(errno));
    exit(1);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: vcanbridge DEV LOCAL_PORT REMOTE_IP:REMOTE_PORT [-v]\n");
        return 2;
    }
    int verbose = argc > 4 && !strcmp(argv[4], "-v");
    int fd = open(argv[1], O_RDWR | O_NONBLOCK);
    if (fd < 0)
        die(argv[1]);
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        die("socket");
    struct sockaddr_in local, remote;
    memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    local.sin_port = htons((uint16_t)atoi(argv[2]));
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&local, sizeof local) < 0)
        die("bind");
    char host[64];
    snprintf(host, sizeof host, "%s", argv[3]);
    char *colon = strchr(host, ':');
    if (!colon)
        return 2;
    *colon = 0;
    memset(&remote, 0, sizeof remote);
    remote.sin_family = AF_INET;
    remote.sin_port = htons((uint16_t)atoi(colon + 1));
    if (inet_pton(AF_INET, host, &remote.sin_addr) != 1)
        die("address");
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    printf("QV BRIDGE %s <-> udp %s (local port %s): running\n", argv[1], argv[3], argv[2]);
    fflush(stdout);

    unsigned long to_udp = 0, to_bus = 0, bad = 0;
    struct vcan_frame f[MAXREC];
    unsigned char pkt[8 + 16 * MAXREC];
    while (!stop) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        FD_SET(s, &rd);
        int r = select((fd > s ? fd : s) + 1, &rd, NULL, NULL, NULL);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            die("select");
        }
        if (FD_ISSET(fd, &rd)) {
            ssize_t n = read(fd, f, sizeof f);
            if (n > 0) {
                size_t k = (size_t)n / sizeof f[0];
                memcpy(pkt, "QVB1", 4);
                pkt[4] = (unsigned char)(k >> 8);
                pkt[5] = (unsigned char)k;
                pkt[6] = pkt[7] = 0;
                for (size_t i = 0; i < k; i++) {
                    unsigned char *p = pkt + 8 + 16 * i;
                    uint32_t id = htonl(f[i].id);
                    memcpy(p, &id, 4);
                    p[4] = f[i].len;
                    p[5] = p[6] = p[7] = 0;
                    memcpy(p + 8, f[i].data, 8);
                }
                if (sendto(s, pkt, 8 + 16 * k, 0, (struct sockaddr *)&remote, sizeof remote) > 0)
                    to_udp += k;
            }
        }
        if (FD_ISSET(s, &rd)) {
            ssize_t n = recv(s, pkt, sizeof pkt, 0);
            if (n >= 8 && !memcmp(pkt, "QVB1", 4)) {
                size_t k = (size_t)pkt[4] << 8 | pkt[5];
                if (k > MAXREC || (size_t)n < 8 + 16 * k) {
                    bad++;
                    continue;
                }
                for (size_t i = 0; i < k; i++) {
                    const unsigned char *p = pkt + 8 + 16 * i;
                    uint32_t id;
                    memcpy(&id, p, 4);
                    memset(&f[i], 0, sizeof f[i]);
                    f[i].id = ntohl(id);
                    f[i].len = p[4] > 8 ? 8 : p[4];
                    memcpy(f[i].data, p + 8, 8);
                }
                ssize_t w = write(fd, f, k * sizeof f[0]);
                if (w > 0)
                    to_bus += (size_t)w / sizeof f[0];
                if (w != (ssize_t)(k * sizeof f[0]))
                    bad += k - (w > 0 ? (size_t)w / sizeof f[0] : 0);
                if (verbose)
                    printf("QV BRIDGE got %zu frames from udp\n", k);
            } else if (n >= 0) {
                bad++;
            }
        }
    }
    printf("QV BRIDGE END to_udp=%lu to_bus=%lu dropped=%lu\n", to_udp, to_bus, bad);
    return 0;
}
