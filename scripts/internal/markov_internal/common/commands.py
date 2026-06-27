"""内部脚本共用的命令表达工具。"""

from __future__ import annotations

import shlex
from typing import Any


def command_from_config(command: Any) -> list[str] | str:
    """校验并返回配置中的命令表达。"""

    if isinstance(command, list) and all(isinstance(item, str) for item in command):
        return command
    if isinstance(command, str):
        return command
    raise TypeError("command must be either a string or a list of strings")


def command_to_text(command: list[str] | str) -> str:
    """把命令转换成可写入审计文件的文本形式。"""

    if isinstance(command, list):
        return shlex.join(command)
    return command


def command_tokens(command: list[str] | str | None) -> list[str]:
    """把命令规整为 token list，解析失败时返回空列表。"""

    if command is None:
        return []
    if isinstance(command, list):
        return list(command)
    try:
        return shlex.split(command)
    except ValueError:
        return []
