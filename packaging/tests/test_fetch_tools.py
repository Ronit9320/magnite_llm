import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('fetch_tools', Path(__file__).parents[1] / 'fetch_tools.py')
fetch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetch)


class FetchTests(unittest.TestCase):
    def test_pin_change_replaces_stale_cache_only_after_verification(self):
        self.check_download(b'reviewed', succeeds=True)

    def test_corrupt_download_preserves_existing_tool_and_cleans_temporary(self):
        self.check_download(b'corrupt', succeeds=False)

    def check_download(self, downloaded, succeeds):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory)
            target = destination / 'tool'
            target.write_bytes(b'previous')
            entry = dict(name='tool', url='https://example.invalid/asset',
                         sha256=hashlib.sha256(b'reviewed').hexdigest())
            def download(command, **kwargs):
                Path(command[-1]).write_bytes(downloaded)
            with patch.object(fetch.subprocess, 'run', side_effect=download) as run:
                if succeeds:
                    fetch.fetch_tool(entry, destination)
                    self.assertEqual(target.read_bytes(), b'reviewed')
                    run.reset_mock()
                    fetch.fetch_tool(entry, destination)
                    run.assert_not_called()
                else:
                    with self.assertRaisesRegex(RuntimeError, 'Checksum mismatch'):
                        fetch.fetch_tool(entry, destination)
                    self.assertEqual(target.read_bytes(), b'previous')
            self.assertEqual(list(destination.iterdir()), [target])
