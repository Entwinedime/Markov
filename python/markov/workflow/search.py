"""Rank a finite predicted candidate set under declared resource reservations."""

from __future__ import annotations

from collections import Counter
from dataclasses import asdict, dataclass
import fcntl
import math
from pathlib import Path

from markov.artifacts.request import (
    PredictionRequest,
    positive_integer,
    read_json,
    reject_unknown,
    required_text,
    resolve_path,
)
from markov.artifacts.writing import write_json
from markov.workflow.prediction import predict


GIB = 1024**3


def positive_number(value: float, name: str) -> float:
    if type(value) not in (int, float) or not math.isfinite(value) or value <= 0:
        raise ValueError(f"{name} must be finite and positive")
    return float(value)


@dataclass(frozen=True)
class ResourcePlan:
    name: str
    device_gib_per_rank: int
    host_gib: int

    @classmethod
    def parse(cls, raw: dict) -> ResourcePlan:
        reject_unknown(raw, {"name", "device_gib_per_rank", "host_gib"}, "resource plan")
        return cls(
            required_text(raw["name"], "resource plan name"),
            positive_integer(raw["device_gib_per_rank"], "device_gib_per_rank"),
            positive_integer(raw["host_gib"], "host_gib"),
        )


@dataclass(frozen=True)
class ResourcePricing:
    device_gib_hour: float
    host_gib_hour: float
    storage_gib_hour: float
    device_to_host_gibps_hour: float
    host_to_device_gibps_hour: float
    host_to_storage_gibps_hour: float
    storage_to_host_gibps_hour: float

    @classmethod
    def parse(cls, raw: dict) -> ResourcePricing:
        names = tuple(cls.__dataclass_fields__)
        reject_unknown(raw, set(names), "resource pricing")
        return cls(*(positive_number(raw[name], name) for name in names))

    def costs(self, device_gib: float, host_gib: float, reservation: dict) -> dict[str, float]:
        rates = reservation["bandwidth_bytes_per_sec"]
        # File directions share one service pool; charge each only its allocated share.
        file_share = min(1.0, rates["storage_shared"] / (rates["host_to_storage"] + rates["storage_to_host"]))
        return {
            "device_storage": device_gib * self.device_gib_hour,
            "host_storage": host_gib * self.host_gib_hour,
            "storage_capacity": reservation["storage_reserved_bytes"] / GIB * self.storage_gib_hour,
            "device_to_host_bandwidth": rates["device_to_host"] / GIB * self.device_to_host_gibps_hour,
            "host_to_device_bandwidth": rates["host_to_device"] / GIB * self.host_to_device_gibps_hour,
            "host_to_storage_bandwidth": rates["host_to_storage"] * file_share / GIB * self.host_to_storage_gibps_hour,
            "storage_to_host_bandwidth": rates["storage_to_host"] * file_share / GIB * self.storage_to_host_gibps_hour,
        }


@dataclass(frozen=True)
class SearchPolicy:
    baseline: str
    fixed_device_gib_per_rank: float
    fixed_host_gib: float
    maximum_time_ratio: float | None
    plans: tuple[ResourcePlan, ...]
    pricing: ResourcePricing
    maximum_time_sec: float | None = None

    @classmethod
    def parse(cls, raw: dict) -> SearchPolicy:
        reject_unknown(
            raw,
            {
                "prediction",
                "baseline",
                "fixed_device_gib_per_rank",
                "fixed_host_gib",
                "maximum_time_ratio",
                "maximum_time_sec",
                "resource_plans",
                "pricing",
            },
            "resource search",
        )
        plans = tuple(ResourcePlan.parse(plan) for plan in raw["resource_plans"])
        if not plans or len({plan.name for plan in plans}) != len(plans):
            raise ValueError("Resource plans must be nonempty and have unique names")
        if ("maximum_time_sec" in raw) == ("maximum_time_ratio" in raw):
            raise ValueError("Declare exactly one of maximum_time_sec and maximum_time_ratio")
        return cls(
            required_text(raw["baseline"], "baseline"),
            positive_number(raw["fixed_device_gib_per_rank"], "fixed_device_gib_per_rank"),
            positive_number(raw["fixed_host_gib"], "fixed_host_gib"),
            positive_number(raw["maximum_time_ratio"], "maximum_time_ratio") if "maximum_time_ratio" in raw else None,
            plans,
            ResourcePricing.parse(raw["pricing"]),
            positive_number(raw["maximum_time_sec"], "maximum_time_sec") if "maximum_time_sec" in raw else None,
        )

    def rank(self, prediction: dict) -> dict:
        participants = prediction["deployment"]["tensor_parallel"]
        rows = []
        for cell in prediction["cells"]:
            row = {
                "name": cell["name"],
                "status": cell["status"],
                "prediction_status": cell["status"],
                "predicted_sec": cell["formal_http_sec"],
                "resource_plan": None,
                "hourly_cost_units": None,
                "device_reserved_gib_per_rank": None,
                "host_reserved_gib": None,
                "device_reserved_gib": None,
                "hourly_cost_breakdown": None,
                "window_cost_units": None,
                "cost_gaps": cell["cost_gaps"],
            }
            reservation = cell.get("resource_reservation")
            if reservation is None or not {"storage_reserved_bytes", "bandwidth_bytes_per_sec"} <= reservation.keys():
                if cell["status"] == "predicted":
                    row["status"] = "resource_accounting_unavailable"
                rows.append(row)
                continue

            device = self.fixed_device_gib_per_rank + reservation["device_kv_bytes_per_rank"] / GIB
            host = self.fixed_host_gib + participants * reservation["host_kv_bytes_per_rank"] / GIB
            row.update(device_reserved_gib_per_rank=device, host_reserved_gib=host)
            device_total = participants * device
            breakdown = self.pricing.costs(device_total, host, reservation)
            hourly = sum(breakdown.values())
            row.update(
                device_reserved_gib=device_total,
                device_kv_gib=participants * reservation["device_kv_bytes_per_rank"] / GIB,
                host_kv_gib=participants * reservation["host_kv_bytes_per_rank"] / GIB,
                fixed_storage_hourly_cost_units=(
                    participants * self.fixed_device_gib_per_rank * self.pricing.device_gib_hour
                    + self.fixed_host_gib * self.pricing.host_gib_hour
                ),
                hourly_cost_units=hourly,
                hourly_cost_breakdown=breakdown,
                storage_reserved_gib=reservation["storage_reserved_bytes"] / GIB,
                bandwidth_gib_per_sec={
                    key: value / GIB for key, value in reservation["bandwidth_bytes_per_sec"].items()
                },
                window_cost_units=hourly * row["predicted_sec"] / 3600 if row["predicted_sec"] is not None else None,
            )

            fits = [plan for plan in self.plans if device <= plan.device_gib_per_rank and host <= plan.host_gib]
            if not fits:
                row["status"] = "resource_envelope_exceeded"
            else:
                plan = min(fits, key=lambda plan: (plan.host_gib, plan.device_gib_per_rank, plan.name))
                row.update(resource_plan=plan.name)
                if cell["status"] == "predicted":
                    row["status"] = "ranked"
            rows.append(row)

        ranked = [row for row in rows if row["status"] == "ranked"]
        baseline = next(row for row in rows if row["name"] == self.baseline)
        limit = self.maximum_time_sec
        if self.maximum_time_ratio is not None and baseline["status"] == "ranked":
            limit = baseline["predicted_sec"] * self.maximum_time_ratio
        feasible = [row for row in ranked if limit is not None and row["predicted_sec"] <= limit]
        chosen = (
            min(feasible, key=lambda row: (row["hourly_cost_units"], row["predicted_sec"], row["name"]))
            if feasible
            else None
        )
        fastest = (
            min(ranked, key=lambda row: (row["predicted_sec"], row["hourly_cost_units"], row["name"]))
            if ranked
            else None
        )
        within_budget = [
            row
            for row in ranked
            if baseline["status"] == "ranked" and row["hourly_cost_units"] <= baseline["hourly_cost_units"]
        ]
        recommended = (
            min(within_budget, key=lambda row: (row["predicted_sec"], row["hourly_cost_units"], row["name"]))
            if within_budget
            else None
        )
        worst = (
            min(within_budget, key=lambda row: (-row["predicted_sec"], row["hourly_cost_units"], row["name"]))
            if within_budget
            else None
        )
        pareto = [
            row["name"]
            for row in ranked
            if not any(
                other["predicted_sec"] <= row["predicted_sec"]
                and other["hourly_cost_units"] <= row["hourly_cost_units"]
                and (
                    other["predicted_sec"] < row["predicted_sec"]
                    or other["hourly_cost_units"] < row["hourly_cost_units"]
                )
                for other in ranked
            )
        ]
        unresolved = [
            row for row in rows if row["status"] not in {"ranked", "resource_envelope_exceeded", "resource_infeasible"}
        ]
        return {
            "status": "completed" if not unresolved and limit is not None else "incomplete",
            "baseline": self.baseline,
            "maximum_time_sec": limit,
            "selected": chosen["name"] if chosen else None,
            "selection_status": "selected" if chosen else "no_ranked_candidate_meets_time_constraint",
            "fastest": fastest["name"] if fastest else None,
            "recommended": recommended["name"] if recommended else None,
            "worst_within_budget": worst["name"] if worst else None,
            "pareto": pareto,
            "expected": len(rows),
            "ranked": len(ranked),
            "statuses": dict(Counter(row["status"] for row in rows)),
            "fixed_device_gib_per_rank": self.fixed_device_gib_per_rank,
            "fixed_host_gib": self.fixed_host_gib,
            "maximum_time_ratio": self.maximum_time_ratio,
            "resource_plans": [asdict(plan) for plan in self.plans],
            "pricing": asdict(self.pricing),
            "candidates": rows,
            "qualification": "Finite supplied candidates, complete formal HTTP. Seven charges cover device/host/file capacity and four transfer reservations; shared file service is apportioned once. PCIe limits are per rank and group totals are charged. No CPU, compute rental or per-byte traffic charge. Fixed reserves and prices are declared assumptions. Transfer admission permits one native-operation burst and retains native operation costs; these are software quotas, not replacement hardware. Incomplete coverage makes selection provisional. No target observations or refitting.",
        }


def search(path: Path) -> dict:
    raw = read_json(path)
    policy = SearchPolicy.parse(raw)
    request = PredictionRequest.load(resolve_path(path.resolve().parent, raw["prediction"]))
    if policy.baseline not in {target.name for target in request.targets}:
        raise ValueError("Search baseline must name one of the supplied prediction targets")

    output = request.execution.output / "search"
    output.mkdir(parents=True, exist_ok=True)
    with (output / "search.lock").open("a") as lease:
        fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        report = output / "report.json"
        report.unlink(missing_ok=True)

        prediction = predict(request)
        result = (
            policy.rank(prediction)
            if "cells" in prediction
            else {"status": "incomplete", "prediction_result": prediction}
        )
        result["prediction_report"] = str(request.execution.output / "prediction/report.json")
        write_json(report, result)
        result["report"] = str(report)
        return result
