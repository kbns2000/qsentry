/* stats.c - SPDX-License-Identifier: Apache-2.0 */
#include "stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

int ipcb_bucket(uint32_t ns)
{
    uint32_t us = ns / 1000u;
    int b = 0;
    while (us && b < IPCB_HIST_BUCKETS - 1) { us >>= 1; b++; }
    return b;
}

/* nearest-rank percentile on sorted data */
static double pct(const uint32_t *s, size_t n, double p)
{
    size_t k = (size_t)(p * (double)n + 0.999999);
    if (k < 1) k = 1;
    if (k > n) k = n;
    return s[k - 1] / 1000.0;
}

void ipcb_stats(uint32_t *v, size_t n, ipcb_stats_t *o)
{
    memset(o, 0, sizeof(*o));
    o->n = n;
    if (!n) return;
    qsort(v, n, sizeof(*v), cmp_u32);
    double sum = 0;
    for (size_t i = 0; i < n; i++) { sum += v[i]; o->hist[ipcb_bucket(v[i])]++; }
    o->min_us  = v[0] / 1000.0;
    o->max_us  = v[n - 1] / 1000.0;
    o->mean_us = sum / (double)n / 1000.0;
    o->p50_us  = pct(v, n, 0.50);
    o->p99_us  = pct(v, n, 0.99);
    o->p999_us = pct(v, n, 0.999);
}

void ipcb_print(const char *test, unsigned size, const char *load, const ipcb_stats_t *s)
{
    printf("IPCB test=%s size=%u load=%s n=%zu min_us=%.2f p50_us=%.2f p99_us=%.2f "
           "p999_us=%.2f max_us=%.2f mean_us=%.2f hist=",
           test, size, load, s->n, s->min_us, s->p50_us, s->p99_us, s->p999_us, s->max_us,
           s->mean_us);
    for (int i = 0; i < IPCB_HIST_BUCKETS; i++) printf(i ? ",%u" : "%u", s->hist[i]);
    printf("\n");
    fflush(stdout);
}
