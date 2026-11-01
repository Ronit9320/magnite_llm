"""Exact download size of a Hugging Face repo.

The Hub's search API cannot return file sizes, and parameter counts would badly
understate a download: the app fetches every file in a repo, so a 135M-parameter
model's repo can be ~2 GB. The only exact figure is the sum of the repo's file
sizes from its metadata. The download worker and the search-result sizes both use
this, so the size shown before a download is the total shown while it runs.
"""
from __future__ import annotations

DEFAULT_TIMEOUT = 10.0


def repo_size_bytes(api, repo_id: str, timeout: float = DEFAULT_TIMEOUT) -> int:
    """Total bytes of every file in the repo; 0 if the Hub reports none.

    Raises whatever the Hub client raises (offline, gated, not found): callers decide
    how much a missing size matters.
    """
    info = api.model_info(repo_id, files_metadata=True, timeout=timeout)
    return sum(int(f.size or 0) for f in (info.siblings or []))
