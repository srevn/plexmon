#ifndef QUEUE_H
#define QUEUE_H

#include <stdbool.h>

/* Queue configuration */
#define PATH_MAX_LEN 1024                  /* Maximum length for filesystem paths */

/* Node structure for the directory queue linked list */
typedef struct node {
	char path[PATH_MAX_LEN];               /* Full path to directory in queue */
	struct node *next;                     /* Pointer to next node in linked list */
} node_t;

/* Queue structure for breadth-first directory traversal */
typedef struct {
	node_t *front;                         /* Pointer to front of queue (dequeue from here) */
	node_t *rear;                          /* Pointer to rear of queue (enqueue to here) */
} queue_t;

/* Queue lifecycle management */
void queue_init(queue_t *queue);
void queue_free(queue_t *queue);

/* Queue operations */
bool queue_enqueue(queue_t *queue, const char *path);
bool queue_dequeue(queue_t *queue, char *path);
bool queue_is_empty(queue_t *queue);

#endif /* QUEUE_H */