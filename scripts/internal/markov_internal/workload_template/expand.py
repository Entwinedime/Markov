"""Deterministic expansion of JSON workloads into a serial request plan."""

from __future__ import annotations

from dataclasses import dataclass
from collections.abc import Iterator
from typing import Any, Mapping, Protocol, Union

from .schema import Template, TemplateValidationError, resolve_request_definition


class Tokenizer(Protocol):
    """Minimal tokenizer surface used by the compiler."""

    def encode(self, text: str, add_special_tokens: bool = False) -> list[int]:
        """Encode deterministic text without implicit special tokens."""


@dataclass(frozen=True)
class RequestPlan:
    """One expanded serial request with immutable prompt identity."""

    step_id: str
    sequence_id: int
    logical_request_id: str
    request_name: str
    phase: str
    measure: bool
    prompt_token_ids: tuple[int, ...]
    anchor_tokens: int
    tail_tokens: int
    max_new_tokens: int


@dataclass(frozen=True)
class StaticPlanStep:
    """One barrier or checkpoint in the fixed logical sequence."""

    step_id: str
    sequence_id: int
    kind: str
    phase: str
    measure: bool
    details: Mapping[str, Any]


PlanStep = Union[RequestPlan, StaticPlanStep]


@dataclass(frozen=True)
class CanonicalPlan:
    """Expanded request plan consumed by the serial executor."""

    template: Template
    steps: tuple[PlanStep, ...]
    formal_start_step: str
    formal_end_step: str

    @property
    def requests(self) -> tuple[RequestPlan, ...]:
        """Return all HTTP request steps in strict serial order."""

        return tuple(step for step in self.steps if isinstance(step, RequestPlan))

    @property
    def request_by_name(self) -> dict[str, RequestPlan]:
        """Return one descriptor per logical request name."""

        result: dict[str, RequestPlan] = {}
        for request in self.requests:
            existing = result.get(request.request_name)
            if existing is None:
                result[request.request_name] = request
            elif existing.prompt_token_ids != request.prompt_token_ids:
                raise TemplateValidationError(
                    f"request {request.request_name} rendered different token paths in one template"
                )
        return result


def load_tokenizer(model_path: str) -> Tokenizer:
    """Load the model tokenizer only for design-time compilation and dry-runs."""

    try:
        from transformers import AutoTokenizer
    except ImportError as error:
        raise RuntimeError("template compilation requires transformers in the SGLang environment") from error
    return AutoTokenizer.from_pretrained(model_path, trust_remote_code=True)


def expand_template(
    template: Template,
    tokenizer: Tokenizer | None,
) -> CanonicalPlan:
    """Expand a template and enforce request token contracts."""

    all_steps: list[PlanStep] = []
    for raw_step in template.data["steps"]:
        for expanded_step in _expand_step(raw_step):
            sequence_id = len(all_steps)
            if expanded_step["kind"] == "request":
                request = _build_request_plan(
                    template,
                    expanded_step,
                    sequence_id,
                    tokenizer,
                )
                all_steps.append(request)
            else:
                static_step = StaticPlanStep(
                    step_id=expanded_step["id"],
                    sequence_id=sequence_id,
                    kind=expanded_step["kind"],
                    phase=expanded_step["phase"],
                    measure=expanded_step["measure"],
                    details=_static_step_details(expanded_step),
                )
                all_steps.append(static_step)

    # The schema fixes the formal stage; repeats may expand its declared boundary
    # into several requests. Execution needs the actual first/last expanded IDs.
    formal_requests = [step for step in all_steps if isinstance(step, RequestPlan) and step.measure]
    if not formal_requests:
        raise TemplateValidationError("the formal stage must contain at least one expanded request")
    return CanonicalPlan(
        template=template,
        steps=tuple(all_steps),
        formal_start_step=formal_requests[0].step_id,
        formal_end_step=formal_requests[-1].step_id,
    )


def request_token_budget(template: Template) -> dict[str, int]:
    """Reserve token-contract bounds without loading a tokenizer on the host.

    Actual execution still validates text/token equality before sending requests.
    This estimate includes preparation requests, not only the formal window.
    """
    counts = []
    output = template.data["defaults"]["sampling"]["max_new_tokens"]
    for raw in template.data["steps"]:
        for step in _expand_step(raw):
            if step["kind"] == "request":
                definition, _ = resolve_request_definition(template.data["request_defs"], step["request"])
                contract = definition["token_contract"]
                counts.append(contract["anchor_tokens"] + contract["tail_tokens"] + output)
    return {"requests": len(counts), "tokens": sum(counts), "output_tokens_per_request": output}


def _expand_step(raw_step: Mapping[str, Any]) -> Iterator[Mapping[str, Any]]:
    """Yield validated steps in fixed order; consumers must not mutate them."""

    if raw_step["kind"] != "repeat_request":
        yield raw_step
        return

    count = raw_step["count"]
    for repeat_index in range(count):
        yield {
            **raw_step,
            "kind": "request",
            "id": _format_repeated_step_id(raw_step["id"], repeat_index, count),
            "request": _format_index(raw_step["request"], repeat_index),
        }


def _format_repeated_step_id(step_id: str, repeat_index: int, count: int) -> str:
    """Produce stable expanded ids without making one-request repeats ambiguous."""

    if "{i}" in step_id:
        return _format_index(step_id, repeat_index)
    if count == 1:
        return step_id
    return f"{step_id}[{repeat_index}]"


def _format_index(value: str, request_index: int) -> str:
    """Replace the sole allowed request index placeholder."""

    return value.replace("{i}", str(request_index))


def _build_request_plan(
    template: Template,
    raw_step: Mapping[str, Any],
    sequence_id: int,
    tokenizer: Tokenizer | None,
) -> RequestPlan:
    """Render one request and verify its local text/token contract."""

    request_name = raw_step["request"]
    request_definition, request_index = resolve_request_definition(template.data["request_defs"], request_name)
    if "prompt_token_ids" in request_definition:
        token_ids = tuple(request_definition["prompt_token_ids"])
    else:
        if tokenizer is None:
            raise TemplateValidationError("text prompts require a tokenizer")
        prompt = _render_prompt(template, request_definition, request_index)
        token_ids = tuple(int(token_id) for token_id in tokenizer.encode(prompt, add_special_tokens=False))
    if not token_ids:
        raise TemplateValidationError(f"request {request_name} rendered an empty token sequence")
    token_contract = request_definition["token_contract"]
    anchor_tokens = token_contract["anchor_tokens"]
    expected_tail_tokens = token_contract["tail_tokens"]
    actual_tail_tokens = len(token_ids) - anchor_tokens
    if actual_tail_tokens != expected_tail_tokens:
        raise TemplateValidationError(
            f"request {request_name}: tail token contract expected {expected_tail_tokens}, got {actual_tail_tokens}"
        )
    return RequestPlan(
        step_id=raw_step["id"],
        sequence_id=sequence_id,
        logical_request_id=f"{template.workload_id}:{raw_step['id']}",
        request_name=request_name,
        phase=raw_step["phase"],
        measure=raw_step["measure"],
        prompt_token_ids=token_ids,
        anchor_tokens=anchor_tokens,
        tail_tokens=actual_tail_tokens,
        max_new_tokens=template.data["defaults"]["sampling"]["max_new_tokens"],
    )


def _render_prompt(template: Template, request_definition: Mapping[str, Any], request_index: int | None) -> str:
    """Render deterministic text from fragment references and literal parts."""

    rendered_parts: list[str] = []
    format_index = 0 if request_index is None else request_index
    branch_markers = request_definition.get("branch_markers")
    branch_marker = request_definition.get("branch_marker")
    if branch_markers is not None:
        branch_marker = branch_markers.get(str(format_index))
        if branch_marker is None:
            raise TemplateValidationError(f"request index {format_index} has no branch marker")
    fragments = template.data["fragments"]
    for part in request_definition["prompt_parts"]:
        if "ref" in part:
            fragment = fragments[part["ref"]]
            source_text = fragment["text"]
            repeat_count = fragment["repeat"]
            rendered_parts.append(_format_request_text(source_text, format_index, branch_marker) * repeat_count)
        else:
            rendered_parts.append(_format_request_text(part["text"], format_index, branch_marker))
    return "".join(rendered_parts)


def _format_request_text(value: str, request_index: int, branch_marker: str | None) -> str:
    """Render the fixed numeric id and optional one-token branch marker placeholders."""

    result = _format_index(value, request_index)
    if "{branch}" not in result:
        return result
    if branch_marker is None:
        raise TemplateValidationError("request text uses {branch} without branch_markers")
    return result.replace("{branch}", branch_marker)


def _static_step_details(raw_step: Mapping[str, Any]) -> dict[str, Any]:
    """Copy only fixed state-gate fields into the canonical plan."""

    kind = raw_step["kind"]
    if kind == "barrier":
        return {"scope": raw_step["scope"], "timeout_sec": raw_step["timeout_sec"]}
    if kind == "checkpoint":
        return {"assertions": raw_step["assertions"]}
    raise TemplateValidationError(f"unsupported static step kind: {kind}")
