# Native bridge comparison

`compare.py` compiles the unchanged `native.c` harness against a baseline source snapshot and this checkout. It uses the existing `forge-platform-release:local` Ubuntu 22.04 toolchain image (GCC 11.4, `-O2`, identical native dependency versions), a Docker CPU quota of two, five repetitions, and alternating variant order. Assertions remain enabled. No production endpoints or database are used.

```bash
mkdir -p /tmp/forge-web-baseline
GIT_MASTER=1 git archive 8ef1ce0 | tar -x -C /tmp/forge-web-baseline
python3 benchmarks/compare.py --baseline /tmp/forge-web-baseline --output /tmp/forge-web-performance.json
```

The four workloads are intentionally separate:

- Receive forty 4 MiB responses in 37-byte callback chunks (160 MiB total), validating every returned byte count.
- Create and release one million JSON number values in 500 scopes of 2,000 values.
- Perform 200,000 rate checks over 2,048 already registered keys with a nonexhausting limit and a one-hour window. This stresses the old linear lookup and new bounded hash lookup; it does not measure key churn/eviction or application requests per second.
- Perform 300 sequential HTTP requests against a local echo server with TCP_NODELAY. Record actual accepted TCP connections as well as elapsed time; TLS, external network latency, and remote rate limits are excluded.

The first three timings are inside the native harness; Docker startup is excluded. The HTTP Python server is on the host and not part of the two-CPU quota. This is a shared AMD BC-250 host with other work running, so results are local microbenchmark evidence rather than universal language or server performance claims.

On 2026-10-01, median elapsed times were:

| Workload | Baseline `8ef1ce0` | Optimized | Baseline / optimized |
| --- | ---: | ---: | ---: |
| Chunk buffering | 79.031 ms | 50.172 ms | 1.58× |
| Scoped JSON ownership | 33.809 ms | 23.455 ms | 1.44× |
| Hot rate checks | 2,509.975 ms | 7.590 ms | 330.67× |
| Sequential HTTP | 170.223 ms | 108.751 ms | 1.57× |

Accepted TCP connections for those 300 HTTP requests fell from **300 to 1**. The rate-check result is large because existing-key lookups no longer scan up to 4,096 slots; insertion into a full table still scans for the oldest eligible entry. The global rate mutex is retained to preserve the exact maximum across concurrent callers. Results are not a forecast of portfolio API throughput.

Tracking blocks cache at most 16 × 128 entries per thread (about 33 KiB). Larger scopes free surplus blocks at scope end; pthread teardown frees the cache and any unfinished scope. Curl handles are per-thread, reset between requests, and freed on thread teardown. Curl global initialization has process lifetime, avoiding cleanup while cached handles exist.

Regression checks:

```bash
cmake -S . -B /tmp/forge-web-tests -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/forge-web-tests -j2
ctest --test-dir /tmp/forge-web-tests --output-on-failure
# Run from a host with Docker and the same dependency/toolchain image:
python3 tests/http_test.py --build /tmp/forge-web-opt-build
```

`native_limits_test` covers exact outbound bounds, size multiplication overflow, concurrent limits, full-table eviction/hash unlinking, scope overflow, bounded cache reuse, and cleanup of unfinished scopes at thread exit. The loopback HTTP test covers streamed inbound bounds, embedded NUL rejection, GET/POST/PUT/DELETE transitions, authorization-header isolation, reusable connections, redirect rejection, recovery after an oversized response, and non-HTTP protocol rejection. Run CTest with AddressSanitizer and UndefinedBehaviorSanitizer as well.
