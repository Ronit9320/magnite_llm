# ISSUES — macOS v0.1 packaging

Failures, limitations and workarounds. Each gets a root cause once known. Open
issues must be reflected in `STATUS.md` blockers when they gate a phase.

---

## ISSUE-001 — No parent-death signal on macOS: force-killed GUI can orphan the backend

- Status: **FIXED (2026-10-24, uncommitted)** — macOS parent watchdog in `backend/main.py`; verified on the packaged app: backend exits 1.0 s after `kill -9` of the GUI (2/2)
- Found: 2026-10-23 (code reading during the Phase 0 feasibility check)
- Severity: functional bug, user-visible (stray `python` process holding a port and RAM)

`backend/src/backend/main.py:14` defines `_enable_parent_death_signal()`, whose
docstring states the intent plainly: "Ask the kernel to SIGTERM us if our parent
process dies, so a crashed or force-killed GUI cannot leave an orphaned backend
behind." The implementation is guarded by `if sys.platform == "linux" and
os.name == "posix"` and uses Linux's `prctl(PR_SET_PDEATHSIG)`. macOS has no
`prctl`, so on macOS the function is a no-op and the stated protection does not
exist.

The cooperative path is unaffected: `BackendManager::stop()` terminates and then
kills the child, so a normal quit is clean. The gap is the non-cooperative path —
`kill -9` of the GUI, or a GUI crash — which `RELEASE_CHECKLIST.md` names under
"backend process cleanup" and which the existing audit already lists as
unverified ("forced-process-death coverage remain unverified").

Not fixed here, per the working rule to report functional bugs rather than
silently change application source during packaging. Phase 9 will measure the
actual behaviour and record it.

Possible fixes, if the maintainer wants one later (each platform-guarded):

1. A kqueue `EVFILT_PROC`/`NOTE_EXIT` watcher on the parent PID in the backend,
   the closest macOS equivalent to `PR_SET_PDEATHSIG`.
2. A small watchdog thread polling `os.getppid()` and exiting when it changes to
   1 — simplest and portable, with poll-interval latency.
3. Client-side only: a `atexit`/signal-handler guarantee in the GUI, which does
   not help on `SIGKILL` and so does not actually close the gap.

Recommendation if asked: option 2 for v0.1 (small, guarded, testable), option 1
if a cleaner solution is wanted.

**Phase 9 measurement (2026-10-23, installed `/Applications/Magnite.app`, launched through LaunchServices):**
- `kill -9` of the GUI (pid 52274) → backend 52282 is **still alive at t+5 s, t+30 s and t+60 s**, reparented to launchd (`ppid=1`),
  and still answering `GET /health` → `{"status":"ok"}`.
- **Orphans accumulate.** Relaunching the app starts a second backend (52552, child of the new GUI) next to the orphan. Both hold a
  `~/.magnite/*.db` file open at the same time (`lsof`). Quitting the new app normally stops only its own backend; the orphan stays.
  Every crash or force-quit therefore leaves one more backend, along with its RAM (GBs if a model was loaded), a port, and concurrent
  writers to the same SQLite stores.
- The cooperative path is clean: a normal quit stops the backend, measured twice.

Severity is therefore higher than first written: a user who force-quits a hung app, then relaunches, ends up with two backends
on one data directory. Recommendation stands (option 2, platform-guarded, in `backend/main.py`). It needs the maintainer's approval
because it changes application source.

**Fix (2026-10-24, approved "fix all").** On macOS, `_enable_parent_death_signal()` now starts a `parent-watch` thread (option 2 above).
It polls `os.getppid()` every second. Once the backend has been reparented, it SIGTERMs itself, which takes uvicorn's graceful shutdown;
if shutdown stalls, it calls `os._exit(1)` after 15 s. Linux keeps `prctl`. It is skipped when the parent is launchd (pid 1).
- Tests: `backend/tests/test_parent_watch.py`, including a real macOS test that `kill -9`s a parent process. It was **RED** before the change
  ("backend child survived its parent's SIGKILL") and **GREEN** after.
- Packaged app (dmg build): GUI `kill -9` → backend gone in **1.0 s**, in 2 of 2 rounds, with nothing left running.

---

## ISSUE-002 — No Metal shader compiler on the build host

- Status: **RESOLVED for build; first-ever-load cost (~10 s, once per user) is a documented limitation** — measured in Phase 6
- Found: 2026-10-23
- Severity: build-time constraint

`xcrun -sdk macosx metal --version` fails with "unable to find utility
\"metal\", not a developer tool or in PATH" because only Command Line Tools 14.3
are installed (no `/Applications/Xcode.app`). A default `GGML_METAL=ON` build of
`llama-cpp-python` compiles `.metal` sources into a `default.metallib` and needs
that utility.

Planned mitigation: `-DGGML_METAL_EMBED_LIBRARY=ON`, which embeds the shader
source and lets the Metal driver compile it at runtime, requiring no build-time
toolchain. If that fails twice, fall back to `-DGGML_METAL=OFF` (CPU-only
llama.cpp) and record it as a documented limitation rather than installing Xcode
for v0.1. Either way the outcome is measured in Phase 6, not assumed.

**Phase 1 update (2026-10-23, measured):** `uv pip install -e backend` built
`llama-cpp-python==0.3.36` from sdist in ~2 minutes with **default flags and no
Metal compiler present**, and the result reports `llama_supports_gpu_offload() == True`
on the M1. The log shows `ggml_metal_library_init: using embedded metal library`, so
this llama.cpp version embeds the shaders by default and compiles them at runtime.
This confirms the mitigation works on this host.

Two things this does **not** establish, deliberately left for later phases:

1. The pinned version is `0.3.35`, not `0.3.36`; the dev venv is unpinned. The pinned
   sdist must be rebuilt with explicit flags in Phase 3 and checked again.
2. Runtime shader compilation has a **startup cost**: `loaded 34 libraries from
   embedded data in 10.195 sec`. A user's first GGUF load on macOS may therefore
   pause ~10 s before the first token. Phase 6 should measure it on the packaged
   runtime and `TESTING.md` should say so; whether it is cached across launches
   is unknown.

**Phase 3 update (2026-10-23, measured):** the *pinned* `llama-cpp-python==0.3.35`, built from sdist through
`packaging/prepare_runtime.py` with `-DGGML_METAL=ON -DGGML_METAL_EMBED_LIBRARY=ON -DGGML_NATIVE=OFF`, reports
`llama_supports_gpu_offload() == True` in the isolated runtime. The embedded Metal library took **10.3 s** to load
(`ggml_metal_library_init: loaded in 10.292 sec`) — consistent with Phase 1's 10.2 s on 0.3.36, so a first GGUF load on
macOS pauses ~10 s. Whether it is cached across launches is still unknown; Phase 6 measures it. The CPU-only fallback was
not needed.

**Phase 6 update (2026-10-23, measured): the cost is paid once per macOS user, not per launch.** Each llama.cpp cell of the
ML matrix runs in a fresh process. In all of them the log shows `ggml_metal_library_init: using embedded metal library`, then
`loaded in 0.013 sec` / `0.014 sec` / `0.050 sec`, against `10.292 sec` in Phase 3. Whole model loads (135M GGUF) took 1.0–1.2 s
in a new process and 0.5–0.7 s in the same process. The driver caches the compiled library per user under
`$(getconf DARWIN_USER_CACHE_DIR)com.apple.metal/<compiler-id>/` (46 MB here; it presumably resets with a macOS/Metal compiler
update). I did **not** clear that system cache to re-measure the cold case. The 10.3 s from Phase 3 stands as the cold number,
and Phase 10's second account will measure the true first-run cost. `TESTING.md` (Phase 12) should say: "the very first chat
after installing may pause ~10 s while macOS prepares GPU shaders; later loads are fast".

**Phase 10 update (2026-10-28, maintainer report):** in the second account `tester`, both chat replies with the 135M GGUF came
in "under a second or so"; no pause was noticed. Only time-to-first-words was observed; the model **load**, where the compile
happens, was not timed, and the backend log was removed by cleanup. The cold cost in a new account is therefore **unverified**.
Keep the `TESTING.md` sentence, but phrase it as "may pause ... when a model is first loaded".

---

## ISSUE-003 — Unsigned build will trip Gatekeeper on download

- Status: **ACCEPTED LIMITATION for v0.1** (see `adr/0005-signing-approach.md`)
- Found: 2026-10-23
- Severity: usability, public-distribution blocker

`security find-identity -v -p codesigning` reports `0 valid identities found`, so
the v0.1 `.dmg` can only be ad-hoc signed. A downloaded copy carries
`com.apple.quarantine`, and macOS will refuse the first launch until the user
goes to System Settings → Privacy & Security → **Open Anyway** (the older
right-click → Open bypass no longer applies on recent macOS). App Translocation
may also run the bundle from a random read-only path until it is moved to
`/Applications`.

Consequences, all already in the plan: the artifact is labelled an unsigned
testing build; `CLEAN_INSTALL.md` (Phase 10) records the exact dialog wording and
the steps that clear it; the macOS public-distribution checklist line stays
unticked; Phase 11 stays BLOCKED.

**Phase 10 update (2026-10-28):** the quarantined download (`0181;…;Safari;`) was blocked and then cleared through Privacy &
Security → Open Anyway in a Standard second account. After that the app ran from `/Applications`, so there was no translocation.
The **verbatim dialog wording was not captured** (no screenshots, nothing transcribed). Capture it before `TESTING.md` quotes it,
or describe the flow without quoting.

---

## ISSUE-004 — Build-host disk headroom is tight

- Status: **OPEN — monitor each phase**
- Found: 2026-10-23
- Severity: could stall the plan if ignored

`df -h` shows ~20 GiB free (91% used) on the data volume, and 8 GiB of RAM. The
staged bundle will carry Qt frameworks plus torch and llama.cpp, and the uv cache
and a test HF model cache sit under `build/packaging/`. Mitigation: record
`du -sh` at each phase in `LOG.md`, keep caches under `build/packaging/`, and
prune the uv cache after a successful runtime build. 8 GiB of RAM also caps which
models can be used for real-model verification — tiny toy models remain the
default, which is a limitation to state, not to hide.

---

## ISSUE-005 — Backend tests fail on macOS because `/var` is a symlink to `/private/var`

- Status: **FIXED (2026-10-28, uncommitted)** — `_remove_hf` compares resolved paths; tests use a resolved temp root; a plain `unittest discover` now passes (60/60)
- Found: 2026-10-23, Phase 1 (`PYTHONPATH=backend/src .venv-macos/bin/python -m unittest discover -s backend/tests -v`)
- Severity: low for users, but it breaks the "regressions all pass" gate on a stock Mac
- Evidence: `build/packaging/phase1-unittest.log` (4 FAIL + 1 ERROR of 53) versus
  `build/packaging/phase1-unittest-resolvedtmp.log` (53 tests, OK, 0 skipped)

**What fails** (all in `backend/tests/`): `test_cache_discovery.py` — 4 tests
(`test_external_cache_is_discovered_offline_and_filtered`,
`test_main_preferred_and_existing_revision_metadata_preserved`,
`test_missing_registered_revision_replaced_by_cached_revision`,
`test_list_endpoint_registers_external_model_for_inference_resolution`) and
`test_regressions.py::StoreTests::test_hf_cache_deletion_preserves_shared_blobs`.

**Root cause.** `tempfile` returns `/var/folders/…` on macOS, which is a symlink to
`/private/var/folders/…`. The backend returns the *resolved* path (`/private/var/…`),
so the four assertions compare `/private/var/…` against `/var/…`. The fifth failure is
the same mismatch on the product side: `ModelRegistry._remove_hf`
(`backend/src/backend/models/registry.py:254`) builds `Path(rec.path).absolute()`, which
keeps the symlink, then compares it with `huggingface_hub`'s `revision.snapshot_path`,
which does not; no revision matches, so it raises `ValueError("Cannot safely identify
cached model revision; files were not removed")`.

**Proof, not assumption.** Re-running with `TMPDIR` set to the already-resolved
directory (`/private/var/folders/…/T`) — no source change — turns every failure green:
`Ran 53 tests … OK`. That isolates symlink handling as the only cause.

**Product impact.** Narrow and fail-safe: nothing is deleted when the match fails. It can
only affect a macOS user whose Hugging Face cache path itself passes through a symlink
(e.g. `HF_HOME` on a symlinked folder), who would be unable to delete a cached model from
the app. A default `~/.cache/huggingface` is not under a symlink.

**Caveat.** The dev venv resolved `huggingface-hub 2.3.0`, whereas the shipped pin is
`1.30.0`. The behaviour under the pinned version is untested until Phase 3/4.

**Workaround used for verification** (recorded in `LOG.md`): run the suite with
`TMPDIR="$(python -c 'import os,tempfile;print(os.path.realpath(tempfile.gettempdir()))')"`.
Phases that re-run the suite after shared-file edits must use the same prefix.

**Possible fixes if wanted** (each tiny): in the tests use
`Path(tempfile.mkdtemp()).resolve()`; in `_remove_hf` use `.resolve()` instead of
`.absolute()` (and confirm it still refuses paths outside a managed `snapshots` dir).
Linux CI is unaffected, which is why this was not caught earlier.

**Fix (2026-10-28, maintainer: "continue the fixes").**
- `ModelRegistry._remove_hf` now uses `Path(rec.path).resolve()` and compares it with `Path(revision.snapshot_path).resolve()`. The
  "managed `snapshots/<rev>` only" guard is unchanged.
- `test_cache_discovery.py` resolves its temp root, because the backend correctly reports resolved paths.
- New test `test_hf_cache_deletion_through_symlinked_cache_path` runs on every platform, with a symlinked cache dir:
  - with the fix removed (file-copy swap) → `ValueError: Cannot safely identify cached model revision`;
  - with the fix → OK.
- A plain `PYTHONPATH=backend/src python -m unittest discover -s backend/tests` (no `TMPDIR` workaround) → **60 tests OK**.
  `run_regressions.sh` still works but no longer needs its workaround.

---

## ISSUE-006 — The staged runtime is not yet relocatable/clean (found in Phase 3)

- Status: **RESOLVED in Phase 5** (2026-10-23) — `finalize_bundle.py` fixes all three; `verify_bundle.py` enforces them (see the resolution note at the end of this issue)
- Found: 2026-10-23, by scanning the staged runtime after the Phase 3 build
- Severity: hygiene/relocatability; none affects the app today because it only runs `bin/python3 -m backend.main`
- Evidence: `LOG.md` Phase 3 §4; helper `packaging/macos/scan_macho.py`

Three separate findings, all consequences of staging a stock uv/PyPI install:

1. **3 universal (x86_64+arm64) extension modules** ship in arm64-only bundle: `uvloop/loop.cpython-312-darwin.so`,
   `charset_normalizer/cd.cpython-312-darwin.so`, `charset_normalizer/md.cpython-312-darwin.so`. They violate ADR 0001's
   "no non-arm64 slice" rule. Fix: `lipo -thin arm64` them (Phase 5), then re-sign.
2. **31 console scripts in `runtime/python/bin` carry a build-host shebang**, e.g.
   `#!/Users/architbhuta/magnite_llm/build/packaging/macos/stage/Magnite.app/…/python3`. They break as soon as the bundle
   moves and leak a developer path. The app never calls them. Fix: delete everything in `bin/` except `python`, `python3`,
   `python3.12`.
3. **Informational build-host paths** in `lib/python3.12/_sysconfigdata__darwin_darwin.py` (uv stamps its install dir) and
   `site-packages/magnite_backend-0.1.0.dist-info/direct_url.json`. Harmless at runtime. Fix: scrub the prefix so
   `verify_bundle.py` can fail on *any* `/Users/` text hit without an allowlist.

Also: `build-info/build.json` recorded `"working_tree_modified": true` because Phase 3 ran on uncommitted edits. That is
accurate, but a package intended for testers should be built from a clean committed tree (Phase 5/12).

Positive result from the same scan, relevant to ADR 0002: of **372 Mach-O files**, **none** has `minos` above 14.0 (histogram:
11.0×85, 12.0×107, 14.0×180). The 14.0 floor is real (torch + llama.cpp), not an artefact.

**Resolution (Phase 5).** `finalize_bundle.py` thins every universal Mach-O (21: the 3 above plus 17 Qt frameworks/plugins and
`libqoffscreen`), deletes 42 `bin/` entries (keeps `python`, `python3`, `python3.12`), restores the `/install` prefix in
`_sysconfigdata`, and removes `direct_url.json` (backend **and** pip's). The stricter gate then found more, all fixed the same phase:

4. **llama.cpp linked Homebrew OpenSSL.** `libllama-common` (in the stray `site-packages/lib/` copy) referenced
   `/opt/homebrew/opt/openssl@3/lib/libssl.3.dylib` / `libcrypto.3.dylib`: `LLAMA_OPENSSL` defaults ON and CMake found the host's
   Homebrew. Not loaded by `llama_cpp` today (it uses `llama_cpp/lib`), so the Phase 3 import check could not see it, but it would
   fail on any Mac without Homebrew. Fixed at source (`-DLLAMA_OPENSSL=OFF`, darwin only) **and** by removing llama-cpp-python's
   CMake development install (`site-packages/lib`, `site-packages/include`: duplicate dylibs, headers, cmake/pkgconfig; 27 MB).
5. **Build-host paths inside binaries.** `libpython3.12.dylib`'s install id was the absolute download path (uv rewrites it) →
   reset to `@rpath/libpython3.12.dylib`; llama.cpp dylibs carried `__FILE__` paths from the uv cache → rebuilt with
   `-ffile-prefix-map=<repo>=.`. (My Phase 5 pre-scan said "no binaries" — that grep pipeline was wrong; the gate was right.)
6. **Absolute `LC_RPATH`s from upstream wheel CI**: 51 in scikit-learn (`/Users/runner/micromamba/envs/build/lib`), 3 in scipy
   `_fblas` (`/opt/homebrew/Cellar/gcc@13/...`). `/opt/homebrew` is user-writable, so with ad-hoc signing (no library
   validation) a dylib planted there could be loaded first. Deleted (54) — only after `finalize_bundle.py` proved every
   dependency of each file still resolves inside the bundle without them.

---

## ISSUE-007 — Pinned `huggingface_hub` 1.30.0 cannot free disk for models in a shared-blobs cache (found in Phase 4, measured in Phase 6)

- Status: **FIXED (2026-10-24, uncommitted)** — `huggingface_hub` pinned to 1.33.0 on both platforms; bundled runtime: delete leaves 244 B, not 1.83 GB
- Found: 2026-10-23, Phase 4 GREEN selftest (`hub/blobs` warning); measured in Phase 6 with `packaging/macos/probe_hf_cache.py`
- Severity: **disk leak on delete** for testers whose HF cache was written by huggingface_hub ≥ 1.33; listing is unaffected
- Evidence: `build/packaging/macos/hf-probe/probe-{130,133}.json` (untracked)

Background: huggingface_hub ≥ 1.33 can keep blob content once in `<hub>/blobs/xx/<sha>` (marker `.huggingface-shared-blobs`) and make each
repo's `blobs/<sha>` a symlink to it. This Mac's real cache has that layout: written on 2026-10-02 by the dev `.venv`'s 1.33.0; 3 of 6 repos
use it. The bundled runtime pins **1.30.0**, the same pin as Linux.

Measurement: two APFS clones of the real 6 GB cache. On each, the backend's own code ran under one hub version:
`scan_cache_dir`, as `_record_hf_models` uses it, then `ModelRegistry._remove_hf` on `HuggingFaceTB/SmolLM2-135M-Instruct`.

| | hub 1.30.0 (bundled) | hub 1.33.0 (dev venv) |
|---|---|---|
| scan warnings | 1 (`…/hub/blobs` "not a valid HuggingFace cache directory") | 0 |
| repos listed / sizes / file counts | 6, identical to 1.33 (e.g. SmolLM2 1,832,693,206 B, 24 files) | 6 |
| `_remove_hf` | `ok`; repo dir fully removed; repo gone from the next scan | same |
| dangling symlinks in *other* repos afterwards | 0 | 0 |
| shared-store bytes left unreferenced afterwards | **1,829,250,588 (≈1.83 GB)** | 244 |

So listing is correct and deletion never corrupts another model. Under the pinned 1.30, though, **deleting a model whose blobs live in
the shared store frees none of its disk space**. The UI reports it deleted while ≈ the model's full size stays in `~/.cache/huggingface/hub/blobs`.
1.30 never *creates* that layout, so only caches written by newer HF tools are affected. That is plausible for exactly the testers who
already use HF tooling.

Options (need a decision; both affect Linux):
1. Raise the `huggingface_hub` pin to ≥ 1.33 on both platforms, then re-run regressions and the Phase 8 selftests. This is the real fix.
2. Keep 1.30 and document "deleting a model downloaded by other HF tools may not free disk; run `hf cache delete`".

**Fix (2026-10-24, approved).** `huggingface_hub==1.33.0` in `packaging/requirements-cpu.txt` (Linux) and
`packaging/macos/requirements-macos-arm64.txt`. Re-measured with the rebuilt bundle on a fresh clone of the real cache: hub 1.33.0, 0 warnings,
6 → 5 repos, delete `ok`, 0 dangling links, **244 bytes** left unreferenced (`hf-probe/probe-bundled-133.json`). Linux was resolved but not built
or run here.

---

## ISSUE-008 — Transitive Python dependencies are not pinned; the runtime is not reproducible (found in Phase 5)

- Status: **FIXED (2026-10-24, uncommitted)** — full locks via `packaging/lock_requirements.sh`; the runtime installs with `-c constraints-*.txt`; the installed set equals the lock
- Found: 2026-10-23, Phase 5 `--fresh-runtime` rebuild vs the Phase 3 package list
- Severity: reproducibility/provenance; no functional failure observed
- Evidence: `build-info/python-packages.txt` vs `docs/release/macos/runtime-packages-macos.txt`: `certifi==2026.10.19` →
  `certifi==2026.10.23` within ~4 hours; the 17 direct pins held

`requirements-macos-arm64.txt` (like Linux's `requirements-cpu.txt`) pins only the 17 direct dependencies; the other ~64 resolve to
whatever is newest at build time. Two builds of the same commit can therefore ship different code. Options: generate a full lock
(`uv pip compile` → a constraints file committed next to the requirements) for macOS, or for both platforms. The Linux side is a
shared decision, so not changed here. `runtime-packages-macos.txt` now records the Phase 5 set.

**Fix (2026-10-24, approved).** `packaging/lock_requirements.sh` runs `uv pip compile` over the direct pins and
`backend/pyproject.toml`, producing `packaging/constraints-cpu.txt` (Linux x86_64, CPU torch, 80 packages) and
`packaging/macos/constraints-macos-arm64.txt` (79). `prepare_runtime.py` installs with `-c` on the lock next to the requirements file.
- The macOS lock equals the previously shipped set except the intended hub bump.
- The rebuilt runtime's package list equals the lock exactly.
- Linux: the install command gains only `-c packaging/constraints-cpu.txt` (harness diff). It was resolved but not run here.

---

## ISSUE-009 — No Qt license texts are bundled (found in Phase 5)

- Status: **FIXED for engineering (2026-10-28, uncommitted); legal sign-off still needed** — Qt LGPL/GPL texts + 37 third-party notices bundled in `Contents/Resources/licenses/qt/`
- Found: 2026-10-23, preparing Phase 5 notices
- Severity: licensing compliance for distribution; nothing functional

The bundle ships 6 Qt frameworks and 12 plugins (LGPLv3 for Qt Base/Svg under the open-source terms), but the pinned Qt install at
`MAGNITE_QT_ROOT` contains **no** `Licenses/` directory or license files (`find … -iname '*licen*'` → nothing). The Linux recipe copies
Arch's `/usr/share/licenses/qt6-*`; macOS has no equivalent. `THIRD_PARTY_NOTICES.md` already says Qt notices must be preserved
"before shipping a binary". Fix belongs to the release review: obtain the Qt 6.8.3 license texts from the Qt source/installer and
bundle them under `Contents/Resources/licenses/qt/` (plus the LGPL relinking note). Not invented here.

**Fix (2026-10-28).** `packaging/macos/qt_notices.py` reads every `qt_attribution.json` of qtbase and qtsvg at `v6.8.3` from Qt's
GitHub mirror (59 components) and classifies each one using this Qt build's own feature flags (`mkspecs/modules/qt_lib_*_private.pri`):
- bundled: doubleconversion, pcre2, freetype, harfbuzz, jpeg, png, textmarkdownreader;
- from the system: zlib;
- disabled: vulkan, OpenGL ES.

Results:
- **37 components included**, each with its own license file, or Qt's SPDX text where the metadata names no file.
- **22 excluded with a written reason**: other OSes, disabled features, system zlib, tests and tools, QtSql not shipped.
- Qt's own `LGPL-3.0-only.txt`, `GPL-2.0-only.txt`, `GPL-3.0-only.txt` and `Qt-GPL-exception-1.0.txt` are included.
- `README.md` lists every component with its version, license, copyright and where it lives, plus where to get the Qt 6.8.3 source and how
  to swap in rebuilt frameworks.
- Any component left unclassified (for example after a Qt upgrade), or a typo in the lists, makes the script fail. It caught one typo of mine.

The notices are stored in `packaging/macos/licenses/qt/` (372 KB). `build_app.sh` copies them into `Contents/Resources/licenses/qt/`, and
`verify_bundle.py` now fails a bundle without them. All 38 downloaded component texts plus the 4 Qt texts were checked to be real license
texts (no HTML or 404 bodies).

**Not claimed:** that this satisfies LGPL obligations. That is a legal judgement for the maintainer, for example whether pointing to Qt's own
source download is a sufficient source offer. The README says it is an engineering inventory, not legal advice.

---

## ISSUE-010 — Default training settings fail on macOS: "Use 4-bit QLoRA" is on by default, and the runtime has no bitsandbytes (found in Phase 6)

- Status: **FIXED (2026-10-24, uncommitted)** — capability-aware backend/UI/selftests; the app copied from the new dmg passes TRAINING and ADAPTER (11/11 acceptance)
- Found: 2026-10-23, ML matrix (`qlora-4bit-shipped`, `qlora-8bit-shipped`) + code reading
- Severity: **high for macOS** — the default training path errors out; LoRA itself works (MPS, measured)
- Evidence: `ML_MATRIX.md`; `app/src/panels/trainingpanel.cpp:144-146` (checkbox `setChecked(true)`), `:260` (combo offers 4-bit/8-bit);
  `app/src/mainwindow.cpp:834`, `:959` (selftests send `"4bit"`)

What happens in the shipped app: the training job ends in `error` with
`Using bitsandbytes 4-bit quantization requires bitsandbytes: pip install -U bitsandbytes>=0.46.1` (8-bit: same text). A packaged-app
user cannot act on a `pip install` instruction. Linux does not hit this, because `packaging/requirements-cpu.txt` ships bitsandbytes 0.50.2;
macOS leaves it out on purpose (ADR 0004).

What bitsandbytes 0.50.2 *would* do on this Mac, measured in a throwaway runtime:
- quantized weights always load on **CPU**, even though the Trainer runs on MPS;
- 4-bit QLoRA trains (finite, falling loss) on mixed devices;
- **8-bit QLoRA fails** with `Tensor for argument #2 'mat2' is on CPU, but expected it to be on GPU`;
- 4-bit generation on the 135M model degenerates into repetition.

None of this was measured above 135M parameters.

Options (all touch shared code or the release scope, so they are the maintainer's call):
1. **Capability-aware UI** (recommended): the backend reports whether quantization is available (`bitsandbytes` importable); the Training
   panel defaults "Use 4-bit QLoRA" off and disables the 4-bit/8-bit choices when it is not. The selftests send `"none"` in that case.
   Platform-neutral, and correct for any future runtime without bitsandbytes.
2. Minimal macOS-only change: default the checkbox off and make the selftests use `"none"` on `Q_OS_MAC`. 8-bit stays selectable and
   fails.
3. Ship bitsandbytes on macOS like Linux: the default 4-bit path then trains with CPU-resident base weights, and 8-bit stays broken.
   Not recommended without measuring a realistic model size.
Until one is chosen, `TESTING.md` must tell macOS testers to untick "Use 4-bit QLoRA".

**Note for any fix (Phase 9):** the choice is *persisted*. This Mac's preferences already hold `panels.Training` with `"qlora":true,
"quantization":"4-bit (NF4)"`. Changing only the checkbox default would leave existing users on 4-bit, so a fix must also handle a saved
4-bit choice when quantization is unavailable.

**Phase 8 (2026-10-23):** reproduced on the app copied out of the `.dmg` (`5a04358`). `selftest-training` and `selftest-adapter` both
end `[selftest] FAIL: … Using bitsandbytes 4-bit quantization requires bitsandbytes …`. These are the only 2 failures in the 11-step
acceptance set. `live_workflow.py`, which sends `quantization: "none"`, passes training → adapter → RAG chat → export/import through
the same HTTP API. So LoRA itself works in the packaged app; only the 4-bit default is broken.

**Fix (2026-10-24, approved).** Option 1 (capability-aware):
- **Backend:** `quantization_support()` returns False on macOS (measured unsuitable) and wherever bitsandbytes is missing.
  `POST /training/start` refuses 4bit/8bit with that reason (HTTP 400), and there is a new `GET /training/capabilities`.
- **Training panel:** it fetches the capability, then unticks and disables "Use 4-bit QLoRA" and the precision combo, with the reason as a
  tooltip. This also overrides a saved 4-bit choice.
- **Selftests:** they request 4-bit only where it is supported, so Linux keeps its QLoRA coverage.
- Tests: `backend/tests/test_quantization_support.py`.
- Verified: dev build `MAGNITE_SELFTEST_TRAINING` PASS with `quantization=none`; `--ui-check` PASS; the app copied from the new dmg passes
  **11/11** acceptance steps, including TRAINING and ADAPTER.

---

## ISSUE-011 — The local backend API has no authentication and accepts any Host (found 2026-10-28)

- Status: **FIXED (2026-10-28, uncommitted)** — adr/0009 implemented: per-launch secret + Host allow-list + data-folder lock; the packaged app refuses outside callers (401/400)
- Found: 2026-10-28, while assessing the maintainer's question about running the backend separately
- Severity: **security** — any process of any macOS user on the same Mac can drive the backend; web pages can try via DNS rebinding
- Affects Linux too: the code is shared

**Proof (2026-10-28):** a throwaway backend (`--port 54871`, scratch `--home`) listening on `127.0.0.1:54871`, driven with plain `curl` and no
credentials:
- `GET /health` → `{"status":"ok"}`.
- `POST /datasets/ingest {"path":"/etc/hosts"}` → the backend **read the file** into a dataset (`rows 1, size_kb 0.2, source /etc/hosts`).
- `GET /datasets` with `Host: evil.example:54871` → **200**: no Host validation, so DNS rebinding is possible.

Why it matters:
- `127.0.0.1` is shared by every user account on a Mac, so another account can make *your* backend read your files (dataset ingest, adapter
  import), delete models, or start training.
- The random port only slows a scanner down.
- CORS plus FastAPI's JSON content-type check already stop ordinary cross-site requests, but not DNS rebinding.

**Fix (2026-10-28, maintainer: "do what you recommended").**
- **Backend:** new `backend/local_access.py`, wired in `backend/main.py`, so only the network-facing server is wrapped and in-process tests
  are unaffected.
  - A pure-ASGI middleware checks the Host (loopback names only, else 400) and the `Authorization: Bearer <MAGNITE_API_TOKEN>` header
    (constant-time compare, else 401). `/health` stays open.
  - `fcntl.flock` on `MAGNITE_HOME/.backend.lock` with a 5 s retry; on conflict the backend exits with code 75.
- **App:**
  - `BackendManager` makes a 256-bit secret per launch (`QRandomGenerator::system()`) and passes it in the environment.
  - `BackendClient` sends it from a single `request()` helper used by GET/POST/DELETE, streaming and download.
  - `MainWindow` shows "Magnite is already running…" on exit code 75.
- **Tests:** `backend/tests/test_local_access.py` (5): stub-app checks for 401/401/200, open `/health`, the Host allow-list including
  `[::1]` and a port-less `127.0.0.1`, and developer mode; a lock held by another process and released after `kill -9`; a launched backend
  returning 401 without the secret, 200 with it, and a second backend exiting 75. Full suite **65 OK**.
- **Packaged app** (dmg `48050836…`):

| Request | Before | After |
|---|---|---|
| no secret | 200 | **401** |
| wrong secret | — | **401** |
| ingest `/etc/hosts` without a secret | **read the file** | **401** |
| fake Host | 200 | **400** |
| `/health` | 200 | 200 |

  - A second copy on the same folder gets "Magnite is already running…", with 1 backend left and the first copy unaffected.
  - The secret is not on the command line.
  - `verify_installed.sh` **11/11**.

**Correction to adr/0009's wording.** `os.environ.pop` only keeps the secret away from the backend's child processes. macOS still shows the
process's *initial* environment through `ps -E`, but only to the same user and root. Measured: my own process's marker variable was visible;
root processes' environments were not, although their command lines were. Other accounts therefore cannot read it. The same user can, which
is outside the threat model: that user can already read `~/.magnite` and the app's memory. `CLEAN_INSTALL.md` §7b lets the second account
confirm this from its own side.
