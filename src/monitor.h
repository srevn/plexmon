#ifndef MONITOR_H
#define MONITOR_H

#include <signal.h>
#include <stdbool.h>
#include <sys/types.h>

#include "config.h"

#define INITIAL_MONITOR_CAPACITY 256       /* Initial size for monitored directories array */
#define USER_EVENT_EXIT 1                  /* User event identifier for exit signal */
#define USER_EVENT_RELOAD 2                /* User event identifier for reload signal */

/* Global variables */
extern uintptr_t user_event;               /* Global user event identifier for kqueue */
extern volatile sig_atomic_t g_running;    /* Global running flag for signal safety */

/* Structure to hold a monitored directory */
typedef struct {
	int fd;                                /* File descriptor for kqueue monitoring */
	const char *path;                      /* Full path to the monitored directory */
	int section_id;                        /* Associated Plex library section ID */
	dev_t device;                          /* Device ID for path validation */
	ino_t inode;                           /* Inode number for path validation */
	int next_free;                         /* For free-list management of the directories array */
} monitored_dir_t;

/* Monitor lifecycle management */
bool monitor_init(void);
void monitor_cleanup(void);
void monitor_exit(void);

/* Monitor control operations */
bool monitor_loop(void);
void monitor_process(void);
void monitor_reload(void);
int monitor_get_kqueue_fd(void);

/* Directory management */
int monitor_add(const char *path, int section_id);
void monitor_remove(int index);
int monitor_count(void);
bool monitor_validate(const char *path);
bool monitor_tree(const char *dir_path, int section_id);

/* Scan operations */
int monitor_scan(const char *dir_path, int section_id);

#endif /* MONITOR_H */