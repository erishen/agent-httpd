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
# Basic Auth: 环境变量翻成命令行 flag (与 RATE_LIMIT 同一套路), 因为
# --env-file 部署的人没有地方追加 CLI 参数, 而不设就等于无鉴权 —— 公网实例
# 必须设 AUTH_HTPASSWD。文件不存在/不可读时**直接退出**: 静默降级成开放
# 访问是最糟的失败模式, 宁可容器起不来。
if [ -n "${AUTH_HTPASSWD:-}" ]; then
    if [ ! -r "$AUTH_HTPASSWD" ]; then
        echo "[httpd] fatal: AUTH_HTPASSWD=$AUTH_HTPASSWD 不可读 (挂载了吗?)" >&2
        echo "        生成:  htpasswd -B -c <file> <user>" >&2
        echo "        服务端只接受强哈希 (\$5\$/\$6\$/bcrypt); 弱哈希需 AGENTHTTPD_ALLOW_WEAK_AUTH=1" >&2
        exit 1
    fi
    ARGS="$ARGS -a $AUTH_HTPASSWD"
fi
[ -n "${AUTH_REALM:-}" ] && ARGS="$ARGS -r $AUTH_REALM"
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

# LLM 网关本地转发侧车:
# pse-review 桥从本地项目 .env 读
# ROUTER_BASE_URL=http://127.0.0.1:9070/v1 (宿主上的 LLM 网关),
# 这个值由桥自行解析, 无法用环境变量覆盖。容器内 127.0.0.1 指向容器自身,
# 桥就永远连不上网关 → 起转发把容器内 127.0.0.1:<port> 引到宿主同端口。
# LLM_FORWARD_PORTS 为空格分隔的端口列表 (如 "9070 11434", 后者给 ollama)。
# 侧车随容器生命周期存活, 且 fork 于 exec 之前 (同上面的日志轮转),
# **不需要任何宿主侧的常驻进程**。未设则不转发。
for _p in ${LLM_FORWARD_PORTS:-}; do
(
    exec python3 /app/scripts/tcp-forward.py \
        --listen-port "$_p" \
        --target-host "${LLM_FORWARD_TARGET_HOST:-host.docker.internal}" \
        --target-port "$_p"
) &
done

echo "[httpd] exec bin/agent-httpd $ARGS $* (docroot: /app/www)"
# "$@" 一并转发: 镜像名之后追加的参数过去是被**静默丢弃**的, 于是
# `docker run ... -a /path/htpasswd` 看起来配了鉴权、实际没有。AUTH_HTPASSWD
# 是推荐路径, 但这里不再让额外参数凭空消失。
exec bin/agent-httpd $ARGS "$@"
