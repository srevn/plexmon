/**
 * dircache.c - Directory structure caching with hash table optimization
 */

#include "plexmon.h"
#include <sys/stat.h>
#include <search.h>

/* Structure to represent a subdirectory entry */
typedef struct dir_entry {
    char path[PATH_MAX_LEN];            /* Subdirectory path */
    struct dir_entry *next;             /* Pointer to next entry */
} dir_entry_t;

/* Structure to represent a cached directory */
typedef struct cached_dir {
    char path[PATH_MAX_LEN];            /* Directory path */
    time_t mtime;                       /* Last modification time from stat() */
    dir_entry_t *subdirs;               /* List of subdirectories */
    int subdir_count;                   /* Number of subdirectories */
    bool validated;                     /* Whether the cache is up-to-date */
} cached_dir_t;

/* Hash table for directory cache */
static struct hsearch_data dir_cache_htab;
static int dir_cache_count = 0;

/* We need to track all allocated entries to properly clean them up */
typedef struct cache_entry_tracker {
    char *key;                          /* Allocated string for hash key */
    cached_dir_t *dir;                  /* The cached directory entry */
    struct cache_entry_tracker *next;   /* Next entry in the tracker */
} cache_entry_tracker_t;

static cache_entry_tracker_t *entry_tracker_head = NULL;

/* Initialize the directory cache */
bool dircache_init(void) {
    log_message(LOG_INFO, "Initializing directory cache with hash table");
    
    /* Initialize hash table */
    memset(&dir_cache_htab, 0, sizeof(dir_cache_htab));
    if (!hcreate_r(HASH_TABLE_SIZE, &dir_cache_htab)) {
        log_message(LOG_ERR, "Failed to create hash table: %s", strerror(errno));
        return false;
    }
    
    dir_cache_count = 0;
    entry_tracker_head = NULL;
    return true;
}

/* Add entry to the tracker */
static bool track_cache_entry(char *key, cached_dir_t *dir) {
    cache_entry_tracker_t *tracker = malloc(sizeof(cache_entry_tracker_t));
    if (!tracker) {
        return false;
    }
    
    tracker->key = key;
    tracker->dir = dir;
    tracker->next = entry_tracker_head;
    entry_tracker_head = tracker;
    
    return true;
}

/* Clean up the directory cache */
void dircache_cleanup(void) {
    log_message(LOG_INFO, "Cleaning up directory cache");
    
    /* Free all tracked entries */
    while (entry_tracker_head) {
        cache_entry_tracker_t *to_free = entry_tracker_head;
        entry_tracker_head = entry_tracker_head->next;
        
        /* Free subdirectory entries */
        dir_entry_t *entry = to_free->dir->subdirs;
        while (entry) {
            dir_entry_t *subdir_to_free = entry;
            entry = entry->next;
            free(subdir_to_free);
        }
        
        /* Free directory entry and key */
        free(to_free->key);
        free(to_free->dir);
        free(to_free);
    }
    
    /* Destroy hash table */
    hdestroy_r(&dir_cache_htab);
    dir_cache_count = 0;
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
    ENTRY item, *result;
    
    /* Set up the search key */
    item.key = (char *)path;  /* hsearch_r doesn't modify the key */
    
    /* Search in hash table */
    if (hsearch_r(item, FIND, &result, &dir_cache_htab)) {
        return (cached_dir_t *)result->data;
    }
    
    return NULL;
}

/* Check if directory structure has changed and updates cache */
static bool sync_directory_tree(const char *path, cached_dir_t *dir, bool *changed) {
    DIR *dirp;
    struct dirent *entry;
    char full_path[PATH_MAX_LEN];
    
    /* Temporary structure to hold new directory information */
    dir_entry_t *new_subdirs = NULL;
    int new_subdir_count = 0;
    bool structure_changed = false;
    
    /* Hash tables for efficient path lookup */
    struct hsearch_data new_paths_htab;
    struct hsearch_data old_paths_htab;
    bool htab_initialized = false;
    
    *changed = false;
    
    if (!(dirp = opendir(path))) {
        log_message(LOG_ERR, "Failed to open directory %s: %s", path, strerror(errno));
        *changed = true; /* Assume changed if we can't check */
        return false;
    }
    
    /* Initialize hash tables for path comparison */
    memset(&new_paths_htab, 0, sizeof(new_paths_htab));
    memset(&old_paths_htab, 0, sizeof(old_paths_htab));
    
    /* Create hash tables with appropriate sizes - 512 entries should be enough for most directories */
    if (!hcreate_r(512, &new_paths_htab) || !hcreate_r(512, &old_paths_htab)) {
        log_message(LOG_ERR, "Failed to create hash tables for directory comparison: %s", strerror(errno));
        closedir(dirp);
        return false;
    }
    htab_initialized = true;
    
    /* First, add existing subdirectories to old_paths_htab */
    if (dir->validated) {
        dir_entry_t *current = dir->subdirs;
        while (current) {
            ENTRY item, *result;
            item.key = strdup(current->path);
            if (!item.key) {
                log_message(LOG_ERR, "Failed to allocate memory for path hash key");
                structure_changed = true;
                break;
            }
            item.data = current;
            
            if (!hsearch_r(item, ENTER, &result, &old_paths_htab)) {
                log_message(LOG_ERR, "Failed to add path to hash table: %s", strerror(errno));
                free(item.key);  /* Free the key if insertion fails */
                structure_changed = true;  /* Assume changed if we can't check properly */
                break;
            }
            
            current = current->next;
        }
    } else {
        structure_changed = true;  /* Not validated, assume changed */
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
            /* Add path to hash table for quick lookups */
            ENTRY item, *result;
            char *path_key = strdup(full_path);
            if (!path_key) {
                log_message(LOG_ERR, "Failed to allocate memory for path hash key");
                structure_changed = true;
                continue;
            }
            item.key = path_key;
            item.data = (void *)1;  /* Just a non-NULL marker */
            
            if (!hsearch_r(item, ENTER, &result, &new_paths_htab)) {
                log_message(LOG_ERR, "Failed to add path to hash table: %s", strerror(errno));
                free(path_key);
                structure_changed = true;
                continue;
            }
            
            /* Create new entry */
            dir_entry_t *new_entry = malloc(sizeof(dir_entry_t));
            if (!new_entry) {
                log_message(LOG_ERR, "Failed to allocate memory for subdirectory entry");
                structure_changed = true;
                continue;
            }
            
            /* Add subdirectory to new structure */
            strncpy(new_entry->path, full_path, PATH_MAX_LEN - 1);
            new_entry->path[PATH_MAX_LEN - 1] = '\0';
            new_entry->next = new_subdirs;
            new_subdirs = new_entry;
            new_subdir_count++;
            
            /* Check if this directory is in our existing cache */
            if (dir->validated && !structure_changed) {
                ENTRY search_item;
                search_item.key = full_path;
                
                if (!hsearch_r(search_item, FIND, &result, &old_paths_htab)) {
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
            ENTRY search_item;
            ENTRY *result;
            
            search_item.key = current->path;
            
            if (!hsearch_r(search_item, FIND, &result, &new_paths_htab)) {
                log_message(LOG_DEBUG, "Subdirectory removed: %s", current->path);
                structure_changed = true;
                break;
            }
            
            current = current->next;
        }
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
    
    /* Clean up hash tables */
    if (htab_initialized) {
        hdestroy_r(&new_paths_htab);
        hdestroy_r(&old_paths_htab);
    }
    
    return true;
}

/* Check if directory has changed and update cache if needed */
bool dircache_refresh(const char *path, bool *changed) {
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
            return sync_directory_tree(path, dir, changed);
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
        
        /* Add to hash table */
        ENTRY item, *result;
        char *key_copy = strdup(path);  /* Must allocate a copy for the key */
        if (!key_copy) {
            log_message(LOG_ERR, "Failed to allocate memory for hash table key");
            free(dir);
            return false;
        }
        item.key = key_copy;
        item.data = dir;
        
        if (!hsearch_r(item, ENTER, &result, &dir_cache_htab)) {
            log_message(LOG_ERR, "Failed to add directory to hash table: %s", strerror(errno));
            free(key_copy);
            free(dir);
            return false;
        }
        
        /* Track this entry for proper cleanup */
        if (!track_cache_entry(key_copy, dir)) {
            log_message(LOG_ERR, "Failed to track cache entry");
            /* Can't remove from hash table, but continue anyway */
        }
        
        dir_cache_count++;
        
        /* Check and update directory structure */
        if (!sync_directory_tree(path, dir, changed)) {
            /* Clean up on failure - note that we can't remove from hash table */
            /* The entry will be cleaned up during dircache_cleanup */
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