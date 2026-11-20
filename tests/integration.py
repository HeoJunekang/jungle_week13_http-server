"""Real TCP acceptance tests. No external Python dependencies."""
import contextlib
import datetime
import os
from pathlib import Path
import signal
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import unittest

REPO = Path(__file__).resolve().parents[1]


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def exchange(port, chunks, pause=0):
    with socket.create_connection(('127.0.0.1', port), timeout=8) as sock:
        for chunk in chunks:
            sock.sendall(chunk)
            if pause:
                time.sleep(pause)
        sock.shutdown(socket.SHUT_WR)
        data = bytearray()
        while True:
            block = sock.recv(65536)
            if not block:
                break
            data.extend(block)
    head, body = bytes(data).split(b'\r\n\r\n', 1)
    lines = head.decode('latin-1').split('\r\n')
    status = int(lines[0].split()[1])
    headers = {name.lower(): value for name, value in (line.split(': ', 1) for line in lines[1:])}
    assert headers['connection'] == 'close', headers
    if chunks[0].startswith(b'HEAD '):
        assert body == b'', body
    else:
        assert int(headers['content-length']) == len(body), (headers, len(body))
    return status, headers, body


class Servers(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='c-http-test-')
        cls.base = Path(cls.temp.name)
        cls.root = cls.base / 'www'
        cls.root.mkdir()
        (cls.root / 'index.html').write_bytes(b'<h1>static test</h1>\n')
        (cls.root / 'app.css').write_bytes(b'body { color: blue; }\n')
        (cls.root / 'app.js').write_bytes(b'console.log("test");\n')
        for name in ('photo.png', 'photo.jpg'):
            (cls.root / name).write_bytes(bytes(range(256)) * 40)
        (cls.root / 'big.bin').write_bytes(bytes(range(256)) * 8192)
        (cls.base / 'secret').write_bytes(b'outside-root-secret')
        (cls.root / 'link').symlink_to(cls.base / 'secret')
        (cls.root / 'dirlink').symlink_to(cls.base, target_is_directory=True)
        os.mkfifo(cls.root / 'pipe')
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as local:
            local.bind(str(cls.root / 'socket'))
        cls.front, cls.echo, cls.clock, cls.fake = [free_port() for _ in range(4)]
        cls.config = cls.base / 'server.conf'
        cls.config.write_text(
            f'# relative to config, not cwd\nlisten 127.0.0.1 {cls.front}\n'
            f'root ./www\nroute /api/echo localhost {cls.echo}\n'
            f'route /api/time localhost {cls.clock}\nroute /fake 127.0.0.1 {cls.fake}\n')
        cls.processes = []
        cls.log = open(cls.base / 'servers.log', 'w+')
        try:
            for binary, arg, port in [('echo_backend', cls.echo, cls.echo),
                                      ('time_backend', cls.clock, cls.clock),
                                      ('webserver', cls.config, cls.front)]:
                p = subprocess.Popen([str(REPO / binary), str(arg)], cwd='/',
                                     stdout=cls.log, stderr=cls.log)
                cls.processes.append(p)
                for _ in range(150):
                    if p.poll() is not None:
                        raise AssertionError(f'{binary} exited {p.returncode}')
                    try:
                        with socket.create_connection(('127.0.0.1', port), timeout=.05):
                            break
                    except OSError:
                        time.sleep(.02)
                else:
                    raise AssertionError(f'{binary} failed to listen')
        except BaseException:
            cls.tearDownClass()
            raise

    @classmethod
    def tearDownClass(cls):
        for p in cls.processes:
            if p.poll() is None:
                p.terminate()
        for p in cls.processes:
            try:
                p.wait(timeout=9)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        cls.log.flush()
        cls.log.seek(0)
        logs = cls.log.read()
        cls.log.close()
        cls.temp.cleanup()
        if 'ERROR: AddressSanitizer' in logs or 'runtime error:' in logs:
            raise AssertionError(logs[-8000:])

    def req(self, target='/', method='GET', body=b'', extra=b'', port=None):
        head = f'{method} {target} HTTP/1.1\r\nHost: example\r\n'.encode()
        if method == 'POST' or body:
            head += f'Content-Length: {len(body)}\r\n'.encode()
        return exchange(port or self.front, [head + extra + b'\r\n' + body])

    def test_static_bytes_and_mime(self):
        for path, mime in [('/', 'text/html'), ('/index.html?q=yes', 'text/html'),
                           ('/app.css', 'text/css'), ('/app.js', 'application/javascript'),
                           ('/photo.png', 'image/png'), ('/photo.jpg', 'image/jpeg'),
                           ('/big.bin', 'application/octet-stream')]:
            with self.subTest(path=path):
                status, headers, body = self.req(path)
                self.assertEqual(status, 200)
                self.assertTrue(headers['content-type'].startswith(mime))
                name = 'index.html' if path == '/' else path[1:].split('?')[0]
                self.assertEqual(body, (self.root / name).read_bytes())

    def test_binary_and_utf8_echo(self):
        for body in ('안녕'.encode(), b'', b'abc\x00def\xff\r\n', bytes(range(256)) * 4096):
            with self.subTest(size=len(body)):
                status, _, actual = self.req('/api/echo?source=browser', 'POST', body)
                self.assertEqual(status, 200)
                self.assertEqual(actual, body)

    def test_time_is_current_utc(self):
        before = datetime.datetime.now(datetime.timezone.utc)
        status, headers, body = self.req('/api/time')
        after = datetime.datetime.now(datetime.timezone.utc)
        self.assertEqual(status, 200)
        self.assertTrue(headers['content-type'].startswith('text/plain'))
        value = datetime.datetime.strptime(body.decode(), '%Y-%m-%dT%H:%M:%SZ').replace(tzinfo=datetime.timezone.utc)
        self.assertLessEqual(before.replace(microsecond=0), value)
        self.assertLessEqual(value, after)

    def test_split_header_and_body(self):
        chunks = [b'PO', b'ST /api/echo HTTP/1.1\r\nHo', b'st: a\r\nContent-Length: 6\r\n\r', b'\n\x00ab', b'c\xffd']
        status, _, body = exchange(self.front, chunks, .01)
        self.assertEqual((status, body), (200, b'\x00abc\xffd'))

    def test_bad_request_statuses(self):
        cases = [
            (b'GET / HTTP/1.1\r\n\r\n', 400),
            (b'GET / HTTP/9.9\r\nHost: a\r\n\r\n', 400),
            (b'GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n', 400),
            (b'GET / HTTP/1.1\r\nHost: a\r\nBad Header: a\r\n\r\n', 400),
            (b'GET / HTTP/1.1\r\nHost: a\r\nX: a\x00b\r\n\r\n', 400),
            (b'GET / HTTP/1.1\nHost: a\n\n', 400),
            (b'POST /api/echo HTTP/1.1\r\nHost: a\r\n\r\n', 411),
            (b'POST /api/echo HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n', 400),
            (b'POST /api/echo HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n', 400),
            (b'POST /api/echo HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n', 400),
            (b'POST /api/echo HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n', 400),
            (b'POST /api/echo HTTP/1.1\r\nHost: a\r\nContent-Length: 1048577\r\n\r\n', 413),
            (b'POST /api/echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\n12', 400),
            (b'GET /' + b'a' * 2048 + b' HTTP/1.1\r\nHost: a\r\n\r\n', 414),
            (b'GET / HTTP/1.1\r\nHost: a\r\nX: ' + b'a' * 17000 + b'\r\n\r\n', 431),
        ]
        for raw, expected in cases:
            with self.subTest(raw=raw[:100]):
                self.assertEqual(exchange(self.front, [raw])[0], expected)

    def test_method_and_path_routing(self):
        for path, method, status, allow in [('/', 'POST', 405, 'GET'),
                ('/api/echo', 'GET', 405, 'POST'), ('/api/time', 'POST', 405, 'GET'),
                ('/', 'HEAD', 405, 'GET'), ('/', 'PUT', 405, 'GET'),
                ('/api/echo/extra', 'GET', 404, None), ('/missing', 'GET', 404, None)]:
            with self.subTest(path=path, method=method):
                actual, headers, _ = self.req(path, method)
                self.assertEqual(actual, status)
                if allow:
                    self.assertEqual(headers['allow'], allow)

    def test_head_errors_have_no_wire_body(self):
        for port, path, allow in [(self.front, '/', 'GET'), (self.front, '/api/echo', 'POST'),
                                   (self.echo, '/api/echo', 'POST')]:
            with self.subTest(port=port, path=path):
                with socket.create_connection(('127.0.0.1', port), timeout=8) as sock:
                    sock.sendall(f'HEAD {path} HTTP/1.1\r\nHost: a\r\n\r\n'.encode())
                    sock.shutdown(socket.SHUT_WR)
                    data = b''
                    while block := sock.recv(4096):
                        data += block
                head, body = data.split(b'\r\n\r\n', 1)
                self.assertTrue(head.startswith(b'HTTP/1.1 405 '))
                self.assertIn(f'Allow: {allow}'.encode(), head)
                self.assertEqual(body, b'')

    def test_proxy_accepts_frontend_header_boundaries(self):
        headers = b''.join(f'X-{i}: a\r\n'.encode() for i in range(99))
        self.assertEqual(self.req('/api/time', extra=headers)[0], 200)
        raw = b'GET /api/time HTTP/1.1\r\nHost: a\r\nX-Large: '
        raw += b'x' * (16384 - len(raw) - 4) + b'\r\n\r\n'
        self.assertEqual(len(raw), 16384)
        self.assertEqual(exchange(self.front, [raw])[0], 200)

    def test_static_traversal_symlinks_and_special_files(self):
        for path in ('/../secret', '/%2e%2e/secret', '/dirlink/secret', '/link', '/socket',
                     '/pipe', '/%00', '/%2e%2e%2fsecret', '/a/../../secret'):
            with self.subTest(path=path):
                self.assertEqual(self.req(path)[0], 403)

    def test_slow_client_does_not_block_other_connections(self):
        with socket.create_connection(('127.0.0.1', self.front)) as slow:
            slow.sendall(b'POST /api/echo HTTP/1.1\r\nHost: a\r\nContent-Length: 9\r\n\r\n1')
            start = time.monotonic()
            self.assertEqual(self.req()[0], 200)
            self.assertLess(time.monotonic() - start, 1.5)

    def test_repeated_aborts_leave_parent_serving_and_no_zombies(self):
        pid = self.processes[-1].pid
        def fd_count():
            proc = Path(f'/proc/{pid}/fd')
            if proc.is_dir():
                return len(list(proc.iterdir()))
            lsof = shutil.which('lsof') or '/usr/sbin/lsof'
            output = subprocess.check_output([lsof, '-a', '-p', str(pid), '-Ff'], text=True)
            return sum(line.startswith('f') and line[1:].isdigit() for line in output.splitlines())
        before = fd_count()
        for _ in range(50):
            with socket.create_connection(('127.0.0.1', self.front)) as sock:
                sock.sendall(b'GET /big.bin HTTP/1.1\r\nHost: a\r\n\r\n')
        self.assertEqual(self.req()[0], 200)
        time.sleep(.25)
        self.assertEqual(fd_count(), before)
        result = subprocess.run(['ps', '-axo', 'ppid=,stat='], capture_output=True, text=True, check=True)
        zombies = [line for line in result.stdout.splitlines()
                   if line.split()[0] == str(pid) and 'Z' in line.split()[1]]
        self.assertEqual(zombies, [])

    def test_echo_backend_outage_does_not_affect_time_or_static(self):
        old = self.processes[0]
        old.terminate()
        old.wait(timeout=8)
        try:
            self.assertEqual(self.req('/api/echo', 'POST', b'hello')[0], 502)
            self.assertEqual(self.req('/api/time')[0], 200)
            self.assertEqual(self.req()[0], 200)
        finally:
            p = subprocess.Popen([str(REPO / 'echo_backend'), str(self.echo)], stdout=self.log, stderr=self.log)
            self.processes[0] = p
            for _ in range(100):
                try:
                    with socket.create_connection(('127.0.0.1', self.echo), timeout=.1):
                        break
                except OSError:
                    time.sleep(.02)
            else:
                self.fail('echo backend did not restart')

    def test_expect_continue(self):
        with socket.create_connection(('127.0.0.1', self.front), timeout=8) as sock:
            sock.sendall(b'POST /api/echo HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\nExpect: 100-continue\r\n\r\n')
            self.assertEqual(sock.recv(4096), b'HTTP/1.1 100 Continue\r\n\r\n')
            sock.sendall(b'yes')
            sock.shutdown(socket.SHUT_WR)
            data = b''
            while block := sock.recv(4096):
                data += block
            self.assertTrue(data.startswith(b'HTTP/1.1 200 '))
            self.assertTrue(data.endswith(b'\r\n\r\nyes'))

    def test_http10_expect_does_not_get_interim_response(self):
        raw = b'POST /api/echo HTTP/1.0\r\nContent-Length: 3\r\nExpect: 100-continue\r\n\r\nyes'
        self.assertEqual(exchange(self.front, [raw])[0], 200)

    @contextlib.contextmanager
    def fake_backend(self, response=None, delay=0, capture=None):
        errors = []
        ready = threading.Event()
        def serve():
            try:
                with socket.socket() as listener:
                    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                    listener.bind(('127.0.0.1', self.fake))
                    listener.listen()
                    listener.settimeout(10)
                    ready.set()
                    with listener.accept()[0] as client:
                        client.settimeout(8)
                        received = b''
                        while b'\r\n\r\n' not in received:
                            received += client.recv(65536)
                        head, body = received.split(b'\r\n\r\n', 1)
                        length = next(int(line.split(b':', 1)[1]) for line in head.split(b'\r\n')[1:]
                                      if line.lower().startswith(b'content-length:'))
                        while len(body) < length:
                            block = client.recv(65536)
                            if not block:
                                break
                            body += block
                        if capture is not None:
                            capture.append((head, body))
                        time.sleep(delay)
                        if response is not None:
                            try:
                                client.sendall(response)
                            except (BrokenPipeError, ConnectionResetError):
                                pass
            except BaseException as exc:
                errors.append(exc)
                ready.set()
        worker = threading.Thread(target=serve, daemon=True)
        worker.start()
        self.assertTrue(ready.wait(3))
        try:
            yield
        finally:
            worker.join(10)
            self.assertFalse(worker.is_alive())
            if errors:
                raise errors[0]

    def test_proxy_rebuilds_headers_preserves_query_body_and_status(self):
        capture = []
        with self.fake_backend(b'HTTP/1.1 201 Created\r\nContent-Type: application/test\r\nContent-Length: 3\r\nConnection: keep-alive\r\n\r\na\x00z', capture=capture):
            status, headers, body = self.req('/fake?x=1', 'POST', b'q\x00r',
                b'Connection: X-Secret, keep-alive\r\nX-Secret: hide\r\nX-End: retain\r\n')
        self.assertEqual((status, body), (201, b'a\x00z'))
        self.assertEqual(headers['content-type'], 'application/test')
        head, body = capture[0]
        self.assertTrue(head.startswith(b'POST /fake?x=1 HTTP/1.1\r\n'))
        self.assertIn(f'Host: 127.0.0.1:{self.fake}'.encode(), head)
        self.assertIn(b'X-End: retain', head)
        self.assertNotIn(b'X-Secret:', head)
        self.assertNotIn(b'keep-alive', head)
        self.assertEqual(body, b'q\x00r')

    def test_proxy_bad_responses(self):
        for response in (b'not HTTP\r\n\r\n',
                         b'HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nx',
                         b'HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx',
                         b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n',
                         b'HTTP/1.1 200 OK\r\nContent-Length: 1048577\r\n\r\n'):
            with self.subTest(response=response):
                with self.fake_backend(response):
                    self.assertEqual(self.req('/fake')[0], 502)

    def test_proxy_bodyless_statuses(self):
        for raw in (b'HTTP/1.1 204 No Content\r\n\r\n',
                    b'HTTP/1.1 304 Not Modified\r\nContent-Length: 999\r\n\r\n'):
            with self.subTest(raw=raw):
                with self.fake_backend(raw):
                    with socket.create_connection(('127.0.0.1', self.front), timeout=8) as sock:
                        sock.sendall(b'GET /fake HTTP/1.1\r\nHost: a\r\n\r\n')
                        sock.shutdown(socket.SHUT_WR)
                        data = b''
                        while block := sock.recv(4096):
                            data += block
                head, body = data.split(b'\r\n\r\n', 1)
                self.assertEqual(head.split(b'\r\n')[0], raw.split(b'\r\n')[0])
                self.assertNotIn(b'Content-Length:', head)
                self.assertEqual(body, b'')

    def test_proxy_connect_refused_and_timeout(self):
        self.assertEqual(self.req('/fake')[0], 502)
        self.assertEqual(self.req('/api/time')[0], 200)
        with self.fake_backend(delay=5.5):
            self.assertEqual(self.req('/fake')[0], 504)


class Startup(unittest.TestCase):
    def test_executables_exist(self):
        for binary in ('webserver', 'echo_backend', 'time_backend'):
            self.assertTrue((REPO / binary).is_file(), f'missing implementation: {binary}')

    def test_invalid_configurations_have_diagnostics(self):
        cases = [('', 'listen'), ('listen 127.0.0.1 0\n', 'line 1'),
                 ('listen 127.0.0.1 65536\n', 'line 1'),
                 ('listen 127.0.0.1 8080\n', 'root'),
                 ('listen 127.0.0.1 8080\nroot /tmp\n', 'route'),
                 ('listen 127.0.0.1 8080\nroot /tmp\nroute /x localhost 1\nroute /x localhost 2\n', 'line 4'),
                 ('listen 127.0.0.1 8080\nlisten 127.0.0.1 8081\n', 'line 2'),
                 ('unknown value\n', 'line 1')]
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / 'server.conf'
            for text, diagnostic in cases:
                with self.subTest(text=text):
                    config.write_text(text)
                    p = subprocess.run([str(REPO / 'webserver'), str(config)], capture_output=True, text=True, timeout=3)
                    self.assertNotEqual(p.returncode, 0)
                    self.assertIn(diagnostic, p.stderr)

    def test_shutdown_waits_for_children_and_releases_listener(self):
        with tempfile.TemporaryDirectory() as directory:
            port = free_port()
            config = Path(directory) / 'server.conf'
            (Path(directory) / 'www').mkdir()
            config.write_text(f'listen 127.0.0.1 {port}\nroot ./www\nroute /x localhost 1\n')
            with open(os.devnull, 'w') as log:
                p = subprocess.Popen([str(REPO / 'webserver'), str(config)], stderr=log)
                try:
                    for _ in range(100):
                        try:
                            slow = socket.create_connection(('127.0.0.1', port), timeout=.1)
                            break
                        except OSError:
                            time.sleep(.02)
                    else:
                        self.fail('server did not start')
                    with slow:
                        slow.sendall(b'GET / HTTP/1.1\r\nHost: a\r\n')
                        time.sleep(.05)
                        listing = subprocess.check_output(['ps', '-axo', 'pid=,ppid='], text=True)
                        children = [row.split()[0] for row in listing.splitlines() if row.split()[1] == str(p.pid)]
                        self.assertTrue(children)
                        p.send_signal(signal.SIGINT)
                        self.assertEqual(p.wait(8), 0)
                        remaining = {row.split()[0] for row in subprocess.check_output(['ps', '-axo', 'pid='], text=True).splitlines()}
                        self.assertFalse(set(children) & remaining)
                    # Rebinding proves the listening descriptor is no longer inherited by a child.
                    with socket.socket() as listener:
                        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                        listener.bind(('127.0.0.1', port))
                finally:
                    if p.poll() is None:
                        p.kill()
                        p.wait()


if __name__ == '__main__':
    unittest.main(verbosity=2)
