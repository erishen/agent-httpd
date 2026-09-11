#!/usr/bin/env python3
"""Basic CGI example for AgentHTTPD, written in Python (stdlib only).

Echoes GET query-string / POST form fields using urllib.parse for
application/x-www-form-urlencoded and email.parser for multipart/form-data.
Note: the stdlib `cgi` module was removed in Python 3.13+, so we parse by hand.
"""

import html
import os
import sys
from email.parser import BytesParser
from urllib.parse import parse_qs, unquote

CONTENT_TYPE = os.environ.get("CONTENT_TYPE", "")
CONTENT_LENGTH = int(os.environ.get("CONTENT_LENGTH", "0") or "0")
REQUEST_METHOD = os.environ.get("REQUEST_METHOD", "GET").upper()
QUERY_STRING = os.environ.get("QUERY_STRING", "")

# --- request body / query parsing -------------------------------------------

def read_body(length):
    if length <= 0:
        return b""
    return sys.stdin.buffer.read(length)


def parse_urlencoded(raw: bytes):
    text = raw.decode("utf-8", "replace")
    return {k: v[-1] for k, v in parse_qs(text, keep_blank_values=True).items()}


def parse_multipart(body: bytes, boundary: str):
    # Use email's parser on a synthesized MIME multipart payload.
    msg = BytesParser().parsebytes(
        b"Content-Type: multipart/form-data; boundary=" + boundary.encode() + b"\r\n\r\n" + body
    )
    params = {}
    for part in msg.get_payload():
        cdisp = part.get("Content-Disposition", "")
        name = None
        for tok in cdisp.split(";"):
            tok = tok.strip()
            if tok.lower().startswith("name="):
                name = unquote(tok[len("name="):].strip('"'))
        if name:
            params[name] = part.get_payload(decode=True).decode("utf-8", "replace")
    return params


def get_params():
    if REQUEST_METHOD == "POST":
        body = read_body(CONTENT_LENGTH)
        lower_type = CONTENT_TYPE.lower()
        if "multipart/form-data" in lower_type and "boundary=" in lower_type:
            boundary = lower_type.split("boundary=", 1)[1].split(";", 1)[0].strip('"')
            return parse_multipart(body, boundary)
        return parse_urlencoded(body)
    if QUERY_STRING:
        return {k: v[-1] for k, v in parse_qs(QUERY_STRING, keep_blank_values=True).items()}
    return {}


params = get_params()
method_label = "POST" if REQUEST_METHOD == "POST" else "GET"

body = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Python CGI</title>
<style>
  * {{ box-sizing: border-box; }}
  body {{ font-family: system-ui, -apple-system, Segoe UI, Roboto, sans-serif;
          margin: 0; background: #0f172a; color: #e2e8f0; line-height: 1.6; }}
  .wrap {{ max-width: 640px; margin: 0 auto; padding: 40px 20px; }}
  h1 {{ margin: 0 0 4px; font-size: 32px; color: #fbbf24; }}
  .sub {{ color: #94a3b8; margin: 0 0 24px; }}
  .card {{ background: #1e293b; border: 1px solid #334155; border-radius: 12px; padding: 20px 24px; margin: 16px 0; }}
  table {{ width: 100%; border-collapse: collapse; }}
  th, td {{ text-align: left; padding: 6px 8px; border-bottom: 1px solid #334155; }}
  code {{ background: #0f172a; padding: 1px 5px; border-radius: 4px; color: #38bdf8; }}
  label {{ display: block; margin: 8px 0 2px; color: #94a3b8; font-size: 14px; }}
  input {{ width: 100%; padding: 8px 10px; border-radius: 8px; border: 1px solid #475569;
           background: #0f172a; color: #e2e8f0; margin-top: 4px; }}
  button {{ margin-top: 14px; background: #fbbf24; color: #422006; border: none;
            padding: 9px 18px; border-radius: 8px; font-weight: 700; cursor: pointer; }}
  button:hover {{ background: #fde68a; }}
</style>
</head>
<body><div class="wrap">
  <h1>Python CGI</h1>
  <p class="sub">Rendered by a Python script executed as CGI behind agent-httpd</p>

  <div class="card">
    <h3>Request info</h3>
    <table>
      <tbody>
        <tr><td>Method</td><td>{method_label}</td></tr>
        <tr><td>Content-Type</td><td>{html.escape(CONTENT_TYPE) or "(none)"}</td></tr>
        <tr><td>Python</td><td>{sys.version.split()[0]}</td></tr>
      </tbody>
    </table>
  </div>

  <div class="card">
    <h3>Received {method_label} parameters ({len(params)})</h3>
"""

if not params:
    body += (
        "    <p>No parameters. Try <code>?name=Alice</code> or submit the form below.</p>\n"
    )
else:
    body += "    <table><thead><tr><th>Key</th><th>Value</th></tr></thead><tbody>\n"
    for k, v in params.items():
        body += f"      <tr><td>{html.escape(k)}</td><td>{html.escape(v)}</td></tr>\n"
    body += "    </tbody></table>\n"

body += f"""  </div>

  <div class="card">
    <h3>Try it</h3>
    <form method="{method_label.lower()}" action="/cgi-bin/python.cgi">
      <label>Name <input type="text" name="name" placeholder="Alice"></label>
      <label>Message <input type="text" name="message" placeholder="Hello from Python"></label>
      <button type="submit">Submit via {method_label}</button>
    </form>
    <p><a href="/cgi-bin/python.cgi?name=SRR&message=query+string">Or hit it with a GET + query string</a></p>
  </div>
</div></body></html>
"""

sys.stdout.write("Content-Type: text/html; charset=utf-8\n\n")
sys.stdout.write(body)
