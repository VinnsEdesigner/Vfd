#!/usr/bin/env python3
"""
rename-to-vyzor.py — call the server "Vyzor", not "engine".

WHY THIS IS A SCRIPT AND NOT A sed ONE-LINER

"engine" is three different words in this repository:

  1. the product noun, in prose and in identifiers — "the engine", `engineVersion`,
     `engine_dir`, the Settings tab labelled "Engine";
  2. a *type* in the file-source union, `source: 'engine' | 'device' | 'remote'`,
     which means "this file lives on the server" and is compared by value;
  3. someone else's engine — "both engines" in a CSS comment about scrollbars.

A blind replace mangles all three together. This script is deliberately
conservative: it renames the product noun, keeps the source union value (so the
wire and any stored records stay valid), and leaves third-party engines alone.

WHAT IT CHANGES

  engine -> Vyzor            prose and identifiers, both cases
  engines -> Vyzors          the plural, when it means this server
  engine_version -> vyzor_version          the wire field, both ends at once
  MI_ENGINE_VERSION -> MI_VYZOR_VERSION    the env constant
  media-ingest -> vyzor                    the binary and the tool name
  EngineStorageNotice.tsx -> VyzorStorageNotice.tsx   the one engine-named file

WHAT IT DELIBERATELY DOES NOT CHANGE

  * `'engine'` as a value of `source` in the view model — that is a wire/storage
    enum, and changing it would silently break records already on disk;
  * "both engines" and other third-party references (see the skip list);
  * the directory `media-ingest-c/` and the module prefix `mi_` — a path rename
    would break the Makefile, the CI workflow, the docker-compose build context
    and every import for no gain, and `mi_` is not the word "engine" at all.

Run it from the repository root. It is idempotent: a second run finds nothing to
do, because the words it looks for are gone.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

# Directories never worth walking. `.git` because rewriting history is not the
# job; `node_modules` because it is not ours.
SKIP_DIRS = {".git", "node_modules", "build", "dist", ".vite", "__pycache__"}

# Extensions we will rewrite. Anything else is left untouched — this is a source
# tree, not a binary one.
TEXT_SUFFIXES = {
    ".ts", ".tsx", ".js", ".mjs", ".cjs", ".json",
    ".c", ".h", ".css", ".html", ".md", ".yml", ".yaml",
    ".sh", ".txt", ".toml", ".cfg", ".ini",
}

# Files with no suffix that are still text we own.
TEXT_NAMES = {"Makefile", "Dockerfile", ".gitignore", ".dockerignore"}

# Lines matching any of these are left exactly as they are. Each is a place
# where "engine" means something other than this server, or where changing it
# would break a contract.
SKIP_LINE_PATTERNS = [
    # Third-party engines (browsers, layout engines).
    re.compile(r"both engines", re.IGNORECASE),
    re.compile(r"third-party engine", re.IGNORECASE),
    re.compile(r"rendering engine", re.IGNORECASE),
    re.compile(r"layout engine", re.IGNORECASE),
    re.compile(r"search engine", re.IGNORECASE),
    # The file-source union *value*. It is a stored/wire enum, and renaming it
    # would invalidate records rather than rename a product. Every place this
    # value is declared or compared is covered by these forms.
    re.compile(r"source:\s*['\"]engine['\"]"),
    re.compile(r"\.source\s*[!=]==?\s*['\"]engine['\"]"),
]

# A quoted `'engine'` that is NOT the source enum is an internal id — a Settings
# tab, a file-toolbar filter — and is renamed to the lowercase `vyzor` so it
# stays a plausible identifier rather than becoming `Vyzor`.
QUOTED_ID = [
    (re.compile(r"'engine'"), "'vyzor'"),
    (re.compile(r'"engine"'), '"vyzor"'),
]

# Ordered replacements, applied to each kept line. Order matters: the longer,
# more specific forms first so `engine_version` is not half-renamed by the bare
# `engine` rule.
REPLACEMENTS: list[tuple[re.Pattern[str], str]] = [
    # The wire field, both ends. Snake case on the wire, camel in TypeScript.
    (re.compile(r"\bengine_version\b"), "vyzor_version"),
    (re.compile(r"\bengineVersion\b"), "vyzorVersion"),
    # The env constant.
    (re.compile(r"\bMI_ENGINE_VERSION\b"), "MI_VYZOR_VERSION"),
    # A TypeScript object property named `engine` (e.g. `engine: EngineFacts;`)
    # and its JSX/property uses. These must keep the *same* spelling everywhere
    # or the code stops compiling, so they are pinned to lowercase before the
    # bare-word rule can capitalise some of them.
    (re.compile(r"\bengine(?=\s*:\s*[A-Z])"), "vyzor"),
    (re.compile(r"\bengine(?=\s*=\s*\{)"), "vyzor"),
    (re.compile(r"\bprops\.engine\b"), "props.vyzor"),
    (re.compile(r"\bengine(?=\.engineVersion\b)"), "vyzor"),
    # Identifiers that carry the word as a unit.
    (re.compile(r"\bengineLocation\b"), "vyzorLocation"),
    (re.compile(r"\bEngineLocationFacts\b"), "VyzorLocationFacts"),
    (re.compile(r"\bengineStorage\b"), "vyzorStorage"),
    (re.compile(r"\bEngineStorageFacts\b"), "VyzorStorageFacts"),
    (re.compile(r"\bEngineStorageNotice\b"), "VyzorStorageNotice"),
    (re.compile(r"\bEngineStorageNoticeProps\b"), "VyzorStorageNoticeProps"),
    (re.compile(r"\bengineIds\b"), "vyzorIds"),
    (re.compile(r"\bengineOnly\b"), "vyzorOnly"),
    (re.compile(r"\bengineHost\b"), "vyzorHost"),
    (re.compile(r"\bengineDir\b"), "vyzorDir"),
    (re.compile(r"\bEngineFacts\b"), "VyzorFacts"),
    (re.compile(r"\bDropStaleEngineData\b"), "DropStaleVyzorData"),
    # The tool/binary name. `media-ingest-c` (the directory) is deliberately not
    # matched: the pattern requires the name not to be followed by `-c`.
    (re.compile(r"\bmedia-ingest(?!-c)\b"), "vyzor"),
    # The plural, then the singular. The plural first so `engines` is not turned
    # into `Vyzors` one letter at a time.
    (re.compile(r"\bengines\b"), "Vyzors"),
    (re.compile(r"\bEngines\b"), "Vyzors"),
    (re.compile(r"\bengine\b"), "Vyzor"),
    (re.compile(r"\bEngine\b"), "Vyzor"),
    # Grammar: "Vyzor" starts with a consonant sound, so the article is "a".
    # These run last, after the words they fix exist.
    (re.compile(r"\ban Vyzor\b"), "a Vyzor"),
    (re.compile(r"\bAn Vyzor\b"), "A Vyzor"),
]

# The one file named after the engine. (old relative path, new relative path)
RENAMES = [
    ("web/src/ui/storage/EngineStorageNotice.tsx", "web/src/ui/storage/VyzorStorageNotice.tsx"),
]


# Files that are made of identifiers rather than prose — YAML, dotfiles — get the
# lowercase form throughout, because `Vyzor:` would be a service name no one
# wants to type and `Vyzor-data` would not match its own volume.
LOWERCASE_FILES = {"docker-compose.yml", "docker-compose.yaml", ".gitignore", ".dockerignore"}

# In those files, the word is replaced in lowercase, including the volume name.
LOWERCASE_REPLACEMENTS: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"\bmedia-ingest(?!-c)\b"), "vyzor"),
    (re.compile(r"\bengines\b"), "vyzors"),
    (re.compile(r"\bengine\b"), "vyzor"),
]


def is_text_file(path: Path) -> bool:
    if path.name in TEXT_NAMES:
        return True
    return path.suffix.lower() in TEXT_SUFFIXES


def should_skip_line(line: str) -> bool:
    return any(p.search(line) for p in SKIP_LINE_PATTERNS)


def rewrite_line(line: str, lowercase: bool = False) -> str:
    if should_skip_line(line):
        return line
    # A quoted internal id is a lowercase identifier, not prose; give it its own
    # pass so the bare-word rules below do not capitalise it.
    for pattern, replacement in QUOTED_ID:
        line = pattern.sub(replacement, line)
    if lowercase:
        for pattern, replacement in LOWERCASE_REPLACEMENTS:
            line = pattern.sub(replacement, line)
        return line
    for pattern, replacement in REPLACEMENTS:
        line = pattern.sub(replacement, line)
    return line


def iter_files(root: Path):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            path = Path(dirpath) / name
            if is_text_file(path):
                yield path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root (default: .)")
    parser.add_argument("--dry-run", action="store_true", help="report, change nothing")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    if not (root / "media-ingest-c").is_dir():
        print(f"error: {root} does not look like this repository (no media-ingest-c/)", file=sys.stderr)
        return 2

    changed_files = 0
    changed_lines = 0
    for path in iter_files(root):
        try:
            original = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        lines = original.splitlines(keepends=True)
        lower = path.name in LOWERCASE_FILES
        new_lines = [rewrite_line(line, lower) for line in lines]
        if new_lines == lines:
            continue
        delta = sum(1 for a, b in zip(lines, new_lines) if a != b)
        rel = path.relative_to(root)
        print(f"  {rel}: {delta} line(s)")
        changed_files += 1
        changed_lines += delta
        if not args.dry_run:
            path.write_text("".join(new_lines), encoding="utf-8")

    for old, new in RENAMES:
        old_path, new_path = root / old, root / new
        if old_path.exists():
            print(f"  rename {old} -> {new}")
            if not args.dry_run:
                new_path.parent.mkdir(parents=True, exist_ok=True)
                old_path.rename(new_path)

    verb = "would change" if args.dry_run else "changed"
    print(f"\n{verb} {changed_lines} line(s) in {changed_files} file(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
