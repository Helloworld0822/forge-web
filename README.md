# Forge Web

`web.fg` provides Forge APIs for HTTP requests/responses, JSON values, validation,
JWT HS256 signing/verification, outbound JSON HTTP requests, rate counters and
bounded file uploads. Validation composition and response helpers are written in
Forge. The native FFI uses libmicrohttpd, json-c, libcurl and OpenSSL.

JSON kinds: null/missing=0, boolean=1, integer=2, string=3, object=4, array=5,
double=6. Handles and returned strings belong to the current request scope.
Standalone programs call `scope_begin` / `scope_end`; HTTP callbacks get automatic
scope cleanup. Never keep a JSON handle/string beyond its scope or transfer it
to another thread. The native `fw_run` accepts a compiled Forge handler via a
small C callback adapter; see the portfolio-platform integration example.

Requires Linux/POSIX, C11, CMake/pkg-config, libmicrohttpd, json-c, libcurl,
OpenSSL and pthreads. `cmake -S . -B build && cmake --build build` builds the bridge.
Use a Forge compiler with imported extern declarations and module prototypes.

Limits: 2 MiB JSON body, 25 MiB multipart request, one 20 MiB file, 4 MiB outbound
response, 256 concurrent connections, 30-second client timeout, 10-second outbound
timeout. Uploads remain temporary until the Forge handler authorizes and calls
`upload`. Temporary files are deleted on every unsuccessful request. File serving
uses no-follow opens and rejects path traversal. The upload destination comes
from application configuration, never from client filenames.

JWT verifies the algorithm, signature in constant time, expiry and required
claims. Outbound HTTP permits only http/https and never follows redirects.
X-Real-IP is trusted only when the direct peer matches the explicit
FORGE_TRUSTED_PROXIES allowlist (empty by default).
Rate counters are bounded to 4096 keys and reset on process restart.

`fw_run` currently binds IPv4 addresses. Parallel metadata HTTP requests use at most four workers. Cookie helpers support
HttpOnly/SameSite and Secure attributes. TLS should terminate at nginx or another
reverse proxy. Keep synchronous database/outbound work within configured worker
and connection bounds. This library does not implement an async reactor.

The HTTP server defaults to `FORGE_WEB_THREADS=connection`, with a separate
thread per accepted connection for synchronous handlers. Up to 256 connections
are accepted with a 30-second inactivity timeout. Thread-local request tracking,
optional curl state and application arenas remain attached to idle keepalive
threads; the connection limit does not bound metadata-fetch helper threads.
`FORGE_WEB_THREADS=pool` selects the original shared poller pool, where the
`workers` argument controls its size and blocking handlers can delay other
connections assigned to the same poller. In connection mode `workers` does not
limit simultaneous handlers.

Set `FORGE_WEB_POLL` to `auto`, `epoll`, `poll` or `select`. Connection threads
support poll/select; epoll requests fall back to poll/select. Pool mode prefers
epoll when available. Startup logs show the selected threading and polling
modes. Inter-thread signalling ensures closed connection threads are reclaimed
even when no new connection arrives.
Method, path, parsed route segments and resolved client IP are stored in the
request object and reused during its callback lifetime.

## Input and proxy security

JSON input rejects duplicate member names within an object, including names
that become identical after escape decoding. Escaped NUL is rejected in both
keys and values because Forge/native string APIs use NUL terminators. Integers
must fit signed 64-bit; nonfinite JSON numbers and raw string control characters
are rejected. Identical names in separate nested/sibling objects remain valid.
The existing 4 MiB parser limit and depth limit remain in place.

JWT verification always uses configured HS256 and authenticates the bounded
encoded content before decoding/parsing its header and payload. It requires
canonical, unpadded base64url, rejects unsupported `crit`/`b64` headers, and
checks optional `nbf` as an integer not later than the current time. `exp` must
be an unexpired signed 64-bit integer; string/double/overflow forms are rejected.
Tokens are limited to 16 KiB and secrets to 64 KiB; signing limits serialized
claims to 12,000 bytes so emitted tokens fit the verifier limit. Authentication
failures remain a single zero result and signature comparison remains constant
time. Outbound bearer tokens must contain printable non-space ASCII and fit
16 KiB. Invalid tokens fail before HTTP is sent; raw NUL inside an outbound
JSON response cannot turn it into an accepted prefix document.

`X-Real-IP` is ignored unless the **direct peer** matches an explicit
`FORGE_TRUSTED_PROXIES` entry. The default is empty (trust no peers), including
private and loopback peers. Configure comma-separated exact IPv4/IPv6 addresses
or CIDRs, for example `127.0.0.1/32,::1/128,192.0.2.0/24`. Whitespace around
entries is allowed; at most 64 entries are accepted. Malformed addresses,
prefixes, empty entries and trailing commas fail server startup. Choose the
actual reverse proxy addresses or its isolated network, and make the proxy
overwrite client-supplied forwarded headers. Broad private networks do not
become trusted automatically. Valid forwarded addresses retain canonical
`inet_ntop` formatting; invalid addresses fall back to the direct peer.

Security regressions are in `tests/security_test.c` and the real loopback HTTP
checks in `tests/http_test.py`. Run both default-untrusted and explicitly trusted
configurations, for example:

```bash
python3 tests/http_test.py --build /tmp/forge-web-opt-build
python3 tests/http_test.py --build /tmp/forge-web-opt-build --trusted-proxies 127.0.0.1/32
```

The security benchmark in `benchmarks/security.c` records acceptance of five
previously ambiguous/unsafe inputs and the time for 20,000 forged approximately
12 KiB HS256 tokens. On the shared AMD BC-250 host, GCC 11.4 `-O2`, two-CPU Docker
quota, and five alternating rounds, median rejection time fell from **536.869 ms
to 231.694 ms (2.32×)** against `7e2684d`. All five unsafe input examples changed
from accepted to rejected. Full samples are in
`benchmarks/security-results-2026-10-02.json`. This is a native abuse-resistance
microbenchmark, not application HTTP throughput. Valid JSON/JWT now does extra
ambiguity validation; actual endpoint measurements are reported by the
portfolio integration separately.
