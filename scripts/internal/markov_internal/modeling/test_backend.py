"""Public DAG entry forms share framework-neutral execution and diagnostic rules."""

from pathlib import Path
from contextlib import redirect_stderr
from io import StringIO
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

from ..common.paths import require_repo_path
from ..common.io import write_json
from .backend import build_trace_graph_command
from .run_config import ModelingRunConfig
from .runner import manifest_run_config, parse_args


class DagEntryTests(unittest.TestCase):
    def test_replay_does_not_silently_ignore_direct_options(self):
        for option in (
            ["--output-dir", "output"],
            ["--model-config", "node_scale.json"],
            ["--threads", "1"],
            ["--file-threads", "2"],
            ["--emit-dag"],
        ):
            with self.subTest(option=option), redirect_stderr(StringIO()), self.assertRaises(SystemExit) as error:
                parse_args(["--config", "runner.json", *option])
            self.assertEqual(error.exception.code, 2)

    def test_manifest_and_config_share_dag_options(self):
        # Both user entry forms must retain identical framework-neutral DAG behavior.
        with TemporaryDirectory(dir=require_repo_path(".")) as temporary:
            root = Path(temporary)
            manifest = root / "profile_manifest.json"
            config = root / "runner.json"
            output = root / "output"
            model = root / "node_scale.json"
            write_json(model, {"node_scale": {"enabled": True, "rules": [{"name": "compute", "factor": 2}]}})
            bench_report = root / "bench.jsonl"
            bench_report.write_text('{"duration": 1.25}\n', encoding="utf-8")
            write_json(manifest, {"bench": {"bench_serving_files": [{"path": str(bench_report)}]}})
            raw = {
                "input": {"profile_manifest": str(manifest)},
                "output_dir": str(output),
                "cpp_trace_graph": {"threads": 2, "file_threads": 3},
                "outputs": {"emit_dag_chrome_trace": True},
                "cpp_model_config": str(model),
            }
            write_json(config, raw)
            args = parse_args(
                [
                    "--profile-manifest",
                    str(manifest),
                    "--output-dir",
                    str(output),
                    "--threads",
                    "2",
                    "--file-threads",
                    "3",
                    "--emit-dag",
                    "--model-config",
                    str(model),
                ]
            )
            with patch("markov_internal.modeling.backend.trace_graph_executable", return_value=Path("trace_graph")):
                self.assertEqual(
                    build_trace_graph_command(manifest_run_config(args)),
                    build_trace_graph_command(ModelingRunConfig.load(config)),
                )
            raw["outputs"]["emit_module_summary"] = True
            write_json(config, raw)
            with self.assertRaisesRegex(ValueError, "validation"):
                ModelingRunConfig.load(config)
            raw["cpp_trace_graph"]["backend_kind"] = "validation"
            raw["cpp_trace_graph"]["trace_channels"] = "python_probe,ld_preload,python_probe"
            write_json(config, raw)
            normalized = ModelingRunConfig.load(config)
            self.assertEqual(normalized.trace_channels, ("python_probe", "ld_preload"))
            self.assertTrue(normalized.outputs.module_summary)


if __name__ == "__main__":
    unittest.main()
