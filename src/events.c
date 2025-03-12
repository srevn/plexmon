/**
 * events.c - Event processing and debouncing
 */

#include "plexmon.h"
#include <time.h>

/* Structure to track recent events */
typedef struct {
    char path[PATH_MAX_LEN];
    int section_id;
    time_t last_scan_time;
} recent_event_t;

/* Array of recent events to implement debouncing */
static recent_event_t recent_events[MAX_EVENT_FDS];
static int num_recent_events = 0;

/* Initialize event processor */
bool events_init(void) {
    log_message(LOG_INFO, "Initializing event processor");
    
    /* Reset recent events */
    memset(recent_events, 0, sizeof(recent_events));
    num_recent_events = 0;
    
    return true;
}

/* Clean up event processor */
void events_cleanup(void) {
    log_message(LOG_INFO, "Cleaning up event processor");
    
    /* No additional cleanup needed for now */
    num_recent_events = 0;
}

/* Find a recent event by path */
static int find_recent_event(const char *path) {
    for (int i = 0; i < num_recent_events; i++) {
        if (strcmp(recent_events[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

/* Handle a file system event */
void events_handle(const char *path, int section_id) {
    int idx;
    time_t now = time(NULL);
    
    /* Look for existing event for this path */
    idx = find_recent_event(path);
    
    if (idx >= 0) {
        /* Event exists, check if we need to debounce */
        if (now - recent_events[idx].last_scan_time < g_config.scan_interval) {
            log_message(LOG_DEBUG, "Debouncing event for %s (%lds ago)", 
                       path, (now - recent_events[idx].last_scan_time));
            return;
        }
        
        /* Update last scan time */
        recent_events[idx].last_scan_time = now;
        
    } else {
        /* New event, add to recent events */
        if (num_recent_events >= MAX_EVENT_FDS) {
            /* Find the oldest event */
            time_t oldest_time = now;
            idx = 0;
            
            for (int i = 0; i < num_recent_events; i++) {
                if (recent_events[i].last_scan_time < oldest_time) {
                    oldest_time = recent_events[i].last_scan_time;
                    idx = i;
                }
            }
            
            log_message(LOG_DEBUG, "Replacing oldest event (%s) with new event", 
                       recent_events[idx].path);
        } else {
            idx = num_recent_events++;
        }
        
        strncpy(recent_events[idx].path, path, PATH_MAX_LEN - 1);
        recent_events[idx].path[PATH_MAX_LEN - 1] = '\0';
        recent_events[idx].section_id = section_id;
        recent_events[idx].last_scan_time = now;
    }
    
    /* Trigger Plex scan */
    plexapi_trigger_scan(path, section_id);
}