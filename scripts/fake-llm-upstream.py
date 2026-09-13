#!/usr/bin/env python3
"""Fake OpenAI-compatible SSE upstream for the agent-httpd chat smoke tests.

Stands in for the real LLM gateway so `make test` can exercise the whole
fork-curl streaming path (src/agent/llm.c) offline and deterministically. Branches
on the last system message and last user message in the request body:

  你好    -> Chinese deltas + [DONE]          (UTF-8 round-trip)
  long    -> one 20000-char delta             (single SSE line > 16KB)
  errnull -> chunks carrying "error": null    (must NOT abort the stream)
  fail    -> {"error":{"message":...}}        (must emit an error event)
  tail    -> final data line WITHOUT newline  (EOF tail must still count)

ReAct tool-call branch: when the request shows a tool result (a
role:"tool" message) it replies "got: <content>"; otherwise it answers with
streamed assistant tool_calls for the matching tool branch:

  tool        -> calc       {"expression":"2*21"}
  skill       -> skill-run  {"skill":"demo"}
  rememberit  -> remember   {"key":"color","value":"blue"}
  recallit    -> recall     {"key":"color"}
  mcp         -> echo__pong  {"text":"hey"}

PSE branches (dispatch on the system role prompt):
  Planner    -> streams a plan ("PLAN: ...") with no tool_calls
  Specialist -> tool_calls round(s) like the ReAct path, then text
  Evaluator  -> streams "PASS ..." (first line PASS ends the orchestrator)

Usage: fake-llm-upstream.py [port]   (default 18901)
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


def last_system(messages):
    sysmsg = ""
    for m in messages:
        if m.get("role") == "system":
            sysmsg = m.get("content", "")
    return sysmsg


def last_user(messages):
    for m in reversed(messages):
        if m.get("role") == "user":
            return m.get("content", "")
    return ""


def last_tool_content(messages):
    for m in reversed(messages):
        if m.get("role") == "tool":
            return m.get("content", "")
    return ""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(length) or b"{}")
        messages = body.get("messages", [])
        msg = last_user(messages).lower()
        sys = last_system(messages)

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()

        def chunk(delta_obj):
            payload = {"choices": [{"delta": delta_obj}]}
            self.wfile.write(("data: " + json.dumps(payload) + "\n").encode())

        def tool_calls(calls):
            # streaming tool-call format: role first, then per-index deltas
            chunk({"role": "assistant", "content": None})
            for c in calls:
                chunk({"tool_calls": [{
                    "index": c["i"],
                    "id": c.get("id", "call_%d" % c["i"]),
                    "type": "function",
                    "function": {"name": c["name"], "arguments": c["args"]},
                }]})
            self.wfile.write(b"data: [DONE]\n")

        def with_tools(calls):
            any_tool = any(m.get("role") == "tool" for m in messages)
            if any_tool:
                chunk({"content": "got: " + last_tool_content(messages)})
            else:
                tool_calls(calls)
            self.wfile.write(b"data: [DONE]\n")

        # PSE orchestrator requests are uniquely tagged by their role system
        # prompt (你是 Planner/Specialist/Evaluator), so they must be matched
        # BEFORE the body-keyword branches below: "pse-fail" contains "fail",
        # which would otherwise be intercepted by the error-branch below.
        if sys.startswith("你是 Evaluator"):
            # pse-fail:      always FAIL  -> orchestrator exhausts 3 attempts
            # pse-retry:     PASS only once the specialist result is "42"
            #                (retry cycle's calc, driven by the PSE_FEEDBACK
            #                marker injected into the 2nd planner)
            # else:          PASS on the first try (plain PSE)
            if "pse-fail" in msg:
                chunk({"content": "FAIL 未达标。PSE_FEEDBACK 请继续重试。"})
            elif "pse-retry" in msg:
                verdict = "PASS 已验证。" if "got: 42" in msg \
                    else "FAIL 不达标。PSE_FEEDBACK 请改用 V2 方案。"
                chunk({"content": verdict})
            else:
                chunk({"content": "PASS 已验证，目标全部达成。"})
            self.wfile.write(b"data: [DONE]\n")
        elif sys.startswith("你是 Planner"):
            if "pse-fail" in msg:
                # fixed plan: the specialist/evaluator never see "PLAN-V2",
                # so every cycle fails (exhaustion path)
                for text in ["PLAN: ", "attempt-1 plan (no V2)"]:
                    chunk({"content": text})
            elif "pse_feedback" in msg:
                # 2nd planner of a retry: the feedback marker routes us here
                # (msg is lowercased, so match the folded marker)
                for text in ["PLAN-V2: ", "redo the calc step"]:
                    chunk({"content": text})
            else:
                for text in ["PLAN: ", "1) 列出步骤 ", "2) 调用 calc 计算 42"]:
                    chunk({"content": text})
            self.wfile.write(b"data: [DONE]\n")
        elif sys.startswith("你是 Specialist"):
            if "pse-fail" in msg:
                with_tools([
                    {"i": 0, "id": "call_s1", "name": "calc",
                     "args": json.dumps({"expression": "40+1"})},
                ])
            elif "pse-retry" in msg:
                if "PLAN-V2" in sys:
                    with_tools([
                        {"i": 0, "id": "call_s2", "name": "calc",
                         "args": json.dumps({"expression": "21*2"})},
                    ])
                else:
                    with_tools([
                        {"i": 0, "id": "call_s1", "name": "calc",
                         "args": json.dumps({"expression": "40+1"})},
                    ])
            else:
                with_tools([
                    {"i": 0, "id": "call_spec", "name": "calc",
                     "args": json.dumps({"expression": "6*7"})},
                ])
        elif "你好" in msg:
            for text in ["你好", "，", "世界", "！", " ok"]:
                chunk({"content": text})
            self.wfile.write(b"data: [DONE]\n")
        elif "long" in msg:
            chunk({"content": "x" * 20000})
            self.wfile.write(b"data: [DONE]\n")
        elif "errnull" in msg:
            self.wfile.write(
                b'data: {"error":null,"choices":[{"delta":{"content":"part1 "}}]}\n')
            self.wfile.write(
                b'data: {"error":null,"choices":[{"delta":{"content":"part2"}}]}\n')
            self.wfile.write(b"data: [DONE]\n")
        elif "fail" in msg:
            self.wfile.write(
                b'data: {"error":{"message":"quota exhausted (fake)"}}\n')
            self.wfile.write(b"data: [DONE]\n")
        elif "tail" in msg:
            self.wfile.write(b'data: {"choices":[{"delta":{"content":"early "}}]}\n')
            # no trailing newline: exercises the EOF-tail flush
            self.wfile.write(b'data: {"choices":[{"delta":{"content":"last words"}}]}')
        elif "skill" in msg:
            # "lab" steers to the committed skills/demo-lab skill; a plain
            # "run the skill" keeps using the smoke-lab "demo" skill so the
            # SKILL_DIR-installed DEMOSKILL_BODY assertions stay untouched.
            skill = "demo-lab" if "lab" in msg else "demo"
            with_tools([
                {"i": 0, "id": "call_skill", "name": "skill-run",
                 "args": json.dumps({"skill": skill})},
            ])
        elif "rememberit" in msg:
            with_tools([
                {"i": 0, "id": "call_mem", "name": "remember",
                 "args": json.dumps({"key": "color", "value": "blue"})},
            ])
        elif "recallit" in msg:
            with_tools([
                {"i": 0, "id": "call_rec", "name": "recall",
                 "args": json.dumps({"key": "color"})},
            ])
        # natural-language memory (checked AFTER the lab keywords so
        # "rememberit"/"recallit" still win for the smoke assertions)
        elif "remember" in msg:
            with_tools([
                {"i": 0, "id": "call_r1", "name": "remember",
                 "args": json.dumps({"key": "color", "value": "blue"})},
            ])
        elif "recall" in msg:
            with_tools([
                {"i": 0, "id": "call_r2", "name": "recall",
                 "args": json.dumps({"key": "color"})},
            ])
        elif "mcp" in msg:
            with_tools([
                {"i": 0, "id": "call_mcp", "name": "echo__pong",
                 "args": json.dumps({"text": "hey"})},
            ])
        elif "time" in msg or "现在" in msg or "几点" in msg:
            # before the generic "tool" branch: "get_time" contains "tool"
            with_tools([
                {"i": 0, "id": "call_time", "name": "get_time",
                 "args": json.dumps({})},
            ])
        elif "read" in msg:
            with_tools([
                {"i": 0, "id": "call_read", "name": "read_file",
                 "args": json.dumps({"path": "index.html"})},
            ])
        elif "tool" in msg:
            with_tools([
                {"i": 0, "id": "call_calc", "name": "calc",
                 "args": json.dumps({"expression": "2*21"})},
            ])
        else:
            chunk({"content": "echo: " + msg[:80]})
            self.wfile.write(b"data: [DONE]\n")


port = int(sys.argv[1]) if len(sys.argv) > 1 else 18901
HTTPServer(("127.0.0.1", port), Handler).serve_forever()
