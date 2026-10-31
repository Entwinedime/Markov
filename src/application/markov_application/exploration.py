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
from typing import Any

from markov_collection.adapters.frameworks.sglang.acquisition import SGLangAcquisition
from markov_collection.artifacts import write_json

from markov_application.command import CommandFailed, terminate
from markov_application.environment import DockerEnvironment
from markov_application.specification import read_specification


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

    def prepare(self) -> None:
        self.environment.start()
        model_configuration = subprocess.check_output(
            [
                "docker",
                "exec",
                str(self.environment.native),
                "python3",
                "-c",
                "import json,sys; print(json.dumps(json.load(open(sys.argv[1]))))",
                str(Path(self.specification["model"]["path"]) / "config.json"),
            ],
            text=True,
            timeout=30,
        )
        observed = {
            "model_configuration": json.loads(model_configuration),
            "native_image": subprocess.check_output(
                ["docker", "inspect", "--format", "{{.Image}}", str(self.environment.native)], text=True, timeout=30
            ).strip(),
            "modeling_image": subprocess.check_output(
                ["docker", "image", "inspect", "--format", "{{.Id}}", self.environment.modeling_image],
                text=True,
                timeout=30,
            ).strip(),
        }
        path = self.directory / "inputs/environment.json"
        if path.exists() and json.loads(path.read_text()) != observed:
            raise ValueError("model configuration or execution images changed; start a new run")
        write_json(path, observed, indent=2)
        self.model_configuration = observed["model_configuration"]

        invalid = self.acquisition.check_workloads(observed["model_configuration"], self.specification)
        self.state["failures"].update(invalid)
        expected = len(self.specification["configurations"]) * len(self.specification["workloads"])
        if len(invalid) == expected:
            return

        self.build_tool()
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
        self.step(
            [
                "docker",
                "exec",
                str(self.environment.native),
                "python3",
                "-m",
                "pip",
                "install",
                "--no-deps",
                "--no-build-isolation",
                "-e",
                "/workspace/trace-sim",
            ],
            "processing",
        )

    def capture_source(self, name: str, domain: dict[str, Any], representative: int, roles: Sequence[int]) -> Path:
        key = f"{name}-source-{representative}"
        if key not in self.state["sources"]:
            self.available("native")
            capture = self.directory / "captures" / f"{key}-attempt-{self.state['native_attempts'] + 1}"

            # A device preflight can stop before spending an Engine attempt, leaving only its request file.
            capture.mkdir(parents=True, exist_ok=True)
            request = self.acquisition.request(
                self.specification,
                domain,
                roles[0],
                self.environment.native_path(capture),
                self.environment.native_path(self.environment.observer_build / self.acquisition.selector_binary),
                self.model_configuration,
            )
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

        capture = self.directory / self.state["sources"][key]
        capture.resolve().relative_to(self.directory)
        return capture

    def acquire(self, name: str, domain: dict[str, Any]) -> None:
        assets = self.directory / "assets" / name
        if assets.exists():
            return
        draft = self.directory / "work" / name
        draft.mkdir(parents=True, exist_ok=True)
        arguments = ["templates"]
        inputs = []

        # The mandatory first representative supplies native module partitions before choosing remaining sessions.
        bootstrap = self.capture_source(name, domain, 0, next(self.acquisition.source_groups(domain)))
        plan = self.acquisition.role_plan(domain, bootstrap)
        instances = self.directory / "inputs/role-plans" / (name + ".json")
        write_json(instances, plan, indent=2)
        arguments += ["--instances", str(instances)]
        inputs.append(instances)
        for representative, roles in enumerate(self.acquisition.source_groups(domain, plan)):
            capture = self.capture_source(name, domain, representative, roles)
            physical_paths = [capture / self.acquisition.physical_name(domain, rank) for rank in roles]
            command = self.acquisition.command("interpret", self.environment.native_path(capture / "capture.json"))
            self.step(
                self.environment.native_command(
                    command, capture, self.acquisition.observer_library, self.available("processing")
                ),
                "processing",
            )
            for rank, physical in zip(roles, physical_paths, strict=True):
                source = capture / self.acquisition.source_name(domain, rank)
                arguments += ["--source", str(source), "--physical", str(physical)]
                inputs += [source, physical]

        # Compile all roles against one catalog, keeping each role's resource and execution bindings independent.
        asset = draft / "templates.json"
        arguments += ["--output", str(asset)]
        self.step(
            self.environment.tool_command(arguments, inputs, asset, self.available("processing")),
            "processing",
        )

        # A domain becomes reusable only after every role's asset has been published together.
        assets.parent.mkdir(parents=True, exist_ok=True)
        draft.replace(assets)

    def simulate(self, asset: Path, target: Path, result: Path, details: bool) -> None:
        arguments = ["simulate", "--templates", str(asset), "--input", str(target), "--output", str(result)]
        if details:
            arguments.append("--details")
        remaining = self.available("processing")
        if self.specification.get("control", "lightweight") == "native":
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
            assets = self.directory / "assets" / domain
            asset = assets / "templates.json"
            for workload in self.specification["workloads"]:
                key = f"{configuration['id']}/{workload['id']}"
                if key in self.state["failures"]:
                    continue
                if key in self.state["results"] and (self.directory / self.state["results"][key]).exists():
                    continue
                target = self.directory / "inputs/targets" / (key + ".json")
                settings = {
                    name: configuration[name]
                    for name in (
                        "max_running_requests",
                        "max_total_tokens",
                        "page_size",
                        "max_prefill_tokens",
                        "prefill_chunk_tokens",
                        "mixed_batches",
                        "prefix_cache",
                        "pd",
                        "pp",
                        "ep",
                        "microbatch_requests",
                        "placement",
                    )
                    if name in configuration
                }
                inputs = {
                    "configuration": settings,
                    "requests": workload["requests"],
                    "node_durations_ns": self.specification["node_durations_ns"],
                }
                if "prefixes" in workload:
                    inputs["prefixes"] = workload["prefixes"]
                pools = configuration["pd"].values() if "pd" in configuration else (configuration,)
                if any("ep" in pool for pool in pools):
                    inputs["ep_routes"] = workload["ep_routes"]
                write_json(target, inputs)
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

    def run(self) -> None:
        expected = len(self.specification["configurations"]) * len(self.specification["workloads"])
        if (
            self.state["status"] == "complete"
            and len(self.state["results"]) == expected
            and all((self.directory / path).is_file() for path in self.state["results"].values())
            and (self.directory / "results/summary.md").is_file()
        ):
            return
        self.state["status"] = "running"
        self.state["failures"] = {}
        self.save()
        domains = self.domain_settings

        # The run lock is held; reclaim only this task's interrupted workers before doing new work.
        try:
            self.environment.cleanup()
            needs_sources = any(not (self.directory / "assets" / name).exists() for name in self.domain_settings)
            if needs_sources:
                self.prepare()
            else:
                observed = json.loads((self.directory / "inputs/environment.json").read_text())
                self.state["failures"].update(
                    self.acquisition.check_workloads(observed["model_configuration"], self.specification)
                )
                if len(self.state["failures"]) != expected:
                    self.build_tool()
            needed = {
                self.configurations[configuration["id"]]
                for configuration in self.specification["configurations"]
                for workload in self.specification["workloads"]
                if f"{configuration['id']}/{workload['id']}" not in self.state["failures"]
            }
            domains = {name: domain for name, domain in self.domain_settings.items() if name in needed}
            for name, domain in domains.items():
                self.acquire(name, domain)

            # Publication may precede interruption; reclaim consumed exports on recovery as well.
            if not self.details:
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
            self.predict()
            self.state["status"] = "complete" if not self.state["failures"] else "partial"
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

                # Published sources no longer need the SDK or observer build, including a partial target result.
                if work.exists() and all((self.directory / "assets" / name).exists() for name in domains):
                    shutil.rmtree(work)
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
        task.run()
        print(directory / "results/summary.md")
        if task.state["status"] != "complete":
            raise SystemExit(2)


if __name__ == "__main__":
    main()
