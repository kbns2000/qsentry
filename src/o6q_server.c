/* o6q_server: the live state of a QNX 8.0 system as an OPC UA server.
 *
 *   o6q_server [-p port] [-s seconds] [--secure] [--trust dir]
 *              [--user name:password] [--log-stderr] [-v]
 *
 *   -p port        TCP port (default 4840)
 *   -s seconds     stop after that many seconds (default: until SIGINT/SIGTERM)
 *   --secure       also offer encrypted endpoints (Basic256Sha256,
 *                  Aes128_Sha256_RsaOaep, Aes256_Sha256_RsaPss; Sign and
 *                  SignAndEncrypt) with a self-signed RSA-2048 certificate
 *                  that is made at start
 *   --trust dir    with --secure: accept only clients whose certificate is a
 *                  file (DER or PEM) in dir. The directory is read again for
 *                  every new connection, so a client can be admitted or
 *                  locked out while the server runs. Without --trust every
 *                  client certificate is accepted (the stack warns).
 *   --user n:p     with --secure: require this user name and password;
 *                  anonymous login is switched off
 *   --log-stderr   print the log messages on stderr as well (they always
 *                  go there when slog2 is not available)
 *   -v             log at level INFO instead of WARNING
 *
 * Address space, namespace "urn:open62541-qnx:system", below Objects/QNX:
 *   OS, Release, Machine, Hostname   String    uname()
 *   CpuCount                         UInt32    system page
 *   CyclesPerSecond                  UInt64    frequency of ClockCycles()
 *   ClockPeriodNs                    UInt32    kernel clock period
 *   RamTotalMB                       Double    RAM ranges of the system page
 *   Uptime                           Double    seconds, CLOCK_MONOTONIC
 *   RamFreeMB                        Double    size of /proc
 *   Processes, Threads               UInt32    procfs
 *   CpuLoad                          Double[]  percent busy, per CPU
 *   CpuLoadAverage                   Double    percent busy, all CPUs
 *   ProcessList                      String[]  "pid name (threads)", every 5 s
 *   OpcUaSessions                    UInt32    sessions of this server
 *   Stack                            String    open62541 version, event loop
 * The dynamic values are sampled once per second; clients can subscribe.
 * On other systems the QNX-only values stay 0.
 *
 * Log messages of the stack go to slog2 (read them with
 * "slog2info -b o6q_server"). The program prints "O6Q DEMO ..." lines on
 * stdout: one when it is ready, then one every 10 seconds.
 *
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef _QNX_SOURCE
#define _QNX_SOURCE
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "open62541.h"
#include "o6q_slog.h"
#include "o6q_sysinfo.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NAMESPACE_URI "urn:open62541-qnx:system"
#define SERVER_URI "urn:open62541.server.application" /* the default of open62541 */
#define MAX_PROCS 512
#define SAMPLE_MS 1000.0
#define PROCLIST_EVERY 5   /* samples */
#define STATUS_EVERY 10    /* samples */

#if defined(O6Q_EVENTLOOP_POLL) || (!defined(O6Q_EVENTLOOP_SELECT) && defined(__QNXNTO__))
#define EVENTLOOP_NAME "ppoll"
#elif defined(O6Q_EVENTLOOP_SELECT) || !defined(__linux__)
#define EVENTLOOP_NAME "select"
#else
#define EVENTLOOP_NAME "epoll"
#endif

enum {
    V_OS, V_RELEASE, V_MACHINE, V_HOSTNAME, V_CPUCOUNT, V_CYCLES, V_CLOCKPERIOD, V_RAMTOTAL,
    V_UPTIME, V_RAMFREE, V_PROCESSES, V_THREADS, V_CPULOAD, V_CPULOADAVG, V_PROCLIST,
    V_SESSIONS, V_STACK, V_COUNT
};

static const char *const var_names[V_COUNT] = {
    "OS", "Release", "Machine", "Hostname", "CpuCount", "CyclesPerSecond", "ClockPeriodNs",
    "RamTotalMB", "Uptime", "RamFreeMB", "Processes", "Threads", "CpuLoad", "CpuLoadAverage",
    "ProcessList", "OpcUaSessions", "Stack"};

typedef struct {
    UA_UInt16 ns;
    O6QSysStatic st;
    O6QSysSample sample;
    double load_avg;
    unsigned samples;
    O6QProc procs[MAX_PROCS];
    double started_s;
} Demo;

static volatile UA_Boolean running = true;

static void on_signal(int sig) {
    (void)sig;
    running = false;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static UA_NodeId var_id(const Demo *d, int v) {
    static char ids[V_COUNT][48];
    snprintf(ids[v], sizeof(ids[v]), "QNX.%s", var_names[v]);
    return UA_NODEID_STRING(d->ns, ids[v]);
}

/* ------------------------------------------------------------- the values */

/* Writes one value; failures are logged by the stack. */
static void write_scalar(UA_Server *server, const Demo *d, int v, void *value,
                         const UA_DataType *type) {
    UA_Variant var;
    UA_Variant_setScalar(&var, value, type);
    UA_Server_writeValue(server, var_id(d, v), var);
}

static void update_load(Demo *d, UA_Double *loads, size_t *n) {
    double sum = 0.0;
    unsigned valid = 0;
    *n = d->sample.num_cpu;
    for(size_t i = 0; i < *n; i++) {
        double l = d->sample.cpu_load[i];
        loads[i] = l >= 0.0 ? l : NAN; /* NaN: not known yet (first sample) */
        if(l >= 0.0) {
            sum += l;
            valid++;
        }
    }
    d->load_avg = valid ? sum / valid : NAN;
}

static void update_process_list(UA_Server *server, Demo *d) {
    static char text[MAX_PROCS][80];
    UA_String list[MAX_PROCS];
    unsigned threads = 0;
    size_t found = o6q_sys_processes(d->procs, MAX_PROCS, &threads);
    size_t n = found < MAX_PROCS ? found : MAX_PROCS;
    UA_Variant var;
    for(size_t i = 0; i < n; i++) {
        snprintf(text[i], sizeof(text[i]), "%d %s (%u threads)", d->procs[i].pid,
                 d->procs[i].name, d->procs[i].threads);
        list[i] = UA_STRING(text[i]);
    }
    UA_Variant_setArray(&var, list, n, &UA_TYPES[UA_TYPES_STRING]);
    UA_Server_writeValue(server, var_id(d, V_PROCLIST), var);
}

/* Once per second, in the thread of the server. */
static void sample_cb(UA_Server *server, void *data) {
    Demo *d = (Demo *)data;
    UA_Double uptime, ram_free, loads[O6Q_MAX_CPU], avg;
    UA_UInt32 processes, threads, sessions;
    UA_Variant var;
    size_t n = 0;

    o6q_sys_sample(&d->sample);
    d->samples++;
    update_load(d, loads, &n);

    uptime = d->sample.uptime_s;
    ram_free = (double)d->sample.ram_free / (1024.0 * 1024.0);
    processes = d->sample.processes;
    threads = d->sample.threads;
    avg = d->load_avg;
    sessions = (UA_UInt32)UA_Server_getStatistics(server).ss.currentSessionCount;

    write_scalar(server, d, V_UPTIME, &uptime, &UA_TYPES[UA_TYPES_DOUBLE]);
    write_scalar(server, d, V_RAMFREE, &ram_free, &UA_TYPES[UA_TYPES_DOUBLE]);
    write_scalar(server, d, V_PROCESSES, &processes, &UA_TYPES[UA_TYPES_UINT32]);
    write_scalar(server, d, V_THREADS, &threads, &UA_TYPES[UA_TYPES_UINT32]);
    write_scalar(server, d, V_CPULOADAVG, &avg, &UA_TYPES[UA_TYPES_DOUBLE]);
    write_scalar(server, d, V_SESSIONS, &sessions, &UA_TYPES[UA_TYPES_UINT32]);
    UA_Variant_setArray(&var, loads, n, &UA_TYPES[UA_TYPES_DOUBLE]);
    UA_Server_writeValue(server, var_id(d, V_CPULOAD), var);
    if(d->samples % PROCLIST_EVERY == 0)
        update_process_list(server, d);

    if(d->samples % STATUS_EVERY == 0)
        printf("O6Q DEMO uptime_s=%.0f cpu_load_avg=%.1f ram_free_mb=%.1f processes=%u "
               "threads=%u opcua_sessions=%u\n",
               uptime, avg, ram_free, (unsigned)processes, (unsigned)threads,
               (unsigned)sessions);
}

/* ---------------------------------------------------------- address space */

static UA_StatusCode add_var(UA_Server *server, const Demo *d, int v, const UA_Variant *value,
                             const UA_DataType *type, UA_Int32 rank, const char *description) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    UA_NodeId id = var_id(d, v);
    attr.displayName = UA_LOCALIZEDTEXT("en", (char *)(uintptr_t)var_names[v]);
    attr.description = UA_LOCALIZEDTEXT("en", (char *)(uintptr_t)description);
    attr.value = *value; /* copied by the server */
    attr.dataType = type->typeId;
    attr.valueRank = rank;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ;
    return UA_Server_addVariableNode(
        server, id, UA_NODEID_STRING(d->ns, "QNX"), UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
        UA_QUALIFIEDNAME(d->ns, (char *)(uintptr_t)var_names[v]),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, NULL, NULL);
}

static UA_StatusCode add_string(UA_Server *server, const Demo *d, int v, const char *text,
                                const char *description) {
    UA_String s = UA_STRING((char *)(uintptr_t)text);
    UA_Variant var;
    UA_Variant_setScalar(&var, &s, &UA_TYPES[UA_TYPES_STRING]);
    return add_var(server, d, v, &var, &UA_TYPES[UA_TYPES_STRING], UA_VALUERANK_SCALAR,
                   description);
}

static UA_StatusCode add_number(UA_Server *server, const Demo *d, int v, void *value,
                                const UA_DataType *type, const char *description) {
    UA_Variant var;
    UA_Variant_setScalar(&var, value, type);
    return add_var(server, d, v, &var, type, UA_VALUERANK_SCALAR, description);
}

static UA_StatusCode add_model(UA_Server *server, Demo *d) {
    UA_ObjectAttributes oattr = UA_ObjectAttributes_default;
    UA_StatusCode rc;
    char stack[96];
    UA_UInt32 cpus, period;
    UA_UInt64 cycles;
    UA_Double ram_total, zero = 0.0, loads[O6Q_MAX_CPU];
    UA_UInt32 zero32 = 0;
    UA_Variant var;
    size_t n = 0;

    d->ns = UA_Server_addNamespace(server, NAMESPACE_URI);
    oattr.displayName = UA_LOCALIZEDTEXT("en", "QNX");
    oattr.description = UA_LOCALIZEDTEXT("en", "The QNX system this server runs on");
    rc = UA_Server_addObjectNode(server, UA_NODEID_STRING(d->ns, "QNX"),
                                 UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                                 UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                                 UA_QUALIFIEDNAME(d->ns, "QNX"),
                                 UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE), oattr, NULL,
                                 NULL);
    if(rc != UA_STATUSCODE_GOOD)
        return rc;

    cpus = d->st.num_cpu;
    cycles = d->st.cycles_per_sec;
    period = (UA_UInt32)d->st.clock_period_ns;
    ram_total = (double)d->st.ram_total / (1024.0 * 1024.0);
    snprintf(stack, sizeof(stack), "open62541 %s (open62541-qnx), event loop %s",
             UA_OPEN62541_VERSION, EVENTLOOP_NAME);

    rc |= add_string(server, d, V_OS, d->st.sysname, "Operating system (uname)");
    rc |= add_string(server, d, V_RELEASE, d->st.release, "OS release (uname)");
    rc |= add_string(server, d, V_MACHINE, d->st.machine, "CPU architecture (uname)");
    rc |= add_string(server, d, V_HOSTNAME, d->st.hostname, "Host name");
    rc |= add_number(server, d, V_CPUCOUNT, &cpus, &UA_TYPES[UA_TYPES_UINT32],
                     "Number of CPUs (system page)");
    rc |= add_number(server, d, V_CYCLES, &cycles, &UA_TYPES[UA_TYPES_UINT64],
                     "Frequency of the ClockCycles() counter, per second");
    rc |= add_number(server, d, V_CLOCKPERIOD, &period, &UA_TYPES[UA_TYPES_UINT32],
                     "Kernel clock period in nanoseconds");
    rc |= add_number(server, d, V_RAMTOTAL, &ram_total, &UA_TYPES[UA_TYPES_DOUBLE],
                     "RAM in MiB (system page)");
    rc |= add_number(server, d, V_UPTIME, &zero, &UA_TYPES[UA_TYPES_DOUBLE],
                     "Seconds since boot (CLOCK_MONOTONIC)");
    rc |= add_number(server, d, V_RAMFREE, &zero, &UA_TYPES[UA_TYPES_DOUBLE],
                     "Free RAM in MiB");
    rc |= add_number(server, d, V_PROCESSES, &zero32, &UA_TYPES[UA_TYPES_UINT32],
                     "Number of processes");
    rc |= add_number(server, d, V_THREADS, &zero32, &UA_TYPES[UA_TYPES_UINT32],
                     "Number of threads in all processes");
    update_load(d, loads, &n);
    UA_Variant_setArray(&var, loads, n, &UA_TYPES[UA_TYPES_DOUBLE]);
    rc |= add_var(server, d, V_CPULOAD, &var, &UA_TYPES[UA_TYPES_DOUBLE],
                  UA_VALUERANK_ONE_DIMENSION, "Percent of time each CPU was busy");
    rc |= add_number(server, d, V_CPULOADAVG, &zero, &UA_TYPES[UA_TYPES_DOUBLE],
                     "Percent of time all CPUs were busy");
    UA_Variant_setArray(&var, NULL, 0, &UA_TYPES[UA_TYPES_STRING]);
    rc |= add_var(server, d, V_PROCLIST, &var, &UA_TYPES[UA_TYPES_STRING],
                  UA_VALUERANK_ONE_DIMENSION, "Processes: pid, name, number of threads");
    rc |= add_number(server, d, V_SESSIONS, &zero32, &UA_TYPES[UA_TYPES_UINT32],
                     "OPC UA sessions of this server");
    rc |= add_string(server, d, V_STACK, stack, "The OPC UA stack of this server");
    return rc;
}

/* ------------------------------------------------------------ security */

static UA_StatusCode make_certificate(const UA_Logger *logger, const char *hostname,
                                      UA_ByteString *cert, UA_ByteString *key) {
    UA_String subject[3], alt[3];
    char cn[96], dns[96];
    UA_UInt16 bits = 2048, days = 365;
    UA_KeyValueMap *params = UA_KeyValueMap_new();
    UA_StatusCode rc;
    if(!params)
        return UA_STATUSCODE_BADOUTOFMEMORY;
    snprintf(cn, sizeof(cn), "CN=o6q_server@%.64s", hostname[0] ? hostname : "qnx");
    snprintf(dns, sizeof(dns), "DNS:%.64s", hostname[0] ? hostname : "localhost");
    subject[0] = UA_STRING("C=CA");
    subject[1] = UA_STRING("O=open62541-qnx");
    subject[2] = UA_STRING(cn);
    alt[0] = UA_STRING("DNS:localhost");
    alt[1] = UA_STRING(dns);
    alt[2] = UA_STRING("URI:" SERVER_URI);
    UA_KeyValueMap_setScalar(params, UA_QUALIFIEDNAME(0, "key-size-bits"), &bits,
                             &UA_TYPES[UA_TYPES_UINT16]);
    UA_KeyValueMap_setScalar(params, UA_QUALIFIEDNAME(0, "expires-in-days"), &days,
                             &UA_TYPES[UA_TYPES_UINT16]);
    rc = UA_CreateCertificate(logger, subject, 3, alt, 3, UA_CERTIFICATEFORMAT_DER, params, key,
                              cert);
    UA_KeyValueMap_delete(params);
    return rc;
}

/* ------------------------------------------------------------------ main */

static int usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [-p port] [-s seconds] [--secure] [--trust dir] [--user name:password]"
            " [--log-stderr] [-v]\n",
            prog);
    return 2;
}

int main(int argc, char **argv) {
    static Demo demo;
    Demo *d = &demo;
    unsigned port = 4840;
    double seconds = 0.0;
    int secure = 0, to_stderr = 0, verbose = 0;
    const char *trust = NULL, *user = NULL, *where = "?";
    char user_name[64] = "", user_pass[64] = "";
    UA_ServerConfig cfg;
    UA_Server *server = NULL;
    UA_ByteString cert = UA_BYTESTRING_NULL, key = UA_BYTESTRING_NULL;
    UA_Logger *logger;
    UA_StatusCode rc;
    struct sigaction sa;
    int status = 0;

    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            long p = strtol(argv[++i], NULL, 10);
            if(p < 1 || p > 65535)
                return usage(argv[0]);
            port = (unsigned)p;
        } else if(strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            seconds = strtod(argv[++i], NULL);
        } else if(strcmp(argv[i], "--secure") == 0) {
            secure = 1;
        } else if(strcmp(argv[i], "--trust") == 0 && i + 1 < argc) {
            trust = argv[++i];
        } else if(strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
            const char *colon;
            user = argv[++i];
            colon = strchr(user, ':');
            if(!colon || colon == user || (size_t)(colon - user) >= sizeof(user_name) ||
               strlen(colon + 1) >= sizeof(user_pass))
                return usage(argv[0]);
            memcpy(user_name, user, (size_t)(colon - user));
            user_name[colon - user] = 0;
            snprintf(user_pass, sizeof(user_pass), "%s", colon + 1);
        } else if(strcmp(argv[i], "--log-stderr") == 0) {
            to_stderr = 1;
        } else if(strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else {
            return usage(argv[0]);
        }
    }
    if((trust || user) && !secure) {
        fprintf(stderr, "--trust and --user need --secure\n");
        return 2;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    logger = o6q_slog_logger(verbose ? UA_LOGLEVEL_INFO : UA_LOGLEVEL_WARNING, to_stderr, &where);

    o6q_sys_static(&d->st);
    o6q_sys_sample(&d->sample); /* the first CPU load sample is the baseline */
    d->started_s = now_s();

    memset(&cfg, 0, sizeof(cfg));
    cfg.logging = logger;
    if(secure) {
        rc = make_certificate(logger, d->st.hostname, &cert, &key);
        if(rc == UA_STATUSCODE_GOOD)
            rc = UA_ServerConfig_setDefaultWithSecurityPolicies(&cfg, (UA_UInt16)port, &cert,
                                                                &key, NULL, 0, NULL, 0, NULL, 0);
#if defined(__linux__) || defined(__QNXNTO__)
        if(rc == UA_STATUSCODE_GOOD && trust) {
            /* the trust list is a directory, read again for every connection
             * (patches/10-pki-folders.change brings this to QNX) */
            rc = UA_CertificateVerification_CertFolders(&cfg.secureChannelPKI, trust, NULL,
                                                        NULL);
            if(rc == UA_STATUSCODE_GOOD)
                rc = UA_CertificateVerification_CertFolders(&cfg.sessionPKI, trust, NULL, NULL);
            /* The function of the release leaves the structure without its
             * logger (the port keeps it); set it again in any case. */
            cfg.secureChannelPKI.logging = cfg.logging;
            cfg.sessionPKI.logging = cfg.logging;
        }
#else
        if(trust) {
            fprintf(stderr, "--trust is not available on this system\n");
            rc = UA_STATUSCODE_BADNOTSUPPORTED;
        }
#endif
        if(rc == UA_STATUSCODE_GOOD && user) {
            UA_UsernamePasswordLogin login;
            login.username = UA_STRING(user_name);
            login.password = UA_STRING(user_pass);
            rc = UA_AccessControl_default(&cfg, false, NULL, 1, &login);
        }
    } else {
        rc = UA_ServerConfig_setMinimal(&cfg, (UA_UInt16)port, NULL);
    }
    if(rc != UA_STATUSCODE_GOOD) {
        fprintf(stderr, "o6q_server: configuration failed: %s\n", UA_StatusCode_name(rc));
        UA_ServerConfig_clean(&cfg);
        status = 1;
        goto out;
    }
    UA_String_clear(&cfg.buildInfo.productName);
    cfg.buildInfo.productName = UA_STRING_ALLOC("open62541-qnx system server");
    UA_LocalizedText_clear(&cfg.applicationDescription.applicationName);
    cfg.applicationDescription.applicationName =
        UA_LOCALIZEDTEXT_ALLOC("en", "open62541-qnx system server");

    server = UA_Server_newWithConfig(&cfg);
    if(!server) {
        fprintf(stderr, "o6q_server: cannot create the server\n");
        status = 1;
        goto out;
    }
    rc = add_model(server, d);
    if(rc == UA_STATUSCODE_GOOD)
        rc = UA_Server_addRepeatedCallback(server, sample_cb, d, SAMPLE_MS, NULL);
    if(rc == UA_STATUSCODE_GOOD)
        rc = UA_Server_run_startup(server);
    if(rc != UA_STATUSCODE_GOOD) {
        fprintf(stderr, "o6q_server: start failed: %s\n", UA_StatusCode_name(rc));
        status = 1;
        goto out;
    }
    update_process_list(server, d);

    printf("O6Q DEMO READY port=%u secure=%s trust=%s user=%s log=%s os=%s release=%s "
           "machine=%s cpus=%u stack=%s eventloop=%s\n",
           port, secure ? "yes" : "no", trust ? trust : "-", user ? user_name : "-", where,
           d->st.sysname, d->st.release, d->st.machine, d->st.num_cpu, UA_OPEN62541_VERSION,
           EVENTLOOP_NAME);

    while(running) {
        UA_Server_run_iterate(server, true);
        if(seconds > 0.0 && now_s() - d->started_s >= seconds)
            running = false;
    }
    UA_Server_run_shutdown(server);
    {
        UA_ServerStatistics stats = UA_Server_getStatistics(server);
        printf("O6Q DEMO STOP after_s=%.0f sessions_total=%lu channels_total=%lu "
               "channels_rejected=%lu sessions_rejected=%lu\n",
               now_s() - d->started_s, (unsigned long)stats.ss.cumulatedSessionCount,
               (unsigned long)stats.scs.cumulatedChannelCount,
               (unsigned long)stats.scs.rejectedChannelCount,
               (unsigned long)(stats.ss.rejectedSessionCount +
                               stats.ss.securityRejectedSessionCount));
    }

out:
    if(server)
        UA_Server_delete(server);
    UA_ByteString_clear(&cert);
    UA_ByteString_clear(&key);
    return status;
}
