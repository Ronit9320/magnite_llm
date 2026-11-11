"""Runtime package catalogue: fixed allowlist, honest status, no environment leakage."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from backend import runtime_packages as rp


def _status(versions: dict, system="Linux", machine="x86_64", installer=("pip",)):
    with patch.object(rp, "installed_version", side_effect=lambda dist: versions.get(dist)), \
         patch.object(rp.platform, "system", return_value=system), \
         patch.object(rp.platform, "machine", return_value=machine), \
         patch.object(rp, "find_installer", return_value=rp.Installer(*installer) if installer else None):
        return {row["id"]: row for row in rp.status()}


ALL = {"llama-cpp-python": "0.3.35", "torch": "2.14.0", "peft": "0.20.0", "vllm": "0.33.0"}


class CatalogueTests(unittest.TestCase):
    def test_catalogue_is_the_four_known_packages(self):
        self.assertEqual([p.id for p in rp.CATALOGUE], ["llama.cpp", "vLLM", "torch", "PEFT"])

    def test_installed_packages_report_their_version(self):
        rows = _status(ALL)
        for row in rows.values():
            self.assertEqual(row["state"], "installed")
            self.assertFalse(row["installable"])
        self.assertEqual(rows["torch"]["installed_version"], "2.14.0")

    def test_missing_package_is_installable_when_an_installer_exists(self):
        rows = _status({k: v for k, v in ALL.items() if k != "llama-cpp-python"})
        self.assertEqual(rows["llama.cpp"]["state"], "missing")
        self.assertTrue(rows["llama.cpp"]["installable"])
        self.assertIsNone(rows["llama.cpp"]["installed_version"])

    def test_vllm_is_not_available_on_macos_or_windows(self):
        for system, label in (("Darwin", "macOS"), ("Windows", "Windows")):
            row = _status({}, system=system, machine="arm64")["vLLM"]
            self.assertEqual(row["state"], "unsupported")
            self.assertFalse(row["installable"])
            self.assertIn(label, row["reason"])

    def test_vllm_needs_linux_x86_64(self):
        self.assertEqual(_status({}, system="Linux", machine="aarch64")["vLLM"]["state"], "unsupported")
        self.assertEqual(_status({}, system="Linux", machine="x86_64")["vLLM"]["state"], "missing")

    def test_an_installed_unsupported_package_is_still_reported_installed(self):
        self.assertEqual(_status({"vllm": "0.1"}, system="Darwin", machine="arm64")["vLLM"]["state"], "installed")

    def test_peft_is_blocked_until_torch_is_installed(self):
        rows = _status({"llama-cpp-python": "1"})
        self.assertEqual(rows["PEFT"]["state"], "blocked")
        self.assertFalse(rows["PEFT"]["installable"])
        self.assertIn("PyTorch", rows["PEFT"]["reason"])
        self.assertEqual(_status({"torch": "2.14.0"})["PEFT"]["state"], "missing")

    def test_no_installer_means_missing_but_not_installable(self):
        row = _status({}, installer=None)["llama.cpp"]
        self.assertEqual(row["state"], "missing")
        self.assertFalse(row["installable"])
        self.assertTrue(row["reason"])

    def test_status_rows_expose_only_safe_fields(self):
        with tempfile.TemporaryDirectory() as home, patch.object(rp, "app_home", return_value=home):
            rows = _status(ALL)
            text = repr(rows)
        self.assertNotIn(home, text)
        for row in rows.values():
            self.assertEqual(set(row), {"id", "label", "role", "installed_version", "state",
                                        "reason", "size_hint", "installable"})


class ActivationTests(unittest.TestCase):
    def test_user_package_directory_is_appended_once_and_never_first(self):
        with tempfile.TemporaryDirectory() as home, patch.object(rp, "app_home", return_value=home), \
             patch.object(sys, "path", ["/bundle/a", "/bundle/b"]):
            rp.activate()
            rp.activate()
            self.assertEqual(sys.path, ["/bundle/a", "/bundle/b", str(Path(home) / "packages")])


class RouteTests(unittest.TestCase):
    def test_status_route_returns_the_catalogue_rows(self):
        from backend.routes import system
        rows = [{"id": "torch", "state": "installed"}]
        with patch.object(rp, "status", return_value=rows):
            self.assertEqual(system.runtime_packages(), {"packages": rows})

    def test_server_activates_user_packages_before_engines_are_probed(self):
        import inspect
        from backend import server
        source = inspect.getsource(server.startup)
        self.assertLess(source.index("activate()"), source.index("get_supervisor"))


class FakeProcess:
    """Stands in for subprocess.Popen: yields log lines, then blocks until released."""
    def __init__(self, lines=(), returncode=0, hold=None, on_finish=None):
        self.stdout = iter(lines)
        self._returncode, self.hold, self.on_finish = returncode, hold, on_finish
        self.returncode = None
        self.terminated = False
        self.pid = 4242

    def wait(self, timeout=None):
        if self.hold is not None:
            self.hold.wait(5)
        self.returncode = -15 if self.terminated else self._returncode
        if self.on_finish and self.returncode == 0:
            self.on_finish()
        return self.returncode

    def poll(self):
        return self.returncode

    def terminate(self):
        self.terminated = True
        if self.hold is not None:
            self.hold.set()

    kill = terminate


class InstallJobTests(unittest.TestCase):
    def setUp(self):
        self.home = tempfile.TemporaryDirectory()
        self.addCleanup(self.home.cleanup)
        self.calls = []
        self.versions = {"torch": "2.14.0"}          # llama.cpp missing, PEFT installable, vLLM unsupported
        patches = [
            patch.object(rp, "app_home", return_value=self.home.name),
            patch.object(rp, "installed_version", side_effect=lambda dist: self.versions.get(dist)),
            patch.object(rp, "find_installer", return_value=rp.Installer("pip", "/py")),
            patch.object(rp, "busy_reason", return_value=""),
            patch.object(rp, "_refresh_engines"),
            patch.object(rp.platform, "system", return_value="Darwin"),
            patch.object(rp.platform, "machine", return_value="arm64"),
        ]
        for item in patches:
            item.start()
            self.addCleanup(item.stop)
        self.refresh = rp._refresh_engines

    def manager(self, process):
        def popen(argv, **kwargs):
            self.calls.append((argv, kwargs))
            return process
        return rp.InstallManager(popen=popen)

    def refused(self, manager, package_id):
        with self.assertRaises(rp.InstallRefused) as caught:
            manager.start(package_id)
        return caught.exception

    def test_refusals_carry_the_right_status(self):
        manager = self.manager(FakeProcess())
        self.assertEqual(self.refused(manager, "../../evil").status_code, 404)
        self.assertEqual(self.refused(manager, "pip install evil").status_code, 404)
        unsupported = self.refused(manager, "vLLM")
        self.assertEqual((unsupported.status_code, unsupported.detail), (409, "Not available on macOS"))
        self.versions.pop("torch")
        self.assertIn("PyTorch", self.refused(manager, "PEFT").detail)
        self.versions["torch"] = "2.14.0"
        self.assertIn("already installed", self.refused(manager, "torch").detail)
        self.assertEqual(self.calls, [])

    def test_a_loaded_model_or_active_training_blocks_installs(self):
        with patch.object(rp, "busy_reason", return_value="Training is queued or running."):
            error = self.refused(self.manager(FakeProcess()), "llama.cpp")
        self.assertEqual((error.status_code, error.detail), (409, "Training is queued or running."))
        self.assertEqual(self.calls, [])

    def test_success_installs_from_the_catalogue_into_the_user_directory(self):
        manager = self.manager(FakeProcess(
            ["Collecting llama-cpp-python\n", "Successfully installed\n"],
            on_finish=lambda: self.versions.update({"llama-cpp-python": "0.3.35"})))
        manager.start("llama.cpp")
        manager.wait(5)
        status = manager.status()
        self.assertEqual((status["id"], status["state"]), ("llama.cpp", "done"))
        argv, kwargs = self.calls[0]
        self.assertEqual(argv[:6], ["/py", "-B", "-I", "-m", "pip", "install"])
        self.assertEqual(argv[-3:], ["--target", str(Path(self.home.name) / "packages"), "llama-cpp-python>=0.3.0"])
        self.assertTrue((Path(self.home.name) / "packages").is_dir())
        self.refresh.assert_called_once()
        self.assertIn("Successfully installed", status["log_tail"])

    def test_a_clean_exit_that_does_not_install_the_package_is_a_failure(self):
        manager = self.manager(FakeProcess(["done\n"]))
        manager.start("llama.cpp")
        manager.wait(5)
        self.assertEqual(manager.status()["state"], "failed")

    def test_failure_is_reported_with_a_redacted_bounded_tail(self):
        noise = ["x" * 100 + "\n"] * 200
        manager = self.manager(FakeProcess(noise + ["Authorization: Bearer abcdef\n", "ERROR: no matching distribution\n"], returncode=1))
        manager.start("llama.cpp")
        manager.wait(5)
        status = manager.status()
        self.assertEqual(status["state"], "failed")
        self.assertLessEqual(len(status["log_tail"]), 4096)
        self.assertNotIn("abcdef", status["log_tail"])
        self.assertIn("no matching distribution", status["log_tail"])
        self.refresh.assert_not_called()

    def test_only_one_install_runs_at_a_time(self):
        hold = __import__("threading").Event()
        manager = self.manager(FakeProcess(hold=hold))
        manager.start("llama.cpp")
        self.assertEqual(manager.status()["state"], "running")
        error = self.refused(manager, "llama.cpp")
        self.assertEqual(error.status_code, 409)
        self.assertIn("running", error.detail)
        hold.set()
        manager.wait(5)

    def test_cancel_stops_the_installer(self):
        hold = __import__("threading").Event()
        process = FakeProcess(hold=hold)
        manager = self.manager(process)
        manager.start("llama.cpp")
        self.assertTrue(manager.cancel())
        manager.wait(5)
        self.assertTrue(process.terminated)
        self.assertEqual(manager.status()["state"], "cancelled")
        self.assertFalse(manager.cancel())

    def test_installer_never_inherits_magnite_environment(self):
        with patch.dict("os.environ", {"MAGNITE_API_TOKEN": "secret", "MAGNITE_HOME": "/x", "PATH": "/usr/bin"}):
            manager = self.manager(FakeProcess())
            manager.start("llama.cpp")
            manager.wait(5)
        env = self.calls[0][1]["env"]
        self.assertFalse([key for key in env if key.startswith("MAGNITE_")])
        self.assertEqual(env["PATH"], "/usr/bin")

    def test_uv_command_targets_the_running_interpreter(self):
        package = rp.get("llama.cpp")
        argv = rp.build_command(rp.Installer("uv", "/bin/uv"), package, Path("/t"))
        self.assertEqual(argv, ["/bin/uv", "pip", "install", "--python", sys.executable,
                                "--target", "/t", "llama-cpp-python>=0.3.0"])


class SupervisorRefreshTests(unittest.TestCase):
    def test_engines_are_reprobed_after_a_package_is_installed(self):
        from backend.inference import supervisor as sup
        instance = sup.InferenceSupervisor.__new__(sup.InferenceSupervisor)
        instance.llamacpp_ok = instance.vllm_ok = instance.torch_peft_ok = False
        with patch.object(sup.LlamaCppEngine, "is_available", return_value=True), \
             patch.object(sup.VllmEngine, "is_available", return_value=False), \
             patch.object(sup.TorchPeftEngine, "is_available", return_value=True):
            instance.refresh_availability()
        self.assertEqual((instance.llamacpp_ok, instance.vllm_ok, instance.torch_peft_ok), (True, False, True))


class ChildProcessVisibilityTests(unittest.TestCase):
    """Installed packages must be importable where they are used, not only in the server process."""

    def test_training_worker_activates_user_packages_before_training(self):
        import io
        from backend.training import train_worker
        with patch.object(rp, "activate") as activate:
            self.assertEqual(train_worker.main([], out=io.StringIO()), 2)   # usage error exits early
        activate.assert_called_once()

    def test_vllm_subprocess_can_import_user_installed_packages(self):
        from backend.inference.engines import vllm as engine
        with tempfile.TemporaryDirectory() as home, patch.object(engine, "app_home", return_value=home), \
             patch.object(rp, "app_home", return_value=home), \
             patch.dict("os.environ", {"PYTHONPATH": "/existing"}), \
             patch("subprocess.Popen") as popen, patch.object(engine.VllmEngine, "_wait_ready"):
            engine.VllmEngine("model").load()
        paths = popen.call_args.kwargs["env"]["PYTHONPATH"].split(os.pathsep)
        self.assertEqual(paths, ["/existing", str(Path(home) / "packages")])


class InstallRouteTests(unittest.IsolatedAsyncioTestCase):
    async def test_install_routes_need_the_desktop_credential_and_map_refusals(self):
        import httpx
        from fastapi import FastAPI
        from backend.local_access import LocalAccessMiddleware
        from backend.routes import system
        app = FastAPI()
        app.include_router(system.router)
        protected = LocalAccessMiddleware(app, "pkg-secret")
        auth = {"Authorization": "Bearer pkg-secret"}
        fake = rp.InstallManager(popen=lambda *a, **k: FakeProcess())
        with patch.object(rp, "get_manager", return_value=fake), \
             patch.object(rp, "installed_version", return_value=None), \
             patch.object(rp, "find_installer", return_value=rp.Installer("pip", "/py")), \
             patch.object(rp.platform, "system", return_value="Darwin"), \
             patch.object(rp.platform, "machine", return_value="arm64"):
            async with httpx.AsyncClient(transport=httpx.ASGITransport(app=protected), base_url="http://127.0.0.1") as client:
                self.assertEqual((await client.post("/system/packages/llama.cpp/install")).status_code, 401)
                self.assertEqual((await client.get("/system/packages/install")).status_code, 401)
                self.assertEqual((await client.post("/system/packages/nope/install", headers=auth)).status_code, 404)
                vllm = await client.post("/system/packages/vLLM/install", headers=auth)
                self.assertEqual((vllm.status_code, vllm.json()["detail"]), (409, "Not available on macOS"))
                self.assertEqual((await client.get("/system/packages/install", headers=auth)).json()["state"], "idle")
                self.assertFalse((await client.post("/system/packages/install/cancel", headers=auth)).json()["cancelled"])


if __name__ == "__main__":
    unittest.main()
