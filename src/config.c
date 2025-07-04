/**
 * config.c - Configuration management
 */

#include "plexmon.h"
#include <ctype.h>

/* Load configuration from file */
bool config_load(const char *config_path) {
    FILE *fp;
    char line[1024];
    char key[256], value[768];

    log_message(LOG_INFO, "Loading configuration from %s", config_path);

    fp = fopen(config_path, "r");
    if (!fp) {
        log_message(LOG_WARNING, "Could not open config file %s: %s",
                    config_path, strerror(errno));
        log_message(LOG_INFO, "Using default configuration");
        return true;  /* Continue with defaults */
    }

    /* Parse configuration file */
    while (fgets(line, sizeof(line), fp)) {
        /* Skip comments and empty lines */
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }

        /* Remove trailing newline */
        int len = strlen(line);
        if (len > 0 && line[len-1] == '\n') {
            line[len-1] = '\0';
        }

        /* Parse key=value format */
        if (sscanf(line, "%255[^=]=%767s", key, value) == 2) {
            /* Trim whitespace from key */
            char *k = key;
            while (isspace(*k)) k++;
            char *end = k + strlen(k) - 1;
            while (end > k && isspace(*end)) *end-- = '\0';

            /* Process configuration options */
            if (strcmp(k, "plex_url") == 0) {
                strncpy(g_config.plex_url, value, PATH_MAX_LEN - 1);
                g_config.plex_url[PATH_MAX_LEN - 1] = '\0';
            }
            else if (strcmp(k, "plex_token") == 0) {
                strncpy(g_config.plex_token, value, TOKEN_MAX_LEN - 1);
                g_config.plex_token[TOKEN_MAX_LEN - 1] = '\0';
            }
            else if (strcmp(k, "scan_interval") == 0) {
                g_config.scan_interval = atoi(value);
            }
            else if (strcmp(k, "startup_timeout") == 0) {
                g_config.startup_timeout = atoi(value);
            }
            else if (strcmp(k, "log_level") == 0) {
                if (strcasecmp(value, "debug") == 0) {
                    g_config.log_level = LOG_DEBUG;
                } else if (strcasecmp(value, "info") == 0) {
                    g_config.log_level = LOG_INFO;
                } else {
                    log_message(LOG_WARNING, "Invalid log_level (%s), using default", value);
                }
            }
            else if (strcmp(k, "log_file") == 0) {
                strncpy(g_config.log_file, value, PATH_MAX_LEN - 1);
                g_config.log_file[PATH_MAX_LEN - 1] = '\0';
            }
            else {
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