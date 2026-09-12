// Streaming chat backend — /react/api/chat (POST).
//
// Sink-agnostic by design: the FastCGI resident backend passes STDOUT-frame
// writers, the Vite dev server passes HTTP res.write — the protocol logic
// lives here once. Transport is SSE (text/event-stream), emitted as
//   data: {"t":"delta","d":"<text>"}\n\n   incremental tokens
//   data: {"t":"note","d":"<text>"}\n\n   one-off status line
//   data: {"t":"error","d":"<text>"}\n\n   fatal, stream ends after
//   data: {"t":"done"}\n\n                terminal marker
//
// Two engines:
//   1. LLM_API_KEY set -> real OpenAI-compatible /chat/completions with
//      stream:true (works with OpenAI, Azure gateways, Ollama, vLLM, ...).
//   2. No key -> local simulation that paces a canned reply word-by-word so
//      the streaming path, UI and proxy chain are fully exercisable offline.
//      This is a teaching server: the demo must not require credentials.

import type { Socket } from "net";

export interface ChatSink {
  write(chunk: string): void;
  end(): void | Promise<void>;
}

interface ChatMessage {
  role: "user" | "assistant" | "system";
  content: string;
}

const MAX_MESSAGE_CHARS = 2000;
const MAX_HISTORY = 12; // messages (user+assistant pairs) sent upstream

const LLM_API_URL = process.env.LLM_API_URL || "https://api.openai.com/v1/chat/completions";
const LLM_API_KEY = process.env.LLM_API_KEY || "";
const LLM_MODEL = process.env.LLM_MODEL || "gpt-4o-mini";

export function isChatRoute(pathname: string, method: string): boolean {
  return pathname === "/react/api/chat" && (method === "POST" || method === "GET");
}

/* Response head, in the two transports' dialects. streamChat emits BODY
 * (SSE events) only — each caller sends its own head:
 *   - server-main.tsx: HEAD_FCgi is the usual HTTP-in-STDOUT header block
 *   - dev-server.js:   HEAD_FIELDS goes through res.writeHead
 * No Content-Length anywhere: the body streams until done/close. */
export const HEAD_FCgi =
  "HTTP/1.1 200 OK\r\n" +
  "Content-Type: text/event-stream\r\n" +
  "Cache-Control: no-cache\r\n" +
  "Connection: close\r\n\r\n";

export const HEAD_FIELDS = {
  "Content-Type": "text/event-stream",
  "Cache-Control": "no-cache",
  "Connection": "close",
} as const;

export function sseEvent(t: string, d?: string): string {
  return "data: " + JSON.stringify(d === undefined ? { t } : { t, d }) + "\n\n";
}

export async function streamChat(rawBody: string, sink: ChatSink): Promise<void> {
  let message = "";
  let history: ChatMessage[] = [];
  try {
    const parsed = JSON.parse(rawBody) as { message?: unknown; history?: unknown };
    if (typeof parsed.message === "string") message = parsed.message;
    if (Array.isArray(parsed.history)) {
      history = parsed.history
        .filter((m): m is ChatMessage =>
          !!m && typeof m === "object" &&
          (m.role === "user" || m.role === "assistant") &&
          typeof m.content === "string")
        .slice(-MAX_HISTORY);
    }
  } catch {
    sink.write(sseEvent("error", "request body must be JSON: {message, history?}"));
    sink.write(sseEvent("done"));
    await sink.end();
    return;
  }
  message = message.trim().slice(0, MAX_MESSAGE_CHARS);
  if (!message) {
    sink.write(sseEvent("error", "empty message"));
    sink.write(sseEvent("done"));
    await sink.end();
    return;
  }

  try {
    if (LLM_API_KEY) {
      await streamFromLlm(message, history, sink);
    } else {
      await simulateReply(message, sink);
    }
  } catch (err) {
    const detail = err instanceof Error ? err.message : String(err);
    sink.write(sseEvent("error", "upstream failed: " + detail));
  }
  sink.write(sseEvent("done"));
  await sink.end();
}

/* ---- engine 1: OpenAI-compatible streaming upstream -------------------- */

/* Accept both spellings people actually use: a full endpoint
 * ("https://host/v1/chat/completions") or a bare base URL
 * ("https://host/v1" — OpenAI-SDK convention). Normalize to the full
 * endpoint; without this, a base URL POSTs to /v1 and 404s. */
const LLM_ENDPOINT = /\/chat\/completions\/?$/.test(LLM_API_URL)
  ? LLM_API_URL
  : LLM_API_URL.replace(/\/+$/, "") + "/chat/completions";

async function streamFromLlm(message: string, history: ChatMessage[], sink: ChatSink): Promise<void> {
  const upstream = await fetch(LLM_ENDPOINT, {
    method: "POST",
    headers: {
      "Authorization": "Bearer " + LLM_API_KEY,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({
      model: LLM_MODEL,
      stream: true,
      messages: [
        { role: "system", content: "You are the demo assistant embedded in agent-httpd's React SSR teaching app. Answer concisely." },
        ...history,
        { role: "user", content: message },
      ],
    }),
  });
  if (!upstream.ok || !upstream.body) {
    const text = await upstream.text().catch(() => "");
    throw new Error(LLM_ENDPOINT + " -> " + upstream.status + " " + text.slice(0, 200));
  }

  // Re-emit upstream SSE deltas in our envelope. Buffer partial lines: the
  // upstream chunks arrive split at arbitrary byte boundaries.
  const decoder = new TextDecoder();
  let buf = "";
  let emitted = false; // some models open with "\n\n" — trim the leading gap
  for await (const chunk of upstream.body) {
    buf += decoder.decode(chunk as Uint8Array, { stream: true });
    let nl: number;
    while ((nl = buf.indexOf("\n")) >= 0) {
      const line = buf.slice(0, nl).trim();
      buf = buf.slice(nl + 1);
      if (!line.startsWith("data:")) continue;
      const payload = line.slice(5).trim();
      if (payload === "[DONE]") return;
      try {
        const json = JSON.parse(payload) as { choices?: Array<{ delta?: { content?: string } }> };
        let delta = json.choices?.[0]?.delta?.content;
        if (!delta) continue;
        if (!emitted) {
          delta = delta.replace(/^\s+/, ""); // drop the model's leading blank lines
          if (!delta) continue; // still only whitespace — wait for real content
          emitted = true;
        }
        sink.write(sseEvent("delta", delta));
      } catch {
        /* keep-alive comments / partial JSON — ignore */
      }
    }
  }
}

/* ---- engine 2: local simulation (no credentials needed) ---------------- */

/* Illustrative agent-stack notes for the offline demo: the real capabilities
 * live in the C backend (src/agent.c, src/pse.c), whose channel for
 * non-text steps is the same "note" event. Here we emit matching note
 * strings for capability keywords so the page renders its activity chips
 * (TOOL / MCP / SKILL / MEM / PSE) without needing an upstream. Keep the
 * note formats identical to the C agent so pages/Chat.tsx needs no
 * engine-specific parsing. */
function demoActNote(message: string): string | null {
  const m = message.toLowerCase();
  if (/\b(mcp|echo:|pong)\b/.test(m) && !/\bcalc\b/.test(m)) return 'tool echo__pong({"text":"hey"})';
  if (/\b(tool|calc|计算)\b/.test(m)) return 'tool calc({"expression":"21*2"})';
  if (/\b(skill|技能)\b/.test(m)) return 'tool skill-run({"skill":"demo-lab"})';
  if (/\b(time|现在|几点)\b/.test(m)) return "tool get_time()";
  if (/\b(fetch|抓取|下载)\b/.test(m)) return 'tool fetch_url({"url":"https://example.com"})';
  if (/\bread\b|读/.test(m)) return 'tool read_file({"path":"index.html"})';
  if (/\b(remember|recall|memory|记忆|session|记住)\b/.test(m)) {
    return 'tool recall({"key":"color"})';
  }
  if (/\b(pse|plan|planning|计划|规划)\b/.test(m)) return "PSE cycle 1/3 - Planner";
  return null;
}

function simulateReply(message: string, sink: ChatSink): Promise<void> {
  sink.write(sseEvent("note", "demo engine (set LLM_API_KEY + LLM_MODEL for a real model)"));
  const act = demoActNote(message);
  if (act) sink.write(sseEvent("note", act));

  const reply = cannedReply(message);
  const words = reply.split(/(\s+)/); // keep whitespace tokens so joins are lossless
  let i = 0;
  return new Promise((resolve) => {
    const tick = (): void => {
      // Emit 1-2 tokens per tick: enough pacing to watch it stream, brisk
      // enough that a full reply takes ~1.5-2s. Draw the step ONCE — using
      // two separate random calls let slice-size and increment disagree,
      // duplicating one word and skipping its neighbour.
      const step = Math.random() < 0.4 ? 2 : 1;
      const batch = words.slice(i, i + step).join("");
      i += step;
      if (batch) sink.write(sseEvent("delta", batch));
      if (i >= words.length) resolve();
      else setTimeout(tick, 28);
    };
    tick();
  });
}

function cannedReply(message: string): string {
  const m = message.toLowerCase();
  const isQuestion = /\?\s*$/.test(message) ||
    /^(what|why|how|when|who|where|which|can|could|does|do|is|are|tell)/.test(m);

  if (/^(hi|hello|hey|yo)\b/.test(m)) {
    return "Hello! This reply is generated locally by agent-httpd's demo engine — no external calls. " +
      "It is streamed token-by-token over SSE through the FastCGI chain, exactly like a real LLM reply would be. " +
      "Ask me about the server, or set LLM_API_KEY to talk to a real model.";
  }
  if (m.includes("agent-httpd") || m.includes("server")) {
    return "agent-httpd is a ~3,000-line teaching HTTP server in C: keep-alive, a prefork worker pool with " +
      "SCM_RIGHTS fd passing, FastCGI client+server, ETag/304, sendfile with Range/206, per-IP rate limiting " +
      "and graceful drain. This chat page rides the FastCGI streaming path: your POST reaches the resident " +
      "React backend, which emits SSE events as STDOUT frames.";
  }
  if (m.includes("stream") || m.includes("sse")) {
    return "The stream works like this: the browser POSTs to /react/api/chat, nginx passes it over FastCGI " +
      "(buffering off), the backend writes text/event-stream events into STDOUT frames, and the page reads " +
      "them with fetch + ReadableStream. nginx forwards each frame as it arrives — that is why tokens appear " +
      "one by one instead of all at once.";
  }
  if (isQuestion) {
    return "Good question — but I am the offline demo engine, so my answer is canned: I detect questions, echo " +
      "a bit of structure, and pace the words to demonstrate streaming. Try asking about agent-httpd, streaming " +
      "or SSE — those I know. For real answers, set LLM_API_KEY and LLM_MODEL.";
  }
  return "You said: \"" + message.slice(0, 80) + (message.length > 80 ? "…" : "") + "\" — " +
    message.trim().split(/\s+/).length + " words received. As the demo engine I mostly mirror and pace; " +
    "ask about agent-httpd, streaming or SSE, or plug in a real model via LLM_API_KEY.";
}

/* The FastCGI caller passes `net.Socket`-based writers; this type import is
 * only consumed by server-main.tsx (kept here for documentation). */
export type ChatSocket = Socket;
