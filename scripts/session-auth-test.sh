#!/bin/sh
# Session-cookie auth smoke test (AUTH_SESSION_FILE).
#
# Covers the login-form flow that Basic Auth alone cannot express: an opaque
# session token issued by POST /login, accepted by the gate in place of a
# Basic credential, and deleted by /logout. Basic Auth keeps working
# unchanged alongside it, and a server started WITHOUT AUTH_SESSION_FILE must
# behave exactly as before the feature existed.
#
# Kept separate from smoke-test.sh for the same reason test-keepalive is:
# this suite spawns its own instances and mutates an htpasswd file at runtime
# (the revocation case), so it needs a private temp dir rather than /tmp
# fixtures shared with the main suite.
set -u

PORT=3111
PLAIN_PORT=3112
TPL_PORT=3113
COOKIE_PORT=3114
BAD_PORT=3115
BASE="http://localhost:${PORT}"
PLAIN_BASE="http://localhost:${PLAIN_PORT}"
TPL_BASE="http://localhost:${TPL_PORT}"
COOKIE_BASE="http://localhost:${COOKIE_PORT}"
BAD_BASE="http://localhost:${BAD_PORT}"
# Absolute: the server runs with its cwd in $TMP so that its default "./www"
# docroot resolves to a private directory (there is no -d flag for docroot).
SERVER="./bin/agent-httpd"
TMP=$(mktemp -d /tmp/agent-httpd-session.XXXXXX)
HTPASSWD="$TMP/htpasswd"
SESSIONS="$TMP/sessions"
REALM_FILE="$TMP/realm"
LOG="$TMP/server.log"

# ---- credential table ----------------------------------------------------
# bcrypt is the only strong form usable on macOS: this libcrypt verifies
# neither $5$ nor $6$, so an SHA-512 htpasswd would be denied forever.
# `htpasswd -B` emits bcrypt, which agent-httpd verifies with its own bundled
# implementation, so it is portable across macOS/Linux. Without the tool we
# fall back to the DES hash smoke-test.sh already carries, behind the
# AGENTHTTPD_ALLOW_WEAK_AUTH escape hatch.
WEAK=0
if command -v htpasswd >/dev/null 2>&1; then
    htpasswd -bcB "$HTPASSWD" alice s3cret >/dev/null 2>&1
    htpasswd -Bb  "$HTPASSWD" bob pw2     >/dev/null 2>&1
    if ! grep -q '^\$2' "$HTPASSWD"; then
        WEAK=1
    fi
fi
if [ ! -s "$HTPASSWD" ] || [ "$WEAK" = 1 ]; then
    WEAK=1
    # cd29FLxV1BmJQ is the DES crypt hash of "s3cret-pw" (see smoke-test.sh).
    printf 'alice:cd29FLxV1BmJQ\nbob:cd29FLxV1BmJQ\n' > "$HTPASSWD"
fi
if [ "$WEAK" = 1 ]; then
    PASSWORD=s3cret-pw
    PASSWORD2=s3cret-pw
else
    PASSWORD=s3cret
    PASSWORD2=pw2
fi

restore() {
    [ -n "${COOKIE_PID:-}" ] && kill "$COOKIE_PID" 2>/dev/null
    [ -n "${BAD_PID:-}" ] && kill "$BAD_PID" 2>/dev/null
    [ -n "${TPL_PID:-}" ] && kill "$TPL_PID" 2>/dev/null
    [ -n "${PLAIN_PID:-}" ] && kill "$PLAIN_PID" 2>/dev/null
    [ -n "${PID:-}" ] && kill "$PID" 2>/dev/null
    pkill -9 -f "agent-httpd -p $PORT " 2>/dev/null
    pkill -9 -f "agent-httpd -p $PLAIN_PORT" 2>/dev/null
    pkill -9 -f "agent-httpd -p $TPL_PORT" 2>/dev/null
    pkill -9 -f "agent-httpd -p $COOKIE_PORT" 2>/dev/null
    pkill -9 -f "agent-httpd -p $BAD_PORT" 2>/dev/null
    rm -rf "$TMP"
}

[ -x "$SERVER" ] || { echo "Run from the repo root (need $SERVER) -- try: make test-session" >&2; exit 1; }
trap restore EXIT INT TERM

pass=0
fail=0
check() {
    desc="$1"; expected="$2"; actual="$3"
    if [ "$actual" = "$expected" ]; then
        echo "PASS: $desc"
        pass=$((pass + 1))
    else
        echo "FAIL: $desc (expected=$expected actual=$actual)"
        fail=$((fail + 1))
    fi
}
status()  { curl -s -o /dev/null --max-time 5 -w "%{http_code}" "$@"; }
headers() { curl -s -D - --max-time 5 -o /dev/null "$@" | tr -d '\r'; }
# POST /login and hand back the minted token, or nothing on failure.
login_tok() {
    headers -d "$1" "$BASE/login" \
        | sed -n 's/^[Ss]et-[Cc]ookie: [^=]*=\([0-9a-f]\{32\}\).*/\1/p' | head -1
}

pkill -9 -f "agent-httpd -p $PORT " 2>/dev/null
pkill -9 -f "agent-httpd -p $PLAIN_PORT" 2>/dev/null
sleep 0.3

AUTH_COMMON="AUTH_SESSION_FILE=$SESSIONS AUTH_REALM_FILE=$REALM_FILE AUTH_SESSION_TTL_DAYS=2"
[ "$WEAK" = 1 ] && AUTH_COMMON="$AUTH_COMMON AGENTHTTPD_ALLOW_WEAK_AUTH=1"

# shellcheck disable=SC2086
env $AUTH_COMMON "$SERVER" -p "$PORT" -a "$HTPASSWD" -r AuthSess -w 2 \
    -L "$TMP/access.log" > "$LOG" 2>&1 &
PID=$!

ready=0
i=0
while [ "$i" -lt 50 ]; do
    curl -s -o /dev/null --max-time 1 "$BASE/" 2>/dev/null && ready=1 && break
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "Server exited prematurely:" >&2
        cat "$LOG" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.1
done
if [ "$ready" -ne 1 ]; then
    echo "Server did not become ready" >&2
    cat "$LOG" >&2
    exit 1
fi

echo "--- 401 points at the login form ---"
h401=$(headers "$BASE/")
check "401 is returned without credentials" "401" "$(status "$BASE/")"
echo "$h401" | grep -qi '^Location: /login?back=' && check "401 offers Location: /login" 1 1 || check "401 offers Location: /login" 1 0
echo "$h401" | grep -qi '^X-Auth-Login: 1' && check "401 offers X-Auth-Login: 1" 1 1 || check "401 offers X-Auth-Login: 1" 1 0
echo "$h401" | grep -qi '^WWW-Authenticate:' && check "401 still carries the Basic challenge" 1 1 || check "401 still carries the Basic challenge" 1 0

echo "--- 401 body: JSON for /api/, HTML elsewhere ---"
# An SPA does fetch().json(); the HTML error page makes it throw
# "Unexpected token '<', \"<!DOCTYPE \"... is not valid JSON" with no way to
# tell the user to log in. Machine routes get a parseable body; browser
# routes keep the human page. The gate runs before routing, so any /api/
# path exercises this without needing a handler behind it.
h401api=$(headers "$BASE/api/stats")
check "/api/ 401 is application/json" "application/json" \
    "$(echo "$h401api" | sed -n 's/^Content-Type: //p' | cut -d';' -f1 | tr -d ' ')"
body401api=$(curl -s --max-time 5 "$BASE/api/stats")
case "$body401api" in
    *unauthorized*) check "/api/ 401 body is parseable JSON" 1 1;;
    *) check "/api/ 401 body is parseable JSON" 1 0;;
esac
case "$body401api" in
    '<'*) check "/api/ 401 body is not HTML" 1 0;;
    *) check "/api/ 401 body is not HTML" 1 1;;
esac
echo "$h401api" | grep -qi '^Location: /login?back=/api/stats' && check "/api/ 401 still offers Location: /login" 1 1 || check "/api/ 401 still offers Location: /login" 1 0
echo "$h401api" | grep -qi '^X-Auth-Login: 1' && check "/api/ 401 still offers X-Auth-Login: 1" 1 1 || check "/api/ 401 still offers X-Auth-Login: 1" 1 0
body401html=$(curl -s --max-time 5 "$BASE/")
case "$body401html" in
    '<!DOCTYPE'*) check "non-/api/ 401 keeps the HTML page" 1 1;;
    *) check "non-/api/ 401 keeps the HTML page" 1 0;;
esac
case "$body401html" in
    *unauthorized*) check "non-/api/ 401 is not JSON" 1 0;;
    *) check "non-/api/ 401 is not JSON" 1 1;;
esac
# There is no /api/ route in this server, so a request that clears the gate
# lands on 404 instead of 401 -- assert the transition, not a specific 2xx.
TOK_API=$(login_tok "username=alice&password=$PASSWORD")
S_API_AUTH=$(status -H "Cookie: lume_session=$TOK_API" "$BASE/api/stats")
[ "$S_API_AUTH" != "401" ] && check "valid cookie clears the /api/ 401 gate" 1 1 \
    || check "valid cookie clears the /api/ 401 gate" 1 0

echo "--- token minting ---"
TOK=$(login_tok "username=alice&password=$PASSWORD")
[ ${#TOK} -eq 32 ] && check "POST /login mints a 32-hex token" 1 1 || check "POST /login mints a 32-hex token" 1 0
check "token persisted to the session file" "1" "$(grep -c "$TOK" "$SESSIONS")"
check "POST /login redirects with 303" "303" "$(status -d "username=alice&password=$PASSWORD" "$BASE/login")"
check "303 defaults to Location: /" "/" \
    "$(headers -d "username=alice&password=$PASSWORD" "$BASE/login" | sed -n 's/^Location: //p')"

echo "--- gate ---"
check "valid cookie is accepted"            "200" "$(status -H "Cookie: lume_session=$TOK" "$BASE/")"
check "bogus token is rejected"             "401" "$(status -H "Cookie: lume_session=deadbeefdeadbeefdeadbeefdeadbeef" "$BASE/")"
check "wrong cookie name is rejected"       "401" "$(status -H "Cookie: lum_session=$TOK" "$BASE/")"
check "unrelated cookies do not block"      "200" "$(status -H "Cookie: junk=1; lume_session=$TOK" "$BASE/")"
check "/health honours the session"         "200" "$(status -H "Cookie: lume_session=$TOK" "$BASE/health")"
check "basic auth still works"              "200" "$(status -u "alice:$PASSWORD" "$BASE/")"
check "basic auth rejects a bad password"   "401" "$(status -u "alice:nope" "$BASE/")"
check "basic auth overrides a bad cookie"   "200" "$(status -u "alice:$PASSWORD" -H "Cookie: lume_session=deadbeef" "$BASE/")"

echo "--- login form ---"
check "GET /login is open"                  "200" "$(status "$BASE/login")"
check "form exposes username field"         "1" "$(curl -s --max-time 5 "$BASE/login" | grep -c 'name="username"')"
check "form uses autocomplete=username"     "1" "$(curl -s --max-time 5 "$BASE/login" | grep -c 'autocomplete="username"')"
check "form uses autocomplete=current-password" "1" "$(curl -s --max-time 5 "$BASE/login" | grep -c 'autocomplete="current-password"')"
check "form is not cacheable"               "1" "$(echo "$(headers "$BASE/login")" | grep -ci 'cache-control: no-store')"
check "wrong password shows an error"       "1" "$(curl -s --max-time 5 -d "username=alice&password=nope" "$BASE/login" | grep -c 'Invalid username or password')"
check "wrong password still returns the form" "1" "$(curl -s --max-time 5 -d "username=alice&password=nope" "$BASE/login" | grep -c 'name="username"')"
check "missing password is refused"         "200" "$(status -d "username=alice" "$BASE/login")"
check "empty POST is refused"               "200" "$(status -X POST "$BASE/login")"
check "unknown user is not distinguishable" "1" \
    "$([ "$(curl -s --max-time 5 -d 'username=nobody&password=x' "$BASE/login" | grep -o 'Invalid username or password')" = "$(curl -s --max-time 5 -d "username=alice&password=nope" "$BASE/login" | grep -o 'Invalid username or password')" ] && echo 1 || echo 0)"

echo "--- ?back= open-redirect guard ---"
for pair in '/:%2F' '/health:%2Fhealth' '/react/chat:%2Freact%2Fchat'; do
    want=${pair%%:*}
    got=$(headers -d "username=alice&password=$PASSWORD" "$BASE/login?back=${pair#*:}" \
        | sed -n 's/^Location: //p')
    check "back=$want is honoured" "$want" "$got"
done
# percent-encode without python/node: the suite must run in a bare container.
urlenc() {
    s=$1
    out=
    i=0
    while [ $i -lt ${#s} ]; do
        c=${s:$i:1}
        case "$c" in
            [a-zA-Z0-9._~]) out="$out$c" ;;
            *)
                printf -v h '%%%02X' "'$c" 2>/dev/null || h="`printf '%%%02X' "'$c"`"
                out="$out$h"
                ;;
        esac
        i=$((i + 1))
    done
    printf '%s' "$out"
}
for bad in '//evil.com' '//evil%2Ecom' '/\evil.com' 'http://evil.com' 'https://evil.com' 'javascript:alert(1)' '../etc/passwd' '%2F%2Fevil.com' ' foo.com'; do
    enc=$(urlenc "$bad")
    got=$(headers -d "username=alice&password=$PASSWORD" "$BASE/login?back=$enc" \
        | sed -n 's/^Location: //p')
    check "back=$bad is rejected" "/" "$got"
done

echo "--- ?back= response splitting and HTML escaping ---"
# back is percent-decoded before we see it, so a %0d%0a in it reaches the header
# serializer as a raw CRLF and can inject a second header of the attacker's
# choosing. The guard must reject any control byte, not just CR and LF.
for c in '%0d%0aX-Injected:%20YES' '%0aX-Injected:%20YES' '%09X-Injected:%20YES' '%00X:%20NUL'; do
    n=$(headers -d "username=alice&password=$PASSWORD" "$BASE/login?back=/$c" \
        | grep -ci '^X-Injected')
    check "no injected header from back=/$c" "0" "$n"
done
# back is also written into a value="" attribute on the form, where a quote
# would break out of it. Verify it comes back entity-escaped instead.
esc=$(curl -s --max-time 5 "$BASE/login?back=/a%22%3E%3Cscript%3Ealert(1)" \
    | grep -c 'value="/a&quot;&gt;&lt;script&gt;alert(1)')
check "back is HTML-escaped in the form" "1" "$esc"
raw=$(curl -s --max-time 5 "$BASE/login?back=/a%22%3E%3Cscript%3Ealert(1)" \
    | grep -c '>value="/a">')
check "back does not break out of the attribute" "0" "$raw"

echo "--- revocation via the htpasswd table ---"
cp "$HTPASSWD" "$TMP/htpasswd.bak"
grep -v '^alice:' "$TMP/htpasswd.bak" > "$HTPASSWD"
sleep 0.5
check "deleted user's session is refused"   "401" "$(status -H "Cookie: lume_session=$TOK" "$BASE/")"
check "deleted user's session is pruned"    "0" "$(grep -c "$TOK" "$SESSIONS")"
mv "$TMP/htpasswd.bak" "$HTPASSWD"
sleep 0.5
check "user restored: login works again"    "303" "$(status -d "username=alice&password=$PASSWORD" "$BASE/login")"

echo "--- expiry ---"
EXP=000000000000000000000000000000a1
echo "$EXP alice $(( $(date +%s) - 120 ))" >> "$SESSIONS"
check "expired token is refused"            "401" "$(status -H "Cookie: lume_session=$EXP" "$BASE/")"
check "expired entry is pruned"             "0" "$(grep -c "$EXP" "$SESSIONS")"

echo "--- logout ---"
TOK=$(login_tok "username=alice&password=$PASSWORD")
check "fresh token minted for logout"       "1" "$(grep -c "$TOK" "$SESSIONS")"
out="$TMP/logout"; : > "$out"
code=$(curl -s --max-time 5 -D "$out" -o /dev/null -w '%{http_code}' \
    -H "Cookie: lume_session=$TOK" "$BASE/logout")
check "GET /logout answers 200"             "200" "$code"
tr -d '\r' < "$out" | grep -qi '^Set-Cookie: lume_session=;' \
    && check "logout clears the cookie" 1 1 || check "logout clears the cookie" 1 0
tr -d '\r' < "$out" | grep -q 'Max-Age=0' \
    && check "cookie cleared with Max-Age=0" 1 1 || check "cookie cleared with Max-Age=0" 1 0
check "logout still returns its JSON body"  "1" "$(curl -s --max-time 5 "$BASE/logout" | grep -c '"ok"')"
check "token removed from the file"         "0" "$(grep -c "$TOK" "$SESSIONS")"
check "stale token is refused"              "401" "$(status -H "Cookie: lume_session=$TOK" "$BASE/")"
check "logout works with no cookie"         "200" "$(status "$BASE/logout")"

# /logout is exempt from the auth gate, so an UNAUTHENTICATED caller can reach
# it. The realm bump is harmless, but the per-user "reject once" record it can
# write is not: anyone who knows a username could otherwise loop
#   curl -H "Authorization: Basic $(echo -n alice:x|base64)" /logout
# and lock that user out for a 401 each time. The username is therefore only
# honoured after the PASSWORD verifies. Uses bob so alice's state above is
# untouched, and both cases are asserted behaviourally (does the next correct
# Basic request get a 401?) rather than by reading the realm file.
b64() { printf '%s' "$1" | base64; }
check "bogus Basic creds on /logout still 200" "200" "$(status -H "Authorization: Basic $(b64 "bob:wrong-pw")" "$BASE/logout")"
check "forged logout left bob usable"         "200" "$(status -u "bob:$PASSWORD2" "$BASE/")"
check "verified Basic creds on /logout still 200" "200" "$(status -H "Authorization: Basic $(b64 "bob:$PASSWORD2")" "$BASE/logout")"
check "real logout arms the one-shot 401"     "401" "$(status -u "bob:$PASSWORD2" "$BASE/")"
check "the 401 is one-shot, bob is back"      "200" "$(status -u "bob:$PASSWORD2" "$BASE/")"

# A Cookie header may legally carry the same name more than once: cookies that
# differ only in Path/Domain arrive as a single header line, and a logout that
# failed to delete the old copy leaves the stale one behind. The gate used to
# answer for whichever occurrence came FIRST, so a leftover shadowed the live
# session and the dashboard rendered as the wrong user (reproduced on the live
# box: test's stale cookie shadowed admin's, so /api/account/info came back
# role=client right after logging in as admin). Every occurrence has to be
# tried, and the first one that actually resolves wins.
echo "--- duplicate session cookies ---"
TOK_BOB="$(login_tok "username=bob&password=$PASSWORD2")"
check "bob session minted"                    "1" "$(grep -c "$TOK_BOB" "$SESSIONS")"
TOK_DEAD="$(login_tok "username=alice&password=$PASSWORD")"
check "alice session minted"                  "1" "$(grep -c "$TOK_DEAD" "$SESSIONS")"
# Make alice's session unresolvable the same way a revoked/logged-out session
# is: the user is gone from htpasswd, so verify() cannot match her line.
cp "$HTPASSWD" "$TMP/htpasswd.dup"
grep -v '^alice:' "$TMP/htpasswd.dup" > "$HTPASSWD"
sleep 0.5
who_of() {
    status -H "Cookie: lume_session=$1; theme=dark" "$BASE/"
}
check "dead token alone is refused"           "401" "$(who_of "$TOK_DEAD")"
check "live token alone still works"          "200" "$(who_of "$TOK_BOB")"
check "dead-first duplicate falls through"    "200" "$(who_of "$TOK_DEAD; lume_session=$TOK_BOB")"
check "live-first duplicate resolves live"     "200" "$(who_of "$TOK_BOB; lume_session=$TOK_DEAD")"
check "three-way duplicate resolves live"     "200" \
    "$(who_of "$TOK_DEAD; theme=x; lume_session=$TOK_BOB")"
check "all copies invalid stays 401"          "401" \
    "$(who_of "0000000000000000000000000000000a; lume_session=0000000000000000000000000000000b")"
mv "$TMP/htpasswd.dup" "$HTPASSWD"
sleep 0.5
# alice's session line was pruned while she was unresolvable, so log her back in
# rather than expecting the old token to survive.
TOK_ALICE2="$(login_tok "username=alice&password=$PASSWORD")"
check "alice restored, fresh login works"      "200" "$(who_of "$TOK_ALICE2")"
check "logout revokes every duplicate token"   "200" "$(status -H "Cookie: lume_session=$TOK_ALICE2; lume_session=$TOK_BOB" -d "" "$BASE/logout")"
check "the second copy is gone too"            "401" "$(who_of "$TOK_BOB")"
check "the first copy is gone too"             "401" "$(who_of "$TOK_ALICE2")"

# POST /login while already carrying a session, and hand back the new token.
login_tok_replacing() {
    headers -H "Cookie: lume_session=$1" -d "$2" "$BASE/login" \
        | sed -n 's/^[Ss]et-[Cc]ookie: [^=]*=\([0-9a-f]\{32\}\).*/\1/p' | head -1
}

echo "--- re-login revokes the session it replaces ---"
# Authenticating again must retire the token the browser was still carrying.
# Newest-wins resolution only *shadows* the old session; without an explicit
# revoke it stays valid on disk until TTL, so anyone still holding it (a stale
# cookie jar, a second tab that never saw the 303, a leaked value) keeps access.
TOK_PREV="$(login_tok "username=alice&password=$PASSWORD")"
check "pre-login token works"                   "200" "$(who_of "$TOK_PREV")"
TOK_NEXT="$(login_tok_replacing "$TOK_PREV" "username=alice&password=$PASSWORD")"
check "re-login returns a different token"      "1" \
    "$([ -n "$TOK_NEXT" ] && [ "$TOK_NEXT" != "$TOK_PREV" ] && echo 1 || echo 0)"
check "the new token works"                     "200" "$(who_of "$TOK_NEXT")"
check "the replaced token is revoked"           "401" "$(who_of "$TOK_PREV")"
check "old token gone from the session file"    "0" \
    "$(grep -c "$TOK_PREV" "$SESSIONS" 2>/dev/null || true)"

kill "$PID" 2>/dev/null
wait "$PID" 2>/dev/null

echo "--- without AUTH_SESSION_FILE (must be unchanged) ---"
env AGENTHTTPD_ALLOW_WEAK_AUTH=1 "$SERVER" -p "$PLAIN_PORT" -a "$HTPASSWD" -r AuthPlain \
    -L "$TMP/access-plain.log" > "$LOG" 2>&1 &
PLAIN_PID=$!
ready=0
i=0
while [ "$i" -lt 50 ]; do
    curl -s -o /dev/null --max-time 1 "$PLAIN_BASE/" 2>/dev/null && ready=1 && break
    i=$((i + 1))
    sleep 0.1
done
check "plain instance up"                   "1" "$ready"
check "still returns 401 without credentials" "401" "$(status "$PLAIN_BASE/")"
# The two session-only headers must be absent: their presence would mean the
# feature leaked into a Basic-only deployment.
h=$(headers "$PLAIN_BASE/")
echo "$h" | grep -qi '^Location:'       && check "no Location header" 0 1 || check "no Location header" 0 0
echo "$h" | grep -qi '^X-Auth-Login:'   && check "no X-Auth-Login header" 0 1 || check "no X-Auth-Login header" 0 0
echo "$h" | grep -qi '^WWW-Authenticate:' && check "Basic challenge intact" 1 1 || check "Basic challenge intact" 1 0
check "/login is gated without sessions"    "401" "$(status "$PLAIN_BASE/login")"
check "POST /login is gated too"            "401" "$(status -d "username=alice&password=$PASSWORD" "$PLAIN_BASE/login")"
check "basic auth unaffected"               "200" "$(status -u "alice:$PASSWORD" "$PLAIN_BASE/")"

kill "$PLAIN_PID" 2>/dev/null
wait "$PLAIN_PID" 2>/dev/null

echo "--- AUTH_LOGIN_PAGE template stays reusable ---"
# A prefork worker serves many requests, so the template must survive its own
# rendering: editing it in place made the first caller's @@BACK@@/@@ERROR@@
# stick forever, leaving every later login pinned to a stale redirect target.
cat > "$TMP/page.html" <<'HTML'
<!doctype html><html><head><title>@@TITLE@@</title></head>
<body><h1>@@REALM@@</h1><p>@@ERROR@@</p>
<input name="back" value="@@BACK@@"">
HTML
env $AUTH_COMMON AUTH_LOGIN_PAGE="$TMP/page.html" \
    "$SERVER" -p "$TPL_PORT" -a "$HTPASSWD" -r 'R<V>&"W' -w 4 \
    -L "$TMP/access-tpl.log" > "$LOG" 2>&1 &
TPL_PID=$!
ready=0
i=0
while [ "$i" -lt 50 ]; do
    curl -s -o /dev/null --max-time 1 "$TPL_BASE/login" 2>/dev/null && ready=1 && break
    i=$((i + 1))
    sleep 0.1
done
check "template instance up"                  "1" "$ready"
if [ "$ready" = 1 ]; then
    bad=0
    for i in 1 2 3 4 5 6 7 8 9 10; do
        want="/w$(( (i * 7) % 5 ))"
        enc=$(urlenc "$want")
        got=$(curl -s --max-time 5 "$TPL_BASE/login?back=$enc" \
            | sed -n 's/.*value="\([^"]*\)".*/\1/p' | head -1)
        [ "$got" = "$want" ] || bad=$((bad + 1))
    done
    check "10 renders each honour their own ?back=" "0" "$bad"
    curl -s -o /dev/null --max-time 5 -d "username=alice&password=nope" "$TPL_BASE/login"
    stuck=$(curl -s --max-time 5 "$TPL_BASE/login" | grep -c 'Invalid username or password')
    check "an error does not persist to the next render" "0" "$stuck"
    # The realm is operator input (-r) and reaches an HTML title and heading.
    # auth_realm_current() appends a "#N" logout-rotation suffix, so match past
    # it rather than the bare realm.
    esc=$(curl -s --max-time 5 "$TPL_BASE/login" \
        | grep -Ec '<title>R&lt;V&gt;&amp;&quot;W(#[0-9]+)?</title>')
    check "realm is HTML-escaped in the title" "1" "$esc"
    raw=$(curl -s --max-time 5 "$TPL_BASE/login" | grep -c '<title>R<V>')
    check "realm is not reflected literally" "0" "$raw"
fi

echo "--- AUTH_SESSION_COOKIE must not alter a header ---"
env $AUTH_COMMON AUTH_SESSION_COOKIE="evil
X-Evil: 1" \
    "$SERVER" -p "$COOKIE_PORT" -a "$HTPASSWD" -r 'RV' -L "$TMP/access-cook.log" \
    > "$TMP/cookie.log" 2>&1 &
COOKIE_PID=$!
ready=0
i=0
while [ "$i" -lt 50 ]; do
    curl -s -o /dev/null --max-time 1 "$COOKIE_BASE/" 2>/dev/null && ready=1 && break
    i=$((i + 1))
    sleep 0.1
done
check "cookie instance up"                    "1" "$ready"
n=$(headers -d "username=alice&password=$PASSWORD" "$COOKIE_BASE/login" \
    | grep -ci '^X-Evil')
check "no header injected from a crafted cookie name" "0" "$n"
grep -q 'AUTH_SESSION_COOKIE: value contains characters' "$TMP/cookie.log" \
    && check "craft rejected with a diagnostic" 1 1 \
    || check "craft rejected with a diagnostic" 1 0
n=$(headers -d "username=alice&password=$PASSWORD" "$COOKIE_BASE/login" \
    | grep -ci '^Set-Cookie: lume_session=')
check "falls back to the default cookie name" "1" "$n"

kill "$TPL_PID" "$COOKIE_PID" 2>/dev/null
# wait() after the kill: a killed background job otherwise gets reported by the
# shell on exit ("Killed: 9 env ..."), which looks like a test failure.
wait "$TPL_PID" 2>/dev/null
wait "$COOKIE_PID" 2>/dev/null

echo "--- an unusable AUTH_SESSION_FILE degrades to off ---"
# With a bad path the server must not serve a login form that can never log
# anyone in (POST would always answer 500). Sessions degrade to off instead, so
# /login is an ordinary gated path.
mkdir -p "$TMP/adir"
# $AUTH_COMMON carries the weak-hash escape hatch too; dropping it here made
# the instance refuse a DES htpasswd and never start.
env $AUTH_COMMON AUTH_SESSION_FILE="$TMP/adir" \
    "$SERVER" -p "$BAD_PORT" -a "$HTPASSWD" -r 'RV' -L "$TMP/access-bad.log" \
    > "$TMP/bad.log" 2>&1 &
BAD_PID=$!
ready=0
i=0
while [ "$i" -lt 50 ]; do
    curl -s -o /dev/null --max-time 1 "$BAD_BASE/" 2>/dev/null && ready=1 && break
    i=$((i + 1))
    sleep 0.1
done
check "bad-store instance up"                 "1" "$ready"
check "/login is gated when the store is unusable" "401" \
    "$(status "$BAD_BASE/login")"
grep -q 'AUTH_SESSION_FILE: .* is not usable' "$TMP/bad.log" \
    && check "bad store reported on stderr" 1 1 \
    || check "bad store reported on stderr" 1 0

kill "$BAD_PID" 2>/dev/null
wait "$BAD_PID" 2>/dev/null

echo "----------------------------------------"
echo "PASS=$pass FAIL=$fail"
[ "$fail" -eq 0 ]
