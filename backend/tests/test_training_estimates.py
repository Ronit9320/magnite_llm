"""Resource budgets are advisory and must not invent calibration data."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch, MagicMock

from backend.training.estimates import estimate, local_config, memory_budget
from backend.training.schemas import TrainingConfig, TrainingJob
from backend.routes import training

CONFIG = {"model_type": "llama", "hidden_size": 512, "num_hidden_layers": 8,
          "vocab_size": 32000, "intermediate_size": 1024, "num_attention_heads": 8}
MEMORY = {"kind": "VRAM", "available_bytes": 8*1024**3, "total_bytes": 12*1024**3}


class ResourceEstimateTests(unittest.TestCase):
    def setUp(self):
        self.cfg = TrainingConfig(base_model="test", dataset_id="data")
        self.config_patch = patch("backend.training.estimates.local_config", return_value=CONFIG)
        self.memory_patch = patch("backend.training.estimates.memory_budget", return_value=MEMORY)
        self.config_patch.start(); self.memory_patch.start()
        self.addCleanup(self.config_patch.stop); self.addCleanup(self.memory_patch.stop)

    def test_model_settings_change_memory_adapter_and_steps_budget(self):
        base = estimate(self.cfg, 100)
        large = estimate(self.cfg.model_copy(update={"rank": 64, "batch_size": 4, "max_length": 2048}), 100)
        quantized = estimate(self.cfg.model_copy(update={"quantization": "4bit"}), 100)
        self.assertGreater(base["memory_high_bytes"], base["memory_low_bytes"])
        self.assertGreater(large["adapter_bytes"], base["adapter_bytes"])
        self.assertGreater(large["memory_high_bytes"], base["memory_high_bytes"])
        self.assertLess(quantized["memory_low_bytes"], base["memory_low_bytes"])
        self.assertEqual(base["steps"], 300)
        self.assertEqual(large["steps"], 75)
        self.assertEqual(base["memory"], MEMORY)
        self.assertIsNone(base["duration_high_seconds"])

    def test_validation_rows_are_excluded_from_step_budget(self):
        budget = estimate(self.cfg.model_copy(update={"validation_split": 0.1}), 101)
        self.assertEqual(budget["steps"], 270)

    def test_duration_needs_actual_matching_completed_run(self):
        prior = TrainingJob(id="p", name="p", base_model="test", dataset_id="d", status="done",
            config=self.cfg.model_dump(), history=[{"step": 1, "loss": 2., "elapsed_seconds": 1.},
                                                  {"step": 11, "loss": 1., "elapsed_seconds": 21.}])
        budget = estimate(self.cfg, 100, [prior])
        self.assertEqual(budget["duration_low_seconds"], 420)
        self.assertEqual(budget["duration_high_seconds"], 900)
        for other in (prior.model_copy(update={"status": "error"}), prior.model_copy(update={"base_model": "other"})):
            self.assertIsNone(estimate(self.cfg, 100, [other])["duration_high_seconds"])
        self.assertIsNone(estimate(self.cfg.model_copy(update={"batch_size": 2}), 100, [prior])["duration_high_seconds"])

    def test_missing_and_unsupported_architectures_are_explicit(self):
        with patch("backend.training.estimates.local_config", side_effect=ValueError("Uncached model")):
            budget = estimate(self.cfg, 100)
        self.assertIsNone(budget["memory_low_bytes"])
        self.assertIsNone(budget["adapter_bytes"])
        self.assertIn("Uncached", budget["detail"])
        with patch("backend.training.estimates.local_config", return_value={"model_type": "unsupported"}):
            self.assertIsNone(estimate(self.cfg, 100)["memory_high_bytes"])

    def test_route_uses_registered_dataset_and_local_history(self):
        datasets = MagicMock(); datasets.get.return_value.rows = 100
        manager = MagicMock(); manager.list.return_value = []
        with patch.object(training, "_get_datasets", return_value=datasets), patch.object(training, "_get_manager", return_value=manager):
            self.assertEqual(training.training_estimate(self.cfg)["steps"], 300)


class EstimateProbeTests(unittest.TestCase):
    def test_nvidia_probe_uses_one_device_not_sum_of_gpus(self):
        with patch("backend.training.estimates.subprocess.run") as run:
            run.return_value.stdout = "4096, 8192\n8192, 16384\n"
            result = memory_budget()
        self.assertEqual(result["available_bytes"], 4096*1024**2)
        self.assertEqual(result["total_bytes"], 8192*1024**2)

    def test_local_configuration_does_not_require_loading_ml(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory)/"config.json"
            config.write_text('{"model_type":"llama"}')
            self.assertEqual(local_config(directory), {"model_type": "llama"})
            config.write_text('[]')
            with self.assertRaisesRegex(ValueError, "object"):
                local_config(directory)


if __name__ == "__main__":
    unittest.main()
