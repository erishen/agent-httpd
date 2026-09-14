#!/usr/bin/env bash
# Bring up the local agent-httpd container exactly as configured, including:
#   - deploy/.env.local            (LLM_* -> local tsm-hub over host.docker.internal)
#   - resolve-skills souls         (PSE role prompts, read-only)
#   - scripts/mcp-tcp-client.py    (stdio->TCP MCP client used inside the container)
#   - deploy/mcp-servers.local.json (echo + portfolio-check + pse-review MCP servers)
#
# The two host relays must be up first (deploy/start-host-relays.sh, or the
# launchd agents from deploy/install-relay-launchd.sh) or the MCP handshake for
# portfolio-check / pse-review fails and those tools are not registered.
#
# Run ON THE HOST.
set -euo pipefail

export PATH="/usr/local/bin:$PATH"   # OrbStack docker
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
WORKSPACE="$(cd "$REPO/../../.." && pwd)"   # .../individular-invest
IMAGE="${AGENT_HTTPD_IMAGE:-agent-httpd:local-arm64}"
NAME="${AGENT_HTTPD_NAME:-agent-httpd-local}"
PORT="${AGENT_HTTPD_PORT:-12080}"
SOULS="$WORKSPACE/work/harness/resolve-skills/souls"

# Make sure the host relays are listening (starts them if not).
bash "$HERE/start-host-relays.sh" || true

docker rm -f "$NAME" >/dev/null 2>&1 || true
docker run -d --name "$NAME" -p "${PORT}:8080" \
  --env-file "$HERE/.env.local" \
  -v "$SOULS":/app/souls:ro \
  -v "$REPO/scripts/mcp-tcp-client.py":/app/scripts/mcp-tcp-client.py:ro \
  -v "$HERE/mcp-servers.local.json":/app/.data/mcp-servers.json:ro \
  --restart unless-stopped "$IMAGE" >/dev/null

echo "started $NAME on http://localhost:$PORT/react/chat"
sleep 3
echo "--- MCP registration ---"
docker logs "$NAME" 2>&1 | grep -E "\[mcp\] (tool|.*resident|.*handshake)" | tail -8 || true
