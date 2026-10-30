"""Explicit conversion of the row-based performance store to exact compact samples."""

from __future__ import annotations

from contextlib import closing
import json
import os
from pathlib import Path
import sqlite3
from tempfile import TemporaryDirectory
from typing import Any

from .database import create_tables
from .measurements import SampleWriter


def migrate_database(source: Path, output: Path) -> dict[str, Any]:
    """Preserve current fact-store measurements and provenance in a new output.

    The source stays read-only. Source positions, duration doubles and sample
    multiplicities remain exact. No raw trace or historic measurement file is
    reopened; trained packages are not changed. Publish only after the complete
    transaction, and never replace an existing output.
    """

    if output.exists():
        raise FileExistsError(output)

    output.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix="cost_import_", dir=output.parent) as directory:
        temporary = Path(directory) / "costs.sqlite"
        with closing(sqlite3.connect(temporary, uri=True)) as connection:
            connection.execute("ATTACH DATABASE ? AS original", (source.resolve().as_uri() + "?mode=ro",))
            create_tables(connection)
            with connection:
                for table in ("deployments", "captures", "capture_sources", "evidence"):
                    connection.execute(f"INSERT INTO {table} SELECT * FROM original.{table}")

                writer = SampleWriter(connection)
                features: dict[str, dict[str, float]] = {}
                for (
                    capture,
                    origin,
                    position,
                    operation,
                    implementation,
                    geometry,
                    duration,
                    scope,
                ) in connection.execute(
                    "SELECT capture_id, source_id, position, operation, implementation, features, duration_us, timing_scope "
                    "FROM original.observations ORDER BY capture_id, position, timing_scope"
                ):
                    if geometry not in features:
                        features[geometry] = json.loads(geometry)
                    writer.add(
                        capture, origin, position, operation, implementation, scope, features[geometry], duration
                    )
                writer.flush()
                count = writer.counts["inserted"]

        os.link(temporary, output)

    return {
        "observations": count,
        "source_bytes": source.stat().st_size,
        "output_bytes": output.stat().st_size,
        "source": str(source),
        "output": str(output),
        "scope": "Exact fact-store conversion, including source positions and duration frequencies; no model training or prediction accuracy claim.",
    }
