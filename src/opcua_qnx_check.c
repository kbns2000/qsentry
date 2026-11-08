/*
 * opcua_qnx_check - end-to-end check of open62541 (OPC UA) on QNX 8.0.
 *
 * Starts an OPC UA server on a thread, connects a client over TCP loopback and
 * exercises the services an industrial client actually uses:
 *
 *   connect     session over opc.tcp://localhost:<port>
 *   browse      Objects folder (finds our nodes and the standard Server object)
 *   read        Int32 variable, N times, latency per call
 *   write       Int32 variable, N times, then read back the last value
 *   call        method "add(a,b)" with arguments, checks the result
 *   subscribe   10 ms sampling of a counter updated by a 10 ms server timer;
 *               counts notifications and checks values are increasing
 *   qnxinfo     reads a node backed by a QNX data source (syspage CPU count,
 *               ClockCycles uptime) to show device data published over OPC UA
 *
 * Output: "O6Q PASS|FAIL <test> ..." lines, latency lines "O6Q LAT ...",
 * and a final "O6Q SUMMARY pass=<p> fail=<f>". Exit code = number of failures.
 *
 * Builds on QNX 8.0 (qcc) and on Linux (gcc) against the open62541 single-file
 * distribution. SPDX-License-Identifier: Apache-2.0
 */
#include "open62541.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __QNXNTO__
#include <sys/neutrino.h>
#include <sys/syspage.h>
#endif

static int g_pass, g_fail;
static volatile UA_Boolean g_running = true;
static UA_Int32 g_counter;
static unsigned g_port = 4840;

#define CHECK(name, cond, ...)                                                   \
    do {                                                                         \
        if (cond) { g_pass++; printf("O6Q PASS %-9s ", name); }                \
        else      { g_fail++; printf("O6Q FAIL %-9s ", name); }                \
        printf(__VA_ARGS__); printf("\n"); fflush(stdout);                       \
    } while (0)

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void lat(const char *what, double *v, size_t n)
{
    qsort(v, n, sizeof(*v), cmp_d);
    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += v[i];
    printf("O6Q LAT %-6s n=%zu p50_us=%.1f p99_us=%.1f max_us=%.1f mean_us=%.1f\n", what, n,
           v[n / 2], v[(size_t)(n * 0.99)], v[n - 1], sum / n);
    fflush(stdout);
}

/* ---------------------------------------------------------------- server */

static UA_StatusCode add_method(UA_Server *s, const UA_NodeId *sid, void *sctx,
                                const UA_NodeId *mid, void *mctx, const UA_NodeId *oid,
                                void *octx, size_t in_n, const UA_Variant *in,
                                size_t out_n, UA_Variant *out)
{
    (void)s; (void)sid; (void)sctx; (void)mid; (void)mctx; (void)oid; (void)octx;
    if (in_n != 2 || out_n != 1) return UA_STATUSCODE_BADARGUMENTSMISSING;
    UA_Int32 r = *(UA_Int32 *)in[0].data + *(UA_Int32 *)in[1].data;
    return UA_Variant_setScalarCopy(out, &r, &UA_TYPES[UA_TYPES_INT32]);
}

static void tick(UA_Server *s, void *d)
{
    (void)d;
    UA_Int32 v = ++g_counter;
    UA_Variant var;
    UA_Variant_setScalar(&var, &v, &UA_TYPES[UA_TYPES_INT32]);
    UA_Server_writeValue(s, UA_NODEID_STRING(1, "counter"), var);
}

/* Data source: computed on every read from QNX system state. */
static UA_StatusCode read_qnxinfo(UA_Server *s, const UA_NodeId *sid, void *sctx,
                                  const UA_NodeId *nid, void *nctx, UA_Boolean ts,
                                  const UA_NumericRange *range, UA_DataValue *dv)
{
    (void)s; (void)sid; (void)sctx; (void)nid; (void)nctx; (void)ts; (void)range;
    char buf[160];
#ifdef __QNXNTO__
    double up = (double)ClockCycles() / (double)SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    snprintf(buf, sizeof(buf), "QNX cpus=%u uptime_s=%.1f", (unsigned)_syspage_ptr->num_cpu, up);
#else
    snprintf(buf, sizeof(buf), "non-QNX host cpus=%ld", sysconf(_SC_NPROCESSORS_ONLN));
#endif
    UA_String str = UA_STRING(buf);
    UA_Variant_setScalarCopy(&dv->value, &str, &UA_TYPES[UA_TYPES_STRING]);
    dv->hasValue = true;
    return UA_STATUSCODE_GOOD;
}

static void add_var(UA_Server *s, const char *id, UA_Int32 init)
{
    UA_VariableAttributes a = UA_VariableAttributes_default;
    UA_Variant_setScalar(&a.value, &init, &UA_TYPES[UA_TYPES_INT32]);
    a.dataType = UA_TYPES[UA_TYPES_INT32].typeId;
    a.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    a.displayName = UA_LOCALIZEDTEXT("en-US", (char *)id);
    UA_Server_addVariableNode(s, UA_NODEID_STRING(1, (char *)id),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                              UA_QUALIFIEDNAME(1, (char *)id),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), a, NULL, NULL);
}

static void build_model(UA_Server *s)
{
    add_var(s, "answer", 42);
    add_var(s, "counter", 0);

    UA_Argument in[2], out;
    for (int i = 0; i < 2; i++) {
        UA_Argument_init(&in[i]);
        in[i].name = UA_STRING(i ? "b" : "a");
        in[i].dataType = UA_TYPES[UA_TYPES_INT32].typeId;
        in[i].valueRank = UA_VALUERANK_SCALAR;
    }
    UA_Argument_init(&out);
    out.name = UA_STRING("sum");
    out.dataType = UA_TYPES[UA_TYPES_INT32].typeId;
    out.valueRank = UA_VALUERANK_SCALAR;
    UA_MethodAttributes ma = UA_MethodAttributes_default;
    ma.displayName = UA_LOCALIZEDTEXT("en-US", "add");
    ma.executable = ma.userExecutable = true;
    UA_Server_addMethodNode(s, UA_NODEID_STRING(1, "add"), UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                            UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT), UA_QUALIFIEDNAME(1, "add"),
                            ma, add_method, 2, in, 1, &out, NULL, NULL);

    UA_VariableAttributes qa = UA_VariableAttributes_default;
    qa.dataType = UA_TYPES[UA_TYPES_STRING].typeId;
    qa.accessLevel = UA_ACCESSLEVELMASK_READ;
    qa.displayName = UA_LOCALIZEDTEXT("en-US", "qnxinfo");
    UA_DataSource ds = { .read = read_qnxinfo, .write = NULL };
    UA_Server_addDataSourceVariableNode(s, UA_NODEID_STRING(1, "qnxinfo"),
                                        UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                                        UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                                        UA_QUALIFIEDNAME(1, "qnxinfo"),
                                        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), qa, ds,
                                        NULL, NULL);
    UA_Server_addRepeatedCallback(s, tick, NULL, 10.0, NULL);
}

static void *server_thread(void *arg)
{
    UA_Server *s = arg;
    while (g_running) UA_Server_run_iterate(s, true);
    return NULL;
}

/* ---------------------------------------------------------------- client */

static UA_Int32 g_last_seen = -1;
static unsigned g_notifs, g_nonincreasing;

static void on_change(UA_Client *c, UA_UInt32 sub, void *sctx, UA_UInt32 mon, void *mctx,
                      UA_DataValue *v)
{
    (void)c; (void)sub; (void)sctx; (void)mon; (void)mctx;
    if (!v->hasValue || v->value.type != &UA_TYPES[UA_TYPES_INT32]) return;
    UA_Int32 x = *(UA_Int32 *)v->value.data;
    if (g_last_seen >= 0 && x <= g_last_seen) g_nonincreasing++;
    g_last_seen = x;
    g_notifs++;
}

int main(int argc, char **argv)
{
    int n = 1000;
    if (argc > 1) n = atoi(argv[1]);
    if (n < 10) n = 10;

    /* warnings and errors only, so the console stays readable */
    UA_ServerConfig scfg;
    memset(&scfg, 0, sizeof(scfg));
    scfg.logging = UA_Log_Stdout_new(UA_LOGLEVEL_WARNING);
    UA_ServerConfig_setMinimal(&scfg, (UA_UInt16)g_port, NULL);
    UA_Server *srv = UA_Server_newWithConfig(&scfg);
    build_model(srv);
    UA_StatusCode rc = UA_Server_run_startup(srv);
    CHECK("startup", rc == UA_STATUSCODE_GOOD, "server on port %u: %s", g_port, UA_StatusCode_name(rc));
    if (rc != UA_STATUSCODE_GOOD) goto out;
    pthread_t th;
    pthread_create(&th, NULL, server_thread, srv);

    UA_ClientConfig ccfg;
    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.logging = UA_Log_Stdout_new(UA_LOGLEVEL_WARNING);
    UA_ClientConfig_setDefault(&ccfg);
    UA_Client *cl = UA_Client_newWithConfig(&ccfg);
    char url[64];
    snprintf(url, sizeof(url), "opc.tcp://localhost:%u", g_port);
    double t0 = now_us();
    rc = UA_Client_connect(cl, url);
    CHECK("connect", rc == UA_STATUSCODE_GOOD, "%s in %.1f ms: %s", url, (now_us() - t0) / 1000,
          UA_StatusCode_name(rc));
    if (rc != UA_STATUSCODE_GOOD) goto stop;

    /* browse */
    UA_BrowseRequest br;
    UA_BrowseRequest_init(&br);
    br.nodesToBrowse = UA_BrowseDescription_new();
    br.nodesToBrowseSize = 1;
    br.nodesToBrowse[0].nodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    br.nodesToBrowse[0].resultMask = UA_BROWSERESULTMASK_ALL;
    UA_BrowseResponse bres = UA_Client_Service_browse(cl, br);
    int found = 0;
    size_t nref = 0;
    if (bres.resultsSize == 1) {
        nref = bres.results[0].referencesSize;
        for (size_t i = 0; i < nref; i++) {
            UA_String *bn = &bres.results[0].references[i].browseName.name;
            if (UA_String_equal(bn, &(UA_String)UA_STRING_STATIC("answer")) ||
                UA_String_equal(bn, &(UA_String)UA_STRING_STATIC("add")) ||
                UA_String_equal(bn, &(UA_String)UA_STRING_STATIC("Server")))
                found++;
        }
    }
    CHECK("browse", found == 3, "Objects folder: %zu references, found answer/add/Server=%d/3", nref, found);
    UA_BrowseRequest_clear(&br);
    UA_BrowseResponse_clear(&bres);

    /* read */
    double *lv = malloc(sizeof(double) * n);
    int okr = 0;
    for (int i = 0; i < n; i++) {
        UA_Variant v;
        UA_Variant_init(&v);
        double a = now_us();
        rc = UA_Client_readValueAttribute(cl, UA_NODEID_STRING(1, "answer"), &v);
        lv[i] = now_us() - a;
        if (rc == UA_STATUSCODE_GOOD && v.type == &UA_TYPES[UA_TYPES_INT32] && *(UA_Int32 *)v.data == 42) okr++;
        UA_Variant_clear(&v);
    }
    CHECK("read", okr == n, "%d/%d reads returned 42", okr, n);
    lat("read", lv, n);

    /* write */
    int okw = 0;
    for (int i = 0; i < n; i++) {
        UA_Int32 x = 1000 + i;
        UA_Variant v;
        UA_Variant_setScalar(&v, &x, &UA_TYPES[UA_TYPES_INT32]);
        double a = now_us();
        rc = UA_Client_writeValueAttribute(cl, UA_NODEID_STRING(1, "answer"), &v);
        lv[i] = now_us() - a;
        if (rc == UA_STATUSCODE_GOOD) okw++;
    }
    UA_Variant rb;
    UA_Variant_init(&rb);
    UA_Client_readValueAttribute(cl, UA_NODEID_STRING(1, "answer"), &rb);
    UA_Int32 last = rb.type == &UA_TYPES[UA_TYPES_INT32] ? *(UA_Int32 *)rb.data : -1;
    UA_Variant_clear(&rb);
    CHECK("write", okw == n && last == 1000 + n - 1, "%d/%d writes ok, read-back %d (expected %d)",
          okw, n, (int)last, 1000 + n - 1);
    lat("write", lv, n);

    /* call */
    int okc = 0;
    int calls = n / 10;
    for (int i = 0; i < calls; i++) {
        UA_Int32 a = i, b = 7;
        UA_Variant in[2];
        UA_Variant_setScalar(&in[0], &a, &UA_TYPES[UA_TYPES_INT32]);
        UA_Variant_setScalar(&in[1], &b, &UA_TYPES[UA_TYPES_INT32]);
        size_t on = 0;
        UA_Variant *ov = NULL;
        double t = now_us();
        rc = UA_Client_call(cl, UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER), UA_NODEID_STRING(1, "add"),
                            2, in, &on, &ov);
        lv[i] = now_us() - t;
        if (rc == UA_STATUSCODE_GOOD && on == 1 && *(UA_Int32 *)ov[0].data == i + 7) okc++;
        UA_Array_delete(ov, on, &UA_TYPES[UA_TYPES_VARIANT]);
    }
    CHECK("call", okc == calls, "%d/%d add(i,7) results correct", okc, calls);
    lat("call", lv, calls);

    /* subscribe */
    UA_CreateSubscriptionRequest sreq = UA_CreateSubscriptionRequest_default();
    sreq.requestedPublishingInterval = 10.0;
    UA_CreateSubscriptionResponse sres = UA_Client_Subscriptions_create(cl, sreq, NULL, NULL, NULL);
    UA_MonitoredItemCreateRequest mreq = UA_MonitoredItemCreateRequest_default(UA_NODEID_STRING(1, "counter"));
    mreq.requestedParameters.samplingInterval = 10.0;
    mreq.requestedParameters.queueSize = 10;
    UA_MonitoredItemCreateResult mres = UA_Client_MonitoredItems_createDataChange(
        cl, sres.subscriptionId, UA_TIMESTAMPSTORETURN_BOTH, mreq, NULL, on_change, NULL);
    double ts = now_us();
    while (now_us() - ts < 2e6) UA_Client_run_iterate(cl, 20);
    double secs = (now_us() - ts) / 1e6;
    CHECK("subscribe", sres.responseHeader.serviceResult == UA_STATUSCODE_GOOD &&
                           mres.statusCode == UA_STATUSCODE_GOOD && g_notifs >= 20 && g_nonincreasing == 0,
          "%u notifications in %.1f s (%.0f/s, revised publish %.0f ms), %u out of order",
          g_notifs, secs, g_notifs / secs, sres.revisedPublishingInterval, g_nonincreasing);
    UA_Client_Subscriptions_deleteSingle(cl, sres.subscriptionId);

    /* qnxinfo */
    UA_Variant qv;
    UA_Variant_init(&qv);
    rc = UA_Client_readValueAttribute(cl, UA_NODEID_STRING(1, "qnxinfo"), &qv);
    if (rc == UA_STATUSCODE_GOOD && qv.type == &UA_TYPES[UA_TYPES_STRING]) {
        UA_String *s = qv.data;
        CHECK("qnxinfo", s->length > 0, "%.*s", (int)s->length, (char *)s->data);
    } else {
        CHECK("qnxinfo", 0, "%s", UA_StatusCode_name(rc));
    }
    UA_Variant_clear(&qv);
    free(lv);

    UA_Client_disconnect(cl);
stop:
    UA_Client_delete(cl);
    g_running = false;
    pthread_join(th, NULL);
    UA_Server_run_shutdown(srv);
out:
    UA_Server_delete(srv);
    printf("O6Q SUMMARY pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
