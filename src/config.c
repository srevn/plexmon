#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logger.h"

/* Load configuration from file */
bool config_load(const char *config_path) {
	FILE *fp;
	char line[1024];
	char key[256], value[768];

	log_message(LOG_INFO, "Loading configuration from %s", config_path);

	fp = fopen(config_path, "r");
	if (!fp) {
		log_message(LOG_WARNING, "Could not open config file %s: %s", config_path,
					strerror(errno));
		log_message(LOG_INFO, "Using default configuration");
		return true; /* Continue with defaults */
	}

	/* Parse configuration file */
	while (fgets(line, sizeof(line), fp)) {
		/* Skip comments and empty lines */
		if (line[0] == '#' || line[0] == '\n') continue;

		/* Remove trailing newline */
		int len = strlen(line);
		if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

		/* Parse key=value format */
		char *separator = strchr(line, '=');
		if (separator) {
			/* Extract key */
			*separator = '\0';
			strncpy(key, line, sizeof(key) - 1);
			key[sizeof(key) - 1] = '\0';

			/* Extract value */
			char *value_ptr = separator + 1;
			strncpy(value, value_ptr, sizeof(value) - 1);
			value[sizeof(value) - 1] = '\0';

			/* Trim leading whitespace from key */
			char *k = key;
			while (isspace((unsigned char) *k)) k++;

			/* Trim trailing whitespace from key */
			char *end = k + strlen(k) - 1;
			while (end > k && isspace((unsigned char) *end)) *end-- = '\0';

			/* Trim leading whitespace from value */
			char *v = value;
			while (isspace((unsigned char) *v)) v++;

			/* Trim trailing whitespace from value */
			end = v + strlen(v) - 1;
			while (end > v && isspace((unsigned char) *end)) *end-- = '\0';

			/* Process configuration options */
			if (strcmp(k, "plex_url") == 0) {
				strncpy(g_config.plex_url, v, PATH_MAX_LEN - 1);
				g_config.plex_url[PATH_MAX_LEN - 1] = '\0';
			} else if (strcmp(k, "plex_token") == 0) {
				strncpy(g_config.plex_token, v, TOKEN_MAX_LEN - 1);
				g_config.plex_token[TOKEN_MAX_LEN - 1] = '\0';
			} else if (strcmp(k, "scan_interval") == 0) {
				g_config.scan_interval = atoi(v);
			} else if (strcmp(k, "startup_timeout") == 0) {
				g_config.startup_timeout = atoi(v);
			} else if (strcmp(k, "log_level") == 0) {
				if (strcasecmp(v, "debug") == 0) {
					g_config.log_level = LOG_DEBUG;
				} else if (strcasecmp(v, "info") == 0) {
					g_config.log_level = LOG_INFO;
				} else {
					log_message(LOG_WARNING, "Invalid log_level (%s), using default", v);
				}
			} else if (strcmp(k, "log_file") == 0) {
				strncpy(g_config.log_file, v, PATH_MAX_LEN - 1);
				g_config.log_file[PATH_MAX_LEN - 1] = '\0';
			} else {
				log_message(LOG_WARNING, "Unknown configuration option: %s", k);
			}
		}
	}

	fclose(fp);
	log_message(LOG_INFO, "Configuration loaded successfully");

	/* Validate configuration */
	if (strlen(g_config.plex_token) == 0) {
		log_message(LOG_WARNING, "No Plex token provided in configuration");
	}

	if (g_config.startup_timeout <= 0) {
		log_message(LOG_WARNING, "Invalid startup timeout (%d), using default of 60s",
					g_config.startup_timeout);
		g_config.startup_timeout = 60;
	}

	if (g_config.scan_interval <= 0) {
		log_message(LOG_WARNING, "Invalid scan interval (%d), using default of %ds",
					g_config.scan_interval, DEFAULT_SCAN_INTERVAL);
		g_config.scan_interval = DEFAULT_SCAN_INTERVAL;
	}

	return true;
}
