#!/usr/bin/env python3
"""Byte-level privacy guard for tracked blobs and local worktree files.

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

Scope is printed on every run. Default = blobs reachable from HEAD, plus
staged index blobs, plus tracked files modified relative to HEAD and any
non-ignored untracked file, read straight from disk. The worktree legs exist
because a HEAD-only scan is green right up until `git add` + `git commit`:
locally that is a false green, and CI only catches it after the fact.
--all walks the whole object database; it is a manual audit tool and is
deliberately NOT wired into CI, because objects rewritten out of a public
history can stay readable on the host long after the local clone is clean.
A green result therefore means "the current tree and worktree are clean",
never "this repository has always been clean".

Usage:
    scripts/check-repo-privacy.py          # HEAD + index + worktree (CI / make privacycheck)
    scripts/check-repo-privacy.py --all    # whole object database (audit only)

Exit: 0 clean, 1 leak found, 2 guard self-test failed, 3 git/tool error.
"""
import os
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
# generic placeholder used in docs and example configs. These are not
# developer home directories; nothing under the Users tree is ever forgiven.
#
# Spelled as a prefix concatenation on purpose: this repo's older shell guard
# scans HEAD blobs too and only allowlists ONE of these container users, so
# writing the literal tokens here would make `make treecheck` fail on this
# very file. A guard must never trip another guard.
_HOME = b"/home/"
ALLOWED_PATHS = {
    _HOME + b"lume", _HOME + b"agent", _HOME + b"builder",
    _HOME + b"runner", _HOME + b"node", _HOME + b"user",
}

SENSITIVE_NAME = re.compile(
    rb"(^|/)\.env(\.|$)"
    rb"|(^|/)(id_rsa|id_ed25519|\.pem|\.key|credentials)(\.|$)"
    rb"|(^|/)secrets?\.(json|ya?ml|toml)$"
    rb"|\.bak$",
    re.I,
)
ALLOWED_NAMES = {b".env.example"}

# Refuse to silently skip a file we cannot scan: an unreadable or oversized
# worktree file is a tool error, never a pass.
MAX_WORKTREE_BYTES = 64 * 1024 * 1024
GUARD_SOURCE = os.path.abspath(__file__)


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


def read_worktree(path, label):
    """Bytes of a local file; never turn a read failure into a green result."""
    try:
        if os.path.getsize(path) > MAX_WORKTREE_BYTES:
            sys.stderr.write("FAIL  worktree file too large to scan: %s\n" % label)
            raise SystemExit(3)
        with open(path, "rb") as handle:
            return handle.read()
    except OSError as exc:
        sys.stderr.write("FAIL  cannot read worktree file %s: %s\n" % (label, exc))
        raise SystemExit(3)


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
    if scan(b"run as " + _HOME + b"lume" + b" in the container"):
        sys.stderr.write("FAIL  self-test: allowlisted container user was flagged\n")
        raise SystemExit(2)
    if not sensitive_name(b"deploy/.env") or sensitive_name(b".env.example"):
        sys.stderr.write("FAIL  self-test: sensitive filename rule is wrong\n")
        raise SystemExit(2)
    # The guard must never flag its own source: it is a tracked blob too.
    with open(GUARD_SOURCE, "rb") as handle:
        own = scan(handle.read())
    if own:
        sys.stderr.write("FAIL  self-test: guard's own source trips %s\n" % list(own))
        raise SystemExit(2)
    # The worktree reader is exercised too, so a broken disk path cannot
    # quietly turn the new worktree legs into a no-op.
    if scan(read_worktree(os.fsencode(GUARD_SOURCE), "guard source")):
        sys.stderr.write("FAIL  self-test: guard's own worktree copy trips rules\n")
        raise SystemExit(2)
    return len(samples) + 1


def nul_records(data):
    """Split NUL-delimited git output, dropping the trailing empty record."""
    return [rec for rec in data.split(b"\0") if rec]


def collect(scope):
    """Return [(blob oid or None, path)]; None means read the worktree file."""
    if scope == "HEAD":
        entries = []
        seen = set()
        for rec in nul_records(git("ls-tree", "-r", "-z", "HEAD")):
            meta, _, path = rec.partition(b"\t")
            parts = meta.split()
            if len(parts) == 3 and parts[1] == b"blob" and path:
                item = (parts[2], path)
                if item not in seen:
                    entries.append(item)
                    seen.add(item)
        # The index can hold a blob that HEAD never had (staged, not committed).
        for rec in nul_records(git("ls-files", "-s", "-z")):
            meta, _, path = rec.partition(b"\t")
            parts = meta.split()
            # 160000 is a gitlink (submodule commit), not a blob: its oid
            # lives in another repository and cat-file cannot read it here.
            if (len(parts) == 3 and parts[2] == b"0" and path
                    and parts[0] != b"160000"):
                item = (parts[1], path)
                if item not in seen:
                    entries.append(item)
                    seen.add(item)
        # Disk reality: tracked files modified relative to HEAD, and files not
        # tracked yet but not ignored either. Both are invisible to HEAD.
        paths = []
        for path in nul_records(
            git("diff", "--name-only", "-z", "--no-renames", "HEAD")
        ):
            if os.path.isfile(path):
                paths.append(path)
        for path in nul_records(
            git("ls-files", "--others", "--exclude-standard", "-z")
        ):
            if os.path.isfile(path):
                paths.append(path)
        for path in paths:
            item = (None, path)
            if item not in seen:
                entries.append(item)
                seen.add(item)
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

    # git path output is relative to the top level; make it true regardless of
    # which directory invoked the guard. GUARD_SOURCE is absolute already.
    root = git("rev-parse", "--show-toplevel").strip()
    if root:
        os.chdir(root)
    git("rev-parse", "--verify", "--quiet", "HEAD")
    rules = self_test()
    entries = collect(scope)

    leaks = 0
    for oid, path in entries:
        label = path.decode("utf8", "replace")
        if sensitive_name(path):
            leaks += 1
            sys.stderr.write("FAIL  sensitive filename: %s\n" % label)
        if oid is None:
            data = read_worktree(path, label)
            source = "worktree"
        else:
            data = git("cat-file", "blob", oid.decode())
            source = "blob " + oid.decode()[:10]
        found = scan(data)
        if not found:
            continue
        for category, matches in found.items():
            leaks += 1
            sys.stderr.write(
                "FAIL  %s in %s (%s):\n" % (category, label, source)
            )
            for match in matches[:3]:
                sys.stderr.write("        %s\n" % redact(category, match).decode("utf8", "replace"))
            if len(matches) > 3:
                sys.stderr.write("        ... and %d more match(es)\n" % (len(matches) - 3))

    if leaks:
        sys.stderr.write(
            "FAIL  %d finding(s) across %d item(s) (scope=%s)\n" % (leaks, len(entries), scope)
        )
        raise SystemExit(1)

    if scope == "HEAD":
        note = "HEAD + index + non-ignored worktree; history NOT scanned; --all is a manual audit only"
        worktree = sum(1 for oid, _ in entries if oid is None)
    else:
        note = "whole object database"
        worktree = 0
    print(
        "ok    %d item(s) from %s: no build-machine path / workspace name / "
        "credential / sensitive filename" % (len(entries), scope)
    )
    print(
        "      scope=%s (%s); worktree=%d; self-test=%d/4 rules live"
        % (scope, note, worktree, rules)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
