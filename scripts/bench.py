#!/usr/bin/env python3
"""Mini load benchmark for AgentHTTPD.

Compares two client strategies against a running server:

  keep-alive : N requests over a small pool of persistent HTTP/1.1
               connections (what browsers/curl do)
  per-conn   : N requests, each on a fresh TCP connection
               (Connection: close semantics)

Usage:
    python3 scripts/bench.py [port] [requests] [concurrency]

Defaults: port 18080, 2000 requests, 8 concurrent workers.
The server must already be running (e.g. `make start` or `./bin/agent-httpd -p 18080`).
Splits the work evenly across workers, runs each mode once, and prints
requests/sec plus latency percentiles (p50/p90/p99 in ms).

Responses answered 429 by a per-IP rate limiter (`-l <rps>`) are counted and
reported in the `429s` column instead of crashing the run; latency
percentiles are computed over successful (200) responses only, so the
numbers stay meaningful for normal clients. req/s counts every completed
exchange, admitted or shed.
"""

import socket
import sys
import time
from concurrent.futures import ThreadPoolExecutor

HOST = "127.0.0.1"
PATH = "/"  # small static page: isolates connection cost from file size


def read_response(s):
    """Read one full HTTP response (headers + Content-Length body).

    Returns the numeric status code. 429 is a measured outcome (rate limiter
    engaged); any other non-200 still raises.
    """
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = s.recv(65536)
        if not chunk:
            raise ConnectionError("closed while reading headers")
        data += chunk
    head, rest = data.split(b"\r\n\r\n", 1)
    clen = 0
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            clen = int(line.split(b":", 1)[1])
    while len(rest) < clen:
        chunk = s.recv(65536)
        if not chunk:
            raise ConnectionError("closed while reading body")
        rest += chunk
    status_line = head.split(b"\r\n", 1)[0]
    try:
        status = int(status_line.split()[1])
    except (IndexError, ValueError):
        raise RuntimeError(f"unparseable status line: {status_line!r}")
    if status not in (200, 429):
        raise RuntimeError(f"unexpected status: {status_line!r}")
    return status, rest[clen:]  # rest[clen:] = leftover bytes (next response)


def worker(mode, port, nreq, results, wid):
    """Run nreq requests in `mode`; append per-request latency (s) to results.

    Reconnects transparently when the server closes a keep-alive connection
    (e.g. after MAX_KEEPALIVE_REQUESTS), like real HTTP clients do.
    """
    lat = []
    rejected = 0
    attempts = 0
    s = None
    done = 0
    retries = 0
    while done < nreq:
        try:
            attempts += 1
            t0 = time.perf_counter()
            if mode == "per-conn" or s is None:
                if s:
                    s.close()
                    s = None
                s = socket.create_connection((HOST, port), timeout=10)
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            req = b"GET " + PATH.encode() + b" HTTP/1.1\r\nHost: bench\r\n"
            if mode == "per-conn":
                req += b"Connection: close\r\n"
            req += b"\r\n"
            s.sendall(req)
            status, _ = read_response(s)
            if status == 429:
                # Limiter shed this request and closed the connection; count
                # it, drop it from latency stats, reconnect for the next one.
                rejected += 1
                s.close()
                s = None
                continue
            lat.append(time.perf_counter() - t0)
            done += 1
        except (ConnectionError, OSError) as exc:
            retries += 1
            if retries > nreq * 2:
                raise RuntimeError(f"too many reconnects: {exc}") from exc
            if s:
                s.close()
                s = None
    if s:
        s.close()
    results[wid] = (lat, rejected, attempts)


def run_mode(mode, port, total, conc):
    per_worker = total // conc
    counts = [per_worker] * conc
    counts[0] += total - per_worker  # distribute the remainder
    results = [None] * conc
    t0 = time.perf_counter()
    with ThreadPoolExecutor(max_workers=conc) as pool:
        futures = []
        for w in range(conc):
            futures.append(pool.submit(worker, mode, port, counts[w], results, w))
        for f in futures:
            f.result()
    elapsed = time.perf_counter() - t0
    all_lat = sorted(x for r, _rej, _att in results for x in r)
    rejected = sum(rej for _r, rej, _att in results)
    attempts = sum(att for _r, _rej, att in results)
    return elapsed, all_lat, rejected, attempts


def percentile(sorted_lat, p):
    if not sorted_lat:
        return 0.0
    idx = min(int(len(sorted_lat) * p / 100), len(sorted_lat) - 1)
    return sorted_lat[idx] * 1000.0  # ms


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
    total = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
    conc = int(sys.argv[3]) if len(sys.argv) > 3 else 8

    # sanity check: server up?
    try:
        s = socket.create_connection((HOST, port), timeout=2)
        s.close()
    except OSError:
        print(f"error: no server on {HOST}:{port} (start one with `make start`)", file=sys.stderr)
        return 1
    if total < conc:
        conc = total

    # per-conn mode burns one ephemeral port per request; warn if the run
    # might exhaust the local port range (macOS default ~27k wide)
    if total > 5000:
        print(f"note: {total} per-conn requests may exhaust ephemeral ports; "
              f"consider widening the range, e.g. on macOS:\n"
              f"  sudo sysctl -w net.inet.ip.portrange.first=1024 "
              f"net.inet.ip.portrange.last=65535")

    print(f"bench: {total} requests x GET {PATH}, concurrency={conc}, host={HOST}:{port}")
    print(f"{'mode':<10} {'req/s':>10} {'p50 ms':>8} {'p90 ms':>8} {'p99 ms':>8} {'429s':>6}")
    summary = {}
    for mode in ("keep-alive", "per-conn"):
        elapsed, lat, rejected, attempts = run_mode(mode, port, total, conc)
        rps = total / elapsed
        summary[mode] = (rps, rejected)
        print(f"{mode:<10} {rps:>10.0f} {percentile(lat, 50):>8.2f} "
              f"{percentile(lat, 90):>8.2f} {percentile(lat, 99):>8.2f} {rejected:>6}")
        if rejected:
            print(f"  note: limiter shed {rejected} of {attempts} attempts "
                  f"({100 * rejected / attempts:.1f}%); 429s counts every 429 "
                  f"response including retries - the run ends at {total} successes")
    if summary["per-conn"][0] > 0:
        print(f"\nkeep-alive speedup: {summary['keep-alive'][0] / summary['per-conn'][0]:.1f}x")
    return 0


if __name__ == "__main__":
    sys.exit(main())
