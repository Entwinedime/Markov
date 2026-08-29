"""Deterministic expansion of JSON workloads into a serial request plan."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from typing import Any, Iterable, Mapping, Protocol, Union

from .schema import Template, TemplateValidationError


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
    prompt: str
    prompt_token_ids: tuple[int, ...]
    anchor_tokens: int
    tail_tokens: int

@dataclass(frozen=True)
class StaticPlanStep:
    """One barrier, checkpoint, or explicit wait in the fixed logical sequence."""

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
    tokenizer: Tokenizer,
) -> CanonicalPlan:
    """Expand a template and enforce request token contracts."""

    raw_steps = template.data["steps"]
    requests: list[RequestPlan] = []
    all_steps: list[PlanStep] = []
    sequence_id = 0
    for raw_step in raw_steps:
        expanded = _expand_step(template, raw_step)
        for expanded_step in expanded:
            if expanded_step["kind"] == "request":
                request = _build_request_plan(
                    template,
                    expanded_step,
                    sequence_id,
                    tokenizer,
                )
                requests.append(request)
                all_steps.append(request)
            else:
                static_step = StaticPlanStep(
                    step_id=str(expanded_step["id"]),
                    sequence_id=sequence_id,
                    kind=str(expanded_step["kind"]),
                    phase=str(expanded_step["phase"]),
                    measure=bool(expanded_step["measure"]),
                    details=_static_step_details(expanded_step),
                )
                all_steps.append(static_step)
            sequence_id += 1

    formal_window = template.data["formal_window"]
    formal_start = str(formal_window["start_step"])
    formal_end = str(formal_window["end_step"])
    return CanonicalPlan(
        template=template,
        steps=tuple(all_steps),
        formal_start_step=formal_start,
        formal_end_step=formal_end,
    )


def prefix_token_digest(token_ids: Iterable[int]) -> str:
    """Return a raw token-prefix digest shared with the read-only diagnostic."""

    return (
        "sha256_u32le:"
        + hashlib.sha256(
            b"".join(int(token_id).to_bytes(4, byteorder="little", signed=False) for token_id in token_ids)
        ).hexdigest()
    )


def _expand_step(template: Template, raw_step: Mapping[str, Any]) -> list[dict[str, Any]]:
    """Expand repeat_request while preserving the template's fixed order."""

    kind = str(raw_step["kind"])
    if kind != "repeat_request":
        result = dict(raw_step)
        if kind == "request":
            result["kind"] = "request"
        return [result]
    count = int(raw_step["count"])
    result: list[dict[str, Any]] = []
    for repeat_index in range(count):
        step = dict(raw_step)
        step["kind"] = "request"
        step["id"] = _format_repeated_step_id(str(raw_step["id"]), repeat_index, count)
        step["request"] = _format_index(str(raw_step["request"]), repeat_index)
        result.append(step)
    return result


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
    tokenizer: Tokenizer,
) -> RequestPlan:
    """Render one request and verify its local text/token contract."""

    request_name = str(raw_step["request"])
    request_definition, request_index = _resolve_request_definition(template, request_name)
    prompt = _render_prompt(template, request_definition, request_index)
    token_ids = tuple(int(token_id) for token_id in tokenizer.encode(prompt, add_special_tokens=False))
    if not token_ids:
        raise TemplateValidationError(f"request {request_name} rendered an empty token sequence")
    token_contract = request_definition["token_contract"]
    anchor_tokens = int(token_contract["anchor_tokens"])
    expected_tail_tokens = int(token_contract["tail_tokens"])
    actual_tail_tokens = len(token_ids) - anchor_tokens
    if actual_tail_tokens != expected_tail_tokens:
        raise TemplateValidationError(
            f"request {request_name}: tail token contract expected {expected_tail_tokens}, got {actual_tail_tokens}"
        )
    return RequestPlan(
        step_id=str(raw_step["id"]),
        sequence_id=sequence_id,
        logical_request_id=f"{template.workload_id}:{raw_step['id']}",
        request_name=request_name,
        phase=str(raw_step["phase"]),
        measure=bool(raw_step["measure"]),
        prompt=prompt,
        prompt_token_ids=token_ids,
        anchor_tokens=anchor_tokens,
        tail_tokens=actual_tail_tokens,
    )


def _resolve_request_definition(template: Template, request_name: str) -> tuple[Mapping[str, Any], int | None]:
    """Resolve a concrete name against an exact or indexed request definition."""

    request_defs = template.data["request_defs"]
    exact = request_defs.get(request_name)
    if isinstance(exact, dict):
        return exact, None
    for pattern, request_definition in request_defs.items():
        if "{i}" not in pattern or not isinstance(request_definition, dict):
            continue
        prefix, suffix = pattern.split("{i}", 1)
        if not request_name.startswith(prefix) or not request_name.endswith(suffix):
            continue
        stop = len(request_name) - len(suffix) if suffix else len(request_name)
        index_text = request_name[len(prefix) : stop]
        if index_text.isdigit():
            return request_definition, int(index_text)
    raise TemplateValidationError(f"request {request_name} does not resolve to a request definition")


def _render_prompt(template: Template, request_definition: Mapping[str, Any], request_index: int | None) -> str:
    """Render deterministic text from fragment references and literal parts."""

    rendered_parts: list[str] = []
    format_index = 0 if request_index is None else request_index
    branch_markers = request_definition.get("branch_markers")
    declared_branch_marker = request_definition.get("branch_marker")
    branch_marker = None
    if isinstance(declared_branch_marker, str) and declared_branch_marker:
        branch_marker = declared_branch_marker
    elif isinstance(branch_markers, dict):
        candidate = branch_markers.get(str(format_index))
        if not isinstance(candidate, str) or not candidate:
            raise TemplateValidationError(f"request index {format_index} has no branch marker")
        branch_marker = candidate
    fragments = template.data["fragments"]
    for part in request_definition["prompt_parts"]:
        if "ref" in part:
            fragment = fragments[str(part["ref"])]
            source_text = str(fragment["text"])
            repeat_count = int(fragment["repeat"])
            rendered_parts.append(_format_request_text(source_text, format_index, branch_marker) * repeat_count)
        else:
            rendered_parts.append(_format_request_text(str(part["text"]), format_index, branch_marker))
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

    kind = str(raw_step["kind"])
    if kind == "barrier":
        return {"scope": raw_step["scope"], "timeout_sec": int(raw_step["timeout_sec"])}
    if kind == "checkpoint":
        return {"assertions": raw_step["assertions"]}
    if kind == "wait":
        return {"duration_ms": int(raw_step["duration_ms"])}
    raise TemplateValidationError(f"unsupported static step kind: {kind}")
