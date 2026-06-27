"""内部脚本共用的 HTTP 与子进程生命周期工具。"""

from __future__ import annotations

import json
import os
import signal
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

from .paths import ROOT_DIR


def api_base_from_ready_url(ready_url: str) -> str:
    """从 ready URL 提取 API base URL。"""

    parsed = urllib.parse.urlsplit(ready_url)
    if not parsed.scheme or not parsed.netloc:
        raise ValueError(f"ready_url must be absolute: {ready_url}")
    return urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, "", "", ""))


def post_json(url: str, body: dict[str, Any] | None, timeout: int = 60) -> Any:
    """发送 JSON POST 并宽松解析响应。"""

    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(url, data=data, headers=headers, method="POST")
    with urllib.request.urlopen(request, timeout=timeout) as response:
        payload = response.read()
    if not payload:
        return None
    try:
        return json.loads(payload.decode("utf-8"))
    except json.JSONDecodeError:
        return payload.decode("utf-8", errors="replace")


def wait_for_ready(process: subprocess.Popen[Any], ready_url: str, timeout_sec: int) -> None:
    """等待 server ready，同时监控进程是否提前退出。"""

    start = time.monotonic()
    while True:
        if process.poll() is not None:
            raise RuntimeError(f"server exited before ready, code={process.returncode}")
        try:
            with urllib.request.urlopen(ready_url, timeout=5) as response:
                if 200 <= response.status < 500:
                    return
        except (urllib.error.URLError, TimeoutError):
            pass

        if time.monotonic() - start > timeout_sec:
            raise TimeoutError(f"server did not become ready within {timeout_sec}s")
        time.sleep(5)


def start_process(
    command: list[str] | str,
    log_path: Path,
    env: dict[str, str],
) -> subprocess.Popen[Any]:
    """启动子进程并把 stdout/stderr 写入日志文件。"""

    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_file = log_path.open("wb")
    try:
        return subprocess.Popen(
            command,
            cwd=ROOT_DIR,
            env=env,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            shell=isinstance(command, str),
            preexec_fn=os.setsid,
        )
    finally:
        # Popen 已经复制 fd，父进程这里可以关闭，避免长时间 suite 泄漏文件句柄。
        log_file.close()


def stop_process(process: subprocess.Popen[Any] | None, timeout_sec: int = 20) -> None:
    """终止进程组，超时后升级为 SIGKILL。"""

    if process is None or process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=timeout_sec)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()
    except ProcessLookupError:
        pass
