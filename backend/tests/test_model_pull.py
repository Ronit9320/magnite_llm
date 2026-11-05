"""Model downloads: byte progress, cancellation and cleanup of partial files.

Hugging Face downloads run in a child process (pull_worker) so a cancel can kill
them even while a native Xet transfer is in flight. These tests use a fake worker
script that lays out a real Hub cache structure, so no network is involved.
"""
import asyncio
import io
import json
import os
from pathlib import Path
import sys
import subprocess
import signal
import tempfile
import textwrap
import threading
import time
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import httpx

from backend.models import pull_worker
from backend.models.registry import ModelRegistry
from backend.models.schemas import PullJob, model_id
from backend.models.store import ModelStore
from backend.routes import models
from backend.server import app

FAKE_WORKER = textwrap.dedent(r'''
    import json, os, sys, time
    from pathlib import Path
    repo, cache = sys.argv[1], sys.argv[2]
    mode = repo.rsplit("/", 1)[-1]
    root = Path(cache).parent
    def emit(**kw):
        print(json.dumps(kw), flush=True)
    folder = Path(cache) / ("models--" + repo.replace("/", "--"))
    def layout(complete):
        blobs = folder / "blobs"; snap = folder / "snapshots" / "rev1"; refs = folder / "refs"
        blobs.mkdir(parents=True, exist_ok=True); snap.mkdir(parents=True, exist_ok=True)
        refs.mkdir(exist_ok=True); (refs / "main").write_text("rev1")
        files = ({"config.json": '{"architectures":["FakeLM"]}', "model.safetensors": "w" * (300 * 1024)}
                 if complete else {"config.json": "{}"})
        for i, (name, content) in enumerate(files.items()):
            (blobs / f"b{i}").write_text(content)
            (snap / name).symlink_to(os.path.relpath(blobs / f"b{i}", snap))
        if not complete:
            (blobs / "part.incomplete").write_text("x" * 100)
            shared = Path(cache) / "blobs"; shared.mkdir(exist_ok=True)
            (shared / "xet-staging").write_text("s")
            lock = Path(cache) / ".locks" / folder.name; lock.mkdir(parents=True, exist_ok=True)
            (lock / "x.lock").write_text("")
        return str(snap)
    if mode == "ok":
        emit(total=3000); emit(done=1000, total=3000); emit(done=3000, total=3000)
        emit(result="ok", path=layout(True))
    elif mode == "noisy":
        print("Warning: unauthenticated requests", flush=True); print("{not json", flush=True)
        emit(total=3000); emit(done=3000, total=3000); emit(result="ok", path=layout(True))
    elif mode == "overreport":
        emit(total=1000); emit(done=5000, total=1000); time.sleep(0.3); emit(result="ok", path=layout(True))
    elif mode == "slow":
        layout(False); emit(total=4000); emit(done=1000, total=4000)
        (root / "started").write_text("1"); time.sleep(120)
    elif mode == "gated":
        layout(False); emit(error="401 Client Error: repository is gated"); sys.exit(1)
    elif mode == "crash":
        layout(False); sys.exit(3)
    elif mode == "gate":
        # reports progress, then waits for the test to release it
        emit(total=2000); emit(done=500, total=2000); (root / "started").write_text("1")
        while not (root / "go").exists(): time.sleep(0.05)
        emit(done=2000, total=2000); emit(result="ok", path=layout(True))
''')


def wait_for(condition, timeout=15.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if condition():
            return True
        time.sleep(0.02)
    return condition()


def tree(path):
    """Every entry under path (symlinks not followed), as relative strings."""
    path = Path(path)
    return sorted(str(p.relative_to(path)) for p in path.rglob("*")) if path.exists() else []


class PullFixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.cache = self.root / "hub"
        self.cache.mkdir()
        self.script = self.root / "fake_worker.py"
        self.script.write_text(FAKE_WORKER)
        self.store = ModelStore(str(self.root / "models.db"))
        self.registry = ModelRegistry(
            self.store, str(self.cache),
            pull_command=lambda repo, cache: [sys.executable, str(self.script), repo, cache])
        self.addCleanup(self.registry.shutdown)
        info = SimpleNamespace(cardData={"license": "mit"}, downloads=5, likes=2, gated=False, tags=["t"])
        patcher = patch.object(self.registry._api, "model_info", return_value=info)
        patcher.start()
        self.addCleanup(patcher.stop)

    def job(self, job_id):
        return self.registry.job_status(job_id)

    def finished(self, job_id, timeout=15.0):
        self.assertTrue(wait_for(lambda: self.job(job_id).status not in ("queued", "running"), timeout),
                        f"job did not finish: {self.job(job_id)}")
        return self.job(job_id)

    def started(self):
        return wait_for((self.root / "started").exists)


class SuccessfulPullTests(PullFixture):
    def test_pull_registers_model_with_full_progress(self):
        job = self.finished(self.registry.pull_hf("org/ok"))
        self.assertEqual(job.status, "done")
        self.assertEqual((job.progress, job.bytes_done, job.bytes_total), (100.0, 3000, 3000))
        record = self.registry.get_model(model_id("huggingface", "org/ok"))
        self.assertEqual(record.status, "ready")
        self.assertEqual(record.architecture, "FakeLM")
        self.assertEqual(record.license, "mit")
        self.assertGreater(record.size_mb, 0)

    def test_progress_and_sizes_are_visible_while_running(self):
        job_id = self.registry.pull_hf("org/gate")
        self.assertTrue(wait_for(lambda: self.job(job_id).bytes_done == 500))
        running = self.job(job_id)
        self.assertEqual((running.status, running.bytes_total, running.progress), ("running", 2000, 25.0))
        self.assertTrue(self.registry.has_active_pulls())
        (self.root / "go").write_text("1")
        self.assertEqual(self.finished(job_id).status, "done")
        self.assertFalse(self.registry.has_active_pulls())

    def test_running_progress_never_reaches_100_before_the_model_is_registered(self):
        job_id = self.registry.pull_hf("org/overreport")
        self.assertTrue(wait_for(lambda: self.job(job_id).bytes_done > 0))
        running = self.job(job_id)
        if running.status == "running":
            self.assertLess(running.progress, 100.0)
            self.assertLessEqual(running.bytes_done, running.bytes_total)
        self.assertEqual(self.finished(job_id).status, "done")

    def test_noise_on_the_pipe_is_ignored(self):
        job = self.finished(self.registry.pull_hf("org/noisy"))
        self.assertEqual((job.status, job.bytes_done), ("done", 3000))

    def test_new_job_reports_zero_bytes(self):
        job = self.job(self.registry.pull_hf("org/gate"))
        self.assertEqual((job.bytes_done, job.bytes_total), (0, 0))
        (self.root / "go").write_text("1")


class CancelTests(PullFixture):
    def before(self):
        return [p for p in tree(self.cache) if Path(p).parts[0] != "blobs"]

    def test_cancel_stops_the_download_and_removes_partial_files(self):
        before = self.before()
        job_id = self.registry.pull_hf("org/slow")
        self.assertTrue(self.started())
        self.assertNotEqual(self.before(), before)           # the worker really created partials
        started = time.monotonic()
        self.assertEqual(self.registry.cancel_pull(job_id).id, job_id)
        job = self.finished(job_id)
        self.assertEqual(job.status, "cancelled")
        self.assertLess(time.monotonic() - started, 10, "cancel must not wait for the worker to finish")
        self.assertEqual(self.before(), before, "repository partials are removed")
        self.assertIsNone(self.registry.get_model(model_id("huggingface", "org/slow")))
        self.assertIsNone(job.error)
        self.assertFalse(self.registry.has_active_pulls())

    def test_cancel_kills_the_worker_process(self):
        job_id = self.registry.pull_hf("org/slow")
        self.assertTrue(self.started())
        proc = self.registry._controls[job_id].proc
        self.registry.cancel_pull(job_id)
        self.finished(job_id)
        self.assertIsNotNone(proc.poll(), "worker process must be dead after cancel")

    def test_cancel_leaves_other_models_untouched(self):
        self.finished(self.registry.pull_hf("org/ok"))
        other = tree(self.cache / "models--org--ok")
        job_id = self.registry.pull_hf("org/slow")
        self.assertTrue(self.started())
        self.registry.cancel_pull(job_id)
        self.finished(job_id)
        self.assertEqual(tree(self.cache / "models--org--ok"), other)
        self.assertEqual(self.registry.get_model(model_id("huggingface", "org/ok")).status, "ready")

    def test_cancelling_a_repull_keeps_the_existing_download(self):
        # Same repo already cached: cancel must remove only what this job added.
        repo_dir = self.cache / "models--org--slow"
        (repo_dir / "blobs").mkdir(parents=True)
        (repo_dir / "blobs" / "old-blob").write_text("existing")
        existing = tree(repo_dir)
        job_id = self.registry.pull_hf("org/slow")
        self.assertTrue(self.started())
        self.registry.cancel_pull(job_id)
        self.assertEqual(self.finished(job_id).status, "cancelled")
        self.assertEqual(tree(repo_dir), existing)
        self.assertEqual((repo_dir / "blobs" / "old-blob").read_text(), "existing")

    def test_cancel_is_idempotent_and_safe_after_completion(self):
        job_id = self.registry.pull_hf("org/slow")
        self.assertTrue(self.started())
        self.registry.cancel_pull(job_id)
        self.registry.cancel_pull(job_id)
        self.assertEqual(self.finished(job_id).status, "cancelled")
        self.assertEqual(self.registry.cancel_pull(job_id).status, "cancelled")
        done = self.finished(self.registry.pull_hf("org/ok"))
        self.assertEqual(self.registry.cancel_pull(done.id).status, "done")
        self.assertIsNotNone(self.registry.get_model(model_id("huggingface", "org/ok")))

    def test_cancel_waits_for_atomic_publication_then_reports_done(self):
        publishing = threading.Event()
        release = threading.Event()
        cancelled = threading.Event()
        result = []
        real_upsert = self.store.upsert
        def upsert(record):
            publishing.set()
            if not release.wait(5):
                raise RuntimeError("publication test timed out")
            return real_upsert(record)
        with patch.object(self.store, "upsert", upsert):
            job_id = self.registry.pull_hf("org/ok")
            self.assertTrue(publishing.wait(5))
            def cancel():
                result.append(self.registry.cancel_pull(job_id))
                cancelled.set()
            thread = threading.Thread(target=cancel)
            thread.start()
            try:
                self.assertFalse(cancelled.wait(0.1), "cancel must serialize with publication")
            finally:
                release.set()
                thread.join(5)
            self.assertTrue(cancelled.is_set())
            self.assertEqual(result[0].status, "done")
            self.assertFalse(self.registry._controls[job_id].cancel.is_set())

    def test_cancel_unknown_job_returns_none(self):
        self.assertIsNone(self.registry.cancel_pull("nope"))

    def test_cancel_immediately_after_start_leaves_nothing_behind(self):
        before = self.before()
        job_id = self.registry.pull_hf("org/slow")
        self.registry.cancel_pull(job_id)              # may land before the worker even spawns
        self.assertEqual(self.finished(job_id).status, "cancelled")
        self.assertEqual(self.before(), before)

    def test_can_download_again_after_a_cancel(self):
        job_id = self.registry.pull_hf("org/gate")
        self.assertTrue(self.started())
        self.registry.cancel_pull(job_id)
        self.assertEqual(self.finished(job_id).status, "cancelled")
        (self.root / "go").write_text("1")
        again = self.finished(self.registry.pull_hf("org/gate"))
        self.assertEqual(again.status, "done")
        self.assertIsNotNone(self.registry.get_model(model_id("huggingface", "org/gate")))

    def test_shutdown_kills_a_running_download(self):
        job_id = self.registry.pull_hf("org/slow")
        self.assertTrue(self.started())
        proc = self.registry._controls[job_id].proc
        self.registry.shutdown()
        self.assertTrue(wait_for(lambda: proc.poll() is not None))
        self.assertTrue(wait_for(lambda: not self.registry.has_active_pulls()))


class FailureTests(PullFixture):
    def test_reported_error_is_surfaced_and_partials_are_kept_for_resume(self):
        job = self.finished(self.registry.pull_hf("org/gated"))
        self.assertEqual(job.status, "error")
        self.assertIn("gated", job.error)
        self.assertIn("gated", job.message)
        self.assertIsNone(self.registry.get_model(model_id("huggingface", "org/gated")))
        self.assertTrue((self.cache / "models--org--gated" / "blobs" / "part.incomplete").exists())

    def test_worker_crash_without_output_is_an_error_with_exit_code(self):
        job = self.finished(self.registry.pull_hf("org/crash"))
        self.assertEqual(job.status, "error")
        self.assertIn("3", job.error)

    def test_worker_that_cannot_start_is_an_error(self):
        registry = ModelRegistry(self.store, str(self.cache),
                                 pull_command=lambda repo, cache: ["/nonexistent/python", repo])
        self.addCleanup(registry.shutdown)
        job_id = registry.pull_hf("org/ok")
        self.assertTrue(wait_for(lambda: registry.job_status(job_id).status == "error"))
        self.assertFalse(registry.has_active_pulls())

    def test_failed_pull_can_be_retried(self):
        self.assertEqual(self.finished(self.registry.pull_hf("org/gated")).status, "error")
        self.assertFalse(self.registry.has_active_pulls())
        self.assertEqual(self.finished(self.registry.pull_hf("org/ok")).status, "done")


class ConcurrencyTests(PullFixture):
    def test_same_repo_returns_the_running_job(self):
        first = self.registry.pull_hf("org/slow")
        self.assertEqual(self.registry.pull_hf("org/slow"), first)
        self.registry.cancel_pull(first)
        self.finished(first)

    def test_a_second_different_download_is_refused_while_one_runs(self):
        first = self.registry.pull_hf("org/slow")
        with self.assertRaises(RuntimeError):
            self.registry.pull_hf("org/ok")
        self.registry.cancel_pull(first)
        self.finished(first)
        self.assertEqual(self.finished(self.registry.pull_hf("org/ok")).status, "done")

    def test_listing_models_mid_download_does_not_register_a_partial_model(self):
        job_id = self.registry.pull_hf("org/gate")
        self.assertTrue(self.started())
        # The worker has a (complete-looking) cache folder only after release; force the case
        # where weights already exist on disk but the pull is still running.
        snap = self.cache / "models--org--gate" / "snapshots" / "rev0"
        blobs = self.cache / "models--org--gate" / "blobs"
        blobs.mkdir(parents=True, exist_ok=True); snap.mkdir(parents=True, exist_ok=True)
        (snap / "config.json").write_text("{}"); (blobs / "w").write_text("weights")
        (snap / "model.safetensors").symlink_to(os.path.relpath(blobs / "w", snap))
        (self.cache / "models--org--gate" / "refs").mkdir(exist_ok=True)
        (self.cache / "models--org--gate" / "refs" / "main").write_text("rev0")
        self.assertEqual(self.registry.list_synced(source="huggingface"), [])
        (self.root / "go").write_text("1")
        self.finished(job_id)
        self.assertEqual(len(self.registry.list_synced(source="huggingface")), 1)

    def test_active_download_is_not_reported_as_downloaded_in_search(self):
        job_id = self.registry.pull_hf("org/gate")
        self.assertTrue(self.started())
        self.assertNotIn("org/gate", self.registry._downloaded_hub_repos())
        self.registry.cancel_pull(job_id)
        self.finished(job_id)


class OllamaCancelTests(PullFixture):
    def test_cancelling_an_ollama_pull_stops_reading_and_registers_nothing(self):
        release = threading.Event()
        consumed = threading.Event()

        class FakeStream:
            def __enter__(self_inner): return self_inner
            def __exit__(self_inner, *exc): return False
            def raise_for_status(self_inner): pass
            def iter_lines(self_inner):
                yield json.dumps({"status": "pulling", "total": 100, "completed": 10})
                consumed.set()
                while not release.is_set():
                    time.sleep(0.01)
                    yield json.dumps({"status": "pulling", "total": 100, "completed": 20})

        with patch("backend.models.registry.httpx.stream", return_value=FakeStream()):
            job_id = self.registry.pull_ollama("tiny:latest")
            self.assertTrue(consumed.wait(5))
            self.registry.cancel_pull(job_id)
            job = self.finished(job_id)
        release.set()
        self.assertEqual(job.status, "cancelled")
        self.assertEqual(self.registry.list_models(source="ollama"), [])
        self.assertFalse(self.registry.has_active_pulls())


class CacheTransactionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.cache = Path(self.temp.name).resolve() / "hub"
        self.cache.mkdir()

    def make(self, *relative, content="x"):
        for rel in relative:
            path = self.cache / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)

    def transaction(self, repo="org/m"):
        from backend.models.registry import CacheTransaction
        return CacheTransaction(str(self.cache), repo)

    def test_new_repo_folder_is_removed_entirely(self):
        txn = self.transaction()
        self.make("models--org--m/blobs/a.incomplete", "models--org--m/refs/main", ".locks/models--org--m/a.lock")
        txn.rollback()
        self.assertEqual(tree(self.cache), [])  # .locks existed only because of the job

    def test_cancelled_repull_restores_reference_to_existing_snapshot(self):
        self.make("models--org--m/snapshots/old/config.json")
        self.make("models--org--m/refs/main", content="old")
        txn = self.transaction()
        self.make("models--org--m/snapshots/new/config.json")
        self.make("models--org--m/refs/main", content="new")
        txn.rollback()
        self.assertEqual((self.cache / "models--org--m/refs/main").read_text(), "old")
        self.assertTrue((self.cache / "models--org--m/snapshots/old/config.json").exists())
        self.assertFalse((self.cache / "models--org--m/snapshots/new").exists())

    def test_existing_files_survive_and_only_new_ones_go(self):
        self.make("models--org--m/blobs/old", "models--org--m/snapshots/r0/config.json", ".locks/keep/other.lock")
        before = tree(self.cache)
        txn = self.transaction()
        self.make("models--org--m/blobs/new.incomplete", "models--org--m/snapshots/r1/config.json")
        txn.rollback()
        self.assertEqual(tree(self.cache), before)

    def test_shared_blobs_from_other_clients_survive(self):
        txn = self.transaction()
        self.make("models--org--m/blobs/new", "blobs/xet/other-client")
        txn.rollback()
        self.assertEqual((self.cache / "blobs/xet/other-client").read_text(), "x")
        self.assertFalse((self.cache / "models--org--m").exists())

    def test_unrelated_repos_are_never_touched(self):
        self.make("models--other--x/blobs/b", "models--other--x/refs/main")
        before = tree(self.cache / "models--other--x")
        txn = self.transaction()
        self.make("models--org--m/blobs/a")
        txn.rollback()
        self.assertEqual(tree(self.cache / "models--other--x"), before)

    def test_rollback_when_nothing_was_created_is_a_noop(self):
        self.make("models--org--m/blobs/old")
        before = tree(self.cache)
        self.transaction().rollback()
        self.assertEqual(tree(self.cache), before)

    def test_rollback_is_idempotent_and_tolerates_vanished_files(self):
        txn = self.transaction()
        self.make("models--org--m/blobs/a", "models--org--m/blobs/b")
        (self.cache / "models--org--m/blobs/a").unlink()
        txn.rollback()
        txn.rollback()
        self.assertEqual(tree(self.cache), [])

    def test_symlinks_are_removed_without_following_them(self):
        outside = Path(self.temp.name) / "outside.txt"
        outside.write_text("keep me")
        txn = self.transaction()
        snap = self.cache / "models--org--m" / "snapshots" / "r1"
        snap.mkdir(parents=True)
        (snap / "link").symlink_to(outside)
        txn.rollback()
        self.assertEqual(outside.read_text(), "keep me")
        self.assertEqual(tree(self.cache), [])

    def test_hostile_repo_id_cannot_escape_the_cache(self):
        txn = self.transaction("../../evil")
        self.assertEqual(txn.repo.parent, self.cache)

    def test_missing_cache_directory_is_fine(self):
        from backend.models.registry import CacheTransaction
        txn = CacheTransaction(str(self.cache / "not-created"), "org/m")
        txn.rollback()


class PullCommandTests(unittest.TestCase):
    def test_worker_disables_bytecode_and_preserves_isolation(self):
        from backend.models.registry import _default_pull_command
        for isolated in (0, 1):
            with patch("backend.models.registry.sys.flags", SimpleNamespace(isolated=isolated)):
                command = _default_pull_command("org/m", "/cache")
            self.assertIn("-B", command)
            self.assertEqual("-I" in command, bool(isolated))
            self.assertEqual(command[-4:], ["-m", "backend.models.pull_worker", "org/m", "/cache"])


class ParentWatchTests(unittest.TestCase):
    def test_worker_exits_when_parent_is_killed(self):
        # Runs the real watcher, including the native Windows process-handle path.
        worker = ("from backend.models.pull_worker import _start_parent_watch; "
                  "import os,time; _start_parent_watch(0.02); "
                  "print(os.getpid(), flush=True); time.sleep(60)")
        parent_code = ("import subprocess,sys; "
                       "p=subprocess.Popen([sys.executable,'-B','-c',sys.argv[1]]); p.wait()")
        environment = dict(os.environ)
        environment["PYTHONPATH"] = os.pathsep.join(map(str, sys.path))
        parent = subprocess.Popen([sys.executable, "-B", "-c", parent_code, worker],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True, env=environment)
        child = None
        try:
            child = int(parent.stdout.readline().strip())
            parent.kill()
            parent.wait(timeout=5)
            # The inherited pipes stay open until the child also exits.
            parent.communicate(timeout=8)
        finally:
            if parent.poll() is None:
                parent.kill()
            if child is not None:
                try:
                    os.kill(child, signal.SIGTERM)
                except OSError:
                    pass
            parent.communicate(timeout=5)


class PullWorkerTests(unittest.TestCase):
    """The child process that talks to the Hub. snapshot_download is faked."""

    def run_worker(self, download, info=None, argv=("org/m", "/cache")):
        out = io.StringIO()
        api = SimpleNamespace(model_info=info or (lambda *a, **k: SimpleNamespace(
            siblings=[SimpleNamespace(size=600), SimpleNamespace(size=400), SimpleNamespace(size=None)])))
        with patch.object(pull_worker, "snapshot_download", download), \
                patch.object(pull_worker, "HfApi", lambda: api), \
                patch.object(pull_worker, "_start_parent_watch"):
            code = pull_worker.main(list(argv), out=out)
        lines = [json.loads(line) for line in out.getvalue().splitlines() if line.strip()]
        return code, lines

    def test_reports_total_from_metadata_then_progress_then_result(self):
        def download(repo_id, cache_dir, tqdm_class, **kw):
            bar = tqdm_class(total=1000, unit="B", desc="Downloading bytes")
            bar.update(400)
            bar.update(600)
            return "/cache/snap"
        code, lines = self.run_worker(download)
        self.assertEqual(code, 0)
        self.assertEqual(lines[0], {"total": 1000})
        self.assertEqual(lines[-1], {"result": "ok", "path": "/cache/snap"})
        progress = [l["done"] for l in lines if "done" in l]
        self.assertEqual(progress, sorted(progress))
        self.assertEqual(progress[-1], 1000)

    def test_only_byte_bars_count_and_duplicates_are_not_summed(self):
        def download(repo_id, cache_dir, tqdm_class, **kw):
            files = tqdm_class(total=9, unit="it", desc="Fetching 9 files")
            data = tqdm_class(total=1000, unit="B", desc="Downloading bytes")
            xet = tqdm_class(total=0, unit="B", desc="Reconstructing")
            data.update(300); xet.update(300); files.update(1)
            data.update(300); xet.update(300)
            return "/p"
        _, lines = self.run_worker(download)
        self.assertEqual(max(l["done"] for l in lines if "done" in l), 600)

    def test_metadata_failure_does_not_stop_the_download(self):
        def boom(*a, **k): raise OSError("offline metadata")
        def download(repo_id, cache_dir, tqdm_class, **kw):
            tqdm_class(total=50, unit="B").update(50)
            return "/p"
        code, lines = self.run_worker(download, info=boom)
        self.assertEqual(code, 0)
        self.assertEqual(lines[-1]["result"], "ok")
        self.assertEqual(max(l["total"] for l in lines if "total" in l), 50)

    def test_download_error_is_reported_with_nonzero_exit(self):
        def download(*a, **k): raise RuntimeError("401 gated repo")
        code, lines = self.run_worker(download)
        self.assertEqual(code, 1)
        self.assertIn("401 gated repo", lines[-1]["error"])

    def test_bad_arguments_are_an_error_not_a_crash(self):
        code, lines = self.run_worker(lambda *a, **k: "/p", argv=("only-one",))
        self.assertEqual(code, 2)
        self.assertIn("error", lines[-1])

    def test_progress_lines_are_throttled_but_the_last_value_is_always_sent(self):
        out = io.StringIO()
        reporter = pull_worker.Reporter(out, interval=3600)
        for i in range(1, 101):
            reporter.update(i, 100)
        reporter.flush()
        lines = [json.loads(l) for l in out.getvalue().splitlines()]
        self.assertLessEqual(len(lines), 3)
        self.assertEqual(lines[-1], {"done": 100, "total": 100})

    def test_reporter_is_thread_safe(self):
        out = io.StringIO()
        reporter = pull_worker.Reporter(out, interval=0)
        threads = [threading.Thread(target=lambda: [reporter.update(i, 1000) for i in range(100)])
                   for _ in range(8)]
        for t in threads: t.start()
        for t in threads: t.join()
        for line in out.getvalue().splitlines():
            json.loads(line)   # no interleaved/corrupt lines


class PullRouteTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.fixture = PullFixture()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        patcher = patch.object(models, "_registry", self.fixture.registry)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.client = httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://test")
        self.addAsyncCleanup(self.client.aclose)

    async def pull(self, ref, source="huggingface"):
        return await self.client.post("/models/pull", json={"ref": ref, "source": source})

    async def wait_status(self, job_id, wanted, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            body = (await self.client.get(f"/models/pull/{job_id}")).json()
            if body["status"] in wanted:
                return body
            await asyncio.sleep(0.05)
        self.fail(f"job never reached {wanted}: {body}")

    async def test_job_exposes_byte_progress(self):
        r = await self.pull("org/gate")
        self.assertEqual(r.status_code, 202)
        job_id = r.json()["id"]
        for key in ("bytes_done", "bytes_total"):
            self.assertIn(key, r.json())
        deadline = time.monotonic() + 10
        body = {}
        while time.monotonic() < deadline and body.get("bytes_done") != 500:
            body = (await self.client.get(f"/models/pull/{job_id}")).json()
            await asyncio.sleep(0.05)
        self.assertEqual((body["bytes_done"], body["bytes_total"], body["status"]), (500, 2000, "running"))
        (self.fixture.root / "go").write_text("1")
        self.assertEqual((await self.wait_status(job_id, {"done"}))["bytes_done"], 2000)

    async def test_cancel_endpoint_cancels_a_running_download(self):
        job_id = (await self.pull("org/slow")).json()["id"]
        self.assertTrue(self.fixture.started())
        r = await self.client.post(f"/models/pull/{job_id}/cancel")
        self.assertEqual(r.status_code, 202)
        self.assertEqual(r.json()["id"], job_id)
        self.assertEqual((await self.wait_status(job_id, {"cancelled"}))["status"], "cancelled")
        listed = (await self.client.get("/models", params={"source": "huggingface"})).json()
        self.assertEqual(listed, [])

    async def test_cancel_unknown_job_is_404(self):
        self.assertEqual((await self.client.post("/models/pull/missing/cancel")).status_code, 404)

    async def test_cancel_after_completion_is_a_harmless_200(self):
        job_id = (await self.pull("org/ok")).json()["id"]
        await self.wait_status(job_id, {"done"})
        r = await self.client.post(f"/models/pull/{job_id}/cancel")
        self.assertEqual((r.status_code, r.json()["status"]), (200, "done"))

    async def test_second_download_is_409_while_one_is_running(self):
        job_id = (await self.pull("org/slow")).json()["id"]
        self.assertTrue(self.fixture.started())
        r = await self.pull("org/ok")
        self.assertEqual(r.status_code, 409)
        self.assertIn("download", r.json()["detail"].lower())
        same = await self.pull("org/slow")
        self.assertEqual((same.status_code, same.json()["id"]), (202, job_id))
        await self.client.post(f"/models/pull/{job_id}/cancel")
        await self.wait_status(job_id, {"cancelled"})

    async def test_unknown_source_is_still_400(self):
        self.assertEqual((await self.pull("x/y", source="nope")).status_code, 400)

    async def test_unknown_job_status_is_404(self):
        self.assertEqual((await self.client.get("/models/pull/missing")).status_code, 404)


if __name__ == "__main__":
    unittest.main()
