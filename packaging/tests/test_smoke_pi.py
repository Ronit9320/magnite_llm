import importlib.util
import os
from pathlib import Path
import unittest


class SmokePiTests(unittest.TestCase):
    def test_packaging_smoke_drives_the_current_pi_bridge(self):
        # Every packaged build runs this smoke; it broke once when the bridge's model hook was renamed.
        # Without the backend or the Pi runtime this skips, unless MAGNITE_REQUIRE_PI makes it a failure.
        try:
            from backend.tooling import pi_bridge
            pi_bridge.node_path()
        except (ImportError, ValueError, OSError) as exc:
            if os.environ.get("MAGNITE_REQUIRE_PI"):
                self.fail(f"MAGNITE_REQUIRE_PI is set but the Pi runtime is unavailable: {exc}")
            self.skipTest("Install the backend and the optional pinned Pi runtime to exercise its subprocess")
        spec = importlib.util.spec_from_file_location('smoke_pi', Path(__file__).parents[1] / 'smoke_pi.py')
        smoke = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(smoke)
        smoke.check_bridge()


if __name__ == '__main__':
    unittest.main()
