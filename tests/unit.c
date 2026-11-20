#include "server.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int request(const char *text, HttpMessage *m) {
    return parse_request_head(text, strlen(text), m);
}

static void test_requests(void) {
    HttpMessage m;
    assert(request("POST /api/echo?q=1 HTTP/1.1\r\nhOsT: localhost\r\ncontent-length: 6\r\nConnection: close\r\n\r\n", &m) == 0);
    assert(strcmp(m.method, "POST") == 0);
    assert(strcmp(m.target, "/api/echo?q=1") == 0);
    assert(strcmp(m.path, "/api/echo") == 0);
    assert(m.content_length == 6);
    assert(strcmp(header_get(&m, "connection"), "close") == 0);
    assert(request("GET / HTTP/1.0\r\n\r\n", &m) == 0);
    const char *bad[] = {
        "GET / HTTP/1.1\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
        "GET / HTTP/2.0\r\nHost: a\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a\r\nBad Header: x\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a\r\n folded: x\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a b\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: +1\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1, 1\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 9999999999999999999999999999999\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n",
        "GET /a#b HTTP/1.1\r\nHost: a\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a\r\nConnection: a,,b\r\n\r\n"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        assert(request(bad[i], &m) == 400);
    assert(request("POST / HTTP/1.1\r\nHost: a\r\n\r\n", &m) == 411);
    assert(request("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n\r\n", &m) == 0);
    assert(request("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1048577\r\n\r\n", &m) == 413);
    char large[4096];
    memset(large, 'a', sizeof(large));
    memcpy(large, "GET /", 5);
    strcpy(large + 2054, " HTTP/1.1\r\nHost: a\r\n\r\n");
    assert(request(large, &m) == 414);
}

static void test_responses(void) {
    HttpMessage m;
    const char *good = "HTTP/1.1 201 Created\r\nContent-Length: 3\r\nContent-Type: text/plain\r\n\r\n";
    assert(parse_response_head(good, strlen(good), &m) == 0);
    assert(m.status == 201 && m.content_length == 3);
    const char *bad[] = {
        "HTTP/1.1 200 OK\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
        "HTTP/1.1 999 Unknown\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 204 No Content\r\nContent-Length: 2\r\n\r\n"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        assert(parse_response_head(bad[i], strlen(bad[i]), &m) == 502);
}

static void test_routing_and_proxy(void) {
    Config cfg = {0};
    cfg.route_count = 1;
    strcpy(cfg.routes[0].path, "/api/echo");
    strcpy(cfg.routes[0].host, "localhost");
    strcpy(cfg.routes[0].port, "8001");
    assert(route_find(&cfg, "/api/echo") == &cfg.routes[0]);
    assert(route_find(&cfg, "/api/echo/extra") == NULL);
    assert(strcmp(mime_type("a.html"), "text/html; charset=utf-8") == 0);
    assert(strcmp(mime_type("a.CSS"), "text/css; charset=utf-8") == 0);
    assert(strcmp(mime_type("a.js"), "application/javascript; charset=utf-8") == 0);
    assert(strcmp(mime_type("a.png"), "image/png") == 0);
    assert(strcmp(mime_type("a.jpg"), "image/jpeg") == 0);
    assert(strcmp(mime_type("a.bin"), "application/octet-stream") == 0);
    HttpMessage m;
    assert(request("POST /api/echo?q=1 HTTP/1.1\r\nHost: old\r\nContent-Length: 3\r\nConnection: keep-alive, X-Secret\r\nX-Secret: hidden\r\nContent-Type: text/plain\r\nX-End: retained\r\n\r\n", &m) == 0);
    char out[PROXY_HEADER_LIMIT];
    int n = build_proxy_head(&m, &cfg.routes[0], out, sizeof(out));
    assert(n > 0 && (size_t)n == strlen(out));
    assert(strstr(out, "POST /api/echo?q=1 HTTP/1.1\r\n") == out);
    assert(strstr(out, "Host: localhost:8001\r\n"));
    assert(strstr(out, "Content-Length: 3\r\n"));
    assert(strstr(out, "Connection: close\r\n"));
    assert(strstr(out, "Content-Type: text/plain\r\n"));
    assert(strstr(out, "X-End: retained\r\n"));
    assert(!strstr(out, "X-Secret") && !strstr(out, "keep-alive") && !strstr(out, "old"));
}

int main(void) {
    test_requests();
    test_responses();
    test_routing_and_proxy();
    puts("unit: all assertions passed");
    return 0;
}
