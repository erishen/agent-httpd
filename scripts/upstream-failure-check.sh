#!/bin/sh
# Guard the upstream-failure path of the native chat endpoint (src/agent/agent.c).
#
# The server ignores SIGCHLD (src/core/main.c), so the kernel reaps the curl child
# the instant it exits and waitpid() returns ECHILD with the status buffer left
# untouched. Reading that zeroed buffer back as "exited 0" turned every *fast*
# upstream failure — connection refused, bad host, immediate reset — into a
# clean, empty answer: no error event, no retry, nothing in the log. Timeouts
# masked it, because that branch is decided before the exit status is consulted.
#
# So: run a throwaway container with no network at all and a deliberately
# unreachable LLM_API_URL, then assert the client is actually told something.
# Needs agent-httpd:latest; does NOT need the compose stack, and never touches
# a running one (own container name, --network none).
#
# Usage: make test-upstream   (or: sh scripts/upstream-failure-check.sh)

set -e
IMG="${IMG:-agent-httpd:latest}"
NAME=agent-httpd-upstream-check

if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    echo "error: image $IMG not found. Build it first: docker build -t $IMG ." >&2
    exit 1
fi

cleanup() { docker rm -f "$NAME" >/dev/null 2>&1 || true; }
trap cleanup EXIT INT TERM
cleanup

# A non-empty key selects the real model path instead of the built-in demo
# engine (which never calls upstream). The values go nowhere: no network here.
docker run -d --name "$NAME" --network none \
    -e PORT=8080 -e WORKERS=1 -e RATE_LIMIT=0 -e LLM_TIMEOUT=6 \
    -e LLM_API_KEY=upstream-check -e LLM_MODEL=upstream-check \
    -e LLM_API_URL=http://127.0.0.1:9/v1 \
    "$IMG" >/dev/null

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

echo "POST /react/api/chat with an unreachable upstream..."
sse=$(docker exec -i "$NAME" python3 -c '
import json, urllib.request
req = urllib.request.Request(
    "http://127.0.0.1:8080/react/api/chat",
    data=json.dumps({"message": "ping"}).encode(),
    headers={"Content-Type": "application/json"})
print(urllib.request.urlopen(req, timeout=90).read().decode())
')
echo "$sse" | sed 's/^/    /'

if echo "$sse" | grep -q '"t":"error"'; then
    echo "PASS: the upstream failure reaches the client as an error event"
else
    echo "FAIL: the upstream failure was reported as a successful empty answer" >&2
    echo "      (expected an error event; none was sent)" >&2
    exit 1
fi
