# Validation measurements

- Measured at: 2026-11-20T05:50:23+00:00
- OS: macOS-15.7.7-x86_64-i386-64bit-Mach-O
- Machine: x86_64, 8 logical CPUs
- Compiler: Apple clang version 17.0.0 (clang-1700.6.4.2)
- Tool: This is ApacheBench, Version 2.3 <$Revision: 1923142 $>
- Build: Makefile default C11 / -O2; sanitizer disabled for throughput measurements.
- Transport: loopback, plain TCP, ab default HTTP/1.0, no -k / no keepalive.
- Static workload: repository www/index.html. Echo workload: UTF-8 안녕 (6 bytes).
- One run per row; these local measurements are not capacity guarantees.

| Resource | Requests | Concurrency | Requests/sec | Failed | Non-2xx | Mean request ms |
|---|---:|---:|---:|---:|---:|---:|
| / | 1000 | 1 | 493.59 | 0 | 0 | 2.026 |
| / | 1000 | 10 | 1654.75 | 0 | 0 | 6.043 |
| / | 1000 | 50 | 3228.52 | 0 | 0 | 15.487 |
| /api/echo | 500 | 10 | 1254.68 | 0 | 0 | 7.970 |
| /api/time | 500 | 10 | 1045.73 | 0 | 0 | 9.563 |

## Resource checks

| Parent | FD before | FD after |
|---|---:|---:|
| echo_backend | 6 | 6 |
| time_backend | 6 | 6 |
| webserver | 7 | 7 |

- Remaining child processes after workloads: 0.
- Commands (temporary paths and ephemeral ports belong to this run):

```sh
/usr/sbin/ab -n 1000 -c 1 -s 10 http://127.0.0.1:63533/
/usr/sbin/ab -n 1000 -c 10 -s 10 http://127.0.0.1:63533/
/usr/sbin/ab -n 1000 -c 50 -s 10 http://127.0.0.1:63533/
/usr/sbin/ab -n 500 -c 10 -s 10 -p /var/folders/8y/6w3pfv896rg984r6qx0027580000gn/T/c-http-bench-rbaqgww7/echo.txt -T text/plain http://127.0.0.1:63533/api/echo
/usr/sbin/ab -n 500 -c 10 -s 10 http://127.0.0.1:63533/api/time
```

- Parent exit codes after SIGTERM: [0, 0, 0].
- All parent PIDs were waited for; listener ports are checked below.
- All three listener ports successfully rebound after shutdown.

## Interpretation

Compare the three static rows: more simultaneous connections do not remove per-connection fork, scheduling and teardown costs. Each proxy request also creates a backend child and an outbound TCP connection. Blocked children retain process and socket resources until the deadline; this design does not scale like an event loop.
