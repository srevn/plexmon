#include "events.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "logger.h"
#include "monitor.h"
#include "plexapi.h"

/* Array of pending scans */
static pending_t pending[MAX_EVENT_FDS];
static int num_pending = 0;

/* Initialize event processor */
bool events_init(void) {
	log_message(LOG_INFO, "Initializing event processor");

	/* Reset pending scans */
	memset(pending, 0, sizeof(pending));
	num_pending = 0;

	return true;
}

/* Clean up event processor */
void events_cleanup(void) {
	log_message(LOG_INFO, "Cleaning up event processor");
	num_pending = 0;
}

/* Find a pending scan by path */
static int pending_find(const char *path) {
	for (int i = 0; i < num_pending; i++) {
		if (pending[i].is_pending && strcmp(pending[i].path, path) == 0) {
			return i;
		}
	}
	return -1;
}

/* Find a pending scan for a parent directory */
static int pending_parent(const char *path) {
	size_t path_len = strlen(path);

	for (int i = 0; i < num_pending; i++) {
		if (!pending[i].is_pending) {
			continue;
		}

		size_t parent_len = strlen(pending[i].path);

		/* Check if this is a parent directory of our path */
		if (parent_len < path_len &&
			strncmp(pending[i].path, path, parent_len) == 0 &&
			(path[parent_len] == '/' || path[parent_len] == '\0')) {
			return i;
		}
	}
	return -1;
}

/* Find pending scans for child directories of the given path */
static void pending_child(const char *path, int *child_indices, int *num_children, int max_children) {
	size_t path_len = strlen(path);
	*num_children = 0;

	for (int i = 0; i < num_pending && *num_children < max_children; i++) {
		if (!pending[i].is_pending) {
			continue;
		}

		size_t child_len = strlen(pending[i].path);

		/* Check if this is a child directory of our path */
		if (path_len < child_len &&
			strncmp(path, pending[i].path, path_len) == 0 &&
			(pending[i].path[path_len] == '/' || pending[i].path[path_len] == '\0')) {
			child_indices[*num_children] = i;
			(*num_children)++;
		}
	}
}

/* Remove completed scans from the array */
static void pending_cleanup(void) {
	int i, j;

	/* Compact the array by removing completed scans */
	for (i = 0, j = 0; i < num_pending; i++) {
		if (pending[i].is_pending) {
			if (i != j) {
				pending[j] = pending[i];
			}
			j++;
		}
	}

	/* Update count */
	num_pending = j;
}

/* Handle a file system event */
void events_handle(const char *path, int section_id) {
	int idx, parent_idx;
	time_t now = time(NULL);
	const int debounce_delay = g_config.scan_interval;

	/* First, check if there's already a pending scan for a parent directory */
	parent_idx = pending_parent(path);
	if (parent_idx >= 0) {
		/* Parent directory scan will cover this one, extend its delay */
		pending[parent_idx].scheduled_time = now + debounce_delay;
		log_message(LOG_DEBUG, "Event for %s covered by parent scan of %s",
					path, pending[parent_idx].path);
		return;
	}

	/* Check if there's already a pending scan for this exact path */
	idx = pending_find(path);

	if (idx >= 0) {
		/* Already scheduled, extend the delay to coalesce with new event */
		pending[idx].scheduled_time = now + debounce_delay;
		log_message(LOG_DEBUG, "Rescheduled scan for %s to coalesce with new event", path);
	} else {
		/* Check if this path is a parent of any pending scans */
		int child_indices[MAX_EVENT_FDS];
		int num_children = 0;

		pending_child(path, child_indices, &num_children, MAX_EVENT_FDS);

		if (num_children > 0) {
			/* This is a parent directory of one or more pending scans */
			log_message(LOG_DEBUG, "Path %s is parent of %d pending scans, consolidating", path, num_children);

			/* Create a new scan for this parent directory */
			if (num_pending >= MAX_EVENT_FDS) {
				/* Use one of the child slots */
				idx = child_indices[0];
			} else {
				idx = num_pending++;
			}

			/* Set up the parent scan */
			strncpy(pending[idx].path, path, PATH_MAX_LEN - 1);
			pending[idx].path[PATH_MAX_LEN - 1] = '\0';
			pending[idx].section_id = section_id;
			pending[idx].first_event_time = now;
			pending[idx].scheduled_time = now + debounce_delay;
			pending[idx].is_pending = true;

			/* Mark child scans as not pending (except the one we reused) */
			for (int i = 0; i < num_children; i++) {
				if (child_indices[i] != idx) {
					pending[child_indices[i]].is_pending = false;
					log_message(LOG_DEBUG, "Removed child scan %s in favor of parent %s",
								pending[child_indices[i]].path, path);
				}
			}

			log_message(LOG_DEBUG, "Scheduled new parent scan for %s (replaced %d child scans)",
						path, num_children);
		} else {
			/* New pending scan with no related existing scans */
			if (num_pending >= MAX_EVENT_FDS) {
				/* Find the oldest scheduled scan to replace */
				time_t oldest_time = now + 86400; /* Initialize with distant future */
				idx = 0;

				for (int i = 0; i < num_pending; i++) {
					if (pending[i].is_pending &&
						pending[i].scheduled_time < oldest_time) {
						oldest_time = pending[i].scheduled_time;
						idx = i;
					}
				}

				log_message(LOG_DEBUG, "Replacing oldest pending scan (%s) with new scan",
							pending[idx].path);
			} else {
				idx = num_pending++;
			}

			strncpy(pending[idx].path, path, PATH_MAX_LEN - 1);
			pending[idx].path[PATH_MAX_LEN - 1] = '\0';
			pending[idx].section_id = section_id;
			pending[idx].first_event_time = now;
			pending[idx].scheduled_time = now + debounce_delay;
			pending[idx].is_pending = true;

			log_message(LOG_DEBUG, "Scheduled new scan for %s", path);
		}
	}
}

/* Process any pending scans that are due */
void events_pending(void) {
	time_t now = time(NULL);
	bool scans_executed = false;

	for (int i = 0; i < num_pending; i++) {
		if (pending[i].is_pending && now >= pending[i].scheduled_time) {
			/* Time to execute this scan */
			log_message(LOG_INFO, "Executing scan for %s (scanning delayed for %lds)",
						pending[i].path, now - pending[i].first_event_time);

			plexapi_scan(pending[i].path, pending[i].section_id);

			/* Mark as completed */
			pending[i].is_pending = false;
			scans_executed = true;
		}
	}

	/* Only clean up if we executed scans */
	if (scans_executed) {
		pending_cleanup();
	}
}

/* Get time until next scheduled scan */
time_t events_schedule(void) {
	time_t next_time = 0;
	time_t now = time(NULL);

	for (int i = 0; i < num_pending; i++) {
		if (pending[i].is_pending && pending[i].scheduled_time > now) {
			if (next_time == 0 || pending[i].scheduled_time < next_time) {
				next_time = pending[i].scheduled_time;
			}
		}
	}
	return next_time;
}

/* Calculate the timeout for the next scan */
void calculate_timeout(time_t next_scan, struct timespec *timeout) {
	time_t now = time(NULL);
	time_t time_left = next_scan > now ? next_scan - now : 0;

	timeout->tv_sec = time_left;
	timeout->tv_nsec = 0;
}
