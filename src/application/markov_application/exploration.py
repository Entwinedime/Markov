"""Acquire representative structures, build assets and execute one bounded exploration."""

from __future__ import annotations

import argparse
import fcntl
import json
import shutil
import stat
import subprocess
import time
from collections.abc import Sequence
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Literal

from markov_collection.adapters.frameworks.sglang.acquisition import SGLangAcquisition
from markov_collection.artifacts import write_json

from markov_application.command import CommandFailed, terminate
from markov_application.environment import DockerEnvironment, NativeDeviceUnavailable
from markov_application.specification import read_specification, serving_input


class BudgetExceeded(RuntimeError):
    pass


class Exploration:
    def __init__(self, root: Path, directory: Path, specification: dict[str, Any], details: bool) -> None:
        self.root = root
        self.directory = directory
        self.specification = specification
        self.acquisition = SGLangAcquisition()
        self.environment = DockerEnvironment(
            root, directory, self.acquisition.service, specification["deployment"]["physical_devices"]
        )
        state = directory / "state.json"
        self.state = (
            json.loads(state.read_text())
            if state.exists()
            else {
                "status": "pending",
                "charged_ns": {"native": 0, "processing": 0},
                "peak_run_bytes": 0,
                "native_attempts": 0,
                "sources": {},
                "results": {},
                "failures": {},
                "pending": None,
                "details": details,
            }
        )
        if details and not self.state["details"]:
            raise ValueError("resume keeps the original output detail; start a new run for full diagnostics")
        self.details = self.state["details"]
        self.domains: dict[tuple[Any, ...], str] = {}
        self.domain_settings: dict[str, dict[str, Any]] = {}
        self.configurations: dict[str, str] = {}
        for configuration in specification["configurations"]:
            domain = self.acquisition.domain(specification["model"], configuration)
            name = self.domains.setdefault(tuple(domain.items()), f"domain-{len(self.domains)}")
            self.domain_settings[name] = domain
            self.configurations[configuration["id"]] = name

    def save(self) -> None:
        write_json(self.directory / "state.json", self.state, indent=2)

    def storage_bytes(self, retained: bool = False) -> int:
        total = 0
        for path in self.directory.rglob("*"):
            if retained and path.relative_to(self.directory).parts[0] == "work":
                continue

            # Producers atomically rename their temporary files while this sampler walks the run.
            try:
                record = path.stat(follow_symlinks=False)
            except FileNotFoundError:
                continue
            if stat.S_ISREG(record.st_mode):
                total += record.st_size
        return total

    def available(self, category: str) -> int:
        remaining = (
            self.specification["budget"][category + "_seconds"] * 1_000_000_000 - self.state["charged_ns"][category]
        )
        if remaining <= 2_000_000_000:
            raise BudgetExceeded(f"{category} time budget exhausted")
        return remaining

    def step(self, command: list[str], category: str) -> None:
        if category == "native":
            try:
                self.step(
                    [
                        "docker", "exec", str(self.environment.native),
                        *self.environment.limited(
                            self.acquisition.device_command(self.environment.devices), self.available("processing")
                        ),
                    ],
                    "processing",
                )
            except CommandFailed as error:
                raise NativeDeviceUnavailable(str(error)) from error
        reserved = self.available(category)

        # A lost launcher or unconfirmed cleanup keeps the reserved allocation charged.
        self.state["charged_ns"][category] += reserved
        self.state["pending"] = category
        if category == "native":
            self.state["native_attempts"] += 1
        self.save()
        started = time.monotonic_ns()
        log_path = self.directory / "logs/run.log"
        log_path.parent.mkdir(parents=True, exist_ok=True)
        process = None
        completed = False
        try:
            with log_path.open("a") as log:
                log.write(json.dumps({"category": category, "command": command}) + "\n")
                log.flush()
                output_begin = log.tell()
                process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
                while process.poll() is None:
                    current_bytes = self.storage_bytes()
                    self.state["peak_run_bytes"] = max(self.state["peak_run_bytes"], current_bytes)
                    if current_bytes > self.specification["budget"]["temporary_bytes"]:
                        raise BudgetExceeded("temporary storage budget exceeded")
                    if time.monotonic_ns() - started > reserved:
                        raise BudgetExceeded(f"{category} time budget exhausted")
                    try:
                        process.wait(timeout=1)
                    except subprocess.TimeoutExpired:
                        pass
            completed = process.returncode == 0
        finally:
            try:
                if process is not None:
                    terminate(process)
            finally:
                if not completed:
                    self.environment.cleanup()

            elapsed = time.monotonic_ns() - started
            self.state["charged_ns"][category] += elapsed - reserved
            self.state["pending"] = None
            self.save()
        if elapsed > reserved or process.returncode == 124:
            raise BudgetExceeded(f"{category} time budget exhausted")

        # A timeout's forced kill also exits 137; use the refunded balance to distinguish budget exhaustion.
        if process.returncode == 137:
            self.available(category)
        if process.returncode:
            raise CommandFailed(process.returncode, log_path, output_begin)

    def build_tool(self) -> None:

        # Reuse the CLI build, while the SDK-dependent observer stays private to this run.
        self.step(
            self.environment.build_command(
                self.environment.build,
                [],
                ["markov"],
                self.available("processing"),
            ),
            "processing",
        )

    def prepare_native(self) -> dict[str, str]:
        self.environment.start()
        model_configuration = subprocess.check_output(
            [
                "docker",
                "exec",
                str(self.environment.native),
                *self.acquisition.model_command(self.specification["model"]["path"]),
            ],
            text=True,
            timeout=30,
        )
        self.model_configuration = json.loads(model_configuration)
        path = self.directory / "inputs/model.json"
        if path.exists():
            if json.loads(path.read_text()) != self.model_configuration:
                raise ValueError("model configuration changed; start a new run")
        else:
            write_json(path, self.model_configuration, indent=2)

        # Production belongs to each capture; later preparation cannot relabel earlier native facts.
        self.native_image = subprocess.check_output(
            ["docker", "inspect", "--format", "{{.Image}}", str(self.environment.native)], text=True, timeout=30
        ).strip()

        invalid = self.acquisition.check_workloads(self.model_configuration, self.specification)
        self.state["failures"].update(invalid)
        expected = len(self.specification["configurations"]) * len(self.specification["workloads"])
        if len(invalid) == expected:
            return invalid

        for command in self.acquisition.observer_commands(
            self.environment.native_path(self.environment.observer_build)
        ):
            self.step(
                [
                    "docker",
                    "exec",
                    str(self.environment.native),
                    *self.environment.limited(command, self.available("processing")),
                ],
                "processing",
            )
        return invalid

    def capture_request(self, domain: dict[str, Any], rank: int, capture: Path) -> dict[str, Any]:
        request = self.acquisition.request(
            self.specification,
            domain,
            rank,
            self.environment.native_path(capture),
            self.environment.native_path(self.environment.observer_build / self.acquisition.selector_binary),
            self.model_configuration,
        )
        request["native_image"] = self.native_image
        return request

    def capture_source(self, name: str, domain: dict[str, Any], representative: int, roles: Sequence[int]) -> Path:
        key = f"{name}-source-{representative}"
        if key not in self.state["sources"]:
            self.available("native")

            # One source bundle cannot mix producer images; target captures have independent provenance.
            if self.state["sources"] and self.state["source_image"] != self.native_image:
                raise ValueError("source producer image changed; acquire the affected source bundle in a new task")
            self.state["source_image"] = self.native_image
            capture = self.directory / "captures" / f"{key}-attempt-{self.state['native_attempts'] + 1}"

            # A device preflight can stop before spending an Engine attempt, leaving only its request file.
            capture.mkdir(parents=True, exist_ok=True)
            request = self.capture_request(domain, roles[0], capture)
            write_json(capture / "capture.json", request, indent=2)
            command = self.acquisition.command("capture", self.environment.native_path(capture / "capture.json"))
            self.step(
                self.environment.native_command(
                    command, capture, self.acquisition.observer_library, self.available("native")
                ),
                "native",
            )
            self.state["sources"][key] = str(capture.relative_to(self.directory))
            self.save()

        return self.source_path(key)

    def source_path(self, key: str) -> Path:
        capture = (self.directory / self.state["sources"][key]).resolve()
        capture.relative_to(self.directory)
        return capture

    def source_plan(self, name: str, domain: dict[str, Any], bootstrap: Path) -> tuple[Path, dict[str, Any]]:
        path = self.directory / "inputs/role-plans" / (name + ".json")
        if path.is_file():
            return path, json.loads(path.read_text())

        plan = self.acquisition.role_plan(domain, bootstrap)
        write_json(path, plan, indent=2)
        return path, plan

    def sources_ready(self, name: str, domain: dict[str, Any]) -> bool:
        key = f"{name}-source-0"
        if key not in self.state["sources"]:
            return False
        _, plan = self.source_plan(name, domain, self.source_path(key))
        return all(
            f"{name}-source-{representative}" in self.state["sources"]
            for representative, _ in enumerate(self.acquisition.source_groups(domain, plan))
        )

    def asset_path(self, name: str) -> Path:
        return self.directory / "assets" / name / "templates.json"

    def collect_sources(self, name: str, domain: dict[str, Any]) -> None:

        # Choose later representatives only after the first native session exposes actual partitions.
        bootstrap = self.capture_source(name, domain, 0, next(self.acquisition.source_groups(domain)))
        _, plan = self.source_plan(name, domain, bootstrap)
        for representative, roles in enumerate(self.acquisition.source_groups(domain, plan)):
            self.capture_source(name, domain, representative, roles)

    def build_templates(self, name: str, domain: dict[str, Any]) -> None:
        library = self.asset_path(name)
        if library.is_file():
            return
        draft = self.directory / "work/sources" / name
        draft.mkdir(parents=True, exist_ok=True)
        arguments = ["templates"]
        inputs = []

        # Production consumes published facts and cannot start another Engine.
        bootstrap = self.source_path(f"{name}-source-0")
        instances, plan = self.source_plan(name, domain, bootstrap)
        arguments += ["--instances", str(instances)]
        inputs.append(instances)
        for representative, roles in enumerate(self.acquisition.source_groups(domain, plan)):
            capture = self.source_path(f"{name}-source-{representative}")
            interpreted = draft / capture.name
            interpreted.mkdir(parents=True, exist_ok=True)
            command = self.acquisition.command(
                "interpret", self.environment.native_path(capture), self.environment.native_path(interpreted)
            )
            self.step(
                self.environment.native_command(
                    command, interpreted, None, self.available("processing")
                ),
                "processing",
            )
            for rank in roles:
                source = interpreted / self.acquisition.source_name(domain, rank)
                physical = interpreted / self.acquisition.physical_name(domain, rank)
                arguments += ["--source", str(source), "--physical", str(physical)]
                inputs += [source, physical]

        # Compile all roles against one catalog, keeping each role's resource and execution bindings independent.
        output = draft / library.name
        arguments += ["--output", str(output)]
        self.step(
            self.environment.tool_command(arguments, inputs, output, self.available("processing")),
            "processing",
        )

        # The single catalog contains every role; publish that file without moving interpretation data into assets.
        library.parent.mkdir(parents=True, exist_ok=True)
        output.replace(library)

    def release_exports(self, domains: dict[str, dict[str, Any]]) -> None:
        if self.details:
            return
        for name, domain in domains.items():
            for key, relative in self.state["sources"].items():
                if not key.startswith(name + "-source-"):
                    continue
                for parent in self.acquisition.observation_directories(domain, self.directory / relative):
                    parent = parent.resolve()
                    parent.relative_to(self.directory)
                    for child in ("native", "runtime"):
                        path = parent / child
                        if path.exists():
                            shutil.rmtree(path)

    def simulate(
        self, asset: Path, target: Path, result: Path, details: bool, *,
        control: str | None = None, completion_times: Path | None = None,
    ) -> None:
        arguments = ["simulate", "--templates", str(asset), "--input", str(target), "--output", str(result)]
        if details:
            arguments.append("--details")
        remaining = self.available("processing")
        selected = self.specification.get("control", "lightweight") if control is None else control
        if completion_times is not None:
            if selected != "native":
                raise ValueError("observed completion times require native control")
            arguments += ["--completion-times", str(completion_times)]
        if selected == "native":
            command = [
                "bash",
                str(self.root / "scripts/markov-control.sh"),
                *arguments[1:],
                "--timeout-seconds",
                str(remaining / 1e9),
                "--task-directory",
                str(self.directory),
            ]
        else:
            command = self.environment.tool_command(arguments, [asset, target], result, remaining)
        self.step(command, "processing")

    def predict(self) -> None:
        for configuration in self.specification["configurations"]:
            domain = self.configurations[configuration["id"]]
            asset = self.asset_path(domain)
            for workload in self.specification["workloads"]:
                key = f"{configuration['id']}/{workload['id']}"
                if key in self.state["failures"]:
                    continue
                if key in self.state["results"] and (self.directory / self.state["results"][key]).exists():
                    continue
                target = self.directory / "inputs/targets" / (key + ".json")
                write_json(target, serving_input(configuration, workload, self.specification["node_durations_ns"]))
                base = "diagnostics" if self.details else "results"
                result = self.directory / base / (key + ".json")
                try:
                    self.simulate(asset, target, result, self.details)
                    self.state["results"][key] = str(result.relative_to(self.directory))
                    self.state["failures"].pop(key, None)
                    self.save()
                except BudgetExceeded:
                    raise
                except RuntimeError as error:
                    self.state["failures"][key] = str(error)

    def report(self) -> None:
        configurations = {item["id"]: item for item in self.specification["configurations"]}
        workloads = {item["id"]: item for item in self.specification["workloads"]}
        lines = [
            "# 条件执行结果",
            "",
            "节点耗时为显式假设；任务完成不表示结构独立验收或性能精度通过。",
            "",
            "请求完成仅指控制流程结束；仿真结果仍保守标为 partial，来源覆盖和适用条件由 structure 单独说明。",
            "",
            f"模型：{self.specification['model']['path']}，{self.specification['model']['dtype']}。",
            "",
            f"任务状态：{self.state['status']}；sources_complete和templates_complete分别只表示对应准备阶段完成。",
            "",
            f"来源实现域 {len(self.domain_settings)} 个，已保存来源会话 {len(self.state['sources'])} 份。",
            "",
            "| 配置 / 请求集 | 结果 | 模式 | TP | PP | EP | 输入请求 |",
            "| --- | --- | --- | ---: | ---: | ---: | ---: |",
        ]
        for key, path in self.state["results"].items():
            configuration, workload = key.split("/")
            link = Path("..") / path
            mode = "PD" if "pd" in configurations[configuration] else "共同实例"
            pools = (
                [configurations[configuration]["pd"][role] for role in ("prefill", "decode")]
                if mode == "PD"
                else [configurations[configuration]]
            )
            tp = "/".join(str(pool["tp"]) for pool in pools)
            pp = "/".join(str(pool.get("pp", 1)) for pool in pools)
            ep = "/".join(str(pool.get("ep", {}).get("size", "—")) for pool in pools)
            lines.append(
                f"| {key} | [请求完成]({link}) | {mode} | {tp} | {pp} | {ep} | {len(workloads[workload]['requests'])} |"
            )
        if self.state["failures"]:
            lines += ["", "未完成项：", ""]
            lines += [f"- {key}: {reason}" for key, reason in self.state["failures"].items()]
        lines += [
            "",
            f"原生启动尝试 {self.state['native_attempts']} 次，当前可复用来源会话 {len(self.state['sources'])} 份；"
            f"累计原生 {self.state['charged_ns']['native'] / 1e9:.3f} 秒，"
            f"处理 {self.state['charged_ns']['processing'] / 1e9:.3f} 秒。",
            "",
        ]
        output = self.directory / "results/summary.md"
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text("\n".join(lines))

    def run(self, stop_after: Literal["sources", "templates", "predictions"] = "predictions") -> None:
        expected = len(self.specification["configurations"]) * len(self.specification["workloads"])
        if (
            self.state["status"] == "complete"
            and len(self.state["results"]) == expected
            and all((self.directory / path).is_file() for path in self.state["results"].values())
            and all(self.asset_path(name).is_file() for name in self.domain_settings)
        ):
            self.report()
            return
        self.state["status"] = "running"
        self.state["failures"] = {}
        self.save()
        domains = self.domain_settings

        # The run lock is held; reclaim only this task's interrupted workers before doing new work.
        try:
            self.environment.cleanup()
            pending_assets = {
                name: domain for name, domain in self.domain_settings.items()
                if not self.asset_path(name).is_file()
            }
            pending_sources = {
                name: domain for name, domain in pending_assets.items() if not self.sources_ready(name, domain)
            }
            if pending_sources:
                self.prepare_native()
            else:
                model = json.loads((self.directory / "inputs/model.json").read_text())
                self.state["failures"].update(
                    self.acquisition.check_workloads(model, self.specification)
                )
            needed = {
                self.configurations[configuration["id"]]
                for configuration in self.specification["configurations"]
                for workload in self.specification["workloads"]
                if f"{configuration['id']}/{workload['id']}" not in self.state["failures"]
            }
            domains = {name: domain for name, domain in self.domain_settings.items() if name in needed}
            for name, domain in domains.items():
                if name in pending_sources:
                    self.collect_sources(name, domain)

            if stop_after != "sources":
                if needed and (pending_assets or stop_after == "predictions"):
                    self.build_tool()
                if pending_assets and needed and self.environment.native is None:

                    # Interpretation reuses native facts with the SDK, without another acquisition setup.
                    self.environment.start()
                for name, domain in domains.items():
                    self.build_templates(name, domain)

                # Publication may precede interruption; reclaim consumed exports on recovery as well.
                self.release_exports(domains)
                if stop_after == "predictions":
                    self.predict()

            self.state["status"] = (
                {"sources": "sources_complete", "templates": "templates_complete", "predictions": "complete"}[
                    stop_after
                ]
                if not self.state["failures"] else "partial"
            )
            if self.storage_bytes(retained=True) > self.specification["budget"]["retained_bytes"]:
                raise BudgetExceeded("retained storage budget exceeded")
        except BaseException as error:
            self.state["status"] = "interrupted" if isinstance(error, KeyboardInterrupt) else "failed"
            self.state["failures"]["task"] = str(error)
            raise
        finally:
            try:
                self.environment.cleanup()
                work = self.directory / "work"

                # Source production owns its subtrees, never pending validation or other consumers' work.
                for name in domains:
                    source_work = work / "sources" / name
                    if self.asset_path(name).is_file() and source_work.exists():
                        shutil.rmtree(source_work)
                if domains and all(self.asset_path(name).is_file() for name in domains):
                    for preparation in (self.environment.observer_build, work / "triton-cache"):
                        if preparation.exists():
                            shutil.rmtree(preparation)
            except BaseException as error:
                self.state["status"] = "interrupted" if isinstance(error, KeyboardInterrupt) else "failed"
                self.state["failures"]["cleanup"] = str(error)
                raise
            finally:
                self.save()
                self.report()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path)
    parser.add_argument("--resume", type=Path)
    parser.add_argument("--details", action="store_true")
    parser.add_argument("--stop-after", choices=("sources", "templates", "predictions"), default="predictions")
    arguments = parser.parse_args()
    if arguments.input is None and arguments.resume is None:
        parser.error("provide --input or --resume")
    root = Path.cwd()
    specification = read_specification(arguments.input or arguments.resume / "inputs/task.json")
    if arguments.resume is None:
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        directory = root / "data" / f"{stamp}_automatic-graph"
        directory.mkdir(parents=True)
    else:
        directory = arguments.resume.resolve()
        directory.relative_to(root / "data")
    with (directory / ".lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        previous = directory / "inputs/task.json"
        changed_durations = False
        old = None
        if previous.exists():
            old = json.loads(previous.read_text())
            old_fixed = {key: value for key, value in old.items() if key not in ("budget", "node_durations_ns")}
            new_fixed = {
                key: value for key, value in specification.items() if key not in ("budget", "node_durations_ns")
            }
            if old_fixed != new_fixed:
                raise ValueError("resume keeps the model, configurations, workloads and deployment fixed")
            changed_durations = old["node_durations_ns"] != specification["node_durations_ns"]
        task = Exploration(root, directory, specification, arguments.details)
        print(directory, flush=True)
        if changed_durations:
            results = task.state["results"]
            task.state["results"] = {}
            task.state["status"] = "pending"

            # Invalidate every published result before changing input or deleting any file.
            task.save()
            for path in results.values():
                owned = (directory / path).resolve()
                owned.relative_to(directory)
                owned.unlink(missing_ok=True)
        if old != specification:
            write_json(previous, specification, indent=2)
        task.run(stop_after=arguments.stop_after)
        print(directory / "results/summary.md")
        if task.state["status"] not in {"sources_complete", "templates_complete", "complete"}:
            raise SystemExit(2)


if __name__ == "__main__":
    main()
