"""Compare local order pipelines on the same labeled JSONL cases."""

import argparse
from collections import Counter
import json
from pathlib import Path
from statistics import median
import subprocess
import sys
from tempfile import TemporaryDirectory
from threading import Event, Thread
from time import monotonic

import psutil

from backend.blueprints.order import EXTRACTOR_PROMPT, SELECTOR_PROMPT, order_clause, price_fields, price_order
from backend.inference.engines.torch import TorchPeftEngine

FIELDS = {
    "quantity": ("requested quantity as digits", "quantity", 8),
    "item": ("item name, including its adjective if present", "item", 16),
    "city": ("destination city", "city", 12),
    "unit_price": ("unit price in dollars as digits", "price", 8),
}
KEYS = (*FIELDS, "subtotal", "shipping", "total")


class StageError(ValueError):
    pass


def parse_field(text, key):
    value = text.splitlines()[0].strip(" \"'`").rstrip(".?!") if text else ""
    if key in ("quantity", "unit_price"):
        return int(value.replace(",", ""))
    if key == "city" and value.lower().startswith(("to ", "in ", "for ")):
        value = value.split(maxsplit=1)[1]
    return value


def evaluate(engine, adapters, method, case, fields_adapter, mistakes):
    def step(stage, prompt, text, adapter, cap, decode):
        try:
            out = engine.generate_blueprint(
                [{"role": "system", "content": prompt}, {"role": "user", "content": text}],
                max_tokens=cap, adapter=str(adapters / adapter), reasoning=False, stop=Event(),
            )
            return decode(out)
        except (ValueError, RuntimeError) as exc:
            raise StageError(stage) from exc

    def price(clause, fields):
        try:
            return price_order(clause, fields)
        except (ValueError, TypeError) as exc:
            raise StageError("price") from exc

    message = case["message"]
    clause = message
    if method != "single":
        index = step("selector", SELECTOR_PROMPT, message, "selector_index", 4, json.loads)
        try:
            if type(index) is not int:
                raise ValueError("Selector did not return an integer")
            clause = order_clause(message, index)
        except ValueError as exc:
            raise StageError("lookup") from exc
        mistakes["selector"] += clause != case["active_clause"]

    if method == "five":
        fields = {}
        for key, (description, adapter, cap) in FIELDS.items():
            agent = "price" if key == "unit_price" else key
            prompt = (f"You are the {agent} agent. Read the order and output only the {description}. "
                      "Output nothing else.")
            decode = lambda text, key=key: parse_field(text, key)
            fields[key] = step(key, prompt, clause, adapter, cap, decode)
            mistakes[key] += fields[key] != case[key]
    else:
        fields = step("extractor", EXTRACTOR_PROMPT, clause, fields_adapter, 96, json.loads)
        if not isinstance(fields, dict) or fields.keys() != FIELDS.keys():
            raise StageError("extractor")
        mistakes["extractor"] += any(fields[key] != case[key] for key in FIELDS)
        if method == "single":
            # The recorded single adapter prices its extracted fields directly.
            try:
                return price_fields(fields)
            except ValueError as exc:
                raise StageError("price") from exc
    return price(clause, fields)


def run_method(model, adapters, method, cases, fields_adapter):
    import torch

    engine = TorchPeftEngine(str(model), offline_only=True)
    process = psutil.Process()
    initial_rss = process.memory_info().rss
    rss = [initial_rss]
    stop = Event()

    def sample_rss():
        while not stop.wait(0.02):
            rss.append(process.memory_info().rss)

    monitor = Thread(target=sample_rss)
    monitor.start()
    if torch.cuda.is_available():
        torch.cuda.reset_peak_memory_stats()

    started = monotonic()
    failed, mistakes, counts, durations = Counter(), Counter(), Counter(), []
    try:
        engine.load()
        load_seconds = monotonic() - started
        for case in cases:
            start = monotonic()
            try:
                prediction = evaluate(engine, adapters, method, case, fields_adapter, mistakes)
                counts["exact"] += prediction == {key: case[key] for key in KEYS}
            except StageError as exc:
                failed[str(exc)] += 1
            durations.append(monotonic() - start)
        return {
            "exact": counts["exact"], "cases": len(cases), "invalid": sum(failed.values()),
            "stage_failures": dict(failed), "stage_mismatches": dict(mistakes),
            "load_s": round(load_seconds, 3), "cold_first_order_s": round(load_seconds + durations[0], 3),
            "warm_median_s": round(median(durations[1:]), 3) if len(durations) > 1 else None,
            "base_rss_mib": round(initial_rss / 1024**2),
            "peak_rss_mib": round(max(rss) / 1024**2),
            "gpu_peak_allocated_mib": round(torch.cuda.max_memory_allocated() / 1024**2)
            if torch.cuda.is_available() else None,
        }
    finally:
        stop.set()
        monitor.join()
        engine.unload()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--adapters", type=Path, required=True)
    parser.add_argument("--cases", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--fields-adapter", default="monolith_fields_hi24")
    parser.add_argument("--method", choices=("two", "five", "single"))
    args = parser.parse_args()
    results = {"mode": "local", "model": args.model.name, "fields_adapter": args.fields_adapter, "methods": {}}
    if args.method:
        cases = [json.loads(line) for line in args.cases.read_text(encoding="utf-8").splitlines() if line.strip()]
        if not cases:
            parser.error("the cases file is empty")
        result = run_method(args.model, args.adapters, args.method, cases, args.fields_adapter)
        results["methods"][args.method] = result
        print(args.method, json.dumps(result), flush=True)
        args.output.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
        return

    # Give each variant a fresh process so the first model imports and peak
    # memory have the same starting conditions.
    with TemporaryDirectory() as folder:
        intermediate = Path(folder) / "method.json"
        for method in ("two", "five", "single"):
            subprocess.run([sys.executable, __file__, *sys.argv[1:], "--output", str(intermediate),
                            "--method", method], check=True)
            result = json.loads(intermediate.read_text(encoding="utf-8"))
            results["methods"].update(result["methods"])
            args.output.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
