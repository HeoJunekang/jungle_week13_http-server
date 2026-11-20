#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int wait_fd(int fd, short events, int64_t deadline) {
    struct pollfd pfd = {.fd = fd, .events = events};
    for (;;) {
        int64_t remaining = deadline - now_ms();
        if (remaining <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        int result = poll(&pfd, 1, remaining > INT_MAX ? INT_MAX : (int)remaining);
        if (result > 0) {
            if (pfd.revents & POLLNVAL) {
                errno = EBADF;
                return -1;
            }
            /* HUP/ERR must reach recv/send to distinguish EOF from a reset. */
            return 0;
        }
        if (result == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (errno != EINTR)
            return -1;
    }
}

ssize_t recv_deadline(int fd, void *buffer, size_t size, int64_t deadline) {
    for (;;) {
        if (wait_fd(fd, POLLIN, deadline) < 0)
            return -1;
        ssize_t n = recv(fd, buffer, size, 0);
        if (n >= 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
            return n;
    }
}

int send_all_until(int fd, const void *data, size_t size, int64_t deadline) {
    const unsigned char *bytes = data;
    size_t sent = 0;
    while (sent < size) {
        if (wait_fd(fd, POLLOUT, deadline) < 0)
            return -1;
        ssize_t n = send(fd, bytes + sent, size - sent, 0);
        if (n > 0)
            sent += (size_t)n;
        else if (n == 0) {
            errno = EPIPE;
            return -1;
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
    }
    return 0;
}

int send_all(int fd, const void *data, size_t size) {
    return send_all_until(fd, data, size, now_ms() + IO_TIMEOUT_MS);
}

static int resolve(const char *host, const char *port, struct addrinfo **list) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int code = getaddrinfo(host, port, &hints, list);
    if (code != 0) {
        fprintf(stderr, "getaddrinfo %s:%s: %s\n", host, port, gai_strerror(code));
        errno = EHOSTUNREACH;
        return -1;
    }
    return 0;
}

int listen_socket(const char *host, const char *port) {
    struct addrinfo *list;
    if (resolve(host, port, &list) < 0)
        return -1;
    int fd = -1;
    int saved = EADDRNOTAVAIL;
    for (struct addrinfo *a = list; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) {
            saved = errno;
            continue;
        }
        int yes = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == 0 &&
            set_nonblocking(fd) == 0 &&
            bind(fd, a->ai_addr, a->ai_addrlen) == 0 &&
            listen(fd, LISTEN_BACKLOG) == 0)
            break;
        saved = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(list);
    if (fd < 0)
        errno = saved;
    return fd;
}

int connect_backend(const char *host, const char *port) {
    struct addrinfo *list;
    if (resolve(host, port, &list) < 0)
        return -1;
    int fd = -1;
    int saved = ECONNREFUSED;
    int64_t deadline = now_ms() + IO_TIMEOUT_MS;
    for (struct addrinfo *a = list; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) {
            saved = errno;
            continue;
        }
        if (set_nonblocking(fd) < 0) {
            saved = errno;
        } else if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) {
            break;
        } else if (errno == EINPROGRESS) {
            if (wait_fd(fd, POLLOUT, deadline) == 0) {
                int error = 0;
                socklen_t length = sizeof(error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0) {
                    if (error == 0)
                        break;
                    saved = error;
                } else
                    saved = errno;
            } else
                saved = errno;
        } else
            saved = errno;
        close(fd);
        fd = -1;
        if (now_ms() >= deadline) {
            saved = ETIMEDOUT;
            break;
        }
    }
    freeaddrinfo(list);
    if (fd < 0)
        errno = saved;
    return fd;
}
