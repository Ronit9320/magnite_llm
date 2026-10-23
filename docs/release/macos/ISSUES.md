# ISSUES — macOS v0.1 packaging

Failures, limitations and workarounds. Each gets a root cause once known. Open
issues must be reflected in `STATUS.md` blockers when they gate a phase.

---

## ISSUE-001 — No parent-death signal on macOS: force-killed GUI can orphan the backend

- Status: **OPEN — to be verified in Phase 9, not fixed as part of packaging**
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

---

## ISSUE-002 — No Metal shader compiler on the build host

- Status: **RESOLVED for build; load-time cost remains a documented limitation (measure in Phase 6)**
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

- Status: **OPEN — reported, not fixed (application/test source is out of scope for packaging)**
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

## ISSUE-007 — Pinned `huggingface_hub` 1.30.0 warns on a cache written by a newer hub (found in Phase 4)

- Status: **OPEN — assess in Phase 6 (ML matrix) / Phase 9 (functional pass)**; no change made
- Found: 2026-10-23, in the Phase 4 GREEN selftest (`build/packaging/phase4-green.log`, 6 occurrences)
- Severity: cosmetic on this host (selftest PASS, `GET /models` 200, 6 models); possible model-listing gap on testers' machines
- Evidence: `Hugging Face cache: Repo path is not a valid HuggingFace cache directory: ~/.cache/huggingface/hub/blobs`,
  logged from `backend/src/backend/models/registry.py:165` (`scan_cache_dir` warnings)

Root cause: `~/.cache/huggingface/hub/blobs/` (with a `.huggingface-shared-blobs` marker) is a shared-blobs layout created on
2026-10-02 by the dev `.venv`'s **huggingface_hub 1.33.0**. The bundled runtime pins **1.30.0** (`requirements-macos-arm64.txt`,
same pin as Linux), whose `scan_cache_dir` does not know that top-level directory and reports it. The plain dev build (1.33.0)
does not warn on the same cache. Not caused by the Phase 4 edit.

Open question for Phase 6/9: whether models whose files live in the shared `hub/blobs` store are listed, sized and deleted
correctly by 1.30.0 (cf. `test_hf_cache_deletion_preserves_shared_blobs`). A tester whose cache was written by newer HF tools
would hit the same path. Options if it matters: raise the pin on both platforms (a Linux-affecting decision), or document it.

---

## ISSUE-008 — Transitive Python dependencies are not pinned; the runtime is not reproducible (found in Phase 5)

- Status: **OPEN — decide before the tester build (Phase 7/12)**; affects Linux the same way
- Found: 2026-10-23, Phase 5 `--fresh-runtime` rebuild vs the Phase 3 package list
- Severity: reproducibility/provenance; no functional failure observed
- Evidence: `build-info/python-packages.txt` vs `docs/release/macos/runtime-packages-macos.txt`: `certifi==2026.10.19` →
  `certifi==2026.10.23` within ~4 hours; the 17 direct pins held

`requirements-macos-arm64.txt` (like Linux's `requirements-cpu.txt`) pins only the 17 direct dependencies; the other ~64 resolve to
whatever is newest at build time. Two builds of the same commit can therefore ship different code. Options: generate a full lock
(`uv pip compile` → a constraints file committed next to the requirements) for macOS, or for both platforms. The Linux side is a
shared decision, so not changed here. `runtime-packages-macos.txt` now records the Phase 5 set.

---

## ISSUE-009 — No Qt license texts are bundled (found in Phase 5)

- Status: **OPEN — release/licensing review (Phase 12)**; not a packaging fix
- Found: 2026-10-23, preparing Phase 5 notices
- Severity: licensing compliance for distribution; nothing functional

The bundle ships 6 Qt frameworks and 12 plugins (LGPLv3 for Qt Base/Svg under the open-source terms), but the pinned Qt install at
`MAGNITE_QT_ROOT` contains **no** `Licenses/` directory or license files (`find … -iname '*licen*'` → nothing). The Linux recipe copies
Arch's `/usr/share/licenses/qt6-*`; macOS has no equivalent. `THIRD_PARTY_NOTICES.md` already says Qt notices must be preserved
"before shipping a binary". Fix belongs to the release review: obtain the Qt 6.8.3 license texts from the Qt source/installer and
bundle them under `Contents/Resources/licenses/qt/` (plus the LGPL relinking note). Not invented here.
