"""4-bit/8-bit QLoRA is refused up front where it cannot run (ISSUE-010)."""
import types
import unittest
from unittest.mock import patch

from backend.routes import training as training_routes
from backend.training import jobs, trainer
from backend.training.schemas import TrainingConfig


class QuantizationSupportTests(unittest.TestCase):
    def test_platform_and_package_decide_support(self):
        cases = (("darwin", object(), False), ("linux", None, False), ("linux", object(), True))
        for platform, spec, expected in cases:
            with patch.object(trainer.sys, "platform", platform), \
                    patch.object(trainer.importlib.util, "find_spec", return_value=spec):
                supported, reason = trainer.quantization_support()
            self.assertEqual(supported, expected, platform)
            self.assertEqual(bool(reason), not expected)

    def test_validate_rejects_quantization_only_when_unsupported(self):
        manager = types.SimpleNamespace(_datasets=types.SimpleNamespace(get=lambda _id: object()))
        cfg = TrainingConfig(base_model="m", dataset_id="d", quantization="4bit")
        with patch.object(jobs, "quantization_support", return_value=(False, "no QLoRA here")):
            with self.assertRaisesRegex(ValueError, "no QLoRA here"):
                jobs.TrainingJobManager._validate(manager, cfg)
            jobs.TrainingJobManager._validate(manager, cfg.model_copy(update={"quantization": "none"}))
        with patch.object(jobs, "quantization_support", return_value=(True, "")):
            jobs.TrainingJobManager._validate(manager, cfg)

    def test_capabilities_route_reports_support(self):
        with patch.object(training_routes, "quantization_support", return_value=(False, "why")):
            self.assertEqual(training_routes.training_capabilities(),
                             {"quantization": False, "quantization_detail": "why"})


if __name__ == "__main__":
    unittest.main()
