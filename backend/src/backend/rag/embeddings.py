from __future__ import annotations

import threading

import numpy as np

_embedder = None
_embedder_model: str | None = None
_lock = threading.Lock()


def is_available() -> bool:
    try:
        import sentence_transformers  # noqa: F401
        return True
    except ImportError:
        return False


def _ensure(model: str):
    """Lazily load the sentence-transformers model (thread-safe); may download."""
    global _embedder, _embedder_model
    if _embedder is not None and _embedder_model == model:
        return _embedder
    with _lock:
        if _embedder is None or _embedder_model != model:
            from sentence_transformers import SentenceTransformer

            _embedder = SentenceTransformer(model)
            _embedder_model = model
    return _embedder


def embed_texts(texts: list[str], model: str) -> np.ndarray:
    """Return an (N, D) float32 matrix of L2-normalized embeddings."""
    if not texts:
        return np.zeros((0, 0), dtype=np.float32)
    model_obj = _ensure(model)
    vecs = model_obj.encode(
        texts,
        normalize_embeddings=True,
        convert_to_numpy=True,
        show_progress_bar=False,
    )
    return np.asarray(vecs, dtype=np.float32).reshape(len(texts), -1)


def dimension(model: str) -> int:
    return embed_texts([" "], model).shape[1]


def cosine_scores(matrix: np.ndarray, query: np.ndarray) -> np.ndarray:
    """Dot product over L2-normalized vectors == cosine similarity."""
    return matrix @ query.reshape(-1)