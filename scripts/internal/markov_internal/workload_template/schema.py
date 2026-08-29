"""Input parsing for manual HiCache JSON workloads."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping


class TemplateValidationError(ValueError):
    """Raised when a workload template cannot enter a server run."""


@dataclass(frozen=True)
class ConfigSpec:
    """Resolved-target contract for one HiCache server configuration."""

    config_id: str
    page_size: int
    device_pages: int
    host_pages: int
    write_policy: str
    prefetch_threshold: int
    prefetch_stop_policy: str
    prefetch_timeout_base_sec: float | None
    prefetch_timeout_per_ki_token_sec: float | None
    prefetch_timeout_max_sec: float | None
    prefetch_capacity_limit_tokens: int


@dataclass(frozen=True)
class Template:
    """Validated JSON template retained in its source representation."""

    path: Path
    data: Mapping[str, Any]

    @property
    def workload_id(self) -> str:
        """Return the stable config-independent workload identifier."""

        return str(self.data["id"])


def load_template(path: Path) -> Template:
    """Load and validate one JSON template before any tokenizer/server use."""

    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise TemplateValidationError(f"invalid JSON template {path}: {error}") from error
    if not isinstance(data, dict):
        raise TemplateValidationError(f"template {path} must be a JSON object")
    _validate_template_shape(data, path)
    return Template(path=path, data=data)


def load_config_specs(path: Path) -> dict[str, ConfigSpec]:
    """Load the five resolved-target HiCache configuration contracts."""

    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise TemplateValidationError(f"invalid config JSON {path}: {error}") from error
    if not isinstance(data, dict):
        raise TemplateValidationError(f"config set {path} must be a JSON object")
    raw_configs = data.get("configs")
    if not isinstance(raw_configs, list) or not raw_configs:
        raise TemplateValidationError("config set must contain a non-empty configs list")

    result: dict[str, ConfigSpec] = {}
    for position, raw_config in enumerate(raw_configs):
        if not isinstance(raw_config, dict):
            raise TemplateValidationError(f"configs[{position}] must be an object")
        config_id = _required_nonempty_string(raw_config, "id", f"configs[{position}]")
        if config_id in result:
            raise TemplateValidationError(f"duplicate config id: {config_id}")
        resolved = _required_object(raw_config, "resolved", f"configs[{position}]")
        policy = _required_object(raw_config, "policy", f"configs[{position}]")
        timeout = policy.get("timeout") if isinstance(policy.get("timeout"), dict) else {}
        page_size = _required_positive_int(resolved, "page_size", f"configs[{position}].resolved")
        device_pages = _required_positive_int(resolved, "device_pages", f"configs[{position}].resolved")
        host_pages = _required_positive_int(resolved, "host_pages", f"configs[{position}].resolved")
        if host_pages <= device_pages:
            raise TemplateValidationError(f"{config_id}: host_pages must exceed device_pages")
        threshold = _required_positive_int(policy, "prefetch_threshold", f"configs[{position}].policy")
        limit = _required_nonnegative_int(
            policy,
            "prefetch_capacity_limit_tokens",
            f"configs[{position}].policy",
        )
        write_policy = _required_nonempty_string(policy, "write_policy", f"configs[{position}].policy")
        if write_policy not in {"write_through", "write_through_selective", "write_back"}:
            raise TemplateValidationError(f"{config_id}: unsupported write_policy {write_policy}")
        stop_policy = _required_nonempty_string(
            policy,
            "prefetch_stop_policy",
            f"configs[{position}].policy",
        )
        if stop_policy not in {"wait_complete", "best_effort", "timeout"}:
            raise TemplateValidationError(f"{config_id}: unsupported prefetch_stop_policy {stop_policy}")
        result[config_id] = ConfigSpec(
            config_id=config_id,
            page_size=page_size,
            device_pages=device_pages,
            host_pages=host_pages,
            write_policy=write_policy,
            prefetch_threshold=threshold,
            prefetch_stop_policy=stop_policy,
            prefetch_timeout_base_sec=_optional_number(timeout, "base_sec"),
            prefetch_timeout_per_ki_token_sec=_optional_number(timeout, "per_ki_token_sec"),
            prefetch_timeout_max_sec=_optional_number(timeout, "max_sec"),
            prefetch_capacity_limit_tokens=limit,
        )
    return result


def _validate_template_shape(data: Mapping[str, Any], path: Path) -> None:
    """Validate template fields that are independent of a tokenizer."""

    _required_nonempty_string(data, "id", "template")
    _required_nonempty_string(data, "description", "template")
    defaults = _required_object(data, "defaults", "template")
    sampling = _required_object(defaults, "sampling", "template.defaults")
    for name in ("max_new_tokens", "top_k"):
        _required_positive_int(sampling, name, "template.defaults.sampling")
    if sampling.get("temperature") != 0:
        raise TemplateValidationError("template.defaults.sampling.temperature must be 0")
    if sampling.get("top_p") != 1.0:
        raise TemplateValidationError("template.defaults.sampling.top_p must be 1.0")
    if sampling.get("ignore_eos") is not True:
        raise TemplateValidationError("template.defaults.sampling.ignore_eos must be true")

    fragments = _required_object(data, "fragments", "template")
    for fragment_name, fragment in fragments.items():
        if not isinstance(fragment_name, str) or not fragment_name:
            raise TemplateValidationError("fragment names must be non-empty strings")
        if not isinstance(fragment, dict):
            raise TemplateValidationError(f"fragment {fragment_name} must be an object")
        _required_nonempty_string(fragment, "text", f"fragment {fragment_name}")
        _required_nonnegative_int(fragment, "repeat", f"fragment {fragment_name}")

    request_defs = _required_object(data, "request_defs", "template")
    if not request_defs:
        raise TemplateValidationError("template.request_defs must not be empty")
    for request_name, request_def in request_defs.items():
        if not isinstance(request_name, str) or not request_name:
            raise TemplateValidationError("request definition names must be non-empty strings")
        if request_name.count("{i}") > 1:
            raise TemplateValidationError(f"request definition {request_name} may contain at most one {{i}}")
        if not isinstance(request_def, dict):
            raise TemplateValidationError(f"request definition {request_name} must be an object")
        parts = request_def.get("prompt_parts")
        if not isinstance(parts, list) or not parts:
            raise TemplateValidationError(f"request definition {request_name} must contain prompt_parts")
        for part_index, part in enumerate(parts):
            if not isinstance(part, dict) or set(part) - {"ref", "text"}:
                raise TemplateValidationError(
                    f"request definition {request_name}.prompt_parts[{part_index}] is invalid"
                )
            if ("ref" in part) == ("text" in part):
                raise TemplateValidationError(
                    f"request definition {request_name}.prompt_parts[{part_index}] needs exactly one of ref/text"
                )
            if "ref" in part and part["ref"] not in fragments:
                raise TemplateValidationError(
                    f"request definition {request_name} references unknown fragment {part['ref']}"
                )
            if "text" in part and not isinstance(part["text"], str):
                raise TemplateValidationError(
                    f"request definition {request_name}.prompt_parts[{part_index}].text must be a string"
                )
        token_contract = _required_object(request_def, "token_contract", f"request definition {request_name}")
        _required_positive_int(token_contract, "anchor_tokens", f"request definition {request_name}.token_contract")
        _required_positive_int(token_contract, "tail_tokens", f"request definition {request_name}.token_contract")
        branch_markers = request_def.get("branch_markers")
        branch_marker = request_def.get("branch_marker")
        if branch_marker is not None and (not isinstance(branch_marker, str) or not branch_marker):
            raise TemplateValidationError(f"request definition {request_name}.branch_marker must be a non-empty string")
        if branch_marker is not None and branch_markers is not None:
            raise TemplateValidationError(
                f"request definition {request_name} may not define both branch_marker and branch_markers"
            )
        if branch_markers is not None:
            if "{i}" not in request_name or not isinstance(branch_markers, dict) or not branch_markers:
                raise TemplateValidationError(
                    f"request definition {request_name}.branch_markers requires an indexed request definition"
                )
            for marker_index, marker in branch_markers.items():
                if (
                    not isinstance(marker_index, str)
                    or not marker_index.isdigit()
                    or not isinstance(marker, str)
                    or not marker
                ):
                    raise TemplateValidationError(
                        f"request definition {request_name}.branch_markers must map numeric ids to non-empty strings"
                    )

    steps = data.get("steps")
    if not isinstance(steps, list) or not steps:
        raise TemplateValidationError("template.steps must be a non-empty list")
    known_step_ids: set[str] = set()
    for step_index, step in enumerate(steps):
        _validate_step(step, step_index, request_defs, known_step_ids)
        known_step_ids.add(str(step["id"]))

    formal_window = _required_object(data, "formal_window", "template")
    start_step = _required_nonempty_string(formal_window, "start_step", "template.formal_window")
    end_step = _required_nonempty_string(formal_window, "end_step", "template.formal_window")
    if start_step not in known_step_ids or end_step not in known_step_ids:
        raise TemplateValidationError("formal_window must reference declared step ids")
    _validate_two_stage_layout(steps, start_step=start_step, end_step=end_step)


def _validate_two_stage_layout(
    steps: list[Any],
    *,
    start_step: str,
    end_step: str,
) -> None:
    """Enforce one settled boundary and a non-trivial request-only formal phase."""

    barrier_positions = [index for index, step in enumerate(steps) if step.get("kind") == "barrier"]
    checkpoint_positions = [index for index, step in enumerate(steps) if step.get("kind") == "checkpoint"]
    if len(barrier_positions) != 1:
        raise TemplateValidationError("two-stage workload must contain exactly one barrier")
    if len(checkpoint_positions) != 1:
        raise TemplateValidationError("two-stage workload must contain exactly one checkpoint")

    barrier_position = barrier_positions[0]
    checkpoint_position = checkpoint_positions[0]
    if checkpoint_position != barrier_position + 1:
        raise TemplateValidationError("the sole checkpoint must immediately follow the sole barrier")

    prepare_steps = steps[:barrier_position]
    formal_steps = steps[checkpoint_position + 1 :]
    if not prepare_steps or not formal_steps:
        raise TemplateValidationError("two-stage workload needs requests on both sides of the boundary")
    request_kinds = {"request", "repeat_request"}
    if any(step.get("kind") not in request_kinds for step in prepare_steps + formal_steps):
        raise TemplateValidationError("only request steps may appear outside the barrier/checkpoint boundary")
    if any(step.get("measure") is not False for step in prepare_steps):
        raise TemplateValidationError("all preparation requests must set measure=false")
    if steps[barrier_position].get("measure") is not False or steps[checkpoint_position].get("measure") is not False:
        raise TemplateValidationError("the barrier and checkpoint boundary must set measure=false")
    if any(step.get("measure") is not True for step in formal_steps):
        raise TemplateValidationError("all formal-stage requests must set measure=true")
    if start_step != formal_steps[0].get("id") or end_step != formal_steps[-1].get("id"):
        raise TemplateValidationError("formal_window must cover the complete request sequence after the boundary")

def _validate_step(
    step: Any,
    step_index: int,
    request_defs: Mapping[str, Any],
    known_step_ids: set[str],
) -> None:
    """Validate one fixed-order step and prohibit runtime branching fields."""

    if not isinstance(step, dict):
        raise TemplateValidationError(f"steps[{step_index}] must be an object")
    step_id = _required_nonempty_string(step, "id", f"steps[{step_index}]")
    if step_id in known_step_ids:
        raise TemplateValidationError(f"duplicate step id: {step_id}")
    kind = _required_nonempty_string(step, "kind", f"steps[{step_index}]")
    if kind not in {"request", "repeat_request", "barrier", "checkpoint", "wait"}:
        raise TemplateValidationError(f"steps[{step_index}].kind is unsupported: {kind}")
    _required_nonempty_string(step, "phase", f"steps[{step_index}]")
    if not isinstance(step.get("measure"), bool):
        raise TemplateValidationError(f"steps[{step_index}].measure must be boolean")
    if kind in {"request", "repeat_request"}:
        request_name = _required_nonempty_string(step, "request", f"steps[{step_index}]")
        if not _request_definition_exists(request_name, request_defs):
            raise TemplateValidationError(f"steps[{step_index}] references unknown request {request_name}")
        if kind == "repeat_request":
            _required_nonnegative_int(step, "count", f"steps[{step_index}]")
    elif kind == "barrier":
        if step.get("scope") != "hicache_idle":
            raise TemplateValidationError("barrier.scope must be hicache_idle")
        _required_positive_int(step, "timeout_sec", f"steps[{step_index}]")
    elif kind == "checkpoint":
        assertions = step.get("assertions")
        if not isinstance(assertions, list) or not assertions:
            raise TemplateValidationError(f"steps[{step_index}].assertions must be non-empty")
        for assertion_index, assertion in enumerate(assertions):
            _validate_checkpoint_assertion(assertion, step_index, assertion_index, request_defs)
    else:
        _required_nonnegative_int(step, "duration_ms", f"steps[{step_index}]")


def _validate_checkpoint_assertion(
    assertion: Any,
    step_index: int,
    assertion_index: int,
    request_defs: Mapping[str, Any],
) -> None:
    """Validate a read-only checkpoint witness declaration."""

    context = f"steps[{step_index}].assertions[{assertion_index}]"
    if not isinstance(assertion, dict):
        raise TemplateValidationError(f"{context} must be an object")
    _required_nonempty_string(assertion, "id", context)
    request_name = _required_nonempty_string(assertion, "request", context)
    if "{i}" in request_name or not _request_definition_exists(request_name, request_defs):
        raise TemplateValidationError(f"{context}.request must name one concrete request")
    if assertion.get("range") not in {"anchor", "admission_tail"}:
        raise TemplateValidationError(f"{context}.range must be anchor or admission_tail")
    expect = _required_object(assertion, "expect", context)
    for key in ("storage", "device", "host"):
        value = _required_nonempty_string(expect, key, f"{context}.expect")
        allowed_values = {"all", "none", "ignore"}
        if key == "device":
            allowed_values.add("none_or_absent")
        if value not in allowed_values:
            allowed_text = ", ".join(sorted(allowed_values))
            raise TemplateValidationError(f"{context}.expect.{key} must be one of: {allowed_text}")


def _request_definition_exists(request_name: str, request_defs: Mapping[str, Any]) -> bool:
    """Return whether exact or one-index-pattern request definition exists."""

    if request_name in request_defs:
        return True
    for pattern in request_defs:
        if "{i}" not in pattern:
            continue
        prefix, suffix = pattern.split("{i}", 1)
        if request_name.startswith(prefix) and request_name.endswith(suffix):
            middle = request_name[len(prefix) : len(request_name) - len(suffix) if suffix else None]
            if middle.isdigit():
                return True
    return False


def _required_object(data: Mapping[str, Any], field: str, context: str) -> Mapping[str, Any]:
    """Read a required object field with a contextual error."""

    value = data.get(field)
    if not isinstance(value, dict):
        raise TemplateValidationError(f"{context}.{field} must be an object")
    return value


def _required_nonempty_string(data: Mapping[str, Any], field: str, context: str) -> str:
    """Read a required non-empty string field."""

    value = data.get(field)
    if not isinstance(value, str) or not value:
        raise TemplateValidationError(f"{context}.{field} must be a non-empty string")
    return value


def _required_positive_int(data: Mapping[str, Any], field: str, context: str) -> int:
    """Read a strictly positive non-boolean integer field."""

    value = data.get(field)
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise TemplateValidationError(f"{context}.{field} must be a positive integer")
    return int(value)


def _required_nonnegative_int(data: Mapping[str, Any], field: str, context: str) -> int:
    """Read a non-negative non-boolean integer field."""

    value = data.get(field)
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise TemplateValidationError(f"{context}.{field} must be a non-negative integer")
    return int(value)


def _optional_number(data: Mapping[str, Any], field: str) -> float | None:
    """Read an optional numeric timeout field."""

    value = data.get(field)
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise TemplateValidationError(f"timeout.{field} must be numeric when set")
    return float(value)
