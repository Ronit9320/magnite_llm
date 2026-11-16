#!/usr/bin/env python3
"""Score model replies against held-out.jsonl (stdlib only).

  python3 score.py replies.jsonl [--held held-out.jsonl] [--json out.json]

replies.jsonl: one JSON object per line: {"id": "inv-001", "reply": "<raw model text>"}
Replies for the same id may be missing (counted as an empty reply).

Metrics (all on the 300 held-out emails unless the replies cover fewer):
  strict_json     reply, trimmed, parses as ONE JSON object and nothing else
  lenient_json    a JSON object can be pulled out of the reply (first '{' .. last '}')
  field accuracy  per key, on the lenient parse (so a chatty base model still gets credit)
  exact_record    strict_json AND all 7 fields correct  <- the headline number
A leading <think>...</think> block (Qwen3) is removed before scoring and counted
separately as `had_think`, so thinking is reported, not silently forgiven.
"""
import argparse
import json
import math
import re
import sys

KEYS = ["invoice_number", "vendor", "amount", "currency", "due_date", "po_number", "payment_terms"]
THINK = re.compile(r"<think>.*?</think>", re.S)


def parse(reply):
    had_think = "<think>" in reply
    text = THINK.sub("", reply).strip()
    strict = lenient = None
    try:
        obj = json.loads(text)
        if isinstance(obj, dict):
            strict = lenient = obj
    except (json.JSONDecodeError, ValueError):
        pass
    if lenient is None and "{" in text and "}" in text:
        try:
            obj = json.loads(text[text.index("{"): text.rindex("}") + 1])
            if isinstance(obj, dict):
                lenient = obj
        except (json.JSONDecodeError, ValueError):
            pass
    return strict, lenient, had_think


def same(key, got, want):
    if key == "amount":
        return isinstance(got, (int, float)) and not isinstance(got, bool) and math.isclose(got, want, abs_tol=0.005)
    return got == want


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("replies")
    ap.add_argument("--held", default="held-out.jsonl")
    ap.add_argument("--json", dest="out")
    a = ap.parse_args()

    held = {}
    for line in open(a.held, encoding="utf-8"):
        r = json.loads(line)
        held[r["id"]] = r
    replies = {}
    for line in open(a.replies, encoding="utf-8"):
        if line.strip():
            r = json.loads(line)
            replies[r["id"]] = r.get("reply", "")
    unknown = set(replies) - set(held)
    if unknown:
        sys.exit(f"unknown ids in replies: {sorted(unknown)[:5]}")
    ids = sorted(replies)

    def blank():
        return {"n": 0, "strict_json": 0, "lenient_json": 0, "exact_record": 0, "had_think": 0,
                "fields": {k: 0 for k in KEYS}}

    groups = {"all": blank(), "normal": blank(), "hard": blank()}
    per = []
    for i in ids:
        h = held[i]
        want = json.loads(h["expected"])
        strict, lenient, think = parse(replies[i])
        ok = {k: lenient is not None and k in lenient and same(k, lenient[k], want[k]) for k in KEYS}
        exact = strict is not None and set(strict) == set(KEYS) and all(ok.values())
        for g in ("all", "hard" if h["hard"] else "normal"):
            s = groups[g]
            s["n"] += 1
            s["strict_json"] += strict is not None
            s["lenient_json"] += lenient is not None
            s["exact_record"] += exact
            s["had_think"] += think
            for k in KEYS:
                s["fields"][k] += ok[k]
        per.append({"id": i, "hard": h["hard"], "tags": h["tags"], "strict_json": strict is not None,
                    "exact_record": exact, "wrong_fields": [k for k in KEYS if not ok[k]]})

    def pct(x, n):
        return f"{100 * x / n:5.1f}%" if n else "  n/a"

    for g, s in groups.items():
        n = s["n"]
        if not n:
            continue
        print(f"\n== {g} (n={n}) ==")
        print(f"strict JSON  {pct(s['strict_json'], n)}   lenient JSON {pct(s['lenient_json'], n)}   "
              f"had <think> {pct(s['had_think'], n)}")
        print(f"EXACT RECORD {pct(s['exact_record'], n)}  ({s['exact_record']}/{n})")
        print("fields: " + "  ".join(f"{k}={pct(s['fields'][k], n).strip()}" for k in KEYS))
    if a.out:
        json.dump({"summary": groups, "per_example": per}, open(a.out, "w"), indent=2)


if __name__ == "__main__":
    main()
