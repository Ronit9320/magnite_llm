"""Deterministic operations supported by saved order workflows."""

from __future__ import annotations

import re


_NUMBER_WORDS = (
    "", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
    "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen",
    "nineteen", "twenty",
)


def order_clause(message: str, index: int) -> str:
    clauses = re.split(r"(?<=[.?!])\s+", message.strip())
    if not 1 <= index <= len(clauses) or not clauses[index - 1]:
        raise ValueError(f"Clause index {index} is outside the order message")
    return clauses[index - 1]


def price_fields(fields: dict) -> dict:
    expected = {"item", "quantity", "unit_price", "city"}
    if fields.keys() != expected:
        raise ValueError("Order fields must contain item, quantity, unit_price, and city")
    quantity, unit_price = fields["quantity"], fields["unit_price"]
    if any(type(value) is not int or value <= 0 for value in (quantity, unit_price)):
        raise ValueError("Quantity and unit price must be positive integers")
    item, city = fields["item"], fields["city"]
    if not all(isinstance(value, str) and value.strip() for value in (item, city)):
        raise ValueError("Item and city must be nonempty text")
    item, city = " ".join(item.lower().split()), " ".join(city.split())
    subtotal = quantity * unit_price
    shipping = 5 if subtotal < 50 else 0
    return {**fields, "item": item, "city": city, "subtotal": subtotal,
            "shipping": shipping, "total": subtotal + shipping}


def price_order(clause: str, fields: dict) -> dict:
    order = price_fields(fields)
    quantity, unit_price = order["quantity"], order["unit_price"]
    item, city = order["item"], order["city"]
    text = " ".join(clause.lower().split())
    quantities = [str(quantity)]
    if quantity < len(_NUMBER_WORDS):
        quantities.append(_NUMBER_WORDS[quantity])
    amount = "|".join(quantities)
    required = {
        "quantity and item": rf"\b(?:{amount})\s+{re.escape(item)}\b",
        "city": rf"(?<!\w){re.escape(city.lower())}(?!\w)",
        "unit price": rf"\b{unit_price}\s+dollars?\b",
    }
    for field, pattern in required.items():
        if not re.search(pattern, text):
            raise ValueError(f"The selected clause does not support the extracted {field}")
    return order
