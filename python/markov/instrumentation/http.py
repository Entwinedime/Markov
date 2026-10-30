"""Collect serial requests through the inference adapters' common HTTP contract."""

from __future__ import annotations

import json
from contextlib import contextmanager
import os
from pathlib import Path
import signal
import subprocess
import time
from typing import Iterator
import urllib.error
import urllib.request

from markov.artifacts.writing import write_json


def encode_inputs(requests: list[dict], model_path: str, input_budget: int, trust_remote_code: bool = False) -> None:
    """Fill text requests in place before startup, within the remaining token budget."""
    tokenizer = None
    if any(row["prompt"] is not None for row in requests):
        from transformers import AutoTokenizer

        tokenizer = AutoTokenizer.from_pretrained(
            model_path, local_files_only=True, trust_remote_code=trust_remote_code
        )

    for row in requests:
        if row["input_ids"] is None:
            row["input_ids"] = tokenizer.encode(row["prompt"])

    if sum(len(row["input_ids"]) for row in requests) > input_budget:
        raise ValueError("The tokenized workload exceeds the remaining input-token budget")


class SerialHttpClient:
    def __init__(self, directory: Path, formal_begin_index: int, deadline_sec: float) -> None:
        self.directory = directory
        self.formal_begin_index = formal_begin_index
        self.deadline_sec = deadline_sec
        self.client = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        self.rows = []

    def remaining_sec(self) -> float:
        value = self.deadline_sec - time.monotonic()
        if value <= 0:
            raise TimeoutError("Capture wall-time budget reached")
        return value

    def http(self, route: str, body: dict | None = None) -> tuple[bytes, int, int]:
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(
            "http://127.0.0.1:30000" + route, data=data, headers={"Content-Type": "application/json"}
        )
        begin_ns = time.time_ns()
        with self.client.open(request, timeout=self.remaining_sec()) as response:
            payload = response.read()
        end_ns = time.time_ns()
        return payload, begin_ns, end_ns

    def request(self, ordinal: int, item: dict) -> None:
        identity = f"request_{ordinal}"
        body = {
            "rid": identity,
            "input_ids": item["input_ids"],
            "return_logprob": item["return_logprob"],
            "sampling_params": {
                "temperature": 0,
                "max_new_tokens": item["max_new_tokens"],
                "ignore_eos": item["ignore_eos"],
            },
        }
        row = {
            "kind": "request",
            "logical_request_id": identity,
            "measure": ordinal >= self.formal_begin_index,
            "input_tokens": len(item["input_ids"]),
            "max_new_tokens": item["max_new_tokens"],
            "status": "running",
        }
        self.rows.append(row)
        write_json(self.directory / "workload_report.json", {"requests": self.rows})
        try:
            payload, begin_ns, end_ns = self.http("/generate", body)
            response = json.loads(payload)
        except Exception as error:
            row.update(status="error", error=str(error))
            write_json(self.directory / "workload_report.json", {"requests": self.rows})
            raise
        row.update(
            status="ok",
            start_time_ms=self.milliseconds(begin_ns),
            end_time_ms=self.milliseconds(end_ns),
            generated_tokens=response["meta_info"]["completion_tokens"],
            response=response,
        )

        finish = response["meta_info"]["finish_reason"]
        if finish["type"] in ("abort", "error") or (
            item["ignore_eos"] and row["generated_tokens"] != item["max_new_tokens"]
        ):
            row.update(status="error", error="The runtime aborted or changed the requested fixed output count")

        write_json(self.directory / "workload_report.json", {"requests": self.rows})
        if row["status"] == "error":
            raise ValueError(row["error"])

    @staticmethod
    def milliseconds(ns: int) -> str:
        return f"{ns // 1_000_000}.{ns % 1_000_000:06d}"


@contextmanager
def inference_server(command: list[str], log_path: Path, client: SerialHttpClient) -> Iterator[bytes]:
    """Own the server process group through readiness, work and bounded shutdown."""
    with log_path.open("w", encoding="utf-8") as log:
        server = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            while True:
                if server.poll() is not None:
                    raise RuntimeError(f"Inference server exited with {server.returncode}; see server.log")
                try:
                    payload, _, _ = client.http("/model_info")
                    break
                except (urllib.error.URLError, TimeoutError):
                    time.sleep(min(0.5, client.remaining_sec()))

            yield payload
        finally:
            if server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    os.killpg(server.pid, signal.SIGKILL)
                    server.wait(timeout=15)
