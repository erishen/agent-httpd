// stdio-over-TCP relay for MCP servers that must run on the HOST.
//
// agent-httpd's MCP client speaks stdio and spawns the server as a child
// *inside* its own process tree. Some MCP bridges (e.g. the data-pipeline
// bridges under a skills root) need a host-native environment (node + uv + make
// + private project dirs) that the container does not have. This relay runs on
// the host: it listens on 127.0.0.1:<port> and, per accepted TCP connection,
// spawns `node <bridge.mjs>` and pipes the socket to the child's stdio. The
// container side uses scripts/mcp-tcp-client.py as the MCP "command", so the
// host bridge appears as an ordinary local stdio server.
//
// Loops back on 127.0.0.1 only: reachable from containers via OrbStack's
// host.docker.internal, but not exposed on the LAN.
//
// Usage: node mcp-host-relay.mjs <port> <bridge.mjs>

import net from 'node:net';
import { spawn } from 'node:child_process';

const port = Number(process.argv[2]);
const script = process.argv[3];
if (!Number.isInteger(port) || port <= 0 || !script) {
  console.error('usage: node mcp-host-relay.mjs <port> <bridge.mjs>');
  process.exit(2);
}

function log(...a) {
  console.log(new Date().toISOString(), ...a);
}

const server = net.createServer({ allowHalfOpen: true }, (sock) => {
  const child = spawn(process.execPath, [script], {
    stdio: ['pipe', 'pipe', 'inherit'],
    env: process.env,
  });
  log(`[conn] -> ${script} (pid ${child.pid})`);

  sock.on('error', () => {});
  child.on('error', (e) => {
    log(`[conn] spawn error: ${e.message}`);
    try { sock.destroy(); } catch {}
  });

  sock.pipe(child.stdin);
  child.stdout.pipe(sock);

  const stopChild = () => {
    try { child.kill('SIGKILL'); } catch {}
  };
  sock.on('close', stopChild);
  child.on('exit', (code, sig) => {
    log(`[conn] child exit code=${code} sig=${sig}`);
    try { sock.end(); } catch {}
  });
});

server.on('error', (e) => {
  log(`[relay] server error: ${e.message}`);
  process.exit(1);
});

server.listen(port, '127.0.0.1', () => {
  log(`[relay] listening 127.0.0.1:${port} -> ${script}`);
});
