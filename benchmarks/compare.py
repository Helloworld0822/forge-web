#!/usr/bin/env python3
"""Run the unchanged native harness against two immutable source snapshots."""
import argparse
import http.server
import importlib.util
import json
from pathlib import Path
import statistics
import subprocess
import threading


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline', required=True, help='Source snapshot (e.g. Git archive of 8ef1ce0)')
    parser.add_argument('--image', default='forge-platform-release:local')
    parser.add_argument('--output', default='/tmp/forge-web-performance.json')
    parser.add_argument('--rounds', type=int, default=5)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1]
    baseline = Path(args.baseline).resolve()
    build = Path('/tmp/forge-web-native-benchmark')
    build.mkdir(exist_ok=True)
    mounts = ['-v', f'{source}:/src:ro', '-v', f'{baseline}:/baseline:ro', '-v', f'{build}:/build']
    base = ['docker', 'run', '--rm', '--cpus', '2', '--network', 'host', *mounts, args.image]
    command = ('cc -O2 -Wall -Wextra -Werror -I/baseline/include '
               '-DWEB_SOURCE=\\\"/baseline/src/bridge.c\\\" /src/benchmarks/native.c '
               '$(pkg-config --cflags --libs libmicrohttpd json-c libcurl openssl) -pthread -o /build/before && '
               'cc -O2 -Wall -Wextra -Werror -I/src/include /src/benchmarks/native.c '
               '$(pkg-config --cflags --libs libmicrohttpd json-c libcurl openssl) -pthread -o /build/after')
    subprocess.run(base + ['sh', '-c', command], check=True)
    spec = importlib.util.spec_from_file_location('http_test', source / 'tests/http_test.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), module.Echo)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f'http://127.0.0.1:{server.server_port}/echo'
    samples = {'before': [], 'after': []}
    try:
        for round in range(args.rounds):
            for variant in (['before', 'after'] if round % 2 == 0 else ['after', 'before']):
                sample = json.loads(subprocess.check_output(base + [f'/build/{variant}'], text=True))
                module.Echo.connections = {}
                sample.update(json.loads(subprocess.check_output(base + [f'/build/{variant}', url], text=True)))
                sample['http_connections'] = len(module.Echo.connections)
                samples[variant].append(sample)
                print(variant, json.dumps(sample), flush=True)
    finally:
        server.shutdown()
        server.server_close()
    metrics = ['buffer_seconds', 'ownership_seconds', 'rate_seconds', 'http_seconds', 'http_connections']
    medians = {variant: {key: statistics.median(row[key] for row in samples[variant]) for key in metrics}
               for variant in samples}
    result = {'baseline': '8ef1ce0 (forge-web 0.1.1)', 'conditions': {'image': args.image, 'compiler': 'GCC 11.4 -O2',
              'cpu_quota': 2, 'host': 'shared AMD BC-250', 'rounds': args.rounds, 'order': 'alternating',
              'limits': 'synthetic native microbenchmarks and sequential loopback HTTP; not application RPS'},
              'samples': samples, 'median': medians,
              'speedup': {key: medians['before'][key] / medians['after'][key] for key in metrics}}
    Path(args.output).write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'median': medians, 'speedup': result['speedup']}, indent=2))

if __name__ == '__main__':
    main()
