/*
 * Sliding-window features. O(QS_WIN) per channel, no allocation.
 *
 * Per channel:
 *  f0 log(std / nominal noise)          - noise blow-up
 *  f1 fraction of exactly-zero steps    - stuck sensor
 *  f2 log1p(max local z-score)          - spikes (vs neighbours)
 *  f3 physics residual of the pair      - drift (speed vs integrated accel,
 *                                          yaw vs bicycle model from steer)
 *  f4 |f3| of the most recent half       - drift growth
 *  f5 log(range / scale)
 *  f6 log(mean |2nd difference| / noise) - roughness
 *  f7 log1p(count of local z > 6)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <string.h>
#include "qs_core.h"

float qs_noise_sd(int ch);
float qs_scale(int ch);
float qs_wheelbase(void);

void qs_window_init(qs_window_t *w)
{
    memset(w, 0, sizeof(*w));
}

void qs_window_push(qs_window_t *w, const float x[QS_NCH])
{
    for (int ch = 0; ch < QS_NCH; ch++)
        w->buf[ch][w->head] = x[ch];
    w->head = (w->head + 1) % QS_WIN;
    if (w->count < QS_WIN)
        w->count++;
}

int qs_window_ready(const qs_window_t *w)
{
    return w->count >= QS_WIN;
}

/* chronological copy: out[0] oldest */
static void unroll(const qs_window_t *w, int ch, float *out)
{
    for (int i = 0; i < QS_WIN; i++)
        out[i] = w->buf[ch][(w->head + i) % QS_WIN];
}

static float mean_range(const float *x, int a, int b)
{
    float s = 0.0f;
    for (int i = a; i < b; i++) s += x[i];
    return s / (float)(b - a);
}

/* speed vs accel: dv measured by averaged endpoints minus integral of accel */
static float resid_speed_accel(const float *v, const float *a, int lo, int hi)
{
    const int k = 10;
    float v0 = mean_range(v, lo, lo + k);
    float v1 = mean_range(v, hi - k, hi);
    /* centres of the two averaging blocks */
    int c0 = lo + k / 2, c1 = hi - k / 2;
    float integ = 0.0f;
    for (int i = c0; i < c1; i++) integ += a[i] * QS_DT;
    return (v1 - v0) - integ;            /* m/s */
}

/* yaw vs bicycle model */
static float resid_yaw_steer(const float *v, const float *st, const float *yw, int lo, int hi)
{
    float s = 0.0f;
    float L = qs_wheelbase();
    for (int i = lo; i < hi; i++)
        s += yw[i] - v[i] * tanf(st[i]) / L;
    return s / (float)(hi - lo);         /* rad/s */
}

void qs_features(const qs_window_t *w, float feat[QS_NCH][QS_NFEAT])
{
    float x[QS_NCH][QS_WIN];
    for (int ch = 0; ch < QS_NCH; ch++)
        unroll(w, ch, x[ch]);

    /* pair residuals, normalised by their nominal sigma (empirical) */
    const float sa_sd = 0.035f, ys_sd = 0.010f;
    float r_sa  = resid_speed_accel(x[QS_CH_SPEED], x[QS_CH_ACCEL], 0, QS_WIN) / sa_sd;
    float r_sa2 = resid_speed_accel(x[QS_CH_SPEED], x[QS_CH_ACCEL], QS_WIN / 2, QS_WIN) / sa_sd;
    float r_ys  = resid_yaw_steer(x[QS_CH_SPEED], x[QS_CH_STEER], x[QS_CH_YAW], 0, QS_WIN) / ys_sd;
    float r_ys2 = resid_yaw_steer(x[QS_CH_SPEED], x[QS_CH_STEER], x[QS_CH_YAW], QS_WIN / 2, QS_WIN) / ys_sd;

    for (int ch = 0; ch < QS_NCH; ch++) {
        const float *c = x[ch];
        float nsd = qs_noise_sd(ch), sc = qs_scale(ch);

        /* std */
        float m = mean_range(c, 0, QS_WIN), var = 0.0f;
        float mn = c[0], mx = c[0];
        for (int i = 0; i < QS_WIN; i++) {
            float d = c[i] - m;
            var += d * d;
            if (c[i] < mn) mn = c[i];
            if (c[i] > mx) mx = c[i];
        }
        float sd = sqrtf(var / QS_WIN);

        /* flat steps, roughness */
        int flat = 0;
        float rough = 0.0f;
        for (int i = 1; i < QS_WIN; i++) {
            if (c[i] == c[i - 1]) flat++;
            if (i >= 2) rough += fabsf(c[i] - 2.0f * c[i - 1] + c[i - 2]);
        }
        rough /= (QS_WIN - 2);

        /* local detrend so a fast but clean signal does not look spiky:
           compare each sample with the mean of its neighbours */
        float zmax = 0.0f;
        int nout = 0;
        float lmad = rough / 1.6f + 1e-6f; /* ~sigma of 2nd diff / 2 */
        for (int i = 1; i < QS_WIN - 1; i++) {
            float dev = fabsf(c[i] - 0.5f * (c[i - 1] + c[i + 1]));
            float z = dev / (0.5f * lmad + 0.6f * nsd + 1e-6f);
            if (z > zmax) zmax = z;
            if (z > 6.0f) nout++;
        }

        float r_full = (ch <= QS_CH_ACCEL) ? r_sa : r_ys;
        float r_half = (ch <= QS_CH_ACCEL) ? r_sa2 : r_ys2;

        float *f = feat[ch];
        f[0] = logf(sd / nsd + 1e-3f);
        f[1] = (float)flat / (QS_WIN - 1);
        f[2] = log1pf(zmax);
        f[3] = log1pf(fabsf(r_full));
        f[4] = log1pf(fabsf(r_half));
        f[5] = logf((mx - mn) / sc + 1e-3f);
        f[6] = logf(rough / nsd + 1e-3f);
        f[7] = log1pf((float)nout);
    }
}
