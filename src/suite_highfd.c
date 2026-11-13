/* Suite "highfd": sockets whose descriptor number is FD_SETSIZE or higher.
 *
 * select() cannot watch such a descriptor, and FD_SET() with it writes
 * outside of the fd_set.  open62541 waits in epoll on Linux; on every other
 * POSIX system its event loop is the select() one, and that one does not
 * check the descriptor number.  Where FD_SETSIZE is smaller than the number
 * of descriptors a process may open, the case is reachable.  This port waits
 * in ppoll() instead.
 *
 * The suite occupies the descriptors below FD_SETSIZE and then runs a server
 * and clients.  It is run twice: with the event loop of the port and, as
 * "highfd@upstream", with the unchanged release (select() on QNX).
 *
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef _QNX_SOURCE
#define _QNX_SOURCE
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "o6q_common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <unistd.h>

#define HF_MARGIN 24  /* descriptors above FD_SETSIZE that are occupied, too */
#define HF_CLIENTS 8
#define HF_READS 100

int o6q_suite_highfd(unsigned port, int n) {
    static O6QModel model;
    O6QServer srv;
    UA_Client *c[HF_CLIENTS + 1];
    struct rlimit rl;
    int target = FD_SETSIZE + HF_MARGIN, want = FD_SETSIZE + HF_MARGIN + 64;
    int *filler = NULL, nfill = 0, highest = -1, probe_fd, fds0;
    int connected = 0, good = 0;
    char url[64];
    UA_StatusCode rc, first_error = UA_STATUSCODE_GOOD;
    double t0;
    (void)n;

    memset(&srv, 0, sizeof(srv));
    memset(c, 0, sizeof(c));
    snprintf(url, sizeof(url), "opc.tcp://127.0.0.1:%u", port);

    o6q_begin("highfd", 120);
    fds0 = o6q_open_fds();

    o6q_step("occupy");
    if(getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        o6q_skip("occupy", "getrlimit(RLIMIT_NOFILE): %s", strerror(errno));
        return o6q_end();
    }
    o6q_info("FD_SETSIZE=%d RLIMIT_NOFILE soft=%ld hard=%ld eventloop=%s", (int)FD_SETSIZE,
             rl.rlim_cur == RLIM_INFINITY ? -1L : (long)rl.rlim_cur,
             rl.rlim_max == RLIM_INFINITY ? -1L : (long)rl.rlim_max, O6Q_EVENTLOOP);
    if(rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < (rlim_t)want) {
        /* Only here the limit is raised; note it, because then the case is
         * not reachable with the defaults of this system. */
        struct rlimit more = rl;
        more.rlim_cur = (rlim_t)want;
        if((rl.rlim_max == RLIM_INFINITY || rl.rlim_max >= (rlim_t)want) &&
           setrlimit(RLIMIT_NOFILE, &more) == 0) {
            o6q_info("soft limit raised from %ld to %d for this test", (long)rl.rlim_cur, want);
        } else {
            o6q_skip("occupy", "a process may open %ld descriptors here; FD_SETSIZE %d cannot be "
                               "exceeded",
                     (long)rl.rlim_cur, (int)FD_SETSIZE);
            return o6q_end();
        }
    }
    filler = (int *)malloc(sizeof(int) * (size_t)(target + 1));
    if(!filler) {
        CHECK("occupy", 0, "out of memory");
        return o6q_end();
    }
    while(nfill <= target) {
        int fd = open("/dev/null", O_RDONLY);
        if(fd < 0)
            break;
        filler[nfill++] = fd;
        highest = fd;
        if(fd >= target)
            break;
    }
    probe_fd = open("/dev/null", O_RDONLY);
    if(probe_fd >= 0)
        close(probe_fd);
    CHECK("occupy", highest >= target && probe_fd > FD_SETSIZE,
          "descriptors up to %d are in use (%d opened here); the next one is %d, FD_SETSIZE is %d",
          highest, nfill, probe_fd, (int)FD_SETSIZE);
    if(highest < target || probe_fd <= FD_SETSIZE)
        goto cleanup;

    /* Every socket from here on has a number above FD_SETSIZE. */
    o6q_step("server");
    t0 = o6q_now_us();
    rc = o6q_server_start_model(&srv, &model, port);
    CHECK("server", rc == UA_STATUSCODE_GOOD, "server on port %u -> %s in %.0f ms", port, SC(rc),
          (o6q_now_us() - t0) / 1000.0);
    if(rc != UA_STATUSCODE_GOOD)
        goto cleanup;

    o6q_step("client");
    c[0] = o6q_client_new();
    rc = c[0] ? UA_Client_connect(c[0], url) : UA_STATUSCODE_BADOUTOFMEMORY;
    for(int i = 0; i < HF_READS && rc == UA_STATUSCODE_GOOD; i++) {
        UA_Int32 v = 0;
        if(o6q_read_i32(c[0], "answer", &v) == UA_STATUSCODE_GOOD && v == 42)
            good++;
    }
    CHECK("client", rc == UA_STATUSCODE_GOOD && good == HF_READS,
          "connect -> %s, %d of %d reads returned the value", SC(rc), good, HF_READS);

    /* More connections: more descriptors for both event loops to wait on.
     * No more attempts after the first failure: each one costs the whole
     * client timeout (30 s), and the watchdog would end the suite before the
     * step "idle". */
    o6q_step("sessions");
    good = 0;
    first_error = rc; /* the connect of the step "client" */
    for(int i = 1; i <= HF_CLIENTS && first_error == UA_STATUSCODE_GOOD; i++) {
        rc = UA_STATUSCODE_BADOUTOFMEMORY;
        c[i] = o6q_client_new();
        if(c[i])
            rc = UA_Client_connect(c[i], url);
        if(rc == UA_STATUSCODE_GOOD)
            connected++;
        else
            first_error = rc;
    }
    for(int round = 0; round < 10; round++) {
        for(int i = 1; i <= HF_CLIENTS; i++) {
            UA_Int32 v = 0;
            if(c[i] && o6q_read_i32(c[i], "answer", &v) == UA_STATUSCODE_GOOD && v == 42)
                good++;
        }
    }
    CHECK("sessions", connected == HF_CLIENTS && good == HF_CLIENTS * 10,
          "%d of %d more clients connected (%s); %d of %d reads returned the value", connected,
          HF_CLIENTS, SC(first_error), good, HF_CLIENTS * 10);

    /* A select() loop that answers but never sleeps would pass the steps
     * above. Without requests the server thread has to wait. */
    o6q_step("idle");
    {
        double c0 = o6q_thread_cpu_us(srv.thread), w0 = o6q_now_us(), c1, w;
        o6q_sleep_ms(1000);
        c1 = o6q_thread_cpu_us(srv.thread);
        w = o6q_now_us() - w0;
        if(c0 < 0 || c1 < 0)
            o6q_skip("idle", "no CPU clock for the server thread");
        else
            CHECK("idle", c1 - c0 < 0.5 * w,
                  "the server thread used %.0f ms of CPU time in %.0f ms without requests",
                  (c1 - c0) / 1000.0, w / 1000.0);
    }

cleanup:
    o6q_step("stop");
    for(int i = 0; i <= HF_CLIENTS; i++) {
        if(c[i]) {
            UA_Client_disconnect(c[i]);
            UA_Client_delete(c[i]);
        }
    }
    o6q_server_stop(&srv);
    for(int i = 0; i < nfill; i++)
        close(filler[i]);
    free(filler);
    {
        int fds1 = o6q_open_fds();
        /* The first server of a process may leave descriptors that the
         * process keeps (random numbers): informative only. */
        CHECK("stop", 1, "everything closed; open descriptors %d -> %d", fds0, fds1);
    }
    return o6q_end();
}
