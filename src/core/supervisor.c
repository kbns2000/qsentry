/*
 * Safety supervisor: debounces classifier output and drives a small,
 * auditable state machine.
 *
 *   NOMINAL --(1 sensor pair faulty)--> DEGRADED --(2 s clean)--> NOMINAL
 *      |                                    |
 *      +--(both pairs faulty | stream lost)-+--> SAFE_STOP (latched; reset)
 *
 * The two pairs (speed/accel, steer/yaw) are physically redundant, so a
 * fault confined to one pair still allows limp-home on the other.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include "qs_core.h"

#define QS_CLEAR_TICKS 100   /* 1 s of confident OK clears a verdict  */
#define QS_CLEAR_PROB  0.90f

void qs_sup_init(qs_sup_t *s)
{
    memset(s, 0, sizeof(*s));
    s->state = QS_ST_NOMINAL;
    snprintf(s->reason, sizeof(s->reason), "startup");
}

void qs_sup_reset(qs_sup_t *s)
{
    uint32_t tr = s->transitions, tk = s->tick;
    qs_sup_init(s);
    s->transitions = tr + 1;
    s->tick = tk;
    snprintf(s->reason, sizeof(s->reason), "operator reset");
}

static int set_state(qs_sup_t *s, enum qs_state st, const char *why)
{
    if (s->state == st)
        return 0;
    s->state = st;
    s->transitions++;
    snprintf(s->reason, sizeof(s->reason), "%s", why);
    return 1;
}

int qs_sup_timeout(qs_sup_t *s, uint32_t gap_ms)
{
    char why[96];
    snprintf(why, sizeof(why), "sensor stream lost (%u ms gap)", (unsigned)gap_ms);
    return set_state(s, QS_ST_SAFE_STOP, why);
}

int qs_sup_update(qs_sup_t *s, const int cls[QS_NCH], const float prob[QS_NCH][QS_NCLASS])
{
    s->tick++;
    for (int ch = 0; ch < QS_NCH; ch++) {
        int c = cls[ch];
        int faulty = (c != QS_F_OK) && (prob[ch][c] >= QS_FAULT_PROB);
        if (faulty) {
            if (s->streak_cls[ch] == c) {
                if (s->streak[ch] < 0xFFFF) s->streak[ch]++;
            } else {
                s->streak_cls[ch] = (uint8_t)c;
                s->streak[ch] = 1;
            }
            if (s->streak[ch] >= QS_DEBOUNCE) {
                s->verdict[ch] = (uint8_t)c;
                s->conf[ch] = prob[ch][c];
            }
        } else {
            if (s->streak_cls[ch] != QS_F_OK) {
                s->streak_cls[ch] = QS_F_OK;
                s->streak[ch] = 0;
            }
            /* clearing needs positive evidence of health, not merely the
               absence of a confident fault (hysteresis against an
               uncertain classifier) */
            if (prob[ch][QS_F_OK] >= QS_CLEAR_PROB) {
                if (s->streak[ch] < 0xFFFF) s->streak[ch]++;
            } else if (s->verdict[ch] != QS_F_OK) {
                s->streak[ch] = 0;
            }
            if (s->streak[ch] >= QS_CLEAR_TICKS) {
                s->verdict[ch] = QS_F_OK;
                s->conf[ch] = prob[ch][QS_F_OK];
            }
        }
    }

    if (s->state == QS_ST_SAFE_STOP)
        return 0;                       /* latched */

    int pair_a = s->verdict[QS_CH_SPEED] || s->verdict[QS_CH_ACCEL];
    int pair_b = s->verdict[QS_CH_STEER] || s->verdict[QS_CH_YAW];
    /* Fault isolation: the steer/yaw cross-check uses wheel speed, so while
       speed itself is condemned, pair-B alarms are treated as consequences
       and do not escalate on their own. */
    if (s->verdict[QS_CH_SPEED])
        pair_b = 0;
    char why[96];

    if (pair_a && pair_b)
        return set_state(s, QS_ST_SAFE_STOP, "faults on both redundant pairs");

    if (pair_a || pair_b) {
        s->clean_ticks = 0;
        for (int ch = 0; ch < QS_NCH; ch++)
            if (s->verdict[ch]) {
                snprintf(why, sizeof(why), "%s %s (p=%.2f)", qs_channel_name[ch],
                         qs_fault_name[s->verdict[ch]], (double)s->conf[ch]);
                break;
            }
        if (s->state != QS_ST_DEGRADED)
            return set_state(s, QS_ST_DEGRADED, why);
        return 0;
    }

    if (s->state == QS_ST_DEGRADED) {
        if (++s->clean_ticks >= QS_RECOVER_TICKS)
            return set_state(s, QS_ST_NOMINAL, "all channels clean for 2 s");
    }
    return 0;
}

void qs_engine_init(qs_engine_t *e)
{
    memset(e, 0, sizeof(*e));
    qs_window_init(&e->win);
    qs_sup_init(&e->sup);
}

int qs_engine_step(qs_engine_t *e, const float x[QS_NCH])
{
    memcpy(e->last, x, sizeof(e->last));
    qs_window_push(&e->win, x);
    if (!qs_window_ready(&e->win))
        return 0;
    qs_features(&e->win, e->feat);
    for (int ch = 0; ch < QS_NCH; ch++)
        e->cls[ch] = qs_model_infer(ch, e->feat[ch], e->prob[ch]);
    return qs_sup_update(&e->sup, e->cls, (const float (*)[QS_NCLASS])e->prob);
}
