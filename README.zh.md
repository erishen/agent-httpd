# AgentHTTPD

[English](README.md) | [简体中文](README.zh.md)

一个用 Linux C 实现的轻量级 HTTP 服务器。项目最初受 ACME Labs `mini_httpd`
的启发，如今已成长为完全独立的实现：事件循环 + worker 池、CGI 动物园、
FastCGI 双向、React SSR 编排，以及**原生 C 的 LLM Agent 栈**（工具调用 /
MCP / 技能 / 记忆 / ReAct / PSE）。
想深入源码设计（进程模型 / 快慢路径 / Agent 栈 / 安全纵深），请读
[架构文档](docs/ARCHITECTURE.md)。

## 特性

- **HTTP 方法**: GET / HEAD / POST / PUT / PATCH / DELETE / OPTIONS —— 读方法走静态链, 写方法 (POST/PUT/PATCH/DELETE) 仅在 CGI 路径放行并经 `REQUEST_METHOD` 透传给脚本 (静态 URL 一律 405 + `Allow`, 服务器不写盘); OPTIONS 由服务器直答 `Allow` 能力菜单 (静态列只读方法, CGI 列全方法), 无 body 的 OPTIONS 进快路径; 其余方法 501
- **静态文件服务**: 支持 HTML, CSS, JS, 图片等常见文件类型
- **CGI 支持**: `fork + pipe + execl`，转发 GET query 和 POST body，传递标准 CGI 环境变量
- **目录穿越防护**: `realpath()` 校验，阻止 `..` 逃逸 Web 根目录
- **目录列表**: 访问目录时生成 HTML 文件列表，目录无尾斜杠自动 `301` 重定向
- **自定义错误页**: `www/error/NNN.html` 覆盖，缺失时用内置默认页
- **访问日志**: Combined Log Format，`SIGHUP` 重开日志文件（支持 logrotate）
- **多进程模型 (默认: master 事件循环 + 慢路径 worker 池)**: 快路径 (静态/health/304/301/404, keep-alive 多路复用) 由单进程事件循环直服 (kqueue/epoll, `recv(MSG_PEEK)` 预览请求头零拷贝分流); CGI / chat SSE / 上游代理等阻塞请求经 SCM_RIGHTS 交给 8 个 prefork worker (`-w <n>` 调大小, `0` = 每连接 fork 旧模型)。实测 per-conn 吞吐约 6 倍 (见"性能设计")
- **MIME 类型检测**: 按扩展名识别内容类型
- **大文件流式下发**: 超过 64KB 的静态文件不再整体缓冲, 直接流式发送 (HTTP 与 FastCGI 均支持)
- **CGI 大响应流式**: CGI 输出缓冲按需增长, 超过阈值 (默认 512KB) 落盘临时文件流式发送, 不再有 64KB 截断
- **Expect: 100-continue (RFC 9110 10.1.1)**: 大体积 POST (curl 超 1KB 自动携带) 不再卡 1 秒 —— 服务器在读 body 前即时下发 `100 Continue` (实测 1.06s → 0.04s); 413 拒绝路径不受影响, 客户端直接收到最终状态
- **Date 响应头**: 每个响应携带 RFC 9110 要求的 IMF-fixdate `Date:` 头
- **CGI 变量补齐 (RFC 3875)**: 脚本可依赖 `GATEWAY_INTERFACE` / `SERVER_SOFTWARE` / `REMOTE_ADDR` (FCGI 后端路径取 nginx 透传的 `REMOTE_ADDR`)
- **超时防护栏**: 请求头/POST 体读取有总超时 (半开请求直接断开, 防 Slowloris); CGI 子进程静默超时被 kill 并回 504, 客户端提前断开也会及时终止 CGI, 不再泄漏进程
- **chunked 请求体 (RFC 9110 8.7)**: `Transfer-Encoding: chunked` 的 POST/PUT/PATCH 就地解码为普通 body (含 trailer 段处理与 `Expect: 100-continue` 握手), 解码后 CGI stdin / chat / FCGI 中继零改动; 与 `Content-Length` 并存的走私形请求按 RFC 6.1 直接 400 断连, 不可解码的传输编码 (如 TE: gzip) 回 501
- **URL query string 兼容**: 静态文件与目录 URL 带 `?query` 正常服务 (此前会 404), `/cgi-bin` 重定向与目录列表同样兼容
- **ETag/304 条件请求 (RFC 7232)**: 静态资源 (含预压缩 `.gz` 表亲) 返回强校验 `ETag: W/"size-mtime"`, 带 `If-None-Match` 再验证未变时回 `304` (实测重复访问传输量 -94%); 304 按 RFC 9110 15.4.5 省略实体头 (Content-Type/Content-Length/Accept-Ranges 不随 304 下发, 避免代理歧义); `If-None-Match` 列表/`*` 通配、跨表示不误命中 (gzip 与明文 ETag 不同, 修改后自动失效)
- **显式缓存策略**: 所有静态响应 (200/206/304) 携带 `Cache-Control: no-cache`, CGI 输出与 SSR 文档一律 `no-store` (脚本自己声明了 `Cache-Control` 就透传它 —— 默认值只是兜底, 永不覆盖)。docroot 的 URL 跨重建始终不变, 所以允许存副本、但每次复用前必须再验证 —— ETag 让这次再验证只花一个 304。不加这个头并非中立: 只带 `Last-Modified` 时浏览器会套用启发式新鲜期 (文件年龄的 10%), 于是能在数小时内一直用旧副本、一次都不问服务器; gzip 表亲还会放大这一点 —— 它的 `Last-Modified` 是 `.gz` 文件构建时的 mtime, 而不是它源文件的
- **带内容指纹的 bundle URL**: `scripts/build-ssr.sh` 先算客户端 bundle 的摘要, 再把摘要嵌进两个服务端 bundle, 于是 SSR 文档引用的是 `/js/react-ssr.js?v=<hash>`。缓存策略只能约束「从此以后」的响应: 浏览器**早已存下**的副本自带它被写入时的规则, 没有任何响应头能把那份副本撤回。URL 随字节变化则直接绕开整个问题 —— 新文档要的是一个客户端从没见过的 URL。无指纹的路径仍然照常再验证, 老客户端与直接抓取不受影响
- **一次性清缓存**(可选开关 `PURGE_CLIENT_CACHE=1`): 文档响应会额外带 `Clear-Site-Data: "cache"`, 让浏览器丢掉本 origin 的 HTTP 缓存 —— 这是修复「策略存在之前就被污染的浏览器缓存」的唯一办法。默认关闭; `docker-compose.yml` 里打开, 等所有客户端都访问过一次后就该关回去 (开着期间每次访问都会重清一次)。只对文档下发: 连静态资源一起清的话, 每看一页就会把 bundle 扔掉一次
- **Last-Modified / If-Modified-Since (RFC 9110 13.2.2)**: 所有静态 200 响应携带 `Last-Modified` (IMF-fixdate); 只认日期的客户端 (部分 CDN/老代理/`curl -z`) 用 `If-Modified-Since` 再验证同样能拿到 304 —— 包括每天都会发生的那种: 把我们刚发的 `Last-Modified` 原样回放 (比较保留时分秒, 因此「相等」即视为未修改); 客户端带 `If-None-Match` 时它让位 (规范优先级)
- **TCP_NODELAY + listen backlog 128**: 每个已接受连接显式关 Nagle (避免头/体两次 send 与延迟 ACK 相互作用引入的数十毫秒首字节停顿), 监听队列从 10 提到 128 承受突发连接
- **sendfile(2) 零拷贝 + Range/206 (RFC 9110 14)**: 大静态文件与 206 字节区间响应走内核零拷贝 (macOS/Linux, 其他平台 fread 回退); 支持单区间 `bytes=N-M`/`N-`/`-N` (多区间回退 200 全量, gzip 协商忽略 Range), 越界回 `416` + `Content-Range: bytes */TOTAL`, 响应携带 `Accept-Ranges: bytes`
- **优雅停机 (排水)**: SIGTERM 后 worker 先完成手头连接再退出 (keep-alive 循环见停机标志即关连接), 父进程排水最多 5 秒后强杀兼底 —— 重启不再把进行中的请求拦腰砍断
- **/health 探活端点**: 固定返回 `ok` (位于限流/认证门禁之内), 负载均衡和监控不用再猜 `GET /`
- **/metrics 可观测端点**: Prometheus 文本格式快照——快/慢路径分流计数、状态码分类、worker 池水位、agent 并发槽占用与上限、上游重试次数、CGI 超时/断开、运行时长; 计数表在 `MAP_SHARED` 共享内存 (master 与 worker 同表, 与限流表同模式), 增量走 `__atomic_fetch_add` 快路径零锁; 端点由快路径直答, chat 挂起时抓取不阻塞
- **431 Request Header Fields Too Large**: 请求头超出 64KB 缓冲时回 431 并断连 (此前落到笼统的 400)
- **gzip 预压缩协商**: 静态文件旁存在 `.gz` 时, 客户端带 `Accept-Encoding: gzip` 自动下发压缩版 (`Content-Encoding: gzip`, MIME 保持原扩展名); 按 RFC 7231 解析 q 值 (`gzip;q=0` 拒绝, 显式 `*` 通配接受, deflate-only 不误发); 构建时用 `gzip -9` 预压缩 client bundle。浏览器**总**带 gzip, 所以表亲才是它真正执行的那份 —— 镜像里这份必须来自生成明文 bundle 的同一个构建阶段 (`.dockerignore` 挡住随 `COPY www/` 混进来的旧表亲); `make test-container` 比对解压后的**字节**而不只是长度
- **CGI 响应头 (RFC 3875)**: 脚本输出的 `Location:` 转为 302 重定向 (本地/绝对 URL 均可, 响应头透传), `Status:` 覆盖响应状态行 (如 `Status: 418 I am a teapot`), `Content-Type` 照旧透传
- **FastCGI 后端**: 可选 `-F <unix-socket>`，充当 FCGI 服务端（类似 PHP-FPM），复用同一套 CGI/静态分发链
- **FastCGI 客户端转发**: 可选 `-R <unix-socket>`，把 `/react/*` 转发给常驻 React FastCGI 后端; 新的统一接线推荐 `-v <port>` (见下条), FastCGI 保留供 nginx `fastcgi_pass` 场景
- **统一 `-v` 渲染接线 (dev/prod 同构)**: agent-httpd 是唯一公网入口, `/@*`、`/src/*`、`/react/*`(除 chat) 经 C 反向代理转发给内部上游——dev 是 Vite (脚本 `dev-server.js`), 生产是 `bin/react-ssr-server` 的 HTTP 端口 (`REACT_HTTP_PORT`, 带 ISR 缓存); HMR WebSocket 升级由 C 做 TCP 隧道透传; 代理按 RFC 9110 7.6.1 剥离 hop-by-hop 头 (Connection/TE/Keep-Alive/Proxy-*/Upgrade/Trailer), 同时防请求走私 (客户端注入的 `Transfer-Encoding` 不透传, 帧化权威始终在 C 侧); `/react/api/chat` 始终由 C 直服
- **安全头与指纹统一 (直服/代理同构)**: C 直服响应带 `X-Content-Type-Options: nosniff` / `X-Frame-Options: DENY` / `Referrer-Policy: no-referrer`; `-v` 代理路径的上游 (dev Vite 中间件与 react-ssr-server HTTP 模式) 输出同套安全头, `Server` 指纹统一为 `AgentHTTPD` 且关闭 `X-Powered-By`; 对外错误文案脱敏 (curl 退出码 / `LLM_API_URL` 排查提示等基础设施细节只写服务器日志)
- **React SSR 全 TS + Tailwind + React Router + SSR/CSR 可切换**: React SSR 源码为 TypeScript (tsc 门禁), 样式用 Tailwind CSS v4 并按需内联进 SSR `<head>`; 客户端路由用 react-router v8 (SSR 深链 + 浏览器无刷新切换); 每个请求可用 `?mode=csr|ssr` 双模式渲染, 产物全部 esbuild `--minify`
- **HMR 开发模式 (`make dev`)**: C 是唯一公网服务器, Vite 退化为内部服务 (`127.0.0.1:PORT+2`) —— 客户端组件 react-refresh 热替换, 服务端代码改动免重启, Tailwind 类名实时重编译; `/@*`、`/src/*`、`/react/*` 经 C `-v` 反向代理 + WebSocket 隧道接到 Vite, 静态/CGI/chat 由 C 直服 (与生产同一条代码路径)
- **LLM 流式聊天 (`/react/chat`)**: SSE 流式对话页 + `/react/api/chat` 数据端点; **数据端点已由 C 进程原生处理 (`src/agent/llm.c`)**: 有 `LLM_API_KEY` 时 fork `curl -N` 调 OpenAI 兼容上游 (TLS 交给 curl, 服务器本体仍只链 libc), 逐 delta 重发 SSE; 上游瞬时故障 (连接拒绝/重置、5xx、429) 自动重试——共 3 次尝试, 间隔 1s/2.5s 退避, 期间 SSE 下发 `upstream hiccup, retrying` note, 退避按 100ms 小步检查客户端断开即中止; 重试判定见 `agent_retryable()` (curl exit 7/18/35/52/55/56、HTTP 429/5xx 视为瞬时, 4xx 拒绝与 127 不重试)。无密钥回落内置 C 演示引擎 (逐词节流的罐头回复)。SSE 信封 (note/delta/error/done) 与 node 后端 `chat.ts` 完全一致, 页面零改动; 配置写在项目根 `.env` (模板见 `.env.example`); nginx 侧 `location = /react/api/chat` 反代回 httpd 并 `proxy_buffering off` 直通
- **Basic Auth 认证 (-a/-r)**: RFC 7617, htpasswd 文件驱动, **仅强哈希** (`$5$`/`$6$`/bcrypt; 明文、DES、`$1$`/`$apr1$` 在加载期即被跳过, 唯一回退途径是开发用的 `AGENTHTTPD_ALLOW_WEAK_AUTH=1`), fail-closed (非法行跳过、全无效拒绝启动), 401 响应带 WWW-Authenticate 质询, CGI 经 REMOTE_USER 获取认证用户, 全站门禁含 `/react/` 转发
- **每 IP 限流 (-l)**: 令牌桶计数 + 共享内存计数表, fork/worker 池模式共用同一配额; IPv4/IPv6 对端各自独立成桶, 可信反代可用 `X-Forwarded-For` (`RATE_LIMIT_TRUSTED_PROXIES`) 提供 key; 检查先于认证 (洪水烧不到 crypt CPU), 超限回 429 + Retry-After 并断连
- **双栈监听**: 同一端口同时绑 IPv4 `0.0.0.0` 与 IPv6 `::` (显式置 `IPV6_V6ONLY`, 否则 `::` 会把 v4 一起吞掉、v4 的 bind 直接撞 EADDRINUSE); 事件循环与每连接 fork 两条 accept 路径都服务两族; 主机无可用 v6 栈时只打一行提示、退回纯 IPv4

## 目录结构

```
agent-httpd/
├── src/                 # 分层结构: 根部放伞型头文件, 每层一个子目录
│   ├── agenthttpd.h     # 公开框架 API: 嵌入服务器 (config + 路由/工具钩子 + run)
│   ├── httpd.h          # 共享结构体/函数申明 (HTTP、服务端/客户端 FCGI 复用)
│   ├── internal.h       # 服务器内部跨模块声明
│   ├── core/            # 引擎层: main.c (CLI 前端) · framework.c (公开 API 实现)
│   │                    #   event.c (主事件循环) · worker.c (prefork 池) · router.c (llm-router 同步)
│   │                    #   metrics.c (共享计数) · minijson.c · util.c
│   ├── http/            # 协议层: http.c (解析/序列化/keep-alive) · http_parse.c
│   │                    #   http_resp.c · http_log.c · http_route.c (分发) · static.c (穿越防护
│   │                    #   + ETag/304 + gzip) · chatio.c (SSE 信封)
│   ├── cgi/             # 网关层: cgi.c (CGI/1.1) · fastcgi.c (服务端+客户端转发)
│   │                    #   vite.c (dev 代理)
│   ├── security/        # auth.c (Basic Auth, 仅强哈希) · ratelimit.c (每 IP 共享内存窗口)
│   └── agent/           # Agent 栈: llm.c (聊天端点) · agent.c (ReAct 循环) · mcp.c (MCP stdio
│                        #   客户端) · pse.c (PSE 三角色编排) · tools.c (工具注册表)
│                        #   skills.c (SKILL.md 索引) · session.c (记忆库)
├── build/               # 编译中间产物 (.o/.d, make clean 清除) —— src/ 只放源码
├── examples/
│   ├── embedded.c       # 嵌入示例: 自定义路由 + exec 工具 + 工具探针 (make example-run)
│   └── tools/wordcount.py # 外部 exec 工具: stdin 进 JSON, stdout 出 JSON
├── cgi-bin/             # 部署产物: 服务器直接 execl 的可执行文件
│   ├── hello.cgi        # bash: 环境变量和时间
│   ├── form.cgi         # bash: GET query / POST body 表单
│   ├── python.cgi       # Python 3: 标准库解析 GET/POST
│   ├── react-ssr.cgi    # React SSR: 服务端渲染 CGI 单文件 (esbuild+tailwindcss 打包)
│   ├── react-ssr/       # React SSR 源码 (App.tsx / client.tsx + server/ 目录: render.tsx / cgi.tsx / main.tsx / chat.ts + styles/main.css + tsconfig.json)
│   ├── go.cgi           # Go 原生二进制 (go build)
│   ├── rust.cgi         # Rust 原生二进制 (rustc -O)
│   ├── java.cgi         # sh 包装器 → java -cp Main.class
│   ├── php.cgi          # sh 包装器 → php-cgi (设 REDIRECT_STATUS)
│   └── ruby.cgi         # Ruby 脚本 (ruby cgi stdlib)
├── cgi-langs/           # 各语言源码 (仅构建时需要)
│   ├── go/ rust/ java/ php/ ruby/
├── scripts/
│   ├── smoke-test.sh    # 冒烟测试脚本 (make test 调用)
│   ├── fcgi-test.py     # FastCGI 客户端测试 (裸协议, 模拟 nginx)
│   ├── build-cgis.sh    # 编译/生成全部语言 CGI (make build-cgis)
│   └── build-ssr.sh     # 构建 SSR 三产物: CGI 入口 + 常驻后端 + 客户端 (make build-ssr)
├── www/
│   ├── index.html       # 网站的根页面
│   ├── js/react-ssr.js  # React SSR client bundle (浏览器 hydration)
│   ├── error/           # 自定义错误页 (404.html / 403.html / 500.html)
│   └── test/            # 目录列表测试目录
├── logs/                # 访问日志目录
├── deploy/
│   ├── nginx.conf           # nginx 站点配置 (反代 agent-httpd + fastcgi_pass 直连 React 后端)
│   └── docker-entrypoint.sh # 容器入口 (httpd / react-backend 两种角色)
├── Dockerfile           # 三阶段构建: C 编译 → SSR 打包 → node:20-slim 运行时
├── docker-compose.yml   # 三服务编排: httpd / react / nginx
├── .dockerignore
├── docs/
│   └── FRAMEWORK.md     # 框架化实录: 动机、设计、过程、验证
├── bin/                 # 编译输出目录 (服务器二进制, libagenthttpd.a)
└── Makefile
```

### 作为库嵌入

服务器同时也是一个框架: 链接 `bin/libagenthttpd.a` (`make lib`), 注册自定义路由
和 Agent 工具, 在自己的进程里跑起来 —— CLI 二进制只是同一套 API 的薄前端:

```c
#include "agenthttpd.h"

agenthttpd_route("GET", "/api/status", my_status_handler);
agenthttpd_tool_exec("wordcount", "...", params_json, "python3 tools/wordcount.py");
agenthttpd_run(&(agenthttpd_config){ .port = 18101, .workers = 4 });
```

```bash
make example-run   # 构建 bin/libagenthttpd.a + examples/embedded 并起服在 :18101
```

设计取舍、预 fork 注册模型、exec 工具语义与完整过程记录见
[docs/FRAMEWORK.md](docs/FRAMEWORK.md)。

## 构建与运行

```bash
# 构建
make

# 启动服务器 (默认端口 18080)
./bin/agent-httpd -p 18080

# 或使用 make 目标（端口可用 PORT=xxx 覆盖）
make start      # 生产模式: 前台启动 C 服务器 (默认 8 worker 池, WORKERS= 置空改为每连接 fork)
make run        # make start 的兼容别名
make dev        # 开发模式: React HMR 开发服务器 (Vite 中间件模式, 端口 DEV_PORT=3000)
make restart    # 先 stop 再启动
make stop       # 优雅停止 (SIGTERM)
make test       # 冒烟测试 (静态/CGI/404)
make install    # 安装到 /usr/local (可用 DESTDIR 覆盖)
make uninstall  # 卸载
```

## Docker 部署

```bash
docker compose up --build -d
#   http://localhost:18080  -> agent-httpd 直连 (静态/CGI/健康检查)
#   http://localhost:18081  -> nginx 入口 (静态/CGI 反代 + React FastCGI 直连)
docker compose down          # 停止并移除容器
docker compose logs -f httpd # 跟看日志
# 运维指标: curl http://localhost:18080/metrics (Prometheus 文本格式,
#   快/慢分流、状态码分类、worker 水位、agent 槽、上游重试; 位于
#   限流/认证门禁之内, 若开了 -a 需带凭据抓取)
```

架构（三服务）:

| 服务 | 镜像 | 角色 |
|---|---|---|
| `httpd` | agent-httpd | agent-httpd 本体: worker 池 + `-F` FastCGI 监听 + `-R` React 中继 |
| `react` | agent-httpd | 驻留 React SSR FastCGI 后端 (UNIX socket, 不暴露端口) |
| `nginx` | nginx:1.27-alpine | 前置反代: `/` proxy_pass 到 httpd; `/react/` fastcgi_pass 直连 react |

运行时镜像以非 root 账号 `agent` (uid 10001) 跑一切业务进程: 服务器本体、CGI/MCP 子进程与 React 后端。需要写的位置 (`/app` —— docroot + 会话存储、访问日志目录、FastCGI socket 目录、npx 缓存 HOME) 在构建期 chown, 其余保持 root 属主只读。compose 的 socket 卷已改名 `agent-sock`: 旧卷 `react-sock` 创建于 root 时代、属主是 root, 换新名让 docker 按新用户的属主重建卷 (旧卷可用 `docker volume rm` 清掉)。

> 提示: 重建 httpd 容器后 nginx 可能仍缓存旧的 upstream IP (502), `docker compose restart nginx` 即可; 也可以先 `docker compose down` 再 `up`。

同一个 SSR 页面有两条链路可对比:

- `:18080/react/*` — agent-httpd 作 FCGI **客户端** (`-R` 中继到常驻后端)
- `:18081/react/*` — nginx 作 FCGI **客户端** (`fastcgi_pass` 直连; socket 需 `REACT_FCGI_SOCK_MODE=0777` 放宽权限, 见下)

说明:

- 镜像四阶段构建 (C 编译 → 原生 CGI 编译 → SSR 打包 → node:20-slim 运行时, 约 620MB); 构建参数与本地 `make` 完全一致 (`-Wall -Wextra -Werror -O2`)
- 容器内 CGI 动物园完整可用: bash/python/ruby/php 解释器内置; go/rust/java 由 `cgi-build` 阶段用 Linux 工具链**在容器内重新编译** (宿主机的 Mach-O 产物被 `.dockerignore` 排除, 不进镜像)
- 环境变量: `WORKERS` (worker 数, 0 = 每连接 fork)、`RATE_LIMIT` (每 IP 限流 req/s)、`LOG_FILE` (默认 `/var/log/agent-httpd/access.log`); C 聊天端点另认 `LLM_API_URL` / `LLM_API_KEY` / `LLM_MODEL` / `LLM_TIMEOUT` (不设则用内置演示引擎, 无需密钥)。Agent 能力另有 `AGENT_MAX_ROUNDS` / `AGENT_MAX_CONCURRENT` (ReAct 轮数/并发槽)、`AGENT_UPSTREAM_ATTEMPTS` / `AGENT_BACKOFF_MS_1` / `AGENT_BACKOFF_MS_2` (上游重试次数与退避间隔, 编译期常量见 `src/agent/agent.h`)、`PSE_ENABLED` / `PSE_SOULS_DIR` (PSE 编排器/角色灵魂目录)、`MCP_SERVERS` (MCP stdio 服务器配置)、`HARNESS_SKILLS_DIR` / `SKILLS_EXTRA_DIRS` (技能目录); llm-router 目录同步认 `ROUTER_API_URL` (缺省复用 `LLM_API_URL`) / `ROUTER_SYNC_BUDGET_SECONDS` (整轮同步的墙钟预算, 默认 10s, 0 = 不限; 上游半死时按剩余预算动态收紧每个 curl 的 `--max-time`, 超墙即跳过剩余拉取——同步陈旧可接受, 启动卡死不可接受); react 后端认 `REACT_HTTP_PORT` / `REACT_RENDER_TTL_MS` / `REACT_FCGI_SOCK_MODE` (socket 权限, 默认 0700); 容器日志自轮转认 `LOG_ROTATE_SECONDS` (默认 86400, 0 = 关闭) / `LOG_ROTATE_KEEP` (默认 7 份)
- **LLM 配置走项目根 `.env`**: `cp .env.example .env` 后填 `LLM_API_KEY` 即接真实模型 (OpenAI 兼容端点均可: OpenAI / DeepSeek / 本地 Ollama); compose 的 httpd 与 react 服务都经 `env_file` 注入 (文件缺失也能启动), `make dev` 用 `--env-file-if-exists=.env` 自动加载; C 服务器在首个聊天请求时也从工作目录读 `.env` 的 `LLM_*` (环境变量已存在的优先 —— 可用空 `LLM_API_KEY` 强制回落演示引擎)。改完 `docker compose up -d` 重建容器生效。`.env` 已被 `.gitignore`/`.dockerignore` 排除, 密钥不进仓库不进镜像
- `docker stop` 的 SIGTERM 直达 agent-httpd 的优雅排水逻辑 (停 accept → worker 排水最多 5 秒 → SIGKILL 兜底), 重启不丢在途请求
- **FCGI socket 权限默认收紧**: react 后端的 UNIX socket 默认 `0700` (owner-only)——socket 直连可绕过 httpd 的认证/限流, 不能让同机任意进程可连; nginx `fastcgi_pass` 场景下 worker 用户不同时, 给 react 服务设 `REACT_FCGI_SOCK_MODE=0777` 放宽

## 测试

> 套件里的探测都指向 `http://localhost:<端口>`。若 shell 导出了
> `http_proxy`/`https_proxy`, 这些请求会被代理接走、而不是打到服务器, 于是几十条
> 断言以 502 失败, 看起来像服务端有 bug —— 有代理时请用
> `env -u http_proxy -u https_proxy -u ALL_PROXY make test`。

```bash
make test   # 冒烟测试: 静态/HEAD/POST/穿越防护/重定向/目录列表/错误页/query string
            #   + 大响应流式/CGI 超时 504/半开请求断开/XSS 转义
            #   + CGI Location/Status 头/Accept-Encoding q 值/Keep-Alive 连接复用
            #   + 全部语言 CGI + FastCGI + HMR 开发服务器
            #   + C 聊天端点 SSE (演示引擎/错误路径/-R 中继旁路)
            #   (HMR 用例在 vite 未安装时自动 SKIP; 聊天用例以空 LLM_API_KEY
            #    压住项目根 .env, 确定性离线运行)
make bench  # 吞吐对比: keep-alive vs 每请求一连接 (默认 5000 请求/16 并发,
            #   可用 BENCH_REQ/BENCH_CONC/PORT 覆盖; 服务器开了 -l 时
            #   429 会计入 429s 列而不是报错, 延迟分位数只统计 200)
make test-unit      # 免服务端单测: dev 代理 header 构造、Basic Auth (b64/凭据解析、
                    #   hash-only htpasswd 策略)、minijson (sbuf/转义/容忍式读器)
                    #   (均 ASan+UBSan) + FastCGI >64KB 流式中继
                    #   (起一个一次性 UNIX socket 后端)
make test-keepalive # 单连接 Keep-Alive 流水线 (真起服务器, fork-per-connection 模式,
                    #   端口随机; 较慢, 依赖 loopback)
make test-linux     # Linux/GCC 构建守卫: 只构建 Dockerfile 的 c-build 阶段。glibc 是唯一
                    #   会暴露 -Wstringop-truncation / -Wformat-truncation /
                    #   -Wuse-after-free 及 -lm/-lcrypt 链接标志的编译器 —— Apple clang
                    #   对这些一律沉默, 所以只跑本机构建看不出容器构建已坏
make test-container # 探测正在运行的容器 (需先 `docker compose up -d`): 直接打镜像里
                    #   发货的二进制 + docroot + CGI 动物园 —— 路由/方法处理/穿越拦截/
                    #   请求走私/Keep-Alive/gzip/Range/304/CGI Location+Status 头/
                    #   大响应流式 (无 64KB 截断)
make test-upstream  # 上游不可达路径: 聊天必须回 error 事件 (并重试), 不能给出静默的
                    #   空答复。自起一个 --network none 的一次性容器, 只需要镜像
make test-stream    # 大响应流式: 300KB 响应体在读取端停顿的情况下也必须完整送达。
                    #   由**第二个容器**经私有 bridge 读取 —— 只有这条链路的 socket
                    #   缓冲区真会被填满, 因此也只有它能抓出「响应体中途被截断」
                    #   (nginx 报 upstream prematurely closed connection; 浏览器则因
                    #   JS 资源解析失败而永不水合)
```

也可打开浏览器访问:

- `http://localhost:18080/`                    - 首页
- `http://localhost:18080/cgi-bin/hello.cgi`   - bash 简单 CGI
- `http://localhost:18080/cgi-bin/form.cgi`    - POST 表单 (或 `?name=test` GET)
- `http://localhost:18080/cgi-bin/python.cgi?name=Alice&message=Hi` - Python 示例
- `http://localhost:18080/cgi-bin/react-ssr.cgi?name=Alice&message=Hi` - React SSR 示例
- `http://localhost:18080/cgi-bin/go.cgi?name=Alice&message=Hi` - Go 原生二进制
- `http://localhost:18080/cgi-bin/rust.cgi?name=Alice&message=Hi` - Rust 原生二进制
- `http://localhost:18080/cgi-bin/java.cgi?name=Alice&message=Hi` - Java (JVM 包装器)
- `http://localhost:18080/cgi-bin/php.cgi?name=Alice&message=Hi` - PHP (php-cgi)
- `http://localhost:18080/cgi-bin/ruby.cgi?name=Alice&message=Hi` - Ruby (cgi stdlib)
- `http://localhost:18080/test/`               - 目录列表
- `http://localhost:18080/test`                - 301 → `/test/`
- `http://localhost:18080/nothing`             - 自定义 404 错误页

### 多语言 CGI

所有语言示例都走同一条 CGI 链路: 服务器 `fork()` 设置环境变量 + 管道连通
stdin/stdout 后 `execl()` 目标文件。各语言差别只在"如何被 exec":

| 语言 | cgi-bin 产物 | 运行方式 | 备注 |
|------|-------------|---------|------|
| Go | `go.cgi` | 原生二进制 | `go build` 编译, 冷启动微秒级 |
| Rust | `rust.cgi` | 原生二进制 | `rustc -O` 编译 |
| Java | `java.cgi` (sh) | `java -cp ... Main.class` | 每请求启用新 JVM(慢); 生产建议 GraalVM 原生镜像 |
| PHP | `php.cgi` (sh) | `php-cgi` | homebrew php-cgi 需 `REDIRECT_STATUS=200` |
| Ruby | `ruby.cgi` | 脚本 | stdlib `cgi` 自动解析 GET/POST |
| Python | `python.cgi` | 脚本 | 手写解析(3.13+ 移除 `cgi` 模块) |
| React SSR | `react-ssr.cgi` + `www/js/react-ssr.js` | node 服务端渲染 + 浏览器 hydration | server/client 双 bundle, esbuild 打包 |

编译/重新生成所有语言 CGI(缺工具自动跳过):

```bash
make build-cgis
```

### Python CGI

`cgi-bin/python.cgi` 是 Python 标准库实现的 CGI 示例, 无第三方依赖, 运行时只需
`python3` 在 PATH。由于 Python 3.13+ 已移除 `cgi` 模块, 脚本手写解析:
`urllib.parse` 处理 GET query 与 urlencoded body; `email.parser` 处理 multipart
表单。支持 GET (query string) / POST (urlencoded 与 multipart), HTML 输出经
`html.escape` 防 XSS。

### React SSR (SSR + hydration)

现代 React 的**服务端渲染 + 浏览器注水 (hydration)** 结构:

- **SSR 入口** `cgi-bin/react-ssr/server/cgi.tsx` → 打包成 `react-ssr.cgi`, 由 C 服务器
  通过 CGI 执行: 用 `react-dom/server` 的 `renderToString` 渲染共享组件 `App.tsx`,
  输出 `<div id="root">` 里的服务端 HTML, 同时内联一份 `window.__SSR_DATA__`
  (参数 + 服务器时间), 并引入 client bundle。
- **Client 入口** `cgi-bin/react-ssr/client.tsx` → 打包成 `www/js/react-ssr.js`,
  由浏览器下载, 用 `react-dom/client` 的 `hydrateRoot` 接管服务端 HTML, 之后组件
  就是正常交互式 React 应用(`useEffect` 实时时钟、表单输入实时回显等)。
- **一致性问题**: 客户端首次渲染的数据与 SSR 完全同源(JSON 回放), 所以 `hydrateRoot`
  不会报 mismatch; 时间等易变值由服务器打包成 `serverTime`, 注水后再在浏览器刷新。
- 组件用到的 hooks: `useState` / `useMemo` / `useId` / `useEffect`。
- **React Router v8**: `App.tsx` 是共享的路由树, 只有路由器包裹层不同 ——
  服务端 `<StaticRouter location={pathname}>`(server/render.tsx 从 `REQUEST_URI` 拆出
  path, 因此 `/react/about` 深链可直接开箱渲染), 客户端 `<BrowserRouter>` +
  `hydrateRoot`(`<Link>` 无刷新切换)。路由表:
  `/react`(Home) / `/react/about` / `/react/counter`(useState 计数器演示) /
  `/react/*`(NotFound); 其余路径(如 `/cgi-bin/react-ssr.cgi` 直接访问)回落到 Home。
- **SSR/CSR 双模式**: 请求带 `?mode=csr` 时服务器跳过 `renderToString`, 只回一个
  空壳 `<div id="root">` + 内联 CSS + `__SSR_DATA__`; 客户端 bundle 读到
  `data.mode` 后改用 `createRoot` 全新挂载(纯 CSR, 服务器零渲染成本)。
  默认(或无 `mode`、或非法值)是 SSR。两种 CGI/常驻入口、GET/POST 语义一致,
  页面 footer 有 "switch to CSR / SSR" 开关(整页 `<a>` 跳转, 让后端重新决策)。

两个 bundle 都是 esbuild 产物, 把 react/react-dom 打进去, 各自自包含
(node 进程 / 浏览器)。

- 共享渲染核心 `server/render.tsx`(parseQuery / safeJson / renderPage)同时被两个入口引用,
  保证 CGI 与常驻后端产出的标记字节级一致; `renderPage` 内联 `__SSR_DATA__`
  并固定引用 `/js/react-ssr.js`。
- **样式用 Tailwind CSS v4**: 类名写在 `App.tsx` 里, `build-ssr.sh` 先用
  `@tailwindcss/cli` 把 `styles/main.css`(`@source` 只扫 react-ssr 目录)编译成
  `tailwind.css`, 再经 esbuild 的 `--loader:.css=text` 内联进 bundle —— SSR 输出
  的 `<head>` 仍是完整自包含(无需额外的样式表请求)。设计 token(如 `bg-surface`)
  定义在 `styles/main.css` 的 `@theme` 里。

构建流程(仅改源码后需要):

```bash
cd cgi-bin/react-ssr && npm install   # 一次性安装构建依赖 (react, esbuild, tailwindcss, typescript, @types/*)
make build-ssr                        # 先 tsc --noEmit 类型检查, 再 Tailwind 编译, 再产物:
                                      # server/cgi.tsx  → cgi-bin/react-ssr.cgi
                                      # server/main.tsx → bin/react-ssr-server (常驻)
                                      # client.tsx      → www/js/react-ssr.js
make typecheck                        # 单独跑类型检查 (esbuild 只转译不查类型)
```

TS 约束与典型坑:

- **esbuild 不查类型**, 所以 `build-ssr.sh` 先跑 `tsc --noEmit`(失败即中止打包)。
- `parseQuery(query: string)` 这类签名直接拦截类型错误: 此前把 `Buffer`
  传给 `parseQuery` 导致 POST 500 的 bug, 现在编译期就会报错
  (常驻后端已改为 `body.toString("utf8")`)。
- **client bundle 绝不能引用 `process`**: esbuild 只替换 `process.env.NODE_ENV`,
  其它 `process.*` 会原样留在浏览器 bundle 里, hydration 时抛
  `ReferenceError`。因此 `nodeVersion` 由服务端算好放进 `__SSR_DATA__`
  (同 `serverTime` 策略), client 只从 blob 读取, 两端标记始终一致。

源码在 `cgi-bin/react-ssr/`(shared 组件 App.tsx + 客户端入口 client.tsx 放根目录,
服务端代码集中在新 `server/` 子目录: 渲染核心 render.tsx + 两个服务端入口
cgi.tsx(CGI) / main.tsx(常驻 FastCGI) + 聊天后端 chat.ts)。支持 GET (query string)
与 POST (body), 通过标准 CGI 环境变量 `REQUEST_METHOD` / `QUERY_STRING` /
`CONTENT_LENGTH` 接收参数。完整渲染链路经 `renderToString` 校验, `make test` 覆盖
GET/POST 与 FastCGI 三个入口。

## 开发模式与 HMR

生产链路每次改源码都要 `make build-ssr`(tsc + tailwind + esbuild 三步)。开发时用
Vite 按需转换代替——同一个 `render.tsx` 渲染核心, 但改完即生效:

```bash
cd cgi-bin/react-ssr && npm install   # 一次性 (vite/@vitejs/plugin-react 已在 devDependencies)
make dev                              # http://localhost:3100/react/?name=Alice
DEV_PORT=3100 make dev                # 换端口
```

工作方式 (`scripts/dev-server.js`, 统一 `-v` 接线):

- **唯一公网入口是 C 进程**: dev-server 在 `DEV_PORT` 拉起 `./bin/agent-httpd
  -v DEV_PORT+2`, 它就是开发服务器的全部 HTTP 面——静态、CGI、`/health`、
  `/react/api/chat`、错误页全部由 C 直服 (与生产完全同一条代码路径)。
- **Vite 退化为内部服务**: dev-server 只把 Vite 绑定到 `127.0.0.1:DEV_PORT+2`
  (不对外), 负责 `transformRequest`、`/react/*` 的 dev SSR、Tailwind 编译与
  HMR WebSocket。C 把 `/@*`、`/src/*`、`/react/*`(除 chat) 反向代理给它;
  HMR WebSocket 升级由 C 做 TCP 隧道透传。
- **客户端热替换**: SSR HTML 里的 `<script src="/js/react-ssr.js">`(esbuild 产物)
  被改写为 Vite 按需转换的 `/react/react-ssr.tsx`, 并在 `<head>` 注入
  react-refresh preamble —— 改 `client.tsx`/页面组件时 Fast Refresh 无刷新换组件。
- **服务端代码免重启**: `render.tsx` / `App.tsx` / `pages/*` 经 `ssrLoadModule`
  加载, 文件变化后失效模块图并广播 full-reload, 下一个请求即用新代码渲染。
- **Tailwind 实时编译**: 保存 `styles/main.css` 或改动含类名的 tsx 时自动重新
  编译 `tailwind.css`(防抖), 再触发整页刷新。

注意: dev 与生产的接线完全一致——浏览器只对 C 说话, C 把渲染交给内部上游
(dev 是 Vite, 生产是 `react-ssr-server` 的 HTTP 端口), 见"React 渲染接线"。
HMR 与代理行为均由 `make test` 的守卫用例覆盖 (vite 未安装时自动 SKIP)。

## 命令行参数

| 参数 | 说明 |
|------|------|
| `-p <port>` | 指定 HTTP 监听端口 (默认 18080) |
| `-F <sock>` | 额外以 FastCGI 后端身份监听 UNIX 套接字 (类似 PHP-FPM) |
| `-R <sock>` | (可选, 旧接线) `/react/*` 请求经 FCGI 转发给常驻 React 后端; 推荐改用 `-v` 统一接线 |
| `-v <port>` | 统一接线: `/@*`、`/src/*`、`/react/*`(除 chat) 经 HTTP 代理转发到 `127.0.0.1:<port>` 的 Vite / react-ssr-server; HMR WebSocket 升级透明隧道 (见"React 渲染接线") |
| `-T <seconds>` | 同时设置请求超时与 CGI 超时 (默认 30 秒) |
| `-a <htpasswd>` | 开启 Basic Auth, 校验指定的 htpasswd 文件 (见下节) |
| `-r <realm>` | Basic Auth 的 realm (默认 `agent-httpd`, 需配合 `-a`) |
| `-w <n>` | 慢路径 worker 池大小 (默认 8; `0` = 每连接 fork)。快路径由 master 事件循环直服, 池只承接 CGI / chat / 代理 / body 请求 (见"性能设计") |
| `-l <rps>` | 每 IP 限流: 每秒最大请求数 (0 = 关闭, 默认; 超限回 429 + Retry-After) |
| `-L <path>` | 访问日志路径 (默认 `./logs/access.log`; 打不开时告警并继续运行) |
| `-h` | 显示帮助 |

### 超时与防护栏

所有防护栏都可用环境变量覆盖 (冒烟测试就是这么把超时调快到 3 秒的), `-T` 则一次性设置两个超时:

| 环境变量 | 默认 | 作用 |
|----------|------|------|
| `REQUEST_TIMEOUT_SECONDS` | 30 | 请求头 + POST 体读取的总时限, 超时断开连接 (客户端半开请求不再永久占用 fork 出的 handler) |
| `CGI_TIMEOUT_SECONDS` | 30 | CGI 子进程无输出即 kill (SIGKILL) 并回 `504 Gateway Timeout`; 向 CGI 写 POST 体同样受轮询保护, 脚本不读 stdin 也不会卡死服务器 |
| `CGI_BODY_TMP_THRESHOLD` | 524288 (512KB) | CGI 响应体超过该值落盘 `/tmp` 临时文件流式发送, 发送完自动删除; 未超过则在内存中按需增长 |

另外, CGI 运行期间会轮询客户端 socket, 客户端提前断开时立即终止 CGI 并放弃发送, "慢 CGI + 提前断开"不再每次泄漏一个进程。

### 每 IP 限流 (-l)

```sh
./bin/agent-httpd -p 18080 -l 20   # 每个 IP 每秒最多 20 个请求
```

- **令牌桶 + 共享表**: 计数表放在 `MAP_SHARED` 匿名共享内存里 (每 IP 经 splitmix 混合后哈希进 4096 个桶, 桶内互斥锁显式 `PTHREAD_PROCESS_SHARED`), 所以**每连接 fork 模式和 worker 池模式强制的是同一个配额**——worker 池下多个进程不会各算各的。令牌按 `-l` 每秒补充, 窗口边界不再出现 2 倍突发。
- **检查顺序在 Basic Auth 之前**: 洪水打不到分发层, 也烧不到 crypt() CPU——用限流挡住 Basic Auth 爆破正合适。
- **超限响应**: `429 Too Many Requests` + `Retry-After: 1`, 并强制 `Connection: close` (排队中的同源请求无法借道溜过计数器)。
- **哈希冲突靠线性探测, 绝不抢占**: 冲突时向后探测最多 8 个槽, 只消费自己名下的桶; 后来者无法清空他人仍在窗口期的计数 (旧的"抢占桶"等于限流可被绕过)。只有 8 个槽全满才退化为共用基槽——同样不覆盖别人的计数。
- **反代场景**: 默认 key 是 TCP 对端地址, 走反代时所有真实客户端会塌缩到代理这一个桶。把 `RATE_LIMIT_TRUSTED_PROXIES` 设为**逗号分隔的代理 IP/CIDR 列表**, `X-Forwarded-For` 最左跳即成为 key——但**仅当对端落在可信列表内**才采纳, 不可信客户端无法伪造自己的桶。条目支持 IPv4 **与** IPv6 (`127.0.0.1,10.0.0.0/8,::1,2001:db8::/32`), v6 可加方括号 (`[::1]`); 非法条目 (地址不合法, 或前缀非数字/越界) 会被**丢弃而非放宽**成"信任所有人"。IPv6 的 XFF 跳与 IPv6 对端用同一套 key 推导 (哈希成 32 位), 两种族各自独立限流。
- 环境变量 `RATE_LIMIT_RPS` 可覆盖 `-l`。
- 环境变量 `RATE_LIMIT_TRUSTED_PROXIES` 启用上面的 X-Forwarded-For 处理 (不设 = 不信任任何代理)。

## Basic Auth 认证

`-a` 开启 HTTP Basic 认证 (RFC 7617), 未带凭据的请求统一回 `401` +
`WWW-Authenticate: Basic realm="...", charset="UTF-8"` 质询, 全站生效
(静态、CGI、`/react/` 转发均在门禁之内):

```bash
# htpasswd 文件: 每行 "user:secret", 只接受强哈希。
# 用 `openssl passwd -6` (SHA-512) 或 `htpasswd -B` (bcrypt) 生成。
printf 'alice:%s\n' "$(openssl passwd -6 'password123')" > /tmp/htpasswd
./bin/agent-httpd -p 18080 -a /tmp/htpasswd -r "Private"

curl -u alice:password123 http://localhost:18080/
```

- **只接受强哈希**: `$5$` (SHA-256)、`$6$` (SHA-512) 与 bcrypt
  (`$2a$`/`$2b$`/`$2y$`)。明文口令、13 字符 DES、`$1$`/`$apr1$` MD5 哈希在
  加载时即被拒绝 —— 该条目被跳过并告警, 永远不可能匹配。
- **开发逃生门**: 导出 `AGENTHTTPD_ALLOW_WEAK_AUTH=1` 可恢复旧的宽容行为
  (明文/DES 可用, 启动时大声警告)。仅限本地开发, 生产环境绝不使用。
- **平台差异**: Linux 链接 `-lcrypt` (glibc/libxcrypt 支持 `$5$`/`$6$`);
  macOS 的 `crypt(3)` 在 libc 中, 仅支持 DES —— 强哈希在本机会校验失败
  (fail-closed 拒绝, 不会误放行)。macOS 上请用上面的逃生门, 或在容器里
  验证认证。
- **fail-closed 设计**: htpasswd 中格式非法的行被跳过并告警, 全部无效则启动
  失败 —— 配置错误永远倒向"拒绝"而非"放行"; 凭据缺失/格式错/未知用户一律 401。
- **CGI 集成**: 认证通过后, CGI 程序经标准变量 `REMOTE_USER` 拿到登录用户名。
- 401 响应强制 `Connection: close`, 避免质询后管道内残留请求的歧义。

## FastCGI 后端模式

除了当普通 HTTP 服务器，agent-httpd 还能作为 **FastCGI 服务端** 运行：监听一个
UNIX 套接字，接受 nginx 等前端通过 `fastcgi_pass` 转发过来的请求。FCGI 帧
(`BEGIN_REQUEST` / `PARAMS` / `STDIN`) 会被重建为内部 `HttpRequest`，然后走与
HTTP 完全相同的分发链（静态文件 / CGI），响应经 `FCGI_STDOUT` 记录 + `FCGI_END_REQUEST`
返回。

```bash
# 同时监听 HTTP 18080 与 FCGI unix socket
./bin/agent-httpd -p 18080 -F /tmp/mini-fcgi.sock
```

nginx 侧配置示例:

```nginx
location / {
    # fastcgi_pass 指向 agent-httpd 的 FCGI socket (CGI 脚本在前端必须先解出)
    fastcgi_pass unix:/tmp/mini-fcgi.sock;
    fastcgi_param REQUEST_METHOD  $request_method;
    fastcgi_param REQUEST_URI     $request_uri;
    fastcgi_param QUERY_STRING    $query_string;
    fastcgi_param CONTENT_TYPE    $content_type;
    fastcgi_param CONTENT_LENGTH  $content_length;
    fastcgi_param SCRIPT_FILENAME $document_root$fastcgi_script_name;
    include fastcgi_params;
}
```

`make test` 会用 `scripts/fcgi-test.py` 以裸 FCGI 协议（模拟 nginx）验证：静态页、
404、GET/POST CGI、React SSR 各场景。

## React 渲染接线 (统一 `-v`)

CGI 每请求 `fork + exec node` 一次，React 冷启动（加载 react、执行打包 JS）约
几十毫秒且不可复用。本项目用 **常驻渲染进程** 消除冷启动，架构与
PHP-FPM / puma / unicorn 相同，且 **开发与生产共用同一条 `-v` 接线**：

```
                 浏览器 (只对 C 说话)
                  │   GET /react/?name=Alice
                  ▼
             agent-httpd :PORT   (唯一公网入口)
                  │   静态/CGI/health/chat/错误页 = C 直服
                  │   /@*、/src/*、/react/* = HTTP 反向代理 (-v)
                  │   HMR WebSocket = TCP 隧道
                  ▼
  127.0.0.1:PORT+2 ◄── dev:  Vite (scripts/dev-server.js)
                  ◄── prod: bin/react-ssr-server (REACT_HTTP_PORT=PORT+2)
                          渲染 renderStream → HTTP 200, 流式/缓存
```

- **生产**: `make react-server` 会先起 `bin/react-ssr-server`(把 React 载入内存
  一次, 并带 ISR 渲染缓存), 再起 `agent-httpd -p PORT -v PORT+2`——C 把
  `/react/*` 页面代理给它, 与 dev 指向 Vite 的方式完全相同。
- **FastCGI 是可选旧接线**: `bin/react-ssr-server` 仍支持 `REACT_HTTP_PORT` 未
  设置时的原生 FCGI 监听 (`-R` 中继), 供 nginx `fastcgi_pass` 或需要 socket
  直连的场景; 新的统一接线推荐 `-v`。
- 后端复用 `render.tsx` 共享渲染核心, 因此与 CGI 渲染结果字节级一致(含
  `window.__SSR_DATA__` 与 `<script src="/js/react-ssr.js">`); 客户端 bundle 与
  CGI 模式共用同一个 `www/js/react-ssr.js`。
- `/react/api/chat` 在 C 进程内直接处理(见下节), **不会**被代理给 React 后端。

```bash
make react-server                      # 一键: react-ssr-server(HTTP) + agent-httpd -v
# 浏览器访问: http://localhost:18080/react/?name=Alice
curl 'http://localhost:18080/react/?name=Alice'   # 经 C → -v → react-ssr-server
# 后端停止时回 502
pkill -f react-ssr-server && curl -i http://localhost:18080/react/ | head -1
```

## C 原生 LLM 聊天端点 (`src/agent/llm.c`)

`/react/api/chat` 的数据端点在 C 进程内处理, 不再经过 React FastCGI 后端:

```
浏览器 POST /react/api/chat {message, history?}
      │
      ▼
handle_client: 限流/认证门禁 → llm_is_chat_route 拦截 (优先于 -R 中继)
      │
      ├─ LLM_API_KEY 已配置 ──► fork curl -N --max-time <LLM_TIMEOUT>
      │        POST <LLM_API_URL>/chat/completions  (stream:true)
      │        │  上游 SSE 逐行解析 (data: {...} / [DONE] / error 事件)
      │        ▼
      │   重发为本项目信封: {"t":"delta"|"note"|"error"|"done", ...}
      │
      └─ 无密钥 ──► 内置 C 演示引擎: 罐头回复逐词节流 (28ms/步)
      │
      ▼
SSE 流式写回客户端 (head+body 由 handler 自发, response->handled,
handle_client 只记日志并断连 —— SSE 以 close 定界, 不进 keep-alive)
```

要点:

- **零新依赖**: TLS 交给 fork 出的 `curl`(1), 服务器本体仍只链 libc —— 与"不引
  OpenSSL"的项目约定一致, 模型上也是 CGI fork+pipe 同构 (stdin 传请求体,
  stdout 读上游流, 轮询总超时, 客户端断开即 kill)
- **手写容错 JSON**: 请求体 `{message, history?}` 与上游 delta 的解析是
  ~200 行的容错读取器 (`\uXXXX`/代理对/嵌套跳过), 无库
- **`.env` 兼容**: 首个聊天请求时从工作目录读 `.env` 的 `LLM_*`
  (`setenv(..., overwrite=0)`, 环境变量优先), 与 compose `env_file` /
  `make dev --env-file-if-exists` 三方共用同一份配置
- **容器内 `LLM_API_URL` 要换主机**: `.env` 里那个值只在宿主成立 ——
  容器里的 `localhost` 是容器自己。出货的 `docker-compose.yml` 因此给
  `httpd` / `react` 两个服务单独覆盖它 (`x-llm-api-url`, 默认
  `host.docker.internal`), `.env` 与宿主 `make dev` 都不受影响
- **上游不可达会被报出来, 不会被吞掉**: 失败的一轮发 `error` 事件并按退避
  重试, 所以路由器挂掉会在聊天里显形, 而不是回一个空答复
  (`make test-upstream` 守卫这条路径)
- **上游 URL 两种写法都收**: 完整端点或 OpenAI-SDK 风格 base URL
  (`https://host/v1` 自动补 `/chat/completions`)
- node 侧 `chat.ts` 保留为 HMR 开发模式 (`make dev`) 的实现, 两端 SSE
  信封字节级同约定, 聊天页面无感知
- nginx 入口 (:18081) 用 `location = /react/api/chat` 精确反代回 httpd
  (`proxy_buffering off`), 其余 `/react/*` 仍 fastcgi_pass 到驻留后端
- 已知边界: HEAD 打到 `/react/api/chat` 不走 C 端点 (落回 `-R` 中继/静态
  链路, 返回页面); 经 `-F` FCGI 服务端路径 (nginx `fastcgi_pass` 直连
  agent-httpd) 的 chat 请求同样不经过这里 —— compose 拓扑里 nginx 对该
  路由是反代回 httpd 的, 两条边界都到不了用户

## C 原生 Agent 栈 (Tool Call / MCP / Skills / Memory / ReAct / PSE)

`src/agent/llm.c` 在 LLM_API_KEY 就绪时把请求交给完整的原生 Agent 栈而不是
单向中继。请求体从 `{message, history?}` 扩展为
`{message, history?, sessionId?}`:

```
浏览器 POST /react/api/chat {message, history?, sessionId?}
      │
      ▼ handle_client → llm_handle_chat (llm.c)
      │
      ├─ 有 sessionId ──► 载入 .data/sessions/<id>.json
      │     · 上一轮 user/assistant 转录回放成 history (节点刷新恢复会话)
      │     · 会话事实 + skills 索引注入 system_extra
      │     · 本轮 delta 全量捕获 → 转录 + 事实 原子落盘
      │
      ├─ PSE_ENABLED=true ──► pse_run (src/agent/pse.c)
      │     Planner(无工具出计划) → Specialist(ReAct 全工具循环)
      │     → Evaluator(PASS/PARTIAL/FAIL, 非 PASS 带反馈重试 ≤3 轮)
      │     · 角色提示词: $PSE_SOULS_DIR/{planner,specialist,evaluator}/SOUL.md
      │       (缺省 <cwd>/souls, 再回退内置默认)
      │     · 整场占 1 个并发槽 (agent_slot_take)
      │
      └─ 缺省 ──► agent_run → ReAct 主循环 (src/agent/agent.c)
            循环输出 assistant tool_calls → tools_dispatch → 结果回灌
            → 直到模型不再要工具 (AGENT_MAX_ROUNDS 上限, 每轮独立
              fork curl 上游, 并发受 AGENT_MAX_CONCURRENT 管道槽约束)
            · 工具表 = 内置 7 个 + MCP 工具, 动态合成 OpenAI tools schema
      │
      ▼
SSE 写回 (note/delta/error/done 信封不变)
```

**内置工具** (`src/agent/tools.c`, `tools_init()` 注册): `get_time` (北京时间 UTC+8, 容器无 tzdata 故显式 +8 小时),
`calc` (递归下降算术解析), `read_file` (web 根内解析 + 穿越防护),
`fetch_url` (http(s) 抓取首 16KB, 见下「SSRF 加固」), `skill-run` (读取技能全文),
`remember` / `recall` (会话记忆事实读写, 无 sessionId 落全局池)。

**Skills** (`src/agent/skills.c`): 按目录扫描 `SKILL.md` (frontmatter `name` /
`description`), 索引注入每个请求的 system 提示, `skill-run` 按名读全文
给模型。目录: `HARNESS_SKILLS_DIR` → `./skills` →
`resolve-skills/skills` → `SKILLS_EXTRA_DIRS` (逗号分隔)。

**Memory** (`src/agent/session.c`): 每会话一个 JSON 文件
`.data/sessions/<id>.json` (`{messages[], facts{}}`), tmp+rename 原子写;
转录回放构成上下文, `remember`/`recall` 写/读事实库, 系统提示注入
"会话记忆" 块, 重启不丢。

**MCP** (`src/agent/mcp.c`, 配置 `MCP_SERVERS` 环境变量或
`.data/mcp-servers.json`, 数组 `{id, command, args, approval}`): stdio
新行分隔 JSON-RPC; 启动时父进程 spawn 一次做 `initialize` +
`tools/list`, 每个工具注册为 `<id>:<toolName>`; 每次 `tools/call` 现拉起
一次性子进程 (spawn→initialize→initialized→call→SIGKILL), worker 常在
也不会泄漏 fd/pid; `structuredContent` 借用 `jq(1)` 美化 (二进制仍只链
libc)。`approval` 目前为审计级 (SSE 路径无交互审批通道, 打 stderr 日志)。

**测试**: smoke-test 内置 fake upstream (`scripts/fake-llm-upstream.py`,
按消息/system 分支模拟各阶段) + fake stdio MCP 服务器
(`scripts/fake-mcp-server.py`) 覆盖: 工具调用循环、`skill-run` 全文回传、
sessionId 事实持久化与 `recall` 回读、`echo:pong` MCP tools/call、
PSE 单轮 Planner→Specialist→Evaluator(PASS)。

**Agent / MCP 相关环境变量**:

| 变量 | 默认 | 说明 |
|------|------|------|
| `AGENT_TOOL_SOURCE` | `local` | 工具来源：`local` 本地注册表执行；`gateway` 交给 tsm-hub 网关（本端不下发本地 schema、不执行工具）|
| `AGENT_MAX_ROUNDS` | `8` | 工具循环最大轮数（1–30）|
| `AGENT_MAX_CONCURRENT` | `4` | agent 并发上限（管道信号量，1–32），超限回 "server busy" |
| `MCP_INIT_BUDGET_SECONDS` | `60` | 启动时所有 MCP server 握手+tools/list 的总预算，超时未完成的服务被跳过 |
| `MCP_FS_ROOT` | — | router 同步的 `fs` MCP 服务的根目录（未设则跳过该服务）|

## 隐私与合规加固

安全边界不只在请求处理层, 数据落盘与对外报错同样守:

- **本地文件权限收紧**: `logs/access.log` 与 dev 模式的 `.dev-httpd.log`
  (含 C stderr: curl 错误、router 同步等调试特征) 均为 `0600`; `.env`
  (LLM 密钥) 本就 `0600` 且被 `.gitignore`/`.dockerignore` 排除。
- **FCGI socket 默认 `0700`**: 直连 socket 可绕过 httpd 的认证/限流,
  不能对同机任意进程开放; 多用户前置代理 (nginx worker 不同账号) 用
  `REACT_FCGI_SOCK_MODE=0777` 显式放宽。
- **安全头全覆盖**: C 直服与 `-v` 代理路径 (dev Vite / 生产 react-ssr-server)
  输出同一套 `X-Content-Type-Options: nosniff` / `X-Frame-Options: DENY` /
  `Referrer-Policy: no-referrer`, `Server` 指纹统一 `AgentHTTPD`,
  `X-Powered-By` 关闭。
- **对外错误脱敏**: chat SSE 的 error 事件只给通用文案
  (`upstream connection failed; retry in a moment` 等), curl 退出码、
  `LLM_API_URL`/`LLM_API_KEY` 排查提示等基础设施细节仅写服务器本地日志;
  dev SSR 500 同样只回 "details in server log"。
- **会话数据边界**: `.data/sessions/*.json` 记录在服务器本地 (内存/事实库),
  不随响应外泄; `read_file` 工具限制在 web 根内 + 穿越防护。
- **无版本指纹 (L1)**: 错误页、目录列表页脚与 CGI `SERVER_SOFTWARE` 统一为
  无版本的 `AgentHTTPD`, 与 `Server:` 头一致 —— 任何地方都不再泄露版本号。
- **htpasswd 强制强哈希 (L2)**: 明文、13 字符 DES 与 MD5-crypt 口令加载即拒;
  只接受 `$5$`/`$6$`/bcrypt。`AGENTHTTPD_ALLOW_WEAK_AUTH=1` 是显式的开发
  逃生门 (见 Basic Auth 认证一节)。
- **dev Vite 代理加护栏 (L3)**: `-v` 保持可选 (默认关闭), 启动时大声警告
  该代理会吐出原始开发源码 —— 带 `-v` 的实例绝不能暴露到公网。
- **点前缀路径一律拒绝**: 静态与 CGI 请求的路径中出现 `.` 开头的组件时直接
  404 (不泄露存在性), 目录列表也隐藏隐藏条目 —— 误放进 docroot 的 `.env`、
  编辑器状态或版本库元数据既不可访问也不可枚举。
- **MCP 文件系统根即模型的文件沙箱**: `MCP_FS_ROOT` 下的所有内容都可被模型
  经聊天工具读写。启动时若该根下存在 `.env` 或 `.data` 会话存储会打印警告;
  请把它指向你能容忍暴露给模型的最小目录。
- **抓取的网页内容是不可信输入**: `fetch_url` 在传输层做了 SSRF 加固, 但返回
  的页面文本会进入 agent 循环 —— 恶意页面可以试图操纵模型 (prompt
  injection)。文件工具的根保持最小化, 绝不把凭据交给 agent。
- **密钥卫生写进仓库, 而不是某台机器**: 各种 `.env` 变体
  (`deploy/.env.local`、`deploy/.env.cloud.*`) 的忽略规则在仓库自己的
  `.gitignore` 里 —— 写进开发者个人的 `~/.gitignore_global` 只保护这一台
  笔记本, 换台机器或 CI 检出时密钥文件就会变成「未跟踪且可暂存」。
  `.dockerignore` 排除的是**全部** `.env` 变体外加 `.data/` (会话记录),
  尽管当前 Dockerfile 只读 `.env`: 构建上下文是整个目录送进构建器的。
  天然需要绝对路径的部署配置以 `.example` 模板入库
  (`deploy/cicdkit-project.example.json`), 真实文件忽略。
- **能触达本地数据的实例只绑回环**: 本地演示容器
  (`deploy/run-local-container.sh`) 默认发布在 `127.0.0.1` —— 它驱动的是
  通往宿主数据管线的 MCP 桥, 发布到 `0.0.0.0` 等于把这些摊到局域网上。
  确有跨机需求时用 `AGENT_HTTPD_BIND` 覆盖。
- **对外可达的实例必须开 `-l` 与认证**: 云机环境模板
  (`deploy/env.cloud.example`) 现在给出非 0 的 `RATE_LIMIT` **和**
  `AUTH_HTPASSWD` —— 两样都没有的对外容器就是一个无鉴权的 LLM 代理, 而且花的是
  你的钱。两者都是 entrypoint 层开关 (`deploy/docker-entrypoint.sh` 翻译成
  `-l` / `-a`): 用 `--env-file` 部署时没有别的地方能追加命令行参数。htpasswd
  文件缺失或不可读时容器**直接退出**, 不会退回开放访问。前面有反向代理时必须设
  `RATE_LIMIT_TRUSTED_PROXIES`, 否则所有请求共用代理那一个桶, 每 IP 限流退化成
  全局限流。或者干脆把安全组限到已知来源。

## CGI 环境变量

传给 CGI 程序的环境变量包括:

| 变量 | 说明 |
|------|------|
| `REQUEST_METHOD` | 请求方法 (GET/POST) |
| `QUERY_STRING` | URL 查询字符串 |
| `CONTENT_TYPE` | 请求 Content-Type |
| `CONTENT_LENGTH` | 请求体长度 |
| `SERVER_NAME` | 服务器名 |
| `SERVER_PORT` | 服务器端口 |
| `SCRIPT_NAME` | 脚本路径 |
| `HTTP_HOST` | 请求的 Host 头 |
| `HTTP_USER_AGENT` | 用户代理 |

## 性能设计 (事件循环 + 快/慢路径)

默认运行模式是 **master 事件循环 + 慢路径 worker 池** (`src/core/event.c`), 替代
早期的 `fork-per-connection`:

```
                     agent-httpd master (单进程, 事件循环 kqueue/epoll)
                          │  accept 所有连接, recv(MSG_PEEK) 预览请求头
        ┌─────────────────┴──────────────────┐
   快路径 (不阻塞, 直服)                慢路径 (交 prefork 池)
   GET/HEAD 静态/health/304/301/404    CGI / chat SSE / 代理 / 带 body
   keep-alive 就地多路复用              经 SCM_RIGHTS 交给 8 个 worker
```

- **快路径零 fork、零等待**: 静态文件、健康检查、重定向、错误页全部在 master
  的事件循环里完成, keep-alive 连接不占任何 worker; 大静态文件在 worker 里
  `sendfile(2)` 零拷贝。
- **慢路径隔离**: 需要阻塞的操作 (CGI 子进程、SSE 流、Vite/react 上游代理) 交给
  prefork 池, 慢请求永远不会卡住快路径。
- **判定零成本**: 用 `recv(MSG_PEEK)` 只预览不消费——慢请求交给 worker 时,
  请求头仍在内核缓冲, worker 直接重读, 无需重放字节。

### 基准 (`make bench`, 本机 macOS, 4000 请求 / 16 并发)

| 模式 | keep-alive | per-conn (每连接) |
|---|---|---|
| 旧 fork-per-connection (`-w 0`) | 10787 req/s | 846 req/s |
| **事件循环 (默认)** | **11528 req/s** | **5142 req/s** |

per-conn 场景 **约 6 倍**——每连接 fork 开销消失。keep-alive 场景两者都靠连接
复用, 事件循环微幅领先且进程数从 "每连接一个" 降到 1 + 8。

### 与 Next.js 应用的定位差异

这套架构和 Next.js 不是同一物种, 取舍如下:

| 维度 | 本项目 | Next.js |
|---|---|---|
| 公网 HTTP 层 | C 手写 (事件循环 + sendfile + 快慢分流) | Node 内嵌 (libuv 也是 C, 但过 Node 抽象 + GC) |
| 每连接开销 | 1 master + 8 worker 复用, 无 GC 停顿 | 单进程事件循环, 靠 keep-alive 复用 |
| 慢请求隔离 | CGI/SSR/chat 独立 worker, 不卡快路径 | 长 agent 循环会占事件循环 (需 worker/队列) |
| SSR/ISR | 自建 react-ssr-server (共享 render.tsx + ISR 缓存) | Next 内置 RSC/SSG/ISR/流式 |
| agent | C 内建 (llm.c + MCP + skills), 改逻辑要重编译 | TS/Node, 热更 + 生态 |
| 开发体验 | `make dev` 双进程, C 改动要 make+重启 | `next dev` 一体化 |
| 安全面 | 手写 HTTP, 边界自己守 (已修 SSRF/越界/泄露, 安全头/脱敏/socket 权限加固) | 框架管路由/编码/头部 |

**一句话**: 比"渲染 + 产品迭代速度", Next.js 赢; 比"最少资源扛住最高并发的
HTTP/agent 面", 这个 C 架构赢——赢在 Next 换不来的无 GC、零拷贝、进程隔离。

## 后续扩展方向

- [x] POST 请求体转发到 CGI (stdin pipe + CONTENT_LENGTH)
- [x] REST 方法: PUT/PATCH/DELETE 透传 CGI (REQUEST_METHOD), OPTIONS 直答 Allow (静态只读 405 防护, 快路径直答 CORS 预检)
- [x] 目录穿越防护、目录列表、trailing-slash 重定向、自定义错误页
- [x] Combined 格式访问日志 + SIGHUP 轮换
- [x] 静态资源 gzip 预压缩协商 (Accept-Encoding + Content-Encoding)
- [x] ETag/304 条件请求 (If-None-Match, 强校验器 size+mtime, 覆盖 gzip 表亲)
- [x] Last-Modified + If-Modified-Since 回退验证 (含原样回放自家 Last-Modified)
- [x] 静态响应的 `Cache-Control: no-cache` 策略 (可存但须再验证, 304 兜住开销)
- [x] CGI 与 SSR 文档一律 `no-store` (脚本自带指令优先) + 可选的一次性清缓存 `Clear-Site-Data` (`PURGE_CLIENT_CACHE`)
- [x] 带内容指纹的 bundle URL (`/js/react-ssr.js?v=<hash>`, 构建期嵌入) —— 重建的 bundle 不会再被缓存副本顶掉
- [x] 中继请求记录真实响应体字节数 (此前每条中继响应在访问日志里都记 0)
- [x] sendfile(2) 零拷贝 + Range/206 断点续传 (单区间, 416, Accept-Ranges)
- [x] TCP_NODELAY + listen backlog 128
- [x] 优雅停机排水 + /health 端点
- [x] FastCGI 后端 (UNIX socket, 复用 HTTP 分发链)
- [x] HTTP/1.1 Keep-Alive 连接复用 (RFC 7230: 1.1 默认持久, 1.0 需显式 keep-alive, `Connection: close` 总是生效; 单连接上限 100 请求, 5xx 响应后关闭, 流式转发响应因无 Content-Length 仍按 close 语义)
- [~] 线程池代替 fork 进程 —— **评估后不做**: 与 prefork worker 池 (`-w N`) 收益重叠 (隔离性反而更差, CGI fork+exec 模型下线程池优势有限); 项目已有两种并发模型可选, 再加第三种只增加教学噪音
- [x] Basic Auth 认证 (`-a htpasswd` + `-r realm`, **仅强哈希** `$5$`/`$6$`/bcrypt, fail-closed, CGI 经 `REMOTE_USER` 获取用户; 弱哈希须显式 `AGENTHTTPD_ALLOW_WEAK_AUTH=1`)
- [x] 每 IP 限流 (`-l <rps>`, 令牌桶 + 共享内存计数表, fork/worker 池共用配额, IPv4/IPv6 key, 可信反代下的 X-Forwarded-For, 429 + Retry-After)
- [x] 双栈监听 (同端口绑 IPv4 `0.0.0.0` + IPv6 `::`, 显式置 `IPV6_V6ONLY`, 两条 accept 路径都服务两族, 主机无 v6 栈时优雅退回纯 IPv4)
- [x] chat 上游瞬时故障重试 (3 次尝试 + 1s/2.5s 退避, 瞬时/永久错误分类判定, 退避期监听客户端断开)
- [x] 隐私与合规加固 (安全头代理同构、FCGI socket 默认 0700、日志文件 0600、对外错误脱敏)
- [x] MCP 子进程密钥脱敏: 在 `execvp` 前的子进程里 `unsetenv` 掉 LLM 凭据 (`LLM_API_KEY` / `LLM_API_URL` / `LLM_MODEL`), 因此经 npx 拉取的第三方 MCP server (如 `server-filesystem`) 绝不会继承密钥。(保留 `MCP_FS_ROOT` —— fs server 需要它作沙箱根; CGI 侧在 `cgi.c` 同样脱敏这三项。)
- [x] 会话记忆严格按会话隔离: `remember` / `recall` 在没有 `sessionId` 时直接拒绝, 聊天提示词也不再注入共享全局池 (`.data/memory.json`)。无会话的请求既不能读也不能写其他用户的 fact (旧的全局回退是跨用户数据泄露 / 提示注入源)。
- [x] 访问日志剥离 query string: 只记录路径, `?...` 里的 token / 会话 id / PII 永不落盘到 `logs/access.log` (该文件权限 `0600`)。
- [x] 响应安全头: 在原有 nosniff / X-Frame-Options / Referrer-Policy 之上, 每个响应新增 `Content-Security-Policy` (`default-src 'self'`、`frame-ancestors 'none'`、`base-uri 'self'`、`object-src 'none'`)、`Cross-Origin-Opener-Policy: same-origin`、`Cross-Origin-Resource-Policy: same-origin`、`X-Permitted-Cross-Domain-Policies: none`。(HSTS 应放在终结 TLS 的 nginx 层。)
- [x] 聊天 CSRF 加固: `/react/api/chat` 仅接受 POST (GET 及其他动词在分发前即被拒); 浏览器 `Origin` 与服务器 `Host` 不一致的跨站请求以 `403` 拒绝。无 `Origin` 的非浏览器客户端 (含自带冒烟/测试脚本) 与同源请求放行; nginx 经 `proxy_set_header Host $host` 保留 Host。
- [x] `fetch_url` SSRF 加固: 对 IP 字面量、方括号 IPv6、以及**每一个**解析出的地址, 一律拦截回环 / 私网 / 链路本地 / 云元数据 (169.254.169.254); 剥离 `userinfo@` 主机混淆; **DNS 解析失败按失败关闭** (fail-closed); **每一个** HTTP 重定向跳都要重新校验, 因此 `302` 跳到内网地址绝不会被跟随。私网 / 回环地址**始终拦截, 设计上无开关可削弱**。
- [x] SSE 反代友好: 聊天流输出 `X-Accel-Buffering: no`, 防止前置 nginx (:18081) 缓冲; 另发 15s 一次的 `:` 心跳注释, 防止空闲反代在长生成时掐断连接。
- [~] 多虚拟主机 —— **暂缓**: 教学场景单 docroot 已够; 真需要时前置 nginx 按 Host 分流到多个 agent-httpd 实例即可, 不必在 C 层重新实现
- [x] URL 路由 (react-router 客户端路由 + SSR 深链; C 层 `/react/` 转发)
- [~] SSL/HTTPS 支持 —— **评估后不做**: 生产部署惯例是 nginx/负载均衡终结 TLS (本仓库的 docker-compose 就是这个形态); 在教学服务器里集成 OpenSSL 会显著膨胀代码而偏离主线

## 许可

MIT —— 见 [LICENSE](LICENSE)。前端依赖保留各自许可 (React、Vite、Tailwind 均为 MIT)。