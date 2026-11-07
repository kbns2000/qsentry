/*
 * Deterministic vehicle + sensor simulator with fault injection.
 * The same code generates training data, offline evaluation and the
 * live sensor stream on QNX, so the model is trained on exactly what
 * it will see at run time.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <string.h>
#include "qs_core.h"


const char *const qs_channel_name[QS_NCH] = { "speed", "accel", "steer", "yaw" };
const char *const qs_fault_name[QS_NCLASS] = { "ok", "stuck", "drift", "spike", "noise" };
const char *const qs_state_name[3] = { "NOMINAL", "DEGRADED", "SAFE_STOP" };

static const qs_profile_t profiles[2] = {
    { "sim",
      { 0.05f, 0.15f, 0.005f, 0.01f }, { 1.0f, 1.0f, 0.05f, 0.05f },
      { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, 0.0f, 0.0f, 0.0f, 0.0f, 0.035f, 0.010f, 2.8f },
    /* measured on comma2k19 Example_1 segment 40 (tools/real_prep.py, docs/real_data.md) */
    { "comma2k19",
      { 0.016f, 0.50f, 0.00002f, 0.0026f }, { 1.0f, 1.0f, 0.05f, 0.05f },
      { 0.0007f, 0, 0.000113f, 0.0012f }, { 0.24f, 0, 0, 0.17f }, 0.04f, 4.0f, 0.30f, 0.6f, 0.45f, 0.006f, 2.70f },
};
const qs_profile_t *qs_prof = &profiles[0];

int qs_set_profile(int id)
{
    if (id < 0 || id > 1) return -1;
    qs_prof = &profiles[id];
    return 0;
}

static uint64_t xs(uint64_t *s)
{
    /* xorshift64* */
    uint64_t x = *s;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

float qs_rand_uniform(uint64_t *st)
{
    return (float)((xs(st) >> 40) * (1.0 / 16777216.0));
}

float qs_rand_normal(uint64_t *st)
{
    /* Irwin-Hall approximation: bounded time, no log/sqrt */
    float s = 0.0f;
    for (int i = 0; i < 12; i++)
        s += qs_rand_uniform(st);
    return s - 6.0f;
}

void qs_sim_init(qs_sim_t *s, uint64_t seed)
{
    memset(s, 0, sizeof(*s));
    s->rng = seed ? seed : 0x9E3779B97F4A7C15ULL;
    s->v = 8.0f + 10.0f * qs_rand_uniform(&s->rng);
    s->next_change = 0;
}

void qs_sim_inject(qs_sim_t *s, int ch, int fault)
{
    if (ch < 0 || ch >= QS_NCH || fault < 0 || fault >= QS_NCLASS)
        return;
    s->fault[ch] = (uint8_t)fault;
    s->fault_t0[ch] = s->tick;
    s->frozen[ch] = NAN;
    s->drift_sign[ch] = (qs_rand_uniform(&s->rng) < 0.5f) ? -1.0f : 1.0f;
}

void qs_sim_clear(qs_sim_t *s)
{
    memset(s->fault, 0, sizeof(s->fault));
}

void qs_sim_step(qs_sim_t *s, float out[QS_NCH])
{
    /* driver: piecewise-constant targets every 1..4 s */
    if (s->tick >= s->next_change) {
        s->a_cmd = -2.0f + 4.0f * qs_rand_uniform(&s->rng);
        s->steer_cmd = -0.25f + 0.5f * qs_rand_uniform(&s->rng);
        s->next_change = s->tick + 100 + (uint32_t)(300 * qs_rand_uniform(&s->rng));
        if (qs_prof->steady_p > 0.0f && qs_rand_uniform(&s->rng) < qs_prof->steady_p)
            s->steer_cmd *= 0.02f;            /* highway: nearly straight, held */
    }
    if (s->v < 3.0f && s->a_cmd < 0.0f)  s->a_cmd = -s->a_cmd;
    if (s->v > 27.0f && s->a_cmd > 0.0f) s->a_cmd = -s->a_cmd;

    /* first-order actuator lag */
    s->a += (s->a_cmd - s->a) * (QS_DT / 0.3f);
    s->steer += (s->steer_cmd - s->steer) * (QS_DT / 0.5f);

    /* keep integration exactly consistent with the reported accel */
    float vn = s->v + s->a * QS_DT;
    if (vn < 0.0f)  { s->a = -s->v / QS_DT; vn = 0.0f; }
    if (vn > 30.0f) { s->a = (30.0f - s->v) / QS_DT; vn = 30.0f; }
    s->v = vn;
    s->yaw = s->v * tanf(s->steer) / qs_prof->wheelbase;

    float truth[QS_NCH] = { s->v, s->a, s->steer, s->yaw };
    if (qs_prof->abias_sd > 0.0f) {           /* Ornstein-Uhlenbeck bias, tau 2 s */
        s->abias += -s->abias * (QS_DT / 2.0f)
                  + qs_prof->abias_sd * sqrtf(2.0f * QS_DT / 2.0f) * qs_rand_normal(&s->rng);
        truth[QS_CH_ACCEL] += s->abias;
    }

    for (int ch = 0; ch < QS_NCH; ch++) {
        const float *noise_sd = qs_prof->noise_sd, *scale = qs_prof->scale;
        float x = truth[ch] + noise_sd[ch] * qs_rand_normal(&s->rng);
        if (ch == QS_CH_ACCEL && qs_prof->tail_p > 0.0f && qs_rand_uniform(&s->rng) < qs_prof->tail_p)
            x += qs_prof->tail_k * noise_sd[ch] * qs_rand_normal(&s->rng);
        float t = (float)(s->tick - s->fault_t0[ch]) * QS_DT;
        switch (s->fault[ch]) {
        case QS_F_STUCK:
            if (isnan(s->frozen[ch]))
                s->frozen[ch] = x;
            x = s->frozen[ch];
            break;
        case QS_F_DRIFT:
            x += s->drift_sign[ch] * 0.5f * scale[ch] * t;
            break;
        case QS_F_SPIKE:
            if (qs_rand_uniform(&s->rng) < 0.08f) {
                float m = 5.0f + 5.0f * qs_rand_uniform(&s->rng);
                x += ((qs_rand_uniform(&s->rng) < 0.5f) ? -m : m) * scale[ch];
            }
            break;
        case QS_F_NOISE:
            x += 12.0f * noise_sd[ch] * qs_rand_normal(&s->rng);
            break;
        default:
            break;
        }
        if (qs_prof->quant[ch] > 0.0f)
            x = roundf(x / qs_prof->quant[ch]) * qs_prof->quant[ch];
        if (qs_prof->dup[ch] > 0.0f && s->tick > 0 && qs_rand_uniform(&s->rng) < qs_prof->dup[ch])
            x = s->last_out[ch];
        s->last_out[ch] = x;
        out[ch] = x;
    }
    s->tick++;
}

/* exported for features.c */
float qs_noise_sd(int ch) { return qs_prof->noise_sd[ch]; }
float qs_scale(int ch)    { return qs_prof->scale[ch]; }
float qs_wheelbase(void)  { return qs_prof->wheelbase; }
