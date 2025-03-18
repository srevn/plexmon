/**
 * dircache.c - Directory structure caching
 */

#include "plexmon.h"
#include <sys/stat.h>

/* Structure to represent a subdirectory entry */
typedef struct dir_entry {
    char path[PATH_MAX_LEN];
    struct dir_entry *next;
} dir_entry_t;

/* Structure to represent a cached directory */
typedef struct cached_dir {
    char path[PATH_MAX_LEN];            /* Directory path */
    time_t mtime;                       /* Last modification time from stat() */
    dir_entry_t *subdirs;               /* List of subdirectories */
    int subdir_count;                   /* Number of subdirectories */
    bool validated;                     /* Whether the cache is up-to-date */
    struct cached_dir *next;            /* Next directory in cache */
} cached_dir_t;

/* Linked list of cached directories */
static cached_dir_t *cache_head = NULL;

/* Initialize the directory cache */
bool dircache_init(void) {
    log_message(LOG_INFO, "Initializing directory cache");
    cache_head = NULL;
    return true;
}

/* Clean up the directory cache */
void dircache_cleanup(void) {
    log_message(LOG_INFO, "Cleaning up directory cache");
    
    cached_dir_t *current = cache_head;
    cached_dir_t *next;
    
    while (current) {
        next = current->next;
        
        /* Free subdirectory entries */
        dir_entry_t *entry = current->subdirs;
        while (entry) {
            dir_entry_t *to_free = entry;
            entry = entry->next;
            free(to_free);
        }
        
        free(current);
        current = next;
    }
    
    cache_head = NULL;
}

/* Get file modification time */
static time_t get_mtime(const char *path) {
    struct stat st;
    
    if (stat(path, &st) != 0) {
        return 0;  /* If we can't stat, return 0 to force refresh */
    }
    
    return st.st_mtime;
}

/* Find a directory in the cache */
static cached_dir_t *find_dir(const char *path) {
    cached_dir_t *current = cache_head;
    
    while (current) {
        if (strcmp(current->path, path) == 0) {
            return current;
        }
        current = current->next;
    }
    
    return NULL;
}

/* Check if directory structure has changed and updates cache */
static bool dirstructure_check_and_update(const char *path, cached_dir_t *dir, bool *changed) {
    DIR *dirp;
    struct dirent *entry;
    char full_path[PATH_MAX_LEN];
    
    /* Temporary structure to hold new directory information */
    dir_entry_t *new_subdirs = NULL;
    int new_subdir_count = 0;
    bool structure_changed = false;
    
    *changed = false;
    
    if (!(dirp = opendir(path))) {
        log_message(LOG_ERR, "Failed to open directory %s: %s", path, strerror(errno));
        *changed = true; /* Assume changed if we can't check */
        return false;
    }
    
    /* Scan for subdirectories and build new structure */
    while ((entry = readdir(dirp))) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        /* Construct full path */
        if (strlen(path) + strlen(entry->d_name) + 2 > PATH_MAX_LEN) {
            log_message(LOG_WARNING, "Path too long: '%s/%s'", path, entry->d_name);
            continue;
        }
        
        snprintf(full_path, PATH_MAX_LEN, "%s/%s", path, entry->d_name);
        
        /* Process subdirectories */
        if (is_directory(full_path)) {
            /* Create new entry */
            dir_entry_t *new_entry = malloc(sizeof(dir_entry_t));
            if (!new_entry) {
                log_message(LOG_ERR, "Failed to allocate memory for subdirectory entry");
                
                /* Clean up temporary structure */
                while (new_subdirs) {
                    dir_entry_t *to_free = new_subdirs;
                    new_subdirs = new_subdirs->next;
                    free(to_free);
                }
                
                closedir(dirp);
                return false;
            }
            
            /* Add subdirectory to new structure */
            strncpy(new_entry->path, full_path, PATH_MAX_LEN - 1);
            new_entry->path[PATH_MAX_LEN - 1] = '\0';
            new_entry->next = new_subdirs;
            new_subdirs = new_entry;
            new_subdir_count++;
            
            /* Check if this directory is in our existing cache */
            if (dir->validated) {
                bool found = false;
                dir_entry_t *current = dir->subdirs;
                while (current && !found) {
                    if (strcmp(current->path, full_path) == 0) {
                        found = true;
                    }
                    current = current->next;
                }
                
                /* If not found, structure has changed */
                if (!found) {
                    log_message(LOG_DEBUG, "New subdirectory found: %s", full_path);
                    structure_changed = true;
                }
            }
        }
    }
    
    closedir(dirp);
    
    /* Check for count mismatch */
    if (!structure_changed && dir->validated && new_subdir_count != dir->subdir_count) {
        log_message(LOG_DEBUG, "Subdirectory count changed: %d -> %d", 
                   dir->subdir_count, new_subdir_count);
        structure_changed = true;
    }
    
    /* Check for deleted subdirectories */
    if (!structure_changed && dir->validated) {
        dir_entry_t *current = dir->subdirs;
        while (current && !structure_changed) {
            bool found = false;
            dir_entry_t *new_current = new_subdirs;
            
            while (new_current && !found) {
                if (strcmp(current->path, new_current->path) == 0) {
                    found = true;
                }
                new_current = new_current->next;
            }
            
            if (!found) {
                log_message(LOG_DEBUG, "Subdirectory removed: %s", current->path);
                structure_changed = true;
                break;
            }
            
            current = current->next;
        }
    }
    
    /* If not validated, assume structure has changed */
    if (!dir->validated) {
        structure_changed = true;
    }
    
    if (structure_changed) {
        log_message(LOG_DEBUG, "Directory structure in %s has changed, updating cache", path);
        
        /* Free existing subdirectory entries */
        dir_entry_t *current = dir->subdirs;
        while (current) {
            dir_entry_t *to_free = current;
            current = current->next;
            free(to_free);
        }
        
        /* Update with new structure */
        dir->subdirs = new_subdirs;
        dir->subdir_count = new_subdir_count;
        dir->mtime = get_mtime(path);
        dir->validated = true;
        
        *changed = true;
    } else {
        log_message(LOG_DEBUG, "Directory structure in %s unchanged", path);
        
        /* Free temporary structure */
        while (new_subdirs) {
            dir_entry_t *to_free = new_subdirs;
            new_subdirs = new_subdirs->next;
            free(to_free);
        }
        
        /* Update timestamp only */
        dir->mtime = get_mtime(path);
        
        *changed = false;
    }
    
    return true;
}

/* Check if directory has changed and update cache if needed */
bool dircache_check_and_update(const char *path, bool *changed) {
    cached_dir_t *dir;
    time_t current_mtime;
    
    *changed = false;
    
    /* Get current mtime */
    current_mtime = get_mtime(path);
    if (current_mtime == 0) {
        log_message(LOG_WARNING, "Failed to get mtime for %s", path);
        return false;
    }
    
    /* Check if directory is in cache */
    dir = find_dir(path);
    
    if (dir) {
        /* Directory is in cache, check if it has changed */
        if (dir->mtime != current_mtime || !dir->validated) {
            log_message(LOG_DEBUG, "Directory %s has modification (mtime: %ld -> %ld), checking structure", 
                       path, dir->mtime, current_mtime);
            
            /* Check and update directory structure in one pass */
            return dirstructure_check_and_update(path, dir, changed);
        } else {
            log_message(LOG_DEBUG, "Directory %s unchanged, using cached data", path);
        }
    } else {
        /* Directory not in cache, add it */
        dir = malloc(sizeof(cached_dir_t));
        if (!dir) {
            log_message(LOG_ERR, "Failed to allocate memory for directory cache");
            return false;
        }
        
        /* Initialize new cache entry */
        strncpy(dir->path, path, PATH_MAX_LEN - 1);
        dir->path[PATH_MAX_LEN - 1] = '\0';
        dir->subdirs = NULL;
        dir->subdir_count = 0;
        dir->validated = false;
        
        /* Add to cache */
        dir->next = cache_head;
        cache_head = dir;
        
        /* Check and update directory structure */
        if (!dirstructure_check_and_update(path, dir, changed)) {
            /* Clean up on failure */
            cache_head = dir->next;
            free(dir);
            return false;
        }
        
        log_message(LOG_DEBUG, "Directory %s added to cache", path);
    }
    
    return true;
}

/* Get subdirectories from cache */
char **dircache_get_subdirs(const char *path, int *count) {
    cached_dir_t *dir;
    char **subdirs;
    int i = 0;
    
    *count = 0;
    
    /* Find directory in cache */
    dir = find_dir(path);
    if (!dir || !dir->validated) {
        /* If not in cache or not valid, return NULL */
        return NULL;
    }
    
    /* If no subdirectories, return NULL */
    if (dir->subdir_count == 0) {
        return NULL;
    }
    
    /* Allocate array of strings */
    subdirs = malloc(dir->subdir_count * sizeof(char *));
    if (!subdirs) {
        log_message(LOG_ERR, "Failed to allocate memory for subdirectory list");
        return NULL;
    }
    
    /* Fill array with subdirectory paths */
    dir_entry_t *entry = dir->subdirs;
    while (entry && i < dir->subdir_count) {
        subdirs[i] = strdup(entry->path);
        if (!subdirs[i]) {
            /* Clean up on failure */
            for (int j = 0; j < i; j++) {
                free(subdirs[j]);
            }
            free(subdirs);
            log_message(LOG_ERR, "Failed to allocate memory for subdirectory path");
            return NULL;
        }
        
        i++;
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