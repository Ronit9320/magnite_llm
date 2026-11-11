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


class CpuAndMacMemoryTests(unittest.TestCase):
    def setUp(self):
        from backend.training import telemetry
        telemetry._cpu_last_time = None
        telemetry._cpu_last_value = None

    @patch("backend.training.estimates.memory_budget", return_value={"kind": "System RAM", "total_bytes": 100, "available_bytes": 30})
    @patch("backend.training.telemetry.subprocess.run", side_effect=FileNotFoundError)
    def test_first_cpu_sample_is_unknown_then_measured(self, run, memory):
        with patch("psutil.cpu_percent", side_effect=[0.0, 37.5]), patch("psutil.cpu_count", return_value=8), \
             patch("backend.training.telemetry.time.monotonic", side_effect=[0.0, 5.0]):
            first, second = device_telemetry(), device_telemetry()
        self.assertIsNone(first["cpu_percent"])
        self.assertEqual(second["cpu_percent"], 37.5)
        self.assertEqual(second["cpu_count"], 8)

    @patch("backend.training.telemetry.subprocess.run")
    def test_cpu_is_reported_alongside_nvidia_gpu(self, run):
        run.return_value.stdout = "NVIDIA RTX 3060, 7475, 12288, 94, 68, 142.5\n"
        with patch("psutil.cpu_percent", side_effect=[0.0, 12.0]), \
             patch("backend.training.telemetry.time.monotonic", side_effect=[0.0, 5.0]):
            device_telemetry()
            data = device_telemetry()
        self.assertEqual(data["utilization_percent"], 94)
        self.assertEqual(data["cpu_percent"], 12.0)

    @patch("backend.training.telemetry.subprocess.run", side_effect=FileNotFoundError)
    def test_macos_memory_used_comes_from_psutil(self, run):
        from backend.training import estimates
        memory = type("M", (), {"available": 2 * 1024**3})()
        with patch.object(estimates.sys, "platform", "darwin"), \
             patch("backend.training.estimates.subprocess.check_output", return_value=b"8589934592") as check, \
             patch("psutil.virtual_memory", return_value=memory):
            # nvidia-smi is absent for memory_budget too (check_output is only the sysctl call).
            with patch("backend.training.estimates.subprocess.run", side_effect=FileNotFoundError):
                budget = estimates.memory_budget()
                data = device_telemetry()
        self.assertEqual(budget["available_bytes"], 2 * 1024**3)
        self.assertEqual(data["used_bytes"], 6 * 1024**3)

    @patch("backend.training.estimates.memory_budget", return_value={"kind": "Memory", "total_bytes": 100, "available_bytes": None})
    @patch("backend.training.telemetry.subprocess.run", side_effect=FileNotFoundError)
    def test_missing_psutil_leaves_cpu_unknown(self, run, memory):
        import builtins
        real = builtins.__import__

        def blocked(name, *args, **kwargs):
            if name == "psutil":
                raise ImportError("no psutil")
            return real(name, *args, **kwargs)
        with patch("builtins.__import__", blocked):
            data = device_telemetry()
        self.assertIsNone(data["cpu_percent"])
        self.assertIsNone(data["cpu_count"])
        self.assertIsNone(data["used_bytes"])

    @patch("backend.training.estimates.memory_budget", return_value={"kind": "System RAM", "total_bytes": 100, "available_bytes": 30})
    @patch("backend.training.telemetry.subprocess.run", side_effect=FileNotFoundError)
    def test_rapid_callers_never_sample_a_tiny_window(self, run, memory):
        # Training panel and Settings both call this; a window of milliseconds reads as 0%.
        with patch("psutil.cpu_percent", side_effect=[0.0, 55.0]) as sample, \
             patch("backend.training.telemetry.time.monotonic", side_effect=[0.0, 0.2, 0.4, 3.0, 3.2]):
            readings = [device_telemetry()["cpu_percent"] for _ in range(5)]
        self.assertEqual(readings, [None, None, None, 55.0, 55.0])
        self.assertEqual(sample.call_count, 2)
