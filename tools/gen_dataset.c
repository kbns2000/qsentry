/*
 * Generate a labelled feature dataset from the simulator.
 * usage: gen_dataset <episodes> <seed> > data.csv
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include "qs_core.h"

#define EP_TICKS   3000   /* 30 s */
#define LABEL_LAG  50     /* fault must be active 0.5 s before it is labelled */

int main(int argc, char **argv)
{
    int episodes = argc > 1 ? atoi(argv[1]) : 400;
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 10) : 1;
    uint64_t r = seed * 7919 + 13;

    printf("ch,label");
    for (int i = 0; i < QS_NFEAT; i++) printf(",f%d", i);
    printf("\n");

    for (int ep = 0; ep < episodes; ep++) {
        qs_sim_t sim;
        qs_window_t win;
        qs_sim_init(&sim, seed * 1000003ULL + (uint64_t)ep + 1);
        qs_window_init(&win);
        /* 1 in 5 episodes stays clean */
        int clean = (qs_rand_uniform(&r) < 0.2f);
        int fch = (int)(qs_rand_uniform(&r) * QS_NCH) % QS_NCH;
        int ftype = 1 + (int)(qs_rand_uniform(&r) * (QS_NCLASS - 1)) % (QS_NCLASS - 1);
        int t_on = 800 + (int)(qs_rand_uniform(&r) * 800);

        for (int t = 0; t < EP_TICKS; t++) {
            float x[QS_NCH];
            if (!clean && t == t_on)
                qs_sim_inject(&sim, fch, ftype);
            qs_sim_step(&sim, x);
            qs_window_push(&win, x);
            if (!qs_window_ready(&win) || (t % 5))
                continue;
            float feat[QS_NCH][QS_NFEAT];
            qs_features(&win, feat);
            for (int ch = 0; ch < QS_NCH; ch++) {
                int label = 0;
                /* a drifting sensor shifts the shared physics residual of its
                   pair partner too; those rows are inherently ambiguous */
                if (!clean && ftype == QS_F_DRIFT && t >= t_on && ch != fch &&
                    (ch <= QS_CH_ACCEL) == (fch <= QS_CH_ACCEL))
                    continue;
                if (!clean && ch == fch && t >= t_on) {
                    if (t - t_on < LABEL_LAG) continue;  /* ambiguous onset */
                    label = ftype;
                }
                printf("%d,%d", ch, label);
                for (int i = 0; i < QS_NFEAT; i++) printf(",%.5f", feat[ch][i]);
                printf("\n");
            }
        }
    }
    return 0;
}
