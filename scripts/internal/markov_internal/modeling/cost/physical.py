"""Physical measurement admission and offline service formulas, without capture orchestration."""

from __future__ import annotations

from collections import defaultdict
import math
from statistics import median
from typing import Any

from ...common.io import load_json
from ...common.paths import require_repo_path
from .io_contract import (
    DMA_SERVICES,
    MissingCostEvidence,
    RUNTIME_DMA_INDEX_RESIDENCY,
    RUNTIME_DMA_OPERATOR_NAME,
    positive_finite_number,
    positive_u64,
)


FIT_RANK_TOLERANCE = 1e-12


def read_physical_measurements(physical: dict[str, Any], deployment: dict[str, Any]) -> list[dict[str, Any]]:
    """Read explicitly declared independent files at import, never during prediction.

    A source may contribute storage grid measurements, DMA device samples or
    page-transfer stages. Report-only sources retain provenance but contribute
    no fabricated samples. Coefficients in historical reports are not imported.
    """

    measurements = []
    for source in dict.fromkeys(physical["measurement_sources"]):
        document = load_json(require_repo_path(source))
        if document.get("target_workload_trace_used") is not False or document.get("target_e2e_used") is not False:
            raise ValueError(f"physical measurement is not target-independent: {source}")

        storage = [
            point
            for point in document.get("host_storage", {}).get("selected_points", [])
            if point["direction"] == "host_to_storage" and point["resource_state"] in {"existing_key", "sustained"}
        ]
        for point in storage:
            for field in ("page_bytes", "operation_pages_per_scope", "selected_service_duration_ns", "bytes"):
                positive_u64(point[field], field)
            positive_finite_number(point["selected_service_bandwidth_bytes_per_sec"], "storage bandwidth")
            if point["resource_state"] == "sustained":
                positive_u64(point["operation_bytes_per_scope"], "operation_bytes_per_scope")
                positive_u64(point["operation_count"], "operation_count")

        prefetch = document.get("prefetch_service_observations", [])
        for sample in prefetch:
            positive_u64(sample["page_bytes"], "prefetch page_bytes")
            for event in sample["events"]:
                if not 0 <= event["start_ns"] <= event["end_ns"]:
                    raise ValueError("physical prefetch requires ordered, nonnegative stage timestamps")
                if event["stage"] == "batch":
                    positive_u64(event["page_count"], "prefetch batch page_count")

        dma = []
        if "runtime_semantics" in document:
            _admit_dma(document, physical, deployment)
            dma = document["samples"]
            for sample in dma:
                if sample["direction"] not in DMA_SERVICES:
                    raise ValueError("physical DMA sample has an unsupported transfer direction")
                for field in ("page_bytes", "operation_pages", "bytes"):
                    positive_u64(sample[field], field)
                positive_finite_number(sample["device_duration_us"], "DMA device_duration_us")

        if storage or prefetch or dma:
            measurements.append(dict(source=source, storage_points=storage, prefetch_samples=prefetch, dma_samples=dma))

    if not measurements:
        raise ValueError("physical I/O import requires original service measurements, not only fitted parameters")

    return measurements


def _admit_dma(document: dict[str, Any], physical: dict[str, Any], deployment: dict[str, Any]) -> None:
    """Check real sampler semantics and placement against the declared deployment."""

    semantics = document["runtime_semantics"]
    if (semantics["backend"], semantics["layout"], semantics["operator"], semantics["index_residency"]) != (
        "kernel_ascend",
        "page_first_direct",
        RUNTIME_DMA_OPERATOR_NAME,
        RUNTIME_DMA_INDEX_RESIDENCY,
    ) or document["model_weights_loaded"] is not False:
        raise ValueError("physical DMA requires independent kernel_ascend/page_first_direct measurements")
    if document["geometry"]["kv_bytes_per_token_per_rank"] != physical["kv_geometry"]["kv_bytes_per_token_per_rank"]:
        raise ValueError("physical DMA KV geometry differs from the declared deployment")

    parameters = document["parameters"]
    server = deployment["server"]
    count = int(server["tp_size"])
    first, step = int(server["base_gpu_id"]), int(server["gpu_id_step"])
    expected_devices = list(range(first, first + count * step, step))
    devices = parameters["devices"]
    ranks = sorted(document["environment"]["ranks"], key=lambda row: row["rank"])
    if (
        parameters["scope_count"] != count
        or len(devices) != count
        or len(set(devices)) != count
        or any(device < 0 for device in devices)
        or [row["rank"] for row in ranks] != list(range(count))
        or [row["device"] for row in ranks] != devices
        or devices != expected_devices
    ):
        raise ValueError("physical DMA devices must match actual ranks and the declared storage placement")
    nodes = parameters.get("numa_nodes")
    declared_nodes = [int(value) for value in server["numa_node"].split()] if server["numa_node"] else []
    if nodes is not None and (
        len(nodes) != count
        or any(node is not None and node < 0 for node in nodes)
        or [row.get("numa_preferred_node") for row in ranks] != nodes
        or (declared_nodes and nodes != [declared_nodes[device] for device in devices])
    ):
        raise ValueError("physical DMA NUMA preference must match actual ranks and the declared placement")


def fit_physical_services(measurements: list[dict[str, Any]]) -> tuple[dict[str, dict[str, Any]], dict[str, str]]:
    """Fit only measured services. Missing families stay absent, never zero.

    Raw stage samples are pooled at their page/batch working coordinates;
    storage grid responses retain the sampler's percentile selection. DMA uses
    device time. These distinct clocks and estimation rules are not conflated.
    """

    prefetch = [sample for source in measurements for sample in source["prefetch_samples"]]
    storage = [point for source in measurements for point in source["storage_points"]]
    dma = [sample for source in measurements for sample in source["dma_samples"]]
    services, missing = {}, {}
    if prefetch:
        try:
            services["prefetch"] = _prefetch_model(prefetch)
        except MissingCostEvidence as error:
            missing["prefetch"] = str(error)

    existing = [point for point in storage if point["resource_state"] == "existing_key"]
    new = [point for point in storage if point["resource_state"] == "sustained"]
    if existing or new:
        model: dict[str, Any] = {"direction": "host_to_storage"}
        if existing:
            by_work: dict[tuple[int, int], list[float]] = defaultdict(list)
            for point in existing:
                by_work[point["page_bytes"], point["operation_pages_per_scope"]].append(
                    point["selected_service_bandwidth_bytes_per_sec"]
                )
            model["existing_key_bandwidth_points"] = [
                dict(page_bytes=page, operation_pages=pages, bandwidth_bytes_per_sec=median(values))
                for (page, pages), values in sorted(by_work.items())
            ]
        if new:
            try:
                model["new_operation_points"] = _new_write_points(new)
            except MissingCostEvidence as error:
                missing["write_host_to_storage_new"] = str(error)
        services["write_host_to_storage"] = model

    for direction, family in DMA_SERVICES.items():
        samples = [sample for sample in dma if sample["direction"] == direction]
        if samples:
            try:
                services[family] = dict(direction=direction, page_bandwidth_points=_dma_points(samples))
            except MissingCostEvidence as error:
                missing[family] = str(error)

    return services, missing


def _prefetch_model(samples: list[dict[str, Any]]) -> dict[str, Any]:
    before, copying, tails = defaultdict(list), defaultdict(list), defaultdict(list)
    for row in samples:
        events, page_bytes = row["events"], row["page_bytes"]
        for batch in (event for event in events if event["stage"] == "batch"):
            inside = [
                event
                for event in events
                if event["stage"] != "batch"
                and batch["start_ns"] <= event["start_ns"] <= event["end_ns"] <= batch["end_ns"]
            ]
            publications = [event for event in inside if event["stage"] == "publish"]
            copies = [event for event in inside if event["stage"] == "copy"]
            pages = batch["page_count"]
            if not publications or len(publications) != len(copies):
                raise MissingCostEvidence("prefetch physical batch lacks copy/publication pairs")

            prefix_ns = sum(
                event["end_ns"] - event["start_ns"] for event in inside if event["stage"] in {"allocate", "read"}
            )
            copying_ns = sum(event["end_ns"] - event["start_ns"] for event in copies + publications)
            before[page_bytes, pages].append(prefix_ns / (1000 * pages))
            copying[page_bytes, pages].append(copying_ns / (1000 * len(copies)))
            end_publish = max(event["end_ns"] for event in publications)
            # Injected cancellation is sampling control, not batch cleanup.
            injected_ns = sum(
                event["end_ns"] - event["start_ns"]
                for event in inside
                if event["stage"] == "injected_cancel" and event["start_ns"] >= end_publish
            )
            tails[pages].append((batch["end_ns"] - end_publish - injected_ns) / 1000)

    prefix = _nonnegative_fit([(1.0, float(size), median(values)) for (size, _), values in sorted(before.items())])
    copy = _nonnegative_fit([(1.0, float(size), median(values)) for (size, _), values in sorted(copying.items())])
    tail = _nonnegative_fit([(1.0, float(pages), median(values)) for pages, values in sorted(tails.items())])
    stages = dict(
        zip(
            (
                "before_copy_us_per_page",
                "before_copy_us_per_byte",
                "copy_publish_us_per_page",
                "copy_publish_us_per_byte",
                "return_us_per_operation",
                "return_us_per_page",
            ),
            prefix + copy + tail,
        )
    )
    return dict(direction="storage_to_host", stages=stages)


def _new_write_points(points: list[dict[str, Any]]) -> list[dict[str, Any]]:
    by_page: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for point in points:
        by_page[point["page_bytes"]].append(point)

    output = []
    for page_bytes, page_points in sorted(by_page.items()):
        if len({point["operation_bytes_per_scope"] for point in page_points}) < 2:
            raise MissingCostEvidence(f"new-write page size {page_bytes} requires at least two operation payloads")
        setup, us_per_byte = _nonnegative_fit(
            [
                (
                    float(point["operation_count"]),
                    float(point["bytes"]),
                    float(point["selected_service_duration_ns"]) / 1000,
                )
                for point in page_points
            ]
        )
        if not math.isfinite(us_per_byte) or us_per_byte <= 0:
            raise MissingCostEvidence("physical new-write measurements did not identify positive byte cost")
        output.append(
            dict(page_bytes=page_bytes, setup_us_per_operation=setup, bandwidth_bytes_per_sec=1e6 / us_per_byte)
        )

    return output


def _dma_points(samples: list[dict[str, Any]]) -> list[dict[str, Any]]:
    by_page: dict[int, dict[int, list[float]]] = {}
    for row in samples:
        by_page.setdefault(row["page_bytes"], {}).setdefault(row["bytes"], []).append(row["device_duration_us"])

    points = []
    for page_bytes, sizes in sorted(by_page.items()):
        if len(sizes) < 2:
            raise MissingCostEvidence("physical DMA requires two operation sizes per page size")
        anchors = [(size, median(times)) for size, times in sorted(sizes.items())]
        small, large = anchors[0], anchors[-1]
        slope = (large[1] - small[1]) / (large[0] - small[0])
        setup = small[1] - slope * small[0]
        if setup < 0:
            # A negative setup uses the constrained origin line, never target residuals.
            setup = 0.0
            slope = math.fsum(size * duration for size, duration in anchors) / math.fsum(
                size * size for size, _ in anchors
            )
        if slope <= 0 or not math.isfinite(slope):
            raise MissingCostEvidence("physical DMA samples do not identify positive byte cost")
        points.append(dict(page_bytes=page_bytes, setup_us_per_operation=setup, bandwidth_bytes_per_sec=1e6 / slope))

    return points


def _nonnegative_fit(rows: list[tuple[float, float, float]]) -> list[float]:
    """Two-coefficient NNLS; compare zero, axis and admissible full solutions."""

    candidates = [[0.0, 0.0]]
    for index in range(2):
        denominator = math.fsum(float(row[index]) ** 2 for row in rows)
        if denominator > 0:
            value = math.fsum(float(row[index]) * float(row[-1]) for row in rows) / denominator
            if value >= 0 and math.isfinite(value):
                candidate = [0.0, 0.0]
                candidate[index] = value
                candidates.append(candidate)

    scales = [max(abs(float(row[index])) for row in rows) for index in range(2)]
    if all(scale > 0 for scale in scales):
        z0 = [float(row[0]) / scales[0] for row in rows]
        z1 = [float(row[1]) / scales[1] for row in rows]
        y = [float(row[-1]) for row in rows]
        a00 = math.fsum(value * value for value in z0)
        a01 = math.fsum(left * right for left, right in zip(z0, z1))
        a11 = math.fsum(value * value for value in z1)
        b0 = math.fsum(value * target for value, target in zip(z0, y))
        b1 = math.fsum(value * target for value, target in zip(z1, y))
        determinant = a00 * a11 - a01 * a01
        if determinant > FIT_RANK_TOLERANCE * max(a00 * a11, 1.0):
            scaled = [(b0 * a11 - b1 * a01) / determinant, (a00 * b1 - a01 * b0) / determinant]
            candidate = [scaled[index] / scales[index] for index in range(2)]
            if all(value >= 0 and math.isfinite(value) for value in candidate):
                candidates.append(candidate)

    return min(
        candidates,
        key=lambda values: math.fsum(
            (math.fsum(float(row[index]) * values[index] for index in range(2)) - float(row[-1])) ** 2 for row in rows
        ),
    )
