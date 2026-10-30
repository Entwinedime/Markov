"""SQLite observation store: deduplicate imports, not independent measurements."""

from __future__ import annotations

from collections import defaultdict
from collections.abc import Iterable
from contextlib import closing
import json
from pathlib import Path
import sqlite3
from typing import Any

from ...common.paths import require_repo_path
from ..deployment import cost_environment_matches
from .control import control_entries
from .measurements import OperationObservation, OperationSamples, SampleWriter, canonical_json, unpack_samples


def _deployment_ids(connection: sqlite3.Connection, deployment: dict[str, Any]) -> tuple[int, ...]:
    """Select the performance domain without copying diagnostic credentials.

    Stored descriptions remain provenance. Training uses the same resource and
    implementation comparison as prediction, not equality of access credentials.
    """

    return tuple(
        identity
        for identity, description in connection.execute("SELECT id, description FROM deployments")
        if cost_environment_matches(json.loads(description), deployment)
    )


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
        for _, value in control_entries(payload["control_measurements"]):
            manifests.update(value["measurement_manifests"])

    return manifests


def aggregate_payload(kind: str, payload: dict[str, Any]) -> dict[str, Any] | None:
    """Keep only independent I/O facts consumed by offline service training.

    Control and CPU primitive rows live in observations, their pairing lives
    in capture_sources. Export layouts and graph diagnostics are not facts to
    store again or compare during reimport. Raw documents remain external.
    """

    if kind == "runtime_io":
        result = {"source_io_observations": payload["source_io_observations"]}
        if "reference_io" in payload:
            result["reference_io"] = payload["reference_io"]
        return result

    if kind == "physical_io":
        result = {
            key: payload[key]
            for key in (
                "kv_geometry",
                "resource_lanes",
                "physical_measurements",
                "storage_batch_pages",
                "measurement_sources",
            )
        }
        if "measurement_scope" in payload:
            result["measurement_scope"] = payload["measurement_scope"]
        return result

    return None


def create_tables(connection: sqlite3.Connection) -> None:
    """Start a transaction covering tables and facts on a fresh connection.

    The caller commits or rolls back. No automatic old-layout detection or
    conversion occurs on import; a column mismatch rolls back schema work too.
    """

    connection.execute("PRAGMA foreign_keys=ON")
    connection.executescript(
        """
        BEGIN;
        CREATE TABLE IF NOT EXISTS deployments (
            id INTEGER PRIMARY KEY, description TEXT NOT NULL UNIQUE
        );
        CREATE TABLE IF NOT EXISTS captures (
            id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE,
            deployment_id INTEGER NOT NULL REFERENCES deployments(id)
        );
        CREATE TABLE IF NOT EXISTS capture_sources (
            id INTEGER PRIMARY KEY, capture_id INTEGER NOT NULL REFERENCES captures(id),
            manifest TEXT NOT NULL, is_capture INTEGER NOT NULL,
            UNIQUE (capture_id, manifest)
        );
        CREATE TABLE IF NOT EXISTS observations (
            capture_id INTEGER NOT NULL REFERENCES captures(id),
            source_id INTEGER NOT NULL REFERENCES capture_sources(id),
            work_id INTEGER NOT NULL REFERENCES work_points(id),
            count INTEGER NOT NULL CHECK (count > 0),
            distribution BLOB NOT NULL, positions BLOB NOT NULL,
            PRIMARY KEY (capture_id, source_id, work_id)
        ) WITHOUT ROWID;
        CREATE TABLE IF NOT EXISTS work_points (
            id INTEGER PRIMARY KEY,
            operation TEXT NOT NULL, implementation TEXT NOT NULL,
            timing_scope TEXT NOT NULL, features TEXT NOT NULL,
            UNIQUE (operation, implementation, timing_scope, features)
        );
        CREATE INDEX IF NOT EXISTS work_observations ON observations(work_id, capture_id);
        CREATE TABLE IF NOT EXISTS evidence (
            capture_id INTEGER NOT NULL REFERENCES captures(id),
            source_id INTEGER NOT NULL REFERENCES capture_sources(id),
            kind TEXT NOT NULL, payload TEXT NOT NULL,
            PRIMARY KEY (capture_id, kind)
        ) WITHOUT ROWID;
        """
    )


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

    def _sources(self) -> list[tuple[str, str, int]]:
        with closing(sqlite3.connect(f"file:{self.path.resolve()}?mode=ro", uri=True)) as connection:
            return connection.execute(
                "SELECT c.name, s.manifest, s.is_capture FROM capture_sources s JOIN captures c ON c.id=s.capture_id"
            ).fetchall()

    def capture_identity(self, manifest: str) -> str:
        """Reuse an admitted original identity, or name a new capture by its path.

        Suite-local run names repeat across actual runs. Only the same original
        manifest, or a caller's explicit alias, establishes measurement identity.
        """

        if not self.path.is_file():
            return manifest

        selected = require_repo_path(manifest).resolve()
        names = {
            capture
            for capture, source, own in self._sources()
            if own and require_repo_path(source).resolve() == selected
        }
        if len(names) > 1:
            raise ValueError(f"multiple original identities for {manifest}; declare --capture-aliases")

        return next(iter(names), manifest)

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
        rows = self._sources()
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

        rows = self._sources()
        selected, _ = _capture_aliases(rows, captures, set())
        return {manifest for capture, manifest, _ in rows if capture in selected}

    def import_measurements(
        self,
        observations: Iterable[OperationObservation],
        evidence: list[dict[str, Any]],
        *,
        replace_captures: bool = False,
    ) -> dict[str, Any]:
        """Admit observations and their evidence in one transaction.

        Reimports deduplicate by original capture/position/scope. An additional
        control export from the same capture may add measured primitives but
        cannot replace one. Only aggregate I/O facts need separate evidence;
        operator exports are not stored a second time. A conflict rolls back
        the entire import, including source aliases and deployment admission.
        Explicit replacement supplies the complete current extraction for each
        incoming capture. Remove its old derived rows, including samples no
        longer measurable, but retain provenance aliases and raw files.
        """

        self.path.parent.mkdir(parents=True, exist_ok=True)
        with closing(sqlite3.connect(self.path)) as connection:
            create_tables(connection)
            with connection:
                return self._import(connection, observations, evidence, replace_captures=replace_captures)

    def _import(
        self,
        connection: sqlite3.Connection,
        observations: Iterable[OperationObservation],
        evidence: list[dict[str, Any]],
        *,
        replace_captures: bool,
    ) -> dict[str, Any]:
        """Apply one admitted import; the caller owns its transaction."""

        evidence_counts = dict(inserted=0, duplicate_imports=0)
        writer = SampleWriter(connection)
        replaced = []
        removed = dict(observations=0, evidence=0)
        deployments = dict(connection.execute("SELECT description, id FROM deployments"))
        captures = {
            name: (identity, json.loads(description))
            for identity, name, description in connection.execute(
                "SELECT c.id, c.name, d.description FROM captures c JOIN deployments d ON d.id=c.deployment_id"
            )
        }
        sources = {
            (capture, manifest): (identity, bool(own))
            for identity, capture, manifest, own in connection.execute(
                "SELECT id, capture_id, manifest, is_capture FROM capture_sources"
            )
        }
        admitted_captures: set[str] = set()

        def admit_source(capture: str, manifest: str, deployment: dict[str, Any], own: bool) -> tuple[int, int]:
            new_capture = capture not in captures
            changed_environment = not new_capture and captures[capture][1] != deployment
            if changed_environment and (not replace_captures or capture in admitted_captures):
                raise ValueError(f"conflicting deployment for original capture: {capture}")

            if new_capture or changed_environment:
                description = canonical_json(deployment)
                if description not in deployments:
                    deployments[description] = connection.execute(
                        "INSERT INTO deployments (description) VALUES (?)", (description,)
                    ).lastrowid
                if new_capture:
                    identity = connection.execute(
                        "INSERT INTO captures (name, deployment_id) VALUES (?, ?)", (capture, deployments[description])
                    ).lastrowid
                else:
                    identity = captures[capture][0]
                    connection.execute(
                        "UPDATE captures SET deployment_id=? WHERE id=?", (deployments[description], identity)
                    )
                captures[capture] = identity, deployment

            identity = captures[capture][0]
            captures[capture] = identity, deployment
            if replace_captures and capture not in admitted_captures:
                replaced.append(capture)
                removed["observations"] += connection.execute(
                    "SELECT COALESCE(SUM(count),0) FROM observations WHERE capture_id=?", (identity,)
                ).fetchone()[0]
                connection.execute("DELETE FROM observations WHERE capture_id=?", (identity,))
                removed["evidence"] += connection.execute(
                    "DELETE FROM evidence WHERE capture_id=?", (identity,)
                ).rowcount
            admitted_captures.add(capture)

            key = identity, manifest
            if key not in sources:
                source_id = connection.execute(
                    "INSERT INTO capture_sources (capture_id, manifest, is_capture) VALUES (?, ?, ?)",
                    (identity, manifest, own),
                ).lastrowid
                sources[key] = source_id, own
            elif own and not sources[key][1]:
                connection.execute("UPDATE capture_sources SET is_capture=1 WHERE id=?", (sources[key][0],))
                sources[key] = sources[key][0], True

            return identity, sources[key][0]

        for row in evidence:
            identity, source_id = admit_source(row["capture"], row["manifest"], row["deployment"], True)
            for manifest in evidence_manifests(row):
                admit_source(row["capture"], manifest, row["deployment"], False)

            payload = aggregate_payload(row["kind"], row["payload"])
            if payload is None:
                continue

            encoded = canonical_json(payload)
            stored = connection.execute(
                "SELECT payload FROM evidence WHERE capture_id=? AND kind=?", (identity, row["kind"])
            ).fetchone()
            if stored is not None:
                if json.loads(stored[0]) != payload:
                    raise ValueError(f"conflicting measurement reimport: {row['capture']}/{row['kind']}")
                evidence_counts["duplicate_imports"] += 1
                continue

            connection.execute("INSERT INTO evidence VALUES (?, ?, ?, ?)", (identity, source_id, row["kind"], encoded))
            evidence_counts["inserted"] += 1

        for row in observations:
            identity, source_id = admit_source(row.capture, row.manifest, row.deployment, True)
            writer.add(
                identity,
                source_id,
                row.position,
                row.operation,
                row.implementation,
                row.timing_scope,
                row.features,
                row.duration_us,
            )

        writer.flush()

        return {
            "operators": writer.counts,
            "evidence": evidence_counts,
            "replaced_captures": sorted(replaced),
            "removed_derived_rows": removed,
            "retrain_exported_packages": bool(replaced),
        }

    def evidence(self, deployment: dict[str, Any], *, excluded_captures: set[str]) -> list[dict[str, Any]]:
        """Select aggregate I/O facts, not whole operator export reports."""

        with closing(sqlite3.connect(f"file:{self.path.resolve()}?mode=ro", uri=True)) as connection:
            deployments = _deployment_ids(connection, deployment)
            placeholders = ",".join("?" for _ in deployments)
            rows = connection.execute(
                "SELECT c.name, e.kind, s.manifest, e.payload FROM evidence e "
                "JOIN captures c ON c.id=e.capture_id JOIN capture_sources s ON s.id=e.source_id "
                f"WHERE c.deployment_id IN ({placeholders}) ORDER BY c.name, e.kind",
                deployments,
            ).fetchall()

        return [
            dict(
                capture=capture,
                kind=kind,
                manifest=manifest,
                payload=dict(json.loads(payload), source_manifest=manifest),
            )
            for capture, kind, manifest, payload in rows
            if capture not in excluded_captures
        ]

    def observations(
        self, deployment: dict[str, Any], *, operations: tuple[str, ...], excluded_captures: set[str]
    ) -> list[OperationSamples]:
        """Read exact work-point distributions without expanding observations or positions."""

        placeholders = ",".join("?" for _ in operations)
        with closing(sqlite3.connect(f"file:{self.path.resolve()}?mode=ro", uri=True)) as connection:
            deployments = _deployment_ids(connection, deployment)
            domains = ",".join("?" for _ in deployments)
            stored = connection.execute(
                "SELECT c.name, s.manifest, w.operation, w.implementation, "
                "w.features, w.timing_scope, o.distribution FROM observations o "
                "JOIN captures c ON c.id=o.capture_id JOIN capture_sources s ON s.id=o.source_id "
                "JOIN work_points w ON w.id=o.work_id "
                f"WHERE c.deployment_id IN ({domains}) AND w.operation IN ({placeholders}) "
                "ORDER BY w.operation, w.implementation, w.timing_scope, c.name, o.source_id, o.work_id",
                (*deployments, *operations),
            )
            return [
                OperationSamples(
                    capture=capture,
                    manifest=manifest,
                    operation=operation,
                    implementation=implementation,
                    features=json.loads(features),
                    timing_scope=scope,
                    durations=dict(unpack_samples(distribution)),
                )
                for capture, manifest, operation, implementation, features, scope, distribution in stored
                if capture not in excluded_captures
            ]

    def measurement_count(self, deployment: dict[str, Any], *, excluded_captures: set[str]) -> int:
        """Count deployment candidates without loading their measured features."""

        with closing(sqlite3.connect(f"file:{self.path.resolve()}?mode=ro", uri=True)) as connection:
            deployments = _deployment_ids(connection, deployment)
            placeholders = ",".join("?" for _ in deployments)
            counts = connection.execute(
                "SELECT c.name, SUM(o.count) FROM observations o JOIN captures c ON c.id=o.capture_id "
                f"WHERE c.deployment_id IN ({placeholders}) GROUP BY c.name",
                deployments,
            )
            return sum(count for capture, count in counts if capture not in excluded_captures)

    def summary(self) -> dict[str, Any]:
        """Report stored deployment/operator coverage without creating a database."""

        with closing(sqlite3.connect(f"file:{self.path.resolve()}?mode=ro", uri=True)) as connection:
            environments = dict(connection.execute("SELECT id, description FROM deployments"))
            groups = connection.execute(
                "SELECT c.deployment_id, w.operation, w.implementation, w.timing_scope, SUM(o.count), COUNT(DISTINCT o.capture_id) "
                "FROM observations o JOIN captures c ON c.id=o.capture_id JOIN work_points w ON w.id=o.work_id "
                "GROUP BY c.deployment_id, w.operation, w.implementation, w.timing_scope"
            ).fetchall()
            groups.sort(key=lambda row: (environments[row[0]], *row[1:4]))

            # Distinct work descriptions suffice for geometry coverage; do not
            # load every repeated layer measurement or infer identifiability
            # from a large raw sample count. Sort by the deployment's integer
            # identity, not its potentially large JSON description per record.
            coverage: dict[tuple[int, str, str, str], dict[str, Any]] = {}
            for environment, operation, implementation, scope, raw_features, count in connection.execute(
                "SELECT c.deployment_id, w.operation, w.implementation, w.timing_scope, w.features, SUM(o.count) "
                "FROM observations o JOIN captures c ON c.id=o.capture_id JOIN work_points w ON w.id=o.work_id "
                "GROUP BY c.deployment_id, w.operation, w.implementation, w.timing_scope, w.features"
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
                    "deployment": json.loads(environments[environment]),
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
