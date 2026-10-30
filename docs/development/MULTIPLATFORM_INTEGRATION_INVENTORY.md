# Integration inventory — 2026-10-30

This records Phase 0 of [the release plan](MULTIPLATFORM_RELEASE_PLAN.md) and
the first Mac integration work. Native platform evidence from the source
branches is historical; it does not verify this integrated source.

## Sources and branch preservation

The working tree was clean on local `dev`. Fetched remote heads over SSH:

| Source | Commit | Relationship |
| --- | --- | --- |
| Local `dev`, integration starting point | `f3d8d363ce165b39ec5267cdb9f06c6a89978a76` | Remote dev plus the owner's release-plan commit |
| `origin/dev` | `af5fb1800464ed117c492a2cf4ce1ff2e9a07110` | Published Linux preview implementation and evidence |
| `origin/mac` | `86950a873dbb2568fa50242d383b45600692b18b` | 18 commits absent from local dev |
| `origin/windows-build-fix` | `2350dff5608e2a2d3892b1350df118e3024e783e` | Two commits absent from local dev |
| `origin/master` | `294c59af6aa9c74d2c614f25528817f6fe4517c4` | Stable source unchanged |

`git ls-remote --heads origin` confirmed the Windows branch is
`windows-build-fix`, not `windows`; the initial fetch of `windows` failed because
that ref does not exist. Use the actual branch without renaming or replacing it.
`release/integration` was created from local `dev`, preserving its planning
commit. All source branches and unrelated local branches remain intact.
No remote push, public release, version bump or master promotion is part of
this inventory/integration session.

The Mac/dev merge base is `0fa53e358ea44a51b6664d1b098d0546f31a1b0b`.
Windows/dev merge base is `af5fb1800464ed117c492a2cf4ce1ff2e9a07110`.
Mac's unique history spans `7fcdb3c` (raw health probe), the packaging work
through `ef173fd`, hardening `8337e65`, and verification docs `86950a8`.
Windows' unique commits are `b96aa41` (Windows compatibility) and `2350dff`
(vcpkg, VS generator and virtualenv selection).

Only the root `AGENTS.md` exists in each of the three source trees; there are
no nested platform `AGENTS.md` files. Read the root instructions, release plan,
pipeline guide, packaging guide, Mac packaging README/TESTING, STATUS, and ADRs
0002/0005/0008/0009; Windows BUILD_WINDOWS.md and both build scripts.
The old Mac STATUS contains stale uncommitted/no-merge statements and completed
phase commands. The owner's current integration request and multi-platform
plan govern this work; those historical commands are not current instructions.

## Shared changes and integration decisions

Mac changes 125 files relative to the merge base, mostly packaging, notices and
evidence. Shared changes include CMake bundle support; runtime lookup; raw
health probes; per-launch API authentication; POSIX data locks; Mac parent
supervision; cache path resolution; and training quantization capabilities.

The files changed by both Mac and subsequent dev work are `.gitignore`,
`README.md`, `RELEASE_CHECKLIST.md`, `app/CMakeLists.txt`,
`app/src/backend_manager.{cpp,h}`, `app/src/mainwindow.{cpp,h}`,
`backend/src/backend/models/registry.py`, `docs/LOCAL_API.md`,
`docs/STORAGE_AND_PRIVACY.md`, `packaging/README.md`,
`packaging/constraints-cpu.txt`, `packaging/prepare_runtime.py`, and
`packaging/record_build.py`. Actual merge conflicts were in `.gitignore`,
README, backend_manager.cpp, packaging README, CPU constraints, and runtime
preparation. Automatic merges were also reviewed for semantic differences.

- Keep Linux's canonical version, signature trust key, updater, graceful
  `stopForUpdate`, maintenance gate and startup health acknowledgement.
- Use Mac's raw socket readiness transport with Linux's exact backend version
  check retained. Bound the probe response to 64 KiB. `/health` remains public
  under the imported API middleware and exposes canonical version information.
- Authenticate regular requests, SSE and downloads through BackendClient.
  Existing quiesce/resume calls use that same client. Add a regression for
  authenticated admission, maintenance rejection and authenticated resumption.
- Keep Mac bundle runtime discovery guarded by `Q_OS_MAC`. A discovered package
  runtime cannot fall back to host Python. Preserve Linux library discovery.
- Keep Linux CPU direct requirements and transitive constraints unchanged.
  Mac's proposed CPU lock replacement drifted many unrelated transitive pins;
  retain Mac's separate ARM lock and huggingface_hub 1.33.0 there. Linux stays on
  its tested huggingface_hub 1.30.0 pending a separately tested dependency change.
  Require a dependency lock in shared runtime preparation.
- Keep the Mac `.app`/DMG build path and historical evidence. Remove the active
  `0.1.0-test.1` label override; CMake derives the display label from `_version.py`.
  Numeric short version is major.minor.patch, padding omitted patch with zero;
  CFBundleVersion uses UPDATE_SEQUENCE. The current preview maps to 0.1.0 / 1.
  Historical records retain their original labels. Stable parsing remains Phase 4.
- Preserve POSIX lock release on process exit, Mac parent-watch behavior and
  platform capability detection. Windows data locking and process supervision
  still require Phase 2 review and native implementation/testing.

Windows changes six files: BUILD_WINDOWS.md, README.md, build_windows.bat,
build_windows.ps1, backend_manager.cpp and backend/main.py. Both platforms touch
README, backend_manager.cpp and backend/main.py. Windows adds upward repo
discovery for build/Release and probes `.venv312` before `.venv`, with host Python
fallback for source builds. Those probes currently happen before package
selection and also alter non-Windows virtualenv precedence; reconcile these
semantically rather than accepting them wholesale. Its backend change moves
the Linux-only ctypes import inside the platform guard.

The Windows branch provides **source builds only**: x64 vcpkg Qt/OpenSSL,
Python 3.12 virtualenv and VS 2026 in the scripts. Documentation still describes
VS 2022/Python 3.10+ and contains a malformed vcpkg command. Scripts have no
failure guards. There is no Windows installer, dependency lock, isolated
packaged runtime staging, updater implementation, signing, hosted Windows job,
or native test record. Do not choose installer tooling based on assumed work.
Data remains `%APPDATA%/Magnite` on Windows and `~/.magnite` on POSIX, overridable
with MAGNITE_HOME; neither platform branch introduces a data migration.

## Baseline evidence

Host: Linux x86_64, Arch, kernel 7.2.11-arch1-1; GCC 16.2.1; CMake 4.4.4;
Qt 6.12.0; OpenSSL 3.6.5; venv Python 3.12.12, FastAPI 0.141.1,
Starlette 1.6.0, httpx 0.28.1, uvicorn 0.52.4. The venv has no pip module;
dependency versions were read with importlib.metadata.

At `f3d8d36`, before merging:

| Check | Result | Local evidence |
| --- | --- | --- |
| `cmake -S app -B build`, `cmake --build build -j2` | Pass; optional Vulkan headers missing warning | `/tmp/magnite-baseline-{configure,build}.log` |
| `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -v` | 58 tests, pass | `build/packaging/multiplatform/baseline/backend-native.log` |
| `ctest --test-dir build --output-on-failure` | 1 manifest test, pass | `baseline/ctest.log` |
| `.venv/bin/python -m unittest discover -s packaging/tests -v` | 6 helper transaction tests, pass | `baseline/helper.log` |
| `QT_QPA_PLATFORM=offscreen ./build/magnite-app --theme-check` | 0 failures | `baseline/theme.log` |
| `QT_QPA_PLATFORM=offscreen ./build/magnite-app --ui-check` | 0 failures | `baseline/ui-native.log` |
| Desktop `--selftest`, isolated MAGNITE_HOME and XDG_CONFIG_HOME under /tmp | 0 failures | `baseline/selftest.log` |

Short evidence paths above are relative to `build/packaging/multiplatform/`.
Sandboxed backend TestClient stalled, and sandboxed UI could not bind its
fixture server. Passing backend/UI/desktop checks ran with authorized loopback
access. Build and tests do not establish the Ubuntu glibc 2.35 package baseline
on this newer Arch host. No real-model baseline was rerun in Phase 0.

Phase 0 acceptance is satisfied: current inputs are inventoried, all branches
are preserved, Linux baseline is understood and passing, and the integration
branch has an explicit starting commit. Phase 1 native Mac and packaged Linux
results are recorded in the main plan as they become available.
