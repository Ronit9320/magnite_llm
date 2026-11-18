"""Workflow fixtures for testing saved order transforms and model training."""

from random import Random

from backend.blueprints.order import _NUMBER_WORDS, price_order


SELECTOR_PROMPT = (
    "You are the routing agent. The message contains two or three order sentences. "
    "Output only the number of the sentence that is the active order being requested "
    "now: 1, 2 or 3. Output nothing else."
)
EXTRACTOR_PROMPT = (
    'You extract the active order. Reply with only a JSON object with keys '
    '"item" (string), "quantity" (integer), "unit_price" (integer) and "city" (string).'
)

_TEXT = {"type": "string", "minLength": 1}
_POSITIVE = {"type": "integer", "minimum": 1}


def _object(properties: dict) -> dict:
    return {"type": "object", "properties": properties, "required": list(properties),
            "additionalProperties": False}


_FIELDS = _object({"item": _TEXT, "quantity": _POSITIVE, "unit_price": _POSITIVE, "city": _TEXT})
_ORDER = _object({**_FIELDS["properties"], "subtotal": _POSITIVE,
                  "shipping": {"type": "integer", "minimum": 0}, "total": _POSITIVE})


def _node(node_id, name, kind, operation, inputs, outputs, **settings) -> dict:
    return {"id": node_id, "name": name, "kind": kind, "operation": operation,
            "inputs": inputs, "outputs": outputs, **settings}


def _edge(src, output, dst, input_port) -> dict:
    return {"source": {"node": src, "port": output}, "target": {"node": dst, "port": input_port}}


def order_blueprint(model_id: str, selector_adapter: str | None = None,
                    extractor_adapter: str | None = None) -> dict:
    return {
        "version": 1, "name": "Order intake",
        "nodes": [
            _node("message", "Read message", "input", "input", {}, {"value": _TEXT}),
            _node("select", "Select active clause", "model", "generate", {"message": _TEXT}, {"index": _POSITIVE},
                  binding={"model_id": model_id, "adapter_id": selector_adapter},
                  system_prompt=SELECTOR_PROMPT, max_tokens=4),
            _node("lookup", "Look up clause", "transform", "order-clause",
                  {"message": _TEXT, "index": _POSITIVE}, {"value": _TEXT}),
            _node("extract", "Extract order fields", "model", "generate", {"clause": _TEXT}, {"fields": _FIELDS},
                  binding={"model_id": model_id, "adapter_id": extractor_adapter},
                  system_prompt=EXTRACTOR_PROMPT, max_tokens=96),
            _node("price", "Validate and price order", "transform", "order-price",
                  {"clause": _TEXT, "fields": _FIELDS}, {"value": _ORDER}),
            _node("order", "Return order", "output", "output", {"value": _ORDER}, {"value": _ORDER}),
        ],
        "edges": [
            _edge("message", "value", "select", "message"),
            _edge("message", "value", "lookup", "message"),
            _edge("select", "index", "lookup", "index"),
            _edge("lookup", "value", "extract", "clause"),
            _edge("lookup", "value", "price", "clause"),
            _edge("extract", "fields", "price", "fields"),
            _edge("price", "value", "order", "value"),
        ],
    }


def _example(clauses: list[str], index: int, item: str, quantity: int, unit_price: int, city: str) -> tuple:
    fields = {"item": item, "quantity": quantity, "unit_price": unit_price, "city": city}
    return " ".join(clauses), index, clauses[index - 1], fields


def order_samples() -> list[dict]:
    data = [
        _example(["Cancel the 4 blue umbrellas order for Berlin.",
                  "Please send six red apples at 3 dollars each to Mumbai by Monday."],
                 2, "red apples", 6, 3, "Mumbai"),
        _example(["Could you deliver 12 laptops to Austin for 7 dollars each by Friday?",
                  "We already shipped 2 ceramic mugs to Osaka last week.",
                  "The quote for 5 monitors in Paris was 9 dollars each."],
                 1, "laptops", 12, 7, "Austin"),
    ]
    return [
        {"request": {"inputs": {"message": message}, "fixtures": {
            "select": {"outputs": {"index": index}}, "extract": {"outputs": {"fields": fields}},
        }}, "expected": {"order": price_order(clause, fields)}}
        for message, index, clause, fields in data
    ]


def training_examples() -> dict[str, list[dict]]:
    rng = Random(41)
    selector, extractor = [], []
    stock = ("red apples", "blue umbrellas", "wooden chairs", "ceramic mugs", "laptops", "floor lamps",
             "copper wires", "running shoes", "garden hoses", "glass bottles")
    cities = ("Mumbai", "Paris", "Austin", "Berlin", "Tokyo", "Delhi", "Rome", "Vienna")
    active = (
        "Please send {q} {item} at {p} dollars each to {city} by Monday.",
        "Ship {q} {item} to {city} at {p} dollars each before Friday.",
        "Could you deliver {q} {item} to {city} for {p} dollars each by Saturday?",
        "We need {q} {item} at {p} dollars each delivered to {city} tomorrow.",
    )
    distractors = (
        "Cancel the {q} {item} order for {city}.",
        "The quote for {q} {item} in {city} was {p} dollars each.",
        "We already shipped {q} {item} to {city} last week.",
    )
    for number in range(120):
        item, wrong_item = rng.sample(stock, 2)
        city, wrong_city = rng.sample(cities, 2)
        quantity, price = rng.randint(2, 20), rng.randint(2, 30)
        qty = _NUMBER_WORDS[quantity] if number % 3 == 0 else quantity
        clause = rng.choice(active).format(q=qty, item=item, p=price, city=city)
        clauses = [style.format(q=rng.randint(1, 20), item=wrong_item,
                                city=wrong_city, p=rng.randint(2, 30))
                   for style in rng.sample(distractors, 1 + number % 2)]
        index = rng.randrange(len(clauses) + 1)
        clauses.insert(index, clause)
        _, _, _, fields = _example(clauses, index + 1, item, quantity, price, city)
        selector.append({"inputs": {"message": " ".join(clauses)}, "outputs": {"index": index + 1}})
        extractor.append({"inputs": {"clause": clause}, "outputs": {"fields": fields}})
    return {"select": selector, "extract": extractor}
