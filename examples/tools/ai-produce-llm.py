#!/usr/bin/env python3
"""External agent tool: the REAL LLM-side producer of the lume-core AI
self-verification loop ("AI writes, machine verifies", lume-core
docs/ai-verify.md P4) — the step scripts/ai-gen-programs.sh stands in for.

This closes the last mile of TODO P4: prompts go to a real LLM, the
reply is turned into a .lume program, the lume-core machine gate judges
it, and a rejection loops the counter-example back to the model (bounded
retries). Programs the gate accepts are dropped into the candidates
directory together with manifest rows — the exact contract
scripts/ai-produce.sh consumes — so the deterministic produce loop takes
over from there.

Configuration (environment; same names as this repo's .env):
  LLM_API_URL        full chat-completions URL (OpenAI-compatible),
                     e.g. http://localhost:19070/v1/chat/completions
                     [default: http://localhost:19070/v1/chat/completions]
  LLM_API_KEY        bearer token (required; never logged, never written)
  LLM_MODEL          model name [default: auto]
  AI_LLM_ROUTER_AGENT  value of the X-Llm-Router-Agent header sent with
                     every call [default: "off" — direct-model traffic;
                     the tsm-hub router hangs direct chat calls on its
                     default agent path]. Set to an empty value to omit
                     the header entirely.
  AI_LLM_ATTEMPTS    gate attempts per prompt, >= 1 [default: 3]
  AI_LLM_TIMEOUT     per-call LLM timeout in seconds [default: 180;
                     reasoning models can legitimately think for
                     minutes — a timeout costs one attempt]
  LUME_CORE_GATE     path to lume-core scripts/verify-ai-lume.sh
                     [default: ../lume-core/scripts/verify-ai-lume.sh]
  LUME_CORE_BASH     bash to run the gate with [default: bash; Windows:
                     set to the MSYS2 bash explicitly]
  AI_CANDIDATES_DIR  where accepted programs land [default:
                     out/ai-candidates — the core repo's produce dir;
                     scripts/ai-produce.sh reads it + manifest.txt]

Prompts (argv[1] JSON file, or stdin JSON, or built-in defaults):
  [{"name": "stable-id", "prompt": "what the program should do"}, ...]

Per prompt the loop is:
  1. chat: system spec (lume subset the gate accepts) + the prompt
  2. extract the program from the reply (```lume fence, else raw)
  3. gate it (verify-ai-lume.sh: type check + semantic declarations +
     interpreter/text/llvm equivalence + optional parser-fuzz leg)
  4. reject -> append the gate's stderr (the counter-example) to the
     conversation and retry; accept -> done; error -> infra, no retry

Accepted programs are written to AI_CANDIDATES_DIR/<name>.lume with a
manifest row "<name>|accept|<prompt>" (ai-produce.sh asserts the gate
verdict again — that second opinion is deliberate, it is what catches a
backend drift between the two runs). Rejected-after-retries programs are
NOT written; their final counter-example goes to the run report and to
AI_REJECTS_LOG (default <candidates dir>/../ai-rejects.log) — the same
log the deterministic loop appends to.

Stdout: one JSON run report
  {"model": str, "candidates": int, "ready": [...], "rejected": [
     {"name": str, "attempts": int, "reason": str}], "errors": [...]}
Exit: 0 = every prompt produced an accepted program, 1 = at least one
prompt exhausted its attempts (report carries the counter-examples),
2 = configuration / infra failure (no key, gate missing, LLM unreachable).

Scope guard (TODO P4): this tool is host-side on purpose — the core
stays no-network, key-free, LLM-free. The key lives in the environment
only; nothing here writes it anywhere.
"""
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request


DEFAULT_URL = "http://localhost:19070/v1/chat/completions"
FENCE_RE = re.compile(r"```(?:lume|text)?\s*\n(.*?)```", re.DOTALL)

# The system spec: exactly the subset the gate's oracle accepts. Keep it
# in sync with lume-core's seeded examples (examples/ai-generated/) —
# they are the ground truth the spec paraphrases.
SYSTEM_SPEC = """\
You write programs in the lume language. Output ONE complete program in
a single ```lume code block and nothing else after it. Rules the
machine gate enforces (violating any of them means rejection):

- Pure computation only: no server{...}, no run(), no env(), no HTTP,
  no file I/O. Compute, then print(...) the results.
- Every output value must go through print(...).
- Types: int, float, bool, string; lists T[]; maps; enum + match.
- let bindings infer their type from the initializer. Numeric builtins
  the compiler has no inference entry for (normcdf, tdist_cdf,
  chisq_cdf, erf, gamma, and any other math function you are not sure
  is a core builtin) MUST be bound with an explicit annotation:
      let p: float = tdist_cdf(2.228139, 10.0);
  If unsure a builtin exists, build the value from literals, arithmetic,
  map/filter/reduce/sort/len/push and user functions instead.
- Functions: func name(a: T): R { ... return ...; } — annotate every
  parameter and the return type. Closures are (x) => expr.
- for loops are counted: for (let i = 0; i < n; i = i + 1) { ... }
- Statements end with ;. A program is a single file, no imports.
- Do not invent output formatting: print one value per statement, the
  three compiler backends must agree on the exact bytes.

This is lume, NOT Python: no colons after blocks, no # comments (use //),
no range()/append()/list multiplication, no bare assignments without let.

A complete reference program (the style and constructs to mirror):

```lume
// sieve of Eratosthenes over [2, n)
func sieve(n: int): int[] {
  let primes = [];
  for (let i = 2; i < n; i = i + 1) {
    let is_prime = true;
    for (let j = 2; j * j <= i; j = j + 1) {
      if (i % j == 0) { is_prime = false; }
    }
    if (is_prime) { push(primes, i); }
  }
  return primes;
}
print(sieve(20));
print(len(sieve(20)));
```

Prefer straightforward deterministic programs over clever ones."""


def log(msg: str) -> None:
    print(msg, file=sys.stderr)


def die(msg: str, code: int = 2) -> None:
    print(json.dumps({"error": msg}, ensure_ascii=False))
    sys.exit(code)


def chat(url: str, key: str, model: str, messages: list,
         router_agent: str, timeout: int = 180):
    """One OpenAI-compatible round. Returns (content, None) or (None, err)."""
    body = json.dumps({"model": model, "messages": messages,
                       "temperature": 0.2, "stream": False}).encode("utf-8")
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {key}",  # never printed anywhere
    }
    if router_agent:
        # tsm-hub-style routers only serve direct model traffic when the
        # request opts out of the default agent path.
        headers["X-Llm-Router-Agent"] = router_agent
    req = urllib.request.Request(url, data=body, method="POST",
                                 headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")[:300]
        return None, f"HTTP {exc.code}: {detail}"
    except (urllib.error.URLError, OSError, TimeoutError) as exc:
        return None, f"unreachable: {exc}"
    try:
        return data["choices"][0]["message"]["content"], None
    except (KeyError, IndexError, TypeError):
        return None, f"unexpected response shape: {str(data)[:300]}"


def extract_program(reply: str) -> str:
    """Program source from the model reply: the first fenced block if the
    reply uses one, else the raw reply. Trailing prose after the block is
    dropped with it; prose before a fence does not survive the regex."""
    m = FENCE_RE.search(reply or "")
    src = m.group(1) if m else (reply or "")
    return src.strip() + "\n"


def run_gate(program: str, gate: str, bash: str):
    """Returns (rc, combined_output). rc: 0 accept, 1 reject, 2 infra."""
    fd, path = tempfile.mkstemp(prefix="ai-produce-", suffix=".lume")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(program)
        env = dict(os.environ)
        env.pop("LUME_BIN", None)  # the gate cds to its own repo
        proc = subprocess.run([bash, gate, path], capture_output=True,
                              text=True, env=env, timeout=300)
        return proc.returncode, ((proc.stderr or "") + (proc.stdout or "")).strip()
    except subprocess.TimeoutExpired:
        return 2, "gate timed out after 300s"
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


def load_prompts() -> list:
    raw = None
    if len(sys.argv) > 1:
        with open(sys.argv[1], encoding="utf-8") as fh:
            raw = fh.read()
    elif not sys.stdin.isatty():
        raw = sys.stdin.read()
    if raw and raw.strip():
        items = json.loads(raw)
    else:
        # Built-in defaults mirror the scripted producer's intents — the
        # same surface ai-gen-programs.sh covers, now authored by the LLM.
        items = [
            {"name": "llm-summary-stats",
             "prompt": "计算列表 [2.5, 3.5, 4.0, 5.5, 6.0] 的均值与方差并打印"},
            {"name": "llm-range-sieve",
             "prompt": "用筛法求 30 以内的所有质数, 打印列表与个数"},
        ]
    out = []
    for it in items:
        name, prompt = it.get("name"), it.get("prompt")
        if isinstance(name, str) and name.strip() and \
           isinstance(prompt, str) and prompt.strip():
            out.append({"name": name.strip(), "prompt": prompt.strip()})
    if not out:
        die("no usable prompts: need [{\"name\", \"prompt\"}, ...]")
    for it in out:
        if not re.fullmatch(r"[a-z0-9][a-z0-9_-]*", it["name"]):
            die(f"bad prompt name {it['name']!r}: [a-z0-9][a-z0-9_-]*")
    return out


def main() -> None:
    url = os.environ.get("LLM_API_URL", DEFAULT_URL)
    key = os.environ.get("LLM_API_KEY")
    model = os.environ.get("LLM_MODEL", "auto")
    attempts = max(1, int(os.environ.get("AI_LLM_ATTEMPTS", "3")))
    gate = os.environ.get("LUME_CORE_GATE",
                          os.path.join("..", "lume-core", "scripts",
                                       "verify-ai-lume.sh"))
    bash = os.environ.get("LUME_CORE_BASH", "bash")
    cand_dir = os.environ.get("AI_CANDIDATES_DIR", "out/ai-candidates")
    router_agent = os.environ.get("AI_LLM_ROUTER_AGENT", "off")
    llm_timeout = int(os.environ.get("AI_LLM_TIMEOUT", "180"))

    if not key:
        die("LLM_API_KEY missing — set it in the environment (never "
            "inline); it is read, used for the bearer header and forgotten")
    if not os.path.isfile(gate):
        die(f"lume-core gate not found at {gate} — set LUME_CORE_GATE")

    prompts = load_prompts()
    os.makedirs(cand_dir, exist_ok=True)
    manifest_path = os.path.join(cand_dir, "manifest.txt")
    rejects_log = os.environ.get(
        "AI_REJECTS_LOG", os.path.join(os.path.dirname(cand_dir.rstrip("/")),
                                       "ai-rejects.log"))

    ready, rejected, errors = [], [], []
    for it in prompts:
        name, prompt = it["name"], it["prompt"]
        messages = [{"role": "system", "content": SYSTEM_SPEC},
                    {"role": "user", "content": prompt}]
        verdict, reason, used = None, "", 0
        for attempt in range(1, attempts + 1):
            used = attempt
            content, err = chat(url, key, model, messages, router_agent,
                                timeout=llm_timeout)
            if content is None:
                errors.append({"name": name, "attempt": attempt,
                               "reason": err})
                log(f"error {name} attempt {attempt}: {err}")
                verdict, reason = "error", err
                break
            program = extract_program(content)
            rc, out = run_gate(program, gate, bash)
            if rc == 0:
                verdict, reason = "accept", out or "gate accepted"
                with open(os.path.join(cand_dir, f"{name}.lume"), "w",
                          encoding="utf-8") as fh:
                    fh.write(program)
                with open(manifest_path, "a", encoding="utf-8") as fh:
                    fh.write(f"{name}|accept|{prompt}\n")
                ready.append({"name": name, "attempts": attempt,
                              "file": os.path.join(cand_dir, f"{name}.lume")})
                log(f"accept {name} (attempt {attempt})")
                break
            if rc == 1:
                verdict, reason = "reject", out
                log(f"reject {name} (attempt {attempt}) — retrying with "
                    "the counter-example")
                messages += [
                    {"role": "assistant", "content":
                     f"```lume\n{program}\n```"},
                    {"role": "user", "content":
                     "The machine gate rejected this program. Fix it and "
                     "output the complete corrected program in one ```lume "
                     "block — lume only, never Python (counted for loops, "
                     "let bindings, // comments, push()). Rejection reason "
                     "(counter-example):\n" + out},
                ]
                continue
            errors.append({"name": name, "attempt": attempt, "reason": out})
            verdict, reason = "error", out
            log(f"error {name} attempt {attempt}: {out}")
            break

        if verdict == "reject":
            rejected.append({"name": name, "attempts": used, "reason": reason})
            with open(rejects_log, "a", encoding="utf-8") as fh:
                fh.write(f"---- llm candidate {name} ({used} attempt(s), "
                         f"model {model})\n")
                for ln in reason.splitlines():
                    fh.write(f"    {ln}\n")

    report = {"model": model, "url_host": urllib.parse.urlsplit(url).netloc,
              "router_agent": router_agent or "(omitted)",
              "candidates_dir": cand_dir, "ready": ready,
              "rejected": rejected, "errors": errors}
    print(json.dumps(report, ensure_ascii=False, indent=2))
    if errors:
        sys.exit(2)
    sys.exit(0 if not rejected else 1)


if __name__ == "__main__":
    main()
