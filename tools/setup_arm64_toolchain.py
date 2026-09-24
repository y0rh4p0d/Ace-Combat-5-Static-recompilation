"""Set up the toolchains needed to build ac5 for Windows x64 and Windows arm64.

An x64 build only needs MSYS2 UCRT64 (see README step 2).  An arm64 build needs
two more things, and this script fetches both:

  * an **x86_64 clang** to act as the cross-compiler.  The AArch64 clang in MSYS2
    is itself an AArch64 binary and cannot run on an x64 host, so the driver has
    to come from the clang64 environment.  It lands in a private directory
    (``-HostTools``) together with the DLLs it needs, so nothing else on the
    machine is touched.
  * an **aarch64-w64-mingw32 sysroot**: mingw-w64 headers, the CRT, winpthreads,
    SDL3 and the Vulkan headers/loader.  These come from the clangarm64
    environment.

Usage::

    python tools/setup_arm64_toolchain.py                  # defaults
    python tools/setup_arm64_toolchain.py --check          # verify what is there
    python tools/setup_arm64_toolchain.py --host-tools D:\\tools\\clang
    python tools/setup_arm64_toolchain.py --sysroot C:\\msys64\\clangarm64

The mirror can be changed with ``--mirror``; the default is the Tsinghua MSYS2
mirror, with the official repo used as a fallback.
"""

from __future__ import annotations

import argparse
import io
import os
import re
import shutil
import sys
import tarfile
import urllib.parse
import urllib.request
from pathlib import Path

try:
    from compression import zstd
except ImportError:  # pragma: no cover - Python < 3.14
    try:
        import zstandard as zstd  # type: ignore
    except ImportError:
        sys.exit("need Python 3.14+ (compression.zstd) or the zstandard module")


DEFAULT_MIRROR = "https://mirrors.tuna.tsinghua.edu.cn/msys2/mingw"
FALLBACK_MIRROR = "https://repo.msys2.org/mingw"

# x86_64 clang driver plus everything its DLLs link against.  ld.lld is here
# because the aarch64 sysroot ships no `ld` (and a GNU ld built for aarch64 could
# not run on an x64 host); llvm-tools supplies llvm-ar/llvm-ranlib for the same
# reason; ninja is the build tool.
HOST_PACKAGES = [
    "clang", "clang-libs", "llvm-libs", "lld", "compiler-rt", "libc++",
    "libunwind", "crt", "headers", "libwinpthread", "winpthreads",
    "zlib", "zstd", "libiconv", "libffi", "libxml2", "ninja", "pkgconf",
]

# Matched separately because the resolver refuses "-tools" suffixes to avoid
# pulling in unrelated helper packages.
HOST_TOOLS_PACKAGE = "mingw-w64-clang-x86_64-llvm-tools"

# AArch64 target: headers, CRT, threads runtime, and the libraries ac5 links.
TARGET_PACKAGES = [
    "crt", "headers", "libwinpthread", "winpthreads", "libmangle", "libssp",
    "libc++", "libunwind", "compiler-rt", "zlib", "zstd", "libiconv",
    "sdl3", "vulkan-headers", "vulkan-loader", "windows-default-manifest",
]

# The -stub package ships an empty include/pthread_time.h and overwrites the real
# header, which then hides clock_gettime/nanosleep.  Never install it.
EXCLUDED_SUFFIXES = ("-stub", "-debug", "-docs", "-dbg")


def _decompress(raw: bytes) -> bytes:
    if hasattr(zstd, "decompress"):
        return zstd.decompress(raw)
    d = zstd.ZstdDecompressor()
    return d.decompress(raw) if hasattr(d, "decompress") else d.stream_reader(io.BytesIO(raw)).read()


class Fetcher:
    def __init__(self, cache: Path, mirror: str, fallback: str, quiet=False):
        self.cache = cache
        self.mirrors = [mirror, fallback]
        self.quiet = quiet

    def fetch(self, repo: str, filename: str) -> Path:
        dest = self.cache / filename
        if dest.exists() and dest.stat().st_size:
            return dest
        dest.parent.mkdir(parents=True, exist_ok=True)
        tmp = dest.with_name(dest.name + ".part")
        last = None
        for base in self.mirrors:
            url = "%s/%s/%s" % (base, repo, urllib.parse.quote(filename))
            try:
                if not self.quiet:
                    print("    downloading %s" % filename, flush=True)
                req = urllib.request.Request(url, headers={"User-Agent": "ac5-setup"})
                with urllib.request.urlopen(req, timeout=600) as r, open(tmp, "wb") as fp:
                    shutil.copyfileobj(r, fp, 1 << 20)
                tmp.replace(dest)
                return dest
            except Exception as e:                      # noqa: BLE001
                last = e
        raise SystemExit("could not download %s: %s" % (filename, last))

    def database(self, repo: str) -> dict:
        """pkgname -> (version, filename) from the repository's package database."""
        db = self.fetch(repo, "%s.db.tar.zst" % repo)
        data = _decompress(db.read_bytes())
        out = {}
        with tarfile.open(fileobj=io.BytesIO(data)) as tf:
            for m in tf.getmembers():
                if not m.isfile() or not m.name.endswith("/desc"):
                    continue
                txt = tf.extractfile(m).read().decode("utf-8", "replace")
                fields = dict(re.findall(
                    r"%([A-Z0-9]+)%\n(.*?)(?=\n%[A-Z0-9]+%\n|\Z)", txt, re.S))
                name = fields.get("NAME", "").strip()
                fn = fields.get("FILENAME", "").strip()
                if name and fn:
                    out[name] = (fields.get("VERSION", "?").strip(), fn)
        return out

    def resolve(self, repo: str, prefix: str, wanted: list[str]) -> dict:
        db = self.database(repo)
        chosen = {}
        for name, meta in db.items():
            if name.endswith(EXCLUDED_SUFFIXES):
                continue
            for w in wanted:
                full = "%s-%s" % (prefix, w)
                if name == full or name.startswith(full + "-"):
                    chosen[name] = meta
                    break
        missing = [w for w in wanted
                   if not any(n == "%s-%s" % (prefix, w)
                              or n.startswith("%s-%s-" % (prefix, w))
                              for n in chosen)]
        if missing:
            print("    warning: no package for %s" % ", ".join(missing))
        return chosen


def _clear_readonly(path: Path) -> None:
    try:
        if path.exists():
            os.chmod(path, 0o666)
    except OSError:
        pass


def extract(pkg: Path, root: Path, prefix: str) -> None:
    """Unpack an MSYS2 package, stripping its top-level repo directory."""
    data = _decompress(pkg.read_bytes())
    with tarfile.open(fileobj=io.BytesIO(data)) as tf:
        for m in tf.getmembers():
            if m.name.startswith("."):
                continue
            rel = m.name
            if rel.startswith(prefix + "/"):
                rel = rel[len(prefix) + 1:]
            elif rel == prefix:
                continue
            if not rel:
                continue
            target = root / rel
            if m.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif m.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                _clear_readonly(target)
                with tf.extractfile(m) as src, open(target, "wb") as dst:
                    shutil.copyfileobj(src, dst)
                try:
                    os.chmod(target, 0o755 if m.mode & 0o111 else 0o644)
                except OSError:
                    pass
            elif m.issym() or m.islnk():
                # These carry the clang driver aliases (clang.exe, clang++.exe,
                # cc.exe).  Windows will not create symlinks without a privilege
                # and tar hard links look the same here, so copy the target.
                target.parent.mkdir(parents=True, exist_ok=True)
                link = m.linkname
                if link.startswith(prefix + "/"):
                    link = link[len(prefix) + 1:]
                src = root / link.lstrip("/")
                if target.exists() or target.is_symlink():
                    try:
                        _clear_readonly(target)
                        target.unlink()
                    except OSError:
                        pass
                if m.issym():
                    try:
                        os.symlink(m.linkname, target)
                        continue
                    except OSError:
                        pass
                if src.is_file():
                    shutil.copy(src, target)
                    try:
                        os.chmod(target, 0o755)
                    except OSError:
                        pass


def install(fetcher: Fetcher, repo: str, prefix: str, root: Path,
            wanted: list[str]) -> int:
    chosen = fetcher.resolve(repo, prefix, wanted)
    if not chosen:
        sys.exit("no packages found for %s in %s" % (", ".join(wanted), repo))
    print("  %d packages" % len(chosen))
    for name, (_ver, fn) in sorted(chosen.items()):
        if not fetcher.quiet:
            print("    %s" % name, flush=True)
        extract(fetcher.fetch(repo, fn), root, prefix)
    return len(chosen)


def check(host_tools: Path, sysroot: Path) -> int:
    problems = []
    clang = host_tools / "bin" / "clang.exe"
    if not clang.exists():
        problems.append("no clang driver at %s" % clang)
    ld = host_tools / "bin" / "ld.lld.exe"
    if not ld.exists():
        problems.append("no linker at %s" % ld)
    ninja = host_tools / "bin" / "ninja.exe"
    if not ninja.exists():
        problems.append("no ninja at %s" % ninja)
    if not (host_tools / "bin" / "llvm-ar.exe").exists() \
            and not (host_tools / "bin" / "ar.exe").exists():
        problems.append("no archiver (llvm-ar) in %s/bin; static libraries "
                        "cannot be created" % host_tools)
    for rel in ("include/stdio.h", "include/windows.h", "include/vulkan/vulkan.h",
                "include/SDL3/SDL.h", "lib/libvulkan-1.dll.a",
                "lib/libSDL3.dll.a", "lib/libwinpthread.a"):
        if not (sysroot / rel).exists():
            problems.append("sysroot is missing %s" % rel)
    tth = sysroot / "include" / "pthread_time.h"
    if tth.exists() and tth.stat().st_size == 0:
        problems.append("sysroot include/pthread_time.h is empty; the "
                        "winpthreads-stub package overwrote the real header")
    builtins = list((sysroot / "lib" / "clang").glob(
        "*/lib/windows/libclang_rt.builtins-aarch64.a"))
    if not builtins:
        problems.append("no compiler-rt builtins for aarch64 in the sysroot")
    if problems:
        print("toolchain problems:")
        for p in problems:
            print("  - %s" % p)
        return 1
    print("toolchain looks complete:")
    print("  driver : %s" % clang)
    print("  sysroot: %s" % sysroot)
    print("  builtins: %s" % builtins[0])
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="setup_arm64_toolchain",
        description="Fetch the clang cross-compiler and AArch64 sysroot for arm64 builds.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    here = Path(__file__).resolve().parent
    ap.add_argument("--host-tools", type=Path,
                    default=here.parent / "work" / "host-tools",
                    help="where to put the x86_64 clang driver (default: work/host-tools)")
    ap.add_argument("--sysroot", type=Path,
                    default=Path(r"C:\msys64\clangarm64"),
                    help="where to put the aarch64 sysroot (default: C:\\msys64\\clangarm64)")
    ap.add_argument("--cache", type=Path,
                    default=here.parent / "work" / "pkgcache",
                    help="downloaded package cache")
    ap.add_argument("--mirror", default=DEFAULT_MIRROR)
    ap.add_argument("--fallback-mirror", default=FALLBACK_MIRROR)
    ap.add_argument("--check", action="store_true",
                    help="only report what is already installed")
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args(argv)

    if args.check:
        return check(args.host_tools, args.sysroot)

    fetcher = Fetcher(args.cache, args.mirror, args.fallback_mirror, args.quiet)
    print("== x86_64 clang driver -> %s" % args.host_tools)
    install(fetcher, "clang64", "mingw-w64-clang-x86_64",
            args.host_tools, HOST_PACKAGES)
    # llvm-tools by exact name: it carries llvm-ar and llvm-ranlib, which the
    # sysroot has no equivalent of.
    print("== archiver (%s)" % HOST_TOOLS_PACKAGE)
    install(fetcher, "clang64", "mingw-w64-clang-x86_64",
            args.host_tools, ["llvm-tools"])
    print("== aarch64 sysroot -> %s" % args.sysroot)
    install(fetcher, "clangarm64", "mingw-w64-clang-aarch64",
            args.sysroot, TARGET_PACKAGES)
    print()
    return check(args.host_tools, args.sysroot)


if __name__ == "__main__":
    sys.exit(main())
