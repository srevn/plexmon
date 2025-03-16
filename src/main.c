/**
 * main.c - Main entry point for plexmon application
 */

#include "plexmon.h"
#include <getopt.h>
#include <stdarg.h>
#include <time.h>

/* Global configuration */
config_t g_config;

/* Global log file handle */
FILE *g_log_file = NULL;

/* Global running flag */
volatile sig_atomic_t g_running = 1;

/* Forward declarations for helper functions */
static void print_usage(const char *prog_name);
static void signal_handler(int sig);
static bool daemonize(void);
static void cleanup(void);

/**
 * Initialize logging
 */
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

/**
 * Clean up logging
 */
void log_cleanup(void) {
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

/**
 * Main entry point
 */
int main(int argc, char *argv[]) {
    int opt;
    char *config_path = DEFAULT_CONFIG_FILE;
    
    /* Set default configuration values */
    memset(&g_config, 0, sizeof(g_config));
    strcpy(g_config.plex_url, DEFAULT_PLEX_URL);
    strcpy(g_config.log_file, DEFAULT_LOG_FILE);
    g_config.scan_interval = 10;
    g_config.startup_timeout = 60;
    g_config.verbose = false;
    g_config.daemonize = false;
    g_config.log_level = DEFAULT_LOG_LEVEL;
    
    /* Parse command line options */
    while ((opt = getopt(argc, argv, "c:vdh")) != -1) {
        switch (opt) {
            case 'c':
                config_path = optarg;
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
    
    /* Plex connection timeout */
    time_t start_time = time(NULL);
    time_t current_time;
    bool connected = false;
    
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
    
    /* Initialize file system monitoring */
    if (!fsmonitor_init()) {
        log_message(LOG_ERR, "Failed to initialize file system monitoring");
        cleanup();
        return EXIT_FAILURE;
    }
    
    /* Get libraries from Plex */
    log_message(LOG_INFO, "Attempting to connect to Plex Media Server...");
    
    while (!connected) {
        if (plexapi_get_libraries()) {
            connected = true;
            log_message(LOG_INFO, "Successfully connected to Plex Media Server");
            break;
        }
        
        current_time = time(NULL);
        
        if (current_time - start_time >= g_config.startup_timeout) {
            log_message(LOG_ERR, "Timeout reached after %d seconds. Exiting.", g_config.startup_timeout);
            cleanup();
            return EXIT_FAILURE;
        }
        
        sleep(5);
    }
    
    log_message(LOG_INFO, "Monitoring %d directories for changes", get_monitored_dir_count());
    
    /* Main event loop */
    if (!fsmonitor_run_loop()) {
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

/**
 * Print usage information
 */
static void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s [OPTIONS]\n\n", prog_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -c FILE    Path to configuration file (default: %s)\n", DEFAULT_CONFIG_FILE);
    fprintf(stderr, "  -v         Verbose mode\n");
    fprintf(stderr, "  -d         Run as daemon\n");
    fprintf(stderr, "  -t SECONDS Startup timeout in seconds (default: 60)\n");
    fprintf(stderr, "  -h         Show this help message\n");
}

/**
 * Signal handler
 */
static void signal_handler(int sig) {
    switch (sig) {
        case SIGINT:
        case SIGTERM:
            log_message(LOG_INFO, "Received signal %d, shutting down", sig);
            fsmonitor_signal_exit();  /* Signal exit through kqueue */
            break;
        case SIGHUP:
            log_message(LOG_INFO, "Received SIGHUP, reloading configuration");
            fsmonitor_signal_reload();  /* Signal reload through kqueue */
            break;
    }
}

/**
 * Daemonize the process
 */
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
        max_fd = 1024;  /* Default if sysconf fails */
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

/**
 * Clean up all components
 */
static void cleanup(void) {
    fsmonitor_cleanup();
    events_cleanup();
    plexapi_cleanup();
    config_free();
}

/**
 * Log message to file and/or stdout
 */
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