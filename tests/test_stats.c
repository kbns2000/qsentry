/* Host unit test for stats.c:  cc -O2 -Wall -Isrc tests/test_stats.c src/stats.c && ./a.out
 * SPDX-License-Identifier: Apache-2.0 */
#include "stats.h"
#include <math.h>
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    /* 1..1000 us, shuffled */
    static uint32_t v[1000];
    for (int i = 0; i < 1000; i++) v[i] = (uint32_t)(((i * 7919) % 1000) + 1) * 1000u;
    ipcb_stats_t s;
    ipcb_stats(v, 1000, &s);
    CHECK(s.n == 1000);
    CHECK(fabs(s.min_us - 1) < 1e-9);
    CHECK(fabs(s.max_us - 1000) < 1e-9);
    CHECK(fabs(s.p50_us - 500) < 1e-9);
    CHECK(fabs(s.p99_us - 990) < 1e-9);
    CHECK(fabs(s.p999_us - 999) < 1e-9);
    CHECK(fabs(s.mean_us - 500.5) < 1e-9);
    unsigned tot = 0;
    for (int i = 0; i < IPCB_HIST_BUCKETS; i++) tot += s.hist[i];
    CHECK(tot == 1000);

    CHECK(ipcb_bucket(0) == 0);
    CHECK(ipcb_bucket(999) == 0);
    CHECK(ipcb_bucket(1000) == 1);
    CHECK(ipcb_bucket(3999) == 2);
    CHECK(ipcb_bucket(4000) == 3);
    CHECK(ipcb_bucket(0xFFFFFFFFu) == IPCB_HIST_BUCKETS - 1);

    ipcb_stats(v, 0, &s);
    CHECK(s.n == 0 && s.max_us == 0);

    uint32_t one = 2500;
    ipcb_stats(&one, 1, &s);
    CHECK(fabs(s.p50_us - 2.5) < 1e-9 && fabs(s.p999_us - 2.5) < 1e-9);

    printf(fails ? "stats: %d FAILED\n" : "stats: all passed\n", fails);
    return fails != 0;
}
