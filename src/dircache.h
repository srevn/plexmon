#ifndef DIRCACHE_H
#define DIRCACHE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "../lib/khash.h"

KHASH_MAP_INIT_STR(subdir_map, uint32_t) /* Define a hash map of strings to integers (generation IDs) */

/* Structure to represent a cached directory with metadata */
typedef struct cached_dir {
	time_t mtime;                      /* Last modification time from stat() */
	khash_t(subdir_map) * subdirs;     /* Hash map of subdirectories for fast lookups */
	uint32_t generation;               /* Generation counter for mark-and-sweep */
	bool validated;                    /* Whether the cache entry is up-to-date */
} cached_dir_t;

/* Structure to track directory changes for efficient monitoring */
typedef struct dir_changes {
	const char **added;                /* Array of added subdirectory paths */
	int added_count;                   /* Number of added subdirectories */
	int added_capacity;                /* Internal: allocated capacity of `added` */
	const char **removed;              /* Array of removed subdirectory paths */
	int removed_count;                 /* Number of removed subdirectories */
	int removed_capacity;              /* Internal: allocated capacity of `removed` */
} dir_changes_t;

/* Directory cache lifecycle management */
bool dircache_init(void);
void dircache_cleanup(void);

/* Directory cache operations */
bool dircache_refresh(const char *path, bool *changed, dir_changes_t *changes);
const char **dircache_subdirs(const char *path, int *count);
void dircache_release(const char **subdirs);
void changes_free(dir_changes_t *changes);

#endif /* DIRCACHE_H */
