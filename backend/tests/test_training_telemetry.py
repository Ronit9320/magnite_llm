"""Raw telemetry and held-out validation regressions without ML imports."""
import math
import tempfile
import unittest
from unittest.mock import MagicMock

from pydantic import ValidationError

from backend.training.jobs import TrainingJobManager
from backend.training.schemas import DatasetRecord, TrainingConfig, TrainingJob
from backend.training.trainer import record_metrics, split_dataset


def job():
    return TrainingJob(id="test", name="test", status="running", base_model="test", dataset_id="test")


class TelemetryTests(unittest.TestCase):
    def test_training_and_validation_are_separate_measured_samples(self):
        run = job()
        record_metrics(run, 2, {"loss": 1.2, "learning_rate": 0.0002, "grad_norm": 0.4}, 0.5, 0.25)
        snapshot = run.model_copy()
        record_metrics(run, 4, {"eval_loss": 1.1}, 1.5, 1.0)
        self.assertEqual(run.loss, 1.2)
        self.assertEqual(run.history[1], {"step": 4, "elapsed_seconds": 1.5, "epoch": 1.0, "eval_loss": 1.1})
        self.assertNotIn("learning_rate", run.history[1])
        self.assertEqual(len(snapshot.history), 1)
        self.assertEqual(TrainingJob.model_validate_json(run.model_dump_json()).history, run.history)

    def test_missing_nonfinite_and_summary_logs_do_not_invent_zeroes(self):
        run = job()
        record_metrics(run, 1, {"loss": math.nan, "grad_norm": math.inf}, 0.1)
        record_metrics(run, 2, {"train_loss": 1.0, "train_runtime": 0.1}, 0.2)
        self.assertEqual(run.history, [])
        self.assertIsNone(run.loss)
        record_metrics(run, 3, {"learning_rate": 0.0, "loss": 0.0}, 0.3)
        self.assertEqual(run.history[0]["loss"], 0.0)
        self.assertNotIn("grad_norm", run.history[0])

    def test_history_is_bounded_with_actual_step_numbers(self):
        run = job()
        for i in range(1005):
            record_metrics(run, i * 3, {"loss": 1.0}, i * 0.1)
        self.assertEqual(len(run.history), 1000)
        self.assertEqual(run.history[0]["step"], 15)
        self.assertEqual(run.history[-1]["step"], 3012)

    def test_validation_split_is_seeded_and_never_empties_training(self):
        data = MagicMock()
        data.__len__.return_value = 3
        data.train_test_split.return_value = {"train": "train", "test": "held-out"}
        cfg = TrainingConfig(base_model="test", dataset_id="test", validation_split=0.5, seed=17)
        self.assertEqual(split_dataset(data, cfg), ("train", "held-out"))
        data.train_test_split.assert_called_once_with(test_size=2, seed=17)
        self.assertEqual(split_dataset([1], cfg.model_copy(update={"validation_split": 0})), ([1], None))
        with self.assertRaisesRegex(ValueError, "two dataset rows"):
            split_dataset([1], cfg)

    def test_terminal_raw_history_survives_manager_restart(self):
        with tempfile.TemporaryDirectory() as directory:
            manager = TrainingJobManager(directory, MagicMock(), MagicMock())
            run = job()
            record_metrics(run, 3, {"loss": 0.9, "grad_norm": 0.2}, 1.0, 1.0)
            record_metrics(run, 3, {"eval_loss": 1.1}, 1.2, 1.0)
            run.status = "done"
            manager._persist(run)
            manager.shutdown()
            restored = TrainingJobManager(directory, MagicMock(), MagicMock())
            try:
                self.assertEqual(restored.get(run.id).history, run.history)
                self.assertEqual(restored.get(run.id).status, "done")
            finally:
                restored.shutdown()

    def test_invalid_split_and_dropout_rejected(self):
        for fields in ({"validation_split": -0.1}, {"validation_split": 0.51}, {"dropout": 1.0}):
            with self.assertRaises(ValidationError):
                TrainingConfig(base_model="test", dataset_id="test", **fields)

    def test_one_row_validation_rejected_before_job_queued(self):
        with tempfile.TemporaryDirectory() as directory:
            datasets = MagicMock()
            datasets.get.return_value = DatasetRecord(id="test", name="test", format="jsonl", path="unused", rows=1)
            manager = TrainingJobManager(directory, MagicMock(), datasets)
            try:
                with self.assertRaisesRegex(ValueError, "two dataset rows"):
                    manager.start(TrainingConfig(base_model="test", dataset_id="test", validation_split=0.1))
                self.assertEqual(manager.list(), [])
                datasets.acquire.assert_not_called()
            finally:
                manager.shutdown()


if __name__ == "__main__":
    unittest.main()
