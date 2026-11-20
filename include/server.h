#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define HEADER_LIMIT 16384u
#define URI_LIMIT 2048u
#define BODY_LIMIT 1048576u
#define HEADER_COUNT_LIMIT 100u
#define PROXY_HEADER_LIMIT (HEADER_LIMIT + 1024u)
#define BACKEND_HEADER_COUNT_LIMIT (HEADER_COUNT_LIMIT + 3u)
#define IO_TIMEOUT_MS 5000
#define SHUTDOWN_GRACE_MS 6000
#define LISTEN_BACKLOG 128
#define ROUTE_LIMIT 32u
#define HOST_LIMIT 255u
#define PATH_LIMIT 4096u
#define FILE_CHUNK_SIZE 65536u

typedef struct {
    char *name;
    char *value;
} Header;

typedef struct {
    char head[PROXY_HEADER_LIMIT + 1];
    char method[32];
    char target[URI_LIMIT + 1];
    char path[URI_LIMIT + 1];
    int http11;
    int status;
    char reason[96];
    Header headers[BACKEND_HEADER_COUNT_LIMIT];
    size_t header_count;
    size_t content_length;
    int has_length;
    unsigned char *body;
} HttpMessage;

typedef struct {
    char path[URI_LIMIT + 1];
    char host[HOST_LIMIT + 1];
    char port[6];
} Route;

typedef struct {
    char listen_host[HOST_LIMIT + 1];
    char listen_port[6];
    char root[PATH_LIMIT];
    int root_fd;
    Route routes[ROUTE_LIMIT];
    size_t route_count;
} Config;

int64_t now_ms(void);
int wait_fd(int fd, short events, int64_t deadline);
ssize_t recv_deadline(int fd, void *buffer, size_t size, int64_t deadline);
int send_all_until(int fd, const void *data, size_t size, int64_t deadline);
int send_all(int fd, const void *data, size_t size);
int set_nonblocking(int fd);
int listen_socket(const char *host, const char *port);
int connect_backend(const char *host, const char *port);

int parse_request_head(const char *data, size_t length, HttpMessage *message);
int parse_response_head(const char *data, size_t length, HttpMessage *message);
const char *header_get(const HttpMessage *message, const char *name);
int header_is_hop(const HttpMessage *message, const char *name);
int read_request(int fd, HttpMessage *message);
int read_backend_request(int fd, HttpMessage *message);
int read_response(int fd, HttpMessage *message, int head_only);
void message_free(HttpMessage *message);
const char *status_reason(int status);
int send_head(int fd, int status, const char *reason, const char *type,
              size_t length, const char *allow);
int send_response(int fd, int status, const char *reason, const char *type,
                  const void *body, size_t length, const char *allow);
void send_error(int fd, int status, const char *allow, const HttpMessage *request);
void request_log(const HttpMessage *message, const char *destination, int status);

int valid_port(const char *port);
int config_load(const char *filename, Config *config);
const Route *route_find(const Config *config, const char *path);
const char *mime_type(const char *path);
int open_static(int root_fd, const char *path, int *status);
int serve_static(int client_fd, const Config *config, const HttpMessage *request);
int build_proxy_head(const HttpMessage *request, const Route *route,
                     char *output, size_t capacity);
int serve_proxy(int client_fd, const HttpMessage *request, const Route *route);

typedef void (*ConnectionHandler)(int client_fd, void *context);
int run_server(const char *host, const char *port, ConnectionHandler handler, void *context);

#endif
