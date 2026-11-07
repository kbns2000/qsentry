/*
 * Run the qsentry pipeline on a recorded 100 Hz CSV (speed,accel,steer,yaw),
 * optionally injecting one fault with the same models as the simulator.
 * usage: replay <csv> [ch fault t_on_s seed]
 * prints: false-alarm ticks before injection, state changes, detection latency, verdict.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "qs_core.h"
#include "tool_profile.h"

float qs_noise_sd(int ch);
float qs_scale(int ch);

#define MAXN 20000
static float data[MAXN][QS_NCH];

int main(int argc, char **argv)
{
    qs_tool_profile();
    if (argc < 2) { fprintf(stderr, "usage: replay <csv> [ch fault t_on_s seed]\n"); return 2; }
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 1; }
    int n = 0;
    while (n < MAXN && fscanf(f, "%f,%f,%f,%f", &data[n][0], &data[n][1], &data[n][2], &data[n][3]) == 4) n++;
    fclose(f);
    int fch = argc > 2 ? atoi(argv[2]) : -1, ft = argc > 3 ? atoi(argv[3]) : 0;
    int t_on = argc > 4 ? (int)(atof(argv[4]) * QS_RATE_HZ) : n;
    uint64_t rng = argc > 5 ? strtoull(argv[5], NULL, 10) : 1;
    if (fch < 0) t_on = n;
    float frozen = 0, sign = qs_rand_uniform(&rng) < 0.5f ? -1.f : 1.f;

    qs_engine_t e; qs_engine_init(&e);
    int pre_ticks = 0, pre_state = 0, t_det = -1, ch_flag[QS_NCH] = {0};
    for (int t = 0; t < n; t++) {
        float x[QS_NCH];
        for (int c = 0; c < QS_NCH; c++) x[c] = data[t][c];
        if (t >= t_on) {
            float dt = (t - t_on) * QS_DT, sc = qs_scale(fch), sd = qs_noise_sd(fch);
            switch (ft) {
            case QS_F_STUCK: if (t == t_on) frozen = x[fch]; x[fch] = frozen; break;
            case QS_F_DRIFT: x[fch] += sign * 0.5f * sc * dt; break;
            case QS_F_SPIKE:
                if (qs_rand_uniform(&rng) < 0.08f) {
                    float m = 5.f + 5.f * qs_rand_uniform(&rng);
                    x[fch] += (qs_rand_uniform(&rng) < 0.5f ? -m : m) * sc;
                }
                break;
            case QS_F_NOISE: x[fch] += 12.f * sd * qs_rand_normal(&rng); break;
            }
        }
        int ch = qs_engine_step(&e, x);
        if (ch) printf("t=%6.2fs %s : %s\n", t * QS_DT, qs_state_name[e.sup.state], e.sup.reason);
        if (t < t_on) {
            for (int c = 0; c < QS_NCH; c++) if (e.sup.verdict[c]) { pre_ticks++; ch_flag[c]++; break; }
            if (e.sup.state != QS_ST_NOMINAL) pre_state++;
        } else if (t_det < 0 && e.sup.state != QS_ST_NOMINAL) t_det = t;
    }
    printf("SUMMARY samples=%d pre_fault_flag_ticks=%d pre_fault_nonnominal_ticks=%d flags_by_ch=%d,%d,%d,%d",
           n, pre_ticks, pre_state, ch_flag[0], ch_flag[1], ch_flag[2], ch_flag[3]);
    if (fch >= 0)
        printf(" detect=%d latency_ms=%d verdict=%s/%s", t_det >= 0, t_det >= 0 ? (t_det - t_on) * 10 : -1,
               qs_channel_name[fch], qs_fault_name[e.sup.verdict[fch]]);
    printf("\n");
    return 0;
}
