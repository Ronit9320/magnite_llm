from __future__ import annotations

from datetime import datetime, timezone
import json
import sqlite3
from uuid import uuid4

from backend.blueprints.schemas import (
    ACTIVE_RUNS, Blueprint, BlueprintSummary, Revision, Run, RunSnapshot, RunSummary, RunSurface, Step,
)
from backend.database import connection, open_or_quarantine
from backend.tooling.approvals import SCHEMA as APPROVAL_SCHEMA, close_approvals


_SCHEMA = """
BEGIN IMMEDIATE;
CREATE TABLE revisions (
    id TEXT NOT NULL,
    revision INTEGER NOT NULL,
    updated_at TEXT NOT NULL,
    name TEXT NOT NULL,
    blueprint TEXT NOT NULL,
    PRIMARY KEY (id, revision)
);
CREATE TABLE runs (
    id TEXT PRIMARY KEY,
    blueprint_id TEXT NOT NULL,
    status TEXT NOT NULL,
    created_at TEXT NOT NULL,
    summary TEXT NOT NULL,
    snapshot TEXT NOT NULL,
    results TEXT NOT NULL DEFAULT '{}'
);
CREATE INDEX runs_by_blueprint ON runs (blueprint_id, created_at DESC);
CREATE TABLE steps (
    run_id TEXT NOT NULL,
    position INTEGER NOT NULL,
    node_id TEXT NOT NULL,
    state TEXT NOT NULL,
    PRIMARY KEY (run_id, node_id)
);
CREATE TABLE events (
    sequence INTEGER PRIMARY KEY,
    run_id TEXT NOT NULL,
    kind TEXT NOT NULL,
    payload TEXT NOT NULL
);
CREATE INDEX events_by_run ON events (run_id, sequence);
PRAGMA user_version = 1;
COMMIT;
"""


def now() -> datetime:
    return datetime.now(timezone.utc)


def _init_db(conn: sqlite3.Connection) -> None:
    version = conn.execute("PRAGMA user_version").fetchone()[0]
    if version == 0:
        conn.executescript(_SCHEMA)
    elif version != 1:
        raise RuntimeError(f"Unsupported blueprints.db storage version: {version}")
    conn.executescript(APPROVAL_SCHEMA)


class RevisionConflict(ValueError):
    pass


class BlueprintStore:
    def __init__(self, path: str):
        self.path = path
        open_or_quarantine(path, _init_db, hint="Saved blueprints and run history are no longer listed.")

    def save(self, blueprint: Blueprint, blueprint_id: str | None = None,
             *, expected_revision: int | None = None) -> Revision | None:
        with connection(self.path) as conn:
            conn.execute("BEGIN IMMEDIATE")
            revision = 1
            if blueprint_id is None:
                blueprint_id = uuid4().hex
            else:
                latest = conn.execute("SELECT MAX(revision) FROM revisions WHERE id = ?",
                                      (blueprint_id,)).fetchone()[0]
                if latest is None and expected_revision != 0:
                    return None
                latest = latest or 0
                if expected_revision is not None and latest != expected_revision:
                    if latest == expected_revision + 1:
                        row = conn.execute("SELECT blueprint, updated_at FROM revisions WHERE id = ? AND revision = ?",
                                           (blueprint_id, latest)).fetchone()
                        if Blueprint.model_validate_json(row["blueprint"]) == blueprint:
                            return Revision(id=blueprint_id, revision=latest, updated_at=row["updated_at"],
                                            blueprint=blueprint)
                    raise RevisionConflict("The blueprint was edited. Reload its latest revision and retry.")
                revision = latest + 1
            updated_at = now()
            conn.execute("INSERT INTO revisions VALUES (?, ?, ?, ?, ?)",
                         (blueprint_id, revision, updated_at.isoformat(), blueprint.name,
                          blueprint.model_dump_json(exclude_none=True)))
        return Revision(id=blueprint_id, revision=revision, updated_at=updated_at, blueprint=blueprint)

    def delete(self, blueprint_id: str, expected_revision: int) -> bool:
        with connection(self.path) as conn:
            conn.execute("BEGIN IMMEDIATE")
            latest = conn.execute("SELECT MAX(revision) FROM revisions WHERE id = ?",
                                  (blueprint_id,)).fetchone()[0]
            if latest is None:
                return False
            if latest != expected_revision:
                raise RevisionConflict("The blueprint was edited. Reload its latest revision and retry.")
            conn.execute("DELETE FROM revisions WHERE id = ?", (blueprint_id,))
        return True

    def revisions(self, limit: int, offset: int) -> list[BlueprintSummary]:
        with connection(self.path) as conn:
            rows = conn.execute(
                """SELECT id, revision, updated_at, name FROM revisions AS rev
                   WHERE NOT EXISTS (SELECT 1 FROM revisions WHERE id = rev.id AND revision > rev.revision)
                   ORDER BY updated_at DESC, id LIMIT ? OFFSET ?""", (limit, offset)).fetchall()
        return [BlueprintSummary(**dict(row)) for row in rows]

    def revision(self, blueprint_id: str, revision: int | None = None) -> Revision | None:
        with connection(self.path) as conn:
            if revision is None:
                row = conn.execute(
                    "SELECT * FROM revisions WHERE id = ? ORDER BY revision DESC LIMIT 1",
                    (blueprint_id,)).fetchone()
            else:
                row = conn.execute("SELECT * FROM revisions WHERE id = ? AND revision = ?",
                                   (blueprint_id, revision)).fetchone()
        if row is None:
            return None
        return Revision(id=row["id"], revision=row["revision"], updated_at=row["updated_at"],
                        blueprint=Blueprint.model_validate_json(row["blueprint"]))

    def create_run(self, run: RunSummary, snapshot: RunSnapshot, steps: list[Step]) -> None:
        with connection(self.path) as conn:
            conn.execute(
                "INSERT INTO runs (id, blueprint_id, status, created_at, summary, snapshot) VALUES (?, ?, ?, ?, ?, ?)",
                (run.id, run.blueprint_id, run.status, run.created_at.isoformat(),
                 run.model_dump_json(), snapshot.model_dump_json(exclude_none=True)))
            conn.executemany("INSERT INTO steps VALUES (?, ?, ?, ?)", [
                (run.id, i, step.node_id, step.model_dump_json()) for i, step in enumerate(steps)
            ])
            self._event(conn, run.id, "run", run.model_dump_json())

    def transition(self, run: RunSummary, steps: list[Step], *, run_event: bool = False,
                   results_json: str | None = None) -> None:
        with connection(self.path) as conn:
            conn.execute("BEGIN IMMEDIATE")
            for step in steps:
                conn.execute("UPDATE steps SET state = ? WHERE run_id = ? AND node_id = ?",
                             (step.model_dump_json(), run.id, step.node_id))
                payload = step.model_dump_json(exclude={"inputs", "outputs"})
                self._event(conn, run.id, "node", payload)
            if run_event:
                if run.status not in ACTIVE_RUNS:
                    close_approvals(conn, run.id, interrupted=bool(run.error and run.error.code == "interrupted"))
                conn.execute("UPDATE runs SET status = ?, summary = ?, results = COALESCE(?, results) WHERE id = ?",
                             (run.status, run.model_dump_json(), results_json, run.id))
                self._event(conn, run.id, "run", run.model_dump_json())

    @staticmethod
    def _event(conn, run_id, kind, payload):
        conn.execute("INSERT INTO events (run_id, kind, payload) VALUES (?, ?, ?)",
                     (run_id, kind, payload))

    @staticmethod
    def _steps(conn, run_id: str) -> list[Step]:
        rows = conn.execute("SELECT state FROM steps WHERE run_id = ? ORDER BY position", (run_id,))
        return [Step.model_validate_json(row["state"]) for row in rows]

    def run(self, run_id: str) -> Run | None:
        with connection(self.path) as conn:
            conn.execute("BEGIN")
            row = conn.execute("SELECT * FROM runs WHERE id = ?", (run_id,)).fetchone()
            if row is None:
                return None
            steps = self._steps(conn, run_id)
        summary = RunSummary.model_validate_json(row["summary"])
        return Run(**summary.model_dump(), snapshot=RunSnapshot.model_validate_json(row["snapshot"]),
                   steps=steps, results=json.loads(row["results"]))

    def summary(self, run_id: str) -> RunSummary | None:
        with connection(self.path) as conn:
            row = conn.execute("SELECT summary FROM runs WHERE id = ?", (run_id,)).fetchone()
        return RunSummary.model_validate_json(row["summary"]) if row is not None else None

    def history(self, blueprint_id: str | None, limit: int, offset: int,
                surface: RunSurface | None = None, active: bool = False) -> list[RunSummary]:
        clauses, params = [], []
        if blueprint_id is not None:
            clauses.append("blueprint_id = ?")
            params.append(blueprint_id)
        if surface is not None:
            clauses.append("COALESCE(json_extract(summary, '$.surface'), 'blueprints') = ?")
            params.append(surface)
        if active:
            clauses.append("status IN (?, ?)")
            params.extend(ACTIVE_RUNS)
        where = "WHERE " + " AND ".join(clauses) if clauses else ""
        with connection(self.path) as conn:
            rows = conn.execute(
                f"SELECT summary FROM runs {where} ORDER BY created_at DESC, id LIMIT ? OFFSET ?",
                (*params, limit, offset)).fetchall()
        return [RunSummary.model_validate_json(row["summary"]) for row in rows]

    def interrupted(self) -> list[tuple[RunSummary, list[Step]]]:
        with connection(self.path) as conn:
            rows = conn.execute("SELECT id, summary FROM runs WHERE status IN (?, ?)", ACTIVE_RUNS).fetchall()
            return [(RunSummary.model_validate_json(row["summary"]), self._steps(conn, row["id"])) for row in rows]

    def events(self, run_id: str, after: int) -> tuple[list[sqlite3.Row], bool]:
        with connection(self.path) as conn:
            conn.execute("BEGIN")
            run = conn.execute("SELECT status FROM runs WHERE id = ?", (run_id,)).fetchone()
            rows = conn.execute(
                "SELECT sequence, kind, payload FROM events WHERE run_id = ? AND sequence > ? ORDER BY sequence",
                (run_id, after)).fetchall()
        return rows, run["status"] not in ACTIVE_RUNS
