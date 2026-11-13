/* See o6q_slog.h.  SPDX-License-Identifier: Apache-2.0 */
#ifndef _QNX_SOURCE
#define _QNX_SOURCE
#endif

#include "o6q_slog.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef O6Q_HAVE_SLOG2
#include <sys/slog2.h>
#endif

static const char *const category_names[] = {
    "network", "channel", "session", "server", "client",
    "userland", "security", "eventloop", "pubsub", "discovery"};

typedef struct {
    UA_LogLevel min_level;
    int copy_to_stderr;
    int have_slog2;
#ifdef O6Q_HAVE_SLOG2
    slog2_buffer_t buffer;
#endif
} SlogState;

static SlogState state;
static pthread_mutex_t stderr_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *level_name(UA_LogLevel level) {
    if(level >= UA_LOGLEVEL_FATAL)
        return "fatal";
    if(level >= UA_LOGLEVEL_ERROR)
        return "error";
    if(level >= UA_LOGLEVEL_WARNING)
        return "warn";
    if(level >= UA_LOGLEVEL_INFO)
        return "info";
    if(level >= UA_LOGLEVEL_DEBUG)
        return "debug";
    return "trace";
}

#ifdef O6Q_HAVE_SLOG2
static uint8_t slog2_severity(UA_LogLevel level) {
    if(level >= UA_LOGLEVEL_FATAL)
        return SLOG2_CRITICAL;
    if(level >= UA_LOGLEVEL_ERROR)
        return SLOG2_ERROR;
    if(level >= UA_LOGLEVEL_WARNING)
        return SLOG2_WARNING;
    if(level >= UA_LOGLEVEL_INFO)
        return SLOG2_INFO;
    return SLOG2_DEBUG1;
}
#endif

static void slog_cb(void *context, UA_LogLevel level, UA_LogCategory category, const char *msg,
                    va_list args) {
    SlogState *s = (SlogState *)context;
    const char *cat = "other";
    char text[1024];

    if(level < s->min_level)
        return;
    if((int)category >= 0 &&
       (size_t)category < sizeof(category_names) / sizeof(category_names[0]))
        cat = category_names[(int)category];
    vsnprintf(text, sizeof(text), msg, args);

#ifdef O6Q_HAVE_SLOG2
    if(s->have_slog2)
        slog2f(s->buffer, (uint16_t)category, slog2_severity(level), "%s/%s %s",
               level_name(level), cat, text);
#endif
    if(!s->have_slog2 || s->copy_to_stderr) {
        pthread_mutex_lock(&stderr_lock);
        fprintf(stderr, "[%s/%s] %s\n", level_name(level), cat, text);
        pthread_mutex_unlock(&stderr_lock);
    }
}

/* clear == NULL: the logger lives as long as the process. */
static UA_Logger logger = {slog_cb, &state, NULL};

UA_Logger *o6q_slog_logger(UA_LogLevel min_level, int copy_to_stderr, const char **where) {
    memset(&state, 0, sizeof(state));
    state.min_level = min_level;
    state.copy_to_stderr = copy_to_stderr;
#ifdef O6Q_HAVE_SLOG2
    {
        slog2_buffer_set_config_t config;
        slog2_buffer_t handles[1];
        memset(&config, 0, sizeof(config));
        memset(handles, 0, sizeof(handles));
        config.buffer_set_name = "o6q_server";
        config.num_buffers = 1;
        config.verbosity_level = SLOG2_DEBUG1; /* filtered above, by min_level */
        config.buffer_config[0].buffer_name = "opcua";
        config.buffer_config[0].num_pages = 8; /* 4 KiB pages */
        if(slog2_register(&config, handles, 0) == 0) {
            state.buffer = handles[0];
            state.have_slog2 = 1;
        }
    }
#endif
    if(where)
        *where = state.have_slog2 ? "slog2" : "stderr";
    return &logger;
}
