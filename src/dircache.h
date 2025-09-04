#ifndef DIRCACHE_H
#define DIRCACHE_H

#include <stdbool.h>
#include <time.h>

/* Directory cache configuration */
#define HASH_TABLE_SIZE 4096               /* Number of hash table buckets for directory cache */
#define PATH_MAX_LEN 1024                  /* Maximum length for filesystem paths */

/* Structure to represent a subdirectory entry in linked list */
typedef struct dir_entry {
	char path[PATH_MAX_LEN];               /* Full path to the subdirectory */
	struct dir_entry *next;                /* Pointer to next entry in linked list */
} dir_entry_t;

/* Structure to represent a cached directory with metadata */
typedef struct cached_dir {
	char path[PATH_MAX_LEN];               /* Full path to the cached directory */
	time_t mtime;                          /* Last modification time from stat() */
	dir_entry_t *subdirs;                  /* Linked list of subdirectories */
	int subdir_count;                      /* Total number of subdirectories found */
	bool validated;                        /* Whether the cache entry is up-to-date */
} cached_dir_t;

/* Cache entry tracker for memory management and cleanup */
typedef struct cache_entry_tracker {
	char *key;                             /* Allocated string for hash table key */
	cached_dir_t *dir;                     /* Pointer to the cached directory entry */
	struct cache_entry_tracker *next;      /* Next entry in the tracker linked list */
} cache_entry_tracker_t;

/* Directory cache lifecycle management */
bool dircache_init(void);
void dircache_cleanup(void);

/* Directory cache operations */
bool dircache_refresh(const char *path, bool *changed);
char **dircache_subdirs(const char *path, int *count);
void dircache_free(char **subdirs, int count);

#endif /* DIRCACHE_H */
