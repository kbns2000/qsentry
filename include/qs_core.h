/*
 * qsentry - on-device sensor-fault sentinel for QNX
 * Portable core: simulator, feature extraction, tiny neural classifier,
 * safety supervisor. No dynamic allocation, no OS calls, bounded time.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QS_CORE_H
#define QS_CORE_H

#include <stdint.h>
#include <stddef.h>

#define QS_NCH        4      /* number of sensor channels            */
#define QS_RATE_HZ    100    /* sample / supervisor tick rate        */
#define QS_DT         (1.0f / QS_RATE_HZ)
#define QS_WIN        100    /* sliding window length (1 s)          */
#define QS_NFEAT      8      /* per-channel features                 */
#define QS_NIN        (QS_NFEAT + QS_NCH) /* features + channel one-hot */
#define QS_NHID       16
#define QS_NCLASS     5

/* Channels: two physically-coupled pairs give the model redundancy. */
enum qs_channel {
    QS_CH_SPEED = 0,  /* wheel speed, m/s                       */
    QS_CH_ACCEL = 1,  /* IMU longitudinal acceleration, m/s^2   */
    QS_CH_STEER = 2,  /* steering angle, rad                    */
    QS_CH_YAW   = 3   /* IMU yaw rate, rad/s                    */
};

enum qs_fault {
    QS_F_OK    = 0,
    QS_F_STUCK = 1,   /* value frozen                           */
    QS_F_DRIFT = 2,   /* growing bias                           */
    QS_F_SPIKE = 3,   /* sparse large outliers                  */
    QS_F_NOISE = 4    /* variance blow-up                       */
};

enum qs_state {
    QS_ST_NOMINAL   = 0,
    QS_ST_DEGRADED  = 1,  /* one channel masked, limp-home      */
    QS_ST_SAFE_STOP = 2   /* latched until operator reset      */
};

extern const char *const qs_channel_name[QS_NCH];
extern const char *const qs_fault_name[QS_NCLASS];
extern const char *const qs_state_name[3];

/* ---------------- simulator (deterministic) ---------------- */
typedef struct {
    uint64_t rng;
    uint32_t tick;
    float    v, a, steer, yaw;     /* true vehicle state            */
    float    a_cmd, steer_cmd;     /* driver targets                */
    uint32_t next_change;
    /* fault injection, per channel */
    uint8_t  fault[QS_NCH];
    uint32_t fault_t0[QS_NCH];
    float    frozen[QS_NCH];
    float    drift_sign[QS_NCH];
} qs_sim_t;

void  qs_sim_init(qs_sim_t *s, uint64_t seed);
/* Produce one sample vector (what the sensors report). */
void  qs_sim_step(qs_sim_t *s, float out[QS_NCH]);
void  qs_sim_inject(qs_sim_t *s, int ch, int fault);
void  qs_sim_clear(qs_sim_t *s);
float qs_rand_uniform(uint64_t *st);
float qs_rand_normal(uint64_t *st);

/* ---------------- features ---------------- */
typedef struct {
    float    buf[QS_NCH][QS_WIN];
    uint32_t head;      /* next write index */
    uint32_t count;     /* samples seen (saturates at QS_WIN) */
} qs_window_t;

void qs_window_init(qs_window_t *w);
void qs_window_push(qs_window_t *w, const float x[QS_NCH]);
int  qs_window_ready(const qs_window_t *w);
/* Compute QS_NFEAT features for every channel. */
void qs_features(const qs_window_t *w, float feat[QS_NCH][QS_NFEAT]);

/* ---------------- model ---------------- */
/* Returns argmax class, writes softmax probabilities. */
int  qs_model_infer(int ch, const float feat[QS_NFEAT], float prob[QS_NCLASS]);

/* ---------------- supervisor ---------------- */
typedef struct {
    enum qs_state state;
    uint8_t  verdict[QS_NCH];      /* debounced fault class per ch */
    float    conf[QS_NCH];         /* probability of verdict        */
    uint8_t  streak_cls[QS_NCH];
    uint16_t streak[QS_NCH];
    uint16_t clean_ticks;
    uint32_t transitions;
    uint32_t tick;
    char     reason[96];
} qs_sup_t;

#define QS_DEBOUNCE      8     /* consecutive ticks to confirm a fault  */
#define QS_RECOVER_TICKS 200   /* 2 s clean before leaving DEGRADED     */
#define QS_FAULT_PROB    0.70f

void qs_sup_init(qs_sup_t *s);
/* One supervisor tick from classifier output. Returns 1 on state change. */
int  qs_sup_update(qs_sup_t *s, const int cls[QS_NCH], const float prob[QS_NCH][QS_NCLASS]);
/* Sensor stream lost (watchdog deadline missed). */
int  qs_sup_timeout(qs_sup_t *s, uint32_t gap_ms);
void qs_sup_reset(qs_sup_t *s);

/* ---------------- whole pipeline ---------------- */
typedef struct {
    qs_window_t win;
    qs_sup_t    sup;
    float       feat[QS_NCH][QS_NFEAT];
    float       prob[QS_NCH][QS_NCLASS];
    int         cls[QS_NCH];
    float       last[QS_NCH];
} qs_engine_t;

void qs_engine_init(qs_engine_t *e);
/* Push a sample and run detection; returns 1 if supervisor state changed. */
int  qs_engine_step(qs_engine_t *e, const float x[QS_NCH]);

#endif
