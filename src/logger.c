#include "logger.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include "config.h"

/* Initialize logging */
bool log_init(void) {
	/* If no log file is specified, use the default */
	if (strlen(g_config.log_file) == 0) {
		strcpy(g_config.log_file, DEFAULT_LOG_FILE);
	}

	/* If we're in daemon mode, open the log file */
	if (g_config.daemonize) {
		g_log_file = fopen(g_config.log_file, "w");
		if (!g_log_file) {
			fprintf(stderr, "Failed to open log file %s: %s\n",
					g_config.log_file, strerror(errno));
			return false;
		}

		/* Set line buffering for the log file */
		setvbuf(g_log_file, NULL, _IOLBF, 0);
	}

	return true;
}

/* Clean up logging */
void log_cleanup(void) {
	if (g_log_file) {
		fclose(g_log_file);
		g_log_file = NULL;
	}
}

/* Log message to file and/or stdout */
void log_message(int priority, const char *format, ...) {
	/* Skip messages with priority lower than configured level */
	if (priority > g_config.log_level) {
		return;
	}

	va_list ap;
	time_t now;
	struct tm *timeinfo;
	char timestamp[20];
	char message[2048];

	/* Get current timestamp */
	time(&now);
	timeinfo = localtime(&now);
	strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);

	/* Format the priority string */
	const char *priority_str;
	switch (priority) {
		case LOG_ERR:
			priority_str = "ERROR";
			break;
		case LOG_WARNING:
			priority_str = "WARNING";
			break;
		case LOG_INFO:
			priority_str = "INFO";
			break;
		case LOG_DEBUG:
			priority_str = "DEBUG";
			break;
		default:
			priority_str = "UNKNOWN";
	}

	/* Format the message */
	va_start(ap, format);
	vsnprintf(message, sizeof(message), format, ap);
	va_end(ap);

	/* Log to file if daemon mode */
	if (g_log_file) {
		fprintf(g_log_file, "[%s] %s: %s\n", timestamp, priority_str, message);
		fflush(g_log_file);
	}

	/* If verbose and not daemon, log to stdout */
	if (g_config.verbose && !g_config.daemonize) {
		fprintf(stdout, "[%s] %s: %s\n", timestamp, priority_str, message);
	}
}
