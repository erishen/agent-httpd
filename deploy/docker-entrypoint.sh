#!/bin/sh
# agent-httpd 容器入口。两种角色:
#   docker-entrypoint           -> 启动 agent-httpd (exec 为 PID 1)
#   docker-entrypoint react-backend -> 启动驻留 React SSR FastCGI 后端
# 以 exec 进服务器进程: docker stop 的 SIGTERM 直达其优雅排水逻辑
# (停 accept -> worker 排水最多 5 秒 -> SIGKILL 兜底)。
set -e

if [ "$1" = "react-backend" ]; then
    SOCK="${REACT_FCGI_SOCK:-/run/agent-httpd/react.sock}"
    mkdir -p "$(dirname "$SOCK")"
    # react-ssr-server 自会 unlink 同路径的 stale socket
    echo "[react-backend] launching: bin/react-ssr-server $SOCK"
    exec bin/react-ssr-server "$SOCK"
fi

FCGI_SOCK="${FCGI_SOCK:-}"
if [ -n "$FCGI_SOCK" ]; then
    mkdir -p "$(dirname "$FCGI_SOCK")"
fi

# 组装命令行 (语义与本地 make start 一致)
ARGS="-p ${PORT:-8080} -w ${WORKERS:-8}"
[ "${RATE_LIMIT:-0}" != "0" ] && ARGS="$ARGS -l $RATE_LIMIT"
[ -n "$FCGI_SOCK" ] && ARGS="$ARGS -F $FCGI_SOCK"
# /react/* 中继到驻留后端 (agent-httpd 作 FCGI 客户端的那条链)
[ -n "${REACT_SOCK:-}" ] && ARGS="$ARGS -R $REACT_SOCK"
# 访问日志: 默认指向镜像内预建的 /var/log/agent-httpd (相对路径 ./logs
# 在容器 WORKDIR 下不存在, 会触发 "cannot open access log" 告警并丢日志)
[ -z "${LOG_FILE:-}" ] && LOG_FILE=/var/log/agent-httpd/access.log
ARGS="$ARGS -L $LOG_FILE"

# 日志轮转侧车 (无 cron/supervisor 的 slim 镜像内自轮转):
# 每 LOG_ROTATE_SECONDS (默认 1 天) mv 一份时间戳副本并 SIGHUP 让服务
# 器重开新文件 (项目原生支持 SIGHUP 重开), 只保留 LOG_ROTATE_KEEP 份。
# 该循环 fork 于 entrypoint、exec 之前, 因此 $PPID 即 exec 后的服务器
# 进程; 服务器退出时循环随之被 reparent/收割, 不留孤儿。
if [ "${LOG_ROTATE_SECONDS:-86400}" != "0" ]; then
(
    sleep "${LOG_ROTATE_SECONDS:-86400}"
    while :; do
        if [ -s "$LOG_FILE" ]; then
            ts=$(date +%Y%m%d-%H%M%S 2>/dev/null || echo rotated)
            mv "$LOG_FILE" "$LOG_FILE.$ts" 2>/dev/null || true
            kill -HUP "$PPID" 2>/dev/null || true
            # prune: keep the newest LOG_ROTATE_KEEP timestamped copies
            keep="${LOG_ROTATE_KEEP:-7}"
            ls -1t "$LOG_FILE".* 2>/dev/null | tail -n +$((keep + 1)) | \
                while IFS= read -r f; do rm -f "$f"; done
        fi
        sleep "${LOG_ROTATE_SECONDS:-86400}"
    done
) &
fi

echo "[httpd] exec bin/agent-httpd $ARGS (docroot: /app/www)"
exec bin/agent-httpd $ARGS
