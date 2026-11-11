"""Capture a native job, export its SDK records, or interpret one recorded worker."""

import argparse
import json
from pathlib import Path

from markov_collection.artifacts import write_json


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    capture = commands.add_parser("capture", help="run one normalized native observation job")
    capture.add_argument("--input", type=Path, required=True)
    capture.add_argument("--output", type=Path, required=True)
    capture.add_argument("--resource-trace", action="store_true", help="observe resources, input reads and CPU protocol for independent comparison")
    interpret = commands.add_parser("interpret", help="interpret an SDK-exported worker recording")
    interpret.add_argument("--trace", type=Path, required=True)
    interpret.add_argument("--evidence", action="store_true", help="retain measured boundary clocks for source auditing")
    export = commands.add_parser("export", help="export one worker through an isolated SDK workspace")
    for command in (interpret, export):
        command.add_argument("--input", type=Path, required=True)
        command.add_argument("--rank", type=int, required=True)
        command.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    if arguments.command == "capture":
        from markov_collection.adapters.frameworks.sglang.session import NativeServingSession

        NativeServingSession(json.loads(arguments.input.read_text()), arguments.output, arguments.resource_trace).run()
    elif arguments.command == "export":
        from markov_collection.adapters.runtimes.ascend.export import AscendExport

        AscendExport.run(arguments.input, arguments.rank, arguments.output)
    else:
        from markov_collection.adapters.frameworks.sglang.kv_facts import NativeKvFacts
        from markov_collection.adapters.runtimes.ascend.observation import AscendObservation

        native = NativeKvFacts(arguments.input, arguments.rank)
        recording = native.document()
        observation = AscendObservation(arguments.input, arguments.rank, recording, arguments.trace)
        write_json(arguments.output, native.groups.project(observation.document(arguments.evidence)), replace=False)


if __name__ == "__main__":
    main()
