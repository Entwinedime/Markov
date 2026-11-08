"""Check retained source evidence independently of template production and prediction."""

import argparse
from bisect import bisect_right
import json
from collections import Counter, defaultdict
from heapq import heappop, heappush
from pathlib import Path

from source_clock import ClockConstraints


class SourceEvidence:
    def __init__(self, facts, graph, clock_samples, details=False):
        self.facts, self.graph = facts, graph
        self.evidence = graph["evidence"]
        self.details = details
        self.counts = Counter()
        self.findings = []
        self.maximum_inversion_ns = 0
        self.coverage = {}
        self.clock_samples = clock_samples

    @staticmethod
    def require(condition, message):
        if not condition:
            raise ValueError(message)

    def coordinate(self, anchor):
        span = self.facts["spans"][anchor["span"]]
        self.require(span["begin"] <= anchor["order"] <= span["end"], f"anchor outside span: {anchor}")
        resource = span["resource"]

        # Serial boundaries can be shared by adjacent spans; concurrent services retain their own span identity.
        return resource, anchor["order"], None if self.facts["resources"][resource]["serial"] else anchor["span"]

    def validate_mapping(self):
        facts, graph, evidence = self.facts, self.graph, self.evidence
        self.require(len(graph["events"]) == len(evidence["events"]), "event evidence count differs")
        self.require(len(graph["work"]) == len(evidence["work_spans"]), "work evidence count differs")
        self.require(len(graph["scopes"]) == len(facts["scopes"]), "source scope count differs")
        self.require(graph["environment"] == facts["environment"], "source environment differs")
        self.require(graph["resources"] == [
            {key: value for key, value in resource.items() if key not in {"observed", "payload"}}
            for resource in facts["resources"]
        ], "source resource descriptors differ")
        measurements = facts["measurements"]
        self.require(all(evidence[key] == measurements[key] for key in ("clock", "window_ns")), "measurement clock/window differs")
        times = {(item["resource"], item["order"]): item["time_ns"] for item in measurements["coordinates"]}
        coordinates = {}
        for index, (event, anchor) in enumerate(zip(graph["events"], evidence["events"], strict=True)):
            key = self.coordinate(anchor)
            self.require(key not in coordinates, f"duplicate projected coordinate: {key}")
            coordinates[key] = index
            self.require(event["resource"] == key[0], f"event {index} changes source resource")
            self.require(anchor["time_ns"] == times[key[:2]], f"event {index} changes source measurement")

        edges = {(edge["source"], edge["target"], edge["kind"]) for edge in graph["edges"]}
        native_edges = set()
        for index, edge in enumerate(facts["dependencies"]):
            source, target = (coordinates[self.coordinate(edge[side])] for side in ("source", "target"))
            self.require(source == target or (source, target, edge["kind"]) in edges, f"native dependency {index} is missing")
            if source != target:
                native_edges.add((source, target, edge["kind"]))
        orders = self.validate_coverage(coordinates)
        self.require(edges == native_edges | orders, "projected dependencies omit or add source/serial relations")
        self.require(len(edges) == len(graph["edges"]), "projected dependencies repeat")


        # Expressions are normalized by the compiler; unchanged call identity and communication facts are not.
        transformed = {"arguments", "guards", "ports", "kv_accesses", "slot_accesses"}
        for index, (native, projected) in enumerate(zip(facts["scopes"], graph["scopes"], strict=True)):
            self.require({key: value for key, value in native.items() if key not in transformed} ==
                         {key: value for key, value in projected.items() if key not in transformed},
                         f"scope {index} changes source identity, context or communication")
            self.require(len(native["ports"]) == len(projected["ports"]), f"scope {index} port count differs")
            for port, target in zip(native["ports"], projected["ports"], strict=True):
                expected = {key: value for key, value in port.items() if key not in {"at", "wait", "rendezvous"}}
                expected["event"] = coordinates[self.coordinate(port["at"])]
                if "wait" in port:
                    expected["wait"] = {
                        "resource": port["wait"]["resource"], "begin": coordinates[self.coordinate(port["wait"]["begin"])],
                    }
                if "rendezvous" in port:
                    expected["rendezvous"] = coordinates[self.coordinate(port["rendezvous"])]

                # This check compares observed geometry, not symbolic formulas normalized by the expression compiler.
                observed = dict(target)
                for value in (expected, observed):
                    if "tensor" in value:
                        value["tensor"] = {key: item for key, item in value["tensor"].items() if key != "shape_rule"}
                self.require(expected == observed, f"scope {index} port {port['name']} differs")
            for field in ("kv_accesses", "slot_accesses"):
                self.require(len(native[field]) == len(projected[field]), f"scope {index} {field} count differs")
                for access, target in zip(native[field], projected[field], strict=True):
                    expected = {key: value for key, value in access.items() if key != "rule"}
                    expected.update({side: coordinates[self.coordinate(access[side])] for side in ("start", "finish")})
                    observed = {key: value for key, value in target.items() if key != "rule"}
                    self.require(expected == observed, f"scope {index} {field} changes source access")

    def validate_coverage(self, coordinates):
        ancestors = []
        for index, scope in enumerate(self.facts["scopes"]):
            parent = scope["parent"]
            self.require((parent is None and index == 0) or (parent is not None and 0 <= parent < index),
                         f"scope {index} has an invalid source parent")
            ancestors.append({index} | (ancestors[parent] if parent is not None else set()))

        spans, boundaries, touching = defaultdict(list), defaultdict(list), defaultdict(set)
        required = set()
        for index, span in enumerate(self.facts["spans"]):
            resource = self.facts["resources"][span["resource"]]
            owner = None if resource["serial"] else index
            spans[span["resource"], owner].append((index, span))
            for position in (span["begin"], span["end"]):
                required.add((span["resource"], position, owner))
                touching[span["resource"], position, owner].add(span["scope"])
        for scope in self.facts["scopes"]:
            for port in scope["ports"]:
                required.add(self.coordinate(port["at"]))
                if "wait" in port:
                    required.add(self.coordinate(port["wait"]["begin"]))
                if "rendezvous" in port:
                    required.add(self.coordinate(port["rendezvous"]))
            for access in scope["kv_accesses"] + scope["slot_accesses"]:
                required.update(self.coordinate(access[side]) for side in ("start", "finish"))
        for edge in self.facts["dependencies"]:
            required.update(self.coordinate(edge[side]) for side in ("source", "target"))
        self.require(required == coordinates.keys(), "projected boundaries omit or add source coordinates")

        for (resource, position, owner), event in coordinates.items():
            boundaries[resource, owner].append((position, event))

        expected, orders = Counter(), set()
        for key, points in boundaries.items():
            resource = self.facts["resources"][key[0]]
            payload = resource["payload"]
            payload_ends = [end for _, end in payload]
            pending = iter(sorted(spans[key], key=lambda item: item[1]["begin"]))
            candidate = next(pending, None)
            active = []
            points.sort()
            preceding = None
            for position, (begin, start) in enumerate(points):
                while candidate is not None and candidate[1]["begin"] <= begin:
                    index, span = candidate

                    # Innermost span owns work; a nested wait suppresses its parent's service cost.
                    heappush(active, (span["end"], -span["begin"], -span["scope"], span["kind"] != "wait", -index))
                    candidate = next(pending, None)
                while active and active[0][0] <= begin:
                    heappop(active)
                index = -active[0][-1] if active else None
                span = self.facts["spans"][index] if index is not None else None

                # A shared boundary belongs to the common ancestor of both sides and all touching spans.
                owners = touching[key[0], begin, key[1]].copy()
                if preceding is not None:
                    owners.add(preceding)
                if span is not None:
                    owners.add(span["scope"])
                common = set.intersection(*(ancestors[owner] for owner in owners))
                self.require(self.graph["events"][start]["scope"] == max(common), f"event {start} changes source semantic owner")
                preceding = span["scope"] if span is not None else None
                if position + 1 == len(points):
                    continue
                end, finish = points[position + 1]
                has_work = False
                if span is not None:
                    self.require(end <= span["end"], f"resource {key} skips an active span boundary")
                    payload_index = bisect_right(payload_ends, begin)
                    has_payload = payload_index < len(payload) and payload[payload_index][0] < end
                    has_work = span["kind"] not in {"wait", "boundary"} and (span["kind"] == "host" or has_payload)
                if has_work:
                    expected[index, span["scope"], key[0], span["kind"], start, finish] += 1
                else:
                    orders.add((start, finish, "order"))

        # Compare the whole partition, not just the pieces that survived projection.
        actual = Counter((span, work["scope"], work["resource"], work["kind"], work["start"], work["finish"])
                         for span, work in zip(self.evidence["work_spans"], self.graph["work"], strict=True))
        self.require(actual == expected,
                     f"source work coverage differs: {sum((expected - actual).values())} missing, {sum((actual - expected).values())} extra")
        self.coverage = {"native_spans": len(self.facts["spans"]), "work_intervals": sum(expected.values()),
                         "ordering_intervals": len(orders)}
        return orders

    def timing(self, kind, index, source, target):
        begin, end = (self.evidence["events"][event]["time_ns"] for event in (source, target))
        issue = "unknown_time" if begin is None or end is None else "time_inversion" if begin > end else None
        if issue is None:
            return
        self.counts[issue] += 1
        difference = None if issue == "unknown_time" else begin - end
        if difference is not None:
            self.maximum_inversion_ns = max(self.maximum_inversion_ns, difference)
        if self.details or self.counts[issue] <= 3:
            self.findings.append({
                "issue": issue, "kind": kind, "index": index, "source": source, "target": target,
                "source_ns": begin, "target_ns": end, "difference_ns": difference,
            })

    def inspect(self):
        self.validate_mapping()
        clock = ClockConstraints(self.graph, self.clock_samples)
        for index, edge in enumerate(self.graph["edges"]):
            self.timing(edge["kind"], index, edge["source"], edge["target"])
            clock.constrain(edge["kind"], index, edge["source"], edge["target"])
        for index, work in enumerate(self.graph["work"]):
            self.timing("work/" + work["kind"], index, work["start"], work["finish"])
            clock.constrain("work/" + work["kind"], index, work["start"], work["finish"])
        return {
            "mapping": "consistent", "timing": clock.report(),
            "events": len(self.graph["events"]), "work": len(self.graph["work"]),
            "coverage": self.coverage,
            "counts": dict(self.counts), "maximum_inversion_ns": self.maximum_inversion_ns, "findings": self.findings,
        }


class ProductionEvidence:
    def __init__(self, production, capture, output, details):
        self.production, self.capture, self.output, self.details = production, capture, output, details

    def run(self):
        publication = json.loads((self.production / "result.json").read_text())
        reports = []
        for rank in publication["observed_roles"]:
            try:
                facts = json.loads((self.production / f"work/{rank}-facts.json").read_text())
                graph = json.loads((self.production / f"work/{rank}-graph.json").read_text())
                native = json.loads((self.capture / f"rank-{rank}.json").read_text())
                SourceEvidence.require(native["environment"] == facts["environment"], "clock source environment differs")
                report = SourceEvidence(facts, graph, native["device_clock"], self.details).inspect()
            except (OSError, ValueError, KeyError, IndexError, TypeError) as error:

                # A malformed or absent source is a finding; continue the remaining independent ranks.
                report = {"mapping": "unverified", "timing": {"status": "unverified"}, "error": str(error)}
            reports.append({"rank": rank, **report})

        self.output.mkdir(parents=True)
        document = {"production": str(self.production.resolve()), "capture": str(self.capture.resolve()), "ranks": reports}
        (self.output / "source-evidence.json").write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n")
        lines = [
            "# 来源证据检查", "", "本检查核对解释结果到粗图的环境、调用层级与上下文、通信声明、完整工作/顺序区间、边界及其语义归属、几何、依赖和状态访问，以及实测时间是否满足图中因果；工作区间不能遗漏或重复，等待和无载荷区间保留顺序而不计为工作。不证明原生插桩完整、符号表达式与条件变换、模板泛化或目标预测正确。",
            "", "| rank | 来源映射 | 校准约束 | 名义时间倒置数 | 最大名义倒置/ns |", "| --- | --- | --- | ---: | ---: |",
        ]
        labels = {"consistent": "一致", "compatible": "校准范围内相容", "inconsistent": "不存在共同校准", "incomplete": "缺少测量", "unverified": "未能核对"}
        for report in reports:
            lines.append(
                f"| {report['rank']} | {labels[report['mapping']]} | {labels[report['timing']['status']]} | "
                f"{report.get('counts', {}).get('time_inversion', '—')} | {report.get('maximum_inversion_ns', '—')} |"
            )
        for report in reports:
            if "error" in report:
                lines.extend(["", f"rank {report['rank']} 无法完成检查：{report['error']}", ""])
        lines.extend([
            "", "名义倒置按原始中点时间统计；校准约束则核对同一rank全部已测关系能否共同满足两端实测查询区间，不能逐边选择不同的时钟。相容只证明现有仿射时钟假设下没有测量矛盾，不证明精确偏移、插桩完整或SDK关联正确。原始时间和图不修改，也不输出拟合后的时间。unknown_time仍是缺少测量。",
            "", "只有映射一致、全部时间已测且存在共同校准才返回 0；其余返回 1，并保留所有 rank 的结果。默认 JSON 每类仅列前三个名义时间问题，--details 输出全部位置。", "",
        ])
        (self.output / "source-evidence.md").write_text("\n".join(lines))
        return 0 if reports and all(report["mapping"] == "consistent" and report["timing"]["status"] == "compatible"
                                   for report in reports) else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--production", type=Path, required=True, help="published produce output with retained intermediates")
    parser.add_argument("--capture", type=Path, required=True, help="original native source containing independent device clock samples")
    parser.add_argument("--output", type=Path, required=True, help="new report directory")
    parser.add_argument("--details", action="store_true", help="include all timing findings")
    arguments = parser.parse_args()
    raise SystemExit(ProductionEvidence(arguments.production, arguments.capture, arguments.output, arguments.details).run())


if __name__ == "__main__":
    main()
