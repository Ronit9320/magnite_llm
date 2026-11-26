"""One-use approval records for the Blueprint and chat run worker."""

from __future__ import annotations

from collections import OrderedDict
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
import hashlib
import json
from pathlib import Path
from threading import Condition, Event
from time import monotonic
from typing import Annotated, Literal
from uuid import uuid4

from pydantic import BaseModel, ConfigDict, StringConstraints

from backend.database import connection
from backend.tooling import actions


SCHEMA = """
CREATE TABLE IF NOT EXISTS tool_approvals (
    id TEXT PRIMARY KEY,
    run_id TEXT NOT NULL,
    audit TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS tool_approvals_by_run ON tool_approvals (run_id);
"""

_ACTIVE = ("pending", "approved", "executing")
MAX_REJECTED_RUNS = 50
ApprovalStatus = Literal["pending", "approved", "executing", "completed", "failed",
                         "denied", "expired", "cancelled", "interrupted", "rejected"]
ApprovalDecision = Literal["approve_once", "deny"]


class Decision(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    decision: ApprovalDecision
    fingerprint: Annotated[str, StringConstraints(pattern=r"^[a-f0-9]{64}$")]


class Approval(BaseModel):
    id: str
    run_id: str
    node_id: str
    kind: Literal["open", "read", "command", "write", "unknown"]
    fingerprint: str
    status: ApprovalStatus = "pending"
    created_at: datetime
    expires_at: datetime
    updated_at: datetime | None = None
    decision: ApprovalDecision | None = None
    decided_at: datetime | None = None
    outcome: str | None = None
    action: dict | None = None


class ApprovalConflict(ValueError):
    pass


@dataclass(frozen=True)
class _Pending:
    action: actions.PreparedAction
    deadline: float


def _now() -> datetime:
    return datetime.now(timezone.utc)


def _records(conn, run_id) -> list[Approval]:
    return [Approval.model_validate_json(row["audit"]) for row in conn.execute(
        "SELECT audit FROM tool_approvals WHERE run_id = ? ORDER BY rowid", (run_id,)
    )]


def _write(conn, audit) -> None:
    audit.updated_at = _now()
    payload = audit.model_dump_json(exclude={"action"}, exclude_none=True)
    conn.execute("INSERT INTO tool_approvals VALUES (?, ?, ?) ON CONFLICT(id) DO UPDATE SET audit = excluded.audit",
                 (audit.id, audit.run_id, payload))
    conn.execute("INSERT INTO events (run_id, kind, payload) VALUES (?, 'approval', ?)", (audit.run_id, payload))


def close_approvals(conn, run_id: str, interrupted: bool) -> None:
    """Settle open approvals in the same transaction as the terminal run event."""
    for item in _records(conn, run_id):
        if item.status in _ACTIVE:
            _write(conn, item.model_copy(update={
                "status": "interrupted" if interrupted else "cancelled",
                "outcome": "backend_interrupted" if interrupted else "run_ended",
            }))


class ToolBroker:
    def __init__(self, db: str, *, ttl: float = 90):
        self.db = db
        self.ttl = ttl
        self._condition = Condition()
        self._pending: dict[str, _Pending] = {}
        # Refused attempts are shown to the user but never stored: no database row, no event.
        self._rejected: OrderedDict[str, list[Approval]] = OrderedDict()

    def reject(self, run_id: str, node_id: str, kind: str, action: dict, reason: str) -> None:
        """Remember an attempt that was refused before it could be offered for approval."""
        created = _now()
        item = Approval(id=uuid4().hex, run_id=run_id, node_id=node_id, kind=kind, fingerprint="",
                        status="rejected", created_at=created, expires_at=created, outcome=reason,
                        action=action)
        with self._condition:
            self._rejected.setdefault(run_id, []).append(item)
            self._rejected.move_to_end(run_id)
            while len(self._rejected) > MAX_REJECTED_RUNS:
                self._rejected.popitem(last=False)

    def list(self, run_id: str) -> list[Approval]:
        with self._condition, connection(self.db) as conn:
            result = _records(conn, run_id)
            for item in result:
                pending = self._pending.get(item.id)
                if item.status == "pending" and pending is not None:
                    item.action = pending.action.display()
            return sorted([*result, *self._rejected.get(run_id, [])], key=lambda item: item.created_at)

    @staticmethod
    def _read(conn, run_id, approval_id) -> Approval | None:
        row = conn.execute("SELECT audit FROM tool_approvals WHERE id = ? AND run_id = ?",
                           (approval_id, run_id)).fetchone()
        return Approval.model_validate_json(row["audit"]) if row is not None else None

    def decide(self, run_id: str, approval_id: str, decision: Decision) -> Approval | None:
        with self._condition, connection(self.db) as conn:
            conn.execute("BEGIN IMMEDIATE")
            item = self._read(conn, run_id, approval_id)
            if item is None:
                return None
            pending = self._pending.get(item.id)
            if item.status != "pending" or pending is None or pending.deadline <= monotonic():
                raise ApprovalConflict("This approval is no longer pending")
            if item.fingerprint != decision.fingerprint:
                raise ApprovalConflict("The approved action fingerprint does not match")
            item.status = "approved" if decision.decision == "approve_once" else "denied"
            item.decision = decision.decision
            item.decided_at = _now()
            _write(conn, item)
            self._condition.notify_all()
            return item

    def perform(self, run_id: str, node_id: str, proposal, directory: Path, stop: Event) -> dict:
        """Only a worker that owns a resolved proposal may claim and execute it."""
        try:
            action = actions.prepare(proposal, directory)
        except actions.Rejected as exc:
            self.reject(run_id, node_id, *actions.describe(proposal), exc.reason)
            raise
        identifier = uuid4().hex
        data = json.dumps([run_id, node_id, identifier, action.display(),
                           [path.identity for path in action.paths]], ensure_ascii=True, sort_keys=True)
        created = _now()
        item = Approval(id=identifier, run_id=run_id, node_id=node_id, kind=action.action,
                        fingerprint=hashlib.sha256(data.encode()).hexdigest(), created_at=created,
                        expires_at=created + timedelta(seconds=self.ttl))
        pending = _Pending(action, monotonic() + self.ttl)
        with self._condition:
            with connection(self.db) as conn:
                conn.execute("BEGIN IMMEDIATE")
                run = conn.execute("SELECT status FROM runs WHERE id = ?", (run_id,)).fetchone()
                if stop.is_set() or run is None or run["status"] != "running":
                    return {"status": "cancelled"}
                _write(conn, item)
            self._pending[item.id] = pending
        try:
            item = self._await_claim(item, pending, stop)
            result = None
            if item.status == "executing":
                try:
                    result = actions.execute(action, stop)
                    outcome = result.outcome
                except (OSError, ValueError, RuntimeError):
                    outcome = "dispatch_failed"
                with connection(self.db) as conn:
                    conn.execute("BEGIN IMMEDIATE")
                    current = self._read(conn, item.run_id, item.id)
                    if current.status == "executing":
                        if outcome in ("completed", "dispatched"):
                            current.status = "completed"
                        else:
                            current.status = "cancelled" if outcome == "cancelled" else "failed"
                        current.outcome = outcome
                        _write(conn, current)
                    item = current
            receipt = {"status": item.status, "outcome": item.outcome, "approval_id": item.id}
            if result is not None and result.exit_code is not None:
                receipt["exit_code"] = result.exit_code
            if result is not None and result.output is not None:
                # Returned to the requesting run only; the audit record keeps the outcome code.
                receipt["output"] = result.output
            return receipt
        finally:
            with self._condition:
                self._pending.pop(item.id, None)

    def _await_claim(self, item: Approval, pending: _Pending, stop: Event) -> Approval:
        with self._condition:
            while True:
                with connection(self.db) as conn:
                    conn.execute("BEGIN IMMEDIATE")
                    item = self._read(conn, item.run_id, item.id)
                    if item.status not in ("pending", "approved"):
                        return item
                    if stop.is_set():
                        item.status = "cancelled"
                    elif pending.deadline <= monotonic():
                        item.status = "expired"
                    elif item.status == "approved":
                        item.status = "executing"
                    if item.status != "pending":
                        _write(conn, item)
                        return item
                self._condition.wait(0.15)
