/* Shared helpers of the open62541-qnx on-target check and demo server.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef O6Q_COMMON_H_
#define O6Q_COMMON_H_

#include "open62541.h"

#include <pthread.h>
#include <stddef.h>

/* ------------------------------------------------------------- reporting
 *
 * Every result is one line that starts with "O6Q " so that the host tools
 * can pick the results out of a serial console log:
 *
 *   O6Q BUILD open62541=<version> eventloop=<ppoll|select|epoll> ...
 *   O6Q BEGIN suite=<suite>
 *   O6Q PASS <suite>.<test> <free text>
 *   O6Q FAIL <suite>.<test> <free text>
 *   O6Q SKIP <suite>.<test> <reason>
 *   O6Q LAT  <suite>.<what> n=.. p50_us=.. p90_us=.. p99_us=.. max_us=.. mean_us=..
 *   O6Q INFO <suite> <free text>
 *   O6Q LOG  [(expected) ]<level>/<category> <open62541 log message>
 *   O6Q SUMMARY suite=<suite> pass=<p> fail=<f> skip=<s> secs=<t> ...
 *
 * A suite that crashes or hangs still ends with a FAIL line: the signal
 * handlers and the watchdog print "O6Q FAIL <suite>.<test> signal=..." for the
 * test that was running.
 *
 * A program that is built with -DO6Q_VARIANT="x" reports its suites as
 * "<suite>@x".  o6q_check_upstream is the same program linked with the
 * unchanged upstream release and reports "<suite>@upstream".
 */
void o6q_begin(const char *suite, unsigned watchdog_seconds);
int o6q_end(void); /* prints the summary, returns the number of failures */
void o6q_step(const char *test); /* names the test that is about to run */
void o6q_report(int ok, const char *test, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void o6q_skip(const char *test, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void o6q_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define CHECK(test, cond, ...) o6q_report((cond) ? 1 : 0, (test), __VA_ARGS__)

/* Short name of a status code, for the result lines. */
#define SC(rc) UA_StatusCode_name(rc)

/* ---------------------------------------------------------- time, stats */
double o6q_now_us(void); /* CLOCK_MONOTONIC in microseconds */
/* CPU time that a thread has used, in microseconds; < 0 if unknown */
double o6q_thread_cpu_us(pthread_t thread);
void o6q_sleep_ms(unsigned ms);
/* Sorts v and prints one "O6Q LAT" line. */
void o6q_lat(const char *what, double *v, size_t n);
/* Number of open file descriptors of this process (to find leaks). */
int o6q_open_fds(void);

/* The function that the event loop of the linked open62541 waits in: the same
 * decision as in the library (patches/01-eventloop-ppoll.change). A program
 * with O6Q_VARIANT is linked with the unchanged release. */
#if defined(O6Q_VARIANT)
# if defined(__linux__)
#  define O6Q_EVENTLOOP "epoll"
# else
#  define O6Q_EVENTLOOP "select"
# endif
#elif defined(O6Q_EVENTLOOP_POLL) || \
    (!defined(O6Q_EVENTLOOP_SELECT) && defined(__QNXNTO__))
# define O6Q_EVENTLOOP "ppoll"
#elif defined(O6Q_EVENTLOOP_SELECT) || !defined(__linux__)
# define O6Q_EVENTLOOP "select"
#else
# define O6Q_EVENTLOOP "epoll"
#endif

/* --------------------------------------------------------------- logging
 * One logger for every server and client of the check. It prints warnings
 * and errors as "O6Q LOG ..." lines (everything from INFO upwards if the
 * environment variable O6Q_VERBOSE is set). While a negative test runs (a
 * connection that must be refused), the lines are marked "(expected)".
 * After O6Q_LOG_CAP lines in one suite the rest is counted, not printed. */
#define O6Q_LOG_CAP 200
UA_Logger *o6q_logger(void);
void o6q_expect_errors(int on);

/* -------------------------------------------------------- server harness */
typedef struct {
    UA_Server *server;
    pthread_t thread;
    volatile int running;
    int started;
} O6QServer;

/* Stack of the thread that runs the server. */
#define O6Q_SERVER_STACK (1024u * 1024u)

/* UA_Server_run_startup, then UA_Server_run_iterate on a thread. */
UA_StatusCode o6q_server_run(O6QServer *s);
/* Stops the thread, shuts the server down and deletes it. */
void o6q_server_stop(O6QServer *s);

/* ----------------------------------------------------------- test model
 * The address space that the suites use (namespace 1), below Objects/o6q:
 *   answer   Int32, read/write, starts at 42
 *   counter  Int32, written by a 10 ms server timer
 *   blob     ByteString, read/write
 *   add      method (Int32 a, Int32 b) -> Int32 sum
 *   fire     method (UInt16 severity): emits a BaseEventType event
 *   qnxinfo  String data source computed from the live QNX system state
 */
#define O6Q_NS 1
#define O6Q_TICK_MS 10.0
#define O6Q_TICK_HISTORY 4096

typedef struct {
    pthread_mutex_t lock;
    UA_Int32 counter;
    unsigned ticks;                        /* timer callbacks so far */
    double tick_err_us[O6Q_TICK_HISTORY];  /* |interval - 10 ms| per tick */
    double last_tick_us;
    unsigned events_fired;
} O6QModel;

UA_StatusCode o6q_model_add(UA_Server *server, O6QModel *m);
/* A server with our logger, the default configuration without encryption on
 * the TCP port and the test model, running in its thread. */
UA_StatusCode o6q_server_start_model(O6QServer *srv, O6QModel *m, unsigned port);
/* Reads the Int32 variable ns=1;s=<name>. */
UA_StatusCode o6q_read_i32(UA_Client *c, const char *name, UA_Int32 *out);
unsigned o6q_model_ticks(O6QModel *m);
/* Copies the newest (at most max) timer errors; returns how many. */
size_t o6q_model_tick_errors(O6QModel *m, double *out, size_t max);

/* A server configuration with our logger; cfg must be zeroed by the caller
 * before the first use. Lowers the subscription limits so that 10 ms
 * publishing works. */
void o6q_server_limits(UA_ServerConfig *cfg);

/* A client with our logger and a timeout that suits a slow emulated CPU.
 * Returns NULL on failure. */
UA_Client *o6q_client_new(void);

/* ------------------------------------------------------------------ suites */
int o6q_suite_core(unsigned port, int n);
int o6q_suite_security(unsigned port, int n);
int o6q_suite_pubsub(unsigned port, int n);
int o6q_suite_highfd(unsigned port, int n);
int o6q_suite_robust(unsigned port, int n);
int o6q_suite_nopipe(unsigned port, int n);

#endif /* O6Q_COMMON_H_ */
