/* Unit tests for the portable core. SPDX-License-Identifier: Apache-2.0 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "qs_core.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void run(qs_engine_t *e, qs_sim_t *s, int ticks)
{
    for (int t = 0; t < ticks; t++) {
        float x[QS_NCH];
        qs_sim_step(s, x);
        qs_engine_step(e, x);
    }
}

static void test_sim_deterministic(void)
{
    qs_sim_t a, b;
    qs_sim_init(&a, 42); qs_sim_init(&b, 42);
    for (int i = 0; i < 1000; i++) {
        float x[QS_NCH], y[QS_NCH];
        qs_sim_step(&a, x); qs_sim_step(&b, y);
        CHECK(memcmp(x, y, sizeof(x)) == 0);
    }
}

static void test_stuck_is_flat(void)
{
    qs_sim_t s; float x[QS_NCH], prev = 0;
    qs_sim_init(&s, 7);
    qs_sim_inject(&s, QS_CH_ACCEL, QS_F_STUCK);
    for (int i = 0; i < 50; i++) {
        qs_sim_step(&s, x);
        if (i) CHECK(x[QS_CH_ACCEL] == prev);
        prev = x[QS_CH_ACCEL];
    }
}

static void test_model_probs(void)
{
    float f[QS_NFEAT] = {0}, p[QS_NCLASS];
    for (int ch = 0; ch < QS_NCH; ch++) {
        int k = qs_model_infer(ch, f, p);
        float s = 0;
        for (int i = 0; i < QS_NCLASS; i++) { CHECK(p[i] >= 0 && p[i] <= 1); s += p[i]; }
        CHECK(fabsf(s - 1.0f) < 1e-4f);
        CHECK(k >= 0 && k < QS_NCLASS);
    }
}

static void test_clean_stays_nominal(void)
{
    qs_sim_t s; qs_engine_t e;
    qs_sim_init(&s, 1234); qs_engine_init(&e);
    run(&e, &s, 3000);
    CHECK(e.sup.state == QS_ST_NOMINAL);
}

static void test_single_fault_degrades_and_recovers(void)
{
    qs_sim_t s; qs_engine_t e;
    qs_sim_init(&s, 99); qs_engine_init(&e);
    run(&e, &s, 500);
    qs_sim_inject(&s, QS_CH_STEER, QS_F_STUCK);
    run(&e, &s, 500);
    CHECK(e.sup.state == QS_ST_DEGRADED);
    CHECK(e.sup.verdict[QS_CH_STEER] == QS_F_STUCK);
    qs_sim_clear(&s);
    run(&e, &s, 600);
    CHECK(e.sup.state == QS_ST_NOMINAL);
}

static void test_two_pairs_safe_stop_latched(void)
{
    qs_sim_t s; qs_engine_t e;
    qs_sim_init(&s, 5); qs_engine_init(&e);
    run(&e, &s, 300);
    qs_sim_inject(&s, QS_CH_ACCEL, QS_F_NOISE);
    qs_sim_inject(&s, QS_CH_YAW, QS_F_SPIKE);
    run(&e, &s, 400);
    CHECK(e.sup.state == QS_ST_SAFE_STOP);
    qs_sim_clear(&s);
    run(&e, &s, 600);
    CHECK(e.sup.state == QS_ST_SAFE_STOP);   /* latched */
    qs_sup_reset(&e.sup);
    CHECK(e.sup.state == QS_ST_NOMINAL);
}

static void test_timeout(void)
{
    qs_sup_t s;
    qs_sup_init(&s);
    CHECK(qs_sup_timeout(&s, 40) == 1);
    CHECK(s.state == QS_ST_SAFE_STOP);
    CHECK(strstr(s.reason, "40 ms") != NULL);
}

int main(void)
{
    test_sim_deterministic();
    test_stuck_is_flat();
    test_model_probs();
    test_clean_stays_nominal();
    test_single_fault_degrades_and_recovers();
    test_two_pairs_safe_stop_latched();
    test_timeout();
    printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
    return fails != 0;
}
