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
	node_t *new_node = malloc(sizeof(node_t));
	if (!new_node) {
		return false;
	}

	strncpy(new_node->path, path, PATH_MAX_LEN - 1);
	new_node->path[PATH_MAX_LEN - 1] = '\0';
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
bool queue_dequeue(queue_t *queue, char *path) {
	if (queue->front == NULL) {
		return false;
	}

	node_t *temp = queue->front;

	strncpy(path, temp->path, PATH_MAX_LEN - 1);
	path[PATH_MAX_LEN - 1] = '\0';

	queue->front = queue->front->next;

	if (queue->front == NULL) {
		queue->rear = NULL;
	}

	free(temp);
	return true;
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
bool queue_is_empty(queue_t *queue) {
	return queue->front == NULL;
}
