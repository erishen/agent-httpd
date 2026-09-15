# AgentHTTPD

[English](README.md) | [简体中文](README.zh.md)

A lightweight HTTP/1.1 server written in C. Originally inspired by ACME Labs'
`mini_httpd`, it has since grown into an entirely independent implementation:
a prefork event loop, a CGI zoo, FastCGI (client & server), a built-in React
SSR pipeline, and a **native C LLM chat agent** (tool calls / MCP / skills /
memory / ReAct / PSE). For a deep dive into the source design (process model /
fast & slow paths / agent stack / security depth) see the
[architecture document](docs/ARCHITECTURE.md).

## Features

- **HTTP methods**: GET / HEAD / POST / PUT / PATCH / DELETE / OPTIONS — read methods
  go through the static chain; writer methods (POST/PUT/PATCH/DELETE) are only
  honored on CGI paths and pass through to scripts via `REQUEST_METHOD`
  (static URLs get 405 + `Allow` — the server never writes disk); OPTIONS is
  answered inline by the server with an `Allow` capability menu (read-only
  list for static, full list for CGI), and body-less OPTIONS rides the fast
  path; anything else gets 501
- **Static file serving**: HTML, CSS, JS, images and other common types
- **CGI**: `fork + pipe + execl`, forwards GET query & POST body, passes
  standard CGI environment variables
- **Path traversal protection**: `realpath()` validation blocks `..` escapes
- **Directory listing**: HTML listing with automatic `301` redirect when the
  trailing slash is missing
- **Custom error pages**: `www/error/NNN.html` overrides built-in defaults
- **Access log**: Combined Log Format, reopened on `SIGHUP` (logrotate-ready)
- **Process model (default: master event loop + slow-path worker pool)**: the
  fast path (static / health / 304 / 301 / 404, keep-alive multiplexing) is
  served directly by a single-process event loop (kqueue/epoll, request headers
  peeked via `recv(MSG_PEEK)` for zero-copy dispatch); blocking work (CGI, chat
  SSE, upstream proxies) is handed to 8 prefork workers over `SCM_RIGHTS`
  (`-w <n>` to resize, `0` = legacy fork-per-connection). ~6x per-conn
  throughput in benchmarks (see "Performance design")
- **MIME detection**: content type by file extension
- **Large file streaming**: static files > 64KB stream instead of buffering
  (both HTTP and FastCGI)
- **Large CGI responses**: CGI output buffer grows on demand; above a
  threshold (default 512KB) it spills to a temp file and streams — no more
  64KB truncation
- **Expect: 100-continue (RFC 9110 10.1.1)**: large POSTs no longer stall for
  1s — the server sends `100 Continue` immediately before reading the body
  (measured 1.06s → 0.04s); the 413 rejection path still returns the final
  status directly
- **Date header**: every response carries the RFC 9110 IMF-fixdate `Date:`
- **CGI variables (RFC 3875)**: scripts can rely on `GATEWAY_INTERFACE` /
  `SERVER_SOFTWARE` / `REMOTE_ADDR` (FCGI backend path takes `REMOTE_ADDR`
  forwarded by nginx)
- **Timeout guardrails**: total timeout on header/body reads (half-open
  requests are dropped — Slowloris defense); silent CGI children are killed
  with a `504`; clients disconnecting mid-CGI kill the child promptly (no
  process leaks)
- **Chunked request bodies (RFC 9110 8.7)**: POST/PUT/PATCH framed with
  `Transfer-Encoding: chunked` are decoded in place into a plain body
  (trailer handling and the `Expect: 100-continue` handshake included);
  CGI stdin / chat / the FCGI relay see an ordinary body with zero changes.
  A request naming both TE and Content-Length is rejected with 400 +
  connection close per RFC 9110 6.1 (smuggling shape); a request transfer
  coding we cannot frame (e.g. TE: gzip) answers 501
- **URL query string support**: static files and directories serve fine with
  `?query` (previously 404); `/cgi-bin` redirects and listings are compatible
- **ETag/304 conditional requests (RFC 7232)**: static resources (including
  pre-compressed `.gz` siblings) return a strong `ETag: W/"size-mtime"`;
  `If-None-Match` revalidation answers `304` (-94% transfer on repeat
  visits); per RFC 9110 15.4.5 the 304 omits entity headers
  (Content-Type/Content-Length/Accept-Ranges stay off 304s, avoiding proxy
  ambiguity); lists / `*` wildcard handled, no cross-representation false hits
- **Explicit cache policy**: every static response (200/206/304) carries
  `Cache-Control: no-cache`, CGI output and the SSR documents carry
  `no-store` (a script that declares its own directive keeps it - the
  default is a fallback, never an override). The docroot URLs are stable
  across rebuilds, so a stored copy has to be revalidated before reuse - the
  ETag turns that into a 304. Omitting the header is not neutral: with only
  `Last-Modified` present a browser may apply heuristic freshness (10% of
  the file's age) and keep serving a stale bundle for hours without ever
  asking the server. The gzip twin makes this worse than it looks, because
  its `Last-Modified` is the build-time mtime of the `.gz` file, not of the
  source it was built from
- **Cache-busting bundle URL**: `scripts/build-ssr.sh` hashes the client
  bundle and embeds the digest into both server bundles, so SSR documents
  link `/js/react-ssr.js?v=<hash>`. A cache policy only governs responses
  from the moment it ships: a copy a browser stored earlier keeps the rules
  it was stored with, and no response header can retract it. A URL that
  changes with the bytes sidesteps that entirely - the new document asks for
  a URL the client has never seen. The un-versioned path still revalidates,
  so old clients and direct fetches keep working
- **One-shot cache eviction** (opt-in, `PURGE_CLIENT_CACHE=1`): document
  responses additionally carry `Clear-Site-Data: "cache"`, which tells a
  browser to drop this origin's HTTP cache - the only way to repair clients
  whose store was poisoned before the policy above existed. Off by default;
  `docker-compose.yml` turns it on until every client has visited once, then
  it should be turned back off (while on, every visit re-evicts). Only
  documents carry it: evicting on assets too would drop the bundle once per
  page view
- **Last-Modified / If-Modified-Since (RFC 9110 13.2.2)**: all static 200s
  carry `Last-Modified`; date-only clients (some CDNs, older proxies, `curl -z`)
  get 304s too - including the case that happens every day, replaying the exact
  `Last-Modified` we just sent (the comparison keeps the time of day, so
  "equal" counts as unmodified); it yields when `If-None-Match` is present
  (spec precedence)
- **TCP_NODELAY + listen backlog 128**: Nagle disabled on every accepted
  connection (kills the tens-of-ms first-byte stall from header/body double
  sends interacting with delayed ACK); backlog raised from 10 to 128 for
  connection bursts
- **sendfile(2) zero-copy + Range/206 (RFC 9110 14)**: large static files and
  206 range responses go through kernel zero-copy (macOS/Linux, fread fallback
  elsewhere); single-range `bytes=N-M`/`N-`/`-N` (multi-range falls back to a
  full 200, gzip negotiation ignores Range), out-of-bounds gets `416` +
  `Content-Range: bytes */TOTAL`; responses carry `Accept-Ranges: bytes`
- **Graceful shutdown (draining)**: on SIGTERM workers finish in-flight
  connections first (keep-alive loops close on the shutdown flag); the parent
  drains up to 5s before SIGKILL — restarts no longer cut requests in half
- **/health endpoint**: fixed `ok` response (inside the rate-limit/auth
  gates), so load balancers and monitors don't have to guess at `GET /`
- **/metrics observability endpoint**: Prometheus text-format snapshot —
  fast/slow dispatch split, status-class counters, worker-pool saturation,
  agent concurrency slots (taken/max), upstream retries, CGI kills, uptime.
  The counter table lives in `MAP_SHARED` memory shared by master and
  workers (same pattern as the rate-limit table), increments are
  `__atomic_fetch_add` (lock-free on the fast path), and the endpoint is
  answered inline — scrapes never stall behind a hung chat
- **431 Request Header Fields Too Large**: headers beyond the 64KB buffer get
  a 431 and a close (previously a generic 400)
- **gzip pre-compressed negotiation**: when a `.gz` sibling exists, clients
  sending `Accept-Encoding: gzip` receive it (`Content-Encoding: gzip`, MIME
  from the original extension); q-values parsed per RFC 7231 (`gzip;q=0`
  rejected, explicit `*` accepted, deflate-only never mis-served); the client
  bundle is pre-compressed with `gzip -9` at build time. Since browsers always
  advertise gzip, the twin is the copy they actually execute - so the image
  takes it from the same build stage that produced the identity bundle
  (`.dockerignore` keeps a stale working-tree twin from riding in with
  `COPY www/`); `make test-container` compares the inflated twin with the
  plain bytes rather than only their lengths
- **CGI response headers (RFC 3875)**: script `Location:` becomes a 302
  redirect (local or absolute URLs, response headers pass through), `Status:`
  overrides the status line (e.g. `Status: 418 I'm a teapot`), `Content-Type`
  passes through as before
- **FastCGI backend**: optional `-F <unix-socket>` to act as an FCGI server
  (like PHP-FPM), reusing the same CGI/static dispatch chain
- **FastCGI client forwarding**: optional `-R <unix-socket>` forwards
  `/react/*` to the resident React FastCGI backend; the new unified wiring is
  `-v <port>` (next item), FastCGI stays for nginx `fastcgi_pass` setups
- **Unified `-v` render wiring (dev/prod parity)**: agent-httpd is the only
  public entry; `/@*`, `/src/*`, `/react/*` (except chat) are reverse-proxied
  to an internal upstream — Vite in dev (`scripts/dev-server.js`), or
  `bin/react-ssr-server`'s HTTP port in production (`REACT_HTTP_PORT`, with
  ISR caching); the proxy strips hop-by-hop headers per RFC 9110 7.6.1
  (Connection/TE/Keep-Alive/Proxy-*/Upgrade/Trailer) and blocks request
  smuggling (a client-injected Transfer-Encoding never reaches the upstream —
  framing authority stays with C); HMR WebSocket upgrades are tunneled at the TCP level by C;
  `/react/api/chat` is always served by C directly
- **Security headers & fingerprint parity (direct/proxied)**: C-served
  responses carry `X-Content-Type-Options: nosniff` / `X-Frame-Options: DENY`
  / `Referrer-Policy: no-referrer`; the `-v` upstreams (dev Vite middleware
  and react-ssr-server HTTP mode) emit the same set, the `Server` fingerprint
  is unified to `AgentHTTPD` and `X-Powered-By` is off; outbound error text is
  scrubbed (curl exit codes and `LLM_API_URL`-style hints go to the server
  log only)
- **React SSR in full TS + Tailwind + React Router + SSR/CSR switch**: the
  SSR source is TypeScript (tsc gate), styles are Tailwind CSS v4 inlined into
  the SSR `<head>`, client routing uses react-router v8 (SSR deep links +
  client-side navigation); every request can render in either mode via
  `?mode=csr|ssr`; all artifacts esbuild `--minify`
- **HMR dev mode (`make dev`)**: C is the only public server, Vite is demoted
  to an internal service (`127.0.0.1:PORT+2`) — client components hot-swap
  via react-refresh, server code changes need no restart, Tailwind classes
  recompile live; `/@*`, `/src/*`, `/react/*` reach Vite through C's `-v`
  proxy + WebSocket tunnel, static/CGI/chat are served by C (same code path
  as production)
- **LLM streaming chat (`/react/chat`)**: SSE chat page + `/react/api/chat`
  data endpoint; **the data endpoint is handled natively by the C process
  (`src/agent/llm.c`)**: with `LLM_API_KEY` set it forks `curl -N` against an
  OpenAI-compatible upstream (TLS is curl's problem, the server binary still
  links nothing beyond libc) and re-emits each upstream delta as SSE;
  transient upstream failures (connection refused/reset, 5xx, 429) are
  retried — 3 total attempts with 1s/2.5s backoff, emitting an
  `upstream hiccup, retrying` note over SSE; backoff sleeps in 100ms steps and
  aborts on client disconnect; retry classification lives in
  `agent_retryable()` (curl exit 7/18/35/52/55/56 and HTTP 429/5xx are
  transient; 4xx rejections and 127 are not). Without a key it falls back to
  the built-in C demo engine (word-throttled canned replies). The SSE envelope
  (note/delta/error/done) matches the node backend `chat.ts` byte for byte, so
  the page needed zero changes; config lives in the project-root `.env`
  (template in `.env.example`); on the nginx side `location =
  /react/api/chat` proxies back to httpd with `proxy_buffering off`
- **Basic Auth (-a/-r)**: RFC 7617, htpasswd-file driven, **strong hashes only**
  (`$5$`/`$6$`/bcrypt; plaintext, DES and `$1$`/`$apr1$` are skipped at load
  time, and the `AGENTHTTPD_ALLOW_WEAK_AUTH=1` dev escape hatch is the only way
  back), fail-closed (malformed lines skipped, all-invalid refuses to start),
  401s carry a WWW-Authenticate challenge, CGI gets the login via `REMOTE_USER`,
  the gate covers the whole site including `/react/` forwarding
- **Per-IP rate limiting (-l)**: token-bucket counting on a shared-memory
  table, fork/worker-pool modes enforce the same quota; IPv4 and IPv6 peers
  both key independently, and a trusted reverse proxy's `X-Forwarded-For` can
  supply the key (`RATE_LIMIT_TRUSTED_PROXIES`); the check runs before auth
  (floods never burn crypt CPU); over-limit gets 429 + Retry-After and a close
- **Dual-stack listeners**: binds IPv4 `0.0.0.0` *and* IPv6 `::` on the same
  port (`IPV6_V6ONLY` set explicitly, otherwise `::` would also swallow v4 and
  the v4 bind would fail with EADDRINUSE); both accept paths — the master event
  loop and fork-per-connection — serve either family, and a host with no usable
  v6 stack logs one notice and serves IPv4 only

## Directory layout

```
agent-httpd/
├── src/                 # layered: umbrella headers at the root, one dir per layer
│   ├── agenthttpd.h     # public framework API: embed the server (config + route/tool hooks + run)
│   ├── httpd.h          # shared structs/declarations (HTTP, FCGI server/client reuse)
│   ├── internal.h       # private cross-module declarations
│   ├── core/            # engine layer: main.c (CLI front end) · framework.c (public API impl)
│   │                    #   event.c (master loop) · worker.c (prefork pool) · router.c (llm-router sync)
│   │                    #   metrics.c (shared counters) · minijson.c · util.c
│   ├── http/            # protocol layer: http.c (parse/serialize/keep-alive) · http_parse.c
│   │                    #   http_resp.c · http_log.c · http_route.c (dispatch) · static.c (traversal
│   │                    #   defense + ETag/304 + gzip) · chatio.c (SSE envelope)
│   ├── cgi/             # gateway layer: cgi.c (CGI/1.1) · fastcgi.c (server + client relay)
│   │                    #   vite.c (dev proxy)
│   ├── security/        # auth.c (Basic Auth, hash-only) · ratelimit.c (per-IP shared-mem window)
│   └── agent/           # agent stack: llm.c (chat endpoint) · agent.c (ReAct loop) · mcp.c (MCP
│                        #   stdio client) · pse.c (Planner/Specialist/Evaluator) · tools.c (registry)
│                        #   skills.c (SKILL.md index) · session.c (memory store)
├── build/               # build artifacts (.o/.d, make clean removes) — src/ holds only sources
├── examples/
│   ├── embedded.c       # embedded demo: custom routes + exec tool + tool probe (make example-run)
│   └── tools/wordcount.py # external exec tool: JSON on stdin, JSON on stdout
├── cgi-bin/             # deploy artifacts: executables the server execl()s directly
│   ├── hello.cgi        # bash: env vars and time
│   ├── form.cgi         # bash: GET query / POST body form
│   ├── python.cgi       # Python 3: stdlib GET/POST parsing
│   ├── react-ssr.cgi    # React SSR: single-file server-rendered CGI (esbuild+tailwindcss)
│   ├── react-ssr/       # React SSR source (App.tsx / client.tsx + server/: render.tsx / cgi.tsx / main.tsx / chat.ts + styles/main.css + tsconfig.json)
│   ├── go.cgi           # Go native binary (go build)
│   ├── rust.cgi         # Rust native binary (rustc -O)
│   ├── java.cgi         # sh wrapper → java -cp Main.class
│   ├── php.cgi          # sh wrapper → php-cgi (sets REDIRECT_STATUS)
│   └── ruby.cgi         # Ruby script (ruby cgi stdlib)
├── cgi-langs/           # per-language sources (build-time only)
│   ├── go/ rust/ java/ php/ ruby/
├── scripts/
│   ├── smoke-test.sh    # smoke test script (make test)
│   ├── fcgi-test.py     # FastCGI client test (raw protocol, simulates nginx)
│   ├── build-cgis.sh    # build all language CGIs (make build-cgis)
│   └── build-ssr.sh     # build the three SSR artifacts (make build-ssr)
├── www/
│   ├── index.html       # site root page
│   ├── js/react-ssr.js  # React SSR client bundle (browser hydration)
│   ├── error/           # custom error pages (404.html / 403.html / 500.html)
│   └── test/            # directory-listing test directory
├── logs/                # access log directory
├── deploy/
│   ├── nginx.conf           # nginx site config (proxy to agent-httpd + fastcgi_pass to React backend)
│   └── docker-entrypoint.sh # container entrypoint (httpd / react-backend roles)
├── Dockerfile           # multi-stage build: C compile → SSR bundle → node:20-slim runtime
├── docker-compose.yml   # three services: httpd / react / nginx
├── .dockerignore
├── docs/
│   └── FRAMEWORK.md     # framework-ization write-up: motivation, design, process, verification
├── bin/                 # build output (server binary, libagenthttpd.a)
└── Makefile
```

### Embedding as a library

The server is also a framework: link `bin/libagenthttpd.a` (`make lib`),
register custom routes and agent tools, and run it inside your own process —
the CLI binary is just a thin front end over the same API:

```c
#include "agenthttpd.h"

agenthttpd_route("GET", "/api/status", my_status_handler);
agenthttpd_tool_exec("wordcount", "...", params_json, "python3 tools/wordcount.py");
agenthttpd_run(&(agenthttpd_config){ .port = 18101, .workers = 4 });
```

```bash
make example-run   # builds bin/libagenthttpd.a + examples/embedded and serves on :18101
```

Design decisions, the pre-fork registration model, exec-tool semantics and the
full process record live in [docs/FRAMEWORK.md](docs/FRAMEWORK.md).

## Build & run

```bash
# Build
make

# Start the server (default port 18080)
./bin/agent-httpd -p 18080

# Or use make targets (override ports with PORT=xxx)
make start      # production: run the C server in the foreground (default 8-worker pool; WORKERS= empty for fork-per-conn)
make run        # compatibility alias for make start
make dev        # dev mode: React HMR dev server (Vite middleware, port DEV_PORT=3000)
make restart    # stop then start
make stop       # graceful stop (SIGTERM)
make test       # smoke tests (static/CGI/404)
make install    # install to /usr/local (DESTDIR overridable)
make uninstall  # uninstall
```

## Docker deployment

```bash
docker compose up --build -d
#   http://localhost:18080  -> agent-httpd direct (static/CGI/health)
#   http://localhost:18081  -> nginx entry (static/CGI proxied + React FastCGI direct)
docker compose down          # stop and remove containers
docker compose logs -f httpd # follow logs
```

Architecture (three services):

| Service | Image | Role |
|---|---|---|
| `httpd` | agent-httpd | agent-httpd itself: worker pool + `-F` FastCGI listen + `-R` React relay |
| `react` | agent-httpd | resident React SSR FastCGI backend (UNIX socket, no exposed port) |
| `nginx` | nginx:1.27-alpine | front proxy: `/` proxy_pass to httpd; `/react/` fastcgi_pass to react |

The runtime image runs everything as the non-root user `agent` (uid 10001):
server, CGI/MCP child processes and the React backend. Writable locations
(`/app` — docroot + session store, the log directory, the FastCGI socket
directory and the npx cache home) are chowned at build time; everything
else stays root-owned. The compose socket volume is named `agent-sock` —
a fresh name was chosen so the volume's ownership matches the new user
(the old `react-sock` volume predates the switch and is root-owned; it can
be removed with `docker volume rm`).

> Tip: after rebuilding the httpd container, nginx may still cache the old
> upstream IP (502) — `docker compose restart nginx` fixes it; alternatively
> `docker compose down` then `up`.

The same SSR page has two comparable paths:

- `:18080/react/*` — agent-httpd as the FCGI **client** (`-R` relay to the
  resident backend)
- `:18081/react/*` — nginx as the FCGI **client** (`fastcgi_pass` direct; the
  socket needs `REACT_FCGI_SOCK_MODE=0777` to loosen permissions, see below)

Notes:

- The image is a multi-stage build (C compile → native CGI compile → SSR
  bundling → node:20-slim runtime, ~620MB); build flags match local `make`
  exactly (`-Wall -Wextra -Werror -O2`)
- The CGI zoo works fully inside the container: bash/python/ruby/php
  interpreters are built in; go/rust/java are **recompiled inside the
  container** by the `cgi-build` stage with Linux toolchains (host Mach-O
  artifacts are excluded by `.dockerignore`)
- Environment variables: `WORKERS` (worker count, 0 = fork-per-connection),
  `RATE_LIMIT` (per-IP req/s), `LOG_FILE` (default
  `/var/log/agent-httpd/access.log`); the C chat endpoint also reads
  `LLM_API_URL` / `LLM_API_KEY` / `LLM_MODEL` / `LLM_TIMEOUT` (unset = the
  built-in demo engine, no key needed). Agent capabilities:
  `AGENT_MAX_ROUNDS` / `AGENT_MAX_CONCURRENT` (ReAct rounds/concurrency
  slots), `AGENT_UPSTREAM_ATTEMPTS` / `AGENT_BACKOFF_MS_1` /
  `AGENT_BACKOFF_MS_2` (upstream retry count & backoff, compile-time
  constants in `src/agent/agent.h`), `PSE_ENABLED` / `PSE_SOULS_DIR` (PSE
  orchestrator/souls dir), `MCP_SERVERS` (MCP stdio server config),
  `HARNESS_SKILLS_DIR` / `SKILLS_EXTRA_DIRS` (skills dirs); the React backend
  reads `REACT_HTTP_PORT` / `REACT_RENDER_TTL_MS` / `REACT_FCGI_SOCK_MODE`
  (socket permissions, default 0700); the llm-router catalog sync reads
  `ROUTER_API_URL` (defaults to `LLM_API_URL`) / `ROUTER_SYNC_BUDGET_SECONDS`
  (wall-clock budget for the whole sync, default 10s, 0 = unlimited — a
  half-dead upstream tightens each curl's `--max-time` to the remaining
  budget and skips the rest past the wall: a stale catalog is acceptable,
  a stalled boot is not); in-container log rotation reads
  `LOG_ROTATE_SECONDS` (default 86400, 0 = off) / `LOG_ROTATE_KEEP` (default 7)
- **LLM config lives in the project-root `.env`**: `cp .env.example .env`,
  fill in `LLM_API_KEY`, and you're on a real model (any OpenAI-compatible
  endpoint: OpenAI / DeepSeek / local Ollama); both the httpd and react
  compose services get it via `env_file` (missing file is fine), `make dev`
  auto-loads it with `--env-file-if-exists=.env`; the C server also reads
  `.env`'s `LLM_*` from the working directory on the first chat request
  (pre-existing env vars win — an empty `LLM_API_KEY` forces the demo engine).
  Re-run `docker compose up -d` to apply changes. `.env` is excluded by
  `.gitignore`/`.dockerignore` — keys never enter the repo or the image
- `docker stop`'s SIGTERM goes straight to agent-httpd's graceful drain (stop
  accept → workers drain up to 5s → SIGKILL fallback) — restarts don't drop
  in-flight requests
- **FCGI socket permissions tightened by default**: the React backend's UNIX
  socket defaults to `0700` (owner-only) — a direct socket connection
  bypasses httpd's auth/rate-limit, so it must not be writable by every local
  process; when an nginx worker runs under a different account, set
  `REACT_FCGI_SOCK_MODE=0777` on the react service to loosen it

## Testing

> Every probe targets `http://localhost:<port>`. If your shell exports
> `http_proxy`/`https_proxy`, those requests get answered by the proxy instead
> of the server and dozens of checks fail with 502s that look like server
> bugs; behind a proxy run
> `env -u http_proxy -u https_proxy -u ALL_PROXY make test`.

```bash
make test   # smoke tests: static/HEAD/POST/traversal/redirects/listings/error pages/query strings
            #   + large response streaming / CGI timeout 504 / half-open request drop / XSS escaping
            #   + CGI Location/Status headers / Accept-Encoding q-values / keep-alive reuse
            #   + every language CGI + FastCGI + the HMR dev server
            #   + the C chat endpoint SSE (demo engine / error paths / -R relay bypass)
            #   (HMR cases SKIP automatically when vite isn't installed; chat cases pin
            #    an empty LLM_API_KEY over the project-root .env for deterministic offline runs)
make bench  # throughput comparison: keep-alive vs per-connection (default 5000 req / 16 conc,
            #   override with BENCH_REQ/BENCH_CONC/PORT; with -l enabled, 429s land in the
            #   429s column instead of failing; latency percentiles count 200s only)
make test-unit      # standalone regressions, no server needed: dev-proxy header builder,
                    #   Basic Auth (b64/credential parsing, hash-only htpasswd policy),
                    #   minijson (sbuf/escapes/tolerant reader) under ASan+UBSan, and the
                    #   FastCGI stream relay (spawns a throwaway backend)
make test-keepalive # keep-alive pipelining on one connection (starts the real server in
                    #   fork-per-connection mode on a free port; slower, needs loopback)
make test-linux     # Linux/GCC build guard: builds the Dockerfile's c-build stage. glibc is the
                    #   only place the -Wstringop-truncation / -Wformat-truncation /
                    #   -Wuse-after-free family and the -lm/-lcrypt link flags surface —
                    #   Apple clang stays silent on all of them, so the host build alone
                    #   cannot see a broken container build
make test-container # probe the running container (start it with `docker compose up -d`
                    #   first): exercises the shipped binary + docroot + CGI zoo over the
                    #   wire — routing/methods/traversal/smuggling/keep-alive/gzip/Range/
                    #   304/CGI Location+Status headers/large-response streaming
make test-upstream  # upstream-failure path: chat must answer an unreachable model with
                    #   an error event (and retry), never a silent empty reply. Spins a
                    #   throwaway container on --network none; needs only the image
make test-stream    # large-response streaming: a 300 KB body must arrive whole when the
                    #   reader pauses. Reads it from a *second* container over a private
                    #   bridge — the only path where the socket buffer actually fills, so
                    #   it is the only one that catches a body truncated mid-stream
                    #   (nginx: "upstream prematurely closed connection"; browser: the
                    #   JS asset never parses, so the React page never hydrates)
```

Or open these in a browser:

- `http://localhost:18080/`                    - home page
- `http://localhost:18080/cgi-bin/hello.cgi`   - simple bash CGI
- `http://localhost:18080/cgi-bin/form.cgi`    - POST form (or `?name=test` GET)
- `http://localhost:18080/cgi-bin/python.cgi?name=Alice&message=Hi` - Python example
- `http://localhost:18080/cgi-bin/react-ssr.cgi?name=Alice&message=Hi` - React SSR example
- `http://localhost:18080/cgi-bin/go.cgi?name=Alice&message=Hi` - Go native binary
- `http://localhost:18080/cgi-bin/rust.cgi?name=Alice&message=Hi` - Rust native binary
- `http://localhost:18080/cgi-bin/java.cgi?name=Alice&message=Hi` - Java (JVM wrapper)
- `http://localhost:18080/cgi-bin/php.cgi?name=Alice&message=Hi` - PHP (php-cgi)
- `http://localhost:18080/cgi-bin/ruby.cgi?name=Alice&message=Hi` - Ruby (cgi stdlib)
- `http://localhost:18080/test/`               - directory listing
- `http://localhost:18080/test`                - 301 → `/test/`
- `http://localhost:18080/nothing`             - custom 404 error page

### The multi-language CGI zoo

Every language example goes through the same CGI chain: the server `fork()`,
sets environment variables, wires stdin/stdout pipes, then `execl()`s the
target. The only per-language difference is "how it gets exec'd":

| Language | cgi-bin artifact | How it runs | Notes |
|------|-------------|---------|------|
| Go | `go.cgi` | native binary | `go build`, microsecond cold start |
| Rust | `rust.cgi` | native binary | `rustc -O` |
| Java | `java.cgi` (sh) | `java -cp ... Main.class` | fresh JVM per request (slow); GraalVM native images recommended for production |
| PHP | `php.cgi` (sh) | `php-cgi` | homebrew php-cgi needs `REDIRECT_STATUS=200` |
| Ruby | `ruby.cgi` | script | stdlib `cgi` parses GET/POST |
| Python | `python.cgi` | script | hand-rolled parsing (3.13+ removed the `cgi` module) |
| React SSR | `react-ssr.cgi` + `www/js/react-ssr.js` | node SSR + browser hydration | server/client dual bundle, esbuild |

Rebuild all language CGIs (missing tools auto-skip):

```bash
make build-cgis
```

### Python CGI

`cgi-bin/python.cgi` is a stdlib-only Python CGI example; runtime only needs
`python3` on PATH. Since Python 3.13+ removed the `cgi` module, the script
parses by hand: `urllib.parse` for GET query and urlencoded bodies,
`email.parser` for multipart forms. GET (query string) / POST (urlencoded
and multipart) both work; HTML output is escaped with `html.escape` against
XSS.

### React SSR (SSR + hydration)

A modern React **server-side rendering + browser hydration** setup:

- **SSR entry** `cgi-bin/react-ssr/server/cgi.tsx` → bundled to
  `react-ssr.cgi`, executed by the C server over CGI: renders the shared
  `App.tsx` with `react-dom/server`'s `renderToString`, emits the server HTML
  inside `<div id="root">`, inlines a `window.__SSR_DATA__` blob (params +
  server time) and references the client bundle.
- **Client entry** `cgi-bin/react-ssr/client.tsx` → bundled to
  `www/js/react-ssr.js`, downloaded by the browser, takes over the server
  HTML with `react-dom/client`'s `hydrateRoot`; afterwards the component is a
  normal interactive React app (live clock via `useEffect`, form echo, etc.).
- **Consistency**: the client's first render replays the exact same data as
  SSR (JSON replay), so `hydrateRoot` never reports mismatches; volatile
  values like time are packed as `serverTime` server-side and refreshed in
  the browser after hydration.
- Hooks in use: `useState` / `useMemo` / `useId` / `useEffect`.
- **React Router v8**: `App.tsx` is the shared route tree; only the router
  wrapper differs — `<StaticRouter location={pathname}>` on the server
  (server/render.tsx derives it from `REQUEST_URI`, so `/react/about` deep
  links render out of the box), `<BrowserRouter>` + `hydrateRoot` in the
  browser (`<Link>` navigates without reloads). Route table:
  `/react` (Home) / `/react/about` / `/react/counter` (useState counter demo)
  / `/react/*` (NotFound); other paths (e.g. `/cgi-bin/react-ssr.cgi`)
  fall back to Home.
- **SSR/CSR dual mode**: with `?mode=csr` the server skips `renderToString`
  and returns an empty `<div id="root">` shell + inline CSS + `__SSR_DATA__`;
  the client bundle reads `data.mode` and mounts fresh via `createRoot` (pure
  CSR, zero server render cost). Default (no/invalid `mode`) is SSR. Both
  CGI/resident entries and GET/POST semantics are identical; the page footer
  has a "switch to CSR / SSR" toggle (full `<a>` navigation so the backend
  decides again).

Both bundles are esbuild artifacts with react/react-dom bundled in,
self-contained per runtime (node process / browser).

- The shared render core `server/render.tsx` (parseQuery / safeJson /
  renderPage) is referenced by both entries, guaranteeing byte-identical
  markup from the CGI and the resident backend; `renderPage` inlines
  `__SSR_DATA__` and references `/js/react-ssr.js`.
- **Styling is Tailwind CSS v4**: classes live in `App.tsx`; `build-ssr.sh`
  first compiles `styles/main.css` (`@source` scans only the react-ssr
  directory) into `tailwind.css` with `@tailwindcss/cli`, then esbuild inlines
  it via `--loader:.css=text` — the SSR `<head>` stays fully self-contained
  (no extra stylesheet request). Design tokens (e.g. `bg-surface`) live in
  `styles/main.css`'s `@theme`.

Build flow (only needed after changing source):

```bash
cd cgi-bin/react-ssr && npm install   # one-time build deps (react, esbuild, tailwindcss, typescript, @types/*)
make build-ssr                        # tsc --noEmit type gate, then Tailwind, then artifacts:
                                      # server/cgi.tsx  → cgi-bin/react-ssr.cgi
                                      # server/main.tsx → bin/react-ssr-server (resident)
                                      # client.tsx      → www/js/react-ssr.js
make typecheck                        # type check alone (esbuild transpiles but doesn't check)
```

TS constraints and classic pitfalls:

- **esbuild doesn't type-check**, so `build-ssr.sh` runs `tsc --noEmit` first
  (a failure aborts the bundling).
- Signatures like `parseQuery(query: string)` catch type errors at compile
  time: the old bug of passing a `Buffer` to `parseQuery` (POST 500) is now a
  build error (the resident backend switched to `body.toString("utf8")`).
- **The client bundle must never reference `process`**: esbuild only replaces
  `process.env.NODE_ENV`; other `process.*` stays in the browser bundle and
  throws at hydration. So `nodeVersion` is computed server-side and packed
  into `__SSR_DATA__` (same policy as `serverTime`); the client only reads
  the blob, keeping both sides' markup identical.

Sources live in `cgi-bin/react-ssr/` (shared App.tsx + client entry client.tsx
at the root, server code in the `server/` subdirectory: render core render.tsx
+ the two server entries cgi.tsx (CGI) / main.tsx (resident FastCGI) + the chat
backend chat.ts). GET (query string) and POST (body) are both supported,
receiving parameters via the standard CGI variables `REQUEST_METHOD` /
`QUERY_STRING` / `CONTENT_LENGTH`. The full render chain is validated through
`renderToString`; `make test` covers GET/POST and all three FastCGI entries.

## Dev mode & HMR

The production chain requires `make build-ssr` per change (tsc + tailwind +
esbuild). Dev uses Vite's on-demand transform against the same `render.tsx`
core instead — changes apply instantly:

```bash
cd cgi-bin/react-ssr && npm install   # one-time (vite/@vitejs/plugin-react are devDependencies)
make dev                              # http://localhost:3100/react/?name=Alice
DEV_PORT=3100 make dev                # different port
```

How it works (`scripts/dev-server.js`, unified `-v` wiring):

- **The C process is the only public entry**: dev-server starts
  `./bin/agent-httpd -v DEV_PORT+2` on `DEV_PORT`; that's the dev server's
  entire HTTP face — static, CGI, `/health`, `/react/api/chat` and error pages
  are all served by C (the exact production code path).
- **Vite is demoted to an internal service**: dev-server binds Vite to
  `127.0.0.1:DEV_PORT+2` (not public) for `transformRequest`, dev SSR of
  `/react/*`, Tailwind compilation and the HMR WebSocket. C reverse-proxies
  `/@*`, `/src/*`, `/react/*` (except chat) to it; WebSocket upgrades are
  TCP-tunneled by C.
- **Client hot swap**: the SSR HTML's `<script src="/js/react-ssr.js">`
  (esbuild artifact) is rewritten to Vite-transformed `/react/react-ssr.tsx`
  with a react-refresh preamble injected into `<head>` — editing
  `client.tsx`/page components swaps them live via Fast Refresh.
- **Server code without restarts**: `render.tsx` / `App.tsx` / `pages/*` load
  via `ssrLoadModule`; on file change the module graph is invalidated and a
  full-reload broadcast — the next request renders with new code.
- **Live Tailwind**: saving `styles/main.css` or touching class-bearing tsx
  recompiles `tailwind.css` (debounced), then triggers a full page reload.

Note: dev and production wiring are identical — the browser only talks to C,
C hands rendering to an internal upstream (Vite in dev, react-ssr-server's
HTTP port in production); see "React render wiring". HMR and proxy behavior
are covered by `make test` guard cases (SKIP when vite isn't installed).

## Command-line flags

| Flag | Description |
|------|------|
| `-p <port>` | HTTP listen port (default 18080) |
| `-F <sock>` | additionally listen on a UNIX socket as a FastCGI backend (like PHP-FPM) |
| `-R <sock>` | (optional, legacy) forward `/react/*` over FCGI to the resident React backend; prefer the unified `-v` wiring |
| `-v <port>` | unified wiring: `/@*`, `/src/*`, `/react/*` (except chat) are HTTP-proxied to Vite / react-ssr-server on `127.0.0.1:<port>`; HMR WebSocket upgrades are transparently tunneled (see "React render wiring") |
| `-T <seconds>` | set both request and CGI timeouts at once (default 30s) |
| `-a <htpasswd>` | enable Basic Auth against the given htpasswd file (see below) |
| `-r <realm>` | Basic Auth realm (default `agent-httpd`, needs `-a`) |
| `-w <n>` | slow-path worker pool size (default 8; `0` = fork-per-connection). The fast path is served by the master event loop; the pool only takes CGI / chat / proxy / body requests (see "Performance design") |
| `-l <rps>` | per-IP rate limit: max requests per second (0 = off, default; over-limit gets 429 + Retry-After) |
| `-L <path>` | access log path (default `./logs/access.log`; unwritable logs warn but keep running) |
| `-h` | show help |

### Timeouts & guardrails

All guardrails are environment-overridable (the smoke tests use exactly that
to shrink timeouts to 3s); `-T` sets both at once:

| Env var | Default | Effect |
|----------|------|------|
| `REQUEST_TIMEOUT_SECONDS` | 30 | total budget for reading request headers + POST body; exceeding drops the connection (half-open clients no longer pin a forked handler forever) |
| `CGI_TIMEOUT_SECONDS` | 30 | silent CGI children are killed (SIGKILL) and answered with `504 Gateway Timeout`; writing the POST body to CGI is polled too — a script that never reads stdin can't wedge the server |
| `CGI_BODY_TMP_THRESHOLD` | 524288 (512KB) | CGI response bodies above this spill to a `/tmp` temp file and stream; smaller ones grow in memory |

Additionally, the client socket is polled while a CGI runs: a client that
disconnects early terminates the CGI immediately and abandons the send —
"slow CGI + early disconnect" no longer leaks a process each time.

### Per-IP rate limiting (-l)

```sh
./bin/agent-httpd -p 18080 -l 20   # at most 20 req/s per IP
```

- **Token bucket, shared table**: the table lives in `MAP_SHARED` anonymous
  shared memory (each IP hashes into 4096 buckets via a splitmix mix, bucket
  mutexes explicitly `PTHREAD_PROCESS_SHARED`), so **fork-per-connection and
  worker-pool modes enforce the same quota** — pooled processes can't each
  count separately. Tokens refill at `-l` per second, so there's no 2x burst
  at a window boundary.
- **The check runs before Basic Auth**: floods never reach the dispatch layer
  nor burn crypt() CPU — rate limiting is exactly the right shield against
  Basic Auth brute force.
- **Over-limit response**: `429 Too Many Requests` + `Retry-After: 1`, with a
  forced `Connection: close` (queued same-source requests can't sneak past the
  counter).
- **Hash collisions probe, never steal**: a colliding IP probes up to 8
  following slots and only consumes a bucket it already owns, so a later
  arrival can't wipe another IP's live counter (the old "bucket stealing" was
  a rate-limit bypass). Only when every probe slot is taken does it degrade to
  sharing the base slot — still without overwriting anyone's accounting.
- **Behind a reverse proxy**: the key is the direct peer's address, which
  collapses every real client onto the proxy. Set
  `RATE_LIMIT_TRUSTED_PROXIES` to a comma-separated list of proxy IPs/CIDRs
  and the leftmost `X-Forwarded-For` hop becomes the key instead — but only
  when the peer is trusted, so an untrusted client can't spoof its bucket.
  Entries are IPv4 **or** IPv6 (`127.0.0.1,10.0.0.0/8,::1,2001:db8::/32`),
  optionally bracketed for v6 (`[::1]`); a malformed entry (bad address, or a
  non-numeric / out-of-range prefix) is dropped rather than widened to
  "trust everyone". IPv6 XFF hops are matched the same way IPv6 peers are
  (hashed to the 32-bit key), so both families limit independently.
- The env var `RATE_LIMIT_RPS` overrides `-l`.
- The env var `RATE_LIMIT_TRUSTED_PROXIES` enables the X-Forwarded-For
  handling above (unset = trust nobody).

## Basic Auth

`-a` enables HTTP Basic authentication (RFC 7617); requests without
credentials get a uniform `401` + `WWW-Authenticate: Basic realm="...",
charset="UTF-8"` challenge, site-wide (static, CGI, and `/react/` forwarding
are all behind the gate):

```bash
# htpasswd file: one "user:secret" per line, strong hashes only.
# Generate with `openssl passwd -6` (SHA-512) or `htpasswd -B` (bcrypt).
printf 'alice:%s\n' "$(openssl passwd -6 'password123')" > /tmp/htpasswd
./bin/agent-httpd -p 18080 -a /tmp/htpasswd -r "Private"

curl -u alice:password123 http://localhost:18080/
```

- **Strong hashes only**: `$5$` (SHA-256), `$6$` (SHA-512) and bcrypt
  (`$2a$`/`$2b$`/`$2y$`) are accepted. Plaintext secrets, 13-character DES
  and `$1$`/`$apr1$` MD5 hashes are rejected at load time - the entry is
  skipped with a warning, so it can never match.
- **Dev escape hatch**: exporting `AGENTHTTPD_ALLOW_WEAK_AUTH=1` restores the
  tolerant behavior (plaintext/DES accepted with a loud startup warning).
  Local development only - never in production.
- **Platform differences**: Linux links `-lcrypt` (glibc/libxcrypt supports
  `$5$`/`$6$`); macOS's `crypt(3)` lives in libc and is DES-only, so strong
  hashes fail validation locally (fail-closed rejection, never a false
  pass). On macOS use the escape hatch above, or exercise auth in the
  container.
- **Fail-closed design**: malformed htpasswd lines are skipped with a warning;
  if none are valid, startup fails — misconfiguration always errs toward
  "deny", not "allow"; missing credentials, malformed input, and unknown users
  all get 401.
- **CGI integration**: after a successful login, CGI programs read the
  username from the standard `REMOTE_USER` variable.
- 401 responses force `Connection: close`, avoiding ambiguity from pipelined
  leftovers after the challenge.

## FastCGI backend mode

Besides being a plain HTTP server, agent-httpd can run as a **FastCGI
server**: it listens on a UNIX socket and accepts requests forwarded by
nginx & co. via `fastcgi_pass`. FCGI frames (`BEGIN_REQUEST` / `PARAMS` /
`STDIN`) are rebuilt into internal `HttpRequest`s and then go through the
exact same dispatch chain as HTTP (static files / CGI); responses return via
`FCGI_STDOUT` records + `FCGI_END_REQUEST`.

```bash
# Listen on both HTTP 18080 and an FCGI unix socket
./bin/agent-httpd -p 18080 -F /tmp/mini-fcgi.sock
```

Example nginx config:

```nginx
location / {
    # fastcgi_pass points at agent-httpd's FCGI socket (the front end must resolve CGI scripts)
    fastcgi_pass unix:/tmp/mini-fcgi.sock;
    fastcgi_param REQUEST_METHOD  $request_method;
    fastcgi_param REQUEST_URI     $request_uri;
    fastcgi_param QUERY_STRING    $query_string;
    fastcgi_param CONTENT_TYPE    $content_type;
    fastcgi_param CONTENT_LENGTH  $content_length;
    fastcgi_param SCRIPT_FILENAME $document_root$fastcgi_script_name;
    include fastcgi_params;
}
```

`make test` uses `scripts/fcgi-test.py` to exercise the raw FCGI protocol
(simulating nginx) across static pages, 404s, GET/POST CGI, and React SSR.

## React render wiring (unified `-v`)

CGI means one `fork + exec node` per request — React's cold start (loading
react, evaluating the bundle) costs tens of milliseconds and is unreusable.
This project removes the cold start with a **resident render process**, the
same architecture as PHP-FPM / puma / unicorn, and **dev and production
share the same `-v` wiring**:

```
                  Browser (talks only to C)
                   │   GET /react/?name=Alice
                   ▼
              agent-httpd :PORT   (the only public entry)
                   │   static/CGI/health/chat/error pages = served by C
                   │   /@*, /src/*, /react/* = HTTP reverse proxy (-v)
                   │   HMR WebSocket = TCP tunnel
                   ▼
   127.0.0.1:PORT+2 ◄── dev:  Vite (scripts/dev-server.js)
                   ◄── prod: bin/react-ssr-server (REACT_HTTP_PORT=PORT+2)
                           renders renderStream → HTTP 200, streaming/cached
```

- **Production**: `make react-server` first starts `bin/react-ssr-server`
  (loading React into memory once, with ISR render caching), then
  `agent-httpd -p PORT -v PORT+2` — C proxies `/react/*` pages to it, exactly
  like dev points at Vite.
- **FastCGI is the optional legacy wiring**: `bin/react-ssr-server` still
  does native FCGI listening when `REACT_HTTP_PORT` is unset (`-R` relay) for
  nginx `fastcgi_pass` or direct-socket scenarios; `-v` is the recommended
  unified wiring.
- The backend reuses the shared `render.tsx` core, so its output is
  byte-identical to the CGI render (including `window.__SSR_DATA__` and
  `<script src="/js/react-ssr.js">`); the client bundle is the same shared
  `www/js/react-ssr.js`.
- `/react/api/chat` is handled inside the C process (next section) and is
  **never** proxied to the React backend.

```bash
make react-server                      # one-shot: react-ssr-server (HTTP) + agent-httpd -v
# Browser: http://localhost:18080/react/?name=Alice
curl 'http://localhost:18080/react/?name=Alice'   # via C → -v → react-ssr-server
# 502 when the backend is stopped
pkill -f react-ssr-server && curl -i http://localhost:18080/react/ | head -1
```

## The native C LLM chat endpoint (`src/agent/llm.c`)

The `/react/api/chat` data endpoint is handled inside the C process, no
longer routed through the React FastCGI backend:

```
Browser POST /react/api/chat {message, history?}
      │
      ▼
handle_client: rate-limit/auth gates → llm_is_chat_route intercepts (before the -R relay)
      │
      ├─ LLM_API_KEY configured ──► fork curl -N --max-time <LLM_TIMEOUT>
      │        POST <LLM_API_URL>/chat/completions  (stream:true)
      │        │  upstream SSE parsed line by line (data: {...} / [DONE] / error events)
      │        ▼
      │   re-emitted as this project's envelope: {"t":"delta"|"note"|"error"|"done", ...}
      │
      └─ no key ──► built-in C demo engine: canned reply, word-throttled (28ms/step)
      │
      ▼
SSE streamed back to the client (head+body emitted by the handler,
response->handled; handle_client just logs and closes — SSE is
close-delimited, no keep-alive)
```

Key points:

- **Zero new dependencies**: TLS belongs to the forked `curl`(1); the server
  binary still links nothing beyond libc — consistent with the project's
  "no OpenSSL" stance. The model even mirrors CGI's fork+pipe shape (request
  body over stdin, upstream stream on stdout, polled total timeout, kill on
  client disconnect)
- **Hand-rolled fault-tolerant JSON**: the request body `{message, history?}`
  and upstream deltas are parsed by a ~200-line tolerant reader
  (`\uXXXX`/surrogate pairs/nested skipping), no libraries
- **`.env` compatibility**: on the first chat request the server reads
  `LLM_*` from the working directory's `.env` (`setenv(..., overwrite=0)`;
  existing env vars win), sharing one config across compose `env_file` /
  `make dev --env-file-if-exists`
- **In a container, `LLM_API_URL` gets a different host**: the value in `.env`
  holds for the host only — inside a container `localhost` is the container.
  The shipped `docker-compose.yml` therefore overrides it for the `httpd` and
  `react` services (`x-llm-api-url`, defaulting to `host.docker.internal`),
  leaving `.env` and host `make dev` untouched
- **An unreachable upstream is reported, not swallowed**: a failed round emits
  an `error` event and retries with backoff, so a dead router shows up in the
  chat instead of an empty answer (`make test-upstream` guards this)
- **Both upstream URL styles accepted**: a full endpoint, or the
  OpenAI-SDK-style base URL (`https://host/v1` gets `/chat/completions`
  appended)
- The node-side `chat.ts` remains the HMR dev-mode (`make dev`)
  implementation; both sides' SSE envelopes are byte-identical by contract,
  the chat page can't tell them apart
- The nginx entry (:18081) uses `location = /react/api/chat` to proxy exactly
  that route back to httpd (`proxy_buffering off`); other `/react/*` paths
  still fastcgi_pass to the resident backend
- Known edges: HEAD on `/react/api/chat` doesn't hit the C endpoint (it falls
  back to the `-R` relay/static chain and returns a page); chat over the
  `-F` FCGI-server path (nginx `fastcgi_pass` straight into agent-httpd)
  doesn't go through here either — in the compose topology nginx proxies
  that route back to httpd, so neither edge reaches users

## The native C agent stack (Tool Call / MCP / Skills / Memory / ReAct / PSE)

With `LLM_API_KEY` set, `src/agent/llm.c` hands the request to a full native agent
stack instead of a one-way relay. The request body grows from
`{message, history?}` to `{message, history?, sessionId?}`:

```
Browser POST /react/api/chat {message, history?, sessionId?}
      │
      ▼ handle_client → llm_handle_chat (llm.c)
      │
      ├─ sessionId present ──► load .data/sessions/<id>.json
      │     · previous user/assistant turns replay as history (survives page refresh)
      │     · session facts + skills index injected into system_extra
      │     · this round's deltas captured in full → transcript + facts, atomic flush
      │
      ├─ PSE_ENABLED=true ──► pse_run (src/agent/pse.c)
      │     Planner (plan without tools) → Specialist (ReAct loop with all tools)
      │     → Evaluator (PASS/PARTIAL/FAIL; non-PASS retries with feedback ≤3 rounds)
      │     · persona prompts: $PSE_SOULS_DIR/{planner,specialist,evaluator}/SOUL.md
      │       (default <cwd>/souls, then built-in fallbacks)
      │     · the whole run holds one concurrency slot (agent_slot_take)
      │
      └─ default ──► agent_run → ReAct main loop (src/agent/agent.c)
            loop: assistant tool_calls → tools_dispatch → results fed back
            → until the model stops calling tools (AGENT_MAX_ROUNDS cap;
              each round forks its own curl upstream; concurrency bounded by
              the AGENT_MAX_CONCURRENT pipe slots)
            · tool table = 7 built-ins + MCP tools, OpenAI tools schema synthesized
      │
      ▼
SSE written back (note/delta/error/done envelope unchanged)
```

**Built-in tools** (`src/agent/tools.c`, registered by `tools_init()`): `get_time`
(Beijing time, UTC+8 — the image ships no tzdata so the offset is applied
explicitly), `calc` (recursive-descent arithmetic parser), `read_file`
(resolved inside the web root + traversal protection), `fetch_url` (grabs up to 16KB over http(s); SSRF-hardened — see the
security checklist below), `skill-run` (reads a skill's full text),
`remember` / `recall` (session memory facts; without a sessionId they land
in the global pool).

**Skills** (`src/agent/skills.c`): scans directories for `SKILL.md` (frontmatter
`name` / `description`), injects the index into every request's system
prompt; `skill-run` reads a named skill's full text for the model.
Directories: `HARNESS_SKILLS_DIR` → `./skills` → `resolve-skills/skills` →
`SKILLS_EXTRA_DIRS` (comma-separated).

**Memory** (`src/agent/session.c`): one JSON file per session,
`.data/sessions/<id>.json` (`{messages[], facts{}}`), written atomically via
tmp+rename; transcript replay builds the context, `remember`/`recall`
write/read the fact store, the system prompt gets a "session memory" block —
survives restarts.

**MCP** (`src/agent/mcp.c`, configured via the `MCP_SERVERS` env var or
`.data/mcp-servers.json`, an array of `{id, command, args, approval}`): stdio
newline-delimited JSON-RPC; at startup the parent spawns each server once
for `initialize` + `tools/list`, registering each tool as
`<id>:<toolName>`; every `tools/call` spawns a one-shot child
(spawn→initialize→initialized→call→SIGKILL), so long-lived workers never
leak fds/pids; `structuredContent` is prettified with `jq(1)` (the binary
still links only libc). `approval` is audit-level for now (the SSE path has
no interactive approval channel; hits the stderr log).

**Testing**: the smoke suite ships a fake upstream
(`scripts/fake-llm-upstream.py`, branching on message/system to simulate each
stage) and a fake stdio MCP server (`scripts/fake-mcp-server.py`) covering:
the tool-call loop, `skill-run` full-text passthrough, sessionId fact
persistence and `recall` readback, the `echo:pong` MCP tools/call, and a
single-round PSE Planner→Specialist→Evaluator (PASS).

**Agent / MCP environment variables**:

| Variable | Default | Description |
|----------|---------|-------------|
| `AGENT_TOOL_SOURCE` | `local` | Tool source: `local` executes the local registry; `gateway` delegates to the tsm-hub gateway (no local schema sent, no local dispatch) |
| `AGENT_MAX_ROUNDS` | `8` | Max tool-loop rounds (1–30) |
| `AGENT_MAX_CONCURRENT` | `4` | Agent concurrency cap (pipe semaphore, 1–32); excess requests get "server busy" |
| `MCP_INIT_BUDGET_SECONDS` | `60` | Total startup budget for MCP handshakes + tools/list; servers not initialized in time are skipped |
| `MCP_FS_ROOT` | — | Root directory for the router-synced `fs` MCP server (server skipped when unset) |

## Privacy & compliance hardening

The security boundary isn't just request handling — data at rest and
outbound error text are guarded too:

- **Local file permissions tightened**: `logs/access.log` and dev-mode
  `.dev-httpd.log` (which carries C stderr: curl errors, router sync, other
  debug fingerprints) are all `0600`; `.env` (the LLM key) is `0600` by
  construction and excluded by `.gitignore`/`.dockerignore`.
- **FCGI socket defaults to `0700`**: a direct socket connection bypasses
  httpd's auth/rate-limit, so it must never be open to every local process;
  multi-user front proxies (nginx workers under a different account) loosen
  it explicitly with `REACT_FCGI_SOCK_MODE=0777`.
- **Security headers everywhere**: C-served responses and `-v` proxied paths
  (dev Vite / production react-ssr-server) emit the same
  `X-Content-Type-Options: nosniff` / `X-Frame-Options: DENY` /
  `Referrer-Policy: no-referrer`; the `Server` fingerprint is unified to
  `AgentHTTPD` and `X-Powered-By` is off.
- **Outbound error scrubbing**: chat SSE error events carry generic text
  only (`upstream connection failed; retry in a moment` etc.); curl exit
  codes, `LLM_API_URL`/`LLM_API_KEY`-style troubleshooting hints and other
  infrastructure details go to the local server log only; dev SSR 500s
  likewise reply "details in server log".
- **Session data boundaries**: `.data/sessions/*.json` stays on the server
  (memory/fact store) and never leaks through responses; the `read_file` tool
  is confined to the web root + traversal protection.
- **No version fingerprint (L1)**: error pages, directory-listing footers and
  CGI `SERVER_SOFTWARE` now carry the same versionless `AgentHTTPD` as the
  `Server:` header - no release details leak anywhere.
- **htpasswd forced onto strong hashes (L2)**: plaintext, 13-character DES
  and MD5-crypt secrets are rejected at load time; only `$5$`/`$6$`/bcrypt
  are accepted. `AGENTHTTPD_ALLOW_WEAK_AUTH=1` is the explicit dev escape
  hatch (see Basic Auth).
- **Dev Vite proxy guarded (L3)**: `-v` stays opt-in (off by default) and now
  prints a loud startup warning that the proxy serves raw dev sources - such
  instances must never be exposed publicly.
- **Dot-prefixed paths refused**: static and CGI requests whose path has a
  `.`-leading component answer 404 (no existence disclosure), and directory
  listings hide hidden entries - a stray `.env`, editor state or VCS
  metadata dropped into the docroot is neither reachable nor enumerable.
- **MCP filesystem root is the model's file sandbox**: everything under
  `MCP_FS_ROOT` is readable (and writable) by the model via chat tools.
  The server warns at startup when that root contains a `.env` or the
  `.data` session store; point it at the narrowest directory you tolerate
  exposing to the model.
- **Fetched web content is untrusted input**: `fetch_url` is SSRF-hardened
  at the transport level, but the page text it returns enters the agent
  loop, so a malicious page can try to steer the model (prompt injection).
  Keep file-tool roots narrow and never hand the agent credentials.
- **Secret hygiene lives in the repo, not on one machine**: the ignore rules
  for every `.env` variant (`deploy/.env.local`, `deploy/.env.cloud.*`) are in
  this repo's `.gitignore`, so a fresh clone or CI checkout cannot stage a key
  file — a developer-local `~/.gitignore_global` entry protects exactly one
  laptop. `.dockerignore` excludes *all* `.env` variants plus `.data/`
  (session transcripts) even though the Dockerfile reads only `.env` today:
  the entire build context is uploaded to the builder. Config that inherently
  needs a machine-specific absolute path ships as a `.example` template
  (`deploy/cicdkit-project.example.json`) with the real file ignored.
- **Instances that can reach local data bind loopback**: the local demo
  container (`deploy/run-local-container.sh`) publishes on `127.0.0.1` by
  default, because it drives MCP bridges into host-side data pipelines —
  publishing on `0.0.0.0` would put those on the LAN. Override with
  `AGENT_HTTPD_BIND` only when another machine genuinely needs it.
- **A reachable instance needs `-l` and auth**: the cloud env template
  (`deploy/env.cloud.example`) ships a non-zero `RATE_LIMIT` *and*
  `AUTH_HTPASSWD`, because a container exposed on a public address with neither
  is an unauthenticated LLM proxy on someone else's bill. Both are entrypoint
  knobs — `deploy/docker-entrypoint.sh` translates them into `-l` / `-a`, since
  a `--env-file` deployment has nowhere else to put a flag. A missing or
  unreadable htpasswd file makes the container **exit** rather than fall back to
  open access. Behind a reverse proxy set `RATE_LIMIT_TRUSTED_PROXIES`, or every
  request shares the proxy's bucket and per-IP limiting degrades into a global
  one. Otherwise restrict the security group to known sources.

## CGI environment variables

Variables passed to CGI programs:

| Variable | Description |
|------|------|
| `REQUEST_METHOD` | request method (GET/POST) |
| `QUERY_STRING` | URL query string |
| `CONTENT_TYPE` | request Content-Type |
| `CONTENT_LENGTH` | request body length |
| `SERVER_NAME` | server name |
| `SERVER_PORT` | server port |
| `SCRIPT_NAME` | script path |
| `HTTP_HOST` | request Host header |
| `HTTP_USER_AGENT` | user agent |

## Performance design (event loop + fast/slow paths)

The default run mode is a **master event loop + slow-path worker pool**
(`src/core/event.c`), replacing the earlier fork-per-connection:

```
                      agent-httpd master (single process, kqueue/epoll event loop)
                           │  accepts all connections, peeks headers via recv(MSG_PEEK)
         ┌─────────────────┴──────────────────┐
    fast path (non-blocking, served inline)   slow path (handed to the prefork pool)
    GET/HEAD static/health/304/301/404        CGI / chat SSE / proxy / body requests
    keep-alive multiplexed in place           passed via SCM_RIGHTS to 8 workers
```

- **Fast path, zero forks & zero waiting**: static files, health checks,
  redirects and error pages all complete inside the master's event loop;
  keep-alive connections occupy no worker; large static files zero-copy via
  `sendfile(2)` inside a worker.
- **Slow-path isolation**: blocking work (CGI children, SSE streams,
  Vite/React upstream proxies) goes to the prefork pool — slow requests
  never stall the fast path.
- **Zero-cost classification**: `recv(MSG_PEEK)` previews without consuming —
  when a slow request is handed to a worker, the headers are still in the
  kernel buffer and the worker re-reads them directly; no byte replay.

### Benchmarks (`make bench`, local macOS, 4000 req / 16 conc)

| Mode | keep-alive | per-conn (fresh connection) |
|---|---|---|
| old fork-per-connection (`-w 0`) | 10787 req/s | 846 req/s |
| **event loop (default)** | **11528 req/s** | **5142 req/s** |

The per-conn scenario is **~6x** — the per-connection fork cost vanishes. In
keep-alive both modes lean on connection reuse; the event loop edges ahead
while the process count drops from "one per connection" to 1 + 8.

### Positioning vs. a Next.js app

This architecture and Next.js are not the same species — the trade-offs:

| Dimension | This project | Next.js |
|---|---|---|
| Public HTTP layer | hand-written C (event loop + sendfile + fast/slow split) | Node built-in (libuv is C too, but behind the Node abstraction + GC) |
| Per-connection cost | 1 master + 8 reused workers, no GC pauses | single-process event loop, keep-alive reuse |
| Slow-request isolation | CGI/SSR/chat in dedicated workers, never blocks the fast path | long agent loops occupy the event loop (need workers/queues) |
| SSR/ISR | homegrown react-ssr-server (shared render.tsx + ISR cache) | built-in RSC/SSG/ISR/streaming |
| agent | native C (llm.c + MCP + skills); logic changes need a recompile | TS/Node, hot reload + ecosystem |
| DX | `make dev` two processes; C changes need make + restart | `next dev` all-in-one |
| Security surface | hand-written HTTP, you guard your own edges (SSRF/OOB/leaks fixed; security headers/scrubbing/socket-perms hardened) | framework manages routing/encoding/headers |

**One line**: for "rendering + product iteration speed", Next.js wins; for
"serving the most concurrent HTTP/agent traffic with the fewest resources",
this C architecture wins — on GC-freedom, zero-copy, and process isolation
that Next can't swap in.

## Future directions

- [x] POST body forwarding to CGI (stdin pipe + CONTENT_LENGTH)
- [x] REST verbs: PUT/PATCH/DELETE passed through to CGI (REQUEST_METHOD), OPTIONS answered inline with Allow (static read-only 405 guard, CORS preflights served on the fast path)
- [x] Path traversal defense, directory listings, trailing-slash redirects, custom error pages
- [x] Combined-format access log + SIGHUP rotation
- [x] gzip pre-compressed negotiation (Accept-Encoding + Content-Encoding)
- [x] ETag/304 conditional requests (If-None-Match, strong size+mtime validator, covers gzip siblings)
- [x] Last-Modified + If-Modified-Since fallback revalidation (including replaying our own Last-Modified)
- [x] `Cache-Control: no-cache` policy on static responses (store but revalidate; the 304 absorbs the cost)
- [x] `no-store` on CGI and SSR documents (script-declared directives win) + opt-in `Clear-Site-Data` eviction (`PURGE_CLIENT_CACHE`)
- [x] Content-hashed bundle URL (`/js/react-ssr.js?v=<hash>`, embedded at build time) so a rebuilt bundle is never shadowed by a cached copy
- [x] Relay requests log real body byte counts (the access log recorded 0 for every relayed response)
- [x] sendfile(2) zero-copy + Range/206 resume (single range, 416, Accept-Ranges)
- [x] TCP_NODELAY + listen backlog 128
- [x] Graceful shutdown draining + /health endpoint
- [x] FastCGI backend (UNIX socket, reusing the HTTP dispatch chain)
- [x] HTTP/1.1 keep-alive connection reuse (RFC 7230: persistent by default in 1.1, explicit opt-in for 1.0, `Connection: close` always honored; 100-request cap per connection, close after 5xx, streamed relay responses without Content-Length follow close semantics)
- [~] Thread pool instead of forked processes — **evaluated, not done**: benefits overlap with the prefork worker pool (`-w N`) (isolation is actually worse; a thread pool buys little under the CGI fork+exec model); the project already offers two concurrency models, a third only adds teaching noise
- [x] Basic Auth (`-a htpasswd` + `-r realm`, strong-hash-only htpasswd policy, fail-closed, CGI reads `REMOTE_USER`)
- [x] Per-IP rate limiting (`-l <rps>`, token bucket + shared-memory table, one quota across fork/pool modes, IPv4/IPv6 keys, X-Forwarded-For behind a trusted proxy, 429 + Retry-After)
- [x] Dual-stack listeners (IPv4 `0.0.0.0` + IPv6 `::` on one port, `IPV6_V6ONLY` set explicitly, both accept paths serve either family, graceful IPv4-only fallback when the host has no v6 stack)
- [x] Chat upstream transient-failure retries (3 attempts + 1s/2.5s backoff, transient/permanent classification, backoff watches for client disconnects)
- [x] Privacy & compliance hardening (proxied-path security header parity, FCGI socket default 0700, log files 0600, outbound error scrubbing)
- [x] MCP subprocess secret scrubbing: the LLM credentials (`LLM_API_KEY` / `LLM_API_URL` / `LLM_MODEL`) are `unsetenv`'d in the child before `execvp`, so third-party npx-fetched MCP servers (e.g. `server-filesystem`) never inherit them. (`MCP_FS_ROOT` is kept — the fs server needs it as its sandbox root; the CGI path scrubs the same trio in `cgi.c`.)
- [x] Session memory is strictly session-scoped: `remember` / `recall` refuse to run without a `sessionId`, and the chat prompt no longer injects the shared global pool (`.data/memory.json`). A request with no session can neither read nor write another user's facts (the old global fallback was a cross-user data-leak / prompt-injection sink).
- [x] Access log strips the query string: URLs are logged path-only, so tokens / session ids / PII carried in `?...` are never persisted to `logs/access.log` (which is `0600`).
- [x] Response security headers: every response now carries a `Content-Security-Policy` (`default-src 'self'`, `frame-ancestors 'none'`, `base-uri 'self'`, `object-src 'none'`), `Cross-Origin-Opener-Policy: same-origin`, `Cross-Origin-Resource-Policy: same-origin`, and `X-Permitted-Cross-Domain-Policies: none`, on top of the existing nosniff / X-Frame-Options / Referrer-Policy. (HSTS belongs at the TLS-terminating nginx.)
- [x] Chat CSRF hardening: `/react/api/chat` is POST-only (GET and other verbs are rejected before dispatch), and a browser `Origin` that does not match the server `Host` is rejected with `403`. Non-browser clients (no `Origin`) and same-origin requests are allowed; nginx preserves `Host` via `proxy_set_header Host $host`.
- [x] SSRF hardening on `fetch_url`: blocks loopback / private / link-local /
  cloud-metadata (169.254.169.254) targets by IP literal, bracketed IPv6, and
  **every** resolved address; strips `userinfo@` host obfuscation; fails
  **closed** on DNS resolution failure; and re-validates **every** HTTP
  redirect hop, so a `302` to an internal address is never followed. Private /
  loopback URLs are always blocked by design — there is no toggle to weaken it.
- [x] SSE proxy-safety: the chat stream emits `X-Accel-Buffering: no` so a
  fronting nginx (`:18081`) does not buffer it, plus a 15s `:` heartbeat comment
  so idle proxies don't drop a long generation.
- [~] Virtual hosts — **deferred**: a single docroot suffices for a teaching server; if truly needed, front nginx and split by Host across multiple agent-httpd instances — no need to re-implement it in C
- [x] URL routing (react-router client-side routing + SSR deep links; C-side `/react/` forwarding)
- [~] SSL/HTTPS support — **evaluated, not done**: production convention is to terminate TLS at nginx/load balancers (this repo's docker-compose does exactly that); integrating OpenSSL into a teaching server would bloat the code and derail the focus

## License

MIT — see [LICENSE](LICENSE). The bundled front-end dependencies keep their
own licenses (React, Vite and Tailwind are all MIT).
