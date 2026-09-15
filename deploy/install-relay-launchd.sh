#!/usr/bin/env bash
# Install two launchd LaunchAgents that keep the host-side MCP relays alive:
# auto-start at login and auto-restart if they exit. Run ON THE HOST.
#
# Uninstall:
#   launchctl bootout gui/$(id -u)/cn.erishen.agent-httpd.relay-portfolio-check
#   launchctl bootout gui/$(id -u)/cn.erishen.agent-httpd.relay-pse-review
#   rm ~/Library/LaunchAgents/cn.erishen.agent-httpd.relay-*.plist
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
WORKSPACE="$(cd "$REPO/../../.." && pwd)"   # <workspace root>
BRIDGES="$WORKSPACE/work/harness/resolve-skills/skills/weekly-investment/scripts"
RELAY="$HERE/mcp-host-relay.mjs"
LOGDIR="$HOME/Library/Logs"
AGENTS="$HOME/Library/LaunchAgents"

NODE_BIN="$(command -v node || true)"
if [ -z "$NODE_BIN" ]; then echo "node not found on PATH" >&2; exit 1; fi
# launchd starts with a bare PATH: give the bridges uv (~/.local/bin) + make/git.
PATH_ENV="$HOME/.local/bin:/usr/local/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin:$(dirname "$NODE_BIN")"

mkdir -p "$AGENTS" "$LOGDIR"

install_one() {
  local label="$1" port="$2" bridge="$3" name="$4"
  local plist="$AGENTS/$label.plist"
  if [ ! -f "$bridge" ]; then echo "!! bridge missing: $bridge" >&2; return 1; fi
  cat > "$plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$label</string>
  <key>ProgramArguments</key>
  <array>
    <string>$NODE_BIN</string>
    <string>$RELAY</string>
    <string>$port</string>
    <string>$bridge</string>
  </array>
  <key>EnvironmentVariables</key>
  <dict>
    <key>PATH</key><string>$PATH_ENV</string>
  </dict>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>$LOGDIR/agent-httpd-relay-$name.log</string>
  <key>StandardErrorPath</key><string>$LOGDIR/agent-httpd-relay-$name.err.log</string>
</dict>
</plist>
PLIST
  launchctl bootout "gui/$(id -u)/$label" 2>/dev/null || true
  launchctl bootstrap "gui/$(id -u)" "$plist"
  echo "installed $label  (port $port, bridge $bridge)"
}

install_one cn.erishen.agent-httpd.relay-portfolio-check 45001 "$BRIDGES/portfolio-check.mjs" portfolio-check
install_one cn.erishen.agent-httpd.relay-pse-review        45002 "$BRIDGES/pse-review.mjs"      pse-review

sleep 1
echo "--- status ---"
launchctl list | grep agent-httpd.relay || true
