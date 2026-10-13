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
from .control import control_entries


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
            position TEXT NOT NULL, operation TEXT NOT NULL, implementation TEXT NOT NULL,
            features TEXT NOT NULL, duration_us REAL NOT NULL, timing_scope TEXT NOT NULL,
            PRIMARY KEY (capture_id, position, timing_scope)
        ) WITHOUT ROWID;
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
        observations: list[OperationObservation],
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
            connection.execute(
                "CREATE INDEX IF NOT EXISTS observations_work ON observations "
                "(operation, implementation, timing_scope, capture_id)"
            )
            with connection:
                return self._import(connection, observations, evidence, replace_captures=replace_captures)

    def _import(
        self,
        connection: sqlite3.Connection,
        observations: list[OperationObservation],
        evidence: list[dict[str, Any]],
        *,
        replace_captures: bool,
    ) -> dict[str, Any]:
        """Apply one admitted import; the caller owns its transaction."""

        evidence_counts = dict(inserted=0, duplicate_imports=0)
        observation_counts = dict(inserted=0, duplicate_imports=0)
        replaced = (
            sorted({row.capture for row in observations} | {row["capture"] for row in evidence})
            if replace_captures
            else []
        )
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
                description = _json(deployment)
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

        if replaced:
            for capture in replaced:
                if capture not in captures:
                    continue
                for table in ("observations", "evidence"):
                    removed[table] += connection.execute(
                        f"DELETE FROM {table} WHERE capture_id=?", (captures[capture][0],)
                    ).rowcount

        for row in evidence:
            identity, source_id = admit_source(row["capture"], row["manifest"], row["deployment"], True)
            for manifest in evidence_manifests(row):
                admit_source(row["capture"], manifest, row["deployment"], False)

            payload = aggregate_payload(row["kind"], row["payload"])
            if payload is None:
                continue

            encoded = _json(payload)
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
            stored = connection.execute(
                "SELECT operation, implementation, features, duration_us FROM observations "
                "WHERE capture_id=? AND position=? AND timing_scope=?",
                (identity, row.position, row.timing_scope),
            ).fetchone()
            if stored is not None:
                # Aliases and numeric JSON spelling do not change a work point.
                if (
                    stored[:2] != (row.operation, row.implementation)
                    or json.loads(stored[2]) != row.features
                    or stored[3] != row.duration_us
                ):
                    raise ValueError(f"conflicting cost reimport: {row.capture}/{row.position}")
                observation_counts["duplicate_imports"] += 1
                continue

            connection.execute(
                "INSERT INTO observations VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                (
                    identity,
                    source_id,
                    row.position,
                    row.operation,
                    row.implementation,
                    _json(row.features),
                    row.duration_us,
                    row.timing_scope,
                ),
            )
            observation_counts["inserted"] += 1

        return {
            "operators": observation_counts,
            "evidence": evidence_counts,
            "replaced_captures": replaced,
            "removed_derived_rows": removed,
            "retrain_exported_packages": bool(replaced),
        }

    def evidence(self, deployment: dict[str, Any], *, excluded_captures: set[str]) -> list[dict[str, Any]]:
        """Select aggregate I/O facts, not whole operator export reports."""

        with closing(sqlite3.connect(f"file:{self.path.resolve()}?mode=ro", uri=True)) as connection:
            rows = connection.execute(
                "SELECT c.name, e.kind, s.manifest, e.payload FROM evidence e "
                "JOIN captures c ON c.id=e.capture_id JOIN capture_sources s ON s.id=e.source_id "
                "JOIN deployments d ON d.id=c.deployment_id WHERE d.description=? ORDER BY c.name, e.kind",
                (_json(deployment),),
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
    ) -> list[OperationObservation]:
        """Read one operation family, omitting holdouts; metadata is read-only.

        Other timing scopes remain available to classify missing measurements.
        Deployment and provenance descriptions are joined, not stored per row.
        """

        environment_json = _json(deployment)
        environment = json.loads(environment_json)
        placeholders = ",".join("?" for _ in operations)
        with closing(sqlite3.connect(f"file:{self.path.resolve()}?mode=ro", uri=True)) as connection:
            stored = connection.execute(
                "SELECT c.name, o.position, s.manifest, o.operation, o.implementation, "
                "o.features, o.duration_us, o.timing_scope FROM observations o "
                "JOIN captures c ON c.id=o.capture_id JOIN capture_sources s ON s.id=o.source_id "
                "JOIN deployments d ON d.id=c.deployment_id WHERE d.description=? "
                f"AND o.operation IN ({placeholders}) "
                "ORDER BY o.operation, o.implementation, o.timing_scope, c.name, o.position",
                (environment_json, *operations),
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
                if capture not in excluded_captures
            ]

    def measurement_count(self, deployment: dict[str, Any], *, excluded_captures: set[str]) -> int:
        """Count deployment candidates without loading their measured features."""

        with closing(sqlite3.connect(f"file:{self.path.resolve()}?mode=ro", uri=True)) as connection:
            counts = connection.execute(
                "SELECT c.name, COUNT(*) FROM observations o JOIN captures c ON c.id=o.capture_id "
                "JOIN deployments d ON d.id=c.deployment_id WHERE d.description=? GROUP BY c.name",
                (_json(deployment),),
            )
            return sum(count for capture, count in counts if capture not in excluded_captures)

    def summary(self) -> dict[str, Any]:
        """Report stored deployment/operator coverage without creating a database."""

        with closing(sqlite3.connect(f"file:{self.path.resolve()}?mode=ro", uri=True)) as connection:
            groups = connection.execute(
                "SELECT d.description, o.operation, o.implementation, o.timing_scope, COUNT(*), COUNT(DISTINCT o.capture_id) "
                "FROM observations o JOIN captures c ON c.id=o.capture_id JOIN deployments d ON d.id=c.deployment_id "
                "GROUP BY d.description, o.operation, o.implementation, o.timing_scope "
                "ORDER BY d.description, o.operation, o.implementation, o.timing_scope"
            ).fetchall()

            # Distinct work descriptions suffice for geometry coverage; do not
            # load every repeated layer measurement or infer identifiability
            # from a large raw sample count.
            coverage: dict[tuple[str, str, str, str], dict[str, Any]] = {}
            for environment, operation, implementation, scope, raw_features, count in connection.execute(
                "SELECT d.description, o.operation, o.implementation, o.timing_scope, o.features, COUNT(*) "
                "FROM observations o JOIN captures c ON c.id=o.capture_id JOIN deployments d ON d.id=c.deployment_id "
                "GROUP BY d.description, o.operation, o.implementation, o.timing_scope, o.features"
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
