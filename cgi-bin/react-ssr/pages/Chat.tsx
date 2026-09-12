// Chat page — lazy-loaded chunk. The server renders the empty shell (the
// transcript is conversation state, deliberately not SSR'd); everything
// interactive happens after hydration: POST /react/api/chat returns an
// text/event-stream that the page consumes with fetch + ReadableStream and
// renders token-by-token. See server/chat.ts for the protocol
//   data: {"t":"delta"|"note"|"error"|"done","d":...}
// and the two engines behind it (LLM upstream when LLM_API_KEY is set,
// local paced simulation otherwise).
//
// Agent stack: the C backend (src/agent.c, src/pse.c) drives tools, MCP,
// skills and session memory — it reports every non-text step as a "note"
// event (e.g. 'tool calc(...)', 'PSE cycle 1/3 - Planner', 'round 2/8').
// This page classifies each note into an activity chip and lays them out
// as a trail above the streaming reply, so Tool Call / MCP / Skills /
// Memory / PSE are visible, not just the final text. A sticky sessionId
// (localStorage) is sent along so the server persists transcript+facts.
import { useEffect, useRef, useState } from "react";
import { Link } from "react-router";
import Markdown from "../lib/md";

type Activity = { kind: ActKind; label: string };

// Kinds map 1:1 to the note strings the backends emit — keep in sync with
// src/agent.c / src/pse.c / server/chat.ts.
type ActKind = "tool" | "mcp" | "skill" | "memory" | "pse" | "round" | "info";

type ActStyle = {
  tag: string;
  chip: string;
  dot: string;
};

const ACT_STYLES: Record<ActKind, ActStyle> = {
  tool: { tag: "TOOL", chip: "border-sky-500/50 text-sky-300", dot: "bg-sky-400" },
  mcp: { tag: "MCP", chip: "border-violet-400/40 text-violet-400", dot: "bg-violet-400" },
  skill: { tag: "SKILL", chip: "border-emerald-400/40 text-emerald-400", dot: "bg-emerald-400" },
  memory: { tag: "MEM", chip: "border-amber-400/40 text-amber-300", dot: "bg-amber-400" },
  pse: { tag: "PSE", chip: "border-fuchsia-400/40 text-fuchsia-400", dot: "bg-fuchsia-400" },
  round: { tag: "ROUND", chip: "border-edge text-slate-400", dot: "bg-slate-500" },
  info: { tag: "NOTE", chip: "border-edge text-slate-500", dot: "bg-slate-600" },
};

// Parse the note strings into a displayable activity. Note formats (kept in
// sync with the C agent): "tool name({json})", "round N/M",
// "PSE cycle n/N - Phase ...", "[planner]", "[evaluator]",
// "PSE exhausted attempts; ...", plus free-form info notes.
function classifyNote(note: string): Activity {
  if (note.startsWith("tool ")) {
    const rest = note.slice(5).trim();
    let name = rest;
    let args = "";
    const open = rest.indexOf("(");
    if (open >= 0 && rest.endsWith(")")) {
      name = rest.slice(0, open).trim();
      args = rest.slice(open + 1, rest.length - 1).trim();
    }
    let kind: ActKind = "tool";
    if (name.includes(":")) kind = "mcp"; // <mcpServer>:<toolName>
    else if (name === "skill-run") kind = "skill";
    else if (name === "remember" || name === "recall") kind = "memory";
    const label = prettyTool(name, args);
    return { kind, label };
  }
  if (
    note.startsWith("PSE ") ||
    note === "[planner]" ||
    note === "[evaluator]" ||
    note.includes("PSE exhausted")
  ) {
    return { kind: "pse", label: note };
  }
  if (/^round \d+\//.test(note)) return { kind: "round", label: note };
  return { kind: "info", label: note };
}

// Example tasks exercising the full agent stack. Each is phrased as a real
// natural-language request (not a tool-macro) and maps to a built-in tool
// (calc / get_time / read_file / fetch_url / skill-run / remember / recall),
// an MCP tool (echo__pong / fs__* / memory__* / think__* synced from the
// llm-router catalog), a skill (local demo-lab, router materialized skills,
// or project skills like code-review / weekly-investment), session memory,
// or the PSE orchestrator. A real upstream parses the wording and picks the
// tool; the local demo engine (server/chat.ts) also keys off these phrases
// to light up the matching activity chip. Keep prompts carrying the keyword
// their kind's branch in demoActNote recognizes.
const EXAMPLES: { kind: ActKind; label: string; prompt: string }[] = [
  // — tool — built-in calculation & IO utilities
  { kind: "tool", label: "算组合数 C(20,8)", prompt: "帮我用计算工具算一下从 20 个人里选 8 个人有多少种组合，也就是 C(20,8) 等于多少？" },
  { kind: "tool", label: "现在几点", prompt: "现在服务器时间是几点？请调用 get_time 工具告诉我当前时间。" },
  { kind: "tool", label: "读首页", prompt: "用 read_file 工具读取网站首页 index.html 的内容，并简单说说它大概由哪些区块组成。" },
  { kind: "tool", label: "抓取并总结网页", prompt: "用 fetch_url 工具抓取 https://example.com 这个页面，然后用一两句话总结它的主要内容。" },
  { kind: "tool", label: "算房贷月供", prompt: "我打算贷款 200 万、年化利率 4.2%、分 30 年还清，请用 calc 工具帮我算一下每个月大概要还多少。" },
  { kind: "tool", label: "算抽签概率", prompt: "一年 365 天里随机抽 3 天且彼此都不重复，这个概率是多少？请用计算工具帮我算一下。" },
  // — mcp — external servers wired through the router catalog
  { kind: "mcp", label: "回声工具", prompt: "调用 echo MCP 服务的 pong 工具，给我回一句 hello。" },
  { kind: "mcp", label: "列项目目录", prompt: "通过 fs MCP 的 list_directory 工具，列出项目根目录 /home/user/projects 下的文件和子目录。" },
  { kind: "mcp", label: "存一条事实", prompt: "用 memory MCP 把『agent-httpd 是一个用 C 写的小型教学用 HTTP 服务器』这条事实写入知识库。" },
  { kind: "mcp", label: "分步推理论证", prompt: "借助 think MCP 的 sequentialthinking 工具，一步步推理一下 llm-router 的 skill 同步设计有什么优点和隐患。" },
  // — skill — local, router & project skills
  { kind: "skill", label: "跑 demo-lab", prompt: "运行 demo-lab 技能，看看它演示了哪些能力。" },
  { kind: "skill", label: "代码评审", prompt: "用 code-review 技能对 src/router.c 做一次代码评审，指出可能的问题。" },
  { kind: "skill", label: "周度投资诊断", prompt: "运行 weekly-investment 技能，生成本周的持仓诊断与配置建议摘要。" },
  { kind: "skill", label: "生成项目 README", prompt: "用 generate-readme 技能，根据 src/ 目录的结构为 agent-httpd 生成一份简洁的 README 草稿。" },
  { kind: "skill", label: "安全扫描", prompt: "运行 security-scan 技能，扫描 src/ 目录下的常见安全隐患并给出整改建议。" },
  // — memory — session-scoped facts
  { kind: "memory", label: "记偏好再回忆", prompt: "先记住『我最喜欢的颜色是蓝色』，然后马上问我喜欢什么颜色，验证它真的记住了。" },
  { kind: "memory", label: "记住我的名字", prompt: "请记住我的名字叫 erishen，之后再问我一次我叫什么，确认你真的记得。" },
  { kind: "memory", label: "忘掉一条事实", prompt: "如果记忆里有关于『最喜欢的颜色』的记录，请把它忘掉，并告诉我你还剩下哪些记忆。" },
  // — pse — planner / specialist / evaluator orchestration
  { kind: "pse", label: "规划再执行", prompt: "用 PSE 编排器先规划、再执行一个小任务：把 /tmp 目录下所有 .log 文件按大小列出来。" },
  { kind: "pse", label: "把任务拆成 3 步", prompt: "用 PSE 编排器把『整理本周工作笔记并生成摘要』这个任务先规划成 3 个子步骤，再逐条执行。" },
  { kind: "pse", label: "失败自动重试", prompt: "用 PSE 编排器规划一个任务并启用失败自动重试：当某一步出错时最多重试 3 次再上报。" },
];

// Bootstrap a sticky session id once (client only): minted and persisted on
// first visit, it is what keeps transcript + facts alive across reloads on
// the server (.data/sessions/<id>.json).
const SESSION_KEY = "agent-httpd.chat.session";
function sessionKey(): string {
  if (typeof window === "undefined") return "";
  try {
    const existing = window.localStorage.getItem(SESSION_KEY);
    if (existing) return existing;
  } catch {
    /* private mode etc. — fall through to an in-memory id */
  }
  const fresh =
    typeof crypto !== "undefined" && typeof crypto.randomUUID === "function"
      ? crypto.randomUUID()
      : "s-" + Math.random().toString(36).slice(2, 10);
  try {
    window.localStorage.setItem(SESSION_KEY, fresh);
  } catch {
    /* storage unavailable — the id still works for this page load */
  }
  return fresh;
}

function prettyTool(name: string, args: string): string {
  if (args && (name === "remember" || name === "recall")) {
    try {
      const o = JSON.parse(args) as Record<string, string>;
      if (name === "recall") return "recall " + (o.key ?? "?");
      return "remember " + (o.key ?? "?") + " = " + (o.value ?? "?");
    } catch {
      /* args not full JSON — fall back to the raw trace */
    }
  }
  return args ? name + "(" + args.slice(0, 90) + (args.length > 90 ? "…)" : ")") : name;
}

interface Msg {
  role: "user" | "assistant";
  content: string;
  acts?: Activity[]; // agent steps leading up to/around this bubble
  pending?: boolean; // assistant bubble still receiving tokens
}

function ActivityChip({ act }: { act: Activity }) {
  const s = ACT_STYLES[act.kind];
  return (
    <span
      className={
        "inline-flex max-w-full items-center gap-1.5 rounded-md border bg-ink/60 px-2 py-0.5 font-mono text-[12px] " +
        s.chip
      }
      title={act.label}
    >
      <span className={"h-1.5 w-1.5 rounded-full " + s.dot} />
      <span className="font-bold uppercase tracking-wider">{s.tag}</span>
      <span className="truncate">{act.label}</span>
    </span>
  );
}

// Copy-to-clipboard button for assistant replies. Sits in the bubble's chip
// row: only useful once streaming finished (no pending flag), and hidden
// until hover/focus so the transcript stays visually quiet.
function CopyButton({ text }: { text: string }) {
  const [copied, setCopied] = useState(false);
  const timerRef = useRef<number | null>(null);
  useEffect(
    () => () => {
      // clear the "copied" reset timer if the button unmounts mid-flight
      if (timerRef.current !== null) window.clearTimeout(timerRef.current);
    },
    []
  );
  const copy = async (): Promise<void> => {
    try {
      await navigator.clipboard.writeText(text);
    } catch {
      // clipboard API denied (http origin, Safari gate): fall back to a
      // transient textarea so the button still works on plain http.
      const ta = document.createElement("textarea");
      ta.value = text;
      ta.style.position = "fixed";
      ta.style.opacity = "0";
      document.body.appendChild(ta);
      ta.select();
      try {
        document.execCommand("copy");
      } finally {
        document.body.removeChild(ta);
      }
    }
    setCopied(true);
    timerRef.current = window.setTimeout(() => setCopied(false), 1200);
  };
  return (
    <button
      onClick={() => void copy()}
      className="rounded border border-edge/60 px-1.5 py-0.5 font-mono text-[10px] text-slate-500 opacity-0 transition hover:border-accent hover:text-accent focus:opacity-100 group-hover:opacity-100"
      title="Copy the full reply"
    >
      {copied ? "copied ✓" : "copy"}
    </button>
  );
}

// Follow-up suggestions shown under a finished reply: rotating examples the
// current conversation hasn't pushed yet, so each answer invites the next
// demo. Deterministic per message index (stable across SSR/hydration).
function suggestFollowUps(msgs: Msg[], i: number): (typeof EXAMPLES)[number][] {
  const asked = new Set(
    msgs.slice(0, i + 1).filter((m) => m.role === "user").map((m) => m.content)
  );
  const pool = EXAMPLES.filter((e) => !asked.has(e.prompt));
  const out: (typeof EXAMPLES)[number][] = [];
  for (let k = 0; k < 3 && out.length < pool.length; k++) {
    const it = pool[(i + k) % pool.length];
    if (!out.includes(it)) out.push(it);
  }
  return out;
}

function Chat() {
  const [messages, setMessages] = useState<Msg[]>([]);
  const [draft, setDraft] = useState("");
  const [busy, setBusy] = useState(false);
  const [hydrated, setHydrated] = useState(false);
  const [sessionId, setSessionId] = useState(() => sessionKey());
  const [engineNote, setEngineNote] = useState("");
  const abortRef = useRef<AbortController | null>(null);
  const scrollRef = useRef<HTMLDivElement | null>(null);
  const inputRef = useRef<HTMLInputElement | null>(null);

  // Keep the newest token in view while streaming, but never yank the
  // scrollbar away from a user who scrolled up to read earlier context.
  const stickRef = useRef(true);
  useEffect(() => {
    const el = scrollRef.current;
    if (!el) return;
    const onScroll = (): void => {
      // within 60px of the bottom counts as "unsick undo" — user is at the
      // newest line, so auto-follow resumes.
      stickRef.current = el.scrollHeight - el.scrollTop - el.clientHeight < 60;
    };
    el.addEventListener("scroll", onScroll, { passive: true });
    return () => el.removeEventListener("scroll", onScroll);
  }, []);
  useEffect(() => {
    const el = scrollRef.current;
    if (el && stickRef.current) el.scrollTop = el.scrollHeight;
  }, [messages]);

  // Post-hydration: only then may the session indicator touch localStorage.
  useEffect(() => {
    setHydrated(true);
  }, []);

  const patchLast = (patch: (m: Msg) => Msg): void => {
    setMessages((prev) => {
      const next = [...prev];
      const last = next[next.length - 1];
      if (last && last.role === "assistant") next[next.length - 1] = patch(last);
      return next;
    });
  };

  // "New session": abandon the persisted store on the server side by minting
  // a fresh id (the old one stays on disk until pruned; a real app would
  // also DELETE it). The transcript is client-side state and stays visible.
  const newSession = (): void => {
    let fresh = "";
    try {
      window.localStorage.removeItem(SESSION_KEY);
      fresh = sessionKey();
    } catch {
      fresh = "s-" + Math.random().toString(36).slice(2, 10);
    }
    setSessionId(fresh);
    setMessages([]);
    setEngineNote("");
  };

  const send = async (): Promise<void> => {
    const text = draft.trim();
    if (!text || busy) return;
    setDraft("");
    const history = messages.map((m) => ({ role: m.role, content: m.content }));
    setMessages((prev) => [
      ...prev,
      { role: "user", content: text },
      { role: "assistant", content: "", acts: [], pending: true },
    ]);
    setBusy(true);
    setEngineNote("");
    const ctl = new AbortController();
    abortRef.current = ctl;
    let aborted = false;
    let sawError = false;
    let finished = false;
    try {
      const res = await fetch("/react/api/chat", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ message: text, history, sessionId }),
        signal: ctl.signal,
      });
      if (!res.ok || !res.body) throw new Error("stream unavailable (HTTP " + res.status + ")");
      const reader = res.body.getReader();
      const decoder = new TextDecoder();
      let buf = "";
      while (!finished) {
        const { value, done } = await reader.read();
        if (done) break;
        buf += decoder.decode(value, { stream: true });
        // SSE frames are separated by a blank line; events can in theory
        // split across chunks, so reassemble here.
        let sep: number;
        while ((sep = buf.indexOf("\n\n")) >= 0) {
          const frame = buf.slice(0, sep);
          buf = buf.slice(sep + 2);
          const dataLine = frame.split("\n").find((l) => l.startsWith("data:"));
          if (!dataLine) continue;
          let ev: { t: string; d?: string };
          try {
            ev = JSON.parse(dataLine.slice(5).trim()) as { t: string; d?: string };
          } catch {
            continue; // partial/foreign event — skip rather than die mid-stream
          }
          if (ev.t === "delta" && typeof ev.d === "string") {
            patchLast((m) => ({ ...m, content: m.content + ev.d }));
          } else if (ev.t === "note" && typeof ev.d === "string") {
            setEngineNote(ev.d);
            patchLast((m) => ({ ...m, acts: [...(m.acts ?? []), classifyNote(ev.d ?? "")] }));
          } else if (ev.t === "error" && typeof ev.d === "string") {
            sawError = true;
            patchLast((m) => ({ ...m, content: m.content + " ⚠ " + ev.d }));
          } else if (ev.t === "done") {
            finished = true;
          }
        }
      }
    } catch (err) {
      if ((err as Error).name === "AbortError") {
        aborted = true;
        patchLast((m) => ({ ...m, content: m.content + " ⏹ stopped" }));
      } else {
        const detail = err instanceof Error ? err.message : String(err);
        patchLast((m) =>
          m.content ? { ...m, content: m.content + " ⚠ " + detail } : { ...m, content: "stream failed: " + detail }
        );
      }
    } finally {
      // Connection dropped before the server's "done": tell the user the
      // transcript may be truncated rather than silently exposing a gap.
      if (!finished && !aborted && !sawError) {
        patchLast((m) =>
          m.content
            ? { ...m, content: m.content + " ⚠ stream interrupted" }
            : { ...m, content: "⚠ stream interrupted before any content arrived" }
        );
      }
      patchLast((m) => ({ ...m, pending: false }));
      setBusy(false);
      abortRef.current = null;
    }
  };

  const bubble =
    "min-w-0 w-fit max-w-full rounded-2xl px-4 py-2.5 text-[15px] leading-relaxed break-words";

  return (
    <section className="flex flex-col rounded-2xl border border-edge bg-surface shadow-xl shadow-black/20">
      <div className="flex flex-wrap items-center gap-x-3 gap-y-1.5 border-b border-edge px-5 py-3">
        <span className="rounded-full bg-accent/15 px-3 py-0.5 text-[11px] font-bold uppercase tracking-wider text-accent">
          SSE stream
        </span>
        <span className="truncate text-xs text-slate-500">
          {engineNote || (busy ? "agent at work…" : "POST /react/api/chat → text/event-stream")}
        </span>
        <span className="ml-auto flex items-center gap-2 text-xs">
          {hydrated && sessionId && (
            <>
              <span className="font-mono text-slate-500" title={"session " + sessionId}>
                session: {sessionId.slice(0, 8)}
              </span>
              <button
                onClick={newSession}
                className="rounded border border-edge px-2 py-0.5 font-mono text-[11px] text-slate-400 transition hover:border-accent hover:text-accent"
                title="Mint a fresh session id — the server keeps per-session transcript + facts in .data/sessions/"
              >
                new session
              </button>
            </>
          )}
        </span>
      </div>

      {/* agent-stack legend — same colors as the chips below */}
      {messages.length > 0 && (
        <div className="flex flex-wrap items-center gap-x-4 gap-y-1 border-b border-edge/40 px-5 py-1.5 text-[10px] uppercase tracking-wider text-slate-600">
          <span>Agent stack</span>
          {(["tool", "mcp", "skill", "memory", "pse", "round"] as ActKind[]).map((k) => (
            <span key={k} className={"flex items-center gap-1 " + "text-slate-500"}>
              <span className={"h-1.5 w-1.5 rounded-full " + ACT_STYLES[k].dot} />
              {ACT_STYLES[k].tag}
            </span>
          ))}
        </div>
      )}

      {/* transcript — fills the viewport so long agent runs stay readable */}
      <div
        ref={scrollRef}
        className="flex h-[max(26rem,calc(100vh_-_19rem))] min-h-96 flex-col gap-3 overflow-y-auto p-5"
      >
        {messages.length === 0 && (
          <div className="m-auto max-w-md text-center text-sm text-slate-500">
            Ask something — the C agent stack (tools, MCP, skills, session
            memory, ReAct, PSE) reports every step as a colored chip above the
            reply.
            <div className="mt-4 space-y-2">
              {(["tool", "mcp", "skill", "memory", "pse"] as ActKind[]).map((kind) => (
                <div key={kind} className="flex flex-wrap items-center justify-center gap-1.5">
                  <span className={"flex items-center gap-1 text-[11px] font-bold uppercase tracking-wider " + ACT_STYLES[kind].chip}>
                    <span className={"h-1.5 w-1.5 rounded-full " + ACT_STYLES[kind].dot} />
                    {ACT_STYLES[kind].tag}
                  </span>
                  {EXAMPLES.filter((e) => e.kind === kind).slice(0, 3).map((e, j) => (
                    <button
                      key={j}
                      onClick={() => setDraft(e.prompt)}
                      className={
                        "inline-flex max-w-full items-center gap-1.5 rounded-md border bg-ink/60 px-2 py-1 font-mono text-[13px] transition hover:brightness-125 " +
                        ACT_STYLES[e.kind].chip
                      }
                      title={e.prompt}
                    >
                      <span className="truncate">{e.label}</span>
                    </button>
                  ))}
                </div>
              ))}
            </div>
          </div>
        )}
        {messages.map((m, i) => {
          const followUps =
            m.role === "assistant" && !m.pending && m.content && !m.content.includes("⚠")
              ? suggestFollowUps(messages, i)
              : [];
          return (
            <div key={i} className={"flex " + (m.role === "user" ? "justify-end" : "justify-start")}>
              <div className="group flex max-w-[92%] flex-col items-start gap-1.5">
              {m.role === "assistant" && m.acts && m.acts.length > 0 && (
                <div className="flex flex-wrap gap-1">
                  {m.acts.map((act, j) => (
                    <ActivityChip key={j} act={act} />
                  ))}
                </div>
              )}
              <div
                className={
                  bubble +
                  (m.role === "user"
                    ? " bg-accent text-accent-ink"
                    : " border border-edge bg-ink text-slate-200") +
                  (m.pending ? " animate-pulse" : "")
                }
              >
                {m.role === "user" ? m.content : <Markdown text={m.content} />}
                {m.pending && <span className="ml-0.5 text-accent">▍</span>}
              </div>
              {/* copy the finished reply — hover-revealed, next to the bubble */}
              {m.role === "assistant" && !m.pending && m.content && (
                <CopyButton text={m.content} />
              )}
              {/* finished answers invite the next example question */}
              {followUps.length > 0 && (
                <div className="mt-1 flex flex-wrap items-center gap-1.5 border-t border-edge/40 pt-2">
                  {followUps.map((sug, k) => (
                    <button
                      key={k}
                      onClick={() => {
                        setDraft(sug.prompt);
                        inputRef.current?.focus();
                      }}
                      className={
                        "inline-flex max-w-full items-center gap-1.5 rounded-md border bg-ink/60 px-2 py-1 font-mono text-[13px] transition hover:brightness-125 " +
                        ACT_STYLES[sug.kind].chip
                      }
                      title={sug.prompt}
                    >
                      <span className="truncate">{sug.label}</span>
                    </button>
                  ))}
                </div>
              )}
            </div>
          </div>
        );
      })}
      </div>

      {/* composer */}
      <div className="flex items-center gap-2 border-t border-edge p-4">
        <input
          ref={inputRef}
          value={draft}
          onChange={(e) => setDraft(e.target.value)}
          onKeyDown={(e) => {
            // IME composition fires Enter to confirm candidates; sending on
            // that would submit a half-typed message.
            if (e.nativeEvent.isComposing) return;
            if (e.key === "Enter" && !e.shiftKey) {
              e.preventDefault();
              void send();
            }
          }}
          placeholder={busy ? "streaming…" : "Type a message…"}
          disabled={busy}
          className="flex-1 rounded-lg border border-edge bg-ink px-4 py-2.5 text-sm text-slate-200 outline-none transition placeholder:text-slate-600 focus:border-accent disabled:opacity-60"
        />
        {busy ? (
          <button
            onClick={() => abortRef.current?.abort()}
            className="rounded-lg border border-edge bg-ink px-5 py-2.5 font-bold text-slate-400 transition hover:border-accent hover:text-accent"
          >
            Stop
          </button>
        ) : (
          <button
            onClick={() => void send()}
            disabled={!draft.trim()}
            className="rounded-lg bg-accent px-6 py-2.5 font-bold text-accent-ink transition hover:bg-accent-hi disabled:opacity-40"
          >
            Send
          </button>
        )}
      </div>

      <p className="px-5 pb-4 text-xs text-slate-500">
        The stream rides the agent stack: <span className="text-sky-300">tool</span> ·{" "}
        <span className="text-violet-400">mcp</span> · <span className="text-emerald-400">skill</span> ·{" "}
        <span className="text-amber-300">memory</span> · <span className="text-fuchsia-400">pse</span>{" "}
        steps arrive as <code className="text-slate-300">note</code> events and render as chips; this page stays
        in sync with <code className="text-sky-300">src/agent.c</code> and{" "}
        <code className="text-sky-300">src/pse.c</code>. The demo engine (no{" "}
        <code className="text-amber-300">LLM_API_KEY</code>) only emits the "demo engine" note — set the key to
        see real tool/MCP/skill/PSE traces. <Link to="/react" className="text-accent hover:underline">Home</Link>
      </p>
    </section>
  );
}

export default Chat;