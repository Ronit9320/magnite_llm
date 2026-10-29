"""Generate the Qt license notices shipped in Magnite.app (ISSUE-009).

Usage: python3 packaging/macos/qt_notices.py [--out packaging/macos/licenses/qt] [--cache build/packaging/qt-licenses-download]

Reads every qt_attribution.json of qtbase and qtsvg at the pinned Qt tag (network: GitHub API + raw files),
keeps the third-party components compiled into the Qt frameworks/plugins this bundle ships, and writes:
  <out>/README.md                      Qt licensing, where the Qt source is, and every component with its copyright
  <out>/LGPL-3.0-only.txt, GPL-*.txt, Qt-GPL-exception-1.0.txt   Qt's own license texts
  <out>/third-party/<Id>/<file>        each included component's own license file (as named in its attribution)
Every component must be classified below, so a Qt upgrade that adds one fails until someone reviews it.
The classification follows this Qt build's configuration (mkspecs/modules/qt_lib_*_private.pri). It is an
engineering inventory for a licensing review, not legal advice.
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
from pathlib import Path

QT_TAG = "v6.8.3"
REPOS = ("qtbase", "qtsvg")
QT_OWN_LICENSES = ("LGPL-3.0-only.txt", "GPL-2.0-only.txt", "GPL-3.0-only.txt", "Qt-GPL-exception-1.0.txt")

INCLUDED = {  # Id -> where it ends up in this bundle (evidence: QtUsage + the pinned build's feature flags)
    "doubleconversion": "QtCore (bundled: feature doubleconversion, not system-doubleconversion)",
    "pcre2": "QtCore (bundled: feature pcre2, not system-pcre2)",
    "pcre2-sljit": "QtCore (part of the bundled PCRE2)",
    "forkfd": "QtCore (QProcess on Unix)",
    "md4": "QtCore (QCryptographicHash)", "md5": "QtCore (QCryptographicHash)", "sha1": "QtCore (QCryptographicHash)",
    "sha3_endian": "QtCore (QCryptographicHash)", "sha3_keccak": "QtCore (QCryptographicHash)",
    "blake2": "QtCore (QCryptographicHash)", "rfc6234": "QtCore (QCryptographicHash, QMessageAuthenticationCode)",
    "siphash": "QtCore (QHash)", "tinycbor": "QtCore (features cborstreamreader/writer)",
    "easing": "QtCore (feature easingcurve)", "qeventdispatcher_cf": "QtCore (macOS event dispatcher)",
    "tika-mimetypes": "QtCore (feature mimetype-database)",
    "unicode-character-database": "QtCore (Unicode tables)", "unicode-cldr": "QtCore (locale data)",
    "freetype": "QtGui (bundled: feature freetype, not system-freetype)",
    "freetype-zlib": "QtGui (part of the bundled FreeType)", "freetype-bdf": "QtGui (part of the bundled FreeType)",
    "freetype-pcf": "QtGui (part of the bundled FreeType)",
    "harfbuzz-ng": "QtGui (bundled: feature harfbuzz, not system-harfbuzz)",
    "libpng": "QtGui (bundled: feature png, not system-png)",
    "md4c": "QtGui (feature textmarkdownreader, not system-textmarkdownreader)",
    "grayraster": "QtGui (raster painting)", "smooth-scaling-algorithm": "QtGui (QImage::transformed)",
    "xserverhelper": "QtGui (QRegion)", "aglfn": "QtGui (PDF generator)",
    "icc-srgb-color-profile": "QtGui (PDF/A output)", "pixman": "QtGui (ARM NEON drawing helpers)",
    "libjpeg": "imageformats/libqjpeg plugin (bundled: feature jpeg, not system-jpeg)",
    "cocoa-platform-plugin": "platforms/libqcocoa plugin",
    "psl-data": "QtNetwork (feature publicsuffix-qt)", "libpsl": "QtNetwork (feature publicsuffix-qt)",
    "libdbus-1-headers": "QtDBus (deployed by macdeployqt; libdbus itself is not shipped)",
    "xsvg": "QtSvg",
}
EXCLUDED = {  # Id -> why it is not in this bundle
    "zlib": "macOS build uses the system zlib (feature system-zlib)",
    "sqlite": "QtSql is not shipped",
    "android-native-style": "Android only", "android-gradle-wrapper": "Android build tool",
    "dejayvu": "WebAssembly only", "wintab": "Windows only", "iaccessible2": "Windows only",
    "xcb-xinput": "X11/Linux only", "d3d12memoryallocator": "Windows Direct3D 12 only",
    "rhi-miniengine-d3d12-mipmap": "Windows Direct3D 12 only",
    "vulkanmemoryallocator": "Vulkan is disabled in this build", "vulkan-xml-spec": "Vulkan is disabled in this build",
    "opengl-headers": "headers used on Windows and Linux only", "opengl-es2-headers": "OpenGL ES is disabled in this build",
    "extra-cmake-modules": "CMake build tooling", "kwin": "CMake build tooling",
    "cmake-runcmake-test-modules": "test infrastructure", "catch2": "QtTest is not shipped", "cycle": "QtTest is not shipped",
    "linuxperf": "QtTest is not shipped (Linux only)", "valgrind": "QtTest is not shipped",
    "webgradients": "build-time generator input (util/gradientgen)",
}


def fetch(url: str) -> bytes:
    # curl uses the macOS trust store; python.org builds of Python often ship without CA certificates.
    return subprocess.run(["curl", "-fsSL", "--retry", "3", url], check=True, capture_output=True).stdout


def attributions(cache: Path):
    """Yield (repo, attribution path, component dict) for every qt_attribution.json in REPOS at QT_TAG."""
    for repo in REPOS:
        tree_file = cache / f"{repo}-tree.json"
        if not tree_file.exists():
            tree_file.write_bytes(fetch(f"https://api.github.com/repos/qt/{repo}/git/trees/{QT_TAG}?recursive=1"))
        tree = json.loads(tree_file.read_text())
        if tree.get("truncated"):
            raise SystemExit(f"{repo}: GitHub tree listing truncated; cannot be sure every attribution was seen")
        for entry in tree["tree"]:
            if not entry["path"].endswith("qt_attribution.json"):
                continue
            local = cache / "attr" / f"{repo}--{entry['path'].replace('/', '_')}"
            if not local.exists():
                local.parent.mkdir(parents=True, exist_ok=True)
                local.write_bytes(fetch(f"https://raw.githubusercontent.com/qt/{repo}/{QT_TAG}/{entry['path']}"))
            data = json.loads(local.read_text(), strict=False)   # some files carry raw control characters
            for component in data if isinstance(data, list) else [data]:
                yield repo, entry["path"], component


def license_files(component: dict) -> list:
    files = component.get("LicenseFiles") or component.get("LicenseFile") or []
    return [files] if isinstance(files, str) else list(files)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    root = Path(__file__).resolve().parents[2]
    ap.add_argument("--out", type=Path, default=root / "packaging/macos/licenses/qt")
    ap.add_argument("--cache", type=Path, default=root / "build/packaging/qt-licenses-download")
    args = ap.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)

    included, unknown, seen = [], [], set()
    for repo, path, c in attributions(args.cache):
        cid = c.get("Id", "")
        seen.add(cid)
        if cid in INCLUDED:
            included.append((repo, path, c))
        elif cid not in EXCLUDED:
            unknown.append(f"{repo}:{path} {cid}")
    if unknown:
        raise SystemExit("unclassified Qt components (add them to INCLUDED or EXCLUDED with a reason):\n  " + "\n  ".join(unknown))
    stale = sorted((set(INCLUDED) | set(EXCLUDED)) - seen)
    if stale:
        raise SystemExit(f"classified but not found at {QT_TAG} (typo or removed upstream): {stale}")

    if args.out.exists():
        shutil.rmtree(args.out)
    (args.out / "third-party").mkdir(parents=True)
    for name in QT_OWN_LICENSES:
        (args.out / name).write_bytes(fetch(f"https://raw.githubusercontent.com/qt/qtbase/{QT_TAG}/LICENSES/{name}"))

    rows = []
    for repo, path, c in sorted(included, key=lambda x: x[2]["Id"]):
        directory = path.rsplit("/", 1)[0]
        shipped = []
        for rel in license_files(c):
            target = args.out / "third-party" / c["Id"] / Path(rel).name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(fetch(f"https://raw.githubusercontent.com/qt/{repo}/{QT_TAG}/{directory}/{rel}"))
            shipped.append(f"third-party/{c['Id']}/{target.name}")
        if not shipped:   # licence stated only in the metadata: ship Qt's SPDX text for each id in the expression
            for token in c.get("LicenseId", "").replace("(", " ").replace(")", " ").split():
                if token in ("OR", "AND", "WITH"):
                    continue
                target = args.out / "third-party" / c["Id"] / f"{token}.txt"
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(fetch(f"https://raw.githubusercontent.com/qt/{repo}/{QT_TAG}/LICENSES/{token}.txt"))
                shipped.append(f"third-party/{c['Id']}/{target.name}")
        holders = c.get("Copyright", "")
        holders = "; ".join(holders) if isinstance(holders, list) else holders
        copyright_text = " ".join(str(holders).split())
        rows.append(f"| {c.get('Name', c['Id'])} | {c.get('Version', '')} | {c.get('LicenseId', '')} | "
                    f"{INCLUDED[c['Id']]} | {copyright_text.replace('|', '/')} | {', '.join(shipped) or '(none named)'} |")

    excluded_rows = [f"| {cid} | {why} |" for cid, why in sorted(EXCLUDED.items())]
    (args.out / "README.md").write_text(f"""# Qt {QT_TAG.lstrip('v')} in Magnite.app — license notices

Magnite links the Qt {QT_TAG.lstrip('v')} libraries dynamically. They ship unmodified, as separate frameworks in
`Contents/Frameworks` and plugins in `Contents/PlugIns`, from the official Qt for macOS binaries. Qt is used under the
**GNU Lesser General Public License v3** (`LGPL-3.0-only.txt`). Qt's alternative open-source licenses (`GPL-2.0-only.txt`,
`GPL-3.0-only.txt`, with `Qt-GPL-exception-1.0.txt`) are included because Qt's source headers name them.

- **Source code** of this exact Qt version: tag `{QT_TAG}` of the `qtbase` and `qtsvg` repositories at https://code.qt.io/ (mirrored at
  https://github.com/qt), or the source packages under https://download.qt.io/archive/qt/6.8/6.8.3/.
- **Replacing Qt:** the frameworks are dynamically linked through `@rpath`, so a user can substitute modified builds of the
  same Qt version in `Contents/Frameworks`. Re-sign the bundle afterwards (`codesign --force --deep -s - Magnite.app`).

## Third-party components inside the shipped Qt libraries

Generated by `packaging/macos/qt_notices.py` from Qt's own `qt_attribution.json` files at `{QT_TAG}`, filtered to what this build
compiles in. The fact that zlib comes from macOS, and that Vulkan, OpenGL ES and QtSql are absent, comes from the build's feature flags.

| Component | Version | License | Where | Copyright | License file |
|---|---|---|---|---|---|
""" + "\n".join(rows) + """

## Qt components not included in this bundle, and why

| Id | Reason |
|---|---|
""" + "\n".join(excluded_rows) + """

_This is an engineering inventory prepared for the release licensing review; it is not legal advice._
""", encoding="utf-8")
    print(f"wrote {args.out}: {len(QT_OWN_LICENSES)} Qt license texts, {len(rows)} third-party components, "
          f"{sum(1 for _ in (args.out / 'third-party').rglob('*') if _.is_file())} component license files; "
          f"{len(EXCLUDED)} components excluded with reasons")


if __name__ == "__main__":
    main()
