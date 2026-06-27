"""内部脚本共用的名称规整工具。"""

from __future__ import annotations

import re


def sanitize(value: str) -> str:
    """把用户可配置名称规整成可作为目录名的短字符串。"""

    value = re.sub(r"[^A-Za-z0-9_.-]+", "_", value.strip())
    return value.strip("._-") or "profile"
