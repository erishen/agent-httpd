#!/usr/bin/env bash
# Start the host-side MCP relays used by the local agent-httpd container.
#
# Run this ON THE HOST (native macOS Terminal), not inside any sandbox: the
# relays must spawn host-native processes (node + uv + make + private project
# dirs) that the container lacks. The agent-httpd container then reaches these
# relays over host.docker.internal and sees the bridges as local stdio servers.
#
# Re-running is safe: ports already listening are left alone.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
WORKSPACE="$(cd "$REPO/../../.." && pwd)"   # .../individular-invest
BRIDGES="$WORKSPACE/work/harness/resolve-skills/skills/weekly-investment/scripts"
LOGDIR="${RELAY_LOG_DIR:-/tmp}"

# uv lives in ~/.local/bin on this machine and is not on the default PATH.
export PATH="$HOME/.local/bin:$PATH"
NODE="${NODE_BIN:-node}"

start() {
  local port="$1" script="$2" name="$3"
  if [ ! -f "$script" ]; then
    echo "!! bridge script not found: $script — skipping $name" >&2
    return 1
  fi
  if lsof -nP -iTCP:"$port" -sTCP:LISTEN >/dev/null 2>&1; then
    echo "port $port already listening — leaving $name as is"
    return 0
  fi
  nohup "$NODE" "$HERE/mcp-host-relay.mjs" "$port" "$script" \
    >"$LOGDIR/mcp-relay-$name.log" 2>&1 &
  echo "started $name (port $port, pid $!, log $LOGDIR/mcp-relay-$name.log)"
}

start 45001 "$BRIDGES/portfolio-check.mjs" portfolio-check
start 45002 "$BRIDGES/pse-review.mjs" pse-review
