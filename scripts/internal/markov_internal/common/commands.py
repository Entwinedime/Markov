"""Command-line value parsing and configured shell command handling."""

from __future__ import annotations

import argparse
import shlex
from typing import Any


def positive_int(raw: str) -> int:
    """Parse a strictly positive integer for argparse."""

    value = int(raw)
    if value < 1:
        raise argparse.ArgumentTypeError("expected a positive integer")
    return value


def nonnegative_int(raw: str) -> int:
    """Parse a non-negative integer for argparse."""

    value = int(raw)
    if value < 0:
        raise argparse.ArgumentTypeError("expected a non-negative integer")
    return value


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
    """Tokenize a command; absent input is empty, malformed shell text raises ValueError."""

    if command is None:
        return []
    if isinstance(command, list):
        return list(command)
    return shlex.split(command)


def replace_command_option(tokens: list[str], option: str, value: str) -> None:
    """Set a value option in place, appending it when absent.

    The caller establishes that the option takes a value. Support both
    ``--option value`` and ``--option=value``, with argparse's last-value semantics.
    """

    index = max((i for i, token in enumerate(tokens) if token.split("=", 1)[0] == option), default=-1)
    if index < 0:
        tokens.extend((option, value))
    elif "=" in tokens[index]:
        tokens[index] = f"{option}={value}"
    else:
        tokens[index + 1] = value
