#!/usr/bin/env python3
"""External agent tool for the embedded example (examples/embedded.c).

Contract for exec tools (registered via agenthttpd_tool_exec):
  - the tool-call arguments arrive on stdin as raw JSON
  - whatever this script writes to stdout becomes the tool result
  - the process environment carries NO LLM credentials (scrubbed)

This one counts words and characters of {"text": "..."} — deliberately
trivial; the point is the transport, not the logic.
"""
import json
import sys


def main() -> None:
    try:
        args = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError) as exc:
        print(json.dumps({"error": f"invalid JSON arguments: {exc}"}))
        return
    text = args.get("text", "")
    if not isinstance(text, str):
        print(json.dumps({"error": "missing 'text' string argument"}))
        return
    print(json.dumps({"words": len(text.split()), "chars": len(text)}))


if __name__ == "__main__":
    main()
