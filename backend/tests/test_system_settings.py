"""Settings reads remain bounded, private and independent of optional ML imports."""
import importlib.metadata
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from backend.routes import system


class SystemSettingsTests(unittest.TestCase):
    def test_scan_ignores_symlinks_and_can_exclude_nested_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "chat.json").write_bytes(b"abc")
            cache = root / "cache"
            cache.mkdir()
            (cache / "weights").write_bytes(b"12345")
            try:
                (root / "linked").symlink_to(cache, target_is_directory=True)
            except OSError:
                pass
            self.assertEqual(system.directory_size(root), {"bytes": 8, "complete": True})
            self.assertEqual(system.directory_size(root, exclude=cache), {"bytes": 3, "complete": True})

    def test_budget_and_entry_limit_report_partial_measurement(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "file").write_bytes(b"x")
            self.assertFalse(system.directory_size(root, max_entries=0)["complete"])
            self.assertFalse(system.directory_size(root, budget_seconds=-1)["complete"])
            self.assertEqual(system.directory_size(root / "missing"), {"bytes": 0, "complete": True})

    def test_nested_storage_roots_are_never_double_counted(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            nested = root / "nested"
            nested.mkdir()
            (root / "other").write_bytes(b"abc")
            (nested / "data").write_bytes(b"12345")
            for home, cache in ((root, nested), (nested, root), (root, root)):
                with patch.object(system, "app_home", return_value=str(home)), patch(
                        "huggingface_hub.constants.HF_HUB_CACHE", str(cache)):
                    info = system.storage_info()
                self.assertEqual(info["workspace"]["bytes"] + info["models"]["bytes"], 8)
                self.assertEqual(info["combined_locations"], home == cache)

    def test_diagnostics_has_no_environment_or_personal_path_fields(self):
        with patch("backend.training.telemetry.device_telemetry", return_value={"total_bytes": None}), \
             patch.object(system, "package_version", return_value=None):
            info = system.system_info()
        self.assertEqual(set(info), {"version", "system", "architecture", "python", "engines", "device"})
        self.assertIsNone(info["engines"]["torch"])
        with patch("importlib.metadata.version", side_effect=importlib.metadata.PackageNotFoundError):
            self.assertIsNone(system.package_version("missing"))

    def test_logs_are_bounded_and_redact_credentials(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(system, "app_home", return_value=directory):
            self.assertIn("No vLLM", system.local_logs()["text"])
            path = Path(directory) / "vllm.log"
            path.write_text("a" * 70000 + "\nAuthorization: Bearer abcdef\napi_key=hidden password: secret\n")
            result = system.local_logs()
            self.assertTrue(result["truncated"])
            self.assertLessEqual(len(result["text"]), 65580)
            self.assertNotIn("abcdef", result["text"])
            self.assertNotIn("hidden", result["text"])
            self.assertNotIn("password: secret", result["text"])
            self.assertIn("[redacted]", result["text"])

class SettingsAccessTests(unittest.IsolatedAsyncioTestCase):
    async def test_settings_info_requires_desktop_credential_and_obeys_update_gate(self):
        import httpx
        from fastapi import FastAPI
        from backend.local_access import LocalAccessMiddleware
        from backend.maintenance import UpdateGate
        app = FastAPI()
        app.include_router(system.router)
        gate = UpdateGate(app)
        protected = LocalAccessMiddleware(gate, "settings-test-secret")
        with patch("backend.training.telemetry.device_telemetry", return_value={}), \
             patch.object(system, "package_version", return_value=None):
            async with httpx.AsyncClient(transport=httpx.ASGITransport(app=protected), base_url="http://127.0.0.1") as client:
                self.assertEqual((await client.get("/system/info")).status_code, 401)
                auth = {"Authorization": "Bearer settings-test-secret"}
                reply = await client.get("/system/info", headers=auth)
                self.assertEqual(reply.status_code, 200)
                self.assertNotIn("environment", reply.json())
                gate.quiesced = True
                self.assertEqual((await client.get("/system/info", headers=auth)).status_code, 503)


if __name__ == "__main__":
    unittest.main()
