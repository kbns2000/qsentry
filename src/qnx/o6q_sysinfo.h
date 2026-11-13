/* Live facts about the QNX system, read from the system page, procfs and the
 * kernel clocks.  Used by the check's "qnxinfo" node and by o6q_server, which
 * publishes them as OPC UA nodes.  On other operating systems the functions
 * return what POSIX offers and mark the rest as unknown.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef O6Q_SYSINFO_H_
#define O6Q_SYSINFO_H_

#include <stddef.h>

#define O6Q_MAX_CPU 32

typedef struct {
    char sysname[32];  /* uname: "QNX" */
    char release[32];  /* uname: "8.0.0" */
    char machine[32];  /* uname: "x86_64" / "aarch64le" */
    char hostname[64];
    unsigned num_cpu;                  /* system page */
    unsigned long long cycles_per_sec; /* ClockCycles() frequency; 0 if unknown */
    unsigned long clock_period_ns;     /* system tick; 0 if unknown */
    unsigned long long ram_total;      /* bytes; 0 if unknown */
} O6QSysStatic;

typedef struct {
    double uptime_s;            /* CLOCK_MONOTONIC */
    unsigned long long ram_free; /* bytes; 0 if unknown */
    unsigned processes;         /* 0 if unknown */
    unsigned threads;           /* 0 if unknown */
    unsigned num_cpu;
    /* Percent of the time each CPU was busy since the previous call
     * (-1 on the first call or if unknown). */
    double cpu_load[O6Q_MAX_CPU];
} O6QSysSample;

typedef struct {
    int pid;
    unsigned threads;
    char name[48];
} O6QProc;

void o6q_sys_static(O6QSysStatic *s);

/* Keeps the previous idle times between calls: call it from one thread. */
void o6q_sys_sample(O6QSysSample *s);

/* Fills up to max entries; returns the number of processes found. */
size_t o6q_sys_processes(O6QProc *out, size_t max, unsigned *total_threads);

#endif /* O6Q_SYSINFO_H_ */
