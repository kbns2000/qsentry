/*
 * Tiny MLP classifier: (QS_NFEAT features + channel one-hot) -> 16 ReLU -> 5.
 * Weights are trained offline by train/train.py on data produced by the very
 * same simulator and feature code, then baked into model_weights.h.
 * ~300 multiply-adds per channel: deterministic and allocation-free.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include "qs_core.h"
#include "model_weights.h"          /* profile 0: simulator sensors   */
#include "model_weights_comma.h"    /* profile 1: comma2k19-calibrated */

int qs_model_infer(int ch, const float feat[QS_NFEAT], float prob[QS_NCLASS])
{
    float in[QS_NIN], h[QS_NHID], o[QS_NCLASS];
    const int real = (qs_prof->name[0] == 'c');
    const float *MU = real ? QSC_MU : QS_MU, *SIGMA = real ? QSC_SIGMA : QS_SIGMA;
    const float (*W1)[QS_NIN] = real ? QSC_W1 : QS_W1, *B1 = real ? QSC_B1 : QS_B1;
    const float (*W2)[QS_NHID] = real ? QSC_W2 : QS_W2, *B2 = real ? QSC_B2 : QS_B2;

    for (int i = 0; i < QS_NFEAT; i++)
        in[i] = (feat[i] - MU[i]) / SIGMA[i];
    for (int i = 0; i < QS_NCH; i++)
        in[QS_NFEAT + i] = (i == ch) ? 1.0f : 0.0f;

    for (int j = 0; j < QS_NHID; j++) {
        float s = B1[j];
        for (int i = 0; i < QS_NIN; i++)
            s += W1[j][i] * in[i];
        h[j] = s > 0.0f ? s : 0.0f;
    }
    float mx = -1e30f;
    for (int k = 0; k < QS_NCLASS; k++) {
        float s = B2[k];
        for (int j = 0; j < QS_NHID; j++)
            s += W2[k][j] * h[j];
        o[k] = s;
        if (s > mx) mx = s;
    }
    float z = 0.0f;
    for (int k = 0; k < QS_NCLASS; k++) {
        o[k] = expf(o[k] - mx);
        z += o[k];
    }
    int best = 0;
    for (int k = 0; k < QS_NCLASS; k++) {
        prob[k] = o[k] / z;
        if (prob[k] > prob[best]) best = k;
    }
    return best;
}
