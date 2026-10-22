# LOG — macOS v0.1 packaging (append-only)

Newest entries at the bottom. Every entry: timestamp, what ran, the result, and
the evidence. Nothing is recorded as verified unless it ran in that session.

---

## 2026-10-23 03:43 IST — Phase 0: environment audit (read-only)

Host audit, all commands run in this session on the build machine.

| Item | Value | Command |
|---|---|---|
| macOS | 27.0.1 (build 26A434) | `sw_vers` |
| Kernel | Darwin 27.0.0 | `uname -a` |
| Architecture | arm64, Apple M1 | `uname -m`, `sysctl -n machdep.cpu.brand_string` |
| RAM | 8 GiB | `sysctl -n hw.memsize` |
| Free disk | ~20 GiB on /System/Volumes/Data (91% used) | `df -h` |
| Xcode | **not installed** (no /Applications/Xcode.app) | `ls /Applications` |
| Command Line Tools | 14.3.1.0.1.1683849156, `/Library/Developer/CommandLineTools` | `pkgutil --pkg-info=com.apple.pkg.CLTools_Executables` |
| Compiler | Apple clang 14.0.3 (clang-1403.0.22.14.1), target arm64-apple-darwin27.0.0 | `clang --version` |
| SDKs available | MacOSX.sdk, 12.3, 12, 13.1, 13.3, 13 — newest **13.3** | `ls /Library/Developer/CommandLineTools/SDKs`, `xcrun --show-sdk-version` |
| Metal compiler | **ABSENT** — "unable to find utility \"metal\"" | `xcrun -sdk macosx metal --version` |
| notarytool | 1.0.0 (28) — present | `xcrun notarytool --version` |
| Codesigning identities | **0 valid identities found** | `security find-identity -v -p codesigning` |
| CMake | 4.4.3 | `cmake --version` |
| uv | 0.11.4 (aarch64-apple-darwin) | `uv --version` |
| System python3 | 3.13.0 (/Library/Frameworks) | `python3 -V` |
| Repo .venv | Python 3.13.0 | `.venv/bin/python -V` |
| Homebrew | 6.0.22 at /opt/homebrew; **no qt formula installed** | `brew --version`, `ls /opt/homebrew/Cellar` |
| hdiutil / codesign / lipo / otool / install_name_tool / iconutil / qlmanage | all present in /usr/bin | `ls /usr/bin/...` |
| create-dmg | not installed (plan uses plain `hdiutil` instead) | `which create-dmg` |
| SVG rasterizers | no librsvg/imagemagick/inkscape; `qlmanage` + `iconutil` available | `ls /opt/homebrew/Cellar` |

Qt discovered (outside this repo, see `adr/0006-qt-provenance.md`):

| Item | Value | Command |
|---|---|---|
| Path | /Users/architbhuta/magnite_llm-master/.qt-tools/Qt/6.8.3/macos | `grep Qt6_DIR build/CMakeCache.txt` |
| Version | 6.8.3 | framework `current version 6.8.3` in `otool -L` |
| Slices | x86_64 + arm64 (universal) | `lipo -info lib/QtCore.framework/QtCore` |
| Deployment target | minos **12.0**, sdk 14.4 | `otool -l .../QtCore \| grep -A4 LC_BUILD_VERSION` |
| macdeployqt | present in `bin/` | `ls bin` |
| Size on disk | 4.8 GiB | `du -sh` |

Existing macOS build artifact (pre-existing, 2026-10-04 00:47):

- `build/magnite-app`: Mach-O 64-bit executable **arm64**, `minos 13.3`, `sdk 13.3`
  — i.e. the deployment target was inherited from the SDK, not chosen.
  `CMAKE_OSX_DEPLOYMENT_TARGET` and `CMAKE_OSX_ARCHITECTURES` are **empty** in
  `build/CMakeCache.txt`. Links Qt via `@rpath`, plus system AppKit/Metal/IOKit.
  Evidence: `file`, `otool -l`, `otool -L`, `grep CMAKE_OSX build/CMakeCache.txt`.
  => The Qt-on-macOS build path is already proven on this machine.

PyPI wheel availability for the pinned versions in `packaging/requirements-cpu.txt`
(queried via pypi.org JSON API in this session):

| Package | Pinned | macOS arm64 wheel | Note |
|---|---|---|---|
| torch | 2.14.0 | `torch-2.14.0-cp312-cp312-**macosx_14_0_arm64**.whl` | **sets the macOS 14.0 floor**; no sdist, no x86_64 macOS wheel |
| bitsandbytes | 0.50.2 | `bitsandbytes-0.50.2-py3-none-macosx_14_0_arm64.whl` | wheel exists; Apple-silicon 4/8-bit support still unproven |
| llama-cpp-python | 0.3.35 | none — **sdist only** | must compile; Metal flags needed (no shader compiler on host) |
| sentence-transformers | 6.0.1 | n/a (pure-python wheel) | fine |

Branch state:

- `git log --oneline dev..mac` → `7fcdb3c Probe backend health over a raw loopback socket`
  (2026-10-04), touching only `app/src/backend_manager.{h,cpp}` (+54/−22). **Not in
  `dev`.** Without it the macOS client never reaches its own backend. See
  `adr/0008-merge-mac-branch.md`.
- `release/v0.1-macos` created from `dev` at `0fa53e3`.

Documentation note: `RELEASE_CHECKLIST.md` in this repo (176 lines) has **no
"Base feature acceptance" section**. The acceptance set used by this plan is §5
*Establish release verification* plus the five `MAGNITE_SELFTEST_*` runs and
`backend/tests/live_workflow.py`.

**Not run in this session** (and therefore not claimed): the offline regression
suite on macOS, any CMake build, any selftest, any packaging step.

## 2026-10-23 03:43 IST — Phase 0: deliverables

- Created branch `release/v0.1-macos` from `dev`.
- Wrote `PLAN.md` (13 phases, 0–12), `STATUS.md`, `LOG.md`, `DECISIONS.md`,
  `ISSUES.md` and `adr/0001`–`adr/0008`.
- Created `packaging/macos/README.md` describing the scripts each later phase adds.
- Appended `build-macos/`, `.venv-macos/` and `*.dmg` to `.gitignore` so macOS
  build outputs and artifacts cannot be committed by accident (`dist/` and
  `build/` were already ignored). Additive, no existing rule changed.
- Phase 0 acceptance: `PLAN.md`, `STATUS.md`, `LOG.md`, `DECISIONS.md`, `ISSUES.md`
  and `adr/0001`–`adr/0008` present; the only files touched outside
  `docs/release/macos/` are `packaging/macos/README.md` (new) and `.gitignore`
  (three appended lines). No application source, no Linux packaging, no
  `RELEASE_CHECKLIST.md` change in this phase.
