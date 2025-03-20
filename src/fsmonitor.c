/**
 * fsmonitor.c - File system monitoring using kqueue
 */

#include "plexmon.h"

/* Array of monitored directories */
static monitored_dir_t monitored_dirs[MAX_EVENT_FDS];
static int num_monitored_dirs = 0;

/* Global kqueue descriptor */
static int g_kqueue_fd = -1;

/* User event identifiers */
#define USER_EVENT_EXIT    1
#define USER_EVENT_RELOAD  2
static uintptr_t g_user_event_ident = 0;

/* Structure to represent a directory queue node */
typedef struct dir_queue_node {
    char path[PATH_MAX_LEN];
    struct dir_queue_node *next;
} dir_queue_node_t;

/* Structure for a FIFO queue */
typedef struct {
    dir_queue_node_t *front;
    dir_queue_node_t *rear;
} dir_queue_t;

/* Initialize an empty queue */
static void queue_init(dir_queue_t *queue) {
    queue->front = NULL;
    queue->rear = NULL;
}

/* Add an item to the queue (enqueue) */
static bool queue_enqueue(dir_queue_t *queue, const char *path) {
    dir_queue_node_t *new_node = malloc(sizeof(dir_queue_node_t));
    if (!new_node) {
        return false;
    }
    
    strncpy(new_node->path, path, PATH_MAX_LEN - 1);
    new_node->path[PATH_MAX_LEN - 1] = '\0';
    new_node->next = NULL;
    
    if (queue->rear == NULL) {
        /* Empty queue */
        queue->front = new_node;
        queue->rear = new_node;
    } else {
        /* Add to the end */
        queue->rear->next = new_node;
        queue->rear = new_node;
    }
    
    return true;
}

/* Remove an item from the queue (dequeue) */
static bool queue_dequeue(dir_queue_t *queue, char *path) {
    if (queue->front == NULL) {
        return false;
    }
    
    dir_queue_node_t *temp = queue->front;
    
    strncpy(path, temp->path, PATH_MAX_LEN - 1);
    path[PATH_MAX_LEN - 1] = '\0';
    
    queue->front = queue->front->next;
    
    if (queue->front == NULL) {
        queue->rear = NULL;
    }
    
    free(temp);
    return true;
}

/* Free all nodes in the queue */
static void queue_free(dir_queue_t *queue) {
    dir_queue_node_t *current, *next;
    
    current = queue->front;
    while (current) {
        next = current->next;
        free(current);
        current = next;
    }
    
    queue->front = NULL;
    queue->rear = NULL;
}

/* Check if queue is empty */
static bool queue_is_empty(dir_queue_t *queue) {
    return queue->front == NULL;
}

/* Initialize file system monitoring */
bool fsmonitor_init(void) {
    log_message(LOG_INFO, "Initializing file system monitoring");
    
    /* Reset monitored directories */
    memset(monitored_dirs, 0, sizeof(monitored_dirs));
    for (int i = 0; i < MAX_EVENT_FDS; i++) {
        monitored_dirs[i].fd = -1; // Initialize with invalid fd
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
    g_user_event_ident = getpid();  /* Use PID as the identifier */
    
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
void fsmonitor_cleanup(void) {
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
void fsmonitor_signal_exit(void) {
    struct kevent kev;
    
    if (g_kqueue_fd == -1) {
        return;
    }
    
    log_message(LOG_INFO, "Sending exit signal to event loop");
    
    /* Set up and trigger the user event for exit */
    EV_SET(&kev, g_user_event_ident, EVFILT_USER, EV_ENABLE, NOTE_TRIGGER, USER_EVENT_EXIT, NULL);
    
    if (kevent(g_kqueue_fd, &kev, 1, NULL, 0, NULL) == -1) {
        log_message(LOG_ERR, "Failed to signal exit event: %s", strerror(errno));
    }
}

/* Signal to the event loop to reload configuration */
void fsmonitor_signal_reload(void) {
    struct kevent kev;
    
    if (g_kqueue_fd == -1) {
        return;
    }
    
    log_message(LOG_INFO, "Sending reload signal to event loop");
    
    /* Set up and trigger the user event for reload */
    EV_SET(&kev, g_user_event_ident, EVFILT_USER, EV_ENABLE, NOTE_TRIGGER, USER_EVENT_RELOAD, NULL);
    
    if (kevent(g_kqueue_fd, &kev, 1, NULL, 0, NULL) == -1) {
        log_message(LOG_ERR, "Failed to signal reload event: %s", strerror(errno));
    }
}

/* Return the current count of monitored directories */
int get_monitored_dir_count(void) {
    return num_monitored_dirs;
}

/* Get the kqueue file descriptor */
int fsmonitor_get_kqueue_fd(void) {
    return g_kqueue_fd;
}

/* Register a directory with kqueue */
static bool register_directory_with_kqueue(int fd, monitored_dir_t *dir_info) {
    struct kevent change;
    
    /* Set up the kevent structure for this directory */
    EV_SET(&change, fd, EVFILT_VNODE, 
           EV_ADD | EV_CLEAR | EV_ENABLE,
           NOTE_WRITE | NOTE_ATTRIB | NOTE_RENAME | NOTE_LINK |
           NOTE_DELETE | NOTE_EXTEND | NOTE_REVOKE,
           0, dir_info);
    
    /* Register event with kqueue */
    if (kevent(g_kqueue_fd, &change, 1, NULL, 0, NULL) == -1) {
        log_message(LOG_ERR, "Error registering directory %s with kqueue: %s", 
                   dir_info->path, strerror(errno));
        return false;
    }
    
    return true;
}

/* Add a directory to the monitoring list */
int fsmonitor_add_directory(const char *path, int plex_section_id) {
    if (num_monitored_dirs >= MAX_EVENT_FDS) {
        log_message(LOG_ERR, "Maximum number of monitored directories reached");
        return -1;
    }
    
    /* Check if the directory is already being monitored */
    for (int i = 0; i < num_monitored_dirs; i++) {
        if (strcmp(monitored_dirs[i].path, path) == 0) {
            log_message(LOG_DEBUG, "Directory %s is already being monitored", path);
            return i;
        }
    }
    
    /* Open directory */
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        log_message(LOG_ERR, "Failed to open directory %s: %s", path, strerror(errno));
        return -1;
    }
    
    /* Add to monitored directories */
    monitored_dirs[num_monitored_dirs].fd = fd;
    strncpy(monitored_dirs[num_monitored_dirs].path, path, PATH_MAX_LEN - 1);
    monitored_dirs[num_monitored_dirs].path[PATH_MAX_LEN - 1] = '\0';
    monitored_dirs[num_monitored_dirs].plex_section_id = plex_section_id;
    
    /* Register with kqueue */
    if (!register_directory_with_kqueue(fd, &monitored_dirs[num_monitored_dirs])) {
        close(fd);
        return -1;
    }
    
    return num_monitored_dirs++;
}

/* Handle directory events */
static void handle_directory_event(const struct kevent *event, monitored_dir_t *md) {
    
    if (!(event->fflags & NOTE_WRITE)) return;

    const char *path = md->path;
    log_message(LOG_INFO, "Change detected in directory: %s", path);

    if (!is_directory(path)) return;
    
    /* Directory cache with mtime checking */
    bool dir_changed = false;
    if (!dircache_refresh(path, &dir_changed)) {
        log_message(LOG_WARNING, "Failed to check directory cache for %s, using targeted refresh", path);
        scan_new_directories(path, md->plex_section_id);
        return;
    }

    if (dir_changed) {
        log_message(LOG_DEBUG, "Directory structure changed in %s, detecting new subdirectories", path);
        /* Only register new subdirectories instead of full tree rescanning */
        int new_dirs = scan_new_directories(path, md->plex_section_id);
        log_message(LOG_DEBUG, "Registered %d new directories under %s", new_dirs, path);
    } else {
        /* Still queue a Plex scan but skip directory tree rescanning */
        log_message(LOG_DEBUG, "File change detected in %s, triggering Plex scan", path);
    }
    
    /* Queue event */
    events_handle(path, md->plex_section_id);
}

/* Process a single event */
static void process_event(const struct kevent *event) {
    /* Check for user events */
    if (event->filter == EVFILT_USER && event->ident == g_user_event_ident) {
        
        uint32_t data = event->data;
        
        if (data == USER_EVENT_EXIT) {
            g_running = 0;
            log_message(LOG_INFO, "Received exit event");
        } else if (data == USER_EVENT_RELOAD) {
            log_message(LOG_INFO, "Received reload event, reloading configuration");
            config_load(DEFAULT_CONFIG_FILE);
        }
        
        return;
    }

    if (event->flags & EV_ERROR) {
        log_message(LOG_ERR, "Event error: %s", strerror(event->data));
        return;
    }

    monitored_dir_t *md = (monitored_dir_t *)event->udata;
    if (!md || !event->fflags) return;

    handle_directory_event(event, md);
}

/* Calculate the timeout for the next scan */
static void calculate_timeout(time_t next_scan, struct timespec *timeout) {
    time_t now = time(NULL);
    time_t time_left = next_scan > now ? next_scan - now : 0;
    
    timeout->tv_sec = time_left;
    timeout->tv_nsec = 0;
}

/* Process events from kqueue */
void fsmonitor_process_events(void) {
    struct kevent events[MAX_EVENT_FDS];
    struct timespec timeout;
    int nev;
    
    calculate_timeout(next_scheduled_scan(), &timeout);

    /* Indefinite wait if no scans and no events */
    nev = kevent(g_kqueue_fd, NULL, 0, events, MAX_EVENT_FDS,
                (timeout.tv_sec == 0 && timeout.tv_nsec == 0) ? NULL : &timeout);
    
    if (nev == -1 && errno != EINTR) {
        log_message(LOG_ERR, "Error in kevent: %s", strerror(errno));
        return;
    }
    
    /* Process received events */
    for (int i = 0; i < nev; i++) {
        process_event(&events[i]);
    }
    
    /* Process any pending scans that are ready */
    events_process_pending();
}

/* Helper function to get subdirectories */
static char **get_subdirectories(const char *path, int *count) {
    char **subdirs = dircache_get_subdirs(path, count);
    
    if (!subdirs) {
        /* Cache miss, update it and try again */
        bool dir_changed = false;
        if (dircache_refresh(path, &dir_changed)) {
            subdirs = dircache_get_subdirs(path, count);
        }
        
        if (!subdirs) {
            log_message(LOG_DEBUG, "No subdirectories found for %s", path);
            *count = 0;
            return NULL;
        }
    }
    
    return subdirs;
}

/* Helper function to check if directory is already being monitored */
static bool is_directory_monitored(const char *path) {
    for (int i = 0; i < num_monitored_dirs; i++) {
        if (strcmp(monitored_dirs[i].path, path) == 0) {
            return true;
        }
    }
    return false;
}

/* Helper function for processing directories */
static bool process_directory_queue(dir_queue_t *queue, int section_id, bool scan_mode) {
    char current_path[PATH_MAX_LEN];
    int new_dirs_count = 0;

    while (!queue_is_empty(queue)) {
        if (!queue_dequeue(queue, current_path)) break;
        
        /* Common monitoring check */
        if (!is_directory_monitored(current_path) && !scan_mode) {
            int dir_idx = fsmonitor_add_directory(current_path, section_id);
            if (dir_idx < 0) continue;
            log_message(LOG_DEBUG, "Added directory %s to monitoring", current_path);
        }
        
        /* Common subdirectory processing */
        int subdir_count = 0;
        char **subdirs = get_subdirectories(current_path, &subdir_count);
        if (!subdirs) continue;
        
        for (int i = 0; i < subdir_count; i++) {
            if (scan_mode && is_directory_monitored(subdirs[i])) continue;
            
            if (scan_mode) {
                int dir_idx = fsmonitor_add_directory(subdirs[i], section_id);
                if (dir_idx >= 0) {
                    new_dirs_count++;
                    log_message(LOG_DEBUG, "Added new directory %s to monitoring", subdirs[i]);
                }
            }
            
            if (!queue_enqueue(queue, subdirs[i])) {
                log_message(LOG_ERR, "Directory queue allocation failed");
                dircache_free_subdirs(subdirs, subdir_count);
                queue_free(queue);
                return false;
            }
        }
        dircache_free_subdirs(subdirs, subdir_count);
    }
    
    return new_dirs_count;
}

/* Recursively add a directory and its subdirectories to the watch list */
bool watch_directory_tree(const char *dir_path, int section_id) {
    dir_queue_t queue;
    queue_init(&queue);

    if (!queue_enqueue(&queue, dir_path)) {
        log_message(LOG_ERR, "Directory queue allocation failed");
        return false;
    }

    log_message(LOG_DEBUG, "Starting directory tree registration from %s", dir_path);
    process_directory_queue(&queue, section_id, false);
    queue_free(&queue);
    return true;
}

/* Detect and register new subdirectories */
int scan_new_directories(const char *dir_path, int section_id) {
    dir_queue_t queue;
    queue_init(&queue);
    int new_dirs_count = 0;

    if (!queue_enqueue(&queue, dir_path)) {
        log_message(LOG_ERR, "Directory queue allocation failed");
        return 0;
    }

    log_message(LOG_DEBUG, "Detecting new subdirectories starting from %s", dir_path);
    new_dirs_count = process_directory_queue(&queue, section_id, true);
    
    if (new_dirs_count > 0) {
        log_message(LOG_INFO, "Added %d new directories under %s", 
                   new_dirs_count, dir_path);
    }

    queue_free(&queue);
    return new_dirs_count;
}

/* Check if a path is a directory */
bool is_directory(const char *path) {
    struct stat st;
    
    if (stat(path, &st) == -1) {
        log_message(LOG_ERR, "Failed to stat %s: %s", path, strerror(errno));
        return false;
    }
    
    return S_ISDIR(st.st_mode);
}

/* Run the filesystem event monitor loop */
bool fsmonitor_run_loop(void) {
    if (g_kqueue_fd == -1) {
        log_message(LOG_ERR, "Invalid kqueue descriptor");
        return false;
    }
    
    /* Process events until g_running becomes false */
    g_running = 1;
    while (g_running) {
        fsmonitor_process_events();
    }
    
    return true;
}