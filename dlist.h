#ifndef __DLIST_H__
#define __DLIST_H__

#include <stdbool.h>
#include "util.h"

typedef struct dlist_header_s {
    struct dlist_header_s *prev;
    struct dlist_header_s *next;
} dlist_header_t;

#define dlist_get_entry(header, stype, member) \
   ((stype *)((char *)(header) - (unsigned long)&(((stype *)(0))->member)))

static inline void
dlist_init (dlist_header_t *head)
{
    head->prev = head;
    head->next = head;
}

static inline bool
dlist_is_empty (dlist_header_t *head)
{
    if (head == head->prev && head == head->next) {
        return True;
    } else {
        return False;
    }
}

static inline void
dlist_append (dlist_header_t *head, dlist_header_t *entry)
{
    entry->next = head;
    entry->prev = head->prev;
    head->prev->next = entry;
    head->prev = entry;
}

static inline void
dlist_append_left (dlist_header_t *head, dlist_header_t *entry)
{
    entry->next = head->next;
    entry->prev = head;
    head->next->prev = entry;
    head->next = entry;
}

static inline dlist_header_t *
dlist_pop (dlist_header_t *head)
{
    dlist_header_t *entry;

    if (dlist_is_empty(head)) {
        return NULL;
    }

    entry = head->prev;
    entry->prev->next = head;
    head->prev = entry->prev;
    return entry;
}

static inline dlist_header_t *
dlist_pop_left (dlist_header_t *head)
{
    dlist_header_t *entry;

    if (head->prev == head && head->next == head) {
        return NULL;
    }

    entry = head->next;
    entry->next->prev = head;
    head->next = entry->next;
    return entry;
}

#endif //__DLIST_H__
