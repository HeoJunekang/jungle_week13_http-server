#include "server.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef BACKEND_ECHO
static const char endpoint[] = "/api/echo";
static const char method[] = "POST";
static const char name[] = "echo";
#else
static const char endpoint[] = "/api/time";
static const char method[] = "GET";
static const char name[] = "time";
#endif

static void handle_backend(int client_fd, void *context) {
    (void)context;
    HttpMessage request;
    int status = read_backend_request(client_fd, &request);
    if (status)
        send_error(client_fd, status, NULL, &request);
    else if (strcmp(request.path, endpoint) != 0) {
        status = 404;
        send_error(client_fd, status, NULL, &request);
    } else if (strcmp(request.method, method) != 0) {
        status = 405;
        send_error(client_fd, status, method, &request);
    } else {
        status = 200;
#ifdef BACKEND_ECHO
        (void)send_response(client_fd, status, NULL, "application/octet-stream",
                            request.body, request.content_length, NULL);
#else
        time_t now = time(NULL);
        struct tm utc;
        char text[32];
        if (now == (time_t)-1 || !gmtime_r(&now, &utc) ||
            !strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc)) {
            status = 500;
            send_error(client_fd, status, NULL, &request);
        } else
            (void)send_response(client_fd, status, NULL, "text/plain; charset=utf-8", text, strlen(text), NULL);
#endif
    }
    request_log(&request, name, status);
    message_free(&request);
}

int main(int argc, char **argv) {
    if (argc != 2 || !valid_port(argv[1])) {
        fprintf(stderr, "usage: %s PORT (1..65535)\n", argv[0]);
        return 1;
    }
    return run_server("127.0.0.1", argv[1], handle_backend, NULL) < 0 ? 1 : 0;
}
