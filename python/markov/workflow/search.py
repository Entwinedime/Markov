"""Rank a finite predicted candidate set under declared resource reservations."""

from __future__ import annotations

from collections import Counter
from dataclasses import asdict, dataclass
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
    hourly_cost_units: float

    @classmethod
    def parse(cls, raw: dict) -> ResourcePlan:
        reject_unknown(raw, {"name", "device_gib_per_rank", "host_gib", "hourly_cost_units"}, "resource plan")
        return cls(
            required_text(raw["name"], "resource plan name"),
            positive_integer(raw["device_gib_per_rank"], "device_gib_per_rank"),
            positive_integer(raw["host_gib"], "host_gib"),
            positive_number(raw["hourly_cost_units"], "hourly_cost_units"),
        )


@dataclass(frozen=True)
class SearchPolicy:
    baseline: str
    fixed_device_gib_per_rank: float
    fixed_host_gib: float
    maximum_time_ratio: float
    plans: tuple[ResourcePlan, ...]

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
                "resource_plans",
            },
            "resource search",
        )
        plans = tuple(ResourcePlan.parse(plan) for plan in raw["resource_plans"])
        if not plans or len({plan.name for plan in plans}) != len(plans):
            raise ValueError("Resource plans must be nonempty and have unique names")
        return cls(
            required_text(raw["baseline"], "baseline"),
            positive_number(raw["fixed_device_gib_per_rank"], "fixed_device_gib_per_rank"),
            positive_number(raw["fixed_host_gib"], "fixed_host_gib"),
            positive_number(raw["maximum_time_ratio"], "maximum_time_ratio"),
            plans,
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
                "cost_gaps": cell["cost_gaps"],
            }
            reservation = cell.get("resource_reservation")
            if reservation is None:
                if cell["status"] == "predicted":
                    row["status"] = "resource_accounting_unavailable"
                rows.append(row)
                continue

            device = self.fixed_device_gib_per_rank + reservation["device_kv_bytes_per_rank"] / GIB
            host = self.fixed_host_gib + participants * reservation["host_kv_bytes_per_rank"] / GIB
            row.update(device_reserved_gib_per_rank=device, host_reserved_gib=host)

            fits = [plan for plan in self.plans if device <= plan.device_gib_per_rank and host <= plan.host_gib]
            if not fits:
                row["status"] = "resource_envelope_exceeded"
            else:
                plan = min(
                    fits, key=lambda plan: (plan.hourly_cost_units, plan.host_gib, plan.device_gib_per_rank, plan.name)
                )
                row.update(resource_plan=plan.name, hourly_cost_units=plan.hourly_cost_units)
                if cell["status"] == "predicted":
                    row["status"] = "ranked"
            rows.append(row)

        ranked = [row for row in rows if row["status"] == "ranked"]
        baseline = next(row for row in rows if row["name"] == self.baseline)
        limit = baseline["predicted_sec"] * self.maximum_time_ratio if baseline["status"] == "ranked" else None
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
            "status": "completed" if not unresolved and baseline["status"] == "ranked" else "incomplete",
            "baseline": self.baseline,
            "maximum_time_sec": limit,
            "selected": chosen["name"] if chosen else None,
            "selection_status": "selected" if chosen else "no_ranked_candidate_meets_time_constraint",
            "fastest": fastest["name"] if fastest else None,
            "pareto": pareto,
            "expected": len(rows),
            "ranked": len(ranked),
            "statuses": dict(Counter(row["status"] for row in rows)),
            "fixed_device_gib_per_rank": self.fixed_device_gib_per_rank,
            "fixed_host_gib": self.fixed_host_gib,
            "maximum_time_ratio": self.maximum_time_ratio,
            "resource_plans": [asdict(plan) for plan in self.plans],
            "candidates": rows,
            "qualification": "Finite supplied candidates, complete formal HTTP. Resource plans assume identical devices/runtime/I/O and differ in reservation and declared price only. Fixed reserves are caller assumptions including weights, workspace and external buffers; KV pools include native padding and in-pool buffers. Price units do not establish actual spending. Missing predictions do not enter ranking; any selection from incomplete coverage is provisional. No target measurements or refitting.",
        }


def search(path: Path) -> dict:
    raw = read_json(path)
    policy = SearchPolicy.parse(raw)
    request = PredictionRequest.load(resolve_path(path.resolve().parent, raw["prediction"]))
    if policy.baseline not in {target.name for target in request.targets}:
        raise ValueError("Search baseline must name one of the supplied prediction targets")

    prediction = predict(request)
    result = (
        policy.rank(prediction) if "cells" in prediction else {"status": "incomplete", "prediction_result": prediction}
    )
    result["prediction_report"] = str(request.execution.output / "prediction/report.json")
    output = request.execution.output / "search"
    output.mkdir(parents=True, exist_ok=True)
    write_json(output / "report.json", result)
    result["report"] = str(output / "report.json")
    return result
