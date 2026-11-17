import os
import sys
import threading
import time
import types
import unittest
from unittest.mock import patch

from backend.rag import memguard
from backend.rag.memguard import MemoryGuard, MemoryGuardTripped, MemoryLimits


def reader(values):
    """Returns a callable that yields the given values in order, then repeats the last."""
    state = {"i": 0}

    def read():
        v = values[min(state["i"], len(values) - 1)]
        state["i"] += 1
        return v
    return read


LIMITS = MemoryLimits(max_footprint_mb=1000, min_available_mb=400, max_swap_growth_mb=500)


class GuardCheckTests(unittest.TestCase):
    def make(self, footprint, available, swap, limits=LIMITS):
        return MemoryGuard(limits, read_footprint=reader(footprint),
                           read_available=reader(available), read_swap=reader(swap))

    def test_within_limits_passes_and_records_peak(self):
        guard = self.make([200, 900, 500], [3000], [100])
        for _ in range(3):
            guard.check()
        self.assertEqual(guard.peak_footprint_mb, 900)

    def test_trips_when_process_footprint_exceeds_limit(self):
        guard = self.make([200, 1001], [3000], [100])
        guard.check()
        with self.assertRaises(MemoryGuardTripped) as ctx:
            guard.check()
        self.assertIn("footprint", str(ctx.exception))

    def test_trips_when_system_available_memory_is_below_floor(self):
        guard = self.make([200], [3000, 399], [100])
        guard.check()
        with self.assertRaises(MemoryGuardTripped) as ctx:
            guard.check()
        self.assertIn("available", str(ctx.exception))

    def test_trips_on_swap_growth_since_guard_started(self):
        guard = self.make([200], [3000], [2000, 2400, 2501])   # baseline 2000 read at construction
        guard.check()
        with self.assertRaises(MemoryGuardTripped) as ctx:
            guard.check()
        self.assertIn("swap", str(ctx.exception))

    def test_existing_swap_is_not_a_trip(self):
        guard = self.make([200], [3000], [3500])
        guard.check()
        guard.check()

    def test_unreadable_values_do_not_trip(self):
        guard = self.make([0], [0], [0])   # 0 means "this platform cannot tell"
        guard.check()


class PreflightTests(unittest.TestCase):
    def test_raises_when_available_is_below_required_with_headroom(self):
        with self.assertRaises(MemoryGuardTripped) as ctx:
            memguard.preflight(required_mb=2000, available_mb=2500, headroom=1.5)
        self.assertIn("2000", str(ctx.exception))

    def test_passes_with_enough_memory(self):
        memguard.preflight(required_mb=2000, available_mb=3001, headroom=1.5)

    def test_reads_available_memory_when_not_given(self):
        with patch.object(memguard, "available_memory_mb", return_value=100.0):
            with self.assertRaises(MemoryGuardTripped):
                memguard.preflight(required_mb=500)

    def test_unknown_available_memory_does_not_block(self):
        memguard.preflight(required_mb=500, available_mb=0)


class WatchTests(unittest.TestCase):
    def test_watch_calls_on_trip_once_when_limit_is_crossed(self):
        guard = MemoryGuard(LIMITS, read_footprint=reader([100, 100, 5000]),
                            read_available=reader([3000]), read_swap=reader([0]))
        tripped = []
        done = threading.Event()

        def on_trip(exc):
            tripped.append(exc)
            done.set()

        with guard.watch(on_trip, interval=0.01):
            self.assertTrue(done.wait(2))
            time.sleep(0.05)
        self.assertEqual(len(tripped), 1)
        self.assertIsInstance(tripped[0], MemoryGuardTripped)

    def test_watch_thread_stops_on_exit(self):
        guard = MemoryGuard(LIMITS, read_footprint=reader([100]), read_available=reader([3000]),
                            read_swap=reader([0]))
        before = threading.active_count()
        with guard.watch(lambda exc: None, interval=0.01):
            time.sleep(0.05)
        time.sleep(0.05)
        self.assertLessEqual(threading.active_count(), before)


class LimitsTests(unittest.TestCase):
    def test_defaults_scale_with_total_ram_and_are_capped(self):
        small = memguard.limits_from_env(total_mb=8192, env={})
        big = memguard.limits_from_env(total_mb=65536, env={})
        self.assertEqual(small.max_footprint_mb, int(8192 * 0.35))
        self.assertEqual(big.max_footprint_mb, 8192)           # cap
        self.assertEqual(small.min_available_mb, 400)
        self.assertEqual(small.max_swap_growth_mb, 1024)

    def test_env_overrides(self):
        lim = memguard.limits_from_env(total_mb=8192, env={
            "MAGNITE_RAG_MAX_MB": "1500", "MAGNITE_RAG_MIN_FREE_MB": "900", "MAGNITE_RAG_MAX_SWAP_MB": "256"})
        self.assertEqual((lim.max_footprint_mb, lim.min_available_mb, lim.max_swap_growth_mb), (1500, 900, 256))

    def test_bad_env_values_fall_back_to_defaults(self):
        lim = memguard.limits_from_env(total_mb=8192, env={"MAGNITE_RAG_MAX_MB": "lots"})
        self.assertEqual(lim.max_footprint_mb, int(8192 * 0.35))


class AcceleratorTests(unittest.TestCase):
    def test_apply_mps_limits_sets_defaults_without_overriding_user_values(self):
        env = {"PYTORCH_MPS_HIGH_WATERMARK_RATIO": "0.9"}
        memguard.apply_mps_limits(env)
        self.assertEqual(env["PYTORCH_MPS_HIGH_WATERMARK_RATIO"], "0.9")
        self.assertEqual(env["PYTORCH_MPS_LOW_WATERMARK_RATIO"], "0.3")
        env2 = {}
        memguard.apply_mps_limits(env2)
        self.assertEqual(env2["PYTORCH_MPS_HIGH_WATERMARK_RATIO"], "0.5")

    def test_release_is_a_noop_when_torch_was_never_imported(self):
        with patch.dict(sys.modules, {"torch": None}):
            memguard.release_accelerator_memory()   # must not import torch or raise

    def test_release_empties_caches_when_torch_is_loaded(self):
        calls = []
        fake = types.SimpleNamespace(
            mps=types.SimpleNamespace(empty_cache=lambda: calls.append("mps")),
            cuda=types.SimpleNamespace(empty_cache=lambda: calls.append("cuda"),
                                       is_available=lambda: True),
            backends=types.SimpleNamespace(mps=types.SimpleNamespace(is_available=lambda: True)),
        )
        with patch.dict(sys.modules, {"torch": fake}):
            memguard.release_accelerator_memory()
        self.assertEqual(sorted(calls), ["cuda", "mps"])


class RealReaderTests(unittest.TestCase):
    def test_readers_return_sane_numbers_on_this_machine(self):
        self.assertGreater(memguard.process_footprint_mb(), 10)
        self.assertGreater(memguard.available_memory_mb(), 0)
        self.assertGreaterEqual(memguard.swap_used_mb(), 0)
        self.assertGreater(memguard.total_memory_mb(), 1000)

    def test_footprint_tracks_a_real_allocation(self):
        before = memguard.process_footprint_mb()
        block = bytearray(200 * 2**20)
        block[::4096] = b"x" * len(block[::4096])       # touch every page
        after = memguard.process_footprint_mb()
        del block
        self.assertGreater(after - before, 150)


class LinuxReaderTests(unittest.TestCase):
    """The Linux branches, driven by realistic /proc contents (these tests run on any OS)."""

    MEMINFO = ("MemTotal:       32768000 kB\nMemFree:         1024000 kB\nMemAvailable:   20480000 kB\n"
               "Buffers:          512000 kB\nSwapTotal:       8388608 kB\nSwapFree:        6291456 kB\n")
    STATUS = "Name:\tpython\nVmPeak:\t 9000000 kB\nVmRSS:\t 2048000 kB\nVmSwap:\t  512000 kB\nThreads:\t4\n"

    def run_linux(self, fn):
        import builtins
        real_open = builtins.open

        def fake_open(path, *args, **kwargs):
            import io
            if path == "/proc/meminfo":
                return io.StringIO(self.MEMINFO)
            if path == "/proc/self/status":
                return io.StringIO(self.STATUS)
            return real_open(path, *args, **kwargs)

        with patch.object(memguard.sys, "platform", "linux"), patch("builtins.open", fake_open):
            return fn()

    def test_total_available_and_swap_come_from_meminfo(self):
        self.assertAlmostEqual(self.run_linux(memguard.total_memory_mb), 32768000 / 1024)
        self.assertAlmostEqual(self.run_linux(memguard.available_memory_mb), 20480000 / 1024)
        self.assertAlmostEqual(self.run_linux(memguard.swap_used_mb), (8388608 - 6291456) / 1024)

    def test_footprint_is_resident_plus_swapped_memory(self):
        self.assertAlmostEqual(self.run_linux(memguard.process_footprint_mb), (2048000 + 512000) / 1024)

    def test_default_limits_on_a_32gb_linux_box(self):
        limits = self.run_linux(memguard.limits_from_env)
        self.assertEqual(limits.max_footprint_mb, 8192)            # 35% of 32 GB, capped at 8 GB

    def test_missing_proc_files_mean_unknown_not_a_crash(self):
        import builtins
        real_open = builtins.open

        def no_proc(path, *a, **k):
            if str(path).startswith("/proc/"):
                raise FileNotFoundError(path)
            return real_open(path, *a, **k)

        with patch.object(memguard.sys, "platform", "linux"), patch("builtins.open", no_proc):
            self.assertEqual(memguard.available_memory_mb(), 0.0)
            self.assertEqual(memguard.swap_used_mb(), 0.0)
            memguard.preflight(required_mb=10 ** 6)               # unknown memory never blocks


if __name__ == "__main__":
    unittest.main()
