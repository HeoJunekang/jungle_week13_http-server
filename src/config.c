#include "server.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int valid_port(const char *port) {
    unsigned long value = 0;
    if (!port || !*port || strlen(port) > 5)
        return 0;
    for (const unsigned char *p = (const unsigned char *)port; *p; p++) {
        if (!isdigit(*p))
            return 0;
        value = value * 10 + (*p - '0');
    }
    return value > 0 && value <= 65535;
}

static int host_valid(const char *host) {
    if (!*host || strlen(host) > HOST_LIMIT)
        return 0;
    for (const unsigned char *p = (const unsigned char *)host; *p; p++)
        if (!isalnum(*p) && !strchr(".-_:%", *p))
            return 0;
    return 1;
}

static int config_error(size_t line, const char *item) {
    if (line)
        fprintf(stderr, "config line %zu: invalid or duplicate %s\n", line, item);
    else
        fprintf(stderr, "config: missing required %s\n", item);
    return -1;
}

int config_load(const char *filename, Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->root_fd = -1;
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror(filename);
        return -1;
    }
    char *line = NULL;
    size_t capacity = 0, number = 0;
    int listen_seen = 0, root_seen = 0, result = -1;
    ssize_t count;
    while ((count = getline(&line, &capacity, file)) >= 0) {
        number++;
        if ((size_t)count > PATH_LIMIT * 2 || memchr(line, '\0', (size_t)count)) {
            config_error(number, "line");
            goto done;
        }
        char *start = line;
        while (isspace((unsigned char)*start))
            start++;
        if (!*start || *start == '#')
            continue;
        char *tokens[5], *save = NULL;
        size_t n = 0;
        for (char *token = strtok_r(start, " \t\r\n", &save); token;
             token = strtok_r(NULL, " \t\r\n", &save)) {
            if (n == 5)
                break;
            tokens[n++] = token;
        }
        if (strcmp(tokens[0], "listen") == 0) {
            if (n != 3 || listen_seen || !host_valid(tokens[1]) || !valid_port(tokens[2])) {
                config_error(number, "listen");
                goto done;
            }
            strcpy(cfg->listen_host, tokens[1]);
            strcpy(cfg->listen_port, tokens[2]);
            listen_seen = 1;
        } else if (strcmp(tokens[0], "root") == 0) {
            if (n != 2 || root_seen) {
                config_error(number, "root");
                goto done;
            }
            int length;
            if (tokens[1][0] == '/')
                length = snprintf(cfg->root, sizeof(cfg->root), "%s", tokens[1]);
            else {
                const char *slash = strrchr(filename, '/');
                size_t prefix = slash ? (size_t)(slash - filename + 1) : 0;
                length = snprintf(cfg->root, sizeof(cfg->root), "%.*s%s", (int)prefix, filename, tokens[1]);
            }
            if (length < 0 || (size_t)length >= sizeof(cfg->root)) {
                config_error(number, "root path length");
                goto done;
            }
            root_seen = 1;
        } else if (strcmp(tokens[0], "route") == 0) {
            if (n != 4 || cfg->route_count == ROUTE_LIMIT || tokens[1][0] != '/' ||
                strlen(tokens[1]) > URI_LIMIT || strpbrk(tokens[1], "?#") ||
                !host_valid(tokens[2]) || !valid_port(tokens[3]) || route_find(cfg, tokens[1])) {
                config_error(number, "route");
                goto done;
            }
            Route *route = &cfg->routes[cfg->route_count++];
            strcpy(route->path, tokens[1]);
            strcpy(route->host, tokens[2]);
            strcpy(route->port, tokens[3]);
        } else {
            config_error(number, "directive");
            goto done;
        }
    }
    if (ferror(file)) {
        perror("read config");
        goto done;
    }
    if (!listen_seen || !root_seen || !cfg->route_count) {
        config_error(0, !listen_seen ? "listen" : !root_seen ? "root" : "route");
        goto done;
    }
    cfg->root_fd = open(cfg->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (cfg->root_fd < 0) {
        fprintf(stderr, "config root %s: %s\n", cfg->root, strerror(errno));
        goto done;
    }
    result = 0;
done:
    free(line);
    fclose(file);
    return result;
}

const Route *route_find(const Config *cfg, const char *path) {
    for (size_t i = 0; i < cfg->route_count; i++)
        if (strcmp(cfg->routes[i].path, path) == 0)
            return &cfg->routes[i];
    return NULL;
}
