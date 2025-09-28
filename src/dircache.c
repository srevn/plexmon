#include "dircache.h"

#include <dirent.h>
#include <errno.h>
#include <search.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "config.h"
#include "logger.h"
#include "utilities.h"

/* Hash table for directory cache */
static struct hsearch_data dir_cache_htab;
static int dir_cache_count = 0;

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

		/* Free all data associated with the cached directory */
		cached_dir_t *dir = to_free->dir;
		if (dir) {
			/* Free all the keys in the subdirectory hash table */
			for (int i = 0; i < dir->subdir_count; i++) {
				free(dir->keys[i]);
			}
			free(dir->keys);

			/* Destroy the subdirectory hash table */
			if (dir->validated) {
				hdestroy_r(&dir->subdirs_htab);
			}
		}

		/* Free directory entry and the main cache key */
		free(to_free->key);
		free(to_free->dir);
		free(to_free);
	}

	/* Destroy the main directory cache hash table */
	hdestroy_r(&dir_cache_htab);
	dir_cache_count = 0;
}

/* Get file modification time */
static time_t get_mtime(const char *path) {
	struct stat st;

	if (stat(path, &st) != 0) {
		return 0; /* If we can't stat, return 0 to force refresh */
	}

	return st.st_mtime;
}

/* Find a directory in the cache */
static cached_dir_t *find_dir(const char *path) {
	ENTRY item, *result;

	/* Set up the search key */
	item.key = (char *) path; /* hsearch_r doesn't modify the key */

	/* Search in hash table */
	if (hsearch_r(item, FIND, &result, &dir_cache_htab)) {
		return (cached_dir_t *) result->data;
	}

	return NULL;
}

/* Helper function to manage the dynamic keys array for a cached directory */
static bool add_key_to_dir(cached_dir_t *dir, char *key) {
	if (dir->subdir_count >= dir->keys_capacity) {
		int new_capacity = (dir->keys_capacity == 0) ? 16 : dir->keys_capacity * 2;
		char **new_keys = realloc(dir->keys, new_capacity * sizeof(char *));
		if (!new_keys) {
			log_message(LOG_ERR, "Failed to reallocate keys array for dircache");
			free(key); /* Avoid leaking the key we failed to add */
			return false;
		}
		dir->keys = new_keys;
		dir->keys_capacity = new_capacity;
	}
	dir->keys[dir->subdir_count++] = key;
	return true;
}

/* Check if directory structure has changed and updates cache */
static bool dircache_sync(const char *path, cached_dir_t *dir, bool *changed) {
	DIR *dirp;
	struct dirent *entry;
	char full_path[PATH_MAX_LEN];
	*changed = false;

	/* Temporary hash table for the current state on disk */
	struct hsearch_data current_htab;
	memset(&current_htab, 0, sizeof(current_htab));
	if (!hcreate_r(512, &current_htab)) {
		log_message(LOG_ERR, "Failed to create temporary hash table for sync: %s", strerror(errno));
		return false;
	}

	char *current_keys[HASH_TABLE_SIZE] = { NULL };
	int current_key_count = 0;
	bool structure_changed = false;

	if (!(dirp = opendir(path))) {
		log_message(LOG_ERR, "Failed to open directory %s: %s", path, strerror(errno));
		hdestroy_r(&current_htab);
		return false;
	}

	/* Scan disk and populate the temporary hash table */
	while ((entry = readdir(dirp))) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}

		snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
		if (is_directory(full_path, entry->d_type)) {
			char *key = strdup(full_path);
			if (!key) continue;

			if (current_key_count < HASH_TABLE_SIZE) {
				current_keys[current_key_count++] = key;
			} else {
				log_message(LOG_WARNING, "Exceeded max subdirectories for temp key tracking");
				free(key);
				continue;
			}

			ENTRY item = { key, (void *) 1 };
			ENTRY *result;
			hsearch_r(item, ENTER, &result, &current_htab);
		}
	}
	closedir(dirp);

	/* Check for changes between cache and disk */
	if (dir->validated) {
		/* Check for count mismatch first for a quick exit */
		if (dir->subdir_count != current_key_count) {
			structure_changed = true;
		} else {
			/* If counts match, check for any new directories */
			for (int i = 0; i < current_key_count; i++) {
				ENTRY search_item = { current_keys[i], NULL };
				ENTRY *result = NULL;
				if (!hsearch_r(search_item, FIND, &result, &dir->subdirs_htab)) {
					structure_changed = true;
					break;
				}
			}
		}
	} else {
		/* Not validated, so always update */
		structure_changed = true;
	}

	/* If changed, rebuild the cache entry from the disk scan */
	if (structure_changed) {
		log_message(LOG_DEBUG, "Directory structure in %s has changed, updating cache", path);
		*changed = true;

		/* Free old cache contents if they existed */
		if (dir->validated) {
			for (int i = 0; i < dir->subdir_count; i++) {
				free(dir->keys[i]);
			}
			free(dir->keys);
			hdestroy_r(&dir->subdirs_htab);
		}

		/* Initialize new cache contents */
		dir->keys = NULL;
		dir->subdir_count = 0;
		dir->keys_capacity = 0;
		memset(&dir->subdirs_htab, 0, sizeof(dir->subdirs_htab));
		if (!hcreate_r(512, &dir->subdirs_htab)) {
			log_message(LOG_ERR, "Failed to create replacement hash table for cache: %s", strerror(errno));
			/* Must free temporary keys before returning */
			for (int i = 0; i < current_key_count; i++) free(current_keys[i]);
			hdestroy_r(&current_htab);
			return false;
		}

		/* Populate the new cache with the data from the disk scan */
		for (int i = 0; i < current_key_count; i++) {
			ENTRY item = { current_keys[i], (void *) 1 };
			ENTRY *result;
			if (hsearch_r(item, ENTER, &result, &dir->subdirs_htab)) {
				/* Key is now owned by the cache, add it to the tracking array */
				add_key_to_dir(dir, current_keys[i]);
			} else {
				/* Failed to insert, so we must free this key */
				free(current_keys[i]);
			}
		}
		dir->validated = true;

	} else {
		log_message(LOG_DEBUG, "Directory structure in %s unchanged", path);
		*changed = false;
		/* Free the temporary keys, as they are not being transferred to the cache */
		for (int i = 0; i < current_key_count; i++) {
			free(current_keys[i]);
		}
	}

	/* Final cleanup */
	dir->mtime = get_mtime(path);
	hdestroy_r(&current_htab);

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
			return dircache_sync(path, dir, changed);
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
		dir->mtime = 0;
		dir->keys = NULL;
		dir->subdir_count = 0;
		dir->keys_capacity = 0;
		dir->validated = false;
		memset(&dir->subdirs_htab, 0, sizeof(dir->subdirs_htab));

		/* Add to hash table */
		ENTRY item, *result;
		char *key_copy = strdup(path); /* Must allocate a copy for the key */
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
		if (!dircache_sync(path, dir, changed)) {
			/* The entry will be cleaned up during dircache_cleanup */
			return false;
		}

		log_message(LOG_DEBUG, "Directory %s added to cache", path);
	}

	return true;
}

/* Get subdirectories from cache */
char **dircache_subdirs(const char *path, int *count) {
	cached_dir_t *dir;
	char **subdirs;

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
	for (int i = 0; i < dir->subdir_count; i++) {
		subdirs[i] = strdup(dir->keys[i]);
		if (!subdirs[i]) {
			/* Clean up on failure */
			for (int j = 0; j < i; j++) {
				free(subdirs[j]);
			}
			free(subdirs);
			log_message(LOG_ERR, "Failed to allocate memory for subdirectory path");
			return NULL;
		}
	}

	*count = dir->subdir_count;
	return subdirs;
}

/* Free subdirectory list */
void dircache_free(char **subdirs, int count) {
	if (!subdirs) return;

	for (int i = 0; i < count; i++) {
		free(subdirs[i]);
	}

	free(subdirs);
}
