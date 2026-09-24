# AgentHTTPD 架构文档

> 本文面向要读懂源码、二次开发或做代码评审的读者；功能与用法请看 [README.zh.md](../README.zh.md)。
> 文中引用格式为 `src/文件.c:行号`，行号以当前 main 分支为准。

## 0. 总览

AgentHTTPD 是一个 ~7,700 行 C 的教学级生产级混合体：**HTTP 服务器 + CGI 运行时 +
FastCGI 双向实现 + React SSR 编排 + 原生 LLM Agent 栈**。设计上始终遵守三条
铁律：

1. **二进制只链 libc**——TLS 交给 fork 出的 curl(1)，JSON 用手写容错解析器，
   MCP 美化借用 jq(1)。"不引 OpenSSL/不引 JSON 库" 是刻意的项目约定。
2. **慢操作永不进主进程**——一切可能阻塞的行为（CGI 子进程、SSE 流、上游代理）
   都被隔离在 worker 池或一次性子进程里。
3. **dev 与 prod 同构**——浏览器永远只对 C 说话；开发时的 Vite 与生产时的
   react-ssr-server 对 C 而言是同一个 `-v` 上游。

```
                     ┌────────────────────────────────────────────┐
                     │              agent-httpd 进程              │
                     │                                            │
   accept() ───────► │  master 事件循环 (src/core/event.c)        │
                     │   │ 快路径直服 (不 fork):                  │
                     │   │   GET/HEAD 静态、health、304/301/404、 │
                     │   │   无 body 的 OPTIONS                   │
                     │   │                                        │
                     │   │ recv(MSG_PEEK) 预览头 → 判定慢路径:    │
                     │   ▼                                        │
                     │  prefork worker 池 × 8 (src/core/worker.c) │
                     │   │  CGI / chat SSE / -v 代理 / 带 body    │
                     │   │  fd 经 SCM_RIGHTS 传入, 不重放字节     │
                     └───┼────────────────────────────────────────┘
                         │
        ┌────────────────┼──────────────────┬─────────────────┐
        ▼                ▼                  ▼                 ▼
   fork+exec CGI     fork curl -N      HTTP 反代 (-v)      FCGI 客户端 (-R)
   (src/cgi/cgi.c)   (src/agent/agent.c)  dev: Vite         (src/cgi/fastcgi.c)
                     POST 上游 LLM     prod: react-ssr-
                     API, 流式 SSE      server + ISR 缓存
```

## 1. 进程模型：事件循环 + 慢路径池

入口 `src/core/main.c`。启动顺序有讲究——**所有共享状态的初始化必须在 fork 之前**，
让 worker 继承而非重建：

```
main()
 ├─ getopt: -p/-F/-R/-v/-w/-a/-r/-l/-L/-T/-n
 ├─ rate_limit_init()      # MAP_SHARED 计数表 (fork 前建立)
 ├─ agent_init()           # 并发槽管道信号量 (fd 需被继承)
 ├─ session_prune_old()    # 30 天会话清理
 ├─ router_sync_all()      # llm-router 目录同步 → skills/router/ + .data/mcp-servers-router.json + 暂存 /v1/tools 目录
 ├─ skills_init()          # SKILL.md 扫描 (src/core/framework.c:375)
 ├─ tools_init()           # 内置 7 工具注册 (src/core/framework.c:376)
 ├─ mcp_init()             # 每个 MCP server 起一次性子进程做 tools/list (src/core/framework.c:377)
 ├─ router_register_tools()# 暂存的 router 工具注册为本地代理 (本地内建优先同名) (src/core/framework.c:379)
 ├─ create_server_socket() + create_server_socket6()   # IPv4 + IPv6 双监听
 ├─ 可选 create_fastcgi_listener(-F)
 └─ start_worker_pool(8)   # 成功 → event_loop(); 失败 → 回退 fork-per-connection
```

两种运行模式（`-w 0` 切换）：

| | master 事件循环 + 池（默认） | fork-per-connection（旧模型） |
|---|---|---|
| 快路径 | master 内零开销直服 | 每连接 fork |
| 慢路径 | `pool_dispatch_fd` 经 SCM_RIGHTS 传 fd | 每连接 fork |
| 基准 | per-conn **5142 req/s**（6 倍） | 846 req/s |

### 1.1 master 事件循环（`src/core/event.c`）

- 单进程 kqueue（macOS）/ epoll（Linux），accept **两个监听 socket** 上的所有连接：
  IPv4（`create_server_socket()`）与 IPv6（`create_server_socket6()`，绑定 `::` 且
  **显式置 `IPV6_V6ONLY=1`**——否则 `::` 会连 v4 一起吞掉、导致 v4 的 `bind` 撞
  `EADDRINUSE`，且该选项的默认值各平台不一致）。两个 listener 由 `accept_http()`
  按 fd 分流。主机没有可用 v6 栈时只打印一行提示并退回纯 IPv4。
- **客户端地址统一为 `sockaddr_storage`**：`handle_client()` 收 `(fd, sockaddr*, socklen_t)`，
  `Conn` 存通用 peer 地址，`sockaddr_to_str()`（`src/core/util.c`）统一做
  `inet_ntop`（v4/v6），未知族回退 `"-"` 以守 CLF 访问日志约定。
- **`recv(MSG_PEEK)` 零消费预览**：只偷看请求头判断快/慢，不消费字节。慢请求
  交给 worker 时头字节仍在内核缓冲，worker 的 `handle_client` 像自己 accept 的
  一样直接重读——不需要主循环→worker 的字节重放（`src/core/event.c:9-12`）。
- 快路径判定在 `is_fast_request()`（`src/http/http_route.c:19`）：GET/HEAD/OPTIONS、
  无 body、非 CGI、非 chat 路由、非 `-v` 代理路由。
- 快路径内部响应（错误页/默认 Content-Type 等）由 `src/core/event.c:199` 处
  `fast_serve()` 里的序列化逻辑生成，慢路径分派时丢弃重建。

### 1.2 prefork worker 池（`src/core/worker.c`）

- N 个 worker 各自阻塞在 UNIX socketpair 上，**从不碰监听 socket**。
- **fd 传递**：`pool_dispatch_fd` 用 SCM_RIGHTS 把 client_fd 发给空闲 worker。
- **并发上限 = 管道计数信号量**：管道初始放 N 个 token 字节（= 空闲 worker 数）；
  master 派活前读走 1 个 token，worker 干完写回。单字节管道操作原子，macOS
  上避开未实现的 unnamed POSIX sem（`src/core/worker.c:1-15` 注释）。
- **优雅排水**：SIGTERM → worker 设 `g_shutdown_requested` → 完成手头连接
  （keep-alive 循环见到标志即关）→ 父进程排水最多 5s → SIGKILL 兜底。

### 1.3 fork-per-connection（`-w 0`）

经典模型，`src/core/framework.c:481` 的 select 循环。保留原因：教学对照 + 池化
故障时的回退路径（`src/core/framework.c:420` 启动失败自动降级）。

## 2. HTTP 核心（`src/http/http.c`）

一次请求的完整生命周期（含可观测埋点 `src/core/metrics.c`——状态码分类在
log_request 汇聚，快/慢分流埋在 event.c 的 fast_serve/go_slow，计数表为
MAP_SHARED 原子增量，快路径零锁）：

```
recv 头 → parse_request (行/头/字长校验, 431 防护)
       → [限流 ratelimit.c: 先于认证, 429+Retry-After]
       → [认证 auth.c: 401+WWW-Authenticate, 全站门禁]
       → Expect: 100-continue? 即时下发 100 (src/http/http.c:434-440)
       → 读 body (与 Method 无关, 只看 content_length; 413/408 防护)
       → process_request (src/http/http_route.c:76)
            │ 方法分发 (见 §3)
            ├─ CGI 路径 → execute_cgi
            └─ 静态路径 → handle_static_file
       → build_response 序列化 (Date/安全头/ETag/Allow...)
       → keep-alive 循环 (≤100 请求/连接, 5xx 后关闭, SSE 以 close 定界)
```

关键机制：

- **`process_request()` 是唯一分发核心**（`src/http/http_route.c:76`）：HTTP 模式、
  FCGI 服务端模式（`src/cgi/fastcgi.c:583` 重建请求后调用同一个函数）、worker 池、
  fork 模式全部收敛到这一条链——四处路径一份语义。
- **`is_fast_request()` / `is_vite_proxy_route()`** 是事件循环的两个路由谓词，
  在头解析后、process_request 前调用。
- **`build_response()`**（`src/http/http_resp.c:57`）统一序列化：每响应必带 Date（RFC
  9110 6.6.1）、安全头三件套（nosniff/DENY/no-referrer）、`Server: AgentHTTPD`
  固定指纹。`Allow` 头（405/OPTIONS）走 `response->allow`。

## 3. 方法分发的语义分层（`src/http/http_route.c:120-131`）

```
GET/HEAD          → 读链 (静态 or CGI)
POST/PUT/PATCH/DELETE → 仅 CGI 路径放行 (REQUEST_METHOD 透传, src/cgi/cgi.c:88)
                       静态 URL → 405 + Allow: GET, HEAD, OPTIONS   ← 服务器不写盘
OPTIONS            → C 直答 200 + Allow 菜单
                       静态 = 读方法集;  CGI = 全方法集
                       无 body 的 OPTIONS 进快路径 (is_fast_request)
其他               → 501 Not Implemented
```

设计取舍：**静态文件只读是安全边界**（教学服务器绝不能把 PUT 落到 www/）；
REST 能力通过 CGI 透传实现——脚本从 `REQUEST_METHOD` 拿到动词，服务器不掺和
语义。OPTIONS 由服务器代答（RFC 9110 9.3.1 允许），避免每个脚本各自暴露
能力清单。

## 4. 静态文件链（`src/http/static.c`）

```
resolve_within (realpath + .. 逃逸防护, src/http/static.c:44)
 → MIME (src/core/util.c 扩展名表)
 → ETag: W/"size-mtime" (强校验器; If-None-Match 命中 → 304, 传输 -94%)
 → Last-Modified + If-Modified-Since (让位 If-None-Match, 规范优先级)
 → gzip 协商: 同目录 .gz 表亲 + Accept-Encoding q 值解析 (RFC 7231)
 → sendfile(2) 零拷贝 (macOS/Linux; 其他平台 fread 回退)
 → Range/206: 单区间 N-M/N-/-N, 越界 416, 多区间回退 200
 → 目录列表 (bounded 64KB, XSS 转义; 无尾斜杠 301)
```

## 5. CGI/1.1（`src/cgi/cgi.c`）

```
fork ─► 子进程: dup2 管道 → setenv (REQUEST_METHOD/QUERY_STRING/CONTENT_*,
        REMOTE_USER, REMOTE_ADDR, RFC 3875 全套) → execl 目标脚本
   │
   ├─ POST body: 父进程写入 stdin (select 轮询, 脚本不读也不卡死)
   ├─ 输出缓冲按需增长; >512KB (CGI_BODY_TMP_THRESHOLD) 落盘 /tmp 流式
   ├─ 静默 CGI_TIMEOUT_SECONDS → SIGKILL + 504
   ├─ 客户端断开 (socket 轮询) → 立即杀子进程 (零泄漏)
   └─ 头解析: Location → 302, Status → 覆盖状态行, Content-Type 透传
```

多语言 CGI 动物园（bash/python/ruby/php/go/rust/java/React）全部走这一条
链——语言差异只剩 execl 的目标是什么。

## 6. FastCGI 双向（`src/cgi/fastcgi.c`）

同一个文件实现两半：

- **服务端（`-F`）**：`fcgi_handle_connection` 解析 FCGI 帧（BEGIN_REQUEST/
  PARAMS/STDIN），重建 `HttpRequest` → **复用 `process_request`** → 响应按
  FCGI_STDOUT 记录回写。nginx `fastcgi_pass` 直连时走这条。
- **客户端（`-R`）**：`forward_to_fcgi` 把请求编码成 FCGI 帧发给常驻 React
  后端，收齐 HTTP 响应回传。旧接线，保留给 socket 直连场景。

## 7. React 渲染接线（统一 `-v`）

```
浏览器 ──GET /react/?name=A──► agent-httpd :PORT (唯一公网入口)
  │  静态/CGI/health/chat/错误页 = C 直服
  │  /@*, /src/*, /react/*(除 chat) = HTTP 反向代理 (-v)
  │  HMR WebSocket = TCP 隧道 (is_websocket_upgrade, src/http/http_route.c:54)
  ▼
127.0.0.1:PORT+2 ◄── dev: Vite (scripts/dev-server.js)
                  ◄── prod: bin/react-ssr-server (REACT_HTTP_PORT, ISR 缓存)
```

三个不变量：

1. **一条代码路径**：dev 的 Vite 与 prod 的 react-ssr-server 都是 C 的 HTTP
   上游，中间件层（安全头、HMR 隧道）行为一致。
2. **`/react/api/chat` 永不被代理**：`is_vite_proxy_route` 显式排除
   （`src/http/http_route.c:39`），chat 始终由 C 原生处理。
3. **FCGI 是逃生口**：`bin/react-ssr-server` 在 `REACT_HTTP_PORT` 未设时回退
   原生 FCGI 监听，供 nginx `fastcgi_pass`；socket 权限默认 0700（直连绕过
   auth/限流，只许 owner），多用户前置代理用 `REACT_FCGI_SOCK_MODE` 放宽。

## 8. LLM Chat / Agent 栈

这是仓库里最大的子系统，四个模块分层清晰：

| 模块 | 职责 |
|---|---|
| `src/agent/llm.c` | **端点壳**：路由谓词、`{message, history?, sessionId?}` 解析、`.env` 的 `LLM_*` 加载（环境变量优先）、离线演示引擎 |
| `src/agent/agent.c` | **ReAct 主循环**：fork curl 上游、tool_calls 流式分片累积、每轮独立 fork、并发槽、上游重试/退避 |
| `src/http/chatio.c` | **SSE 信封**：note/delta/error/done 四事件 + 捕获缓冲 |
| `src/core/minijson.c` | sbuf + 容错 JSON 读取器（\uXXXX/代理对/嵌套跳过） |

请求路由（`llm_handle_chat`）：

```
有 LLM_API_KEY?
 ├─ PSE_ENABLED=true  → pse_run (src/agent/pse.c): Planner(无工具出计划)
 │                       → Specialist(ReAct 全循环) → Evaluator(PASS/FAIL,
 │                       非 PASS 带反馈重试 ≤3 轮); 整场占 1 个并发槽
 └─ 否则              → agent_run: 单一 ReAct 循环
每轮: 构建 messages (system+history+user+tool 回灌)
      → fork curl -N POST <LLM_API_URL>/chat/completions (stream:true, tools:[...])
      → 上游 SSE 逐行解析: content delta → "delta" 事件
                            tool_calls 分片 → 按 index 累积
      → finish_reason=stop 且无 tool_calls → 结束
      → 有 tool_calls → tools_dispatch 执行 → note 事件 → 回灌 → 下一轮
      (上限 AGENT_MAX_ROUNDS=8; 并发受 AGENT_MAX_CONCURRENT=4 管道槽约束)
```

**并发槽**（`src/agent/agent.h:1-30` 注释）：agent 一跑可能几分钟，占住 worker。管道
信号量限制同时在跑的 agent 数；满槽时 fail-fast 返回 "busy" 错误事件——
保护 worker 池不被长会话饿死。PSE 全场算 1 个槽。

**上游瞬时故障重试**（`src/agent/agent.c:657` `agent_retryable` + 586 重试循环）：

```
可重试: UP_ERR_TIMEOUT, curl 7/18/35/52/55/56 (连接拒绝/重置/部分传输),
        HTTP 429/5xx (上游配额/网关)
不重试: 4xx 拒绝 (22), 127 (无 curl), 本地 UP_ERR_LOCAL
节奏:   3 次尝试 (AGENT_UPSTREAM_ATTEMPTS), 间隔 1s/2.5s 指数退避;
        退避按 100ms 小步睡, 每步检查 out->ok —— 客户端断开立即放弃
```

幂等性保证在 `agent_run_inner`（`src/agent/agent.c:676` 注释）：**messages 只在整轮成功
后追加**，失败尝试不提交任何状态，重试从同一快照开始。

**工具生态**：

| 层 | 实现 | 说明 |
|---|---|---|
| 内置 7 工具 | `src/agent/tools.c` | get_time/calc/read_file(fetch 16KB)/skill-run/remember/recall；`tools_dispatch` 统一分发 |
| Skills 索引 | `src/agent/skills.c` | SKILL.md frontmatter 扫描 → 注入 system 提示；目录链 HARNESS_SKILLS_DIR → ./skills → resolve-skills/skills → SKILLS_EXTRA_DIRS |
| Memory | `src/agent/session.c` | `.data/sessions/<id>.json` `{messages[], facts{}}`，tmp+rename 原子写；转录回放构成 history；30 天自动清理 |
| MCP | `src/agent/mcp.c` | stdio JSON-RPC；启动 spawn 一次做 initialize+tools/list（pre-fork，worker 继承活代理）；每次 tools/call 起一次性子进程，不泄漏 fd/pid |

**MCP 的两层生命周期**是有意设计：`tools/list` 是启动期一次性成本
（pre-fork 让所有 worker 共享结果），`tools/call` 是请求期一次性 spawn
（worker 常驻也不积累子进程）。`approval` 字段目前审计级——SSE 路径没有
交互审批通道，只打 stderr 日志。

**安全边界**（对应 README「隐私与合规加固」）：

- `read_file` 限制在 web 根 + realpath 穿越防护
- SSE error 事件脱敏（curl 退出码/env 提示只进服务器日志）
- `fetch_url` 抓取上限 16KB

## 9. 安全模型（纵深）

请求必须穿越的有序防线：

```
TCP accept (backlog 128, TCP_NODELAY)
 → 请求头 64KB 上限 (431) / 半开请求超时断开 (Slowloris)
 → body 1MB 上限 (413) / Expect:100-continue 即时响应
 → 每 IP 限流 (先于认证——洪水烧不到 crypt CPU; 429+Retry-After+close)
 → Basic Auth (fail-closed: 全无效拒绝启动; 401 后强制 close)
 → 路径 realpath 穿越防护 (静态/CGI/read_file 工具共用)
 → MIME 白名单 + nosniff + DENY + no-referrer (每响应)
 → 方法白名单 (静态只读 405; 未知 501)
 → CGI 子进程隔离 (超时杀/断开杀/资源上限)
 → SSE 错误脱敏 + 会话数据不出本地
 → 日志/数据文件 0600, FCGI socket 0700
```

## 10. 构建产物与部署拓扑

```
make
 ├─ bin/agent-httpd          # C 服务器 (~7.7k 行, 只链 libc)
 ├─ bin/react-ssr-server    # 常驻 SSR 后端 (esbuild bundle, node 运行时)
 └─ cgi-bin/* + www/js/*    # CGI 动物园 + 客户端 bundle

docker compose (三服务):
 ├─ httpd   :  agent-httpd -F (FCGI 服务端) + -R (React 中继)
 ├─ react   :  react-ssr-server (FCGI 模式, UNIX socket, 0700)
 └─ nginx   :  / → proxy_pass httpd;  /react/ → fastcgi_pass react
               /react/api/chat → location = 精确反代回 httpd (proxy_buffering off)
```

## 11. 模块依赖图

```
                    main.c
                      │ 初始化顺序: ratelimit → agent → session → router
                      │            → skills → tools → mcp (全部 pre-fork)
                      ▼
        ┌────────── event.c (master 循环) ──────────┐
        │            │ 快路径直服                    │ 慢路径 dispatch
        │            ▼                              ▼
        │        http.c ◄── process_request ──── worker.c (池)
        │        │    ▲                             │
        │        ▼    │                             ▼
        │    static.c │                        (同一 handle_client)
        │             │                             │
        └─── fastcgi.c (服务端 -F 也走 process_request; 客户端 -R)
                      │
                      ▼
              llm.c (chat 路由谓词先于 -R)
               ├─ pse.c ──► agent_round (复用)
               └─ agent.c ─► tools.c ──► skills.c / session.c / mcp.c
                              │
                              └─ chatio.c + minijson.c (SSE 信封/JSON)
```

要点：

- `process_request`（http.c）是**收敛点**——HTTP、FCGI 服务端、池、fork 四条
  入口路径一份语义。
- agent 子树（llm/pse/agent/tools/skills/session/mcp/chatio/minijson）是
  **闭环**，只依赖 httpd.h 的类型和 chatio 的信封，不反向侵入 HTTP 层。
- `router.c` 的同步/注册（`router_sync_all` / `router_register_tools`）只在
  启动/SIGHUP 时跑：把 llm-router 目录落到本地 skills/MCP 配置，并把
  `/v1/tools` 目录（跳过 `mcp:*` 与本地同名内建）注册为 POST
  `/v1/tools/invoke` 的代理工具。注册后代理工具的**执行**在请求路径上——
  模型每次调用它都会走一次网关往返；本地内建同名工具优先，不绕网关。

## 12. 阅读路线建议

按依赖顺序读，每个文件头部注释都是设计文档（本项目惯例：注释写 why）：

1. `src/core/event.c` + `src/core/worker.c` —— 进程模型与快慢分流
2. `src/http/http.c` 的 `handle_client`/`process_request` —— 请求生命周期
3. `src/http/static.c` + `src/cgi/cgi.c` —— 两条分发链
4. `src/agent/agent.c` 头部 + `agent_run_inner` —— ReAct 循环与重试/并发
5. `src/agent/llm.c` 的 `llm_handle_chat` —— chat 端点路由
6. `src/agent/mcp.c` 的 `mcp_config_load` + 两层生命周期 —— MCP 桥
7. 其余（auth/ratelimit/pse/session/skills/tools/router）按需查阅

## 13. 已知边界与设计取舍记录

| 决策 | 理由 |
|---|---|
| 不集成 OpenSSL | nginx/负载均衡终结 TLS 是生产惯例；教学二进制膨胀不可取 |
| 无多虚拟主机 | 单 docroot 够教学用；真需要时前置 nginx 按 Host 分流多个实例 |
| 线程池不引入 | 与 prefork 池收益重叠，CGI fork+exec 模型下隔离性反而更差 |
| OPTIONS 由服务器代答 | 脚本各自暴露能力清单会失控；C 直答 Allow 保证一致性 |
| HEAD 打 chat 路由不走 C 端点 | 落回 `-R`/静态链返回页面；SSE 语义上只有 POST/GET |
| MCP `approval` 仅审计级 | SSE 是单向流，没有交互审批通道；先打日志留扩展点 |
| 上游重试只认瞬时类 | 4xx/127 重试必然同样失败；退避期间监听断开避免白占 worker |

---

*维护提示：本文的行号引用随代码演进而漂移，以文件头部注释与函数名为准；
发现文档与代码不一致时，以代码为准并顺手更新本文。*
