"""Acquire representative structures, build assets and execute one bounded exploration."""

from __future__ import annotations

import argparse
import fcntl
import json
import re
import shutil
import stat
import subprocess
import time
from collections import deque
from collections.abc import Sequence
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from markov_collection.adapters.frameworks.sglang.acquisition import SGLangAcquisition
from markov_collection.artifacts.json_file import write_json
from markov_collection.workflow.environment import DockerEnvironment


class BudgetExceeded(RuntimeError):
    pass


class CommandFailed(RuntimeError):
    def __init__(self, returncode: int, stderr: str, log: Path) -> None:
        super().__init__(f"command failed ({returncode}): {stderr}\nlog: {log}")
        self.returncode = returncode
        self.stderr = stderr


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

        # An interrupted launcher keeps its reservation charged; only a known return refunds the unused time.
        self.state["charged_ns"][category] += reserved
        self.state["pending"] = category
        if category == "native":
            self.state["native_attempts"] += 1
        self.save()
        started = time.monotonic_ns()
        log_path = self.directory / "logs/run.log"
        log_path.parent.mkdir(parents=True, exist_ok=True)
        try:
            with log_path.open("a") as log:
                log.write(json.dumps({"category": category, "command": command}) + "\n")
                log.flush()
                output_begin = log.tell()
                process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
                try:
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
                except BaseException:
                    self.environment.cleanup()
                    process.terminate()
                    process.wait()
                    raise
        finally:
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
            with log_path.open() as log:
                log.seek(output_begin)
                lines = (line.strip() for line in log)
                lines = (line for line in lines if line)
                first = next(lines, "")
                tail = deque(lines, maxlen=12)
            message = "\n".join(line[:600] for line in (first, *tail))
            raise CommandFailed(process.returncode, message, log_path)

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
                pools = configuration["pd"].values() if "pd" in configuration else (configuration,)
                if any("ep" in pool for pool in pools):
                    inputs["ep_routes"] = workload["ep_routes"]
                write_json(target, inputs)
                base = "diagnostics" if self.details else "results"
                result = self.directory / base / (key + ".json")
                arguments = ["simulate", "--templates", str(asset), "--input", str(target), "--output", str(result)]
                if self.details:
                    arguments.append("--details")
                try:
                    self.step(
                        self.environment.tool_command(arguments, [asset, target], result, self.available("processing")),
                        "processing",
                    )
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
            self.environment.cleanup()
            work = self.directory / "work"

            # Published sources no longer need the SDK or observer build, including a partial target result.
            if work.exists() and all((self.directory / "assets" / name).exists() for name in domains):
                shutil.rmtree(work)
            self.save()
            self.report()


def check_fields(source: dict[str, Any], fields: set[str], context: str) -> None:
    unknown = source.keys() - fields
    if unknown:
        raise ValueError(f"unsupported {context} fields: {sorted(unknown)}")


def read_specification(path: Path) -> dict[str, Any]:
    document = json.loads(path.read_text())
    check_fields(
        document,
        {"framework", "model", "deployment", "budget", "configurations", "workloads", "node_durations_ns"},
        "task",
    )
    check_fields(document["model"], {"path", "dtype"}, "model")
    check_fields(document["deployment"], {"physical_devices"}, "deployment")
    check_fields(
        document["budget"], {"native_seconds", "processing_seconds", "temporary_bytes", "retained_bytes"}, "budget"
    )
    if document["framework"] != "sglang":
        raise ValueError("this acquisition entrypoint currently provides SGLang/Ascend")
    deployment = document["deployment"]["physical_devices"]
    if (
        not isinstance(deployment, list)
        or not deployment
        or any(type(device) is not int or device < 0 for device in deployment)
        or len(set(deployment)) != len(deployment)
    ):
        raise ValueError("physical_devices must contain distinct nonnegative integers")
    for category in ("configurations", "workloads"):
        identities = [item["id"] for item in document[category]]
        if (
            not identities
            or len(set(identities)) != len(identities)
            or any(not re.fullmatch(r"[A-Za-z0-9_-]+", name) for name in identities)
        ):
            raise ValueError(f"{category} need nonempty, unique path-safe IDs")
    maximum = (1 << 63) - 1
    topologies = {}
    for configuration in document["configurations"]:
        pd = "pd" in configuration
        capacity = {"max_running_requests", "max_total_tokens", "max_prefill_tokens"}
        geometry = {"tp", "pp", "ep"}
        check_fields(
            configuration,
            {"id", "attention_backend", "page_size", "placement"}
            | ({"pd"} if pd else geometry | capacity | {"microbatch_requests"}),
            "configuration",
        )
        if type(configuration["page_size"]) is not int or not 0 < configuration["page_size"] <= maximum:
            raise ValueError("configuration page_size is outside its positive integer range")

        pools = [(configuration, capacity)]
        if pd:
            check_fields(configuration["pd"], {"prefill", "decode"}, "pd")
            pools = []
            for role in ("prefill", "decode"):
                fields = capacity if role == "prefill" else capacity - {"max_prefill_tokens"}
                pool = configuration["pd"][role]
                check_fields(pool, fields | geometry, "pd." + role)
                pools.append((pool, fields))

        ranks = 0
        for pool, fields in pools:
            for name in fields | {"tp"}:
                if type(pool[name]) is not int or not 0 < pool[name] <= maximum:
                    raise ValueError(f"configuration {name} is outside its positive integer range")
            if pool["max_total_tokens"] < configuration["page_size"]:
                raise ValueError("configuration KV capacity needs at least one page")
            pp = pool.get("pp", 1)
            if type(pp) is not int or not 0 < pp <= maximum:
                raise ValueError("configuration pp is outside its positive integer range")
            ranks += pool["tp"] * pp

            if "ep" in pool:
                ep = pool["ep"]
                check_fields(ep, {"id", "size"}, "ep")
                if (
                    not isinstance(ep["id"], str)
                    or not ep["id"]
                    or type(ep["size"]) is not int
                    or not 0 < ep["size"] <= maximum
                ):
                    raise ValueError("EP topology needs a nonempty ID and positive integer size")
                layout = pool["tp"], pp, ep["size"]
                if ep["id"] in topologies and topologies[ep["id"]] != layout:
                    raise ValueError("one EP topology ID cannot refer to different parallel layouts")
                topologies[ep["id"]] = layout

        if not pd:
            microbatch = configuration.get("microbatch_requests", (configuration["max_running_requests"] - 1) // pp + 1)
            if type(microbatch) is not int or not 0 < microbatch <= configuration["max_running_requests"]:
                raise ValueError("microbatch_requests must fit the request capacity")
        domain = SGLangAcquisition.domain(document["model"], configuration)
        if SGLangAcquisition.required_devices(domain) > len(deployment):
            raise ValueError("declared collection devices cannot run the required native source participants")

        if "placement" in configuration:
            placement = configuration["placement"]
            check_fields(placement, {"devices"}, "placement")
            devices = placement["devices"]
            if (
                not isinstance(devices, list)
                or len(devices) != ranks
                or any(type(device) is not int or not 0 <= device <= maximum for device in devices)
                or len(set(devices)) != len(devices)
            ):
                raise ValueError("placement needs one distinct nonnegative device per logical rank")

    # Reject malformed workloads before paying for a native source; per-configuration capacity remains a result.
    for workload in document["workloads"]:
        check_fields(workload, {"id", "requests", "ep_routes"}, "workload")
        routes = workload.get("ep_routes", {})
        if not isinstance(routes, dict) or routes.keys() - topologies.keys():
            raise ValueError("workload routes must be keyed by declared EP topology IDs")
        requests = workload["requests"]
        identities = [request["id"] for request in requests]
        if (
            not requests
            or any(not isinstance(name, str) or not name for name in identities)
            or len(set(identities)) != len(identities)
        ):
            raise ValueError("workload request IDs must be nonempty strings and unique")
        for request in requests:
            check_fields(request, {"id", "arrival_ns", "prompt_tokens", "output_tokens"}, "request")
            for name in ("arrival_ns", "prompt_tokens", "output_tokens"):
                value = request[name]
                minimum = 0 if name == "arrival_ns" else 1
                if type(value) is not int or not minimum <= value <= maximum:
                    raise ValueError(f"request {name} is outside its integer range: {request['id']}")
            if request["prompt_tokens"] + request["output_tokens"] - 1 > maximum:
                raise ValueError(f"request token length overflows: {request['id']}")

    durations = document["node_durations_ns"]
    if not isinstance(durations, dict) or any(
        type(value) is not int or not 0 <= value <= maximum for value in durations.values()
    ):
        raise ValueError("given node durations must contain nonnegative integer nanoseconds")

    for name in ("native_seconds", "processing_seconds", "temporary_bytes", "retained_bytes"):
        if type(document["budget"][name]) is not int or document["budget"][name] < 0:
            raise ValueError(f"budget {name} must be a nonnegative integer")
    return document


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
        if old != specification:
            write_json(previous, specification, indent=2)
        task = Exploration(root, directory, specification, arguments.details)
        print(directory, flush=True)
        if changed_durations:
            for path in task.state["results"].values():
                owned = (directory / path).resolve()
                owned.relative_to(directory)
                owned.unlink(missing_ok=True)
            task.state["results"] = {}
            task.state["status"] = "pending"
        task.run()
        print(directory / "results/summary.md")
        if task.state["status"] != "complete":
            raise SystemExit(2)


if __name__ == "__main__":
    main()
