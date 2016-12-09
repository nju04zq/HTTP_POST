#ifndef __TASK_QUEUE_H__
#define __TASK_QUEUE_H__

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "queue.h"

#define TASK_QUEUE_POOL_SIZE 50
#define TASK_QUEUE_MAX_SIZE TASK_QUEUE_POOL_SIZE

typedef struct task_queue_data_s {
    void *p;
} task_queue_data_t;

typedef struct task_queue_s {
    queue_t queue;
    pthread_mutex_t lock;
    pthread_cond_t cond_sender;
    pthread_cond_t cond_producer;
} task_queue_t;

int
task_queue_init(task_queue_t *tqueue);

void
task_queue_clean(task_queue_t *tqueue);

void
task_queue_set_max_size(task_queue_t *tqueue, uint32_t max_size);

void
task_queue_get(task_queue_t *tqueue, task_queue_data_t *data);

void
task_queue_put(task_queue_t *tqueue, task_queue_data_t *data);
#endif //__TASK_QUEUE_H__
