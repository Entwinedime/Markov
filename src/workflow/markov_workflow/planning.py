"""Publish a finite source plan and its concrete capture inputs."""

import json

from markov_adapters.artifacts import write_json
from markov_workflow.adapters.sglang_planning import SourcePlanning


class PlanningProcess:
    def __init__(self, arguments, *, service_evidence=False):
        self.arguments = arguments
        self.service_evidence = service_evidence

    def run(self):
        source = json.loads(self.arguments.input.read_text())
        planner = SourcePlanning(source)
        plan = planner.document(service_evidence=self.service_evidence)
        output = self.arguments.output
        output.mkdir(parents=True)
        write_json(output / "input.json", source, indent=2, replace=False)
        (output / "sources").mkdir()
        workload_basis = (
            "Performance sources retain length endpoints and batching opportunities."
            if self.service_evidence else "Structure sources shorten requests where possible and include the required lifecycle cases."
        )
        lines = [
            "# Source Capture Plan", "",
            f"Planned {plan['native_jobs']} native jobs for {plan['targets']['count']} target combinations, including local operation sources.",
            "Complementary configurations or distinct padding layouts can supply evidence for the same implementation domain. "
            "Each prediction uses one complete set of worker templates.",
            workload_basis + " Each engine processes one submission group at a time. Native records establish actual batches and cache hits.",
            "", "| Source job | Representative configuration / workload | Requests / submission groups | Prompt / decode tokens | "
            "Original selected tokens | Native workers |",
            "| --- | --- | ---: | ---: | ---: | ---: |",
        ]
        for job in plan["sources"]:
            selected = job["source"]
            cost = selected["input_cost"]
            write_json(output / selected["input"], planner.capture_inputs[job["id"]], indent=2, replace=False)
            lines.append(f"| {job['id']} | {selected['configuration']} / {', '.join(selected['workloads'])} | {cost['requests']} / {selected['submission_group_count']} | "
                         f"{cost['prompt_tokens']} / {cost['decode_tokens']} | {selected['original_tokens']} | {job['native_workers']} |")
        for job in plan["sources"]:
            required = job["required_coverage"]
            if not required:
                continue
            consumers = ["targets in this domain"] if job["configurations"] else []
            if "local_consumers" in job:
                consumers.extend(f"source {item['source']}/{item['mode']}" for item in job["local_consumers"])
            coverage = ", ".join(f"{mode}: {', '.join(witnesses)}" for mode, witnesses in required.items())
            lines.append(f"\nJob {job['id']} supplies evidence for {', '.join(consumers)}. "
                         f"Publishing the source requires {coverage}.")
        lines.extend([
            "", "Native execution owns batching and the request lifecycle. Representative observation limits profiling and export. "
            "Every worker in the parallel protocol still runs.",
            "`sources/` contains the capture inputs. `plan.json` records the selection and intended coverage.",
            "Capture reports any coverage gaps against this fixed plan.", "",
        ])
        (output / "plan.md").write_text("\n".join(lines))

        # Publish the index only after every referenced capture input exists.
        write_json(output / "plan.json", plan, indent=2, replace=False)
