"""Run the macOS in-app update script against disposable bundles.

`open` and `xattr` are replaced by stand-ins, so the transaction logic (wait for
the client, swap bundles, check startup, roll back) runs on any POSIX host.
Gatekeeper, App Management and LaunchServices still need a real Mac.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "macos/update_install.sh"

# Simulates LaunchServices: a bundle "starts" by writing its version to the health file,
# unless it is marked broken. Every launch is recorded.
OPEN_STUB = """#!/bin/bash
app= health= previous=
for argument; do
    case $argument in *.app) app=$argument ;; esac
    [[ $previous == --update-health-file ]] && health=$argument
    previous=$argument
done
echo "$app" >> "$STUB_LOG"
if [[ -n $health && ! -e $app/Contents/broken ]]; then cat "$app/Contents/version" > "$health"; fi
exit 0
"""


@unittest.skipIf(shutil.which("bash") is None or os.name != "posix", "needs a POSIX shell")
class MacUpdateInstallTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        root = Path(self.temporary.name)
        self.applications = root / "Applications"
        self.applications.mkdir()
        self.installed = self.bundle(self.applications / "Magnite.app", "v0.2-preview")
        self.transaction = self.applications / ".magnite-update-test"
        self.transaction.mkdir(mode=0o700)
        self.incoming = self.bundle(self.transaction / "new/Magnite.app", "v0.2-preview.1")
        stubs = root / "bin"
        stubs.mkdir()
        (stubs / "open").write_text(OPEN_STUB)
        (stubs / "xattr").write_text("#!/bin/sh\nexit 0\n")
        for stub in stubs.iterdir():
            stub.chmod(0o755)
        self.launches = root / "launches.log"
        self.env = {"PATH": f"{stubs}:/usr/bin:/bin", "STUB_LOG": str(self.launches), "MAGNITE_UPDATE_TICKS": "15"}

    @staticmethod
    def bundle(path: Path, version: str) -> Path:
        (path / "Contents/MacOS").mkdir(parents=True)
        (path / "Contents/MacOS/Magnite").write_text("#!/bin/sh\n")
        (path / "Contents/version").write_text(version)
        return path

    def finished_parent(self) -> str:
        process = subprocess.Popen(["true"])
        process.wait()
        return str(process.pid)

    def run_script(self, *, installed=None, parent=None):
        return subprocess.run(["bash", str(SCRIPT), str(installed or self.installed), str(self.incoming),
                               str(self.transaction), parent or self.finished_parent(), "v0.2-preview.1"],
                              env=self.env, capture_output=True, text=True, timeout=60)

    def status(self) -> str:
        return (self.transaction / "status").read_text()

    def test_successful_update_swaps_bundles_and_keeps_the_previous_one(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.status(), "success: v0.2-preview.1")
        self.assertEqual((self.installed / "Contents/version").read_text(), "v0.2-preview.1")
        self.assertEqual((self.transaction / "previous.app/Contents/version").read_text(), "v0.2-preview")
        self.assertEqual(self.launches.read_text().split(), [str(self.installed)])

    def test_version_that_does_not_start_is_rolled_back(self):
        (self.incoming / "Contents/broken").write_text("")
        result = self.run_script()
        self.assertEqual(result.returncode, 6, result.stderr)
        self.assertTrue(self.status().startswith("rolled back"))
        self.assertEqual((self.installed / "Contents/version").read_text(), "v0.2-preview")
        self.assertEqual((self.transaction / "failed.app/Contents/version").read_text(), "v0.2-preview.1")
        # The new copy was tried once, then the restored copy was reopened.
        self.assertEqual(self.launches.read_text().split(), [str(self.installed)] * 2)

    def test_running_client_or_cancel_leaves_the_installation_alone(self):
        sleeper = subprocess.Popen(["sleep", "30"])
        self.addCleanup(sleeper.wait)
        self.addCleanup(sleeper.kill)
        result = self.run_script(parent=str(sleeper.pid))
        self.assertEqual(result.returncode, 3)
        self.assertEqual(self.status(), "cancelled: application did not exit")
        (self.transaction / "cancel").write_text("cancel")
        self.assertEqual(self.run_script().returncode, 3)
        self.assertEqual((self.installed / "Contents/version").read_text(), "v0.2-preview")
        self.assertFalse(self.launches.exists())

    def test_unexpected_paths_are_refused(self):
        elsewhere = self.bundle(Path(self.temporary.name) / "Other/Magnite.app", "v0.2-preview")
        self.assertEqual(self.run_script(installed=elsewhere).returncode, 2)
        self.assertEqual(self.run_script(installed=Path("relative.app")).returncode, 2)
        link = self.applications / "Linked.app"
        link.symlink_to(self.installed)
        self.assertEqual(self.run_script(installed=link).returncode, 2)
        self.assertEqual(self.run_script(parent="12x").returncode, 2)
        self.assertFalse((self.transaction / "status").exists())


if __name__ == "__main__":
    unittest.main()
