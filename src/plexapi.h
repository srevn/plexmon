#ifndef PLEXAPI_H
#define PLEXAPI_H

#include <stdbool.h>
#include <stddef.h>

/* Structure for HTTP response data from curl */
typedef struct {
	char *data;                            /* Response data buffer */
	size_t size;                           /* Size of response data in bytes */
} curl_response_t;

/* Plex API lifecycle management */
bool plexapi_init(void);
void plexapi_cleanup(void);

/* Plex server communication */
bool plexapi_check(void);
bool plexapi_libraries(void);

/* Library scanning operations */
bool plexapi_scan(const char *path, int section_id);

#endif /* PLEXAPI_H */
