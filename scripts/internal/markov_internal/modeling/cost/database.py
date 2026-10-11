"""SQLite observation store: deduplicate imports, not independent measurements."""

from __future__ import annotations

from collections import defaultdict
from contextlib import closing
from dataclasses import dataclass
import json
import math
from pathlib import Path
import sqlite3
from typing import Any

from ...common.paths import require_repo_path


@dataclass(frozen=True)
class OperationObservation:
    """One operation measured in one capture; durations are microseconds.

    capture and position identify measurement provenance, not fitting features.
    manifest names the original profile manifest or independent primitive
    measurement document; the latter does not claim to contain a runtime trace.
    deployment contains semantic resource/implementation settings. Different
    captures with identical work remain distinct measurements.
    """

    capture: str
    manifest: str
    deployment: dict[str, Any]
    position: str
    operation: str
    implementation: str
    features: dict[str, float]
    duration_us: float
    timing_scope: str

    @classmethod
    def from_raw(cls, raw: dict[str, Any]) -> OperationObservation:
        observation = cls(**raw)
        if not observation.capture or not observation.position or not observation.operation or not observation.manifest:
            raise ValueError("cost observations need capture, manifest, operation and source position")
        if observation.duration_us < 0 or not math.isfinite(observation.duration_us):
            raise ValueError("measured duration_us must be finite and non-negative")
        if not observation.timing_scope or any(not math.isfinite(value) for value in observation.features.values()):
            raise ValueError("cost observations need a timing scope and finite features")

        return observation


def _json(value: Any) -> str:
    """Canonical semantic values for SQLite equality, not an integrity digest."""

    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False)


def merge_control_measurements(selected: dict[str, Any], incoming: dict[str, Any]) -> None:
    """Add disjoint policy/page evidence without replacing a measured primitive.

    Used at import and offline selection. Alternatives for the same work need
    an explicit choice, not a file-order-dependent overwrite.
    """

    for key, value in incoming.items():
        if key not in selected:
            selected[key] = value
        elif selected[key] == value:
            continue
        elif "role" not in selected[key] and "role" not in value:
            merge_control_measurements(selected[key], value)
        else:
            raise ValueError(f"select one independent control measurement for {key}")


def evidence_manifests(evidence: dict[str, Any]) -> set[str]:
    """Original captures contributing to measured values, including paired light runs.

    Only stored provenance is inspected here. Calibration documents are expanded
    at explicit import; training never opens their historic measurement files.
    """
    manifests = {evidence["manifest"]}
    payload = evidence["payload"]
    if evidence["kind"] == "runtime_io":
        manifests.update(payload["measurement_manifests"])
    elif evidence["kind"] == "physical_io":
        manifests.update(payload["measurement_sources"])
    elif evidence["kind"] == "control_costs":

        def collect(value: dict[str, Any]) -> None:
            if "role" in value:
                manifests.update(value["measurement_manifests"])
                return

            for child in value.values():
                collect(child)

        collect(payload["control_measurements"])

    return manifests


def _capture_aliases(
    rows: list[tuple[str, str, int]], captures: set[str], manifests: set[Path]
) -> tuple[set[str], set[Path]]:
    """Expand capture identities, without following derived measurement inputs."""

    aliases: dict[str, set[Path]] = defaultdict(set)
    owners: dict[Path, set[str]] = defaultdict(set)
    for capture, manifest, is_capture in rows:
        if is_capture:
            path = require_repo_path(manifest).resolve()
            aliases[capture].add(path)
            owners[path].add(capture)

    paths = set(manifests)
    selected = set(captures)
    selected.update(capture for manifest in paths for capture in owners[manifest])
    pending = list(selected)
    while pending:
        capture = pending.pop()
        for manifest in aliases[capture]:
            paths.add(manifest)
            for owner in owners[manifest] - selected:
                selected.add(owner)
                pending.append(owner)

    return selected, paths


class CostDatabase:
    """Explicit import/write boundary; prediction does not open this database."""

    def __init__(self, path: Path) -> None:
        self.path = path

    def excluded_captures(self, captures: set[str], manifests: list[str]) -> set[str]:
        """Resolve declared holdouts across known aliases and paired sources.

        Reads stored provenance only, never the excluded trace files. Both
        observation-only and aggregate evidence imports participate. Paths
        normalize host/container spellings; renamed copies still require an
        explicit original capture identity, not a content fingerprint.
        """
        if not captures and not manifests:
            return set()

        excluded = {require_repo_path(path).resolve() for path in manifests}
        with closing(sqlite3.connect(f"file:{self.path.resolve()}?mode=ro", uri=True)) as connection:
            rows = connection.execute("SELECT capture, manifest, is_capture FROM capture_sources").fetchall()
        selected, excluded = _capture_aliases(rows, captures, excluded)
        # Selecting an original capture also excludes measurements derived
        # from any of its declared aliases. Do not recursively hold out the
        # other inputs of a derived measurement: sharing a light reference
        # does not turn otherwise independent full captures into aliases.
        return selected | {
            capture for capture, manifest, _ in rows if require_repo_path(manifest).resolve() in excluded
        }

    def manifests_for_captures(self, captures: set[str]) -> set[str]:
        """Keep all declared aliases and paired sources in the portable package.

        Prediction cannot consult this database to resolve an alias later, so
        provenance must travel with the trained parameters, not only the first
        path that happened to insert a deduplicated measurement.
        """

        with closing(sqlite3.connect(f"file:{self.path.resolve()}?mode=ro", uri=True)) as connection:
            rows = connection.execute("SELECT capture, manifest, is_capture FROM capture_sources").fetchall()
        selected, _ = _capture_aliases(rows, captures, set())
        return {manifest for capture, manifest, _ in rows if capture in selected}

    def import_measurements(
        self,
        observations: list[OperationObservation],
        evidence: list[dict[str, Any]],
        *,
        replace_captures: bool = False,
    ) -> dict[str, Any]:
        """Admit observations and their evidence in one transaction.

        Reimports deduplicate by original capture/position/scope. An additional
        control export from the same capture may add measured primitives but
        cannot replace one. A conflict rolls back the entire import.
        Explicit replacement supplies the complete current extraction for each
        incoming capture. Remove its old derived rows, including samples no
        longer measurable, but retain provenance aliases and raw files.
        """

        self.path.parent.mkdir(parents=True, exist_ok=True)
        evidence_counts = dict(inserted=0, extended=0, duplicate_imports=0)
        observation_counts = dict(inserted=0, duplicate_imports=0)
        replaced = (
            sorted({row.capture for row in observations} | {row["capture"] for row in evidence})
            if replace_captures
            else []
        )
        removed = dict(observations=0, evidence=0)
        with closing(sqlite3.connect(self.path)) as connection, connection:
            connection.execute(
                "CREATE TABLE IF NOT EXISTS capture_sources (capture TEXT NOT NULL, manifest TEXT NOT NULL, is_capture INTEGER NOT NULL, "
                "PRIMARY KEY (capture, manifest))"
            )
            connection.execute(
                "CREATE TABLE IF NOT EXISTS evidence (capture TEXT NOT NULL, kind TEXT NOT NULL, "
                "manifest TEXT NOT NULL, deployment TEXT NOT NULL, payload TEXT NOT NULL, "
                "PRIMARY KEY (capture, kind))"
            )
            connection.execute(
                "CREATE TABLE IF NOT EXISTS observations ("
                "capture TEXT NOT NULL, position TEXT NOT NULL, manifest TEXT NOT NULL, "
                "deployment TEXT NOT NULL, operation TEXT NOT NULL, implementation TEXT NOT NULL, "
                "features TEXT NOT NULL, duration_us REAL NOT NULL, timing_scope TEXT NOT NULL, "
                "PRIMARY KEY (capture, position, timing_scope))"
            )
            if replaced:
                for capture in replaced:
                    for table in ("observations", "evidence"):
                        removed[table] += connection.execute(
                            f"DELETE FROM {table} WHERE capture=?", (capture,)
                        ).rowcount

            for row in evidence:
                values = (row["capture"], row["kind"], row["manifest"], _json(row["deployment"]), _json(row["payload"]))
                stored = connection.execute(
                    "SELECT deployment, payload FROM evidence WHERE capture=? AND kind=?", values[:2]
                ).fetchone()
                if stored is not None:
                    if stored == values[3:]:
                        evidence_counts["duplicate_imports"] += 1
                        continue
                    if stored[0] != values[3] or row["kind"] != "control_costs":
                        raise ValueError(f"conflicting measurement reimport: {row['capture']}/{row['kind']}")

                    payload = json.loads(stored[1])
                    for key, value in row["payload"].items():
                        if key == "control_measurements":
                            merge_control_measurements(payload.setdefault(key, {}), value)
                        elif key not in payload or payload[key] == value:
                            payload[key] = value
                        else:
                            raise ValueError(f"conflicting measurement reimport: {row['capture']}/{key}")
                    combined = _json(payload)
                    if combined == stored[1]:
                        evidence_counts["duplicate_imports"] += 1
                        continue

                    connection.execute(
                        "UPDATE evidence SET payload=? WHERE capture=? AND kind=?", (combined, *values[:2])
                    )
                    evidence_counts["extended"] += 1
                    continue

                connection.execute("INSERT INTO evidence VALUES (?, ?, ?, ?, ?)", values)
                evidence_counts["inserted"] += 1

            connection.execute(
                "CREATE INDEX IF NOT EXISTS observations_operation "
                "ON observations (deployment, operation, implementation, timing_scope)"
            )
            for row in observations:
                values = (
                    row.capture,
                    row.position,
                    row.manifest,
                    _json(row.deployment),
                    row.operation,
                    row.implementation,
                    _json(row.features),
                    row.duration_us,
                    row.timing_scope,
                )
                stored = connection.execute(
                    "SELECT capture, position, manifest, deployment, operation, implementation, "
                    "features, duration_us, timing_scope FROM observations "
                    "WHERE capture=? AND position=? AND timing_scope=?",
                    (row.capture, row.position, row.timing_scope),
                ).fetchone()
                if stored is not None:
                    # Asset aliases may change the manifest path, but not the
                    # operation, measured work, deployment or measured duration.
                    if stored[:2] != values[:2] or stored[3:] != values[3:]:
                        raise ValueError(f"conflicting cost reimport: {row.capture}/{row.position}")
                    observation_counts["duplicate_imports"] += 1
                    continue

                connection.execute("INSERT INTO observations VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)", values)
                observation_counts["inserted"] += 1

            # A duplicate measurement still adds its explicitly declared path.
            # Store pair members too: excluding a light capture also excludes
            # the corrected service that was derived from it. This transaction
            # rolls back provenance together with any conflicting measurement.
            sources = {(row.capture, row.manifest): True for row in observations}
            for row in evidence:
                sources[row["capture"], row["manifest"]] = True
                for path in evidence_manifests(row):
                    sources.setdefault((row["capture"], path), False)
            connection.executemany(
                "INSERT INTO capture_sources VALUES (?, ?, ?) ON CONFLICT(capture, manifest) "
                "DO UPDATE SET is_capture=MAX(capture_sources.is_capture, excluded.is_capture)",
                [(capture, manifest, own) for (capture, manifest), own in sorted(sources.items())],
            )

        return {
            "operators": observation_counts,
            "evidence": evidence_counts,
            "replaced_captures": replaced,
            "removed_derived_rows": removed,
            "retrain_exported_packages": bool(replaced),
        }

    def evidence(self, deployment: dict[str, Any], *, excluded_captures: set[str]) -> list[dict[str, Any]]:
        """Select historical evidence only; each caller declares its deployment."""

        with closing(sqlite3.connect(f"file:{self.path.resolve()}?mode=ro", uri=True)) as connection:
            rows = connection.execute(
                "SELECT capture, kind, manifest, payload FROM evidence WHERE deployment=? ORDER BY capture, kind",
                (_json(deployment),),
            ).fetchall()

        return [
            dict(capture=capture, kind=kind, manifest=manifest, payload=json.loads(payload))
            for capture, kind, manifest, payload in rows
            if capture not in excluded_captures
        ]

    def observations(
        self, deployment: dict[str, Any], *, excluded_captures: set[str] | None = None
    ) -> list[OperationObservation]:
        """Read one deployment, omitting holdouts; returned metadata is read-only.

        All rows share the selected deployment description. Stream SQLite rows
        instead of retaining a second complete list of serialized measurements.
        """

        excluded = excluded_captures or set()
        environment_json = _json(deployment)
        environment = json.loads(environment_json)
        with closing(sqlite3.connect(f"file:{self.path.resolve()}?mode=ro", uri=True)) as connection:
            stored = connection.execute(
                "SELECT capture, position, manifest, operation, implementation, "
                "features, duration_us, timing_scope FROM observations WHERE deployment=? "
                "ORDER BY operation, implementation, timing_scope, capture, position",
                (environment_json,),
            )

            return [
                OperationObservation(
                    capture=capture,
                    manifest=manifest,
                    deployment=environment,
                    position=position,
                    operation=operation,
                    implementation=implementation,
                    features=json.loads(features),
                    duration_us=duration,
                    timing_scope=scope,
                )
                for capture, position, manifest, operation, implementation, features, duration, scope in stored
                if capture not in excluded
            ]

    def summary(self) -> dict[str, Any]:
        """Report stored deployment/operator coverage without creating a database."""

        with closing(sqlite3.connect(f"file:{self.path.resolve()}?mode=ro", uri=True)) as connection:
            groups = connection.execute(
                "SELECT deployment, operation, implementation, timing_scope, COUNT(*), COUNT(DISTINCT capture) "
                "FROM observations GROUP BY deployment, operation, implementation, timing_scope "
                "ORDER BY deployment, operation, implementation, timing_scope"
            ).fetchall()

            # Distinct work descriptions suffice for geometry coverage; do not
            # load every repeated layer measurement or infer identifiability
            # from a large raw sample count.
            coverage: dict[tuple[str, str, str, str], dict[str, Any]] = {}
            for environment, operation, implementation, scope, raw_features, count in connection.execute(
                "SELECT deployment, operation, implementation, timing_scope, features, COUNT(*) "
                "FROM observations GROUP BY deployment, operation, implementation, timing_scope, features"
            ):
                key = environment, operation, implementation, scope
                group = coverage.setdefault(key, {"distinct_work_points": 0, "features": {}})
                group["distinct_work_points"] += 1
                for name, value in json.loads(raw_features).items():
                    feature = group["features"].setdefault(name, {"range": [value, value], "measurements": 0})
                    feature["range"][0] = min(feature["range"][0], value)
                    feature["range"][1] = max(feature["range"][1], value)
                    feature["measurements"] += count

        for group in coverage.values():
            group["constant_features"] = sorted(
                name for name, feature in group["features"].items() if feature["range"][0] == feature["range"][1]
            )

        return {
            "measurement_count": sum(row[4] for row in groups),
            "coverage_scope": (
                "Stored observation geometry, not trained-model readiness. Feature counts reveal missing coordinates; "
                "ranges do not prove joint coverage or identifiable coefficients."
            ),
            "groups": [
                {
                    "deployment": json.loads(environment),
                    "operation": operation,
                    "implementation": implementation,
                    "timing_scope": scope,
                    "measurements": count,
                    "captures": captures,
                    **coverage[(environment, operation, implementation, scope)],
                }
                for environment, operation, implementation, scope, count, captures in groups
            ],
        }
