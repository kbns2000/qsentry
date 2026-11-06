/*
 * qsensord - simulated vehicle sensor ECU for QNX 8.0
 * Samples 4 sensors at 100 Hz (absolute-time clock_nanosleep) and delivers
 * each sample to qsentryd with MsgSend(). The reply may carry a
 * fault-injection command that the operator wrote to /dev/qsentry.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "qs_core.h"
#include "qs_proto.h"

int main(int argc, char **argv)
{
    int prio = 30, opt;
    uint64_t seed = 2026;
    while ((opt = getopt(argc, argv, "p:s:h")) != -1) {
        switch (opt) {
        case 'p': prio = atoi(optarg); break;
        case 's': seed = strtoull(optarg, NULL, 10); break;
        default:
            fprintf(stderr, "usage: qsensord [-p prio] [-s seed]\n");
            return 1;
        }
    }
    struct sched_param sp = { .sched_priority = prio };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    int coid;
    while ((coid = name_open(QS_SERVICE_NAME, 0)) == -1) {
        fprintf(stderr, "qsensord: waiting for qsentryd (%s)\n", strerror(errno));
        sleep(1);
    }
    printf("qsensord: connected, streaming 4 channels at %d Hz (prio %d, seed %llu)\n",
           QS_RATE_HZ, prio, (unsigned long long)seed);
    fflush(stdout);

    qs_sim_t sim;
    qs_sim_init(&sim, seed);
    qs_sample_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = QS_MSG_SAMPLE;

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (uint32_t seq = 1;; seq++) {
        next.tv_nsec += 1000000000L / QS_RATE_HZ;
        if (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR)
            ;

        qs_sim_step(&sim, m.x);
        m.seq = seq;
        m.t_cycles = ClockCycles();

        qs_sample_reply_t rep;
        if (MsgSend(coid, &m, sizeof(m), &rep, sizeof(rep)) == -1) {
            fprintf(stderr, "qsensord: MsgSend: %s - reconnecting\n", strerror(errno));
            name_close(coid);
            while ((coid = name_open(QS_SERVICE_NAME, 0)) == -1)
                sleep(1);
            continue;
        }
        if (rep.cmd == QS_CMD_INJECT) {
            qs_sim_inject(&sim, rep.ch, rep.fault);
            printf("qsensord: injecting %s on %s\n", qs_fault_name[rep.fault], qs_channel_name[rep.ch]);
            fflush(stdout);
        } else if (rep.cmd == QS_CMD_CLEAR) {
            qs_sim_clear(&sim);
            printf("qsensord: faults cleared\n");
            fflush(stdout);
        }
    }
}
