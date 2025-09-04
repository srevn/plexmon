#include "utilities.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "logger.h"

/* Check if a path is a directory */
bool is_directory(const char *path) {
	struct stat st;

	if (stat(path, &st) == -1) {
		log_message(LOG_ERR, "Failed to stat %s: %s", path, strerror(errno));
		return false;
	}

	return S_ISDIR(st.st_mode);
}
