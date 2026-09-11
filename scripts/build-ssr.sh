#!/bin/sh
# Build React SSR (TypeScript + Tailwind) into three bundles:
#   server.tsx      -> cgi-bin/react-ssr.cgi      (CGI, one process per request)
#   server/main.tsx -> bin/react-ssr-server       (resident FastCGI backend)
#   server/cgi.tsx  -> cgi-bin/react-ssr.cgi      (per-request CGI entry)
#   client.tsx      -> www/js/react-ssr.js        (browser hydration)
# All share App.tsx so the hydrated first render equals the server markup.
# A tsc --noEmit pass runs first: esbuild transpiles but never type-checks.
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

# Server bundles: node platform, CJS (executed as CGI / resident daemon).
# loader:.css=text inlines the compiled Tailwind into the bundle as a string.
# It also covers the ../tailwind.css?raw import in render.tsx (esbuild
# matches loaders by extension and ignores the query), while the ?raw suffix
# makes Vite's dev SSR pipeline (scripts/dev-server.js) return the text too.
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
        --banner:js='#!/usr/bin/env node' \
        --outfile="$out"
    chmod +x "$out"
done

# Client bundle: browser platform, IIFE (loaded via <script>).
"$ESBUILD" "$SRC_DIR/client.tsx" \
    --bundle \
    --platform=browser \
    --format=iife \
    --jsx=automatic \
    --minify \
    --define:process.env.NODE_ENV='"production"' \
    --outfile="$CLIENT_OUT"

# Precompressed twin: agent-httpd serves the .gz next to a file when the
# client sends Accept-Encoding: gzip (Content-Encoding negotiation in C).
GZ_OUT="$CLIENT_OUT.gz"
gzip -9 -c "$CLIENT_OUT" > "$GZ_OUT"

echo "Built:        $CGI_OUT ($(wc -c < "$CGI_OUT") bytes)"
echo "Resident:     $SERVER_OUT ($(wc -c < "$SERVER_OUT") bytes)"
echo "Client:       $CLIENT_OUT ($(wc -c < "$CLIENT_OUT") bytes)"
echo "Client gzip:  $GZ_OUT ($(wc -c < "$GZ_OUT") bytes)"
echo "Tailwind:     $TAILWIND_OUT ($(wc -c < "$TAILWIND_OUT") bytes)"