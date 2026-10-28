"""Collect independent Ascend input-kernel traces without loading a model."""

import argparse
from decimal import Decimal
import json
from pathlib import Path

from markov.artifacts.request import positive_integer, read_json, reject_unknown


class InputKernelSampler:
    operations = ("positions", "request_table")
    kernel_names = {"positions": "compute_position_kernel", "request_table": "write_req_to_token_pool_triton"}

    @staticmethod
    def device_measurements(trace: Path, calls: list[dict]) -> list[dict]:
        events = json.loads(trace.read_text(encoding="utf-8"))
        marked = [event for event in events if event.get("name", "").startswith("markov.input_kernel:")]
        markers = {int(event["name"].split(":")[-1]): event for event in marked}
        tasks = sorted(
            ((i, event) for i, event in enumerate(events) if "Task Type" in event.get("args", {})),
            key=lambda row: Decimal(row[1]["ts"]),
        )
        if len(tasks) != len(calls) or len(marked) != len(calls) or set(markers) != set(range(len(calls))):
            raise RuntimeError("Input-kernel capture does not contain exactly the declared calls and tasks")

        measurements = []
        for ordinal, ((record, event), call) in enumerate(zip(tasks, calls)):
            if (
                event["ph"] != "X"
                or event["name"] != InputKernelSampler.kernel_names[call["operation"]]
                or event["args"]["Task Type"] != "KERNEL_AIVEC"
            ):
                raise RuntimeError("Input-kernel device task does not match its declared operation")
            begin = Decimal(event["ts"])
            end = begin + Decimal(str(event["dur"]))
            if begin < Decimal(markers[ordinal]["ts"]) or (
                ordinal + 1 < len(calls) and end > Decimal(markers[ordinal + 1]["ts"])
            ):
                raise RuntimeError("Input-kernel task is outside its synchronized invocation")
            duration = float(Decimal(str(event["dur"])) * 1000)
            if duration <= 0:
                raise RuntimeError("Input-kernel device service must be positive")
            measurements.append(
                {
                    "trace": trace.name,
                    "record": record,
                    "ordinal": ordinal,
                    **call,
                    "duration_ns": duration,
                    "observed_unit": "us",
                    "observed_duration": event["dur"],
                }
            )
        return measurements

    def __init__(self, device: int, columns: int):
        import torch
        import torch_npu
        from sglang.srt.mem_cache.triton_ops.common import (
            write_req_to_token_pool_triton,
        )
        from sglang.srt.model_executor.triton_ops.position import (
            compute_position_triton,
        )

        torch.npu.set_device(device)
        self.torch = torch
        self.profiler = torch_npu.profiler
        self.position = compute_position_triton
        self.write = write_req_to_token_pool_triton
        self.device = f"npu:{device}"
        self.columns = columns

    def prepare(self, query: int, prefix: int) -> dict:
        torch = self.torch
        old = torch.arange(prefix, dtype=torch.int64, device=self.device)
        new = torch.arange(prefix, prefix + query, dtype=torch.int32, device=self.device)
        return {
            "query_tokens": query,
            "prefix_tokens": prefix,
            "old": old,
            "new": new,
            "table": torch.empty((1, self.columns), dtype=torch.int32, device=self.device),
            "row": torch.tensor([0], dtype=torch.int64, device=self.device),
            "pointers": torch.tensor([old.data_ptr()], dtype=torch.uint64).to(self.device),
            "prefix64": torch.tensor([prefix], dtype=torch.int64, device=self.device),
            "sequence64": torch.tensor([prefix + query], dtype=torch.int64, device=self.device),
            "extend64": torch.tensor([query], dtype=torch.int64, device=self.device),
            "prefix32": torch.tensor([prefix], dtype=torch.int32, device=self.device),
            "extend32": torch.tensor([query], dtype=torch.int32, device=self.device),
        }

    def launch(self, case: dict, operation: str) -> None:
        if operation == "positions":
            case["positions"], case["starts"] = self.position(case["prefix32"], case["extend32"], case["query_tokens"])
        else:
            self.write[(1,)](
                case["table"],
                case["row"],
                case["pointers"],
                case["prefix64"],
                case["sequence64"],
                case["extend64"],
                case["new"],
                self.columns,
            )

    def verify(self, case: dict) -> None:
        torch = self.torch
        prefix, query = case["prefix_tokens"], case["query_tokens"]
        if not torch.equal(case["positions"].cpu(), torch.arange(prefix, prefix + query, dtype=torch.int64)):
            raise RuntimeError("Position kernel returned incorrect positions")
        if case["starts"].cpu().tolist() != [0]:
            raise RuntimeError("Position kernel returned an incorrect start offset")
        if not torch.equal(case["table"][0, : prefix + query].cpu(), torch.arange(prefix + query, dtype=torch.int32)):
            raise RuntimeError("Request-table kernel returned incorrect token locations")

    def collect(self, shapes: list[tuple[int, int]], warmup: int, repeats: int, output: Path) -> None:
        cases = [self.prepare(query, prefix) for query, prefix in shapes]
        for case in cases:
            for _ in range(warmup):
                for operation in self.operations:
                    self.launch(case, operation)
            self.torch.npu.synchronize()
            self.verify(case)

        calls = []
        activities = self.profiler.ProfilerActivity
        with self.profiler.profile(
            activities=[activities.CPU, activities.NPU],
            record_shapes=False,
            profile_memory=False,
            with_stack=False,
            experimental_config=self.profiler._ExperimentalConfig(
                profiler_level=self.profiler.ProfilerLevel.Level0,
            ),
        ) as profiler:
            for index, case in enumerate(cases):
                for operation in self.operations:
                    for repeat in range(repeats):
                        with self.torch.profiler.record_function(f"markov.input_kernel:{len(calls)}"):
                            self.launch(case, operation)
                        self.torch.npu.synchronize()
                        calls.append({"case": index, "operation": operation, "repeat": repeat})
        profiler.export_chrome_trace(str(output / "trace.json"))

        for case in cases:
            self.verify(case)
        measurements = self.device_measurements(output / "trace.json", calls)
        (output / "measurements.json").write_text(
            json.dumps(
                {
                    "scope": "Ordered device tasks from synchronized single-kernel calls; no CPU or E2E qualification",
                    "trace": "trace.json",
                    "measurements": measurements,
                    "functional_results_verified": True,
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        (output / "capture.json").write_text(
            json.dumps(
                {
                    "status": "complete",
                    "scope": "Independent single-row Triton device services; no CPU cost or E2E qualification",
                    "torch": self.torch.__version__,
                    "device": self.device,
                    "table_columns": self.columns,
                    "cases": [{"query_tokens": query, "prefix_tokens": prefix} for query, prefix in shapes],
                    "calls": calls,
                    "warmup_per_operation_per_case": warmup,
                    "results_verified": True,
                    "limits": [
                        "Inputs and compilation are prepared before profiling; no weights, HTTP or snapshots.",
                        "One synchronized invocation at a time; device contention and overlapping kernels are not covered.",
                        "Device-task ownership and timing must be checked before importing any cost; profiler CPU time is not normal CPU service.",
                    ],
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recipe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    recipe = read_json(args.recipe)
    reject_unknown(recipe, {"device", "table_columns", "warmup", "repeats", "cases"}, "input-kernel recipe")
    device = recipe["device"]
    if isinstance(device, bool) or not isinstance(device, int) or device < 0:
        raise ValueError("device must be a nonnegative integer")
    columns = positive_integer(recipe["table_columns"], "table_columns")
    warmup = positive_integer(recipe["warmup"], "warmup")
    repeats = positive_integer(recipe["repeats"], "repeats")
    if not isinstance(recipe["cases"], list) or not recipe["cases"]:
        raise ValueError("cases must be a nonempty array")
    shapes = []
    for case in recipe["cases"]:
        reject_unknown(case, {"query_tokens", "prefix_tokens"}, "input-kernel case")
        query = positive_integer(case["query_tokens"], "query_tokens")
        prefix = case["prefix_tokens"]
        if isinstance(prefix, bool) or not isinstance(prefix, int) or prefix < 0 or prefix + query > columns:
            raise ValueError("prefix and query must fit the request table")
        shapes.append((query, prefix))

    args.output.mkdir(parents=True, exist_ok=False)
    sampler = InputKernelSampler(device, columns)
    sampler.collect(shapes, warmup, repeats, args.output)


if __name__ == "__main__":
    main()
