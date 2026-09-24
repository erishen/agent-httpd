#!/usr/bin/env bash
# 宿主侧 MCP 中继管理脚本（tsm-hub 容器 -> 宿主桥脚本）。
#
# tsm-hub（OrbStack 容器）的 settings.mcps 把 portfolio-check / pse-review 配成
# `nc host.docker.internal 45001 / 45002` 的 stdio server；宿主要把这两个端口
# 用 mcp-host-relay.mjs 中继到本地桥脚本（asset-lens 管线需要 host 的 node/uv/make）。
#
# 用法:
#   ./start-mcp-relays.sh [start|stop|status|restart]   (缺省 = status)
#
# 中继是纯转发（无鉴权，仅绑定 127.0.0.1），端口/桥路径可用环境变量覆盖：
#   MCP_RELAY_P1 / MCP_RELAY_P2 / PORTFOLIO_CHECK / PSE_REVIEW_BRIDGE
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
RELAY="$DIR/mcp-host-relay.mjs"
LOGDIR="$DIR/logs"
mkdir -p "$LOGDIR"

CANON="${PORTFOLIO_CHECK:-""}"
if [ -z "$CANON" ]; then
  # 默认指向 canonical resolve-skills 桥脚本（与 lume 本地 mcp 配置一致）。
  CANON="$(cd "$DIR/../../../.." && pwd)/work/harness/resolve-skills/skills/weekly-investment/scripts"
fi
P1="${MCP_RELAY_P1:-45001}"
P2="${MCP_RELAY_P2:-45002}"
BRIDGE1="${PORTFOLIO_CHECK:-$CANON/portfolio-check.mjs}"
BRIDGE2="${PSE_REVIEW_BRIDGE:-$CANON/pse-review.mjs}"

listening() { # $1=port -> 0 if bound on 127.0.0.1
  lsof -nP -iTCP:"$1" -sTCP:LISTEN >/dev/null 2>&1
}

start_one() { # $1=port  $2=bridge
  local port="$1" bridge="$2"
  if listening "$port"; then
    echo "[relay] $port already listening — skip"
    return 0
  fi
  if [ -f "$LOGDIR/relay-$port.pid" ] && kill -0 "$(cat "$LOGDIR/relay-$port.pid")" 2>/dev/null; then
    echo "[relay] $port pidfile alive — skip"
    return 0
  fi
  echo "[relay] starting $port -> $(basename "$bridge")"
  nohup node "$RELAY" "$port" "$bridge" \
    >> "$LOGDIR/relay-$port.log" 2>&1 &
  echo $! > "$LOGDIR/relay-$port.pid"
  for _ in 1 2 3 4 5 6 7 8; do
    listening "$port" && break
    sleep 0.5
  done
  listening "$port" \
    && echo "[relay] $port up (pid $(cat "$LOGDIR/relay-$port.pid"))" \
    || { echo "[relay] $port FAILED — see $LOGDIR/relay-$port.log"; kill -9 "$(cat "$LOGDIR/relay-$port.pid")" 2>/dev/null; }
}

stop_one() { # $1=port
  if [ -f "$LOGDIR/relay-$1.pid" ]; then
    local pid; pid="$(cat "$LOGDIR/relay-$1.pid")"
    if kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null && echo "[relay] $1 stopped (pid $pid)"
    else
      echo "[relay] $1 pid $pid already gone"
    fi
    rm -f "$LOGDIR/relay-$1.pid"
  else
    echo "[relay] $1 no pidfile"
  fi
}

status() {
  for port in "$P1" "$P2"; do
    if listening "$port"; then
      local pid
      pid="$(lsof -tiTCP:"$port" -sTCP:LISTEN 2>/dev/null | head -1)"
      echo "[relay] $port listening (pid ${pid:-?})"
    else
      echo "[relay] $port DOWN"
    fi
  done
}

case "${1:-status}" in
  start) start_one "$P1" "$BRIDGE1"; start_one "$P2" "$BRIDGE2";;
  stop)  stop_one "$P1"; stop_one "$P2";;
  restart) stop_one "$P1"; stop_one "$P2"; start_one "$P1" "$BRIDGE1"; start_one "$P2" "$BRIDGE2";;
  status) status ;;
  *) echo "usage: $0 [start|stop|status|restart]"; exit 2 ;;
esac