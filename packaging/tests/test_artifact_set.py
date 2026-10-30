import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from identity import identity
from verify_artifact_set import REQUIRED, verify


class ArtifactSetTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.sha = "a" * 40
        canonical = identity()
        self.manifests = []
        for system, arch in REQUIRED:
            directory = self.root / system
            directory.mkdir()
            package = directory / "package.bin"
            package.write_bytes(b"tested package")
            info = {"version": canonical["version"], "update_sequence": canonical["sequence"],
                    "data_schema": canonical["data_schema"], "commit": self.sha,
                    "working_tree_modified": False, "package_platform": system, "package_architecture": arch,
                    "assets": [{"name": package.name, "size": package.stat().st_size,
                                "sha256": hashlib.sha256(package.read_bytes()).hexdigest()}]}
            manifest = directory / f"provenance-{system}-{arch}.json"
            manifest.write_text(json.dumps(info))
            self.manifests.append(manifest)

    def test_complete_same_source_set_passes_and_missing_platform_fails(self):
        verify(self.root, self.sha)
        self.manifests[0].unlink()
        with self.assertRaisesRegex(ValueError, "Missing platform"):
            verify(self.root, self.sha)

    def test_mismatched_source_and_dirty_provenance_fail(self):
        for field, bad in (("commit", "b" * 40), ("working_tree_modified", True), ("update_sequence", -1)):
            manifest = self.manifests[0]
            original = manifest.read_text()
            info = json.loads(original)
            info[field] = bad
            manifest.write_text(json.dumps(info))
            with self.assertRaisesRegex(ValueError, "Source identity mismatch"):
                verify(self.root, self.sha)
            manifest.write_text(original)

    def test_changed_package_bytes_and_duplicate_platform_fail(self):
        manifest = self.manifests[0]
        package = manifest.parent / "package.bin"
        package.write_bytes(b"tamper package")  # same length as the original
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            verify(self.root, self.sha)
        package.write_bytes(b"tested package")
        (manifest.parent / "provenance-duplicate.json").write_text(manifest.read_text())
        with self.assertRaisesRegex(ValueError, "duplicate platform"):
            verify(self.root, self.sha)


if __name__ == "__main__":
    unittest.main()
