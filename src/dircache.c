#include "dircache.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
			kh_destroy(subdir_map, dir->subdirs);
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

/* Scans a directory on disk, updates the cache using generational marking */
static bool dircache_sweep(const char *path, cached_dir_t *dir, dir_changes_t *changes) {
	DIR *dirp;
	struct dirent *entry;
	bool changed = false; /* Tracks if cache structure was modified */

	/* Dynamic buffer for constructing full paths */
	char *full_path = NULL;
	size_t full_path_cap = 0;
	size_t path_len = strlen(path);

	int skipped_symlinks = 0;
	int skipped_unknown = 0;

	if (!(dirp = opendir(path))) {
		log_message(LOG_ERR, "Failed to open directory %s: %s", path, strerror(errno));
		return false;
	}

	/* Scan the directory on disk */
	while ((entry = readdir(dirp))) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}

		/* Skip symlinks to avoid stat() calls */
		if (entry->d_type == DT_LNK) {
			skipped_symlinks++;
			continue;
		}

		/* Warn about filesystems that don't provide d_type */
		if (entry->d_type == DT_UNKNOWN) {
			skipped_unknown++;
		}

		size_t name_len = strlen(entry->d_name);
		size_t required_len = path_len + name_len + 2; /* for '/' and '\0' */
		/* Grow buffer only when needed - amortized O(1) across all entries */
		if (required_len > full_path_cap) {
			char *new_buf = realloc(full_path, required_len);
			if (!new_buf) {
				log_message(LOG_WARNING, "Failed to allocate memory for full path buffer");
				continue;
			}
			full_path = new_buf;
			full_path_cap = required_len;
		}
		sprintf(full_path, "%s/%s", path, entry->d_name);

		if (!is_directory(full_path, entry->d_type)) {
			continue;
		}

		/* Check if directory already exists in cache */
		khint_t k = kh_get(subdir_map, dir->subdirs, full_path);

		if (k != kh_end(dir->subdirs)) {
			/* Exists: update generation */
			kh_value(dir->subdirs, k) = dir->generation;
			continue;
		}

		/* Pre-check changes capacity if we are tracking changes */
		if (changes) {
			if (changes->added_count >= changes->added_capacity) {
				int new_cap = changes->added_capacity == 0 ? 16 : changes->added_capacity * 2;
				const char **new_list = realloc((void *) changes->added, new_cap * sizeof(char *));
				if (!new_list) {
					log_message(LOG_ERR, "Failed to realloc - aborting add of %s", full_path);
					continue; /* Skip adding this directory to cache to maintain consistency */
				}
				changes->added = new_list;
				changes->added_capacity = new_cap;
			}
		}

		/* It's a new directory, add it to the cache */
		char *key = strdup(full_path);
		if (!key) {
			log_message(LOG_WARNING, "Failed to allocate memory for subdirectory key");
			continue;
		}

		/* Insert into hash table */
		int ret;
		k = kh_put(subdir_map, dir->subdirs, key, &ret);
		if (ret == -1) {
			log_message(LOG_WARNING, "Failed to insert key into hash map");
			free(key);
			continue;
		}

		/* Set generation and track change */
		kh_value(dir->subdirs, k) = dir->generation;
		changed = true;

		if (changes) {
			changes->added[changes->added_count++] = key;
		}
	}
	closedir(dirp);
	free(full_path);

	if (skipped_symlinks > 0) {
		log_message(LOG_DEBUG, "Skipped %d symlinks in %s", skipped_symlinks, path);
	}

	if (skipped_unknown > 0) {
		log_message(LOG_WARNING, "Encountered %d entries with DT_UNKNOWN in %s",
					skipped_unknown, path);
	}

	return changed;
}

/* Reaps directories that were not marked in the current generation */
static bool dircache_reap(cached_dir_t *dir, dir_changes_t *changes) {
	bool changed = false;
	khint_t k;

	/* Iterate over all subdirectories */
	for (k = kh_begin(dir->subdirs); k != kh_end(dir->subdirs);) {
		if (!kh_exist(dir->subdirs, k)) {
			++k;
			continue;
		}

		/* If generation doesn't match, it wasn't seen in the last sweep */
		if (kh_value(dir->subdirs, k) != dir->generation) {
			const char *key_to_del = kh_key(dir->subdirs, k);

			if (changes) {
				/* Grow removed list if needed */
				if (changes->removed_count >= changes->removed_capacity) {
					int new_cap = changes->removed_capacity == 0 ? 16 : changes->removed_capacity * 2;
					const char **new_list = realloc((void *) changes->removed, new_cap * sizeof(char *));
					if (!new_list) {
						log_message(LOG_WARNING, "Failed to realloc - deletion not reported");
						/* We still remove it from cache to keep cache clean, but monitor won't know. */
					} else {
						changes->removed = new_list;
						changes->removed_capacity = new_cap;
					}
				}

				if (changes->removed_count < changes->removed_capacity) {
					char *key_copy = strdup(key_to_del);
					if (key_copy) {
						changes->removed[changes->removed_count++] = key_copy;
					}
				}
			}

			/* Remove from hash */
			free((void *) key_to_del);
			kh_del(subdir_map, dir->subdirs, k);
			changed = true;
		}
		++k;
	}

	return changed;
}

/* Check if directory structure has changed and updates cache */
static bool dircache_sync(const char *path, cached_dir_t *dir, bool *changed, dir_changes_t *changes) {
	time_t start_mtime, end_mtime;

	*changed = false;
	if (changes) {
		/* Initialize changes struct */
		changes->added = NULL;
		changes->added_count = 0;
		changes->added_capacity = 0;
		changes->removed = NULL;
		changes->removed_count = 0;
		changes->removed_capacity = 0;
	}

	start_mtime = dircache_mtime(path);
	if (start_mtime == 0) {
		log_message(LOG_ERR, "Failed to get mtime for %s", path);
		return false;
	}

	/* Ensure the primary subdirs hash map exists */
	if (!dir->subdirs) {
		dir->subdirs = kh_init(subdir_map);
		if (!dir->subdirs) {
			log_message(LOG_ERR, "Failed to create subdirectory hash map");
			return false;
		}
	}

	/* Increment generation for this sync */
	dir->generation++;

	/* Sweep: Scan disk for new/existing dirs, marking them with current generation */
	bool added = dircache_sweep(path, dir, changes);

	/* Reap: Remove dirs that have old generation */
	bool removed = dircache_reap(dir, changes);

	*changed = added || removed;

	if (*changed) {
		log_message(LOG_DEBUG, "Directory structure in %s has changed", path);
	}

	/* Check if directory was modified during scan */
	end_mtime = dircache_mtime(path);
	if (end_mtime != start_mtime) {
		log_message(LOG_DEBUG, "Directory %s modified during scan, forcing refresh", path);
		*changed = true;
	}

	dir->validated = true;
	dir->mtime = start_mtime;

	return true;
}

/* Adds a new directory to the cache and performs an initial sync */
static bool dircache_add(const char *path, bool *changed, dir_changes_t *changes) {
	cached_dir_t *dir = malloc(sizeof(cached_dir_t));
	if (!dir) {
		log_message(LOG_ERR, "Failed to allocate memory for directory cache");
		return false;
	}

	/* Initialize new cache entry */
	dir->mtime = 0;
	dir->subdirs = NULL;
	dir->validated = false;
	dir->generation = 0;

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
	if (!dircache_sync(path, dir, changed, changes)) {
		/* Clean up on failure to avoid stale cache entries */
		kh_del(dir_cache, cache_hash, k);
		free(key_copy);
		free(dir);
		return false;
	}

	log_message(LOG_DEBUG, "Directory %s added to cache", path);
	return true;
}

/* Check if directory has changed and update cache if needed */
bool dircache_refresh(const char *path, bool *changed, dir_changes_t *changes) {
	cached_dir_t *dir;
	time_t current_mtime;

	*changed = false;
	if (changes) {
		changes->added = NULL;
		changes->added_count = 0;
		changes->removed = NULL;
		changes->removed_count = 0;
	}

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
			log_message(LOG_DEBUG, "Directory %s has changed (mtime: %ld -> %ld), checking structure",
						path, dir->mtime, current_mtime);

			/* Check and update directory structure in one pass */
			return dircache_sync(path, dir, changed, changes);
		} else {
			log_message(LOG_DEBUG, "Directory %s unchanged, using cached data", path);
		}
	} else {
		/* Directory not in cache, add it and sync */
		return dircache_add(path, changed, changes);
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
	int size = kh_size(dir->subdirs);
	if (size == 0) {
		return NULL;
	}

	/* Allocate array of strings (plus one for NULL terminator) */
	subdirs_array = malloc((size + 1) * sizeof(char *));
	if (!subdirs_array) {
		log_message(LOG_ERR, "Failed to allocate memory for subdirectory list");
		return NULL;
	}

	/* Fill array with copies of subdirectory paths */
	int i = 0;
	khint_t k;
	for (k = kh_begin(dir->subdirs); k != kh_end(dir->subdirs); ++k) {
		if (!kh_exist(dir->subdirs, k)) {
			continue;
		}
		subdirs_array[i] = strdup(kh_key(dir->subdirs, k));
		if (!subdirs_array[i]) {
			log_message(LOG_ERR, "Failed to duplicate subdirectory string");
			/* Cleanup partial allocation */
			for (int j = 0; j < i; j++) {
				free(subdirs_array[j]);
			}
			free(subdirs_array);
			return NULL;
		}
		i++;
	}
	subdirs_array[i] = NULL; /* NULL terminate */
	*count = i;

	return subdirs_array;
}

/* Free subdirectory list */
void dircache_free(char **subdirs) {
	if (!subdirs) return;

	for (int i = 0; subdirs[i] != NULL; i++) {
		free(subdirs[i]);
	}
	free(subdirs);
}

/* Free directory changes structure */
void changes_free(dir_changes_t *changes) {
	if (!changes) return;

	/* The 'added' list contains pointers to keys owned by the cache */
	free((void *) changes->added);
	changes->added = NULL;
	changes->added_count = 0;
	changes->added_capacity = 0;

	/* The 'removed' list contains heap-allocated copies of keys */
	if (changes->removed) {
		for (int i = 0; i < changes->removed_count; i++) {
			free((void *) changes->removed[i]);
		}
		free((void *) changes->removed);
	}
	changes->removed = NULL;
	changes->removed_count = 0;
	changes->removed_capacity = 0;
}
