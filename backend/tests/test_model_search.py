"""Hub search: paging past the first page and marking already-downloaded models.

The Hub is replaced by an in-memory fake, so these run offline.
"""
import os
from pathlib import Path
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import httpx
from huggingface_hub.errors import HfHubHTTPError

from backend.models.registry import ModelRegistry
from backend.models.schemas import HubSearchResult, ModelRecord, model_id
from backend.models.store import ModelStore
from backend.routes import models
from backend.server import app


def hub_info(repo_id, downloads=0, likes=0, gated=False, tags=None):
    return SimpleNamespace(id=repo_id, downloads=downloads, likes=likes, gated=gated,
                           cardData={"license": "mit"}, tags=tags or [])


class FakeHub:
    """Stand-in for HfApi.list_models: honours `limit`, records every call."""

    def __init__(self, repos):
        self.repos = list(repos)
        self.calls = []

    def __call__(self, **kwargs):
        self.calls.append(kwargs)
        limit = kwargs.get("limit")
        return iter(self.repos if limit is None else self.repos[:limit])


class SearchFixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.cache = self.root / "hub"
        self.cache.mkdir()
        self.store = ModelStore(str(self.root / "models.db"))
        self.registry = ModelRegistry(self.store, str(self.cache))
        self.hub = FakeHub([hub_info(f"org/model-{i:03d}", downloads=1000 - i) for i in range(250)])
        patcher = patch.object(self.registry._api, "list_models", self.hub)
        patcher.start()
        self.addCleanup(patcher.stop)

    def cache_snapshot(self, repo, revision="abc123", files=None):
        """A real Hub cache layout (blobs + snapshot symlinks) with model weights."""
        repo_path = self.cache / ("models--" + repo.replace("/", "--"))
        snap = repo_path / "snapshots" / revision
        snap.mkdir(parents=True)
        (repo_path / "blobs").mkdir(exist_ok=True)
        files = files or {"config.json": '{"architectures":["T"]}', "model.safetensors": "weights"}
        for i, (name, content) in enumerate(files.items()):
            blob = repo_path / "blobs" / f"{revision}-{i}"
            blob.write_text(content)
            (snap / name).symlink_to(os.path.relpath(blob, snap))
        (repo_path / "refs").mkdir(exist_ok=True)
        (repo_path / "refs" / "main").write_text(revision)
        return snap

    def register(self, repo, source="huggingface", status="ready"):
        self.store.upsert(ModelRecord(id=model_id(source, repo), name=repo.rsplit("/", 1)[-1],
                                      source=source, ref=repo, path=f"/x/{repo}", status=status))


class SearchPagingTests(SearchFixture):
    def test_first_page_returns_limit_results_in_hub_order(self):
        results = self.registry.search_hub("model", limit=50)
        self.assertEqual(len(results), 50)
        self.assertEqual([r.repo_id for r in results[:3]], ["org/model-000", "org/model-001", "org/model-002"])

    def test_offset_returns_the_next_page_without_overlap(self):
        first = self.registry.search_hub("model", limit=50, offset=0)
        second = self.registry.search_hub("model", limit=50, offset=50)
        self.assertEqual(second[0].repo_id, "org/model-050")
        self.assertEqual(second[-1].repo_id, "org/model-099")
        self.assertFalse({r.repo_id for r in first} & {r.repo_id for r in second})

    def test_pages_together_are_the_full_ordered_list(self):
        seen = []
        for offset in range(0, 250, 100):
            seen += [r.repo_id for r in self.registry.search_hub("model", limit=100, offset=offset)]
        self.assertEqual(seen, [r.id for r in self.hub.repos])

    def test_offset_past_the_end_is_empty_not_an_error(self):
        self.assertEqual(self.registry.search_hub("model", limit=50, offset=250), [])
        self.assertEqual(self.registry.search_hub("model", limit=50, offset=10_000 - 1), [])

    def test_partial_last_page(self):
        results = self.registry.search_hub("model", limit=100, offset=200)
        self.assertEqual(len(results), 50)

    def test_hub_is_asked_for_exactly_offset_plus_limit(self):
        self.registry.search_hub("llama", limit=50, offset=100)
        self.assertEqual(self.hub.calls[-1]["limit"], 150)
        self.assertEqual(self.hub.calls[-1]["search"], "llama")

    def test_defaults_keep_the_previous_behaviour(self):
        self.assertEqual(len(self.registry.search_hub("model")), 20)

    def test_result_fields_are_mapped(self):
        self.hub.repos = [hub_info("a/b", downloads=7, likes=3, gated=True, tags=["x", "license:mit"])]
        r = self.registry.search_hub("b")[0]
        self.assertEqual((r.id, r.repo_id, r.downloads, r.likes, r.gated, r.license, r.tags),
                         (model_id("huggingface", "a/b"), "a/b", 7, 3, True, "mit", ["x", "license:mit"]))

    def test_missing_counts_default_to_zero(self):
        info = SimpleNamespace(id="a/b", downloads=None, likes=None, gated=False, cardData=None, tags=None)
        self.hub.repos = [info]
        r = self.registry.search_hub("b")[0]
        self.assertEqual((r.downloads, r.likes, r.license, r.tags), (0, 0, "", []))


class DownloadedFlagTests(SearchFixture):
    def flags(self, **kw):
        return {r.repo_id: r.downloaded for r in self.registry.search_hub("model", **kw)}

    def test_nothing_downloaded_by_default(self):
        self.assertFalse(any(self.flags(limit=20).values()))

    def test_registered_model_is_flagged(self):
        self.register("org/model-003")
        flags = self.flags(limit=10)
        self.assertTrue(flags["org/model-003"])
        self.assertEqual(sum(flags.values()), 1)

    def test_model_downloaded_outside_the_app_is_flagged_via_the_cache(self):
        self.cache_snapshot("org/model-005")
        self.assertTrue(self.flags(limit=10)["org/model-005"])

    def test_flag_is_independent_of_which_page_is_requested(self):
        self.register("org/model-120")
        self.assertTrue(self.flags(limit=50, offset=100)["org/model-120"])
        self.assertFalse(self.flags(limit=50, offset=0)["org/model-001"])

    def test_ollama_model_with_the_same_name_is_not_a_hub_download(self):
        self.register("org/model-001", source="ollama")
        self.assertFalse(self.flags(limit=10)["org/model-001"])

    def test_incomplete_registration_is_not_flagged(self):
        self.register("org/model-002", status="downloading")
        self.assertFalse(self.flags(limit=10)["org/model-002"])

    def test_cache_without_weights_is_not_flagged(self):
        self.cache_snapshot("org/model-004", files={"config.json": "{}", "tokenizer.json": "{}"})
        self.assertFalse(self.flags(limit=10)["org/model-004"])

    def test_repo_id_case_differences_still_match(self):
        self.register("ORG/Model-007")
        self.assertTrue(self.flags(limit=10)["org/model-007"])

    def test_deleting_a_model_clears_the_flag(self):
        self.register("org/model-003")
        self.assertTrue(self.flags(limit=10)["org/model-003"])
        self.store.delete(model_id("huggingface", "org/model-003"))
        self.assertFalse(self.flags(limit=10)["org/model-003"])

    def test_search_does_not_modify_the_shared_cache(self):
        snap = self.cache_snapshot("org/model-005")
        before = sorted(str(p) for p in self.cache.rglob("*"))
        self.flags(limit=10)
        self.assertEqual(sorted(str(p) for p in self.cache.rglob("*")), before)
        self.assertTrue(snap.exists())

    def test_cache_scan_failure_does_not_break_search(self):
        with patch.object(self.registry, "list_synced", side_effect=OSError("cache unreadable")):
            results = self.registry.search_hub("model", limit=5)
        self.assertEqual(len(results), 5)
        self.assertFalse(any(r.downloaded for r in results))

    def test_schema_default_is_not_downloaded(self):
        self.assertFalse(HubSearchResult(id="x", repo_id="a/b").downloaded)


class SearchRouteTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.fixture = SearchFixture()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        patcher = patch.object(models, "_registry", self.fixture.registry)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.client = httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://test")
        self.addAsyncCleanup(self.client.aclose)

    async def search(self, **params):
        params.setdefault("q", "model")
        return await self.client.get("/models/search", params=params)

    async def test_offset_and_limit_are_honoured(self):
        r = await self.search(limit=50, offset=50)
        self.assertEqual(r.status_code, 200)
        body = r.json()
        self.assertEqual(len(body), 50)
        self.assertEqual(body[0]["repo_id"], "org/model-050")

    async def test_response_includes_downloaded_flag(self):
        self.fixture.register("org/model-000")
        body = (await self.search(limit=3)).json()
        self.assertEqual([m["downloaded"] for m in body], [True, False, False])

    async def test_default_limit_and_offset(self):
        self.assertEqual(len((await self.search()).json()), 20)

    async def test_limit_100_is_accepted_and_101_rejected(self):
        self.assertEqual((await self.search(limit=100)).status_code, 200)
        self.assertEqual((await self.search(limit=101)).status_code, 422)
        self.assertEqual((await self.search(limit=0)).status_code, 422)

    async def test_invalid_offset_is_rejected(self):
        self.assertEqual((await self.search(offset=-1)).status_code, 422)
        self.assertEqual((await self.search(offset="x")).status_code, 422)

    async def test_offset_has_an_upper_bound(self):
        self.assertEqual((await self.search(offset=100_000)).status_code, 422)

    async def test_blank_query_returns_empty_without_hitting_the_hub(self):
        r = await self.search(q="   ")
        self.assertEqual((r.status_code, r.json()), (200, []))
        self.assertEqual(self.fixture.hub.calls, [])

    async def test_missing_query_is_rejected(self):
        self.assertEqual((await self.client.get("/models/search")).status_code, 422)

    async def test_hub_http_error_becomes_502_with_a_readable_message(self):
        failure = HfHubHTTPError("boom", response=httpx.Response(
            503, request=httpx.Request("GET", "https://huggingface.co/api/models")))
        with patch.object(self.fixture.registry._api, "list_models", side_effect=failure):
            r = await self.search()
        self.assertEqual(r.status_code, 502)
        self.assertIn("Hub", r.json()["detail"])

    async def test_offline_becomes_502(self):
        with patch.object(self.fixture.registry._api, "list_models", side_effect=httpx.ConnectError("offline")):
            r = await self.search()
        self.assertEqual(r.status_code, 502)

    async def test_search_route_is_not_shadowed_by_model_id_route(self):
        self.assertEqual((await self.search(limit=1)).json()[0]["repo_id"], "org/model-000")


if __name__ == "__main__":
    unittest.main()
