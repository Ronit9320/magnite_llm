"""Gate for a finished Magnite.app: fail on anything that would break on another Mac.

Usage: python3 packaging/macos/verify_bundle.py build/packaging/macos/stage/Magnite.app

Checks (each prints PASS/FAIL; exit 1 on any FAIL):
  layout      Info.plist, executable, LSMinimumSystemVersion, bundled python3, Qt frameworks/plugins
  arch        every Mach-O is exactly arm64 (ADR 0001)
  minos       no Mach-O requires macOS newer than 14.0 (ADR 0002)
  linkage     dylib references are @rpath/@loader_path/@executable_path or system paths; rpaths are relative
  resolve     every non-weak @rpath/@loader_path/@executable_path dependency exists inside the bundle
  host-paths  no file (text or binary) mentions this build host's home, the checkout, or the Qt install
  symlinks    no absolute symlinks and none that escape the bundle
  signature   codesign --verify --deep --strict
Grown from scan_macho.py (Phase 3). Runs on the host's python3 (3.9+); stdlib only.
"""
from __future__ import annotations

import os
import plistlib
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

MIN_MACOS = (14, 0)
ALLOWED_DYLIB_PREFIXES = ("@rpath/", "@loader_path/", "@executable_path/", "/usr/lib/", "/System/Library/")
_THIN_MAGIC = {b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe"}
_FAT_MAGIC = {b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf"}


def is_macho(path: Path) -> bool:
    try:
        with open(path, "rb") as f:
            head = f.read(8)
    except OSError:
        return False
    if head[:4] in _THIN_MAGIC:
        return True
    # 0xcafebabe is also a Java class file; a fat header has a small arch count instead of a version.
    return head[:4] in _FAT_MAGIC and 0 < int.from_bytes(head[4:8], "big") < 32


def iter_files(root: Path):
    """Regular files under root, never following or yielding symlinks."""
    for dirpath, dirnames, filenames in os.walk(root):
        for name in filenames:
            path = Path(dirpath, name)
            if not path.is_symlink() and path.is_file():
                yield path


def macho_files(root: Path) -> list:
    with ThreadPoolExecutor(8) as ex:
        files = list(iter_files(root))
        return [f for f, ok in zip(files, ex.map(is_macho, files)) if ok]


def archs(path: Path) -> list:
    out = subprocess.run(["lipo", "-archs", str(path)], capture_output=True, text=True).stdout
    return out.split()


def load_commands(path: Path) -> dict:
    """minos, dylib references and rpaths from otool -l (arm64 slice)."""
    out = subprocess.run(["otool", "-arch", "arm64", "-l", str(path)], capture_output=True, text=True).stdout
    info = {"minos": None, "dylibs": [], "weak": [], "rpaths": []}
    for block in out.split("Load command ")[1:]:
        cmd = re.search(r"cmd (\S+)", block)
        cmd = cmd.group(1) if cmd else ""
        if cmd == "LC_BUILD_VERSION":
            m = re.search(r"minos (\S+)", block)
            info["minos"] = m and m.group(1)
        elif cmd == "LC_VERSION_MIN_MACOSX" and info["minos"] is None:
            m = re.search(r"version (\S+)", block)
            info["minos"] = m and m.group(1)
        elif cmd in ("LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB", "LC_LAZY_LOAD_DYLIB"):
            name = re.search(r"name (.+?) \(offset", block).group(1)
            info["dylibs"].append(name)
            if cmd == "LC_LOAD_WEAK_DYLIB":
                info["weak"].append(name)
        elif cmd == "LC_RPATH":
            info["rpaths"].append(re.search(r"path (.+?) \(offset", block).group(1))
    return info


def host_executable(app: Path, path: Path) -> Path:
    """The executable whose @executable_path and rpaths apply when `path` is loaded."""
    runtime = app / "Contents/Resources/runtime/python"
    if runtime in path.parents:
        return runtime / "bin/python3.12"
    plist = plistlib.loads((app / "Contents/Info.plist").read_bytes())
    return app / "Contents/MacOS" / plist["CFBundleExecutable"]


def unresolved(app: Path, path: Path, info: dict, exe_rpaths: list, drop_absolute_rpaths=False) -> list:
    """Non-weak relative dependencies of `path` that dyld could not find inside the bundle.

    Searches the image's own rpaths, then its executable's (an approximation of dyld's
    loader chain that is exact for this bundle's layout)."""
    exe = host_executable(app, path)
    own = [r for r in info["rpaths"] if not (drop_absolute_rpaths and not r.startswith("@"))]

    def expand(ref):
        if ref.startswith("@loader_path"):
            return Path(str(path.parent) + ref[len("@loader_path"):])
        if ref.startswith("@executable_path"):
            return Path(str(exe.parent) + ref[len("@executable_path"):])
        return Path(ref)

    missing = []
    for dep in info["dylibs"]:
        if dep in info["weak"] or not dep.startswith("@"):
            continue
        if dep.startswith("@rpath/"):
            tail = dep[len("@rpath/"):]
            found = any((expand(r) / tail).exists() for r in own + exe_rpaths)
        else:
            found = expand(dep).exists()
        if not found:
            missing.append(dep)
    return missing


def version_tuple(text):
    try:
        return tuple(int(x) for x in text.split("."))
    except (AttributeError, ValueError):
        return None


class Report:
    def __init__(self):
        self.failures = 0

    def check(self, name, problems, detail=""):
        if problems:
            self.failures += 1
            print(f"FAIL  {name}: {len(problems)} problem(s)")
            for p in problems[:25]:
                print(f"        {p}")
            if len(problems) > 25:
                print(f"        ... {len(problems) - 25} more")
        else:
            print(f"PASS  {name}{': ' + detail if detail else ''}")


def check_layout(app: Path) -> list:
    problems = []
    contents = app / "Contents"
    try:
        plist = plistlib.loads((contents / "Info.plist").read_bytes())
    except (OSError, plistlib.InvalidFileException) as exc:
        return [f"Info.plist unreadable: {exc}"]
    exe = contents / "MacOS" / plist.get("CFBundleExecutable", "")
    if not (exe.is_file() and os.access(exe, os.X_OK)):
        problems.append(f"executable missing: {exe}")
    if plist.get("LSMinimumSystemVersion") != "%d.%d" % MIN_MACOS:
        problems.append(f"LSMinimumSystemVersion is {plist.get('LSMinimumSystemVersion')!r}")
    python = contents / "Resources/runtime/python/bin/python3"
    if not (python.exists() and os.access(python, os.X_OK)):
        problems.append(f"bundled interpreter missing: {python}")
    for rel in ("Frameworks/QtCore.framework", "Frameworks/QtWidgets.framework", "Frameworks/QtSvg.framework",
                "PlugIns/platforms/libqcocoa.dylib", "PlugIns/platforms/libqoffscreen.dylib",
                "PlugIns/imageformats/libqsvg.dylib", "Resources/qt.conf"):
        if not (contents / rel).exists():
            problems.append(f"missing {rel}")
    return problems


def check_host_paths(app: Path, needles: list) -> list:
    encoded = [n.encode() for n in needles]
    hits = []
    for path in iter_files(app):
        try:
            data = path.read_bytes()
        except OSError as exc:
            hits.append(f"unreadable {path}: {exc}")
            continue
        found = [n.decode() for n in encoded if n in data]
        if found:
            hits.append(f"{path.relative_to(app)}  ({', '.join(found)})")
    return hits


def check_symlinks(app: Path) -> list:
    problems = []
    root = app.resolve()
    for dirpath, dirnames, filenames in os.walk(app):
        for name in dirnames + filenames:
            path = Path(dirpath, name)
            if not path.is_symlink():
                continue
            target = os.readlink(path)
            if os.path.isabs(target):
                problems.append(f"absolute symlink {path.relative_to(app)} -> {target}")
                continue
            resolved = (path.parent / target).resolve()
            if root not in resolved.parents and resolved != root:
                problems.append(f"escaping symlink {path.relative_to(app)} -> {target}")
            elif not resolved.exists():
                problems.append(f"dangling symlink {path.relative_to(app)} -> {target}")
    return problems


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    app = Path(sys.argv[1]).resolve()
    if not (app / "Contents/Info.plist").is_file():
        sys.exit(f"not an app bundle: {app}")
    report = Report()
    report.check("layout", check_layout(app))

    machos = macho_files(app)
    with ThreadPoolExecutor(8) as ex:
        arch_list = list(ex.map(archs, machos))
        commands = list(ex.map(load_commands, machos))
    rel = [m.relative_to(app) for m in machos]
    report.check("arch", [f"{r}: {' '.join(a) or '?'}" for r, a in zip(rel, arch_list) if a != ["arm64"]],
                 f"{len(machos)} Mach-O files, all arm64")
    minos_problems, histogram = [], {}
    for r, c in zip(rel, commands):
        histogram[c["minos"]] = histogram.get(c["minos"], 0) + 1
        v = version_tuple(c["minos"])
        if v is None or v > MIN_MACOS:
            minos_problems.append(f"{r}: minos {c['minos']}")
    ordered = sorted(histogram.items(), key=lambda kv: version_tuple(kv[0]) or (99,))
    report.check("minos", minos_problems, "  ".join(f"{k}x{v}" for k, v in ordered))
    linkage = []
    for r, c in zip(rel, commands):
        linkage += [f"{r}: links {d}" for d in c["dylibs"] if not d.startswith(ALLOWED_DYLIB_PREFIXES)]
        linkage += [f"{r}: rpath {p}" for p in c["rpaths"] if not p.startswith("@")]
    report.check("linkage", linkage, "only relative and system references")
    exe_rpaths = {}
    resolve_problems = []
    for m, r, c in zip(machos, rel, commands):
        exe = host_executable(app, m)
        if exe not in exe_rpaths:
            exe_rpaths[exe] = load_commands(exe)["rpaths"] if exe.exists() else []
        resolve_problems += [f"{r}: cannot resolve {d}" for d in unresolved(app, m, c, exe_rpaths[exe])]
    report.check("resolve", resolve_problems, "every relative dependency found in the bundle")

    needles = sorted({str(Path.home()), str(Path(__file__).resolve().parents[2])}
                     | ({os.environ["MAGNITE_QT_ROOT"]} if os.environ.get("MAGNITE_QT_ROOT") else set()))
    report.check("host-paths", check_host_paths(app, needles), "none of " + ", ".join(needles))
    report.check("symlinks", check_symlinks(app))

    sig = subprocess.run(["codesign", "--verify", "--deep", "--strict", "--verbose=2", str(app)],
                         capture_output=True, text=True)
    report.check("signature", [] if sig.returncode == 0 else sig.stderr.strip().splitlines(),
                 "codesign --verify --deep --strict")

    print(f"{report.failures} failure(s)")
    sys.exit(1 if report.failures else 0)


if __name__ == "__main__":
    main()
