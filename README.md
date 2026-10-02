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
X-Real-IP is trusted only when the direct peer is a private/loopback address.
Rate counters are bounded to 4096 keys and reset on process restart.

`fw_run` currently binds IPv4 addresses. Parallel metadata HTTP requests use at most four workers. Cookie helpers support
HttpOnly/SameSite and Secure attributes. TLS should terminate at nginx or another
reverse proxy. Keep synchronous database/outbound work within configured worker
and connection bounds. This library does not implement an async reactor.

The HTTP server uses libmicrohttpd epoll polling when supported and falls back
to poll or select. Set `FORGE_WEB_POLL` to `auto`, `epoll`, `poll` or `select` to
choose a mode; startup falls back if the selected mode cannot initialize.
Method, path, parsed route segments and resolved client IP are stored in the
request object and reused during its callback lifetime.
