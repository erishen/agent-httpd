/* Unit tests for src/core/minijson.c: the sbuf string builder and the
 * tolerant JSON reader (jread_string escapes/surrogates, jskip_value
 * nesting, jfind_value top-level lookup). Compiled standalone with the
 * module under ASan/UBSan (see Makefile test-unit). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minijson.h"

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) {                                                     \
            printf("PASS: %s\n", #cond);                                \
        } else {                                                        \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__);            \
            failures++;                                                 \
        }                                                               \
    } while (0)

static void test_sbuf(void) {
    sbuf b = {0};
    sb_str(&b, "he");
    sb_chr(&b, 'l');
    sb_mem(&b, "lo", 2);
    CHECK(strcmp(b.p, "hello") == 0);
    CHECK(b.len == 5);
    free(b.p);

    /* sb_json_str: short escapes, control chars as \u00XX, non-ASCII
     * passes through byte-for-byte. */
    sbuf j = {0};
    sb_json_str(&j, "a\"b\\c\n\t\x01\xe4\xb8\xad");
    CHECK(strcmp(j.p, "\"a\\\"b\\\\c\\n\\t\\u0001\xe4\xb8\xad\"") == 0);
    free(j.p);

    /* Empty string -> just quotes. */
    sbuf e = {0};
    sb_json_str(&e, "");
    CHECK(strcmp(e.p, "\"\"") == 0);
    free(e.p);
}

static void test_jread_string(void) {
    char out[128];
    const char *s;
    const char *p;

    /* Plain pass-through, cursor ends after the closing quote. */
    p = "\"hello\"rest";
    s = jread_string(&p, out, sizeof out);
    CHECK(s != NULL && strcmp(out, "hello") == 0 && strcmp(s, "rest") == 0);

    /* Short escapes incl. \/ and \\; malformed escape -> NULL. */
    p = "\"a\\nb\\t\\\"c\\\\d\\/e\"";
    CHECK(jread_string(&p, out, sizeof out) != NULL);
    CHECK(strcmp(out, "a\nb\t\"c\\d/e") == 0);

    p = "\"a\\x\"";
    CHECK(jread_string(&p, out, sizeof out) == NULL);

    /* Unterminated / empty input. */
    p = "\"abc";
    CHECK(jread_string(&p, out, sizeof out) == NULL);
    p = "";
    CHECK(jread_string(&p, out, sizeof out) == NULL);

    /* \uXXXX: ASCII, BMP CJK, bad hex -> NULL. */
    p = "\"\\u0041\\u4e2d\"";
    CHECK(jread_string(&p, out, sizeof out) != NULL);
    CHECK(strcmp(out, "A\xe4\xb8\xad") == 0);

    p = "\"\\u12g4\"";
    CHECK(jread_string(&p, out, sizeof out) == NULL);

    /* Surrogate pair -> 4-byte UTF-8; lone surrogate -> U+FFFD. */
    p = "\"\\ud83d\\ude00\"";
    CHECK(jread_string(&p, out, sizeof out) != NULL);
    CHECK(strcmp(out, "\xf0\x9f\x98\x80") == 0);

    p = "\"\\ud83d\"";
    CHECK(jread_string(&p, out, sizeof out) != NULL);
    CHECK(strcmp(out, "\xef\xbf\xbd") == 0);

    /* Non-escape UTF-8 survives byte-for-byte (no re-encoding). */
    p = "\"\xe4\xb8\xad\xe6\x96\x87\"";
    CHECK(jread_string(&p, out, sizeof out) != NULL);
    CHECK(strcmp(out, "\xe4\xb8\xad\xe6\x96\x87") == 0);

    /* Truncation always NUL-terminates. */
    p = "\"abcdef\"";
    CHECK(jread_string(&p, out, 3) != NULL);
    CHECK(strcmp(out, "ab") == 0);
}

static void test_jskip_value(void) {
    const char *p;

    p = "  \"str\" ,";
    CHECK(jskip_value(&p) == 0 && strcmp(p, " ,") == 0);

    p = "42,";
    CHECK(jskip_value(&p) == 0 && strcmp(p, ",") == 0);

    p = "true,";
    CHECK(jskip_value(&p) == 0 && strcmp(p, ",") == 0);

    /* Primitives scan to the next delimiter, so a bare "true false" is
     * consumed whole - only separators cut it. */
    p = "true false}";
    CHECK(jskip_value(&p) == 0 && strcmp(p, "}") == 0);

    /* Nested object/array skipped whole, string contents ignored (a
     * brace or quote inside a string must not confuse the depth count). */
    p = "{\"a\":{\"b\":[1,{\"c\":\"}{\"}]},\"e\":2} tail";
    CHECK(jskip_value(&p) == 0 && strcmp(p, " tail") == 0);

    p = "[[\"a]b\"],1]";
    /* The "]" inside the string must not close the inner array: the whole
     * outer value is one skip, cursor lands at the end. */
    CHECK(jskip_value(&p) == 0 && *p == '\0');

    /* Unclosed container -> malformed. */
    p = "{\"a\":1";
    CHECK(jskip_value(&p) == -1);
    p = "[1,2";
    CHECK(jskip_value(&p) == -1);
    p = "";
    CHECK(jskip_value(&p) == -1);

    /* Deliberately non-validating: a missing value before '}' is tolerated
     * (the '}' reads as the closing brace), not an error. */
    p = "{\"a\":}";
    CHECK(jskip_value(&p) == 0 && *p == '\0');
}

static void test_jfind_value(void) {
    char out[64];
    const char *v;

    /* Flat object: returns a pointer at the VALUE start. */
    v = jfind_value("{\"code\":5,\"msg\":\"ok\"}", "msg");
    CHECK(v != NULL && *v == '"');
    CHECK(jread_string(&v, out, sizeof out) != NULL && strcmp(out, "ok") == 0);

    v = jfind_value("{\"code\":5,\"msg\":\"ok\"}", "code");
    CHECK(v != NULL && *v == '5');

    /* Nested objects are skipped whole: only top-level keys match. */
    v = jfind_value("{\"a\":{\"msg\":\"inner\"},\"msg\":\"outer\"}", "msg");
    CHECK(v != NULL);
    CHECK(jread_string(&v, out, sizeof out) != NULL && strcmp(out, "outer") == 0);

    /* Absent key, non-object input, malformed JSON. */
    CHECK(jfind_value("{\"a\":1}", "b") == NULL);
    CHECK(jfind_value("[1,2]", "a") == NULL);
    CHECK(jfind_value("", "a") == NULL);

    /* Tolerant, not validating: for a missing value the pointer comes back
     * parked on '}', but no usable value can be read from it. */
    v = jfind_value("{\"a\":}", "a");
    CHECK(v != NULL && *v == '}');
    CHECK(jread_string(&v, out, sizeof out) == NULL);
}

int main(void) {
    test_sbuf();
    test_jread_string();
    test_jskip_value();
    test_jfind_value();
    printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
