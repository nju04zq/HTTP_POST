#include <stdlib.h>
#include <stdbool.h>
#include "util.h"
#include "queue.h"
#include "dlist.h"

#define get_entry_from_header(dlist_header) \
    dlist_get_entry((dlist_header), queue_entry_t, header)

typedef struct queue_entry_s {
    dlist_header_t header;
    void *data;
    bool free_for_use;
    bool from_pool;
} queue_entry_t;

void
queue_init (queue_t *queue)
{
    memzero(queue, sizeof(queue_t));
    dlist_init(&queue->head);
}

void
queue_set_max_size (queue_t *queue, uint32_t max_size)
{
    queue->max_size = max_size;
}

int
queue_set_pool (queue_t *queue, uint32_t pool_size)
{
    queue_entry_t *pool;
    int i;

    if (queue->size > 0) {
        return -1;
    }

    if (queue->pool != NULL) {
        free(queue->pool);
    }

    queue->pool = calloc(pool_size, sizeof(queue_entry_t));
    if (queue->pool == NULL) {
        return -1;
    }

    queue->pool_size = pool_size;

    pool = queue->pool;
    for (i = 0; i < pool_size; i++) {
        pool[i].free_for_use = True;
        pool[i].from_pool = True;
    }
    return 0;
}

void
queue_clean (queue_t *queue)
{
    if (queue->pool) {
        free(queue->pool);
    }
}

uint32_t
queue_get_size (queue_t *queue)
{
    return queue->size;
}

bool
queue_is_empty (queue_t *queue)
{
    if (dlist_is_empty(&queue->head)) {
        return True;
    } else {
        return False;
    }
}

bool
queue_is_full (queue_t *queue)
{
    if (queue->max_size == 0) {
        return False;
    }
    if (queue->size == queue->max_size) {
        return True;
    } else {
        return False;
    }
}

static queue_entry_t *
get_queue_entry_from_pool (queue_t *queue)
{
    queue_entry_t *pool;
    int i;

    pool = queue->pool;
    for (i = 0; i < queue->pool_size; i++) {
        if (!pool[i].free_for_use) {
            continue;
        }
        pool[i].free_for_use = False;
        return &pool[i];
    }
    return NULL;
}

static queue_entry_t *
get_queue_entry (queue_t *queue)
{
    queue_entry_t *entry = NULL;

    if (queue->pool != NULL) {
        entry = get_queue_entry_from_pool(queue);
    }
    if (entry != NULL) {
        return entry;
    }

    entry = calloc(1, sizeof(queue_entry_t));
    if (entry == NULL) {
        return NULL;
    }
    entry->from_pool = False;
    return entry;
}

int
queue_enqueue (queue_t *queue, void *data)
{
    queue_entry_t *entry;

    if (data == NULL) {
        return -1;
    }

    if (queue_is_full(queue)) {
        return -1;
    }

    entry = get_queue_entry(queue);
    if (entry == NULL) {
        return -1;
    }

    entry->data = data;
    dlist_append(&queue->head, &entry->header);
    queue->size++;
    return 0;
}

static void
free_queue_entry (queue_entry_t *entry)
{
    if (entry->from_pool) {
        entry->free_for_use = True;
    } else {
        free(entry);
    }
}

void *
queue_dequeue (queue_t *queue)
{
    dlist_header_t *dlist_header;
    queue_entry_t *entry;
    void *data;

    if (queue_is_empty(queue)) {
        return NULL;
    }

    dlist_header = dlist_pop_left(&queue->head);
    entry = get_entry_from_header(dlist_header);
    queue->size--;
    data = entry->data;
    free_queue_entry(entry);
    return data;
}

static queue_entry_t *
find_queue_entry (queue_t *queue, void *data)
{
    dlist_header_t *dlist_header;
    queue_entry_t *entry;

    dlist_header = queue->head.next;
    while (dlist_header != &queue->head) {
        entry = get_entry_from_header(dlist_header);
        if (entry->data == data) {
            return entry;
        }
        dlist_header = dlist_header->next;
    }
    return NULL;
}

void
queue_remove (queue_t *queue, void *data)
{
    queue_entry_t *entry;
    dlist_header_t *dlist_header;

    entry = find_queue_entry(queue, data);
    if (entry == NULL) {
        return;
    }

    dlist_header = &entry->header;
    dlist_header->next->prev = dlist_header->prev;
    dlist_header->prev->next = dlist_header->next;
    queue->size--;
    free_queue_entry(entry);
}

void
queue_seek_head (queue_t *queue)
{
    if (queue_is_empty(queue)) {
        queue->pos = NULL;
    } else {
        queue->pos = &queue->head;
    }
}

void *
queue_get_next (queue_t *queue)
{
    dlist_header_t *dlist_header_next;
    queue_entry_t *entry;

    if (queue->pos == NULL) {
        return NULL;
    }
    dlist_header_next = queue->pos->next;
    if (dlist_header_next == &queue->head) {
        queue->pos = NULL;
        return NULL;
    }

    queue->pos = dlist_header_next;
    entry = get_entry_from_header(dlist_header_next);
    return entry->data;
}

