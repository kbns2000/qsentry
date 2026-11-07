/* stats.h - latency sample statistics (portable C, unit-tested on the host)
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef IPCB_STATS_H
#define IPCB_STATS_H
#include <stddef.h>
#include <stdint.h>

#define IPCB_HIST_BUCKETS 16 /* [0,1) [1,2) [2,4) ... microseconds, last is open */

typedef struct {
    size_t   n;
    double   min_us, p50_us, p99_us, p999_us, max_us, mean_us;
    uint32_t hist[IPCB_HIST_BUCKETS];
} ipcb_stats_t;

/* Sorts samples (nanoseconds) in place and fills *out. n may be 0. */
void ipcb_stats(uint32_t *samples_ns, size_t n, ipcb_stats_t *out);

/* Bucket index for a latency in nanoseconds (log2 of microseconds). */
int ipcb_bucket(uint32_t ns);

/* Prints one machine-readable result line, e.g.
 * IPCB test=msg size=64 load=none n=20000 min_us=.. p50_us=.. ... hist=a,b,c */
void ipcb_print(const char *test, unsigned size, const char *load, const ipcb_stats_t *s);

#endif
