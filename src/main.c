#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "dircache.h"
#include "events.h"
#include "logger.h"
#include "monitor.h"
#include "plexapi.h"

#define PLEXMON_VERSION "1.0.0" /* Version information */

volatile sig_atomic_t g_running = 1;    /* Global running flag */
FILE *g_log_file = NULL;                /* Global log file handle */
config_t g_config;                      /* Global configuration */

/* Forward declarations for helper functions */
static bool daemonize(void);
static void cleanup(void);

/* Print usage information */
static void print_usage(const char *prog_name) {
	fprintf(stderr, "Usage: %s [OPTIONS]\n\n", prog_name);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  -c FILE    Path to configuration file (default: %s)\n", DEFAULT_CONFIG_FILE);
	fprintf(stderr, "  -v         Verbose mode\n");
	fprintf(stderr, "  -d         Run as daemon\n");
	fprintf(stderr, "  -t SECONDS Startup timeout in seconds (default: 60)\n");
	fprintf(stderr, "  -h         Show this help message\n");
}

/* Signal handler */
static void signal_handler(int sig) {
	switch (sig) {
		case SIGINT:
		case SIGTERM:
			log_message(LOG_INFO, "Received signal %d, shutting down", sig);
			monitor_exit(); /* Signal exit through kqueue */
			break;
		case SIGHUP:
			log_message(LOG_INFO, "Received SIGHUP, reloading configuration");
			monitor_reload(); /* Signal reload through kqueue */
			break;
	}
}

/* Main entry point */
int main(int argc, char *argv[]) {
	int opt;
	char *config_path = DEFAULT_CONFIG_FILE;

	/* Set default configuration values */
	memset(&g_config, 0, sizeof(g_config));
	strcpy(g_config.plex_url, DEFAULT_PLEX_URL);
	strcpy(g_config.log_file, DEFAULT_LOG_FILE);
	g_config.scan_interval = DEFAULT_SCAN_INTERVAL;
	g_config.startup_timeout = 60;
	g_config.verbose = false;
	g_config.daemonize = false;
	g_config.log_level = DEFAULT_LOG_LEVEL;

	/* Parse command line options */
	while ((opt = getopt(argc, argv, "c:t:vdh")) != -1) {
		switch (opt) {
			case 'c':
				config_path = optarg;
				break;
			case 't':
				g_config.startup_timeout = atoi(optarg);
				if (g_config.startup_timeout <= 0) {
					fprintf(stderr, "Invalid timeout value: %s\n", optarg);
					return EXIT_FAILURE;
				}
				break;
			case 'v':
				g_config.verbose = true;
				break;
			case 'd':
				g_config.daemonize = true;
				break;
			case 'h':
				print_usage(argv[0]);
				return EXIT_SUCCESS;
			default:
				print_usage(argv[0]);
				return EXIT_FAILURE;
		}
	}

	/* Load configuration */
	if (!config_load(config_path)) {
		fprintf(stderr, "Failed to load configuration from %s\n", config_path);
		return EXIT_FAILURE;
	}

	/* Initialize logging */
	if (!log_init()) {
		fprintf(stderr, "Failed to initialize logging\n");
		return EXIT_FAILURE;
	}

	/* Log startup message */
	log_message(LOG_INFO, "Starting plexmon version %s", PLEXMON_VERSION);

	/* Daemonize if requested */
	if (g_config.daemonize && !daemonize()) {
		log_message(LOG_ERR, "Failed to daemonize process");
		log_cleanup();
		return EXIT_FAILURE;
	}

	/* Set up signal handlers */
	signal(SIGINT, signal_handler);
	signal(SIGHUP, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Initialize components */
	if (!plexapi_init()) {
		log_message(LOG_ERR, "Failed to initialize Plex API client");
		cleanup();
		return EXIT_FAILURE;
	}

	if (!events_init()) {
		log_message(LOG_ERR, "Failed to initialize event processor");
		cleanup();
		return EXIT_FAILURE;
	}

	/* Initialize directory cache */
	if (!dircache_init()) {
		log_message(LOG_ERR, "Failed to initialize directory cache");
		cleanup();
		return EXIT_FAILURE;
	}

	/* Initialize file system monitoring */
	if (!monitor_init()) {
		log_message(LOG_ERR, "Failed to initialize file system monitoring");
		cleanup();
		return EXIT_FAILURE;
	}

	/* Check connectivity to Plex Media Server */
	if (!plexapi_check()) {
		log_message(LOG_ERR, "Failed to connect to Plex Media Server. Exiting.");
		cleanup();
		return EXIT_FAILURE;
	}

	/* Get libraries from Plex */
	if (!plexapi_libraries()) {
		log_message(LOG_ERR, "Failed to get library directories from Plex");
		cleanup();
		return EXIT_FAILURE;
	}

	log_message(LOG_INFO, "Monitoring %d directories for changes", monitor_count());

	/* Main event loop */
	if (!monitor_loop()) {
		log_message(LOG_ERR, "Event processing loop failed");
		cleanup();
		return EXIT_FAILURE;
	}

	/* Clean up */
	cleanup();

	log_message(LOG_INFO, "plexmon terminated");
	log_cleanup();

	return EXIT_SUCCESS;
}

/* Daemonize the process */
static bool daemonize(void) {
	pid_t pid, sid;

	/* Fork off the parent process */
	pid = fork();

	/* An error occurred */
	if (pid < 0) {
		log_message(LOG_ERR, "Failed to fork process: %s", strerror(errno));
		return false;
	}

	/* Success: Let the parent terminate */
	if (pid > 0) {
		exit(EXIT_SUCCESS);
	}

	/* The child process becomes session leader */
	sid = setsid();
	if (sid < 0) {
		log_message(LOG_ERR, "Failed to create new session: %s", strerror(errno));
		return false;
	}

	/* Ignore signals that might terminate the process */
	signal(SIGCHLD, SIG_IGN);
	signal(SIGHUP, SIG_IGN);

	/* Fork off for the second time to ensure we can't acquire a terminal */
	pid = fork();

	/* An error occurred */
	if (pid < 0) {
		log_message(LOG_ERR, "Failed to fork daemon process: %s", strerror(errno));
		return false;
	}

	/* Success: Let the parent terminate */
	if (pid > 0) {
		exit(EXIT_SUCCESS);
	}

	/* Set new file permissions */
	umask(0);

	/* Close all open file descriptors */
	int max_fd = sysconf(_SC_OPEN_MAX);
	if (max_fd == -1) {
		max_fd = 1024; /* Default if sysconf fails */
	}
	for (int fd = 0; fd < max_fd; fd++) {
		if (fd != fileno(g_log_file)) { /* Keep log file open */
			close(fd);
		}
	}

	/* Reopen standard file descriptors */
	stdin = fopen("/dev/null", "r");
	if (stdin == NULL) {
		log_message(LOG_WARNING, "Failed to reopen stdin: %s", strerror(errno));
	}

	stdout = fopen("/dev/null", "w");
	if (stdout == NULL) {
		log_message(LOG_WARNING, "Failed to reopen stdout: %s", strerror(errno));
	}

	stderr = fopen("/dev/null", "w");
	if (stderr == NULL) {
		log_message(LOG_WARNING, "Failed to reopen stderr: %s", strerror(errno));
	}

	return true;
}

/* Clean up all components */
static void cleanup(void) {
	monitor_cleanup();
	events_cleanup();
	dircache_cleanup();
	plexapi_cleanup();
}
