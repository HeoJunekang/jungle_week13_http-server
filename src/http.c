#include "server.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static int token_char(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || (c && strchr("!#$%&'*+-.^_`|~", c));
}

static int token(const char *text) {
    if (!*text)
        return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (!token_char(*p))
            return 0;
    return 1;
}

static int connection_valid(const char *value) {
    const unsigned char *p = (const unsigned char *)value;
    for (;;) {
        while (*p == ' ' || *p == '\t')
            p++;
        const unsigned char *begin = p;
        while (token_char(*p))
            p++;
        if (p == begin)
            return 0;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            return 1;
        if (*p++ != ',')
            return 0;
    }
}

const char *header_get(const HttpMessage *m, const char *name) {
    for (size_t i = 0; i < m->header_count; i++)
        if (strcasecmp(m->headers[i].name, name) == 0)
            return m->headers[i].value;
    return NULL;
}

int header_is_hop(const HttpMessage *m, const char *name) {
    const char *fixed[] = {"Connection", "Keep-Alive", "Proxy-Authenticate",
        "Proxy-Authorization", "Proxy-Connection", "TE", "Trailer", "Transfer-Encoding", "Upgrade"};
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++)
        if (strcasecmp(name, fixed[i]) == 0)
            return 1;
    size_t name_length = strlen(name);
    for (size_t i = 0; i < m->header_count; i++) {
        if (strcasecmp(m->headers[i].name, "Connection") != 0)
            continue;
        const char *p = m->headers[i].value;
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == ',')
                p++;
            const char *start = p;
            while (token_char((unsigned char)*p))
                p++;
            if ((size_t)(p - start) == name_length && strncasecmp(start, name, name_length) == 0)
                return 1;
            while (*p == ' ' || *p == '\t')
                p++;
            if (*p && *p != ',')
                break;
        }
    }
    return 0;
}

static char *next_line(char **cursor) {
    char *line = *cursor;
    char *end = strstr(line, "\r\n");
    if (!end)
        return NULL;
    *end = '\0';
    *cursor = end + 2;
    return line;
}

static int parse_headers(char **cursor, HttpMessage *m, int response, size_t field_limit) {
    char *line;
    size_t host_count = 0, type_count = 0;
    while ((line = next_line(cursor)) != NULL && *line) {
        if (m->header_count == field_limit)
            return response ? 502 : 431;
        char *colon = strchr(line, ':');
        if (!colon)
            return 400;
        *colon = '\0';
        if (!token(line))
            return 400;
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t')
            value++;
        for (const unsigned char *p = (const unsigned char *)value; *p; p++)
            if ((*p < 32 && *p != '\t') || *p == 127)
                return 400;
        char *end = value + strlen(value);
        while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        m->headers[m->header_count++] = (Header){.name = line, .value = value};
        if (strcasecmp(line, "Host") == 0) {
            if (++host_count > 1 || !*value)
                return 400;
            for (const unsigned char *p = (const unsigned char *)value; *p; p++)
                if (*p <= 32 || *p == 127 || strchr("/\\,@?#", *p))
                    return 400;
        } else if (strcasecmp(line, "Content-Length") == 0) {
            if (m->has_length || !*value)
                return 400;
            size_t length = 0;
            for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
                if (*p < '0' || *p > '9' || length > (SIZE_MAX - (*p - '0')) / 10)
                    return 400;
                length = length * 10 + (*p - '0');
            }
            m->has_length = 1;
            m->content_length = length;
        } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
            return 400; /* No chunked or competing message framing. */
        } else if (strcasecmp(line, "Connection") == 0) {
            if (!connection_valid(value))
                return 400;
        } else if (strcasecmp(line, "Content-Type") == 0) {
            if (++type_count > 1 || !*value)
                return 400;
        }
    }
    if (!line)
        return 400;
    if (!response && m->http11 && host_count != 1)
        return 400;
    if (response && !m->has_length && m->status != 204 && m->status != 304)
        return 502;
    if (m->content_length > BODY_LIMIT && (!response || m->status != 304))
        return response ? 502 : 413;
    if (!response && strcmp(m->method, "POST") == 0 && !m->has_length)
        return 411;
    if (response && m->status == 204 && m->has_length)
        return 502;
    return 0;
}

static int prepare_head(const char *data, size_t length, HttpMessage *m, size_t limit) {
    memset(m, 0, sizeof(*m));
    if (length > limit)
        return 431;
    if (length < 4 || memchr(data, '\0', length) ||
        memcmp(data + length - 4, "\r\n\r\n", 4) != 0)
        return 400;
    memcpy(m->head, data, length);
    m->head[length] = '\0';
    return 0;
}

static int parse_request_limits(const char *data, size_t length, HttpMessage *m, int backend) {
    int error = prepare_head(data, length, m, backend ? PROXY_HEADER_LIMIT : HEADER_LIMIT);
    if (error)
        return error;
    char *cursor = m->head;
    char *line = next_line(&cursor);
    if (!line)
        return 400;
    char *space = strchr(line, ' ');
    if (!space)
        return 400;
    *space++ = '\0';
    if (!token(line) || strlen(line) >= sizeof(m->method))
        return 400;
    strcpy(m->method, line);
    char *version = strchr(space, ' ');
    if (!version)
        return 400;
    *version++ = '\0';
    if (strlen(space) > URI_LIMIT)
        return 414;
    if (space[0] != '/')
        return 400;
    for (const unsigned char *p = (const unsigned char *)space; *p; p++)
        if (*p <= 32 || *p == 127 || *p == '#')
            return 400;
    strcpy(m->target, space);
    strcpy(m->path, space);
    char *query = strchr(m->path, '?');
    if (query)
        *query = '\0';
    if (strcmp(version, "HTTP/1.1") == 0)
        m->http11 = 1;
    else if (strcmp(version, "HTTP/1.0") != 0)
        return 400;
    error = parse_headers(&cursor, m, 0, backend ? BACKEND_HEADER_COUNT_LIMIT : HEADER_COUNT_LIMIT);
    if (!error && cursor != m->head + length)
        return 400;
    return error;
}

int parse_request_head(const char *data, size_t length, HttpMessage *m) {
    return parse_request_limits(data, length, m, 0);
}

int parse_response_head(const char *data, size_t length, HttpMessage *m) {
    if (prepare_head(data, length, m, HEADER_LIMIT))
        return 502;
    char *cursor = m->head;
    char *line = next_line(&cursor);
    if (!line || strlen(line) < 13 ||
        (strncmp(line, "HTTP/1.1 ", 9) && strncmp(line, "HTTP/1.0 ", 9)) ||
        line[9] < '2' || line[9] > '5' || !isdigit((unsigned char)line[10]) ||
        !isdigit((unsigned char)line[11]) || line[12] != ' ')
        return 502;
    m->http11 = line[7] == '1';
    m->status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + line[11] - '0';
    if (strlen(line + 13) >= sizeof(m->reason))
        return 502;
    for (const unsigned char *p = (const unsigned char *)line + 13; *p; p++)
        if (*p < 32 || *p == 127)
            return 502;
    strcpy(m->reason, line + 13);
    if (parse_headers(&cursor, m, 1, HEADER_COUNT_LIMIT) || cursor != m->head + length)
        return 502;
    return 0;
}

static int read_message(int fd, HttpMessage *m, int kind, int head_only) {
    int response = kind == 1;
    size_t head_limit = kind == 2 ? PROXY_HEADER_LIMIT : HEADER_LIMIT;
    unsigned char buffer[PROXY_HEADER_LIMIT];
    size_t used = 0, head_length = 0;
    memset(m, 0, sizeof(*m));
    int64_t deadline = now_ms() + IO_TIMEOUT_MS;
    while (!head_length) {
        if (used == head_limit)
            return response ? 502 : 431;
        ssize_t n = recv_deadline(fd, buffer + used, head_limit - used, deadline);
        if (n <= 0)
            return response ? (n < 0 && errno == ETIMEDOUT ? 504 : 502) :
                              (n < 0 && errno == ETIMEDOUT ? 408 : 400);
        size_t start = used >= 3 ? used - 3 : 0;
        used += (size_t)n;
        if (!response) {
            unsigned char *first = memchr(buffer, ' ', used);
            if (first) {
                size_t rest = used - (size_t)(first + 1 - buffer);
                unsigned char *second = memchr(first + 1, ' ', rest);
                if ((second ? (size_t)(second - first - 1) : rest) > URI_LIMIT)
                    return 414;
            }
        }
        for (size_t i = start; i + 3 < used; i++)
            if (memcmp(buffer + i, "\r\n\r\n", 4) == 0) {
                head_length = i + 4;
                break;
            }
    }
    int error = response ? parse_response_head((char *)buffer, head_length, m) :
                           parse_request_limits((char *)buffer, head_length, m, kind == 2);
    if (error)
        return error;
    const char *expect = header_get(m, "Expect");
    if (!response && expect && m->http11) {
        if (strcasecmp(expect, "100-continue") != 0)
            return 417;
        static const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if (send_all_until(fd, interim, sizeof(interim) - 1, deadline) < 0)
            return 400;
    }
    if (!m->content_length || (response && (head_only || m->status == 204 || m->status == 304)))
        return 0;
    m->body = malloc(m->content_length);
    if (!m->body)
        return response ? 502 : 500;
    size_t received = used - head_length;
    if (received > m->content_length)
        received = m->content_length;
    memcpy(m->body, buffer + head_length, received);
    while (received < m->content_length) {
        ssize_t n = recv_deadline(fd, m->body + received, m->content_length - received, deadline);
        if (n <= 0)
            return response ? (n < 0 && errno == ETIMEDOUT ? 504 : 502) :
                              (n < 0 && errno == ETIMEDOUT ? 408 : 400);
        received += (size_t)n;
    }
    return 0;
}

int read_request(int fd, HttpMessage *m) {
    return read_message(fd, m, 0, 0);
}

int read_backend_request(int fd, HttpMessage *m) {
    return read_message(fd, m, 2, 0);
}

int read_response(int fd, HttpMessage *m, int head_only) {
    return read_message(fd, m, 1, head_only);
}

void message_free(HttpMessage *m) {
    free(m->body);
    m->body = NULL;
}

const char *status_reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 504: return "Gateway Timeout";
    default: return "Response";
    }
}

int send_head(int fd, int status, const char *reason, const char *type,
              size_t length, const char *allow) {
    char buffer[HEADER_LIMIT + 1024];
    char length_header[64] = "";
    if (status != 204 && status != 304)
        snprintf(length_header, sizeof(length_header), "Content-Length: %zu\r\n", length);
    int n = snprintf(buffer, sizeof(buffer),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n%sConnection: close\r\n%s%s%s\r\n",
        status, reason ? reason : status_reason(status), type, length_header,
        allow ? "Allow: " : "", allow ? allow : "", allow ? "\r\n" : "");
    if (n < 0 || (size_t)n >= sizeof(buffer)) {
        errno = EOVERFLOW;
        return -1;
    }
    return send_all(fd, buffer, (size_t)n);
}

int send_response(int fd, int status, const char *reason, const char *type,
                  const void *body, size_t length, const char *allow) {
    if (send_head(fd, status, reason, type, length, allow) < 0)
        return -1;
    if (status == 204 || status == 304)
        return 0;
    return send_all(fd, body, length);
}

void send_error(int fd, int status, const char *allow, const HttpMessage *request) {
    char body[128];
    int n = snprintf(body, sizeof(body), "%d %s\n", status, status_reason(status));
    if (n > 0 && (size_t)n < sizeof(body)) {
        if (request && strcmp(request->method, "HEAD") == 0)
            (void)send_head(fd, status, NULL, "text/plain; charset=utf-8", (size_t)n, allow);
        else
            (void)send_response(fd, status, NULL, "text/plain; charset=utf-8", body, (size_t)n, allow);
    }
}

void request_log(const HttpMessage *m, const char *destination, int status) {
    /* Request-line controls are rejected; fixed-size fields bound every log. */
    fprintf(stderr, "pid=%ld method=%s path=%s destination=%s status=%d\n",
        (long)getpid(), *m->method ? m->method : "-", *m->path ? m->path : "-",
        destination, status);
}
