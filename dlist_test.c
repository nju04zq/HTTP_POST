#include <stdio.h>
#include "dlist.h"

typedef struct test_entry_s {
    int val;
    dlist_header_t header;
} test_entry_t;

#define TEST_ENTRY_CNT 5

static void
dump_dlist (dlist_header_t *head)
{
    dlist_header_t *p = NULL;
    test_entry_t *entry;

    if (head == NULL) {
        printf("\n");
        return;
    }

    p = head->next;
    while (p != head) {
        entry = dlist_get_entry(p, test_entry_t, header);
        printf("%d->", entry->val);
        p = p->next;
    }
    printf("\n");
}

int main (void)
{
    dlist_header_t head, *p;
    test_entry_t entries[TEST_ENTRY_CNT], *entry;
    int i;

    dlist_init(&head);

    for (i = 0; i < TEST_ENTRY_CNT; i++) {
        entries[i].val = i;
        dlist_append_left(&head, &entries[i].header);
    }
    dump_dlist(&head);
    for (;;) {
        p = dlist_pop_left(&head);
        if (p == NULL) {
            break;
        }
        entry = dlist_get_entry(p, test_entry_t, header);
        printf("%d->", entry->val);
    }
    printf("\n");
    dump_dlist(&head);
    return 0;
}

