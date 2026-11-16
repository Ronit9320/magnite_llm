#!/usr/bin/env python3
"""Seeded generator for the invoice-extraction example (synthetic data only).

Usage:  python3 generate.py            # rewrites the JSONL files next to this script
Stdlib only. Same seed -> byte-identical output. Every company name, invoice
number and amount is invented; no real emails or customers are used.

Task: read a short, messy invoice email and reply with ONE JSON object:
  invoice_number, vendor, amount, currency, due_date, po_number, payment_terms
"""
from __future__ import annotations

import datetime as dt
import json
import os
import random

SEED = 20261116
N_TRAIN = 3000
N_HELD = 300
HERE = os.path.dirname(os.path.abspath(__file__))

INSTRUCTION = (
    "Extract the invoice details from the email below. Reply with only a JSON "
    "object with keys invoice_number, vendor, amount, currency, due_date, "
    "po_number, payment_terms. Use null for anything the email does not state. "
    "Dates are YYYY-MM-DD; currency is an ISO code; payment_terms is one of "
    "net15, net30, net60, due_on_receipt. The amount is the sum currently owed."
)
KEYS = ["invoice_number", "vendor", "amount", "currency", "due_date", "po_number", "payment_terms"]

# --- invented vendors (train and held-out pools are disjoint) ---------------
V_A = ["Harlow", "Brightwater", "Kestrel", "Marlowe", "Ashgrove", "Tallis", "Quillon", "Fernwick",
       "Oakhurst", "Lindqvist", "Ravenscar", "Dunmore", "Peregrine", "Thistledown", "Corvane",
       "Eastvale", "Wrenfield", "Halcombe", "Norrington", "Silverbeck", "Ironwell", "Pellham"]
V_B = ["Finch", "Logistics", "Print Works", "Analytics", "Catering", "Supplies", "Design Studio",
       "Electrical", "Cleaning Services", "Freight", "Consulting", "Joinery", "Labs", "Textiles",
       "Security", "Media", "Hydraulics", "Courier Co"]
V_HELD_A = ["Zephyrine", "Gallowglass", "Moorcroft", "Tamsin", "Vantongeren", "Boskovic", "Saltmarsh", "Underhill"]
V_HELD_B = ["Beverages", "Orchards", "Surveyors", "Pipeworks", "Typesetters", "Anchorage Ltd"]
SUFFIX = ["", "", "", " Ltd", " LLC", " GmbH", " Inc.", " & Co."]


def vendor(rng, held=False):
    a = rng.choice(V_HELD_A if held else V_A)
    b = rng.choice(V_HELD_B if held else V_B)
    if rng.random() < 0.3:
        a2 = rng.choice(V_HELD_A if held else V_A)
        if a2 != a:
            return f"{a} & {a2}"
    if b.endswith("Ltd"):
        return f"{a} {b}"
    return f"{a} {b}{rng.choice(SUFFIX)}"


# --- invoice numbers --------------------------------------------------------
def invoice_no(rng, held=False):
    n = rng.randint(100, 99999)
    forms = [
        lambda: f"INV-{n}", lambda: f"INV/2026/{n % 9000 + 100}",
        lambda: f"{rng.choice('ABCDEFGHJKLMNPRSTW')}{rng.choice('ABCDEFGHJKLMNPRSTW')}-{n:05d}",
        lambda: f"#{rng.randint(100000, 999999)}", lambda: f"{rng.randint(2025, 2026)}-{n % 900 + 100:04d}",
        lambda: f"BL{n:06d}",
    ]
    if held and rng.random() < 0.5:           # a format never seen in training
        return f"INV.{rng.randint(100, 999)}-{rng.choice('QXZ')}{rng.choice('QXZ')}"
    return rng.choice(forms)()


# --- money ------------------------------------------------------------------
CURRENCIES = {  # code -> list of (prefix, suffix) renderings
    "USD": [("$", ""), ("US$", ""), ("", " USD")],
    "EUR": [("€", ""), ("", " EUR"), ("", " €")],
    "GBP": [("£", ""), ("", " GBP")],
    "CAD": [("C$", ""), ("", " CAD")],
    "AUD": [("A$", ""), ("", " AUD")],
}


def fmt_amount(value: float, style: str) -> str:
    whole, cents = f"{value:.2f}".split(".")
    groups = f"{int(whole):,}"
    if style == "us":
        return f"{groups}.{cents}"
    if style == "eu":
        return groups.replace(",", ".") + "," + cents
    if style == "plain":
        return f"{whole}.{cents}"
    if style == "space":
        return groups.replace(",", " ") + "," + cents
    if style == "swiss":                      # held-out only
        return groups.replace(",", "'") + "." + cents
    if style == "round":                      # no cents shown
        return groups if cents == "00" else f"{groups}.{cents}"
    raise ValueError(style)


def money(rng, value, code, held=False):
    styles = ["us", "us", "eu", "plain", "space", "round"] + (["swiss"] if held else [])
    style = rng.choice(styles)
    if style == "round" and f"{value:.2f}".endswith("00") is False:
        style = "us"
    if style in ("eu", "space") and code != "EUR":
        style = "us"                          # keep European number style for euro amounts only
    pre, suf = rng.choice(CURRENCIES[code])
    return f"{pre}{fmt_amount(value, style)}{suf}"


# --- dates ------------------------------------------------------------------
MONTHS = ["January", "February", "March", "April", "May", "June", "July", "August",
          "September", "October", "November", "December"]


def ordinal(n):
    return f"{n}{'th' if 11 <= n % 100 <= 13 else {1: 'st', 2: 'nd', 3: 'rd'}.get(n % 10, 'th')}"


def fmt_date(d: dt.date, style: str) -> str:
    m, mon = MONTHS[d.month - 1], MONTHS[d.month - 1][:3]
    return {
        "dmy_long": f"{d.day} {m} {d.year}", "mdy_long": f"{m} {d.day}, {d.year}",
        "iso": d.isoformat(), "dmy_short": f"{d.day:02d} {mon} {d.year}",
        "mdy_ord": f"{m} {ordinal(d.day)}, {d.year}", "mon_day": f"{mon} {d.day}, {d.year}",
        "weekday": f"{d.strftime('%a')} {d.day} {m} {d.year}",      # held-out only
        "dash2": f"{d.day}-{mon}-{d.strftime('%y')}",              # held-out only
    }[style]


TRAIN_DATES = ["dmy_long", "mdy_long", "iso", "dmy_short", "mdy_ord", "mon_day"]
HELD_DATES = TRAIN_DATES + ["weekday", "dash2", "weekday", "dash2"]


def rand_date(rng, lo=dt.date(2026, 1, 5), hi=dt.date(2027, 3, 28)):
    return lo + dt.timedelta(days=rng.randint(0, (hi - lo).days))


# --- payment terms / PO -----------------------------------------------------
TERMS = {
    "net15": ["net 15", "payable within 15 days", "terms: 15 days"],
    "net30": ["net 30", "payable within 30 days", "30-day terms", "Net30"],
    "net60": ["net 60", "payable within 60 days", "60 days from invoice date"],
    "due_on_receipt": ["due on receipt", "payable immediately upon receipt", "due upon receipt"],
}
PO_FORMS = [lambda n: f"PO {n}", lambda n: f"PO-{n}", lambda n: f"purchase order {n}",
            lambda n: f"PO#{n}", lambda n: f"P.O. {n}"]

# --- people / noise ---------------------------------------------------------
FIRST = ["Sam", "Priya", "Jonas", "Maren", "Tobias", "Ines", "Callum", "Noor", "Elif", "Rafael", "Hana", "Dev"]
LAST = ["Okafor", "Lindgren", "Varga", "Moreau", "Haldane", "Brandt", "Castellan", "Iyer"]
GREET = ["Hi,", "Hello,", "Hi team,", "Dear accounts payable,", "Good morning,", "Hey,", "To whom it may concern,", ""]
SIGN = ["Thanks", "Kind regards", "Best", "Regards", "Cheers", "Many thanks", "Thank you"]


def person(rng):
    return f"{rng.choice(FIRST)} {rng.choice(LAST)}"


def phone(rng):
    return f"+{rng.choice([1, 44, 49, 61])} {rng.randint(100, 999)} {rng.randint(100, 999)} {rng.randint(1000, 9999)}"


def signature(rng, v):
    parts = [person(rng), f"Accounts, {v}"]
    r = rng.random()
    if r < 0.5:
        parts.append(f"Tel {phone(rng)}")
    if r < 0.3:
        parts.append(f"{rng.randint(2, 480)} {rng.choice(['Mill', 'Station', 'Orchard', 'Canal'])} {rng.choice(['Road', 'Lane', 'Street'])}")
    if rng.random() < 0.2:
        parts.append(f"Ticket ref {rng.randint(10000, 99999)}")
    return "\n".join(parts)


# --- body templates. Each gets a dict of rendered pieces and returns sentences.
# {inv} {ven} {amt} {due} are filled by the caller; due may be None.
def due_clause(p, lead):
    return f" {lead} {p['due']}" if p["due"] else ""


BODY_TRAIN = [
    lambda p: f"Just a quick nudge that invoice {p['inv']} from {p['ven']} for {p['amt']} is due on {p['due']}." if p["due"] else f"Just a quick nudge that invoice {p['inv']} from {p['ven']} for {p['amt']} is still open.",
    lambda p: f"Please find attached invoice {p['inv']} totalling {p['amt']}." + (f" Payment is due by {p['due']}." if p["due"] else ""),
    lambda p: f"This is a reminder from {p['ven']}: {p['amt']} is outstanding on invoice {p['inv']}." + (f" It falls due on {p['due']}." if p["due"] else ""),
    lambda p: f"We have issued invoice {p['inv']} for {p['amt']}." + (f" Kindly settle by {p['due']}." if p["due"] else ""),
    lambda p: f"Our records show {p['inv']} ({p['amt']}) has not been paid." + (f" The due date was {p['due']}." if p["due"] else ""),
    lambda p: f"Following up on {p['inv']}: the balance of {p['amt']} is payable to {p['ven']}" + (f" by {p['due']}." if p["due"] else "."),
    lambda p: f"Invoice {p['inv']} - {p['amt']}" + (f" - due {p['due']}." if p["due"] else "."),
    lambda p: f"{p['ven']} has billed us {p['amt']} on invoice {p['inv']}." + (f" They want it paid by {p['due']}." if p["due"] else " No due date was given."),
    lambda p: f"Could you confirm receipt of invoice {p['inv']} for {p['amt']}?" + (f" Payment deadline is {p['due']}." if p["due"] else ""),
    lambda p: f"Heads up: {p['amt']} to {p['ven']} (inv. {p['inv']})" + (f" has to go out by {p['due']}." if p["due"] else " is waiting for approval."),
    lambda p: f"Statement of account. Invoice: {p['inv']}. Amount: {p['amt']}." + (f" Due: {p['due']}." if p["due"] else ""),
    lambda p: f"We're writing about {p['inv']}. The total, {p['amt']}, remains unpaid" + (f" and was due {p['due']}." if p["due"] else "."),
]
BODY_HELD_ONLY = [   # phrasings never used in training
    lambda p: f"Re: billing. {p['ven']} would appreciate settlement of {p['inv']}, in the sum of {p['amt']}" + (f", no later than {p['due']}." if p["due"] else "."),
    lambda p: f"fyi - {p['inv']} / {p['amt']} / {p['ven']}" + (f" / pay before {p['due']}" if p["due"] else ""),
    lambda p: f"Per our conversation, the amount owing under {p['inv']} is {p['amt']}." + (f" Please remit on or before {p['due']}." if p["due"] else ""),
]


def terms_sentence(rng, key):
    t = rng.choice(TERMS[key])
    return rng.choice([f"Terms: {t}.", f"Our standard terms: {t}.", f"({t})", f"Payment terms - {t}."])


# --- hard-case wrappers (held-out tags; mild forms also appear in training) --
def prior_invoice_note(rng, code, held=False):
    old_no = invoice_no(rng, held)
    old_amt = money(rng, round(rng.uniform(80, 9000), 2), code, held)
    return rng.choice([
        f"Invoice {old_no} for {old_amt} was already settled last month, thank you.",
        f"(Ignore {old_no} - {old_amt} - that one has been paid.)",
        f"Note that {old_no}, {old_amt}, cleared on the 2nd.",
    ])


def forwarded_wrap(rng, body, v):
    sender = person(rng)
    return (f"{rng.choice(['FYI', 'Can you handle this?', 'Please action.', 'Passing this on.'])}\n\n"
            f"---------- Forwarded message ----------\nFrom: {sender} <billing@{v.split()[0].lower().strip('&,.')}.example>\n"
            f"Subject: Invoice\n\n{body}")


def make(rng, held=False, hard=None):
    """Return (email_text, label_dict, tags)."""
    code = rng.choice(["USD", "USD", "EUR", "GBP", "CAD", "AUD"])
    value = round(rng.choice([rng.uniform(40, 900), rng.uniform(900, 12000), rng.uniform(12000, 95000)]), 2)
    if rng.random() < 0.25:
        value = float(round(value))
    inv, ven = invoice_no(rng, held), vendor(rng, held)
    tags = []

    due_obj = rand_date(rng)
    no_due = rng.random() < (0.12 if not hard == "missing" else 1.0)
    due_str = None if no_due else fmt_date(due_obj, rng.choice(HELD_DATES if held else TRAIN_DATES))
    p = {"inv": inv, "ven": ven, "amt": money(rng, value, code, held), "due": due_str}

    bodies = BODY_TRAIN + (BODY_HELD_ONLY if held else [])
    if held and rng.random() < 0.45:
        bodies = BODY_HELD_ONLY
    body = rng.choice(bodies)(p)

    po = terms = None
    if rng.random() < 0.5:
        po_no = str(rng.randint(10000, 99999))
        po = po_no
        body += " " + rng.choice([f"Your reference: {PO_FORMS[rng.randrange(5)](po_no)}.", f"Re {PO_FORMS[rng.randrange(5)](po_no)}."])
    if rng.random() < 0.55:
        terms = rng.choice(list(TERMS))
        body += " " + terms_sentence(rng, terms)

    if hard == "prior" or (not held and rng.random() < 0.12):
        body += " " + prior_invoice_note(rng, code, held)
        tags.append("prior-invoice")
    if hard == "missing":
        tags.append("missing-due-date")

    greet = rng.choice(GREET)
    close = f"{rng.choice(SIGN)},\n{signature(rng, ven)}" if rng.random() < 0.7 else ""
    style = rng.random()
    if style < 0.12:
        text = body                                      # terse one-liner
    else:
        text = "\n\n".join(x for x in (greet, body, close) if x)
    if hard == "forwarded" or (not held and rng.random() < 0.06):
        text = forwarded_wrap(rng, text, ven)
        tags.append("forwarded")
    if hard == "noisy":
        text += f"\n\nPS call us on {phone(rng)} or ref ticket {rng.randint(10000, 99999)} / order {rng.randint(1000, 9999)}."
        tags.append("noisy")

    label = {"invoice_number": inv, "vendor": ven, "amount": value, "currency": code,
             "due_date": None if no_due else due_obj.isoformat(), "po_number": po, "payment_terms": terms}
    return text, label, tags


def dump_label(lab) -> str:
    out = {}
    for k in KEYS:
        v = lab[k]
        out[k] = int(v) if k == "amount" and float(v).is_integer() else v
    return json.dumps(out, ensure_ascii=False, separators=(",", ":"))


def main():
    rng = random.Random(SEED)
    seen = set()

    def unique(held, hard=None):
        while True:
            text, lab, tags = make(rng, held, hard)
            if text not in seen:
                seen.add(text)
                return text, lab, tags

    train = []
    for _ in range(N_TRAIN):
        text, lab, _t = unique(False)
        train.append({"instruction": INSTRUCTION, "input": text, "output": dump_label(lab)})

    held = []
    hard_plan = ["prior"] * 14 + ["missing"] * 12 + ["forwarded"] * 10 + ["noisy"] * 9   # 45 = 15%
    plan = hard_plan + [None] * (N_HELD - len(hard_plan))
    rng.shuffle(plan)
    for i, hard in enumerate(plan, 1):
        text, lab, tags = unique(True, hard)
        held.append({"id": f"inv-{i:03d}", "prompt": f"{INSTRUCTION}\n{text}", "expected": dump_label(lab),
                     "tags": tags, "hard": hard is not None, "sample": i <= 30})

    def write(name, rows):
        with open(os.path.join(HERE, name), "w", encoding="utf-8") as f:
            for r in rows:
                f.write(json.dumps(r, ensure_ascii=False) + "\n")
        print(f"{name}: {len(rows)} rows")

    write("train.jsonl", train)
    write("train-200.jsonl", train[:200])
    write("train-1000.jsonl", train[:1000])
    write("held-out.jsonl", held)


if __name__ == "__main__":
    main()
