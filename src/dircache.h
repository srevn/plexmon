#ifndef DIRCACHE_H
#define DIRCACHE_H

#include <search.h>
#include <stdbool.h>
#include <time.h>

/* Directory cache configuration */
#define HASH_TABLE_SIZE 4096               /* Number of hash table buckets for directory cache */
#define PATH_MAX_LEN 1024                  /* Maximum length for filesystem paths */

/* Structure to represent a cached directory with metadata */
typedef struct cached_dir {
	char path[PATH_MAX_LEN];               /* Full path to the cached directory */
	time_t mtime;                          /* Last modification time from stat() */
	char **keys;                           /* Dynamically allocated array of keys for cleanup */
	struct hsearch_data subdirs_htab;      /* Hash table of subdirectories for fast lookups */
	int subdir_count;                      /* Total number of subdirectories found */
	int keys_capacity;                     /* Capacity of the keys array */
	bool validated;                        /* Whether the cache entry is up-to-date */
} cached_dir_t;

/* Cache entry tracker for memory management and cleanup */
typedef struct cache_tracker {
	char *key;                             /* Allocated string for hash table key */
	cached_dir_t *dir;                     /* Pointer to the cached directory entry */
	struct cache_tracker *next;            /* Next entry in the tracker linked list */
} cache_tracker_t;

/* Directory cache lifecycle management */
bool dircache_init(void);
void dircache_cleanup(void);

/* Directory cache operations */
bool dircache_refresh(const char *path, bool *changed);
char **dircache_subdirs(const char *path, int *count);
void dircache_free(char **subdirs, int count);

#endif /* DIRCACHE_H */
