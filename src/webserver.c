#include "server.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void handle_frontend(int client_fd, void *context) {
    const Config *cfg = context;
    HttpMessage request;
    int status = read_request(client_fd, &request);
    char destination[HOST_LIMIT + 32] = "parser";
    if (status)
        send_error(client_fd, status, NULL, &request);
    else {
        const Route *route = route_find(cfg, request.path);
        if (route) {
            snprintf(destination, sizeof(destination), "%s:%s", route->host, route->port);
            status = serve_proxy(client_fd, &request, route);
        } else {
            strcpy(destination, "static");
            if (strcmp(request.method, "GET") != 0) {
                status = 405;
                send_error(client_fd, status, "GET", &request);
            } else
                status = serve_static(client_fd, cfg, &request);
        }
    }
    request_log(&request, destination, status);
    message_free(&request);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s CONFIG\n", argv[0]);
        return 1;
    }
    Config cfg;
    if (config_load(argv[1], &cfg) < 0)
        return 1;
    int result = run_server(cfg.listen_host, cfg.listen_port, handle_frontend, &cfg);
    close(cfg.root_fd);
    return result < 0 ? 1 : 0;
}
