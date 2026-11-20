"""Reproducible ab HTTP/1.0 workload; writes measured evidence, never estimates."""
import argparse
import datetime
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import socket
import subprocess
import tempfile
import time

from integration import REPO, free_port


def descriptors(pid):
    proc = Path(f'/proc/{pid}/fd')
    if proc.is_dir():
        return len(list(proc.iterdir()))
    lsof = shutil.which('lsof') or '/usr/sbin/lsof'
    output = subprocess.check_output([lsof, '-a', '-p', str(pid), '-Ff'], text=True)
    return sum(row.startswith('f') and row[1:].isdigit() for row in output.splitlines())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=REPO / 'docs' / 'validation.md')
    args = parser.parse_args()
    ab = shutil.which('ab')
    if not ab:
        parser.error('ab is required (macOS: /usr/sbin/ab; Linux: apache2-utils/httpd-tools)')
    results = []
    processes = []
    report = [
        '# Validation measurements', '',
        f'- Measured at: {datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")}',
        f'- OS: {platform.platform()}',
        f'- Machine: {platform.machine()}, {os.cpu_count()} logical CPUs',
        f'- Compiler: {subprocess.check_output(["cc", "--version"], text=True).splitlines()[0]}',
        f'- Tool: {subprocess.check_output([ab, "-V"], text=True).splitlines()[0]}',
        '- Build: Makefile default C11 / -O2; sanitizer disabled for throughput measurements.',
        '- Transport: loopback, plain TCP, ab default HTTP/1.0, no -k / no keepalive.',
        '- Static workload: repository www/index.html. Echo workload: UTF-8 안녕 (6 bytes).',
        '- One run per row; these local measurements are not capacity guarantees.', '',
        '| Resource | Requests | Concurrency | Requests/sec | Failed | Non-2xx | Mean request ms |',
        '|---|---:|---:|---:|---:|---:|---:|',
    ]
    with tempfile.TemporaryDirectory(prefix='c-http-bench-') as directory:
        base = Path(directory)
        shutil.copytree(REPO / 'www', base / 'www')
        front, echo, clock = [free_port() for _ in range(3)]
        config = base / 'server.conf'
        config.write_text(f'listen 127.0.0.1 {front}\nroot ./www\n'
                          f'route /api/echo localhost {echo}\nroute /api/time localhost {clock}\n')
        payload = base / 'echo.txt'
        payload.write_bytes('안녕'.encode())
        commands = []
        with open(base / 'servers.log', 'w') as logs:
            try:
                for binary, arg, port in [('echo_backend', echo, echo), ('time_backend', clock, clock),
                                          ('webserver', config, front)]:
                    p = subprocess.Popen([str(REPO / binary), str(arg)], stdout=logs, stderr=logs)
                    processes.append(p)
                    for _ in range(100):
                        if p.poll() is not None:
                            raise RuntimeError(f'{binary} failed: {p.returncode}')
                        try:
                            with socket.create_connection(('127.0.0.1', port), timeout=.1):
                                break
                        except OSError:
                            time.sleep(.02)
                    else:
                        raise RuntimeError(f'{binary} failed to listen')
                time.sleep(.1)
                before = [descriptors(p.pid) for p in processes]
                workloads = [('/', 1000, c, []) for c in (1, 10, 50)]
                workloads += [('/api/echo', 500, 10, ['-p', str(payload), '-T', 'text/plain']),
                              ('/api/time', 500, 10, [])]
                for path, count, concurrency, extra in workloads:
                    cmd = [ab, '-n', str(count), '-c', str(concurrency), '-s', '10',
                           *extra, f'http://127.0.0.1:{front}{path}']
                    p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
                    if p.returncode:
                        raise RuntimeError(p.stdout + p.stderr)
                    output = p.stdout
                    def field(pattern, default=None):
                        match = re.search(pattern, output, re.M)
                        if not match and default is None:
                            raise RuntimeError(f'missing ab statistic: {pattern}\n{output}')
                        return match.group(1) if match else default
                    rps = field(r'^Requests per second:\s+([\d.]+)')
                    failed = field(r'^Failed requests:\s+(\d+)')
                    non2xx = field(r'^Non-2xx responses:\s+(\d+)', '0')
                    mean = field(r'^Time per request:\s+([\d.]+).*\(mean\)$')
                    report.append(f'| {path} | {count} | {concurrency} | {rps} | {failed} | {non2xx} | {mean} |')
                    commands.append(shlex.join(cmd))
                    print(f'{path} c={concurrency}: {rps} req/s, failed={failed}, non2xx={non2xx}', flush=True)
                    results.append((int(failed), int(non2xx)))
                time.sleep(.25)
                after = [descriptors(p.pid) for p in processes]
                ps = subprocess.check_output(['ps', '-axo', 'pid=,ppid=,stat='], text=True)
                parent_ids = {str(p.pid) for p in processes}
                children = [line.strip() for line in ps.splitlines() if line.split()[1] in parent_ids]
                report.extend(['', '## Resource checks', '',
                    '| Parent | FD before | FD after |', '|---|---:|---:|'])
                for name, a, b in zip(('echo_backend', 'time_backend', 'webserver'), before, after):
                    report.append(f'| {name} | {a} | {b} |')
                report += ['', f'- Remaining child processes after workloads: {len(children)}.',
                           '- Commands (temporary paths and ephemeral ports belong to this run):', '', '```sh', *commands, '```']
                if before != after or children:
                    raise RuntimeError(f'resource leak: FDs {before} -> {after}, children={children}')
            finally:
                for p in processes:
                    if p.poll() is None:
                        p.terminate()
                for p in processes:
                    try:
                        p.wait(timeout=9)
                    except subprocess.TimeoutExpired:
                        p.kill()
                        p.wait()
        report += ['', f'- Parent exit codes after SIGTERM: {[p.returncode for p in processes]}.',
                   '- All parent PIDs were waited for; listener ports are checked below.']
        for port in (front, echo, clock):
            with socket.socket() as listener:
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                listener.bind(('127.0.0.1', port))
        report += ['- All three listener ports successfully rebound after shutdown.', '',
                   '## Interpretation', '',
                   'Compare the three static rows: more simultaneous connections do not remove per-connection fork, scheduling and teardown costs. '
                   'Each proxy request also creates a backend child and an outbound TCP connection. '
                   'Blocked children retain process and socket resources until the deadline; this design does not scale like an event loop.', '']
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text('\n'.join(report))
    print(f'Report: {args.output}')
    if any(failed or non2xx for failed, non2xx in results):
        raise SystemExit('Workload failures; inspect the report.')


if __name__ == '__main__':
    main()
