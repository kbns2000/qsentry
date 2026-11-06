/*
 * qsload - CPU hog for demonstrating that priority-driven scheduling keeps
 * qsentryd's deadlines while the CPU is saturated.
 *   qsload -p 9 -n 2 &   (below the shell, far below qsentryd)
 * SPDX-License-Identifier: Apache-2.0
 */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void *burn(void *a) { volatile unsigned long x = 0; (void)a; for (;;) x++; return NULL; }

int main(int argc, char **argv)
{
    int prio = 9, n = 2, opt;
    while ((opt = getopt(argc, argv, "p:n:")) != -1) {
        if (opt == 'p') prio = atoi(optarg);
        else if (opt == 'n') n = atoi(optarg);
        else { fprintf(stderr, "usage: qsload [-p prio] [-n threads]\n"); return 1; }
    }
    pthread_attr_t at;
    struct sched_param sp = { .sched_priority = prio };
    pthread_attr_init(&at);
    pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&at, SCHED_FIFO);
    pthread_attr_setschedparam(&at, &sp);
    for (int i = 0; i < n; i++) {
        pthread_t t;
        if (pthread_create(&t, &at, burn, NULL) != 0) perror("qsload: pthread_create");
    }
    printf("qsload: %d busy threads at SCHED_FIFO %d\n", n, prio);
    fflush(stdout);
    pause();
    return 0;
}
