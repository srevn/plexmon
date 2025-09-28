#include "monitor.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "config.h"
#include "dircache.h"
#include "events.h"
#include "logger.h"
#include "queue.h"
#include "utilities.h"

/* Global user event identifier */
uintptr_t g_user_event_ident = 0;

/* Array of monitored directories */
static monitored_dir_t monitored_dirs[MAX_EVENT_FDS];
static int num_monitored_dirs = 0;

/* Global kqueue descriptor */
static int g_kqueue_fd = -1;

/* Initialize file system monitoring */
bool monitor_init(void) {
	log_message(LOG_INFO, "Initializing file system monitoring");

	/* Reset monitored directories */
	memset(monitored_dirs, 0, sizeof(monitored_dirs));
	for (int i = 0; i < MAX_EVENT_FDS; i++) {
		monitored_dirs[i].fd = -1; /* Initialize with invalid fd */
	}
	num_monitored_dirs = 0;

	/* Create kqueue */
	g_kqueue_fd = kqueue();
	if (g_kqueue_fd == -1) {
		log_message(LOG_ERR, "Failed to create kqueue: %s", strerror(errno));
		return false;
	}

	/* Set up user event for clean wake-up */
	struct kevent kev;
	g_user_event_ident = getpid(); /* Use PID as the identifier */

	EV_SET(&kev, g_user_event_ident, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
	if (kevent(g_kqueue_fd, &kev, 1, NULL, 0, NULL) == -1) {
		log_message(LOG_ERR, "Failed to register user event: %s", strerror(errno));
		close(g_kqueue_fd);
		g_kqueue_fd = -1;
		return false;
	}

	log_message(LOG_INFO, "Kqueue created successfully with descriptor %d", g_kqueue_fd);
	return true;
}

/* Clean up file system monitoring */
void monitor_cleanup(void) {
	log_message(LOG_INFO, "Cleaning up file system monitoring");

	/* Close all file descriptors */
	for (int i = 0; i < num_monitored_dirs; i++) {
		if (monitored_dirs[i].fd >= 0) {
			close(monitored_dirs[i].fd);
			monitored_dirs[i].fd = -1;
		}
	}

	/* Close kqueue */
	if (g_kqueue_fd != -1) {
		close(g_kqueue_fd);
		g_kqueue_fd = -1;
	}

	num_monitored_dirs = 0;
}

/* Signal to the event loop to exit */
void monitor_exit(void) {
	struct kevent kev;

	if (g_kqueue_fd == -1) return;

	log_message(LOG_INFO, "Sending exit signal to event loop");

	/* Set up and trigger the user event for exit */
	EV_SET(&kev, g_user_event_ident, EVFILT_USER, EV_ENABLE, NOTE_TRIGGER, USER_EVENT_EXIT, NULL);

	if (kevent(g_kqueue_fd, &kev, 1, NULL, 0, NULL) == -1) {
		log_message(LOG_ERR, "Failed to signal exit event: %s", strerror(errno));
	}
}

/* Signal to the event loop to reload configuration */
void monitor_reload(void) {
	struct kevent kev;

	if (g_kqueue_fd == -1) return;

	log_message(LOG_INFO, "Sending reload signal to event loop");

	/* Set up and trigger the user event for reload */
	EV_SET(&kev, g_user_event_ident, EVFILT_USER, EV_ENABLE, NOTE_TRIGGER, USER_EVENT_RELOAD, NULL);

	if (kevent(g_kqueue_fd, &kev, 1, NULL, 0, NULL) == -1) {
		log_message(LOG_ERR, "Failed to signal reload event: %s", strerror(errno));
	}
}

/* Get the kqueue file descriptor */
int monitor_get_kqueue_fd(void) {
	return g_kqueue_fd;
}

/* Return the current count of monitored directories */
int monitor_count(void) {
	return num_monitored_dirs;
}

/* Helper function to find a monitored directory by its path */
static int find_monitored_dir_by_path(const char *path) {
	for (int i = 0; i < num_monitored_dirs; i++) {
		if (strcmp(monitored_dirs[i].path, path) == 0) {
			return i;
		}
	}
	return -1;
}

/* Helper function to check if directory is already being monitored */
bool is_directory_monitored(const char *path) {
	int index = find_monitored_dir_by_path(path);
	if (index == -1) {
		return false;
	}

	monitored_dir_t *dir = &monitored_dirs[index];

	/* Verify the directory still exists and is the same */
	struct stat path_stat;
	if (dir->fd >= 0 && stat(path, &path_stat) == 0 &&
		path_stat.st_dev == dir->device && path_stat.st_ino == dir->inode) {
		return true;
	}

	/* Directory was deleted/recreated or fd is invalid, remove from monitoring */
	monitor_remove(index);
	return false;
}

/* Register a directory with kqueue */
static bool monitor_register(int fd, monitored_dir_t *dir_info) {
	struct kevent change;

	/* Set up the kevent structure for this directory */
	EV_SET(&change, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR | EV_ENABLE,
		   NOTE_WRITE | NOTE_RENAME | NOTE_DELETE | NOTE_EXTEND, 0, dir_info);

	/* Register event with kqueue */
	if (kevent(g_kqueue_fd, &change, 1, NULL, 0, NULL) == -1) {
		log_message(LOG_ERR, "Error registering directory %s with kqueue: %s",
					dir_info->path, strerror(errno));
		return false;
	}

	return true;
}

/* Remove a directory from the monitoring list */
void monitor_remove(int index) {
	if (index < 0 || index >= num_monitored_dirs) {
		return;
	}

	/* Close file descriptor if valid */
	if (monitored_dirs[index].fd >= 0) {
		close(monitored_dirs[index].fd);
	}

	/* Shift remaining entries down */
	for (int i = index; i < num_monitored_dirs - 1; i++) {
		monitored_dirs[i] = monitored_dirs[i + 1];
	}

	/* Clear the last entry */
	memset(&monitored_dirs[num_monitored_dirs - 1], 0, sizeof(monitored_dir_t));
	monitored_dirs[num_monitored_dirs - 1].fd = -1;

	num_monitored_dirs--;
}

/* Add a directory to the monitoring list */
int monitor_add(const char *path, int plex_section_id) {
	if (num_monitored_dirs >= MAX_EVENT_FDS) {
		log_message(LOG_ERR, "Maximum number of monitored directories reached");
		return -1;
	}

	/* Check if the directory is already being monitored */
	int index = find_monitored_dir_by_path(path);
	if (index != -1) {
		/* It's in our list. Verify if it's still the same directory. */
		struct stat path_stat;
		monitored_dir_t *dir = &monitored_dirs[index];

		if (dir->fd >= 0 && stat(path, &path_stat) == 0 &&
			path_stat.st_dev == dir->device && path_stat.st_ino == dir->inode) {
			log_message(LOG_DEBUG, "Directory %s is already being monitored", path);
			return index;
		} else {
			/* Stale entry. Remove it before adding the new one. */
			log_message(LOG_DEBUG, "Removing stale monitor for path %s before re-adding", path);
			monitor_remove(index);
		}
	}

	/* Open directory */
	int fd = open(path, O_RDONLY);
	if (fd == -1) {
		log_message(LOG_ERR, "Failed to open directory %s: %s", path, strerror(errno));
		return -1;
	}

	/* Get directory stats for validation */
	struct stat dir_stat;
	if (fstat(fd, &dir_stat) == -1) {
		log_message(LOG_ERR, "Failed to stat directory %s: %s", path, strerror(errno));
		close(fd);
		return -1;
	}

	/* Add to monitored directories */
	monitored_dirs[num_monitored_dirs].fd = fd;
	strncpy(monitored_dirs[num_monitored_dirs].path, path, PATH_MAX_LEN - 1);
	monitored_dirs[num_monitored_dirs].path[PATH_MAX_LEN - 1] = '\0';
	monitored_dirs[num_monitored_dirs].plex_section_id = plex_section_id;
	monitored_dirs[num_monitored_dirs].device = dir_stat.st_dev;
	monitored_dirs[num_monitored_dirs].inode = dir_stat.st_ino;

	/* Register with kqueue */
	if (!monitor_register(fd, &monitored_dirs[num_monitored_dirs])) {
		close(fd);
		return -1;
	}

	return num_monitored_dirs++;
}

/* Handle directory events */
static void monitor_event(monitored_dir_t *md, int fflags) {
	log_message(LOG_INFO, "Change detected in directory: %s (flags: 0x%x)", md->path, fflags);

	/* Check for new subdirectories that need to be monitored */
	if (!is_directory(md->path)) {
		events_handle(md->path, md->plex_section_id);
		return;
	}

	bool dir_changed = false;

	/* Directory cache with mtime checking */
	if (dircache_refresh(md->path, &dir_changed)) {
		if (dir_changed) {
			log_message(LOG_DEBUG, "Directory structure changed in %s, detecting new subdirectories", md->path);
			/* Register new subdirectories */
			int new_dirs = monitor_scan(md->path, md->plex_section_id);
			log_message(LOG_DEBUG, "Registered %d new directories under %s", new_dirs, md->path);
		} else {
			/* Still queue a Plex scan but skip directory tree rescanning */
			log_message(LOG_DEBUG, "File change detected in %s, triggering Plex scan without directory rescan", md->path);
		}
	} else {
		/* Cache check failed, fall back to targeted refresh */
		log_message(LOG_WARNING, "Failed to check directory cache for %s, using targeted refresh", md->path);
		monitor_scan(md->path, md->plex_section_id);
	}

	/* Queue event */
	events_handle(md->path, md->plex_section_id);
}

/* Process events from kqueue */
void monitor_process(void) {
	struct kevent events[MAX_EVENT_FDS];
	struct timespec timeout;
	int nev;

	calculate_timeout(events_schedule(), &timeout);

	/* Indefinite wait if no scans and no events */
	nev = kevent(g_kqueue_fd, NULL, 0, events, MAX_EVENT_FDS,
				 (timeout.tv_sec == 0 && timeout.tv_nsec == 0) ? NULL : &timeout);

	if (nev == -1) {
		if (errno != EINTR) {
			log_message(LOG_ERR, "Error in kevent: %s", strerror(errno));
		}
		return;
	}

	/* Process received events */
	for (int i = 0; i < nev; i++) {
		/* Check for user events */
		if (events[i].filter == EVFILT_USER && events[i].ident == g_user_event_ident) {
			uint32_t data = events[i].data;

			if (data == USER_EVENT_EXIT) {
				g_running = 0; /* Signal to exit the main loop */
				log_message(LOG_INFO, "Received exit event");
			} else if (data == USER_EVENT_RELOAD) {
				log_message(LOG_INFO, "Received reload event, reloading configuration");
				config_load(DEFAULT_CONFIG_FILE);
			}
			continue;
		}

		monitored_dir_t *md = (monitored_dir_t *) events[i].udata;

		if (events[i].flags & EV_ERROR) {
			log_message(LOG_ERR, "Event error: %s", strerror(events[i].data));
			continue;
		}

		if (md && events[i].fflags) {
			monitor_event(md, events[i].fflags);
		}
	}

	/* Process any pending scans that are ready */
	events_pending();
}

/* Run the filesystem event monitor loop */
bool monitor_loop(void) {
	if (g_kqueue_fd == -1) {
		log_message(LOG_ERR, "Invalid kqueue descriptor");
		return false;
	}

	/* Process events until g_running becomes false */
	g_running = 1;
	while (g_running) {
		monitor_process();
	}

	return true;
}

/* Detect and register new subdirectories */
int monitor_scan(const char *dir_path, int section_id) {
	queue_t queue;
	char current_path[PATH_MAX_LEN];
	int new_dirs_count = 0;

	/* Initialize queue */
	queue_init(&queue);

	/* Start with the initial directory */
	if (!queue_enqueue(&queue, dir_path)) {
		log_message(LOG_ERR, "Failed to allocate memory for directory queue");
		return 0;
	}

	log_message(LOG_DEBUG, "Detecting new subdirectories starting from %s", dir_path);

	/* Process directories from the queue */
	while (!queue_is_empty(&queue)) {
		if (!queue_dequeue(&queue, current_path)) {
			break; /* Should not happen */
		}

		/* Get subdirectories */
		int subdir_count = 0;
		char **subdirs = dircache_subdirs(current_path, &subdir_count);

		if (!subdirs) {
			continue; /* No subdirectories or error */
		}

		/* Check each subdirectory */
		for (int i = 0; i < subdir_count; i++) {
			/* Skip if already monitored */
			if (is_directory_monitored(subdirs[i])) {
				continue;
			}

			/* Add this new directory to monitoring */
			int dir_idx = monitor_add(subdirs[i], section_id);
			if (dir_idx >= 0) {
				new_dirs_count++;
				log_message(LOG_DEBUG, "Added new directory %s to monitoring", subdirs[i]);

				/* Add this directory to the queue for further processing */
				if (!queue_enqueue(&queue, subdirs[i])) {
					log_message(LOG_ERR, "Failed to allocate memory for directory queue");
					dircache_free(subdirs, subdir_count);
					queue_free(&queue);
					return new_dirs_count;
				}
			} else {
				log_message(LOG_WARNING, "Failed to add directory %s to monitoring", subdirs[i]);
			}
		}

		/* Free subdirectory list */
		dircache_free(subdirs, subdir_count);
	}

	/* Clean up queue */
	queue_free(&queue);

	if (new_dirs_count > 0) {
		log_message(LOG_INFO, "Added %d new directories under %s to monitoring",
					new_dirs_count, dir_path);
	}

	return new_dirs_count;
}

/* Recursively add a directory and its subdirectories to the watch list */
bool monitor_tree(const char *dir_path, int section_id) {
	queue_t queue;
	char current_path[PATH_MAX_LEN];

	/* Initialize queue */
	queue_init(&queue);

	/* Start with the initial directory */
	if (!queue_enqueue(&queue, dir_path)) {
		log_message(LOG_ERR, "Failed to allocate memory for directory queue");
		return false;
	}

	log_message(LOG_DEBUG, "Starting directory tree registration from %s", dir_path);

	/* Process directories from the queue */
	while (!queue_is_empty(&queue)) {
		if (!queue_dequeue(&queue, current_path)) {
			break; /* Should not happen */
		}

		/* Add current directory to monitoring if not already monitored */
		if (!is_directory_monitored(current_path)) {
			int dir_idx = monitor_add(current_path, section_id);
			if (dir_idx < 0) {
				log_message(LOG_WARNING, "Failed to add directory %s to monitoring", current_path);
				continue;
			}
			log_message(LOG_DEBUG, "Added directory %s to monitoring", current_path);
		}

		/* Get subdirectories */
		int subdir_count = 0;
		char **subdirs = dircache_subdirs(current_path, &subdir_count);

		if (!subdirs) {
			/* Cache miss, update it and try again */
			bool dir_changed = false;
			if (dircache_refresh(current_path, &dir_changed)) {
				subdirs = dircache_subdirs(current_path, &subdir_count);
			}

			if (!subdirs) {
				log_message(LOG_DEBUG, "No subdirectories found for %s", current_path);
				continue;
			}
		}

		/* Add all subdirectories to queue */
		for (int i = 0; i < subdir_count; i++) {
			if (!queue_enqueue(&queue, subdirs[i])) {
				log_message(LOG_ERR, "Failed to allocate memory for directory queue");
				dircache_free(subdirs, subdir_count);
				queue_free(&queue);
				return false;
			}
		}

		/* Free subdirectory list */
		dircache_free(subdirs, subdir_count);
	}

	/* Clean up queue */
	queue_free(&queue);

	return true;
}
