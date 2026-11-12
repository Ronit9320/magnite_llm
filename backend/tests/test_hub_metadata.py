"""Native Hub discovery metadata and detail requests, without network access."""
import asyncio
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

import httpx

from backend.models.hubmetadata import model_details, search_metadata
from backend.models.registry import ModelRegistry
from backend.models.store import ModelStore
from backend.routes import models
from backend.server import app


def info(**overrides):
    values = dict(cardData={"license": "apache-2.0", "language": ["en"]},
                  config={}, tags=["gguf"], pipeline_tag="text-generation", library_name="transformers",
                  safetensors=SimpleNamespace(total=7_000_000_000), sha="abc123", gated=False,
                  downloads=10, likes=2, siblings=[])
    values.update(overrides)
    return SimpleNamespace(**values)


class HubMetadataTests(unittest.TestCase):
    def test_missing_metadata_stays_unknown(self):
        data = search_metadata(SimpleNamespace())
        self.assertIsNone(data["parameters"])
        self.assertIsNone(data["context_length"])
        self.assertEqual(data["languages"], [])
        self.assertEqual(data["architecture"], "")

    def test_gguf_parameter_context_and_architecture_metadata(self):
        data = search_metadata(info(safetensors=None, gguf={"total": 500000000, "context_length": 8192, "architecture": "qwen2"}))
        self.assertEqual(data["parameters"], 500000000)
        self.assertEqual(data["context_length"], 8192)
        self.assertEqual(data["architecture"], "qwen2")
        self.assertIn("gguf", data["formats"])

    def test_license_falls_back_to_tags_and_formats_are_reported(self):
        data = search_metadata(info(cardData=None, tags=["license:mit", "gguf", "safetensors"]))
        self.assertEqual(data["license"], "mit")
        self.assertEqual(data["formats"], ["gguf", "safetensors"])

    def test_invalid_counts_and_config_are_tolerated(self):
        data = search_metadata(info(config=["invalid"], cardData="invalid", safetensors={"total": float("nan")}))
        self.assertIsNone(data["parameters"])
        self.assertIsNone(data["context_length"])

    def test_configuration_is_bounded_and_pinned_to_revision(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            path.write_text(json.dumps({"model_type": "qwen2", "max_position_embeddings": 32768,
                                        "num_hidden_layers": 32, "hidden_size": 4096,
                                        "num_attention_heads": 32, "num_key_value_heads": 8}))
            metadata = info(siblings=[SimpleNamespace(rfilename="config.json", size=200),
                                      SimpleNamespace(rfilename="weights.gguf", size=4000)])
            api = SimpleNamespace(model_info=lambda *args, **kwargs: metadata)
            with patch("backend.models.hubmetadata.hf_hub_download", return_value=str(path)) as download:
                data = model_details(api, "org/model", directory)
            self.assertEqual(download.call_args.kwargs["revision"], "abc123")
            self.assertEqual(download.call_args.args[1], "config.json")
            self.assertEqual(data["architecture"], "qwen2")
            self.assertEqual(data["context_length"], 32768)
            self.assertEqual(data["total_bytes"], 4200)
            self.assertEqual(data["kv_heads"], 8)

    def test_unknown_file_size_keeps_total_unknown(self):
        metadata = info(siblings=[SimpleNamespace(rfilename="weights.gguf", size=None)])
        data = model_details(SimpleNamespace(model_info=lambda *args, **kwargs: metadata), "org/model", "/unused")
        self.assertIsNone(data["total_bytes"])
        self.assertIsNone(data["files"][0]["size_bytes"])

    def test_oversized_config_is_never_downloaded(self):
        metadata = info(siblings=[SimpleNamespace(rfilename="config.json", size=2_000_000)])
        with patch("backend.models.hubmetadata.hf_hub_download") as download:
            data = model_details(SimpleNamespace(model_info=lambda *args, **kwargs: metadata), "org/model", "/unused")
        download.assert_not_called()
        self.assertTrue(data["config_note"])

    def test_config_network_failure_preserves_repository_metadata(self):
        metadata = info(siblings=[SimpleNamespace(rfilename="config.json", size=100)])
        with patch("backend.models.hubmetadata.hf_hub_download", side_effect=httpx.ConnectError("offline")):
            data = model_details(SimpleNamespace(model_info=lambda *args, **kwargs: metadata), "org/model", "/unused")
        self.assertEqual(data["file_count"], 1)
        self.assertTrue(data["config_note"])

    def test_missing_config_does_not_hide_file_inventory(self):
        metadata = info(siblings=[SimpleNamespace(rfilename="model.gguf", size=100)])
        data = model_details(SimpleNamespace(model_info=lambda *args, **kwargs: metadata), "org/model", "/unused")
        self.assertEqual(data["file_count"], 1)
        self.assertIsNone(data["context_length"])
        self.assertEqual(data["evaluations"], [])

    def test_evaluations_are_publisher_reported(self):
        metadata = info(cardData={"model-index": [{"results": [{"dataset": {"name": "Test set"},
                        "metrics": [{"name": "Accuracy", "value": 0.75}]}]}]})
        data = model_details(SimpleNamespace(model_info=lambda *args, **kwargs: metadata), "org/model", "/unused")
        self.assertEqual(data["evaluations"], [{"dataset": "Test set", "metric": "Accuracy", "value": "0.75"}])

    def test_malformed_publisher_evaluations_are_tolerated(self):
        metadata = info(cardData={"model-index": [{"results": [{"dataset": "invalid", "metrics": [{"value": 5}]}]}]})
        data = model_details(SimpleNamespace(model_info=lambda *args, **kwargs: metadata), "org/model", "/unused")
        self.assertEqual(data["evaluations"][0]["dataset"], "Unspecified")

    def test_file_listing_is_bounded_but_total_covers_all_files(self):
        metadata = info(siblings=[SimpleNamespace(rfilename=f"file-{i}.gguf", size=100) for i in range(300)])
        data = model_details(SimpleNamespace(model_info=lambda *args, **kwargs: metadata), "org/model", "/unused")
        self.assertEqual(len(data["files"]), 250)
        self.assertEqual(data["file_count"], 300)
        self.assertEqual(data["total_bytes"], 30000)


class HubRouteTests(unittest.TestCase):
    def request(self, path):
        async def run():
            async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://test") as client:
                return await client.get(path)
        return asyncio.run(run())

    def test_discovery_allows_empty_query_and_requests_enriched_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            registry = ModelRegistry(ModelStore(str(Path(directory) / "models.db")), directory)
            with patch.object(models, "_registry", registry), patch.object(registry._api, "list_models", return_value=iter([])) as query:
                response = self.request("/models/discover")
            self.assertEqual(response.status_code, 200)
            self.assertEqual(query.call_args.kwargs["sort"], "downloads")
            self.assertIn("safetensors", query.call_args.kwargs["expand"])
            self.assertIn("gguf", query.call_args.kwargs["expand"])

    def test_detail_validates_repository_before_network_request(self):
        with patch.object(models, "_registry") as registry:
            response = self.request("/models/hub-detail?repo=../../secret")
        self.assertEqual(response.status_code, 400)
        registry.hub_details.assert_not_called()

    def test_discovery_reports_network_failure(self):
        with patch.object(models, "_registry") as registry:
            registry.search_hub.side_effect = httpx.ConnectError("offline")
            response = self.request("/models/discover?q=example")
        self.assertEqual(response.status_code, 502)
        self.assertIn("offline", response.json()["detail"])

    def test_detail_network_failure_is_retryable(self):
        with patch.object(models, "_registry") as registry:
            registry.hub_details.side_effect = httpx.ConnectError("offline")
            response = self.request("/models/hub-detail?repo=org/model")
        self.assertEqual(response.status_code, 502)
