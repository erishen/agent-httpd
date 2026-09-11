#!/usr/bin/env bash
# 轮转 agent-httpd access.log：改名 + SIGHUP 重开文件，gzip 旧档，保留最近 KEEP=7 份。
# 用法：scripts/rotate-logs.sh   （配合 cron / launchd 每周执行）
set -euo pipefail
cd "$(dirname "$0")/.."
LOG_DIR="logs"
LOG="$LOG_DIR/access.log"
KEEP="${KEEP:-7}"

if [ ! -f "$LOG" ] || [ ! -s "$LOG" ]; then
  echo "nothing to rotate ($LOG missing or empty)"
  exit 0
fi

# 定位在跑的 agent-httpd：优先 pidfile，其次按开发/生产端口探测
SERVER_PID=""
for port in 3101 8080; do
  p=$(lsof -tiTCP:"$port" -sTCP:LISTEN 2>/dev/null | head -1 || true)
  [ -n "${p:-}" ] && SERVER_PID="$p" && break
done

TS=$(date +%Y%m%d-%H%M%S)
mv "$LOG" "$LOG_DIR/access-$TS.log"

if [ -n "${SERVER_PID:-}" ]; then
  kill -HUP "$SERVER_PID" 2>/dev/null || true
  sleep 0.3
fi
[ -f "$LOG" ] || : > "$LOG"

# gzip 刚轮出的档并裁剪到 KEEP 份（压缩档含 IP/URL，只给所有者读）
find "$LOG_DIR" -maxdepth 1 -name 'access-*.log' -print0 \
  | while IFS= read -r -d '' f; do gzip -f "$f" 2>/dev/null || true; done
ls -t "$LOG_DIR"/access-*.log.gz 2>/dev/null | tail -n +"$((KEEP + 1))" \
  | while read -r f; do rm -f "$f"; done
chmod 600 "$LOG_DIR"/access-*.log.gz 2>/dev/null || true

echo "rotated $TS (pid=${SERVER_PID:-none}, server signal=${SERVER_PID:-none}, kept=$KEEP)"