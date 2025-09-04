#ifndef EVENTS_H
#define EVENTS_H

#include <stdbool.h>
#include <time.h>

/* Event processing configuration */
#define PATH_MAX_LEN 1024                  /* Maximum length for filesystem paths */

/* Structure to track pending scan requests */
typedef struct pending {
	char path[PATH_MAX_LEN];               /* Path to scan when delay expires */
	int section_id;                        /* Associated Plex library section ID */
	time_t first_event_time;               /* Timestamp when first event was received */
	time_t scheduled_time;                 /* Timestamp when the scan is scheduled to run */
	bool is_pending;                       /* Whether this scan is still pending execution */
} pending_t;

/* Event processing lifecycle */
bool events_init(void);
void events_cleanup(void);

/* Event handling operations */
void events_handle(const char *path, int section_id);
void events_pending(void);

/* Event scheduling utilities */
time_t events_schedule(void);
void calculate_timeout(time_t next_scan, struct timespec *timeout);

#endif /* EVENTS_H */