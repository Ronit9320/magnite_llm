"""Regression tests for BREAK_REPORT_2026-11-10.md (one class per finding).

Run: PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -p test_break_fixes_2026_11.py -k Finding1 -v
"""
import asyncio
import glob
import json
import os
import signal
import sqlite3
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import MagicMock, patch

import test_regressions as fixtures

from backend import database
from backend.adapters.schemas import AdapterRecord
from backend.adapters.store import AdapterStore
from backend.database import connection
from backend.inference.engines.base import EngineUnloaded
from backend.inference.engines.llamacpp import LlamaCppEngine
from backend.inference.engines.torch import TorchPeftEngine
from backend.models.store import ModelStore
from backend.rag.chunking import extract_content
import httpx
import uvicorn

from backend import main as backend_main
from backend.inference.engines import streaming
from backend.request_limits import BodyLimitMiddleware
from backend.routes import adapters as adapter_routes
from backend.routes import training as training_routes
from backend.rag.store import RagStore
from backend.training.datasets import DatasetStore
from backend.training.jobs import TrainingJobManager
from pydantic import ValidationError

from backend.training.schemas import DatasetRecord, MAX_TOTAL_STEPS, TrainingConfig, TrainingJob


class Finding1TolerantStartup(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.home = self.temp.name
        database.NOTICES.clear()

    def tearDown(self):
        database.NOTICES.clear()
        self.temp.cleanup()

    def path(self, name):
        return os.path.join(self.home, name)

    def test_garbage_models_db_is_quarantined(self):
        db = self.path("models.db")
        with open(db, "wb") as f:
            f.write(b"this is not sqlite")
        with open(db + "-wal", "wb") as f:
            f.write(b"stale journal")
        store = ModelStore(db)
        self.assertEqual(store.list(), [])
        quarantined = glob.glob(db + ".corrupt-*")
        self.assertTrue(any(Path(p).read_bytes() == b"this is not sqlite" for p in quarantined), quarantined)
        self.assertFalse(os.path.exists(db + "-wal"), "stale -wal must move with the corrupt database")
        self.assertEqual(len(database.NOTICES), 1)
        self.assertIn("models.db", database.NOTICES[0])
        self.assertNotIn(self.home, database.NOTICES[0], "notices carry file names, never full paths")

    def test_every_store_survives_a_garbage_database(self):
        for name, build in (
            ("adapters.db", lambda p: AdapterStore(p)),
            ("datasets.db", lambda p: DatasetStore(self.path("datasets"), p)),
            ("rag.db", lambda p: RagStore(self.home)),
        ):
            with open(self.path(name), "wb") as f:
                f.write(b"this is not sqlite")
            build(self.path(name))
            self.assertTrue(glob.glob(self.path(name) + ".corrupt-*"), name)

    def test_bad_job_row_is_quarantined_not_dropped(self):
        db = self.path("training_jobs.db")
        good = TrainingJob(id="good", name="good", status="done", base_model="b", dataset_id="d")
        with connection(db) as conn:
            conn.execute("CREATE TABLE jobs (id TEXT PRIMARY KEY, record TEXT NOT NULL)")
            conn.execute("INSERT INTO jobs VALUES (?, ?)", ("good", good.model_dump_json()))
            conn.execute("INSERT INTO jobs VALUES (?, ?)", ("x", "{not json"))
        manager = TrainingJobManager(self.home, MagicMock(), MagicMock())
        try:
            self.assertEqual([j.id for j in manager.list()], ["good"])
        finally:
            manager.shutdown()
        with connection(db) as conn:
            rows = conn.execute("SELECT id, record, error FROM jobs_quarantine").fetchall()
        self.assertEqual([(r["id"], r["record"]) for r in rows], [("x", "{not json")])
        self.assertTrue(rows[0]["error"])
        self.assertEqual(len(database.NOTICES), 1)

    def test_garbage_jobs_db_is_quarantined(self):
        db = self.path("training_jobs.db")
        with open(db, "wb") as f:
            f.write(b"this is not sqlite")
        manager = TrainingJobManager(self.home, MagicMock(), MagicMock())
        try:
            self.assertEqual(manager.list(), [])
        finally:
            manager.shutdown()
        self.assertTrue(glob.glob(db + ".corrupt-*"))

    def test_transient_errors_are_not_treated_as_corruption(self):
        db = self.path("models.db")
        ModelStore(db)  # a healthy database now exists

        def locked(conn):
            raise sqlite3.OperationalError("database is locked")

        with self.assertRaises(sqlite3.OperationalError):
            database.open_or_quarantine(db, locked)
        self.assertEqual(glob.glob(db + ".corrupt-*"), [])
        self.assertTrue(os.path.exists(db))

    # ---- deferred minor #2: only real corruption is quarantined ----

    def healthy_db(self):
        db = self.path("models.db")
        ModelStore(db)
        database.NOTICES.clear()
        return db

    @staticmethod
    def failing(error):
        def init(conn):
            raise error
        return init

    @staticmethod
    def failing_once(error):
        calls = []
        def init(conn):
            calls.append(1)
            if len(calls) == 1:
                raise error
        return init

    @staticmethod
    def with_code(message, code):
        error = sqlite3.DatabaseError(message)
        error.sqlite_errorcode = code
        return error

    def assert_left_alone(self, db):
        self.assertEqual(glob.glob(db + ".corrupt-*"), [])
        self.assertTrue(os.path.exists(db))
        self.assertEqual(database.NOTICES, [])

    def test_integrity_error_is_not_corruption(self):
        db = self.healthy_db()
        with self.assertRaises(sqlite3.IntegrityError):
            database.open_or_quarantine(db, self.failing(sqlite3.IntegrityError("UNIQUE constraint failed: t.x")))
        self.assert_left_alone(db)

    def test_programming_error_is_not_corruption(self):
        db = self.healthy_db()
        with self.assertRaises(sqlite3.ProgrammingError):
            database.open_or_quarantine(db, self.failing(sqlite3.ProgrammingError("Cannot operate on a closed database.")))
        self.assert_left_alone(db)

    def test_error_code_beats_message(self):
        db = self.healthy_db()
        constraint = self.with_code("database disk image is malformed", 19)   # SQLITE_CONSTRAINT
        with self.assertRaises(sqlite3.DatabaseError):
            database.open_or_quarantine(db, self.failing(constraint))
        self.assert_left_alone(db)

    def test_message_only_corruption_is_quarantined(self):
        # Python 3.10 has no sqlite_errorcode: the two known corruption messages must still be recognized.
        for message in ("database disk image is malformed", "file is not a database"):
            db = self.healthy_db()
            before = len(glob.glob(db + ".corrupt-*"))
            database.open_or_quarantine(db, self.failing_once(sqlite3.DatabaseError(message)))
            self.assertEqual(len(glob.glob(db + ".corrupt-*")), before + 1, message)
            self.assertEqual(len(database.NOTICES), 1)
            os.remove(db)

    def test_corruption_error_codes_are_quarantined_including_extended_ones(self):
        for code in (11, 26, 267):   # SQLITE_CORRUPT, SQLITE_NOTADB, SQLITE_CORRUPT_VTAB (extended)
            db = self.healthy_db()
            before = len(glob.glob(db + ".corrupt-*"))
            database.open_or_quarantine(db, self.failing_once(self.with_code("unrelated text", code)))
            self.assertEqual(len(glob.glob(db + ".corrupt-*")), before + 1, code)
            os.remove(db)

    def test_healthy_database_is_untouched(self):
        db = self.path("models.db")
        ModelStore(db)
        ModelStore(db)
        self.assertEqual(glob.glob(db + ".corrupt-*"), [])
        self.assertEqual(database.NOTICES, [])


class Finding1HealthNotices(unittest.IsolatedAsyncioTestCase):
    asyncSetUp = fixtures.ApiTests.asyncSetUp
    asyncTearDown = fixtures.ApiTests.asyncTearDown

    async def test_health_reports_notices(self):
        database.NOTICES.clear()
        db = os.path.join(self.temp.name, "side.db")
        with open(db, "wb") as f:
            f.write(b"this is not sqlite")
        ModelStore(db)
        try:
            response = await self.client.get("/health")
            self.assertEqual(response.status_code, 200)
            body = response.json()
            self.assertEqual(body["status"], "ok")
            self.assertEqual(len(body["notices"]), 1)
            self.assertIn("side.db", body["notices"][0])
        finally:
            database.NOTICES.clear()

    async def test_health_has_empty_notices_when_clean(self):
        database.NOTICES.clear()
        self.assertEqual((await self.client.get("/health")).json()["notices"], [])


class Break2ProgressPersistence(unittest.TestCase):
    """break-02 Task A2: a job's progress survives a backend crash."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.store = DatasetStore(str(self.root / "datasets"), str(self.root / "datasets.db"))
        self.db = str(self.root / "training_jobs.db")

    def tearDown(self):
        self.temp.cleanup()

    def saved(self, job_id):
        with connection(self.db) as conn:
            row = conn.execute("SELECT record FROM jobs WHERE id = ?", (job_id,)).fetchone()
        return TrainingJob.model_validate_json(row["record"])

    def test_progress_is_saved_while_the_job_runs(self):
        entered, release = threading.Event(), threading.Event()

        def train(job, cfg, path, *args):
            job.status, job.current_step, job.total_steps = "running", 7, 400
            entered.set()
            release.wait(5)
            job.status = "done"

        with patch("backend.training.jobs.run_training", train), \
                patch.object(TrainingJobManager, "_PROGRESS_SAVE_SECONDS", 0.05):
            manager = TrainingJobManager(self.temp.name, AdapterStore(str(self.root / "adapters.db")), self.store)
            try:
                dataset = self.store.ingest_content("x", "sample")
                job = manager.start(TrainingConfig(base_model="test", dataset_id=dataset.id))
                self.assertTrue(entered.wait(5))
                deadline, snapshot = time.time() + 3, None
                while time.time() < deadline:
                    snapshot = self.saved(job.id)
                    if snapshot.current_step == 7:
                        break
                    time.sleep(0.05)
                self.assertEqual((snapshot.status, snapshot.current_step, snapshot.total_steps), ("running", 7, 400))
            finally:
                release.set()
                manager.shutdown()

    def test_interrupted_job_keeps_its_progress_and_says_where_it_stopped(self):
        running = TrainingJob(id="cut", name="cut", status="running", base_model="b", dataset_id="d",
                              current_step=7, total_steps=400, progress=6.0, loss=0.6,
                              history=[{"step": 7, "loss": 0.6}])
        with connection(self.db) as conn:
            conn.execute("CREATE TABLE jobs (id TEXT PRIMARY KEY, record TEXT NOT NULL)")
            conn.execute("INSERT INTO jobs VALUES (?, ?)", ("cut", running.model_dump_json()))
        manager = TrainingJobManager(self.temp.name, MagicMock(), MagicMock())
        try:
            job = manager.get("cut")
        finally:
            manager.shutdown()
        self.assertEqual(job.status, "error")
        self.assertEqual((job.current_step, job.total_steps, job.loss), (7, 400, 0.6))
        self.assertEqual(job.history, [{"step": 7, "loss": 0.6}])
        self.assertIn("step 7/400", job.message)
        self.assertIn("retry", job.message)

    def test_progress_saver_stops_when_the_job_ends(self):
        with patch("backend.training.jobs.run_training", lambda job, *a: setattr(job, "status", "done")), \
                patch.object(TrainingJobManager, "_PROGRESS_SAVE_SECONDS", 0.05):
            manager = TrainingJobManager(self.temp.name, AdapterStore(str(self.root / "adapters.db")), self.store)
            try:
                dataset = self.store.ingest_content("x", "sample")
                job = manager.start(TrainingConfig(base_model="test", dataset_id=dataset.id))
                deadline = time.time() + 3
                while time.time() < deadline and self.saved(job.id).status != "done":
                    time.sleep(0.05)
                time.sleep(0.2)
                names = [t.name for t in threading.enumerate()]
                self.assertNotIn("training-progress-save", names)
            finally:
                manager.shutdown()


class Finding3LinearHtml(unittest.TestCase):
    def timed(self, text):
        started = time.time()
        extract_content(text, "html")
        return time.time() - started

    def test_stray_lt_is_linear(self):
        self.assertLess(self.timed("a < b " * 40000), 1.0)

    def test_unclosed_scripts_are_linear(self):
        self.assertLess(self.timed("<script>x" * 10000), 1.0)

    def test_ordinary_page_text_is_unchanged(self):
        page = ("<p>Hello&nbsp;<b>world</b></p><script>var a=1;</script><style>p{}</style>"
                "<!-- note --><p>x &amp; y &#x1F600;</p>")
        self.assertEqual(extract_content(page, "html").split(), ["Hello", "world", "x", "&", "y", "\U0001F600"])

    def test_tags_separate_words(self):
        self.assertEqual(extract_content("a<b>c</b>d", "html").split(), ["a", "c", "d"])

    def test_unclosed_script_keeps_the_text_after_it(self):
        words = extract_content("<p>before</p><script>var a=1; after text", "html").split()
        self.assertIn("before", words)
        self.assertIn("after", words)
        self.assertIn("text", words)


class Finding4LoadKeepsModel(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.sup = fixtures.supervisor()
        self.old = fixtures.FakeEngine("A")
        self.old.load()
        self.sup._engine, self.sup.model_ref = self.old, "A"
        self.built = []

    def tearDown(self):
        self.temp.cleanup()

    def make_engine(self, bad=None):
        def make(name, ref):
            self.built.append(ref)
            engine = fixtures.FakeEngine(ref)
            if ref == bad:
                engine.load = MagicMock(side_effect=RuntimeError("boom"))
            return engine
        return patch.object(self.sup, "make_engine", side_effect=make)

    def test_missing_path_keeps_the_old_model(self):
        with self.assertRaisesRegex(ValueError, "not found"):
            self.sup.load_model("/nonexistent/x.gguf", "llamacpp")
        self.assertIs(self.sup._engine, self.old)
        self.assertTrue(self.old.loaded)
        self.assertEqual(self.sup.status().loaded_model, "A")

    def test_bad_gguf_files_are_rejected_before_unloading(self):
        for name, content in (("empty.gguf", b""), ("text.gguf", b"this is not a model")):
            path = os.path.join(self.temp.name, name)
            with open(path, "wb") as f:
                f.write(content)
            with self.assertRaisesRegex(ValueError, "GGUF"):
                self.sup.load_model(path, "llamacpp")
            self.assertIs(self.sup._engine, self.old, name)

    def test_real_gguf_header_is_accepted(self):
        path = os.path.join(self.temp.name, "ok.gguf")
        with open(path, "wb") as f:
            f.write(b"GGUF" + b"\x00" * 16)
        with self.make_engine():
            self.sup.load_model(path, "llamacpp")
        self.assertEqual(self.sup._engine.model_ref, path)

    def test_load_failure_restores_the_previous_model(self):
        with self.make_engine(bad="B"):
            with self.assertRaisesRegex(RuntimeError, "boom"):
                self.sup.load_model("B", "torch")
        self.assertEqual(self.sup.status().loaded_model, "A")
        self.assertEqual(self.sup._engine.model_ref, "A")
        self.assertTrue(self.sup._engine.loaded)
        self.assertEqual(self.built, ["B", "A"])

    def test_failed_load_with_nothing_loaded_stays_empty(self):
        self.sup._engine = None
        with self.make_engine(bad="B"):
            with self.assertRaisesRegex(RuntimeError, "boom"):
                self.sup.load_model("B", "torch")
        self.assertIsNone(self.sup._engine)
        self.assertEqual(self.built, ["B"])

    def adapter(self):
        rec = AdapterRecord(id="ad", name="ad", base_model=self.temp.name, path=self.temp.name, status="ready")  # a local dir: resolves offline
        store = AdapterStore(os.path.join(self.temp.name, "adapters.db"))
        store.upsert(rec)
        self.sup.set_adapter_store(store)
        return rec

    def test_adapter_load_failure_restores_the_previous_model(self):
        self.adapter()
        def torch_engine(ref, adapter=None):
            engine = fixtures.FakeEngine(ref)
            if adapter:
                engine.load = MagicMock(side_effect=RuntimeError("adapter boom"))
            return engine
        with patch("backend.inference.supervisor.TorchPeftEngine", side_effect=torch_engine), self.make_engine():
            with self.assertRaisesRegex(RuntimeError, "adapter boom"):
                self.sup.activate_adapter("ad")
        self.assertEqual(self.sup._engine.model_ref, "A")
        self.assertIsNone(self.sup.active_adapter)

    def test_failed_load_while_an_adapter_is_active_restores_the_adapter(self):
        rec = self.adapter()
        self.sup.active_adapter, self.sup.model_ref = rec, "base"
        adapter_calls = []
        def torch_engine(ref, adapter=None):
            adapter_calls.append((ref, adapter))
            return fixtures.FakeEngine(ref)
        with patch("backend.inference.supervisor.TorchPeftEngine", side_effect=torch_engine), self.make_engine(bad="B"):
            with self.assertRaisesRegex(RuntimeError, "boom"):
                self.sup.load_model("B", "torch")
        self.assertIs(self.sup.active_adapter, rec)
        self.assertEqual(adapter_calls, [("base", rec.path)])
        self.assertEqual(self.sup.model_ref, "base")


class Finding5ImportedBase(unittest.IsolatedAsyncioTestCase):
    asyncSetUp = fixtures.ApiTests.asyncSetUp
    asyncTearDown = fixtures.ApiTests.asyncTearDown
    archive = fixtures.ApiTests.archive

    async def import_adapter(self, **record):
        base = dict(id="adapter:evil", name="evil", base_model="attacker-org/some-model", path="x")
        path = self.archive(record={**base, **record})
        response = await self.client.post("/adapters/import", json={"path": str(path)})
        self.assertEqual(response.status_code, 201, response.text)
        return base["id"]

    async def test_import_strips_resolved_base_model(self):
        adapter_id = await self.import_adapter(meta={"resolved_base_model": "attacker-org/some-model", "keep": 1})
        meta = adapter_routes._get_store().get(adapter_id).meta
        self.assertNotIn("resolved_base_model", meta)
        self.assertEqual(meta.get("keep"), 1)

    async def test_unknown_base_is_a_400_and_never_touches_the_network(self):
        from huggingface_hub.errors import LocalEntryNotFoundError
        adapter_id = await self.import_adapter(meta={"resolved_base_model": "attacker-org/some-model"})
        def never_built(*args, **kwargs):
            raise AssertionError("an engine must not be built for an undownloaded base model")
        with patch("backend.models.resolution.snapshot_download",
                   side_effect=LocalEntryNotFoundError("not cached")) as download, \
                patch("backend.inference.supervisor.TorchPeftEngine", side_effect=never_built):
            response = await self.client.post("/inference/adapter", json={"adapter_id": adapter_id})
        self.assertEqual(response.status_code, 400, response.text)
        self.assertIn("not downloaded", response.json()["detail"])
        self.assertTrue(download.called)
        for call in download.call_args_list:
            self.assertIs(call.kwargs.get("local_files_only"), True)

    async def test_base_found_in_the_local_cache_activates(self):
        adapter_id = await self.import_adapter()
        snapshot = self.root / "snapshot"
        snapshot.mkdir()
        with patch("backend.models.resolution.snapshot_download", return_value=str(snapshot)) as download, \
                patch("backend.inference.supervisor.TorchPeftEngine",
                      side_effect=lambda ref, adapter=None: fixtures.FakeEngine(ref)):
            response = await self.client.post("/inference/adapter", json={"adapter_id": adapter_id})
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(self.sup._engine.model_ref, str(snapshot))
        self.assertIs(download.call_args.kwargs.get("local_files_only"), True)

    async def test_record_imported_before_the_fix_cannot_pick_a_hub_model(self):
        # Review finding: a record already in adapters.db from the old import path still carries the archive's value.
        from huggingface_hub.errors import LocalEntryNotFoundError
        adapter_id = await self.import_adapter()
        store = adapter_routes._get_store()
        record = store.get(adapter_id)
        record.meta["resolved_base_model"] = "attacker-org/some-model"
        store.upsert(record)
        def never_built(*args, **kwargs):
            raise AssertionError("an engine must not be built for an undownloaded base model")
        with patch("backend.models.resolution.snapshot_download", side_effect=LocalEntryNotFoundError("not cached")) as download, \
                patch("backend.inference.supervisor.TorchPeftEngine", side_effect=never_built):
            response = await self.client.post("/inference/adapter", json={"adapter_id": adapter_id})
        self.assertEqual(response.status_code, 400, response.text)
        self.assertIn("not downloaded", response.json()["detail"])
        self.assertIs(download.call_args.kwargs.get("local_files_only"), True)

    async def test_locally_trained_adapter_keeps_its_pinned_base(self):
        adapter_id = await self.import_adapter(base_model="HuggingFaceTB/SmolLM2-135M")
        pinned = self.root / "pinned-snapshot"
        pinned.mkdir()
        store = adapter_routes._get_store()
        record = store.get(adapter_id)
        record.meta["resolved_base_model"] = str(pinned)   # only the trainer writes this, never an import
        store.upsert(record)
        with patch("backend.models.resolution.snapshot_download", side_effect=AssertionError("pinned base must win")), \
                patch("backend.inference.supervisor.TorchPeftEngine",
                      side_effect=lambda ref, adapter=None: fixtures.FakeEngine(ref)):
            response = await self.client.post("/inference/adapter", json={"adapter_id": adapter_id})
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(self.sup._engine.model_ref, str(pinned))


class Finding6TrainingLimits(unittest.TestCase):
    def config(self, **overrides):
        return TrainingConfig(base_model="test", dataset_id="d", **overrides)

    def manager(self, rows):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        datasets = MagicMock()
        datasets.get.return_value = DatasetRecord(id="d", name="d", format="text", path="x", rows=rows)
        manager = TrainingJobManager(self.temp.name, MagicMock(), datasets)
        self.addCleanup(manager.shutdown)
        return manager

    def test_max_steps_has_an_upper_bound(self):
        with self.assertRaises(ValidationError):
            self.config(max_steps=10**12)
        with self.assertRaises(ValidationError):
            self.config(max_steps=MAX_TOTAL_STEPS + 1)
        self.assertEqual(self.config(max_steps=MAX_TOTAL_STEPS).max_steps, MAX_TOTAL_STEPS)

    def test_seed_must_fit_32_bits(self):
        for bad in (2**40, 2**32, -1):
            with self.assertRaises(ValidationError):
                self.config(seed=bad)
        self.assertEqual(self.config(seed=2**32 - 1).seed, 2**32 - 1)
        self.assertEqual(self.config(seed=0).seed, 0)

    def test_derived_step_count_is_limited(self):
        # epochs * ceil(rows / batch_size) == 100 * 1000 == the limit exactly: allowed.
        self.manager(rows=1000)._validate(self.config(epochs=100, batch_size=1))
        with self.assertRaisesRegex(ValueError, f"limit is {MAX_TOTAL_STEPS}"):
            self.manager(rows=1001)._validate(self.config(epochs=100, batch_size=1))

    def test_a_larger_batch_brings_a_big_dataset_under_the_limit(self):
        self.manager(rows=1_000_000)._validate(self.config(epochs=1, batch_size=16))

    def test_explicit_max_steps_overrides_the_derived_count(self):
        self.manager(rows=1_000_000)._validate(self.config(epochs=100, batch_size=1, max_steps=500))


class Finding7ExportPrune(unittest.IsolatedAsyncioTestCase):
    asyncSetUp = fixtures.ApiTests.asyncSetUp
    asyncTearDown = fixtures.ApiTests.asyncTearDown
    archive = fixtures.ApiTests.archive

    def touch(self, directory, name, age_seconds):
        path = Path(directory) / name
        path.write_bytes(b"x")
        stamp = time.time() - age_seconds
        os.utime(path, (stamp, stamp))
        return path

    def test_prune_removes_only_old_exports(self):
        with tempfile.TemporaryDirectory() as directory:
            old = self.touch(directory, "old.zip", 2 * 86400)
            fresh = self.touch(directory, "fresh.zip", 60)
            stale_temp = self.touch(directory, ".export-stale", 2 * 3600)
            active_temp = self.touch(directory, ".export-active", 300)
            unrelated = self.touch(directory, "notes.txt", 10 * 86400)
            removed = adapter_routes._prune_exports(directory)
            self.assertEqual(removed, 2)
            self.assertFalse(old.exists())
            self.assertFalse(stale_temp.exists())
            for kept in (fresh, active_temp, unrelated):
                self.assertTrue(kept.exists(), kept.name)

    def test_prune_ignores_files_it_cannot_remove(self):
        with tempfile.TemporaryDirectory() as directory:
            self.touch(directory, "old.zip", 2 * 86400)
            with patch("os.unlink", side_effect=PermissionError("in use")):
                self.assertEqual(adapter_routes._prune_exports(directory), 0)

    async def test_export_prunes_old_files_and_keeps_new_ones(self):
        path = self.archive(record=dict(id="adapter:exp", name="exp", base_model="t/b", path="x"))
        self.assertEqual((await self.client.post("/adapters/import", json={"path": str(path)})).status_code, 201)
        first = (await self.client.post("/adapters/export/adapter:exp")).json()["download_path"].rsplit("/", 1)[1]
        second = (await self.client.post("/adapters/export/adapter:exp")).json()["download_path"].rsplit("/", 1)[1]
        exports = self.root / "adapters" / "exports"
        old = time.time() - 2 * 86400
        os.utime(exports / first, (old, old))
        third = (await self.client.post("/adapters/export/adapter:exp")).json()["download_path"].rsplit("/", 1)[1]
        self.assertEqual(sorted(p.name for p in exports.iterdir()), sorted([second, third]))


class Finding8RequestLimits(unittest.IsolatedAsyncioTestCase):
    asyncSetUp = fixtures.ApiTests.asyncSetUp
    asyncTearDown = fixtures.ApiTests.asyncTearDown

    @staticmethod
    def echo_app(seen):
        async def app(scope, receive, send):
            body = b""
            while True:
                message = await receive()
                body += message.get("body", b"")
                if not message.get("more_body"):
                    break
            seen.append(len(body))
            await send({"type": "http.response.start", "status": 200, "headers": []})
            await send({"type": "http.response.body", "body": b"ok"})
        return app

    async def middleware_client(self, max_bytes, seen):
        transport = httpx.ASGITransport(app=BodyLimitMiddleware(self.echo_app(seen), max_bytes=max_bytes))
        return httpx.AsyncClient(transport=transport, base_url="http://test")

    async def test_declared_oversized_body_is_rejected_before_the_handler_runs(self):
        seen = []
        async with await self.middleware_client(100, seen) as client:
            response = await client.post("/", content=b"x" * 101)
        self.assertEqual(response.status_code, 413)
        self.assertIn("too large", response.json()["detail"])
        self.assertEqual(seen, [])

    async def test_body_at_the_limit_is_accepted(self):
        seen = []
        async with await self.middleware_client(100, seen) as client:
            response = await client.post("/", content=b"x" * 100)
        self.assertEqual((response.status_code, seen), (200, [100]))

    async def test_chunked_body_without_content_length_is_counted(self):
        seen = []
        async def chunks():
            for _ in range(3):
                yield b"x" * 50
        async with await self.middleware_client(100, seen) as client:
            response = await client.post("/", content=chunks())
        self.assertEqual(response.status_code, 413)
        self.assertEqual(seen, [])

    async def test_app_rejects_a_huge_dataset_body_without_parsing_it(self):
        with patch("backend.training.datasets.DatasetStore.ingest_content") as ingest:
            response = await self.client.post("/datasets/ingest", content=b"x" * (33 * 1024 ** 2),
                                              headers={"content-type": "application/json"})
        self.assertEqual(response.status_code, 413)
        ingest.assert_not_called()

    async def test_content_field_has_its_own_limit(self):
        training_routes._training_setup()
        try:
            too_long = await self.client.post("/datasets/ingest", json={"name": "big", "format": "text",
                                                                          "content": "x" * 16_000_001})
            fine = await self.client.post("/datasets/ingest", json={"name": "ok", "format": "text",
                                                                    "content": "a line\n" * 1000})
        finally:
            training_routes._get_manager().shutdown()
        self.assertEqual(too_long.status_code, 413)
        self.assertEqual(fine.status_code, 201, fine.text)

    async def test_rag_content_field_has_the_same_limit(self):
        response = await self.client.post("/rag/documents", json={"name": "big.txt", "kind": "txt",
                                                                  "content": "x" * 16_000_001})
        self.assertEqual(response.status_code, 413)

    async def test_context_meter_does_not_tokenize_a_huge_message(self):
        self.sup._engine.count_messages = MagicMock(side_effect=AssertionError("must not tokenize"))
        started = time.time()
        response = await self.client.post("/inference/context-usage", json={
            "messages": [{"role": "user", "content": "word " * 400_000}], "max_tokens": 64})
        self.assertEqual(response.status_code, 200, response.text)
        body = response.json()
        self.assertLess(time.time() - started, 1.0)
        self.assertFalse(body["exact"])
        self.assertGreater(body["fill"], 1.0)
        self.assertEqual(body["limit"], self.sup._engine.context_size())

    async def test_context_meter_still_counts_normal_messages(self):
        response = await self.client.post("/inference/context-usage", json={
            "messages": [{"role": "user", "content": "hello"}], "max_tokens": 64})
        self.assertEqual(response.status_code, 200)
        self.assertTrue(response.json()["exact"])


class Finding9MeterRace(unittest.IsolatedAsyncioTestCase):
    asyncSetUp = fixtures.ApiTests.asyncSetUp
    asyncTearDown = fixtures.ApiTests.asyncTearDown

    class FakeLlm:
        def tokenize(self, data):
            time.sleep(0.001)
            return list(data)

    def test_llamacpp_count_after_unload_is_engine_unloaded(self):
        engine = LlamaCppEngine("x")
        engine._llm = self.FakeLlm()
        self.assertEqual(engine.count_tokens("abc"), 3)
        engine.unload()
        with self.assertRaises(EngineUnloaded):
            engine.count_tokens("abc")

    def test_torch_count_after_unload_is_engine_unloaded(self):
        engine = TorchPeftEngine("x")
        with self.assertRaises(EngineUnloaded):
            engine.count_tokens("abc")
        with self.assertRaises(EngineUnloaded):
            engine.context_size()

    def test_counting_while_unloading_never_raises_attribute_error(self):
        engine = LlamaCppEngine("x")
        engine._llm = self.FakeLlm()
        stop, outcomes = threading.Event(), set()

        def count():
            while not stop.is_set():
                try:
                    engine.count_tokens("hello")
                    outcomes.add("ok")
                except EngineUnloaded:
                    outcomes.add("unloaded")
                except Exception as exc:  # AttributeError before the fix
                    outcomes.add(type(exc).__name__)
        threads = [threading.Thread(target=count) for _ in range(4)]
        for t in threads:
            t.start()
        for _ in range(150):
            engine.unload()
            time.sleep(0.0005)
            engine._llm = self.FakeLlm()   # a fresh load
            time.sleep(0.0005)
        stop.set()
        for t in threads:
            t.join()
        self.assertLessEqual(outcomes, {"ok", "unloaded"}, outcomes)

    async def test_route_answers_409_when_the_engine_is_gone_mid_count(self):
        self.sup._engine.count_messages = MagicMock(side_effect=EngineUnloaded("The model was unloaded"))
        response = await self.client.post("/inference/context-usage", json={
            "messages": [{"role": "user", "content": "hi"}], "max_tokens": 16})
        self.assertEqual(response.status_code, 409)
        self.assertIn("unloaded", response.json()["detail"])


class Finding10BadCharacters(unittest.IsolatedAsyncioTestCase):
    asyncSetUp = fixtures.ApiTests.asyncSetUp
    asyncTearDown = fixtures.ApiTests.asyncTearDown

    def raw(self, body):
        # httpx's json= cannot encode a lone surrogate; the JSON escape \\ud800 is what a client sends.
        return dict(content=json.dumps(body), headers={"content-type": "application/json"})

    def dataset_files(self):
        return sorted(p.name for p in (self.root / "datasets").iterdir() if p.suffix in (".jsonl", ".tmp"))

    def test_dataset_store_rejects_a_lone_surrogate_and_leaves_no_file(self):
        store = DatasetStore(str(self.root / "datasets"), str(self.root / "datasets.db"))
        for fmt, content in (("text", "a\ud800b"), ("jsonl", '{"text": "a\\ud800b"}')):
            with self.assertRaisesRegex(ValueError, "invalid character"):
                store.ingest_content("s", content, fmt=fmt)
        self.assertEqual(self.dataset_files(), [])
        record = store.ingest_content("ok", "fine line", fmt="text")
        self.assertEqual(self.dataset_files(), [Path(record.path).name])

    async def test_dataset_route_is_400_with_no_orphan_file(self):
        training_routes._training_setup()
        try:
            response = await self.client.post("/datasets/ingest", **self.raw({"name": "s", "format": "text", "content": "a\ud800b"}))
        finally:
            training_routes._get_manager().shutdown()
        self.assertEqual(response.status_code, 400)
        self.assertIn("invalid character", response.json()["detail"])
        self.assertEqual(self.dataset_files(), [])

    async def test_rag_route_names_the_problem_and_never_embeds(self):
        with patch("backend.routes.rag.embeddings.is_available", return_value=True), \
                patch("backend.routes.rag.embeddings.embed_texts", side_effect=AssertionError("must not embed")):
            response = await self.client.post("/rag/documents", **self.raw({"name": "s.txt", "kind": "txt", "content": "a\ud800b"}))
        self.assertEqual(response.status_code, 400, response.text)
        self.assertIn("invalid character", response.json()["detail"])


class Finding11EmptyPrompt(unittest.IsolatedAsyncioTestCase):
    asyncSetUp = fixtures.ApiTests.asyncSetUp
    asyncTearDown = fixtures.ApiTests.asyncTearDown

    async def test_empty_and_blank_prompts_are_400_with_a_message(self):
        for prompt in ("", "   ", "\n\t"):
            response = await self.client.post("/inference/completion", json={"prompt": prompt, "stream": False})
            self.assertEqual(response.status_code, 400, (prompt, response.text))
            self.assertEqual(response.json()["detail"], "prompt must not be empty")

    async def test_a_real_prompt_still_completes(self):
        response = await self.client.post("/inference/completion", json={"prompt": "hello", "stream": False})
        self.assertEqual(response.status_code, 200, response.text)


class Finding12FastShutdown(unittest.IsolatedAsyncioTestCase):
    @staticmethod
    def slow_producer(stop):
        for i in range(200):          # 10 s of "generation", one token per 50 ms
            time.sleep(0.05)
            yield f"t{i} "

    async def test_abort_all_streams_ends_an_active_stream_quickly(self):
        received = []
        started = time.time()
        async def consume():
            async for text in streaming.threaded_stream(self.slow_producer):
                received.append(text)
        task = asyncio.create_task(consume())
        while not received:
            await asyncio.sleep(0.02)
        self.assertEqual(streaming.abort_all_streams(), 1)
        await asyncio.wait_for(task, timeout=2)
        self.assertLess(time.time() - started, 2.0)
        self.assertLess(len(received), 100)

    async def test_finished_streams_are_forgotten(self):
        def two_tokens(stop):
            yield "a"
            yield "b"
        async for _ in streaming.threaded_stream(two_tokens):
            pass
        self.assertEqual(streaming.abort_all_streams(), 0)

    def test_sigterm_handler_aborts_streams_then_requests_exit(self):
        server = backend_main.MagniteServer(uvicorn.Config(app=lambda *a: None, port=0))
        order = []
        with patch.object(backend_main, "abort_all_streams", side_effect=lambda: order.append("abort") or 1):
            with patch.object(uvicorn.Server, "handle_exit", side_effect=lambda *a: order.append("exit")):
                server.handle_exit(signal.SIGTERM, None)
        self.assertEqual(order, ["abort", "exit"])

    def test_desktop_shutdown_path_aborts_streams_too(self):
        server = backend_main.MagniteServer(uvicorn.Config(app=lambda *a: None, port=0))
        with patch.object(backend_main, "abort_all_streams", return_value=1) as abort:
            server.request_exit()
        abort.assert_called_once()
        self.assertTrue(server.should_exit)
