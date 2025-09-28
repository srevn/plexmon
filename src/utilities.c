#include "utilities.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/dirent.h>
#include <sys/stat.h>

#include "logger.h"

/* Check if a path is a directory, using d_type for optimization */
bool is_directory(const char *path, int d_type) {
	/* If d_type is available, use it for a fast path */
	if (d_type != D_TYPE_UNAVAILABLE) {
		if (d_type == DT_DIR) {
			return true;
		}
		/* For symlinks or unknown types, we must stat to be sure */
		if (d_type != DT_LNK && d_type != DT_UNKNOWN) {
			return false; /* It's a file, socket, etc. */
		}
	}

	/* Fallback to stat() if type is unavailable, unknown, or a symlink */
	struct stat st;
	if (stat(path, &st) == -1) {
		/* ENOENT is not a critical error in some contexts (e.g., deleted file) */
		if (errno != ENOENT) {
			log_message(LOG_ERR, "Failed to stat %s: %s", path, strerror(errno));
		}
		return false;
	}

	return S_ISDIR(st.st_mode);
}
