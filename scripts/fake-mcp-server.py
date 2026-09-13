#!/usr/bin/env python3
"""Minimal NEWLINE-delimited JSON-RPC stdio MCP server for the smoke tests.

Implements initialize / initialized / tools/list / tools/call against
agent-httpd's MCP stdio client (src/agent/mcp.c). One tool:

   echo:{text}  ->  content: [{ "type": "text", "text": "echo: <text>" }]

Usage: fake-mcp-server.py
"""
import json
import sys


def send(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception:
            send({"jsonrpc": "2.0", "id": None,
                  "error": {"code": -32700, "message": "parse error"}})
            continue
        method = req.get("method")
        mid = req.get("id")
        if method == "initialize":
            send({"jsonrpc": "2.0", "id": mid, "result": {
                "protocolVersion": "2025-06-18",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "fake-mcp", "version": "0.0.1"},
            }})
        elif method == "tools/list":
            send({"jsonrpc": "2.0", "id": mid, "result": {"tools": [{
                "name": "pong",
                "description": "Echo the given text back",
                "inputSchema": {"type": "object",
                                "properties": {"text": {"type": "string"}}},
            }]}})
        elif method == "tools/call":
            args = req.get("params", {}).get("arguments", {}) or {}
            params = req.get("params", {})
            args = params.get("arguments", {}) or {}
            send({"jsonrpc": "2.0", "id": mid, "result": {
                "content": [{"type": "text",
                             "text": "echo: " + str(args.get("text", ""))}],
                "isError": False,
            }})
        else:
            # notifications (initialized, etc.) get no reply
            if mid is None:
                continue
            send({"jsonrpc": "2.0", "id": mid,
                  "error": {"code": -32601, "message": "method not found"}})


if __name__ == "__main__":
    main()