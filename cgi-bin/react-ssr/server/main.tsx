// Resident FastCGI server for React SSR. Bundled to bin/react-ssr-server.
//
// Unlike server.tsx (which cold-starts a node process + reloads react per
// request), this process loads the app ONCE and then serves an endless
// stream of FCGI requests over a UNIX socket — the FastCGI model that
// removes render cold-start entirely, same role as PHP-FPM/puma/unicorn.
//
// It implements just enough of the FCGI server-side protocol to work with
// nginx fastcgi_pass and agent-httpd -R forwarding:
//   read   BEGIN_REQUEST | PARAMS* | STDIN(empty)
//   write  STDOUT(HTTP response) | END_REQUEST

import net from "net";
import http from "http";
import fs from "fs";
import path from "path";
import { isRenderMode, parseQuery, renderStream, type RenderMode } from "./render";
import { isChatRoute, streamChat, HEAD_FCgi, type ChatSink } from "./chat";

// --- ISR-style render cache ---
// GET documents are cached in memory by (mode, path, query) for
// RENDER_CACHE_TTL_MS. Cache hits skip React entirely (just frame I/O);
// misses render once and are both streamed to the client AND cached, so one
// render serves both. POST/DELETE-style requests never cache.
const RENDER_CACHE_TTL_MS = Number(process.env.REACT_RENDER_TTL_MS || 60_000);
const RENDER_CACHE_MAX = 200;

interface CacheEntry {
  html: string;
  at: number;
}

const renderCache = new Map<string, CacheEntry>();

function cacheKey(pathname: string, mode: string, params: Params): string {
  const q = Object.keys(params)
    .sort()
    .map((k) => k + "=" + params[k])
    .join("&");
  return mode + "|" + pathname + "|" + q;
}

function cacheGet(key: string): string | null {
  const hit = renderCache.get(key);
  if (!hit) return null;
  if (Date.now() - hit.at > RENDER_CACHE_TTL_MS) {
    renderCache.delete(key);
    return null;
  }
  return hit.html;
}

function cacheSet(key: string, html: string): void {
  if (renderCache.size >= RENDER_CACHE_MAX) {
    // evict oldest by insertion order (Map preserves it)
    const oldest = renderCache.keys().next().value;
    if (oldest !== undefined) renderCache.delete(oldest);
  }
  renderCache.set(key, { html, at: Date.now() });
}

// --- FCGI wire constants (spec, Dec 1996) ---
const VERSION = 1;
const BEGIN_REQUEST = 1;
const END_REQUEST = 3;
const PARAMS = 4;
const STDIN = 5;
const STDOUT = 6;
const REQUEST_COMPLETE = 0;

type Params = Record<string, string>;

function sendFrame(sock: net.Socket, type: number, reqId: number, payload: Buffer | string): void {
  const body = Buffer.isBuffer(payload) ? payload : Buffer.from(payload);
  const padLen = (8 - (body.length % 8)) % 8;
  const hdr = Buffer.alloc(8);
  hdr[0] = VERSION;
  hdr[1] = type;
  hdr.writeUInt16BE(reqId, 2);
  hdr.writeUInt16BE(body.length, 4);
  hdr[6] = padLen;
  sock.write(hdr);
  if (body.length) sock.write(body);
  if (padLen) sock.write(Buffer.alloc(padLen));
}

interface Frame {
  type: number;
  reqId: number;
  content: Buffer;
}

/* Incremental FCGI frame reader. Accumulates raw bytes and yields parsed
 * frames via a callback. Returns an error string on malformed input. */
class FcgParser {
  private pending: Buffer[] = [];
  private pendingLen = 0;
  private hdr: { type: number; reqId: number; clen: number; plen: number; content: Buffer | null } | null = null;

  constructor(private readonly onRecord: (frame: Frame) => void) {}

  push(chunk: Buffer): string | undefined {
    this.pending.push(chunk);
    this.pendingLen += chunk.length;
    return this._drain();
  }

  private _take(n: number): Buffer | null {
    const buf = Buffer.alloc(n);
    let got = 0;
    while (got < n) {
      const part = this.pending[0];
      if (!part) return null; // not enough buffered yet
      const take = Math.min(part.length, n - got);
      part.copy(buf, got, 0, take);
      got += take;
      if (take === part.length) this.pending.shift();
      else this.pending[0] = part.subarray(take);
    }
    this.pendingLen -= n;
    return buf;
  }

  private _drain(): string | undefined {
    while (true) {
      if (!this.hdr) {
        const h = this._take(8);
        if (!h) return;
        this.hdr = {
          type: h[1],
          reqId: h.readUInt16BE(2),
          clen: h.readUInt16BE(4),
          plen: h[6],
          content: null,
        };
      }
      if (!this.hdr.content) {
        if (this.pendingLen < this.hdr.clen) return;
        this.hdr.content = this._take(this.hdr.clen) as Buffer;
      }
      if (this.pendingLen < this.hdr.plen) return;
      if (this.hdr.plen > 0) this._take(this.hdr.plen);
      const frame: Frame = {
        type: this.hdr.type,
        reqId: this.hdr.reqId,
        content: this.hdr.content,
      };
      this.hdr = null;
      if (frame.type === END_REQUEST) return; // peer closed the exchange
      this.onRecord(frame);
    }
  }
}

/* Parse PARAMS content; multiple PARAMS frames accumulate into `params`.
 * Wire order per the FCGI spec is nameLen valueLen name value — both
 * lengths come FIRST, back to back. (An earlier version read nameLen
 * name valueLen value, which happens to round-trip against our own C
 * client but silently drops every param nginx sends.) */
function parseParamsFrame(content: Buffer, params: Params): void {
  const len = content.length;
  let off = 0;
  while (off < len) {
    let nameLen: number;
    if ((content[off] & 0x80) === 0) nameLen = content[off++];
    else {
      nameLen = ((content[off] & 0x7f) << 24) | content[off + 1] << 16 | content[off + 2] << 8 | content[off + 3];
      off += 4;
    }
    let valueLen: number;
    if ((content[off] & 0x80) === 0) valueLen = content[off++];
    else {
      valueLen = ((content[off] & 0x7f) << 24) | content[off + 1] << 16 | content[off + 2] << 8 | content[off + 3];
      off += 4;
    }
    if (off + nameLen + valueLen > len) break;
    const name = content.subarray(off, off + nameLen).toString();
    off += nameLen;
    params[name] = content.subarray(off, off + valueLen).toString();
    off += valueLen;
  }
}

/* Reassemble one request from the raw frames, render, and reply with an
 * HTTP response wrapped in STDOUT frames (possibly several, streaming the
 * slow parts) + END_REQUEST. */
async function handleRequest(sock: net.Socket, reqId: number, params: Params, body: Buffer): Promise<void> {
  const method = (params.REQUEST_METHOD || "GET").toUpperCase();
  const isPost = method === "POST";
  if (method !== "GET" && method !== "POST") {
    const err =
      "HTTP/1.1 405 Method Not Allowed\r\n" +
      docHeaders() +
      "Content-Length: 0\r\nConnection: close\r\n\r\n";
    sendFrame(sock, STDOUT, reqId, err);
    sendRequestEnd(sock, reqId);
    return;
  }
  // REQUEST_URI carries the path (router) and optional query; nginx also
  // splits QUERY_STRING for us.
  let query = params.QUERY_STRING || "";
  let pathname = "/react";
  const uri = params.REQUEST_URI || "";
  const qi = uri.indexOf("?");
  if (qi >= 0) {
    pathname = uri.slice(0, qi) || "/react";
    if (!query) query = uri.slice(qi + 1);
  } else if (uri) {
    pathname = uri;
  }
  const input = isPost ? body.toString("utf8") : query;
  console.error("[REQ]", method, "uri=", params.REQUEST_URI, "pathname=", pathname, "isPost=", isPost, "q=", query.slice(0, 30));

  // Chat API: SSE streaming via the shared chat module. Runs before the
  // page pipeline — /react/api/chat is a data endpoint, not a page.
  if (isChatRoute(pathname, method)) {
    const fcgiSink: ChatSink = {
      write: (chunk) => sendStreamChunk(sock, reqId, chunk),
      end: () => {}, // END_REQUEST is sent by handleRequest below
    };
    try {
      sendStreamChunk(sock, reqId, HEAD_FCgi);
      await streamChat(input, fcgiSink);
    } catch (err) {
      console.error("[DEBUG] chat failed:", err);
      // Mid-stream failure: close cleanly so the client sees a truncated
      // stream rather than a hang; the done marker may already be out.
    }
    sendRequestEnd(sock, reqId);
    return;
  }

  const parsed = parseQuery(input);
  // ?mode=csr|ssr picks the render path; strip it before the params table.
  const mode = isRenderMode(parsed.mode) ? parsed.mode : "ssr";
  delete parsed.mode;

  try {
    if (!isPost) {
      const key = cacheKey(pathname, mode, parsed);
      const cached = cacheGet(key);
      if (cached !== null) {
        // Cache hit: complete buffered response (headers + body in one go).
        sendHttpResponse(sock, reqId, cached);
      } else {
        // Miss: stream head first (fast TTFB), then page sections as React
        // resolves them; the resolved full string becomes the cache entry.
        sendHeadOnly(sock, reqId);
        const html = await renderStream(parsed, "GET", pathname, mode, (chunk) => {
          sendStreamChunk(sock, reqId, chunk);
        });
        cacheSet(key, html);
      }
    } else {
      // POST: streaming + cache off.
      sendHeadOnly(sock, reqId);
      await renderStream(parsed, "POST", pathname, mode, (chunk) => {
        sendStreamChunk(sock, reqId, chunk);
      });
    }
  } catch (err) {
    console.error("[DEBUG] render failed:", new Date().toISOString(), err);
    const resp = Buffer.from(
      "HTTP/1.1 500 Internal Server Error\r\n" +
        docHeaders() +
        "Content-Length: 21\r\nConnection: close\r\n\r\n" +
        "React SSR: render error",
      "utf8"
    );
    sendFrame(sock, STDOUT, reqId, resp);
  }
  // END_REQUEST must fire exactly once for every path — the C relay waits
  // for it and without it the connection hangs until timeout → 502.
  sendRequestEnd(sock, reqId);
}

// Headers every document response carries. The C server streams these bytes
// to the client verbatim ("-R" relay) or nginx does (fastcgi_pass), so
// anything missing here is missing end to end - this process is the origin.
//
// Date: RFC 9110 6.6.1 requires it on every response. The C paths get it
// from build_response; nothing adds it back on this one.
//
// Cache-Control: the document embeds per-request state (serverTime, the
// rendered view) on top of this process's own 60s ISR entry, so the client
// must store it only to revalidate. Silence is not neutral: with no policy
// the browser invents a heuristic freshness window from Last-Modified and
// serves a stale page - which a freshly built bundle then fails to hydrate.
//
// PURGE_CLIENT_CACHE="1": one-shot eviction for caches that were already
// poisoned before the policy above existed. A response header cannot
// retract a copy a browser stored earlier, but Clear-Site-Data makes it
// drop the origin's HTTP cache, after which the next fetch is fresh. Keep
// it enabled only until every client has visited once: it re-evicts on
// every visit while on.
const PURGE_CLIENT_CACHE = process.env.PURGE_CLIENT_CACHE === "1";

function docHeaders(): string {
  let out = "Date: " + new Date().toUTCString() + "\r\n";
  out += "Cache-Control: no-store\r\n";
  out += "X-Content-Type-Options: nosniff\r\n";
  out += "X-Frame-Options: DENY\r\n";
  out += "Referrer-Policy: no-referrer\r\n";
  if (PURGE_CLIENT_CACHE) out += 'Clear-Site-Data: "cache"\r\n';
  return out;
}

// Buffered response (cache hit / non-streaming path): carries Content-Length.
function sendHttpResponse(sock: net.Socket, reqId: number, html: string): void {
  const head =
    "HTTP/1.1 200 OK\r\n" +
    "Content-Type: text/html; charset=utf-8\r\n" +
    "Content-Length: " + Buffer.byteLength(html) + "\r\n" +
    docHeaders() +
    "Connection: close\r\n\r\n";
  sendStreamChunk(sock, reqId, head);
  sendStreamChunk(sock, reqId, html);
}

// Streaming response: headers FIRST (no Content-Length — body streams until
// END_REQUEST/close), then each rendered section as it arrives.
function sendHeadOnly(sock: net.Socket, reqId: number): void {
  sendStreamChunk(
    sock,
    reqId,
    "HTTP/1.1 200 OK\r\n" +
      "Content-Type: text/html; charset=utf-8\r\n" +
      docHeaders() +
      "Connection: close\r\n\r\n"
  );
}

/* STDOUT frame content length is 16-bit: split oversized payloads. */
function sendStreamChunk(sock: net.Socket, reqId: number, text: string): void {
  const buf = Buffer.from(text, "utf8");
  for (let off = 0; off < buf.length; off += 0xffff) {
    sendFrame(sock, STDOUT, reqId, buf.subarray(off, off + 0xffff));
  }
}

function sendRequestEnd(sock: net.Socket, reqId: number): void {
  const end = Buffer.alloc(8);
  end.writeUInt32BE(0, 0); // appStatus
  end[4] = REQUEST_COMPLETE; // protocolStatus
  sendFrame(sock, END_REQUEST, reqId, end);
}

function createServer(sockPath: string): net.Server {
  try {
    fs.unlinkSync(sockPath);
  } catch {}
  fs.mkdirSync(path.dirname(sockPath), { recursive: true });

  const server = net.createServer((conn) => {
    const state: { reqId: number; params: Params; body: Buffer; done: boolean } = {
      reqId: 0,
      params: {},
      body: Buffer.alloc(0),
      done: false,
    };
    const parser = new FcgParser((frame) => {
      if (state.done) return;
      if (frame.type === BEGIN_REQUEST) {
        state.reqId = frame.reqId;
      } else if (frame.type === PARAMS) {
        if (frame.content.length > 0) parseParamsFrame(frame.content, state.params);
      } else if (frame.type === STDIN) {
        if (frame.content.length > 0) {
          state.body = Buffer.concat([state.body, frame.content]);
        } else {
          // empty STDIN terminates the request; render and reply.
          // handleRequest is async (streaming) — must await before
          // closing the socket, or chunks get truncated.
          state.done = true;
          void (async () => {
            await handleRequest(conn, state.reqId, state.params, state.body);
            conn.end();
          })();
        }
      }
    });
    conn.on("data", (chunk: Buffer) => parser.push(chunk));
    conn.on("error", () => conn.destroy());
  });

  server.listen(sockPath, () => {
    // The FCGI socket bypasses the C server's auth/rate-limit when used
    // directly, so default to owner-only (a local process must not be able
    // to render/probe the backend unchecked). Multi-user front proxies
    // (e.g. an nginx worker under a different account) can widen it via
    // REACT_FCGI_SOCK_MODE=0777.
    const sockMode = parseInt(process.env.REACT_FCGI_SOCK_MODE || "0700", 8);
    fs.chmodSync(sockPath, sockMode);
    console.log("React FastCGI server resident on " + sockPath);
    console.log("React " + require("react").version + " loaded once; serving forever.");
  });
  return server;
}

const sockPath = process.argv[2] || process.env.REACT_FCGI_SOCK || "/tmp/react-ssr.sock";
const server = createServer(sockPath);

// --- HTTP transport (production, unified -v wiring) -----------------------
// The same render core + ISR cache behind a plain HTTP listener so the C
// server can proxy /react/* pages to it via -v (identical to the Vite dev
// path). Only /react/* pages are served; the C process itself answers
// /react/api/chat and everything else.
const httpPort = Number(process.env.REACT_HTTP_PORT || process.argv[3] || 0);
if (httpPort > 0) {
  const httpSrv = http.createServer(async (req, res) => {
    // Match the C server's security headers so proxied responses carry the
    // same set as direct ones (the -v relay streams these verbatim). The
    // http module supplies Date on its own here.
    res.setHeader("Cache-Control", "no-store");
    res.setHeader("X-Content-Type-Options", "nosniff");
    res.setHeader("X-Frame-Options", "DENY");
    res.setHeader("Referrer-Policy", "no-referrer");
    res.setHeader("Server", "AgentHTTPD");
    if (PURGE_CLIENT_CACHE) res.setHeader("Clear-Site-Data", '"cache"');
    try {
      const url = req.url || "/react";
      const qi = url.indexOf("?");
      const pathname = (qi >= 0 ? url.slice(0, qi) : url).replace(/\/+$/, "") || "/react";
      const query = qi >= 0 ? url.slice(qi + 1) : "";
      const params = parseQuery(query);
      const mode: RenderMode = params.mode === "csr" ? "csr" : "ssr";
      delete params.mode;
      const isPost = (req.method || "GET").toUpperCase() === "POST";

      if (!isPost) {
        const key = cacheKey(pathname, mode, params);
        const cached = cacheGet(key);
        if (cached !== null) {
          res.writeHead(200, {
            "Content-Type": "text/html; charset=utf-8",
            "Content-Length": Buffer.byteLength(cached),
          });
          res.end(cached);
          return;
        }
        res.writeHead(200, {
          "Content-Type": "text/html; charset=utf-8",
          "Transfer-Encoding": "chunked",
        });
        const html = await renderStream(params, "GET", pathname, mode, (c) => res.write(c));
        cacheSet(key, html);
        res.end();
      } else {
        res.writeHead(200, {
          "Content-Type": "text/html; charset=utf-8",
          "Transfer-Encoding": "chunked",
        });
        await renderStream(params, "POST", pathname, mode, (c) => res.write(c));
        res.end();
      }
    } catch (err) {
      console.error("[DEBUG] HTTP render failed:", new Date().toISOString(), err);
      if (!res.headersSent) {
        res.writeHead(500, { "Content-Type": "text/plain" });
        res.end("React SSR: render error");
      } else {
        res.destroy();
      }
    }
  });
  httpSrv.listen(httpPort, "127.0.0.1", () => {
    console.log("React SSR HTTP on 127.0.0.1:" + httpPort + " (for agent-httpd -v relay)");
  });
  process.on("SIGINT", () => {
    httpSrv.close();
    server.close(() => {
      try {
        fs.unlinkSync(sockPath);
      } catch {}
      process.exit(0);
    });
  });
} else {
  process.on("SIGINT", () => {
    server.close(() => {
      try {
        fs.unlinkSync(sockPath);
      } catch {}
      process.exit(0);
    });
  });
}