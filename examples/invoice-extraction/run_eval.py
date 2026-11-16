#!/usr/bin/env python3
"""Headless before/after runner for held-out.jsonl (stdlib only).

  python3 run_eval.py --url http://127.0.0.1:PORT --model <hf-id-or-folder> --out before.jsonl
  python3 run_eval.py --url http://127.0.0.1:PORT --adapter <adapter-id>    --out after.jsonl

It talks to a running Magnite backend (`magnite-backend --port 0 --home <dir>`; the port is
printed on start-up). It loads the model (or activates the adapter, which loads the adapter's
own base model), confirms through /inference/status what is loaded, then sends every held-out
`prompt` verbatim as a fresh one-message chat: temperature 0, 256 response tokens, RAG off.
Replies are written as {"id", "reply"} lines, the shape score.py reads, and a
<out>.meta.json records the settings and the backend status seen. Re-running with the same
--out resumes: ids already in the file are skipped.

  --ids inv-001,inv-002   only these emails (smoke test)     --limit 3   only the first 3
  --token T               Bearer credential, if the backend was started by the desktop app
"""
import argparse
import json
import sys
import time
import urllib.error
import urllib.request


def call(base, path, body=None, token="", timeout=900, method=None):
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = "Bearer " + token
    data = None if body is None else json.dumps(body).encode("utf-8")
    req = urllib.request.Request(base + path, data=data, headers=headers,
                                 method=method or ("GET" if body is None else "POST"))
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")
        sys.exit(f"{path}: HTTP {exc.code}: {detail}")
    except urllib.error.URLError as exc:
        sys.exit(f"{path}: cannot reach the backend ({exc.reason})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", required=True, help="backend base URL, e.g. http://127.0.0.1:8123")
    ap.add_argument("--model", help="model to load: Hub id or a local model folder (not needed with --adapter)")
    ap.add_argument("--adapter", help="adapter id to activate (it loads its own base model)")
    ap.add_argument("--held", default="held-out.jsonl")
    ap.add_argument("--out", required=True)
    ap.add_argument("--max-tokens", type=int, default=256)
    ap.add_argument("--temperature", type=float, default=0.0)
    ap.add_argument("--ids", default="", help="comma-separated ids to run")
    ap.add_argument("--limit", type=int, default=0, help="run only the first N emails")
    ap.add_argument("--token", default="")
    a = ap.parse_args()
    if bool(a.model) == bool(a.adapter):
        sys.exit("give exactly one of --model (base run) or --adapter (adapter run)")
    base = a.url.rstrip("/")

    call(base, "/health", token=a.token)
    if a.adapter:
        status = call(base, "/inference/adapter", {"adapter_id": a.adapter}, a.token)
        if status.get("active_adapter_id") != a.adapter:
            sys.exit(f"adapter {a.adapter} is not active after activation: {status}")
    else:
        status = call(base, "/inference/load", {"model": a.model, "engine": None}, a.token)
        if not status.get("loaded_model") or status.get("active_adapter_id"):
            sys.exit(f"expected the plain base model to be loaded with no adapter: {status}")
    print("loaded:", json.dumps(status), file=sys.stderr)

    held = [json.loads(line) for line in open(a.held, encoding="utf-8") if line.strip()]
    if a.ids:
        want = set(a.ids.split(","))
        held = [h for h in held if h["id"] in want]
    if a.limit:
        held = held[:a.limit]
    done = set()
    try:
        done = {json.loads(line)["id"] for line in open(a.out, encoding="utf-8") if line.strip()}
    except FileNotFoundError:
        pass

    began = time.monotonic()
    sent = 0
    with open(a.out, "a", encoding="utf-8") as out:
        for h in held:
            if h["id"] in done:
                continue
            body = {"messages": [{"role": "user", "content": h["prompt"]}],   # a fresh chat each time
                    "max_tokens": a.max_tokens, "temperature": a.temperature,
                    "stream": False, "rag": {"enabled": False}}
            reply = call(base, "/inference/chat", body, a.token)["choices"][0]["message"]["content"]
            out.write(json.dumps({"id": h["id"], "reply": reply}, ensure_ascii=False) + "\n")
            out.flush()
            sent += 1
            if sent % 10 == 0:
                print(f"{sent}/{len(held) - len(done)}  {time.monotonic() - began:.0f}s", file=sys.stderr)
    after = call(base, "/inference/status", token=a.token)
    meta = {"settings": {"temperature": a.temperature, "max_tokens": a.max_tokens, "rag": False,
                         "fresh_chat_per_email": True, "model": a.model, "adapter": a.adapter},
            "status_before": status, "status_after": after, "emails_run": sent, "emails_skipped_resumed": len(done),
            "seconds": round(time.monotonic() - began, 1)}
    with open(a.out + ".meta.json", "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=2)
    print(f"wrote {sent} replies to {a.out} in {meta['seconds']}s", file=sys.stderr)


if __name__ == "__main__":
    main()
