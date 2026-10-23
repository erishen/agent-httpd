#!/bin/sh
# php2shell build-time guard (CVE-2026-109618 / CVE-2026-109619).
#
# Fails the image build when the installed php-cgi is an affected version,
# so a cached/old base image cannot silently pass. The CVE is in PHP
# request startup / OOM cleanup and triggers before application code runs,
# so no wrapper or config can mitigate it — only a fixed PHP.
#
# Affected: <= 8.2.34, <= 8.3.35, <= 8.4.26, <= 8.5.11.
# Fixed in: 8.2.35 / 8.3.36 / 8.4.27 / 8.5.12.
#
# Usage: run after installing php-cgi in a Docker stage:
#     RUN sh scripts/php-guard.sh
# Exit 0 = safe / no php-cgi; exit 1 = affected version found.

if ! command -v php-cgi >/dev/null 2>&1; then
    echo "php-guard: php-cgi not installed, nothing to guard"
    exit 0
fi

version=$(php-cgi -v 2>/dev/null | sed -n '1s/.*PHP \([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p')
if [ -z "$version" ]; then
    echo "php-guard: could not parse 'php-cgi -v', failing closed" >&2
    exit 1
fi

major=$(printf '%s' "$version" | cut -d. -f1)
minor=$(printf '%s' "$version" | cut -d. -f2)
patch=$(printf '%s' "$version" | cut -d. -f3)

affected=0
case "$major" in
    8)
        case "$minor" in
            0|1) affected=1 ;;  # 8.0/8.1 predate every fixed release
            2) [ "$patch" -le 34 ] && affected=1 ;;
            3) [ "$patch" -le 35 ] && affected=1 ;;
            4) [ "$patch" -le 26 ] && affected=1 ;;
            5) [ "$patch" -le 11 ] && affected=1 ;;
            # 8.6+ postdates the CVE's fixed releases; treat as safe
            *) ;;
        esac
        ;;
    # 6.x / 7.x predate every fixed release
    6|7) affected=1 ;;
    # major 9+ postdates the fixes; fail-open (too new for this CVE)
    *) ;;
esac

if [ "$affected" -eq 1 ]; then
    echo "php-guard: FAIL php-cgi $version is affected by php2shell (CVE-2026-109618/109619)" >&2
    echo "php-guard: need >= 8.2.35 / 8.3.36 / 8.4.27 / 8.5.12; rebuild the stage without cache" >&2
    exit 1
fi
echo "php-guard: php-cgi $version not affected (>= fixed threshold)"
exit 0
