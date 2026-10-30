# Linux re-verification after the macOS v0.1 fixes

The macOS release work (branch `release/v0.1-macos`) changed code that the Linux AppImage shares. None of it has run on Linux yet.
The last verified Linux build is the 2026-10-08 AppImage in [V0_1_LINUX_TESTING.md](V0_1_LINUX_TESTING.md). It predates every
change below, so **rebuild before testing**. These steps take about an hour on a warm machine.

Record results in the table at the end. A check counts as passed only if you ran it on this build.

## What changed that affects Linux

| Change | Where | What to expect on Linux |
|---|---|---|
| **Local API lock-down** (ISSUE-011, [adr/0009](macos/adr/0009-local-api-authentication.md)) | `backend/local_access.py`, `backend/main.py`, `BackendManager`, `BackendClient`, `MainWindow` | The app passes a per-launch secret in `MAGNITE_API_TOKEN`. Every request except `/health` needs `Authorization: Bearer <secret>`, and any non-loopback `Host` gets 400. A second backend on the same data folder exits with code 75, and the app says "already running". |
| **Quantization capability check** (ISSUE-010) | `training/trainer.py`, `routes/training.py`, `TrainingPanel` | Linux keeps 4-/8-bit QLoRA when `bitsandbytes` is installed (the CPU set pins `bitsandbytes==0.50.2`). Without it, training is refused with a clear message instead of a crash. |
| **Full dependency lock** (ISSUE-008) | `packaging/constraints-cpu.txt`, `prepare_runtime.py` (`-c`), `packaging/lock_requirements.sh` | `prepare_runtime.py` now installs with every transitive package pinned (80 pins, `torch==2.14.0+cpu`). The lock was generated on macOS for `x86_64-manylinux_2_28` and has **never been installed on Linux**. |
| **huggingface_hub 1.30.0 → 1.33.0** (ISSUE-007) | `packaging/requirements-cpu.txt` | Deleting a model frees its disk space. |
| macOS parent-death watcher (ISSUE-001) | `backend/main.py`, `darwin` only | No change: Linux still uses `PR_SET_PDEATHSIG`. Re-check force-kill anyway, because the new data-folder lock must be released on kill. |
| Test path fixes (ISSUE-005), Qt notice inventory (ISSUE-009) | tests, `packaging/macos/` | Nothing to do. The AppImage already copies Arch's `/usr/share/licenses/qt6-*`. |

## 0. Get the code onto the Linux machine

The changes are **uncommitted** on the maintainer's Mac. Commit and push them, or copy the working tree over, before you start.
Then check you have them:

```sh
git log -1 --oneline
test -f backend/src/backend/local_access.py && test -f packaging/constraints-cpu.txt && echo "macOS fixes present"
```

## 1. Lock file installs on Linux

```sh
bash packaging/lock_requirements.sh          # needs uv
git diff --stat packaging/constraints-cpu.txt
```

**Pass:** no diff, or only comment lines. A version change means the macOS-generated lock does not match what uv resolves on Linux.
In that case, keep the Linux-generated file and note it.

## 2. Backend regressions and plain build

```sh
pip install -e "backend[ml]"    # or reuse .venv
PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -v
cmake -S app -B build && cmake --build build -j
./build/magnite-app --theme-check
./build/magnite-app --selftest
```

**Pass:**
- unittest ends with `OK` (macOS reported 65 tests; on Linux the macOS-only watcher test is skipped, so expect `OK (skipped=N)`);
- `--theme-check` prints `0 failure(s)`;
- `--selftest` prints `[selftest] PASS (0 failures)`;
- the selftest log has **no** `MAGNITE_API_TOKEN not set` warning, which proves the app passed the secret.

## 3. Rebuild the AppImage and run the existing smoke

```sh
bash packaging/build_appimage.sh 2>&1 | tee build/packaging/rebuild.log
grep -n "no lock at" build/packaging/rebuild.log || echo "lock was used"
python3 packaging/smoke_appimage.py dist/Magnite-0.1.0-test.1-x86_64.AppImage \
  --logs build/packaging/relocated-tests --cache build/packaging/test-cache --features
```

**Pass:**
- the build log shows the lock was used;
- the smoke passes every check, as in the 2026-10-08 record (desktop, GGUF inference, LoRA/adapter/RAG workflow);
- `usr/bin/build-info/` in the image lists `huggingface-hub==1.33.0`.

## 4. Security checks on the running AppImage

Start the AppImage normally, with disposable data: `MAGNITE_HOME=$(mktemp -d) ./Magnite-…AppImage`. Then, in another terminal:

```sh
PID=$(pgrep -f "backend.main" | head -1)
PORT=$(ss -ltnp | grep "pid=$PID," | grep -oE '127\.0\.0\.1:[0-9]+' | cut -d: -f2)
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:$PORT/health                        # expect 200
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:$PORT/datasets                      # expect 401
curl -s -o /dev/null -w "%{http_code}\n" -H "Authorization: Bearer wrong" http://127.0.0.1:$PORT/datasets  # expect 401
curl -s -o /dev/null -w "%{http_code}\n" -H "Host: evil.example" http://127.0.0.1:$PORT/health  # expect 400
tr '\0' ' ' < /proc/$PID/cmdline | grep -c MAGNITE_API_TOKEN                                   # expect 0
```

**Cross-user check** (needs a second Linux user): as that user, run `cat /proc/<PID>/environ` and expect `Permission denied`.
Then repeat the `/datasets` curl and expect `401`. The same user *can* read `/proc/<PID>/environ`. That is expected, as adr/0009
documents.

**Single instance:** with the first copy still open, start a second copy with the **same** `MAGNITE_HOME`.
**Pass:** the second copy shows the "Magnite is already running" dialog; `pgrep -fc backend.main` stays `1`; the first copy keeps working.

## 5. Force-kill and relaunch

```sh
kill -9 "$(pgrep -f 'magnite-app|AppRun' | head -1)"; sleep 2; pgrep -fl backend.main || echo "backend gone"
```

**Pass:** `backend gone` within ~2 s, then relaunching with the same `MAGNITE_HOME` connects normally. That proves the data-folder lock
was released by the kill, not left stale.

## 6. Training panel

In the app, open Training and start a tiny LoRA job with quantization **None** (any toy model).
**Pass:** the job finishes, and 4-bit QLoRA is still offered, because `bitsandbytes` is bundled. On CPU it may still fail at run time.
That was outside the verified Linux scope before and still is, so note the behaviour; don't count it as a failure.

## 7. Model delete frees disk

Download a small model in Library, note `du -sh ~/.cache/huggingface/hub`, delete the model in the app, and check again.
**Pass:** the space is freed.

## Results

| # | Check | Result | Evidence (log path / output) |
|---|---|---|---|
| 1 | Lock matches Linux resolve | | |
| 2 | Regressions / plain build / theme / selftest, no token warning | | |
| 3 | AppImage rebuilt with lock; `smoke_appimage.py --features` | | |
| 4 | 200 / 401 / 401 / 400; secret not in cmdline; cross-user denied; single instance | | |
| 5 | Force-kill clears the backend and the lock | | |
| 6 | LoRA with quantization None | | |
| 7 | Model delete frees disk | | |

When everything passes, add a dated section to [V0_1_LINUX_TESTING.md](V0_1_LINUX_TESTING.md) with the new AppImage size and SHA-256,
and replace `dist/SHA256SUMS`. Not claimed by this checklist: older distros (the build still needs glibc 2.43), GPU or CUDA, Wayland/X11
interactive sessions, or a clean second machine.
