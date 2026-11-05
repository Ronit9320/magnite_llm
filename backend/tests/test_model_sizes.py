"""Download sizes for Hub search results.

The Hub's search API cannot return file sizes, so exact sizes (the sum of the repo's
files, i.e. what a download fetches) come from one metadata request per repo, run in
parallel, cached, and never allowed to break search. The Hub is faked here.
"""
import threading
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import httpx

from backend.models import hubsize
from backend.models.registry import ModelRegistry
from backend.models.store import ModelStore
from backend.routes import models
from backend.server import app


def info(*sizes):
    return SimpleNamespace(siblings=[SimpleNamespace(size=s) for s in sizes])


class FakeApi:
    """Stands in for HfApi.model_info; records calls and concurrency."""

    def __init__(self, table=None, delay=0.0):
        self.table = table or {}
        self.delay = delay
        self.calls = []
        self.active = 0
        self.peak = 0
        self.lock = threading.Lock()

    def model_info(self, repo, **kwargs):
        with self.lock:
            self.calls.append((repo, kwargs))
            self.active += 1
            self.peak = max(self.peak, self.active)
        try:
            if self.delay:
                time.sleep(self.delay)
            result = self.table.get(repo, info(100))
            if isinstance(result, Exception):
                raise result
            return result
        finally:
            with self.lock:
                self.active -= 1


class RepoSizeBytesTests(unittest.TestCase):
    def test_sums_every_file_the_download_would_fetch(self):
        self.assertEqual(hubsize.repo_size_bytes(FakeApi({"a/b": info(600, 400, 5)}), "a/b"), 1005)

    def test_files_without_a_size_count_as_zero(self):
        self.assertEqual(hubsize.repo_size_bytes(FakeApi({"a/b": info(None, 7)}), "a/b"), 7)

    def test_repo_with_no_files_has_no_size(self):
        self.assertEqual(hubsize.repo_size_bytes(FakeApi({"a/b": info()}), "a/b"), 0)
        self.assertEqual(hubsize.repo_size_bytes(FakeApi({"a/b": SimpleNamespace(siblings=None)}), "a/b"), 0)

    def test_asks_for_file_metadata_with_a_timeout(self):
        api = FakeApi()
        hubsize.repo_size_bytes(api, "a/b")
        self.assertTrue(api.calls[0][1]["files_metadata"])
        self.assertGreater(api.calls[0][1]["timeout"], 0)

    def test_errors_propagate_to_the_caller(self):
        with self.assertRaises(OSError):
            hubsize.repo_size_bytes(FakeApi({"a/b": OSError("offline")}), "a/b")


class RegistrySizesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.registry = ModelRegistry(ModelStore(str(Path(self.temp.name) / "m.db")), str(Path(self.temp.name) / "hub"))

    def use(self, api):
        patcher = patch.object(self.registry, "_api", api)
        patcher.start()
        self.addCleanup(patcher.stop)
        return api

    def test_returns_one_size_per_requested_repo(self):
        self.use(FakeApi({"a/one": info(1000), "b/two": info(2000, 5)}))
        self.assertEqual(self.registry.repo_sizes(["a/one", "b/two"]), {"a/one": 1000, "b/two": 2005})

    def test_failures_are_none_and_do_not_affect_the_others(self):
        self.use(FakeApi({"ok/repo": info(50), "gated/repo": httpx.HTTPError("401"),
                          "gone/repo": OSError("offline"), "empty/repo": info()}))
        self.assertEqual(self.registry.repo_sizes(["ok/repo", "gated/repo", "gone/repo", "empty/repo"]),
                         {"ok/repo": 50, "gated/repo": None, "gone/repo": None, "empty/repo": None})

    def test_successful_sizes_are_cached_across_calls_and_pages(self):
        api = self.use(FakeApi({"a/one": info(10), "b/two": info(20)}))
        self.registry.repo_sizes(["a/one"])
        self.assertEqual(self.registry.repo_sizes(["a/one", "b/two"]), {"a/one": 10, "b/two": 20})
        self.assertEqual(sorted(c[0] for c in api.calls), ["a/one", "b/two"], "a/one is fetched only once")

    def test_failures_are_not_cached_so_a_retry_can_succeed(self):
        api = self.use(FakeApi({"a/one": OSError("flaky")}))
        self.assertEqual(self.registry.repo_sizes(["a/one"]), {"a/one": None})
        api.table["a/one"] = info(77)
        self.assertEqual(self.registry.repo_sizes(["a/one"]), {"a/one": 77})

    def test_duplicates_and_blanks_are_ignored(self):
        api = self.use(FakeApi())
        result = self.registry.repo_sizes(["a/one", "a/one", "", "  "])
        self.assertEqual(result, {"a/one": 100})
        self.assertEqual(len(api.calls), 1)

    def test_no_repos_means_no_requests(self):
        api = self.use(FakeApi())
        self.assertEqual(self.registry.repo_sizes([]), {})
        self.assertEqual(api.calls, [])

    def test_lookups_run_in_parallel_but_within_a_limit(self):
        api = self.use(FakeApi(delay=0.05))
        repos = [f"org/m{i}" for i in range(30)]
        started = time.monotonic()
        self.assertEqual(len(self.registry.repo_sizes(repos)), 30)
        elapsed = time.monotonic() - started
        self.assertGreater(api.peak, 1, "must overlap requests")
        self.assertLessEqual(api.peak, 6, "must not hammer the Hub")
        self.assertLess(elapsed, 30 * 0.05 * 0.6, "30 serial requests would take 1.5s")

    def test_a_slow_hub_cannot_hold_the_response_forever(self):
        api = self.use(FakeApi({"slow/repo": info(1)}, delay=1.5))
        started = time.monotonic()
        result = self.registry.repo_sizes(["slow/repo"], timeout=0.2)
        self.assertEqual(result, {"slow/repo": None})
        self.assertLess(time.monotonic() - started, 1.0)

    def test_the_cache_is_bounded(self):
        self.use(FakeApi())
        self.registry._size_cache_limit = 5
        self.registry.repo_sizes([f"org/m{i}" for i in range(20)])
        self.assertLessEqual(len(self.registry._size_cache), 5)

    def test_cache_keys_ignore_case_like_the_hub(self):
        api = self.use(FakeApi({"Org/Model": info(9)}))
        self.registry.repo_sizes(["Org/Model"])
        self.registry.repo_sizes(["Org/Model"])
        self.assertEqual(len(api.calls), 1)


class SizesRouteTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        fixture = RegistrySizesTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        self.registry = fixture.registry
        self.api = FakeApi({"a/one": info(1000), "b/two": OSError("offline")})
        patcher = patch.object(self.registry, "_api", self.api)
        patcher.start()
        self.addCleanup(patcher.stop)
        reg = patch.object(models, "_registry", self.registry)
        reg.start()
        self.addCleanup(reg.stop)
        self.client = httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://test")
        self.addAsyncCleanup(self.client.aclose)

    async def test_returns_sizes_and_nulls(self):
        r = await self.client.get("/models/sizes", params=[("repo", "a/one"), ("repo", "b/two")])
        self.assertEqual((r.status_code, r.json()), (200, {"a/one": 1000, "b/two": None}))

    async def test_requires_at_least_one_repo(self):
        self.assertEqual((await self.client.get("/models/sizes")).status_code, 422)

    async def test_limits_how_many_repos_one_request_may_ask_for(self):
        ok = await self.client.get("/models/sizes", params=[("repo", f"o/m{i}") for i in range(100)])
        too_many = await self.client.get("/models/sizes", params=[("repo", f"o/m{i}") for i in range(101)])
        self.assertEqual((ok.status_code, too_many.status_code), (200, 422))

    async def test_rejects_absurdly_long_names(self):
        self.assertEqual((await self.client.get("/models/sizes", params={"repo": "x" * 300})).status_code, 422)

    async def test_a_hub_outage_is_not_a_server_error(self):
        self.api.table["a/one"] = httpx.ConnectError("offline")
        r = await self.client.get("/models/sizes", params={"repo": "a/one"})
        self.assertEqual((r.status_code, r.json()), (200, {"a/one": None}))

    async def test_route_is_not_shadowed_by_the_model_id_route(self):
        r = await self.client.get("/models/sizes", params={"repo": "a/one"})
        self.assertEqual(r.status_code, 200)
        self.assertIn("a/one", r.json())


if __name__ == "__main__":
    unittest.main()
