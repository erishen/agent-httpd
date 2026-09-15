#!/usr/bin/env bash
# Bring up the local agent-httpd container with the weekly-investment MCP bridges
# running INSIDE the container — no host-side relay, no launchd, nothing to keep
# alive on the host:
#
#   deploy/.env.local                        LLM_* -> local tsm-hub
#   resolve-skills souls                     PSE role prompts (read-only)
#   deploy/mcp-servers.container-local.json  echo + portfolio-check + pse-review
#   scripts/tcp-forward.py + LLM_FORWARD_PORTS
#                                            container-side forwards so the
#                                            bridge's hard-coded 127.0.0.1:9070
#                                            reaches the host gateway
#   asset-lens / autogen-pse (bind mounts)   the Python projects the bridges drive
#   named volumes                            Linux .venv + uv package cache kept
#                                            out of the macOS tree and reused
#                                            across container rebuilds
#
# Both bridges drive their project with `uv run`, and asset-lens uses
# `uv run --no-sync`, so each project needs a one-off `uv sync` first — run
# deploy/init-mcp-venvs.sh after the container is up.
#
# Run ON THE HOST.
set -euo pipefail

export PATH="/usr/local/bin:$PATH"   # OrbStack docker
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
WORKSPACE="$(cd "$REPO/../../.." && pwd)"   # <workspace root>
IMAGE="${AGENT_HTTPD_IMAGE:-agent-httpd:local-arm64}"
NAME="${AGENT_HTTPD_NAME:-agent-httpd-local}"
PORT="${AGENT_HTTPD_PORT:-12080}"
AGENT_UID=10001
WS=/workspace

SOULS="$WORKSPACE/work/harness/resolve-skills/souls"
SKILLS="$WORKSPACE/work/harness/resolve-skills/skills/weekly-investment/scripts"
ASSET_LENS="$WORKSPACE/invest-kit/apps/asset-lens"
AUTOGEN_PSE="$WORKSPACE/frameworks/autogen-pse"
STUDIO_SANDBOX="$WORKSPACE/work/harness/resolve-studio/sandbox"

VOL_VENV_ASSET=agent-httpd-venv-asset-lens
VOL_VENV_PSE=agent-httpd-venv-autogen-pse
VOL_UV_CACHE=agent-httpd-uv-cache

# Named volumes default to root:root, but the container runs as uid 10001 — give
# the venv/cache volumes an owner once, otherwise `uv sync` cannot write them.
for v in "$VOL_VENV_ASSET" "$VOL_VENV_PSE" "$VOL_UV_CACHE"; do
  docker volume create "$v" >/dev/null
  docker run --rm -u 0:0 -v "$v":/v --entrypoint sh "$IMAGE" \
    -c "chown -R $AGENT_UID:$AGENT_UID /v" >/dev/null
done

docker rm -f "$NAME" >/dev/null 2>&1 || true
# Bind loopback by default: this instance drives the weekly-investment bridges,
# so it can read REAL holdings — publishing on 0.0.0.0 would put that on the
# LAN (café wifi, hotel network). Override with AGENT_HTTPD_BIND only when you
# actually need it reachable from another machine.
BIND="${AGENT_HTTPD_BIND:-127.0.0.1}"
docker run -d --name "$NAME" -p "${BIND}:${PORT}:8080" \
  --env-file "$HERE/.env.local" \
  -e ASSET_LENS_DIR="$WS/invest-kit/apps/asset-lens" \
  -e AUTOGEN_PSE_DIR="$WS/frameworks/autogen-pse" \
  -e RESOLVE_STUDIO_DIR="$WS/work/harness/resolve-studio" \
  -e LLM_FORWARD_PORTS="9070 11434" \
  -v "$SOULS":/app/souls:ro \
  -v "$SKILLS":"$WS"/work/harness/resolve-skills/skills/weekly-investment/scripts:ro \
  -v "$ASSET_LENS":"$WS"/invest-kit/apps/asset-lens \
  -v "$AUTOGEN_PSE":"$WS"/frameworks/autogen-pse \
  -v "$VOL_VENV_ASSET":"$WS"/invest-kit/apps/asset-lens/.venv \
  -v "$VOL_VENV_PSE":"$WS"/frameworks/autogen-pse/.venv \
  -v "$VOL_UV_CACHE":/home/agent/.cache/uv \
  -v "$STUDIO_SANDBOX":"$WS"/work/harness/resolve-studio/sandbox \
  -v "$HERE/mcp-servers.container-local.json":/app/.data/mcp-servers.json:ro \
  -v "$REPO/scripts/tcp-forward.py":/app/scripts/tcp-forward.py:ro \
  --restart unless-stopped "$IMAGE" >/dev/null

echo "started $NAME on http://localhost:$PORT/react/chat"
echo "next: bash $HERE/init-mcp-venvs.sh   (one-off uv sync for both bridges)"
sleep 3
echo "--- MCP registration ---"
docker logs "$NAME" 2>&1 | grep -E "\[mcp\] (tool|.*resident|.*handshake)" | tail -8 || true
