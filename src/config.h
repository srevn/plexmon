#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

/* Configuration constants */
#define DEFAULT_CONFIG_FILE "/usr/local/etc/plexmon.conf"  /* Default configuration file path */
#define DEFAULT_PLEX_URL "http://localhost:32400"          /* Default Plex server URL */
#define DEFAULT_SCAN_INTERVAL 1                            /* Default scan delay in seconds */
#define PATH_MAX_LEN 1024                                  /* Maximum length for filesystem paths */
#define TOKEN_MAX_LEN 128                                  /* Maximum length for authentication token */

/* Configuration structure */
typedef struct config {
	char plex_url[PATH_MAX_LEN];           /* Base URL of the Plex Media Server */
	char plex_token[TOKEN_MAX_LEN];        /* Authentication token for Plex API access */
	char log_file[PATH_MAX_LEN];           /* Path to the log file for daemon mode */
	int scan_interval;                     /* Delay in seconds before triggering a scan */
	int startup_timeout;                   /* Maximum time to wait for Plex server in seconds */
	int log_level;                         /* Logging level threshold (syslog levels) */
	bool verbose;                          /* Enable verbose output to console */
	bool daemonize;                        /* Run process as background daemon */
} config_t;

/* Global configuration instance */
extern config_t g_config;

/* Configuration management */
bool config_load(const char *config_path);

#endif /* CONFIG_H */