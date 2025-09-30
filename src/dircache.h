#ifndef DIRCACHE_H
#define DIRCACHE_H

#include <stdbool.h>
#include <time.h>

#include "../lib/khash.h"

KHASH_SET_INIT_STR(str_set)            /* Define a hash set of strings */

/* Structure to represent a cached directory with metadata */
typedef struct cached_dir {
	time_t mtime;                      /* Last modification time from stat() */
	khash_t(str_set) * subdirs;        /* Hash set of subdirectories for fast lookups */
	bool validated;                    /* Whether the cache entry is up-to-date */
} cached_dir_t;

/* Structure to track directory changes for efficient monitoring */
typedef struct dir_changes {
	const char **added;                /* Array of added subdirectory paths */
	int added_count;                   /* Number of added subdirectories */
	const char **removed;              /* Array of removed subdirectory paths */
	int removed_count;                 /* Number of removed subdirectories */
} dir_changes_t;

/* Directory cache lifecycle management */
bool dircache_init(void);
void dircache_cleanup(void);

/* Directory cache operations */
bool dircache_refresh(const char *path, bool *changed, dir_changes_t *changes);
const char **dircache_subdirs(const char *path, int *count);
void dircache_free(const char **subdirs);
void dircache_free_changes(dir_changes_t *changes);

#endif /* DIRCACHE_H */
