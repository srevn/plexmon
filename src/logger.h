#ifndef LOGGER_H
#define LOGGER_H

#include <stdbool.h>
#include <stdio.h>
#include <syslog.h>

/* Logger configuration */
#define DEFAULT_LOG_FILE "/var/log/plexmon.log" /* Default log file path for daemon mode */
#define DEFAULT_LOG_LEVEL LOG_INFO              /* Default logging level (syslog levels) */

/* Global log file handle */
extern FILE *g_log_file;                        /* File handle for log output in daemon mode */

/* Logger lifecycle management */
bool log_init(void);
void log_cleanup(void);

/* Logging operations */
void log_message(int priority, const char *format, ...);

#endif /* LOGGER_H */
