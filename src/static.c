/* Static content: URL resolution inside the document root (traversal
 * guard), directory listings + trailing-slash redirects, gzip sibling
 * negotiation (RFC 7231 Accept-Encoding), and ETag/304 conditional
 * requests (RFC 7232 If-None-Match). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include "internal.h"

/* 内存分支的安全上限:build_response 把头部(~几百字节)与 body 一起塞进
 * 64KB (MAX_RESPONSE_SIZE) 写缓冲。若 body 顶格 64KB,头部挤爆后被截断,
 * 但 Content-Length 仍报全长,客户端会挂死。超此上限的一律走流式。 */
#define MAX_MEM_BODY_SIZE 60000

/* snprintf 返回「应写入长度」而非实际字节;缓冲将满时直接累加会让 p 越过
 * end,随后 (end - p) 转 size_t 变成巨量而越界写。LIST_APPEND 每次写完后
 * 把 p 钳制在缓冲内,满了则置 end+1,调用方以 p > end 判定截断。 */
#define LIST_APPEND(p, e, ...)                                             \
    do {                                                                   \
        size_t rem_ = (size_t)((e) - (p)) + 1;                             \
        int n_ = snprintf((p), rem_, __VA_ARGS__);                         \
        if (n_ >= 0 && (size_t)n_ < rem_) (p) += n_; else (p) = (e) + 1;   \
    } while (0)

int is_cgi_request(const char *path) {
    if (strncmp(path, "/cgi-bin", 8) != 0) return 0;
    return path[8] == '\0' || path[8] == '/';
}

const char *resolve_within(const char *base_real, const char *decoded_path, char *out, size_t out_size) {
    char full[MAX_PATH_SIZE];
    /* resolve_within receives the URL path only, so strip any query string
     * before joining with the document root (realpath would otherwise fail). */
    char clean_path[MAX_PATH_SIZE];
    snprintf(clean_path, sizeof(clean_path), "%s", decoded_path);
    char *qm = strchr(clean_path, '?');
    if (qm) *qm = '\0';
    snprintf(full, sizeof(full), "%s%s", base_real, clean_path);

    char *rp = realpath(full, NULL);
    if (!rp) return NULL;

    size_t base_len = strlen(base_real);
    if (strncmp(rp, base_real, base_len) != 0) {
        free(rp);
        return NULL;
    }
    if (rp[base_len] != '\0' && rp[base_len] != '/') {
        free(rp);
        return NULL;
    }

    strncpy(out, rp, out_size - 1);
    out[out_size - 1] = '\0';
    free(rp);
    return out;
}

void read_file_into_response(const char *file_path, HttpResponse *response) {
    struct stat st;
    if (stat(file_path, &st) < 0 || !S_ISREG(st.st_mode)) {
        response->body = NULL;
        response->body_length = 0;
        return;
    }
    FILE *file = fopen(file_path, "rb");
    if (!file) {
        response->body = NULL;
        response->body_length = 0;
        return;
    }
    response->body = malloc(st.st_size + 1);
    if (response->body) {
        response->body_length = fread(response->body, 1, st.st_size, file);
        response->body[response->body_length] = '\0';
    }
    fclose(file);
}

/* Strong validator for static content: size + mtime. A byte-changing edit
 * always moves mtime, and same-size rewrites move it too, so pairs collide
 * only across a same-second, same-size, different-content rewrite - which
 * a build system would normally avoid by touching files it rewrites.
 * Weak form (W/"...") because byte-exact equality is not guaranteed across
 * gzip variants: the .gz sibling is a different representation of the same
 * entity and shares the validator. */
void compute_etag(const struct stat *st, char *out, size_t outsz) {
    snprintf(out, outsz, "W/\"%zx-%zx\"", (size_t)st->st_size, (size_t)st->st_mtime);
}

/* RFC 9110 5.6.7: emit Last-Modified as IMF-fixdate from the file mtime.
 * GMT, C locale keeps %a/%b in English. */
void format_http_date(char *out, size_t outsz, time_t t) {
    struct tm tm_gmt;
    gmtime_r(&t, &tm_gmt);
    strftime(out, outsz, "%a, %d %b %Y %H:%M:%S GMT", &tm_gmt);
}

/* RFC 9110 13.2.2 fallback validator: parse an If-Modified-Since header
 * (IMF-fixdate; the obsolete RFC 850 and asctime forms are also accepted
 * leniently via the same day/month/year scan) and compare against the
 * file mtime. Returns 1 when the resource has NOT changed since then. */
int not_modified_since(const char *header, time_t mtime) {
    if (!header || !*header) return 0;
    struct tm tm_v;
    memset(&tm_v, 0, sizeof(tm_v));
    /* IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT" */
    const char *p = strchr(header, ',');
    if (p) p++;
    else p = header;
    int day = 0, year = 0;
    char mon[16] = "";
    if (sscanf(p, " %d %15[a-zA-Z] %d", &day, mon, &year) != 3) return 0;
    static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int mon_idx = -1;
    for (int i = 0; i < 12; i++) {
        if (strncasecmp(mon, months[i], 3) == 0) { mon_idx = i; break; }
    }
    if (mon_idx < 0 || day < 1 || day > 31 || year < 1970) return 0;
    /* Two-digit years from the obsolete RFC 850 form: 00-49 -> 20xx,
     * 50-99 -> 19xx (RFC 9110 5.6.7 interpretation rules). */
    if (year < 100) year += (year < 50) ? 2000 : 1900;
    tm_v.tm_mday = day;
    tm_v.tm_mon = mon_idx;
    tm_v.tm_year = year - 1900;
    time_t vtime = timegm(&tm_v);
    if (vtime == (time_t)-1) return 0;
    /* Not modified when the file has not been changed strictly after the
     * validator time (equal seconds count as unmodified). */
    return mtime <= vtime;
}

/* RFC 7232 If-None-Match: "*" matches any, a list matches when any member
 * equals the validator (W/ prefix ignored - validators compare opaque). */
int etag_matches(const char *header, const char *etag) {
    if (!header || !header[0]) return 0;
    if (strcmp(header, "*") == 0) return 1;
    const char *p = header;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *stop = p;
        while (stop > start && (stop[-1] == ' ' || stop[-1] == '\r')) stop--;
        size_t len = (size_t)(stop - start);
        if (len > 2 && start[0] == 'W' && start[1] == '/') { start += 2; len -= 2; }
        const char *q = etag;
        if (q[0] == 'W' && q[1] == '/') q += 2;
        if (len == strlen(q) && strncmp(start, q, len) == 0) return 1;
    }
    return 0;
}

int handle_directory(const char *real_path, const char *request_path, HttpResponse *response) {
    DIR *dir = opendir(real_path);
    if (!dir) {
        response->status_code = 403;
        strcpy(response->status_text, "Forbidden");
        return -1;
    }

    char listing[MAX_RESPONSE_SIZE];
    char *p = listing;
    char *const end = listing + sizeof(listing) - 1;
    char escaped_name[MAX_PATH_SIZE];
    char entry_path[MAX_PATH_SIZE];

    LIST_APPEND(p, end,
                "<!DOCTYPE html>\n<html>\n<head><title>Index of %s</title></head>\n"
                "<body>\n<h1>Index of %s</h1>\n<hr>\n<table>\n"
                "<tr><th align=\"left\">Name</th><th>Last modified</th><th>Size</th></tr>\n",
                request_path, request_path);
    LIST_APPEND(p, end,
                "<tr><td><a href=\"../\">Parent Directory</a></td><td>-</td><td>-</td></tr>\n");

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (p > end) break; /* 列表缓冲已满:停止继续收集 */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        snprintf(entry_path, sizeof(entry_path), "%s/%s", real_path, entry->d_name);

        struct stat st;
        int is_dir = 0;
        if (stat(entry_path, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
        }

        html_escape(entry->d_name, escaped_name, sizeof(escaped_name));

        char time_str[32] = "-";
        if (!is_dir) {
            struct tm tm_entry;
            localtime_r(&st.st_mtime, &tm_entry);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M", &tm_entry);
        }

        char size_str[32] = "-";
        if (!is_dir) {
            snprintf(size_str, sizeof(size_str), "%lld", (long long)st.st_size);
        }

        LIST_APPEND(p, end,
                    "<tr><td><a href=\"%s%s\">%s</a></td><td>%s</td><td>%s</td></tr>\n",
                    escaped_name, is_dir ? "/" : "", escaped_name, time_str, size_str);
    }
    closedir(dir);

    LIST_APPEND(p, end,
                "</table>\n<hr>\n<p>%s</p>\n</body>\n</html>\n", SERVER_VERSION);

    if (p > end) p = end; /* 截断时也保证缓冲内的终止符 */
    *p = '\0';

    response->status_code = 200;
    strcpy(response->status_text, "OK");
    strcpy(response->content_type, "text/html");
    response->body = strdup(listing);
    response->body_length = (int)(p - listing);
    return 0;
}

/* RFC 9110 14.1.1: parse a single-range "bytes=N-M" / "bytes=N-" /
 * "bytes=-suffix" header. Returns 1 and fills start/len on success
 * (len is clamped to the file size; suffix = last N bytes). Returns 0
 * for anything unsatisfiable, multi-range, or non-byte units - the
 * caller then serves the whole file (or 416 when the header was a
 * malformed byte range). */
int parse_range(const char *header, off_t size, off_t *start, off_t *len) {
    if (!header || strncmp(header, "bytes=", 6) != 0) return 0;
    const char *spec = header + 6;
    if (strchr(spec, ',')) return 0; /* multi-range: serve 200 full */

    /* suffix form: bytes=-N (last N bytes) */
    if (*spec == '-') {
        char *end = NULL;
        long long n = strtoll(spec + 1, &end, 10);
        if (end == spec + 1 || *end != '\0' || n <= 0) return 0;
        if ((off_t)n > size) n = (long long)size;
        *start = size - (off_t)n;
        *len = (off_t)n;
        return *len > 0;
    }

    char *dash = strchr(spec, '-');
    if (!dash) return 0;
    char *end = NULL;
    long long s = strtoll(spec, &end, 10);
    if (end != dash || s < 0) return 0;
    if ((off_t)s >= size) return -1; /* syntactically valid, unsatisfiable */

    long long e = -1;
    if (dash[1] != '\0') {
        char *eend = NULL;
        e = strtoll(dash + 1, &eend, 10);
        if (eend == dash + 1 || *eend != '\0' || e < 0) return 0;
    }
    if (e < 0 || (off_t)e >= size) e = (long long)size - 1;
    if ((off_t)e < (off_t)s) return 0;
    *start = (off_t)s;
    *len = (off_t)(e - s + 1);
    return *len > 0;
}

/* Does one Accept-Encoding token name the given coding? Case-insensitive
 * exact token match, so "x-gzip" no longer matches "gzip". */
static int codings_match(const char *tok, size_t tok_len, const char *coding) {
    size_t cl = strlen(coding);
    if (tok_len != cl) return 0;
    for (size_t i = 0; i < cl; i++) {
        char a = tok[i], b = coding[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

/* RFC 7231 Accept-Encoding: comma-separated "codings [;q=x]" items. gzip is
 * served only when listed with q > 0, or when an explicit "*" with q > 0
 * covers the unlisted coding. "gzip;q=0", unknown tokens and deflate-only
 * lists no longer match. */
static int accepts_gzip(const char *s) {
    if (!s || !*s) return 0;
    int gzip_q = -1;   /* -1 = not mentioned */
    int star_q = -1;   /* explicit "*" only; absent => unlisted codings refused */

    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char *tok = p;
        while (*p && *p != ',' && *p != ';') p++;
        size_t tok_len = (size_t)(p - tok);

        int q = 100; /* default q = 1.0 in hundredths */
        if (*p == ';') {
            p++;
            while (*p == ' ') p++;
            if ((p[0] == 'q' || p[0] == 'Q') && p[1] == '=') {
                p += 2;
                q = atoi(p);
                while (*p && *p != ',') p++;
            }
        }
        while (*p == ' ') p++;
        if (*p == ',') p++;

        if (tok_len == 0) continue;
        if (codings_match(tok, tok_len, "gzip") || codings_match(tok, tok_len, "x-gzip")) {
            gzip_q = q;
        } else if (tok_len == 1 && tok[0] == '*') {
            star_q = q;
        }
    }

    if (gzip_q > 0) return 1;
    if (gzip_q == 0) return 0;            /* explicitly refused */
    return star_q > 0;                    /* explicit wildcard only */
}

int handle_static_file(const HttpRequest *request, HttpResponse *response) {
    char decoded_path[MAX_PATH_SIZE];
    char real_path[MAX_PATH_SIZE];

    /* request->path may still carry a query string (e.g. "/?x=1");
     * resolve_within strips it, but the directory/index logic below needs
     * the bare path, so cut it off here. */
    char path_only[MAX_PATH_SIZE];
    snprintf(path_only, sizeof(path_only), "%s", request->path);
    char *qm = strchr(path_only, '?');
    if (qm) *qm = '\0';

    url_decode(decoded_path, path_only);

    if (strcmp(decoded_path, "/") == 0) {
        strcpy(decoded_path, "/index.html");
    }

    if (strncmp(decoded_path, "/", 1) != 0) {
        decoded_path[0] = '/';
    }

    if (!resolve_within(g_web_root_real, decoded_path, real_path, sizeof(real_path))) {
        response->status_code = 404;
        strcpy(response->status_text, "Not Found");
        return -1;
    }

    struct stat st;
    if (stat(real_path, &st) < 0) {
        response->status_code = 404;
        strcpy(response->status_text, "Not Found");
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        const char *slash = strrchr(path_only, '/');
        if (!slash || slash[1] != '\0') {
            response->status_code = 301;
            strcpy(response->status_text, "Moved Permanently");
            /* %.*s bounds the copy so GCC can prove no truncation; a
             * >510-char request path is pathological, truncation is fine. */
            snprintf(response->location, sizeof(response->location), "%.*s/",
                     (int)sizeof(response->location) - 2, request->path);
            return 0;
        }

        /* +16 headroom for the "/index.html" suffix: GCC can then prove the
         * join always fits (real_path < MAX_PATH_SIZE), silencing
         * -Wformat-truncation on strict GCC 12+ -O2 builds. */
        char index_path[MAX_PATH_SIZE + 16];
        snprintf(index_path, sizeof(index_path), "%s/index.html", real_path);
        if (stat(index_path, &st) == 0 && S_ISREG(st.st_mode)) {
            read_file_into_response(index_path, response);
            if (response->body) {
                response->status_code = 200;
                strcpy(response->status_text, "OK");
                strcpy(response->content_type, "text/html");
                return 0;
            }
        }

        if (g_no_directory_listing) {
            /* -n: refuse to enumerate the filesystem to the browser. */
            response->status_code = 404;
            strcpy(response->status_text, "Not Found");
            response->body = NULL;
            response->body_length = 0;
            return 0;
        }
        return handle_directory(real_path, path_only, response);
    }

    if (!(st.st_mode & S_IRUSR)) {
        response->status_code = 403;
        strcpy(response->status_text, "Forbidden");
        return -1;
    }

    /* Content-Encoding negotiation: if a precompressed "<file>.gz" sits next
     * to the file and the client accepts gzip, serve the compressed version
     * (MIME stays that of the original, so a .js.gz still comes back as JS). */
    /* +4 headroom for the ".gz" suffix (see index_path note above). */
    char gz_path[MAX_PATH_SIZE + 4];
    const char *serve_path = real_path;
    const char *validated = NULL; /* set once a 200 is guaranteed */
    char etag_buf[80];
    if (accepts_gzip(request->accept_encoding)) {
        snprintf(gz_path, sizeof(gz_path), "%s.gz", real_path);
        struct stat gz_st;
        if (stat(gz_path, &gz_st) == 0 && S_ISREG(gz_st.st_mode)) {
            serve_path = gz_path;
            st = gz_st;
            strcpy(response->content_encoding, "gzip");
        }
    }

    /* Conditional request (RFC 7232): when If-None-Match matches the file's
     * validator, skip the body entirely - 304 costs only the header block.
     * The validator reflects the served representation's size (the .gz
     * sibling when gzip is active) with the original's mtime, so a rebuilt
     * file invalidates caches even when its compressed size is unchanged. */
    compute_etag(&st, etag_buf, sizeof(etag_buf));
    /* RFC 9110 13.2.2: when the client sends If-None-Match it is the
     * authoritative validator - If-Modified-Since is then ignored
     * (If-Modified-Since only applies to validators that lack ETags). */
    if (etag_matches(request->if_none_match, etag_buf)) {
        response->status_code = 304;
        strcpy(response->status_text, "Not Modified");
        set_str(response->content_type, sizeof(response->content_type), get_content_type(real_path));
        response->body_length = 0;
        set_str(response->etag, sizeof(response->etag), etag_buf);
        return 0;
    }
    if (request->if_none_match[0] == '\0' &&
        not_modified_since(request->if_modified_since, st.st_mtime)) {
        response->status_code = 304;
        strcpy(response->status_text, "Not Modified");
        set_str(response->content_type, sizeof(response->content_type), get_content_type(real_path));
        response->body_length = 0;
        format_http_date(response->last_modified, sizeof(response->last_modified), st.st_mtime);
        return 0;
    }
    validated = etag_buf;

    /* Large files don't fit MAX_RESPONSE_SIZE: defer the body to a stream
     * (headers carry the real Content-Length; handle_client/fastcgi send the
     * file directly after the header block). */
    /* Range (RFC 9110 14.2): single byte range only; gzip-negotiated
     * responses ignore Range (the range would name the raw entity while
     * the .gz bytes differ), multi-range falls back to 200 full. */
    off_t r_start = 0, r_len = 0;
    int is_partial = 0;
    if (request->range[0] && strcmp(response->content_encoding, "gzip") != 0) {
        int rv = parse_range(request->range, st.st_size, &r_start, &r_len);
        if (rv == -1) {
            response->status_code = 416;
            strcpy(response->status_text, "Range Not Satisfiable");
            snprintf(response->content_range, sizeof(response->content_range),
                     "bytes */%lld", (long long)st.st_size);
            response->body_length = 0;
            return -1;
        }
        if (rv == 1) {
            is_partial = 1;
        }
    }

    if (is_partial || st.st_size > MAX_MEM_BODY_SIZE) {
        response->status_code = is_partial ? 206 : 200;
        strcpy(response->status_text, is_partial ? "Partial Content" : "OK");
        set_str(response->content_type, sizeof(response->content_type), get_content_type(real_path));
        response->body_length = (int)(is_partial ? r_len : st.st_size);
        if (is_partial) {
            snprintf(response->content_range, sizeof(response->content_range),
                     "bytes %lld-%lld/%lld", (long long)r_start,
                     (long long)(r_start + r_len - 1), (long long)st.st_size);
        }
        if (!is_partial) {
            set_str(response->etag, sizeof(response->etag), validated);
            format_http_date(response->last_modified, sizeof(response->last_modified), st.st_mtime);
        }
        response->stream_offset = r_start;
        response->stream_path = strdup(serve_path);
        if (!response->stream_path) {
            set_error_response(response, 500, "Internal Server Error");
        }
        return 0;
    }

    read_file_into_response(serve_path, response);
    if (!response->body) {
        response->status_code = 403;
        strcpy(response->status_text, "Forbidden");
        return -1;
    }

    response->status_code = 200;
    strcpy(response->status_text, "OK");
    set_str(response->content_type, sizeof(response->content_type), get_content_type(real_path));
    set_str(response->etag, sizeof(response->etag), validated);
    format_http_date(response->last_modified, sizeof(response->last_modified), st.st_mtime);
    return 0;
}
