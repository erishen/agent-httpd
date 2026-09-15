#!/bin/sh
# Smoke test for AgentHTTPD: static, HEAD, POST, traversal, redirect, listing, error page
set -u

# Test-only port, distinct from the canonical 18080 (the project's Docker
# stack also publishes 18080; reusing it here makes the suite answer the
# container instead of the freshly-built binary).
PORT=19080
# Tight guard-rail timeouts so misbehaving-client/CGI tests stay fast.
# These only affect this test run; production defaults are 30s.
export REQUEST_TIMEOUT_SECONDS=3
export CGI_TIMEOUT_SECONDS=3
BASE="http://localhost:${PORT}"
SERVER="./bin/agent-httpd"
LOG="/tmp/agent-httpd-test.log"
FCGI_SOCK="/tmp/agent-httpd-fcgi-test.sock"
REACT_SOCK="/tmp/agent-httpd-react-test.sock"

# Only enable the /react/ relay if the resident backend bundle exists
REACT_ARGS=""
[ -x "bin/react-ssr-server" ] && REACT_ARGS="-R $REACT_SOCK"
# Force the demo engine for every smoke instance: an empty LLM_API_KEY in
# the environment beats the project-root .env (which may hold a real key),
# keeping chat cases deterministic and offline.
export LLM_API_KEY=""
# The one-shot cache purge must be OFF unless a case turns it on explicitly:
# the suite asserts its absence as the default, so a developer shell that
# happened to export it would silently invert that check.
unset PURGE_CLIENT_CACHE
# MCP servers from the developer's .data/mcp-servers.json (npx-based ones can
# take seconds to cold-start) must not slow instance startup here — C loads
# that file unconditionally, so park it for the run and restore on exit. The
# dedicated MCP case below re-enables one explicitly via an inline env
# assignment, which takes precedence over this export.
export MCP_SERVERS=""
MCP_BAK=".data/mcp-servers.json.smoke-bak"
if [ -f .data/mcp-servers.json ]; then
    mv .data/mcp-servers.json "$MCP_BAK"
fi
restore_mcp() {
    if [ -f "$MCP_BAK" ]; then
        mv "$MCP_BAK" .data/mcp-servers.json
    fi
    # Transient CGI fixtures written further down (zz-{big,slow,mid,redir,
    # status,cachectl}-test.cgi) must not survive the run: they are ordinary
    # files now, so anything left behind shows up in `git status` after every
    # `make test`. Glob rather than list them, so a new fixture is covered.
    rm -f cgi-bin/zz-*-test.cgi
}
trap restore_mcp EXIT INT TERM

pass=0
fail=0

check() {
    desc="$1"
    expected="$2"
    actual="$3"
    if [ "$actual" = "$expected" ]; then
        echo "PASS: $desc"
        pass=$((pass + 1))
    else
        echo "FAIL: $desc (expected=$expected actual=$actual)"
        fail=$((fail + 1))
    fi
}

pkill -9 -x agent-httpd 2>/dev/null
pkill -9 -f 'react-ssr-server' 2>/dev/null
rm -f "$FCGI_SOCK"
rm -f "$REACT_SOCK"
sleep 0.5

"$SERVER" -p "$PORT" -F "$FCGI_SOCK" $REACT_ARGS > "$LOG" 2>&1 &
PID=$!

# wait for server to accept connections (up to ~5s)
ready=0
i=0
while [ "$i" -lt 50 ]; do
    if curl -s -o /dev/null --max-time 0.3 "http://localhost:${PORT}/" 2>/dev/null; then
        ready=1
        break
    fi
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "Server exited prematurely:" >&2
        cat "$LOG" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.1
done
if [ "$ready" -ne 1 ]; then
    echo "Server did not become ready" >&2
    cat "$LOG" >&2
    exit 1
fi

status() { curl -s -o /dev/null -w "%{http_code}" "$@"; }

static=$(status "$BASE/")
check "static /" "200" "$static"

# large static files (>64KB) are streamed; downloaded size must match disk
if [ -f "www/js/react-ssr.js" ]; then
    fsize=$(wc -c < "www/js/react-ssr.js" | tr -d ' ')
    dsize=$(curl -s -o /dev/null -w "%{size_download}" "$BASE/js/react-ssr.js")
    check "large static file streamed (size $fsize)" "$fsize" "$dsize"

    # gzip precompression: .gz twin served on Accept-Encoding negotiation
    gzip_hdr=$(curl -s -I -H "Accept-Encoding: gzip, deflate" "$BASE/js/react-ssr.js" \
        | grep -c "^Content-Encoding: gzip")
    check "gzip negotiation header" "1" "$gzip_hdr"
    gz_len=$(curl -s -o /dev/null -w "%{size_download}" -H "Accept-Encoding: gzip" \
        "$BASE/js/react-ssr.js")
    gz_ok=0
    [ -n "$gz_len" ] && [ "$gz_len" -lt "$fsize" ] && gz_ok=1
    check "gzip response smaller than raw" "1" "$gz_ok"
    roundtrip=$(curl -s -H "Accept-Encoding: gzip" "$BASE/js/react-ssr.js" | gzip -dc | wc -c | tr -d ' ')
    check "gzip body decompresses to raw size" "$fsize" "$roundtrip"
    # ...and to the same bytes. Browsers always advertise gzip, so the .gz twin
    # is what they actually cache and run; a size-only check would let a stale
    # twin through whenever a rebuild happened to keep the compressed length.
    gz_same=$(curl -s -H "Accept-Encoding: gzip" "$BASE/js/react-ssr.js" | gzip -dc \
        | cmp -s - "www/js/react-ssr.js" && echo 1 || echo 0)
    check "gzip twin decompresses to the same bytes" "1" "$gz_same"
    no_gzip=$(curl -s -I "$BASE/js/react-ssr.js" | grep -c "Content-Encoding")
    check "no Content-Encoding without Accept-Encoding" "0" "$no_gzip"
fi

# regression: files just under the stream threshold must not be truncated.
# Static file bodies above MAX_MEM_BODY_SIZE (static.c) are streamed from
# stream_path; a 63KB file must download in full, not hang on a short body /
# lying Content-Length.
mkdir -p www/js
head -c 63000 /dev/urandom > www/js/fix-mid.bin
m_fsize=$(wc -c < www/js/fix-mid.bin | tr -d ' ')
m_dsize=$(curl -s -o /dev/null -w "%{size_download}" "$BASE/js/fix-mid.bin")
check "sub-threshold static file not truncated ($m_fsize)" "$m_fsize" "$m_dsize"
rm -f www/js/fix-mid.bin

head_line=$(curl -s -I "$BASE/" | head -1)
head=$(echo "$head_line" | grep -c "200 OK")
head_has_body=$(curl -s -I "$BASE/" | grep -c "<!DOCTYPE")
check "HEAD method" "1" "$head"
check "HEAD no body" "0" "$head_has_body"

post=$(curl -s -X POST -d "name=T&message=M" "$BASE/cgi-bin/form.cgi" \
    | grep -c "<li><strong>name:</strong> T</li>")
check "POST to CGI" "1" "$post"

# REST verbs: OPTIONS is answered inline with an Allow menu (RFC 9110 9.3.1
# server-preflight), writer methods pass through to CGI via REQUEST_METHOD,
# and static URLs stay read-only with 405 + Allow.
opts_static_allow=$(curl -s -X OPTIONS -D- -o /dev/null "$BASE/" | grep -ci '^allow: GET, HEAD, OPTIONS')
check "OPTIONS static: Allow menu" "1" "$opts_static_allow"
opts_cgi_allow=$(curl -s -X OPTIONS -D- -o /dev/null "$BASE/cgi-bin/hello.cgi" \
    | grep -ci '^allow: GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS')
check "OPTIONS CGI: full Allow menu" "1" "$opts_cgi_allow"
put_static=$(curl -s -X PUT -d x=1 -o /dev/null -w "%{http_code}" "$BASE/")
put_allow=$(curl -s -X PUT -D- -o /dev/null "$BASE/" | grep -ci '^allow: GET, HEAD, OPTIONS')
[ "$put_static" = "405" ] && [ "$put_allow" = "1" ] && put_ok=1 || put_ok=0
check "PUT to static: 405 + Allow" "1" "$put_ok"
del_cgi=$(curl -s -X DELETE "$BASE/cgi-bin/hello.cgi" | grep -c 'REQUEST_METHOD:</strong> DELETE')
check "DELETE passes through to CGI" "1" "$del_cgi"
patch_cgi=$(curl -s -X PATCH "$BASE/cgi-bin/hello.cgi" | grep -c 'REQUEST_METHOD:</strong> PATCH')
check "PATCH passes through to CGI" "1" "$patch_cgi"
not_impl=$(curl -s -X TRACE -o /dev/null -w "%{http_code}" "$BASE/")
check "unknown method: 501" "501" "$not_impl"

# Chunked request bodies (RFC 9110 8.7): curl streams the POST as chunks,
# the server decodes to a plain body, and the CGI sees the joined payload.
chunk_body=$(curl -s -X POST -H 'Transfer-Encoding: chunked' \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data-binary 'name=Chunky&message=Wire' "$BASE/cgi-bin/form.cgi" \
    | grep -c '<li><strong>name:</strong> Chunky</li>')
check "chunked body decoded for CGI" "1" "$chunk_body"
# TE+CL smuggling shape must be rejected outright (RFC 9110 6.1).
smuggle=$(printf 'POST /cgi-bin/form.cgi HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\nx=1&' \
    | python3 -c "
import socket, sys
req = sys.stdin.buffer.read()
s = socket.create_connection(('localhost', int('$PORT')), timeout=6)
s.sendall(req)
try:
    print(s.recv(64).decode(errors='replace').split()[1])
except Exception:
    print('000')
")
check "TE+CL smuggling rejected: 400" "400" "$smuggle"

cgi=$(status "$BASE/cgi-bin/hello.cgi")
check "GET CGI" "200" "$cgi"

trav=$(curl -s -o /dev/null -w "%{http_code}" --path-as-is "$BASE/../README.md")
check "path traversal blocked" "404" "$trav"

redir=$(status "$BASE/cgi-bin")
check "trailing-slash redirect" "301" "$redir"

redir_loc=$(curl -s -o /dev/null -w "%{redirect_url}" "$BASE/test")
check "redirect location" "http://localhost:${PORT}/test/" "$redir_loc"

listing=$(status "$BASE/test/")
check "directory listing" "200" "$listing"

# regression: a listing that would overflow the 64KB page must stay bounded
# instead of walking p past `end` (snprintf returns the would-be length).
mkdir -p www/fixlist
for i in $(seq 1 2000); do : > "www/fixlist/f$i"; done
l_out="/tmp/fixlist_$$.out"
l_code=$(curl -s -o "$l_out" -w "%{http_code}" "$BASE/fixlist/")
l_size=$(wc -c < "$l_out")
rm -f "$l_out"
l_ok=0
[ "$l_code" = "200" ] && [ "$l_size" -le 65535 ] && l_ok=1
check "directory listing bounded (large dir)" "1" "$l_ok"
rm -rf www/fixlist

err=$(status "$BASE/nothing")
check "404 status" "404" "$err"

# React SSR CGI (GET + POST) - requires node (compiled single-file cgi)
if command -v node >/dev/null 2>&1; then
    curl -s -D /tmp/ssr-h.txt -o /tmp/ssr-b.txt "$BASE/cgi-bin/react-ssr.cgi?name=SSR&message=hi"
    check "React SSR GET" "1" "$(grep -c 'Rendered on the server' /tmp/ssr-b.txt)"
    # The document must ask for a cache-busted bundle URL: with a stable URL
    # a browser that cached the previous bundle keeps serving it, and the new
    # markup fails to hydrate against the old JS.
    check "SSR document links the fingerprinted bundle" "1" \
        "$(grep -c 'react-ssr\.js?v=[0-9a-f]\{8\}' /tmp/ssr-b.txt)"
    # Script output is per-request, so it must not be reusable without asking.
    check "SSR CGI response carries Cache-Control: no-store" "1" \
        "$(grep -ci '^cache-control: no-store' /tmp/ssr-h.txt)"
    rm -f /tmp/ssr-h.txt /tmp/ssr-b.txt
    ssr_post=$(curl -s -X POST -d "name=SRRPOST&message=yo" "$BASE/cgi-bin/react-ssr.cgi" \
        | grep -c "Hello, SRRPOST")
    check "React SSR POST" "1" "$ssr_post"
fi

# Python CGI (GET + POST, stdlib)
if command -v python3 >/dev/null 2>&1; then
    py_get=$(curl -s "$BASE/cgi-bin/python.cgi?name=PY&message=hi" \
        | grep -c "Rendered by a Python script")
    check "Python CGI GET" "1" "$py_get"
    py_post=$(curl -s -X POST -d "name=PYPOST&message=yo" "$BASE/cgi-bin/python.cgi" \
        | grep -c "PYPOST")
    check "Python CGI POST" "1" "$py_post"
fi

# Multi-language CGI examples (built by make build-cgis; skipped if absent)
# Each language test uses a distinctive marker echoed from its page.
# --- regression guardrails ---------------------------------------------
# Static file WITH a query string must serve (used to 404: the full URI was
# fed to realpath; index/directory logic also has to ignore the query).
q_index=$(status "$BASE/?x=1")
check "static / with query string" "200" "$q_index"
q_html=$(status "$BASE/index.html?a=b")
check "static file with query string" "200" "$q_html"
q_dir=$(status "$BASE/test/?q=1")
check "directory with query string" "200" "$q_dir"
q_redir=$(status "$BASE/test?tab=x")
check "dir redirect with query string" "301" "$q_redir"

# CGI emitting >512KB (over the tmp-threshold) must arrive complete via the
# streaming temp-file path, byte-for-byte.
bigcgi="$PWD/cgi-bin/zz-big-test.cgi"
printf '#!/bin/bash\necho "Content-Type: text/plain"\necho ""\nhead -c 600000 /dev/zero | tr "\\0" "x"\n' > "$bigcgi"
chmod +x "$bigcgi"
big_size=$(curl -s "$BASE/cgi-bin/zz-big-test.cgi" | wc -c | tr -d ' ')
check "CGI output 600000 streamed complete" "600000" "$big_size"

# Slow CGI: handler must kill it and answer 504 instead of hanging forever
# (CGI timeout is pre-set to 3s via CGI_TIMEOUT_SECONDS above).
slowcgi="$PWD/cgi-bin/zz-slow-test.cgi"
printf '#!/bin/bash\necho "Content-Type: text/plain"\necho ""\necho tick\nsleep 30\n' > "$slowcgi"
chmod +x "$slowcgi"
slow_start=$(date +%s)
slow_status=$(curl -s -o /dev/null -w "%{http_code}" --max-time 15 "$BASE/cgi-bin/zz-slow-test.cgi")
slow_elapsed=$(( $(date +%s) - slow_start ))
check "slow CGI killed with 504" "504" "$slow_status"
slow_fast=0
[ "$slow_elapsed" -le 8 ] && slow_fast=1
check "slow CGI did not hang (took ${slow_elapsed}s)" "1" "$slow_fast"

# Large CGI output (~700KB, under the tmp threshold) must not be truncated
# at the old 64KB buffer ceiling.
midcgi="$PWD/cgi-bin/zz-mid-test.cgi"
printf '#!/bin/bash\necho "Content-Type: text/plain"\necho ""\nhead -c 700000 /dev/zero | tr "\\0" "y"\n' > "$midcgi"
chmod +x "$midcgi"
mid_size=$(curl -s "$BASE/cgi-bin/zz-mid-test.cgi" | wc -c | tr -d ' ')
check "CGI output 700000 complete (no 64KB truncation)" "700000" "$mid_size"

# form.cgi must escape HTML metacharacters in user input (XSS guard).
xss=$(curl -s "$BASE/cgi-bin/form.cgi?name=%3Cscript%3Ealert(1)%3C/script%3E" \
    | grep -c '<script>alert(1)</script>')
check "form.cgi escapes user input (no raw <script>)" "0" "$xss"
xss_esc=$(curl -s "$BASE/cgi-bin/form.cgi?name=%3Cscript%3Ealert(1)%3C/script%3E" \
    | grep -c '&lt;script&gt;alert(1)&lt;/script&gt;')
check "form.cgi shows escaped script tag" "1" "$xss_esc"

# CGI redirect via Location header (RFC 3875): 302 + Location passthrough,
# and the script's body is still forwarded.
redircgi="$PWD/cgi-bin/zz-redir-test.cgi"
printf '#!/bin/bash\necho "Location: /test/"\necho "Content-Type: text/plain"\necho ""\necho moved\n' > "$redircgi"
chmod +x "$redircgi"
redir_code=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/cgi-bin/zz-redir-test.cgi")
check "CGI Location -> 302" "302" "$redir_code"
redir_loc=$(curl -s -o /dev/null -w "%{redirect_url}" "$BASE/cgi-bin/zz-redir-test.cgi")
check "CGI Location header value" "$BASE/test/" "$redir_loc"
redir_body=$(curl -s "$BASE/cgi-bin/zz-redir-test.cgi" | grep -c "moved")
check "CGI redirect still carries body" "1" "$redir_body"

# CGI Status header overrides the response status line (RFC 3875).
statuscgi="$PWD/cgi-bin/zz-status-test.cgi"
printf '#!/bin/bash\necho "Status: 418 I am a teapot"\necho "Content-Type: text/plain"\necho ""\necho short and stout\n' > "$statuscgi"
chmod +x "$statuscgi"
teapot=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/cgi-bin/zz-status-test.cgi")
check "CGI Status header honoured (418)" "418" "$teapot"
teapot_body=$(curl -s "$BASE/cgi-bin/zz-status-test.cgi" | grep -c "short and stout")
check "CGI Status response has body" "1" "$teapot_body"

# Cache policy on the CGI path (src/cgi/cgi.c). Script output is produced per
# request, so silence must not mean "cache me heuristically": the default is
# no-store, while a script that states its own directive keeps it - the
# default is a fallback, never an override.
chkcgi="$PWD/cgi-bin/zz-cachectl-test.cgi"
printf '#!/bin/bash\necho "Content-Type: text/plain"\necho "Cache-Control: max-age=60"\necho ""\necho script policy\n' > "$chkcgi"
chmod +x "$chkcgi"
check "CGI default cache policy is no-store" "1" \
    "$(curl -s -D - -o /dev/null "$BASE/cgi-bin/hello.cgi" | grep -ci '^cache-control: no-store')"
check "CGI explicit Cache-Control is honoured, not overridden" "1" \
    "$(curl -s -D - -o /dev/null "$BASE/cgi-bin/zz-cachectl-test.cgi" | grep -ci '^cache-control: max-age=60')"
rm -f "$chkcgi"

# Clear-Site-Data is opt-in: without PURGE_CLIENT_CACHE nothing may ask a
# browser to drop its store, or every response would evict the bundle.
check "Clear-Site-Data absent unless purge is enabled" "0" \
    "$(curl -s -D - -o /dev/null "$BASE/" | grep -ci '^clear-site-data')"

# Accept-Encoding quality values: gzip;q=0 must NOT be served gzip,
# wildcard * must be, plain list still matches, deflate-only does not.
no_gzip_q0=$(curl -s -I -H "Accept-Encoding: gzip;q=0" "$BASE/js/react-ssr.js" | grep -c "Content-Encoding")
check "gzip;q=0 not served compressed" "0" "$no_gzip_q0"
star_gzip=$(curl -s -I -H "Accept-Encoding: *" "$BASE/js/react-ssr.js" | grep -c "Content-Encoding: gzip")
check "wildcard Accept-Encoding gets gzip" "1" "$star_gzip"
plain_gzip=$(curl -s -I -H "Accept-Encoding: deflate, gzip" "$BASE/js/react-ssr.js" | grep -c "Content-Encoding: gzip")
check "plain gzip still negotiated" "1" "$plain_gzip"
deflate_only=$(curl -s -I -H "Accept-Encoding: deflate" "$BASE/js/react-ssr.js" | grep -c "Content-Encoding")
check "deflate-only gets no gzip" "0" "$deflate_only"

rm -f "$redircgi" "$statuscgi"

# HTTP/1.1 keep-alive: two fetches in one curl run share one TCP connection.
# curl prints num_connects per URL: "1" for the first, "0" when the second
# reuses the pooled connection.
ka_connects=$(curl -s -o /dev/null -o /dev/null -w "%{num_connects}\n" "$BASE/" "$BASE/index.html" 2>/dev/null | tail -1)
check "keep-alive reuses connection (second fetch new conns=$ka_connects)" "0" "$ka_connects"
ka_hdr=$(curl -s -I "$BASE/" | grep -c "^Connection: keep-alive")
check "keep-alive header on HTTP/1.1" "1" "$ka_hdr"
ka_close=$(curl -s -I -H "Connection: close" "$BASE/" | grep -c "^Connection: close")
check "Connection: close honoured" "1" "$ka_close"
ka_10=$(curl -s --http1.0 -I "$BASE/" | grep -c "^Connection: close")
check "HTTP/1.0 defaults to close" "1" "$ka_10"

# 10 sequential requests over ONE raw socket, all must answer 200.
ka_seq=$(python3 - "$PORT" <<'PYEOF'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.settimeout(5)
ok = 0
for i in range(10):
    s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
    data = b""
    while b"\r\n\r\n" not in data:
        c = s.recv(4096)
        if not c: break
        data += c
    head, rest = data.split(b"\r\n\r\n", 1)
    clen = int([l for l in head.split(b"\r\n") if l.lower().startswith(b"content-length:")][0].split(b":")[1])
    while len(rest) < clen:
        rest += s.recv(4096)
    if head.startswith(b"HTTP/1.1 200"):
        ok += 1
s.close()
print(ok)
PYEOF
)
check "10 requests on one connection" "10" "$ka_seq"

# Half-open request (headers never terminated) must be dropped by the
# request-timeout guard (REQUEST_TIMEOUT_SECONDS=3), not pinned forever:
# the server closes the socket, so the probe sees EOF within a few seconds.
slowloris_start=$(date +%s)
slowloris_result=$(python3 - "$PORT" <<'PYEOF'
import socket, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n")
s.settimeout(15)
t0 = time.time()
try:
    data = s.recv(4096)
    print("closed" if data == b"" else "early-response")
except socket.timeout:
    print("pinned")
except (ConnectionResetError, BrokenPipeError):
    print("closed")
s.close()
PYEOF
)
slowloris_elapsed=$(( $(date +%s) - slowloris_start ))
slowloris_ok=0
case "$slowloris_result" in
    closed|early-response) slowloris_ok=1 ;;
esac
slowloris_fast=0
[ "$slowloris_elapsed" -le 8 ] && slowloris_fast=1
check "half-request closed by timeout (${slowloris_elapsed}s, $slowloris_result)" "1" "$slowloris_ok"
check "half-request did not hang (${slowloris_elapsed}s)" "1" "$slowloris_fast"

rm -f "$bigcgi" "$slowcgi" "$midcgi"

lang_get_marker() {
    case "$1" in
        go)   echo "Go CGI" ;;
        rust) echo "Rust CGI" ;;
        java) echo "Java CGI" ;;
        php)  echo "PHP CGI" ;;
        ruby) echo "Ruby CGI" ;;
    esac
}

# Basic Auth (-a htpasswd): dedicated instance on a separate port. Entries:
# alice:password123 (plaintext), bob:cd29FLxV1BmJQ (DES crypt hash of
# "s3cret-pw", portable across macOS/Linux).
# AGENTHTTPD_ALLOW_WEAK_AUTH=1 is required here: auth.c hard-rejects plaintext
# and DES/MD5 entries by default, and macOS crypt(3) cannot produce the strong
# $6$ (SHA-512) form the default policy wants — the container/CI path covers
# the strong-hash case separately. Without the escape hatch every entry is
# skipped, load_htpasswd() returns -1 and the instance never starts.
AUTH_PORT=3110
AUTH_BASE="http://localhost:$AUTH_PORT"
HTPASSWD="/tmp/agent-httpd-htpasswd-test"
printf 'alice:password123\nbob:cd29FLxV1BmJQ\n' > "$HTPASSWD"
sleep 0.4
AGENTHTTPD_ALLOW_WEAK_AUTH=1 "$SERVER" -p "$AUTH_PORT" -a "$HTPASSWD" -r "AuthTest" > /tmp/agent-httpd-auth.log 2>&1 &
AUTH_PID=$!
auth_ready=0
i=0
while [ "$i" -lt 30 ]; do
    curl -s -o /dev/null --max-time 1 "$AUTH_BASE/" && auth_ready=1 && break
    i=$((i + 1))
    sleep 0.2
done
check "auth instance up" "1" "$auth_ready"
check "auth: no credentials -> 401" "401" "$(status "$AUTH_BASE/")"
check "auth: 401 carries WWW-Authenticate challenge" "1" \
    "$(curl -s -i --max-time 5 "$AUTH_BASE/" | grep -ci 'www-authenticate: basic realm="AuthTest"')"
check "auth: plaintext entry accepted" "200" \
    "$(status -u alice:password123 "$AUTH_BASE/")"
check "auth: crypt hash entry accepted" "200" \
    "$(status -u bob:s3cret-pw "$AUTH_BASE/")"
check "auth: wrong password -> 401" "401" \
    "$(status -u alice:wrongpw "$AUTH_BASE/")"
check "auth: unknown user -> 401" "401" \
    "$(status -u mallory:x "$AUTH_BASE/")"
check "auth: malformed Authorization -> 401" "401" \
    "$(status -H 'Authorization: Basic !!!notbase64!!!' "$AUTH_BASE/")"
check "auth: protects CGI too" "1" \
    "$(curl -s --max-time 10 -u alice:password123 "$AUTH_BASE/cgi-bin/hello.cgi" | grep -c 'Hello from CGI!')"
kill "$AUTH_PID" 2>/dev/null
wait "$AUTH_PID" 2>/dev/null
rm -f "$HTPASSWD"

# ETag / conditional requests (RFC 7232): static files carry a W/"size-mtime"
# validator; a matching If-None-Match short-circuits to a body-less 304.
ETAG_PORT=3113
ETAG_BASE="http://localhost:$ETAG_PORT"
"$SERVER" -p "$ETAG_PORT" > /tmp/agent-httpd-etag.log 2>&1 &
ETAG_PID=$!
etag_ready=0
i=0
while [ "$i" -lt 30 ]; do
    curl -s -o /dev/null --max-time 1 "$ETAG_BASE/" && etag_ready=1 && break
    i=$((i + 1))
    sleep 0.2
done
check "etag instance up" "1" "$etag_ready"
ETAG_H=/tmp/agent-httpd-etag-h1.txt
ETAG_B=/tmp/agent-httpd-etag-body.txt
curl -s -D "$ETAG_H" -o "$ETAG_B" --max-time 5 "$ETAG_BASE/index.html"
ETAG_VAL=$(grep -i '^etag:' "$ETAG_H" | tr -d '\r' | awk '{print $2}')
check "etag: static 200 carries ETag + Vary" "2" \
    "$(grep -icE '^etag:|^vary: accept-encoding' "$ETAG_H")"
check "etag: validator is W/\"size-mtime\" form" "1" \
    "$(printf '%s' "$ETAG_VAL" | grep -c '^W/"[0-9a-f]*-[0-9a-f]*"$')"
rm -f "$ETAG_B" # curl only touches -o when body bytes arrive
check "etag: matching If-None-Match -> 304 without body" "304|" \
    "$(curl -s -o "$ETAG_B" -w '%{http_code}' -H "If-None-Match: $ETAG_VAL" "$ETAG_BASE/index.html")|$(wc -c < "$ETAG_B" 2>/dev/null | tr -d ' ')"
check "etag: stale validator -> 200 full body" "200" \
    "$(curl -s -o /dev/null -w '%{http_code}' -H 'If-None-Match: W/"999-999"' "$ETAG_BASE/index.html")"
check "etag: '*' matches any current entity" "304" \
    "$(curl -s -o /dev/null -w '%{http_code}' -H 'If-None-Match: *' "$ETAG_BASE/index.html")"
kill "$ETAG_PID" 2>/dev/null
wait "$ETAG_PID" 2>/dev/null
rm -f "$ETAG_H" "$ETAG_B"

# Caching/Range/health additions: Last-Modified + If-Modified-Since
# fallback, single-range 206/416, /health endpoint.
CACHE_PORT=3114
CACHE_BASE="http://localhost:$CACHE_PORT"
TESTBIN="${WWW_DIR:-.}/www/range-test-smoke.bin"
dd if=/dev/zero of="$TESTBIN" bs=1024 count=64 2>/dev/null
"$SERVER" -p "$CACHE_PORT" > /tmp/agent-httpd-cache.log 2>&1 &
CACHE_PID=$!
cache_ready=0
i=0
while [ "$i" -lt 30 ]; do
    curl -s -o /dev/null --max-time 1 "$CACHE_BASE/" && cache_ready=1 && break
    i=$((i + 1))
    sleep 0.2
done
check "cache instance up" "1" "$cache_ready"
check "Last-Modified header present" "1" \
    "$(curl -sI "$CACHE_BASE/index.html" | grep -ci '^last-modified: .*GMT')"
check "If-Modified-Since (file newer) -> 200" "200" \
    "$(status -H 'If-Modified-Since: Mon, 01 Jan 2024 00:00:00 GMT' "$CACHE_BASE/index.html")"
check "If-Modified-Since (future) -> 304" "304" \
    "$(status -H 'If-Modified-Since: Fri, 01 Jan 2100 00:00:00 GMT' "$CACHE_BASE/index.html")"
# The case that actually happens: a client replays the Last-Modified we just
# sent it. RFC 9110 13.2.2 counts "equal" as unmodified, so this must be 304 -
# and it only is if the parser keeps the time of day (same-day replay).
CACHE_LM=$(curl -sI "$CACHE_BASE/index.html" | tr -d '\r' | awk -F': ' 'tolower($1)=="last-modified"{print $2}')
check "If-Modified-Since replay of Last-Modified -> 304" "304" \
    "$(status -H "If-Modified-Since: $CACHE_LM" "$CACHE_BASE/index.html")"
# Same replay through the gzip twin, which is the representation a browser
# actually caches (its Last-Modified is the .gz file's own mtime).
GZ_LM=$(curl -sI -H 'Accept-Encoding: gzip' "$CACHE_BASE/js/react-ssr.js" | tr -d '\r' | awk -F': ' 'tolower($1)=="last-modified"{print $2}')
check "gzip twin: Last-Modified replay -> 304" "304" \
    "$(status -H 'Accept-Encoding: gzip' -H "If-Modified-Since: $GZ_LM" "$CACHE_BASE/js/react-ssr.js")"
# Cache policy: without an explicit directive a browser may keep serving a
# stale copy for 10% of the file's age without asking (hours, for a bundle
# whose .gz sibling carries a build-time mtime).
check "static 200 carries Cache-Control" "1" \
    "$(curl -sI "$CACHE_BASE/index.html" | grep -ci '^cache-control: no-cache')"
check "304 revalidation repeats the cache policy" "1" \
    "$(curl -sI -H "If-None-Match: *" "$CACHE_BASE/index.html" | grep -ci '^cache-control: no-cache')"
check "206 start-end exact" "100" \
    "$(curl -s -H 'Range: bytes=0-99' "$CACHE_BASE/range-test-smoke.bin" | wc -c | tr -d ' ')"
check "206 open-ended range" "65436" \
    "$(curl -s -H 'Range: bytes=100-' "$CACHE_BASE/range-test-smoke.bin" | wc -c | tr -d ' ')"
check "206 suffix range" "50" \
    "$(curl -s -H 'Range: bytes=-50' "$CACHE_BASE/range-test-smoke.bin" | wc -c | tr -d ' ')"
check "206 carries Content-Range" "1" \
    "$(curl -sI -H 'Range: bytes=0-9' "$CACHE_BASE/range-test-smoke.bin" | grep -ci '^content-range: bytes 0-9/65536')"
check "416 for out-of-bounds range" "416" \
    "$(status -H 'Range: bytes=999999-' "$CACHE_BASE/range-test-smoke.bin")"
check "multi-range falls back to 200" "200" \
    "$(status -H 'Range: bytes=0-1,5-6' "$CACHE_BASE/range-test-smoke.bin")"
check "Accept-Ranges advertised" "1" \
    "$(curl -sI "$CACHE_BASE/index.html" | grep -ci '^accept-ranges: bytes')"
check "/health answers ok" "ok" "$(curl -s "$CACHE_BASE/health")"
kill "$CACHE_PID" 2>/dev/null
wait "$CACHE_PID" 2>/dev/null
rm -f "$TESTBIN"

# --- Bundle fingerprint + the SSR response policy ------------------------
# Two halves of one failure mode: what the server EMBEDDED at build time
# (checked offline against the bytes actually shipped) and what it SERVES
# over the wire (checked against a live resident backend). A stale bundle
# that no URL change ever invalidates looks exactly like "the server is
# serving new code" while every browser runs the old one.
if [ -f www/js/react-ssr.js ]; then
    want_hash=$( (sha256sum www/js/react-ssr.js 2>/dev/null \
        || shasum -a 256 www/js/react-ssr.js) | cut -c1-8 )
    for art in cgi-bin/react-ssr.cgi bin/react-ssr-server; do
        [ -f "$art" ] || continue
        # The bundles are minified, so the URL and the hash do not survive as
        # one literal to regex out: assert both halves are present instead.
        url_ok=$(grep -qF 'react-ssr.js?v=' "$art" && echo 1 || echo 0)
        hash_ok=$(grep -qF "$want_hash" "$art" && echo 1 || echo 0)
        check "$art links the fingerprinted bundle URL" "1" "$url_ok"
        check "$art embeds the shipped bundle's hash" "1" "$hash_ok"
    done
    # The fingerprinted URL must resolve: the static chain has to ignore the
    # query when locating the file, exactly as it does for any other ?query.
    check "fingerprinted bundle URL serves the whole file" \
        "$(wc -c < www/js/react-ssr.js | tr -d ' ')" \
        "$(curl -s "$BASE/js/react-ssr.js?v=$want_hash" | wc -c | tr -d ' ')"
fi

if [ -x bin/react-ssr-server ]; then
    SSR_PORT=3118
    SSR_BASE="http://localhost:$SSR_PORT"
    SSR_SOCK="/tmp/agent-httpd-react-smoke.sock"
    rm -f "$SSR_SOCK"
    # The resident backend is the origin for these documents - it composes
    # the response the relay forwards verbatim - so the purge switch has to
    # be set on BOTH processes, exactly as docker-compose does it.
    PURGE_CLIENT_CACHE=1 REACT_FCGI_SOCK="$SSR_SOCK" \
        bin/react-ssr-server > /tmp/agent-httpd-react-smoke.log 2>&1 &
    SSR_REACT_PID=$!
    PURGE_CLIENT_CACHE=1 "$SERVER" -p "$SSR_PORT" -R "$SSR_SOCK" \
        -L /tmp/agent-httpd-ssr-access.log > /tmp/agent-httpd-ssr-stdout.log 2>&1 &
    SSR_PID=$!
    ssr_ready=0
    i=0
    while [ "$i" -lt 60 ]; do
        if curl -s -o /dev/null --max-time 1 "$SSR_BASE/react/chat" 2>/dev/null; then
            ssr_ready=1
            break
        fi
        i=$((i + 1))
        sleep 0.25
    done
    check "SSR relay instance up" "1" "$ssr_ready"
    curl -s -D /tmp/ssr-relay-h.txt -o /tmp/ssr-relay-b.txt "$SSR_BASE/react/chat"
    # The relay streams the backend's bytes verbatim, so whatever the
    # resident server omits is missing end to end - Date included.
    check "SSR relay document carries Date" "1" \
        "$(grep -ci '^date: .*GMT' /tmp/ssr-relay-h.txt)"
    check "SSR relay document carries Cache-Control: no-store" "1" \
        "$(grep -ci '^cache-control: no-store' /tmp/ssr-relay-h.txt)"
    check "SSR relay document asks for the fingerprinted bundle" "1" \
        "$(grep -c 'react-ssr\.js?v=[0-9a-f]\{8\}' /tmp/ssr-relay-b.txt)"
    check "purge on: document asks to drop the origin cache" "1" \
        "$(grep -ci '^clear-site-data: "cache"' /tmp/ssr-relay-h.txt)"
    # Only documents evict. A per-asset Clear-Site-Data would drop the bundle
    # on every page view, defeating the policy it exists to repair.
    check "purge on: assets do NOT evict" "0" \
        "$(curl -s -D - -o /dev/null "$SSR_BASE/js/react-ssr.js" | grep -ci '^clear-site-data')"
    rm -f /tmp/ssr-relay-h.txt /tmp/ssr-relay-b.txt
    kill "$SSR_PID" "$SSR_REACT_PID" 2>/dev/null
    wait "$SSR_PID" "$SSR_REACT_PID" 2>/dev/null
    rm -f "$SSR_SOCK"
fi

# Native C LLM chat endpoint (src/agent/llm.c): SSE envelope identical to the
# node backend's chat.ts. LLM_API_KEY is exported empty above, so the
# deterministic offline demo engine answers (the C demo reply mentions
# "C demo engine" — the node one doesn't — which pins the serving side).
CHAT_PORT=3116
CHAT_BASE="http://localhost:$CHAT_PORT"
"$SERVER" -p "$CHAT_PORT" > /tmp/agent-httpd-chat.log 2>&1 &
CHAT_PID=$!
chat_ready=0
i=0
while [ "$i" -lt 30 ]; do
    curl -s -o /dev/null --max-time 1 "$CHAT_BASE/" && chat_ready=1 && break
    i=$((i + 1))
    sleep 0.2
done
check "chat instance up" "1" "$chat_ready"
chat_out=$(curl -s --max-time 15 -X POST "$CHAT_BASE/react/api/chat" \
    -H 'Content-Type: application/json' \
    -d '{"message":"hello there","history":[{"role":"weird","content":"skip"},{"role":"user","content":"a"},{"role":"assistant","content":"b"}]}')
# deltas arrive token-split; join the stream into prose before asserting
chat_join=$(printf '%s' "$chat_out" | tr -d '\n' | sed 's/data: {"t":"delta","d":"//g; s/"}//g')
check "chat: C demo engine answers (not a relay)" "1" \
    "$(printf '%s' "$chat_join" | grep -c 'C demo engine')"
check "chat: opens with note event" "1" \
    "$(printf '%s' "$chat_out" | head -2 | grep -c '"t":"note"')"
check "chat: streams multiple delta events" "1" \
    "$(printf '%s' "$chat_out" | grep -c '"t":"delta"' | awk '{print ($1 > 3) ? 1 : 0}')"
check "chat: terminates with done event" "1" \
    "$(printf '%s' "$chat_out" | grep -c '"t":"done"')"
check "chat: bad JSON -> error event" "1" \
    "$(curl -s --max-time 5 -X POST "$CHAT_BASE/react/api/chat" -d 'not-json' | grep -c '"t":"error"')"
check "chat: empty message -> error event" "1" \
    "$(curl -s --max-time 5 -X POST "$CHAT_BASE/react/api/chat" -H 'Content-Type: application/json' -d '{"message":"   "}' | grep -c 'empty message')"
check "chat: SSE content type" "1" \
    "$(curl -s -D - -o /dev/null --max-time 5 -X POST "$CHAT_BASE/react/api/chat" -H 'Content-Type: application/json' -d '{"message":"hi"}' | grep -ci '^content-type: text/event-stream')"
kill "$CHAT_PID" 2>/dev/null
wait "$CHAT_PID" 2>/dev/null

# Same endpoint against a fake OpenAI-compatible upstream
# (scripts/fake-llm-upstream.py): exercises the fork-curl streaming path —
# UTF-8 round-trip, a single >16KB SSE line, "error":null tolerance (must
# NOT abort), error objects, and a final line without newline (EOF tail) —
# all offline and deterministic. Env prefixes override the demo-engine
# export above for this instance only.
if command -v python3 >/dev/null 2>&1; then
    FAKE_PORT=3117
    python3 scripts/fake-llm-upstream.py "$FAKE_PORT" > /tmp/fake-llm-smoke.log 2>&1 &
    FAKE_PID=$!
    # the python server needs its own readiness probe — the C instance's
    # static probe says nothing about the upstream being accept()ed yet
    fake_ready=0
    i=0
    while [ "$i" -lt 30 ]; do
        curl -s -o /dev/null --max-time 1 -X POST \
            "http://localhost:$FAKE_PORT/v1/chat/completions" \
            -d '{"messages":[]}' && fake_ready=1 && break
        i=$((i + 1))
        sleep 0.2
    done
    check "fake LLM upstream up" "1" "$fake_ready"
    CHAT2_PORT=3118
    CHAT2_BASE="http://localhost:$CHAT2_PORT"
    LLM_API_KEY=fakekey LLM_API_URL="http://localhost:$FAKE_PORT/v1" \
        LLM_MODEL=fake-1 LLM_TIMEOUT=15 \
        "$SERVER" -p "$CHAT2_PORT" > /tmp/agent-httpd-chat2.log 2>&1 &
    CHAT2_PID=$!
    chat2_ready=0
    i=0
    while [ "$i" -lt 30 ]; do
        curl -s -o /dev/null --max-time 1 "$CHAT2_BASE/" && chat2_ready=1 && break
        i=$((i + 1))
        sleep 0.2
    done
    check "chat upstream instance up" "1" "$chat2_ready"
    join_deltas() {
        printf '%s' "$1" | tr -d '\n' | sed 's/data: {"t":"delta","d":"//g; s/"}//g'
    }
    u1=$(curl -s --max-time 10 -X POST "$CHAT2_BASE/react/api/chat" \
        -H 'Content-Type: application/json' -d '{"message":"你好"}')
    check "chat upstream: UTF-8 round-trip" "1" \
        "$(printf '%s' "$(join_deltas "$u1")" | grep -c '你好，世界')"
    u2=$(curl -s --max-time 10 -X POST "$CHAT2_BASE/react/api/chat" \
        -H 'Content-Type: application/json' -d '{"message":"long"}' | wc -c | tr -d ' ')
    [ "$u2" -ge 20000 ] && u2=1 || u2=0
    check "chat upstream: 20KB single SSE line intact" "1" "$u2"
    u3=$(curl -s --max-time 10 -X POST "$CHAT2_BASE/react/api/chat" \
        -H 'Content-Type: application/json' -d '{"message":"errnull"}')
    u3d=$(printf '%s' "$u3" | grep -c '"t":"delta"')
    u3n=$(printf '%s' "$u3" | grep -c '"t":"done"')
    [ "$u3d" -eq 2 ] && [ "$u3n" -eq 1 ] && u3=1 || u3=0
    check "chat upstream: error:null tolerated, stream continues" "1" "$u3"
    u4=$(curl -s --max-time 10 -X POST "$CHAT2_BASE/react/api/chat" \
        -H 'Content-Type: application/json' -d '{"message":"fail"}')
    check "chat upstream: error object -> error event" "1" \
        "$(printf '%s' "$u4" | grep -c 'quota exhausted')"
    u5=$(curl -s --max-time 10 -X POST "$CHAT2_BASE/react/api/chat" \
        -H 'Content-Type: application/json' -d '{"message":"tail"}')
    check "chat upstream: EOF tail without newline still counted" "1" \
        "$(printf '%s' "$(join_deltas "$u5")" | grep -c 'last words')"
    kill "$CHAT2_PID" "$FAKE_PID" 2>/dev/null
    wait "$CHAT2_PID" "$FAKE_PID" 2>/dev/null
else
    echo "SKIP: chat upstream cases (python3 missing)"
fi

# Tool Call / Skills / Memory(session) / MCP / PSE through the same fake
# upstream on fresh instances (the first fake died with CHAT2 above).
if command -v python3 >/dev/null 2>&1; then
    FAKE2_PORT=3299
    python3 scripts/fake-llm-upstream.py "$FAKE2_PORT" > /tmp/fake2-smoke.log 2>&1 &
    FAKE2_PID=$!
    fake2_ready=0
    i=0
    while [ "$i" -lt 30 ]; do
        curl -s -o /dev/null --max-time 1 -X POST \
            "http://localhost:$FAKE2_PORT/v1/chat/completions" \
            -d '{"messages":[]}' && fake2_ready=1 && break
        i=$((i + 1))
        sleep 0.2
    done
    check "fake2 LLM upstream up" "1" "$fake2_ready"

    # Tool Call: ReAct loop via calc (message "tool" -> a calc tool call,
    # second round answers "got: 42").
    CHAT3_PORT=3119
    LLM_API_KEY=fakekey LLM_API_URL="http://localhost:$FAKE2_PORT/v1" \
        LLM_MODEL=fake-1 LLM_TIMEOUT=15 \
        "$SERVER" -p "$CHAT3_PORT" > /tmp/agent-httpd-chat3.log 2>&1 &
    CHAT3_PID=$!
    chat3_ready=0
    i=0
    while [ "$i" -lt 30 ]; do
        curl -s -o /dev/null --max-time 1 "http://localhost:$CHAT3_PORT/" \
            && chat3_ready=1 && break
        i=$((i + 1))
        sleep 0.2
    done
    check "chat tool instance up" "1" "$chat3_ready"
    u6=$(curl -s --max-time 20 -X POST "http://localhost:$CHAT3_PORT/react/api/chat" \
        -H 'Content-Type: application/json' -d '{"message":"tool demo plz"}')
    u6t=$(printf '%s' "$u6" | grep -c '"t":"done"')
    u6c=$(printf '%s' "$u6" | tr -d '\n' | grep -c 'got: 42')
    [ "$u6t" -eq 1 ] && [ "$u6c" -eq 1 ] && u6=1 || u6=0
    check "tool call: calc runs and result streams (got: 42)" "1" "$u6"
    kill "$CHAT3_PID" 2>/dev/null
    wait "$CHAT3_PID" 2>/dev/null

    # Skills: dedicated skills dir with a demo SKILL.md; the model runs
    # skill-run and the next round relays the body ("DEMOSKILL_BODY").
    SKILL_DIR=/tmp/agent-httpd-smoke-skills
    rm -rf "$SKILL_DIR" && mkdir -p "$SKILL_DIR/demo"
    printf -- '---\nname: demo\ndescription: Demo skill for smoke tests\n---\nDEMOSKILL_BODY step-by-step instructions\n' \
        > "$SKILL_DIR/demo/SKILL.md"
    CHAT4_PORT=3121
    HARNESS_SKILLS_DIR="$SKILL_DIR" LLM_API_KEY=fakekey \
        LLM_API_URL="http://localhost:$FAKE2_PORT/v1" LLM_MODEL=fake-1 \
        LLM_TIMEOUT=15 "$SERVER" -p "$CHAT4_PORT" \
        > /tmp/agent-httpd-chat4.log 2>&1 &
    CHAT4_PID=$!
    chat4_ready=0
    i=0
    while [ "$i" -lt 30 ]; do
        curl -s -o /dev/null --max-time 1 "http://localhost:$CHAT4_PORT/" \
            && chat4_ready=1 && break
        i=$((i + 1))
        sleep 0.2
    done
    check "chat skills instance up" "1" "$chat4_ready"
    u7=$(curl -s --max-time 20 -X POST "http://localhost:$CHAT4_PORT/react/api/chat" \
        -H 'Content-Type: application/json' -d '{"message":"run the skill"}')
    u7c=$(printf '%s' "$u7" | tr -d '\n' | grep -c 'DEMOSKILL_BODY')
    [ "$u7c" -eq 1 ] && u7=1 || u7=0
    check "skill-run: SKILL.md body reaches the loop" "1" "$u7"
    kill "$CHAT4_PID" 2>/dev/null
    wait "$CHAT4_PID" 2>/dev/null

    # Memory (sessionId): remember writes .data/sessions/<id>.json; a later
    # request on the same sessionId replays the transcript and recall reads
    # the stored fact back ("blue").
    rm -f .data/sessions/s1.json
    CHAT5_PORT=3122
    LLM_API_KEY=fakekey LLM_API_URL="http://localhost:$FAKE2_PORT/v1" \
        LLM_MODEL=fake-1 LLM_TIMEOUT=15 "$SERVER" -p "$CHAT5_PORT" \
        > /tmp/agent-httpd-chat5.log 2>&1 &
    CHAT5_PID=$!
    chat5_ready=0
    i=0
    while [ "$i" -lt 30 ]; do
        curl -s -o /dev/null --max-time 1 "http://localhost:$CHAT5_PORT/" \
            && chat5_ready=1 && break
        i=$((i + 1))
        sleep 0.2
    done
    check "chat memory instance up" "1" "$chat5_ready"
    u8=$(curl -s --max-time 20 -X POST "http://localhost:$CHAT5_PORT/react/api/chat" \
        -H 'Content-Type: application/json' \
        -d '{"message":"rememberit","sessionId":"s1"}')
    # remember returned "memo ..." — the fact value must land on disk
    rec_file=0
    [ -f .data/sessions/s1.json ] && [ "$(grep -c '"blue"' .data/sessions/s1.json)" -ge 1 ] \
        && rec_file=1
    u9=$(curl -s --max-time 20 -X POST "http://localhost:$CHAT5_PORT/react/api/chat" \
        -H 'Content-Type: application/json' \
        -d '{"message":"recallit","sessionId":"s1"}')
    u9c=$(printf '%s' "$u9" | tr -d '\n' | grep -c 'got: color = blue')
    [ "$rec_file" -eq 1 ] && [ "$u9c" -eq 1 ] && u9=1 || u9=0
    check "memory: remember persists, recall reads back (got: color = blue)" "1" "$u9"
    kill "$CHAT5_PID" 2>/dev/null
    wait "$CHAT5_PID" 2>/dev/null

    # MCP: stdio server (scripts/fake-mcp-server.py) registers echo__pong;
    # message "mcp" routes a tools/call through it and streams "echo: hey".
    CHAT6_PORT=3123
    MCP_SERVERS='[{"id":"echo","transport":"stdio","command":"python3","args":"scripts/fake-mcp-server.py","approval":false}]' \
        LLM_API_KEY=fakekey LLM_API_URL="http://localhost:$FAKE2_PORT/v1" \
        LLM_MODEL=fake-1 LLM_TIMEOUT=30 "$SERVER" -p "$CHAT6_PORT" \
        > /tmp/agent-httpd-chat6.log 2>&1 &
    CHAT6_PID=$!
    chat6_ready=0
    i=0
    while [ "$i" -lt 30 ]; do
        curl -s -o /dev/null --max-time 1 "http://localhost:$CHAT6_PORT/" \
            && chat6_ready=1 && break
        i=$((i + 1))
        sleep 0.2
    done
    check "chat MCP instance up" "1" "$chat6_ready"
    mcp_registered=0
    grep -q "\[mcp\] tool: echo__pong" /tmp/agent-httpd-chat6.log && mcp_registered=1
    u10=$(curl -s --max-time 30 -X POST "http://localhost:$CHAT6_PORT/react/api/chat" \
        -H 'Content-Type: application/json' -d '{"message":"mcp call"}')
    u10c=$(printf '%s' "$u10" | tr -d '\n' | grep -c 'got: echo: hey')
    [ "$mcp_registered" -eq 1 ] && [ "$u10c" -eq 1 ] && u10=1 || u10=0
    check "MCP: tool registered + tools/call streams 'echo: hey'" "1" "$u10"
    kill "$CHAT6_PID" 2>/dev/null
    wait "$CHAT6_PID" 2>/dev/null

    # PSE orchestrator: planner streams PLAN:..., specialist runs the calc
    # loop ("got: 42"), evaluator yields PASS (no retry cycle).
    CHAT7_PORT=3124
    PSE_ENABLED=true LLM_API_KEY=fakekey \
        LLM_API_URL="http://localhost:$FAKE2_PORT/v1" LLM_MODEL=fake-1 \
        LLM_TIMEOUT=30 "$SERVER" -p "$CHAT7_PORT" \
        > /tmp/agent-httpd-chat7.log 2>&1 &
    CHAT7_PID=$!
    chat7_ready=0
    i=0
    while [ "$i" -lt 30 ]; do
        curl -s -o /dev/null --max-time 1 "http://localhost:$CHAT7_PORT/" \
            && chat7_ready=1 && break
        i=$((i + 1))
        sleep 0.2
    done
    check "chat PSE instance up" "1" "$chat7_ready"
    u11=$(curl -s --max-time 40 -X POST "http://localhost:$CHAT7_PORT/react/api/chat" \
        -H 'Content-Type: application/json' -d '{"message":"pse"}')
    plan_seen=0; spec_seen=0; verdict_seen=0; no_retry=0
    printf '%s' "$u11" | grep -q 'PLAN: ' && plan_seen=1
    printf '%s' "$u11" | grep -q 'PSE cycle 1/3 - Specialist' && spec_seen=1
    printf '%s' "$u11" | grep -q 'got: 42' && verdict_seen=1
    rc=$(printf '%s' "$u11" | grep -c 'PSE cycle 1/3 - Planner' || true)
    if [ "$plan_seen" -eq 1 ] && [ "$spec_seen" -eq 1 ] \
        && [ "$verdict_seen" -eq 1 ] && [ "$rc" -eq 1 ]; then u11=1; else u11=0; fi
    check "PSE: planner+specialist+evaluator(PASS) one cycle" "1" "$u11"

    # PSE retry: the fake evaluator rejects the first cycle (specialist's
    # calc result "41", rejects with a PSE_FEEDBACK marker), the second
    # planner sees the feedback and plans the V2 calc ("21*2" -> "42"),
    # then the evaluator passes. Proves the feedback injection loop.
    u12=$(curl -s --max-time 40 -X POST "http://localhost:$CHAT7_PORT/react/api/chat" \
        -H 'Content-Type: application/json' -d '{"message":"pse-retry"}')
    r_plan1=0; r_plan2=0; r_notpass=0; r_41=0; r_42=0; r_exh_ok=1
    printf '%s' "$u12" | grep -q 'PSE cycle 1/3 - Planner' && r_plan1=1
    printf '%s' "$u12" | grep -q 'PSE cycle 2/3 - Planner' && r_plan2=1
    rc=$(printf '%s' "$u12" | grep -c 'not PASS, retrying' || true)
    [ "$rc" -eq 1 ] && r_notpass=1
    printf '%s' "$u12" | grep -q 'got: 41' && r_41=1
    printf '%s' "$u12" | grep -q 'got: 42' && r_42=1
    if [ "$r_plan1" -eq 1 ] && [ "$r_plan2" -eq 1 ] && [ "$r_notpass" -eq 1 ] \
        && [ "$r_41" -eq 1 ] && [ "$r_42" -eq 1 ]; then
        printf '%s' "$u12" | grep -q 'PSE exhausted attempts' && r_exh_ok=0
    fi
    if [ "$r_plan1" -eq 1 ] && [ "$r_plan2" -eq 1 ] && [ "$r_notpass" -eq 1 ] \
        && [ "$r_41" -eq 1 ] && [ "$r_42" -eq 1 ] && [ "$r_exh_ok" -eq 1 ]; then u12=1; else u12=0; fi
    check "PSE: non-PASS retries with feedback then passes (2 cycles)" "1" "$u12"

    # PSE exhaustion: the planner never emits PLAN-V2 (planner keeps the
    # same plan even after feedback), specialist always yields "41",
    # evaluator always fails -> 3 cycles then "PSE exhausted attempts".
    u13=$(curl -s --max-time 60 -X POST "http://localhost:$CHAT7_PORT/react/api/chat" \
        -H 'Content-Type: application/json' -d '{"message":"pse-fail"}')
    e_1=0; e_2=0; e_3=0; e_exh=0; e_three=0
    printf '%s' "$u13" | grep -q 'PSE cycle 1/3 - Planner' && e_1=1
    printf '%s' "$u13" | grep -q 'PSE cycle 2/3 - Planner' && e_2=1
    printf '%s' "$u13" | grep -q 'PSE cycle 3/3 - Planner' && e_3=1
    printf '%s' "$u13" | grep -q 'PSE exhausted attempts' && e_exh=1
    rc=$(printf '%s' "$u13" | grep -c 'not PASS, retrying' || true)
    [ "$rc" -eq 3 ] && e_three=1
    if [ "$e_1" -eq 1 ] && [ "$e_2" -eq 1 ] && [ "$e_3" -eq 1 ] \
        && [ "$e_exh" -eq 1 ] && [ "$e_three" -eq 1 ]; then u13=1; else u13=0; fi
    check "PSE: all-FAIL exhausts 3 attempts" "1" "$u13"

    kill "$CHAT7_PID" 2>/dev/null
    wait "$CHAT7_PID" 2>/dev/null

    kill "$FAKE2_PID" 2>/dev/null
    wait "$FAKE2_PID" 2>/dev/null
    rm -rf "$SKILL_DIR"
    rm -f .data/sessions/s1.json
fi

# Per-IP rate limiting (-l <rps>): dedicated instance, fixed-window quota.
# The probe/burst share one window, so success counts use <= -l (a burst
# crossing a 1s boundary can see a fresh quota, but never exceeds it).
RATE_PORT=3111
RATE_BASE="http://localhost:$RATE_PORT"
"$SERVER" -p "$RATE_PORT" -l 3 > /tmp/agent-httpd-rate.log 2>&1 &
RATE_PID=$!
rate_ready=0
i=0
while [ "$i" -lt 30 ]; do
    curl -s -o /dev/null --max-time 1 "$RATE_BASE/" && rate_ready=1 && break
    i=$((i + 1))
    sleep 0.2
done
check "rate-limit instance up" "1" "$rate_ready"
# Align to just after a wall-clock second boundary: the fixed window keys
# on time(NULL), so a burst fired right after a boundary lands entirely in
# one window. Without this the 8 sequential curls can straddle a boundary,
# see a fresh quota and flake the counts below (5+ 200s at limit 3).
rate_sec=$(date +%S)
while [ "$(date +%S)" = "$rate_sec" ]; do :; done
rate_headers="/tmp/agent-httpd-rate-headers.txt"
: > "$rate_headers"
i=0
while [ "$i" -lt 8 ]; do
    curl -s -D - -o /dev/null --max-time 5 "$RATE_BASE/" >> "$rate_headers" 2>/dev/null
    i=$((i + 1))
done
rate_200=$(grep -c '^HTTP/1.1 200' "$rate_headers")
rate_429=$(grep -c '^HTTP/1.1 429' "$rate_headers")
check "rate-limit: burst respects quota (200s=$rate_200 429s=$rate_429 of 8, limit 3)" "1" \
    "$([ "$rate_200" -le 3 ] && [ "$rate_429" -ge 4 ] && echo 1 || echo 0)"
check "rate-limit: every 429 carries Retry-After: 1" "$rate_429" \
    "$(grep -ci '^retry-after: 1' "$rate_headers")"
check "rate-limit: every 429 forces Connection: close" "$rate_429" \
    "$(grep -ci '^connection: close' "$rate_headers")"
kill "$RATE_PID" 2>/dev/null
wait "$RATE_PID" 2>/dev/null
rm -f "$rate_headers"

# Pool mode: the counter table lives in shared memory, so parallel workers
# must jointly enforce one per-IP quota (not one quota each).
RATE_PORT=3112
RATE_BASE="http://localhost:$RATE_PORT"
"$SERVER" -p "$RATE_PORT" -w 4 -l 5 > /tmp/agent-httpd-rate-pool.log 2>&1 &
RATE_PID=$!
rate_ready=0
i=0
while [ "$i" -lt 30 ]; do
    curl -s -o /dev/null --max-time 1 "$RATE_BASE/" && rate_ready=1 && break
    i=$((i + 1))
    sleep 0.2
done
seq 1 20 | xargs -P 20 -I{} curl -s -o /dev/null --max-time 5 -w '%{http_code}\n' "$RATE_BASE/" > /tmp/agent-httpd-rate-pool.out
pool_200=$(grep -c '^200$' /tmp/agent-httpd-rate-pool.out)
pool_429=$(grep -c '^429$' /tmp/agent-httpd-rate-pool.out)
# 容差说明: 固定窗口每秒重置, 20 连发可能横跨两个窗口 (每窗各放行 5 个),
# 故 200s 上限取 10 (两窗之和) 而非 5; 判别力不损 —— 若配额不共享
# (4 worker × 5 = 20 个几乎全过), 200s ≤ 10 依然会失败。
check "rate-limit (pool): workers share one quota (200s=$pool_200 429s=$pool_429 of 20, limit 5)" "1" \
    "$([ "$pool_200" -le 10 ] && [ "$pool_429" -ge 5 ] && echo 1 || echo 0)"
kill "$RATE_PID" 2>/dev/null
wait "$RATE_PID" 2>/dev/null
rm -f /tmp/agent-httpd-rate-pool.out

# The limiter is opt-in: the main instance runs without -l and must never 429.
main_429=0
i=0
while [ "$i" -lt 30 ]; do
    c=$(status "$BASE/")
    [ "$c" = "429" ] && main_429=$((main_429 + 1))
    i=$((i + 1))
done
check "rate-limit: off by default (30 rapid requests, no -l)" "0" "$main_429"

for lang in go ruby rust java php; do
    if [ -x "cgi-bin/$lang.cgi" ]; then
        marker=$(lang_get_marker "$lang")
        got=$(curl -s "$BASE/cgi-bin/$lang.cgi?name=t&msg=langtest" \
            | grep -c "<h1>$marker</h1>")
        check "$lang CGI GET ($marker)" "1" "$got"
        got_post=$(curl -s -X POST -d "p_${lang}=langtest" "$BASE/cgi-bin/$lang.cgi" \
            | grep -c "<code>p_${lang}</code>")
        check "$lang CGI POST" "1" "$got_post"
    fi
done

# FastCGI backend (server-side of FCGI, via -F unix socket)
if [ -S "$FCGI_SOCK" ] && command -v python3 >/dev/null 2>&1; then
    fcgi_out=$(python3 scripts/fcgi-test.py "$FCGI_SOCK")
    fcgi_status=$?
    fcgi_pass=$(echo "$fcgi_out" | grep -c "^\[PASS\]")
    echo "$fcgi_out"
    pass=$((pass + fcgi_pass))
    if [ "$fcgi_status" -ne 0 ]; then
        fail=$((fail + $(echo "$fcgi_out" | grep -c "^\[FAIL\]")))
    fi
else
    echo "SKIP: FastCGI tests (socket or python3 missing)"
fi

# Resident React FastCGI backend + agent-httpd -R relay (-R as FCGI client)
if [ -x "bin/react-ssr-server" ] && command -v node >/dev/null 2>&1; then
    REACT_SOCK="/tmp/agent-httpd-react-test.sock"
    pkill -9 -f 'react-ssr-server' 2>/dev/null
    rm -f "$REACT_SOCK"
    sleep 0.3
    "$PWD/bin/react-ssr-server" "$REACT_SOCK" > "/tmp/react-fcgi-test.log" 2>&1 &
    react_pid=$!
    # The resident backend takes a moment to bind its socket; the FCGI and
    # relay checks below must not race it (a cold disk can push startup past
    # the fixed 0.5s the relay used to wait for — that raced into 502s).
    i=0
    while [ ! -S "$REACT_SOCK" ] && [ "$i" -lt 50 ]; do
        sleep 0.1
        i=$((i + 1))
    done

    # direct FCGI to the resident backend (bypasses the relay); the backend
    # renders the app for any path, so the 404-expectation subtest is
    # structurally unfulfillable there — 4 PASS / 1 FAIL is the baseline.
    if [ -S "$REACT_SOCK" ] && command -v python3 >/dev/null 2>&1; then
        rcgi=$(python3 scripts/fcgi-test.py "$REACT_SOCK" 2>/dev/null | grep -c "^\[PASS\]")
        check "resident backend responds via FCGI" "4" "$rcgi"
    fi

    # relay: HTTP /react/ -> -R -> resident backend
    # Other instances append to logs/access.log too, so remember where the
    # log stood and look at what THIS case appended.
    RELAY_LOG_BASE=$(wc -l < logs/access.log | tr -d ' ')
    relay_status=$(status "$BASE/react/?name=Resident")
    check "HTTP /react/ relay (GET)" "200" "$relay_status"
    relay_token=$(curl -s "$BASE/react/?name=Resident" | grep -c "Hello, Resident")
    check "relayed SSR renders Hello, Resident" "1" "$relay_token"

    # -R relay must log real body byte counts (it once logged a hardcoded 0
    # for every relayed response). Read the lines this case appended, not the
    # file's last line: several instances share this log, so "tail -1" used
    # to report whatever request happened to land last, passing or failing
    # for reasons that had nothing to do with the relay.
    # Match on the PATH only: log_request() in src/http/http_log.c strips the
    # query string on purpose (URLs carry tokens/session ids/PII that must not
    # be persisted), so the line reads "GET /react/ HTTP/1.1", never
    # "...?name=Resident". This case only runs when bin/react-ssr-server
    # exists, so the mismatch stayed hidden while that binary was missing.
    relay_line=$(tail -n +"$((RELAY_LOG_BASE + 1))" logs/access.log \
        | grep -E '"GET /react/ ' | tail -1)
    relay_bytes=$(printf '%s' "$relay_line" | sed -E 's/.*" ([0-9]{3}) ([0-9]+) .*/\2/')
    relay_bytes_ok=0
    [ -n "$relay_bytes" ] && [ "$relay_bytes" -gt 0 ] && relay_bytes_ok=1
    check "relay logs nonzero body bytes ($relay_bytes)" "1" "$relay_bytes_ok"
    relay_post=$(curl -s -X POST -d "name=SVCPOST" "$BASE/react/" | grep -c "Hello, SVCPOST")
    check "relayed SSR POST" "1" "$relay_post"
    relay_about=$(curl -s "$BASE/react/about" | grep -c "How the routing works")
    check "relayed SSR deep-link /react/about" "1" "$relay_about"
    relay_404=$(curl -s "$BASE/react/nope" | grep -c "Nothing is routed")
    check "relayed SSR /react/nope NotFound" "1" "$relay_404"

    # /react/api/chat must be intercepted by the native C endpoint even
    # with -R active (the C demo reply names itself; the node one doesn't)
    relay_chat=$(curl -s --max-time 15 -X POST "$BASE/react/api/chat" \
        -H 'Content-Type: application/json' -d '{"message":"hello"}')
    relay_chat_join=$(printf '%s' "$relay_chat" | tr -d '\n' | sed 's/data: {"t":"delta","d":"//g; s/"}//g')
    check "chat bypasses -R relay (served by C endpoint)" "1" \
        "$(printf '%s' "$relay_chat_join" | grep -c 'C demo engine')"

    # CSR mode: server must ship an EMPTY shell (no React markup), the
    # client bundle then mounts via createRoot instead of hydrateRoot.
    csr_empty=$(curl -s "$BASE/react/?mode=csr" | grep -c '<main')
    check "relayed CSR shell has no server markup" "0" "$csr_empty"
    csr_plugin=$(curl -s "$BASE/react/?mode=csr&name=CSR" | grep -c '<div id="root"></div>')
    check "relayed CSR ships empty root" "1" "$csr_plugin"

    # backend killed -> agent-httpd must answer 502, not hang
    pkill -9 -f 'react-ssr-server' 2>/dev/null
    sleep 0.3
    relay_down=$(status "$BASE/react/")
    check "relay 502 when backend down" "502" "$relay_down"
    kill "$react_pid" 2>/dev/null
else
    echo "SKIP: resident React relay (react-ssr-server or node missing)"
fi

# HMR dev server (scripts/dev-server.js, Vite middleware mode). Guarded on
# node + vite being installed; runs its own instance on a separate port.
if command -v node >/dev/null 2>&1 && [ -d "cgi-bin/react-ssr/node_modules/vite" ]; then
    HMR_PORT=3107
    pkill -f 'scripts/dev-server.js' 2>/dev/null
    sleep 0.3
    (PORT=$HMR_PORT node scripts/dev-server.js > /tmp/hmr-dev-test.log 2>&1 &)
    hmr_ready=0
    i=0
    while [ "$i" -lt 40 ]; do
        curl -s -o /dev/null --max-time 1 "http://localhost:$HMR_PORT/health" && hmr_ready=1 && break
        i=$((i + 1))
        sleep 0.25
    done
    check "HMR dev server comes up (health)" "1" "$hmr_ready"
    # Keep the response: when this flakes, the bytes are the only way to tell a
    # slow first SSR compile from a broken render (see /tmp/hmr-ssr-probe.html).
    curl -s --max-time 30 "http://localhost:$HMR_PORT/react/?name=Smoke" \
        -o /tmp/hmr-ssr-probe.html
    hmr_ssr=$(grep -c 'Hello, Smoke' /tmp/hmr-ssr-probe.html)
    check "HMR dev server SSR renders" "1" "$hmr_ssr"
    hmr_pre=$(curl -s --max-time 30 "http://localhost:$HMR_PORT/react/" | grep -c '@react-refresh')
    check "HMR page injects react-refresh preamble" "1" "$hmr_pre"
    # The client entry's transformed code mentions hydrateRoot once per
    # occurrence in source (import destructure + call); vite 7 emits both,
    # so assert presence, not an exact count.
    if curl -s --max-time 30 "http://localhost:$HMR_PORT/react/react-ssr.tsx" | grep -q 'hydrateRoot'; then
        hmr_entry=1
    else
        hmr_entry=0
    fi
    check "HMR dev client entry served (transformRequest)" "1" "$hmr_entry"
    # Chat API must stream through the dev server too. Express matches in
    # registration order: if the chat POST route ever ends up AFTER the
    # "/react" page catch-all, this request renders a page instead of the
    # SSE stream (0 delta events) and this case fails. Test env has no
    # LLM key, so the demo engine answers — deterministic deltas.
    hmr_chat=$(curl -s --max-time 30 -X POST "http://localhost:$HMR_PORT/react/api/chat" \
        -H 'Content-Type: application/json' -d '{"message":"Smoke"}' | grep -c '"t":"delta"')
    [ "$hmr_chat" -gt 0 ] && hmr_chat=1 || hmr_chat=0
    check "HMR dev chat SSE streams (not shadowed by page route)" "1" "$hmr_chat"
    # Watcher pipeline: touching a server-side module must schedule a full
    # reload (source edit pickup without restart was verified manually).
    touch cgi-bin/react-ssr/App.tsx
    sleep 1.5
    hmr_watch=$(grep -c 'full reload: App.tsx changed' /tmp/hmr-dev-test.log)
    check "HMR watcher reacts to App.tsx change" "1" "$hmr_watch"
    # Non-/react paths proxy to a spawned real agent-httpd (bin/agent-httpd):
    # CGI must work exactly like production, not 404.
    if [ -x "bin/agent-httpd" ]; then
        hmr_cgi=$(curl -s --max-time 15 "http://localhost:$HMR_PORT/cgi-bin/hello.cgi" | grep -c 'Hello from CGI!')
        check "HMR dev proxies CGI to agent-httpd backend" "1" "$hmr_cgi"
        hmr_backend_up=0
        pgrep -f "agent-httpd -p $HMR_PORT" >/dev/null && hmr_backend_up=1
        check "HMR dev spawned its agent-httpd backend" "1" "$hmr_backend_up"
    else
        echo "SKIP: HMR CGI proxy (bin/agent-httpd not built)"
    fi
    pkill -f 'scripts/dev-server.js' 2>/dev/null
    pkill -f "agent-httpd -p $HMR_PORT" 2>/dev/null
else
    echo "SKIP: HMR dev server (node or vite missing)"
fi

kill "$PID" 2>/dev/null
wait "$PID" 2>/dev/null
pkill -9 -f cgi-bin 2>/dev/null

echo "----------------------------------------"
echo "PASS=$pass FAIL=$fail"
[ "$fail" -eq 0 ]