"""Interpretable operator responses, independent of bases and target identities."""

from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass, replace
import math
from statistics import median
from typing import Any

from .database import OperationObservation


@dataclass(frozen=True)
class ModelSpec:
    """Declare regression variables and optional measured-domain coordinates.

    Coverage-only variables constrain application without adding coefficients.
    Neither set is discovered by searching target errors.
    Named extrapolation coordinates relax only their ranges, not the remaining
    geometry. An empty tuple permits none unless allow_extrapolation is true.
    observation_operations explicitly pools equivalent backend measurements
    labeled by different callers; empty means this operation only. It never
    relaxes deployment, implementation, timing or geometry requirements.
    """

    operation: str
    implementation: str
    timing_scope: str
    features: tuple[str, ...]
    form: str = "linear"
    allow_extrapolation: bool = False
    fit_intercept: bool = True
    coverage_features: tuple[str, ...] = ()
    observation_operations: tuple[str, ...] = ()
    extrapolation_features: tuple[str, ...] = ()

    @property
    def source_operations(self) -> tuple[str, ...]:
        return self.observation_operations or (self.operation,)

    @property
    def required_features(self) -> tuple[str, ...]:
        return tuple(dict.fromkeys((*self.features, *self.coverage_features)))

    @classmethod
    def from_raw(cls, raw: dict[str, Any]) -> ModelSpec:
        values = dict(raw)
        values["features"] = tuple(values["features"])
        values["coverage_features"] = tuple(values.get("coverage_features", ()))
        values["observation_operations"] = tuple(dict.fromkeys(values.get("observation_operations", ())))
        values["extrapolation_features"] = tuple(values.get("extrapolation_features", ()))
        spec = cls(**values)
        if spec.form not in {"constant", "linear", "piecewise_linear"}:
            raise ValueError(f"unsupported cost model form: {spec.form}")
        if spec.form == "constant" and spec.features:
            raise ValueError("constant models do not have variable features")
        if spec.form == "piecewise_linear" and len(spec.features) != 1:
            raise ValueError("piecewise-linear models need one measured coordinate")
        if spec.form == "piecewise_linear" and (spec.allow_extrapolation or spec.extrapolation_features):
            raise ValueError("piecewise-linear models do not extrapolate outside measured anchors")
        if set(spec.extrapolation_features) - set((*spec.features, *spec.coverage_features)):
            raise ValueError("extrapolation features must be declared regression or coverage coordinates")
        if not spec.fit_intercept and (spec.form != "linear" or not spec.features):
            raise ValueError("zero-intercept fitting requires a linear model with variable work")

        return spec


def parse_model_specs(declarations: list[dict[str, Any]]) -> list[ModelSpec]:
    """Expand explicit operation/scope lists at the training-input boundary.

    A name is shorthand for a singleton list. Each operation/scope pair keeps
    its own samples and coefficients; only the declared fitting policy is
    shared. Implementation wildcards and explicit observation pooling retain
    their existing meanings. An empty selector is not a successful no-op.
    """

    specifications = []
    for declaration in declarations:
        operation = declaration["operation"]
        scope = declaration["timing_scope"]
        operations = [operation] if isinstance(operation, str) else operation
        scopes = [scope] if isinstance(scope, str) else scope
        if not operations or not scopes:
            raise ValueError("model operation and timing_scope selectors must not be empty")

        specification = ModelSpec.from_raw(dict(declaration, operation=operations[0], timing_scope=scopes[0]))
        specifications.extend(
            replace(specification, operation=name, timing_scope=timing_scope)
            for name in operations
            for timing_scope in scopes
        )

    return specifications


def train_models(
    specifications: list[ModelSpec], observations: list[OperationObservation]
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """Expand declared implementations and fit them from admitted measurements.

    Wildcards select only implementations measured in the requested scope.
    Return fitted/missing responses and separately report other implementations;
    neither outcome changes work generation or starts acquisition.
    """

    observations_by_operation: dict[str, list[OperationObservation]] = defaultdict(list)
    for row in observations:
        observations_by_operation[row.operation].append(row)

    models = []
    unmeasured_implementations = []
    measurement_pools: dict[tuple[str, ...], dict[str, list[OperationObservation]]] = {}
    for spec in specifications:
        if spec.source_operations not in measurement_pools:
            by_implementation: dict[str, list[OperationObservation]] = defaultdict(list)
            for operation in spec.source_operations:
                for row in observations_by_operation[operation]:
                    by_implementation[row.implementation].append(row)
            measurement_pools[spec.source_operations] = by_implementation

        implementation_rows = measurement_pools[spec.source_operations]

        if spec.implementation != "*":
            models.append(_train_model(spec, implementation_rows.get(spec.implementation, [])))
            continue

        scopes = {name: {row.timing_scope for row in rows} for name, rows in implementation_rows.items()}
        implementations = sorted(name for name, available in scopes.items() if spec.timing_scope in available)
        for implementation in sorted(scopes.keys() - set(implementations)):
            unmeasured_implementations.append(
                {
                    "operation": spec.operation,
                    "implementation": implementation,
                    "timing_scope": spec.timing_scope,
                    "reason": "missing_timing_scope",
                    "available_timing_scopes": sorted(scopes[implementation]),
                }
            )
        # A direct call need not have queued-worker measurements. Wildcards
        # select implementations measured in this scope, not every backend
        # ever seen under the same operation name.
        models.extend(
            _train_model(replace(spec, implementation=name), implementation_rows[name]) for name in implementations
        )
        if not implementations:
            models.append(
                {
                    "operation": spec.operation,
                    "implementation": "*",
                    "timing_scope": spec.timing_scope,
                    "status": "missing_data",
                    "reason": "missing_timing_scope" if scopes else "missing_operation",
                    "available_timing_scopes": sorted({scope for available in scopes.values() for scope in available}),
                    "features": list(spec.required_features),
                    "ineligible_measurements": 0,
                }
            )
    return models, unmeasured_implementations


def _solve(matrix: list[list[float]], response: list[float]) -> list[float] | None:
    """Pivoted elimination on normalized features; singularity means missing work."""

    size = len(response)
    rows = [row[:] + [response[index]] for index, row in enumerate(matrix)]
    for column in range(size):
        pivot = max(range(column, size), key=lambda index: abs(rows[index][column]))
        if abs(rows[pivot][column]) <= 1e-10:
            return None

        rows[column], rows[pivot] = rows[pivot], rows[column]
        divisor = rows[column][column]
        rows[column] = [value / divisor for value in rows[column]]
        for index in range(size):
            if index == column:
                continue
            factor = rows[index][column]
            rows[index] = [left - factor * right for left, right in zip(rows[index], rows[column])]

    return [row[-1] for row in rows]


def _response(model: dict[str, Any], features: dict[str, float]) -> float:
    """Evaluate a declared response in microseconds, before rounding or domain checks."""

    if model["form"] != "piecewise_linear":
        return model["intercept_us"] + math.fsum(
            value * features[name] for name, value in model["coefficients"].items()
        )

    coordinate = features[model["coordinate"]]
    points = model["points"]
    left, right = points[0], points[-1]
    for candidate_left, candidate_right in zip(points, points[1:]):
        if candidate_left[0] <= coordinate <= candidate_right[0]:
            left, right = candidate_left, candidate_right
            break

    return left[1] + (coordinate - left[0]) * (right[1] - left[1]) / (right[0] - left[0])


def _with_training_fit(
    model: dict[str, Any], measurements: list[tuple[tuple[float, ...], float]], variables: tuple[str, ...]
) -> dict[str, Any]:
    """Describe fit on capture/work-point medians, not held-out prediction accuracy."""

    predictions = [_response(model, dict(zip(variables, point))) for point, _ in measurements]
    errors = [abs(predicted - observed) for predicted, (_, observed) in zip(predictions, measurements)]
    observed_total = math.fsum(observed for _, observed in measurements)
    parameters = (
        len(model["points"])
        if model["form"] == "piecewise_linear"
        else len(model["coefficients"]) + int(model.get("fit_intercept", True))
    )
    model["training_fit"] = {
        "work_point_observations": len(measurements),
        "fitted_parameters": parameters,
        "residual_degrees_of_freedom": len(measurements) - parameters,
        "mean_absolute_error_us": math.fsum(errors) / len(errors),
        "max_absolute_error_us": max(errors),
        "wape": math.fsum(errors) / observed_total if observed_total else None,
        "negative_predicted_points": sum(value < 0 for value in predictions),
        "scope": "In-sample capture/work-point medians, before rounding; not held-out validation, E2E accuracy or a confidence interval.",
    }
    return model


def _train_model(spec: ModelSpec, operation_rows: list[OperationObservation]) -> dict[str, Any]:
    """Fit one concrete implementation selected by train_models, preserving gaps."""

    source_operations = spec.source_operations
    candidates = [row for row in operation_rows if row.timing_scope == spec.timing_scope]
    required = spec.required_features
    rows = [row for row in candidates if all(name in row.features for name in required)]
    missing_features = sorted({name for row in candidates for name in required if name not in row.features})
    identity = {"operation": spec.operation, "implementation": spec.implementation, "timing_scope": spec.timing_scope}
    if not rows:
        reason = "missing_operation"
        if operation_rows:
            reason = "missing_timing_scope"
        if candidates:
            reason = "missing_features"
        return {
            **identity,
            "status": "missing_data",
            "reason": reason,
            "available_timing_scopes": sorted({row.timing_scope for row in operation_rows}),
            "features": missing_features or list(required),
            "ineligible_measurements": len(candidates),
        }

    # Collapse repeated feature points within each capture first. A profiler's
    # repeated layers must not outweigh independent runs with fewer operations.
    grouped: dict[tuple[str, tuple[float, ...]], list[float]] = defaultdict(list)
    grouped_work: dict[tuple[str, tuple[float, ...]], set[tuple[tuple[str, float], ...]]] = defaultdict(set)
    for row in rows:
        key = row.capture, tuple(row.features[name] for name in spec.features)
        grouped[key].append(row.duration_us)
        grouped_work[key].add(tuple(sorted(row.features.items())))
    measurements = [(point, median(values)) for (_, point), values in sorted(grouped.items())]
    feature_points = sorted({point for point, _ in measurements})
    # Keep measured-domain coordinates separate from fitting coordinates:
    # adding a coverage condition must not silently add a coefficient or
    # reweight repeated measurements within a capture.
    coverage_points = sorted({tuple(row.features[name] for name in required) for row in rows})
    ranges = {
        name: [min(row.features[name] for row in rows), max(row.features[name] for row in rows)] for name in required
    }
    result: dict[str, Any] = {
        **identity,
        "status": "ready",
        "form": spec.form,
        "observation_operations": list(source_operations),
        "regression_features": list(spec.features),
        "coverage_features": list(spec.coverage_features),
        "feature_ranges": ranges,
        "allow_extrapolation": spec.allow_extrapolation,
        "extrapolation_features": list(spec.extrapolation_features),
        "source_captures": sorted({row.capture for row in rows}),
        "source_manifests": sorted({row.manifest for row in rows}),
        "measurement_count": len(rows),
        "training_points": len(measurements),
        "measured_feature_points": [dict(zip(required, point)) for point in coverage_points],
        "ineligible_measurements": len(candidates) - len(rows),
        "observed_duration_range_us": [min(row.duration_us for row in rows), max(row.duration_us for row in rows)],
        "max_within_capture_spread_us": max(max(values) - min(values) for values in grouped.values()),
        "collapsed_distinct_work_groups": sum(len(points) > 1 for points in grouped_work.values()),
        "omitted_observed_features": sorted({name for row in rows for name in row.features} - set(spec.features)),
        "unchecked_observed_features": sorted({name for row in rows for name in row.features} - set(required)),
        "coverage_scope": "Declared regression and coverage coordinates only; other or unobserved geometry is not proven covered. Coordinate ranges do not establish joint coverage.",
        "variability_note": "Observed service spread, not a confidence interval or an E2E uncertainty bound.",
        "duration_unit": "us",
    }
    if spec.form == "constant":
        result.update(intercept_us=median(value for _, value in measurements), coefficients={})
        return _with_training_fit(result, measurements, spec.features)

    if spec.form == "piecewise_linear":
        points: dict[float, list[float]] = defaultdict(list)
        for point, value in measurements:
            points[point[0]].append(value)
        if len(points) < 2:
            return {**identity, "status": "missing_data", "reason": "missing_distinct_anchors"}

        result["coordinate"] = spec.features[0]
        result["points"] = [[point, median(values)] for point, values in sorted(points.items())]
        return _with_training_fit(result, measurements, spec.features)

    scales = [max(abs(point[index]) for point, _ in measurements) or 1.0 for index in range(len(spec.features))]
    # Additive work may have a known zero origin: no operations means no
    # service. An explicit request can enforce that instead of learning an
    # arbitrary launch cost from aggregated primitive measurements.
    vectors = [
        [*([1.0] if spec.fit_intercept else []), *(value / scale for value, scale in zip(point, scales))]
        for point, _ in measurements
    ]
    width = len(spec.features) + int(spec.fit_intercept)
    products = [
        [math.fsum(row[left] * row[right] for row in vectors) for right in range(width)] for left in range(width)
    ]
    responses = [
        math.fsum(row[index] * value for row, (_, value) in zip(vectors, measurements)) for index in range(width)
    ]
    solution = _solve(products, responses)
    if solution is None:
        return {
            **identity,
            "status": "missing_data",
            "reason": "features_not_identifiable",
            "features": list(spec.features),
            "feature_ranges": ranges,
            "distinct_work_points": len(feature_points),
            "required_coefficients": width,
            "constant_features": [name for name in spec.features if ranges[name][0] == ranges[name][1]],
            "source_captures": sorted({row.capture for row in rows}),
            "next_step": "Choose fewer justified variables or independently measure varied work; repeated identical points do not identify new coefficients.",
        }

    # Coefficients may have either sign. Negative predictions are a cost gap,
    # not a reason to clip the result to zero or secretly switch model forms.
    result.update(
        intercept_us=solution[0] if spec.fit_intercept else 0.0,
        coefficients={
            name: value / scale
            for name, value, scale in zip(spec.features, solution[int(spec.fit_intercept) :], scales)
        },
        fit_intercept=spec.fit_intercept,
    )
    return _with_training_fit(result, measurements, spec.features)


def estimate(model: dict[str, Any], features: dict[str, float]) -> dict[str, Any]:
    """Inspect a trained response and its range with the same policy as C++."""

    if model["status"] != "ready":
        return {"status": "missing_data", "reason": model["reason"]}
    required = set(model["feature_ranges"])
    if required - features.keys():
        return {"status": "missing_data", "reason": "missing_features", "features": sorted(required - features.keys())}

    outside = [name for name, (low, high) in model["feature_ranges"].items() if not low <= features[name] <= high]
    unsupported = set(outside) - set(model.get("extrapolation_features", ()))
    if unsupported and not model["allow_extrapolation"]:
        return {
            "status": "missing_data",
            "reason": "outside_measured_range",
            "features": outside,
            "measured_feature_ranges": model["feature_ranges"],
        }

    duration = _response(model, features)
    if duration < 0 or not math.isfinite(duration):
        return {"status": "missing_data", "reason": "invalid_predicted_duration"}

    point = {name: features[name] for name in model["feature_ranges"]}
    return {
        "status": "ready",
        "duration_us": math.floor(duration + 0.5),
        "outside_measured_range": outside,
        "unseen_feature_combination": point not in model["measured_feature_points"],
    }
