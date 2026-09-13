# agent-httpd 框架化实录

> 本文记录 2026-09-13 把 agent-httpd 从「单体二进制」改造为「可嵌入框架」的完整过程：
> 动机、设计取舍、每一步改了什么、为什么这么改、验证结果。对应示例见 `examples/embedded.c`，
> 公开 API 见 `src/agenthttpd.h`。

## 一、动机：为什么框架化，以及为什么不做"通用 Web 框架"

agent-httpd 此前是一个教学向单体：`main.c` 拥有全部装配逻辑（解析 argv → 装配置 →
初始化注册表 → 起 socket → 进事件循环），想把它嵌进自己的 C 程序只能整个二进制跑起来。

方向选择上先排除了一个歧路：**不做通用 C Web 框架**。那条路上 libmicrohttpd /
mongoose / drogon 已经站好了位，C 的 Web 受众本身也小。这个项目真正独特的资产是
**Agent 栈**——纯 C、零依赖、单二进制里内置 MCP 客户端 / 工具注册表 / 会话记忆 /
PSE 编排的 Agent 服务端，这在 C 语言生态里没有对标物。所以定位是：

> **Agent 服务框架**：把 agent-httpd 变成一个可链接的库，让你在任何 C 程序里
> 嵌入一个"自带 Agent 的 HTTP 服务器"，并且能用任意语言写它的工具。

## 二、改造前的盘点（哪些是白捡的）

动手前扫了一遍现有代码，发现框架化的三大件里有两件已经存在，只需要"接出来"：

| 框架需要的能力 | 现状 | 结论 |
|---|---|---|
| 动态工具注册 | `tools_register()` 已存在（MCP 层在用），`tools_init()` 只追加不清表 | **白捡**——`agenthttpd_tool` 直接包一层即可 |
| HTTP 分发点 | `process_request()` 是所有请求的必经之路（fast path 与 worker 池都汇到它） | 只需在方法闸门前挂一个路由表钩子 |
| 外部进程扩展 | FastCGI 客户端/服务端、CGI exec、MCP spawn 都有 | 选了 **exec 工具**形态（最贴合"工具即进程"，复用 CGI 的密钥清洗经验） |
| 配置装配 | 散在 main.c 的 getopt + 全局变量 | 需要收敛成 `agenthttpd_config` |

关键约束（设计前必须吃透的既有事实）：

1. **SIGCHLD 是进程级 `SIG_IGN`**（main.c）：内核自动收割子进程，裸 `waitpid`
   会 ECHILD 且 status 不动——这是这个项目踩过并写进注释的坑，exec 工具必须
   沿用 `reaped == pid` 才信退出码的守卫。
2. **预 fork 快照模型**：工具表 / skills / MCP 在 fork 前初始化，worker 继承
   冻结副本。所以路由和工具的注册必须是"run 之前"——run 之后的注册既不可见
   （worker 看不到）又有竞态，直接拒绝而不是假装成功。
3. **fast path 与 slow path 都汇入 `process_request`**：自定义 GET 路由会跑在
   master 事件循环（进程内），带 body 的 POST 跑在 fork 出的 worker 里——
   这决定了"handler 必须无状态"的契约。

## 三、设计决策

### 3.1 公开 API 面（`src/agenthttpd.h`）

刻意做小，只有 4 个函数 + 1 个结构体：

```c
typedef struct { int port; int workers; const char *docroot; /* ... */ } agenthttpd_config;
typedef int (*agenthttpd_route_fn)(HttpRequest *, HttpResponse *);

int agenthttpd_route(const char *method, const char *path, agenthttpd_route_fn fn);
int agenthttpd_tool(const char *name, const char *desc, const char *params_json, ToolFn fn, void *data);
int agenthttpd_tool_exec(const char *name, const char *desc, const char *params_json, const char *command);
int agenthttpd_run(const agenthttpd_config *cfg);   /* 阻塞，返回进程退出码 */
```

取舍：

- **config 用 struct 而不是 setter 链**：C 里 setter 链要么宏魔法要么冗长；
  零初始化 + NULL/0 即默认值的 struct 是最诚实的 C 风格，也和现有 `-p/-w/-a`
  旗标一一对应。
- **路由 handler 直接复用 `HttpRequest`/`HttpResponse`**：不为框架另造一套
  请求/响应抽象——那只会制造两套语义。handler 返回 0 = 已处理，-1 = 放行给
  内建 CGI/静态分发（组合而非覆盖）。
- **`agenthttpd_run` 返回退出码而不是 server 句柄**：这个服务器的并发模型是
  prefork/事件循环，不是"一个 server 对象多线程跑"。嵌入场景下进程生命周期
  即服务生命周期，阻塞语义最简单也最诚实。

### 3.2 路由匹配语义

- 方法精确匹配（`"*"` = 任意）；路径精确匹配，模式尾缀 `*` 时前缀匹配；
- 匹配前剥掉 query string（`/health?x` 这种带 query 的路径在此项目里合法）；
- 分发点在方法闸门**之前**（`process_request` 开头），所以自定义路由可以用
  POST 而不用伪装成 `/cgi-bin/` 路径，也可以自己处理 OPTIONS；
- handler 返回 -1 放行 → 内建 405/静态/CGI 逻辑接管。

### 3.3 exec 工具（外部工具进程）

`agenthttpd_tool_exec(name, desc, params, "python3 tool.py")` 的执行模型：

```
LLM tool_call ──► tools_dispatch ──► fork ──► /bin/sh -c <command>
                    │ args JSON        │        stdin  = 工具参数 (JSON)
                    ▼                  │        stdout = 工具结果 (任意文本)
                 sbuf result ◄─────────┘        stderr 合并进结果
```

- 密钥清洗：子进程 `unsetenv(LLM_API_KEY/LLM_API_URL/LLM_MODEL)`，与 CGI/MCP
  子进程同一纪律——第三方工具代码永远看不到 LLM 凭据；
- 输出封顶 256KB，超限截断并标注；读端读到 EOF 才收，子进程不会卡死在满管道上；
- 退出码只在 `reaped == pid` 时可信（SIGCHLD=SIG_IGN 的既有守卫），非零退出
  以 `[exit code N]` 附在结果尾部交给模型自行判断；
- 命令串是**开发者写死的配置**（类比 crontab），绝不接受模型输出拼接——
  模型只能控制 stdin 里的参数 JSON，这就是注入边界。

### 3.4 main.c 的归宿

改造后 `main.c`（383 → 150 行）只剩一件事：getopt 解析 → 填 `agenthttpd_config`
→ `agenthttpd_run(&cfg)`。CLI 二进制因此变成框架的**第一个用户**——这保证了
公开 API 覆盖了 CLI 的全部能力（任何 `-x` 旗标都能在 config 里找到对应字段），
API 不会退化成"演示用的第二接口"。

### 3.5 Node 宿主集成（`examples/node-host/`）

「其他运行时怎么用这个框架」的第一个答案：**Node 进程当宿主，C 服务器当受管子进程**。
`examples/node-host/host.js`（`make node-example` 一键跑）演示完整生命周期：

```
node (host.js)                          examples/embedded (:18101)
  ├─ spawn(cwd = repo 根)  ──────────►  agenthttpd_run()
  ├─ 轮询 GET /api/status  ──就绪───►   master event loop
  ├─ GET  /api/status    ──► JSON 存活
  ├─ POST /api/echo      ──► worker 池回显
  ├─ POST /api/tool      ──► tools_dispatch → python3 wordcount.py（真实工具链路，无需 LLM）
  ├─ GET  /api/nope      ──► 内建 404 兜底
  └─ SIGTERM             ──► 优雅排水退出
```

两个设计要点：

1. **进程边界是刻意的**。曾评估把 `libagenthttpd.a` 链进 N-API addon（Node
   原生插件）——否决：`agenthttpd_run()` 是阻塞 accept 循环 + 自带
   SIGINT/SIGTERM 处理 + prefork worker 池，与 libuv 事件循环天生冲突
   （addon 里得占死一个 worker 线程，fork 出的 worker 与 V8 堆互不相干）。
   spawn + HTTP 让两侧崩溃域完全隔离，且零原生编译依赖。
2. **cwd 必须是 repo 根**。exec 工具命令 `"python3 examples/tools/wordcount.py"`
   是相对路径，宿主 spawn 时把子进程 cwd 指到 repo 根，工具链路才能通。

四条探针（status/echo/tool/404）全过即退出码 0，可直接当 CI 冒烟用。

## 四、改动清单（文件级 before/after）

| 文件 | 改动 |
|---|---|
| `src/agenthttpd.h` | **新增**：公开 API（config/route/tool/tool_exec/run） |
| `src/core/framework.c` | **新增**（~480 行）：路由表+分发、exec 工具、`agenthttpd_run`（从 main.c 整体搬入的启动序列与 accept 循环） |
| `src/core/main.c` | 383 → ~150 行：只留 argv 解析 + usage |
| `src/http/http_route.c` | `process_request` 方法闸门前加 3 行框架分发钩子 |
| `src/internal.h` | 声明 `framework_route_dispatch` |
| `Makefile` | SRCS/HDRS 加新文件；新增 `lib`（`bin/libagenthttpd.a`，过滤 main.o）、`example`、`example-run`、`node-example` 目标 |
| `examples/embedded.c` | **新增**：~150 行嵌入式示例（3 条自定义路由 + 1 个 exec 工具 + 工具直调探针路由） |
| `examples/tools/wordcount.py` | **新增**：外部工具进程（stdin JSON → stdout JSON） |
| `examples/node-host/host.js` | **新增**：Node 宿主——spawn 嵌入示例 + 就绪轮询 + 4 探针 + SIGTERM 收尾（见 3.5） |

源码里所有 `#include "x.h"` 裸名一行未动——`-I` 搜索路径天然覆盖新文件。

## 五、验证

| 项 | 结果 |
|---|---|
| `make`（macOS clang `-Werror`） | ✅ |
| `make example`（lib + examples/embedded 链接） | ✅ |
| `make node-example`（Node 宿主 4 探针：status/echo/tool/404） | ✅ 全 PASS |
| `make test-unit`（vite/fcgi/auth/minijson 四套 ASan+UBSan） | ✅ ALL PASS |
| `make test-linux`（glibc/GCC `-Werror` 容器构建） | ✅ |
| 镜像重建 + `make test-container` | ✅ 63/63 |
| 标准二进制回归（18099：health/index/CGI） | ✅ 全 200 |

示例运行时探针（18101）：

```
GET  /api/status -> {"ok":true,"server":"embedded-example"}
POST /api/echo   -> {"bytes":21,"text":"hello framework world"}
POST /api/tool   -> {"words": 4, "chars": 18}          ← 真的 fork 出 Python 子进程
POST /api/tool   -> {"error": "invalid JSON arguments: ..."}  ← 工具自己的错误路径
GET  /           -> 200（docroot 静态服务照常）
GET  /test/.smoke-dotfile -> 404（dotfile 拦截照常）
```

`/api/tool` 走的是 `tools_dispatch` 真实分发路径——证明模型侧调用 exec 工具时
走的就是这条已验证的链路。

## 六、踩坑记录

1. **`-Wcomment`**：注释里写 `/api/*` 会命中 `/*` 序列，`-Werror` 直接拒——
   这个项目已第二次踩（上次是 Dockerfile 注释里的 `.json`）。注释里描述通配
   模式要用文字，不画星号斜杠。
2. **并行编辑同一文件会互相覆盖**：对 Makefile 的三处编辑并行提交，后写的
   用旧内容整文件覆盖了先写的，导致 SRCS 丢了 framework.c 却看起来"编辑成功"。
   同文件多次编辑必须串行。
3. **`g_log_fp` 的归属**：搬启动序列时把它的定义也搬进了 framework.c，与
   http.c 的定义重复链接失败——它属于日志写侧（http.c），globals 搬家前先
   grep 定义点。

## 七、已知边界与后续方向

- **API 稳定性承诺从这里开始**：`src/agenthttpd.h` 里的签名现在是对外契约。
  本仓可以继续随意重构内部（`internal.h` 以下仍是私有的），但公开头变更需要
  意识到它会破坏嵌入方。
- **路由/工具表仍是预 fork 快照**：运行期动态注册（SIGHUP 之类）需要把表挪进
  共享内存或引入 IPC，当前刻意不做——复杂度不匹配收益。
- **FastCGI 工具 SDK**：`forward_to_fcgi` 已具备 FastCGI 客户端能力，下一步
  可以让工具通过 UNIX socket 调常驻后端进程（免 fork、可常驻），exec 工具
  保留为"零基础设施"档位。
- **libhttpd / libagent 二分**：当前单一 `libagenthttpd.a`。若真出现"只要
  HTTP 内核不要 Agent 栈"的嵌入方，再按 `core/http/cgi/security` 与
  `agent/` 的既有分层拆成两个库——目录结构已经为此铺好。
- **教学叙事**：这次改造本身就是很好的课程素材——"同一个二进制如何变成
  库"、"预 fork 模型如何决定 API 形状"，建议随路线 C（课程化）展开。
