#include "server.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static int append(char *out, size_t capacity, size_t *used, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int n = vsnprintf(out + *used, capacity - *used, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= capacity - *used)
        return -1;
    *used += (size_t)n;
    return 0;
}

int build_proxy_head(const HttpMessage *request, const Route *route, char *out, size_t capacity) {
    size_t used = 0;
    int ipv6 = strchr(route->host, ':') != NULL;
    if (!capacity || append(out, capacity, &used,
        "%s %s HTTP/1.1\r\nHost: %s%s%s:%s\r\nContent-Length: %zu\r\nConnection: close\r\n",
        request->method, request->target, ipv6 ? "[" : "", route->host,
        ipv6 ? "]" : "", route->port, request->content_length) < 0)
        return -1;
    for (size_t i = 0; i < request->header_count; i++) {
        const Header *h = &request->headers[i];
        if (strcasecmp(h->name, "Host") == 0 || strcasecmp(h->name, "Content-Length") == 0 ||
            strcasecmp(h->name, "Expect") == 0 || header_is_hop(request, h->name))
            continue;
        if (append(out, capacity, &used, "%s: %s\r\n", h->name, h->value) < 0)
            return -1;
    }
    if (append(out, capacity, &used, "\r\n") < 0)
        return -1;
    return (int)used;
}

int serve_proxy(int client_fd, const HttpMessage *request, const Route *route) {
    char head[PROXY_HEADER_LIMIT];
    int length = build_proxy_head(request, route, head, sizeof(head));
    if (length < 0) {
        send_error(client_fd, 500, NULL, request);
        return 500;
    }
    int backend_fd = connect_backend(route->host, route->port);
    if (backend_fd < 0) {
        int status = errno == ETIMEDOUT ? 504 : 502;
        send_error(client_fd, status, NULL, request);
        return status;
    }
    int64_t deadline = now_ms() + IO_TIMEOUT_MS;
    if (send_all_until(backend_fd, head, (size_t)length, deadline) < 0 ||
        send_all_until(backend_fd, request->body, request->content_length, deadline) < 0) {
        int status = errno == ETIMEDOUT ? 504 : 502;
        close(backend_fd);
        send_error(client_fd, status, NULL, request);
        return status;
    }
    HttpMessage response;
    int head_only = strcmp(request->method, "HEAD") == 0;
    int status = read_response(backend_fd, &response, head_only);
    close(backend_fd);
    if (status)
        send_error(client_fd, status, NULL, request);
    else {
        const char *type = header_get(&response, "Content-Type");
        if (!type || header_is_hop(&response, "Content-Type"))
            type = "application/octet-stream";
        status = response.status;
        const char *allow = header_is_hop(&response, "Allow") ? NULL : header_get(&response, "Allow");
        /* Buffering within BODY_LIMIT lets all upstream errors become one clean 502/504. */
        if (head_only)
            (void)send_head(client_fd, status, response.reason, type, response.content_length, allow);
        else
            (void)send_response(client_fd, status, response.reason, type,
                                response.body, response.content_length, allow);
    }
    message_free(&response);
    return status;
}
