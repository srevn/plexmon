#include "dircache.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "logger.h"
#include "utilities.h"

KHASH_MAP_INIT_STR(dir_cache, cached_dir_t *) /* Main hash map from string to cached_dir_t* */
static khash_t(dir_cache) * cache_hash;		  /* Hash table for directory cache */

/* Initialize the directory cache */
bool dircache_init(void) {
	log_message(LOG_INFO, "Initializing directory cache with hash table");
	cache_hash = kh_init(dir_cache);
	if (!cache_hash) {
		log_message(LOG_ERR, "Failed to create directory cache hash table");
		return false;
	}
	return true;
}

/* Clean up the directory cache */
void dircache_cleanup(void) {
	if (!cache_hash) {
		return;
	}

	log_message(LOG_INFO, "Cleaning up directory cache");

	khint_t k;
	for (k = kh_begin(cache_hash); k != kh_end(cache_hash); ++k) {
		if (!kh_exist(cache_hash, k)) {
			continue;
		}

		const char *path_key = kh_key(cache_hash, k);
		cached_dir_t *dir = kh_value(cache_hash, k);

		if (dir && dir->subdirs) {
			khint_t sub_k;
			for (sub_k = kh_begin(dir->subdirs); sub_k != kh_end(dir->subdirs); ++sub_k) {
				if (kh_exist(dir->subdirs, sub_k)) {
					free((void *) kh_key(dir->subdirs, sub_k));
				}
			}
			kh_destroy(str_set, dir->subdirs);
		}
		free(dir);
		free((void *) path_key);
	}

	kh_destroy(dir_cache, cache_hash);
	cache_hash = NULL;
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
	if (!cache_hash) return NULL;
	khint_t k = kh_get(dir_cache, cache_hash, path);
	if (k == kh_end(cache_hash)) {
		return NULL;
	}
	return kh_value(cache_hash, k);
}

/* Check if directory structure has changed and updates cache */
static bool dircache_sync(const char *path, cached_dir_t *dir, bool *changed) {
	DIR *dirp;
	struct dirent *entry;
	char full_path[PATH_MAX_LEN];

	if (!(dirp = opendir(path))) {
		log_message(LOG_ERR, "Failed to open directory %s: %s", path, strerror(errno));
		*changed = false;
		return false;
	}

	/* Create a new hash set for the subdirectories found on disk */
	khash_t(str_set) *new_subdirs = kh_init(str_set);
	if (!new_subdirs) {
		log_message(LOG_ERR, "Failed to create temporary hash set for sync: %s", strerror(errno));
		closedir(dirp);
		return false;
	}

	/* Scan disk and populate the new hash set */
	while ((entry = readdir(dirp))) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}

		snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
		if (is_directory(full_path, entry->d_type)) {
			char *key = strdup(full_path);
			if (!key) {
				log_message(LOG_WARNING, "Failed to allocate memory for subdirectory key");
				continue;
			}
			int ret;
			kh_put(str_set, new_subdirs, key, &ret);
			if (ret == -1) {
				log_message(LOG_WARNING, "Failed to insert key into temporary hash set");
				free(key);
			}
		}
	}
	closedir(dirp);

	/* Check for changes between the old cache and the new state from disk */
	*changed = false;
	if (!dir->validated || !dir->subdirs || kh_size(dir->subdirs) != kh_size(new_subdirs)) {
		*changed = true;
	} else {
		/* If counts match, check if all new keys exist in the old cache */
		khint_t k;
		for (k = kh_begin(new_subdirs); k != kh_end(new_subdirs); ++k) {
			if (kh_exist(new_subdirs, k)) {
				const char *key = kh_key(new_subdirs, k);
				khint_t old_k = kh_get(str_set, dir->subdirs, key);
				if (old_k == kh_end(dir->subdirs)) {
					*changed = true;
					break;
				}
			}
		}
	}

	/* If changed, replace the old cache with the new state */
	if (*changed) {
		log_message(LOG_DEBUG, "Directory structure in %s has changed, updating cache", path);

		/* Free old cache contents if they existed */
		if (dir->validated && dir->subdirs) {
			khint_t k;
			for (k = kh_begin(dir->subdirs); k != kh_end(dir->subdirs); ++k) {
				if (kh_exist(dir->subdirs, k)) {
					free((void *) kh_key(dir->subdirs, k));
				}
			}
			kh_destroy(str_set, dir->subdirs);
		}

		/* Promote the new state to be the current cache state */
		dir->subdirs = new_subdirs;

	} else {
		log_message(LOG_DEBUG, "Directory structure in %s unchanged", path);

		/* Discard the new state structures as they are not needed */
		khint_t k;
		for (k = kh_begin(new_subdirs); k != kh_end(new_subdirs); ++k) {
			if (kh_exist(new_subdirs, k)) {
				free((void *) kh_key(new_subdirs, k));
			}
		}
		kh_destroy(str_set, new_subdirs);
	}

	dir->validated = true;
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
		dir->mtime = 0;
		dir->subdirs = NULL;
		dir->validated = false;

		/* Add to hash table */
		char *key_copy = strdup(path); /* Must allocate a copy for the key */
		if (!key_copy) {
			log_message(LOG_ERR, "Failed to allocate memory for hash table key");
			free(dir);
			return false;
		}

		int ret;
		khint_t k = kh_put(dir_cache, cache_hash, key_copy, &ret);
		if (ret == -1) {
			log_message(LOG_ERR, "Failed to add directory to hash table");
			free(key_copy);
			free(dir);
			return false;
		}
		kh_value(cache_hash, k) = dir;

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
	char **subdirs_array;

	*count = 0;

	/* Find directory in cache */
	dir = dircache_find(path);
	if (!dir || !dir->validated || !dir->subdirs) {
		return NULL;
	}

	/* If no subdirectories, return NULL */
	*count = kh_size(dir->subdirs);
	if (*count == 0) {
		return NULL;
	}

	/* Allocate array of strings */
	subdirs_array = malloc(*count * sizeof(char *));
	if (!subdirs_array) {
		log_message(LOG_ERR, "Failed to allocate memory for subdirectory list");
		*count = 0;
		return NULL;
	}

	/* Fill array with subdirectory paths */
	int i = 0;
	khint_t k;
	for (k = kh_begin(dir->subdirs); k != kh_end(dir->subdirs); ++k) {
		if (!kh_exist(dir->subdirs, k)) {
			continue;
		}

		subdirs_array[i] = strdup(kh_key(dir->subdirs, k));
		if (!subdirs_array[i]) {
			/* Clean up on failure */
			for (int j = 0; j < i; j++) {
				free(subdirs_array[j]);
			}
			free(subdirs_array);
			log_message(LOG_ERR, "Failed to allocate memory for subdirectory path");
			*count = 0;
			return NULL;
		}
		i++;
	}

	return subdirs_array;
}

/* Free subdirectory list */
void dircache_free(char **subdirs, int count) {
	if (!subdirs) return;

	for (int i = 0; i < count; i++) {
		free(subdirs[i]);
	}

	free(subdirs);
}
