#ifndef UTILITIES_H
#define UTILITIES_H

#include <stdbool.h>

/* Pass D_TYPE_UNAVAILABLE if d_type is not known from readdir() */
#define D_TYPE_UNAVAILABLE -1

/* Filesystem utility functions */
bool is_directory(const char *path, int d_type);

#endif /* UTILITIES_H */
