"""Path contracts for HiCache transition-validation artifacts."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from markov_internal.common.paths import resolve_repo_path


@dataclass(frozen=True)
class PathsForPrediction:
    """Standard transition artifacts located under one prediction directory."""

    prediction_dir: Path
    predicted_trace: Path
    validation: Path
    model_self_check: Path


def resolve_required_path(path: Path | None, flag_name: str) -> Path:
    """Resolve a required repository path or terminate with a CLI error."""

    if path is None:
        raise SystemExit(f"missing required {flag_name}")
    resolved = resolve_repo_path(path)
    if resolved is None:
        raise SystemExit(f"missing required {flag_name}")
    return resolved


def resolve_output(path: Path | None, default: Path) -> Path:
    """Resolve an optional output path or use its stable default."""

    resolved = resolve_repo_path(path) if path is not None else default
    if resolved is None:
        raise SystemExit("internal error: output path unexpectedly empty")
    return resolved
