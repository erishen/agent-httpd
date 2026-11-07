#!/usr/bin/env python3
"""Byte-level privacy guard for blobs tracked by git.

Why a second guard beside check-tree-privacy.sh
-----------------------------------------------
The shell guard matches only /Users/<name> and /home/<name>. A compiled CGI
that carried a private workspace token stayed green through the 2026 history
scrub: the scrub replaced the build-machine path but never looked for that
token, and no pattern covered credentials or sensitive filenames either.
This guard widens the pattern set and adds two things a grep pipeline cannot
do reliably here:

  * byte-level matching inside binary blobs (Mach-O / ELF CGI artifacts);
  * a self-test on every invocation, so a pattern that silently stops
    matching cannot report a false green. The shell guard's own header
    documents exactly that failure mode (a first cut scanned 0 blobs and
    still printed success).

Scope is printed on every run. Default = blobs reachable from HEAD only.
--all walks the whole object database; it is a manual audit tool and is
deliberately NOT wired into CI, because objects rewritten out of a public
history can stay readable on the host long after the local clone is clean.
A green result therefore means "the current tree is clean", never "this
repository has always been clean".

Usage:
    scripts/check-repo-privacy.py          # HEAD blobs (CI / make privacycheck)
    scripts/check-repo-privacy.py --all    # whole object database (audit only)

Exit: 0 clean, 1 leak found, 2 guard self-test failed, 3 git/tool error.
"""
import re
import subprocess
import sys

# Written as character classes on purpose: a guard that spells the literals
# it forbids would flag its own source as a leak. `Workspace/CN[B]` matches
# the token but never appears in full in this file; same for the other one.
PATTERNS = {
    "build-machine path": re.compile(rb"/(?:Users|home)/[A-Za-z0-9_.@-]{2,80}"),
    "private workspace name": re.compile(rb"Workspace/CN[B]|individular-inves[t]"),
    "credential": re.compile(
        rb"\bsk-[A-Za-z0-9_-]{20,}"
        rb"|\bgh[pousr]_[A-Za-z0-9]{20,}"
        rb"|\bgithub_pat_[A-Za-z0-9_]{20,}"
        rb"|\bAKIA[0-9A-Z]{16}"
        rb"|\bAIza[0-9A-Za-z_-]{30,}"
        rb"|\bxox[baprs]-[A-Za-z0-9-]{10,}"
        rb"|-----BEGIN [A-Z ]*PRIVATE KEY-----"
        rb"|\bglpat-[A-Za-z0-9_-]{20,}"
    ),
}

# Fixed-name accounts created by our own Dockerfiles / CI images, plus the
# generic /home/user placeholder used in docs and example configs. These are
# not developer home directories; nothing under /Users/ is ever forgiven.
ALLOWED_PATHS = {
    b"/home/lume", b"/home/agent", b"/home/builder",
    b"/home/runner", b"/home/node", b"/home/user",
}

SENSITIVE_NAME = re.compile(
    rb"(^|/)\.env(\.|$)"
    rb"|(^|/)(id_rsa|id_ed25519|\.pem|\.key|credentials)(\.|$)"
    rb"|(^|/)secrets?\.(json|ya?ml|toml)$"
    rb"|\.bak$",
    re.I,
)
ALLOWED_NAMES = {b".env.example"}


def git(*args):
    try:
        return subprocess.run(
            ["git", *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            check=True,
        ).stdout
    except subprocess.CalledProcessError as exc:
        sys.stderr.write(
            "FAIL  git %s: %s\n" % (" ".join(args), exc.stderr.decode("utf8", "replace").strip())
        )
        raise SystemExit(3)


def scan(data):
    """Return {category: [matched bytes]} for one blob, allowlist applied."""
    found = {}
    for label, pattern in PATTERNS.items():
        matches = pattern.findall(data)
        if label == "build-machine path":
            matches = [m for m in matches if m not in ALLOWED_PATHS]
        if matches:
            found[label] = matches
    return found


def sensitive_name(path):
    name = path.rsplit(b"/", 1)[-1]
    if name in ALLOWED_NAMES:
        return False
    return bool(SENSITIVE_NAME.search(path))


def self_test():
    """Prove every pattern still fires before declaring anything clean."""
    samples = {
        "build-machine path": b"DWARF " + b"/Users/" + b"devuser" + b"/src/a.c",
        "private workspace name": b"cwd " + b"Workspace/CN" + b"B/project",
        "credential": b"token=" + b"sk-" + b"A" * 32,
    }
    for label, sample in samples.items():
        if label not in scan(sample):
            sys.stderr.write("FAIL  self-test: %s pattern did not fire\n" % label)
            raise SystemExit(2)
    if scan(b"run as " + b"/home/lume" + b" in the container"):
        sys.stderr.write("FAIL  self-test: allowlisted container user was flagged\n")
        raise SystemExit(2)
    if not sensitive_name(b"deploy/.env") or sensitive_name(b".env.example"):
        sys.stderr.write("FAIL  self-test: sensitive filename rule is wrong\n")
        raise SystemExit(2)
    # The guard must never flag its own source: it is a tracked blob too.
    with open(__file__, "rb") as handle:
        own = scan(handle.read())
    if own:
        sys.stderr.write("FAIL  self-test: guard's own source trips %s\n" % list(own))
        raise SystemExit(2)
    return len(samples) + 1


def collect(scope):
    """Return [(blob oid, path)] for the requested scope."""
    if scope == "HEAD":
        entries = []
        for line in git("ls-tree", "-r", "HEAD").splitlines():
            meta, _, path = line.partition(b"\t")
            parts = meta.split()
            if len(parts) == 3 and parts[1] == b"blob":
                entries.append((parts[2], path))
        return entries
    names = {}
    for line in git("rev-list", "--objects", "--all").splitlines():
        parts = line.split(b" ", 1)
        if len(parts) == 2:
            names.setdefault(parts[0], parts[1])
    entries = []
    for line in git(
        "cat-file", "--batch-all-objects",
        "--batch-check=%(objectname) %(objecttype)",
    ).splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1] == b"blob":
            entries.append((parts[0], names.get(parts[0], b"<unreachable>")))
    return entries


def redact(label, value):
    if label == "credential":
        return value[:10] + b"...<redacted>"
    return value[:100]


def main():
    args = sys.argv[1:]
    if args == ["--all"]:
        scope = "all objects"
    elif args:
        sys.stderr.write("usage: check-repo-privacy.py [--all]\n")
        raise SystemExit(3)
    else:
        scope = "HEAD"

    git("rev-parse", "--verify", "--quiet", "HEAD")
    rules = self_test()
    entries = collect(scope)

    leaks = 0
    for oid, path in entries:
        label = path.decode("utf8", "replace")
        if sensitive_name(path):
            leaks += 1
            sys.stderr.write("FAIL  sensitive filename tracked: %s\n" % label)
        data = git("cat-file", "blob", oid.decode())
        found = scan(data)
        if not found:
            continue
        for category, matches in found.items():
            leaks += 1
            sys.stderr.write(
                "FAIL  %s in %s (blob %s):\n" % (category, label, oid.decode()[:10])
            )
            for match in matches[:3]:
                sys.stderr.write("        %s\n" % redact(category, match).decode("utf8", "replace"))
            if len(matches) > 3:
                sys.stderr.write("        ... and %d more match(es)\n" % (len(matches) - 3))

    if leaks:
        sys.stderr.write(
            "FAIL  %d finding(s) across %d blob(s) (scope=%s)\n" % (leaks, len(entries), scope)
        )
        raise SystemExit(1)

    note = (
        "history NOT scanned; --all is a manual audit only"
        if scope == "HEAD" else "whole object database"
    )
    print(
        "ok    %d blob(s) from %s: no build-machine path / workspace name / "
        "credential / sensitive filename" % (len(entries), scope)
    )
    print("      scope=%s (%s); self-test=%d/4 rules live" % (scope, note, rules))
    return 0


if __name__ == "__main__":
    sys.exit(main())
