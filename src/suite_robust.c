/* Suite "robust": what peers that misbehave do to a server and a client.
 *
 * Each test puts the library into a situation that every network service
 * meets sooner or later, and that a BSD-derived network stack (io-sock of
 * QNX 8.0) answers differently from Linux:
 *
 *   idle_client      a client that iterates while it has no connection
 *   refused          a connection attempt to a port without a listener
 *   aborted_connect  a peer that resets its connection before accept()
 *   peer_close       a peer that closes while errno still holds EAGAIN
 *
 * See the changes eventloop-ppoll, tcp-accept-aborted and tcp-recv-eof in
 * patches/ for what each of them is about.
 *
 * Suite "nopipe": the server on a system without the QNX pipe manager (change
 * interrupt-selfpipe).  It only tests something where pipe() fails; the
 * target script stops the pipe manager before it runs this suite.
 *
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef _QNX_SOURCE
#define _QNX_SOURCE
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "o6q_common.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NODE(name) UA_NODEID_STRING(O6Q_NS, name)

#define RB_ROUNDS 5
/* larger than what the loopback socket buffers take while the client does not
 * read, so that the server's send() meets EAGAIN */
#define RB_BIG (8u * 1024u * 1024u)
#define RB_CLOSE_WAIT_MS 3000.0

/* A plain TCP connection to the loopback address, without OPC UA on it. */
static int raw_tcp(unsigned port) {
    struct sockaddr_in a;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0)
        return -1;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if(connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

/* close() that sends a reset instead of the orderly FIN */
static void tcp_reset(int fd) {
    struct linger l;
    l.l_onoff = 1;
    l.l_linger = 0;
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
    close(fd);
}

/* ------------------------------------------------------------ idle_client */

/* An event loop without a descriptor: a client that is not connected. The
 * application calls UA_Client_run_iterate(client, timeout) in its loop and
 * relies on the call to wait. */
static void test_idle_client(void) {
    UA_Client *c;
    double t0, ms;
    const unsigned rounds = 10, timeout_ms = 50;

    o6q_step("idle_client");
    c = o6q_client_new();
    if(!c) {
        CHECK("idle_client", 0, "no client");
        return;
    }
    t0 = o6q_now_us();
    for(unsigned i = 0; i < rounds; i++)
        UA_Client_run_iterate(c, timeout_ms);
    ms = (o6q_now_us() - t0) / 1000.0;
    CHECK("idle_client", ms >= 0.8 * rounds * timeout_ms && ms < 20.0 * rounds * timeout_ms,
          "%u x UA_Client_run_iterate(client, %u ms) without a connection took %.1f ms "
          "(%u ms if every call waits; near 0 means that the loop of the application spins)",
          rounds, timeout_ms, ms, rounds * timeout_ms);
    UA_Client_delete(c);
}

/* ---------------------------------------------------------------- refused */

static void test_refused(unsigned port) {
    UA_Client *c;
    UA_StatusCode rc;
    char url[64];
    double t0, ms;

    o6q_step("refused");
    snprintf(url, sizeof(url), "opc.tcp://127.0.0.1:%u", port);
    c = o6q_client_new();
    if(!c) {
        CHECK("refused", 0, "no client");
        return;
    }
    o6q_expect_errors(1);
    t0 = o6q_now_us();
    rc = UA_Client_connect(c, url);
    ms = (o6q_now_us() - t0) / 1000.0;
    o6q_expect_errors(0);
    CHECK("refused", rc != UA_STATUSCODE_GOOD && ms < 10000.0,
          "connect to %s, where nothing listens -> %s after %.1f ms", url, SC(rc), ms);
    UA_Client_delete(c);
}

/* -------------------------------------------------------- aborted_connect */

/* A peer completes the TCP handshake and resets the connection before the
 * server has called accept().  To have that order for certain, the test holds
 * the lock of the server's event loop meanwhile: the handshake is done by the
 * network stack, the connection waits in the listen queue and is reset
 * there. */
static void test_aborted_connect(unsigned port) {
    static O6QModel model;
    O6QServer srv;
    UA_Client *before = NULL, *after = NULL;
    UA_EventLoop *el;
    UA_StatusCode rc, rc_before, rc_after;
    UA_Int32 v = 0;
    char url[64];
    int reset = 0, refused = 0;

    o6q_step("aborted_connect");
    snprintf(url, sizeof(url), "opc.tcp://127.0.0.1:%u", port);
    rc = o6q_server_start_model(&srv, &model, port);
    if(rc != UA_STATUSCODE_GOOD) {
        CHECK("aborted_connect", 0, "server on port %u -> %s", port, SC(rc));
        return;
    }
    before = o6q_client_new();
    rc_before = before ? UA_Client_connect(before, url) : UA_STATUSCODE_BADOUTOFMEMORY;

    el = UA_Server_getConfig(srv.server)->eventLoop;
    for(int i = 0; i < RB_ROUNDS; i++) {
        int fd;
        el->lock(el);
        fd = raw_tcp(port);
        if(fd >= 0) {
            tcp_reset(fd);
            reset++;
        } else {
            refused++;
        }
        /* the reset arrives while the server still cannot accept; the network
         * stack is another process, and the CPU may be emulated */
        o6q_sleep_ms(200);
        el->unlock(el);
        o6q_sleep_ms(50); /* now the server looks at its listen socket */
    }

    rc_after = UA_STATUSCODE_BADOUTOFMEMORY;
    after = o6q_client_new();
    if(after) {
        rc_after = UA_Client_connect(after, url);
        if(rc_after == UA_STATUSCODE_GOOD)
            rc_after = o6q_read_i32(after, "answer", &v);
    }
    CHECK("aborted_connect",
          rc_before == UA_STATUSCODE_GOOD && reset == RB_ROUNDS && rc_after == UA_STATUSCODE_GOOD &&
              v == 42,
          "%d of %d connections were reset while they waited in the listen queue (%d refused); "
          "a new client after that: %s",
          reset, RB_ROUNDS, refused, SC(rc_after));

    if(after) {
        UA_Client_disconnect(after);
        UA_Client_delete(after);
    }
    if(before) {
        UA_Client_disconnect(before);
        UA_Client_delete(before);
    }
    o6q_server_stop(&srv);
}

/* ------------------------------------------------------------- peer_close */

typedef struct {
    volatile int done;
    UA_StatusCode status;
    size_t length;
} BigRead;

static void big_read_cb(UA_Client *client, void *userdata, UA_UInt32 request_id,
                        UA_ReadResponse *rr) {
    BigRead *b = (BigRead *)userdata;
    (void)client; (void)request_id;
    b->status = rr->responseHeader.serviceResult;
    if(b->status == UA_STATUSCODE_GOOD) {
        if(rr->resultsSize != 1)
            b->status = UA_STATUSCODE_BADUNEXPECTEDERROR;
        else if(rr->results[0].hasStatus && rr->results[0].status != UA_STATUSCODE_GOOD)
            b->status = rr->results[0].status;
        else if(rr->results[0].hasValue &&
                UA_Variant_hasScalarType(&rr->results[0].value, &UA_TYPES[UA_TYPES_BYTESTRING]))
            b->length = ((const UA_ByteString *)rr->results[0].value.data)->length;
        else
            b->status = UA_STATUSCODE_BADTYPEMISMATCH;
    }
    b->done = 1;
}

/* A client is slow to read a large response: the socket buffer of the server
 * fills up, send() reports EAGAIN, the server waits and goes on.  Meanwhile
 * another peer closes its connection.  recv() returns 0 for that one, and
 * errno still holds the EAGAIN of the send(). */
/* Whether an accepted socket inherits O_NONBLOCK from the listen socket (1)
 * or not (0); -1 if that cannot be found out. */
static int accept_inherits_nonblock(void) {
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    int l, c = -1, s = -1, res = -1, flags;
    l = socket(AF_INET, SOCK_STREAM, 0);
    if(l < 0)
        return -1;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if(bind(l, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(l, 1) != 0 ||
       getsockname(l, (struct sockaddr *)&a, &len) != 0)
        goto done;
    flags = fcntl(l, F_GETFL);
    if(flags < 0 || fcntl(l, F_SETFL, flags | O_NONBLOCK) != 0)
        goto done;
    c = socket(AF_INET, SOCK_STREAM, 0);
    if(c < 0 || connect(c, (struct sockaddr *)&a, sizeof(a)) != 0)
        goto done;
    for(int i = 0; i < 100 && s < 0; i++) {
        s = accept(l, NULL, NULL);
        if(s < 0) {
            if(errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                goto done;
            o6q_sleep_ms(10);
        }
    }
    if(s >= 0) {
        flags = fcntl(s, F_GETFL);
        if(flags >= 0)
            res = (flags & O_NONBLOCK) ? 1 : 0;
    }
done:
    if(s >= 0)
        close(s);
    if(c >= 0)
        close(c);
    close(l);
    return res;
}

static void test_peer_close(unsigned port) {
    static O6QModel model;
    O6QServer srv;
    UA_Client *c = NULL;
    BigRead big;
    UA_StatusCode rc;
    char url[64];
    int raw = -1, closed = 0;
    double t0, read_ms = 0, close_ms = 0, cpu0, cpu1;

    memset(&big, 0, sizeof(big));
    big.status = UA_STATUSCODE_BADINTERNALERROR;

    o6q_step("peer_close");
    /* The release can only fail here if the server's send() returns EAGAIN,
     * which needs a non-blocking accepted socket. The server never sets that
     * itself: it relies on the listen socket passing it on. */
    if(accept_inherits_nonblock() == 0) {
        o6q_skip("peer_close", "accepted sockets do not inherit O_NONBLOCK here, so send() "
                               "blocks instead of returning EAGAIN: the release cannot fail");
        return;
    }
    snprintf(url, sizeof(url), "opc.tcp://127.0.0.1:%u", port);
    rc = o6q_server_start_model(&srv, &model, port);
    if(rc != UA_STATUSCODE_GOOD) {
        CHECK("peer_close", 0, "server on port %u -> %s", port, SC(rc));
        return;
    }

    { /* the large value, written on the server side */
        UA_ByteString blob;
        UA_Variant var;
        rc = UA_ByteString_allocBuffer(&blob, RB_BIG);
        if(rc == UA_STATUSCODE_GOOD) {
            memset(blob.data, 0x5a, RB_BIG);
            UA_Variant_setScalar(&var, &blob, &UA_TYPES[UA_TYPES_BYTESTRING]);
            rc = UA_Server_writeValue(srv.server, NODE("blob"), var);
            UA_ByteString_clear(&blob);
        }
    }
    if(rc == UA_STATUSCODE_GOOD) {
        c = o6q_client_new();
        rc = c ? UA_Client_connect(c, url) : UA_STATUSCODE_BADOUTOFMEMORY;
    }
    if(rc == UA_STATUSCODE_GOOD) {
        raw = raw_tcp(port);
        if(raw < 0)
            rc = UA_STATUSCODE_BADCONNECTIONREJECTED;
    }
    if(rc != UA_STATUSCODE_GOOD) {
        CHECK("peer_close", 0, "preparation -> %s", SC(rc));
        goto cleanup;
    }
    o6q_sleep_ms(200); /* the server has accepted the second connection */

    { /* ask for the large value and do not read the answer for a moment */
        UA_ReadRequest req;
        UA_ReadValueId id;
        UA_ReadRequest_init(&req);
        UA_ReadValueId_init(&id);
        id.nodeId = NODE("blob");
        id.attributeId = UA_ATTRIBUTEID_VALUE;
        req.nodesToRead = &id;
        req.nodesToReadSize = 1;
        req.timestampsToReturn = UA_TIMESTAMPSTORETURN_NEITHER;
        t0 = o6q_now_us();
        rc = UA_Client_sendAsyncReadRequest(c, &req, big_read_cb, &big, NULL);
    }
    if(rc != UA_STATUSCODE_GOOD) {
        CHECK("peer_close", 0, "read request -> %s", SC(rc));
        goto cleanup;
    }
    o6q_sleep_ms(50);

    /* The second peer closes its side while the server is busy sending. */
    shutdown(raw, SHUT_WR);

    while(!big.done && (o6q_now_us() - t0) < 60e6)
        UA_Client_run_iterate(c, 50);
    read_ms = (o6q_now_us() - t0) / 1000.0;

    /* The server has to notice the closed connection and close its side. */
    cpu0 = o6q_thread_cpu_us(srv.thread);
    t0 = o6q_now_us();
    while(!closed && (o6q_now_us() - t0) / 1000.0 < RB_CLOSE_WAIT_MS) {
        struct pollfd p;
        p.fd = raw;
        p.events = POLLIN;
        p.revents = 0;
        if(poll(&p, 1, 50) > 0) {
            char byte;
            ssize_t k = recv(raw, &byte, 1, 0);
            if(k == 0 || (k < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
                closed = 1;
        }
    }
    close_ms = (o6q_now_us() - t0) / 1000.0;
    cpu1 = o6q_thread_cpu_us(srv.thread);

    CHECK("peer_close",
          big.done && big.status == UA_STATUSCODE_GOOD && big.length == RB_BIG && closed,
          "%u bytes read by a client that paused for 50 ms -> %s in %.0f ms; the connection of a "
          "peer that had closed meanwhile was %s %.0f ms; the server thread used %.0f ms of CPU "
          "time while that was waited for",
          (unsigned)big.length, big.done ? SC(big.status) : "no answer", read_ms,
          closed ? "closed by the server after" : "still open after", close_ms,
          (cpu0 >= 0 && cpu1 >= 0) ? (cpu1 - cpu0) / 1000.0 : -1.0);

cleanup:
    if(raw >= 0)
        close(raw);
    if(c) {
        UA_Client_disconnect(c);
        UA_Client_delete(c);
    }
    o6q_server_stop(&srv);
}

/* ------------------------------------------------------------------ suite */

int o6q_suite_robust(unsigned port, int n) {
    int fds0, fds1;
    (void)n;

    o6q_begin("robust", 300);

    /* Whatever the process opens once and keeps is open after this. */
    o6q_step("baseline");
    {
        static O6QModel model;
        O6QServer srv;
        UA_StatusCode rc = o6q_server_start_model(&srv, &model, port + 3);
        o6q_server_stop(&srv);
        CHECK("baseline", rc == UA_STATUSCODE_GOOD, "server started and stopped -> %s", SC(rc));
        if(rc != UA_STATUSCODE_GOOD)
            return o6q_end();
    }
    fds0 = o6q_open_fds();

    test_idle_client();
    test_refused(port + 4);
    test_aborted_connect(port + 5);
    test_peer_close(port + 6);

    o6q_step("descriptors");
    fds1 = o6q_open_fds();
    CHECK("descriptors", fds1 == fds0, "open descriptors before and after the tests: %d -> %d", fds0,
          fds1);
    return o6q_end();
}

/* ----------------------------------------------------------------- nopipe */

int o6q_suite_nopipe(unsigned port, int n) {
    static O6QModel model;
    O6QServer srv;
    UA_Client *c = NULL;
    UA_StatusCode rc;
    UA_Int32 v = 0;
    char url[64];
    int p[2], pipe_errno = 0;
    double t0;
    (void)n;

    o6q_begin("nopipe", 120);
    snprintf(url, sizeof(url), "opc.tcp://127.0.0.1:%u", port + 7);

    o6q_step("pipe");
    if(pipe(p) == 0) {
        close(p[0]);
        close(p[1]);
        o6q_skip("server", "pipe() works here, so the pipe manager is running; the suite is for "
                           "a system without it");
        o6q_skip("client", "pipe() works here");
        return o6q_end();
    }
    pipe_errno = errno;
    o6q_info("pipe() fails: %s", strerror(pipe_errno));

    o6q_step("server");
    t0 = o6q_now_us();
    rc = o6q_server_start_model(&srv, &model, port + 7);
    CHECK("server", rc == UA_STATUSCODE_GOOD,
          "server start while pipe() fails (%s) -> %s in %.0f ms", strerror(pipe_errno), SC(rc),
          (o6q_now_us() - t0) / 1000.0);
    if(rc != UA_STATUSCODE_GOOD) {
        CHECK("client", 0, "not run: the server did not start (%s)", SC(rc));
        return o6q_end();
    }

    o6q_step("client");
    c = o6q_client_new();
    rc = c ? UA_Client_connect(c, url) : UA_STATUSCODE_BADOUTOFMEMORY;
    if(rc == UA_STATUSCODE_GOOD)
        rc = o6q_read_i32(c, "answer", &v);
    CHECK("client", rc == UA_STATUSCODE_GOOD && v == 42, "connect and read -> %s, value %d", SC(rc),
          (int)v);
    if(c) {
        UA_Client_disconnect(c);
        UA_Client_delete(c);
    }

    o6q_step("stop");
    t0 = o6q_now_us();
    o6q_server_stop(&srv);
    CHECK("stop", 1, "server stopped in %.0f ms", (o6q_now_us() - t0) / 1000.0);
    return o6q_end();
}
