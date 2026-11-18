"""Opt-in real LoRA training and two-model Blueprint exercise in a temporary profile."""

from copy import deepcopy
import json
import os
from pathlib import Path
from tempfile import TemporaryDirectory
import time

from fastapi.testclient import TestClient
from huggingface_hub import snapshot_download

from backend.local_access import LocalAccessMiddleware
from backend.models.schemas import ModelRecord
from backend.routes.models import _get_registry
from backend.server import app


EXAMPLES = Path(__file__).resolve().parents[2] / "docs/development/blueprints/examples"
MODEL_REF = "Qwen/Qwen3-0.6B"
MODEL_ID = "huggingface:Qwen--Qwen3-0.6B"
MODEL_REVISION = "c1899de289a04d12100db370d81485cdf75e47ca"


def read_example(name):
    return json.loads((EXAMPLES / f"support-triage{name}.json").read_text(encoding="utf-8"))


def checked(response, status=200):
    if response.status_code != status:
        raise AssertionError(f"{response.request.url}: {response.status_code}: {response.text}")
    return response.json()


def run_training(client, saved, rows):
    examples = {
        "extract": [
            {"inputs": {"ticket": row["ticket"]}, "outputs": {"raw_json": json.dumps(row["record"])}}
            for row in rows
        ],
        "route": [
            {"inputs": {"record": row["record"]}, "outputs": {"queue": row["queue"], "priority": row["priority"]}}
            for row in rows
        ],
    }
    adapters = {}
    for node, samples in examples.items():
        job = checked(client.post(f"/blueprints/{saved['id']}/nodes/{node}/training", json={
            "revision": saved["revision"], "examples": samples,
            "settings": {"max_steps": 12, "max_length": 256, "learning_rate": 0.0006,
                         "rank": 4, "alpha": 8, "target_modules": "all-linear", "dropout": 0},
        }), 202)
        deadline = time.monotonic() + 600
        while job["status"] in ("queued", "running") and time.monotonic() < deadline:
            time.sleep(1)
            job = checked(client.get(f"/training/status/{job['id']}"))
        if job["status"] != "done" or not job["adapter_id"]:
            raise AssertionError(f"{node} training did not complete: {job}")
        adapters[node] = job["adapter_id"]
        print(f"Trained {node} LoRA in {job['current_step']} steps", flush=True)
    return adapters


def run_cases(client, graph, cases, adapters, shared):
    graph = deepcopy(graph)
    label = "shared extraction LoRA" if shared else "separate LoRAs"
    graph["name"] += f" ({label})"
    extract_id = adapters["extract"]
    route_id = extract_id if shared else adapters["route"]
    for node in graph["nodes"]:
        if node["id"] == "extract-lora":
            node["adapter_id"] = extract_id
        elif node["id"] == "route-lora":
            node["adapter_id"] = route_id
    if shared:
        graph["nodes"] = [node for node in graph["nodes"] if node["id"] != "route-lora"]
        for edge in graph["edges"]:
            if edge["source"]["node"] == "route-lora":
                edge["source"]["node"] = "extract-lora"

    saved = checked(client.post("/blueprints", json=graph), 201)
    exact = completed = extraction_matches = 0
    for case in cases:
        started = checked(client.post(f"/blueprints/{saved['id']}/runs", json={
            "mode": "local", "inputs": {"ticket": case["ticket"]},
        }), 202)
        run_path = f"/blueprints/runs/{started['id']}"
        with client.stream("GET", f"{run_path}/events") as events:
            events.raise_for_status()
            for _ in events.iter_lines():
                pass
        run = checked(client.get(run_path))
        bindings = run["snapshot"]["bindings"]["adapters"]
        assert (bindings["extract"]["id"], bindings["route"]["id"]) == (extract_id, route_id)
        steps = {step["node_id"]: step for step in run["steps"]}
        parsed = steps["parse"].get("outputs", {}).get("value")
        route_input = steps["route"].get("inputs")
        if route_input is not None:
            assert route_input == {"record": parsed}
        result = run["results"].get("result")
        completed += run["status"] == "passed"
        extraction_matches += parsed == case["expected"]["ticket"]
        exact += result == case["expected"]
        print(f"{label} / {case['id']}: {json.dumps(run.get('error') or result)}", flush=True)
    print(f"{label}: completed {completed}/{len(cases)}, "
          f"extraction exact {extraction_matches}/{len(cases)}, end-to-end exact {exact}/{len(cases)}",
          flush=True)
    return exact == len(cases)


def main():
    checkpoint = snapshot_download(MODEL_REF, revision=MODEL_REVISION, local_files_only=True)
    graph = read_example("")
    cases = read_example("-cases")
    rows = read_example("-training")
    token = "blueprint-live-support-test"
    with TemporaryDirectory(prefix="magnite-blueprint-support-") as home:
        os.environ["MAGNITE_HOME"] = home
        with TestClient(LocalAccessMiddleware(app, token), base_url="http://127.0.0.1",
                        headers={"Authorization": f"Bearer {token}"}) as client:
            _get_registry()._store.upsert(ModelRecord(id=MODEL_ID, name=MODEL_REF, ref=MODEL_REF,
                                                      source="huggingface", path=checkpoint))
            draft = checked(client.post("/blueprints", json=graph), 201)
            adapters = run_training(client, draft, rows)
            distinct = run_cases(client, graph, cases, adapters, shared=False)
            shared = run_cases(client, graph, cases, adapters, shared=True)
    return 0 if distinct and shared else 1


if __name__ == "__main__":
    raise SystemExit(main())
