#ifndef __QUEUE_H__
#define __QUEUE_H__

#include <stdint.h>
#include "dlist.h"

struct queue_entry_s;

typedef struct queue_s {
    dlist_header_t head;
    uint32_t size;
    uint32_t max_size;
    uint32_t pool_size;
    struct queue_entry_s *pool;
    dlist_header_t *pos;
} queue_t;

void
queue_init(queue_t *queue);

void
queue_clean(queue_t *queue);

void
queue_set_max_size(queue_t *queue, uint32_t max_size);

int
queue_set_pool(queue_t *queue, uint32_t pool_size);

uint32_t
queue_get_size(queue_t *queue);

bool
queue_is_empty(queue_t *queue);

bool
queue_is_full(queue_t *queue);

int
queue_enqueue(queue_t *queue, void *data);

void *
queue_dequeue(queue_t *queue);

void
queue_remove(queue_t *queue, void *data);

void
queue_seek_head(queue_t *queue);

void *
queue_get_next(queue_t *queue);
#endif //__QUEUE_H__
