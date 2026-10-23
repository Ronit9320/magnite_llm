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

---

## 2026-10-23 15:36 IST — Phase 3: relocatable Python runtime + pinned dependencies

Logs: `build/packaging/phase3-*.log` (gitignored). Branch `release/v0.1-macos` at `540a7a4` + uncommitted Phase 3 edits.

**New files:** `packaging/macos/requirements-macos-arm64.txt`, `requirements-macos-quantization.txt`, `prepare_runtime.sh`,
`scan_macho.py`; `docs/release/macos/runtime-packages-macos.txt` (81-package list).
**Shared-file edits:** `packaging/prepare_runtime.py`, `packaging/record_build.py` (darwin-guarded; see `DECISIONS.md`).

**1. Linux behaviour unchanged (proved, not assumed).** Loaded the original (`git show HEAD:packaging/prepare_runtime.py`) and the patched
script, simulated `sys.platform == "linux"`, stubbed `run()`/`record_build`, fixed the destination path, and compared every command and the
env vars they would receive: **`linux: original == patched -> True`**. Same harness under `darwin` shows: no `--torch-backend`,
`CMAKE_ARGS=-DGGML_METAL=ON -DGGML_METAL_EMBED_LIBRARY=ON -DGGML_NATIVE=OFF -DGGML_CUDA=OFF -DLLAMA_CURL=OFF
-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0`, `MACOSX_DEPLOYMENT_TARGET=14.0`, parallelism 4. (My first
harness reported `False` only because each run used a different random temp dir in the `--python` path; fixed, not waved away.)
Harness: `build/packaging/macos/linux-equivalence/` (gitignored, throwaway).

**2. Runtime build** — `bash packaging/macos/prepare_runtime.sh` → `exit=0`, ~2 minutes, log ends
`runtime imports OK` / `Runtime staged at …/Contents/Resources/runtime/python`. Installed into
`build/packaging/macos/stage/Magnite.app/Contents/Resources/`: `runtime/`, `build-info/{build.json,python-packages.txt}`,
`licenses/{LICENSE,THIRD_PARTY_NOTICES.md,OFL-DMSans.txt,OFL-SpaceGrotesk.txt}`, `magnite.icns`. No `TESTING.md` yet (macOS one is
a Phase 12 deliverable; the script prints a note instead of copying the Linux AppImage text).

**3. Acceptance check, in a clean environment** (`env -i`, `-I -B`):
| Item | Result |
|---|---|
| Python | **3.12.12** arm64 (the pinned python-build-standalone, not 3.12.13) |
| torch | **2.14.0**, `mps_available True`, `mps_built True`, `torch.version.cuda None` |
| llama-cpp-python | **0.3.35** (pinned), `llama_supports_gpu_offload() True`; embedded Metal library loaded in **10.292 s**; GPU `Apple M1`, `MTLGPUFamilyApple7`, unified memory |
| transformers / peft / sentence-transformers / datasets / huggingface_hub / numpy | 5.16.1 / 0.20.0 / 6.0.1 / 5.0.1 / 1.30.0 / 2.5.2 |
| Pins | **17/17 match** installed versions; 81 packages in total; `bitsandbytes` **not** installed (as intended) |
| `backend` | real installed copy under `site-packages/backend/`, not the repo; repo not on `sys.path`; `python3 -I -B -m backend.main --help` works with `PYTHONHOME`/`PYTHONPATH` poisoned |
| Size | runtime **1.1 GB**; uv cache 1.7 GB; python-download 50 MB; **21 GiB free** (ISSUE-004 stays open) |

**4. Early Mach-O scan** (`packaging/macos/scan_macho.py`, 372 files): all minos ≤ **14.0** (11.0×85, 12.0×107, 14.0×180), **0** above — this
validates ADR 0002 on real data. **3 files are x86_64+arm64** (`uvloop`, `charset_normalizer` ×2). Also **31 console scripts** with a
build-host shebang, and build-host paths in `_sysconfigdata` and `direct_url.json`. All recorded as **ISSUE-006**, assigned to Phase 5.

**5. Shared-edit re-verification:** `run_regressions.sh` → **53 tests OK**; plain `cmake --build build -j4` → 0;
`./build/magnite-app --theme-check` → `0 failure(s)`.

**6. Idempotency:** re-running the wrapper (existing runtime → `--resume`) took **12 s**, re-built/reinstalled only the `magnite-backend`
wheel, and ended `runtime imports OK`.

**7. Provenance caveat:** `build.json` says `"working_tree_modified": true` (built on uncommitted edits) at commit `540a7a4`. Accurate,
but the package for testers must be built from a clean committed tree.

**Plan corrections made this phase** (in `PLAN.md`): Phase 4's recipe ran `--selftest` from the installed bundle, which cannot launch before
Phase 5 (same flaw found in Phase 2); it now uses `build-macos/Magnite.app` with an APFS-cloned runtime and a red/green step. Phase 5 gained
the ISSUE-006 work items.

**Not claimed:** that the *app* starts this runtime (Phase 4); anything about `.app` self-containment, signing, torch MPS *behaviour* under
training/inference, or quantization (Phases 5–6). `mps_available True` shows MPS is present, not that LoRA training works on it.

---

## 2026-10-23 15:58 IST — Phase 4: bundle path discovery (`app/src/backend_manager.cpp`)

Logs: `build/packaging/phase4-{red,green,plain-selftest,regressions}.log`; harness `build/packaging/macos/phase4-selftest.sh`
(all gitignored). Branch `release/v0.1-macos` at `0632eb7` + the Phase 4 edit.

**Fixture:** `build-tree` bundle `build-macos/Magnite.app` (Phase 2 configure: arm64, 14.0, `MAGNITE_MACOS_BUNDLE=ON`) with the Phase 3
runtime APFS-cloned into `Contents/Resources/runtime` (`cp -c -R`). Every selftest ran under `env -i` with `PATH=/usr/bin:/bin`,
`PYTHONHOME=/nonexistent`, `PYTHONPATH=/nonexistent`, `QT_QPA_PLATFORM=offscreen`, and a fresh `MAGNITE_HOME` under `$TMPDIR`.

**1. RED (before the edit)** → `[selftest] FAIL (1 failures)`, `backend process died during startup`. The client did not see
`Contents/Resources/runtime`, used the bare `python3` fallback (`/Library/Developer/CommandLineTools/usr/bin/python3`, 3.9, `isolated = 0`),
which honoured the poisoned `PYTHONHOME` and died with `ModuleNotFoundError: No module named 'encodings'`. That is the bug Phase 4 fixes
(and shows the old path would also have run the user's Python on any real Mac).

**2. Edit** (additive, guarded; see `DECISIONS.md`): under `#ifdef Q_OS_MAC`, prefer `QDir::cleanPath(appDir + "/../Resources/runtime")`
when it exists, else keep `appDir + "/runtime"`; the `LD_LIBRARY_PATH` block is now `#ifndef Q_OS_MAC` inside the existing non-Windows
branch. `-I -B`, removal of `PYTHONPATH`/`PYTHONHOME`, `MAGNITE_PACKAGED=1` and the "no fallback when packaged" rule are untouched.

**3. GREEN** (`cmake --build build-macos -j4`, then the same harness) → `[selftest] PASS (0 failures)`, `backend health OK`, harness exit 0.
The harness recorded the backend child's command line:
`…/build-macos/Magnite.app/Contents/Resources/runtime/python/bin/python3 -B -I -m backend.main --port 0`.
After exit: `backend.main still running after exit: none` (normal-exit cleanup only; the kill -9 case is ISSUE-001, Phase 9).
Client binary still `minos 14.0`.

**4. Shared-edit re-verification:** `run_regressions.sh` → **53 tests OK**; plain `cmake --build build -j4` → 0;
`./build/magnite-app --theme-check` → `0 failure(s)`. Extra: bare `--selftest` of the plain dev binary → **PASS**, backend spawned as
the repo `.venv` interpreter (`…/Python.framework/Versions/3.13/…/Python -m backend.main --port 0`, no `-I -B`) — the dev discovery path
is unchanged, and no orphan afterwards.

**5. Finding:** the GREEN run logs 6× `Hugging Face cache: Repo path is not a valid HuggingFace cache directory: …/hub/blobs`. The
plain dev run on the same cache logs 0. Cause: pinned huggingface_hub 1.30.0 vs a shared-blobs dir written by the dev venv's 1.33.0.
Recorded as **ISSUE-007**; not fixed.

**Harness note:** the first two GREEN runs did not capture the child's command line: `ps` truncated it (fixed with `-ww`), then the app's
`>` redirect overwrote the harness's appended line (fixed by opening the log with `>>`). The process table was also checked by hand
mid-run and showed the same bundled interpreter, so this was a harness bug, not an app one.

**Not verified / not claimed:** the *installed* bundle (`stage/`) launching — still Phase 5, after `macdeployqt`; Linux/Windows
compilation of the edited file (the Linux branch is the same code with an added `#ifndef Q_OS_MAC`, which is false there, but that
is reasoning, not a Linux build); App Translocation or a quarantined bundle path (Phase 8/10).

---

## 2026-10-23 18:05 IST — Phase 5: `.app` assembly, macdeployqt, thinning, ad-hoc signing

Logs: `build/packaging/phase5-{build1,build2,build3,regressions}.log`; smoke logs under `$TMPDIR/magnite phase5/logs` (all
untracked). Branch `release/v0.1-macos` at `e73c714` + Phase 5 edits.

**New:** `packaging/macos/build_app.sh`, `finalize_bundle.py`, `verify_bundle.py`, `smoke_bundle.sh`. **Shared-file edits:**
`app/CMakeLists.txt` (bundle `INSTALL_RPATH`), `packaging/prepare_runtime.py` (darwin: `LLAMA_OPENSSL=OFF`, `-ffile-prefix-map`).
**Changed:** `prepare_runtime.sh` (default destination `build/packaging/macos/runtime`; `--fresh` also cleans uv's llama-cpp-python wheel).

**1. Trial `macdeployqt` on a bare bundle (before writing the script).**
- `cmake --install` leaves the executable with **no `LC_RPATH`**, and Qt 6.8.3 `macdeployqt` only *rewrites* rpaths, so the deployed app died:
  `dyld: Library not loaded: @rpath/QtWidgets.framework/…`.
- Fix: bundle-only `INSTALL_RPATH "@executable_path/../Frameworks"`. The build tree keeps the dev-Qt rpath; the installed bundle gets the
  relative one.
- Next run: exit **137** (SIGKILL), because macdeployqt's install-name edits invalidated the signatures. `codesign --force --deep -s -`
  on the trial copy then gave `--theme-check` `0 failure(s)` with cocoa.
- `QT_QPA_PLATFORM=offscreen` → `Available platform plugins are: cocoa.` macdeployqt deploys only cocoa, so `libqoffscreen` is now copied in.

**2. First full build (`build1`)** → `verify_bundle.py` **2 failures**, both real:
- `linkage`: 60 problems.
  - **`libllama-common` links `/opt/homebrew/opt/openssl@3/lib/libssl.3.dylib` + `libcrypto.3.dylib`.** This is the host's Homebrew; a
    clean Mac would not load it.
  - 51 sklearn + 3 scipy absolute rpaths (`/Users/runner/micromamba/envs/build/lib`, `/opt/homebrew/Cellar/gcc@13/…`).
- `host-paths`: 44 binaries with this host's path.
  - `libpython3.12.dylib` has an absolute install id (uv). `bin/python3.12` itself links `@rpath/libpython3.12.dylib` via
    `@executable_path/../lib`, so it was not broken, only leaking.
  - The llama.cpp dylibs contain `__FILE__` strings under `build/packaging/uv-cache/sdists-v9/…`.
- New `resolve` check: only the stray `site-packages/lib/libllama-common*` failed to resolve. That copy has no rpath; `llama_cpp` loads
  from `llama_cpp/lib` (`llama_cpp.py:37-43`, `mtmd_cpp.py:48-55`).
- Fixes: darwin `-DLLAMA_OPENSSL=OFF` + `-ffile-prefix-map=<repo>=.`; finalize removes the llama.cpp dev install
  (`site-packages/lib`, `include`), resets the libpython id, and deletes absolute rpaths only after proving nothing needs them.

**3. Full build with `--fresh-runtime` (`build2`)**, 3 min 08 s, exit 0:
- Clean step:
  - removed 42 `bin/` entries (kept `python`, `python3`, `python3.12`)
  - `_sysconfigdata` prefix → `/install`
  - removed `direct_url.json` ×2 (backend, pip)
  - removed `site-packages/lib` + `include`
  - removed 17 `__pycache__` directories
- Relink: libpython id → `@rpath/…`; **54 absolute rpaths deleted** ("every dependency still resolves").
- Thin: **21** universal files → arm64.
- Sign: 361 Mach-O, 6 frameworks, then the bundle.
- `verify_bundle.py`:

| Check | Result |
|---|---|
| layout | PASS |
| arch | PASS — 368 Mach-O, all arm64 |
| minos | PASS — 11.0×85, 12.0×125, 14.0×158 |
| linkage | PASS — only relative and system references |
| resolve | PASS — every relative dependency found in the bundle |
| host-paths | PASS — none of `/Users/architbhuta`, the checkout, `MAGNITE_QT_ROOT` |
| symlinks | PASS |
| signature | PASS — `codesign --verify --deep --strict` |
| **total** | **0 failure(s)** |

Size **1.1 GB**: runtime 1.0 GB, Qt `Frameworks` 22 MB (thinned; ~62 MB fat at source), `PlugIns` 2.7 MB, `MacOS` 4.4 MB; 29,885 files.

**4. Acceptance: run from outside the checkout.**
- `ditto` to `$TMPDIR/magnite phase5/Magnite.app` (path contains a space), then `bash packaging/macos/smoke_bundle.sh`. Host env:
  `env -i`, `PATH=/usr/bin:/bin`, `PYTHONHOME`/`PYTHONPATH=/nonexistent`.
- Results:
  - `PASS signature before run`
  - `PASS --theme-check: 0 failure(s)`
  - `PASS --selftest: [selftest] PASS (0 failures), backend health OK`
  - `PASS backend interpreter: bundled python3 -B -I`
  - `PASS no backend left running`
  - `PASS signature after run (bundle unmodified)`
  - **0 failure(s).**
- First attempt reported the interpreter as "not seen". Harness bug: the backend is a grandchild of a backgrounded shell function. The
  harness now matches on the bundle's runtime path.
- Same copy, bundled interpreter under the poisoned env: `3.12.12 llama_cpp 0.3.35 gpu_offload True torch 2.14.0 mps True`. So
  `LLAMA_OPENSSL=OFF` kept Metal. No `ssl` in any `llama_cpp/lib` dylib. Signature still valid afterwards.

**5. Idempotency.** `build_app.sh` re-run without `--fresh-runtime` (`build3`): **72 s**, same clean/relink/thin/sign counts,
**0 failure(s)**.

**6. Shared-edit re-verification.**
- `run_regressions.sh` → **53 tests OK**.
- Plain `cmake --build build -j4` → OK; `./build/magnite-app --theme-check` → `0 failure(s)`. The 5 `-Wunused-lambda-capture` warnings in
  `mainwindow.cpp` predate this branch.
- Fresh default configure (no bundle option) → installs only `bin/magnite-app`, **no rpath** (unchanged).
- Linux equivalence harness re-run against `git show HEAD:packaging/prepare_runtime.py`: **`linux: original == patched -> True`**;
  Linux `CFLAGS`/`CXXFLAGS`/`OBJCFLAGS` stay unset; darwin shows `-DLLAMA_OPENSSL=OFF` and the prefix map.

**7. Findings.**
- ISSUE-006 resolved, plus §4–6 found by the stricter gate.
- **ISSUE-008**: `certifi` drifted 2026.10.19 → 2026.10.23 between Phase 3 and this rebuild, because transitive deps are unpinned.
  `runtime-packages-macos.txt` is updated to this build.
- **ISSUE-009**: no Qt license texts exist to bundle.
- `build.json` says `working_tree_modified: true` for these builds. The clean-tree rebuild is recorded below once committed.

**8. Clean-tree build (after committing `3edf65d`).**
- `build_app.sh` (`build4-clean`) → no uncommitted-changes warning; `verify_bundle.py` **0 failure(s)**.
- `build.json`: `"commit": "3edf65df055f89f32c48ce61ebb21b955a8b93ee"`, `"working_tree_modified": false`.
- `ditto` to `$TMPDIR/magnite phase5 clean/`, then `smoke_bundle.sh` → **6/6 PASS, 0 failure(s)**. Logs copied to
  `build/packaging/phase5-smoke-logs/`; the 1.1 GB copy was deleted.

**Not verified / not claimed:**
- Gatekeeper/quarantine behaviour, App Translocation and `/Applications` (Phases 7–10).
- Another Mac or user account (Phase 10).
- Model loading/inference/training through the bundled app (Phases 6, 9).
- Linux/Windows compilation of the CMake change. It sits inside `if(APPLE AND MAGNITE_MACOS_BUNDLE)`; that is reasoning, not a run.

---

## 2026-10-23 19:05 IST — Phase 6: ML dependency matrix

Logs: `build/packaging/macos/ml-matrix/{results.json,logs/}`, `build/packaging/macos/hf-probe/probe-{130,133}.json`,
`build/packaging/phase6-selftest-training.log` (all untracked). Bundle under test: `stage/Magnite.app`, runtime of commit `3edf65d`.

**New:** `packaging/macos/ml_matrix.py`, `probe_hf_cache.py`, `docs/release/macos/ML_MATRIX.md`. **No shared-file edits** this phase
(only new scripts and docs), so the regression/plain-build re-run rule did not apply.

**1. Method.**
- Every cell runs as its own `-I -B` subprocess of the **bundled** interpreter, with `HF_HOME` set to a dedicated test cache.
- Each cell goes through the backend's own code where the app has one: `LlamaCppEngine` (the supervisor's `n_gpu_layers=20`),
  `TorchPeftEngine`, `run_training`, `rag.embeddings`.
- Peak RSS comes from `wait4`.
- bitsandbytes cells use a throwaway APFS clone of the runtime with `requirements-macos-quantization.txt` installed.
- `codesign --verify --deep --strict` on the bundle **still passes after every matrix run** (`-B` writes nothing into the bundle).

**2. Harness bugs found and fixed before the final run** (no result below depends on them):
- `snapshot_download` without a filter pulled 2.6 GB of ONNX/other exports. Now `allow_patterns`; the test cache is 1.5 GB.
- "CPU training" first patched `torch.backends.mps.is_available` → transformers failed to import `Trainer`. The second attempt replaced
  `transformers.TrainingArguments`, which `from transformers import …` ignores, so it **silently trained on MPS** (caught because the
  cell records `args.device`). The final version wraps `TrainingArguments.__post_init__` → `device=cpu`, `param_device=cpu`.
- Single 48-token throughput samples were noise: llama.cpp CPU read 55 tok/s in one run and 295 tok/s in the next. Now one warm-up,
  then the median of 5 (llama.cpp) or 3 (torch) 128-token runs, with the range.

**3. Final run** (all 17 cells in one fresh run, 2026-10-23 ~18:55): **13 PASS, 4 FAIL**. Full table in `ML_MATRIX.md`. Key numbers:
- llama.cpp 135M: Metal 20/31 → 95.6 tok/s; all layers → 103.8; CPU → 291.7.
- llama.cpp 1.5B Q4_K_M (Qwen2.5): 20/29 → 46.0 tok/s, 1,514 MB; all → 43.8, 1,298 MB, load 0.31 s; CPU → 49.3, 2,110 MB.
- torch: MPS fp16 → 42.5 tok/s (load 9.0 s); CPU fp32 → 44.1 (load 2.6 s).
- LoRA (12 steps): MPS fp32 → 8.8 s, loss 4.07 → 3.13; CPU (`use_cpu=True`) → 201.0 s, same curve. Each adapter served on MPS.
- Embeddings on `mps:0`: 2,132 sentences/s; cosine related 0.608 / unrelated −0.044.
- QLoRA 4-bit/8-bit in the **shipped** runtime → FAIL: `Using bitsandbytes 4-bit quantization requires bitsandbytes: pip install -U bitsandbytes>=0.46.1`.
- With bitsandbytes 0.50.2 added:
  - 4-bit and 8-bit loads run, but on `device=cpu` (210 quantized layers); the 4-bit text degenerates into repetition;
  - QLoRA 4-bit trains with `param_device=cpu` under an MPS Trainer;
  - QLoRA 8-bit → FAIL `Tensor for argument #2 'mat2' is on CPU, but expected it to be on GPU`.
- `llama_supports_gpu_offload()` → `True`.

**4. Metal shader cache (ISSUE-002).**
- Every Phase 6 process logged `ggml_metal_library_init: loaded in 0.013–0.050 sec`, against 10.292 s in Phase 3.
- The compiled library is cached per user in `$(getconf DARWIN_USER_CACHE_DIR)com.apple.metal/` (46 MB).
- I did not clear it to re-measure the cold case; Phase 10's fresh account will.

**5. ISSUE-007 measured** (`probe_hf_cache.py` on two APFS clones of the real 6 GB cache, backend code under hub 1.30.0 vs 1.33.0):
- Listing and sizes are identical.
- `_remove_hf` succeeds under both and leaves no dangling links in other repos.
- Under 1.30, **1,829,250,588 bytes** of the deleted model stay unreferenced in `hub/blobs`; under 1.33, 244 bytes.

**6. ISSUE-010 (new) confirmed in the app itself.**
- `MAGNITE_SELFTEST_TRAINING=1 Magnite.app/Contents/MacOS/Magnite --selftest`, run on the bundle (`env -i`, isolated `HF_HOME`/`MAGNITE_HOME`),
  printed:
  - `[selftest] ok: training started …`
  - `[selftest] FAIL: training job error: Using bitsandbytes 4-bit quantization requires bitsandbytes…`
  - `[selftest] FAIL (1 failures)`
- No orphan backend afterwards.
- Cause: the selftests send `quantization: "4bit"`, and the Training panel's "Use 4-bit QLoRA" is checked by default.

**Not verified / not claimed:**
- Models above 1.5B; prompt-prefill speed; fine-tuning quality.
- macOS 14/15; any Mac other than this M1 8 GB.
- The HTTP/UI paths for these features (Phase 8 selftests).
- The cold Metal compile on a fresh user (Phase 10).

---

## 2026-10-23 19:12 IST — Phase 7: `.dmg` creation

Logs: `build/packaging/phase7-{dmg-trial,build-app,build-dmg,check-dmg}.log`, `build/packaging/phase7-dmg-check-logs/` (untracked).
**New:** `packaging/macos/build_dmg.sh`, `packaging/macos/check_dmg.sh` (committed `5a04358`). No shared-file edits.

**1. Trial** (bundle of `3edf65d`).
- `build_dmg.sh` → the `verify_bundle.py` gate showed `0 failure(s)`; `hdiutil create` UDZO/HFS+ took 54 s → **320 MB** image (bundle 1.1 GB);
  `hdiutil verify` VALID.
- macOS 27 prints `hdiutil: WARNING: 'hdiutil create -volname -format ...' is deprecated. Please use 'diskutil image create from/blank …'`
  (and the same for `attach`). Kept `hdiutil`; see `DECISIONS.md`.
- First `check_dmg.sh` run: 9 PASS, 1 FAIL, `volume is not read-only`. Cause: a harness bug. `mount` reports
  `/private/var/folders/…` while `mktemp` returned the `/var/…` symlink (the ISSUE-005 quirk again). `touch` on the volume gives
  `Read-only file system`. Fixed with `cd -P`; re-run 10/10 PASS.

**2. Final artifact from a clean tree.**
- `build_app.sh` at `5a04358` → `verify_bundle.py` `0 failure(s)`; `build.json`: `"commit": "5a043588b3fd…"`, `"working_tree_modified": false`.
- `build_dmg.sh` → `dist/Magnite-0.1.0-test.1-arm64.dmg`, **321 MB**, `hdiutil verify` VALID.
- `dist/SHA256SUMS-macos`:
  `f72686db72243f7945b9c3f1568f03dde567044656b51d4d4e02208b72d3ab43  Magnite-0.1.0-test.1-arm64.dmg`
- `check_dmg.sh` (app run **inside the read-only mounted volume**, poisoned host env):

| Check | Result |
|---|---|
| hdiutil verify | PASS |
| volume contents | PASS — `Applications Magnite.app` |
| `Applications` link | PASS — `-> /Applications` |
| mounted read-only | PASS |
| signature before run | PASS |
| `--theme-check` | PASS — 0 failure(s) |
| `--selftest` | PASS — `[selftest] PASS (0 failures)`, backend health OK |
| backend interpreter | PASS — bundled `python3 -B -I` |
| no backend left running | PASS |
| signature after run | PASS — bundle unmodified |
| **total** | **0 failure(s)** |

- `hdiutil info` shows no leftover mount.
- `dist/` is gitignored; nothing under it is committed.
- The volume has no `TESTING.md` yet (Phase 12); `build_dmg.sh` prints a note.

**Not verified / not claimed:**
- Gatekeeper / quarantine on a downloaded `.dmg`, drag-install to `/Applications`, App Translocation (Phases 8–10).
- The `MAGNITE_SELFTEST_*` workloads from the dmg copy (Phase 8).
- Any other Mac or account.
- No custom Finder window layout (deliberate).

---

## 2026-10-23 19:35 IST — Phase 8: functional verification of the installed app

Logs: `build/packaging/phase8/*.log` (untracked); screenshots committed in `docs/release/macos/screenshots/`.
**New:** `packaging/macos/verify_installed.sh`. No shared-file edits.

**Setup.**
- `(cd dist && shasum -a 256 -c SHA256SUMS-macos)` → `OK`.
- Mounted the `.dmg` read-only, `ditto`'d `Magnite.app` to `$TMPDIR/Magnite-verify.app`, detached.
- `codesign --verify --deep --strict` passes on the copy; `build.json` commit `5a04358`.
- Every step below ran with:
  - `env -i`, `PATH=/usr/bin:/bin`, `PYTHONHOME`/`PYTHONPATH` poisoned;
  - a fresh `MAGNITE_HOME` per step;
  - `HF_HOME=build/packaging/macos/test-cache` (never the real HF cache);
  - `QT_QPA_PLATFORM=offscreen`, a 900 s guard, and an orphan check after each step.

**Results** (`bash packaging/macos/verify_installed.sh "$TMPDIR/Magnite-verify.app" build/packaging/phase8 build/packaging/macos/test-cache`,
2 min 32 s in total):

| Step | Result | Evidence (log line) |
|---|---|---|
| `--theme-check` | PASS (8 s) | `[theme-check] 0 failure(s)` |
| `--ui-check` | PASS (10 s) | `[ui-check] PASS (0 failures)`; 43 captures |
| `--selftest` | PASS (13 s) | `[selftest] PASS (0 failures)` |
| `MAGNITE_SELFTEST_PULL` | PASS (16 s) | `pull e2e: models now registered=5` |
| `MAGNITE_SELFTEST_INFERENCE` | PASS (16 s) | `inference load engine=llamacpp`, `inference e2e: status=200, deltas=8`, `inference unload` |
| `MAGNITE_SELFTEST_TRAINING` | **FAIL** (15 s) | `Using bitsandbytes 4-bit quantization requires bitsandbytes …` → **ISSUE-010** |
| `MAGNITE_SELFTEST_ADAPTER` | **FAIL** (13 s) | same → **ISSUE-010** |
| `MAGNITE_SELFTEST_RAG` | PASS (19 s) | `rag ingest … chunks=1`, `rag query: status=200 score=0.652`, `rag delete`, `rag cleanup` |
| `live_workflow.py` (bundled `python3 -I -B`) | PASS (28 s) | `PASS registered transformer model -> auto engine -> nonstreaming chat` · `PASS mixed instruction/text training -> registered adapter` · `PASS active adapter + real RAG retrieval -> chat response (quality not assessed)` · `PASS real PEFT export -> delete -> import; RAG cleanup` · `PASS training history survives backend lifespan restart` |
| `--screenshot` | PASS (11 s) | `[screenshot] done`; 6 panels |
| signature after all steps | PASS | bundle unmodified |

**9 of 11 steps PASS, 2 FAIL.** Both failures are the known ISSUE-010, now reproduced on the dmg copy.

- No backend was orphaned after any step.
- The screenshots show the bundled fonts, icons and brand rendering correctly. Each shows "Connecting", because `--screenshot` captures
  400 ms after launch, before the backend is ready. That is a property of the dev tool, not a defect.

**Acceptance not fully met:** the plan requires every selftest to PASS. TRAINING and ADAPTER do not, so the macOS checklist line for
"supported inference/training/RAG paths" cannot be ticked until ISSUE-010 is fixed and these two steps are re-run.

**Not verified / not claimed:**
- An interactive (non-offscreen) GUI session.
- Gatekeeper / quarantine and `/Applications` (Phases 9–10).
- Real-size models through the UI (`ML_MATRIX.md` covers ≤ 1.5B through the backend).
