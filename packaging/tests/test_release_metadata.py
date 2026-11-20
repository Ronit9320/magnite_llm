"""Signed update manifests describe each platform's exact package."""
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release_metadata

ROOT = Path(__file__).resolve().parents[2]


class ReleaseMetadataTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.release = {"RELEASE_VERSION": "v0.2-preview", "UPDATE_SEQUENCE": 2, "DATA_SCHEMA": 2}

    def package(self, platform):
        name = release_metadata.PLATFORMS[platform][1].format(version="v0.2-preview")
        path = self.root / name
        path.write_bytes(b"tested package bytes")
        return path

    def test_each_platform_gets_its_own_manifest_and_compatibility_field(self):
        (self.root / "linux-compatibility.json").write_text(json.dumps({"offenders": [], "minimum_glibc": "2.35"}))
        expected = {"linux": ("update-linux-x86_64.json", "minimum_glibc", "2.35"),
                    "macos": ("update-macos-arm64.json", "minimum_macos", "14.0"),
                    "windows": ("update-windows-x86_64.json", "minimum_windows", "10.0.17763")}
        for platform, (filename, field, value) in expected.items():
            manifest = release_metadata.make_manifest(platform, self.package(platform), "test/magnite",
                                                      self.release, "c" * 40)
            data = json.loads(manifest.read_text())
            self.assertEqual(manifest.name, filename)
            self.assertEqual(data[field], value)
            self.assertEqual((data["platform"], data["sequence"], data["channel"]), (platform, 2, "preview"))
            self.assertTrue(data["url"].endswith("/download/v0.2-preview/" + self.package(platform).name))

    def test_wrong_package_name_or_failed_glibc_audit_is_refused(self):
        wrong = self.root / "Magnite-v0.2-preview-arm64.dmg"
        wrong.write_bytes(b"x" * 20)
        with self.assertRaises(SystemExit):
            release_metadata.make_manifest("macos", wrong, "test/magnite", self.release, "c" * 40)
        (self.root / "linux-compatibility.json").write_text(json.dumps({"offenders": ["libx"], "minimum_glibc": "2.40"}))
        with self.assertRaises(SystemExit):
            release_metadata.make_manifest("linux", self.package("linux"), "test/magnite", self.release, "c" * 40)

    def test_package_names_match_the_client(self):
        source = (ROOT / "app/src/update_manager.cpp").read_text()
        for platform, (architecture, pattern) in release_metadata.PLATFORMS.items():
            suffix = pattern.split("{version}", 1)[1]
            self.assertRegex(source, re.escape(f'platform == "{platform}-{architecture}") return "Magnite-" + version + "{suffix}"'))

    def test_installer_and_updater_share_the_windows_minimum(self):
        self.assertEqual(release_metadata.minimum_windows(), "10.0.17763")


if __name__ == "__main__":
    unittest.main()
