#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

const char *mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext)
        return "application/octet-stream";
    if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcasecmp(ext, ".css") == 0)
        return "text/css; charset=utf-8";
    if (strcasecmp(ext, ".js") == 0)
        return "application/javascript; charset=utf-8";
    if (strcasecmp(ext, ".png") == 0)
        return "image/png";
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0)
        return "image/jpeg";
    return "application/octet-stream";
}

static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int decode_path(const char *path, char *out) {
    size_t j = 0;
    for (size_t i = 0; path[i]; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c == '%') {
            if (!path[i + 1] || !path[i + 2])
                return 400;
            int high = hex_value((unsigned char)path[i + 1]);
            int low = hex_value((unsigned char)path[i + 2]);
            if (high < 0 || low < 0)
                return 400;
            c = (unsigned char)(high * 16 + low);
            i += 2;
        }
        if (c < 32 || c == 127 || c == '\\')
            return 403;
        if (j == URI_LIMIT)
            return 414;
        out[j++] = (char)c;
    }
    out[j] = '\0';
    return 0;
}

static int file_error(void) {
    switch (errno) {
    case ENOENT: return 404;
    case EACCES: case EPERM: case ELOOP: case ENOTDIR: return 403;
    case ENAMETOOLONG: return 414;
    default: return 500;
    }
}

int open_static(int root_fd, const char *path, int *status) {
    char decoded[URI_LIMIT + 1];
    *status = decode_path(path, decoded);
    if (*status)
        return -1;
    if (decoded[0] != '/') {
        *status = 403;
        return -1;
    }
    if (strcmp(decoded, "/") == 0)
        strcpy(decoded, "/index.html");
    /* Validate every component before walking, even if an earlier one is absent. */
    for (const char *p = decoded + 1; *p;) {
        const char *end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if ((n == 1 && *p == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) {
            *status = 403;
            return -1;
        }
        p += n;
        if (*p)
            p++;
    }
    int directory = dup(root_fd);
    if (directory < 0) {
        *status = 500;
        return -1;
    }
    char *component = decoded + 1;
    int fd = -1;
    for (;;) {
        char *slash = strchr(component, '/');
        if (slash)
            *slash = '\0';
        if (!*component) {
            *status = 403;
            break;
        }
        struct stat entry;
        if (fstatat(directory, component, &entry, AT_SYMLINK_NOFOLLOW) < 0) {
            *status = file_error();
            break;
        }
        if ((slash && !S_ISDIR(entry.st_mode)) || (!slash && !S_ISREG(entry.st_mode))) {
            *status = 403;
            break;
        }
        int flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
        if (slash)
            flags |= O_DIRECTORY;
        fd = openat(directory, component, flags);
        if (fd < 0) {
            *status = file_error();
            break;
        }
        close(directory);
        directory = -1;
        if (!slash) {
            struct stat info;
            if (fstat(fd, &info) < 0)
                *status = 500;
            else if (!S_ISREG(info.st_mode))
                *status = 403;
            else {
                *status = 200;
                return fd;
            }
            close(fd);
            fd = -1;
            break;
        }
        directory = fd;
        fd = -1;
        component = slash + 1;
    }
    if (directory >= 0)
        close(directory);
    return -1;
}

int serve_static(int client_fd, const Config *cfg, const HttpMessage *request) {
    int status;
    int file_fd = open_static(cfg->root_fd, request->path, &status);
    if (file_fd < 0) {
        send_error(client_fd, status, NULL, request);
        return status;
    }
    struct stat info;
    if (fstat(file_fd, &info) < 0 || info.st_size < 0 || (uintmax_t)info.st_size > SIZE_MAX) {
        close(file_fd);
        send_error(client_fd, 500, NULL, request);
        return 500;
    }
    char decoded[URI_LIMIT + 1];
    (void)decode_path(request->path, decoded);
    const char *type = strcmp(decoded, "/") == 0 ? mime_type("index.html") : mime_type(decoded);
    if (send_head(client_fd, 200, NULL, type, (size_t)info.st_size, NULL) == 0) {
        unsigned char buffer[FILE_CHUNK_SIZE];
        size_t remaining = (size_t)info.st_size;
        int64_t deadline = now_ms() + IO_TIMEOUT_MS;
        while (remaining) {
            size_t want = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
            ssize_t n = read(file_fd, buffer, want);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0 || send_all_until(client_fd, buffer, (size_t)n, deadline) < 0)
                break; /* Headers already sent: close instead of writing a second response. */
            remaining -= (size_t)n;
        }
    }
    close(file_fd);
    return 200;
}
