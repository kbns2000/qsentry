/* o6q_envprobe: what does this system do in the situations that the event
 * loop of a network library runs into?
 *
 * The program knows nothing about open62541.  It asks the operating system
 * directly and prints one fact per line, "O6Q ENV <key>=<value>".  The port
 * rests on these facts (see the files in patches/ and README.md); the same
 * program on Linux gives the column to compare with.
 *
 * It opens sockets on the loopback address and changes no system state. Each
 * group of probes runs in a child process of its own, so that a crash or a hang
 * loses only that group. "o6q_envprobe nopipe" runs only the probes for a
 * system without the pipe manager (vm/run.sh stops it for that).
 *
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef _QNX_SOURCE
#define _QNX_SOURCE
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
/* what open62541.h includes before its IEEE 754 test (see float_config()) */
#include <assert.h>
#include <float.h>
#include <inttypes.h>
#ifdef __QNXNTO__
#include <gulliver.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
#endif
#ifdef O6Q_HAVE_OPENSSL
#include <openssl/crypto.h>
#include <openssl/rand.h>
#endif

static const char *g_probe = "start"; /* for the watchdog */

static void env(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void env(const char *key, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("O6Q ENV %s=%s\n", key, buf);
}

/* Name of an errno value, where the name tells more than the text */
static const char *ename(int e) {
    static char other[64];
    switch(e) {
    case 0: return "0";
    case EAGAIN: return "EAGAIN";
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK: return "EWOULDBLOCK";
#endif
    case EINTR: return "EINTR";
    case EINVAL: return "EINVAL";
    case EBADF: return "EBADF";
    case ECONNABORTED: return "ECONNABORTED";
    case ECONNRESET: return "ECONNRESET";
    case ECONNREFUSED: return "ECONNREFUSED";
    case EINPROGRESS: return "EINPROGRESS";
    case ENOSYS: return "ENOSYS";
    case ENOTSUP: return "ENOTSUP";
    case ENOPROTOOPT: return "ENOPROTOOPT";
    case EADDRNOTAVAIL: return "EADDRNOTAVAIL";
    case EADDRINUSE: return "EADDRINUSE";
    case ETIMEDOUT: return "ETIMEDOUT";
    case EPIPE: return "EPIPE";
    case ENOTCONN: return "ENOTCONN";
    case EMFILE: return "EMFILE";
    case ENOENT: return "ENOENT";
    case EPERM: return "EPERM";
    case EACCES: return "EACCES";
    case ENODEV: return "ENODEV";
    case ENXIO: return "ENXIO";
#if EOPNOTSUPP != ENOTSUP
    case EOPNOTSUPP: return "EOPNOTSUPP";
#endif
    default:
        snprintf(other, sizeof(other), "errno %d (%s)", e, strerror(e));
        return other;
    }
}

static const char *revents_name(short r) {
    static char buf[96];
    buf[0] = 0;
    if(r & POLLIN) strcat(buf, "POLLIN|");
    if(r & POLLPRI) strcat(buf, "POLLPRI|");
    if(r & POLLOUT) strcat(buf, "POLLOUT|");
    if(r & POLLERR) strcat(buf, "POLLERR|");
    if(r & POLLHUP) strcat(buf, "POLLHUP|");
    if(r & POLLNVAL) strcat(buf, "POLLNVAL|");
#ifdef POLLRDHUP
    if(r & POLLRDHUP) strcat(buf, "POLLRDHUP|");
#endif
    if(buf[0])
        buf[strlen(buf) - 1] = 0;
    else
        snprintf(buf, sizeof(buf), "none");
    return buf;
}

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while(nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* revents of one descriptor after waiting at most ms; -1 if poll() failed */
static int poll_one(int fd, short events, int ms, short *revents) {
    struct pollfd p;
    int rc;
    p.fd = fd;
    p.events = events;
    p.revents = 0;
    rc = poll(&p, 1, ms);
    *revents = p.revents;
    return rc;
}

/* ------------------------------------------------------ loopback sockets */

static void loopback(struct sockaddr_in *a, unsigned port) {
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = htons((uint16_t)port);
    a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
}

/* A listening TCP socket on a port that the system picks. */
static int tcp_listener(unsigned *port, int nonblocking) {
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    int one = 1, fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    loopback(&a, 0);
    if(bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 16) != 0 ||
       getsockname(fd, (struct sockaddr *)&a, &len) != 0) {
        close(fd);
        return -1;
    }
    if(nonblocking) {
        int fl = fcntl(fd, F_GETFL);
        if(fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) {
            close(fd); /* accept() on it could block */
            return -1;
        }
    }
    *port = ntohs(a.sin_port);
    return fd;
}

static int tcp_client(unsigned port) {
    struct sockaddr_in a;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0)
        return -1;
    loopback(&a, port);
    if(connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void tcp_reset(int fd) {
    struct linger l;
    l.l_onoff = 1;
    l.l_linger = 0;
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
    close(fd);
}

/* ------------------------------------------------------------ the basics */

static void resolve(const char *name) {
    struct addrinfo hints, *res = NULL, *ai;
    char buf[INET6_ADDRSTRLEN], key[300];
    int rc, n = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(name, "4840", &hints, &res);
    if(rc != 0) {
        snprintf(key, sizeof(key), "resolve.%s", name);
        env(key, "FAILED(%s)", gai_strerror(rc));
        return;
    }
    for(ai = res; ai; ai = ai->ai_next) {
        const void *a;
        if(ai->ai_family == AF_INET)
            a = &((struct sockaddr_in *)(void *)ai->ai_addr)->sin_addr;
        else if(ai->ai_family == AF_INET6)
            a = &((struct sockaddr_in6 *)(void *)ai->ai_addr)->sin6_addr;
        else
            continue;
        snprintf(key, sizeof(key), "resolve.%s.%d", name, n++);
        env(key, "%s", inet_ntop(ai->ai_family, a, buf, sizeof(buf)) ? buf : "?");
    }
    freeaddrinfo(res);
}

static void passive(void) {
    struct addrinfo hints, *res = NULL, *ai;
    char key[32];
    int rc, n = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    rc = getaddrinfo(NULL, "4840", &hints, &res);
    if(rc != 0) {
        env("passive", "FAILED(%s)", gai_strerror(rc));
        return;
    }
    for(ai = res; ai; ai = ai->ai_next) {
        snprintf(key, sizeof(key), "passive.%d", n++);
        env(key, "%s", ai->ai_family == AF_INET ? "AF_INET" :
                       ai->ai_family == AF_INET6 ? "AF_INET6" : "other");
    }
    freeaddrinfo(res);
}

static void try_socket(const char *key, int family, int type) {
    int fd = socket(family, type, 0);
    if(fd < 0) {
        env(key, "FAILED(%s)", ename(errno));
        return;
    }
    env(key, "ok");
    close(fd);
}

/* Name and index of the first interface that is up, can do multicast, is not
 * the loopback and has an IPv4 address. */
static char g_mc_if[IFNAMSIZ];
static struct in_addr g_mc_addr;
static unsigned g_mc_index;

static void interfaces(void) {
    struct ifaddrs *ifa = NULL, *i;
    char buf[INET6_ADDRSTRLEN], key[64];
    if(getifaddrs(&ifa) != 0) {
        env("if", "getifaddrs FAILED(%s)", ename(errno));
        return;
    }
    for(i = ifa; i; i = i->ifa_next) {
        int fam;
        const void *a;
        if(!i->ifa_addr)
            continue;
        fam = i->ifa_addr->sa_family;
        if(fam == AF_INET)
            a = &((struct sockaddr_in *)(void *)i->ifa_addr)->sin_addr;
        else if(fam == AF_INET6)
            a = &((struct sockaddr_in6 *)(void *)i->ifa_addr)->sin6_addr;
        else
            continue;
        snprintf(key, sizeof(key), "if.%s.%s", i->ifa_name, fam == AF_INET ? "ipv4" : "ipv6");
        env(key, "%s index=%u%s%s%s%s", inet_ntop(fam, a, buf, sizeof(buf)) ? buf : "?",
            if_nametoindex(i->ifa_name), (i->ifa_flags & IFF_UP) ? " UP" : "",
            (i->ifa_flags & IFF_RUNNING) ? " RUNNING" : "",
            (i->ifa_flags & IFF_LOOPBACK) ? " LOOPBACK" : "",
            (i->ifa_flags & IFF_MULTICAST) ? " MULTICAST" : "");
        if(fam == AF_INET && !g_mc_if[0] && (i->ifa_flags & IFF_UP) &&
           (i->ifa_flags & IFF_MULTICAST) && !(i->ifa_flags & IFF_LOOPBACK)) {
            snprintf(g_mc_if, sizeof(g_mc_if), "%s", i->ifa_name);
            g_mc_addr = ((struct sockaddr_in *)(void *)i->ifa_addr)->sin_addr;
            g_mc_index = if_nametoindex(i->ifa_name);
        }
    }
    freeifaddrs(ifa);
}

static void basics(void) {
    struct utsname u;
    struct timespec ts;
    struct rlimit rl;
    char host[256] = "";
    pthread_attr_t attr;
    size_t stack = 0;
    int fd;

    g_probe = "basics";
    if(uname(&u) == 0)
        env("uname", "%s %s %s %s", u.sysname, u.release, u.version, u.machine);
    gethostname(host, sizeof(host) - 1);
    env("hostname", "%s", host);
    env("compiler", "%s", __VERSION__);
    env("sizeof", "long=%u pointer=%u time_t=%u bool=%u fd_set=%u", (unsigned)sizeof(long),
        (unsigned)sizeof(void *), (unsigned)sizeof(time_t), (unsigned)sizeof(bool),
        (unsigned)sizeof(fd_set));
#ifdef __STDC_IEC_559__
    env("macro.__STDC_IEC_559__", "defined");
#else
    env("macro.__STDC_IEC_559__", "not defined");
#endif
    env("fd.FD_SETSIZE", "%d", (int)FD_SETSIZE);
    env("fd.sysconf_open_max", "%ld", sysconf(_SC_OPEN_MAX));
    if(getrlimit(RLIMIT_NOFILE, &rl) == 0)
        env("fd.rlimit_nofile", "soft=%ld hard=%ld",
            rl.rlim_cur == RLIM_INFINITY ? -1L : (long)rl.rlim_cur,
            rl.rlim_max == RLIM_INFINITY ? -1L : (long)rl.rlim_max);
    env("cpus", "%ld", sysconf(_SC_NPROCESSORS_ONLN));
    if(clock_getres(CLOCK_MONOTONIC, &ts) == 0)
        env("clock.monotonic_resolution_ns", "%ld", (long)(ts.tv_sec * 1000000000L + ts.tv_nsec));
#ifdef __QNXNTO__
    {
        struct _clockperiod cp;
        env("qnx.cycles_per_sec", "%llu",
            (unsigned long long)SYSPAGE_ENTRY(qtime)->cycles_per_sec);
        if(ClockPeriod(CLOCK_REALTIME, NULL, &cp, 0) == 0)
            env("qnx.clock_period_ns", "%lu", (unsigned long)cp.nsec);
    }
#endif
    pthread_attr_init(&attr);
    pthread_attr_getstacksize(&attr, &stack);
    pthread_attr_destroy(&attr);
    env("pthread.default_stack", "%lu", (unsigned long)stack);

    try_socket("socket.tcp4", AF_INET, SOCK_STREAM);
    try_socket("socket.tcp6", AF_INET6, SOCK_STREAM);
    try_socket("socket.udp4", AF_INET, SOCK_DGRAM);
    try_socket("socket.udp6", AF_INET6, SOCK_DGRAM);
    resolve("localhost");
    if(host[0])
        resolve(host);
    passive();
    interfaces();

    fd = open("/dev/random", O_RDONLY | O_NONBLOCK);
    if(fd < 0) {
        env("dev_random", "FAILED(%s)", ename(errno));
    } else {
        unsigned char b[16];
        env("dev_random", "read %ld of %u bytes", (long)read(fd, b, sizeof(b)), (unsigned)sizeof(b));
        close(fd);
    }
#ifdef O6Q_HAVE_OPENSSL
    {
        unsigned char b[16];
        env("openssl.version", "%s", OpenSSL_version(OPENSSL_VERSION));
        env("openssl.rand_bytes", "%s", RAND_bytes(b, sizeof(b)) == 1 ? "ok" : "FAILED");
    }
#endif
}

/* -------------------------------------------- descriptors above FD_SETSIZE */

static void high_descriptors(void) {
    int sp[2], high, rc;
    short re = 0;
    struct timespec ts;
    struct pollfd p;

    g_probe = "high_descriptors";
    if(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) {
        env("highfd", "socketpair FAILED(%s)", ename(errno));
        return;
    }
    high = fcntl(sp[0], F_DUPFD, FD_SETSIZE + 16);
    if(high < 0) {
        env("highfd.dup", "FAILED(%s): no descriptor above FD_SETSIZE with the current limit",
            ename(errno));
        close(sp[0]);
        close(sp[1]);
        return;
    }
    env("highfd.dup", "descriptor %d (FD_SETSIZE %d)", high, (int)FD_SETSIZE);
    if(write(sp[1], "x", 1) != 1)
        env("highfd.write", "FAILED(%s)", ename(errno));

    rc = poll_one(high, POLLIN, 500, &re);
    env("highfd.poll", "returns %d, revents %s%s%s", rc, revents_name(re), rc < 0 ? ", " : "",
        rc < 0 ? ename(errno) : "");

    p.fd = high;
    p.events = POLLIN;
    p.revents = 0;
    ts.tv_sec = 0;
    ts.tv_nsec = 500000000L;
    rc = ppoll(&p, 1, &ts, NULL);
    env("highfd.ppoll", "returns %d, revents %s%s%s", rc, revents_name(p.revents),
        rc < 0 ? ", " : "", rc < 0 ? ename(errno) : "");

    { /* select() with a set that is large enough for the descriptor. Bit n
       * of an fd_set is bit n%8 of byte n/8 on a little-endian CPU. */
        size_t bytes = (size_t)(high / 8 + 1 + 64);
        unsigned char *set = (unsigned char *)calloc(1, bytes);
        struct timeval tv;
        if(set) {
            set[high / 8] = (unsigned char)(set[high / 8] | (1u << (high % 8)));
            tv.tv_sec = 0;
            tv.tv_usec = 500000;
            errno = 0;
            rc = select(high + 1, (fd_set *)(void *)set, NULL, NULL, &tv);
            env("highfd.select_large_set", "returns %d%s%s%s", rc, rc < 0 ? ", " : "",
                rc < 0 ? ename(errno) : "",
                rc == 1 && (set[high / 8] & (1u << (high % 8))) ? ", descriptor reported readable"
                                                                 : "");
            free(set);
        }
    }
    close(high);
    close(sp[0]);
    close(sp[1]);
}

/* ------------------------------------------------------- poll() semantics */

static void poll_semantics(void) {
    struct pollfd p[2];
    struct timespec ts;
    double t0;
    int rc, sp[2];

    g_probe = "poll_semantics";

    /* no descriptor at all: a plain timed wait? */
    ts.tv_sec = 0;
    ts.tv_nsec = 20000000L;
    t0 = now_us();
    errno = 0;
    rc = ppoll(p, 0, &ts, NULL);
    env("ppoll.no_descriptor", "returns %d after %.1f ms (timeout 20 ms)%s%s", rc,
        (now_us() - t0) / 1000.0, rc < 0 ? ", " : "", rc < 0 ? ename(errno) : "");

    /* a negative descriptor is to be ignored */
    p[0].fd = -1;
    p[0].events = POLLIN;
    p[0].revents = 0;
    t0 = now_us();
    rc = poll(p, 1, 20);
    env("poll.negative_descriptor", "returns %d after %.1f ms, revents %s", rc,
        (now_us() - t0) / 1000.0, revents_name(p[0].revents));

    /* a descriptor that is not open */
    if(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0) {
        close(sp[0]);
        close(sp[1]);
        p[0].fd = sp[0];
        p[0].events = POLLIN;
        p[0].revents = 0;
        rc = poll(p, 1, 20);
        env("poll.closed_descriptor", "returns %d, revents %s", rc, revents_name(p[0].revents));
    }

    /* events == 0: are error conditions still reported? (a UDP socket that is
     * only used for sending is registered like that) */
    if(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0) {
        close(sp[1]);
        p[0].fd = sp[0];
        p[0].events = 0;
        p[0].revents = 0;
        rc = poll(p, 1, 20);
        env("poll.events0_peer_closed", "returns %d, revents %s", rc, revents_name(p[0].revents));
        close(sp[0]);
    }

    /* events == 0 on a TCP connection that the peer has reset: does the
     * network stack report what the change eventloop-ppoll says it does? */
    {
        unsigned port = 0;
        int ls = tcp_listener(&port, 1), c = -1, s = -1;
        short re = 0;
        if(ls >= 0) {
            c = tcp_client(port);
            (void)poll_one(ls, POLLIN, 500, &re);
            s = accept(ls, NULL, NULL);
        }
        if(c >= 0 && s >= 0) {
            tcp_reset(c);
            c = -1;
            sleep_ms(300);
            rc = poll_one(s, 0, 20, &re);
            env("poll.events0_tcp_reset", "returns %d, revents %s", rc, revents_name(re));
        }
        if(s >= 0)
            close(s);
        if(c >= 0)
            close(c);
        if(ls >= 0)
            close(ls);
    }
}

/* ---------------------------------------------------- errno after success */

static void errno_semantics(void) {
    struct pollfd p;
    struct timespec ts;
    struct timeval tv;
    int sp[2], rc;
    char byte;

    g_probe = "errno_semantics";
    if(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) {
        env("errno", "socketpair FAILED(%s): no errno.* probes", ename(errno));
        return;
    }

    p.fd = sp[0];
    p.events = POLLIN;
    ts.tv_sec = 0;
    ts.tv_nsec = 2000000L;
    errno = EAGAIN;
    rc = ppoll(&p, 1, &ts, NULL);
    env("errno.after_ppoll_timeout", "%s (was EAGAIN before; ppoll returned %d)", ename(errno), rc);

    errno = EAGAIN;
    rc = poll(&p, 1, 2);
    env("errno.after_poll_timeout", "%s (was EAGAIN before; poll returned %d)", ename(errno), rc);

    {
        fd_set r;
        FD_ZERO(&r);
        if(sp[0] < FD_SETSIZE) {
            FD_SET(sp[0], &r);
            tv.tv_sec = 0;
            tv.tv_usec = 2000;
            errno = EAGAIN;
            rc = select(sp[0] + 1, &r, NULL, NULL, &tv);
            env("errno.after_select_timeout", "%s (was EAGAIN before; select returned %d)",
                ename(errno), rc);
        }
    }

    if(write(sp[1], "x", 1) == 1) {
        errno = EAGAIN;
        rc = ppoll(&p, 1, &ts, NULL);
        env("errno.after_ppoll_event", "%s (was EAGAIN before; ppoll returned %d)", ename(errno),
            rc);
        errno = EAGAIN;
        rc = (int)recv(sp[0], &byte, 1, 0);
        env("errno.after_recv_data", "%s (was EAGAIN before; recv returned %d)", ename(errno), rc);
    }
    close(sp[1]);
    errno = EAGAIN;
    rc = (int)recv(sp[0], &byte, 1, 0);
    env("errno.after_recv_end", "%s (was EAGAIN before; recv returned %d for a closed peer)",
        ename(errno), rc);
    close(sp[0]);
}

/* ------------------------------------------------------------ TCP: accept */

static void tcp_semantics(void) {
    unsigned port = 0;
    int ls, c, s, rc, flags;
    short re = 0;
    char byte;
    struct sockaddr_storage peer;
    socklen_t peer_len;

    g_probe = "tcp_semantics";
    ls = tcp_listener(&port, 1);
    if(ls < 0) {
        env("tcp", "no listening socket on the loopback address: %s", ename(errno));
        return;
    }

    /* a pending connection */
    c = tcp_client(port);
    rc = poll_one(ls, POLLIN, 500, &re);
    env("tcp.listen_pending", "poll returns %d, revents %s", rc, revents_name(re));
    peer_len = sizeof(peer);
    s = accept(ls, (struct sockaddr *)&peer, &peer_len);
    if(c < 0 || s < 0) {
        env("tcp.accept", "FAILED(%s)", ename(errno));
        close(ls);
        return;
    }
    flags = fcntl(s, F_GETFL);
    env("tcp.accept_inherits_nonblock", "%s",
        (flags >= 0 && (flags & O_NONBLOCK)) ? "yes" : "no");
    env("tcp.accept_address_length", "%u (sockaddr_in %u, sockaddr_storage %u)",
        (unsigned)peer_len, (unsigned)sizeof(struct sockaddr_in),
        (unsigned)sizeof(struct sockaddr_storage));

    { /* getnameinfo() with the real length and with the size of the storage */
        char host[NI_MAXHOST];
        rc = getnameinfo((struct sockaddr *)&peer, peer_len, host, sizeof(host), NULL, 0,
                         NI_NUMERICHOST);
        env("getnameinfo.real_length", "%s%s", rc == 0 ? "ok " : "FAILED: ",
            rc == 0 ? host : gai_strerror(rc));
        rc = getnameinfo((struct sockaddr *)&peer, sizeof(peer), host, sizeof(host), NULL, 0,
                         NI_NUMERICHOST);
        env("getnameinfo.storage_length", "%s%s", rc == 0 ? "ok " : "FAILED: ",
            rc == 0 ? host : gai_strerror(rc));
    }

    { /* socket buffers, and how much a send() takes before it says EAGAIN */
        int snd = 0, rcv = 0;
        socklen_t len = sizeof(int);
        static char chunk[65536];
        size_t total = 0;
        ssize_t k = 0;
        getsockopt(s, SOL_SOCKET, SO_SNDBUF, &snd, &len);
        len = sizeof(int);
        getsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcv, &len);
        env("tcp.buffers", "SO_SNDBUF=%d SO_RCVBUF=%d", snd, rcv);
        fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
        while(total < 64u * 1024u * 1024u) {
            k = send(s, chunk, sizeof(chunk), MSG_DONTWAIT); /* also if O_NONBLOCK did not take */
            if(k < 0)
                break;
            total += (size_t)k;
        }
        env("tcp.send_until_full", "%lu bytes accepted while the peer does not read, then %s",
            (unsigned long)total, k < 0 ? ename(errno) : "no error");
    }
    close(c);
    close(s);

    /* the peer closes its side (FIN) */
    c = tcp_client(port);
    poll_one(ls, POLLIN, 500, &re);
    s = accept(ls, NULL, NULL);
    if(c >= 0 && s >= 0) {
        shutdown(c, SHUT_WR);
        rc = poll_one(s, POLLIN, 500, &re);
        errno = 0;
        env("tcp.peer_shutdown_wr", "poll returns %d, revents %s; recv returns %d", rc,
            revents_name(re), (int)recv(s, &byte, 1, MSG_DONTWAIT));
        close(c);
        sleep_ms(50);
        rc = poll_one(s, POLLIN, 500, &re);
        env("tcp.peer_close", "poll returns %d, revents %s; recv returns %d", rc, revents_name(re),
            (int)recv(s, &byte, 1, MSG_DONTWAIT));
    }
    if(c >= 0 && s < 0)
        close(c);
    if(s >= 0)
        close(s);

    /* the peer resets an accepted connection */
    c = tcp_client(port);
    poll_one(ls, POLLIN, 500, &re);
    s = accept(ls, NULL, NULL);
    if(c >= 0 && s >= 0) {
        int k;
        tcp_reset(c);
        sleep_ms(50);
        rc = poll_one(s, POLLIN, 500, &re);
        errno = 0;
        k = (int)recv(s, &byte, 1, MSG_DONTWAIT);
        env("tcp.peer_reset", "poll returns %d, revents %s; recv returns %d%s%s", rc,
            revents_name(re), k, k < 0 ? " " : "", k < 0 ? ename(errno) : "");
    } else if(c >= 0) {
        close(c);
    }
    if(s >= 0)
        close(s);

    /* the peer resets its connection before accept() */
    c = tcp_client(port);
    if(c >= 0) {
        tcp_reset(c);
        sleep_ms(300); /* time for the reset to arrive, also on an emulated CPU */
        rc = poll_one(ls, POLLIN, 200, &re);
        errno = 0;
        s = accept(ls, NULL, NULL);
        if(s < 0) {
            env("tcp.reset_before_accept", "poll returns %d, revents %s; accept fails with %s", rc,
                revents_name(re), ename(errno));
        } else {
            int k;
            short re2 = 0;
            (void)poll_one(s, POLLIN, 1000, &re2); /* a reset that is late still arrives */
            errno = 0;
            k = (int)recv(s, &byte, 1, MSG_DONTWAIT);
            env("tcp.reset_before_accept",
                "poll returns %d, revents %s; accept returns a socket, recv on it returns %d%s%s",
                rc, revents_name(re), k, k < 0 ? " " : "", k < 0 ? ename(errno) : "");
            close(s);
        }
    }
    close(ls);

    { /* a non-blocking connect() to a port without a listener */
        struct sockaddr_in a;
        int err = 0;
        socklen_t len = sizeof(err);
        c = socket(AF_INET, SOCK_STREAM, 0);
        if(c >= 0) {
            fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK);
            loopback(&a, port); /* the listener above is closed now */
            errno = 0;
            rc = connect(c, (struct sockaddr *)&a, sizeof(a));
            if(rc == 0) {
                env("tcp.connect_refused", "connect returned 0 (somebody listens on port %u)", port);
            } else if(errno != EINPROGRESS) {
                env("tcp.connect_refused", "connect fails at once with %s", ename(errno));
            } else {
                rc = poll_one(c, POLLOUT, 2000, &re);
                getsockopt(c, SOL_SOCKET, SO_ERROR, &err, &len);
                env("tcp.connect_refused",
                    "connect says EINPROGRESS; poll(POLLOUT) returns %d, revents %s; SO_ERROR %s",
                    rc, revents_name(re), ename(err));
            }
            close(c);
        }
    }
}

/* ------------------------------------------------------------------- UDP */

static void udp_semantics(void) {
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    int r, w, rc;
    short re = 0;
    char buf[16];
    ssize_t k;

    g_probe = "udp_semantics";
    r = socket(AF_INET, SOCK_DGRAM, 0);
    w = socket(AF_INET, SOCK_DGRAM, 0);
    loopback(&a, 0);
    if(r < 0 || w < 0 || bind(r, (struct sockaddr *)&a, sizeof(a)) != 0 ||
       getsockname(r, (struct sockaddr *)&a, &len) != 0) {
        env("udp", "no UDP socket on the loopback address: %s", ename(errno));
        if(r >= 0)
            close(r);
        if(w >= 0)
            close(w);
        return;
    }
    /* non-blocking as open62541's UDP sockets: recvfrom() cannot block even
     * if MSG_DONTWAIT were ignored */
    fcntl(r, F_SETFL, fcntl(r, F_GETFL) | O_NONBLOCK);

    /* a datagram without payload */
    errno = 0;
    k = sendto(w, "", 0, 0, (struct sockaddr *)&a, sizeof(a));
    if(k != 0) {
        env("udp.empty_datagram", "sendto returns %d %s", (int)k, ename(errno));
    } else {
        rc = poll_one(r, POLLIN, 500, &re);
        errno = 0;
        k = recvfrom(r, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL);
        env("udp.empty_datagram", "poll returns %d, revents %s; recvfrom returns %d, errno %s", rc,
            revents_name(re), (int)k, ename(errno));
    }

    /* nothing to read */
    errno = 0;
    k = recvfrom(r, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL);
    env("udp.nothing_to_read", "recvfrom(MSG_DONTWAIT) returns %d %s", (int)k,
        k < 0 ? ename(errno) : "");
    close(r);
    close(w);
}

/* ------------------------------------------------------------- multicast */

static void try_opt(const char *key, int fd, int level, int opt, const void *val, socklen_t len) {
    if(setsockopt(fd, level, opt, val, len) == 0)
        env(key, "ok");
    else
        env(key, "FAILED(%s)", ename(errno));
}

static void multicast(const char *group, const char *tag) {
    struct ip_mreqn mn;
    struct ip_mreq mr;
    struct sockaddr_in a;
    char key[96];
    int fd, one = 1;
    unsigned char one_c = 1;

    g_probe = "multicast";
    if(strcmp(tag, "group") == 0) {
        if(g_mc_if[0])
            env("multicast.interface", "%s index=%u", g_mc_if, g_mc_index);
        else
            env("multicast", "skipped: no interface that is up, can multicast, is not the "
                             "loopback and has an IPv4 address (yet)");
    }
    if(!g_mc_if[0])
        return;

    /* the forms that open62541 uses: struct ip_mreqn with an interface index */
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd < 0)
        return;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(48400);
    inet_pton(AF_INET, group, &a.sin_addr);
    snprintf(key, sizeof(key), "multicast.%s.bind_to_group", tag);
    if(bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0)
        env(key, "ok");
    else
        env(key, "FAILED(%s)", ename(errno));

    memset(&mn, 0, sizeof(mn));
    inet_pton(AF_INET, group, &mn.imr_multiaddr);
    mn.imr_ifindex = (int)g_mc_index;
    snprintf(key, sizeof(key), "multicast.%s.join_mreqn_index", tag);
    try_opt(key, fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mn, sizeof(mn));
    setsockopt(fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mn, sizeof(mn));

    mn.imr_ifindex = 0;
    snprintf(key, sizeof(key), "multicast.%s.join_mreqn_index0", tag);
    try_opt(key, fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mn, sizeof(mn));
    setsockopt(fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mn, sizeof(mn));

    memset(&mr, 0, sizeof(mr));
    inet_pton(AF_INET, group, &mr.imr_multiaddr);
    mr.imr_interface = g_mc_addr;
    snprintf(key, sizeof(key), "multicast.%s.join_mreq_address", tag);
    try_opt(key, fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr));
    close(fd);

    if(strcmp(tag, "examples") == 0)
        return;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd < 0)
        return;
    memset(&mn, 0, sizeof(mn));
    mn.imr_ifindex = (int)g_mc_index;
    try_opt("multicast.if_mreqn_index", fd, IPPROTO_IP, IP_MULTICAST_IF, &mn, sizeof(mn));
    mn.imr_ifindex = 0;
    try_opt("multicast.if_mreqn_index0", fd, IPPROTO_IP, IP_MULTICAST_IF, &mn, sizeof(mn));
    try_opt("multicast.if_address", fd, IPPROTO_IP, IP_MULTICAST_IF, &g_mc_addr,
            sizeof(g_mc_addr));
    try_opt("multicast.loop_int", fd, IPPROTO_IP, IP_MULTICAST_LOOP, &one, sizeof(one));
    try_opt("multicast.loop_char", fd, IPPROTO_IP, IP_MULTICAST_LOOP, &one_c, sizeof(one_c));
    try_opt("multicast.ttl_int", fd, IPPROTO_IP, IP_MULTICAST_TTL, &one, sizeof(one));
    try_opt("multicast.ttl_char", fd, IPPROTO_IP, IP_MULTICAST_TTL, &one_c, sizeof(one_c));
    close(fd);
}

/* ------------------------------------------- name lookups, pipe, signals */

static volatile sig_atomic_t g_signals;

static void on_signal(int sig) {
    (void)sig;
    g_signals++;
}

static void misc(void) {
    int p[2];
    double t0;
    char host[NI_MAXHOST];
    struct sockaddr_in any;
    int rc;

    g_probe = "misc";

    if(pipe(p) == 0) {
        env("pipe", "ok");
        close(p[0]);
        close(p[1]);
    } else {
        env("pipe", "FAILED(%s)", ename(errno));
    }
    if(socketpair(AF_UNIX, SOCK_STREAM, 0, p) == 0) {
        env("socketpair.unix_stream", "ok");
        close(p[0]);
        close(p[1]);
    } else {
        env("socketpair.unix_stream", "FAILED(%s)", ename(errno));
    }

    /* The name of the wildcard address: a reverse DNS query. */
    memset(&any, 0, sizeof(any));
    any.sin_family = AF_INET;
    t0 = now_us();
    rc = getnameinfo((struct sockaddr *)&any, sizeof(any), host, sizeof(host), NULL, 0, 0);
    env("getnameinfo.wildcard_name", "%s%s after %.0f ms", rc == 0 ? "ok " : "FAILED: ",
        rc == 0 ? host : gai_strerror(rc), (now_us() - t0) / 1000.0);

    { /* Does a handler that was installed with signal() stay installed? */
        struct sigaction cur;
        g_signals = 0;
        signal(SIGUSR1, on_signal);
        memset(&cur, 0, sizeof(cur));
        sigaction(SIGUSR1, NULL, &cur);
        env("signal.sa_flags", "0x%x%s%s%s", (unsigned)cur.sa_flags,
#ifdef SA_RESTART
            (cur.sa_flags & SA_RESTART) ? " SA_RESTART" : "",
#else
            "",
#endif
            (cur.sa_flags & SA_RESETHAND) ? " SA_RESETHAND" : "",
            (cur.sa_flags & SA_NODEFER) ? " SA_NODEFER" : "");
        raise(SIGUSR1);
        memset(&cur, 0, sizeof(cur));
        sigaction(SIGUSR1, NULL, &cur);
        env("signal.handler_after_delivery", "%s (%d delivered)",
            cur.sa_handler == on_signal ? "still installed" :
            cur.sa_handler == SIG_DFL ? "reset to SIG_DFL" : "changed",
            (int)g_signals);
        signal(SIGUSR1, SIG_IGN);
    }

    {
        clockid_t clock;
        struct timespec ts;
        rc = pthread_getcpuclockid(pthread_self(), &clock);
        if(rc == 0 && clock_gettime(clock, &ts) == 0)
            env("pthread_getcpuclockid", "ok");
        else
            env("pthread_getcpuclockid", "FAILED(%s)", ename(rc ? rc : errno));
    }
}

/* ------------------------------------------------------- timeout accuracy */

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void timing_line(const char *key, double *v, int n, int failed) {
    double sum = 0;
    char note[32] = "";
    qsort(v, (size_t)n, sizeof(double), cmp_double);
    for(int i = 0; i < n; i++)
        sum += v[i];
    if(failed)
        snprintf(note, sizeof(note), " FAILED=%d", failed);
    env(key, "n=%d overshoot_us min=%.0f p50=%.0f p99=%.0f max=%.0f mean=%.0f%s", n, v[0],
        v[n / 2], v[(n * 99) / 100], v[n - 1], sum / n, note);
}

#define TIMING_N 200

/* How long does a wait of 1 ms take?  The event loop waits like this for
 * its next timer. */
static void timing(void) {
    static double v[TIMING_N];
    struct pollfd none[1]; /* as in the port: an array with no entry in use */
    struct timespec ts;
    struct timeval tv;
    double t0;
    int failed;

    g_probe = "timing";
    failed = 0;
    for(int i = 0; i < TIMING_N; i++) {
        ts.tv_sec = 0;
        ts.tv_nsec = 1000000L;
        t0 = now_us();
        failed += ppoll(none, 0, &ts, NULL) != 0;
        v[i] = now_us() - t0 - 1000.0;
    }
    timing_line("wait_1ms.ppoll", v, TIMING_N, failed);
    failed = 0;
    for(int i = 0; i < TIMING_N; i++) {
        tv.tv_sec = 0;
        tv.tv_usec = 1000;
        t0 = now_us();
        failed += select(0, NULL, NULL, NULL, &tv) != 0;
        v[i] = now_us() - t0 - 1000.0;
    }
    timing_line("wait_1ms.select", v, TIMING_N, failed);
    failed = 0;
    for(int i = 0; i < TIMING_N; i++) {
        t0 = now_us();
        failed += poll(none, 0, 1) != 0;
        v[i] = now_us() - t0 - 1000.0;
    }
    timing_line("wait_1ms.poll", v, TIMING_N, failed);
    failed = 0;
    for(int i = 0; i < TIMING_N; i++) {
        ts.tv_sec = 0;
        ts.tv_nsec = 1000000L;
        t0 = now_us();
        failed += nanosleep(&ts, NULL) != 0;
        v[i] = now_us() - t0 - 1000.0;
    }
    timing_line("wait_1ms.nanosleep", v, TIMING_N, failed);
}

/* ------------------------------------------------------------------ main */

static void on_alarm(int sig) {
    static const char head[] = "\nO6Q ENV watchdog.", tail[] = "=did not return\n";
    (void)sig;
    if(write(STDOUT_FILENO, head, sizeof(head) - 1) > 0 &&
       write(STDOUT_FILENO, g_probe, strlen(g_probe)) > 0 &&
       write(STDOUT_FILENO, tail, sizeof(tail) - 1) > 0)
        _exit(3);
    _exit(3);
}

/* ------------------------------------------- change 02: after an abort */

/* accept() delay_ms after the peer has reset its connection in the listen
 * queue, on a listen socket of its own (robust.aborted_connect waits 20 ms) */
static const char *reset_then_accept(long delay_ms) {
    static char out[64];
    unsigned port = 0;
    int ls = tcp_listener(&port, 1), c, s;
    if(ls < 0)
        return "no listening socket";
    c = tcp_client(port);
    if(c < 0) {
        close(ls);
        return "no connection";
    }
    tcp_reset(c);
    if(delay_ms > 0)
        sleep_ms(delay_ms);
    errno = 0;
    s = accept(ls, NULL, NULL);
    snprintf(out, sizeof(out), "%s", s < 0 ? ename(errno) : "a socket");
    if(s >= 0)
        close(s);
    close(ls);
    return out;
}

/* accept() on a non-blocking listen socket when nothing waits, and what is
 * left after a connection that was reset in the listen queue: is the listen
 * socket still readable (the port returns and the EventLoop polls it again),
 * and does the next connection get through? */
static void accept_after_abort(void) {
    unsigned port = 0;
    int ls, c, s, rc, k;
    short re = 0;
    char byte, first[80], again[48];

    g_probe = "accept_after_abort";
    ls = tcp_listener(&port, 1);
    if(ls < 0) {
        env("tcp.accept_empty_queue", "no listening socket: %s", ename(errno));
        return;
    }

    /* nothing waits */
    errno = 0;
    s = accept(ls, NULL, NULL);
    env("tcp.accept_empty_queue", "accept returns %d%s%s", s, s < 0 ? " " : "",
        s < 0 ? ename(errno) : "");
    if(s >= 0)
        close(s);

    /* a connection that is reset in the listen queue */
    c = tcp_client(port);
    if(c < 0) {
        env("tcp.reset_before_accept_after", "no connection: %s", ename(errno));
        close(ls);
        return;
    }
    tcp_reset(c);
    sleep_ms(300);
    errno = 0;
    s = accept(ls, NULL, NULL);
    if(s < 0) {
        snprintf(first, sizeof(first), "accept fails with %s", ename(errno));
    } else {
        (void)poll_one(s, POLLIN, 1000, &re);
        errno = 0;
        k = (int)recv(s, &byte, 1, MSG_DONTWAIT);
        snprintf(first, sizeof(first), "accept returns a socket, recv %d%s%s", k,
                 k < 0 ? " " : "", k < 0 ? ename(errno) : "");
        close(s);
    }

    /* what is left in the queue */
    rc = poll_one(ls, POLLIN, 0, &re);
    errno = 0;
    s = accept(ls, NULL, NULL);
    snprintf(again, sizeof(again), "%s", s < 0 ? ename(errno) : "a socket");
    if(s >= 0)
        close(s);
    env("tcp.reset_before_accept_after",
        "%s; then the listen socket: poll returns %d, revents %s; accept again: %s", first, rc,
        revents_name(re), again);

    /* the next connection */
    c = tcp_client(port);
    rc = poll_one(ls, POLLIN, 1000, &re);
    errno = 0;
    s = accept(ls, NULL, NULL);
    env("tcp.accept_after_abort_next", "poll returns %d, revents %s; accept %s", rc,
        revents_name(re), s >= 0 ? "returns a socket" : ename(errno));
    if(s >= 0)
        close(s);
    if(c >= 0)
        close(c);
    close(ls);

    { /* how soon the reset is seen */
        char at0[64], at20[64];
        snprintf(at0, sizeof(at0), "%s", reset_then_accept(0));
        snprintf(at20, sizeof(at20), "%s", reset_then_accept(20));
        env("tcp.reset_before_accept_by_delay", "accept 0 ms after the reset: %s; 20 ms: %s; "
            "300 ms: %s", at0, at20, reset_then_accept(300));
    }
}

/* The change keeps closing the listen socket after EMFILE because "the
 * connection stays in the queue then". The soft limit is lowered to the
 * descriptors in use for one accept() and restored at once. */
static void accept_emfile(void) {
    struct rlimit rl, low;
    unsigned port = 0;
    int ls, c, s, next, rc, e;
    short re = 0;

    g_probe = "accept_emfile";
    if(getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        env("tcp.accept_emfile", "getrlimit FAILED(%s)", ename(errno));
        return;
    }
    ls = tcp_listener(&port, 1);
    if(ls < 0)
        return;
    c = tcp_client(port);
    next = dup(ls); /* the lowest free descriptor, the one accept() would take */
    if(c < 0 || next < 0) {
        env("tcp.accept_emfile", "no connection: %s", ename(errno));
        if(c >= 0)
            close(c);
        close(ls);
        return;
    }
    close(next);
    (void)poll_one(ls, POLLIN, 500, &re);
    low = rl;
    low.rlim_cur = (rlim_t)next;
    if(setrlimit(RLIMIT_NOFILE, &low) != 0) {
        env("tcp.accept_emfile", "setrlimit FAILED(%s)", ename(errno));
        close(c);
        close(ls);
        return;
    }
    errno = 0;
    s = accept(ls, NULL, NULL);
    e = errno;
    setrlimit(RLIMIT_NOFILE, &rl);
    if(s >= 0) {
        env("tcp.accept_emfile", "accept returns a socket although the soft limit is %d", next);
        close(s);
    } else {
        rc = poll_one(ls, POLLIN, 500, &re);
        s = accept(ls, NULL, NULL);
        env("tcp.accept_emfile",
            "accept fails with %s; with the limit restored: poll returns %d, revents %s; "
            "accept %s",
            ename(e), rc, revents_name(re), s >= 0 ? "returns the connection" : ename(errno));
        if(s >= 0)
            close(s);
    }
    close(c);
    close(ls);
}

/* ----------------------------------------------- change 03: errno on TCP */

/* Whether an accepted socket behaves as non-blocking (not only what F_GETFL
 * says), errno after a poll() and a send() that succeed while errno holds
 * EAGAIN (the retry in TCP_sendWithConnection), and errno after recv() == 0
 * on TCP. */
static void tcp_errno(void) {
    unsigned port = 0;
    int ls, c, s, rc;
    short re = 0;
    char byte = 0;
    ssize_t k;
    struct pollfd p;

    g_probe = "tcp_errno";
    ls = tcp_listener(&port, 1);
    if(ls < 0)
        return;
    c = tcp_client(port);
    (void)poll_one(ls, POLLIN, 500, &re);
    s = accept(ls, NULL, NULL);
    if(c < 0 || s < 0) {
        env("errno.tcp", "no connection: %s", ename(errno));
    } else {
        struct timeval tv;
        double ms;
        tv.tv_sec = 0;
        tv.tv_usec = 200000; /* the bound if recv() blocks */
        if(setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
            env("tcp.accept_inherits_nonblock_behaviour", "SO_RCVTIMEO FAILED(%s)",
                ename(errno));
        } else {
            ms = now_us();
            errno = 0;
            k = recv(s, &byte, 1, 0);
            ms = (now_us() - ms) / 1000.0;
            env("tcp.accept_inherits_nonblock_behaviour",
                "recv without data returns %d %s after %.1f ms: %s", (int)k,
                k < 0 ? ename(errno) : "", ms, ms < 100.0 ? "non-blocking" : "blocking");
        }

        p.fd = s;
        p.events = POLLOUT;
        p.revents = 0;
        errno = EAGAIN;
        rc = poll(&p, 1, 100);
        k = send(s, "x", 1, MSG_NOSIGNAL);
        env("errno.after_poll_send_ok", "%s (was EAGAIN before; poll returned %d, send %d)",
            ename(errno), rc, (int)k);

        shutdown(c, SHUT_WR);
        (void)poll_one(s, POLLIN, 500, &re);
        errno = EAGAIN;
        k = recv(s, &byte, 1, MSG_DONTWAIT);
        env("errno.after_tcp_recv_eof", "%s (was EAGAIN before; recv returned %d after FIN)",
            ename(errno), (int)k);
    }
    if(s >= 0)
        close(s);
    if(c >= 0)
        close(c);
    close(ls);
}

/* ------------------------------------------- change 05: no pipe manager */

/* "o6q_envprobe nopipe", run by vm/run.sh while the pipe manager is stopped:
 * what pipe() says then, and whether a socket pair does the job of the
 * self-pipe of the interrupt manager (non-blocking, a byte written is
 * reported readable and read back, then EAGAIN). */
static void nopipe(void) {
    int p[2], k, ok, rc, r1, r2;
    short re = 0;
    char buf[8];

    g_probe = "nopipe";
    errno = 0;
    if(pipe(p) == 0) {
        env("nopipe.pipe", "ok");
        close(p[0]);
        close(p[1]);
    } else {
        env("nopipe.pipe", "FAILED(%s)", ename(errno));
    }
    if(socketpair(AF_UNIX, SOCK_STREAM, 0, p) != 0) {
        env("nopipe.socketpair", "FAILED(%s)", ename(errno));
        return;
    }
    ok = fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK) == 0 &&
         fcntl(p[1], F_SETFL, fcntl(p[1], F_GETFL) | O_NONBLOCK) == 0;
    k = (int)write(p[1], ".", 1);
    rc = poll_one(p[0], POLLIN, 500, &re);
    r1 = (int)read(p[0], buf, sizeof(buf));
    errno = 0;
    r2 = (int)read(p[0], buf, sizeof(buf));
    env("nopipe.socketpair",
        "ok; O_NONBLOCK %s; write %d; poll returns %d, revents %s; read %d, then %d %s",
        ok ? "set" : "FAILED", k, rc, revents_name(re), r1, r2, r2 < 0 ? ename(errno) : "");
    close(p[0]);
    close(p[1]);
}

/* ---------------------------------------------- floating point encoding */

/* What the IEEE 754 test of open62541.h v1.4.10 (without the port) decides
 * here. The headers that open62541.h includes before the test are included
 * above; the exact value of the release is "float_ieee754=" in the BUILD line
 * of o6q_check_upstream. */
static void float_config(void) {
    const char *release;
#if defined(_WIN32)
    release = "1 (_WIN32)";
#elif defined(__i386__) || defined(__x86_64__) || defined(__amd64__) || \
    defined(__ia64__) || defined(__powerpc__) || defined(__sparc__) || defined(__arm__)
    release = "1 (CPU list)";
#elif defined(__STDC_IEC_559__)
    release = "1 (__STDC_IEC_559__)";
#elif defined(ESP_PLATFORM)
    release = "1 (ESP_PLATFORM)";
#else
    release = "0 (portable conversion)";
#endif
    g_probe = "float_config";
    env("float.release_ua_float_ieee754", "%s", release);
#ifdef __GCC_IEC_559
    env("macro.__GCC_IEC_559", "%d", (int)__GCC_IEC_559);
#else
    env("macro.__GCC_IEC_559", "not defined");
#endif
}

/* ------------------------------------------- change 01: FD_SET() overflow */

/* What FD_SET() does with a descriptor of FD_SETSIZE or more: write past the
 * fd_set (here into guard bytes), ignore it, or stop the program (glibc with
 * _FORTIFY_SOURCE aborts). In a child process, so that an abort ends only the
 * child. */
static void fdset_above_setsize(void) {
    pid_t pid;
    int st = 0;

    g_probe = "fdset_above_setsize";
    fflush(stdout);
    pid = fork();
    if(pid == 0) {
        /* the fd_set and 64 guard bytes behind it; the pointer is opaque to
         * the compiler, which could otherwise assume that a store into the
         * fd_set leaves the guard bytes alone */
        static long area[sizeof(fd_set) / sizeof(long) + 64 / sizeof(long)];
        fd_set *volatile set = (fd_set *)(void *)area;
        const unsigned char *guard = (const unsigned char *)(void *)area + sizeof(fd_set);
        volatile int fd = FD_SETSIZE + 8; /* not a constant: no check at compile time */
        size_t changed = 0;
        FD_ZERO(set);
        FD_SET(fd, set);
        for(size_t i = 0; i < 64; i++)
            changed += guard[i] != 0;
        _exit(changed ? 10 : 11);
    }
    if(pid < 0) {
        env("fd.FD_SET_above_setsize", "fork FAILED(%s)", ename(errno));
        return;
    }
    while(waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    if(WIFEXITED(st) && WEXITSTATUS(st) == 10)
        env("fd.FD_SET_above_setsize", "writes past the fd_set");
    else if(WIFEXITED(st) && WEXITSTATUS(st) == 11)
        env("fd.FD_SET_above_setsize", "writes nothing past the fd_set");
    else if(WIFSIGNALED(st))
        env("fd.FD_SET_above_setsize", "stops the program (signal %d)", WTERMSIG(st));
    else
        env("fd.FD_SET_above_setsize", "child status 0x%x", (unsigned)st);
}

/* ------------------------------------------------------------------ main */

static void multicast_groups(void) {
    multicast("239.192.41.62", "group");
    multicast("224.0.0.22", "examples");
}

static int g_failed; /* probes that crashed or hung */

/* Runs one probe in a child process with its own watchdog: a crash or a
 * hang ends that probe only, the others still report. */
static void isolated(const char *name, void (*probe)(void)) {
    char key[64];
    pid_t pid;
    int st = 0;

    fflush(stdout);
    pid = fork();
    if(pid < 0) {
        probe();
        return;
    }
    if(pid == 0) {
        alarm(60);
        probe();
        fflush(stdout);
        _exit(0);
    }
    while(waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    snprintf(key, sizeof(key), "probe_failed.%s", name);
    if(WIFSIGNALED(st)) {
        env(key, "signal %d", WTERMSIG(st));
        g_failed++;
    } else if(WIFEXITED(st) && WEXITSTATUS(st) != 0) {
        env(key, "exit %d", WEXITSTATUS(st));
        g_failed++;
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGALRM, on_alarm);

    if(argc > 1 && strcmp(argv[1], "nopipe") == 0) {
        alarm(60);
        nopipe();
        env("nopipe.done", "1");
        return 0;
    }

    alarm(120);
    basics(); /* in this process: interfaces() finds the interface for multicast() */
    float_config();
    alarm(0);
    isolated("high_descriptors", high_descriptors);
    isolated("fdset_above_setsize", fdset_above_setsize);
    isolated("poll_semantics", poll_semantics);
    isolated("errno_semantics", errno_semantics);
    isolated("tcp_semantics", tcp_semantics);
    isolated("accept_after_abort", accept_after_abort);
    isolated("accept_emfile", accept_emfile);
    isolated("tcp_errno", tcp_errno);
    isolated("udp_semantics", udp_semantics);
    isolated("multicast", multicast_groups);
    isolated("misc", misc);
    isolated("timing", timing);
    env("done", "1");
    return g_failed ? 3 : 0; /* as the watchdog: vm/run.sh reports the exit status */
}

