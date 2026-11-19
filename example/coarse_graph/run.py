"""Launch the formal exploration task with the repository example inputs."""

import argparse
import json
import shutil
import subprocess
from pathlib import Path


class TaskCommands:
    """Call a task module from a coordinator sharing the host daemon's paths."""

    def __init__(self, repository, images):
        self.repository, self.images = repository, images

    def command(self, module, *arguments, model=None, files=()):

        # The coordinator and host daemon must see the same absolute paths for child mounts.
        mounts = [(self.repository, self.repository, False)]
        if model is not None:
            destination = Path(model).absolute()
            mounts.append((destination.resolve(strict=True), destination, True))
            for name in ("/usr/local/Ascend/driver", "/etc/ascend_install.info", "/etc/hccn.conf"):
                mounts.append((Path(name).resolve(strict=True), Path(name), True))
        mounts.extend((path, path, readonly) for path, readonly in files if not path.is_relative_to(self.repository))
        mounts.extend([
            (Path("/var/run/docker.sock"), Path("/var/run/docker.sock"), False),
            (Path(shutil.which("docker")).resolve(strict=True), Path("/usr/bin/docker"), True),
        ])
        command = ["docker", "run", "--rm", "--init", "-i", "--runtime", "runc", "--network", "none"]
        for source, destination, readonly in mounts:
            command.extend(["--mount", f"type=bind,src={source},dst={destination}" + (",readonly" if readonly else "")])
        command.extend([
            "--workdir", str(self.repository),
            "--env", "PYTHONPATH=src/workflow:src/verification:src/adapters:src/collection:src/workload",
            "--entrypoint", "python3", self.images["control"], "-m", module,
        ])
        return command + [str(value) for value in arguments]


def main():
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--input", type=Path, default=Path(__file__).with_name("input.json"))
    parser.add_argument("--deployment", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--reuse", type=Path)
    parser.add_argument("--binary", type=Path, default=repository / "build/coarse-graph/markov")
    parser.add_argument("--source-image", default="markov-next-sglang:ubuntu22.04")
    parser.add_argument("--modeling-image", default="markov-next-modeling:ubuntu24.04")
    parser.add_argument("--control-image", default="markov-next-control:ubuntu22.04")
    options, remaining = parser.parse_known_args()
    images = {role: getattr(options, f"{role}_image") for role in ("source", "control", "modeling")}
    commands = TaskCommands(repository, images)
    if "--help" in remaining or "-h" in remaining:
        raise SystemExit(subprocess.call(commands.command("markov_workflow", "explore", "--help")))
    if options.deployment is None:
        parser.error("--deployment is required")

    arguments, files = [], {}
    for name in ("input", "deployment", "binary", "reuse", "output"):
        path = getattr(options, name)
        if path is None:
            continue
        path = path.resolve(strict=name != "output")
        mounted = path.parent if name == "output" else path
        if name == "output":
            mounted.mkdir(parents=True, exist_ok=True)
        files[mounted] = name != "output"
        arguments.extend(("--" + name, path))
    model = json.loads(options.input.read_text())["model"]["path"]
    for role, image in images.items():
        arguments.extend(("--" + role + "-image", image))
    command = commands.command("markov_workflow", "explore", *arguments, *remaining, model=model, files=files.items())
    raise SystemExit(subprocess.call(command))


if __name__ == "__main__":
    main()
