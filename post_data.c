/*
 * Some tricky options:
 * #1. socket option TCP_NODELAY, default not set
 * #2. nginx configuration keepalive_requests, default 100
 */

#define _GNU_SOURCE

#include <time.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/syscall.h>  
#include <netinet/in.h>
#include <netinet/tcp.h>

#ifndef True
#define True true
#endif

#ifndef False
#define False false
#endif

#ifdef _DEBUG_MODE_
#define LOGGER_TIME_TS_MAX_LEN 63
#define gettid() syscall(__NR_gettid)
#define logger(level, fmt, args...) \
do {\
    struct timeval __cur_tv; \
    struct tm __cur_tm; \
    char __ts[LOGGER_TIME_TS_MAX_LEN+1]; \
    gettimeofday(&__cur_tv, NULL); \
    localtime_r(&__cur_tv.tv_sec, &__cur_tm); \
    strftime(__ts, LOGGER_TIME_TS_MAX_LEN, "%T", &__cur_tm); \
    printf("%s.%03d <%04x> "#level" "fmt"\n", __ts, (int)__cur_tv.tv_usec/1000,\
           (unsigned int)gettid(), ##args); \
    fflush(stdin); \
} while (0)
#else
#define logger(level, fmt, args...)
#endif

#define MAX(a, b) ((a) > (b) ? (a) : (b))

typedef struct task_queue_data_s {
    void *p;
    uint32_t size;
} task_queue_data_t;

typedef struct task_queue_entry_s {
    struct task_queue_entry_s *prev;
    struct task_queue_entry_s *next;
    task_queue_data_t data;
    bool from_pool;
    bool free;
} task_queue_entry_t;

#define TASK_QUEUE_ENTRY_POOL_SIZE 50
#define TASK_QUEUE_MAX_SIZE TASK_QUEUE_ENTRY_POOL_SIZE

typedef struct task_queue_s {
    task_queue_entry_t entry_pool[TASK_QUEUE_ENTRY_POOL_SIZE];
    task_queue_entry_t *head;
    uint32_t queue_size;
    pthread_mutex_t lock;
    pthread_cond_t cond_sender;
    pthread_cond_t cond_producer;
} task_queue_t;

task_queue_t task_queue;

static void
memzero (void *p, uint32_t size)
{
    memset(p, 0, size);
    return;
}

static int
task_queue_init (task_queue_t *queue)
{
    int rc, i;

    memzero(queue, sizeof(task_queue_t));

    rc = pthread_mutex_init(&queue->lock, NULL);
    if (rc != 0) {
        logger(ERROR, "Fail to init mutex.");
        return rc;
    }

    rc = pthread_cond_init(&queue->cond_sender, NULL);
    if (rc != 0) {
        logger(ERROR, "Fail to init cond sender.");
        return rc;
    }

    rc = pthread_cond_init(&queue->cond_producer, NULL);
    if (rc != 0) {
        logger(ERROR, "Fail to init cond producer.");
        return rc;
    }

    for (i = 0; i < TASK_QUEUE_ENTRY_POOL_SIZE; i++) {
        queue->entry_pool[i].from_pool = True;
        queue->entry_pool[i].free = True;
    }

    return 0;
}

static void
task_queue_clean (task_queue_t *queue)
{
    pthread_mutex_destroy(&queue->lock);
    pthread_cond_destroy(&queue->cond_sender);
    pthread_cond_destroy(&queue->cond_producer);
}

static void
task_queue_lock (task_queue_t *queue)
{
    pthread_mutex_lock(&queue->lock);
}

static void
task_queue_unlock (task_queue_t *queue)
{
    pthread_mutex_unlock(&queue->lock);
}

static bool
task_queue_is_empty (task_queue_t *queue)
{
    if (queue->queue_size > 0) {
        return False;
    } else {
        return True;
    }
}

static bool
task_queue_is_full (task_queue_t *queue)
{
    if (queue->queue_size < TASK_QUEUE_MAX_SIZE) {
        return False;
    } else {
        return True;
    }
}

static task_queue_entry_t *
task_queue_get_entry_from_pool (task_queue_t *queue)
{
    uint32_t i;

    for (i = 0; i < TASK_QUEUE_ENTRY_POOL_SIZE; i++) {
        if (queue->entry_pool[i].free) {
            queue->entry_pool[i].free = False;
            return &queue->entry_pool[i];
        }
    }
    return NULL;
}

static task_queue_entry_t *
task_queue_get_entry (task_queue_t *queue)
{
    task_queue_entry_t *entry;

    entry = task_queue_get_entry_from_pool(queue);
    if (entry) {
        return entry;
    }

    entry = calloc(1, sizeof(task_queue_entry_t));
    if (!entry) {
        return NULL;
    }
    entry->from_pool = False;
    return entry;
}

static void
task_queue_free_entry (task_queue_entry_t *entry)
{
    if (entry->from_pool) {
        entry->free = True;
    } else {
        free(entry);
    }
}

static void
task_queue_enqueue (task_queue_t *queue, task_queue_data_t *data)
{
    task_queue_entry_t *entry;

    entry = task_queue_get_entry(queue);
    if (!entry) {
        logger(ERROR, "Task queue: fail to get entry.");
        return;
    }
    memcpy(&entry->data, data, sizeof(task_queue_data_t));

    if (queue->head) {
        entry->next = queue->head;
        entry->prev = queue->head->prev;
        entry->next->prev = entry;
        entry->prev->next = entry;
    } else {
        entry->next = entry;
        entry->prev = entry;
    }
    queue->head = entry;
    queue->queue_size += 1;
}

static void
task_queue_dequeue (task_queue_t *queue, task_queue_data_t *data)
{
    task_queue_entry_t *entry;

    if (queue->queue_size == 1) {
        entry = queue->head;
        queue->head = NULL;
    } else {
        entry = queue->head->prev;
        entry->prev->next = entry->next;
        entry->next->prev = entry->prev;
    }
    queue->queue_size -= 1;
    memcpy(data, &entry->data, sizeof(task_queue_data_t));
    task_queue_free_entry(entry);
}

static void
task_queue_get (task_queue_t *queue, task_queue_data_t *data)
{
    task_queue_lock(queue);
    for (;;) {
        if (!task_queue_is_empty(queue)) {
            break;
        }
        pthread_cond_wait(&queue->cond_sender, &queue->lock);
    }

    task_queue_dequeue(queue, data);
    task_queue_unlock(queue);
    pthread_cond_broadcast(&queue->cond_producer);
}

static void
task_queue_put (task_queue_t *queue, task_queue_data_t *data)
{
    task_queue_lock(queue);
    for (;;) {
        if (!task_queue_is_full(queue)) {
            break;
        }
        pthread_cond_wait(&queue->cond_producer, &queue->lock);
    }

    task_queue_enqueue(queue, data);
    task_queue_unlock(queue);
    pthread_cond_broadcast(&queue->cond_sender);
}

typedef struct global_counter_s {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool started;
    uint32_t total;
    uint32_t success;
    uint32_t failure;
} global_counter_t;

static global_counter_t gcounter;

static int
gcounter_init (global_counter_t *p)
{
    int rc;

    memzero(p, sizeof(global_counter_t));

    rc = pthread_mutex_init(&p->lock, NULL);
    if (rc != 0) {
        logger(ERROR, "Fail to init gcounter mutex.");
        return rc;
    }
    return 0;
}

static void
gcounter_clean (global_counter_t *p)
{
    pthread_mutex_destroy(&p->lock);
}

static void
gcounter_lock (global_counter_t *p)
{
    pthread_mutex_lock(&p->lock);
}

static void
gcounter_unlock (global_counter_t *p)
{
    pthread_mutex_unlock(&p->lock);
}

static void
gcounter_wait_for_start (global_counter_t *p)
{
    gcounter_lock(p);
    for (;;) {
        if (p->started) {
            break;
        }
        pthread_cond_wait(&p->cond, &p->lock);
    }
    gcounter_unlock(p);
}

static void
gcounter_signal_start (global_counter_t *p)
{
    gcounter_lock(p);
    if (!p->started) {
        pthread_cond_broadcast(&p->cond);
    }
    p->started = True;
    gcounter_unlock(p);
}

static void
gcounter_inc_success (global_counter_t *p)
{
    gcounter_lock(p);
    p->success++;
    p->total++;
    gcounter_unlock(p);
}

static void
gcounter_inc_failure (global_counter_t *p)
{
    gcounter_lock(p);
    p->failure++;
    p->total++;
    gcounter_unlock(p);
}

static void
gcounter_get_snapshot (global_counter_t *src, global_counter_t *dst)
{
    gcounter_lock(src);
    dst->success = src->success;
    dst->failure = src->failure;
    dst->total = src->total;
    gcounter_unlock(src);
    return;
}

static uint32_t
gcounter_get_total (global_counter_t *p)
{
    uint32_t total;

    gcounter_lock(p);
    total = p->total;
    gcounter_unlock(p);
    return total;
}

typedef struct sender_env_s {
    int epfd;
    char *ip;
    int port;
    uint32_t msg_cnt;
} sender_env_t;

typedef struct sender_ctrl_s {
    int epfd;
    char *ip;
    int port;
    int sockfd;
    struct sockaddr_in sockaddr;
    bool resp_ready;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} sender_ctrl_t;

#define RESP_MAX_BUF_LEN 1023
#define SENDER_WAIT_RESP_TIMEOUT 30 //seconds

static int
sender_socket_open (sender_ctrl_t *sender_ctrl)
{
    int rc, one;

    sender_ctrl->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sender_ctrl->sockfd < 0) {
        logger(ERROR, "Fail to open socket.");
        free(sender_ctrl);
        return -1;
    }
    logger(DEBUG, "Create sockfd %d", sender_ctrl->sockfd);

    one = 1;
    rc = setsockopt(sender_ctrl->sockfd, IPPROTO_TCP, TCP_NODELAY,
                    &one, sizeof(one));
    if (rc != 0) {
        logger(ERROR, "Fail to set sockopt");
        close(sender_ctrl->sockfd);
        return -1;
    }

    return 0;
}

static void
sender_socket_close (sender_ctrl_t *sender_ctrl)
{
    close(sender_ctrl->sockfd);
}

static int
sender_socket_connect (sender_ctrl_t *sender_ctrl)
{
    char *err;
    int rc;

    rc = connect(sender_ctrl->sockfd,
                 (struct sockaddr *)&sender_ctrl->sockaddr,
                 sizeof(struct sockaddr_in));
    if (rc != 0) {
        err = strerror(errno);
        logger(ERROR, "Fail to connect, %s", err);
        return rc;
    }
    logger(INFO, "Connect succeed.");
    return 0;
}

static int
sender_socket_create (sender_ctrl_t *sender_ctrl)
{
    int rc;

    rc = sender_socket_open(sender_ctrl);
    if (rc != 0) {
        return rc;
    }
    rc = sender_socket_connect(sender_ctrl);
    if (rc != 0) {
        sender_socket_close(sender_ctrl);
        return rc;
    }
    return 0;
}

static int
sender_wait (sender_ctrl_t *sender_ctrl)
{
    struct timeval tv;
    struct timespec timeout;
    char *err;
    int rc = 0;

    logger(DEBUG, "Wait for resp...");

    gettimeofday(&tv, NULL);
    timeout.tv_sec = tv.tv_sec + SENDER_WAIT_RESP_TIMEOUT;
    timeout.tv_nsec = 0;
    pthread_mutex_lock(&sender_ctrl->lock);
    for (;;) {
        if (sender_ctrl->resp_ready) {
            break;
        }
        rc = pthread_cond_timedwait(&sender_ctrl->cond,
                                    &sender_ctrl->lock, &timeout);
        if (rc != 0) {
            err = strerror(errno);
            logger(ERROR, "Sender wait failed: %s", err);
            break;
        }
    }
    pthread_mutex_unlock(&sender_ctrl->lock);
    return rc;
}

static int
wait_for_resp (sender_ctrl_t *sender_ctrl)
{
    struct epoll_event ev;
    char resp[RESP_MAX_BUF_LEN+1];
    char *err;
    int rc;

    sender_ctrl->resp_ready = False;

    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = sender_ctrl;
    rc = epoll_ctl(sender_ctrl->epfd, EPOLL_CTL_ADD,
                   sender_ctrl->sockfd, &ev);
    if (rc != 0) {
        logger(ERROR, "Fail to epoll_ctl.");
        return -1;
    }

    rc = sender_wait(sender_ctrl);
    epoll_ctl(sender_ctrl->epfd, EPOLL_CTL_DEL,
              sender_ctrl->sockfd, &ev);
    if (rc != 0) {
        return -1;
    }

    memzero(resp, RESP_MAX_BUF_LEN+1);
    rc = read(sender_ctrl->sockfd, resp, RESP_MAX_BUF_LEN);
    if (rc > 0) {
        logger(DEBUG, "Get resp as\n%s", resp);
    } else if (rc ==0) {
        logger(DEBUG, "Get FIN, connection reset.");
        rc = -1;
    } else {
        rc = -1;
        err = strerror(errno);
        logger(ERROR, "Read resp error, rc %d, %s", rc, err);
    }
    return rc;
}

static int
send_http_msg_once (sender_ctrl_t *sender_ctrl, char *msg)
{
    int rc;

    gcounter_signal_start(&gcounter);

    rc = write(sender_ctrl->sockfd, msg, strlen(msg));
    if (rc == -1) {
        logger(ERROR, "Fail to write socket.");
        return rc;
    }

    rc = wait_for_resp(sender_ctrl);
    if (rc == -1) {
        logger(ERROR, "Fail to get response from server.");
        return rc;
    }

    return 0;
}

#define SEND_MSG_MAX_TRY 3

static void
send_http_msg (sender_ctrl_t *sender_ctrl, char *msg)
{
    int i, rc;

    for (i = 0; i < SEND_MSG_MAX_TRY; i++) {
        rc = send_http_msg_once(sender_ctrl, msg);
        if (rc == 0) {
            gcounter_inc_success(&gcounter);
            return;
        }
        sender_socket_close(sender_ctrl);
        sender_socket_create(sender_ctrl);
    }
    gcounter_inc_failure(&gcounter);
}

static sender_ctrl_t *
sender_init (sender_env_t *sender_env)
{
    sender_ctrl_t *sender_ctrl;
    int rc;

    sender_ctrl = calloc(1, sizeof(sender_ctrl_t));
    if (!sender_ctrl) {
        logger(ERROR, "Fail to calloc for sender ctrl.");
        return NULL;
    }
    sender_ctrl->epfd = sender_env->epfd;
    sender_ctrl->ip = sender_env->ip;
    sender_ctrl->port = sender_env->port;

    rc = pthread_mutex_init(&sender_ctrl->lock, NULL);
    if (rc != 0) {
        logger(ERROR, "Fail to init mutex.");
        close(sender_ctrl->sockfd);
        free(sender_ctrl);
        return NULL;
    }

    rc = pthread_cond_init(&sender_ctrl->cond, NULL);
    if (rc != 0) {
        logger(ERROR, "Fail to init cond.");
        pthread_mutex_destroy(&sender_ctrl->lock);
        free(sender_ctrl);
        close(sender_ctrl->sockfd);
        return NULL;
    }

    bzero((char *)&sender_ctrl->sockaddr, sizeof(struct sockaddr_in));
    sender_ctrl->sockaddr.sin_family = AF_INET;
    sender_ctrl->sockaddr.sin_addr.s_addr = inet_addr(sender_ctrl->ip);
    sender_ctrl->sockaddr.sin_port = htons(sender_ctrl->port);
    return sender_ctrl;
}

static void
sender_clean (sender_ctrl_t *sender_ctrl)
{
    sender_socket_close(sender_ctrl);
    pthread_mutex_destroy(&sender_ctrl->lock);
    pthread_cond_destroy(&sender_ctrl->cond);
    free(sender_ctrl);
}

static void *
sender_thread (void *arg)
{
    sender_env_t *sender_env = (sender_env_t *)arg;
    sender_ctrl_t *sender_ctrl;
    task_queue_data_t data;
    char *msg;
    int rc;

    sender_ctrl = sender_init(sender_env);
    if (!sender_ctrl) {
        return NULL;
    }

    rc = sender_socket_create(sender_ctrl);
    if (rc != 0) {
        sender_clean(sender_ctrl);
        return NULL;
    }

    for (;;) {
        task_queue_get(&task_queue, &data);
        msg = (char *)data.p;
        logger(DEBUG, "Fetch msg as:\n%s", msg);
        send_http_msg(sender_ctrl, msg);
        free(msg);
    }

    sender_clean(sender_ctrl);
    return NULL;
}

#define SENDER_THREAD_CNT 10

static int
create_sender_threads (sender_env_t *sender_env)
{
    int rc;
    uint32_t i;
    pthread_t thread_id;

    for (i = 0; i < SENDER_THREAD_CNT; i++) {
        rc = pthread_create(&thread_id, NULL, sender_thread, sender_env);
        if (rc != 0) {
            logger(ERROR, "Fail to create sender thread");
            return -1;
        }
    }
    return 0;
}

static char *
msg_header_template = "POST /graph/ HTTP/1.1\r\n"
                      "Content-length: %d\r\n"
                      "Host: %s:%d\r\n"
                      "Content-type: application/json\r\n"
                      "\r\n"; 

static char *
msg_body_template = "{\"vertices\": {\"Txn\": {\"tran_id%d_%d\": "
                    "{\"clt_nbr\": {\"value\":\"2645\"}}}}}"; 

static char *
generate_msg (char *ip, int port, uint32_t i)
{
    char *msg, *msg_body, *msg_header;
    time_t t;

    time(&t);
    asprintf(&msg_body, msg_body_template, t, i);
    if (!msg_body) {
        return NULL;
    }

    asprintf(&msg_header, msg_header_template, strlen(msg_body), ip, port);
    if (!msg_header) {
        free(msg_body);
        return NULL;
    }

    msg = calloc(strlen(msg_body) + strlen(msg_header) + 1, sizeof(char));
    if (!msg) {
        free(msg_body);
        free(msg_header);
        return NULL;
    }

    strncpy(msg, msg_header, strlen(msg_header));
    strncpy(msg+strlen(msg_header), msg_body, strlen(msg_body));

    free(msg_body);
    free(msg_header);

    //logger(DEBUG, "Produce msg as:\n%s", msg);
    return msg;
}

static void *
producer_thread (void *arg)
{
    sender_env_t *sender_env = (sender_env_t *)arg;
    task_queue_data_t data;
    uint32_t i;
    char *msg, *ip = sender_env->ip;
    int port = sender_env->port;
    uint32_t msg_cnt = sender_env->msg_cnt;

    for (i = 0; i < msg_cnt; i++) {
        msg = generate_msg(ip, port, i);
        if (!msg) {
            continue;
        }
        data.p = msg;
        data.size = strlen(msg) + 1;
        task_queue_put(&task_queue, &data);
    }

    return NULL;
}

static int
create_producer_thread (sender_env_t *sender_env)
{
    pthread_t thread_id;
    int rc;

    rc = pthread_create(&thread_id, NULL, producer_thread, sender_env);
    if (rc != 0) {
        logger(ERROR, "Fail to create producer thread");
        return -1;
    }
    return 0;
}

enum {
    COLUMN_ELAPSED = 0,
    COLUMN_STATS,
    COLUMN_PROGRESS,
    COLUMN_QPS,
    COLUMN_MAX
};

typedef struct column_s {
    char *header;
    char *value;
    uint32_t max_width;
    int i1;
    int i2;
    float f1;
    char * (*maker)(struct column_s *);
} column_t;

typedef struct column_mgr_s {
    column_t columns[COLUMN_MAX];
    char *header;
    char *seperator;
    char *line;
    uint32_t max_width;
    uint32_t last_line_len;
} column_mgr_t;

column_mgr_t g_column_mgr;

static char *
column_elpased_maker (column_t *p)
{
    int seconds, hours, minutes;
    char *s = NULL;

    seconds = p->i1;
    hours = seconds/3600;
    seconds %= 3600;
    minutes = seconds/60;
    seconds %= 60;
    if (!p->value) {
        asprintf(&s, "%02d:%02d:%02d", hours, minutes, seconds);
    } else {
        snprintf(p->value, p->max_width+1,
                 "%02d:%02d:%02d", hours, minutes, seconds);
    }
    return s;
}

static char *
column_stats_maker (column_t *p)
{
    int success, total;
    char *s = NULL;

    success = p->i1;
    total = p->i2;
    if (!p->value) {
        asprintf(&s, "%d/%d", success, total);
    } else {
        snprintf(p->value, p->max_width+1, "%d/%d", success, total);
    }
    return s;
}

static char *
column_progress_maker (column_t *p)
{
    float progress;
    char *s = NULL;

    progress = p->f1;
    if (!p->value) {
        asprintf(&s, "%.1f%%", progress);
    } else {
        snprintf(p->value, p->max_width+1, "%.1f%%", progress);
    }
    return s;
}

static char *
column_qps_maker (column_t *p)
{
    float qps;
    char *s = NULL;

    qps = p->f1;
    if (!p->value) {
        asprintf(&s, "%.1f", qps);
    } else {
        snprintf(p->value, p->max_width+1, "%.1f", qps);
    }
    return s;
}

static void
fill_in_spaces (char *s, uint32_t cnt)
{
    uint32_t i;

    for (i = 0; i < cnt; i++) {
        s[i] = ' ';
    }
}

static void
column_mgr_make_header (column_mgr_t *column_mgr)
{
    column_t *column;
    char *s;
    int i, header_len;

    column_mgr->header = calloc(column_mgr->max_width+1, sizeof(char));
    if (!column_mgr->header) {
        return;
    }

    s = column_mgr->header;
    s[0] = ' ';
    s++;
    
    for (i = 0; i < COLUMN_MAX; i++) {
        column = &column_mgr->columns[i];
        header_len = strlen(column->header);
        strncpy(s, column->header, header_len);
        s += header_len;
        if (header_len < column->max_width) {
            fill_in_spaces(s, column->max_width - header_len);
            s += (column->max_width - header_len);
        }
        *s = ' ';
        s++;
    }
}

static void
column_mgr_make_seperator (column_mgr_t *column_mgr)
{
    char *s;
    int i;

    column_mgr->seperator = calloc(column_mgr->max_width+1, sizeof(char));
    if (!column_mgr->seperator) {
        return;
    }

    s = column_mgr->seperator;
    for (i = 0; i < column_mgr->max_width; i++) {
        s[i] = '-';
    }
}

static void
column_mgr_make_line (column_mgr_t *column_mgr)
{
    column_t *column;
    char *line = column_mgr->line;
    int i, column_len;

    column_mgr->last_line_len = strlen(line);

    line[0] = ' ';
    line++;
    
    for (i = 0; i < COLUMN_MAX; i++) {
        column = &column_mgr->columns[i];
        column->maker(column);
        column_len = strlen(column->value);
        strncpy(line, column->value, column_len);
        line += column_len;
        if (column_len < column->max_width) {
            fill_in_spaces(line, column->max_width - column_len);
            line += (column->max_width - column_len);
        }
        *line = ' ';
        line++;
    }
}

static int
column_mgr_init_one_column (column_t *column)
{
    char *s;

    s = column->maker(column);
    if (!s) {
        logger(ERROR, "Fail to init column header");
        return -1;
    }

    column->max_width = MAX(strlen(column->header), strlen(s));
    free(s);

    column->value = calloc(column->max_width+1, sizeof(char));
    if (!column->value) {
        logger(ERROR, "Fail to calloc column value");
        return -1;
    }
    return 0;
}

static int
column_mgr_init_columns (column_mgr_t *column_mgr)
{
    column_t *p = column_mgr->columns;
    int rc, i, max_width = 0;

    for (i = 0; i < COLUMN_MAX; i++) {
        rc = column_mgr_init_one_column(&p[i]);
        if (rc != 0) {
            return -1;
        }
        max_width += p[i].max_width;
    }

    max_width += COLUMN_MAX; //Each filed has a tailing space
    max_width += 1; //one heading space
    column_mgr->max_width = max_width;
    return 0;
}

static int
column_mgr_init (column_mgr_t *column_mgr, sender_env_t *sender_env)
{
    column_t *p = column_mgr->columns;
    int rc;

    memzero(column_mgr, sizeof(column_mgr_t));

    p[COLUMN_ELAPSED].header = "Elapsed";
    p[COLUMN_ELAPSED].i1 = 1;
    p[COLUMN_ELAPSED].maker = column_elpased_maker;

    p[COLUMN_STATS].header = "Success/Total";
    p[COLUMN_STATS].i1 = sender_env->msg_cnt;
    p[COLUMN_STATS].i2 = sender_env->msg_cnt;
    p[COLUMN_STATS].maker = column_stats_maker;

    p[COLUMN_PROGRESS].header = "Progress";
    p[COLUMN_PROGRESS].f1 = 100.0;
    p[COLUMN_PROGRESS].maker = column_progress_maker;

    p[COLUMN_QPS].header = "QPS";
    p[COLUMN_QPS].f1 = 1500.0;
    p[COLUMN_QPS].maker = column_qps_maker;

    rc = column_mgr_init_columns(column_mgr);
    if (rc != 0) {
        return rc;
    }
    column_mgr_make_header(column_mgr);
    column_mgr_make_seperator(column_mgr);

    column_mgr->line = calloc(column_mgr->max_width+1, sizeof(char));
    if (!column_mgr->line) {
        logger(ERROR, "Fail to calloc for column mgr line.");
        return -1;
    }
    column_mgr->last_line_len = 0;
    return 0;
}

static void
column_mgr_clean (column_mgr_t *column_mgr)
{
    int i;

    if (column_mgr->header) {
        free(column_mgr->header);
    }
    if (column_mgr->seperator) {
        free(column_mgr->seperator);
    }
    if (column_mgr->line) {
        free(column_mgr->line);
    }

    for (i = 0; i < COLUMN_MAX; i++) {
        if (column_mgr->columns[i].value) {
            free(column_mgr->columns[i].value);
        }
    }
}

static void
dump_backspace (column_mgr_t *column_mgr)
{
    int last_len, cur_len, i, cnt;

    last_len = column_mgr->last_line_len;
    cur_len = strlen(column_mgr->line);
    if (last_len <= cur_len) {
        return;
    }

    cnt = last_len - cur_len;
    for (i = 0; i < cnt; i++) {
        printf("\b");
    }
}

static void
dump_stats (uint32_t total, uint32_t success, uint32_t msg_cnt,
            struct timeval *start_ts)
{
    column_t *p;
    struct timeval now;
    int seconds;
    float elapsed = 0, progress, qps;

    gettimeofday(&now, NULL);
    seconds = now.tv_sec - start_ts->tv_sec;
    elapsed += seconds;
    elapsed += (float)(now.tv_usec - start_ts->tv_usec)/1000/1000;
    progress = (float)total/msg_cnt*100;
    qps = (float)total/elapsed;

    p = g_column_mgr.columns;
    p[COLUMN_ELAPSED].i1 = seconds;
    p[COLUMN_STATS].i1 = success;
    p[COLUMN_STATS].i2 = total;
    p[COLUMN_PROGRESS].f1 = progress;
    p[COLUMN_QPS].f1 = qps;

    column_mgr_make_line(&g_column_mgr);
    dump_backspace(&g_column_mgr);

#ifdef _DEBUG_MODE_
    logger(INFO, "%s", g_column_mgr.line);
#else
    printf("\r%s", g_column_mgr.line);
    fflush(stdout);
#endif
    return;
}

static void *
counter_thread (void *arg)
{
    sender_env_t *sender_env = (sender_env_t *)arg;
    global_counter_t counter_snapshot;
    uint32_t msg_cnt = sender_env->msg_cnt;
    struct timeval start_ts;

    printf("%s\n", g_column_mgr.header);
    printf("%s\n", g_column_mgr.seperator);

    logger(INFO, "Wait for counter start.");
    gcounter_wait_for_start(&gcounter);
    logger(INFO, "Counter started.");
    gettimeofday(&start_ts, NULL);
    for (;;) {
        usleep(500*1000); //500ms
        gcounter_get_snapshot(&gcounter, &counter_snapshot);
        dump_stats(counter_snapshot.total, counter_snapshot.success,
                   msg_cnt, &start_ts);
                   
        if (counter_snapshot.total == msg_cnt) {
            break;
        }
    }
    return NULL;
}

static int
create_counter_thread (sender_env_t *sender_env, pthread_t *thread_id)
{
    int rc;

    rc = pthread_create(thread_id, NULL, counter_thread, sender_env);
    if (rc != 0) {
        logger(ERROR, "Fail to create counter thread");
        return -1;
    }
    return 0;
}

static int
prepare_env (sender_env_t *sender_env)
{
    int rc;

    memzero(sender_env, sizeof(sender_env_t));

    rc = gcounter_init(&gcounter);
    if (rc != 0) {
        return -1;
    }

    rc = task_queue_init(&task_queue);
    if (rc != 0) {
        return -1;
    }

    sender_env->epfd = epoll_create1(0);
    if (sender_env->epfd == -1) {
        logger(ERROR, "Fail to create epfd.");
        return -1;
    }

    rc = column_mgr_init(&g_column_mgr, sender_env);
    if (rc != 0) {
        column_mgr_clean(&g_column_mgr);
        return -1;
    }

    sender_env->ip = "127.0.0.1";
    sender_env->port = 9000;
    sender_env->msg_cnt = 50000;
    return 0;
}

static void
clean_env (sender_env_t *sender_env)
{
    close(sender_env->epfd);
    task_queue_clean(&task_queue);
    gcounter_clean(&gcounter);
    column_mgr_clean(&g_column_mgr);
}

static void
notify_sender (sender_ctrl_t *sender_ctrl)
{
    logger(DEBUG, "Sender sock %d", sender_ctrl->sockfd);
    pthread_mutex_lock(&sender_ctrl->lock);
    sender_ctrl->resp_ready = True;
    pthread_mutex_unlock(&sender_ctrl->lock);
    pthread_cond_signal(&sender_ctrl->cond);
}

static void
notify_epoll_events (struct epoll_event *evlist,  int ready)
{
    int i;

    logger(DEBUG, "There are %d events to notify.", ready);
    for (i = 0; i < ready; i++) {
        logger(DEBUG, "Epoll event %d", evlist[i].events);
        if (evlist[i].events & EPOLLIN) {
            notify_sender((sender_ctrl_t *)evlist[i].data.ptr);
        }
    }
}

static bool
is_task_done (sender_env_t *sender_env)
{
    uint32_t total;

    total = gcounter_get_total(&gcounter);
    if (total == sender_env->msg_cnt) {
        return True;
    } else {
        return False;
    }
}

#define EPOLL_WAIT_MAX_EVENTS SENDER_THREAD_CNT
#define EPOLL_WAIT_TIMEOUT 500 //ms

int main (void)
{
    int rc, ready;
    sender_env_t sender_env;
    struct epoll_event evlist[EPOLL_WAIT_MAX_EVENTS];
    pthread_t counter_thread_id;

    rc = prepare_env(&sender_env);
    if (rc != 0) {
        return -1;
    }

    rc = create_sender_threads(&sender_env);
    if (rc != 0) {
        return -1;
    }

    rc = create_producer_thread(&sender_env);
    if (rc != 0) {
        return -1;
    }

    rc = create_counter_thread(&sender_env, &counter_thread_id);
    if (rc != 0) {
        return -1;
    }

    for (;;) {
        ready = epoll_wait(sender_env.epfd, evlist,
                           EPOLL_WAIT_MAX_EVENTS, EPOLL_WAIT_TIMEOUT);
        if (ready == -1) {
            if (errno == EINTR) {
                continue;
            } else {
                logger(ERROR, "Fail on epoll_wait");
                return -1;
            }
        }
        notify_epoll_events(evlist, ready);
        if (is_task_done(&sender_env)) {
            break;
        }
    }

    pthread_join(counter_thread_id, NULL);

    clean_env(&sender_env);
    printf("\nPost done.\n");
    return 0;
}

