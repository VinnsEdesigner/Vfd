#!/usr/bin/env python3
"""Resolve and copy a cross-compiled binary's shared-library closure.

`ldd` cannot be used here: it *runs* the binary, which for a foreign
architecture is an "exec format error" (and needs qemu). So the closure is
resolved statically from the ELF `DT_NEEDED` entries, which is what the loader
does anyway, and each soname is located by searching the multiarch library
directories.

The glibc core is deliberately left out. The device's rootfs is Debian bookworm
(glibc 2.36), the same suite this cross-build links against, so the rootfs
provides those — and shipping a second copy of libc is how a bundle breaks a
device. Everything else the binary needs is copied, so the bundle is
self-contained.

Usage: libclosure.py <binary> <out-dir> [<search-dir> ...]
"""

import os
import re
import shutil
import subprocess
import sys

# The glibc core, provided by the rootfs. Copied files are matched by their
# soname, so these are prefixes.
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

DEFAULT_SEARCH = (
    "/usr/lib/aarch64-linux-gnu",
    "/lib/aarch64-linux-gnu",
    "/usr/lib/arm-linux-gnueabihf",
    "/lib/arm-linux-gnueabihf",
    "/usr/lib/x86_64-linux-gnu",
    "/lib/x86_64-linux-gnu",
    "/usr/lib64",
    "/lib64",
    "/usr/lib",
    "/lib",
)


def needed(path, readelf):
    """The DT_NEEDED sonames of an ELF file."""
    out = subprocess.run(
        [readelf, "-d", path], capture_output=True, text=True, check=False
    ).stdout
    return re.findall(r"\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]", out)


def find(soname, search_dirs, gcc_cross):
    for d in search_dirs:
        candidate = os.path.join(d, soname)
        if os.path.exists(candidate):
            return candidate
    # The cross toolchain's own runtime dir (libgcc_s lives here).
    for d in gcc_cross:
        candidate = os.path.join(d, soname)
        if os.path.exists(candidate):
            return candidate
    return None


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    binary, out_dir = sys.argv[1], sys.argv[2]
    search = list(sys.argv[3:]) or list(DEFAULT_SEARCH)

    readelf = os.environ.get("CROSS_READELF", "aarch64-linux-gnu-readelf")
    gcc_cross = []
    try:
        base = "/usr/lib/gcc-cross/aarch64-linux-gnu"
        for v in os.listdir(base):
            gcc_cross.append(os.path.join(base, v))
    except OSError:
        pass

    os.makedirs(out_dir, exist_ok=True)

    seen = set()
    queue = [binary]
    copied = 0
    while queue:
        path = queue.pop()
        for soname in needed(path, readelf):
            if soname in seen:
                continue
            seen.add(soname)
            if soname.startswith(SKIP):
                continue
            found = find(soname, search, gcc_cross)
            if not found:
                # Not fatal by itself, but a missing library is a process that
                # will not start on the device, so it is reported loudly.
                print(f"libclosure: MISSING {soname} (needed by {os.path.basename(path)})",
                      file=sys.stderr)
                continue
            dest = os.path.join(out_dir, soname)
            shutil.copy2(os.path.realpath(found), dest)  # follow the soname symlink
            copied += 1
            queue.append(dest)

    print(f"libclosure: copied {copied} libraries into {out_dir}")


if __name__ == "__main__":
    main()
