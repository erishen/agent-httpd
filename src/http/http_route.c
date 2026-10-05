/* Route classification + dispatch: decide, for a parsed request, whether it
 * can run on the fast in-process path (is_fast_request), whether it is a
 * dev-mode Vite proxy target (is_vite_proxy_route), whether its header block
 * is a WebSocket upgrade (is_websocket_upgrade), and how a non-streaming
 * request maps to static file / CGI / health / metrics (process_request).
 * The keep-alive connection loop that drives these lives in src/http/http.c. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "internal.h"
#include "httpd.h"
#include "llm.h"
#include "metrics.h"

int is_fast_request(const HttpRequest *request) {
    /* OPTIONS is answered inline by process_request with an Allow menu (no
     * CGI spawn, no disk beyond a stat on the path) — CORS preflights are
     * frequent enough to deserve the loop. Body-carrying OPTIONS still
     * falls through to the worker pool via the content_length check. */
    if (strcmp(request->method, "GET") != 0 && strcmp(request->method, "HEAD") != 0 &&
        strcmp(request->method, "OPTIONS") != 0) {
        return 0;
    }
    if (request->content_length > 0) return 0; /* body needs the blocking pipeline */
    if (llm_is_chat_route(request->path, request->method)) return 0;
    if (strncmp(request->path, "/react", 6) == 0 &&
        (request->path[6] == '\0' || request->path[6] == '/')) {
        return 0;
    }
    if (is_cgi_request(request->path)) return 0;
    if (g_vite_upstream_port > 0 && is_vite_proxy_route(request)) return 0;
    return 1;
}

int is_vite_proxy_route(const HttpRequest *request) {
    if (g_vite_upstream_port <= 0) return 0;
    if (request->content_length > 0) return 0;
    const char *p = request->path;
    if (strncmp(p, "/@", 2) == 0) return 1; /* /@vite, /@fs, /@id, /@react-refresh */
    if (strcmp(p, "/react/react-ssr.tsx") == 0) return 1;
    if (strncmp(p, "/react", 6) == 0 &&
        (p[6] == '\0' || p[6] == '/') &&
        strcmp(p, "/react/api/chat") != 0) {
        return 1; /* dev SSR pages */
    }
    if (strncmp(p, "/src/", 5) == 0) return 1;
    return 0;
}

int is_websocket_upgrade(const char *raw, size_t len) {
    if (len < 16) return 0;
    if (strncasecmp(raw, "GET", 3) != 0) return 0;
    const char *conn = raw, *end = raw + len;
    /* scan header lines for Connection: ...upgrade... and Upgrade: websocket */
    int has_upgrade = 0, has_connection_upgrade = 0;
    while (conn < end) {
        const char *nl = memchr(conn, '\n', (size_t)(end - conn));
        size_t ll = nl ? (size_t)(nl - conn + 1) : (size_t)(end - conn);
        if (ll > 2 && strncasecmp(conn, "Upgrade:", 8) == 0 &&
            strstr(conn, "websocket")) {
            has_upgrade = 1;
        } else if (ll > 2 && strncasecmp(conn, "Connection:", 11) == 0 &&
                   (strstr(conn, "upgrade") || strstr(conn, "Upgrade"))) {
            has_connection_upgrade = 1;
        }
        if (conn + ll >= end) break;
        conn += ll;
    }
    return has_upgrade && has_connection_upgrade;
}

/* ---- /login (session-cookie login form) ----
 *
 * AUTH_LOGIN_PAGE points at an HTML template with these placeholders:
 *   @@TITLE@@  document title            @@BACK@@   where to go on success
 *   @@REALM@@  visible heading           @@ERROR@@  message for a bad attempt
 * Missing file or a too-large file degrades to the built-in form below, so the
 * feature never hard-depends on a shipped asset.
 *
 * Two buffers, on purpose: g_login_template must stay pristine so @@BACK@@ and
 * @@ERROR@@ can be re-substituted on the next request. Editing it in place made
 * the first request's values stick forever -- every later render saw an already
 * consumed template and echoed whatever the first caller had passed (visible as
 * a login form stuck on a stale redirect target in prefork mode).
 */
#define LOGIN_PAGE_MAX 65536

static char g_login_template[LOGIN_PAGE_MAX] = "";
static char g_login_render[LOGIN_PAGE_MAX] = "";
static int g_login_template_loaded = 0;

static void load_login_template(void) {
    const char *e;
    if (g_login_template_loaded) return;
    g_login_template_loaded = 1;
    e = getenv("AUTH_LOGIN_PAGE");
    if (!e || !e[0]) return;
    FILE *f = fopen(e, "rb");
    if (!f) {
        fprintf(stderr, "AUTH_LOGIN_PAGE: cannot open %s, using built-in form\n", e);
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (long)ftell(f) < 0) {
        fclose(f);
        return;
    }
    long n = ftell(f);
    rewind(f);
    if ((size_t)n >= sizeof(g_login_template)) {
        fprintf(stderr, "AUTH_LOGIN_PAGE: %s is %ld bytes, refusing (max %zu)\n",
                e, n, sizeof(g_login_template) - 1);
    } else {
        size_t got = fread(g_login_template, 1, (size_t)n, f);
        g_login_template[got] = '\0';
    }
    fclose(f);
}

/* Replace every occurrence of key with value, packing left. A substitution
 * that would overflow buf_size aborts the whole substitution (the string is
 * already mutated, but this only ever truncates toward the buffer end). */
static void subst(char *s, size_t buf_size, const char *key, const char *val) {
    char *p = s;
    while ((p = strstr(p, key)) != NULL) {
        size_t kl = strlen(key), vl = strlen(val);
        const char *tail = p + kl;
        if (p < s + buf_size - vl && strlen(tail) + vl + 1 <= buf_size - (size_t)(p - s)) {
            memmove(p + vl, tail, strlen(tail) + 1);
            memcpy(p, val, vl);
            p += vl;
        } else {
            *p = '\0';
            return;
        }
    }
}

/* Copy n bytes of src into dst at offset off. The offset is explicit --
 * conflating it with the copy length is what produced an empty body at
 * offset 0 and a body starting 1.8 KB in. */
static int put_str(char *dst, size_t cap, size_t off, size_t n, const char *src) {
    if (off + n + 1 > cap) return 0;
    memcpy(dst + off, src, n);
    dst[off + n] = '\0';
    return 1;
}

/* Deliberately zero-dependency: no framework, no fonts, no JS. The two
 * autocomplete hints are the point of this whole feature -- they are what let a
 * password manager fill the fields in. */
static const char *builtin_login_page(void) {
    static const char page[] =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>@@TITLE@@</title>"
        "<style>body{font:16px/1.5 system-ui,-apple-system,sans-serif;"
        "display:grid;place-items:center;min-height:100vh;margin:0;"
        "background:#f6f7f9;color:#111827}.box{background:#fff;border:"
        "1px solid #e5e7eb;border-radius:12px;padding:28px;width:min(340px,90vw);}"
        "h1{font-size:17px;margin:0 0 2px}p{color:#6b7280;font-size:13px;"
        "margin:0 0 18px}label{display:block;font-size:13px;color:#374151;"
        "margin-bottom:4px}input{width:100%;box-sizing:border-box;padding:9px 10px;"
        "border:1px solid #d1d5db;border-radius:8px;font:inherit;margin-bottom:14px;}"
        "button{width:100%;padding:10px;border:0;border-radius:8px;background:#111827;"
        "color:#fff;font:inherit;font-weight:600;cursor:pointer}.err{background:#fef2f2;"
        "color:#b91c1c;border:1px solid #fecaca;border-radius:8px;padding:8px 10px;"
        "font-size:13px;margin-bottom:14px}"
        "</style></head><body><div class=\"box\"><h1>@@REALM@@</h1>"
        "<p>Sign in to continue.</p>"
        "<div>@@ERROR@@</div>"
        "<form method=\"POST\" action=\"/login\">"
        "<input type=\"hidden\" name=\"back\" value=\"@@BACK@@\">"
        "<label for=\"u\">Username</label>"
        "<input id=\"u\" name=\"username\" autocomplete=\"username\" autofocus required>"
        "<label for=\"p\">Password</label>"
        "<input id=\"p\" name=\"password\" type=\"password\""
        " autocomplete=\"current-password\" required>"
        "<button type=\"submit\">Sign in</button></form></div></body></html>";
    return page;
}

/* back is echoed into the Location: header, so it is treated as an
 * open-redirect vector: only a same-origin absolute path survives.
 * Rejecting "//host" (protocol-relative) and backslashes covers the usual
 * bypass tricks.
 *
 * No control byte at all: form_field() percent-decodes back before we see it,
 * so ?back=/%0d%0aX-Injected%3a+yes reached the header serializer as a raw
 * CRLF and produced a second response header of the attacker's choosing
 * (HTTP response splitting). Rejecting every byte below 0x20 plus DEL makes
 * the rule one simple loop instead of a list of individually dangerous chars. */
static int is_safe_back(const char *s) {
    if (s[0] != '/' || s[1] == '/' || strchr(s, '\\') != NULL) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) return 0;
    }
    return 1;
}

static void render_login_page(HttpResponse *response, const char *error, const char *back) {
    load_login_template();
    size_t cap = sizeof(g_login_render);
    /* Render into the scratch buffer; g_login_template stays pristine (see the
     * note above). @@BACK@@ is escaped for HTML because it is written into an
     * attribute, not because of the header (is_safe_back covers that). */
    const char *src = g_login_template[0] ? g_login_template : builtin_login_page();
    put_str(g_login_render, cap, 0, strlen(src), src);
    char *tmpl = g_login_render;
    /* Every substitution target is echoed into HTML, so every value is
     * escaped -- including the realm, which comes from -r and from
     * auth_realm_current()'s "<realm>#N" rotation suffix. One scratch buffer
     * suffices: subst() copies the value in, so it is safe to reuse. */
    char esc[LOGIN_PAGE_MAX / 4];
    html_escape(auth_realm_current(), esc, sizeof(esc));
    subst(tmpl, cap, "@@TITLE@@", esc);
    subst(tmpl, cap, "@@REALM@@", esc);
    html_escape(back, esc, sizeof(esc));
    subst(tmpl, cap, "@@BACK@@", esc);
    html_escape(error, esc, sizeof(esc));
    subst(tmpl, cap, "@@ERROR@@", esc);
    if (strlen(tmpl) + 1 > cap) tmpl[cap - 1] = '\0';
    response->status_code = 200;
    strcpy(response->status_text, "OK");
    strcpy(response->content_type, "text/html; charset=utf-8");
    response->body = strdup(tmpl);
    response->body_length = response->body ? (long)strlen(response->body) : 0;
    snprintf(response->cache_control, sizeof(response->cache_control),
             "no-store");
}

/* One named field out of an application/x-www-form-urlencoded body. '+' is a
 * space and %XX is decoded; anything else passes through. Last value wins for
 * repeated keys, which is the urlencoded convention. Not a general parser: no
 * nested structures and no array syntax. */
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int form_field(const char *body, long len, const char *name,
                      char *out, size_t out_size) {
    if (!body || len <= 0) return 0;
    size_t nlen = strlen(name);
    const unsigned char *p = (const unsigned char *)body;
    const unsigned char *end = p + (size_t)len;
    while (p < end) {
        const unsigned char *amp = memchr(p, '&', (size_t)(end - p));
        const unsigned char *seg = amp ? amp : end;
        const unsigned char *eq = memchr(p, '=', (size_t)(seg - p));
        if (eq && (size_t)(eq - p) == nlen && strncmp((const char *)p, name, nlen) == 0) {
            const unsigned char *v = eq + 1;
            size_t oi = 0;
            while (v < seg && oi + 1 < out_size) {
                unsigned char c;
                if (*v == '+') {
                    c = ' ';
                } else if (*v == '%' && (size_t)(seg - v) >= 3) {
                    int a = hexval((char)v[1]), b = hexval((char)v[2]);
                    /* Invalid hex is kept literally rather than decoded to 0x00,
                     * which would otherwise inject a NUL into the field. */
                    c = (a >= 0 && b >= 0) ? (unsigned char)(a * 16 + b) : '%';
                    v += 2;
                } else {
                    c = *v;
                }
                out[oi++] = (char)c;
                v++;
            }
            out[oi] = '\0';
            return 1;
        }
        p = seg < end ? seg + 1 : end;
    }
    return 0;
}

static void handle_login(HttpRequest *request, HttpResponse *response) {
    /* The query string is never split off the path (parse_headers reads the
     * whole target with one %s), so carve it here. */
    const char *qs = strchr(request->path, '?');
    qs = qs ? qs + 1 : "";
    /* ?back= is echoed into the Location: header, so it is treated as an
     * open-redirect vector: only a same-origin absolute path survives.
     * Rejecting "//host" (protocol-relative) and backslashes covers the usual
     * bypass tricks.
     *
     * back_buf lives for the whole function on purpose. Declaring it inside
     * the `if (qs[0])` block left `back` dangling past that block, and the
     * compiler reused the slot for `pass` below -- so the Location header
     * printed the password. */
    char back_buf[256] = "/";
    const char *back = back_buf;
    if (qs[0] &&
        form_field(qs, (long)strlen(qs), "back", back_buf, sizeof(back_buf)) &&
        is_safe_back(back_buf)) {
        back = back_buf;
    } else if (qs[0]) {
        snprintf(back_buf, sizeof(back_buf), "%s", "/");
    }
    if (strcmp(request->method, "GET") != 0 && strcmp(request->method, "HEAD") != 0) {
        char user[128] = "", pass[256] = "";
        long bl = request->content_length > 0 ? (long)request->content_length : 0;
        if (!form_field(request->body, bl, "username", user, sizeof(user)) ||
            !form_field(request->body, bl, "password", pass, sizeof(pass))) {
            render_login_page(response, "Missing username or password.", back);
            return;
        }
        if (!auth_check_credentials(user, pass)) {
            /* 200 rather than 401: a 401 without WWW-Authenticate is not a
             * useful response here, and returning the form keeps the retry
             * loop visible instead of bouncing through the auth gate. The
             * message is deliberately generic -- no "user not found" -- so a
             * probe cannot enumerate usernames. */
            render_login_page(response, "Invalid username or password.", back);
            return;
        }
        char token[64];
        if (!auth_session_create(user, token, sizeof(token))) {
            set_error_response(response, 500, "Internal Server Error");
            return;
        }
        /* Replace, do not accumulate: retire every session this request was
         * still carrying. The gate's newest-wins resolution only *shadows* an
         * older token -- it stays valid on disk until TTL, so a stale cookie
         * jar or a tab that never saw the 303 would keep authenticating with
         * it. Revoking AFTER the create means a failure here cannot cost the
         * user the session they just earned, and the fresh token is not in
         * the request cookie yet, so it cannot delete itself. */
        auth_session_delete(request->cookie);
        auth_session_cookie_line(token, response->set_cookie,
                                 sizeof(response->set_cookie));
        /* 303 (See Other) rather than 302: POST semantics do not carry over,
         * so a refresh of the resulting page re-GETs instead of re-submitting
         * the password. */
        response->status_code = 303;
        strcpy(response->status_text, "See Other");
        snprintf(response->location, sizeof(response->location), "%s", back);
        return;
    }
    render_login_page(response, "", back);
}

int process_request(HttpRequest *request, HttpResponse *response, int client_fd) {
    /* Liveness probe for load balancers / uptime checks: fixed tiny 200.
     * Sits behind the rate-limit and auth gates in handle_client, so an
     * authed server requires credentials here too (documented). */
    if (strncmp(request->path, "/health", 7) == 0 &&
        (request->path[7] == '\0' || request->path[7] == '?')) {
        response->status_code = 200;
        strcpy(response->status_text, "OK");
        strcpy(response->content_type, "text/plain");
        response->body = strdup("ok\n");
        response->body_length = response->body ? 3 : 0;
        return 0;
    }

    /* Logout / switch-user endpoint: bump the Basic-Auth realm rotation
     * counter (AUTH_REALM_FILE). Exempt from the 401 gate (see
     * is_logout_path in http.c / event.c). It carries no data — but the
     * realm bump and the per-user reject window it may write ARE observable
     * side effects, so they are not unconditionally safe to trigger from an
     * unauthenticated request. When AUTH_REALM_FILE is unset (local dev) the
     * bump no-ops and the response says so. */
    if (is_logout_path(request->path)) {
        /* 解码当前请求附带的凭据（浏览器静默重发缓存凭据时非空）。用户名只有在
         * 密码验过之后才用来写「一次性拒绝」记录：否则任何人都能拿
         * `curl -H 'Authorization: Basic base64(admin:x)' /logout` 把某个用户
         * 反复打进 30s 拒绝窗口，并每次轮换 realm 让浏览器缓存的 Basic 凭据桶
         * 失效、被迫重新弹框——一个知道用户名的未认证 DoS。
         * 密码对不上（伪造头 / 手写头）就退化成只 bump realm，不写用户记录。
         * 用 auth_check_credentials 而不是 check_basic_auth：后者内部会
         * auth_logout_consume()，会把这里刚写的拒绝记录吃掉当自用。 */
        char cu[128] = "";
        int user_verified = 0;
        if (request->authorization[0] &&
            strncasecmp(request->authorization, "Basic ", 6) == 0) {
            char creds[256 + 128 + 2];
            creds[0] = '\0';
            b64_decode(request->authorization + 6, creds, sizeof(creds));
            char *colon = strchr(creds, ':');
            if (colon) {
                *colon = '\0';
                if (creds[0] && colon[1] &&
                    auth_check_credentials(creds, colon + 1)) {
                    snprintf(cu, sizeof(cu), "%s", creds);
                    user_verified = 1;
                }
            }
        }
        int n = auth_logout_realm_bump(user_verified ? cu : "");
        /* Session half of logout: without this a valid cookie would keep the
         * gate green for its whole remaining TTL, so the 401 the browser is
         * about to get would never arrive and it would keep using the
         * previous account's session. */
        if (auth_session_delete(request->cookie)) {
            auth_session_cookie_line("", response->set_cookie,
                                     sizeof(response->set_cookie));
        }
        char out[192];
        int len;
        if (n > 0)
            len = snprintf(out, sizeof(out),
                           "{\"ok\":true,\"realm\":%d,\"user\":\"%s\"}\n", n,
                           user_verified ? cu : "");
        else
            len = snprintf(out, sizeof(out),
                           "{\"ok\":false,\"detail\":\"AUTH_REALM_FILE unset or unwritable\"}\n");
        response->status_code = 200;
        strcpy(response->status_text, "OK");
        strcpy(response->content_type, "application/json");
        response->body = strdup(out);
        response->body_length = len;
        return 0;
    }

    /* /login: the session-cookie login form. GET renders the form (it is the
     * Location: target of the 401 gate when nothing is cached), POST verifies
     * against the htpasswd table and mints a session token. Both are exempt
     * from the 401 gate (is_login_path) because there is no other way in.
     *
     * The form is real HTML rather than a 401 dialog on purpose: a native
     * auth prompt is not a form, so no password manager can store its
     * contents — that is what forces the re-type on every browser session.
     * SameSite=Lax on the issued cookie also makes cross-site POSTs to this
     * endpoint (login CSRF) inert. */
    if (is_login_path(request->path)) {
        handle_login(request, response);
        return 0;
    }

    /* Operations endpoint: Prometheus text format snapshot of the shared
     * counter table (see src/core/metrics.c). Same gate position as /health —
     * behind rate-limit/auth, ride-alive on the fast path. */
    if (strncmp(request->path, "/metrics", 8) == 0 &&
        (request->path[8] == '\0' || request->path[8] == '?')) {
        char *buf = malloc(8192);
        response->status_code = 200;
        strcpy(response->status_text, "OK");
        strcpy(response->content_type,
                "text/plain; version=0.0.4; charset=utf-8");
        if (buf) {
            response->body_length = metrics_render(buf, 8192);
            response->body = buf;
        } else {
            response->body = NULL;
            response->body_length = 0;
        }
        return 0;
    }

    /* Framework layer: routes registered through agenthttpd_route() run
     * before the built-in method gate, so a custom route may serve any verb
     * (POST included, without needing the CGI path form). A handler returns
     * 0 = handled; -1 = fall through to the dispatch below. */
    if (framework_route_dispatch(request, response)) return 0;

    /* Method dispatch (RFC 9110 9): GET/HEAD read resources, POST/PUT/PATCH
     * carry bodies to CGI, DELETE asks a script to remove something, and
     * OPTIONS is answered by the server itself with an Allow menu. Static
     * files are read-only: writer methods only make sense on CGI paths —
     * a PUT to a static URL gets 405 + Allow (never written to disk). */
    static const char *const READ_METHODS = "GET, HEAD, OPTIONS";
    static const char *const ALL_METHODS =
        "GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS";
    if (strcmp(request->method, "GET") == 0 || strcmp(request->method, "HEAD") == 0) {
        /* read methods flow to the dispatch below */
    } else if (strcmp(request->method, "POST") == 0 ||
               strcmp(request->method, "PUT") == 0 ||
               strcmp(request->method, "PATCH") == 0 ||
               strcmp(request->method, "DELETE") == 0) {
        if (!is_cgi_request(request->path)) {
            set_error_response(response, 405, "Method Not Allowed");
            snprintf(response->allow, sizeof response->allow, "%s", READ_METHODS);
            return -1;
        }
    } else if (strcmp(request->method, "OPTIONS") == 0) {
        /* Serve the capability menu inline: scripts stay in charge of the
         * verbs themselves, but the server can truthfully advertise them
         * without executing anything. 200 (not 204): the serializer always
         * emits Content-Length, which RFC 9110 8.6 forbids on 204. */
        response->status_code = 200;
        strcpy(response->status_text, "OK");
        response->body = strdup("ok\n");
        response->body_length = response->body ? 3 : 0;
        snprintf(response->allow, sizeof response->allow, "%s",
                 is_cgi_request(request->path) ? ALL_METHODS : READ_METHODS);
        return 0;
    } else {
        set_error_response(response, 501, "Not Implemented");
        return -1;
    }

    if (is_cgi_request(request->path)) {
        if (request->path[8] == '\0') {
            response->status_code = 301;
            strcpy(response->status_text, "Moved Permanently");
            strcpy(response->location, "/cgi-bin/");
        } else if (request->path[8] == '/' && request->path[9] == '\0') {
            handle_directory(g_cgi_bin_real, "/cgi-bin/", response);
        } else {
            execute_cgi(request, response, client_fd);
        }
    } else {
        /* Static files are read-only; writer methods were 405'd above, so
         * only GET/HEAD reach this branch. A configured "views" directory is
         * tried first (page shells), then the document root (shared bundles
         * and everything else). handle_views_file: 0 served / -1 refused /
         * 1 absent. */
        int v = handle_views_file(request, response);
        if (v != 1) return 0;
        handle_static_file(request, response);
    }
    return 0;
}
