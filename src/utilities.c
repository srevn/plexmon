/**
 * utilities.c - Utility functions for plexmon
 */

#include "plexmon.h"

/* Check if a path is a directory */
bool is_directory(const char *path) {
    struct stat st;
    
    if (stat(path, &st) == -1) {
        log_message(LOG_ERR, "Failed to stat %s: %s", path, strerror(errno));
        return false;
    }
    
    return S_ISDIR(st.st_mode);
}

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

/* Helper function for processing directories */
static int process_directory_queue(dir_queue_t *queue, int section_id, bool scan_mode) {
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