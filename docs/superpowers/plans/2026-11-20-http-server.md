# C HTTP Server Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement the tasks in order.

**Goal:** Implement DEVELOPMENT.md with three standalone C/POSIX servers and reproducible acceptance tests.

**Architecture:** A shared HTTP parser and bounded socket I/O serve the frontend and both backends. The parent accepts and forks, a child handles one request, and the parent reaps children through a signal pipe. Static files use descriptor-relative traversal; proxy responses are validated completely before forwarding.

**Tech Stack:** C11, POSIX sockets/processes, Make, Python 3 standard-library tests.

**Spec:** ../../../DEVELOPMENT.md

## Global Constraints

- macOS and Linux; no HTTP library/framework.
- One child per connection; one request/response; Connection: close.
- All addresses, ports, document root and exact route mappings come from configuration.
- Binary-safe bodies, strict HTTP framing, bounded input and I/O timeouts.
- Preserve the existing DEVELOPMENT.md; work directly in the supplied repository, leave changes reviewable without committing or publishing.

## Review Focus

- Symlink components, encoded traversal and special files must never expose files outside the static root.
- Duplicate/conflicting length headers and hop-by-hop nominations must not create ambiguous framing.
- Split reads, early EOF and client resets must release resources and leave the parent alive.
- An unavailable/malformed/slow backend must become 502/504 before a response starts.
- Signals arriving between accept, fork and poll must not lose children or leave zombies.

### Task 1: HTTP and configuration foundations

**Files:** include/server.h, src/http.c, src/config.c, src/io.c, tests/unit.c, Makefile.
**Interfaces:** HttpMessage; parse_request_head/parse_response_head; read_request/read_response; send_response; Config; config_load; route_find; mime_type; open_static; connect_backend.

- [x] Write unit assertions for strict framing, URI limits, MIME, exact routes, proxy serialization and backend response validation; run and observe missing implementation.
- [x] Implement bounded CRLF parsing and deadline-based partial reads/writes, config validation with line diagnostics.
- [x] Run `make unit`; expect all assertions to pass with compiler warnings treated as errors.

### Task 2: Servers, static delivery and proxy

**Files:** src/process.c, src/static.c, src/proxy.c, src/webserver.c, src/backend.c, server.conf, www/*, tests/integration.py.
**Interfaces:** run_server(host, port, handler, context); handle callbacks consume HttpMessage and Config from Task 1.

- [x] Write real-socket integration tests for static bytes, UTF-8/binary echo, time, malformed requests, concurrency, backend faults, config diagnostics, shutdown and resource recovery; observe missing servers.
- [x] Implement descriptor-safe static files, rebuilt proxy requests, bounded response validation, fork/signal lifecycle and backend handlers.
- [x] Add a browser demo using same-origin fetch and separate HTML/CSS/JS.
- [x] Run `make test`; expect unit + integration tests to pass.

### Task 3: Verification and handoff

**Files:** tests/benchmark.py, README.md, docs/validation.md.

- [x] Measure HTTP/1.0 non-keepalive ab workloads with concurrency 1/10/50, recording environment, commands, throughput and failures.
- [x] Run the suite under AddressSanitizer/UndefinedBehaviorSanitizer; inspect child and FD cleanup separately.
- [x] Review code against the spec, correct test-backed defects, and rerun the complete suite.
- [x] Document quick start, constants, exact function traces, CS:APP concepts, measured limitations and a four-minute demo. Record platform checks actually performed; leave after-merge checks clearly identified.

## Execution record

- Initial tests failed for absent server executable and missing implementation source.
- Unit assertions passed after HTTP/config/static/proxy implementations. macOS exposes O_NOFOLLOW with _DARWIN_C_SOURCE, added alongside the POSIX feature macro.
- First full integration pass: 17 tests. Final pass after review regressions: 21 tests on macOS and Linux. ASan/UBSan final pass: 21 tests, no diagnostics.
- Independent read-only review identified HEAD/bodyless response framing, generated proxy-header overhead, and Unix socket classification. Reproduced failures before fixes; regression tests now pass. Proxy-Connection stripping and HTTP/1.0 Expect handling were also corrected.
- Ruling: system getaddrinfo timeout remains separate from socket deadlines; documented in README. Default localhost/numeric loopback avoids reliance on external DNS. A slow external resolver may exceed IO_TIMEOUT_MS.
- Chrome verified rendered HTML/CSS/JS, UTF-8 6-byte equality and UTC response. curl verified response framing and actual PNG/JPG fixture bytes.
- ab: 4,000 requests across five workloads, no failures/non-2xx; stable parent FD counts and zero remaining children. See docs/validation.md.
- Work remains uncommitted for user review; no merge, push or publishing performed. Repeat validation after final merge.
