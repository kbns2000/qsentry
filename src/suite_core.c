/* Suite "core": the OPC UA client/server services over TCP on the target,
 * without encryption.  SPDX-License-Identifier: Apache-2.0 */
#ifndef _QNX_SOURCE
#define _QNX_SOURCE
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "o6q_common.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NODE(name) UA_NODEID_STRING(O6Q_NS, name)

typedef struct {
    unsigned port;
    int n;
    O6QServer srv;
    O6QModel *model;
    UA_Client *client; /* the long-lived session most tests use */
    char url[64];
} Core;

/* ---------------------------------------------------------------- helpers */

static UA_StatusCode write_i32(UA_Client *c, char *name, UA_Int32 value) {
    UA_Variant v;
    UA_Variant_setScalar(&v, &value, &UA_TYPES[UA_TYPES_INT32]);
    return UA_Client_writeValueAttribute(c, NODE(name), &v);
}

/* Connects a new client, reads "answer", disconnects. */
static UA_StatusCode connect_and_read(const char *url, UA_Int32 *answer, double *connect_ms) {
    UA_Client *c = o6q_client_new();
    UA_StatusCode rc;
    double t0;
    if(!c)
        return UA_STATUSCODE_BADOUTOFMEMORY;
    t0 = o6q_now_us();
    rc = UA_Client_connect(c, url);
    if(connect_ms)
        *connect_ms = (o6q_now_us() - t0) / 1000.0;
    if(rc == UA_STATUSCODE_GOOD)
        rc = o6q_read_i32(c, "answer", answer);
    UA_Client_disconnect(c);
    UA_Client_delete(c);
    return rc;
}

/* ------------------------------------------------------------------ tests */

static void test_connect(Core *t) {
    UA_StatusCode rc;
    UA_Int32 answer = 0;
    double ms = 0, t0;
    unsigned gni0, gni;
    char url[64];

    o6q_step("connect.ipv4");
    t->client = o6q_client_new();
    if(!t->client) {
        CHECK("connect.ipv4", 0, "cannot create a client");
        return;
    }
    gni0 = o6q_log_getnameinfo();
    t0 = o6q_now_us();
    rc = UA_Client_connect(t->client, t->url);
    ms = (o6q_now_us() - t0) / 1000.0;
    /* The server logs the new connection before it answers, so a warning of
     * its getnameinfo() call is counted by now */
    gni = o6q_log_getnameinfo() - gni0;
    CHECK("connect.ipv4", rc == UA_STATUSCODE_GOOD && gni == 0,
          "%s -> %s in %.1f ms; getnameinfo warnings of the server: %u", t->url, SC(rc), ms,
          gni);

    /* Name resolution on the target: /etc/hosts and the resolver of libsocket. */
    o6q_step("connect.localhost");
    snprintf(url, sizeof(url), "opc.tcp://localhost:%u", t->port);
    rc = connect_and_read(url, &answer, &ms);
    CHECK("connect.localhost", rc == UA_STATUSCODE_GOOD && answer == 42,
          "%s -> %s answer=%d in %.1f ms", url, SC(rc), (int)answer, ms);

    /* IPv6 loopback, if the target has the address ::1. */
    o6q_step("connect.ipv6");
    {
        int s = socket(AF_INET6, SOCK_DGRAM, 0);
        int have_v6 = 0;
        int err = 0;
        if(s >= 0) {
            struct sockaddr_in6 a;
            memset(&a, 0, sizeof(a));
            a.sin6_family = AF_INET6;
            a.sin6_addr = in6addr_loopback;
            have_v6 = bind(s, (struct sockaddr *)&a, sizeof(a)) == 0;
            err = errno;
            close(s);
        } else {
            err = errno;
        }
        if(!have_v6) {
            o6q_skip("connect.ipv6", "no usable ::1 on this system (%s)", strerror(err));
        } else {
            snprintf(url, sizeof(url), "opc.tcp://[::1]:%u", t->port);
            answer = 0;
            rc = connect_and_read(url, &answer, &ms);
            CHECK("connect.ipv6", rc == UA_STATUSCODE_GOOD && answer == 42,
                  "%s -> %s answer=%d in %.1f ms", url, SC(rc), (int)answer, ms);
        }
    }
}

static void test_endpoints(Core *t) {
    UA_Client *c = o6q_client_new();
    UA_EndpointDescription *eps = NULL;
    size_t n = 0;
    UA_StatusCode rc;
    int none = 0;

    o6q_step("endpoints");
    if(!c) {
        CHECK("endpoints", 0, "cannot create a client");
        return;
    }
    rc = UA_Client_getEndpoints(c, t->url, &n, &eps);
    for(size_t i = 0; i < n; i++) {
        UA_String none_uri = UA_STRING("http://opcfoundation.org/UA/SecurityPolicy#None");
        if(UA_String_equal(&eps[i].securityPolicyUri, &none_uri))
            none++;
        o6q_info("endpoint %lu url=%.*s policy=%.*s mode=%d", (unsigned long)i,
                 (int)eps[i].endpointUrl.length, (const char *)eps[i].endpointUrl.data,
                 (int)eps[i].securityPolicyUri.length,
                 (const char *)eps[i].securityPolicyUri.data, (int)eps[i].securityMode);
    }
    CHECK("endpoints", rc == UA_STATUSCODE_GOOD && n >= 1 && none >= 1,
          "GetEndpoints -> %s, %lu endpoint(s), %d without security", SC(rc), (unsigned long)n,
          none);
    UA_Array_delete(eps, n, &UA_TYPES[UA_TYPES_ENDPOINTDESCRIPTION]);
    UA_Client_delete(c);
}

/* Browses one node; returns the number of references and whether one of them
 * has the given browse name. */
static UA_StatusCode browse(UA_Client *c, UA_NodeId node, const char *look_for, size_t *count,
                            int *found) {
    UA_BrowseDescription bd;
    UA_BrowseRequest req;
    UA_BrowseResponse resp;
    UA_StatusCode rc;
    UA_String name = UA_STRING((char *)(uintptr_t)look_for);

    *count = 0;
    *found = 0;
    UA_BrowseDescription_init(&bd);
    bd.nodeId = node;
    bd.browseDirection = UA_BROWSEDIRECTION_FORWARD;
    bd.referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HIERARCHICALREFERENCES);
    bd.includeSubtypes = true;
    bd.resultMask = UA_BROWSERESULTMASK_ALL;
    UA_BrowseRequest_init(&req);
    req.nodesToBrowse = &bd; /* on the stack: the request is not cleared */
    req.nodesToBrowseSize = 1;
    resp = UA_Client_Service_browse(c, req);
    rc = resp.responseHeader.serviceResult;
    if(rc == UA_STATUSCODE_GOOD && resp.resultsSize == 1) {
        rc = resp.results[0].statusCode;
        *count = resp.results[0].referencesSize;
        for(size_t i = 0; i < resp.results[0].referencesSize; i++) {
            if(UA_String_equal(&resp.results[0].references[i].browseName.name, &name))
                *found = 1;
        }
    } else if(rc == UA_STATUSCODE_GOOD) {
        rc = UA_STATUSCODE_BADUNEXPECTEDERROR;
    }
    UA_BrowseResponse_clear(&resp);
    return rc;
}

static void test_browse(Core *t) {
    size_t n1 = 0, n2 = 0;
    int f1 = 0, f2 = 0;
    UA_StatusCode rc1, rc2;

    o6q_step("browse");
    rc1 = browse(t->client, UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER), "o6q", &n1, &f1);
    rc2 = browse(t->client, NODE("o6q"), "qnxinfo", &n2, &f2);
    CHECK("browse", rc1 == UA_STATUSCODE_GOOD && rc2 == UA_STATUSCODE_GOOD && f1 && f2 && n2 == 6,
          "Objects: %s, %lu refs, o6q %s; o6q: %s, %lu refs (expected 6), qnxinfo %s", SC(rc1),
          (unsigned long)n1, f1 ? "found" : "missing", SC(rc2), (unsigned long)n2,
          f2 ? "found" : "missing");

    o6q_step("translate");
    {
        UA_RelativePathElement el[2];
        UA_BrowsePath bp;
        UA_TranslateBrowsePathsToNodeIdsRequest req;
        UA_TranslateBrowsePathsToNodeIdsResponse resp;
        UA_NodeId expected = NODE("answer");
        int ok;
        for(int i = 0; i < 2; i++) {
            UA_RelativePathElement_init(&el[i]);
            el[i].referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HIERARCHICALREFERENCES);
            el[i].includeSubtypes = true;
        }
        el[0].targetName = UA_QUALIFIEDNAME(O6Q_NS, "o6q");
        el[1].targetName = UA_QUALIFIEDNAME(O6Q_NS, "answer");
        UA_BrowsePath_init(&bp);
        bp.startingNode = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
        bp.relativePath.elements = el;
        bp.relativePath.elementsSize = 2;
        UA_TranslateBrowsePathsToNodeIdsRequest_init(&req);
        req.browsePaths = &bp;
        req.browsePathsSize = 1;
        resp = UA_Client_Service_translateBrowsePathsToNodeIds(t->client, req);
        ok = resp.responseHeader.serviceResult == UA_STATUSCODE_GOOD && resp.resultsSize == 1 &&
             resp.results[0].statusCode == UA_STATUSCODE_GOOD &&
             resp.results[0].targetsSize == 1 &&
             UA_NodeId_equal(&resp.results[0].targets[0].targetId.nodeId, &expected);
        CHECK("translate", ok, "Objects/1:o6q/1:answer -> %s",
              ok ? "ns=1;s=answer" : SC(resp.responseHeader.serviceResult));
        UA_TranslateBrowsePathsToNodeIdsResponse_clear(&resp);
    }
}

/* Nodes of namespace 0 that a full information model must serve. */
static void test_namespace0(Core *t) {
    UA_Variant v;
    UA_StatusCode rc;

    o6q_step("ns0.namespaces");
    UA_Variant_init(&v);
    rc = UA_Client_readValueAttribute(t->client, UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER_NAMESPACEARRAY),
                                      &v);
    {
        UA_String ns0 = UA_STRING("http://opcfoundation.org/UA/");
        int ok = rc == UA_STATUSCODE_GOOD &&
                 UA_Variant_hasArrayType(&v, &UA_TYPES[UA_TYPES_STRING]) &&
                 v.arrayLength >= 2 && UA_String_equal(&((UA_String *)v.data)[0], &ns0);
        if(ok) {
            UA_String *ns = (UA_String *)v.data;
            CHECK("ns0.namespaces", 1, "%lu namespaces, [1]=%.*s", (unsigned long)v.arrayLength,
                  (int)ns[1].length, (const char *)ns[1].data);
        } else {
            CHECK("ns0.namespaces", 0, "NamespaceArray -> %s", SC(rc));
        }
    }
    UA_Variant_clear(&v);

    o6q_step("ns0.status");
    {
        UA_Variant state, now, version;
        UA_StatusCode r1, r2, r3;
        double skew_ms = 1e12;
        int running = 0;
        char ver[64] = "?";
        UA_Variant_init(&state);
        UA_Variant_init(&now);
        UA_Variant_init(&version);
        r1 = UA_Client_readValueAttribute(
            t->client, UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER_SERVERSTATUS_STATE), &state);
        r2 = UA_Client_readValueAttribute(
            t->client, UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER_SERVERSTATUS_CURRENTTIME), &now);
        r3 = UA_Client_readValueAttribute(
            t->client, UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER_SERVERSTATUS_BUILDINFO_SOFTWAREVERSION),
            &version);
        if(r1 == UA_STATUSCODE_GOOD && state.type && state.data &&
           state.type->memSize == sizeof(UA_Int32))
            running = *(UA_Int32 *)state.data == (UA_Int32)UA_SERVERSTATE_RUNNING;
        if(r2 == UA_STATUSCODE_GOOD && UA_Variant_hasScalarType(&now, &UA_TYPES[UA_TYPES_DATETIME])) {
            skew_ms = (double)(UA_DateTime_now() - *(UA_DateTime *)now.data) /
                      (double)UA_DATETIME_MSEC;
            if(skew_ms < 0)
                skew_ms = -skew_ms;
        }
        if(r3 == UA_STATUSCODE_GOOD && UA_Variant_hasScalarType(&version, &UA_TYPES[UA_TYPES_STRING])) {
            UA_String *s = (UA_String *)version.data;
            snprintf(ver, sizeof(ver), "%.*s", (int)s->length, (const char *)s->data);
        }
        CHECK("ns0.status", running && skew_ms < 5000.0 && strncmp(ver, "1.4.", 4) == 0,
              "State %s (%s), CurrentTime %s (off by %.1f ms), SoftwareVersion %s (%s)",
              running ? "Running" : "not running", SC(r1), SC(r2), skew_ms, ver, SC(r3));
        UA_Variant_clear(&state);
        UA_Variant_clear(&now);
        UA_Variant_clear(&version);
    }
}

static void test_read_write(Core *t) {
    int n = t->n;
    double *rd = (double *)malloc(sizeof(double) * (size_t)n);
    double *wr = (double *)malloc(sizeof(double) * (size_t)n);
    int bad = 0;
    UA_StatusCode first_error = UA_STATUSCODE_GOOD;

    o6q_step("readwrite");
    if(!rd || !wr) {
        CHECK("readwrite", 0, "out of memory");
        free(rd);
        free(wr);
        return;
    }
    for(int i = 0; i < n; i++) {
        UA_Int32 got = -1;
        double t0 = o6q_now_us(), t1, t2;
        UA_StatusCode rc = write_i32(t->client, "answer", (UA_Int32)(1000 + i));
        t1 = o6q_now_us();
        if(rc == UA_STATUSCODE_GOOD)
            rc = o6q_read_i32(t->client, "answer", &got);
        t2 = o6q_now_us();
        wr[i] = t1 - t0;
        rd[i] = t2 - t1;
        if(rc != UA_STATUSCODE_GOOD || got != (UA_Int32)(1000 + i)) {
            if(!bad)
                first_error = rc;
            bad++;
        }
    }
    write_i32(t->client, "answer", 42);
    CHECK("readwrite", bad == 0, "%d write+read round trips, %d wrong (%s)", n, bad,
          SC(first_error));
    o6q_lat("write", wr, (size_t)n);
    o6q_lat("read", rd, (size_t)n);
    free(rd);
    free(wr);
}

static void test_call(Core *t) {
    int n = t->n;
    double *lat = (double *)malloc(sizeof(double) * (size_t)n);
    int bad = 0;
    UA_StatusCode first_error = UA_STATUSCODE_GOOD;

    o6q_step("call");
    if(!lat) {
        CHECK("call", 0, "out of memory");
        return;
    }
    for(int i = 0; i < n; i++) {
        UA_Int32 a = (UA_Int32)i, b = 22, sum = 0;
        UA_Variant in[2];
        UA_Variant *out = NULL;
        size_t out_n = 0;
        double t0;
        UA_StatusCode rc;
        UA_Variant_setScalar(&in[0], &a, &UA_TYPES[UA_TYPES_INT32]);
        UA_Variant_setScalar(&in[1], &b, &UA_TYPES[UA_TYPES_INT32]);
        t0 = o6q_now_us();
        rc = UA_Client_call(t->client, NODE("o6q"), NODE("add"), 2, in, &out_n, &out);
        lat[i] = o6q_now_us() - t0;
        if(rc == UA_STATUSCODE_GOOD && out_n == 1 &&
           UA_Variant_hasScalarType(&out[0], &UA_TYPES[UA_TYPES_INT32]))
            sum = *(UA_Int32 *)out[0].data;
        else if(rc == UA_STATUSCODE_GOOD)
            rc = UA_STATUSCODE_BADTYPEMISMATCH;
        if(rc != UA_STATUSCODE_GOOD || sum != a + b) {
            if(!bad)
                first_error = rc;
            bad++;
        }
        UA_Array_delete(out, out_n, &UA_TYPES[UA_TYPES_VARIANT]);
    }
    CHECK("call", bad == 0, "%d calls of add(a, 22), %d wrong (%s)", n, bad, SC(first_error));
    o6q_lat("call", lat, (size_t)n);
    free(lat);
}

/* Values larger than one 64 KiB chunk travel as several chunks in both
 * directions. */
static void test_bigdata(Core *t) {
    static const size_t sizes[] = {1024, 65536, 1048576, 4194304};
    static const char *const names[] = {"bigdata.1KiB", "bigdata.64KiB", "bigdata.1MiB",
                                        "bigdata.4MiB"};
    UA_Byte *buf = (UA_Byte *)malloc(sizes[3]);

    if(!buf) {
        CHECK("bigdata", 0, "out of memory");
        return;
    }
    for(size_t i = 0; i < sizes[3]; i++)
        buf[i] = (UA_Byte)((i * 31u + (i >> 8) + 7u) & 0xffu);

    for(size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        UA_ByteString bs;
        UA_Variant in, out;
        UA_StatusCode rc_w, rc_r = UA_STATUSCODE_BADINVALIDSTATE;
        double t0, t1, t2;
        int same = 0;

        o6q_step(names[k]);
        bs.length = sizes[k];
        bs.data = buf;
        UA_Variant_setScalar(&in, &bs, &UA_TYPES[UA_TYPES_BYTESTRING]);
        UA_Variant_init(&out);
        t0 = o6q_now_us();
        rc_w = UA_Client_writeValueAttribute(t->client, NODE("blob"), &in);
        t1 = o6q_now_us();
        if(rc_w == UA_STATUSCODE_GOOD)
            rc_r = UA_Client_readValueAttribute(t->client, NODE("blob"), &out);
        t2 = o6q_now_us();
        if(rc_r == UA_STATUSCODE_GOOD && UA_Variant_hasScalarType(&out, &UA_TYPES[UA_TYPES_BYTESTRING])) {
            UA_ByteString *got = (UA_ByteString *)out.data;
            same = got->length == sizes[k] && memcmp(got->data, buf, sizes[k]) == 0;
        }
        CHECK(names[k], same, "write %s in %.1f ms, read %s in %.1f ms, %s", SC(rc_w),
              (t1 - t0) / 1000.0, SC(rc_r), (t2 - t1) / 1000.0,
              same ? "content identical" : "content differs");
        UA_Variant_clear(&out);
    }
    {
        UA_ByteString empty = UA_BYTESTRING_NULL;
        UA_Variant v;
        UA_Variant_setScalar(&v, &empty, &UA_TYPES[UA_TYPES_BYTESTRING]);
        UA_Client_writeValueAttribute(t->client, NODE("blob"), &v);
    }
    free(buf);
}

static UA_StatusCode read_session_count(UA_Client *c, UA_UInt32 *count) {
    UA_Variant v;
    UA_StatusCode rc;
    UA_Variant_init(&v);
    rc = UA_Client_readValueAttribute(
        c, UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER_SERVERDIAGNOSTICS_SERVERDIAGNOSTICSSUMMARY_CURRENTSESSIONCOUNT),
        &v);
    if(rc == UA_STATUSCODE_GOOD) {
        if(UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_UINT32]))
            *count = *(UA_UInt32 *)v.data;
        else
            rc = UA_STATUSCODE_BADTYPEMISMATCH;
    }
    UA_Variant_clear(&v);
    return rc;
}

#define SESSIONS 32

static void test_sessions(Core *t) {
    UA_Client *c[SESSIONS];
    int connected = 0, read_ok = 0;
    UA_UInt32 before = 0, during = 0, after = 0;
    UA_StatusCode rc0, rc1, rc2 = UA_STATUSCODE_BADINVALIDSTATE, first_error = UA_STATUSCODE_GOOD;
    double t0;

    o6q_step("sessions");
    rc0 = read_session_count(t->client, &before);
    t0 = o6q_now_us();
    for(int i = 0; i < SESSIONS; i++) {
        UA_StatusCode rc = UA_STATUSCODE_BADOUTOFMEMORY;
        c[i] = o6q_client_new();
        if(c[i])
            rc = UA_Client_connect(c[i], t->url);
        if(rc == UA_STATUSCODE_GOOD)
            connected++;
        else if(first_error == UA_STATUSCODE_GOOD)
            first_error = rc;
    }
    for(int i = 0; i < SESSIONS; i++) {
        UA_Int32 v = 0;
        if(c[i] && o6q_read_i32(c[i], "answer", &v) == UA_STATUSCODE_GOOD && v == 42)
            read_ok++;
    }
    rc1 = read_session_count(t->client, &during);
    for(int i = 0; i < SESSIONS; i++) {
        if(c[i]) {
            UA_Client_disconnect(c[i]);
            UA_Client_delete(c[i]);
        }
    }
    /* The server notices the closed sessions in its own thread. */
    for(int i = 0; i < 40; i++) {
        rc2 = read_session_count(t->client, &after);
        if(rc2 != UA_STATUSCODE_GOOD || after == before)
            break;
        o6q_sleep_ms(50);
    }
    CHECK("sessions",
          connected == SESSIONS && read_ok == SESSIONS && rc0 == UA_STATUSCODE_GOOD &&
              rc1 == UA_STATUSCODE_GOOD && rc2 == UA_STATUSCODE_GOOD &&
              during == before + SESSIONS && after == before,
          "%d of %d extra sessions connected (%s), %d read the value; CurrentSessionCount "
          "%u -> %u -> %u; %.0f ms",
          connected, SESSIONS, SC(first_error), read_ok, (unsigned)before, (unsigned)during,
          (unsigned)after, (o6q_now_us() - t0) / 1000.0);
}

/* ------------------------------------------------- subscriptions, events */

#define SUB_MAX 4096

typedef struct {
    unsigned n;
    UA_Int32 last;
    unsigned not_increasing;
    unsigned skipped; /* counter values that never arrived */
    double age_us[SUB_MAX];
    unsigned n_age;
} SubCtx;

static void on_data(UA_Client *client, UA_UInt32 subId, void *subContext, UA_UInt32 monId,
                    void *monContext, UA_DataValue *value) {
    SubCtx *s = (SubCtx *)monContext;
    UA_Int32 v;
    UA_DateTime ts = 0;
    (void)client; (void)subId; (void)subContext; (void)monId;
    if(!value->hasValue || !UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_INT32]))
        return;
    v = *(UA_Int32 *)value->value.data;
    if(s->n > 0) {
        if(v <= s->last)
            s->not_increasing++;
        else
            s->skipped += (unsigned)(v - s->last - 1);
    }
    s->last = v;
    s->n++;
    if(value->hasSourceTimestamp)
        ts = value->sourceTimestamp;
    else if(value->hasServerTimestamp)
        ts = value->serverTimestamp;
    if(ts != 0 && s->n_age < SUB_MAX)
        s->age_us[s->n_age++] = (double)(UA_DateTime_now() - ts) / (double)UA_DATETIME_USEC;
}

typedef struct {
    unsigned n;
    UA_UInt16 severity[16];
    unsigned message_ok;
} EventCtx;

static void on_event(UA_Client *client, UA_UInt32 subId, void *subContext, UA_UInt32 monId,
                     void *monContext, size_t nEventFields, UA_Variant *eventFields) {
    EventCtx *e = (EventCtx *)monContext;
    UA_String expected = UA_STRING("o6q test event");
    UA_UInt16 severity = 0;
    (void)client; (void)subId; (void)subContext; (void)monId;
    if(nEventFields >= 1 && UA_Variant_hasScalarType(&eventFields[0], &UA_TYPES[UA_TYPES_UINT16]))
        severity = *(UA_UInt16 *)eventFields[0].data;
    if(nEventFields >= 2 &&
       UA_Variant_hasScalarType(&eventFields[1], &UA_TYPES[UA_TYPES_LOCALIZEDTEXT])) {
        UA_LocalizedText *lt = (UA_LocalizedText *)eventFields[1].data;
        if(UA_String_equal(&lt->text, &expected))
            e->message_ok++;
    }
    if(e->n < 16)
        e->severity[e->n] = severity;
    e->n++;
}

static void test_subscription(Core *t) {
    static SubCtx sub;
    static EventCtx ev;
    UA_CreateSubscriptionRequest sreq = UA_CreateSubscriptionRequest_default();
    UA_CreateSubscriptionResponse sresp;
    UA_MonitoredItemCreateRequest mreq;
    UA_MonitoredItemCreateResult mres;
    UA_UInt32 sub_id;
    double t0, secs;
    const double window_ms = 3000.0;

    memset(&sub, 0, sizeof(sub));
    memset(&ev, 0, sizeof(ev));

    o6q_step("subscribe.create");
    sreq.requestedPublishingInterval = 20.0;
    sreq.requestedMaxKeepAliveCount = 50;
    sresp = UA_Client_Subscriptions_create(t->client, sreq, NULL, NULL, NULL);
    sub_id = sresp.subscriptionId;
    mreq = UA_MonitoredItemCreateRequest_default(NODE("counter"));
    mreq.requestedParameters.samplingInterval = 10.0;
    mreq.requestedParameters.queueSize = 20;
    memset(&mres, 0, sizeof(mres));
    mres.statusCode = UA_STATUSCODE_BADNOTCONNECTED;
    if(sresp.responseHeader.serviceResult == UA_STATUSCODE_GOOD)
        mres = UA_Client_MonitoredItems_createDataChange(
            t->client, sub_id, UA_TIMESTAMPSTORETURN_BOTH, mreq, &sub, on_data, NULL);
    CHECK("subscribe.create",
          sresp.responseHeader.serviceResult == UA_STATUSCODE_GOOD &&
              mres.statusCode == UA_STATUSCODE_GOOD,
          "subscription %s, publishing interval %.0f ms; monitored item %s, sampling %.0f ms, "
          "queue %u",
          SC(sresp.responseHeader.serviceResult), sresp.revisedPublishingInterval,
          SC(mres.statusCode), mres.revisedSamplingInterval, (unsigned)mres.revisedQueueSize);
    if(sresp.responseHeader.serviceResult != UA_STATUSCODE_GOOD)
        return;

    /* The 10 ms server timer changes "counter"; the notifications must arrive
     * in order and at about that rate. */
    o6q_step("subscribe.datachange");
    t0 = o6q_now_us();
    while(o6q_now_us() - t0 < window_ms * 1000.0)
        UA_Client_run_iterate(t->client, 10);
    secs = (o6q_now_us() - t0) / 1e6;
    CHECK("subscribe.datachange", sub.n >= 100 && sub.not_increasing == 0,
          "%u notifications in %.1f s (%.0f per second, timer runs at 100 per second), "
          "%u out of order, %u values not delivered",
          sub.n, secs, (double)sub.n / secs, sub.not_increasing, sub.skipped);
    /* Time from the server's write to the client's callback. */
    o6q_lat("datachange_age", sub.age_us, sub.n_age);

    o6q_step("events");
    {
        UA_SimpleAttributeOperand select[2];
        UA_QualifiedName names[2];
        UA_EventFilter filter;
        UA_MonitoredItemCreateRequest item;
        UA_MonitoredItemCreateResult eres;
        int calls_ok = 0, order_ok = 1;

        names[0] = UA_QUALIFIEDNAME(0, "Severity");
        names[1] = UA_QUALIFIEDNAME(0, "Message");
        for(int i = 0; i < 2; i++) {
            UA_SimpleAttributeOperand_init(&select[i]);
            select[i].typeDefinitionId = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEEVENTTYPE);
            select[i].browsePathSize = 1;
            select[i].browsePath = &names[i];
            select[i].attributeId = UA_ATTRIBUTEID_VALUE;
        }
        UA_EventFilter_init(&filter);
        filter.selectClauses = select;
        filter.selectClausesSize = 2;

        UA_MonitoredItemCreateRequest_init(&item);
        item.itemToMonitor.nodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER);
        item.itemToMonitor.attributeId = UA_ATTRIBUTEID_EVENTNOTIFIER;
        item.monitoringMode = UA_MONITORINGMODE_REPORTING;
        item.requestedParameters.queueSize = 100;
        item.requestedParameters.discardOldest = true;
        item.requestedParameters.filter.encoding = UA_EXTENSIONOBJECT_DECODED_NODELETE;
        item.requestedParameters.filter.content.decoded.data = &filter;
        item.requestedParameters.filter.content.decoded.type = &UA_TYPES[UA_TYPES_EVENTFILTER];
        eres = UA_Client_MonitoredItems_createEvent(t->client, sub_id, UA_TIMESTAMPSTORETURN_BOTH,
                                                    item, &ev, on_event, NULL);
        if(eres.statusCode == UA_STATUSCODE_GOOD) {
            for(int i = 0; i < 5; i++) {
                UA_UInt16 severity = (UA_UInt16)(100 * (i + 1));
                UA_Variant in;
                UA_Variant *out = NULL;
                size_t out_n = 0;
                UA_Variant_setScalar(&in, &severity, &UA_TYPES[UA_TYPES_UINT16]);
                if(UA_Client_call(t->client, NODE("o6q"), NODE("fire"), 1, &in, &out_n, &out) ==
                   UA_STATUSCODE_GOOD)
                    calls_ok++;
                UA_Array_delete(out, out_n, &UA_TYPES[UA_TYPES_VARIANT]);
            }
            t0 = o6q_now_us();
            while(ev.n < 5 && o6q_now_us() - t0 < 5e6)
                UA_Client_run_iterate(t->client, 10);
            for(unsigned i = 0; i < ev.n && i < 5; i++) {
                if(ev.severity[i] != (UA_UInt16)(100 * (i + 1)))
                    order_ok = 0;
            }
        }
        CHECK("events",
              eres.statusCode == UA_STATUSCODE_GOOD && calls_ok == 5 && ev.n == 5 &&
                  ev.message_ok == 5 && order_ok,
              "event item %s; %d of 5 fire() calls; %u events received, %u with the right "
              "message, severities %s",
              SC(eres.statusCode), calls_ok, ev.n, ev.message_ok,
              order_ok ? "100..500 in order" : "wrong");
    }

    o6q_step("subscribe.delete");
    {
        UA_StatusCode rc = UA_Client_Subscriptions_deleteSingle(t->client, sub_id);
        CHECK("subscribe.delete", rc == UA_STATUSCODE_GOOD, "DeleteSubscriptions -> %s", SC(rc));
    }
}

static void test_nodemanagement(Core *t) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    UA_Double value = 3.25, got = 0;
    UA_Variant v;
    UA_StatusCode rc_add, rc_read, rc_del, rc_gone;

    o6q_step("nodemanagement");
    UA_Variant_setScalar(&attr.value, &value, &UA_TYPES[UA_TYPES_DOUBLE]);
    attr.dataType = UA_TYPES[UA_TYPES_DOUBLE].typeId;
    attr.valueRank = UA_VALUERANK_SCALAR;
    attr.displayName = UA_LOCALIZEDTEXT("en", "dyn");
    attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    rc_add = UA_Client_addVariableNode(t->client, NODE("dyn"), NODE("o6q"),
                                       UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                                       UA_QUALIFIEDNAME(O6Q_NS, "dyn"),
                                       UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr,
                                       NULL);
    UA_Variant_init(&v);
    rc_read = UA_Client_readValueAttribute(t->client, NODE("dyn"), &v);
    if(rc_read == UA_STATUSCODE_GOOD && UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_DOUBLE]))
        got = *(UA_Double *)v.data;
    UA_Variant_clear(&v);
    rc_del = UA_Client_deleteNode(t->client, NODE("dyn"), true);
    UA_Variant_init(&v);
    rc_gone = UA_Client_readValueAttribute(t->client, NODE("dyn"), &v);
    UA_Variant_clear(&v);
    CHECK("nodemanagement",
          rc_add == UA_STATUSCODE_GOOD && rc_read == UA_STATUSCODE_GOOD && got == value &&
              rc_del == UA_STATUSCODE_GOOD && rc_gone == UA_STATUSCODE_BADNODEIDUNKNOWN,
          "AddNodes %s, read %s (%.2f), DeleteNodes %s, read again %s", SC(rc_add), SC(rc_read),
          got, SC(rc_del), SC(rc_gone));
}

/* How exactly does the event loop of open62541 keep a 10 ms period on this
 * system? */
static void test_timer(Core *t) {
    static double err[O6Q_TICK_HISTORY];
    unsigned ticks0, ticks1;
    double t0, secs, rate;
    size_t n;

    o6q_step("timer");
    ticks0 = o6q_model_ticks(t->model);
    t0 = o6q_now_us();
    o6q_sleep_ms(2000);
    ticks1 = o6q_model_ticks(t->model);
    secs = (o6q_now_us() - t0) / 1e6;
    rate = (double)(ticks1 - ticks0) / secs;
    n = o6q_model_tick_errors(t->model, err, ticks1 - ticks0);
    CHECK("timer", rate > 80.0 && rate < 120.0,
          "10 ms repeated callback ran %u times in %.2f s (%.1f per second)", ticks1 - ticks0,
          secs, rate);
    /* |measured period - 10 ms| while nothing else happens */
    o6q_lat("timer_10ms_error", err, n);
}

static void test_qnxinfo(Core *t) {
    UA_Variant v;
    UA_StatusCode rc;
    char text[512] = "";

    o6q_step("qnxinfo");
    UA_Variant_init(&v);
    rc = UA_Client_readValueAttribute(t->client, NODE("qnxinfo"), &v);
    if(rc == UA_STATUSCODE_GOOD && UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_STRING])) {
        UA_String *s = (UA_String *)v.data;
        snprintf(text, sizeof(text), "%.*s", (int)s->length, (const char *)s->data);
    }
    UA_Variant_clear(&v);
#ifdef __QNXNTO__
    CHECK("qnxinfo", rc == UA_STATUSCODE_GOOD && strncmp(text, "QNX ", 4) == 0, "%s: %s", SC(rc),
          text);
#else
    CHECK("qnxinfo", rc == UA_STATUSCODE_GOOD && text[0] != 0, "%s: %s", SC(rc), text);
#endif
}

/* Float and Double on the wire.  OPC UA binary is the IEEE 754 bit pattern,
 * least significant byte first (Part 6, 5.2.2.3).  Where open62541 does not
 * recognise the CPU as IEEE 754 it converts with a generic routine instead of
 * copying the bits; this test compares what it writes and reads with the bit
 * pattern, for ordinary values and for the smallest (subnormal) ones. */
static void test_float_wire(void) {
    static const double dv[] = {1.0, -2.5, 3.141592653589793, 1e300, DBL_MAX, DBL_MIN,
                                DBL_MIN / 2, 1e-310, 4.9406564584124654e-324, -1e-320};
    static const float fv[] = {1.0f, -2.5f, 3.1415927f, 1e30f, FLT_MAX, FLT_MIN,
                               FLT_MIN / 2, 1e-40f, 1.4e-45f, -1e-42f};
    char first[160] = "";
    unsigned bad = 0, total = 0;

    o6q_step("float_wire");
    for(size_t i = 0; i < sizeof(dv) / sizeof(dv[0]); i++) {
        UA_Byte wire[8];
        UA_ByteString buf = {sizeof(wire), wire};
        UA_Double in = dv[i], out = 0.0;
        uint64_t bits, got = 0, back = 0;
        UA_StatusCode rc = UA_encodeBinary(&in, &UA_TYPES[UA_TYPES_DOUBLE], &buf);
        memcpy(&bits, &in, sizeof(bits));
        for(int b = 7; b >= 0; b--)
            got = (got << 8) | wire[b];
        if(rc == UA_STATUSCODE_GOOD)
            rc = UA_decodeBinary(&buf, &out, &UA_TYPES[UA_TYPES_DOUBLE], NULL);
        memcpy(&back, &out, sizeof(back));
        total++;
        if(rc != UA_STATUSCODE_GOOD || got != bits || back != bits) {
            if(!bad)
                snprintf(first, sizeof(first),
                         "Double %.17g (bits %016llx) is written as %016llx and read back as "
                         "%016llx (%s)",
                         in, (unsigned long long)bits, (unsigned long long)got,
                         (unsigned long long)back, SC(rc));
            bad++;
        }
    }
    for(size_t i = 0; i < sizeof(fv) / sizeof(fv[0]); i++) {
        UA_Byte wire[4];
        UA_ByteString buf = {sizeof(wire), wire};
        UA_Float in = fv[i], out = 0.0f;
        uint32_t bits, got = 0, back = 0;
        UA_StatusCode rc = UA_encodeBinary(&in, &UA_TYPES[UA_TYPES_FLOAT], &buf);
        memcpy(&bits, &in, sizeof(bits));
        for(int b = 3; b >= 0; b--)
            got = (got << 8) | wire[b];
        if(rc == UA_STATUSCODE_GOOD)
            rc = UA_decodeBinary(&buf, &out, &UA_TYPES[UA_TYPES_FLOAT], NULL);
        memcpy(&back, &out, sizeof(back));
        total++;
        if(rc != UA_STATUSCODE_GOOD || got != bits || back != bits) {
            if(!bad)
                snprintf(first, sizeof(first),
                         "Float %.9g (bits %08lx) is written as %08lx and read back as %08lx (%s)",
                         (double)in, (unsigned long)bits, (unsigned long)got,
                         (unsigned long)back, SC(rc));
            bad++;
        }
    }
    {
        /* special values: only the class must survive, a NaN has many bit patterns */
        UA_Byte wire[8];
        UA_ByteString buf = {sizeof(wire), wire};
        UA_Double in = (UA_Double)NAN, out = 0.0;
        UA_StatusCode rc = UA_encodeBinary(&in, &UA_TYPES[UA_TYPES_DOUBLE], &buf);
        if(rc == UA_STATUSCODE_GOOD)
            rc = UA_decodeBinary(&buf, &out, &UA_TYPES[UA_TYPES_DOUBLE], NULL);
        total++;
        if(rc != UA_STATUSCODE_GOOD || !isnan(out)) {
            if(!bad)
                snprintf(first, sizeof(first), "Double NaN is read back as %g (%s)", out, SC(rc));
            bad++;
        }
        in = -(UA_Double)INFINITY;
        out = 0.0;
        buf.length = sizeof(wire);
        rc = UA_encodeBinary(&in, &UA_TYPES[UA_TYPES_DOUBLE], &buf);
        if(rc == UA_STATUSCODE_GOOD)
            rc = UA_decodeBinary(&buf, &out, &UA_TYPES[UA_TYPES_DOUBLE], NULL);
        total++;
        if(rc != UA_STATUSCODE_GOOD || !isinf(out) || out > 0) {
            if(!bad)
                snprintf(first, sizeof(first), "Double -Infinity is read back as %g (%s)", out,
                         SC(rc));
            bad++;
        }
        in = -0.0;
        out = 1.0;
        buf.length = sizeof(wire);
        rc = UA_encodeBinary(&in, &UA_TYPES[UA_TYPES_DOUBLE], &buf);
        if(rc == UA_STATUSCODE_GOOD)
            rc = UA_decodeBinary(&buf, &out, &UA_TYPES[UA_TYPES_DOUBLE], NULL);
        total++;
        if(rc != UA_STATUSCODE_GOOD || out != 0.0 || !signbit(out)) {
            if(!bad)
                snprintf(first, sizeof(first), "Double -0.0 is read back as %g (%s)", out, SC(rc));
            bad++;
        }
    }
    CHECK("float_wire", bad == 0,
          "%u of %u Float/Double values keep their IEEE 754 bit pattern through encode and "
          "decode (UA_FLOAT_IEEE754=%d)%s%s",
          total - bad, total, UA_FLOAT_IEEE754, bad ? "; first: " : "", first);
}

/* A server that goes away and comes back on the same port. */
static void test_restart(Core *t) {
    UA_Int32 v = 0;
    UA_StatusCode rc_down, rc_start, rc_again;
    double t0, stop_ms, start_ms;

    o6q_step("restart.stop");
    t0 = o6q_now_us();
    o6q_server_stop(&t->srv);
    stop_ms = (o6q_now_us() - t0) / 1000.0;
    o6q_expect_errors(1);
    rc_down = o6q_read_i32(t->client, "answer", &v);
    o6q_expect_errors(0);
    CHECK("restart.stop", rc_down != UA_STATUSCODE_GOOD,
          "server stopped in %.0f ms; a read on the old session -> %s", stop_ms, SC(rc_down));

    /* Binding the same port again at once needs SO_REUSEADDR to work. */
    o6q_step("restart.start");
    t0 = o6q_now_us();
    rc_start = o6q_server_start_model(&t->srv, t->model, t->port);
    start_ms = (o6q_now_us() - t0) / 1000.0;
    CHECK("restart.start", rc_start == UA_STATUSCODE_GOOD, "same port again -> %s in %.0f ms",
          SC(rc_start), start_ms);

    o6q_step("restart.reconnect");
    v = 0;
    o6q_expect_errors(1); /* the old session is gone; the client logs that */
    rc_again = UA_Client_connect(t->client, t->url);
    o6q_expect_errors(0);
    if(rc_again == UA_STATUSCODE_GOOD)
        rc_again = o6q_read_i32(t->client, "answer", &v);
    CHECK("restart.reconnect", rc_again == UA_STATUSCODE_GOOD && v == 42,
          "the same client object connects again -> %s, answer=%d", SC(rc_again), (int)v);
}

/* Start, use and stop a server several times: the port must be free again at
 * once and no descriptor may stay open. */
static void test_lifecycle(Core *t) {
    int fds0 = o6q_open_fds(), fds1, good = 0;
    const int cycles = 5;
    UA_StatusCode first_error = UA_STATUSCODE_GOOD;
    double t0 = o6q_now_us();

    o6q_step("lifecycle");
    for(int i = 0; i < cycles; i++) {
        UA_Int32 v = 0;
        UA_StatusCode rc = o6q_server_start_model(&t->srv, t->model, t->port);
        if(rc == UA_STATUSCODE_GOOD)
            rc = connect_and_read(t->url, &v, NULL);
        o6q_server_stop(&t->srv);
        if(rc == UA_STATUSCODE_GOOD && v == 42)
            good++;
        else if(first_error == UA_STATUSCODE_GOOD)
            first_error = rc != UA_STATUSCODE_GOOD ? rc : UA_STATUSCODE_BADUNEXPECTEDERROR;
    }
    fds1 = o6q_open_fds();
    CHECK("lifecycle", good == cycles && fds1 == fds0,
          "%d of %d start/connect/read/stop cycles on the same port (%s) in %.0f ms; open "
          "descriptors %d -> %d",
          good, cycles, SC(first_error), (o6q_now_us() - t0) / 1000.0, fds0, fds1);
}

/* ------------------------------------------------------------------ suite */

int o6q_suite_core(unsigned port, int n) {
    static O6QModel model;
    Core t;
    UA_StatusCode rc;
    double t0;
    int fds_before, fds_after;

    memset(&t, 0, sizeof(t));
    t.port = port;
    t.n = n > 0 ? n : 1000;
    t.model = &model;
    snprintf(t.url, sizeof(t.url), "opc.tcp://127.0.0.1:%u", port);

    o6q_begin("core", 900);
    fds_before = o6q_open_fds();

    o6q_step("startup");
    t0 = o6q_now_us();
    rc = o6q_server_start_model(&t.srv, &model, port);
    CHECK("startup", rc == UA_STATUSCODE_GOOD,
          "server with the full namespace 0 on port %u -> %s in %.0f ms", port, SC(rc),
          (o6q_now_us() - t0) / 1000.0);
    if(rc != UA_STATUSCODE_GOOD)
        return o6q_end();

    test_float_wire();
    test_connect(&t);
    if(t.client) {
        UA_SessionState ss = UA_SESSIONSTATE_CLOSED;
        UA_Client_getState(t.client, NULL, &ss, NULL);
        if(ss == UA_SESSIONSTATE_ACTIVATED) {
            test_endpoints(&t);
            test_browse(&t);
            test_namespace0(&t);
            test_read_write(&t);
            test_call(&t);
            test_bigdata(&t);
            test_sessions(&t);
            test_subscription(&t);
            test_nodemanagement(&t);
            test_timer(&t);
            test_qnxinfo(&t);
            test_restart(&t);
        }
    }

    o6q_step("shutdown");
    if(t.client) {
        UA_Client_disconnect(t.client);
        UA_Client_delete(t.client);
        t.client = NULL;
    }
    t0 = o6q_now_us();
    o6q_server_stop(&t.srv);
    fds_after = o6q_open_fds();
    CHECK("shutdown", 1, "server stopped in %.0f ms; open descriptors at start %d, now %d",
          (o6q_now_us() - t0) / 1000.0, fds_before, fds_after);

    test_lifecycle(&t);

    return o6q_end();
}
