#!/usr/bin/env python3
"""External agent tool: run an LLM-written .lume program through the
lume-core AI self-verification gate ("AI writes, machine verifies",
lume-core docs/ai-verify.md — this is the host-side producer half).

Contract for exec tools (registered via agenthttpd_tool_exec), same as
wordcount.py:
  - tool-call arguments arrive on stdin as raw JSON
  - whatever this script writes to stdout becomes the tool result
  - the process environment carries NO LLM credentials (scrubbed)

Arguments (stdin JSON):
  {
    "program": "<.lume source text>",   // required — what the model wrote
    "label":   "<optional stable id>"   // default: <source hash>
  }

Behavior:
  - writes the program to a temp file,
  - runs the lume-core gate on it (verify-ai-lume.sh: type check +
    semantic declarations + interpreter/text/llvm backend equivalence +
    parser-fuzz crash leg),
  - returns the verdict as JSON on stdout.

The gate script is NOT part of this repo; point LUME_CORE_GATE at the
lume-core checkout (default: ../lume-core/scripts/verify-ai-lume.sh,
i.e. the sibling checkout under the same parent). The gate needs the
native toolchain (clang) on PATH to run every backend leg; fewer than
two legs available makes the gate report "cannot judge" (rc=2) — an
infrastructure condition, reported as verdict "error", never blamed on
the program.

Stdout JSON:
  {"verdict": "accept"|"reject"|"error", "reason": str,
   "label": str, "exit": int}
Exit code: 0 = accept, 1 = reject (program's fault; reason is the
counter-example to hand back to the model for retry), 2 = infra error.
"""
import hashlib
import json
import os
import subprocess
import sys
import tempfile


DEFAULT_GATE = os.path.join("..", "lume-core", "scripts", "verify-ai-lume.sh")


def result(verdict: str, reason: str, label: str, code: int) -> None:
    print(json.dumps({"verdict": verdict, "reason": reason,
                      "label": label, "exit": code}, ensure_ascii=False))
    sys.exit(code)


def main() -> None:
    try:
        args = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError) as exc:
        result("error", f"invalid JSON arguments: {exc}", "", 2)
    program = args.get("program")
    if not isinstance(program, str) or not program.strip():
        result("error", "missing 'program' string argument", "", 2)
    label = args.get("label")
    if not isinstance(label, str) or not label.strip():
        label = "src-" + hashlib.sha256(program.encode("utf-8")).hexdigest()[:12]

    gate = os.environ.get("LUME_CORE_GATE", DEFAULT_GATE)
    if not os.path.isfile(gate):
        result("error", f"lume-core gate not found at {gate} — set LUME_CORE_GATE",
               label, 2)

    # Write the model's program to a temp file; the gate reads files.
    fd, path = tempfile.mkstemp(prefix="ai-verify-", suffix=".lume")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(program)
        # LUME_BIN must be the gate's own build (the gate cd's to its repo).
        # The gate is a bash script: run it via $LUME_CORE_BASH (default
        # "bash"). On Windows, set LUME_CORE_BASH to the MSYS2 bash
        # explicitly (PATH resolution otherwise tends to find the WSL
        # stub in System32).
        env = dict(os.environ)
        env.pop("LUME_BIN", None)
        bash = os.environ.get("LUME_CORE_BASH", "bash")
        proc = subprocess.run([bash, gate, path], capture_output=True,
                              text=True, env=env, timeout=180)
    except subprocess.TimeoutExpired:
        result("error", "gate timed out after 180s", label, 2)
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass

    reason = (proc.stderr or proc.stdout or "").strip()
    if proc.returncode == 0:
        result("accept", reason or "gate accepted the program", label, 0)
    if proc.returncode == 1:
        result("reject", reason or "gate rejected the program", label, 1)
    result("error", f"gate could not judge (rc={proc.returncode}): {reason}",
           label, 2)


if __name__ == "__main__":
    main()
