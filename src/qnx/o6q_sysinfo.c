/* See o6q_sysinfo.h.  SPDX-License-Identifier: Apache-2.0 */
#ifndef _QNX_SOURCE
#define _QNX_SOURCE
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "o6q_sysinfo.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#ifdef __QNXNTO__
#include <devctl.h>
#include <sys/dcmd_proc.h>
#include <sys/neutrino.h>
#include <sys/procfs.h>
#include <sys/syspage.h>
#endif

static void copy_str(char *dst, size_t n, const char *src) {
    size_t len;
    if(n == 0)
        return;
    len = src ? strnlen(src, n - 1) : 0;
    if(len)
        memcpy(dst, src, len);
    dst[len] = 0;
}

#ifdef __QNXNTO__

/* The system page lists the address ranges of the machine; RAM ranges are
 * the entries named "ram". */
static unsigned long long qnx_ram_total(void) {
    struct asinfo_entry *as = SYSPAGE_ENTRY(asinfo);
    const char *strings = SYSPAGE_ENTRY(strings)->data;
    unsigned n = (unsigned)(SYSPAGE_ENTRY_SIZE(asinfo) / sizeof(*as));
    unsigned long long total = 0;
    for(unsigned i = 0; i < n; i++) {
        if(strcmp(strings + as[i].name, "ram") == 0)
            total += (unsigned long long)(as[i].end - as[i].start) + 1;
    }
    return total;
}

/* The size of /proc is the amount of free system RAM. */
static unsigned long long qnx_ram_free(void) {
    struct stat st;
    if(stat("/proc", &st) != 0)
        return 0;
    return (unsigned long long)st.st_size;
}

/* procnto runs one idle thread per CPU: thread (cpu + 1) of process 1.
 * Its consumed CPU time tells how long that CPU was idle. */
static int qnx_idle_ns(unsigned cpu, unsigned long long *ns) {
    static int fd = -1;
    procfs_status st;
    if(fd < 0)
        fd = open("/proc/1/as", O_RDONLY);
    if(fd < 0)
        return -1;
    memset(&st, 0, sizeof(st));
    st.tid = (int)(cpu + 1);
    if(devctl(fd, DCMD_PROC_TIDSTATUS, &st, sizeof(st), NULL) != EOK)
        return -1;
    if(st.tid != (int)(cpu + 1))
        return -1; /* the kernel returned the next existing thread instead */
    *ns = (unsigned long long)st.sutime;
    return 0;
}

#endif /* __QNXNTO__ */

void o6q_sys_static(O6QSysStatic *s) {
    struct utsname u;
    memset(s, 0, sizeof(*s));
    if(uname(&u) == 0) {
        copy_str(s->sysname, sizeof(s->sysname), u.sysname);
        copy_str(s->release, sizeof(s->release), u.release);
        copy_str(s->machine, sizeof(s->machine), u.machine);
    }
    if(gethostname(s->hostname, sizeof(s->hostname) - 1) != 0)
        s->hostname[0] = 0;
#ifdef __QNXNTO__
    s->num_cpu = (unsigned)_syspage_ptr->num_cpu;
    s->cycles_per_sec = (unsigned long long)SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    {
        struct _clockperiod cp;
        if(ClockPeriod(CLOCK_REALTIME, NULL, &cp, 0) == 0)
            s->clock_period_ns = (unsigned long)cp.nsec;
    }
    s->ram_total = qnx_ram_total();
#else
    {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        struct timespec res;
        s->num_cpu = n > 0 ? (unsigned)n : 1;
        if(clock_getres(CLOCK_MONOTONIC, &res) == 0)
            s->clock_period_ns = (unsigned long)res.tv_nsec;
    }
#endif
}

void o6q_sys_sample(O6QSysSample *s) {
    struct timespec ts;
    memset(s, 0, sizeof(*s));
    clock_gettime(CLOCK_MONOTONIC, &ts);
    s->uptime_s = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#ifdef __QNXNTO__
    {
        static unsigned long long prev_idle[O6Q_MAX_CPU];
        static double prev_wall_ns;
        static int have_prev;
        double wall_ns = (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
        unsigned n = (unsigned)_syspage_ptr->num_cpu;
        if(n > O6Q_MAX_CPU)
            n = O6Q_MAX_CPU;
        s->num_cpu = n;
        for(unsigned i = 0; i < n; i++) {
            unsigned long long idle = 0;
            s->cpu_load[i] = -1.0;
            if(qnx_idle_ns(i, &idle) != 0)
                continue;
            if(have_prev && wall_ns > prev_wall_ns) {
                double busy = 1.0 - (double)(idle - prev_idle[i]) / (wall_ns - prev_wall_ns);
                if(busy < 0.0)
                    busy = 0.0;
                if(busy > 1.0)
                    busy = 1.0;
                s->cpu_load[i] = busy * 100.0;
            }
            prev_idle[i] = idle;
        }
        prev_wall_ns = wall_ns;
        have_prev = 1;
    }
    s->ram_free = qnx_ram_free();
#else
    {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        s->num_cpu = n > 0 ? (unsigned)n : 1;
        if(s->num_cpu > O6Q_MAX_CPU)
            s->num_cpu = O6Q_MAX_CPU;
        for(unsigned i = 0; i < s->num_cpu; i++)
            s->cpu_load[i] = -1.0;
    }
#endif
    s->processes = (unsigned)o6q_sys_processes(NULL, 0, &s->threads);
}

size_t o6q_sys_processes(O6QProc *out, size_t max, unsigned *total_threads) {
    size_t count = 0;
    unsigned threads = 0;
#ifdef __QNXNTO__
    DIR *d = opendir("/proc");
    struct dirent *e;
    if(d) {
        while((e = readdir(d)) != NULL) {
            char path[64];
            procfs_info info;
            int fd;
            if(!isdigit((unsigned char)e->d_name[0]))
                continue;
            snprintf(path, sizeof(path), "/proc/%.20s/as", e->d_name);
            fd = open(path, O_RDONLY);
            if(fd < 0)
                continue;
            if(devctl(fd, DCMD_PROC_INFO, &info, sizeof(info), NULL) == EOK) {
                threads += (unsigned)info.num_threads;
                if(out && count < max) {
                    struct {
                        procfs_debuginfo info;
                        char buff[256];
                    } name;
                    const char *base = "?";
                    memset(&name, 0, sizeof(name));
                    if(devctl(fd, DCMD_PROC_MAPDEBUG_BASE, &name, sizeof(name), NULL) == EOK) {
                        /* info.path is declared with one byte and continues
                         * into buff; address it through the whole structure */
                        const char *image =
                            (const char *)&name + offsetof(procfs_debuginfo, path);
                        const char *slash;
                        name.buff[sizeof(name.buff) - 1] = 0;
                        slash = strrchr(image, '/');
                        base = slash ? slash + 1 : image;
                    }
                    out[count].pid = (int)info.pid;
                    out[count].threads = (unsigned)info.num_threads;
                    copy_str(out[count].name, sizeof(out[count].name), base);
                }
                count++;
            }
            close(fd);
        }
        closedir(d);
    }
#else
    (void)out;
    (void)max;
#endif
    if(total_threads)
        *total_threads = threads;
    return count;
}
