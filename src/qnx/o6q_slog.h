/* An open62541 logger that writes to the QNX system logger, slog2.
 *
 * The messages land in the buffer set "o6q_server", buffer "opcua". Read them
 * on the target with
 *
 *   slog2info -b o6q_server        (add -w to follow new messages)
 *
 * Where slog2 is not available (another operating system, a build without
 * O6Q_HAVE_SLOG2, or slogger2 is not running) the messages go to stderr.
 *
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef O6Q_SLOG_H_
#define O6Q_SLOG_H_

#include "open62541.h"

/* Returns the logger (one per process). Messages below min_level are
 * dropped. copy_to_stderr != 0 also prints every message on stderr. *where
 * (if not NULL) is set to "slog2" or "stderr". */
UA_Logger *o6q_slog_logger(UA_LogLevel min_level, int copy_to_stderr, const char **where);

#endif /* O6Q_SLOG_H_ */
