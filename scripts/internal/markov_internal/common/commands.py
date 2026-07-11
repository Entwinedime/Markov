"""Validation and rendering helpers for configured shell commands."""

from __future__ import annotations

import shlex
from typing import Any


def command_from_config(command: Any) -> list[str] | str:
    """Validate a configured command without changing its list/string form."""

    if isinstance(command, list) and all(isinstance(item, str) for item in command):
        return command
    if isinstance(command, str):
        return command
    raise TypeError("command must be either a string or a list of strings")


def command_to_text(command: list[str] | str) -> str:
    """Render a command as shell-readable text for audit artifacts."""

    if isinstance(command, list):
        return shlex.join(command)
    return command


def command_tokens(command: list[str] | str | None) -> list[str]:
    """Tokenize a command, returning an empty list for absent or invalid text."""

    if command is None:
        return []
    if isinstance(command, list):
        return list(command)
    try:
        return shlex.split(command)
    except ValueError:
        return []
