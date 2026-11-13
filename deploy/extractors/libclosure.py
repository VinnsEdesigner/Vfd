#!/usr/bin/env python3
"""Resolve and copy a binary's shared-library closure from an unpacked tree.

`ldd` cannot be used: it *runs* the binary to trace the loader, and these are
arm64 binaries in an amd64 build container — "exec format error". So the closure
is read statically from the ELF `DT_NEEDED` entries, which is what the loader
does anyway, and each soname is looked for in the search directories given.

A miss is reported loudly, not dropped: a library left out of the bundle is a
tool that fails only on the device. This is why the search list must include the
subdirectories Debian puts some libraries in (`blas/`, `lapack/`, …) — a flat
search misses them, and the miss looks exactly like a library that is genuinely
absent.

Usage: libclosure.py <binary> <out-dir> <search-dir> [<search-dir> ...]
"""

import os
import re
import shutil
import subprocess
import sys

# The glibc core, provided by the device rootfs. Matched by soname prefix.
SKIP = (
    "libc.so",
    "libm.so",
    "libpthread.so",
    "libdl.so",
    "librt.so",
    "libresolv.so",
    "libnsl.so",
    "libutil.so",
    "ld-linux",
)


def needed(path):
    out = subprocess.run(
        ["readelf", "-d", path], capture_output=True, text=True, check=False
    ).stdout
    return re.findall(r"\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]", out)


def find(soname, dirs):
    for d in dirs:
        candidate = os.path.join(d, soname)
        if os.path.exists(candidate):
            return candidate
    return None


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    binary, out_dir = sys.argv[1], sys.argv[2]
    search = sys.argv[3:]

    os.makedirs(out_dir, exist_ok=True)

    seen, queue, copied, missing = set(), [binary], 0, []
    while queue:
        path = queue.pop()
        for soname in needed(path):
            if soname in seen:
                continue
            seen.add(soname)
            if soname.startswith(SKIP):
                continue
            found = find(soname, search)
            if not found:
                missing.append(soname)
                continue
            dest = os.path.join(out_dir, soname)
            shutil.copy2(os.path.realpath(found), dest)  # follow the soname symlink
            copied += 1
            queue.append(dest)

    print(f"libclosure: {os.path.basename(binary)} -> {copied} libraries")
    if missing:
        # Non-zero exit: a caller that pipes this into a build must notice.
        print(f"libclosure: MISSING {sorted(set(missing))}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
