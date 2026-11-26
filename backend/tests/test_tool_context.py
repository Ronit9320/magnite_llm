"""Room for a tool task: the llama.cpp window and trimming long tool output."""

import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from backend.inference.engines.llamacpp import LlamaCppEngine
from backend.tooling import pi_bridge


class ToolContextTests(unittest.TestCase):
    def test_llamacpp_context_is_8192_capped_by_the_model(self):
        for trained, expected in ((40960, 8192), (4096, 4096)):
            loads = []

            def llama(**options):
                loads.append(options)
                return SimpleNamespace(close=lambda: None, metadata={
                    "general.architecture": "llama", "llama.context_length": str(trained)})

            with self.subTest(trained=trained), \
                    patch.dict(sys.modules, {"llama_cpp": SimpleNamespace(Llama=llama)}), \
                    patch.object(LlamaCppEngine, "_resolve_gguf_path", return_value="m.gguf"):
                engine = LlamaCppEngine("m")
                engine.load()
                self.assertEqual(engine.context_size(), expected)
                # The weights are loaded once, with a context no larger than the model's.
                self.assertTrue(loads[0]["vocab_only"])
                self.assertEqual([load["n_ctx"] for load in loads if not load.get("vocab_only")], [expected])

    def test_oversized_tool_output_is_trimmed_to_fit(self):
        size = lambda items: sum(len(item["content"]) for item in items)
        messages = [{"role": "user", "content": "q"}, {"role": "tool", "content": "x" * 10_000}]
        fitted = pi_bridge._fit_tool_output(messages, size, 2_000)
        self.assertLessEqual(size(fitted), 2_000)
        self.assertIn("truncated to fit", fitted[1]["content"])
        self.assertEqual(len(messages[1]["content"]), 10_000)
        with self.assertRaisesRegex(ValueError, "longer than"):
            pi_bridge._fit_tool_output([{"role": "user", "content": "x" * 3_000}], size, 2_000)

    def test_a_trimmed_result_keeps_the_step_reminder(self):
        size = lambda items: sum(len(item["content"]) for item in items)
        messages = [{"role": "tool", "content": "x" * 10_000 + "\n\n" + pi_bridge.STEP_REMINDER}]
        fitted = pi_bridge._fit_tool_output(messages, size, 2_000)
        self.assertTrue(fitted[0]["content"].endswith(pi_bridge.STEP_REMINDER))


if __name__ == "__main__":
    unittest.main()
