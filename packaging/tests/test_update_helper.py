"""Exercise real atomic replacement, lock contention, and startup rollback."""
import os
import fcntl
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "build/magnite-update-helper"


@unittest.skipUnless(HELPER.is_file() and shutil.which("c++"), "Build the Linux update helper first")
class InstallationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiled = tempfile.TemporaryDirectory(prefix="magnite-update-fixtures-")
        cls.fixtures = Path(cls.compiled.name)
        source = cls.fixtures / "launcher.cpp"
        source.write_text(r'''
#include <cstdio>
#include <cstring>
#include <string>
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--update-health-file")) {
#ifdef FAIL_STARTUP
        return 9;
#else
        FILE *f = fopen(argv[2], "w");
        if (!f) return 2;
        fputs("test-preview-new", f); fclose(f); return 0;
#endif
    }
    std::string path = std::string(argv[0]) + ".started";
    FILE *f = fopen(path.c_str(), "w");
    if (f) { fputs("old-preview", f); fclose(f); }
    return 0;
}
''')
        for name, flags in [("old", []), ("new", []), ("broken", ["-DFAIL_STARTUP"])]:
            subprocess.run(["c++", str(source), "-o", str(cls.fixtures / name), *flags], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.compiled.cleanup()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="magnite-update-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.target = self.root / "Magnite.AppImage"
        self.stage = self.root / ".magnite-update-fixture"
        self.stage.mkdir(mode=0o700)
        shutil.copy2(self.fixtures / "old", self.target)
        self.old = self.target.read_bytes()

    def run_helper(self, incoming, expected_status):
        shutil.copy2(self.fixtures / incoming, self.stage / "incoming.AppImage")
        parent = subprocess.Popen(["sleep", "0.2"])
        process = subprocess.Popen([str(HELPER), str(self.target), str(self.stage), str(parent.pid), "test-preview-new"])
        self.addCleanup(lambda: process.poll() is None and process.kill())
        parent.wait(timeout=5)
        result = process.wait(timeout=15)
        self.assertEqual(result, expected_status)

    def test_installs_complete_image_and_keeps_previous_after_health_ack(self):
        self.run_helper("new", 0)
        self.assertEqual(self.target.read_bytes(), (self.fixtures / "new").read_bytes())
        self.assertEqual((self.stage / "previous.AppImage").read_bytes(), self.old)
        self.assertIn("success", (self.stage / "status").read_text())

    def test_failed_startup_restores_previous_image_and_relaunches_it(self):
        self.run_helper("broken", 6)
        self.assertEqual(self.target.read_bytes(), self.old)
        self.assertIn("rolled back", (self.stage / "status").read_text())
        for _ in range(50):
            if Path(str(self.target) + ".started").exists():
                break
            time.sleep(0.02)
        self.assertEqual(Path(str(self.target) + ".started").read_text(), "old-preview")

    def test_symlink_target_is_never_replaced(self):
        actual = self.root / "actual.AppImage"
        self.target.rename(actual)
        self.target.symlink_to(actual)
        self.run_helper("new", 2)
        self.assertTrue(self.target.is_symlink())
        self.assertEqual(actual.read_bytes(), self.old)

    def test_world_accessible_staging_directory_is_rejected(self):
        self.stage.chmod(0o755)
        self.run_helper("new", 2)
        self.assertEqual(self.target.read_bytes(), self.old)

    def test_cancelled_transaction_does_not_install(self):
        (self.stage / "cancel").write_text("cancel")
        self.run_helper("new", 3)
        self.assertEqual(self.target.read_bytes(), self.old)

    def test_install_waits_for_the_application_lock_to_be_released(self):
        shutil.copy2(self.fixtures / "new", self.stage / "incoming.AppImage")
        with Path(str(self.target) + ".update-lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            parent = subprocess.Popen(["sleep", "0.2"])
            process = subprocess.Popen([str(HELPER), str(self.target), str(self.stage), str(parent.pid), "test-preview-new"])
            self.addCleanup(lambda: process.poll() is None and process.kill())
            parent.wait(timeout=5)
            time.sleep(0.3)
            self.assertIsNone(process.poll())
            self.assertEqual(self.target.read_bytes(), self.old)
            fcntl.flock(lock, fcntl.LOCK_UN)
            self.assertEqual(process.wait(timeout=15), 0)
