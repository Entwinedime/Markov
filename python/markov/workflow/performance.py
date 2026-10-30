"""Import independent facts and export selected offline responses; never collect or predict."""

from __future__ import annotations

from dataclasses import asdict
from itertools import combinations, groupby
import json
import math
from pathlib import Path
from statistics import median

from markov.artifacts.performance import PerformanceData, encoded
from markov.artifacts.request import Deployment, read_json, reject_unknown, resolve_path
from markov.artifacts.writing import write_json
from markov.adapters.sglang import cache_measurements, region_measurements
from markov.adapters.ascend import tensor_device_measurements, tensor_measurements, tensor_queue_measurements


ADAPTERS = {
    "sglang_cache_io": cache_measurements,
    "sglang_regions": region_measurements,
    "ascend_tensor": tensor_measurements,
    "ascend_tensor_device": tensor_device_measurements,
    "ascend_tensor_queue": tensor_queue_measurements,
}


Points = list[tuple[dict[str, float], float]]


def select_models(
    deployment: Deployment, models: Path | None, database: Path | None, output: Path, allow_empty: bool = False
) -> Path:
    """Export the maintained selection; prediction neither fits nor opens SQLite."""
    if models is not None:
        models.stat()
        return models
    if database is None:
        raise ValueError("Prediction needs performance.models or a maintained performance.database")
    if allow_empty:
        database.parent.mkdir(parents=True, exist_ok=True)
    else:
        database.stat()
    store = PerformanceData(database)
    try:
        selected = store.selected_models(asdict(deployment))
    finally:
        store.close()
    if selected is None and not allow_empty:
        raise ValueError("The performance database has no maintained models for this deployment")
    if selected is None:
        selected = {"deployment": asdict(deployment), "models": []}

    output.mkdir(parents=True, exist_ok=True)
    destination = output / "selected_models.json"
    if destination == database:
        raise ValueError("Selected model output would replace the performance database")
    write_json(destination, selected)
    return destination


def least_squares(columns: list[list[float]], target: list[float]) -> list[float] | None:
    """Scaled QR; a dependent feature set has no unique least-squares response."""
    count = len(columns)
    q, scales = [], []
    upper = [[0.0] * count for _ in columns]
    for index, column in enumerate(columns):
        scale = math.sqrt(sum(value * value for value in column))
        if scale == 0:
            return None
        scales.append(scale)
        values = [value / scale for value in column]
        for before, basis in enumerate(q):
            upper[before][index] = sum(a * b for a, b in zip(basis, values))
            values = [value - upper[before][index] * axis for value, axis in zip(values, basis)]
        upper[index][index] = math.sqrt(sum(value * value for value in values))
        if upper[index][index] <= math.ulp(1.0) * len(target):
            return None
        q.append([value / upper[index][index] for value in values])

    projected = [sum(axis * value for axis, value in zip(basis, target)) for basis in q]
    fitted = [0.0] * count
    for index in reversed(range(count)):
        fitted[index] = (
            projected[index] - sum(upper[index][after] * fitted[after] for after in range(index + 1, count))
        ) / upper[index][index]
    return [value / scale for value, scale in zip(fitted, scales)]


def affine(points: Points, feature: str | list[str] | None) -> dict:
    if feature is None:
        return {"intercept_ns": median(duration for _, duration in points), "coefficients_ns": {}}

    names = feature if isinstance(feature, list) else [feature]
    columns = [[work[name] for work, _ in points] for name in names]
    target = [duration for _, duration in points]
    centers = [sum(column) / len(column) for column in columns]
    center_y = sum(target) / len(target)
    centered = [[value - center for value in column] for column, center in zip(columns, centers)]
    coefficients = least_squares(centered, [value - center_y for value in target])
    if coefficients is None:
        raise ValueError(f"Measured feature columns are constant or linearly dependent: {names}")
    intercept = center_y - sum(value * center for value, center in zip(coefficients, centers))
    if not all(math.isfinite(value) for value in (intercept, *coefficients)):
        raise ValueError("Training produced a nonfinite response")
    if any(
        intercept + sum(value * features[name] for name, value in zip(names, coefficients)) < 0
        for features, _ in points
    ):
        raise ValueError("Training produced a negative duration at a measured work point")
    return {"intercept_ns": intercept, "coefficients_ns": dict(zip(names, coefficients))}


def nonnegative_affine(points: Points, features: str | list[str] | None) -> dict:
    """Enumerate active sets for the few nonnegative additive service terms."""
    if features is None:
        return affine(points, None)

    names = features if isinstance(features, list) else [features]
    columns = [[1.0] * len(points)] + [[work[name] for work, _ in points] for name in names]
    target = [duration for _, duration in points]
    mean = sum(target) / len(target)
    best_error, best = sum((value - mean) ** 2 for value in target), {0: mean}
    for count in range(1, len(columns) + 1):
        for active in combinations(range(len(columns)), count):
            fitted = least_squares([columns[index] for index in active], target)
            if fitted is None or any(value < 0 for value in fitted):
                continue
            error = sum(
                (duration - sum(value * columns[column][row] for value, column in zip(fitted, active))) ** 2
                for row, duration in enumerate(target)
            )
            if error < best_error:
                best_error = error
                best = dict(zip(active, fitted))
    return {
        "intercept_ns": best.get(0, 0.0),
        "coefficients_ns": {name: best.get(index + 1, 0.0) for index, name in enumerate(names)},
    }


def train_response(kind: tuple[str, str, str], recipe: dict, samples: Points, sources: list[str]) -> dict:
    points = []
    for features, rows in groupby(sorted(samples, key=lambda row: encoded(row[0])), key=lambda row: encoded(row[0])):
        points.append((json.loads(features), median(duration for _, duration in rows)))
    if not points:
        raise ValueError(f"No non-warmup samples for {kind[0]}")
    names = set(points[0][0])
    if any(set(features) != names for features, _ in points):
        raise ValueError(f"Inconsistent work variables for {kind[0]}")
    feature, curve = recipe["feature"], recipe.get("curve")
    predictors = set(feature if isinstance(feature, list) else [feature]) - {None}
    coverage = set(recipe.get("coverage_features", []))
    if not coverage <= names:
        raise ValueError(f"Unknown coverage coordinates for {kind[0]}")
    for name in names - predictors - {curve} - coverage:
        if len({features[name] for features, _ in points}) != 1:
            raise ValueError(f"Response needs a different model form for varying {name}: {kind[0]}")

    fit = nonnegative_affine if recipe.get("nonnegative", False) else affine
    if curve is None:
        response = {"form": "affine", **fit(points, feature)}
    else:
        response = {"form": "coefficient_curve", "feature": curve, "points": []}
        for coordinate in sorted({features[curve] for features, _ in points}):
            selected = [(features, duration) for features, duration in points if features[curve] == coordinate]
            response["points"].append({"coordinate": coordinate, **fit(selected, feature)})
    return {
        "operation": kind[0],
        "implementation": kind[1],
        "timing_scope": kind[2],
        "ranges": {name: [min(p[name] for p, _ in points), max(p[name] for p, _ in points)] for name in sorted(names)},
        "measured_points": [features for features, _ in points],
        "allow_new_combinations": True,
        "extrapolation_features": recipe.get("extrapolation_features", []),
        "sources": sources,
        "limitations": recipe["limitations"],
        "response": response,
    }


def maintain_performance(path: Path):
    raw = read_json(path)
    reject_unknown(
        raw, {"database", "deployment", "imports", "training", "models", "output"}, "performance maintenance"
    )
    directory = path.resolve().parent
    database = resolve_path(directory, raw["database"])
    deployment = asdict(Deployment.parse(raw["deployment"]))
    training = [resolve_path(directory, item) for item in raw.get("training", [])]
    if len(set(training)) != len(training):
        raise ValueError("A training capture can only be selected once")
    if not training and ("output" in raw or "models" in raw):
        raise ValueError("Model export requires an explicit training capture selection")
    if not training and not raw.get("imports"):
        raise ValueError("Performance maintenance needs imports or an explicit training selection")
    output = resolve_path(directory, raw["output"]) if training else None
    imports = [resolve_path(directory, item["capture"]) for item in raw.get("imports", [])]
    if output is not None and (
        output in (database, path.resolve())
        or any(output.is_relative_to(source.parent) for source in training + imports)
    ):
        raise ValueError("Model output must be separate from the database, request and captured source directories")
    database.parent.mkdir(parents=True, exist_ok=True)
    store = PerformanceData(database)
    imported = []
    try:
        selected = (
            read_json(resolve_path(directory, raw["models"]))
            if "models" in raw
            else store.selected_models(deployment) or {"deployment": deployment, "models": []}
        )
        if selected["deployment"] != deployment:
            raise ValueError("Selected models belong to a different deployment")

        for item in raw.get("imports", []):
            reject_unknown(item, {"adapter", "capture"}, "measurement import")
            adapter = ADAPTERS[item["adapter"]]
            source = resolve_path(directory, item["capture"])
            metadata = adapter.metadata(source)
            imported.append(
                store.import_capture(
                    source,
                    item["adapter"],
                    adapter.context(metadata, deployment),
                    metadata,
                    adapter.measurements(source, metadata),
                )
            )

        grouped, recipes, sources = {}, {}, {}
        conditions = {}
        sample_count = 0
        for source in training:
            adapter_name, current, metadata, samples = store.samples(source)
            if current != ADAPTERS[adapter_name].context(metadata, deployment):
                raise ValueError(f"Training capture belongs to different measurement conditions: {source}")
            recipes.update(ADAPTERS[adapter_name].recipes(metadata))
            for operation, implementation, scope, features, duration in samples:
                kind = (operation, implementation, scope)
                if kind in conditions and conditions[kind] != current:
                    raise ValueError(f"Training selection mixes measurement conditions for {operation}/{scope}")
                conditions[kind] = current
                grouped.setdefault(kind, []).append((features, duration))
                sources.setdefault(kind, set()).add(str(source))
            sample_count += len(samples)
        trained = [
            train_response(kind, recipe, grouped.get(kind, []), sorted(sources.get(kind, set())))
            for kind, recipe in recipes.items()
        ]
        # Publish only successfully trained responses. SQLite keeps the current
        # selection; the small export is the predictor's isolated input.
        by_kind = {}
        for model in selected["models"]:
            kind = tuple(model[key] for key in ("operation", "implementation", "timing_scope"))
            if kind in by_kind:
                raise ValueError(f"Duplicate selected response: {kind}")
            by_kind[kind] = model
        for model in trained:
            by_kind[tuple(model[key] for key in ("operation", "implementation", "timing_scope"))] = model
        if output is not None:
            output.parent.mkdir(parents=True, exist_ok=True)
            write_json(output, {"deployment": deployment, "models": list(by_kind.values())})
            store.publish_models(deployment, list(by_kind.values()))
    finally:
        store.close()
    return {
        "status": "completed",
        "database": str(database),
        "imports": imported,
        "training_captures": len(training),
        "training_samples": sample_count,
        "trained_models": len(trained),
        "selected_models": len(by_kind),
        "models": str(output) if output else None,
        "qualification": "Offline fit from explicitly selected independent captures; not held-out or E2E accuracy evidence",
    }
