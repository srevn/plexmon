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

/* Add a subdirectory to the cache entry */
static bool add_subdir(cached_dir_t *dir, const char *path) {
    dir_entry_t *new_entry = malloc(sizeof(dir_entry_t));
    if (!new_entry) {
        log_message(LOG_ERR, "Failed to allocate memory for subdirectory entry");
        return false;
    }
    
    strncpy(new_entry->path, path, PATH_MAX_LEN - 1);
    new_entry->path[PATH_MAX_LEN - 1] = '\0';
    new_entry->next = dir->subdirs;
    
    dir->subdirs = new_entry;
    dir->subdir_count++;
    
    return true;
}

/* Scan a directory and update the cache */
static bool scan_directory(const char *path, cached_dir_t *dir) {
    DIR *dirp;
    struct dirent *entry;
    char full_path[PATH_MAX_LEN];
    
    if (!(dirp = opendir(path))) {
        log_message(LOG_ERR, "Failed to open directory %s: %s", path, strerror(errno));
        return false;
    }
    
    /* Free existing subdirectory entries */
    dir_entry_t *current = dir->subdirs;
    while (current) {
        dir_entry_t *to_free = current;
        current = current->next;
        free(to_free);
    }
    
    dir->subdirs = NULL;
    dir->subdir_count = 0;
    
    /* Scan for subdirectories */
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
        
        /* Add subdirectories to cache */
        if (is_directory(full_path)) {
            if (!add_subdir(dir, full_path)) {
                log_message(LOG_WARNING, "Failed to add subdirectory %s to cache", full_path);
            }
        }
    }
    
    closedir(dirp);
    
    /* Update directory metadata */
    dir->mtime = get_mtime(path);
    dir->validated = true;
    
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
            log_message(LOG_DEBUG, "Directory %s has changed (mtime: %ld -> %ld), updating cache", 
                       path, dir->mtime, current_mtime);
            
            /* Update cache */
            if (!scan_directory(path, dir)) {
                return false;
            }
            
            *changed = true;
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
        
        /* Scan directory */
        if (!scan_directory(path, dir)) {
            return false;
        }
        
        *changed = true;
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