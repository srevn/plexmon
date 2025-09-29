#ifndef UTILITIES_H
#define UTILITIES_H

#include <stdbool.h>

#define D_TYPE_UNAVAILABLE -1       /* If d_type is not known from readdir() */

/* Filesystem utility functions */
bool is_directory(const char *path, int d_type);

#endif /* UTILITIES_H */
