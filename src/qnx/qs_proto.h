/*
 * Message protocol between qsensord (client) and qsentryd (server).
 * Native QNX synchronous message passing: the sensor MsgSend()s each sample
 * and is unblocked by the reply, which may carry a fault-injection command.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QS_PROTO_H
#define QS_PROTO_H

#include <stdint.h>
#include <sys/iomsg.h>
#include "qs_core.h"

#define QS_SERVICE_NAME   "qsentry"
#define QS_DEV_PATH       "/dev/qsentry"

#define QS_MSG_SAMPLE     (_IO_MAX + 1)

#define QS_PULSE_TICK     (_PULSE_CODE_MINAVAIL + 0)

typedef struct {
    uint16_t type;          /* QS_MSG_SAMPLE */
    uint16_t pad;
    uint32_t seq;
    uint64_t t_cycles;      /* ClockCycles() at sampling */
    float    x[QS_NCH];
} qs_sample_msg_t;

enum qs_cmd { QS_CMD_NONE = 0, QS_CMD_INJECT = 1, QS_CMD_CLEAR = 2 };

typedef struct {
    int32_t cmd;
    int32_t ch;
    int32_t fault;
    int32_t pad;
} qs_sample_reply_t;

#endif
