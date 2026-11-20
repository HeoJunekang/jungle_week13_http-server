CC ?= cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -Iinclude
CFLAGS ?= -std=c11 -O2 -g
CFLAGS += -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=

COMMON = src/http.c src/io.c src/process.c src/config.c
FRONT = src/webserver.c src/static.c src/proxy.c
HEADERS = include/server.h

.PHONY: all unit test clean sanitize benchmark
all: webserver echo_backend time_backend

webserver: $(COMMON) $(FRONT) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(COMMON) $(FRONT) $(LDFLAGS) -o $@

echo_backend: $(COMMON) src/backend.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DBACKEND_ECHO $(COMMON) src/backend.c $(LDFLAGS) -o $@

time_backend: $(COMMON) src/backend.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(COMMON) src/backend.c $(LDFLAGS) -o $@

tests/unit: tests/unit.c src/http.c src/io.c src/config.c src/static.c src/proxy.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit.c src/http.c src/io.c src/config.c src/static.c src/proxy.c $(LDFLAGS) -o $@

unit: tests/unit
	./tests/unit

test: all unit
	python3 tests/integration.py

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' test

benchmark: all
	python3 tests/benchmark.py

clean:
	rm -f webserver echo_backend time_backend tests/unit
