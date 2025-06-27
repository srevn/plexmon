/**
 * events.c - Event processing with smart coalescing
 */

#include "plexmon.h"
#include <time.h>

/* Structure to track pending scan requests */
typedef struct {
    char path[PATH_MAX_LEN];       /* Path to scan */
    int section_id;                /* Plex section ID */
    time_t first_event_time;       /* When first event was received */
    time_t scheduled_scan_time;    /* When the scan is scheduled to run */
    bool is_pending;               /* Is this scan still pending execution? */
} pending_scan_t;

/* Array of pending scans */
static pending_scan_t pending_scans[MAX_EVENT_FDS];
static int num_pending_scans = 0;

/* Forward declarations */
static int find_pending_scan(const char *path);
static int find_parent_pending_scan(const char *path);
static void find_child_pending_scans(const char *path, int *child_indices, int *num_children, int max_children);
static void cleanup_completed_scans(void);

/* Initialize event processor */
bool events_init(void) {
    log_message(LOG_INFO, "Initializing event processor");
    
    /* Reset pending scans */
    memset(pending_scans, 0, sizeof(pending_scans));
    num_pending_scans = 0;
    
    return true;
}

/* Clean up event processor */
void events_cleanup(void) {
    log_message(LOG_INFO, "Cleaning up event processor");
    num_pending_scans = 0;
}

/* Find a pending scan by path */
static int find_pending_scan(const char *path) {
    for (int i = 0; i < num_pending_scans; i++) {
        if (pending_scans[i].is_pending && strcmp(pending_scans[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

/* Find a pending scan for a parent directory */
static int find_parent_pending_scan(const char *path) {
    size_t path_len = strlen(path);
    
    for (int i = 0; i < num_pending_scans; i++) {
        if (!pending_scans[i].is_pending) {
            continue;
        }
        
        size_t parent_len = strlen(pending_scans[i].path);
        
        /* Check if this is a parent directory of our path */
        if (parent_len < path_len && 
            strncmp(pending_scans[i].path, path, parent_len) == 0 && 
            (path[parent_len] == '/' || path[parent_len] == '\0')) {
            return i;
        }
    }
    return -1;
}

/* Find pending scans for child directories of the given path */
static void find_child_pending_scans(const char *path, int *child_indices, int *num_children, int max_children) {
    size_t path_len = strlen(path);
    *num_children = 0;
    
    for (int i = 0; i < num_pending_scans && *num_children < max_children; i++) {
        if (!pending_scans[i].is_pending) {
            continue;
        }
        
        size_t child_len = strlen(pending_scans[i].path);
        
        /* Check if this is a child directory of our path */
        if (path_len < child_len && 
            strncmp(path, pending_scans[i].path, path_len) == 0 && 
            (pending_scans[i].path[path_len] == '/' || pending_scans[i].path[path_len] == '\0')) {
            child_indices[*num_children] = i;
            (*num_children)++;
        }
    }
}

/* Handle a file system event */
void events_handle(const char *path, int section_id) {
    int idx, parent_idx;
    time_t now = time(NULL);
    const int debounce_delay = g_config.scan_interval;
    
    /* First, check if there's already a pending scan for a parent directory */
    parent_idx = find_parent_pending_scan(path);
    if (parent_idx >= 0) {
        /* Parent directory scan will cover this one, extend its delay */
        pending_scans[parent_idx].scheduled_scan_time = now + debounce_delay;
        log_message(LOG_DEBUG, "Event for %s covered by parent scan of %s", 
                   path, pending_scans[parent_idx].path);
        return;
    }
    
    /* Check if there's already a pending scan for this exact path */
    idx = find_pending_scan(path);
    
    if (idx >= 0) {
        /* Already scheduled, extend the delay to coalesce with new event */
        pending_scans[idx].scheduled_scan_time = now + debounce_delay;
        log_message(LOG_DEBUG, "Rescheduled scan for %s to coalesce with new event", path);
    } else {
        /* Check if this path is a parent of any pending scans */
        int child_indices[MAX_EVENT_FDS];
        int num_children = 0;
        
        find_child_pending_scans(path, child_indices, &num_children, MAX_EVENT_FDS);
        
        if (num_children > 0) {
            /* This is a parent directory of one or more pending scans */
            log_message(LOG_DEBUG, "Path %s is parent of %d pending scans, consolidating", path, num_children);
            
            /* Create a new scan for this parent directory */
            if (num_pending_scans >= MAX_EVENT_FDS) {
                /* Use one of the child slots */
                idx = child_indices[0];
            } else {
                idx = num_pending_scans++;
            }
            
            /* Set up the parent scan */
            strncpy(pending_scans[idx].path, path, PATH_MAX_LEN - 1);
            pending_scans[idx].path[PATH_MAX_LEN - 1] = '\0';
            pending_scans[idx].section_id = section_id;
            pending_scans[idx].first_event_time = now;
            pending_scans[idx].scheduled_scan_time = now + debounce_delay;
            pending_scans[idx].is_pending = true;
            
            /* Mark child scans as not pending (except the one we reused) */
            for (int i = 0; i < num_children; i++) {
                if (child_indices[i] != idx) {
                    pending_scans[child_indices[i]].is_pending = false;
                    log_message(LOG_DEBUG, "Removed child scan %s in favor of parent %s", 
                               pending_scans[child_indices[i]].path, path);
                }
            }
            
            log_message(LOG_DEBUG, "Scheduled new parent scan for %s (replaced %d child scans)", 
                       path, num_children);
        } else {
            /* New pending scan with no related existing scans */
            if (num_pending_scans >= MAX_EVENT_FDS) {
                /* Find the oldest scheduled scan to replace */
                time_t oldest_time = now + 86400; /* Initialize with distant future */
                idx = 0;
                
                for (int i = 0; i < num_pending_scans; i++) {
                    if (pending_scans[i].is_pending && 
                        pending_scans[i].scheduled_scan_time < oldest_time) {
                        oldest_time = pending_scans[i].scheduled_scan_time;
                        idx = i;
                    }
                }
                
                log_message(LOG_DEBUG, "Replacing oldest pending scan (%s) with new scan", 
                           pending_scans[idx].path);
            } else {
                idx = num_pending_scans++;
            }
            
            strncpy(pending_scans[idx].path, path, PATH_MAX_LEN - 1);
            pending_scans[idx].path[PATH_MAX_LEN - 1] = '\0';
            pending_scans[idx].section_id = section_id;
            pending_scans[idx].first_event_time = now;
            pending_scans[idx].scheduled_scan_time = now + debounce_delay;
            pending_scans[idx].is_pending = true;
            
            log_message(LOG_DEBUG, "Scheduled new scan for %s", path);
        }
    }
}

/* Process any pending scans that are due */
void events_process_pending(void) {
    time_t now = time(NULL);
    bool scans_executed = false;
    
    for (int i = 0; i < num_pending_scans; i++) {
        if (pending_scans[i].is_pending && now >= pending_scans[i].scheduled_scan_time) {
            /* Time to execute this scan */
            log_message(LOG_INFO, "Executing scan for %s (scanning delayed for %lds)", 
                       pending_scans[i].path, 
                       now - pending_scans[i].first_event_time);
            
            plexapi_trigger_scan(pending_scans[i].path, pending_scans[i].section_id);
            
            /* Mark as completed */
            pending_scans[i].is_pending = false;
            scans_executed = true;
        }
    }
    
    /* Only clean up if we executed scans */
    if (scans_executed) {
        cleanup_completed_scans();
    }
}

/* Get time until next scheduled scan */
time_t next_scheduled_scan(void) {
    time_t next_time = 0;
    time_t now = time(NULL);

    for (int i = 0; i < num_pending_scans; i++) {
        if (pending_scans[i].is_pending && pending_scans[i].scheduled_scan_time > now) {
            if (next_time == 0 || pending_scans[i].scheduled_scan_time < next_time) {
                next_time = pending_scans[i].scheduled_scan_time;
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

/* Remove completed scans from the array */
static void cleanup_completed_scans(void) {
    int i, j;
    
    /* Compact the array by removing completed scans */
    for (i = 0, j = 0; i < num_pending_scans; i++) {
        if (pending_scans[i].is_pending) {
            if (i != j) {
                pending_scans[j] = pending_scans[i];
            }
            j++;
        }
    }
    
    /* Update count */
    num_pending_scans = j;
}