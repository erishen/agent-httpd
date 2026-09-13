#!/usr/bin/env node
/* Node host for the embedded agent-httpd example.
 *
 * This is the "Node calls the C framework" integration shown in
 * docs/FRAMEWORK.md: a Node process owns the lifecycle of the C server
 * (spawn / health-check / shutdown) and talks to it over plain HTTP.
 * Process boundary is deliberate — the C server keeps its own prefork
 * worker pool and signal handling; a crash on either side never takes
 * the other down. (Linking libagenthttpd.a into a N-API addon was
 * considered and rejected: agenthttpd_run() blocks + prefork workers
 * fight libuv's event loop.)
 *
 * Run:   make example-node      (or: make example && node examples/node-host/host.js)
 *
 * The script:
 *   1. spawns examples/embedded (cwd = repo root: the "wordcount" exec
 *      tool invokes "python3 examples/tools/wordcount.py" relative to cwd)
 *   2. polls /api/status until the server answers (10s budget)
 *   3. runs four probes: status / echo / real tool dispatch / 404 fallback
 *   4. shuts the server down with SIGTERM and reports a pass/fail table
 */

"use strict";

const { spawn } = require("node:child_process");
const path = require("node:path");

const ROOT = path.resolve(__dirname, "..", "..");
const BIN = path.join(ROOT, "examples", "embedded");
const BASE = "http://127.0.0.1:18101";
const READY_TIMEOUT_MS = 10_000;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function waitUntilReady() {
  const deadline = Date.now() + READY_TIMEOUT_MS;
  while (Date.now() < deadline) {
    try {
      const res = await fetch(`${BASE}/api/status`);
      if (res.ok) return;
    } catch {
      /* not up yet — retry */
    }
    await sleep(200);
  }
  throw new Error(`server not ready within ${READY_TIMEOUT_MS / 1000}s`);
}

async function probeStatus() {
  const res = await fetch(`${BASE}/api/status`);
  const body = await res.json();
  return {
    ok: res.status === 200 && body.ok === true && body.server === "embedded-example",
    detail: `HTTP ${res.status} ${JSON.stringify(body)}`,
  };
}

async function probeEcho() {
  const text = "hello from the node host";
  const res = await fetch(`${BASE}/api/echo`, { method: "POST", body: text });
  const body = await res.json();
  return {
    ok: res.status === 200 && body.bytes === Buffer.byteLength(text) && body.text === text,
    detail: `HTTP ${res.status} ${JSON.stringify(body)}`,
  };
}

/* Goes through the real tools_dispatch path — the same one the LLM loop
 * uses — into the external Python tool process. No model needed. */
async function probeTool() {
  const res = await fetch(`${BASE}/api/tool`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ text: "one two three four" }),
  });
  const body = await res.json();
  return {
    ok: res.status === 200 && body.words === 4 && body.chars === 18,
    detail: `HTTP ${res.status} ${JSON.stringify(body)}`,
  };
}

/* Unregistered path must fall through to the built-in 404. */
async function probe404() {
  const res = await fetch(`${BASE}/api/nope`);
  return { ok: res.status === 404, detail: `HTTP ${res.status} (expected 404)` };
}

async function main() {
  if (process.platform !== "win32") {
    require("node:fs").accessSync(BIN); // throws with a clear message if missing
  }

  console.log(`node-host: spawning ${BIN} (cwd=${ROOT})`);
  const child = spawn(BIN, [], { cwd: ROOT, stdio: ["ignore", "inherit", "inherit"] });
  child.on("error", (err) => {
    console.error(`node-host: failed to spawn server: ${err.message}`);
    process.exit(1);
  });

  const probes = [
    ["GET  /api/status", probeStatus],
    ["POST /api/echo", probeEcho],
    ["POST /api/tool (real dispatch)", probeTool],
    ["GET  /api/nope -> 404", probe404],
  ];
  const results = [];
  let failed = false;

  try {
    await waitUntilReady();
    console.log("node-host: server ready, running probes\n");
    for (const [name, fn] of probes) {
      try {
        const { ok, detail } = await fn();
        results.push([ok ? "PASS" : "FAIL", name, detail]);
        if (!ok) failed = true;
      } catch (err) {
        results.push(["FAIL", name, err.message]);
        failed = true;
      }
    }
  } catch (err) {
    console.error(`node-host: ${err.message}`);
    failed = true;
  } finally {
    if (!child.killed && child.exitCode === null) {
      child.kill("SIGTERM");
      const gone = await Promise.race([
        new Promise((r) => child.on("exit", r)),
        sleep(3000).then(() => false),
      ]);
      if (gone === false) child.kill("SIGKILL");
    }
  }

  console.log("");
  for (const [status, name, detail] of results) {
    console.log(`  [${status}] ${name.padEnd(32)} ${detail}`);
  }
  console.log(failed ? "\nnode-host: FAILED" : "\nnode-host: all probes passed");
  process.exitCode = failed ? 1 : 0;
}

main();
