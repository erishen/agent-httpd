#!/usr/bin/env python3
"""容器内的本地端口转发：把宿主上的服务暴露成容器内的 127.0.0.1:<port>。

为什么需要它：weekly-investment 的 pse-review 桥从 autogen-pse/.env 读
ROUTER_BASE_URL（= http://127.0.0.1:9070/v1，宿主上的 tsm-hub / llm-router
网关），而这个值是由桥自己解析、无法用环境变量覆盖。容器内 127.0.0.1 指向
容器自身，桥就永远连不上网关。

这个转发器在容器内监听 127.0.0.1:<port>，把每个连接中继到
<target_host>:<target_port>（默认 host.docker.internal:9070，即宿主），
于是桥里写死的 127.0.0.1 地址照常可用。

由 docker-entrypoint.sh 在 exec 服务器之前拉起，生命周期 = 容器生命周期，
**不需要任何宿主侧的常驻进程**（这正是替换 stdio-over-TCP 宿主中继的原因）。

用法:
  tcp-forward.py [--listen-host 127.0.0.1] [--listen-port 9070] \
                 [--target-host host.docker.internal] [--target-port 9070]
环境变量（命令行参数优先）:
  LLM_FORWARD_LISTEN_HOST / LLM_FORWARD_PORT /
  LLM_FORWARD_TARGET_HOST / LLM_FORWARD_TARGET_PORT
"""
import argparse
import os
import socket
import sys
import threading

BUF = 65536


def _pump(src: socket.socket, dst: socket.socket) -> None:
    """单向搬运，直到任一端关闭；随后双向 shutdown，让对端也收到 EOF。"""
    try:
        while True:
            data = src.recv(BUF)
            if not data:
                break
            dst.sendall(data)
    except OSError:
        pass
    finally:
        for s in (src, dst):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                s.close()
            except OSError:
                pass


def _handle(client: socket.socket, target_host: str, target_port: int) -> None:
    try:
        upstream = socket.create_connection((target_host, target_port), timeout=10)
    except OSError as e:
        sys.stderr.write(
            f"[tcp-forward] upstream {target_host}:{target_port} unreachable: {e}\n"
        )
        sys.stderr.flush()
        client.close()
        return
    upstream.settimeout(None)
    client.settimeout(None)
    threading.Thread(target=_pump, args=(client, upstream), daemon=True).start()
    threading.Thread(target=_pump, args=(upstream, client), daemon=True).start()


def main() -> int:
    ap = argparse.ArgumentParser(description="local TCP forwarder for the container")
    ap.add_argument("--listen-host", default=os.environ.get("LLM_FORWARD_LISTEN_HOST", "127.0.0.1"))
    ap.add_argument("--listen-port", type=int, default=int(os.environ.get("LLM_FORWARD_PORT", "9070")))
    ap.add_argument("--target-host", default=os.environ.get("LLM_FORWARD_TARGET_HOST", "host.docker.internal"))
    ap.add_argument("--target-port", type=int, default=int(os.environ.get("LLM_FORWARD_TARGET_PORT", "9070")))
    a = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.bind((a.listen_host, a.listen_port))
    except OSError as e:
        sys.stderr.write(f"[tcp-forward] bind {a.listen_host}:{a.listen_port} failed: {e}\n")
        return 1
    srv.listen(64)
    sys.stderr.write(
        f"[tcp-forward] {a.listen_host}:{a.listen_port} -> {a.target_host}:{a.target_port}\n"
    )
    sys.stderr.flush()

    while True:
        try:
            client, _ = srv.accept()
        except OSError:
            continue
        threading.Thread(target=_handle, args=(client, a.target_host, a.target_port), daemon=True).start()


if __name__ == "__main__":
    sys.exit(main())
