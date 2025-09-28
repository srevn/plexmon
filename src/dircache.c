#include "dircache.h"

#include <dirent.h>
#include <errno.h>
#include <search.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "logger.h"
#include "utilities.h"

/* Hash table for directory cache */
static struct hsearch_data dir_cache_htab;
static int dir_cache_count = 0;

static cache_tracker_t *entry_tracker_head = NULL;

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
static bool dircache_track(char *key, cached_dir_t *dir) {
	cache_tracker_t *tracker = malloc(sizeof(cache_tracker_t));
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
		cache_tracker_t *to_free = entry_tracker_head;
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
static time_t dircache_mtime(const char *path) {
	struct stat st;

	if (stat(path, &st) != 0) {
		return 0; /* If we can't stat, return 0 to force refresh */
	}

	return st.st_mtime;
}

/* Find a directory in the cache */
static cached_dir_t *dircache_find(const char *path) {
	ENTRY item, *result;

	/* Set up the search key */
	item.key = (char *) path; /* hsearch_r doesn't modify the key */

	/* Search in hash table */
	if (hsearch_r(item, FIND, &result, &dir_cache_htab)) {
		return (cached_dir_t *) result->data;
	}

	return NULL;
}

/* Check if directory structure has changed and updates cache */
static bool dircache_sync(const char *path, cached_dir_t *dir, bool *changed) {
	DIR *dirp;
	struct dirent *entry;
	char full_path[PATH_MAX_LEN];
	*changed = false;

	/* Structures to hold the new state read from disk */
	struct hsearch_data new_htab;
	memset(&new_htab, 0, sizeof(new_htab));
	if (!hcreate_r(512, &new_htab)) {
		log_message(LOG_ERR, "Failed to create temporary hash table for sync: %s", strerror(errno));
		return false;
	}

	char **new_keys = NULL;
	int new_key_count = 0;
	int new_keys_capacity = 0;

	if (!(dirp = opendir(path))) {
		log_message(LOG_ERR, "Failed to open directory %s: %s", path, strerror(errno));
		hdestroy_r(&new_htab);
		return false;
	}

	/* Scan disk and populate the new state structures */
	while ((entry = readdir(dirp))) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}

		snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
		if (is_directory(full_path, entry->d_type)) {
			char *key = strdup(full_path);
			if (!key) continue;

			/* Add key to the dynamic array */
			if (new_key_count >= new_keys_capacity) {
				int new_capacity = (new_keys_capacity == 0) ? 128 : new_keys_capacity * 2;
				char **new_keys_arr = realloc(new_keys, new_capacity * sizeof(char *));
				if (!new_keys_arr) {
					log_message(LOG_WARNING, "Failed to allocate memory for temp key tracking");
					free(key);
					continue;
				}
				new_keys = new_keys_arr;
				new_keys_capacity = new_capacity;
			}
			new_keys[new_key_count++] = key;

			/* Add key to the hash table */
			ENTRY item = { key, (void *) 1 };
			ENTRY *result;
			hsearch_r(item, ENTER, &result, &new_htab);
		}
	}
	closedir(dirp);

	/* Check for changes between the old cache and the new state from disk */
	bool structure_changed = false;
	if (!dir->validated || dir->subdir_count != new_key_count) {
		structure_changed = true;
	} else {
		/* If counts match, check if all new keys exist in the old cache */
		for (int i = 0; i < new_key_count; i++) {
			ENTRY search_item = { new_keys[i], NULL };
			ENTRY *result = NULL;
			if (!hsearch_r(search_item, FIND, &result, &dir->subdirs_htab)) {
				structure_changed = true;
				break;
			}
		}
	}

	/* If changed, replace the old cache with the new state */
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

		/* Promote the new state to be the current cache state */
		dir->keys = new_keys;
		dir->subdir_count = new_key_count;
		dir->keys_capacity = new_keys_capacity;
		dir->subdirs_htab = new_htab; /* Struct copy */
		dir->validated = true;

	} else {
		log_message(LOG_DEBUG, "Directory structure in %s unchanged", path);
		*changed = false;

		/* Discard the new state structures as they are not needed */
		for (int i = 0; i < new_key_count; i++) {
			free(new_keys[i]);
		}
		free(new_keys);
		hdestroy_r(&new_htab);
	}

	dir->mtime = dircache_mtime(path);
	return true;
}

/* Check if directory has changed and update cache if needed */
bool dircache_refresh(const char *path, bool *changed) {
	cached_dir_t *dir;
	time_t current_mtime;

	*changed = false;

	/* Get current mtime */
	current_mtime = dircache_mtime(path);
	if (current_mtime == 0) {
		log_message(LOG_WARNING, "Failed to get mtime for %s", path);
		return false;
	}

	/* Check if directory is in cache */
	dir = dircache_find(path);

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
		if (!dircache_track(key_copy, dir)) {
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
	dir = dircache_find(path);
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
