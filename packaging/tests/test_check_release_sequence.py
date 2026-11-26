import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check_release_sequence import published_sequences


def release(tag, sequence, draft=False, names=("update-linux-x86_64.json",)):
    return {"tag_name": tag, "draft": draft,
            "assets": [{"name": n, "browser_download_url": f"https://example.invalid/{tag}/{n}"} for n in names]}


class SequenceTests(unittest.TestCase):
    def test_highest_published_sequence_per_release(self):
        manifests = {"v0.1-preview": 1, "v0.2-preview": 2, "v0.3-preview": 9}
        fetch = lambda url: json.dumps({"sequence": manifests[url.split("/")[3]]}).encode()
        pages = [[release("v0.1-preview", 1)],
                 [release("v0.2-preview", 2, names=("update-linux-x86_64.json", "update-macos-arm64.json")),
                  release("v0.3-preview", 9, draft=True)]]
        self.assertEqual(published_sequences(pages, fetch), {"v0.1-preview": 1, "v0.2-preview": 2})
        self.assertEqual(published_sequences([release("v0.1-preview", 1)], fetch), {"v0.1-preview": 1})

    def test_releases_without_signed_metadata_are_ignored(self):
        self.assertEqual(published_sequences([release("notes-only", 0, names=("README.md",))], None), {})


if __name__ == "__main__":
    unittest.main()
