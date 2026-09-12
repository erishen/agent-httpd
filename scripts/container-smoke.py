#!/usr/bin/env python3
"""Protocol-semantics probe for a *running* agent-httpd container.

Run it with `make test-container`, or by hand:

    docker cp scripts/container-smoke.py agent-httpd:/tmp/ && \
    docker exec -i agent-httpd python3 /tmp/container-smoke.py

scripts/smoke-test.sh spawns its own server from the repo tree, so it never
touches the image. This probe instead talks to the instance the container is
already serving, which makes it the only check that covers the shipped
artifact (compiled binary + docroot + CGI zoo) end to end. It needs to live
inside the container because the CGI cases drop temporary scripts in
/app/cgi-bin.

Covers the protocol half of the smoke suite: routing, method handling,
request smuggling, connection reuse, content negotiation, ranges, and the
64KB streaming ceiling.
"""
import gzip
import http.client
import os
import socket
import sys

HOST = "127.0.0.1"
PORT = int(os.environ.get("PORT", "8080"))
CBIN = "/app/cgi-bin"
BASE = "http://%s:%d" % (HOST, PORT)

pass_n = 0
fail_n = 0


def check(desc, expected, actual):
    global pass_n, fail_n
    ok = expected == actual
    if ok:
        pass_n += 1
        print("PASS: %s" % desc)
    else:
        fail_n += 1
        print("FAIL: %s  (expected %r, got %r)" % (desc, expected, actual))


def req(path, method="GET", headers=None, body=None, timeout=20):
    c = http.client.HTTPConnection(HOST, PORT, timeout=timeout)
    try:
        c.request(method, path, body=body, headers=headers or {})
        r = c.getresponse()
        return r.status, {k.lower(): v for k, v in r.getheaders()}, r.read()
    finally:
        c.close()


def raw(payload, timeout=10):
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    try:
        s.sendall(payload)
        chunks = []
        while True:
            b = s.recv(65536)
            if not b:
                break
            chunks.append(b)
        return b"".join(chunks)
    finally:
        s.close()


print("=== A. static / routing ===")
st, _, _ = req("/")
check("static /", 200, st)
st, h, b = req("/", method="HEAD")
check("HEAD / status", 200, st)
check("HEAD / no body", 0, len(b))
check("HEAD / keeps Content-Length", True, int(h.get("content-length", "0")) > 0)

st, h, _ = req("/test")
check("trailing-slash redirect", 301, st)
check("redirect Location", "/test/", h.get("location", "").replace(BASE, ""))

check("directory listing", 200, req("/test/")[0])
st, _, b = req("/definitely-missing")
check("404 status", 404, st)
check("404 carries a body", True, len(b) > 0)

check("path traversal blocked (/../etc/passwd)", True, req("/../etc/passwd")[0] != 200)
check(
    "encoded traversal blocked (%2e%2e)",
    True,
    req("/%2e%2e/%2e%2e/etc/passwd")[0] != 200,
)
check("dotfile in cgi-bin not served", True, req("/cgi-bin/.hidden.cgi")[0] != 200)

print("\n=== B. methods ===")
check("GET CGI", 200, req("/cgi-bin/hello.cgi")[0])
check(
    "POST to CGI",
    200,
    req(
        "/cgi-bin/hello.cgi",
        method="POST",
        headers={"Content-Type": "application/x-www-form-urlencoded"},
        body=b"a=1",
    )[0],
)
st, h, _ = req("/", method="PUT")
check("PUT to static: 405", 405, st)
check("405 carries Allow", True, "allow" in h)
st, h, _ = req("/", method="OPTIONS")
check("OPTIONS static advertises Allow", True, "allow" in h)
check("unknown method: 501", 501, req("/", method="FROBNICATE")[0])

print("\n=== C. request smuggling ===")
smug = raw(
    b"POST /cgi-bin/hello.cgi HTTP/1.1\r\nHost: x\r\n"
    b"Content-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"
)
check("TE+CL rejected: 400", True, b" 400 " in smug.split(b"\r\n")[0])

print("\n=== D. connection handling ===")
c = http.client.HTTPConnection(HOST, PORT, timeout=20)
c.request("GET", "/")
c.getresponse().read()
sock_a = c.sock
c.request("GET", "/")
c.getresponse().read()
check("keep-alive reuses the connection", True, c.sock is sock_a)
c.close()

cl = raw(b"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
check("Connection: close honoured (EOF)", True, b" 200 " in cl.split(b"\r\n")[0])
h10 = raw(b"GET / HTTP/1.0\r\n\r\n")
check("HTTP/1.0 answered", True, b" 200 " in h10.split(b"\r\n")[0])

# Two requests in ONE write. The server must answer both without waiting for
# more bytes: a carried-over pipelined request is already complete, so
# blocking on recv() before parsing would park it until the keep-alive
# timeout (the client sees one response and a hang).
pipe = raw(
    b"GET /__pipeline_probe__ HTTP/1.1\r\nHost: x\r\n\r\n"
    b"GET /__pipeline_probe__ HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
)
check("pipelined requests both answered", 2, pipe.count(b"HTTP/1.1 "))

print("\n=== E. negotiation / caching / ranges (/js/react-ssr.js) ===")
st, h, raw_body = req("/js/react-ssr.js")
check("bundle served", 200, st)
size = len(raw_body)
check("ETag present", True, "etag" in h)
check("Last-Modified present", True, "last-modified" in h)
check("Cache-Control forbids silent staleness", "no-cache", h.get("cache-control"))

st, h, gz = req("/js/react-ssr.js", headers={"Accept-Encoding": "gzip"})
check("gzip negotiated", "gzip", h.get("content-encoding"))
check("gzip body inflates to the raw size", size, len(gzip.decompress(gz)))
check("gzip twin carries the same cache policy", "no-cache", h.get("cache-control"))

st, h, plain = req("/js/react-ssr.js", headers={"Accept-Encoding": "gzip;q=0"})
check("gzip;q=0 stays uncompressed", None, h.get("content-encoding"))
check("uncompressed body matches", size, len(plain))
# A length check cannot tell a stale twin from a fresh one, so compare bytes:
# the gzip side is the only one browsers ever run, so an image that assembled
# it from a different revision than the identity bundle would otherwise pass.
check("gzip twin inflates to the identity bytes", plain, gzip.decompress(gz))

st, h, part = req("/js/react-ssr.js", headers={"Range": "bytes=0-99"})
check("206 partial content", 206, st)
check("Content-Range is exact", "bytes 0-99/%d" % size, h.get("content-range"))
check("206 body is 100 bytes", 100, len(part))
check(
    "416 for out-of-bounds range",
    416,
    req("/js/react-ssr.js", headers={"Range": "bytes=99999999-"})[0],
)

etag = req("/js/react-ssr.js")[1].get("etag")
st, _, b = req("/js/react-ssr.js", headers={"If-None-Match": etag})
check("If-None-Match -> 304", 304, st)
check("304 has no body", 0, len(b))

print("\n=== F. CGI protocol details ===")
tmp = []
try:
    big = os.path.join(CBIN, ".probe-big.cgi")
    with open(big, "w") as f:
        f.write(
            "#!/bin/sh\necho 'Content-Type: text/plain'\necho\n"
            "exec head -c 700000 /dev/zero | tr '\\0' 'y'\n"
        )
    os.chmod(big, 0o755)
    tmp.append(big)
    check(
        "CGI output 700000 complete (no 64KB truncation)",
        700000,
        len(req("/cgi-bin/.probe-big.cgi")[2]),
    )

    stc = os.path.join(CBIN, ".probe-status.cgi")
    with open(stc, "w") as f:
        f.write(
            "#!/bin/sh\necho 'Status: 418 I am a teapot'\n"
            "echo 'Content-Type: text/plain'\necho\necho 'short and stout'\n"
        )
    os.chmod(stc, 0o755)
    tmp.append(stc)
    st, _, b = req("/cgi-bin/.probe-status.cgi")
    check("CGI Status header honoured (418)", 418, st)
    check("418 response has a body", True, len(b) > 0)

    loc = os.path.join(CBIN, ".probe-loc.cgi")
    with open(loc, "w") as f:
        f.write(
            "#!/bin/sh\necho 'Location: %s/test/'\n"
            "echo 'Content-Type: text/plain'\necho\necho 'moved'\n" % BASE
        )
    os.chmod(loc, 0o755)
    tmp.append(loc)
    st, h, b = req("/cgi-bin/.probe-loc.cgi")
    check("CGI Location -> 302", 302, st)
    check("Location value preserved", "%s/test/" % BASE, h.get("location"))
    check("redirect still carries the body", True, len(b) > 0)

    _, _, xss = req("/cgi-bin/form.cgi?name=%3Cscript%3Ealert(1)%3C/script%3E")
    check("form.cgi escapes input (no raw <script>)", True, b"<script>alert" not in xss)
    check("form.cgi shows the escaped tag", True, b"&lt;script&gt;" in xss)

    # A POST whose body never reaches the handler still answers 200, so the
    # status-only check above cannot catch a dropped body: assert the CGI
    # actually received the bytes. Read stdin into a variable first — `wc -c`
    # drains stdin, so counting before `cat` would echo nothing.
    echo_cgi = os.path.join(CBIN, ".probe-echo.cgi")
    with open(echo_cgi, "w") as f:
        f.write(
            "#!/bin/sh\n"
            "echo 'Content-Type: text/plain'\n"
            "echo\n"
            "body=$(cat)\n"
            "echo \"STDIN_BYTES=$(printf '%s' \"$body\" | wc -c | tr -d ' ')\"\n"
            "printf '%s' \"$body\"\n"
        )
    os.chmod(echo_cgi, 0o755)
    tmp.append(echo_cgi)
    payload = b"name=Alice&x=1"
    _, _, echoed = req(
        "/cgi-bin/.probe-echo.cgi",
        method="POST",
        headers={"Content-Type": "application/x-www-form-urlencoded"},
        body=payload,
    )
    check(
        "POST body reaches the CGI (%d bytes)" % len(payload),
        True,
        ("STDIN_BYTES=%d" % len(payload)).encode() in echoed,
    )
    check("POST body is intact", True, payload in echoed)

    # The chat endpoint keeps its own body parse; a valid body must not come
    # back as a parse error (the upstream itself may legitimately fail).
    _, _, chat = req(
        "/react/api/chat",
        method="POST",
        headers={"Content-Type": "application/json"},
        body=b'{"message":"ping"}',
    )
    check("chat accepts a valid body", True, b"request body must be JSON" not in chat)
    check("chat answers as SSE", True, chat.startswith(b"data: "))
finally:
    for p in tmp:
        try:
            os.remove(p)
        except OSError:
            pass

print("\n=== summary: %d passed, %d failed ===" % (pass_n, fail_n))
sys.exit(1 if fail_n else 0)
