#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include "util.h"
#include "queue.h"
#include "task_queue.h"
#include "server_common.h"

#define SERVER_LISTEN_PORT 9999

#define WORKER_THREAD_CNT 4
#define EPOLL_WAIT_MAX_EVENTS WORKER_THREAD_CNT
#define BUF_MAX_LEN 1023

#define dump_one_session(session) \
do {\
    logger(DEBUG, "session, socket %d, %s:%d",\
           (session)->sockfd, (session)->client_ip, (session)->client_port);\
} while (0)

typedef struct session_s {
    int sockfd;
    char client_ip[INET_ADDRSTRLEN+1];
    int client_port;
} session_t;

static queue_t session_queue;
static task_queue_t request_tqueue;

static int listen_fd;
static int epoll_fd;

static void
close_session(session_t *);

#define TXN_ID_MAX_LEN 31

static char *
switch_to_last_line (char *s)
{
    char *p;

    p = s + strlen(s);
    for (; p > s; p--) {
        if (*p == '\n') {
            return p+1;
        }
    }
    return p;
}

static void
extract_txn_id (char *s, char *txn_id)
{
    char *p;

    memzero(txn_id, TXN_ID_MAX_LEN+1);

    s = switch_to_last_line(s);

    p = strchr(s, ':');
    if (p == NULL) {
        strncpy(txn_id, "??", TXN_ID_MAX_LEN);
        return;
    }

    p++;
    while (*p && (*p == ' ' || *p == '\"')) {
        p++;
    }

    for (; *p && *p != '\"'; p++, txn_id++) {
        *txn_id = *p;
    }
}

static void *
worker_thread (void *args)
{
    task_queue_data_t data;
    session_t *session;
    char buf[BUF_MAX_LEN+1];
    char txn_id[TXN_ID_MAX_LEN+1];
    int n;

    for (;;) {
        task_queue_get(&request_tqueue, &data);
        session = (session_t *)data.p;
        memzero(buf, BUF_MAX_LEN+1);
        n = read(session->sockfd, buf, BUF_MAX_LEN);
        if (n == -1) {
            logger(ERROR, "Fail to read from socket %d", session->sockfd);
            continue;
        } else if (n == 0) {
            close_session(session);
            continue;
        }
        logger(DEBUG, "Request msg:\n%s", buf);
        extract_txn_id(buf, txn_id);
        snprintf(buf, BUF_MAX_LEN+1, "Get txn_id %s\n", txn_id);
        logger(DEBUG, "Response msg:\n%s", buf);
        n = write(session->sockfd, buf, strlen(buf));
        if (n == -1) {
            logger(ERROR, "Fail to write to socket %d", session->sockfd);
            continue;
        }
    }
    return NULL;
}

static int
start_worker_threads (void)
{
    pthread_t thread_id;
    int i, rc;

    for (i = 0; i < WORKER_THREAD_CNT; i++) {
        rc = pthread_create(&thread_id, NULL, worker_thread, NULL);
        if (rc != 0) {
            logger(ERROR, "Fail to create worker thread");
            return -1;
        }
    }
    return 0;
}

static void
notify_epoll_events (struct epoll_event *evlist,  int ready)
{
    task_queue_data_t data;
    int i;

    logger(DEBUG, "There are %d events to notify.", ready);
    for (i = 0; i < ready; i++) {
        logger(DEBUG, "Epoll event %d", evlist[i].events);
        if (evlist[i].events & EPOLLIN) {
            data.p = (session_t *)evlist[i].data.ptr;
            task_queue_put(&request_tqueue, &data);
        }
    }
}

static void *
epoll_thread (void *args)
{
    int ready;
    struct epoll_event evlist[EPOLL_WAIT_MAX_EVENTS];

    for (;;) {
        ready = epoll_wait(epoll_fd, evlist,
                           EPOLL_WAIT_MAX_EVENTS, -1);
        if (ready == -1) {
            if (errno == EINTR) {
                continue;
            } else {
                logger(ERROR, "Fail on epoll_wait");
                return NULL;
            }
        }
        notify_epoll_events(evlist, ready);
    }
    return NULL;
}

static int
start_epoll_thread (void)
{
    pthread_t thread_id;
    int rc;

    rc = pthread_create(&thread_id, NULL, epoll_thread, NULL);
    if (rc != 0) {
        logger(ERROR, "Fail to create epoll thread");
        return -1;
    }
    return 0;
}

static void
save_session (session_t *session)
{
    struct epoll_event ev;
    int rc;

    queue_enqueue(&session_queue, session);

    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = session;

    rc = epoll_ctl(epoll_fd, EPOLL_CTL_ADD,
                   session->sockfd, &ev);
    if (rc != 0) {
        logger(ERROR, "Fail on epoll_ctl.");
        return;
    }
}

static void
dump_all_sessions (void)
{
    session_t *session;

    logger(DEBUG, "Remaining sessions:");
    queue_seek_head(&session_queue);
    for (;;) {
        session = queue_get_next(&session_queue);
        if (!session) {
            break;
        }
        dump_one_session(session);
    }
}

static void
close_session (session_t *session)
{
    logger(DEBUG, "Close session:");
    dump_one_session(session);
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, session->sockfd, NULL);
    close(session->sockfd);
    queue_remove(&session_queue, session);
    free(session); 
    dump_all_sessions();
}

static void
handle_accepted_connection (int sockfd, struct sockaddr_in *sockaddr)
{
    session_t *session;

    session = calloc(1, sizeof(session_t));
    if (session == NULL) {
        return;
    }

    inet_ntop(AF_INET, &sockaddr->sin_addr,
              session->client_ip, INET_ADDRSTRLEN+1);
    session->client_port = sockaddr->sin_port;
    session->sockfd = sockfd;
    logger(DEBUG, "Accept sockfd %d from %s:%d",
           sockfd, session->client_ip, session->client_port);
    save_session(session);
}

static int
create_listen_socket (void)
{
    int rc, one, sockfd;
    struct sockaddr_in sockaddr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        logger(ERROR, "Fail to open socket.");
        return -1;
    }
    logger(DEBUG, "Create listen sockfd %d", sockfd);

    one = 1;
    rc = setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY,
                    &one, sizeof(one));
    if (rc != 0) {
        logger(ERROR, "Fail to set sockopt");
        close(sockfd);
        return -1;
    }

    bzero((char *)&sockaddr, sizeof(struct sockaddr_in));
    sockaddr.sin_family = AF_INET;
    sockaddr.sin_addr.s_addr = INADDR_ANY;
    sockaddr.sin_port = htons(SERVER_PORT);

    rc = bind(sockfd, (struct sockaddr *)&sockaddr, sizeof(struct sockaddr_in));
    if (rc != 0) {
        logger(ERROR, "Fail to bind to port %d", SERVER_PORT);
        printf("Fail to bind to port %d.\n", SERVER_PORT);
        return -1;
    }

    return sockfd;
}

static int
server_init (void)
{
    int rc;
    char *err;

    queue_init(&session_queue);

    rc = task_queue_init(&request_tqueue);
    if (rc != 0) {
        task_queue_clean(&request_tqueue);
        return -1;
    }
    task_queue_set_max_size(&request_tqueue, 0);

    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        logger(ERROR, "Fail to create epfd.");
        task_queue_clean(&request_tqueue);
        return -1;
    }

    listen_fd = create_listen_socket();
    if (listen_fd == -1) {
        err = strerror(errno);
        printf("Fail to create listen socket, %s.\n", err);
        task_queue_clean(&request_tqueue);
        close(epoll_fd);
        return -1;
    }

    rc = listen(listen_fd, 0);
    if (rc != 0) {
        err = strerror(errno);
        printf("Fail to listen to socket, %s.\n", err);
        task_queue_clean(&request_tqueue);
        close(epoll_fd);
        close(listen_fd);
        return -1;
    }

    return 0;
}

static int
start_threads (void)
{
    int rc;

    rc = start_epoll_thread();
    if (rc != 0) {
        return -1;
    }
    rc = start_worker_threads();
    if (rc != 0) {
        return -1;
    }
    return 0;
}

static void
wait_on_client_connection (void)
{
    int sockfd, addr_len;
    struct sockaddr_storage sockaddr_accpet;
    struct sockaddr_in *sockaddr_p;
    char *err;

    for (;;) {
        addr_len = sizeof(struct sockaddr_storage);
        sockfd = accept(listen_fd,
                        (struct sockaddr *)&sockaddr_accpet,
                        (socklen_t *)&addr_len);
        if (sockfd == -1) {
            logger(ERROR, "Fail on accept, %d, %d", errno, listen_fd);
            err = strerror(errno);
            printf("Fail to accept incoming connection, %s.\n", err);
            break;
        }

        if (addr_len != sizeof(struct sockaddr_in)) {
            logger(INFO, "Drop NON-IPV4 peer.");
            continue;
        }

        sockaddr_p = (struct sockaddr_in *)&sockaddr_accpet;
        handle_accepted_connection(sockfd, sockaddr_p);
    }
}

static void
server_clean (void)
{
    task_queue_clean(&request_tqueue);
    close(epoll_fd);
    close(listen_fd);
}

int main (void)
{
    int rc;

    rc = server_init();
    if (rc != 0) {
        return -1;
    }

    rc = start_threads();
    if (rc != 0) {
        server_clean();
        return -1;
    }

    wait_on_client_connection();
    server_clean();
    return 0;
}

