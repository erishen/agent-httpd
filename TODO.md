# TODO — agent-httpd

## Security — php2shell exposure through bundled PHP CGI

Status: open (2026-10-22).

CVE-2026-109618 / CVE-2026-109619 ("php2shell") are critical PHP RCEs
(CVSS 9.2) affecting PHP <= 8.2.34, <= 8.3.35, <= 8.4.26, <= 8.5.11.
Fixed in 8.2.35 / 8.3.36 / 8.4.27 / 8.5.12. The flaw is in PHP request
startup / OOM cleanup, so it triggers before application code runs.

This tree is not PHP itself, but both agent-httpd images install
`php-cgi` in their build and runtime stages and ship `cgi-bin/php.cgi`:

- `Dockerfile:32` (build), `Dockerfile:75` (runtime)
- `Dockerfile.slim:33` (build), `Dockerfile.slim:52` (runtime)
- `cgi-bin/php.cgi` executes `cgi-langs/php/main.php` via `php-cgi`

The fixed wrapper script does **not** mitigate the CVE: PHP parses the
request before it reaches `main.php`.

The standard `lume` runtime image does not copy `cgi-bin`; this risk is
limited to images/containers built from this tree or a deployment that
explicitly serves `/cgi-bin`.

- [ ] Inventory every built image/container for `php-cgi -v` and
      `/cgi-bin/php.cgi`; record versions. Old Debian bookworm builds may
      contain a vulnerable 8.2.x even though current bookworm-security
      provides `8.2.35`.
- [ ] If PHP is only a demo, remove `php-cgi` from the runtime stages
      (`Dockerfile:75`, `Dockerfile.slim:52`) and stop copying
      `php.cgi` into the runtime `cgi-bin`; the build stage may keep it
      only if `build-cgis.sh` genuinely needs it to generate the wrapper.
      Verify no PHP interpreter remains in the runtime image.
- [ ] If PHP is required, pin a fixed PHP version (at least
      `8.2.35`; prefer the newest fixed branch compatible with the demo)
      in both build and runtime stages, and rebuild without cache.
- [ ] Add a build-time guard that fails when `php-cgi -v` reports an
      affected version, so a cached/old base image cannot silently pass.
- [ ] Ensure `/cgi-bin` is never in the public auth allowlist. If the
      demo is exposed, require Basic Auth and/or network isolation, then
      verify an unauthenticated `/cgi-bin/php.cgi` request is rejected
      before CGI execution.

## C robustness — unchecked request-body allocation on OOM

`src/http/http.c:715` allocates the Content-Length body without checking
the result. On allocation failure `request.body` stays `NULL` while
`request.content_length > 0`; the login form path later passes that
`NULL` body to `form_field()` (`src/http/http_route.c:307`), which can
dereference it and crash the worker.

This is an OOM robustness / DoS issue, not a php2shell-style UAF or RCE.
Fix and test it independently so the parser cannot dispatch a request
with a partially initialised body.

- [ ] On body allocation failure, set a 500 response, force-close the
      connection, and skip dispatch.
- [ ] Add a regression test using allocation fault injection or a tiny
      address-space limit; assert no SIGSEGV and a bounded response.
- [ ] Audit the remaining request-path allocations for the same pattern;
      prefer a checked allocator or explicit `if (!p)` handling.

## P1 — 2026-10-22 memory-safety audit follow-ups (php2shell-class review)

Status: open (filed 2026-10-22, after the CVE-2026-109618/109619
php2shell review). A line-by-line pass over the request path
(`http.c` / `http_parse.c` / `http_resp.c` / `ratelimit.c` /
`worker.c` / `sqlite_tool.c`) found **no php2shell-class stale
global state / use-after-free**: request state is stack-local in
`handle_client` (zeroed each keep-alive round), shared globals are
fixed-size statics or PTHREAD_PROCESS_SHARED shm, and OOM paths
either exit cleanly or free-and-500. Remaining hardening items:

### Confirmed safe during the audit (context — no action needed)

- Request/response state: stack-local in `handle_client`, `memset`
  per keep-alive iteration — no global pointer into freed request
  memory (the shape CVE-2026-109619's UAF requires is absent).
- Rate-limit shared memory: mutexes explicitly `PTHREAD_PROCESS_
  SHARED` and initialised pre-fork (`ratelimit.c:316-323`) — the
  classic forked-worker UB is avoided.
- OOM on the chunked body path: checked allocation, `free` on the
  error branch, 500 + force-close.
- `sqlite_system_extra`'s `g_cache` (`sqlite_tool.c`): per-worker
  COW copy, mtime-guarded; worst case is a ≤1 s stale schema hint in
  the LLM prompt, not a bug.

### Lone-LF header-line tolerance vs CRLF-only framing (smuggling adjacency)

`parse_headers` (`http_parse.c`) accepts a bare `\n` as a header-line
terminator, while `handle_client` (`http.c`) detects end-of-headers
with `strstr(buffer, "\r\n\r\n")` — the two disagree on lone-LF
frames. Safe today only because every deployed entry point (Nginx
reverse proxy) also tolerates lone LF, so both hops frame
identically (no CL.LF/TE.LF divergence). That is a latent property,
not a guarantee: exposing agent-httpd directly, or putting a
CRLF-strict intermediary in front of it, makes the asymmetry
exploitable.

- [x] Add a strict-CRLF mode (`-s` flag or `STRICT_CRLF=1`): when set,
      `parse_headers` rejects header lines terminated by a bare LF and
      header framing requires CRLF. Default stays lenient for dev.
      **Done 2026-10-22**: strict is now the default; `AGENTHTTPD_LENIENT
      _CRLF=1` restores the old tolerance (read once in main.c); covered
      by `tests/test_crlf.c` (6 shapes) inside `make test-unit`.
- [x] Document the deployment boundary in `README` / `docs`: behind an
      LF-tolerant proxy = safe; standalone public exposure = use
      strict mode.
      **Done 2026-10-22**: README.zh.md documents the env switch.

### ASAN coverage for the keep-alive + streaming loops

`tests/` already runs under LSAN (lume-core ships a suppressions
file), but the two loops that reuse state across requests on one
worker — `test_keepalive_pipeline.c` and `test_fcgi_stream.c`,
exactly the shape php2shell exploited — have no ASAN make target.

- [x] Add `make test-asan` (CC/FLAGS + `-fsanitize=address,leak`),
      run the whole `tests/` suite, keep the existing pass/fail
      contract.
      **Done 2026-10-22**: `test-asan` re-runs `test-unit` (all suites)
      with ASan+LSan and `test-keepalive` with ASan (leak detection off,
      forked server); SAN_FLAGS parameterized, t_fcgi + t_ka now under
      sanitizers too.
- [x] Run it in CI (Linux at minimum); record the baseline first and
      file suppressions for intentional globals rather than "fixing"
      them.
      **Done 2026-10-22**: `make test-asan` added to the `ubuntu-fuzz`
      CI job; first CI run is the baseline — if LSAN flags intentional
      globals, file a suppressions list (see lume-core's LSAN file)
      instead of changing code.

### Fuzz target for the request parser + chunked decoder

lume-core fuzzes the language parser (P1/P8 over there); the HTTP
layer has no fuzzer. `parse_headers`, the chunked reassembly loop in
`http.c`, and `line_has_embedded_crlf` are the remaining
malformed-input memory-management surface in this tree.

- [x] Add `tests/fuzz_http.c` (libFuzzer): harness 1 drives
      `parse_headers` on raw bytes; harness 2 exercises the chunked
      branch of `handle_client` (oversize chunk-size hex, CL+TE
      conflict, embedded CRLF, lone-LF variants). Seed corpus from
      the existing smoke tests.
      **Done 2026-10-22**: chunked framing was factored out of
      `handle_client` into a pure state machine `src/http/http_chunk.c`
      (`http_dechunk_feed`, resumable) — the fuzz target drives both
      parsers and cross-checks incremental vs one-shot verdicts; the
      same source's regression main runs inside `make test-unit`.
- [x] Time-boxed `make fuzz-http` target (≈5 min under ASAN) so the
      loop stays cheap enough for a pre-push hook.
      **Done 2026-10-22**: `make fuzz-http` = clang libFuzzer 20k runs
      under ASan+UBSan; wired into CI (`ubuntu-fuzz` job).

### Session prune vs worker writes (data integrity, not memory safety)

`session_prune_thread` (`framework.c:377`) runs in the **parent**
process (created pre-fork; threads do not survive into workers) and
unlinks transcripts older than 30 days every 24 h, while prefork
workers read/write session files in their COW address spaces.
Confirm the file-level contract:

- [x] Verify `session.c` prune uses unlink-then-recreate (or atomic
      rename) semantics so a worker mid-write cannot silently lose an
      active session — symptom would be sporadic logouts / vanished
      sessions. Add a test that prunes an active session file while a
      fake worker keeps appending.
      **Done 2026-10-22**: `session_save` is tmp+rename (atomic, 0600
      via fchmod, tmp cleaned on failure); the prune only matches
      `*.json` names, so a worker's in-flight `x.json.tmp.<pid>` can
      never be unlinked under it, and `memory.json` / non-JSON files are
      untouched. Residual TOCTOU: a session pruned while its rename lands
      in the stat→unlink window was already past TTL, so the outcome is
      equivalent to session expiry (not data corruption). Covered by
      `tests/test_session_prune.c` (stale pruned / fresh kept / tmp kept /
      memory kept / non-JSON kept) in `make test-unit`.
