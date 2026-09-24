CC = gcc
# crypt(3) for Basic Auth: glibc >= 2.2 needs -lcrypt (Linux); macOS has it
# in libc. HAVE_CRYPT_H guards the #include (macOS lacks <crypt.h> header).
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    LDFLAGS_EXTRA += -lcrypt
    # libm (pow/fmod in src/agent/tools.c): glibc keeps it out of libc, so the link
    # fails with "undefined reference to pow" without it; macOS folds libm
    # into libSystem and needs no flag.
    LDFLAGS_EXTRA += -lm
    CFLAGS_EXTRA += -DHAVE_CRYPT -DHAVE_CRYPT_H
    # _GNU_SOURCE: strcasestr 等非 POSIX 扩展; 必须在 CFLAGS 里定义
    # (内部头里定义会晚于首个系统头, glibc features.h 已锁死特性集)
    CFLAGS_EXTRA += -D_GNU_SOURCE
    # gcc12 新警告族 (format-truncation/stringop-truncation): 由 glibc 的
    # __builtin 导向产生, macOS clang 不存在这两族 -> 只豁免 Linux。
    # 不豁免则 lume 容器 (debian/gcc12) 编 libagenthttpd.a 在
    # src/core/framework.c:355/359 遇 -Werror 直接红, 和 lume/Makefile
    # 的 -D_GNU_SOURCE 是同一个坑的两半。
    CFLAGS_EXTRA += -Wno-format-truncation -Wno-stringop-truncation
    # glibc _FORTIFY_SOURCE (Ubuntu 默认注入): realpath/getcwd 等带 _chk
    # 检查的 libc 调用会校验调用方缓冲是否 >= PATH_MAX。显式开启让本地
    # Linux 构建与 CI 行为一致, 避免只在 GitHub runner 上才崩。
    CFLAGS_EXTRA += -D_FORTIFY_SOURCE=2
endif
ifeq ($(UNAME_S),Darwin)
    # crypt(3) is in libc; no <crypt.h> header, no extra link flag.
    CFLAGS_EXTRA += -DHAVE_CRYPT
    # _DARWIN_C_SOURCE: 全量 BSD 声明 (sendfile 等)。注意不要定义
    # _POSIX_C_SOURCE —— 它会把 macOS 头收紧到纯 POSIX, 反而藏掉 BSD API。
    CFLAGS_EXTRA += -D_DARWIN_C_SOURCE
endif
# Test-only garbage-collection flags: standalone compilation of a single .c
# (e.g. src/cgi/fastcgi.c for the stream test) discards unused functions that
# reference main.c globals, so no project globals need stubbing. The comma
# inside -Wl,--gc-sections must live inside a variable, not a $(if) argument.
GC_LINUX = -ffunction-sections -fdata-sections -Wl,--gc-sections
GC_DARWIN = -Wl,-dead_strip
GC = $(if $(filter $(UNAME_S),Linux),$(GC_LINUX),$(GC_DARWIN))

# Header search roots: src/ (umbrella headers stay at the root) plus every
# layer subdir, so each #include "x.h" resolves no matter which subdir the
# including .c lives in.
INCDIRS = -I src -I src/core -I src/http -I src/cgi -I src/security -I src/agent
CFLAGS = -Wall -Wextra -Werror -O2 $(INCDIRS) $(CFLAGS_EXTRA)
# libsqlite3 for the native SQL tools (src/agent/sqlite_tool.c): macOS ships
# the dylib in the SDK; Linux needs libsqlite3-dev (container build installs
# it in the lume-build stage). The static container link (-static in
# LDFLAGS_EXTRA) pulls libsqlite3.a so the final image stays dependency-free.
LDFLAGS = $(LDFLAGS_EXTRA) -pthread -lsqlite3
TARGET = bin/agent-httpd
# Object/dependency files live in build/ so src/ holds sources only.
BUILD_DIR = build
# Sources are grouped under src/<layer>/ to keep the flat src/ readable.
# httpd.h / internal.h stay at src/ root as the shared umbrella headers; the
# -I src flag (in CFLAGS) lets every #include "x.h" resolve from any subdir.
SRCS = src/core/main.c src/core/framework.c src/core/event.c src/core/worker.c src/core/router.c \
       src/core/metrics.c src/core/minijson.c src/core/util.c \
       src/http/http.c src/http/http_parse.c src/http/http_resp.c src/http/http_log.c \
       src/http/http_route.c src/http/static.c src/http/chatio.c \
       src/cgi/cgi.c src/cgi/fastcgi.c src/cgi/vite.c \
       src/security/auth.c src/security/ratelimit.c \
       src/agent/llm.c src/agent/agent.c src/agent/mcp.c src/agent/pse.c \
       src/agent/tools.c src/agent/skills.c src/agent/session.c \
       src/agent/sqlite_tool.c
HDRS = src/httpd.h src/agenthttpd.h src/internal.h \
       src/core/minijson.h src/core/metrics.h src/core/router.h \
       src/http/chatio.h \
       src/agent/llm.h src/agent/agent.h src/agent/mcp.h src/agent/pse.h \
       src/agent/tools.h src/agent/skills.h src/agent/session.h \
       src/agent/sqlite_tool.h
OBJS = $(SRCS:src/%.c=$(BUILD_DIR)/%.o)
INSTALL_DIR = /usr/local/bin

PREFIX = /usr/local
CONFIG_DIR = $(PREFIX)/etc/agent-httpd
DATA_DIR = $(PREFIX)/var/www/agent-httpd

PORT = 18080
# FastCGI backend socket; empty disables the FCGI listener
FCGI_SOCK = /tmp/agent-httpd-fcgi.sock
FCGI_ARGS = $(if $(FCGI_SOCK),-F $(FCGI_SOCK))
# Resident React FastCGI backend socket; empty disables /react/ relaying
REACT_SOCK = /tmp/agent-httpd-react.sock
REACT_ARGS = $(if $(REACT_SOCK),-R $(REACT_SOCK))
# Prefork worker count; empty = fork-per-connection
WORKERS = 8
WORKER_ARGS = $(if $(WORKERS),-w $(WORKERS))

.DEFAULT_GOAL := all

all: $(TARGET)

# Framework artifacts: the same objects as the server binary minus main.o,
# archived as a static library; examples/embedded.c links it and registers
# its own routes + tools through the public API (src/agenthttpd.h).
LIB = bin/libagenthttpd.a
LIB_OBJS = $(filter-out $(BUILD_DIR)/core/main.o,$(OBJS))

lib: $(LIB)

$(LIB): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	ar rcs $@ $(LIB_OBJS)
	@echo "Library complete: $@"

examples/embedded: examples/embedded.c $(LIB)
	$(CC) $(CFLAGS) -o $@ examples/embedded.c $(LIB) $(LDFLAGS)

example: $(TARGET) examples/embedded
	@echo "Embedded example built: examples/embedded (run: make example-run)"

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	# LDFLAGS (-lcrypt/-lm) 必须在目标文件之后: GNU ld 默认 --as-needed,
	# 库放在对象前面会被当作无引用而丢弃 (macOS ld 无此限制)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)
	# 容忍 cgi-bin 缺失 (如容器 C 构建阶段只拷贝了 Makefile + src/)
	@chmod +x cgi-bin/*.cgi 2>/dev/null || true
	@echo "Build complete: $@"

# Compile React SSR TypeScript into three bundles (see scripts/build-ssr.sh)
build-ssr:
	@sh scripts/build-ssr.sh

# Type-check the React SSR TypeScript sources (tsc --noEmit; esbuild does not type-check)
typecheck:
	@cd cgi-bin/react-ssr && pnpm run typecheck

# Development mode: HMR dev server for the React app (Vite middleware mode,
# same render.tsx core as production). Edit App.tsx / render.tsx /
# styles/main.css and the browser updates automatically - no esbuild rebuild,
# no server restart. One-time setup: cd cgi-bin/react-ssr && pnpm install.
# Override with DEV_PORT=. Stops a previous dev-server instance first.
DEV_PORT ?= 3100
dev:
	@pkill -f '[d]ev-server.js' 2>/dev/null; sleep 0.3; \
	echo "Starting HMR dev server on port $(DEV_PORT)..."; \
	PORT=$(DEV_PORT) node --env-file-if-exists=.env scripts/dev-server.js

# Compile / copy all language CGI examples into cgi-bin/ (see scripts/build-cgis.sh)
build-cgis:
	@sh scripts/build-cgis.sh

$(BUILD_DIR)/%.o: src/%.c $(HDRS) | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(OBJS:.o=.d)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# Production mode: build and run the C server in the foreground (Ctrl+C stops).
start: all
	@echo "Starting AgentHTTPD on port $(PORT) (workers: $(if $(WORKERS),$(WORKERS),fork-per-conn), FCGI: $(FCGI_SOCK), React: $(REACT_SOCK))..."
	./$(TARGET) -p $(PORT) $(WORKER_ARGS) $(FCGI_ARGS) $(REACT_ARGS)

# Backward-compatible alias for `make start`.
run: start

restart: all
	@$(MAKE) --no-print-directory stop; \
	./$(TARGET) -p $(PORT) $(FCGI_ARGS) $(REACT_ARGS)

# Run the resident React FastCGI backend (foreground), then agent-httpd relays
# browser /react/ requests through -R to it. Ctrl+C stops both.
react-server: all build-ssr
	@echo "Starting resident React SSR backend (HTTP on :$$(($(PORT)+2))) + agent-httpd -v relay..."
	@REACT_HTTP_PORT=$$(( $(PORT) + 2 )) bin/react-ssr-server > /tmp/react-ssr.log 2>&1 & \
	  echo "react pid: $$!"; \
	  sleep 0.5; \
	  ./$(TARGET) -p $(PORT) $(FCGI_ARGS) -v $$(( $(PORT) + 2 ))

react-server-stop:
	@pkill -9 -f 'react-ssr-server' 2>/dev/null; \
	echo "Resident React backend stopped."

stop:
	@if pgrep -f '$(TARGET)' > /dev/null; then \
		pkill -f '$(TARGET)'; \
		echo "Server stopped."; \
	else \
		echo "No running server."; \
	fi

install: all
	@mkdir -p $(DESTDIR)$(INSTALL_DIR) $(DESTDIR)$(CONFIG_DIR) $(DESTDIR)$(DATA_DIR) $(DESTDIR)$(DATA_DIR)/cgi-bin
	cp $(TARGET) $(DESTDIR)$(INSTALL_DIR)/agent-httpd
	cp -r www/* $(DESTDIR)$(DATA_DIR)/
	cp cgi-bin/*.cgi $(DESTDIR)$(DATA_DIR)/cgi-bin/
	chmod +x $(DESTDIR)$(DATA_DIR)/cgi-bin/*.cgi
	echo "installed to $(DESTDIR)$(INSTALL_DIR) and $(DESTDIR)$(DATA_DIR)"

uninstall:
	rm -f $(DESTDIR)$(INSTALL_DIR)/agent-httpd
	rm -rf $(DESTDIR)$(CONFIG_DIR) $(DESTDIR)$(DATA_DIR)
	echo "uninstalled."

test: all
	@echo "Running smoke tests..."
	@sh scripts/smoke-test.sh

# Unit tests for the dev-proxy header builder (ASan+UBSan) and the FastCGI
# stream relay (end-to-end, large responses must not be capped at 64KB).
# Standalone: no live server required, so they run in CI without a backend.
test-unit: all
	@mkdir -p tests
	$(CC) $(CFLAGS) -fsanitize=address,undefined tests/test_vite_header.c src/cgi/vite.c -o tests/t_vite
	./tests/t_vite
	$(CC) -Wall -Wextra -O2 -pthread $(INCDIRS) tests/test_fcgi_stream.c src/cgi/fastcgi.c $(GC) -o tests/t_fcgi
	./tests/t_fcgi
	$(CC) $(CFLAGS) -fsanitize=address,undefined tests/test_auth.c src/security/auth.c -o tests/t_auth
	./tests/t_auth
	$(CC) $(CFLAGS) -fsanitize=address,undefined tests/test_minijson.c src/core/minijson.c -o tests/t_json
	./tests/t_json

# Keep-alive pipelining regression (fix D): start the real server in
# fork-per-connection mode, pipeline two GETs on one connection, assert both
# are served. The spawned server uses REQUEST_TIMEOUT_SECONDS=2 so the
# broken path fails fast instead of hanging for the default 60s. This is a
# slower integration test (the server spawns resident MCP subprocesses at
# startup), so it is kept separate from the fast `test-unit` target.
test-keepalive: all
	@mkdir -p tests
	$(CC) $(CFLAGS) -I src tests/test_keepalive_pipeline.c -o tests/t_ka
	./tests/t_ka

# Guard the Linux/GCC build without leaving macOS. glibc is the only place the
# -Wstringop-truncation / -Wformat-truncation / -Wuse-after-free family shows
# up (Apple clang stays silent on all three), and glibc is also the only place
# where -lm and -lcrypt are not free. Builds just the Dockerfile's c-build
# stage, so it needs Docker but no daemon-side state.
test-linux:
	@echo "Checking the Linux/GCC build (Dockerfile stage c-build)..."
	DOCKER_BUILDKIT=0 docker build --target c-build -t agent-httpd-c-build-check .

# 上游不可达时, 聊天必须把失败告诉客户端, 而不是回一个「成功但空」的答复。
# 自带 --network none 的一次性容器 + 故意打不通的 LLM_API_URL, 不需要 compose 栈。
test-upstream:
	@echo "Checking the upstream-failure path (must surface an error, not a silent empty answer)..."
	sh scripts/upstream-failure-check.sh

# 大响应必须完整送达 —— 而且要在「直连」下测: 事件循环用 O_NONBLOCK accept
# 后把同一个 socket 交给 worker 池, worker 里发满发送缓冲区时曾把 EAGAIN 当
# 致命错误, 于是客户端拿到被截断的响应 (nginx: upstream prematurely closed
# connection; 浏览器: /js/react-ssr.js 解析失败 -> 页面静默不水合, 两个入口
# 看起来不一样)。其余探针全部经发布端口 (docker-proxy), 用户态转发会持续
# 排空缓冲区, 所以永远看不到。本目标用私有网络里的一次性容器做真直连。
test-stream:
	@echo "Checking that a large body survives a direct container-to-container connection..."
	sh scripts/stream-truncation-check.sh

# Probe the running container over the wire: the shipped binary, docroot and
# CGI zoo, not the repo tree. scripts/smoke-test.sh spawns its own server, so
# nothing else in the suite reaches the image. Requires `docker compose up -d`
# first; the probe copies itself into the container because its CGI cases write
# temporary scripts into /app/cgi-bin.
test-container:
	@docker inspect -f '{{.State.Running}}' agent-httpd 2>/dev/null | grep -q true \
		|| { echo "agent-httpd is not running — start it with: docker compose up -d"; exit 1; }
	@echo "Probing the running container on port 8080..."
	docker cp scripts/container-smoke.py agent-httpd:/tmp/container-smoke.py
	docker exec -i agent-httpd python3 /tmp/container-smoke.py

# Throughput benchmark: keep-alive vs connection-per-request.
# Override with PORT=xxx BENCH_REQ=5000 BENCH_CONC=16.
bench: all
	@echo "Benchmarking on port $(PORT)..."
	@pkill -f '$(TARGET)' > /dev/null 2>&1; sleep 0.3; \
		./$(TARGET) -p $(PORT) > /tmp/agent-httpd-bench.log 2>&1 & \
		trap 'pkill -f "$(TARGET) -p $(PORT)" 2>/dev/null' EXIT INT TERM; \
		for i in $$(seq 1 60); do \
			curl -s -o /dev/null --max-time 1 http://127.0.0.1:$(PORT)/ 2>/dev/null && break; \
			sleep 0.5; \
		done; \
		python3 scripts/bench.py $(PORT) $(BENCH_REQ) $(BENCH_CONC)

# Run the embedded example (examples/embedded.c) on :18101 in the foreground.
example-run: example
	./examples/embedded

# Node host drives the embedded example over HTTP (lifecycle + probes).
# Requires the managed/system node on PATH; see examples/node-host/host.js.
node-example: example
	node examples/node-host/host.js

clean:
	rm -rf $(BUILD_DIR) bin examples/embedded

.PHONY: all lib example example-run node-example build-ssr typecheck build-cgis start run dev restart stop install uninstall test bench clean react-server react-server-stop test-unit test-keepalive test-linux test-container test-upstream test-stream