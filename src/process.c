#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct Child {
    pid_t pid;
    struct Child *next;
} Child;

static volatile sig_atomic_t stopping;
static volatile sig_atomic_t signal_fd = -1;

static void on_signal(int signum) {
    int saved = errno;
    if (signum == SIGINT || signum == SIGTERM)
        stopping = 1;
    if (signal_fd >= 0) {
        unsigned char byte = (unsigned char)signum;
        ssize_t ignored = write((int)signal_fd, &byte, 1);
        (void)ignored; /* A full pipe is already readable. */
    }
    errno = saved;
}

static void reap_children(Child **children) {
    pid_t pid;
    for (;;) {
        pid = waitpid(-1, NULL, WNOHANG);
        if (pid < 0 && errno == EINTR)
            continue;
        if (pid <= 0)
            break;
        Child **entry = children;
        while (*entry && (*entry)->pid != pid)
            entry = &(*entry)->next;
        if (*entry) {
            Child *old = *entry;
            *entry = old->next;
            free(old);
        }
    }
}

static void drain_pipe(int fd) {
    unsigned char bytes[128];
    while (read(fd, bytes, sizeof(bytes)) > 0) {}
}

static void finish_client(int fd) {
    /* Deliver an error even when the rejected request left buffered input. */
    (void)shutdown(fd, SHUT_WR);
    unsigned char buffer[4096];
    size_t drained = 0;
    while (drained < BODY_LIMIT + HEADER_LIMIT) {
        ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        drained += (size_t)n;
    }
    close(fd);
}

int run_server(const char *host, const char *port, ConnectionHandler handler, void *context) {
    int listener = listen_socket(host, port);
    if (listener < 0) {
        perror("listen");
        return -1;
    }
    int notifications[2];
    if (pipe(notifications) < 0) {
        perror("signal pipe");
        close(listener);
        return -1;
    }
    if (set_nonblocking(notifications[0]) < 0 || set_nonblocking(notifications[1]) < 0) {
        close(listener);
        close(notifications[0]);
        close(notifications[1]);
        return -1;
    }
    struct sigaction action, previous[4];
    const int signals[] = {SIGCHLD, SIGINT, SIGTERM, SIGPIPE};
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = on_signal;
    stopping = 0;
    signal_fd = notifications[1];
    size_t installed = 0;
    for (; installed < 4; installed++) {
        action.sa_handler = signals[installed] == SIGPIPE ? SIG_IGN : on_signal;
        if (sigaction(signals[installed], &action, &previous[installed]) < 0)
            break;
    }
    if (installed != 4) {
        while (installed) {
            installed--;
            sigaction(signals[installed], &previous[installed], NULL);
        }
        signal_fd = -1;
        close(listener);
        close(notifications[0]);
        close(notifications[1]);
        return -1;
    }
    fprintf(stderr, "parent=%ld listening=%s:%s\n", (long)getpid(), host, port);
    Child *children = NULL;
    sigset_t fork_mask;
    sigemptyset(&fork_mask);
    sigaddset(&fork_mask, SIGCHLD);
    sigaddset(&fork_mask, SIGINT);
    sigaddset(&fork_mask, SIGTERM);
    int result = 0;
    while (!stopping) {
        reap_children(&children);
        struct pollfd fds[2] = {{.fd = listener, .events = POLLIN},
                               {.fd = notifications[0], .events = POLLIN}};
        int ready = poll(fds, 2, -1);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            perror("poll listener");
            result = -1;
            break;
        }
        if (fds[1].revents)
            drain_pipe(notifications[0]);
        reap_children(&children);
        if (stopping)
            break;
        if (!(fds[0].revents & POLLIN))
            continue;
        int client_fd = accept(listener, NULL, NULL);
        if (client_fd < 0) {
            if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
                perror("accept");
            continue;
        }
        if (set_nonblocking(client_fd) < 0) {
            close(client_fd);
            continue;
        }
        Child *entry = malloc(sizeof(*entry));
        if (!entry) {
            close(client_fd);
            continue;
        }
        sigset_t previous_mask;
        sigprocmask(SIG_BLOCK, &fork_mask, &previous_mask);
        if (stopping) {
            close(client_fd);
            free(entry);
            sigprocmask(SIG_SETMASK, &previous_mask, NULL);
            break;
        }
        pid_t pid = fork();
        if (pid == 0) {
            close(listener);
            close(notifications[0]);
            close(notifications[1]);
            signal_fd = -1;
            action.sa_handler = SIG_DFL;
            sigaction(SIGCHLD, &action, NULL);
            sigaction(SIGINT, &action, NULL);
            sigaction(SIGTERM, &action, NULL);
            sigprocmask(SIG_SETMASK, &previous_mask, NULL);
            free(entry);
            handler(client_fd, context);
            finish_client(client_fd);
            _exit(0);
        }
        close(client_fd); /* The parent never owns request I/O. */
        if (pid < 0) {
            perror("fork");
            free(entry);
        } else {
            entry->pid = pid;
            entry->next = children;
            children = entry;
        }
        sigprocmask(SIG_SETMASK, &previous_mask, NULL);
    }
    close(listener);
    int64_t deadline = now_ms() + SHUTDOWN_GRACE_MS;
    reap_children(&children);
    while (children && now_ms() < deadline) {
        struct pollfd pfd = {.fd = notifications[0], .events = POLLIN};
        int64_t remaining = deadline - now_ms();
        if (remaining <= 0)
            break;
        (void)poll(&pfd, 1, (int)remaining);
        drain_pipe(notifications[0]);
        reap_children(&children);
    }
    for (Child *entry = children; entry; entry = entry->next)
        (void)kill(entry->pid, SIGTERM);
    while (children) {
        Child *entry = children;
        while (waitpid(entry->pid, NULL, 0) < 0 && errno == EINTR) {}
        children = entry->next;
        free(entry);
    }
    signal_fd = -1;
    for (size_t i = 0; i < 4; i++)
        sigaction(signals[i], &previous[i], NULL);
    close(notifications[0]);
    close(notifications[1]);
    return result;
}
