#include <errno.h>
#include <stdlib.h>
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

#define SERVER_LISTEN_PORT 19999

int main (void)
{
    int rc, one, listen_fd, sockfd;
    struct sockaddr_storage sockaddr_accept;
    struct sockaddr_in sockaddr_in;
    struct sockaddr_in *sockaddr_p;
    char ip[INET_ADDRSTRLEN+1];
    int addr_len;
    char *err;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd  < 0) {
        logger(ERROR, "Fail to open socket.");
        return -1;
    }
    logger(DEBUG, "Create listen sockfd %d", listen_fd);

    one = 1;
    rc = setsockopt(listen_fd, IPPROTO_TCP, TCP_NODELAY,
                    &one, sizeof(one));
    if (rc != 0) {
        logger(ERROR, "Fail to set sockopt");
        return -1;
    }

    bzero((char *)&sockaddr_in, sizeof(struct sockaddr_in));
    sockaddr_in.sin_family = AF_INET;
    sockaddr_in.sin_addr.s_addr = INADDR_ANY;
    sockaddr_in.sin_port = htons(SERVER_LISTEN_PORT);

    rc = bind(listen_fd, (struct sockaddr *)&sockaddr_in, sizeof(struct sockaddr_in));
    if (rc != 0) {
        logger(ERROR, "Fail to bind to port %d", SERVER_LISTEN_PORT);
        printf("Fail to bind to port %d.\n", SERVER_LISTEN_PORT);
        return -1;
    }

    rc = listen(listen_fd, 0);
    if (rc != 0) {
        err = strerror(errno);
        printf("Fail to listen to socket, %s.\n", err);
        return -1;
    }


    for (;;) {
        addr_len = sizeof(struct sockaddr_storage);
        sockfd = accept(listen_fd, (struct sockaddr *)&sockaddr_accept, (socklen_t *)&addr_len);
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
        sockaddr_p = (struct sockaddr_in *)&sockaddr_accept;
        inet_ntop(AF_INET, &sockaddr_p->sin_addr,
                  ip, INET_ADDRSTRLEN+1);
        printf("Accept sock %d, %s:%d\n", sockfd, ip, sockaddr_p->sin_port);
    }

    return 0;
}
