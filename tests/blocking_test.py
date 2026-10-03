#!/usr/bin/env python3
import argparse
import concurrent.futures
import http.client
import json
import select
import socket
import subprocess
import time
from pathlib import Path


def request(port, path):
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
    try:
        connection.request('GET', path)
        response = connection.getresponse()
        assert response.status == 200
        return json.loads(response.read())
    finally:
        connection.close()


def wait_started(process):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        ready, _, _ = select.select([process.stderr], [], [], .1)
        if ready and b'SLOW_STARTED' in process.stderr.readline():
            return
    raise AssertionError('slow handler did not start')


def check(build, image, mode, polling):
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    name = f'forge-web-blocking-test-{port}'
    process = subprocess.Popen(['docker', 'run', '--rm', '--name', name, '--network', 'host',
        '--no-healthcheck', '--entrypoint', '/build/http_server_test',
        '-e', f'FORGE_WEB_THREADS={mode}', '-e', f'FORGE_WEB_POLL={polling}',
        '-v', f'{build}:/build:ro', image, str(port), '1'], stderr=subprocess.PIPE, bufsize=0)
    try:
        for attempt in range(100):
            try:
                assert request(port, '/')['path'] == '/'
                break
            except (ConnectionRefusedError, ConnectionResetError):
                time.sleep(.05)
        else:
            raise AssertionError('server did not start')
        with concurrent.futures.ThreadPoolExecutor(2) as executor:
            slow = executor.submit(request, port, '/slow')
            wait_started(process)
            tick = time.monotonic()
            assert request(port, '/fast')['path'] == '/fast'
            elapsed = time.monotonic() - tick
            if mode == 'connection':
                assert elapsed < .5 and not slow.done(), elapsed
            else:
                assert elapsed > .7, elapsed
            assert slow.result(timeout=5)['path'] == '/slow'
        with concurrent.futures.ThreadPoolExecutor(16) as executor:
            results = list(executor.map(lambda i: request(port, f'/isolation/{i}'), range(64)))
        assert all(row['path'] == f'/isolation/{i}' for i, row in enumerate(results))
        abandoned = socket.create_connection(('127.0.0.1', port), timeout=5)
        abandoned.sendall(b'GET /slow HTTP/1.1\r\nHost: localhost\r\n\r\n')
        wait_started(process)
        abandoned.close()
        assert request(port, '/fast')['path'] == '/fast'
        print(f'{mode}/{polling}: blocking isolation, 64 requests and disconnect passed; fast={elapsed:.3f}s')
    finally:
        subprocess.run(['docker', 'stop', '-t', '5', name], check=True, stdout=subprocess.DEVNULL)
        assert process.wait(timeout=10) == 0, 'server failed during shutdown'
        process.stderr.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--image', required=True)
    args = parser.parse_args()
    build = str(args.build.resolve())
    for mode, polling in [('pool', 'auto'), ('connection', 'auto'),
                          ('connection', 'poll'), ('connection', 'select'),
                          ('connection', 'epoll')]:
        check(build, args.image, mode, polling)
    bad = subprocess.run(['docker', 'run', '--rm', '--no-healthcheck',
        '--entrypoint', '/build/http_server_test', '-v', f'{build}:/build:ro',
        '-e', 'FORGE_WEB_THREADS=invalid', args.image, '18080'],
        capture_output=True, timeout=10)
    assert bad.returncode != 0 and b'Invalid FORGE_WEB_THREADS' in bad.stderr


if __name__ == '__main__':
    main()
