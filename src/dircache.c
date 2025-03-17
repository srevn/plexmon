/**
 * dircache.c - Directory structure caching
 */

#include "plexmon.h"
#include <limits.h>

/* Structure to represent a directory entry */
typedef struct dir_entry {
    char name[NAME_MAX];
    bool is_dir;
    struct dir_entry *next;
} dir_entry_t;

/* Structure to represent a cached directory */
typedef struct cached_dir {
    char path[PATH_MAX_LEN];
    dir_entry_t *entries;
    time_t last_modified;
    struct cached_dir *next;
} cached_dir_t;

/* Linked list of cached directories */
static cached_dir_t *cached_dirs = NULL;

/* Forward declarations */
static void free_dir_entries(dir_entry_t *entries);
static dir_entry_t *read_dir_entries(const char *path);
static bool dir_entries_changed(dir_entry_t *old_entries, dir_entry_t *new_entries);
static bool is_special_dir(const char *name);

/* Initialize the directory cache */
bool dircache_init(void) {
    log_message(LOG_INFO, "Initializing directory cache");
    cached_dirs = NULL;
    return true;
}

/* Clean up the directory cache */
void dircache_cleanup(void) {
    log_message(LOG_INFO, "Cleaning up directory cache");
    
    cached_dir_t *current = cached_dirs;
    cached_dir_t *next;
    
    while (current) {
        next = current->next;
        free_dir_entries(current->entries);
        free(current);
        current = next;
    }
    
    cached_dirs = NULL;
}

/* Free a linked list of directory entries */
static void free_dir_entries(dir_entry_t *entries) {
    dir_entry_t *current = entries;
    dir_entry_t *next;
    
    while (current) {
        next = current->next;
        free(current);
        current = next;
    }
}

/* Check if a directory name is "." or ".." */
static bool is_special_dir(const char *name) {
    return (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
}

/* Read directory entries into a linked list */
static dir_entry_t *read_dir_entries(const char *path) {
    DIR *dir;
    struct dirent *entry;
    dir_entry_t *head = NULL;
    dir_entry_t *tail = NULL;
    dir_entry_t *new_entry;
    char full_path[PATH_MAX_LEN];
    
    if (!(dir = opendir(path))) {
        log_message(LOG_ERR, "Failed to open directory %s: %s", path, strerror(errno));
        return NULL;
    }
    
    while ((entry = readdir(dir))) {
        /* Skip . and .. */
        if (is_special_dir(entry->d_name)) {
            continue;
        }
        
        new_entry = malloc(sizeof(dir_entry_t));
        if (!new_entry) {
            log_message(LOG_ERR, "Failed to allocate memory for directory entry");
            free_dir_entries(head);
            closedir(dir);
            return NULL;
        }
        
        strncpy(new_entry->name, entry->d_name, NAME_MAX - 1);
        new_entry->name[NAME_MAX - 1] = '\0';
        
        /* Determine if it's a directory */
        snprintf(full_path, PATH_MAX_LEN, "%s/%s", path, entry->d_name);
        new_entry->is_dir = is_directory(full_path);
        new_entry->next = NULL;
        
        if (!head) {
            head = new_entry;
            tail = new_entry;
        } else {
            tail->next = new_entry;
            tail = new_entry;
        }
    }
    
    closedir(dir);
    return head;
}

/* Compare two directory entry lists to detect changes */
static bool dir_entries_changed(dir_entry_t *old_entries, dir_entry_t *new_entries) {
    dir_entry_t *old_curr, *new_curr;
    
    /* Compare entries in both lists */
    old_curr = old_entries;
    while (old_curr) {
        bool found = false;
        
        new_curr = new_entries;
        while (new_curr) {
            if (strcmp(old_curr->name, new_curr->name) == 0 && old_curr->is_dir == new_curr->is_dir) {
                found = true;
                break;
            }
            new_curr = new_curr->next;
        }
        
        if (!found) {
            /* An entry from old list is missing in new list */
            return true;
        }
        
        old_curr = old_curr->next;
    }
    
    /* Check for new entries */
    new_curr = new_entries;
    while (new_curr) {
        bool found = false;
        
        old_curr = old_entries;
        while (old_curr) {
            if (strcmp(new_curr->name, old_curr->name) == 0 && new_curr->is_dir == old_curr->is_dir) {
                found = true;
                break;
            }
            old_curr = old_curr->next;
        }
        
        if (!found) {
            /* A new entry exists that wasn't in the old list */
            return true;
        }
        
        new_curr = new_curr->next;
    }
    
    /* No changes detected */
    return false;
}

/* Find directory in cache by path */
static cached_dir_t *find_cached_dir(const char *path) {
    cached_dir_t *current = cached_dirs;
    
    while (current) {
        if (strcmp(current->path, path) == 0) {
            return current;
        }
        current = current->next;
    }
    
    return NULL;
}

/* Add directory to cache */
static cached_dir_t *add_to_cache(const char *path, dir_entry_t *entries) {
    cached_dir_t *new_dir = malloc(sizeof(cached_dir_t));
    if (!new_dir) {
        log_message(LOG_ERR, "Failed to allocate memory for cached directory");
        return NULL;
    }
    
    strncpy(new_dir->path, path, PATH_MAX_LEN - 1);
    new_dir->path[PATH_MAX_LEN - 1] = '\0';
    new_dir->entries = entries;
    new_dir->last_modified = time(NULL);
    
    /* Add to the front of the list */
    new_dir->next = cached_dirs;
    cached_dirs = new_dir;
    
    return new_dir;
}

/* Check if directory has changed and update cache if needed */
bool dircache_check_and_update(const char *path, bool *changed) {
    dir_entry_t *current_entries = NULL;
    cached_dir_t *cached_dir = NULL;
    
    *changed = false;
    
    /* Find cached directory */
    cached_dir = find_cached_dir(path);
    
    /* Read current directory entries */
    current_entries = read_dir_entries(path);
    if (!current_entries) {
        /* Failed to read directory, treat as unchanged if we can't read it */
        return false;
    }
    
    if (cached_dir) {
        /* Check if directory has changed */
        *changed = dir_entries_changed(cached_dir->entries, current_entries);
        
        if (*changed) {
            /* Update cache with new entries */
            free_dir_entries(cached_dir->entries);
            cached_dir->entries = current_entries;
            cached_dir->last_modified = time(NULL);
            log_message(LOG_DEBUG, "Directory %s has changed, cache updated", path);
        } else {
            /* No changes, free the current entries as we don't need them */
            free_dir_entries(current_entries);
            log_message(LOG_DEBUG, "Directory %s unchanged, using cached data", path);
        }
    } else {
        /* Directory not in cache, add it */
        cached_dir = add_to_cache(path, current_entries);
        if (!cached_dir) {
            /* Failed to add to cache */
            free_dir_entries(current_entries);
            return false;
        }
        
        /* First time we're seeing this directory, treat as changed */
        *changed = true;
        log_message(LOG_DEBUG, "Directory %s added to cache", path);
    }
    
    return true;
}

/* Get subdirectories from cache */
char **dircache_get_subdirs(const char *path, int *count) {
    cached_dir_t *cached_dir = find_cached_dir(path);
    dir_entry_t *entry;
    char **subdirs = NULL;
    int num_subdirs = 0;
    int i = 0;
    
    *count = 0;
    
    if (!cached_dir) {
        return NULL;
    }
    
    /* First count the number of subdirectories */
    entry = cached_dir->entries;
    while (entry) {
        if (entry->is_dir) {
            num_subdirs++;
        }
        entry = entry->next;
    }
    
    if (num_subdirs == 0) {
        return NULL;
    }
    
    /* Allocate array of strings */
    subdirs = malloc(num_subdirs * sizeof(char *));
    if (!subdirs) {
        log_message(LOG_ERR, "Failed to allocate memory for subdirectory list");
        return NULL;
    }
    
    /* Fill array */
    entry = cached_dir->entries;
    while (entry && i < num_subdirs) {
        if (entry->is_dir) {
            subdirs[i] = malloc(PATH_MAX_LEN);
            if (!subdirs[i]) {
                /* Clean up previously allocated memory */
                for (int j = 0; j < i; j++) {
                    free(subdirs[j]);
                }
                free(subdirs);
                log_message(LOG_ERR, "Failed to allocate memory for subdirectory path");
                return NULL;
            }
            
            snprintf(subdirs[i], PATH_MAX_LEN, "%s/%s", path, entry->name);
            i++;
        }
        entry = entry->next;
    }
    
    *count = i;
    return subdirs;
}

/* Free subdirectory list */
void dircache_free_subdirs(char **subdirs, int count) {
    if (!subdirs) {
        return;
    }
    
    for (int i = 0; i < count; i++) {
        free(subdirs[i]);
    }
    
    free(subdirs);
}