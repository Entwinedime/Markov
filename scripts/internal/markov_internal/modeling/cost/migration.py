"""Explicit conversion of the previous performance store to current fact tables."""

from __future__ import annotations

from contextlib import closing
import json
import os
from pathlib import Path
import sqlite3
from tempfile import TemporaryDirectory
from typing import Any

from .database import aggregate_payload, create_tables
from .physical import read_physical_measurements


def migrate_database(source: Path, output: Path) -> dict[str, Any]:
    """Convert declared old storage, resolving physical sources at this explicit boundary.

    Source is read-only. Publish a new database exclusively after its complete
    transaction; an existing output is never replaced. Measurement values and
    aliases remain unchanged, unused report copies are omitted. Physical parameters
    are replaced by their original declared measurements, never inverted into
    synthetic samples. Missing source files fail naturally. Failure removes
    only this call's temporary output, leaving source and published assets alone.
    """

    output.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix="cost_import_", dir=output.parent) as directory:
        temporary = Path(directory) / "costs.sqlite"
        with closing(sqlite3.connect(temporary, uri=True)) as connection:
            connection.execute("ATTACH DATABASE ? AS original", (source.resolve().as_uri() + "?mode=ro",))
            create_tables(connection)
            with connection:
                # Capture identity is small; do not load the deployment JSON
                # carried by millions of old rows into a second Python list.
                for (capture,) in connection.execute("SELECT DISTINCT capture FROM original.capture_sources"):
                    environment = connection.execute(
                        "SELECT deployment FROM original.observations WHERE capture=? LIMIT 1", (capture,)
                    ).fetchone()
                    if environment is None:
                        environment = connection.execute(
                            "SELECT deployment FROM original.evidence WHERE capture=? LIMIT 1", (capture,)
                        ).fetchone()

                    connection.execute(
                        "INSERT INTO deployments (description) VALUES (?) ON CONFLICT(description) DO NOTHING",
                        environment,
                    )
                    connection.execute(
                        "INSERT INTO captures (name, deployment_id) SELECT ?, id FROM deployments WHERE description=?",
                        (capture, environment[0]),
                    )

                connection.execute(
                    "INSERT INTO capture_sources (capture_id, manifest, is_capture) "
                    "SELECT c.id, s.manifest, s.is_capture FROM original.capture_sources s "
                    "LEFT JOIN captures c ON c.name=s.capture"
                )
                # A missing source or conflicting deployment produces a NULL
                # in a required column rather than silently dropping a fact.
                measurements = connection.execute(
                    "INSERT INTO observations "
                    "SELECT CASE WHEN d.description=o.deployment THEN c.id END, s.id, "
                    "o.position, o.operation, o.implementation, o.features, o.duration_us, o.timing_scope "
                    "FROM original.observations o LEFT JOIN captures c ON c.name=o.capture "
                    "LEFT JOIN deployments d ON d.id=c.deployment_id "
                    "LEFT JOIN capture_sources s ON s.capture_id=c.id AND s.manifest=o.manifest"
                ).rowcount

                aggregates = 0
                for capture, kind, manifest, deployment, raw in connection.execute(
                    "SELECT capture, kind, manifest, deployment, payload FROM original.evidence"
                ):
                    if kind not in {"runtime_io", "physical_io"}:
                        continue
                    payload = json.loads(raw)
                    if kind == "physical_io":
                        payload["physical_measurements"] = read_physical_measurements(payload, json.loads(deployment))
                    payload = aggregate_payload(kind, payload)
                    connection.execute(
                        "INSERT INTO evidence SELECT CASE WHEN d.description=? THEN c.id END, s.id, ?, ? "
                        "FROM captures c JOIN deployments d ON d.id=c.deployment_id "
                        "LEFT JOIN capture_sources s ON s.capture_id=c.id AND s.manifest=? WHERE c.name=?",
                        (
                            deployment,
                            kind,
                            json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":")),
                            manifest,
                            capture,
                        ),
                    )
                    aggregates += 1

                connection.execute(
                    "CREATE INDEX observations_work ON observations (operation, implementation, timing_scope, capture_id)"
                )

        os.link(temporary, output)

    return {
        "observations": measurements,
        "io_evidence": aggregates,
        "source_bytes": source.stat().st_size,
        "output_bytes": output.stat().st_size,
        "source": str(source),
        "output": str(output),
        "scope": "Fact conversion and declared physical measurement admission; no model training or prediction accuracy claim.",
    }
