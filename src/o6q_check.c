/* o6q_check: the on-target check of the open62541 port.
 *
 *   o6q_check <core|security|pubsub|highfd|robust|nopipe|all> [port] [n]
 *
 * Every suite starts its own servers and clients inside this process and
 * prints one "O6Q ..." line per result (see o6q_common.h).  The exit status
 * is 0 if no test failed, 1 if one did, 2 for a usage error, 3 after the
 * watchdog and 4 after a crash.
 *
 * SPDX-License-Identifier: Apache-2.0 */
#include "o6q_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

typedef int (*SuiteFn)(unsigned port, int n);

static const struct {
    const char *name;
    SuiteFn fn;
    const char *what;
} suites[] = {
    {"core", o6q_suite_core, "client/server services over TCP, timers, restart"},
    {"security", o6q_suite_security, "certificates, three security policies, user login"},
    {"pubsub", o6q_suite_pubsub, "PubSub UADP over UDP, unicast and multicast"},
    {"highfd", o6q_suite_highfd, "descriptor numbers above FD_SETSIZE"},
    {"robust", o6q_suite_robust, "peers that reset, close or do not listen"},
    {"nopipe", o6q_suite_nopipe, "a system without the pipe manager (not part of \"all\")"},
};
#define N_SUITES (sizeof(suites) / sizeof(suites[0]))

static int usage(const char *prog) {
    fprintf(stderr, "usage: %s <suite|all> [port] [n]\n", prog);
    for(size_t i = 0; i < N_SUITES; i++)
        fprintf(stderr, "  %-9s %s\n", suites[i].name, suites[i].what);
    fprintf(stderr, "  port: first TCP/UDP port to use, default 4840 (up to port+12 are used)\n"
                    "  n:    repetitions of the timed loops, default per suite\n");
    return 2;
}

int main(int argc, char **argv) {
    struct utsname un;
    unsigned port = 4840;
    int n = 0, failures = 0, ran = 0;

    if(argc < 2)
        return usage(argv[0]);
    if(argc > 2) {
        long p = strtol(argv[2], NULL, 10);
        if(p < 1024 || p > 65000)
            return usage(argv[0]);
        port = (unsigned)p;
    }
    if(argc > 3) {
        n = atoi(argv[3]);
        if(n < 0)
            return usage(argv[0]);
    }

    memset(&un, 0, sizeof(un));
    uname(&un);
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("O6Q BUILD open62541=%s eventloop=%s"
#ifdef O6Q_VARIANT
           " variant=" O6Q_VARIANT
#endif
           " float_ieee754=%d compiler=%s os=%s release=%s machine=%s\n",
           UA_OPEN62541_VERSION, O6Q_EVENTLOOP, UA_FLOAT_IEEE754, __VERSION__, un.sysname,
           un.release, un.machine);

    for(size_t i = 0; i < N_SUITES; i++) {
        int all = strcmp(argv[1], "all") == 0;
        if(all ? strcmp(suites[i].name, "nopipe") == 0 : strcmp(argv[1], suites[i].name) != 0)
            continue;
        failures += suites[i].fn(port, n);
        ran++;
    }
    if(!ran)
        return usage(argv[0]);
    return failures ? 1 : 0;
}
