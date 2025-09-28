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
#include "khash.h"
#include "logger.h"
#include "queue.h"
#include "utilities.h"

KHASH_MAP_INIT_STR(mon_dir, int)               /* Hash map from string to monitored_dir_t index */

/* Global variables */
uintptr_t g_user_event_ident = 0;              /* Global user event identifier */

/* Static variables for monitor implementation */
#define INITIAL_MONITOR_CAPACITY 256
static monitored_dir_t *monitored_dirs = NULL; /* Dynamic array of monitored directories */
static int monitored_dirs_capacity = 0;		   /* Current capacity of the array */
static int active_dir_count = 0;			   /* Number of active directories */
static int free_list_head = -1;				   /* Head of the free list for empty slots */
static khash_t(mon_dir) * monitored_dirs_htab; /* Hash table for fast path lookups */
static int g_kqueue_fd = -1;				   /* Global kqueue descriptor */

/* Initialize file system monitoring */
bool monitor_init(void) {
	log_message(LOG_INFO, "Initializing file system monitoring");

	/* Allocate and initialize the dynamic array for monitored directories */
	monitored_dirs_capacity = INITIAL_MONITOR_CAPACITY;
	monitored_dirs = malloc(monitored_dirs_capacity * sizeof(monitored_dir_t));
	if (!monitored_dirs) {
		log_message(LOG_ERR, "Failed to allocate memory for monitored directories");
		return false;
	}

	/* Build the free list */
	for (int i = 0; i < monitored_dirs_capacity; i++) {
		monitored_dirs[i].fd = -1;
		monitored_dirs[i].next_free = (i + 1);
	}
	monitored_dirs[monitored_dirs_capacity - 1].next_free = -1; /* End of list */
	free_list_head = 0;
	active_dir_count = 0;

	/* Create kqueue */
	g_kqueue_fd = kqueue();
	if (g_kqueue_fd == -1) {
		log_message(LOG_ERR, "Failed to create kqueue: %s", strerror(errno));
		free(monitored_dirs);
		monitored_dirs = NULL;
		return false;
	}

	/* Initialize hash table for path lookups */
	monitored_dirs_htab = kh_init(mon_dir);
	if (!monitored_dirs_htab) {
		log_message(LOG_ERR, "Failed to create monitored dirs hash table");
		close(g_kqueue_fd);
		g_kqueue_fd = -1;
		free(monitored_dirs);
		monitored_dirs = NULL;
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
		kh_destroy(mon_dir, monitored_dirs_htab);
		monitored_dirs_htab = NULL;
		free(monitored_dirs);
		monitored_dirs = NULL;
		return false;
	}

	log_message(LOG_INFO, "Kqueue created successfully with descriptor %d", g_kqueue_fd);
	return true;
}

/* Clean up file system monitoring */
void monitor_cleanup(void) {
	log_message(LOG_INFO, "Cleaning up file system monitoring");

	if (monitored_dirs) {
		/* Close all file descriptors */
		for (int i = 0; i < monitored_dirs_capacity; i++) {
			if (monitored_dirs[i].fd >= 0) {
				close(monitored_dirs[i].fd);
				monitored_dirs[i].fd = -1;
			}
		}
	}

	/* Close kqueue */
	if (g_kqueue_fd != -1) {
		close(g_kqueue_fd);
		g_kqueue_fd = -1;
	}

	/* Destroy the hash table */
	if (monitored_dirs_htab) {
		khint_t k;
		for (k = kh_begin(monitored_dirs_htab); k != kh_end(monitored_dirs_htab); ++k) {
			if (kh_exist(monitored_dirs_htab, k)) {
				free((void *) kh_key(monitored_dirs_htab, k));
			}
		}
		kh_destroy(mon_dir, monitored_dirs_htab);
		monitored_dirs_htab = NULL;
	}

	/* Free the dynamic array */
	if (monitored_dirs) {
		free(monitored_dirs);
		monitored_dirs = NULL;
	}

	monitored_dirs_capacity = 0;
	active_dir_count = 0;
	free_list_head = -1;
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
	return active_dir_count;
}

/* Helper function to find a monitored directory by its path */
static int find_monitored_dir_by_path(const char *path) {
	if (!monitored_dirs_htab) {
		return -1;
	}

	khint_t k = kh_get(mon_dir, monitored_dirs_htab, path);
	if (k != kh_end(monitored_dirs_htab)) {
		int index = kh_value(monitored_dirs_htab, k);
		/* Check if the found dir is active */
		if (index >= 0 && index < monitored_dirs_capacity && monitored_dirs[index].fd != -1) {
			return index;
		}
	}

	return -1;
}

/* Remove a directory from the monitoring list by marking it as inactive */
void monitor_remove(int index) {
	if (index < 0 || index >= monitored_dirs_capacity) {
		return;
	}

	monitored_dir_t *dir = &monitored_dirs[index];

	/* Close file descriptor if valid */
	if (dir->fd >= 0) {
		log_message(LOG_DEBUG, "Removing directory %s from monitoring", dir->path);
		close(dir->fd);
		dir->fd = -1; /* Mark as inactive */

		/* Remove from hash table */
		if (monitored_dirs_htab) {
			khint_t k = kh_get(mon_dir, monitored_dirs_htab, dir->path);
			if (k != kh_end(monitored_dirs_htab)) {
				free((void *) kh_key(monitored_dirs_htab, k));
				kh_del(mon_dir, monitored_dirs_htab, k);
			}
		}

		/* Add to free list */
		dir->next_free = free_list_head;
		free_list_head = index;
		active_dir_count--;
	}
}

/* Helper function to check if a directory is already monitored and still valid */
bool monitor_validate(const char *path) {
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
	int index = (int) (dir_info - monitored_dirs);

	/* Set up the kevent structure for this directory */
	EV_SET(&change, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR | EV_ENABLE,
		   NOTE_WRITE | NOTE_RENAME | NOTE_DELETE | NOTE_EXTEND, 0, (void *) (intptr_t) index);

	/* Register event with kqueue */
	if (kevent(g_kqueue_fd, &change, 1, NULL, 0, NULL) == -1) {
		log_message(LOG_ERR, "Error registering directory %s with kqueue: %s",
					dir_info->path, strerror(errno));
		return false;
	}

	return true;
}

/* Add a directory to the monitoring list */
int monitor_add(const char *path, int section_id) {
	if (monitor_validate(path)) {
		log_message(LOG_DEBUG, "Directory %s is already being monitored and is valid", path);
		return find_monitored_dir_by_path(path);
	}

	/* If no free slots, resize the array */
	if (free_list_head == -1) {
		int old_capacity = monitored_dirs_capacity;
		int new_capacity = (old_capacity > 0) ? (old_capacity * 2) : INITIAL_MONITOR_CAPACITY;
		monitored_dir_t *new_dirs = realloc(monitored_dirs, new_capacity * sizeof(monitored_dir_t));

		if (!new_dirs) {
			log_message(LOG_ERR, "Failed to resize monitored directories array");
			return -1;
		}
		monitored_dirs = new_dirs;
		monitored_dirs_capacity = new_capacity;

		/* Rebuild the free list for the new portion */
		for (int i = old_capacity; i < new_capacity; i++) {
			monitored_dirs[i].fd = -1;
			monitored_dirs[i].next_free = (i + 1);
		}
		monitored_dirs[new_capacity - 1].next_free = -1;
		free_list_head = old_capacity;
		log_message(LOG_DEBUG, "Resized monitored directories to %d", new_capacity);
	}

	/* Get a free slot from the head of the list */
	int new_index = free_list_head;
	monitored_dir_t *new_dir = &monitored_dirs[new_index];
	free_list_head = new_dir->next_free;

	/* Open directory */
	int fd = open(path, O_RDONLY);
	if (fd == -1) {
		log_message(LOG_ERR, "Failed to open directory %s: %s", path, strerror(errno));
		new_dir->next_free = free_list_head;
		free_list_head = new_index; /* Return slot to free list */
		return -1;
	}

	/* Get directory stats for validation */
	struct stat dir_stat;
	if (fstat(fd, &dir_stat) == -1) {
		log_message(LOG_ERR, "Failed to stat directory %s: %s", path, strerror(errno));
		close(fd);
		new_dir->next_free = free_list_head;
		free_list_head = new_index; /* Return slot to free list */
		return -1;
	}

	/* Add to hash table for fast lookups */
	char *key = strdup(path);
	if (!key) {
		log_message(LOG_ERR, "Failed to allocate memory for hash table key");
		close(fd);
		new_dir->next_free = free_list_head;
		free_list_head = new_index; /* Return slot to free list */
		return -1;
	}

	int ret;
	khint_t k = kh_put(mon_dir, monitored_dirs_htab, key, &ret);
	if (ret == -1) {
		log_message(LOG_ERR, "Failed to add directory to hash table");
		free(key);
		close(fd);
		new_dir->next_free = free_list_head;
		free_list_head = new_index; /* Return slot to free list */
		return -1;
	}

	/* Add to monitored directories array */
	new_dir->fd = fd;
	new_dir->path = kh_key(monitored_dirs_htab, k);
	new_dir->section_id = section_id;
	new_dir->device = dir_stat.st_dev;
	new_dir->inode = dir_stat.st_ino;
	kh_value(monitored_dirs_htab, k) = new_index;

	/* Register with kqueue */
	if (!monitor_register(fd, new_dir)) {
		/* If registration fails, we need to undo the add */
		free((void *) kh_key(monitored_dirs_htab, k));
		kh_del(mon_dir, monitored_dirs_htab, k);
		close(fd);
		new_dir->fd = -1;
		new_dir->next_free = free_list_head;
		free_list_head = new_index; /* Return slot to free list */
		return -1;
	}

	active_dir_count++;
	return new_index;
}

/* Handle directory events */
static void monitor_event(monitored_dir_t *md, int fflags) {
	log_message(LOG_INFO, "Change detected in directory: %s (flags: 0x%x)", md->path, fflags);

	/* Check for new subdirectories that need to be monitored */
	if (!is_directory(md->path, D_TYPE_UNAVAILABLE)) {
		events_handle(md->path, md->section_id);
		return;
	}

	bool dir_changed = false;

	/* Directory cache with mtime checking */
	if (dircache_refresh(md->path, &dir_changed)) {
		if (dir_changed) {
			log_message(LOG_DEBUG, "Directory structure changed in %s, detecting new subdirectories", md->path);
			/* Register new subdirectories */
			int new_dirs = monitor_scan(md->path, md->section_id);
			log_message(LOG_DEBUG, "Registered %d new directories under %s", new_dirs, md->path);
		} else {
			/* Still queue a Plex scan but skip directory tree rescanning */
			log_message(LOG_DEBUG, "File change detected in %s, triggering Plex scan without directory rescan", md->path);
		}
	} else {
		/* Cache check failed, fall back to targeted refresh */
		log_message(LOG_WARNING, "Failed to check directory cache for %s, using targeted refresh", md->path);
		monitor_scan(md->path, md->section_id);
	}

	/* Queue event */
	events_handle(md->path, md->section_id);
}

/* Process events from kqueue */
void monitor_process(void) {
	struct kevent events[INITIAL_MONITOR_CAPACITY];
	struct timespec timeout;
	int nev;

	calculate_timeout(events_schedule(), &timeout);

	/* Indefinite wait if no scans and no events */
	nev = kevent(g_kqueue_fd, NULL, 0, events, INITIAL_MONITOR_CAPACITY,
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

		if (events[i].flags & EV_ERROR) {
			log_message(LOG_ERR, "Event error: %s", strerror(events[i].data));
			continue;
		}

		int md_idx = (int) (intptr_t) events[i].udata;
		if (md_idx >= 0 && md_idx < monitored_dirs_capacity) {
			monitored_dir_t *md = &monitored_dirs[md_idx];

			/* Ensure the directory wasn't removed while the event was pending */
			if (md && md->fd >= 0 && events[i].fflags) {
				monitor_event(md, events[i].fflags);
			}
		} else {
			log_message(LOG_WARNING, "Received event for invalid directory index: %d", md_idx);
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
			/* Skip if already monitored and valid */
			if (monitor_validate(subdirs[i])) {
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

		/* Add current directory to monitoring if not already monitored and valid */
		if (!monitor_validate(current_path)) {
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
