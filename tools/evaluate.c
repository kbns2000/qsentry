/*
 * Offline end-to-end evaluation of the full pipeline (features -> model ->
 * supervisor) on held-out simulator seeds.
 * usage: evaluate <episodes_per_fault> <seed>
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "qs_core.h"

#define T_ON   1000   /* fault injected at 10 s */
#define T_END  3000   /* episode 30 s           */

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 50;
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 10) : 9;
    int det[QS_NCH][QS_NCLASS] = {{0}}, cls_ok[QS_NCH][QS_NCLASS] = {{0}};
    int ch_ok[QS_NCH][QS_NCLASS] = {{0}}, pair_ok[QS_NCH][QS_NCLASS] = {{0}}, safe[QS_NCH][QS_NCLASS] = {{0}};
    double lat[QS_NCH][QS_NCLASS] = {{0}};
    int pre_fa = 0, runs = 0;
    double t_infer = 0; long n_infer = 0;

    printf("fault          detect  latency_ms  right_class  right_channel  right_pair  false_safe_stop\n");
    for (int fch = 0; fch < QS_NCH; fch++)
    for (int ft = 1; ft < QS_NCLASS; ft++) {
        for (int ep = 0; ep < n; ep++) {
            qs_sim_t sim; qs_engine_t eng;
            qs_sim_init(&sim, seed * 7777777ULL + (uint64_t)(fch * 100000 + ft * 1000 + ep));
            qs_engine_init(&eng);
            int t_det = -1, pre = 0;
            runs++;
            for (int t = 0; t < T_END; t++) {
                float x[QS_NCH];
                if (t == T_ON) qs_sim_inject(&sim, fch, ft);
                qs_sim_step(&sim, x);
                double t0 = now_s();
                qs_engine_step(&eng, x);
                t_infer += now_s() - t0; n_infer++;
                if (t < T_ON && eng.sup.state != QS_ST_NOMINAL) pre = 1;
                if (t >= T_ON && t_det < 0 && eng.sup.state != QS_ST_NOMINAL)
                    t_det = t;
            }
            pre_fa += pre;
            if (pre) continue;
            if (t_det >= 0) {
                det[fch][ft]++;
                lat[fch][ft] += (t_det - T_ON) * 10.0;
            }
            if (eng.sup.verdict[fch] == ft) cls_ok[fch][ft]++;
            if (eng.sup.verdict[fch] != QS_F_OK) ch_ok[fch][ft]++;
            {
                int mate = fch ^ 1;   /* 0<->1, 2<->3 */
                if (eng.sup.verdict[fch] != QS_F_OK || eng.sup.verdict[mate] != QS_F_OK)
                    pair_ok[fch][ft]++;
            }
            if (eng.sup.state == QS_ST_SAFE_STOP) safe[fch][ft]++;
        }
        printf("%-6s %-6s  %3d/%-3d  %8.0f    %3d/%-3d      %3d/%-3d      %3d/%-3d     %3d/%-3d\n",
               qs_channel_name[fch], qs_fault_name[ft], det[fch][ft], n,
               det[fch][ft] ? lat[fch][ft] / det[fch][ft] : -1.0,
               cls_ok[fch][ft], n, ch_ok[fch][ft], n, pair_ok[fch][ft], n, safe[fch][ft], n);
    }

    /* clean drives: false alarm rate */
    int fa = 0, clean_n = n * 4;
    double hours = 0;
    for (int ep = 0; ep < clean_n; ep++) {
        qs_sim_t sim; qs_engine_t eng;
        qs_sim_init(&sim, seed * 3333331ULL + (uint64_t)ep + 17);
        qs_engine_init(&eng);
        int alarm = 0;
        for (int t = 0; t < 6000; t++) {   /* 60 s */
            float x[QS_NCH];
            qs_sim_step(&sim, x);
            qs_engine_step(&eng, x);
            if (eng.sup.state != QS_ST_NOMINAL) alarm = 1;
        }
        fa += alarm;
        hours += 60.0 / 3600.0;
    }
    printf("\nclean drives with any alarm: %d/%d (%.2f h simulated)\n", fa, clean_n, hours);
    printf("fault runs with alarm before injection: %d/%d\n", pre_fa, runs);
    printf("mean pipeline time per 100 Hz tick (host): %.1f us\n", t_infer / n_infer * 1e6);
    return 0;
}
