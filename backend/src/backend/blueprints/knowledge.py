"""Row rules and Knowledge search used by Blueprint nodes."""

from __future__ import annotations

from backend.blueprints.schemas import Issue, Problems


def _row(rows, index: int, node_id: str) -> dict:
    row = rows[index]
    if not isinstance(row, dict):
        raise Problems([Issue(code="invalid_value", node_id=node_id, path=f"inputs.rows[{index}]",
                              message="Each row must be an object")])
    return row


def filter_rows(rows: list, field: str, operator: str, value, node_id: str) -> list:
    """Keep rows whose field passes the rule. Rows without the field are left out."""
    from backend.blueprints.runner import compare

    kept = []
    for index in range(len(rows)):
        row = _row(rows, index, node_id)
        if field not in row:
            continue
        try:
            if compare(operator, row[field], value):
                kept.append(row)
        except TypeError as exc:
            raise Problems([Issue(code="invalid_value", node_id=node_id, path=f"inputs.rows[{index}].{field}",
                                  message=f"Cannot apply '{operator}' to this value")]) from exc
    return kept


def sort_rows(rows: list, field: str, descending: bool, node_id: str) -> list:
    """Stable sort by one field. Rows without the field go last in either direction."""
    present, missing = [], []
    kinds = set()
    for index in range(len(rows)):
        row = _row(rows, index, node_id)
        if field in row and row[field] is not None:
            value = row[field]
            kinds.add("number" if isinstance(value, (int, float)) and not isinstance(value, bool)
                      else type(value).__name__)
            present.append(row)
        else:
            missing.append(row)
    if len(kinds) > 1 or kinds - {"number", "str", "bool"}:
        raise Problems([Issue(code="invalid_value", node_id=node_id, path=f"inputs.rows[*].{field}",
                              message="Sort needs one kind of value in the field: numbers, text, or booleans")])
    return sorted(present, key=lambda row: row[field], reverse=descending) + missing


def search(query: str, top_k: int | None, min_score: float | None) -> tuple[list[dict], str]:
    """Search the Knowledge collection. No match returns empty results, not an error."""
    from backend.rag.context import shape_query
    from backend.textcheck import ensure_utf8
    from backend.rag.embeddings import embed_query, is_available
    from backend.rag.store import get_rag_store

    ensure_utf8(query, "Query")
    store = get_rag_store()
    if store is None or not is_available():
        raise RuntimeError("Knowledge search needs the ML extras for embeddings")
    if store.count_chunks() == 0:
        raise ValueError("No documents are indexed. Add documents in the Knowledge panel first.")
    config = store.get_config()
    query = shape_query(query)
    vector = embed_query(query, config.model, config.truncate_dim)
    chunks = store.search(vector, config.model, top_k=top_k or config.top_k,
                          min_score=config.min_score if min_score is None else min_score,
                          document_ids=None, query_text=query, mode=config.search_mode)
    results = [{"document": chunk.document_name, "document_id": chunk.document_id, "chunk": chunk.index,
                "text": chunk.text, "score": round(float(chunk.score), 4)} for chunk in chunks]
    context = "\n\n".join(f"[{number}] {item['document']}: {item['text']}"
                          for number, item in enumerate(results, 1))
    return results, context
