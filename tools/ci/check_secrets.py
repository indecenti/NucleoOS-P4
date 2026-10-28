#!/usr/bin/env python3
"""Refuse commits that would put something private on the PUBLIC repo.

The repo is public and a pushed commit stays in its history, forks and caches even after a revert,
so the check runs BEFORE the push (git pre-push hook, see --install-hook) and again in CI (late, but
it turns the run red so nobody builds on top of it).

Blocks:
  - commit metadata: an author/committer that is not the project identity; an AI signature in the
    message or in an added line (co-author trailers naming an AI, its no-reply address, "generated
    with" footers, session links)
  - added lines: private keys, provider API keys and tokens, paths with the developer's user name
  - added files: teacher.json, private *.pem / *.key, .env, id_rsa, secrets.json
  - private words: every line of %USERPROFILE%/.nucleo/private-words.txt (home Wi-Fi name, home IPs,
    work e-mail, city...) — kept OUT of the repo on purpose: listing them here would publish them.
    CI has no such file and checks only the generic rules.
Warns (does not block): new images — screenshots show the Wi-Fi name and the city in the status bar;
look at each one (every frame of a GIF) before pushing.

  python tools/ci/check_secrets.py --range origin/main..HEAD     a commit range
  python tools/ci/check_secrets.py --pre-push                    as the pre-push hook (refs on stdin)
  python tools/ci/check_secrets.py --install-hook                writes .git/hooks/pre-push

Exit code: 0 clean, 1 something to fix (nothing was pushed).
"""
import argparse
import os
import re
import subprocess
import sys

IDENTITY_NAME = "indecenti"
IDENTITY_EMAIL = "niki070585@gmail.com"     # the public contact address (README, LICENSE, SECURITY)

# written so that this file's own source never matches them
AI_SIGNATURE = re.compile(
    r"co-authored-by:\s*(claude|anthropic)|noreply@anthropic\.com|generated with \[?claude|"
    r"claude-session:\s|claude\.ai/code/session", re.I)

SECRET_LINE = [
    # an input's placeholder (the SSH app's "paste your key here" field) is the header alone, never a key
    ("private key", re.compile(r"(?<!placeholder=[\"'])-----BEGIN [A-Z ]*PRIVATE KEY-----")),
    ("Anthropic key", re.compile(r"sk-ant-[A-Za-z0-9_-]{20,}")),
    ("OpenAI key", re.compile(r"\bsk-(proj-)?[A-Za-z0-9_-]{32,}")),
    ("Google API key", re.compile(r"\bAIza[0-9A-Za-z_-]{35}\b")),
    ("GitHub token", re.compile(r"\bgh[pousr]_[A-Za-z0-9]{36,}\b|\bgithub_pat_[A-Za-z0-9_]{40,}")),
    ("Groq key", re.compile(r"\bgsk_[A-Za-z0-9]{40,}")),
    ("Hugging Face token", re.compile(r"\bhf_[A-Za-z0-9]{30,}")),
    ("Slack token", re.compile(r"\bxox[abprs]-[A-Za-z0-9-]{10,}")),
    ("AWS key", re.compile(r"\bAKIA[0-9A-Z]{16}\b")),
    ("Telegram bot token", re.compile(r"\b\d{8,10}:AA[A-Za-z0-9_-]{33}\b")),
    ("user home path", re.compile(r"(?:Users|home)[\\/]+[i]ndecenti\b|[I]NDECE~\d", re.I)),
]

SECRET_FILE = re.compile(
    r"(^|/)(teacher\.json|ai-keys\.json|secrets\.json|\.env|id_rsa[^/]*|id_ed25519[^/]*|token)$|"
    r"\.(key|secret|p12|pfx)$|signing-key[^/]*\.pem$|private[^/]*\.pem$", re.I)
IMAGE = re.compile(r"\.(png|jpe?g|gif|webp|bmp)$", re.I)


def git(*args):
    r = subprocess.run(["git", *args], capture_output=True)
    if r.returncode != 0:
        sys.exit(f"check_secrets: git {' '.join(args)}: {r.stderr.decode('utf-8', 'replace').strip()}")
    return r.stdout.decode("utf-8", "replace")


def private_words():
    """Lines of ~/.nucleo/private-words.txt (# comments and blanks skipped), lowercased."""
    path = os.path.join(os.path.expanduser("~"), ".nucleo", "private-words.txt")
    try:
        with open(path, encoding="utf-8") as f:
            return [w.strip().lower() for w in f if w.strip() and not w.lstrip().startswith("#")]
    except OSError:
        return []


def commits_in(rng):
    out = git("rev-list", "--reverse", *rng.split())
    return [c for c in out.split() if c]


def check_commit(sha, words, problems, images):
    meta = git("show", "-s", "--format=%an%x00%ae%x00%cn%x00%ce%x00%P%x00%B", sha).split("\x00")
    an, ae, cn, ce, parents, body = meta[0], meta[1], meta[2], meta[3], meta[4], "\x00".join(meta[5:])
    short = sha[:8]
    for who, name, mail in (("author", an, ae), ("committer", cn, ce)):
        if name != IDENTITY_NAME or mail.lower() != IDENTITY_EMAIL:
            problems.append(f"{short}: {who} is '{name} <{mail}>', not '{IDENTITY_NAME} <{IDENTITY_EMAIL}>'")
    for line in body.splitlines():
        if AI_SIGNATURE.search(line):
            problems.append(f"{short}: AI signature in the message: {line.strip()[:80]}")
        low = line.lower()
        for w in words:
            if w in low:
                problems.append(f"{short}: a private word (private-words.txt) in the message")
    if len(parents.split()) > 1:
        return                                   # a merge adds nothing of its own: its parents are checked
    for row in git("show", "--format=", "--name-status", "--no-renames", sha).splitlines():
        parts = row.split("\t")
        if len(parts) < 2 or parts[0].startswith("D"):
            continue
        path = parts[-1]
        if SECRET_FILE.search(path) and not path.endswith(".example"):
            problems.append(f"{short}: secret file added: {path}")
        if IMAGE.search(path):
            images.add(path)
    current = "?"
    for line in git("show", "--format=", "-U0", "--no-color", "--no-ext-diff", sha).splitlines():
        if line.startswith("+++ "):
            current = line[6:] if line.startswith("+++ b/") else line[4:]
            continue
        if not line.startswith("+") or line.startswith("+++"):
            continue
        text = line[1:]
        if AI_SIGNATURE.search(text):
            problems.append(f"{short}: AI signature in {current}: {text.strip()[:60]}")
        for what, rx in SECRET_LINE:
            if rx.search(text):
                problems.append(f"{short}: {what} in {current}: {text.strip()[:60]}...")
        low = text.lower()
        for w in words:
            if w in low:
                problems.append(f"{short}: a private word (private-words.txt) in {current}")


def run(ranges):
    words = private_words()
    problems, images, seen = [], set(), set()
    for rng in ranges:
        for sha in commits_in(rng):
            if sha not in seen:
                seen.add(sha)
                check_commit(sha, words, problems, images)
    for p in dict.fromkeys(problems):
        print("BLOCKED", p)
    if images:
        print(f"check_secrets: {len(images)} new image(s): look at each (Wi-Fi name, city, names, notifications)"
              " before they go public:")
        for p in sorted(images):
            print("  image", p)
    if not words:
        print("check_secrets: no ~/.nucleo/private-words.txt: home Wi-Fi name / IPs / e-mails not checked")
    print(f"check_secrets: {len(seen)} commit(s), {len(set(problems))} problem(s)")
    return 1 if problems else 0


def pre_push_ranges(stdin):
    """git pre-push: '<local ref> <local sha> <remote ref> <remote sha>' per line."""
    zero = "0" * 40
    ranges = []
    for line in stdin.read().splitlines():
        parts = line.split()
        if len(parts) != 4 or parts[1] == zero:
            continue                                  # a delete pushes no content
        local, remote = parts[1], parts[3]
        ranges.append(f"{remote}..{local}" if remote != zero else f"{local} --not --remotes")
    return ranges


HOOK = """#!/bin/sh
# Installed by tools/ci/check_secrets.py --install-hook: nothing private reaches the public repo.
top=$(git rev-parse --show-toplevel)
script="$top/tools/ci/check_secrets.py"
[ -f "$script" ] || script="$(git rev-parse --git-common-dir)/../tools/ci/check_secrets.py"
exec python "$script" --pre-push
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--range", action="append", help="git rev-list range, e.g. origin/main..HEAD")
    ap.add_argument("--pre-push", action="store_true", help="read the pushed refs from stdin (git hook)")
    ap.add_argument("--install-hook", action="store_true", help="write the pre-push hook of this clone")
    a = ap.parse_args()
    if a.install_hook:
        hooks = os.path.join(git("rev-parse", "--git-common-dir").strip(), "hooks")
        path = os.path.join(hooks, "pre-push")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(HOOK)
        os.chmod(path, 0o755)
        print(f"installed {path}")
        return 0
    ranges = pre_push_ranges(sys.stdin) if a.pre_push else (a.range or ["origin/main..HEAD"])
    if os.environ.get("CHECK_SECRETS_DEBUG"):
        print("check_secrets: ranges", ranges)
    return run(ranges)


if __name__ == "__main__":
    sys.exit(main())
