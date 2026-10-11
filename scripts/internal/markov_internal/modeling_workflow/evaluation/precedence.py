"""Compare semantic reachability without materializing all operation pairs."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any


# Bound temporary reachability masks independently of the number of operators.
# This is a memory/throughput choice, not a modeling or comparison tolerance.
REACHABILITY_BLOCK_BITS = 1024

# Diagnostic examples must not materialize millions of differing endpoint
# pairs. All relations are still compared and counted, independently of this
# report-size limit.
PRECEDENCE_DETAIL_LIMIT = 64


@dataclass
class PrecedenceGraph:
    successors: list[list[int]]
    order: list[int]
    endpoints: list[int]

    @classmethod
    def from_report(cls, report: dict[str, Any], identities: list[str]) -> PrecedenceGraph:
        """Load report-local indices, including anonymous dependency junctions.

        Named slots precede anonymous slots. A partition may replace a name
        with None while retaining that node as an intermediate. No cost,
        cross-trace node id or source timestamp participates in matching.
        """

        successors = report["precedence_successors"]
        indices = {name: index for index, name in enumerate(report["precedence_identities"]) if name is not None}
        indegree = [0] * len(successors)
        for children in successors:
            for child in children:
                indegree[child] += 1

        order = [node for node, degree in enumerate(indegree) if degree == 0]
        index = 0
        while index < len(order):
            for child in successors[order[index]]:
                indegree[child] -= 1
                if indegree[child] == 0:
                    order.append(child)
            index += 1

        return cls(successors, order, [indices[name] for name in identities])

    def reachable_block(self, begin: int, end: int) -> list[int]:
        """Return each matched endpoint's reachability to one destination block."""

        masks = [0] * len(self.successors)
        for bit, node in enumerate(self.endpoints[begin:end]):
            masks[node] = 1 << bit

        for node in reversed(self.order):
            mask = masks[node]
            for child in self.successors[node]:
                mask |= masks[child]
            masks[node] = mask

        result = [masks[node] for node in self.endpoints]
        for bit, index in enumerate(range(begin, end)):
            result[index] &= ~(1 << bit)
        return result


def compare_precedence(
    predicted: dict[str, Any], target: dict[str, Any], common: set[str], *, details: bool
) -> dict[str, Any]:
    """Compare all reachable matched pairs, including paths through unmatched work.

    Redundant edges do not change the result. Anonymous junctions preserve
    paths but make no claim about the identity of their intermediate work.
    Cyclic/unprocessed regions remain explicit and cannot certify exactness.
    All reports count every difference; explicit details retain bounded
    examples instead of allocating one object per differing pair.
    """

    identities = sorted(common)
    expected = PrecedenceGraph.from_report(predicted, identities)
    observed = PrecedenceGraph.from_report(target, identities)
    result: dict[str, Any] = {
        "compared_operators": len(identities),
        "compared_prediction_relations": 0,
        "compared_target_relations": 0,
        "prediction_unprocessed_nodes": predicted["precedence_unprocessed_nodes"],
        "target_unprocessed_nodes": target["precedence_unprocessed_nodes"],
        "prediction_unprocessed_projection": len(expected.successors) - len(expected.order),
        "target_unprocessed_projection": len(observed.successors) - len(observed.order),
        "missing_relations": 0,
        "extra_relations": 0,
        "relation_comparison": "all reachable matched operation pairs; unmatched intermediates and redundant edges do not become dependency differences",
        "count_interpretation": "Counts are endpoint pairs, not broken edges or operations; one ordering difference can affect many pairs.",
    }
    if details:
        result.update(missing=[], extra=[], detail_example_limit=PRECEDENCE_DETAIL_LIMIT)

    for begin in range(0, len(identities), REACHABILITY_BLOCK_BITS):
        end = min(begin + REACHABILITY_BLOCK_BITS, len(identities))
        prediction = expected.reachable_block(begin, end)
        actual = observed.reachable_block(begin, end)
        for index, (predicted_mask, actual_mask) in enumerate(zip(prediction, actual)):
            result["compared_prediction_relations"] += predicted_mask.bit_count()
            result["compared_target_relations"] += actual_mask.bit_count()
            for kind, difference in (
                ("missing", actual_mask & ~predicted_mask),
                ("extra", predicted_mask & ~actual_mask),
            ):
                result[kind + "_relations"] += difference.bit_count()
                if not details:
                    continue

                while difference and len(result[kind]) < PRECEDENCE_DETAIL_LIMIT:
                    bit = difference & -difference
                    result[kind].append(
                        {"from_operator": identities[index], "to_operator": identities[begin + bit.bit_length() - 1]}
                    )
                    difference ^= bit

    if details:
        for kind in ("missing", "extra"):
            result[kind + "_details_truncated"] = result[kind + "_relations"] > len(result[kind])

    return result
