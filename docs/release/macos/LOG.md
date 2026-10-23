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

---

## 2026-10-23 15:04 IST — Phase 1: macOS baseline (branch `release/v0.1-macos`)

Everything below ran in this session on the build host; raw logs are under
`build/packaging/phase1-*.log` (gitignored). Host/toolchain/Qt pins are in
`environment-macos.txt`.

**1. Merge of the `mac` branch (ADR 0008)**
- `git merge --no-ff mac` → "Merge made by the 'ort' strategy", no conflicts;
  `app/src/backend_manager.{h,cpp}` +54/−22. Commit `30ec89a`.
- `git merge-base --is-ancestor mac HEAD` → true.

**2. Dev regression venv** (`uv venv --python 3.12 .venv-macos`; `uv pip install -e backend`)
- Python 3.12.13 (uv's latest 3.12; the shipped runtime will pin 3.12.12, confirmed
  downloadable for macos-aarch64 via `uv python list 3.12.12`).
- Install succeeded in ~111 s wall. **Versions are unpinned** (this is a baseline venv,
  not the shipped runtime): fastapi 0.161.1, pydantic 2.14.1, huggingface-hub 2.3.0,
  numpy 2.5.4, uvicorn 0.54.0, llama-cpp-python **0.3.36** (pinned set has 0.3.35).
  Full list: `environment-macos.txt`.
- `llama-cpp-python` compiled from sdist with **no flags and no Metal compiler**; it
  reports `llama_supports_gpu_offload() == True` and "using embedded metal library"
  on the M1; 34 shader libraries loaded in 10.195 s (startup cost → ISSUE-002 update).
- Added `pypdf==6.19.0` (an `[ml]` extra) to remove the only skipped test.

**3. Offline regression suite**
| Run | Command | Result |
|---|---|---|
| a | bare `PYTHONPATH=backend/src .venv-macos/bin/python -m unittest discover -s backend/tests -v` | **53 tests: 4 FAIL, 1 ERROR, 1 skipped (pypdf)** |
| b | same, `TMPDIR` = symlink-resolved `/private/var/folders/…/T` | 53 tests, OK, 1 skipped (pypdf) |
| c | b + `pypdf==6.19.0` installed | **53 tests, OK, 0 skipped** |
| d | `bash packaging/macos/run_regressions.sh` (new wrapper) | **53 tests, OK (exit 0)** |

Root cause of run a: macOS `/var` → `/private/var` symlink vs resolved backend paths.
Run b is the controlled experiment that isolates it. Full analysis: **ISSUE-005**
(reported, source not changed). Linux had 41 tests at its build; `dev` now has 53.
Caveat: run on unpinned huggingface-hub 2.3.0, not the shipped 1.30.0.

**4. CMake build** — `build-macos/` (new dir; `build/` and the AGENTS.md workflow untouched)
- Configure: `-DCMAKE_PREFIX_PATH=$MAGNITE_QT_ROOT -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
  -DMAGNITE_BUILD_LABEL=0.1.0-test.1` → exit 0, Qt 6.8.3 found.
- Build `-j4` → exit 0 in 17.4 s wall; 4.8 MB binary. 5 compiler warnings, all
  `-Wunused-lambda-capture` in `app/src/mainwindow.cpp` (lines 454, 777, 878, 1088, 1163),
  pre-existing and not macOS-specific; not touched.
- `file` → Mach-O 64-bit executable **arm64**.
- `otool -l` → `LC_BUILD_VERSION` **`minos 14.0`**, `sdk 13.3` (SDK is older than the
  target; that is permitted and the SDK is the newest installed).
  The 2026-10-04 binary had inherited `minos 13.3`; the deployment target is now explicit (R2).

**5. Desktop checks**
- `QT_QPA_PLATFORM=offscreen ./build-macos/magnite-app --theme-check` → **`0 failure(s)`**.
- `--selftest` (disposable `MAGNITE_HOME`, offscreen, dev layout → repo `.venv`):
  **`[selftest] PASS (0 failures)`**; log contains
  `[magnite] backend health OK (http://127.0.0.1:49375)` after ~6 "Backend health check
  failed - retrying" lines (normal ~13 s backend startup). `pgrep -fl backend.main` after
  exit → none, i.e. clean shutdown. This is the direct observable for the merged `mac` fix.
  Scope: dev-layout only; the packaged runtime is not exercised until Phase 4.
- Note: macOS has no `timeout(1)`; use a shell-level guard in future scripts.

**6. Disk** — `build-macos` 35 MB, `.venv-macos` 150 MB, `build/packaging/uv-cache` 413 MB;
23 GiB free on the data volume (89% used) after the phase. ISSUE-004 stays open.

**New files:** `packaging/macos/run_regressions.sh` (verified: exit 0, 53 OK),
`docs/release/macos/environment-macos.txt`.

**Shared-file edits this phase:** none. (The `mac` merge is a branch merge of an
existing commit, covered by ADR 0008.) No re-verification after a shared edit was
therefore required.

**Phase 1 acceptance:** regressions pass (53/53 via the wrapper, bare-run caveat
documented) ✔ · `--theme-check` 0 failure(s) ✔ · `minos 14.0` ✔ · versions pinned in
`LOG.md` + `environment-macos.txt` ✔ · `build-macos/` gitignored ✔.

**Not claimed:** anything about the packaged runtime, `.app` bundle, pinned dependency
set, macOS 14/15 behaviour, MPS, or training/RAG/adapter paths.

---

## 2026-10-23 15:12 IST — Phase 2: bundle-aware CMake (shared file `app/CMakeLists.txt`)

Logs: `build/packaging/phase2-*.log` (gitignored).

**New files:** `packaging/macos/make_icon.sh`, `packaging/macos/Info.plist.in`.
**Shared-file edit:** `app/CMakeLists.txt` — additive; everything under
`if(APPLE AND MAGNITE_MACOS_BUNDLE)`, option default `OFF`; the original
`install(TARGETS magnite-app RUNTIME DESTINATION bin)` is kept verbatim in the `else()` branch.

**1. Icon** — `bash packaging/macos/make_icon.sh` → `build/packaging/macos/magnite.icns`, 267,678 bytes, `file`:
"Mac OS X icon". Tokens read from `theme.h`: ink `#201411`, orange `#FF4A0E`, paper `#F1EFE7`.
Finding: `app/resources/brand/symbol.svg` uses `@brandInk`/`@brandOrange` placeholders, so `qlmanage` on the raw file
renders a **solid black blob** (checked visually); the script substitutes them first. The 1024 px master has an alpha
channel and was inspected visually: orange symbol with ink dots on a rounded paper tile.

**2. Bundle build** (`-DMAGNITE_MACOS_BUNDLE=ON`, arm64, deployment target 14.0, label `0.1.0-test.1`):
configure 0, build 0 (links `Magnite.app/Contents/MacOS/Magnite`), `cmake --install` 0.
Installed tree: `Magnite.app/Contents/{Info.plist, MacOS/Magnite, Resources/magnite.icns}`; icon `cmp`-identical to source.
`plutil -lint` OK. `plutil -p`: `CFBundleIdentifier=com.magnite.magnite-llm`, `LSMinimumSystemVersion=14.0`,
`CFBundleShortVersionString=0.1.0`, `MagniteBuildLabel=0.1.0-test.1`, `NSHighResolutionCapable=true`,
`CFBundleExecutable=Magnite`, `CFBundleIconFile=magnite.icns`. Binary: Mach-O arm64, `minos 14.0`.

**3. Theme check** — `build-macos/Magnite.app/Contents/MacOS/Magnite --theme-check` → **`0 failure(s)`**.
**The installed bundle does not launch yet**: dyld cannot find `QtWidgets.framework`. Mechanism verified with
`otool -l`: the build-tree binary has `LC_RPATH` → the developer Qt; the installed binary has none (`cmake --install`
strips it, which is correct), and there is no `Contents/Frameworks` until `macdeployqt` (Phase 5). The plan's
acceptance text asked the installed bundle to run `--theme-check`; that was unachievable by design, so it was
**corrected in PLAN.md** and the installed-bundle launch check moved to Phase 5. Not a defect, but not hidden either.

**4. Non-bundle regression** (the point of the opt-in design):
- `cmake -S app -B build ...` + `cmake --build build -j4` → 0/0; cache shows `MAGNITE_MACOS_BUNDLE:BOOL=OFF`;
  `./build/magnite-app --theme-check` → `0 failure(s)`.
- Fresh-cache default configure in a throwaway dir + build + install → only `bin/magnite-app` installed, i.e. the
  layout the Linux AppImage recipe relies on is unchanged.

**5. Offline regressions** after the shared edit: `bash packaging/macos/run_regressions.sh` → **53 tests, OK** (exit 0).

**Deviations from the plan, all in `DECISIONS.md`:** executable is `Magnite` in bundle mode (needed for `Magnite.app`);
`CFBundleShortVersionString` is `0.1.0` rather than the label (label is not a valid short-version; kept in a custom
key); no cairosvg fallback written.

**Not verified / not claimed:** that the bundle launches (needs Phase 5); Windows/Linux CMake behaviour (the
non-bundle path was checked on macOS only — the guard is `if(APPLE AND ...)`, so it is unreachable elsewhere, but
that is reasoning, not a Linux/Windows run).
