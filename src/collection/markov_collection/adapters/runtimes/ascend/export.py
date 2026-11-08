"""Run the SDK in disposable storage, keeping captured records immutable."""

import os
import shutil
from pathlib import Path


class AscendExport:
    @staticmethod
    def command(rank: int, output: str) -> list[str]:
        return [
            "/opt/markov/export-profile.sh", "--input", "/source", "--rank", str(rank),
            "--output", output,
        ]

    @staticmethod
    def run(source: Path, rank: int, output: Path) -> None:
        from torch_npu.profiler.profiler import analyse

        profile, = (source / "native" / f"rank-{rank}").glob("*_ascend_pt")
        output.mkdir()
        working = output / profile.name
        working.mkdir()
        try:

            # SDK analysis writes alongside its inputs; copy raw framework/CANN records, not prior exports.
            for metadata in profile.glob("profiler_*.json"):
                shutil.copyfile(metadata, working / metadata.name)
            shutil.copytree(profile / "FRAMEWORK", working / "FRAMEWORK")
            cann, = profile.glob("PROF_*")
            for directory in (cann / "host", *cann.glob("device_*")):
                shutil.copytree(directory, working / cann.name / directory.name)
            analyse(str(working), max_process_number=1)

            # SDK exit alone is insufficient; interpretation will check the exported queue and marker evidence.
            os.link(working / "ASCEND_PROFILER_OUTPUT/trace_view.json", output / "trace.json")
        finally:
            shutil.rmtree(working)
