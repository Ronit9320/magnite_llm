from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "macos"))
from verify_bundle import check_host_paths


class HostPathAuditTests(unittest.TestCase):
    def test_upstream_runner_diagnostics_do_not_hide_real_checkout_or_script_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory)
            site = app / "Contents/Resources/runtime/python/lib/python3.12/site-packages"
            site.mkdir(parents=True)
            binary = site / "upstream.so"
            binary.write_bytes(b"\xcf\xfa\xed\xfe/Users/runner/work/upstream/assertion.cc")
            home = "/Users/runner"
            checkout = home + "/work/magnite"
            self.assertTrue(check_host_paths(app, [home, checkout]))
            self.assertEqual(check_host_paths(app, [home, checkout], upstream_home=home), [])
            binary.write_bytes(b"\xcf\xfa\xed\xfe" + (checkout + "/build/path").encode())
            self.assertTrue(check_host_paths(app, [home, checkout], upstream_home=home))
            binary.unlink()
            (site / "script.py").write_text(home + "/host-only/dependency.py")
            self.assertTrue(check_host_paths(app, [home, checkout], upstream_home=home))
