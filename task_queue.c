#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include "util.h"
#include "queue.h"
#include "task_queue.h"

int
task_queue_init (task_queue_t *tqueue)
{
    int rc;

    memzero(tqueue, sizeof(task_queue_t));

    queue_init(&tqueue->queue);
    queue_set_max_size(&tqueue->queue, TASK_QUEUE_MAX_SIZE);
    rc = queue_set_pool(&tqueue->queue, TASK_QUEUE_POOL_SIZE);
    if (rc != 0) {
        logger(ERROR, "Fail to set pool.");
        return rc;
    }

    rc = pthread_mutex_init(&tqueue->lock, NULL);
    if (rc != 0) {
        logger(ERROR, "Fail to init mutex.");
        queue_clean(&tqueue->queue);
        return rc;
    }

    rc = pthread_cond_init(&tqueue->cond_sender, NULL);
    if (rc != 0) {
        logger(ERROR, "Fail to init cond sender.");
        pthread_mutex_destroy(&tqueue->lock);
        queue_clean(&tqueue->queue);
        return rc;
    }

    rc = pthread_cond_init(&tqueue->cond_producer, NULL);
    if (rc != 0) {
        logger(ERROR, "Fail to init cond producer.");
        pthread_cond_destroy(&tqueue->cond_sender);
        pthread_mutex_destroy(&tqueue->lock);
        queue_clean(&tqueue->queue);
        return rc;
    }

    return 0;
}

void
task_queue_clean (task_queue_t *tqueue)
{
    queue_clean(&tqueue->queue);
    pthread_mutex_destroy(&tqueue->lock);
    pthread_cond_destroy(&tqueue->cond_sender);
    pthread_cond_destroy(&tqueue->cond_producer);
}

void
task_queue_set_max_size (task_queue_t *tqueue, uint32_t max_size)
{
    queue_set_max_size(&tqueue->queue, max_size);
}

static void
task_queue_lock (task_queue_t *tqueue)
{
    pthread_mutex_lock(&tqueue->lock);
}

static void
task_queue_unlock (task_queue_t *tqueue)
{
    pthread_mutex_unlock(&tqueue->lock);
}

static inline void
task_queue_enqueue (task_queue_t *tqueue, task_queue_data_t *data)
{
    queue_enqueue(&tqueue->queue, data->p);
}

static inline void
task_queue_dequeue (task_queue_t *tqueue, task_queue_data_t *data)
{
    data->p = queue_dequeue(&tqueue->queue);
}

void
task_queue_get (task_queue_t *tqueue, task_queue_data_t *data)
{
    task_queue_lock(tqueue);
    for (;;) {
        if (!queue_is_empty(&tqueue->queue)) {
            break;
        }
        pthread_cond_wait(&tqueue->cond_sender, &tqueue->lock);
    }

    task_queue_dequeue(tqueue, data);
    task_queue_unlock(tqueue);
    pthread_cond_broadcast(&tqueue->cond_producer);
}

void
task_queue_put (task_queue_t *tqueue, task_queue_data_t *data)
{
    task_queue_lock(tqueue);
    for (;;) {
        if (!queue_is_full(&tqueue->queue)) {
            break;
        }
        pthread_cond_wait(&tqueue->cond_producer, &tqueue->lock);
    }

    task_queue_enqueue(tqueue, data);
    task_queue_unlock(tqueue);
    pthread_cond_broadcast(&tqueue->cond_sender);
}

