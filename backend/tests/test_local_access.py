"""Only the app that started the backend may drive it (ISSUE-011, adr/0009)."""
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest

import httpx
from fastapi import FastAPI

from backend import local_access
from backend.maintenance import UpdateGate
from unittest.mock import patch

TOKEN = "s3cret-for-tests"


def _stub_app():
    app = FastAPI()

    @app.get("/health")
    def health():
        return {"status": "ok"}

    @app.get("/datasets")
    def datasets():
        return []
    return app


class LocalAccessTests(unittest.IsolatedAsyncioTestCase):
    async def request(self, path, token=TOKEN, host="127.0.0.1:5555", header=None):
        app = local_access.LocalAccessMiddleware(_stub_app(), token)
        headers = {"Host": host}
        if header is not None:
            headers["Authorization"] = header
        async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://127.0.0.1:5555") as client:
            return (await client.get(path, headers=headers)).status_code

    async def test_secret_is_required_except_for_health(self):
        self.assertEqual(await self.request("/datasets"), 401)
        self.assertEqual(await self.request("/datasets", header="Bearer wrong"), 401)
        self.assertEqual(await self.request("/datasets", header=f"Bearer {TOKEN}"), 200)
        self.assertEqual(await self.request("/health"), 200)

    async def test_only_loopback_host_names_are_accepted(self):
        for host in ("127.0.0.1", "127.0.0.1:5555", "localhost:5555", "[::1]:5555", "LOCALHOST"):
            self.assertEqual(await self.request("/datasets", host=host, header=f"Bearer {TOKEN}"), 200, host)
        for host in ("evil.example:5555", "evil.example", "127.0.0.1.evil.example", ""):
            self.assertEqual(await self.request("/datasets", host=host, header=f"Bearer {TOKEN}"), 400, host)
        self.assertEqual(await self.request("/health", host="evil.example"), 400)

    async def test_developer_mode_without_secret_still_checks_host(self):
        self.assertEqual(await self.request("/datasets", token=None), 200)
        self.assertEqual(await self.request("/datasets", token=None, host="evil.example"), 400)

    async def test_update_coordination_requires_secret_and_preserves_gate(self):
        gate = UpdateGate(_stub_app())
        app = local_access.LocalAccessMiddleware(gate, TOKEN)
        async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app),
                                     base_url="http://127.0.0.1:5555") as client:
            auth = {"Authorization": f"Bearer {TOKEN}"}
            with patch.object(gate, "background_busy", return_value=""):
                self.assertEqual((await client.post("/updates/quiesce")).status_code, 401)
                self.assertFalse(gate.quiesced)
                self.assertEqual((await client.post("/updates/quiesce", headers=auth)).status_code, 200)
            self.assertEqual((await client.get("/datasets", headers=auth)).status_code, 503)
            self.assertEqual((await client.get("/health")).status_code, 200)
            self.assertEqual((await client.post("/updates/resume")).status_code, 401)
            self.assertTrue(gate.quiesced)
            self.assertEqual((await client.post("/updates/resume", headers=auth)).status_code, 200)
            self.assertEqual((await client.get("/datasets", headers=auth)).status_code, 200)


    async def test_desktop_shutdown_is_authenticated_and_unavailable_in_developer_mode(self):
        from unittest.mock import Mock
        shutdown = Mock()
        app = local_access.LocalAccessMiddleware(_stub_app(), TOKEN, shutdown=shutdown)
        async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app),
                                     base_url="http://127.0.0.1") as client:
            self.assertEqual((await client.post("/_desktop/shutdown")).status_code, 401)
            shutdown.assert_not_called()
            self.assertEqual((await client.post("/_desktop/shutdown",
                             headers={"Authorization": f"Bearer {TOKEN}"})).status_code, 200)
            shutdown.assert_called_once()
        developer = local_access.LocalAccessMiddleware(_stub_app(), None, shutdown=shutdown)
        async with httpx.AsyncClient(transport=httpx.ASGITransport(app=developer),
                                     base_url="http://127.0.0.1") as client:
            self.assertEqual((await client.post("/_desktop/shutdown")).status_code, 404)


class DataDirLockTests(unittest.TestCase):
    def test_second_holder_is_refused_and_a_killed_holder_releases(self):
        with tempfile.TemporaryDirectory() as home:
            holder = subprocess.Popen(
                [sys.executable, "-c", "import sys, time; from backend import local_access; "
                 f"held_lock = local_access.acquire_data_dir_lock({home!r}, wait=0); print('held', flush=True); time.sleep(60)"],
                stdout=subprocess.PIPE, text=True, env=dict(os.environ))
            try:
                self.assertEqual(holder.stdout.readline().strip(), "held")
                with self.assertRaises(local_access.DataDirInUse):
                    local_access.acquire_data_dir_lock(home, wait=0.3)
                holder.kill()
                holder.wait(5)
                handle = local_access.acquire_data_dir_lock(home, wait=2)   # the OS released the dead holder's lock
                handle.close()
            finally:
                if holder.poll() is None:
                    holder.kill()
                    holder.wait(5)
                holder.stdout.close()


class BackendProcessTests(unittest.TestCase):
    def test_launched_backend_enforces_the_secret_and_the_lock(self):
        with tempfile.TemporaryDirectory() as home:
            env = dict(os.environ, MAGNITE_API_TOKEN=TOKEN)
            port = 50000 + os.getpid() % 10000
            backend = subprocess.Popen([sys.executable, "-m", "backend.main", "--port", str(port), "--home", home],
                                       env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                base = f"http://127.0.0.1:{port}"
                deadline = time.monotonic() + 60
                while time.monotonic() < deadline:
                    try:
                        if httpx.get(base + "/health", timeout=1).status_code == 200:
                            break
                    except httpx.HTTPError:
                        time.sleep(0.3)
                self.assertEqual(httpx.get(base + "/datasets", timeout=5).status_code, 401)
                self.assertEqual(httpx.get(base + "/datasets", timeout=5,
                                           headers={"Authorization": f"Bearer {TOKEN}"}).status_code, 200)
                second = subprocess.run([sys.executable, "-m", "backend.main", "--port", "0", "--home", home],
                                        env=env, capture_output=True, text=True, timeout=60)
                self.assertEqual(second.returncode, local_access.EXIT_DATA_DIR_IN_USE, second.stderr[-500:])
                response = httpx.post(base + "/_desktop/shutdown", timeout=5,
                                      headers={"Authorization": f"Bearer {TOKEN}"})
                self.assertEqual(response.status_code, 200)
                self.assertEqual(backend.wait(20), 0)
            finally:
                if backend.poll() is None:
                    backend.terminate()
                backend.wait(20)


if __name__ == "__main__":
    unittest.main()
