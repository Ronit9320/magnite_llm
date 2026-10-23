# macOS v0.1 preview package (.app in .dmg) — phased plan

Status legend: TODO / IN-PROGRESS / DONE / BLOCKED. Update the status on a phase
heading the moment it changes, and mirror it in `STATUS.md`.

## Context

`RELEASE_CHECKLIST.md` has four unticked macOS lines. Only the Linux AppImage test
build exists; Windows is explicitly out of scope. The goal is a self-contained
`Magnite.app` inside a `.dmg` that runs on an Apple Silicon Mac with no developer
checkout, no Homebrew and no system Python — mirroring the Linux pattern (adjacent
isolated Python runtime, relocatable discovery, CMake install rules), labeled
honestly as an unsigned testing build.

Work spans many sessions, so all state lives in `docs/release/macos/`:
`PLAN.md`, `STATUS.md`, `LOG.md`, `DECISIONS.md`, `ISSUES.md`, `adr/`.
Branch: `release/v0.1-macos`, branched from `dev`.

## Decisions confirmed with the user

| Topic | Decision |
|---|---|
| Architecture | **arm64 only.** torch 2.14.0 publishes only `macosx_14_0_arm64` wheels and python-build-standalone has no universal2 build, so a universal bundle is not achievable. Qt 6.8.3 is universal and will be thinned with `lipo`. |
| Minimum macOS | **14.0 (Sonoma)**, forced by the torch wheel tag. Qt (minos 12.0) and the Python runtime go lower. Only macOS 27.0.1 will actually be tested; 14.x/15.x stay unverified claims. |
| Python runtime | python-build-standalone 3.12.12 via `uv`, copied into `Contents/Resources/runtime/python`, launched `-I -B`, no system-Python fallback (same contract as Linux). |
| ML strategy | arm64 wheels pinned in a macOS-specific requirements file; torch CPU+MPS; llama.cpp built from sdist with Metal via `GGML_METAL_EMBED_LIBRARY` (no Metal toolchain on this host); bitsandbytes quantization treated **unsupported until proven**. |
| Signing | Ad-hoc (`codesign -s -`) now; Developer ID + notarization is a separate phase that stays **BLOCKED** until a certificate exists. |
| Clean install | Second macOS user account on this M1, recorded as a **proxy**; the true clean-machine checklist line stays unticked. |

Each row has a short ADR under `adr/`, indexed in `DECISIONS.md`.

## Feasibility check — top risks

Audited in this session on the host (macOS 27.0.1 / Darwin 27, Apple M1, 8 GiB RAM,
20 GiB free disk, CLT clang 14.0.3, SDK 13.3 max, no full Xcode, cmake 4.4.3,
uv 0.11.4, no codesigning identities).

| # | Risk | Verify by | Status |
|---|---|---|---|
| R1 | **Qt deployment.** Needs official Qt (not Homebrew, whose bottles pin minos to the host OS). | Found pinned **Qt 6.8.3 universal, `minos 12.0`, SDK 14.4** at `/Users/architbhuta/magnite_llm-master/.qt-tools/Qt/6.8.3/macos` with `macdeployqt`. `build/magnite-app` was already built arm64 against it. | **Low** — toolchain proven, but Qt lives outside this repo; must be pinned by env var and recorded. |
| R2 | **Deployment target leaks.** `CMAKE_OSX_DEPLOYMENT_TARGET` is empty in the current cache, so the binary inherits the SDK (13.3) / host. | Set `-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DCMAKE_OSX_ARCHITECTURES=arm64`; assert with `otool -l … \| grep -A3 LC_BUILD_VERSION`. | **Low**, but must be explicit. |
| R3 | **Relocatable Python.** PBS binaries are ad-hoc signed; any `install_name_tool` edit invalidates the signature, and `codesign --deep` over ~10k files is slow. | Copy the runtime unmodified, never rewrite its install names, sign inner Mach-O first then the bundle; verify `codesign --verify --deep --strict`. | **Medium.** |
| R4 | **llama.cpp Metal.** `xcrun metal` is **absent** (CLT only, no Xcode). Default Metal builds want it. | Build with `-DGGML_METAL=ON -DGGML_METAL_EMBED_LIBRARY=ON` (runtime shader compilation, no toolchain). Fallback `-DGGML_METAL=OFF` (CPU-only, still correct). | **Medium** — mitigated by a documented CPU fallback; cannot block the package. |
| R5 | **torch MPS for LoRA training.** `trainer.py:292` gates fp16 on `torch.cuda.is_available()` and sets `use_cpu=False`, so transformers may pick MPS; unsupported MPS ops are a known class of failure. | Run `MAGNITE_SELFTEST_TRAINING=1` and `backend/tests/live_workflow.py` under the packaged runtime; if MPS fails, record the limit and document CPU training. | **Medium** — a failure narrows the ML matrix, it does not block packaging. |
| R6 | **Quantization.** bitsandbytes 0.50.2 *does* ship `macosx_14_0_arm64`, but Apple-silicon 4/8-bit support is experimental. | Probe `BitsAndBytesConfig(load_in_4bit=True)` on a tiny model in the packaged runtime. | **Medium** — expected outcome is "quantized training unsupported on macOS", documented like the Linux build's QLoRA exclusion. |
| R7 | **Gatekeeper / translocation.** Unsigned + quarantined apps get App Translocation and scary dialogs; bundled child executables can be blocked. | Set `com.apple.quarantine` on the `.dmg` by hand and test the whole install flow, including launching after dragging to `/Applications`. | **Medium** — affects usability, handled by instructions + ad-hoc signing. |
| R8 | **Orphaned backend.** `backend/main.py:17` only arms `PR_SET_PDEATHSIG` on Linux, so a force-killed GUI can leave the macOS backend running. | Force-`kill -9` the GUI and check for a surviving `python3 -m backend.main`. | **Medium** — a real functional gap. Report in `ISSUES.md`; fix only if the user approves a platform-guarded addition. |
| R9 | **Disk/RAM.** 20 GiB free; Qt is 4.8 GiB elsewhere, torch + runtime ≈ 2–3 GiB staged, plus uv cache. 8 GiB RAM limits model size. | Track `du -sh` at each phase; keep the uv cache under `build/packaging/`, prune after a successful build. | **Medium** — mitigate by pruning, not by changing the design. |
| R10 | **`mac` branch fix not in `dev`.** Commit `7fcdb3c` replaces the `QNetworkAccessManager` health probe with a raw `QTcpSocket` probe; without it the app reports "Unavailable" on macOS. | `git merge mac` into `release/v0.1-macos` in Phase 1, then confirm the app reaches "backend health OK". | **Blocking if skipped** — hence Phase 1. |

**No risk looks plan-infeasible.** R4/R5/R6 can only narrow the advertised ML matrix;
R10 is a merge; the rest are mechanical. The one thing that could stall the plan is
disk exhaustion (R9), which is observable early.

Note: `RELEASE_CHECKLIST.md` in this repo has **no "Base feature acceptance" section**.
The acceptance set used throughout is §5 *Establish release verification* plus the five
built-in selftests and `backend/tests/live_workflow.py`.

## Phases

Each phase: one sitting, one commit named for the phase. Stop after the acceptance
check passes and report that it is safe to `/clear`.

Shared preamble for every command block:

```sh
cd /Users/architbhuta/magnite_llm
export MAGNITE_QT_ROOT=/Users/architbhuta/magnite_llm-master/.qt-tools/Qt/6.8.3/macos
```

---

### Phase 0 — Plan, ADRs and state files · model: Opus · **status: DONE**
**Goal:** land the plan and the resume-anywhere state files on a clean branch.

Steps / commands:
```sh
git switch -c release/v0.1-macos dev
mkdir -p docs/release/macos/adr packaging/macos
# write PLAN.md (this document), STATUS.md, LOG.md, DECISIONS.md, ISSUES.md
# write adr/0001..0008 (architecture, min-macOS, Python runtime, ML strategy,
#   signing, Qt provenance, bundle layout, mac-branch merge)
git add docs/release/macos packaging/macos && git commit -m "docs(macos): Phase 0 plan, ADRs and state files"
```
**Acceptance:** `ls docs/release/macos` shows the five state files + `adr/` with 8 files;
`git log -1 --name-only` lists them; nothing outside `docs/release/macos/` is touched.
**Idempotent:** yes — rewriting the docs is safe. Retry: `git checkout -- docs/release/macos`.
**Rollback:** `git switch dev && git branch -D release/v0.1-macos`.
**Depends on:** nothing.

---

### Phase 1 — Environment audit, pins, and macOS baseline · model: Sonnet · **status: DONE** (2026-10-23)
**Goal:** merge the required macOS fix and prove the repo's own checks pass on this Mac before any packaging work.

```sh
git merge --no-ff mac -m "merge: macOS loopback health probe (7fcdb3c) for v0.1 macOS packaging"
sw_vers; uname -m; clang --version | head -2; cmake --version | head -1; uv --version
"$MAGNITE_QT_ROOT/bin/qmake6" -query QT_VERSION
uv venv --python 3.12 .venv-macos && uv pip install --python .venv-macos -e "backend"
uv pip install --python .venv-macos "pypdf==6.19.0"   # [ml]-extra; avoids a skipped test
bash packaging/macos/run_regressions.sh   # NOT bare unittest: macOS /var symlink breaks 5 tests (ISSUE-005)
cmake -S app -B build-macos -DCMAKE_PREFIX_PATH="$MAGNITE_QT_ROOT" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DMAGNITE_BUILD_LABEL=0.1.0-test.1
cmake --build build-macos -j4
./build-macos/magnite-app --theme-check
otool -l build-macos/magnite-app | grep -A3 LC_BUILD_VERSION
```
**Acceptance:** regressions all pass (count recorded in `LOG.md`); `--theme-check` prints
`0 failure(s)`; `LC_BUILD_VERSION` shows `minos 14.0`; versions pinned in
`docs/release/macos/LOG.md` and an `environment-macos.txt`.
**Idempotent:** yes. Clean retry: `rm -rf build-macos .venv-macos`.
**Rollback:** `git merge --abort`, or `git reset --hard origin/dev` on the branch.
**Depends on:** Phase 0.
**Result (2026-10-23):** merge clean; 53 regressions OK (0 skipped) via `run_regressions.sh`; `--theme-check` 0 failure(s); `minos 14.0`; bare `--selftest` PASS with `backend health OK`. Bare `unittest` fails 5 tests on macOS (symlinked `/var`) — see ISSUE-005. Evidence in `LOG.md`.
**Note:** `build-macos/` is a new build dir so the existing `build/` (and the AGENTS.md
dev workflow) is untouched. Add `build-macos/` to `.gitignore` if not already covered.

---

### Phase 2 — Bundle-aware CMake (shared file, APPLE-guarded) · model: Sonnet · **status: TODO**
**Goal:** teach `app/CMakeLists.txt` to produce and install a `.app` **only** when asked, leaving Linux and the macOS dev workflow unchanged.

Changes to `app/CMakeLists.txt`, all inside `if(APPLE)` and behind
`option(MAGNITE_MACOS_BUNDLE "Build a macOS .app bundle" OFF)`:
- `MACOSX_BUNDLE TRUE`, `MACOSX_BUNDLE_INFO_PLIST` → `packaging/macos/Info.plist.in`
- bundle identifier `com.magnite.magnite-llm`, short version from `MAGNITE_BUILD_LABEL`
- `MACOSX_BUNDLE_ICON_FILE magnite.icns`, icon installed into `Contents/Resources`
- `install(TARGETS magnite-app BUNDLE DESTINATION .)` for the bundle case; the existing
  `RUNTIME DESTINATION bin` path stays for every other configuration
- `LSMinimumSystemVersion 14.0`, `NSHighResolutionCapable`, `LSApplicationCategoryType`

New files: `packaging/macos/Info.plist.in`, `packaging/macos/make_icon.sh`
(`qlmanage -t -s <px>` → PNG set → `iconutil -c icns`; fallback `uv run --with cairosvg`).

```sh
bash packaging/macos/make_icon.sh                 # -> build/packaging/macos/magnite.icns
cmake -S app -B build-macos -DCMAKE_PREFIX_PATH="$MAGNITE_QT_ROOT" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DMAGNITE_MACOS_BUNDLE=ON \
      -DMAGNITE_BUILD_LABEL=0.1.0-test.1
cmake --build build-macos -j4
cmake --install build-macos --prefix build/packaging/macos/stage
# regression: the non-bundle build must still work
cmake -S app -B build -DCMAKE_PREFIX_PATH="$MAGNITE_QT_ROOT" && cmake --build build -j4
```
**Acceptance:** `build/packaging/macos/stage/Magnite.app/Contents/MacOS/magnite-app --theme-check`
prints `0 failure(s)`; `plutil -p .../Contents/Info.plist` shows the identifier and
`LSMinimumSystemVersion 14.0`; the plain `./build/magnite-app` build still succeeds;
offline regressions re-run and logged (shared-file edit rule).
**Idempotent:** yes. Retry: `rm -rf build-macos build/packaging/macos/stage`.
**Rollback:** `git checkout -- app/CMakeLists.txt`; the option defaults OFF so nothing else regresses.
**Depends on:** Phase 1. **Log in `DECISIONS.md`:** shared-file edit to `app/CMakeLists.txt`.

---

### Phase 3 — Relocatable Python runtime + pinned deps · model: Sonnet (Opus if llama.cpp fights back) · **status: TODO**
**Goal:** stage an isolated arm64 Python 3.12.12 with the backend wheel and pinned ML deps, importable with `-I`.

New: `packaging/macos/requirements-macos-arm64.txt` (fork of
`packaging/requirements-cpu.txt`; **bitsandbytes moved to a separate optional file**
so a failed probe cannot break the build) and `packaging/macos/prepare_runtime.sh`,
which calls the existing `packaging/prepare_runtime.py` with macOS `CMAKE_ARGS`.
`prepare_runtime.py` needs a small platform-guarded edit: macOS `CMAKE_ARGS`
(`-DGGML_METAL=ON -DGGML_METAL_EMBED_LIBRARY=ON -DGGML_NATIVE=OFF -DLLAMA_CURL=OFF`)
instead of the Linux AVX flags, and `--torch-backend cpu` dropped on macOS.

```sh
bash packaging/macos/prepare_runtime.sh --destination build/packaging/macos/stage/Magnite.app/Contents/Resources
R=build/packaging/macos/stage/Magnite.app/Contents/Resources/runtime/python/bin/python3
"$R" -I -B -c "import backend.server, llama_cpp, torch, transformers, peft, sentence_transformers, datasets; \
print(torch.__version__, torch.backends.mps.is_available())"
"$R" -I -B -c "import llama_cpp, ctypes; print(llama_cpp.__version__)"
du -sh build/packaging/macos/stage/Magnite.app
```
**Acceptance:** the import line prints a torch version and `True` for MPS; `llama_cpp`
imports; `build-info/python-packages.txt` and `build.json` exist under
`Contents/Resources`; sizes recorded in `LOG.md`.
**Idempotent:** `--resume` re-runs the dependency install; a clean retry is
`rm -rf .../Contents/Resources/runtime` (and `rm -rf build/packaging/uv-cache` if deps are suspect).
**Rollback:** delete the staged runtime; revert `prepare_runtime.py`.
**Depends on:** Phase 2. **If Metal fails twice:** stop, log in `ISSUES.md`, retry with
`-DGGML_METAL=OFF` and record CPU-only llama.cpp as the shipped configuration.

---

### Phase 4 — Bundle path discovery (shared file, `Q_OS_MAC`-guarded) · model: Opus · **status: TODO**
**Goal:** make the client find the bundled runtime at `Contents/Resources/runtime` and never fall back to a system Python.

`app/src/backend_manager.cpp` — inside `#ifdef Q_OS_MAC`, additive only:
- packaged-runtime probe order: `applicationDirPath()/../Resources/runtime` first, then the
  existing `applicationDirPath()/runtime` (keeps the Linux layout working verbatim)
- no `LD_LIBRARY_PATH` injection on macOS (the Linux `../lib` hop does not apply; torch and
  llama.cpp dylibs resolve through `@loader_path` inside the wheels)
- keep `-I -B`, `MAGNITE_PACKAGED=1`, and the "never fall back" contract unchanged

`backend/src/backend/main.py` — **no change in this phase.** The missing macOS
parent-death signal (R8) is recorded in `ISSUES.md` and only fixed if the user approves.

```sh
cmake --build build-macos -j4 && cmake --install build-macos --prefix build/packaging/macos/stage
env -i HOME="$HOME" PATH=/usr/bin:/bin PYTHONHOME=/nonexistent PYTHONPATH=/nonexistent \
    MAGNITE_HOME="$TMPDIR/magnite-phase4" QT_QPA_PLATFORM=offscreen \
    build/packaging/macos/stage/Magnite.app/Contents/MacOS/magnite-app --selftest
bash packaging/macos/run_regressions.sh   # sets a resolved TMPDIR; see ISSUE-005
cmake --build build -j4   # Linux-shaped non-bundle build still fine
```
**Acceptance:** the `--selftest` run prints `[selftest] PASS (0 failures)` **with a broken
host Python environment**, and the log contains `backend health OK`; regressions still pass.
**Idempotent:** yes. **Rollback:** `git checkout -- app/src/backend_manager.cpp`.
**Depends on:** Phase 3. **Log in `DECISIONS.md`:** shared-file edit + re-run evidence.

---

### Phase 5 — `.app` assembly, macdeployqt, thinning, ad-hoc signing · model: Sonnet · **status: TODO**
**Goal:** one re-runnable script that produces a complete, self-contained, ad-hoc-signed `Magnite.app`.

New `packaging/macos/build_app.sh` — ordering matters:
1. `cmake` configure + build + install (Phases 2/4 flags)
2. `macdeployqt` on the **bare** bundle, before the Python runtime is copied in
   (`macdeployqt … -verbose=1`; do **not** pass `-codesign`, do not let it walk 10k Python files)
3. `lipo -thin arm64` every universal Mach-O under `Contents/Frameworks` (halves Qt's weight)
4. stage the Python runtime (Phase 3 script)
5. copy `LICENSE`, `THIRD_PARTY_NOTICES.md`, font OFL files, `packaging/TESTING.md`
6. `codesign --force --timestamp=none -s -` over inner Mach-O files first, then the bundle
7. `packaging/macos/verify_bundle.py` — the real gate

`verify_bundle.py` fails on: any Mach-O with a non-arm64 slice; any `otool -L` entry
pointing at `/opt/homebrew`, `/usr/local`, `$MAGNITE_QT_ROOT` or `/Users/`; any
`LC_BUILD_VERSION` minos above 14.0 in the client or Qt frameworks; a missing
`Contents/Resources/runtime/python/bin/python3`; `codesign --verify --deep --strict` failure.

```sh
bash packaging/macos/build_app.sh
python3 packaging/macos/verify_bundle.py build/packaging/macos/stage/Magnite.app
codesign --verify --deep --strict --verbose=2 build/packaging/macos/stage/Magnite.app
du -sh build/packaging/macos/stage/Magnite.app
```
**Acceptance:** `verify_bundle.py` exits 0 and prints a zero-failure summary; `codesign --verify`
passes; the bundle launches `--theme-check` from a path outside the checkout.
**Idempotent:** the script rebuilds the bundle from scratch except the Python runtime,
which it reuses unless `--fresh-runtime` is passed. Clean retry: `rm -rf build/packaging/macos`.
**Rollback:** delete the staging dir; no tracked file changes beyond the new scripts.
**Depends on:** Phases 2–4.

---

### Phase 6 — ML dependency matrix · model: Opus · **status: TODO**
**Goal:** measure, not assume, what works on Apple Silicon — CPU vs MPS vs llama.cpp Metal, and whether quantization is usable at all.

New `packaging/macos/ml_matrix.py`, run **with the bundled interpreter**, writing
`docs/release/macos/ML_MATRIX.md`: for each of llama.cpp (Metal on/off), torch CPU,
torch MPS inference, LoRA training on CPU, LoRA training on MPS, and 4-bit/8-bit
bitsandbytes, record pass/fail, wall time, peak RSS and the exact error on failure.

```sh
R=build/packaging/macos/stage/Magnite.app/Contents/Resources/runtime/python/bin/python3
"$R" -I -B packaging/macos/ml_matrix.py --out docs/release/macos/ML_MATRIX.md --cache build/packaging/macos/test-cache
"$R" -I -B -c "import llama_cpp; print(llama_cpp.llama_supports_gpu_offload())"
```
**Acceptance:** `ML_MATRIX.md` exists with a verdict for every row and no row left
"untested"; whatever fails is also written to `ISSUES.md` with its error; the supported
set is what later docs and release notes are allowed to claim.
**Idempotent:** yes, re-runnable; uses a dedicated HF cache under `build/packaging/macos/test-cache`.
**Rollback:** delete the generated matrix.
**Depends on:** Phase 5. **Expected narrowing:** quantized (QLoRA) training unsupported,
matching how the Linux build excluded QLoRA.

---

### Phase 7 — `.dmg` creation · model: Sonnet · **status: TODO**
**Goal:** a compressed, checksummed `.dmg` with a drag-to-Applications layout, using only `hdiutil` (no Homebrew `create-dmg` dependency).

New `packaging/macos/build_dmg.sh`: stage `Magnite.app` + an `Applications` symlink +
`TESTING.md` into a scratch dir, then
`hdiutil create -volname "Magnite 0.1.0-test.1" -srcfolder … -ov -format UDZO -fs HFS+`.

```sh
bash packaging/macos/build_dmg.sh
hdiutil verify dist/Magnite-0.1.0-test.1-arm64.dmg
shasum -a 256 dist/Magnite-0.1.0-test.1-arm64.dmg | tee -a dist/SHA256SUMS
ls -lh dist/Magnite-0.1.0-test.1-arm64.dmg
```
**Acceptance:** `hdiutil verify` passes; the `.dmg` mounts, shows `Magnite.app` plus an
`Applications` alias, and the app inside launches `--theme-check` from the mounted volume;
SHA-256 appended to `dist/SHA256SUMS` and recorded in `LOG.md`.
**Idempotent:** yes (`-ov` overwrites). Retry: `rm -f dist/Magnite-*-arm64.dmg` and re-run.
**Rollback:** delete the artifact. **`dist/` stays untracked — never commit the `.dmg`.**
**Depends on:** Phase 5 (Phase 6 only gates what the README may claim).

---

### Phase 8 — Functional verification of the installed app · model: Sonnet · **status: TODO**
**Goal:** run the full acceptance set against the installed bundle, not the build tree.

```sh
hdiutil attach dist/Magnite-0.1.0-test.1-arm64.dmg
cp -R /Volumes/Magnite*/Magnite.app "$TMPDIR/Magnite-verify.app" && hdiutil detach /Volumes/Magnite*
A="$TMPDIR/Magnite-verify.app/Contents/MacOS/magnite-app"
export MAGNITE_HOME="$TMPDIR/magnite-verify-home" HF_HOME=build/packaging/macos/test-cache
"$A" --theme-check && "$A" --ui-check && "$A" --selftest
MAGNITE_SELFTEST_PULL=1      "$A" --selftest
MAGNITE_SELFTEST_INFERENCE=1 "$A" --selftest
MAGNITE_SELFTEST_TRAINING=1  "$A" --selftest
MAGNITE_SELFTEST_ADAPTER=1   "$A" --selftest
MAGNITE_SELFTEST_RAG=1       "$A" --selftest
"$TMPDIR/Magnite-verify.app/Contents/Resources/runtime/python/bin/python3" -I -B backend/tests/live_workflow.py
"$A" --screenshot docs/release/macos/screenshots
```
**Acceptance:** every selftest prints `[selftest] PASS (0 failures)`; `live_workflow.py`
completes train → adapter → chat → RAG → export/import; each command's result and log path
appended to `LOG.md`. Any failure goes to `ISSUES.md` and blocks the checklist tick for that line.
**Idempotent:** yes, with a disposable `MAGNITE_HOME` each run. Retry: `rm -rf "$TMPDIR/magnite-verify-home"`.
**Rollback:** nothing to revert; evidence only.
**Depends on:** Phases 6–7.

---

### Phase 9 — Process cleanup, upgrade and removal · model: Sonnet · **status: TODO**
**Goal:** prove the lifecycle behaviours the checklist names, including that user data survives upgrade and removal.

Steps: launch the app; capture the backend PID; quit normally and confirm the child exits;
relaunch and `kill -9` the GUI, then check for a surviving `backend.main` (R8); install
`0.1.0-test.1` over an existing copy in `/Applications` and confirm `~/.magnite` and the
`Magnite LLM` preferences plist are intact; delete the `.app` and confirm user data remains
and nothing is left running or in `~/Library/LaunchAgents`.

```sh
pgrep -fl "backend.main"              # before, between and after each step
ls -la ~/.magnite ~/Library/Preferences/com.magnite.magnite-llm.plist 2>/dev/null
ls ~/Library/LaunchAgents ~/Library/Application\ Support/Magnite* 2>/dev/null
```
**Acceptance:** a written table in `LOG.md` with the observed result for each of: clean quit,
force-kill, upgrade-in-place, removal. Expected and acceptable finding: the force-kill case
leaves an orphan (R8) — recorded in `ISSUES.md` as a functional bug, not silently patched.
**Idempotent:** yes. **Rollback:** reinstall from the `.dmg`.
**Depends on:** Phase 8.

---

### Phase 10 — Clean-install proxy verification (second user account) · model: Sonnet · **status: TODO**
**Goal:** prove the bundle depends on nothing from the developer account, under a quarantined download.

Steps: write `docs/release/macos/CLEAN_INSTALL.md` as a tester script; set
`xattr -w com.apple.quarantine …` on the `.dmg` to simulate a web download; from a second
macOS user account with no checkout and no Homebrew on `PATH`, copy the `.dmg`, install to
`/Applications`, record the exact Gatekeeper dialog, use **System Settings → Privacy &
Security → Open Anyway**, then run `--theme-check`, `--selftest` and one real chat.

```sh
xattr -w com.apple.quarantine "0181;$(printf %x $(date +%s));Safari;" dist/Magnite-0.1.0-test.1-arm64.dmg
xattr -p com.apple.quarantine dist/Magnite-0.1.0-test.1-arm64.dmg
# then, in the second account's session, follow docs/release/macos/CLEAN_INSTALL.md
```
**Acceptance:** the app launches and passes `--selftest` in the second account; the exact
Gatekeeper wording and the steps that cleared it are recorded verbatim. Recorded as a
**proxy** — the clean-machine checklist line stays unticked.
**Idempotent:** yes. Retry: `xattr -d com.apple.quarantine …`, delete the installed copy.
**Rollback:** remove the app and the second account's `~/.magnite`.
**Depends on:** Phase 7 (ideally after 9).
**Needs you:** a second macOS user account must exist and I need to be told its name, or you run the script there yourself.

---

### Phase 11 — Developer ID signing and notarization · model: Sonnet · **status: BLOCKED**
**Goal:** the public-distribution gate. Cannot start without credentials.

Blocked on, exactly: (1) an Apple Developer Program enrollment; (2) a **Developer ID
Application** certificate + private key in the login keychain (`security find-identity -v
-p codesigning` must list it; today it lists `0 valid identities found`); (3) the Team ID;
(4) an app-specific password stored via
`xcrun notarytool store-credentials magnite-notary --apple-id … --team-id … --password …`.

Planned work once unblocked: `packaging/macos/sign_and_notarize.sh` — deep sign with the
Developer ID identity, `--options runtime`, and entitlements
`com.apple.security.cs.disable-library-validation` plus
`com.apple.security.cs.allow-unsigned-executable-memory` (required because the bundle loads
unsigned-by-Apple Python extension modules and JIT-ish ML code); submit the `.dmg` with
`notarytool submit --wait`; `xcrun stapler staple`; verify with
`spctl -a -vvv -t install` and `codesign -dv --verbose=4`.
**Acceptance:** `spctl -a -vvv -t install dist/…dmg` reports `accepted  source=Notarized Developer ID`.
**Depends on:** Phase 7. **Until then:** every artifact and doc says *unsigned testing build*.

---

### Phase 12 — Checksums, provenance, docs and checklist · model: Sonnet · **status: TODO**
**Goal:** make the artifact reproducible-on-paper and tick only what has logged evidence.

Steps: write `docs/release/macos/V0_1_MACOS_TESTING.md` in the shape of
`docs/release/V0_1_LINUX_TESTING.md` — artifact name/size/SHA-256, host OS/compiler/Qt/Python
versions, resolved dependency list location, exactly what was verified, and an explicit list
of what is **not** claimed (no notarization, no clean second machine, no Intel, no macOS 14/15
testing, whatever Phase 6 ruled out). Update `packaging/README.md` with a macOS section,
`README.md`'s verification matrix, and `packaging/macos/TESTING.md` for testers. Then tick
**only macOS lines** in `RELEASE_CHECKLIST.md` that have logged evidence — realistically the
first three; the signing line stays unticked with a BLOCKED note.

```sh
shasum -a 256 -c dist/SHA256SUMS
git diff --stat
git add -A docs/release/macos packaging RELEASE_CHECKLIST.md README.md
git commit -m "release(macos): v0.1 testing dmg provenance and verification record"
```
**Acceptance:** `shasum -c` passes; `git diff dev -- RELEASE_CHECKLIST.md` touches only lines
in the `### macOS .dmg — pending` block; every ticked line cites evidence that exists in
`LOG.md`; no `dist/` artifact or weight is staged (`git status --porcelain dist/` → ignored).
**Idempotent:** yes. **Rollback:** `git revert` the commit.
**Depends on:** Phases 8–10 (11 if unblocked).

---

## Shared-file edit policy (per your rules)

Only these shared files are expected to change, all additively and platform-guarded, each
logged in `DECISIONS.md` with offline-regression + CMake-build evidence in `LOG.md`:

| File | Change | Guard |
|---|---|---|
| `app/CMakeLists.txt` | bundle target, Info.plist, icon, `BUNDLE DESTINATION` | `if(APPLE)` + `option(MAGNITE_MACOS_BUNDLE OFF)` |
| `app/src/backend_manager.cpp` | `Contents/Resources/runtime` probe; no `LD_LIBRARY_PATH` hop | `#ifdef Q_OS_MAC` |
| `packaging/prepare_runtime.py` | macOS `CMAKE_ARGS`, no `--torch-backend cpu` | `sys.platform == "darwin"` |
| `packaging/README.md`, `README.md`, `RELEASE_CHECKLIST.md` | macOS sections / macOS lines only | n/a |

Everything else lives in `packaging/macos/` and `docs/release/macos/`. The Linux
`packaging/build_appimage.sh`, `dist/` AppImage and `docs/release/V0_1_LINUX_TESTING.md`
are read-only references.

## Verification summary (how we know it worked, end to end)

1. `bash packaging/macos/run_regressions.sh` — after every shared-file edit (wraps the unittest run with a symlink-resolved `TMPDIR`; ISSUE-005).
2. `./build-macos/magnite-app --theme-check` → `0 failure(s)`.
3. `packaging/macos/verify_bundle.py` → no non-arm64 slices, no host paths, minos ≤ 14.0, codesign valid.
4. The five `MAGNITE_SELFTEST_*` runs plus `--ui-check` and `--selftest` against the app copied out of the mounted `.dmg`, with a disposable `MAGNITE_HOME`.
5. `backend/tests/live_workflow.py` under the bundled interpreter — train → adapter → chat → RAG → export/import.
6. Second-user-account install of a quarantined `.dmg`, launched through *Open Anyway*.
7. `shasum -a 256 -c dist/SHA256SUMS`.

## Model guidance

Opus for Phase 0 (this), Phase 4 (path-discovery design), Phase 6 (ML matrix judgment), and
any phase that fails the same way twice. Sonnet for everything else — the commands above are
written to be executed without re-deriving anything.
