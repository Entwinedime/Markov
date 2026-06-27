"""内部脚本共用的轻量日志输出。"""

from __future__ import annotations

import time


def log(message: str) -> None:
    """输出带时间戳的 runner 日志。"""

    timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
    print(f"[{timestamp}] {message}", flush=True)
