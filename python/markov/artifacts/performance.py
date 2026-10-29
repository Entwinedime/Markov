"""Store independent measurements by capture, original position and quantity."""

from dataclasses import dataclass
import json
from pathlib import Path
import sqlite3


@dataclass(frozen=True)
class Measurement:
    position: int
    quantity: str
    operation: str
    implementation: str
    timing_scope: str
    features: dict[str, float]
    duration_ns: int
    warmup: bool


def encoded(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


class PerformanceData:
    def __init__(self, path: Path):
        self.connection = sqlite3.connect(path)
        self.connection.execute("PRAGMA foreign_keys = ON")
        self.connection.executescript(
            """
            CREATE TABLE IF NOT EXISTS captures (
                identity TEXT PRIMARY KEY,
                adapter TEXT NOT NULL,
                context TEXT NOT NULL,
                metadata TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS measurements (
                capture TEXT NOT NULL REFERENCES captures(identity),
                position INTEGER NOT NULL,
                quantity TEXT NOT NULL,
                operation TEXT NOT NULL,
                implementation TEXT NOT NULL,
                timing_scope TEXT NOT NULL,
                features TEXT NOT NULL,
                duration_ns INTEGER NOT NULL CHECK(duration_ns >= 0),
                warmup INTEGER NOT NULL,
                PRIMARY KEY(capture, position, quantity)
            );
            """
        )

    def close(self):
        self.connection.close()

    def import_capture(self, path, adapter, context, metadata, measurements):
        identity = str(path)
        capture = (adapter, encoded(context), encoded(metadata))
        inserted = reused = 0
        with self.connection:
            existing = self.connection.execute(
                "SELECT adapter, context, metadata FROM captures WHERE identity = ?", (identity,)
            ).fetchone()
            if existing is not None and existing != capture:
                raise ValueError(f"Capture identity conflicts with stored metadata: {identity}")
            self.connection.execute("INSERT OR IGNORE INTO captures VALUES (?, ?, ?, ?)", (identity, *capture))
            for sample in measurements:
                key = (identity, sample.position, sample.quantity)
                value = (
                    sample.operation,
                    sample.implementation,
                    sample.timing_scope,
                    encoded(sample.features),
                    sample.duration_ns,
                    int(sample.warmup),
                )
                existing = self.connection.execute(
                    "SELECT operation, implementation, timing_scope, features, duration_ns, warmup "
                    "FROM measurements WHERE capture = ? AND position = ? AND quantity = ?",
                    key,
                ).fetchone()
                if existing is not None:
                    if existing != value:
                        raise ValueError(
                            f"Conflicting measurement: {identity}, line {sample.position}, {sample.quantity}"
                        )
                    reused += 1
                    continue
                self.connection.execute("INSERT INTO measurements VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)", (*key, *value))
                inserted += 1
        return {"capture": identity, "inserted": inserted, "reused": reused}

    def samples(self, path: Path):
        capture = self.connection.execute(
            "SELECT adapter, context, metadata FROM captures WHERE identity = ?", (str(path),)
        ).fetchone()
        if capture is None:
            raise ValueError(f"Training capture has not been imported: {path}")
        rows = self.connection.execute(
            "SELECT operation, implementation, timing_scope, features, duration_ns "
            "FROM measurements WHERE capture = ? AND warmup = 0 ORDER BY position, quantity",
            (str(path),),
        )
        return (
            capture[0],
            json.loads(capture[1]),
            json.loads(capture[2]),
            [
                (operation, implementation, scope, json.loads(features), duration)
                for operation, implementation, scope, features, duration in rows
            ],
        )
