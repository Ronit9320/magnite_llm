"""Context meter and auto-compact: pure functions and routes (offline, fake engine)."""
import unittest

import test_regressions as fixtures
from backend.inference.context import (
    drop_to_fit,
    fill_of,
    split_for_compaction,
    summarize_prompt,
    summary_message,
)
from backend.inference.schemas import ChatMessage, ChatRole


def msgs(*roles, size=2):
    """Messages with content 'm0', 'm1', ... (padded to `size` chars when larger)."""
    out = []
    for i, role in enumerate(roles):
        out.append(ChatMessage(role=ChatRole(role), content=(f"m{i}" + "x" * size)[:max(size, 2)]))
    return out


def alternating(count):
    return msgs(*[("user" if i % 2 == 0 else "assistant") for i in range(count)])


def char_count(messages):
    return sum(len(m.content) for m in messages)


class PureFunctionTests(unittest.TestCase):
    def test_fill_of_basic(self):
        self.assertAlmostEqual(fill_of(700, 100, 1000), 0.8)
        self.assertEqual(fill_of(0, 0, 1000), 0.0)

    def test_fill_of_over_and_zero_limit(self):
        self.assertAlmostEqual(fill_of(900, 500, 1000), 1.4)
        self.assertEqual(fill_of(1, 1, 0), float("inf"))

    def test_split_fewer_than_keep_recent(self):
        self.assertEqual(split_for_compaction(alternating(3), 4), 0)

    def test_split_keeps_pair_whole(self):
        self.assertEqual(split_for_compaction(alternating(8), 3), 4)

    def test_split_exact_boundary_on_user(self):
        self.assertEqual(split_for_compaction(alternating(8), 4), 4)

    def test_split_leading_system_message(self):
        messages = msgs("system", "user", "assistant", "user", "assistant", "user", "assistant")
        self.assertEqual(split_for_compaction(messages, 2), 5)

    def test_split_keep_recent_zero_still_keeps_last_message(self):
        messages = alternating(6)
        result = split_for_compaction(messages, 0)
        self.assertEqual(result, 4)
        self.assertLess(result, len(messages))

    def test_split_empty_history(self):
        self.assertEqual(split_for_compaction([], 4), 0)

    def test_drop_to_fit_drops_just_enough(self):
        messages = msgs(*(["user", "assistant"] * 3), size=100)
        self.assertEqual(drop_to_fit(char_count, messages, "", 0, 1000, 0.45), 2)

    def test_drop_to_fit_counts_previous_summary(self):
        messages = msgs(*(["user", "assistant"] * 3), size=100)
        without = drop_to_fit(char_count, messages, "", 0, 1000, 0.45)
        with_summary = drop_to_fit(char_count, messages, "s" * 200, 0, 1000, 0.45)
        self.assertGreater(with_summary, without)

    def test_drop_to_fit_never_drops_last_message(self):
        messages = msgs(*(["user", "assistant"] * 3), size=100)
        self.assertEqual(drop_to_fit(char_count, messages, "", 0, 10, 0.5), len(messages) - 1)

    def test_summary_message_shape(self):
        message = summary_message("they discussed cats")
        self.assertEqual(message.role, ChatRole.system)
        self.assertTrue(message.content.startswith("Summary of the earlier conversation: "))
        self.assertIn("they discussed cats", message.content)

    def test_summarize_prompt_includes_previous_summary_and_transcript(self):
        messages = msgs("user", "assistant", size=10)
        prompt = summarize_prompt(messages, "EARLIER-SUMMARY")
        self.assertEqual(prompt[0].role, ChatRole.system)
        text = " ".join(m.content for m in prompt)
        self.assertIn("EARLIER-SUMMARY", text)
        self.assertIn(messages[0].content, text)
        self.assertIn(messages[1].content, text)


class UsageRouteTests(unittest.IsolatedAsyncioTestCase):
    asyncSetUp = fixtures.ApiTests.asyncSetUp
    asyncTearDown = fixtures.ApiTests.asyncTearDown

    async def post(self, messages, max_tokens=100):
        return await self.client.post(
            "/inference/context-usage",
            json={"messages": messages, "max_tokens": max_tokens},
        )

    async def test_usage_numbers(self):
        response = await self.post([
            {"role": "user", "content": "x" * 10},
            {"role": "assistant", "content": "y" * 10},
        ])
        self.assertEqual(response.status_code, 200, response.text)
        body = response.json()
        self.assertEqual(body["used"], 36)
        self.assertEqual(body["reserved"], 100)
        self.assertEqual(body["limit"], 2048)
        self.assertAlmostEqual(body["fill"], (36 + 100) / 2048)
        self.assertIs(body["exact"], True)

    async def test_usage_no_model_is_409(self):
        self.sup._engine = None
        self.assertEqual((await self.post([])).status_code, 409)

    async def test_usage_not_blocked_while_busy(self):
        with self.sup.operation():
            response = await self.post([{"role": "user", "content": "hi"}])
        self.assertEqual(response.status_code, 200, response.text)

    async def test_usage_empty_messages(self):
        response = await self.post([])
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(response.json()["used"], 0)

    async def test_usage_rejects_bad_max_tokens(self):
        self.assertEqual((await self.post([], max_tokens=0)).status_code, 422)

    async def test_estimate_flag_false_for_other_engines(self):
        self.sup._engine.name = "llamacpp"
        response = await self.post([{"role": "user", "content": "hi"}])
        self.assertIs(response.json()["exact"], False)


class SummaryEngine(fixtures.FakeEngine):
    """FakeEngine with a small window whose chat_stream returns a scripted summary."""

    def __init__(self, *args, summary="short", error=None, **kwargs):
        super().__init__(*args, **kwargs)
        self.summary = summary
        self.error = error
        self.calls = []

    def context_size(self):
        return 600

    async def chat_stream(self, messages, **kwargs):
        self.calls.append(list(messages))
        yield self.summary[: len(self.summary) // 2]
        if self.error:
            raise self.error
        yield self.summary[len(self.summary) // 2:]


def history(count=10, size=40):
    return [{"role": "user" if i % 2 == 0 else "assistant", "content": f"{i}".ljust(size, "x")}
            for i in range(count)]


class CompactRouteTests(unittest.IsolatedAsyncioTestCase):
    asyncSetUp = fixtures.ApiTests.asyncSetUp
    asyncTearDown = fixtures.ApiTests.asyncTearDown

    def use_engine(self, **kwargs):
        self.engine = SummaryEngine("test", **kwargs)
        self.sup._engine = self.engine

    async def compact(self, messages=None, **extra):
        body = {"messages": history() if messages is None else messages, "max_tokens": 100,
                "target_fill": 0.85, "keep_recent": 4}
        body.update(extra)
        return await self.client.post("/inference/compact", json=body)

    def remaining_fill(self, messages, covered, summary=""):
        kept = [ChatMessage(**m) for m in messages[covered:]]
        if summary:
            kept = [summary_message(summary)] + kept
        return fill_of(self.engine.count_messages(kept), 100, self.engine.context_size())

    async def test_summary_used(self):
        self.use_engine(summary="short")
        response = await self.compact()
        self.assertEqual(response.status_code, 200, response.text)
        body = response.json()
        self.assertEqual(body["method"], "summary")
        self.assertEqual(body["summary"], "short")
        self.assertEqual(body["covered"], 6)
        self.assertEqual(body["covered"], split_for_compaction([ChatMessage(**m) for m in history()], 4))

    async def test_nothing_to_compact(self):
        self.use_engine()
        response = await self.compact(history(3), previous_summary="kept")
        body = response.json()
        self.assertEqual((body["covered"], body["summary"]), (0, "kept"))
        self.assertEqual(self.engine.calls, [])

    async def test_engine_error_falls_back_to_dropped(self):
        self.use_engine(summary="partial text", error=RuntimeError("boom"))
        body = (await self.compact()).json()
        self.assertEqual(body["method"], "dropped")
        self.assertGreater(body["covered"], 0)
        self.assertEqual(body["summary"], "")

    async def test_empty_summary_falls_back(self):
        self.use_engine(summary="   ")
        self.assertEqual((await self.compact()).json()["method"], "dropped")

    async def test_too_large_summary_falls_back(self):
        self.use_engine(summary="s" * 5000)
        body = (await self.compact()).json()
        self.assertEqual(body["method"], "dropped")
        self.assertLess(self.remaining_fill(history(), body["covered"]), 0.85)

    async def test_previous_summary_given_to_engine_and_kept_on_drop(self):
        self.use_engine(summary="   ")
        body = (await self.compact(previous_summary="OLD-SUMMARY")).json()
        sent = " ".join(m.content for m in self.engine.calls[0])
        self.assertIn("OLD-SUMMARY", sent)
        self.assertEqual(body["summary"], "OLD-SUMMARY")
        self.assertEqual(body["method"], "dropped")

    async def test_result_below_target(self):
        self.use_engine(summary="")
        body = (await self.compact(target_fill=0.5)).json()
        self.assertEqual(body["method"], "dropped")
        self.assertLess(self.remaining_fill(history(), body["covered"]), 0.5)

    async def test_dropped_can_cover_nothing_when_chat_already_fits(self):
        # Characterization: the client must not show a "dropped" divider when covered == 0.
        self.use_engine(summary="")
        body = (await self.compact(history(6, size=10), target_fill=0.95)).json()
        self.assertEqual((body["method"], body["covered"]), ("dropped", 0))

    async def test_kept_messages_alone_over_target_skips_compaction(self):
        # 4 kept messages + reserve = 292/600 = 0.49, already above a 0.3 target: compaction cannot help.
        self.use_engine(summary="short", )
        body = (await self.compact(target_fill=0.3, previous_summary="keep me")).json()
        self.assertEqual((body["covered"], body["summary"]), (0, "keep me"))
        self.assertEqual(self.engine.calls, [])

    async def test_reserve_at_or_above_window_skips_compaction(self):
        self.use_engine(summary="short")
        body = (await self.compact(max_tokens=700)).json()
        self.assertEqual(body["covered"], 0)
        self.assertEqual(self.engine.calls, [])

    async def test_no_model_is_409(self):
        self.sup._engine = None
        self.assertEqual((await self.compact()).status_code, 409)

    async def test_busy_is_409(self):
        self.use_engine()
        with self.sup.operation():
            response = await self.compact()
        self.assertEqual(response.status_code, 409)

    async def test_validation(self):
        self.use_engine()
        self.assertEqual((await self.compact(target_fill=0)).status_code, 422)
        self.assertEqual((await self.compact(keep_recent=-1)).status_code, 422)


if __name__ == "__main__":
    unittest.main()
