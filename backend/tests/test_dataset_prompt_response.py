import csv
import io
import json
from pathlib import Path
import tempfile
import unittest

from backend.training.datasets import DatasetStore, _normalize_csv, _normalize_jsonl


class PromptResponseDatasetTests(unittest.TestCase):
    def setUp(self):
        self.sample = {"prompt": "Explain this code:\nprint('hello')", "response": "It prints hello.\nनमस्ते!"}
        self.expected = {"messages": [
            {"role": "user", "content": self.sample["prompt"]},
            {"role": "assistant", "content": self.sample["response"]},
        ]}

    def test_jsonl_preserves_turns_multiline_and_unicode(self):
        self.assertEqual(_normalize_jsonl(json.dumps(self.sample)), [self.expected])

    def test_csv_uses_the_same_chat_normalization(self):
        content = io.StringIO()
        writer = csv.DictWriter(content, fieldnames=["prompt", "response"])
        writer.writeheader()
        writer.writerow(self.sample)
        self.assertEqual(_normalize_csv(content.getvalue()), [self.expected])

    def test_invalid_fields_report_the_correct_row(self):
        for field in ("prompt", "response"):
            for value in (None, False, 1, [], {}, "", " \n\t"):
                with self.subTest(field=field, value=value):
                    invalid = {**self.sample, field: value}
                    content = json.dumps(self.sample) + "\n" + json.dumps(invalid)
                    with self.assertRaisesRegex(ValueError, "Row 2: prompt and response must be nonempty strings"):
                        _normalize_jsonl(content)

    def test_file_import_persists_normalized_training_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.jsonl"
            original = json.dumps(self.sample, ensure_ascii=False) + "\n"
            source.write_text(original, encoding="utf-8")
            store = DatasetStore(str(Path(directory) / "datasets"), str(Path(directory) / "data.db"))
            record = store.ingest_path(str(source))
            self.assertEqual(record.rows, 1)
            self.assertEqual(json.loads(Path(record.path).read_text(encoding="utf-8")), self.expected)
            self.assertEqual(source.read_text(encoding="utf-8"), original)
