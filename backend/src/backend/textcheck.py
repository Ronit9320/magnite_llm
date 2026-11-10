"""Shared input checks that turn a cryptic failure into a clear message."""

# Pasted or uploaded dataset/document text: far above any real file, far below swapping the machine.
# Enforced in the routes, not as a pydantic max_length: a constrained pydantic str rejects lone
# surrogates, and FastAPI's 422 handler then crashes trying to echo them back (a 500).
MAX_CONTENT_CHARS = 16_000_000


def ensure_utf8(text: str, what: str) -> None:
    """Raise ValueError naming the problem if ``text`` cannot be written as UTF-8.

    The usual cause is a lone surrogate (e.g. ``\\ud800``), which JSON allows but UTF-8 does not.
    """
    try:
        text.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise ValueError(f"{what} contains an invalid character (lone surrogate at position {exc.start}); "
                         "remove it and try again.") from None
