"""One batched embedding loop shared by reindex and large-document ingest.

Each batch is checked for cancellation and memory *before* it is embedded, so a cancelled or
over-budget run stops within one batch and never leaves partial results behind (callers write into a
buffer they own and only commit when this returns).
"""
from __future__ import annotations

import threading
from typing import Callable, Iterable, Optional

import numpy as np


NON_FINITE = ("The embedding model produced invalid numbers (NaN or infinity), so nothing was stored. "
              "This can happen with half-precision on a GPU: set MAGNITE_RAG_DTYPE=float32 and try again.")


class IngestCancelled(RuntimeError):
    """Raised between batches when the client went away or the operation was cancelled."""


def embed_batches(batches: Iterable[list], embed_fn: Callable[[list], np.ndarray],
                  write: Callable[[int, np.ndarray], None],
                  cancelled: Optional[threading.Event] = None,
                  progress: Optional[Callable[[int], None]] = None,
                  guard=None) -> int:
    """Embed each batch of texts and hand the vectors to ``write(offset, vectors)``.

    Returns the number of texts embedded. Raises ``IngestCancelled`` if ``cancelled`` is set, and lets
    ``guard.check()`` raise (``MemoryGuardTripped``) before the next batch starts.
    """
    done = 0
    for batch in batches:
        if cancelled is not None and cancelled.is_set():
            raise IngestCancelled("Operation cancelled")
        if guard is not None:
            guard.check()
        vectors = np.asarray(embed_fn(batch), dtype=np.float32)
        if vectors.ndim != 2 or vectors.shape[0] != len(batch):
            raise ValueError(f"Embedder returned {vectors.shape[0] if vectors.ndim else 0} vectors "
                             f"for {len(batch)} texts")
        if not np.isfinite(vectors).all():
            raise ValueError(NON_FINITE)
        write(done, vectors)
        done += len(batch)
        if progress is not None:
            progress(done)
    return done
