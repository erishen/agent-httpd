#!/bin/sh
# Build React SSR (TypeScript + Tailwind) into three bundles:
#   server.tsx      -> cgi-bin/react-ssr.cgi      (CGI, one process per request)
#   server/main.tsx -> bin/react-ssr-server       (resident FastCGI backend)
#   server/cgi.tsx  -> cgi-bin/react-ssr.cgi      (per-request CGI entry)
#   client.tsx      -> www/js/react-ssr.js        (browser hydration)
# All share App.tsx so the hydrated first render equals the server markup.
# A tsc --noEmit pass runs first: esbuild transpiles but never type-checks.
# Build order matters: the client bundle is compiled BEFORE the server
# bundles, because its content hash is embedded into them as the cache key
# the SSR documents link the bundle with.
# Usage: sh scripts/build-ssr.sh

set -e
SRC_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/../cgi-bin/react-ssr" && pwd)"
BIN_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/../bin" && pwd)"
CGI_OUT="$(CDPATH= cd -- "$(dirname -- "$0")/../cgi-bin" && pwd)/react-ssr.cgi"
SERVER_OUT="$BIN_DIR/react-ssr-server"
CLIENT_OUT="$(CDPATH= cd -- "$(dirname -- "$0")/../www" && pwd)/js/react-ssr.js"
ESBUILD="$SRC_DIR/node_modules/.bin/esbuild"
TSC="$SRC_DIR/node_modules/.bin/tsc"
TAILWIND="$SRC_DIR/node_modules/.bin/tailwindcss"
TAILWIND_OUT="$SRC_DIR/tailwind.css"
TAILWIND_IN="$SRC_DIR/styles/main.css"

if [ ! -x "$ESBUILD" ] || [ ! -x "$TSC" ] || [ ! -x "$TAILWIND" ]; then
    echo "error: esbuild/tsc/tailwindcss missing. Run 'cd cgi-bin/react-ssr && npm install' first" >&2
    exit 1
fi

mkdir -p "$BIN_DIR" "$(dirname "$CLIENT_OUT")"

# Type-check before bundling (esbuild will NOT catch type errors).
( cd "$SRC_DIR" && "$TSC" -p tsconfig.json )

# Compile Tailwind v4: scans App.tsx for class names (see @source in the
# input), emits a minified stylesheet that render.tsx inlines into <head>.
"$TAILWIND" -i "$TAILWIND_IN" -o "$TAILWIND_OUT" --minify >/dev/null

# Client bundle: browser platform, IIFE (loaded via <script>).
"$ESBUILD" "$SRC_DIR/client.tsx" \
    --bundle \
    --platform=browser \
    --format=iife \
    --jsx=automatic \
    --minify \
    --define:process.env.NODE_ENV='"production"' \
    --outfile="$CLIENT_OUT"

# Cache-busting hash of the client bundle's BYTES. The server bundles embed
# it and SSR documents link /js/react-ssr.js?v=<hash>, so a rebuilt bundle
# gets a URL no client has cached. Hashed before the server bundles are
# built, which is why the client is compiled first. sha256sum is GNU
# coreutils (Linux/CI), shasum is the BSD/macOS spelling - one of the two
# exists everywhere this script runs.
BUNDLE_HASH=$(
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$CLIENT_OUT"
    else
        shasum -a 256 "$CLIENT_OUT"
    fi | cut -c1-8
)

# Precompressed twin: agent-httpd serves the .gz next to a file when the
# client sends Accept-Encoding: gzip (Content-Encoding negotiation in C).
# -n keeps the archive free of the source mtime/name: without it every
# rebuild produces different bytes for identical input, and the .gz shows up
# as a spurious diff (and a spurious ETag change) after a no-op build.
GZ_OUT="$CLIENT_OUT.gz"
gzip -n -9 -c "$CLIENT_OUT" > "$GZ_OUT"

# Server bundles: node platform, CJS (executed as CGI / resident daemon).
# loader:.css=text inlines the compiled Tailwind into the bundle as a string.
# It also covers the ../tailwind.css?raw import in render.tsx (esbuild
# matches loaders by extension and ignores the query), while the ?raw suffix
# makes Vite's dev SSR pipeline (scripts/dev-server.js) return the text too.
# BUNDLE_QUERY must be a quoted JS literal: esbuild substitutes the bare
# identifier, and render.tsx guards it with typeof for the Vite dev path.
for entry in cgi main; do
    case "$entry" in
        cgi)  out="$CGI_OUT" ;;
        main) out="$SERVER_OUT" ;;
    esac
    "$ESBUILD" "$SRC_DIR/server/$entry.tsx" \
        --bundle \
        --platform=node \
        --format=cjs \
        --jsx=automatic \
        --minify \
        --loader:.css=text \
        --define:process.env.NODE_ENV='"production"' \
        --define:BUNDLE_QUERY="\"$BUNDLE_HASH\"" \
        --banner:js='#!/usr/bin/env node' \
        --outfile="$out"
    chmod +x "$out"
done

echo "Built:        $CGI_OUT ($(wc -c < "$CGI_OUT") bytes)"
echo "Resident:     $SERVER_OUT ($(wc -c < "$SERVER_OUT") bytes)"
echo "Client:       $CLIENT_OUT ($(wc -c < "$CLIENT_OUT") bytes)"
echo "Client gzip:  $GZ_OUT ($(wc -c < "$GZ_OUT") bytes)"
echo "Bundle hash:  $BUNDLE_HASH (referenced as /js/react-ssr.js?v=$BUNDLE_HASH)"
echo "Tailwind:     $TAILWIND_OUT ($(wc -c < "$TAILWIND_OUT") bytes)"