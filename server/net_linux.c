// required for accept4 to be defined...
#define _GNU_SOURCE

#include <errno.h>      // errno
#include <string.h>     // strcpy, strerror
#include <stddef.h>     // size_t
#include <stdio.h>      // printf
#include <sys/epoll.h>  // epoll_create1, epoll_wait, epoll_event, ...
#include <sys/socket.h> // socket, bind, listen, send
#include <sys/un.h>     // sockaddr_un
#include <netinet/in.h> // sockaddr_in, INADDR_ANY, htons
#include <netinet/tcp.h> // TCP_NODELAY
#include <unistd.h>     // unlink, close
#include "net.h"

static void print_err(const char *context)
{
    printf("net error (%s): %s\n", context, strerror(errno));
}

int net_port(unsigned short port)
{
    int server = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (server == -1)
        goto abort;
    
    int reuse = 1;
    if (setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1)
        goto abort;
    
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    
    if (bind(server, (struct sockaddr *) &address, sizeof(address)) == -1)
        goto abort;
    
    if (listen(server, SOMAXCONN) == -1)
        goto abort;
    
    return server;
    
    abort:
    print_err("net_port");
    close(server);
    return -1;
}

int net_sock(char *path)
{
    if (!path) {
        printf("error: no path provided for socket.\n");
        return -1;
    }
    
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server == -1)
        goto abort;
    
    int reuse = 1;
    if (setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1)
        goto abort;
    
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    if (unlink(path) == -1)
        goto abort;
    
    if (bind(server, (struct sockaddr *) &address, sizeof(address)) == -1)
        goto abort;
    
    if (listen(server, SOMAXCONN) == -1)
        goto abort;
    
    return server;
    
    abort:
    print_err("net_sock");
    close(server);
    return -1;
}

void net_listen(int server, net_handler *handler)
{
    static struct epoll_event events[32] = {0};
    static unsigned char read_buf[8192] = {0};
    
    if (!handler) {
        printf("net error: no socket request handler provided.\n");
        return;
    }
    
    int epoll_fd = 0;
    struct epoll_event event = {0};
    int ev_count = 0;
    
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1)
        goto abort;
    
    event.data.fd = server;
    event.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server, &event) == -1)
        goto abort;
    
    while (1) {
        ev_count = epoll_wait(epoll_fd, events, sizeof(events) / sizeof(*events), -1);
        if (ev_count == -1)
            goto abort;
        
        for (int i = 0; i < ev_count; i += 1) {
            
            // server
            if (events[i].data.fd == server) {
                while (1) {
                    int client = accept4(server, 0, 0, SOCK_NONBLOCK);
                    if (client == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
                        break;
                    if (client == -1) {
                        print_err("failed to accept new client in loop");
                        break;
                    }
                    // the game server batches its packets per tick,
                    // nagle would only add 40-200ms of extra latency.
                    int nodelay = 1;
                    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
                    event.data.fd = client;
                    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client, &event) == -1) {
                        print_err("failed to add new client to epoll");
                        close(client);
                    } else {
                        handler(client, net_conn, 0, 0);
                    }
                }
                continue;
            }
            
            // clients.
            int fd = events[i].data.fd;
            int closed = 0;
            if (events[i].events & EPOLLIN) {
                while (1) {
                    ssize_t received = recv(fd, read_buf, sizeof(read_buf), 0);
                    if (received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
                        break;
                    if (received == -1 && errno == EINTR)
                        continue;
                    if (received <= 0) {
                        closed = 1;
                        break;
                    }
                    handler(fd, net_read, read_buf, (size_t) received);
                }
            }
            if (events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP))
                closed = 1;
            if (closed) {
                // notify first, close after: the handler makes sure no
                // other thread uses this socket anymore before returning.
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &event);
                handler(fd, net_closed, 0, 0);
                close(fd);
            }
        }
    }
    
    abort:
    print_err("net_listen");
    close(epoll_fd);
    close(server);
}

unsigned long long net_send(int socket, void *buf, unsigned long long n)
{
    unsigned long long sent = 0;
    if (!buf)
        return 0;
    while (sent < n) {
        long long tmp = send(socket, (char *) buf + sent, n - sent, MSG_NOSIGNAL);
        if (tmp == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (tmp == -1 && errno == EINTR)
            continue;
        if (tmp == -1) {
            // the peer went away; the listen loop will report the close.
            if (errno != EPIPE && errno != ECONNRESET)
                print_err("net_write");
            break;
        }
        sent += tmp;
    }
    return sent;
}

void net_close(int socket)
{
    close(socket);
}

void net_shutdown(int socket)
{
    shutdown(socket, SHUT_RDWR);
}
