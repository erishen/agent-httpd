#!/usr/bin/env bash
# One-off init for the in-container weekly-investment bridges.
#
# Both bridges drive their Python project with `uv run`, and the venvs live in
# named volumes (see run-local-container.sh) so the container never writes a
# Linux .venv into the macOS working tree. That means each venv starts empty and
# has to be populated once; asset-lens in particular runs `uv run --no-sync`, so
# its venv MUST exist before the first portfolio-check call.
#
# Runs `uv sync --frozen` (uses the committed uv.lock, never rewrites it).
# Re-run after a dependency change. Run ON THE HOST, container already up.
set -euo pipefail

export PATH="/usr/local/bin:$PATH"   # OrbStack docker
NAME="${AGENT_HTTPD_NAME:-agent-httpd-local}"
WS=/workspace

sync_one() {
  local dir="$1" label="$2" attempt
  # asset-lens 的依赖树很重 (pandas/numpy/scipy/xgboost/lightgbm … 未缓存时
  # 要下 1GB+), 默认 30s 的 uv HTTP 超时在国内网络下会中途失败 → 放宽到
  # 600s, 并允许重试: 已下载的部分留在 uv 缓存卷里, 重试可续传。
  local tries="${UV_SYNC_ATTEMPTS:-3}"
  for ((attempt = 1; attempt <= tries; attempt++)); do
    echo "--- uv sync: $label ($dir) [attempt $attempt/$tries] ---"
    if docker exec -e UV_HTTP_TIMEOUT="${UV_HTTP_TIMEOUT:-600}" "$NAME" \
         sh -c "cd $dir && uv sync --frozen"; then
      echo "[ok] $label venv ready"
      return 0
    fi
    echo "[warn] $label attempt $attempt failed; retrying if attempts remain…" >&2
  done
  echo "[fail] $label venv sync failed after $tries attempts" >&2
  return 1
}

sync_one "$WS/invest-kit/apps/asset-lens" asset-lens
sync_one "$WS/frameworks/autogen-pse" autogen-pse
echo "all venvs ready"
