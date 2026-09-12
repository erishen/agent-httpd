#!/bin/sh
# Guard the large-response streaming path: a body bigger than the socket send
# buffer must arrive whole, even when the reader pauses.
#
# The event loop accepts with O_NONBLOCK (it needs the flag for epoll/kqueue)
# and hands the very same socket to a pool worker over SCM_RIGHTS. The worker's
# send loops assume blocking I/O, so a body that overflowed the send buffer used
# to fail with EAGAIN mid-stream, and the response was abandoned under a
# Content-Length that had already promised every byte. Through nginx that reads
# as "upstream prematurely closed connection"; in a browser it means
# /js/react-ssr.js never parses, so the React page silently never hydrates and
# the two entry points (:18080 direct httpd, :18081 nginx) end up looking
# different.
#
# Why nothing else caught it: every other probe reaches the server through the
# *published port*, i.e. through docker-proxy, whose userland copy drains the
# socket continuously - the buffer never fills, so EAGAIN never happens. (A
# loopback client inside the same container is no good either: the loopback
# buffers are big enough to swallow the whole body.) So this guard stands up a
# private bridge network, serves a 300 KB file from one container and reads it
# from a *second* container over the veth pair, deliberately reading nothing for
# a moment first.
#
# Needs agent-httpd:latest only; it does not depend on, or disturb, a running
# compose stack (own container names, own network).
#
# Usage: make test-stream   (or: sh scripts/stream-truncation-check.sh)

set -e
IMG="${IMG:-agent-httpd:latest}"
NAME=agent-httpd-stream-check
NET=agent-httpd-stream-net
PROBE=agent-httpd-stream-probe
SIZE_KB="${SIZE_KB:-300}"
DOCROOT=/app/www
FILE="$DOCROOT/big-stream.bin"

if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    echo "error: image $IMG not found. Build it first: docker build -t $IMG ." >&2
    exit 1
fi

# Idempotent, and thorough enough to recover from an earlier hard kill: a stray
# probe container left attached to the network would make `docker network rm`
# fail, and the next run would then die on `docker network create`.
cleanup() {
    docker rm -f "$NAME" "$PROBE" >/dev/null 2>&1 || true
    for c in $(docker network inspect -f '{{range .Containers}}{{.Name}} {{end}}' \
        "$NET" 2>/dev/null); do
        docker rm -f "$c" >/dev/null 2>&1 || true
    done
    docker network rm "$NET" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM
cleanup

docker network create "$NET" >/dev/null
# The image entrypoint always launches the server (only "react-backend" is
# special-cased), so both roles run the same image: the server normally, the
# probe with the entrypoint bypassed.
docker run -d --name "$NAME" --network "$NET" \
    -e PORT=8080 -e WORKERS=4 -e RATE_LIMIT=0 "$IMG" >/dev/null

echo "waiting for the server to listen..."
i=0
up=0
while [ "$i" -lt 45 ]; do
    if docker exec -i "$NAME" sh -c \
        'curl -s -o /dev/null -m 1 http://127.0.0.1:8080/health' 2>/dev/null; then
        up=1
        break
    fi
    i=$((i + 1))
    sleep 1
done
if [ "$up" -ne 1 ]; then
    echo "FAIL: server did not listen within 45s" >&2
    docker logs --tail 20 "$NAME" >&2
    exit 1
fi

# Random bytes, so a stale copy or a cached entry cannot pass by accident. The
# size is the point: it cannot fit in the socket send buffer, and that overflow
# is what raises EAGAIN on the non-blocking socket.
docker exec -i "$NAME" sh -c "dd if=/dev/urandom of='$FILE' bs=1024 count=$SIZE_KB 2>/dev/null"
echo "serving $((SIZE_KB * 1024)) bytes from $FILE to a second container over the bridge..."

set +e
out=$(docker run --rm --name "$PROBE" --entrypoint python3 --network "$NET" \
    -e TARGET="$NAME" "$IMG" -c '
import os, socket, time

host = os.environ["TARGET"]
s = socket.create_connection((host, 8080), timeout=30)
s.sendall(b"GET /big-stream.bin HTTP/1.1\r\nHost: " + host.encode() +
          b"\r\nConnection: close\r\n\r\n")

# Header block first: it carries the Content-Length we have to honour.
head = b""
while b"\r\n\r\n" not in head:
    d = s.recv(4096)
    if not d:
        print("FAIL: the connection closed before the headers arrived")
        raise SystemExit(1)
    head += d
hdr, _, rest = head.partition(b"\r\n\r\n")
decl = None
for line in hdr.split(b"\r\n"):
    if line.lower().startswith(b"content-length:"):
        decl = int(line.split(b":", 1)[1].strip())
body = len(rest)

time.sleep(1.0)   # <- the pause that exposes the EAGAIN path

s.settimeout(30)
while True:
    d = s.recv(65536)
    if not d:
        break
    body += len(d)
s.close()

print("    declared=%s received=%s" % (decl, body))
if decl is None:
    print("FAIL: the response carried no Content-Length")
    raise SystemExit(1)
if body != decl:
    print("FAIL: truncated body - %d bytes promised, %d delivered" % (decl, body))
    raise SystemExit(1)
print("PASS: the whole body arrived over the bridge")
')
rc=$?
set -e
echo "$out"
if [ "$rc" -ne 0 ]; then
    echo "      nginx would log 'upstream prematurely closed connection' for this," >&2
    echo "      and a browser would fail to parse the truncated asset (no hydration)." >&2
    exit 1
fi
