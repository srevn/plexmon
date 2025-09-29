#include "queue.h"

#include <stdlib.h>
#include <string.h>

/* Initialize an empty queue */
void queue_init(queue_t *queue) {
	queue->front = NULL;
	queue->rear = NULL;
}

/* Add an item to the queue (enqueue) */
bool queue_enqueue(queue_t *queue, const char *path) {
	size_t path_len = strlen(path);
	node_t *new_node = malloc(sizeof(node_t) + path_len + 1);
	if (!new_node) {
		return false;
	}

	strcpy(new_node->path, path);
	new_node->next = NULL;

	if (queue->rear == NULL) {
		/* Empty queue */
		queue->front = new_node;
		queue->rear = new_node;
	} else {
		/* Add to the end */
		queue->rear->next = new_node;
		queue->rear = new_node;
	}

	return true;
}

/* Remove an item from the queue (dequeue) */
char *queue_dequeue(queue_t *queue) {
	if (queue->front == NULL) {
		return NULL;
	}

	node_t *temp = queue->front;
	char *path = strdup(temp->path);

	queue->front = queue->front->next;

	if (queue->front == NULL) {
		queue->rear = NULL;
	}

	free(temp);
	return path;
}

/* Free all nodes in the queue */
void queue_free(queue_t *queue) {
	node_t *current, *next;

	current = queue->front;
	while (current) {
		next = current->next;
		free(current);
		current = next;
	}

	queue->front = NULL;
	queue->rear = NULL;
}

/* Check if queue is empty */
bool queue_empty(queue_t *queue) {
	return queue->front == NULL;
}
