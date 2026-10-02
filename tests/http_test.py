#!/usr/bin/env python3
"""Loopback HTTP regression checks; native binaries run in an existing toolchain image."""
import argparse
import http.client
import http.server
import json
import socket
import subprocess
import threading
import time
from pathlib import Path

class Echo(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    connections = {}
    def setup(self):
        super().setup()
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    def do_GET(self):
        if self.path == '/oversized':
            body = b'x' * (4 * 1024 * 1024 + 1)
            status = 200
        elif self.path == '/nul-json':
            body, status = b'{"role":"admin"}\x00{"role":"user"}', 200
        elif self.path == '/duplicate-json':
            body, status = b'{"role":"user","role":"admin"}', 200
        elif self.path == '/redirect':
            body, status = b'{}', 302
        else:
            connection = self.connections.setdefault(self.client_address, len(self.connections) + 1)
            size = int(self.headers.get('Content-Length', '0'))
            body = json.dumps(dict(method=self.command, authorization=self.headers.get('Authorization', ''),
                                   body=self.rfile.read(size).decode(), connection=connection)).encode()
            status = 200
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        if status == 302:
            self.send_header('Location', '/echo')
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass
    do_POST = do_PUT = do_DELETE = do_GET
    def log_message(self, *args):
        pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', required=True)
    parser.add_argument('--image', default='forge-platform-release:local')
    parser.add_argument('--trusted-proxies', default='')
    parser.add_argument('--polling', default='auto', choices=['auto', 'select', 'poll', 'epoll'])
    args = parser.parse_args()
    build = str(Path(args.build).resolve())
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Echo)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    base = ['docker', 'run', '--rm', '--network', 'host', '-v', f'{build}:/build:ro', args.image]
    subprocess.run(base + ['/build/http_client_test', f'http://127.0.0.1:{server.server_port}'], check=True)
    server.shutdown()
    server.server_close()
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    name = f'forge-web-native-http-test-{port}'
    process = subprocess.Popen(base[:2] + ['--name', name, '-e', f'FORGE_WEB_POLL={args.polling}', '-e', f'FORGE_TRUSTED_PROXIES={args.trusted_proxies}'] + base[2:] + ['/build/http_server_test', str(port)])
    try:
        for attempt in range(100):
            try:
                connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
                connection.request('GET', '/')
                assert connection.getresponse().status == 200
                connection.close()
                break
            except ConnectionRefusedError:
                time.sleep(.05)
        else:
            raise AssertionError('native HTTP server did not start')
        # Reuse one connection with different per-request proxy headers; cached
        # values must never leak between requests or outlive the request data.
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
        for path, header, expected_ip, parts in [
            ('/api/posts', '2001:4860:4860::8888', '2001:4860:4860::8888', ['api', 'posts']),
            ('/api//posts/', 'invalid-ip', '127.0.0.1', ['api', '', 'posts', '']),
            ('/one/two/three/four/five/six/seven/eight/nine/ten', '8.8.8.8', '8.8.8.8',
             ['one', 'two', 'three', 'four', 'five', 'six', 'seven', 'eight', 'nine', 'ten']),
            ('/api/health', '', '127.0.0.1', ['api', 'health']),
        ]:
            headers = {'X-Real-IP': header} if header else {}
            connection.request('GET', path, headers=headers)
            response = connection.getresponse()
            result = json.loads(response.read())
            if not args.trusted_proxies:
                expected_ip = '127.0.0.1'
            assert response.status == 200 and result['ip'] == expected_ip, result
            assert result['path'] == path and result['parts'] == parts, result
            assert result['count'] == len(parts), result
        connection.close()
        for size, expected in [(0, 200), (8193, 200), (2 * 1024 * 1024, 200), (2 * 1024 * 1024 + 1, 413)]:
            connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
            body = b'x' * size
            chunks = (body[i:i + 257] for i in range(0, len(body), 257))
            connection.request('POST', '/', chunks, {'Content-Type': 'application/json'}, encode_chunked=True)
            response = connection.getresponse()
            result = json.loads(response.read())
            assert response.status == expected, (size, response.status, result)
            if expected == 200:
                assert result['bytes'] == size
            connection.close()
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
        connection.request('POST', '/', b'contains\x00nul', {'Content-Type': 'application/json'})
        response = connection.getresponse()
        response.read()
        assert response.status == 400
        connection.close()
        # Proxy configuration is checked before the daemon starts. Even when a
        # valid prefix precedes a malformed entry, startup must fail closed.
        invalid = subprocess.run(base[:2] + ['-e', 'FORGE_TRUSTED_PROXIES=127.0.0.1/32,invalid'] + base[2:] +
                                 ['/build/http_server_test', str(port)], text=True, capture_output=True, timeout=10)
        assert invalid.returncode != 0 and 'Invalid FORGE_TRUSTED_PROXIES' in invalid.stderr
    finally:
        subprocess.run(['docker', 'stop', '-t', '3', name], check=False, stdout=subprocess.DEVNULL)
        process.wait(timeout=10)
    print(f'Polling={args.polling}: cached request paths/IP, HTTP chunks, exact bounds, NUL rejection, GET/POST/PUT/DELETE isolation, connection reuse, redirects and protocols passed')

if __name__ == '__main__':
    main()
