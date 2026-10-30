"""Import independent facts and export selected offline responses; never collect or predict."""

from dataclasses import asdict
from itertools import groupby
import json
import math
from pathlib import Path
from statistics import median

from markov.artifacts.performance import PerformanceData, encoded
from markov.artifacts.request import Deployment, read_json, reject_unknown, resolve_path
from markov.artifacts.writing import write_json
from markov.adapters.sglang import cache_measurements
from markov.adapters.ascend import tensor_device_measurements, tensor_measurements


ADAPTERS = {
    "sglang_cache_io": cache_measurements,
    "ascend_tensor": tensor_measurements,
    "ascend_tensor_device": tensor_device_measurements,
}


def affine(points, feature):
    x = [features[feature] for features, _ in points]
    y = [duration for _, duration in points]
    center_x, center_y = sum(x) / len(x), sum(y) / len(y)
    spread = sum((value - center_x) ** 2 for value in x)
    if spread == 0:
        raise ValueError(f"Linear response needs at least two distinct measured {feature} values")
    coefficient = sum((a - center_x) * (b - center_y) for a, b in zip(x, y)) / spread
    intercept = center_y - coefficient * center_x
    if not all(math.isfinite(value) for value in (intercept, coefficient)):
        raise ValueError("Training produced a nonfinite response")
    if min(intercept + coefficient * value for value in (min(x), max(x))) < 0:
        raise ValueError("Training produced a negative duration inside its measured domain")
    return {"intercept_ns": intercept, "coefficients_ns": {feature: coefficient}}


def train_response(kind, recipe, samples, sources):
    points = []
    for features, rows in groupby(sorted(samples, key=lambda row: encoded(row[0])), key=lambda row: encoded(row[0])):
        points.append((json.loads(features), median(duration for _, duration in rows)))
    if not points:
        raise ValueError(f"No non-warmup samples for {kind[0]}")
    names = set(points[0][0])
    if any(set(features) != names for features, _ in points):
        raise ValueError(f"Inconsistent work variables for {kind[0]}")
    feature, curve = recipe["feature"], recipe.get("curve")
    for name in names - {feature, curve}:
        if len({features[name] for features, _ in points}) != 1:
            raise ValueError(f"Response needs a different model form for varying {name}: {kind[0]}")

    if curve is None:
        response = {"form": "affine", **affine(points, feature)}
    else:
        response = {"form": "coefficient_curve", "feature": curve, "points": []}
        for coordinate in sorted({features[curve] for features, _ in points}):
            selected = [(features, duration) for features, duration in points if features[curve] == coordinate]
            response["points"].append({"coordinate": coordinate, **affine(selected, feature)})
        if len(response["points"]) < 2:
            raise ValueError(f"Coefficient interpolation needs at least two measured {curve} coordinates")
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
    selected = (
        read_json(resolve_path(directory, raw["models"]))
        if "models" in raw
        else {"deployment": deployment, "models": []}
    )
    if selected["deployment"] != deployment:
        raise ValueError("Selected models belong to a different deployment")

    database.parent.mkdir(parents=True, exist_ok=True)
    store = PerformanceData(database)
    imported = []
    try:
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
    finally:
        store.close()

    # Only explicitly selected responses are replaced; prediction reads this file, not SQLite.
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
