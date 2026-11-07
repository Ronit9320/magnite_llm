"""Pure helpers for the context meter and auto-compact. No engine or HTTP I/O."""
from __future__ import annotations

from typing import Callable

from backend.inference.schemas import ChatMessage, ChatRole

SUMMARY_PREFIX = "Summary of the earlier conversation: "

_SUMMARIZE_INSTRUCTION = (
    "You condense conversations. Write a short, factual summary of the conversation below "
    "so it can replace the original text. Keep names, numbers, decisions, open questions "
    "and the user's goals. Do not add anything new. Reply with the summary only."
)


def fill_of(used: int, reserved: int, limit: int) -> float:
    """Fraction of the context window taken by the messages plus the reserved reply."""
    if limit <= 0:
        return float("inf")
    return (used + reserved) / limit


def split_for_compaction(messages: list[ChatMessage], keep_recent: int) -> int:
    """Number of leading messages to summarize.

    The last ``keep_recent`` messages stay verbatim, the split moves earlier until the
    kept part starts at a user message (a user/assistant pair is never cut), and the
    final message is always kept. Returns 0 when there is nothing older to summarize.
    """
    if not messages:
        return 0
    index = max(0, min(len(messages) - keep_recent, len(messages) - 1))
    while index > 0 and messages[index].role != ChatRole.user:
        index -= 1
    return index


def summary_message(summary: str) -> ChatMessage:
    return ChatMessage(role=ChatRole.system, content=SUMMARY_PREFIX + summary)


def drop_to_fit(
    count: Callable[[list[ChatMessage]], int],
    messages: list[ChatMessage],
    previous_summary: str,
    reserved: int,
    limit: int,
    target_fill: float,
) -> int:
    """Smallest number of oldest messages to drop so the rest sits below ``target_fill``.

    ``previous_summary`` (if any) stays in the prompt and is counted. The last message is
    never dropped, so when nothing fits the answer is ``len(messages) - 1``.
    """
    prefix = [summary_message(previous_summary)] if previous_summary else []
    last = max(len(messages) - 1, 0)
    for dropped in range(last + 1):
        if fill_of(count(prefix + messages[dropped:]), reserved, limit) < target_fill:
            return dropped
    return last


def summarize_prompt(messages: list[ChatMessage], previous_summary: str) -> list[ChatMessage]:
    """Messages that ask the loaded model to write the summary."""
    lines = []
    if previous_summary:
        lines.append(f"Earlier summary:\n{previous_summary}\n")
    lines.append("Conversation:")
    lines.extend(f"{m.role.value}: {m.content}" for m in messages)
    return [
        ChatMessage(role=ChatRole.system, content=_SUMMARIZE_INSTRUCTION),
        ChatMessage(role=ChatRole.user, content="\n".join(lines)),
    ]
