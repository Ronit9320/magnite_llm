"""Update lifecycle admission must preserve active jobs and streamed work."""
import asyncio
import unittest
from unittest.mock import patch

from backend.maintenance import UpdateGate
from backend._version import RELEASE_VERSION, PACKAGE_VERSION
import re


class UpdateGateTests(unittest.IsolatedAsyncioTestCase):
    async def request(self, gate, path, method="POST"):
        messages = []
        async def send(message):
            messages.append(message)
        async def receive():
            return {"type": "http.request", "body": b"", "more_body": False}
        await gate({"type": "http", "path": path, "method": method}, receive, send)
        return messages[0]["status"]

    async def test_quiesce_blocks_new_work_but_preserves_health_and_resume(self):
        async def app(scope, receive, send):
            await UpdateGate.respond(send, 200, "ok")
        gate = UpdateGate(app)
        with patch.object(gate, "background_busy", return_value=""):
            self.assertEqual(await self.request(gate, "/updates/quiesce"), 200)
        self.assertEqual(await self.request(gate, "/training/start"), 503)
        self.assertEqual(await self.request(gate, "/models", "GET"), 503)
        self.assertEqual(await self.request(gate, "/health", "GET"), 200)
        self.assertEqual(await self.request(gate, "/updates/resume"), 200)
        self.assertEqual(await self.request(gate, "/training/start"), 200)

    async def test_active_response_prevents_quiescing_until_its_cleanup_finishes(self):
        started = asyncio.Event()
        finished = asyncio.Event()
        async def app(scope, receive, send):
            started.set()
            await finished.wait()
            await UpdateGate.respond(send, 200, "ok")
        gate = UpdateGate(app)
        task = asyncio.create_task(self.request(gate, "/inference/chat"))
        await started.wait()
        self.assertEqual(await self.request(gate, "/updates/quiesce"), 409)
        self.assertFalse(gate.quiesced)
        finished.set()
        await task
        with patch.object(gate, "background_busy", return_value=""):
            self.assertEqual(await self.request(gate, "/updates/quiesce"), 200)

    async def test_background_training_and_downloads_prevent_restart(self):
        gate = UpdateGate(None)
        with patch("backend.routes.training._get_manager") as manager, patch("backend.routes.models._get_registry") as registry:
            job = type("Job", (), {"status": "queued"})()
            manager.return_value.list.return_value = [job]
            self.assertEqual(await self.request(gate, "/updates/quiesce"), 409)
            manager.return_value.list.return_value = []
            registry.return_value.has_active_pulls.return_value = True
            self.assertEqual(await self.request(gate, "/updates/quiesce"), 409)
            registry.return_value.has_active_pulls.return_value = False
            self.assertEqual(await self.request(gate, "/updates/quiesce"), 200)

    async def test_exception_cleans_up_activity_count(self):
        async def app(scope, receive, send):
            raise RuntimeError("failed request")
        gate = UpdateGate(app)
        with self.assertRaises(RuntimeError):
            await self.request(gate, "/datasets/ingest")
        self.assertEqual(gate.active, 0)


class ReleaseIdentityTests(unittest.TestCase):
    def test_display_identity_and_python_metadata_share_one_source(self):
        match = re.fullmatch(r"v(\d+\.\d+(?:\.\d+)?)-preview(?:\.(\d+))?", RELEASE_VERSION)
        self.assertIsNotNone(match)
        self.assertEqual(PACKAGE_VERSION, match[1] + "rc" + (match[2] or "0"))
