# syntax=docker/dockerfile:1
# agent-httpd 教学服务器容器 —— 四阶段构建:
#   阶段 1 (c-build):   用与本地 make 完全相同的 CFLAGS 编译 C 服务器
#   阶段 2 (cgi-build): 容器内编译原生语言 CGI (go/rust/java/php 包装器)
#   阶段 3 (ssr-build): React SSR 三件套 (tsc 类型检查 + esbuild 打包 + Tailwind)
#   阶段 4 (runtime):   Debian + Node 运行时, 附带脚本类 CGI 解释器 + JVM
#
# 用法 (见 docker-compose.yml):
#   docker compose up --build
#     http://localhost:18080  -> agent-httpd (HTTP 直连)
#     http://localhost:18081  -> nginx 反代 (HTTP 反代 + fastcgi_pass 直连演示)

# ---------- 阶段 1: C 服务器 ----------
FROM debian:bookworm-slim AS c-build
RUN apt-get update \
    && apt-get install -y --no-install-recommends gcc libc6-dev libcrypt-dev make \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY src/ src/
# Makefile 按平台区分 -lcrypt/-DHAVE_CRYPT_H, Linux 容器内自动带上
RUN make all

# ---------- 阶段 2: 原生语言 CGI 动物园 (容器内编译, 非宿主机二进制) ----------
# 用 Debian 自带工具链编译 go/rust/java —— 宿主机预编译的 Mach-O 无法在
# Linux 容器运行, 必须在此重新编译。工具链只存在于本阶段, 最终镜像只拿
# 编译产物 (几 MB)。build-cgis.sh 对缺失工具链自动跳过。
FROM debian:bookworm-slim AS cgi-build
RUN apt-get update \
    && apt-get install -y --no-install-recommends golang-go rustc default-jdk-headless php-cgi \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY cgi-langs/ cgi-langs/
COPY scripts/build-cgis.sh scripts/
RUN sh scripts/build-cgis.sh

# ---------- 阶段 3: React SSR 构建产物 ----------
FROM node:20-bookworm-slim AS ssr-build
# 镜像内保持仓库布局 (/build 为仓库根), build-ssr.sh 按自身位置寻址
WORKDIR /build
# 先只拷贝 lockfile 安装依赖, 充分利用层缓存 (源码改动不重装 npm 包)
COPY cgi-bin/react-ssr/package.json cgi-bin/react-ssr/package-lock.json cgi-bin/react-ssr/
RUN cd cgi-bin/react-ssr && npm ci --no-audit --no-fund
COPY cgi-bin/react-ssr/ cgi-bin/react-ssr/
COPY scripts/build-ssr.sh scripts/
# 产物: cgi-bin/react-ssr.cgi (CGI) + bin/react-ssr-server (驻留后端)
#       + www/js/react-ssr.js (浏览器 hydration)
# build-ssr.sh 按仓库布局写入 bin/ 与 www/js, 先建空目录
RUN mkdir -p bin www/js && sh scripts/build-ssr.sh

# ---------- 阶段 4: 运行时 ----------
FROM node:20-bookworm-slim
# bash: shell CGI 解释器 (node:slim 已自带);
# python3/ruby/php-cgi: CGI 动物园的可移植成员; python3 兼做容器 healthcheck;
# default-jre-headless: java.cgi 包装器运行 Main.class 需要 JVM;
# curl: src/agent/llm.c 的 LLM 上游通道 (fork curl -N 流式拉取 chat/completions)。
RUN apt-get update \
    && apt-get install -y --no-install-recommends python3 ruby php-cgi default-jre-headless curl \
    && rm -rf /var/lib/apt/lists/*
# 非 root 运行 (最小权限): 服务器/CGI/MCP 子进程全部以该账号跑。
# 演示功能需要写的位置: /app (docroot + .data 会话存储, fs MCP 的演示根
# MCP_FS_ROOT=/app)、访问日志目录、FastCGI socket 目录、npx 缓存 (HOME)。
RUN useradd --system --uid 10001 --create-home --home-dir /home/agent agent
# MCP 依赖预热: agent 的 MCP 清单由 router 同步生成 (npx 启动配方, 版本钉在
# src/core/router.c 的 k_known_mcps)。npx 冷启动要现场拉包, 国内网络很容易吃满
# MCP init 预算导致全部握手失败。这里 ① registry 指向国内镜像; ② 把与配方
# 完全相同的钉定版本预热进 npx 缓存 (HOME 与运行时用户一致, 运行时命中同一份)。
# MCP server 起来后会在 stdio 上等服务请求, 用 timeout 掐掉, 只为留下缓存;
# 预热失败不阻断构建 (运行时 npx 仍会自行下载, 只是慢)。
ENV NPM_CONFIG_REGISTRY=https://registry.npmmirror.com HOME=/home/agent
RUN for p in server-filesystem server-memory server-sequential-thinking; do \
        timeout 60 npx -y "@modelcontextprotocol/$p@2026.8.31" --help >/dev/null 2>&1 || true; \
    done
WORKDIR /app
# 容器专用的基础 MCP 配置 (echo 演示 server, python3 自包含零下载), 与
# 运行时 router 同步出来的 .data/mcp-servers-router.json 合并加载
# (src/agent/mcp.c 按 id 去重, 先到先得)。宿主机 dev 用 gitignored 的
# .data/mcp-servers.json, 互不影响。
COPY deploy/mcp-servers.container.json .data/mcp-servers.json
COPY scripts/fake-mcp-server.py scripts/
# 技能索引 (src/agent/skills.c) 扫描 <cwd>/skills —— 本地技能 (demo-lab) 来自仓库
# 树, 必须随镜像走, 否则容器里只剩 router 同步的技能, "跑 demo-lab" 这类
# 本地示例在聊天页必然落空。skills/router/ 也会被带上, 但运行时 router
# 每次启动都会重新物化同名文件覆盖之 (内容以 tsm-hub 为准)。
COPY skills/ skills/
# 工作目录即 docroot (agent-httpd 以 cwd 为根, 需要 /app/www)
COPY --from=c-build /src/bin/agent-httpd bin/
COPY --from=ssr-build /build/bin/react-ssr-server bin/
COPY www/ www/
# 客户端 bundle 以 ssr-build 阶段产出为准, 保证与 SSR 服务端同源。预压缩表亲
# 也必须来自同一阶段: 它是浏览器唯一真正执行的那份 (浏览器总带
# Accept-Encoding: gzip), 若留着构建上下文里的旧副本, 就只有非浏览器客户端
# 能拿到新代码 —— 表面"构建成功", 实际所有人都在跑上一次的 bundle。
COPY --from=ssr-build /build/www/js/react-ssr.js www/js/react-ssr.js
COPY --from=ssr-build /build/www/js/react-ssr.js.gz www/js/react-ssr.js.gz
# 脚本类 CGI (bash/python/ruby 源自仓库); 原生类 (go/rust/java/php 包装器)
# 取自 cgi-build 阶段 —— 与容器工具链匹配的新鲜产物
COPY cgi-bin/ cgi-bin/
# react-ssr.cgi 用 ssr-build 的产物覆盖, 与 react-ssr-server 严格同版本
COPY --from=ssr-build /build/cgi-bin/react-ssr.cgi cgi-bin/
COPY --from=cgi-build /build/cgi-bin/go.cgi /build/cgi-bin/rust.cgi \
     /build/cgi-bin/java.cgi /build/cgi-bin/php.cgi cgi-bin/
# php.cgi / java.cgi 包装器按相对路径引用 cgi-langs 源码/类文件。
# 注意必须整树单源 COPY: 多源 COPY 会把各目录"内容"摊平合并进目标,
# 破坏仓库布局 (java/classes 会变成 cgi-langs/classes)
COPY --from=cgi-build /build/cgi-langs cgi-langs/
RUN chmod +x bin/agent-httpd bin/react-ssr-server cgi-bin/*.cgi \
    && mkdir -p /var/log/agent-httpd /run/agent-httpd \
    && chown -R agent:agent /app /var/log/agent-httpd /run/agent-httpd /home/agent
COPY deploy/docker-entrypoint.sh /usr/local/bin/docker-entrypoint
RUN chmod +x /usr/local/bin/docker-entrypoint

ENV PORT=8080 WORKERS=8 RATE_LIMIT=0
EXPOSE 8080
# 非 root 账号跑业务进程 (8080 非特权端口, socket/日志/数据目录已 chown)
USER agent
# exec 进 agent-httpd 成为 PID 1: docker stop 的 SIGTERM 直达其优雅排水逻辑
ENTRYPOINT ["docker-entrypoint"]
