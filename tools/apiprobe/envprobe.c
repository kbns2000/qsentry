/* o6q_envprobe: print the run-time facts of a QNX 8.0 target that matter to a
 * network library port.  One fact per line: "O6Q ENV <key>=<value>".
 * It only reads system state; it changes nothing.
 * SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#ifdef __QNXNTO__
#include <sys/neutrino.h>
#include <sys/syspage.h>
#endif
#ifdef O6Q_HAVE_OPENSSL
#include <openssl/crypto.h>
#include <openssl/rand.h>
#endif

static void resolve(const char *name) {
    struct addrinfo hints, *res = NULL, *ai;
    char buf[INET6_ADDRSTRLEN];
    int rc, n = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(name, "4840", &hints, &res);
    if(rc != 0) {
        printf("O6Q ENV resolve.%s=FAILED(%s)\n", name, gai_strerror(rc));
        return;
    }
    for(ai = res; ai; ai = ai->ai_next) {
        const void *a = (ai->ai_family == AF_INET) ?
            (const void*)&((struct sockaddr_in*)ai->ai_addr)->sin_addr :
            (const void*)&((struct sockaddr_in6*)ai->ai_addr)->sin6_addr;
        if(ai->ai_family != AF_INET && ai->ai_family != AF_INET6)
            continue;
        printf("O6Q ENV resolve.%s.%d=%s\n", name, n++,
               inet_ntop(ai->ai_family, a, buf, sizeof(buf)) ? buf : "?");
    }
    freeaddrinfo(res);
}

static void passive(void) {
    struct addrinfo hints, *res = NULL, *ai;
    int rc, n = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    rc = getaddrinfo(NULL, "4840", &hints, &res);
    if(rc != 0) {
        printf("O6Q ENV passive=FAILED(%s)\n", gai_strerror(rc));
        return;
    }
    for(ai = res; ai; ai = ai->ai_next)
        printf("O6Q ENV passive.%d=%s\n", n++,
               ai->ai_family == AF_INET ? "AF_INET" :
               ai->ai_family == AF_INET6 ? "AF_INET6" : "other");
    freeaddrinfo(res);
}

static void try_socket(const char *what, int family, int type) {
    int fd = socket(family, type, 0);
    if(fd < 0) {
        printf("O6Q ENV socket.%s=FAILED(%s)\n", what, strerror(errno));
        return;
    }
    printf("O6Q ENV socket.%s=ok(fd=%d)\n", what, fd);
    close(fd);
}

static void interfaces(void) {
    struct ifaddrs *ifa = NULL, *i;
    char buf[INET6_ADDRSTRLEN];
    if(getifaddrs(&ifa) != 0) {
        printf("O6Q ENV ifaddrs=FAILED(%s)\n", strerror(errno));
        return;
    }
    for(i = ifa; i; i = i->ifa_next) {
        int fam;
        const void *a;
        if(!i->ifa_addr)
            continue;
        fam = i->ifa_addr->sa_family;
        if(fam == AF_INET)
            a = &((struct sockaddr_in*)i->ifa_addr)->sin_addr;
        else if(fam == AF_INET6)
            a = &((struct sockaddr_in6*)i->ifa_addr)->sin6_addr;
        else
            continue;
        printf("O6Q ENV if.%s=%s idx=%u%s%s%s%s\n", i->ifa_name,
               inet_ntop(fam, a, buf, sizeof(buf)) ? buf : "?",
               if_nametoindex(i->ifa_name),
               (i->ifa_flags & IFF_UP) ? " UP" : "",
               (i->ifa_flags & IFF_RUNNING) ? " RUNNING" : "",
               (i->ifa_flags & IFF_LOOPBACK) ? " LOOPBACK" : "",
               (i->ifa_flags & IFF_MULTICAST) ? " MULTICAST" : "");
    }
    freeifaddrs(ifa);
}

int main(void) {
    struct utsname u;
    struct timespec ts;
    char host[256] = "";
    pthread_attr_t attr;
    size_t stack = 0;
    int fd;

    if(uname(&u) == 0)
        printf("O6Q ENV uname=%s %s %s %s\n", u.sysname, u.release, u.version, u.machine);
    gethostname(host, sizeof(host) - 1);
    printf("O6Q ENV hostname=%s\n", host);
    printf("O6Q ENV time_utc=%lld\n", (long long)time(NULL));
    printf("O6Q ENV sizeof.long=%u sizeof.ptr=%u sizeof.time_t=%u sizeof.fd_set=%u\n",
           (unsigned)sizeof(long), (unsigned)sizeof(void*), (unsigned)sizeof(time_t),
           (unsigned)sizeof(fd_set));
    printf("O6Q ENV FD_SETSIZE=%d open_max=%ld nproc=%ld pagesize=%ld\n", (int)FD_SETSIZE,
           sysconf(_SC_OPEN_MAX), sysconf(_SC_NPROCESSORS_ONLN), sysconf(_SC_PAGESIZE));
    if(clock_getres(CLOCK_MONOTONIC, &ts) == 0)
        printf("O6Q ENV clock.monotonic.res_ns=%ld\n", (long)(ts.tv_sec * 1000000000L + ts.tv_nsec));
    if(clock_getres(CLOCK_REALTIME, &ts) == 0)
        printf("O6Q ENV clock.realtime.res_ns=%ld\n", (long)(ts.tv_sec * 1000000000L + ts.tv_nsec));
#ifdef __QNXNTO__
    printf("O6Q ENV qnx.num_cpu=%u cycles_per_sec=%llu\n", (unsigned)_syspage_ptr->num_cpu,
           (unsigned long long)SYSPAGE_ENTRY(qtime)->cycles_per_sec);
    {
        struct _clockperiod cp;
        if(ClockPeriod(CLOCK_REALTIME, NULL, &cp, 0) == 0)
            printf("O6Q ENV qnx.clockperiod_ns=%lu\n", (unsigned long)cp.nsec);
    }
#endif
    pthread_attr_init(&attr);
    pthread_attr_getstacksize(&attr, &stack);
    pthread_attr_destroy(&attr);
    printf("O6Q ENV pthread.default_stack=%lu\n", (unsigned long)stack);

    try_socket("tcp4", AF_INET, SOCK_STREAM);
    try_socket("tcp6", AF_INET6, SOCK_STREAM);
    try_socket("udp4", AF_INET, SOCK_DGRAM);
    try_socket("udp6", AF_INET6, SOCK_DGRAM);
    resolve("localhost");
    resolve("127.0.0.1");
    resolve("::1");
    if(host[0])
        resolve(host);
    passive();
    interfaces();

    fd = open("/dev/random", O_RDONLY | O_NONBLOCK);
    if(fd < 0) {
        printf("O6Q ENV dev_random=FAILED(%s)\n", strerror(errno));
    } else {
        unsigned char b[16];
        ssize_t n = read(fd, b, sizeof(b));
        printf("O6Q ENV dev_random=read %ld of %u bytes\n", (long)n, (unsigned)sizeof(b));
        close(fd);
    }
#ifdef O6Q_HAVE_OPENSSL
    {
        unsigned char b[16];
        printf("O6Q ENV openssl.version=%s\n", OpenSSL_version(OPENSSL_VERSION));
        printf("O6Q ENV openssl.rand_bytes=%s\n", RAND_bytes(b, sizeof(b)) == 1 ? "ok" : "FAILED");
    }
#endif
    printf("O6Q ENV done=1\n");
    return 0;
}
