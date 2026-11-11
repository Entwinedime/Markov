"""Run source planning, bounded acquisition and all exploration targets in one directory."""

import argparse
import json
import shutil
import subprocess
from datetime import datetime, timezone
from pathlib import Path


class WorkflowCommands:
    """Call the formal task CLI from a coordinator sharing the host daemon's paths."""

    def __init__(self, repository, images):
        self.repository, self.images = repository, images

    def command(self, action, *arguments, model=None):

        # The coordinator and host daemon must see the same absolute paths for child mounts.
        mounts = [(self.repository, self.repository, False)]
        if action in {"capture", "acquire-sources"}:
            model = Path(model).resolve(strict=True)
            mounts.append((model, model, True))
            for name in ("/usr/local/Ascend/driver", "/etc/ascend_install.info", "/etc/hccn.conf"):
                mounts.append((Path(name).resolve(strict=True), Path(name), True))
        mounts.extend([
            (Path("/var/run/docker.sock"), Path("/var/run/docker.sock"), False),
            (Path(shutil.which("docker")).resolve(strict=True), Path("/usr/bin/docker"), True),
        ])
        command = ["docker", "run", "--rm", "--init", "-i", "--network", "none"]
        for source, destination, readonly in mounts:
            command.extend(["--mount", f"type=bind,src={source},dst={destination}" + (",readonly" if readonly else "")])
        command.extend([
            "--workdir", str(self.repository),
            "--env", "PYTHONPATH=src/workflow:src/adapters:src/collection:src/workload",
            "--entrypoint", "python3", self.images["control"], "-m", "markov_workflow", action,
        ])
        return command + [str(value) for value in arguments]


class SourceProductions:
    """Read the published finite collection and preserve primary/local assembly roles."""

    def __init__(self, directory, plan):
        self.plan = json.loads((plan / "plan.json").read_text())
        publication = json.loads((directory / "result.json").read_text())
        if not len(self.plan["domains"]) == len(publication["productions"]) == len(publication["captures"]):
            raise ValueError("source publication does not match the planned domains")

        self.paths = {domain["id"]: directory / name
                      for domain, name in zip(self.plan["domains"], publication["productions"])}
        self.captures = {domain["id"]: directory / name
                         for domain, name in zip(self.plan["domains"], publication["captures"])}

    def arguments(self, primary):
        result = ["--production", self.paths[primary]]
        result.extend(value for identity, path in self.paths.items() if identity != primary for value in ("--local-production", path))
        return result


class ExplorationRun:
    def __init__(self, arguments):
        self.arguments = arguments
        self.repository = Path(__file__).resolve().parents[2]
        self.source = json.loads(arguments.input.read_text())
        self.source["model"]["path"] = str(Path(self.source["model"]["path"]).resolve(strict=True))
        self.deployment = json.loads(arguments.deployment.read_text())
        self.binary = arguments.binary.resolve(strict=True)
        self.images = {
            role: subprocess.check_output([
                "docker", "image", "inspect", name, "--format", "{{.Id}}",
            ], text=True).strip()
            for role, name in (
                ("source", "markov-next-sglang:ubuntu22.04"),
                ("control", "markov-next-control:ubuntu22.04"),
                ("modeling", "markov-next-modeling:ubuntu24.04"),
            )
        }
        self.commands = WorkflowCommands(self.repository, self.images)
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        self.output = self.repository / "data" / f"{stamp}_coarse-graph"
        self.results = []

    def execute(self, name, action, *arguments):
        command = self.commands.command(action, *arguments, model=self.source["model"]["path"])
        log = self.output / "logs" / f"{name}.log"
        log.with_suffix(".command.json").write_text(json.dumps(command, indent=2) + "\n")
        print(f"{name}: {log}", flush=True)
        with log.open("x") as stream:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(f"{name} exited with {result.returncode}; inspect {log.relative_to(self.output)}")

    def targets(self):
        for configuration in self.source["configurations"]:
            for workload in self.source["workloads"]:
                yield configuration["id"], workload["id"], {
                    "model": self.source["model"], "instances": configuration["instances"],
                    "workload": workload, "given_work_ns": self.arguments.given_work_ns,
                }

    def report(self, error):
        lines = [
            "# 执行结构示例", "",
            f"状态：{'失败，保留已完成步骤' if error else '全部目标展开完成'}。",
            f"已完成目标：{len(self.results)}。每工作统一给定 {self.arguments.given_work_ns} ns；这是结构开发输入，不是性能预测。",
            "来源由初始代表计划选择；目标完成不等于结构独立验证通过，也不保证未观察分支覆盖。", "",
        ]
        if error:
            lines.extend([f"错误：{error}", "", "检查 logs 中最后一步及其引用的任务日志；不会自动重试或补采。", ""])
        for index, result in enumerate(self.results):
            names = json.dumps([result["configuration"], result["workload"]], ensure_ascii=False)
            lines.append(f"- 目标 {index}（配置、workload）：{names}；{result['requests']} 请求，"
                         f"{result['outputs']} 次输出。[结果]({result['result']})")
        lines.extend(["", "规划见 plan/plan.md，采集额度见 collection/status.json，运行输入及镜像见 inputs/。", ""])
        (self.output / "summary.md").write_text("\n".join(lines))

    def run(self):
        args = self.arguments
        self.output.mkdir(parents=True)
        for name in ("inputs", "logs", "targets"):
            (self.output / name).mkdir()
        for name, value in (("exploration", self.source), ("deployment", self.deployment), ("images", self.images)):
            (self.output / "inputs" / f"{name}.json").write_text(json.dumps(value, indent=2) + "\n")

        # One executable snapshot serves production and every target, even if the build is later replaced.
        binary = self.output / "inputs/markov"
        shutil.copyfile(self.binary, binary)
        binary.chmod(0o500)
        print(f"Run directory: {self.output}", flush=True)
        try:
            self.execute("plan", "plan-sources", "--input", self.output / "inputs/exploration.json",
                         "--output", self.output / "plan")
            self.execute(
                "acquire", "acquire-sources", "--plan", self.output / "plan",
                "--deployment", self.output / "inputs/deployment.json", "--output", self.output / "collection",
                "--binary", binary, "--source-image", self.images["source"],
                "--modeling-image", self.images["modeling"],
                "--timeout", args.timeout, "--native-budget-seconds", args.native_budget_seconds,
                "--postprocessing-budget-seconds", args.postprocessing_budget_seconds,
                "--attempt-limit", args.attempt_limit, "--storage-bytes", args.storage_bytes,
                *(["--keep-intermediates", "--resource-trace"] if args.details else []),
            )
            collected = SourceProductions(self.output / "collection", self.output / "plan")
            for index, (configuration, workload, target) in enumerate(self.targets()):
                primary = collected.plan["targets"]["configurations"][configuration]
                directory = self.output / "targets" / str(index)
                directory.mkdir()
                task = directory / "input.json"
                task.write_text(json.dumps(target, indent=2) + "\n")
                self.execute(
                    f"predict-{index}", "predict", *collected.arguments(primary), "--input", task, "--output", directory / "prediction",
                    "--binary", binary, "--modeling-image", self.images["modeling"],
                    "--control-image", self.images["control"], "--timeout", args.timeout,
                    *(["--execution-trace"] if args.details else []),
                )
                result = json.loads((directory / "prediction/result.json").read_text())
                self.results.append({
                    "configuration": configuration, "workload": workload,
                    "requests": result["requests"], "outputs": len(result["outputs"]),
                    "result": f"targets/{index}/prediction/result.json",
                })
        except BaseException as error:
            self.report(f"{type(error).__name__}: {error}")
            raise

        # Publish the small index only after every target succeeded; raw output records stay in their task.
        publication = self.output / ".result.json"
        result = {"qualification": "given_duration_execution_only", "targets": self.results}
        publication.write_text(json.dumps(result, indent=2) + "\n")
        publication.rename(self.output / "result.json")
        self.report(None)
        print(f"Summary: {self.output / 'summary.md'}", flush=True)


def main():
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=Path(__file__).with_name("input.json"))
    parser.add_argument("--deployment", type=Path, required=True, help="physical devices and optional PD rendezvous")
    parser.add_argument("--binary", type=Path, default=repository / "build/coarse-graph/markov")
    parser.add_argument("--timeout", type=float, default=900, help="seconds per native attempt or target prediction")
    parser.add_argument("--native-budget-seconds", type=float, required=True)
    parser.add_argument("--postprocessing-budget-seconds", type=float, required=True)
    parser.add_argument("--attempt-limit", type=int, required=True)
    parser.add_argument("--storage-bytes", type=int, required=True, help="collection/ bytes; excludes predictions")
    parser.add_argument("--given-work-ns", type=int, default=1, help="uniform given duration, not measured performance")
    parser.add_argument("--details", action="store_true", help="retain intermediates, resource records and traces")
    arguments = parser.parse_args()
    if not 0 <= arguments.given_work_ns < 1 << 63:
        parser.error("--given-work-ns must be a nonnegative signed 64-bit integer")
    ExplorationRun(arguments).run()


if __name__ == "__main__":
    main()
