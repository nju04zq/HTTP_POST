#include <stdio.h>
#include "queue.h"

#define DATA_CNT 5

static void
dump_queue (queue_t *queue)
{
    int *p;

    queue_seek_head(queue);
    for (;;) {
        p = queue_get_next(queue);
        if (p == NULL) {
            break;
        }
        printf("%d->", *p);
    }
    printf("\n");
}

static void
test_queue (void)
{
    int data[DATA_CNT], i, *p, rc;
    queue_t queue;

    for (i = 0; i < DATA_CNT; i++) {
        data[i] = i;
    } 

    queue_init(&queue);
    queue_set_pool(&queue, DATA_CNT-2);
    dump_queue(&queue);
    for (i = 0; i < DATA_CNT; i++) {
        rc = queue_enqueue(&queue, &data[i]);
        if (rc != 0) {
            return;
        }
        printf("Queu size: %d\n", queue_get_size(&queue));
    }
    dump_queue(&queue);
    while (!queue_is_empty(&queue)) {
        p = queue_dequeue(&queue);
        printf("%d->", *p);
    }
    printf("\n");
    dump_queue(&queue);
    printf("Queu size: %d\n", queue_get_size(&queue));
    queue_clean(&queue);
    return;
}

int main (void)
{
    test_queue();
    return 0;
}
