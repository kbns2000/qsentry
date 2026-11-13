/* See o6q_common.h.  SPDX-License-Identifier: Apache-2.0 */
#ifndef _QNX_SOURCE
#define _QNX_SOURCE
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "o6q_common.h"

#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static struct {
    pthread_mutex_t lock; /* one output line at a time */
    char suite[32];
    char test[64]; /* the running test; read by the signal handlers */
    unsigned pass, fail, skip;
    unsigned log_warnings, log_errors; /* unexpected ones only */
    unsigned log_lines, log_suppressed;
    double t0_us;
    int expect_errors;
    int verbose;
} g = {.lock = PTHREAD_MUTEX_INITIALIZER};

/* ------------------------------------------------------------ time, stats */

double o6q_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

void o6q_sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while(nanosleep(&ts, &ts) != 0) {
        /* interrupted by a signal: sleep the rest */
    }
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double percentile(const double *sorted, size_t n, double p) {
    /* nearest rank */
    size_t rank = (size_t)(p * (double)n + 0.999999);
    if(rank < 1)
        rank = 1;
    if(rank > n)
        rank = n;
    return sorted[rank - 1];
}

void o6q_lat(const char *what, double *v, size_t n) {
    double sum = 0;
    if(n == 0) {
        o6q_info("%s: no samples", what);
        return;
    }
    qsort(v, n, sizeof(double), cmp_double);
    for(size_t i = 0; i < n; i++)
        sum += v[i];
    pthread_mutex_lock(&g.lock);
    printf("O6Q LAT %s.%s n=%lu p50_us=%.1f p90_us=%.1f p99_us=%.1f max_us=%.1f mean_us=%.1f\n",
           g.suite, what, (unsigned long)n, percentile(v, n, 0.50), percentile(v, n, 0.90),
           percentile(v, n, 0.99), v[n - 1], sum / (double)n);
    pthread_mutex_unlock(&g.lock);
}

int o6q_open_fds(void) {
    long max = sysconf(_SC_OPEN_MAX);
    int count = 0;
    if(max < 0 || max > 4096)
        max = 4096;
    for(int fd = 0; fd < (int)max; fd++) {
        if(fcntl(fd, F_GETFD) != -1)
            count++;
    }
    return count;
}

/* -------------------------------------------------------------- reporting */

/* Only async-signal-safe calls below: write() and _exit(). */
static void emit(const char *s) {
    size_t n = 0;
    while(s[n])
        n++;
    while(n > 0) {
        ssize_t w = write(STDOUT_FILENO, s, n);
        if(w <= 0)
            break;
        s += w;
        n -= (size_t)w;
    }
}

static void on_fatal_signal(int sig) {
    const char *name = "signal";
    switch(sig) {
    case SIGALRM: name = "SIGALRM(watchdog)"; break;
    case SIGSEGV: name = "SIGSEGV"; break;
    case SIGBUS: name = "SIGBUS"; break;
    case SIGFPE: name = "SIGFPE"; break;
    case SIGILL: name = "SIGILL"; break;
    case SIGABRT: name = "SIGABRT"; break;
    default: break;
    }
    emit("\nO6Q FAIL ");
    emit(g.suite);
    emit(".");
    emit(g.test[0] ? g.test : "unknown");
    emit(" signal=");
    emit(name);
    emit("\nO6Q ABORT suite=");
    emit(g.suite);
    emit(" signal=");
    emit(name);
    emit("\n");
    _exit(sig == SIGALRM ? 3 : 4);
}

void o6q_begin(const char *suite, unsigned watchdog_seconds) {
    static const int fatal[] = {SIGALRM, SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
    struct sigaction sa;

    setvbuf(stdout, NULL, _IOLBF, 0);

    pthread_mutex_lock(&g.lock);
#ifdef O6Q_VARIANT
    snprintf(g.suite, sizeof(g.suite), "%s@%s", suite, O6Q_VARIANT);
#else
    snprintf(g.suite, sizeof(g.suite), "%s", suite);
#endif
    snprintf(g.test, sizeof(g.test), "%s", "begin");
    g.pass = g.fail = g.skip = 0;
    g.log_warnings = g.log_errors = 0;
    g.log_lines = g.log_suppressed = 0;
    g.expect_errors = 0;
    g.verbose = getenv("O6Q_VERBOSE") != NULL;
    g.t0_us = o6q_now_us();
    printf("O6Q BEGIN suite=%s\n", g.suite);
    pthread_mutex_unlock(&g.lock);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_fatal_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = (int)SA_RESETHAND;
    for(size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++)
        sigaction(fatal[i], &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    alarm(watchdog_seconds);
}

int o6q_end(void) {
    int failures;
    alarm(0);
    pthread_mutex_lock(&g.lock);
    snprintf(g.test, sizeof(g.test), "%s", "end");
    printf("O6Q SUMMARY suite=%s pass=%u fail=%u skip=%u secs=%.1f log_warnings=%u log_errors=%u "
           "log_suppressed=%u\n",
           g.suite, g.pass, g.fail, g.skip, (o6q_now_us() - g.t0_us) / 1e6,
           g.log_warnings, g.log_errors, g.log_suppressed);
    failures = (int)g.fail;
    pthread_mutex_unlock(&g.lock);
    return failures;
}

void o6q_step(const char *test) {
    pthread_mutex_lock(&g.lock);
    snprintf(g.test, sizeof(g.test), "%s", test);
    pthread_mutex_unlock(&g.lock);
}

void o6q_report(int ok, const char *test, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g.lock);
    if(ok)
        g.pass++;
    else
        g.fail++;
    printf("O6Q %s %s.%s %s\n", ok ? "PASS" : "FAIL", g.suite, test, buf);
    pthread_mutex_unlock(&g.lock);
}

void o6q_skip(const char *test, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g.lock);
    g.skip++;
    printf("O6Q SKIP %s.%s %s\n", g.suite, test, buf);
    pthread_mutex_unlock(&g.lock);
}

void o6q_info(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g.lock);
    printf("O6Q INFO %s %s\n", g.suite, buf);
    pthread_mutex_unlock(&g.lock);
}

/* ---------------------------------------------------------------- logging */

static void log_cb(void *context, UA_LogLevel level, UA_LogCategory category,
                   const char *msg, va_list args) {
    static const char *const categories[] = {
        "network", "channel", "session", "server", "client",
        "userland", "security", "eventloop", "pubsub", "discovery"};
    const char *lvl;
    const char *cat = "other";
    char buf[1024];
    (void)context;

    if((int)level < (int)UA_LOGLEVEL_WARNING && !g.verbose)
        return;
    if((int)level >= (int)UA_LOGLEVEL_FATAL)
        lvl = "fatal";
    else if((int)level >= (int)UA_LOGLEVEL_ERROR)
        lvl = "error";
    else if((int)level >= (int)UA_LOGLEVEL_WARNING)
        lvl = "warn";
    else if((int)level >= (int)UA_LOGLEVEL_INFO)
        lvl = "info";
    else
        lvl = "debug";
    if((int)category >= 0 && (size_t)category < sizeof(categories) / sizeof(categories[0]))
        cat = categories[(int)category];

    vsnprintf(buf, sizeof(buf), msg, args);

    pthread_mutex_lock(&g.lock);
    if(!g.expect_errors) {
        if((int)level >= (int)UA_LOGLEVEL_ERROR)
            g.log_errors++;
        else if((int)level >= (int)UA_LOGLEVEL_WARNING)
            g.log_warnings++;
    }
    if(g.log_lines < O6Q_LOG_CAP) {
        printf("O6Q LOG %s%s/%s %s\n", g.expect_errors ? "(expected) " : "", lvl, cat, buf);
        if(++g.log_lines == O6Q_LOG_CAP)
            printf("O6Q INFO %s %d log lines printed; the rest is only counted\n", g.suite,
                   O6Q_LOG_CAP);
    } else {
        g.log_suppressed++;
    }
    pthread_mutex_unlock(&g.lock);
}

/* clear == NULL: the configurations that share this logger never free it. */
static UA_Logger g_logger = {log_cb, NULL, NULL};

UA_Logger *o6q_logger(void) {
    return &g_logger;
}

void o6q_expect_errors(int on) {
    pthread_mutex_lock(&g.lock);
    g.expect_errors = on;
    pthread_mutex_unlock(&g.lock);
}

/* --------------------------------------------------------- server harness */

static void *server_thread(void *arg) {
    O6QServer *s = (O6QServer *)arg;
    while(s->running)
        UA_Server_run_iterate(s->server, true);
    return NULL;
}

UA_StatusCode o6q_server_run(O6QServer *s) {
    pthread_attr_t attr;
    int err;
    UA_StatusCode rc = UA_Server_run_startup(s->server);
    if(rc != UA_STATUSCODE_GOOD)
        return rc;
    /* Do not depend on the default thread stack of the system (256 KiB on
     * QNX, 8 MiB on Linux): ask for what the server gets everywhere. */
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, O6Q_SERVER_STACK);
    s->running = 1;
    err = pthread_create(&s->thread, &attr, server_thread, s);
    pthread_attr_destroy(&attr);
    if(err != 0) {
        s->running = 0;
        UA_Server_run_shutdown(s->server);
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    s->started = 1;
    return UA_STATUSCODE_GOOD;
}

void o6q_server_stop(O6QServer *s) {
    if(s->started) {
        s->running = 0;
        pthread_join(s->thread, NULL);
        UA_Server_run_shutdown(s->server);
        s->started = 0;
    }
    if(s->server) {
        UA_Server_delete(s->server);
        s->server = NULL;
    }
}

void o6q_server_limits(UA_ServerConfig *cfg) {
    /* The defaults (100 ms publishing, 50 ms sampling) are too coarse to say
     * anything about a real-time target. */
    cfg->publishingIntervalLimits.min = 5.0;
    cfg->samplingIntervalLimits.min = 5.0;
    cfg->queueSizeLimits.max = 1000;
}

UA_Client *o6q_client_new(void) {
    UA_ClientConfig cc;
    UA_Client *client;
    memset(&cc, 0, sizeof(cc));
    cc.logging = o6q_logger();
    if(UA_ClientConfig_setDefault(&cc) != UA_STATUSCODE_GOOD) {
        UA_ClientConfig_clear(&cc);
        return NULL;
    }
    cc.timeout = 30000; /* ms; QEMU without hardware acceleration is slow */
    client = UA_Client_newWithConfig(&cc);
    if(!client)
        UA_ClientConfig_clear(&cc);
    return client;
}
