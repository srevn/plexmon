/**
 * plexmon.h - Main header file for plexmon application
 * 
 * A FreeBSD application that monitors filesystem changes in Plex Media Server
 * library directories and triggers partial library scans.
 */

#ifndef PLEXMON_H
#define PLEXMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <syslog.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <dirent.h>

/* Version information */
#define PLEXMON_VERSION "1.0.0"

/* Default configuration values */
#define DEFAULT_CONFIG_FILE "/usr/local/etc/plexmon.conf"
#define DEFAULT_PLEX_URL "http://localhost:32400"
#define DEFAULT_LOG_FILE "/var/log/plexmon.log"
#define DEFAULT_LOG_LEVEL LOG_INFO

/* Maximum number of simultaneous events to monitor */
#define MAX_EVENT_FDS 2048

/* Hash table for directory cache */
#define HASH_TABLE_SIZE 2048 

/* Maximum length for paths */
#define PATH_MAX_LEN 1024

/* Maximum length for Plex token */
#define TOKEN_MAX_LEN 128

/* Structure to hold a monitored directory */
typedef struct {
    int fd;                   /* File descriptor */
    char path[PATH_MAX_LEN];  /* Path to the directory */
    int plex_section_id;      /* Associated Plex library section ID */
} monitored_dir_t;

/* Structure to hold configuration */
typedef struct {
    char plex_url[PATH_MAX_LEN];
    char plex_token[TOKEN_MAX_LEN];
    char *directories[MAX_EVENT_FDS];  /* List of directories to monitor */
    char log_file[PATH_MAX_LEN];
    int startup_timeout;     /* Maximum time to wait for Plex in seconds */
    int num_directories;
    int log_level;          /* Log level threshold */
    bool verbose;           /* Verbose output flag */
    bool daemonize;         /* Run as daemon */
} config_t;

/* Global configuration */
extern config_t g_config;

/* Global log file handle */
extern FILE *g_log_file;

/* Global running flag */
extern volatile sig_atomic_t g_running;

/* Function prototypes */
/* Configuration management */
bool config_load(const char *config_path);
void config_free(void);

/* Filesystem monitoring */
bool fsmonitor_init(void);
bool fsmonitor_run_loop(void);
void fsmonitor_process_events(void);
void fsmonitor_cleanup(void);
void fsmonitor_signal_exit(void);
void fsmonitor_signal_reload(void);
int fsmonitor_get_kqueue_fd(void);
int fsmonitor_add_directory(const char *path, int plex_section_id);
int get_monitored_dir_count(void);

/* Plex API */
bool plexapi_init(void);
void plexapi_cleanup(void);
bool plexapi_get_libraries(void);
bool plexapi_trigger_scan(const char *path, int section_id);

/* Event processing */
bool events_init(void);
void events_cleanup(void);
void events_handle(const char *path, int section_id);
void events_process_pending(void);
time_t next_scheduled_scan(void);

/* Logging */
bool log_init(void);
void log_cleanup(void);
void log_message(int priority, const char *format, ...);

/* Directory cache */
bool dircache_init(void);
void dircache_cleanup(void);
bool dircache_refresh(const char *path, bool *changed);
char **dircache_get_subdirs(const char *path, int *count);
void dircache_free_subdirs(char **subdirs, int count);

/* Utilities */
bool is_directory(const char *path);
bool watch_directory_tree(const char *dir_path, int section_id);
int scan_new_directories(const char *dir_path, int section_id);

#endif /* PLEXMON_H */