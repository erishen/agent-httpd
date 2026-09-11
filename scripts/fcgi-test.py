#!/usr/bin/env python3
"""FastCGI client smoke tests for agent-httpd.

Speaks the raw FCGI wire protocol over a UNIX socket (like nginx's
fastcgi_pass would), sends BEGIN_REQUEST/PARAMS/STDIN frames and validates
the STDOUT frames contain a well-formed HTTP response.
"""
import socket
import sys
import os

FCGI_VERSION = 1
BEGIN_REQUEST = 1
END_REQUEST = 3
PARAMS = 4
STDIN = 5
STDOUT = 6
RESPONDER = 1

def recv_n(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            break
        buf += chunk
    return buf

def read_records(s):
    out = b""
    end = None
    while True:
        hdr = recv_n(s, 8)
        if len(hdr) < 8:
            break
        _v, rtype, reqid, clen, plen, _r = (
            hdr[0], hdr[1], (hdr[2] << 8) | hdr[3],
            (hdr[4] << 8) | hdr[5], hdr[6], hdr[7],
        )
        content = recv_n(s, clen)
        if plen:
            recv_n(s, plen)
        if rtype == STDOUT:
            out += content
        elif rtype == END_REQUEST:
            end = content
            break
    return out, end

def send_record(s, rtype, content, reqid=1):
    clen = len(content)
    plen = (8 - (clen % 8)) % 8
    hdr = bytes([FCGI_VERSION, rtype, reqid >> 8, reqid & 0xFF,
                 clen >> 8, clen & 0xFF, plen, 0])
    s.sendall(hdr + content + b"\x00" * plen)

def sname(name, value):
    """Encode one PARAMS name/value pair.

    Wire order per the FCGI spec: name_len, value_len, name, value — both
    lengths come FIRST, back to back. (agent-httpd's own C encoder used to
    emit name_len name value_len value, which only round-tripped against
    its own equally-wrong parser; fixed to the standard order.)"""
    n = name.encode()
    v = value.encode()
    def enc(x):
        if len(x) < 128:
            return bytes([len(x)])
        return len(x).to_bytes(4, "big")
    return enc(n) + enc(v) + n + v

def run_request(sock_path, params, body=b""):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(20)
    s.connect(sock_path)
    send_record(s, BEGIN_REQUEST, (RESPONDER).to_bytes(2, "big") + b"\x00" * 6)
    send_record(s, PARAMS, b"".join(sname(k, v) for k, v in params.items()))
    send_record(s, PARAMS, b"")
    if body:
        send_record(s, STDIN, body)
    send_record(s, STDIN, b"")
    out, end = read_records(s)
    s.close()
    return out, end

BASE_PARAMS = {
    "GATEWAY_INTERFACE": "CGI/1.1",
    "SERVER_PROTOCOL": "HTTP/1.1",
    "SERVER_SOFTWARE": "nginx",
    "SERVER_NAME": "localhost",
    "SERVER_PORT": "80",
    "REMOTE_ADDR": "127.0.0.1",
    "HTTPS": "",
}

def expect(sock_path, name, params, want_status, want_token=None, method="GET", body=b""):
    params = dict(BASE_PARAMS, **params)
    params["REQUEST_METHOD"] = method
    out, end = read_records_hook(sock_path, params, body)
    status_line = out.split(b"\r\n", 1)[0].decode("latin1")
    has_status = status_line.startswith(f"HTTP/1.1 {want_status} ")
    token_ok = (want_token is None) or (want_token.encode() in out)
    if has_status and token_ok:
        print(f"[PASS] {name} ({status_line})")
        return True
    print(f"[FAIL] {name}: status={status_line!r} token_ok={token_ok} end={end!r}")
    return False

def read_records_hook(sock_path, params, body):
    out, end = run_request(sock_path, params, body)
    return out, end

def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <unix-socket-path>")
        return 1
    sock = sys.argv[1]
    if not os.path.exists(sock):
        print(f"[FAIL] socket not found: {sock}")
        return 1

    ok = True
    ok &= expect(sock, "FCGI GET /", {"REQUEST_URI": "/"},
                 "200", want_token="Agent")
    ok &= expect(sock, "FCGI GET hello.cgi", {"REQUEST_URI": "/cgi-bin/hello.cgi"},
                 "200", want_token="Hello")
    ok &= expect(sock, "FCGI GET notfound", {"REQUEST_URI": "/nope.html"},
                 "404")
    ok &= expect(sock, "FCGI POST form.cgi", {"REQUEST_URI": "/cgi-bin/form.cgi"},
                 "200", want_token="World", method="POST",
                 body=b"name=World")
    ok &= expect(sock, "FCGI GET react-ssr.cgi",
                 {"REQUEST_URI": "/cgi-bin/react-ssr.cgi?name=SSR&message=hi"},
                 "200", want_token="SSR")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())