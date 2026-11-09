import subprocess
import unittest
from unittest.mock import patch

from backend.training.telemetry import device_telemetry


class DeviceTelemetryTests(unittest.TestCase):
    @patch("backend.training.telemetry.subprocess.run")
    def test_one_device_is_reported_without_aggregating(self, run):
        run.return_value.stdout = "NVIDIA RTX 3060, 7475, 12288, 94, 68, 142.5\nOther GPU, 100, 8000, 5, 35, 30\n"
        data = device_telemetry()
        self.assertEqual(data["device"], "NVIDIA RTX 3060")
        self.assertEqual(data["used_bytes"], 7475 * 1024**2)
        self.assertEqual(data["total_bytes"], 12288 * 1024**2)
        self.assertEqual(data["utilization_percent"], 94)
        self.assertEqual(data["power_w"], 142.5)

    @patch("backend.training.telemetry.subprocess.run")
    def test_unsupported_and_nonfinite_readings_are_unknown(self, run):
        run.return_value.stdout = "GPU, 0, 12000, 999, N/A, nan\n"
        data = device_telemetry()
        self.assertEqual(data["used_bytes"], 0)
        self.assertIsNone(data["utilization_percent"])
        self.assertIsNone(data["temperature_c"])
        self.assertIsNone(data["power_w"])

    @patch("backend.training.estimates.memory_budget", return_value={"kind": "System RAM", "total_bytes": 100, "available_bytes": 30})
    @patch("backend.training.telemetry.subprocess.run", side_effect=FileNotFoundError)
    def test_unavailable_gpu_falls_back_to_labelled_system_memory(self, run, memory):
        data = device_telemetry()
        self.assertEqual(data["kind"], "System RAM")
        self.assertEqual(data["used_bytes"], 70)
        self.assertEqual(data["accelerator"], "SYSTEM")
        self.assertIsNone(data["utilization_percent"])

    @patch("backend.training.estimates.memory_budget", return_value={"kind": "Memory", "total_bytes": None, "available_bytes": None})
    @patch("backend.training.telemetry.subprocess.run", side_effect=subprocess.TimeoutExpired("nvidia-smi", 2))
    def test_probe_timeout_does_not_block_the_page_or_invent_readings(self, run, memory):
        data = device_telemetry()
        self.assertIsNone(data["used_bytes"])
        self.assertIsNone(data["temperature_c"])
