/**
 * fsmonitor.c - File system monitoring using kqueue
 */

#include "plexmon.h"

/* Array of monitored directories */
static monitored_dir_t monitored_dirs[MAX_EVENT_FDS];
static int num_monitored_dirs = 0;

/* Global kqueue descriptor */
static int g_kqueue_fd = -1;

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

/* Process events from kqueue */
void fsmonitor_process_events(void) {
    struct kevent events[MAX_EVENT_FDS];
    int nev;
    
    struct timespec timeout;
    timeout.tv_sec = 1;  /* 1 second timeout */
    timeout.tv_nsec = 0;
    
    nev = kevent(g_kqueue_fd, NULL, 0, events, MAX_EVENT_FDS, &timeout);
    
    if (nev == -1) {
        if (errno != EINTR) {
            log_message(LOG_ERR, "Error in kevent: %s", strerror(errno));
        }
        return;
    }
    
    /* Process received events */
    for (int i = 0; i < nev; i++) {
        monitored_dir_t *md = (monitored_dir_t *)events[i].udata;
        
        if (events[i].flags & EV_ERROR) {
            log_message(LOG_ERR, "Event error: %s", strerror(events[i].data));
            continue;
        }
        
        if (md && events[i].fflags) {
            /* Handle directory events */
            if (events[i].fflags & NOTE_WRITE) {
                log_message(LOG_INFO, "Change detected in directory: %s", md->path);
                
                /* Check for new subdirectories that need to be monitored */
                if (is_directory(md->path)) {
                    add_watch_recursive(md->path, md->plex_section_id);
                }
            }
            
            /* Trigger Plex scan */
            events_handle(md->path, md->plex_section_id);
        }
    }
}

/* Recursively add a directory and its subdirectories to the watch list */
bool add_watch_recursive(const char *dir_path, int section_id) {
    dir_queue_t queue;
    char current_path[PATH_MAX_LEN];
    DIR *dir;
    struct dirent *entry;
    char path[PATH_MAX_LEN];
    bool success = true;
    
    /* Initialize queue */
    queue_init(&queue);
    
    /* Start with the initial directory */
    if (!queue_enqueue(&queue, dir_path)) {
        log_message(LOG_ERR, "Failed to allocate memory for directory queue");
        return false;
    }
    
    /* Process directories from the queue */
    while (!queue_is_empty(&queue)) {
        if (!queue_dequeue(&queue, current_path)) {
            break;  /* Should not happen */
        }
        
        /* Open current directory */
        if (!(dir = opendir(current_path))) {
            log_message(LOG_ERR, "Failed to open directory %s: %s", current_path, strerror(errno));
            continue; /* Skip this directory but continue with others */
        }
        
        /* Add current directory to monitoring */
        int dir_idx = fsmonitor_add_directory(current_path, section_id);
        if (dir_idx < 0) {
            log_message(LOG_WARNING, "Failed to add directory %s to monitoring", current_path);
            closedir(dir);
            continue; /* Skip this directory but continue with others */
        }
        
        /* Process all entries in current directory */
        while ((entry = readdir(dir))) {
            /* Skip . and .. */
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            
            snprintf(path, sizeof(path), "%s/%s", current_path, entry->d_name);
            
            /* If it's a directory, add to queue */
            if (is_directory(path)) {
                if (!queue_enqueue(&queue, path)) {
                    log_message(LOG_ERR, "Failed to allocate memory for directory queue");
                    success = false;
                    break;
                }
            }
        }
        
        closedir(dir);
        
        /* If we had a memory allocation error, break out of the loop */
        if (!success) {
            break;
        }
    }
    
    /* Clean up any remaining queue entries */
    queue_free(&queue);
    
    return success;
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
    
    /* Process events as long as g_running is true */
    while (g_running) {
        fsmonitor_process_events();
    }
    
    return true;
}