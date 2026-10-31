"""Share package work coordinates and per-model measurement lineage."""

from __future__ import annotations

from typing import Any


def compact_package(package: dict[str, Any]) -> dict[str, Any]:
    """Consume an expanded training result without changing responses or coverage.

    This is also the explicit conversion for historical packages. Prediction
    reads only the shared layout; it never expands an older package implicitly.
    """

    if "work_point_sets" in package or "source_sets" in package:
        raise ValueError("The cost package already uses shared tables; use it directly")

    work_sets: list[dict[str, Any]] = []
    source_sets: list[dict[str, Any]] = []
    work_indices: dict[tuple, int] = {}
    source_indices: dict[tuple, int] = {}
    for collection in ("operator_models", "missing_models", "wildcard_unmeasured_implementations"):
        for model in package.get(collection, []):
            if "measured_feature_points" in model:
                features = tuple(sorted(model["feature_ranges"]))
                points = model.pop("measured_feature_points")
                if not points or any(set(point) != set(features) for point in points):
                    raise ValueError("Measured work points must cover every declared coordinate")

                coordinates = tuple(tuple(point[name] for name in features) for point in points)
                key = features, coordinates
                if key not in work_indices:
                    work_indices[key] = len(work_sets)
                    work_sets.append({"features": list(features), "points": [list(row) for row in coordinates]})
                model["work_point_set"] = work_indices[key]

            source_fields = ("source_captures", "source_manifests")
            if any(field in model for field in source_fields):
                key = tuple(tuple(model[field]) if field in model else None for field in source_fields)
                if key not in source_indices:
                    source_indices[key] = len(source_sets)
                    source_sets.append({field: model[field] for field in source_fields if field in model})
                for field in source_fields:
                    model.pop(field, None)
                model["source_set"] = source_indices[key]

    package["work_point_sets"] = work_sets
    package["source_sets"] = source_sets
    return package


def validate_package_references(package: dict[str, Any]) -> None:
    """Check shared references once when an external package enters Python."""

    if "work_point_sets" not in package or "source_sets" not in package:
        raise ValueError("Cost package needs shared work/source tables; convert it with cost-db migrate --package")
    for collection in ("operator_models", "missing_models", "wildcard_unmeasured_implementations"):
        for model in package.get(collection, []):
            for field, table in (("work_point_set", "work_point_sets"), ("source_set", "source_sets")):
                if collection != "operator_models" and field not in model:
                    continue
                reference = model[field]
                if type(reference) is not int or not 0 <= reference < len(package[table]):
                    raise ValueError(f"Invalid cost package {field} reference")
