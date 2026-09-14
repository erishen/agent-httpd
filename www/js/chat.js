(function () {
  "use strict";

  // ---- visible error surface (never fail silently to a blank page) ---------
  // If anything below throws at init or during a stream, print it into the
  // transcript instead of leaving the user staring at an empty <div id="log">.
  function showFatal(msg) {
    var pre = document.createElement("pre");
    pre.style.color = "#f87171";
    pre.style.padding = "16px";
    pre.style.whiteSpace = "pre-wrap";
    pre.textContent = "⚠ 页面脚本出错（请把这段发给我）：\n" + msg;
    var logEl = document.getElementById("log");
    if (logEl) logEl.appendChild(pre);
    else if (document.body) document.body.insertAdjacentElement("afterbegin", pre);
  }
  window.addEventListener("error", function (e) {
    showFatal((e.message || "error") + (e.filename ? " @ " + e.filename + ":" + e.lineno : ""));
  });
  window.addEventListener("unhandledrejection", function (e) {
    var r = e.reason;
    showFatal("unhandled promise rejection: " + (r && r.message ? r.message : String(r)));
  });

  // ---- design mirrors of React Chat.tsx ------------------------------------
  var ACT_STYLES = {
    tool:   { tag: "TOOL",  cls: "c-tool"   },
    mcp:    { tag: "MCP",   cls: "c-mcp"    },
    skill:  { tag: "SKILL", cls: "c-skill"  },
    memory: { tag: "MEM",   cls: "c-memory" },
    pse:    { tag: "PSE",   cls: "c-pse"    },
    round:  { tag: "ROUND", cls: "c-round"  },
    info:   { tag: "NOTE",  cls: "c-info"   }
  };
  var EXAMPLES = [
    { kind: "tool",   label: "算组合数 C(20,8)",   prompt: "帮我用计算工具算一下从 20 个人里选 8 个人有多少种组合，也就是 C(20,8) 等于多少？" },
    { kind: "tool",   label: "现在几点",           prompt: "现在服务器时间是几点？请调用 get_time 工具告诉我当前时间。" },
    { kind: "tool",   label: "读首页",             prompt: "用 read_file 工具读取网站首页 index.html 的内容，并简单说说它大概由哪些区块组成。" },
    { kind: "tool",   label: "抓取并总结网页",     prompt: "用 fetch_url 工具抓取 https://example.com 这个页面，然后用一两句话总结它的主要内容。" },
    { kind: "tool",   label: "算房贷月供",         prompt: "我打算贷款 200 万、年化利率 4.2%、分 30 年还清，请用 calc 工具帮我算一下每个月大概要还多少。" },
    { kind: "tool",   label: "算抽签概率",         prompt: "一年 365 天里随机抽 3 天且彼此都不重复，这个概率是多少？请用计算工具帮我算一下。" },
    { kind: "mcp",    label: "回声工具",           prompt: "调用 echo MCP 服务的 pong 工具，给我回一句 hello。" },
    { kind: "mcp",    label: "列网站目录",         prompt: "通过 fs MCP 的 list_directory 工具，列出网站根目录 /app/www 下的文件和子目录。" },
    { kind: "mcp",    label: "存一条事实",         prompt: "用 memory MCP 把『agent-httpd 是一个用 C 写的小型教学用 HTTP 服务器』这条事实写入知识库。" },
    { kind: "mcp",    label: "分步推理论证",       prompt: "借助 think MCP 的 sequentialthinking 工具，一步步推理一下 llm-router 的 skill 同步设计有什么优点和隐患。" },
    { kind: "skill",  label: "跑 demo-lab",        prompt: "运行 demo-lab 技能，看看它演示了哪些能力。" },
    { kind: "skill",  label: "代码评审",           prompt: "用 code-review 技能对 src/router.c 做一次代码评审，指出可能的问题。" },
    { kind: "skill",  label: "周度投资诊断",       prompt: "运行 weekly-investment 技能，生成本周的持仓诊断与配置建议摘要。" },
    { kind: "skill",  label: "生成项目 README",    prompt: "用 generate-readme 技能，根据 src/ 目录的结构为 agent-httpd 生成一份简洁的 README 草稿。" },
    { kind: "skill",  label: "安全扫描",           prompt: "运行 security-scan 技能，扫描 src/ 目录下的常见安全隐患并给出整改建议。" },
    { kind: "memory", label: "记偏好再回忆",       prompt: "先记住『我最喜欢的颜色是蓝色』，然后马上问我喜欢什么颜色，验证它真的记住了。" },
    { kind: "memory", label: "记住我的名字",       prompt: "请记住我的名字叫 erishen，之后再问我一次我叫什么，确认你真的记得。" },
    { kind: "memory", label: "忘掉一条事实",       prompt: "如果记忆里有关于『最喜欢的颜色』的记录，请把它忘掉，并告诉我你还剩下哪些记忆。" },
    { kind: "pse",    label: "规划再执行",         prompt: "用 PSE 编排器先规划、再执行一个小任务：把 /tmp 目录下所有 .log 文件按大小列出来。" },
    { kind: "pse",    label: "把任务拆成 3 步",    prompt: "用 PSE 编排器把『整理本周工作笔记并生成摘要』这个任务先规划成 3 个子步骤，再逐条执行。" },
    { kind: "pse",    label: "失败自动重试",       prompt: "用 PSE 编排器规划一个任务并启用失败自动重试：当某一步出错时最多重试 3 次再上报。" }
  ];

  // ---- note classification (kept in sync with src/agent.c / src/pse.c) -----
  function prettyTool(name, args) {
    if (args && (name === "remember" || name === "recall")) {
      try {
        var o = JSON.parse(args);
        if (name === "recall") return "recall " + (o.key != null ? o.key : "?");
        return "remember " + (o.key != null ? o.key : "?") + " = " + (o.value != null ? o.value : "?");
      } catch (e) { /* fall through */ }
    }
    return args ? name + "(" + args.slice(0, 90) + (args.length > 90 ? "…)" : ")") : name;
  }
  function classifyNote(note) {
    if (note.indexOf("tool ") === 0) {
      var rest = note.slice(5).trim(), name = rest, args = "";
      var open = rest.indexOf("(");
      if (open >= 0 && rest.charAt(rest.length - 1) === ")") {
        name = rest.slice(0, open).trim();
        args = rest.slice(open + 1, rest.length - 1).trim();
      }
      var kind = "tool";
      if (name.indexOf(":") >= 0) kind = "mcp";
      else if (name === "skill-run") kind = "skill";
      else if (name === "remember" || name === "recall") kind = "memory";
      return { kind: kind, label: prettyTool(name, args) };
    }
    if (note.indexOf("PSE ") === 0 || note === "[planner]" || note === "[evaluator]" || note.indexOf("PSE exhausted") >= 0)
      return { kind: "pse", label: note };
    if (/^round \d+\//.test(note)) return { kind: "round", label: note };
    return { kind: "info", label: note };
  }

  // ---- tiny markdown renderer (escape first, then decorate) ----------------
  function escapeHtml(s) {
    return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
            .replace(/"/g, "&quot;").replace(/'/g, "&#39;");
  }
  function inlineMd(s) {
    return s
      .replace(/`([^`]+)`/g, function (_, c) { return "<code>" + c + "</code>"; })
      .replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>")
      .replace(/(^|[^*])\*([^*\s][^*]*)\*/g, "$1<em>$2</em>")
      .replace(/\[([^\]]+)\]\((https?:\/\/[^)\s]+)\)/g, '<a href="$2" target="_blank" rel="noopener">$1</a>');
  }
  function mdRender(text) {
    var out = [], lines = escapeHtml(text).split("\n"), i = 0;
    while (i < lines.length) {
      var line = lines[i];
      if (/^```/.test(line)) {
        var buf = [];
        i++;
        while (i < lines.length && !/^```/.test(lines[i])) { buf.push(lines[i]); i++; }
        i++; // closing fence
        out.push("<pre><code>" + buf.join("\n") + "</code></pre>");
        continue;
      }
      var h = line.match(/^(#{1,4})\s+(.*)/);
      if (h) { out.push("<h" + h[1].length + ">" + inlineMd(h[2]) + "</h" + h[1].length + ">"); i++; continue; }
      if (/^\s*([-*])\s+/.test(line)) {
        var ul = [];
        while (i < lines.length && /^\s*([-*])\s+/.test(lines[i])) {
          ul.push("<li>" + inlineMd(lines[i].replace(/^\s*([-*])\s+/, "")) + "</li>"); i++;
        }
        out.push("<ul>" + ul.join("") + "</ul>"); continue;
      }
      if (/^\s*\d+\.\s+/.test(line)) {
        var ol = [];
        while (i < lines.length && /^\s*\d+\.\s+/.test(lines[i])) {
          ol.push("<li>" + inlineMd(lines[i].replace(/^\s*\d+\.\s+/, "")) + "</li>"); i++;
        }
        out.push("<ol>" + ol.join("") + "</ol>"); continue;
      }
      if (/^>\s?/.test(line)) {
        var q = [];
        while (i < lines.length && /^>\s?/.test(lines[i])) { q.push(inlineMd(lines[i].replace(/^>\s?/, ""))); i++; }
        out.push("<blockquote>" + q.join("<br>") + "</blockquote>"); continue;
      }
      if (line.trim() === "") { i++; continue; }
      var para = [];
      while (i < lines.length && lines[i].trim() !== "" && !/^(#{1,4}\s|```|\s*[-*]\s|\s*\d+\.\s|>\s?)/.test(lines[i])) {
        para.push(inlineMd(lines[i])); i++;
      }
      out.push("<p>" + para.join("<br>") + "</p>");
    }
    return out.join("");
  }

  // ---- session --------------------------------------------------------------
  var SESSION_KEY = "agent-httpd.chat.session";
  function sessionKey() {
    try {
      var existing = window.localStorage.getItem(SESSION_KEY);
      if (existing) return existing;
    } catch (e) { /* fall through */ }
    var fresh = (window.crypto && typeof window.crypto.randomUUID === "function")
      ? window.crypto.randomUUID()
      : "s-" + Math.random().toString(36).slice(2, 10);
    try { window.localStorage.setItem(SESSION_KEY, fresh); } catch (e) { /* ok */ }
    return fresh;
  }
  var sessionId = sessionKey();

  // ---- state / dom ----------------------------------------------------------
  var messages = [];      // {role, content, acts[], pending}
  var busy = false;
  var log = document.getElementById("log");
  var input = document.getElementById("input");
  var sendBtn = document.getElementById("send");
  var stopBtn = document.getElementById("stop");
  var legend = document.getElementById("legend");
  var engineNote = document.getElementById("engine-note");
  var sessionIdEl = document.getElementById("session-id");
  var modeSeg = document.getElementById("mode-seg");
  var mode = "agent";   // "agent" = single ReAct loop, "pse" = PSE orchestrator
  function endpointFor(m) { return m === "pse" ? "/react/api/pse" : "/react/api/chat"; }
  function noteFor(m) { return "POST " + endpointFor(m) + " → text/event-stream"; }
  var abortCtl = null;
  var stick = true;
  sessionIdEl.textContent = "session: " + sessionId.slice(0, 8);
  sessionIdEl.title = "session " + sessionId;

  log.addEventListener("scroll", function () {
    stick = log.scrollHeight - log.scrollTop - log.clientHeight < 60;
  }, { passive: true });
  function autoscroll() { if (stick) log.scrollTop = log.scrollHeight; }

  function el(tag, cls, text) {
    var d = document.createElement(tag);
    if (cls) d.className = cls;
    if (text != null) d.textContent = text;
    return d;
  }
  function makeChip(act) {
    var s = ACT_STYLES[act.kind];
    var c = el("span", "chip " + s.cls);
    c.title = act.label;
    c.appendChild(el("span", "dot d-" + act.kind));
    c.appendChild(el("span", "chip-tag", s.tag));
    c.appendChild(el("span", "label", act.label));
    return c;
  }
  function renderEmpty() {
    var box = el("div", "empty");
    box.appendChild(el("div", null,
      "Ask something — the C agent stack (tools, MCP, skills, session memory, ReAct, PSE) reports every step as a colored chip above the reply."));
    var kinds = ["tool", "mcp", "skill", "memory", "pse"];
    kinds.forEach(function (kind) {
      var row = el("div", "kind-row");
      var tag = el("span", "kind-tag " + ACT_STYLES[kind].cls);
      tag.appendChild(el("span", "dot d-" + kind));
      tag.appendChild(el("span", null, ACT_STYLES[kind].tag));
      row.appendChild(tag);
      EXAMPLES.filter(function (e) { return e.kind === kind; }).slice(0, 3).forEach(function (e) {
        var b = el("button", "chip " + ACT_STYLES[e.kind].cls);
        b.type = "button"; b.title = e.prompt;
        b.appendChild(el("span", "label", e.label));
        b.addEventListener("click", function () {
          if (e.kind === "pse") setMode("pse");
          input.value = e.prompt; input.focus(); syncSend();
        });
        row.appendChild(b);
      });
      box.appendChild(row);
    });
    log.appendChild(box);
  }
  function refreshChrome() {
    legend.classList.toggle("show", messages.length > 0);
    var empty = log.querySelector(".empty");
    if (messages.length > 0 && empty) empty.remove();
    if (messages.length === 0 && !empty) renderEmpty();
  }
  function patchLast(fn) {
    var last = messages[messages.length - 1];
    if (last && last.role === "assistant") { fn(last); }
    return last;
  }
  function appendUser(text) {
    var row = el("div", "row user");
    var col = el("div", "col");
    var b = el("div", "bubble");
    b.textContent = text;
    col.appendChild(b);
    row.appendChild(col);
    log.appendChild(row);
  }
  function appendAssistant() {
    var row = el("div", "row bot");
    var col = el("div", "col");
    var acts = el("div", "acts");
    var b = el("div", "bubble pending");
    b.appendChild(el("span", "cursor", "▍"));
    col.appendChild(acts);
    col.appendChild(b);
    row.appendChild(col);
    log.appendChild(row);
    return { col: col, acts: acts, bubble: b };
  }
  function finishAssistant(node) {
    var last = messages[messages.length - 1];
    node.bubble.classList.remove("pending");
    node.bubble.innerHTML = mdRender(last ? last.content : "");
    // copy button
    if (last && last.content) {
      var cp = el("button", "copy-btn", "copy");
      cp.type = "button"; cp.title = "Copy the full reply";
      cp.addEventListener("click", function () { copyText(last.content, cp); });
      node.col.appendChild(cp);
    }
    // follow-up suggestions
    if (last && last.content && last.content.indexOf("⚠") < 0) {
      var asked = {};
      messages.forEach(function (m) { if (m.role === "user") asked[m.content] = true; });
      var pool = EXAMPLES.filter(function (e) { return !asked[e.prompt]; });
      if (pool.length > 0) {
        var fu = el("div", "followups");
        for (var k = 0; k < 3 && k < pool.length; k++) {
          (function (sug) {
            var b = el("button", "chip " + ACT_STYLES[sug.kind].cls);
            b.type = "button"; b.title = sug.prompt;
            b.appendChild(el("span", "label", sug.label));
            b.addEventListener("click", function () {
              if (sug.kind === "pse") setMode("pse");
              input.value = sug.prompt; input.focus(); syncSend();
            });
            fu.appendChild(b);
          })(pool[k]);
        }
        node.col.appendChild(fu);
      }
    }
  }
  function copyText(text, btn) {
    function done() {
      var old = btn.textContent;
      btn.textContent = "copied ✓";
      setTimeout(function () { btn.textContent = old; }, 1200);
    }
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).then(done, function () { fallbackCopy(text); done(); });
    } else { fallbackCopy(text); done(); }
  }
  function fallbackCopy(text) {
    var ta = document.createElement("textarea");
    ta.value = text; ta.style.position = "fixed"; ta.style.opacity = "0";
    document.body.appendChild(ta); ta.select();
    try { document.execCommand("copy"); } finally { document.body.removeChild(ta); }
  }

  function setBusy(v) {
    busy = v;
    sendBtn.style.display = v ? "none" : "";
    stopBtn.style.display = v ? "" : "none";
    input.disabled = v;
    input.placeholder = v ? "streaming…" : "Type a message…";
    if (!v) input.focus();
  }
  function syncSend() { sendBtn.disabled = !input.value.trim() || busy; }

  function send() {
    var text = input.value.trim();
    if (!text || busy) return;
    input.value = ""; syncSend();
    stick = true;
    var history = messages.map(function (m) { return { role: m.role, content: m.content }; });
    messages.push({ role: "user", content: text });
    messages.push({ role: "assistant", content: "", acts: [], pending: true });
    refreshChrome();
    appendUser(text);
    var node = appendAssistant();
    autoscroll();
    setBusy(true);
    abortCtl = new AbortController();
    var finished = false, aborted = false, sawError = false;

    /* Wait ticker: the upstream (agnes) is intermittently slow — the server
     * holds the SSE open with heartbeat comments (invisible to the parser)
     * and retries timeouts up to LLM_TIMEOUT×3, so a "stuck" bubble can be
     * legitimate waiting. Surface elapsed time and silence so slow reads as
     * slow, not dead. Notes/deltas mark activity; the ticker summarizes. */
    var t0 = Date.now(), lastAct = Date.now(), lastNote = null;
    var waitTimer = setInterval(function () {
      var total = Math.round((Date.now() - t0) / 1000);
      var quiet = Math.round((Date.now() - lastAct) / 1000);
      var what = lastNote ? String(lastNote) : "等待上游首个回复";
      if (what.length > 46) what = what.slice(0, 46) + "…";
      engineNote.textContent = "agent at work… " + total + "s · " + what +
        (quiet > 3 ? "（已 " + quiet + "s 无新内容；上游偶发较慢，可 Stop 中止）" : "");
    }, 1000);
    engineNote.textContent = "agent at work… 0s · 等待上游首个回复";

    fetch(endpointFor(mode), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ message: text, history: history, sessionId: sessionId }),
      signal: abortCtl.signal
    }).then(function (res) {
      if (!res.ok || !res.body) throw new Error("stream unavailable (HTTP " + res.status + ")");
      var reader = res.body.getReader();
      var dec = new TextDecoder();
      var buf = "";
      function pump() {
        return reader.read().then(function (chunk) {
          if (chunk.done) { finish(); return; }
          buf += dec.decode(chunk.value, { stream: true });
          var sep;
          while ((sep = buf.indexOf("\n\n")) >= 0) {
            var frame = buf.slice(0, sep);
            buf = buf.slice(sep + 2);
            var dataLine = null;
            frame.split("\n").forEach(function (l) { if (l.indexOf("data:") === 0) dataLine = l; });
            if (dataLine === null) continue;
            var ev;
            try { ev = JSON.parse(dataLine.slice(5).trim()); } catch (e) { continue; }
            if (ev.t === "delta" && typeof ev.d === "string") {
              lastAct = Date.now(); lastNote = null;
              patchLast(function (m) { m.content += ev.d; });
              node.bubble.innerHTML = mdRender(messages[messages.length - 1].content) + '<span class="cursor">▍</span>';
              autoscroll();
            } else if (ev.t === "note" && typeof ev.d === "string") {
              lastAct = Date.now(); lastNote = ev.d;
              engineNote.textContent = ev.d;
              var act = classifyNote(ev.d);
              patchLast(function (m) { m.acts.push(act); });
              node.acts.appendChild(makeChip(act));
              autoscroll();
            } else if (ev.t === "error" && typeof ev.d === "string") {
              sawError = true;
              patchLast(function (m) { m.content += " ⚠ " + ev.d; });
              node.bubble.innerHTML = '<span class="err-inline">' + escapeHtml(messages[messages.length - 1].content) + "</span>";
            } else if (ev.t === "done") {
              finished = true;
            }
          }
          return pump();
        });
      }
      return pump();
    }).catch(function (err) {
      if (err && err.name === "AbortError") {
        aborted = true;
        patchLast(function (m) { m.content += " ⏹ stopped"; });
      } else {
        var detail = err && err.message ? err.message : String(err);
        patchLast(function (m) {
          if (m.content) m.content += " ⚠ " + detail;
          else m.content = "stream failed: " + detail;
        });
      }
    }).then(function () {
      clearInterval(waitTimer);
      if (!finished && !aborted && !sawError) {
        patchLast(function (m) {
          if (m.content) m.content += " ⚠ stream interrupted";
          else m.content = "⚠ stream interrupted before any content arrived";
        });
      }
      patchLast(function (m) { m.pending = false; });
      finishAssistant(node);
      engineNote.textContent = noteFor(mode);
      setBusy(false);
      abortCtl = null;
      autoscroll();
    });

    function finish() { /* handled in the trailing .then */ }
  }

  document.getElementById("new-session").addEventListener("click", function () {
    try { window.localStorage.removeItem(SESSION_KEY); } catch (e) { /* ok */ }
    sessionId = sessionKey();
    sessionIdEl.textContent = "session: " + sessionId.slice(0, 8);
    sessionIdEl.title = "session " + sessionId;
    messages = [];
    log.innerHTML = "";
    refreshChrome();
    engineNote.textContent = noteFor(mode);
  });

  function setMode(m) {
    if (m !== "agent" && m !== "pse") return;
    mode = m;
    if (modeSeg) {
      Array.prototype.forEach.call(modeSeg.querySelectorAll(".seg-btn"), function (b) {
        b.classList.toggle("active", b.getAttribute("data-mode") === m);
      });
    }
    engineNote.textContent = noteFor(mode);
  }

  if (modeSeg) {
    modeSeg.addEventListener("click", function (e) {
      var btn = e.target.closest(".seg-btn");
      if (btn) setMode(btn.getAttribute("data-mode"));
    });
  }

  sendBtn.addEventListener("click", send);
  stopBtn.addEventListener("click", function () { if (abortCtl) abortCtl.abort(); });
  input.addEventListener("input", syncSend);
  input.addEventListener("keydown", function (e) {
    if (e.isComposing) return; // IME composition: Enter confirms candidates
    if (e.key === "Enter" && !e.shiftKey) { e.preventDefault(); send(); }
  });

  refreshChrome();
  syncSend();
  input.focus();
})();
