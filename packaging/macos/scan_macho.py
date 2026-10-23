r"""List Mach-O files read from stdin with their minos and architectures; flag minos > 14.0 and non-arm64.
Usage: find <dir> -type f \( -name "*.so" -o -name "*.dylib" -o -perm -u+x \) | python3 -I packaging/macos/scan_macho.py
Phase 3 prototype of the check that packaging/macos/verify_bundle.py (Phase 5) will enforce."""
import subprocess, sys, re, collections
from concurrent.futures import ThreadPoolExecutor
files = [l for l in sys.stdin.read().splitlines() if l]
def info(f):
    try:
        out = subprocess.run(["otool", "-l", f], capture_output=True, text=True).stdout
    except Exception:
        return None
    if "LC_BUILD_VERSION" not in out and "LC_VERSION_MIN" not in out: return None
    m = re.search(r"LC_BUILD_VERSION.*?minos (\S+)", out, re.S) or re.search(r"LC_VERSION_MIN_MACOSX.*?version (\S+)", out, re.S)
    arch = subprocess.run(["lipo", "-archs", f], capture_output=True, text=True).stdout.strip()
    return f, (m.group(1) if m else "?"), arch
with ThreadPoolExecutor(8) as ex: res = [r for r in ex.map(info, files) if r]
key = lambda v: tuple(int(x) for x in v.split(".")) if re.fullmatch(r"[\d.]+", v) else (0,)
print("Mach-O files:", len(res))
print("archs:", dict(collections.Counter(a for _,_,a in res)))
print("minos histogram:", dict(sorted(collections.Counter(v for _,v,_ in res).items(), key=lambda kv: key(kv[0]))))
over = [(f,v) for f,v,_ in res if key(v) > (14,0)]
print("ABOVE 14.0:", len(over)); [print("  ", v, f) for f,v in over[:15]]
bad_arch = [(f,a) for f,_,a in res if a != "arm64"]
print("non-arm64:", len(bad_arch)); [print("  ", a, f) for f,a in bad_arch[:10]]
