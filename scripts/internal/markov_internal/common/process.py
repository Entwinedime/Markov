"""HTTP requests and subprocess lifecycle helpers for internal workflows."""

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
    """Extract the API origin from an absolute readiness URL."""

    parsed = urllib.parse.urlsplit(ready_url)
    if not parsed.scheme or not parsed.netloc:
        raise ValueError(f"ready_url must be absolute: {ready_url}")
    return urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, "", "", ""))


def post_json(
    url: str,
    body: dict[str, Any] | None,
    timeout: int = 60,
    api_key: str | None = None,
) -> Any:
    """Send a JSON POST request and decode JSON or text responses."""

    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"
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
    """Wait for server readiness while detecting an early process exit."""

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
    """Start a process group and redirect both output streams to ``log_path``.

    ``start_new_session`` creates the process group without running Python code
    between ``fork`` and ``exec``, which keeps this helper safe when callers use
    threads.
    """

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
            start_new_session=True,
        )
    finally:
        # Popen duplicates the descriptor, so the parent can close its copy and
        # avoid leaking one descriptor for every long-running suite process.
        log_file.close()


def stop_process(process: subprocess.Popen[Any] | None, timeout_sec: int = 20) -> None:
    """Terminate a process group and escalate to SIGKILL after the timeout."""

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


def run_command(
    command: list[str],
    *,
    log_path: Path | None = None,
    capture_output: bool = False,
) -> subprocess.CompletedProcess[str]:
    """Run a one-shot repository command with one explicit output policy.

    Exactly one of ``log_path`` and ``capture_output`` may be selected. Callers
    receive the completed process and retain responsibility for interpreting a
    nonzero return code.
    """

    if log_path is not None and capture_output:
        raise ValueError("log_path and capture_output are mutually exclusive")
    if log_path is None:
        return subprocess.run(
            command,
            cwd=ROOT_DIR,
            text=True,
            capture_output=capture_output,
            check=False,
        )

    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8") as log_file:
        return subprocess.run(
            command,
            cwd=ROOT_DIR,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
