"""Make an assembled Magnite.app relocatable and ad-hoc signed (ISSUE-006, ADR 0005).

Usage: python3 packaging/macos/finalize_bundle.py build/packaging/macos/stage/Magnite.app

Order matters: every edit invalidates a signature, so signing is last.
  1. clean   drop console scripts with build-host shebangs; restore python-build-standalone's
             /install prefix in _sysconfigdata (uv rewrote it to the download dir); remove the
             backend's direct_url.json (it names the checkout); remove stray __pycache__; remove
             llama-cpp-python's CMake development install (site-packages/lib, include)
  2. relink  libpython's install id back to @rpath (uv made it absolute); delete absolute
             LC_RPATHs left by upstream wheel CI (/Users/runner/..., /opt/homebrew/...) after
             proving every dependency still resolves inside the bundle
  3. thin    lipo -thin arm64 every universal Mach-O (Qt ships x86_64+arm64; so do a few wheels)
  4. sign    ad-hoc: loose Mach-O files, then each framework, then the bundle (no --deep)
Idempotent: re-running on a finished bundle changes nothing but the signatures.
"""
from __future__ import annotations

import ast
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from verify_bundle import archs, host_executable, load_commands, macho_files, unresolved  # noqa: E402

KEEP_IN_BIN = {"python", "python3", "python3.12"}
PBS_PREFIX = "/install"


def clean_runtime(python_root: Path):
    bin_dir = python_root / "bin"
    removed = [p for p in bin_dir.iterdir() if p.name not in KEEP_IN_BIN]
    for p in removed:
        p.unlink()
    print(f"clean: removed {len(removed)} entries from runtime/python/bin, kept {sorted(KEEP_IN_BIN)}")

    for sysconfig in (python_root / "lib/python3.12").glob("_sysconfigdata_*.py"):
        text = sysconfig.read_text(encoding="utf-8")
        tree = ast.parse(text)
        values = next(ast.literal_eval(n.value) for n in tree.body
                      if isinstance(n, ast.Assign) and n.targets[0].id == "build_time_vars")
        prefix = values["prefix"]
        if prefix != PBS_PREFIX:
            sysconfig.write_text(text.replace(prefix, PBS_PREFIX), encoding="utf-8")
            print(f"clean: {sysconfig.name}: prefix {prefix} -> {PBS_PREFIX}")

    site = python_root / "lib/python3.12/site-packages"
    for direct_url in site.glob("*.dist-info/direct_url.json"):
        record = direct_url.parent / "RECORD"
        if record.exists():
            lines = record.read_text(encoding="utf-8").splitlines(keepends=True)
            record.write_text("".join(l for l in lines if not l.startswith(f"{direct_url.parent.name}/direct_url.json,")),
                              encoding="utf-8")
        direct_url.unlink()
        print(f"clean: removed {direct_url.relative_to(site)}")

    # llama_cpp loads its libraries from llama_cpp/lib; the wheel also carries llama.cpp's
    # CMake install (duplicate dylibs, headers, cmake/pkgconfig) at the site-packages root.
    for record in site.glob("llama_cpp_python-*.dist-info/RECORD"):
        owned = {line.split("/", 1)[0] for line in record.read_text(encoding="utf-8").splitlines()}
        for top in ("lib", "include"):
            if top in owned and (site / top).is_dir():
                shutil.rmtree(site / top)
                print(f"clean: removed site-packages/{top} (llama.cpp development install)")

    caches = list(python_root.rglob("__pycache__"))
    for cache in caches:
        shutil.rmtree(cache)
    if caches:
        print(f"clean: removed {len(caches)} __pycache__ directories")


def relink(app: Path, python_root: Path):
    libpython = python_root / "lib/libpython3.12.dylib"
    subprocess.run(["install_name_tool", "-id", "@rpath/libpython3.12.dylib", str(libpython)],
                   check=True, capture_output=True)
    print("relink: libpython3.12.dylib id -> @rpath/libpython3.12.dylib")

    exe_rpaths, deleted = {}, 0
    for path in macho_files(app):
        info = load_commands(path)
        absolute = [r for r in info["rpaths"] if not r.startswith("@")]
        if not absolute:
            continue
        exe = host_executable(app, path)
        if exe not in exe_rpaths:
            exe_rpaths[exe] = load_commands(exe)["rpaths"]
        before = unresolved(app, path, info, exe_rpaths[exe])
        after = unresolved(app, path, info, exe_rpaths[exe], drop_absolute_rpaths=True)
        if set(after) - set(before):
            sys.exit(f"relink: {path} needs an absolute rpath for {sorted(set(after) - set(before))}")
        for rpath in dict.fromkeys(absolute):   # a duplicated rpath is deleted one entry per call
            for _ in range(info["rpaths"].count(rpath)):
                subprocess.run(["install_name_tool", "-delete_rpath", rpath, str(path)],
                               check=True, capture_output=True)
                deleted += 1
    print(f"relink: deleted {deleted} absolute rpaths (every dependency still resolves)")


def thin(app: Path):
    thinned = 0
    for path in macho_files(app):
        a = archs(path)
        if a == ["arm64"]:
            continue
        if "arm64" not in a:
            sys.exit(f"thin: {path} has no arm64 slice ({a})")
        tmp = path.with_name(path.name + ".thin")
        subprocess.run(["lipo", str(path), "-thin", "arm64", "-output", str(tmp)], check=True)
        tmp.chmod(path.stat().st_mode)
        tmp.replace(path)
        thinned += 1
    print(f"thin: {thinned} universal Mach-O files reduced to arm64")


def codesign(target: Path):
    subprocess.run(["codesign", "--force", "--timestamp=none", "--sign", "-", str(target)],
                   check=True, capture_output=True, text=True)


def sign(app: Path):
    contents = app / "Contents"
    main_exe = contents / "MacOS"
    frameworks = sorted((contents / "Frameworks").glob("*.framework"))
    loose = [p for p in macho_files(app)
             if ".framework/" not in str(p.relative_to(app)) and p.parent != main_exe]
    with ThreadPoolExecutor(8) as ex:
        list(ex.map(codesign, loose))
    for framework in frameworks:
        codesign(framework)
    codesign(app)
    print(f"sign: ad-hoc signed {len(loose)} Mach-O files, {len(frameworks)} frameworks, then {app.name}")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    app = Path(sys.argv[1]).resolve()
    python_root = app / "Contents/Resources/runtime/python"
    if not python_root.is_dir():
        sys.exit(f"no runtime at {python_root}; stage it before finalizing")
    clean_runtime(python_root)
    relink(app, python_root)
    thin(app)
    sign(app)


if __name__ == "__main__":
    main()
