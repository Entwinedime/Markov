#!/usr/bin/env python3
"""profiling Python probe fixtures。"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROBE_ROOT = ROOT / "src/profiling/python_probe"
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(PROBE_ROOT))

from profiling import normalize_profiling_config  # noqa: E402


def run_probe_fixture() -> None:
    """验证 Python probe 能包装模块函数、实例方法和 async 方法。"""

    with tempfile.TemporaryDirectory() as raw_tmp:
        tmp = Path(raw_tmp)
        package = tmp / "probe_pkg"
        package.mkdir()
        (package / "__init__.py").write_text("", encoding="utf-8")
        (package / "demo.py").write_text(
            """
import asyncio

class Result:
    def __init__(self, value):
        self.value = value
        self.host_hit_length = value

class Req:
    def __init__(self, rid):
        self.rid = rid

class Params:
    def __init__(self, rid, key):
        self.req = Req(rid)
        self.key = key

def module_fn(request_id, num_pages=1):
    return Result(request_id + num_pages)

class Worker:
    def __init__(self, page_size):
        self.page_size = page_size

    def run(self, request_id, num_pages=1):
        return Result(request_id + num_pages)

    def inspect(self, params):
        return Result(len(params.key))

    async def arun(self, request_id, num_pages=1):
        await asyncio.sleep(0)
        return Result(request_id + num_pages)
""",
            encoding="utf-8",
        )
        output_dir = tmp / "python_probe"
        targets = [
            {
                "id": "module.fn",
                "target": "probe_pkg.demo.module_fn",
                "events": ["module_fn_start", "module_fn_end"],
                "fields": [
                    {"name": "request_id", "source": "arg:request_id"},
                    {"name": "num_pages", "source": "arg:num_pages"},
                    {"name": "value", "source": "return.value", "required": False},
                ],
            },
            {
                "id": "worker.run",
                "module": "probe_pkg.demo",
                "target": "Worker.run",
                "events": ["run_start", "run_end"],
                "fields": [
                    {"name": "request_id", "source": "arg:request_id"},
                    {"name": "page_size", "source": "self.page_size"},
                    {"name": "num_pages", "source": "arg:num_pages"},
                    {"name": "value", "source": "return.value", "required": False},
                ],
            },
            {
                "id": "worker.arun",
                "module": "probe_pkg.demo",
                "target": "Worker.arun",
                "events": ["arun_start", "arun_end"],
                "fields": [
                    {"name": "request_id", "source": "arg:request_id"},
                    {"name": "page_size", "source": "self.page_size"},
                    {"name": "value", "source": "return.value", "required": False},
                ],
            },
            {
                "id": "worker.inspect",
                "module": "probe_pkg.demo",
                "target": "Worker.inspect",
                "events": ["inspect_start", "inspect_end"],
                "fields": [
                    {"name": "request_id", "source": "arg:params.req.rid"},
                    {"name": "input_tokens", "source": "len:arg:params.key"},
                    {"name": "page_size", "source": "self.page_size"},
                    {"name": "host_hit_length", "source": "return.host_hit_length"},
                ],
            },
        ]
        env = os.environ.copy()
        env["TRACE_SIM_PYTHON_PROBE"] = "1"
        env["TRACE_SIM_PYTHON_PROBES"] = "generic_callable"
        env["TRACE_SIM_PYTHON_PROBE_TARGETS"] = json.dumps(targets)
        env["TRACE_SIM_PYTHON_PROBE_OUTPUT"] = str(output_dir)
        env["PYTHONPATH"] = os.pathsep.join([str(PROBE_ROOT), str(tmp), env.get("PYTHONPATH", "")])
        subprocess.check_call(
            [
                sys.executable,
                "-c",
                "import asyncio; from probe_pkg.demo import Params, Worker, module_fn; "
                "assert module_fn(5, num_pages=2).value == 7; "
                "w=Worker(64); assert w.run(7, num_pages=3).value == 10; "
                "assert w.inspect(Params('r1', [1, 2, 3, 4])).value == 4; "
                "assert asyncio.run(w.arun(8, num_pages=4)).value == 12",
            ],
            env=env,
        )

        files = sorted(output_dir.glob("python_probe_trace.*.json"))
        assert len(files) == 1, files
        payload = json.loads(files[0].read_text(encoding="utf-8"))
        events = payload["traceEvents"]
        rows = [event["args"] for event in events if event.get("cat") == "python_probe"]
        phases = [row["phase"] for row in rows]
        assert phases == ["start", "end", "start", "end", "start", "end", "start", "end"], phases
        assert any(row.get("target_id") == "module.fn" and row.get("value") == 7 for row in rows)
        assert any(row.get("target_id") == "worker.run" and row.get("page_size") == 64 for row in rows)
        assert any(row.get("target_id") == "worker.arun" and row.get("value") == 12 for row in rows)
        assert any(
            row.get("target_id") == "worker.inspect"
            and row.get("request_id") == "r1"
            and row.get("input_tokens") == 4
            and row.get("host_hit_length") == 4
            for row in rows
        )


def sglang_hicache_targets() -> list[dict[str, object]]:
    """返回当前主线 SGLang HiCache Python probe target set。"""

    config_path = ROOT / "configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json"
    config = json.loads(config_path.read_text(encoding="utf-8"))
    return config["profiling"]["python_probe"]["targets"]


def run_config_fixture() -> None:
    """验证 profiling 配置只把 Python probe 和 LD_PRELOAD 开关规整成 runtime 信息。"""

    runtime = normalize_profiling_config(
        {
            "profiling": {
                "channels": ["python_probe", "ld_preload"],
                "python_probe": {
                    "targets": [
                        {
                            "id": "demo.target",
                            "module": "probe_pkg.demo",
                            "target": "Worker.run",
                        }
                    ]
                },
            },
        }
    )
    assert runtime.channels == ("python_probe", "ld_preload"), runtime
    assert runtime.python_probes == ("generic_callable",), runtime
    assert runtime.python_targets[0]["id"] == "demo.target", runtime
    assert runtime.python_state_trace_enabled is False, runtime


def run_hicache_token_source_fixture() -> None:
    """验证 HiCache token/range source 和 request runtime source。"""

    from trace_sim_probe.probes import sglang_hicache_callable

    class Req:
        def __init__(self):
            self.rid = "req-token"
            self.fill_ids = [1, 2, 3, 4, 5, 6]
            self.origin_input_ids = [1, 2, 3, 4]
            self.output_ids = [5, 6]
            self.kv_committed_len = 5
            self.kv_committed_freed = False
            self.cache_protected_len = 2
            self.extend_input_len = 4
            self.prefix_indices = [10, 11]
            self.host_hit_length = 2
            self.storage_hit_length = 1
            self.priority = 7
            self.last_node = None
            self.last_host_node = None
            self.best_match_node = None

        def _cache_commit_len(self):
            return 5

    class Cache:
        page_size = 2

    cache = Cache()
    req = Req()
    found_path, path_record = sglang_hicache_callable._extract_request_token_path(
        "arg:req,committed,arg:cache",
        {"req": req, "cache": cache},
        (),
        {},
        None,
    )
    found_span, span_record = sglang_hicache_callable._extract_request_token_span(
        "arg:req,committed",
        {"req": req},
        (),
        {},
        None,
    )
    handled_runtime, found_runtime, runtime = sglang_hicache_callable._hicache_request_runtime_source(
        "hicache_request_runtime:arg:req",
        "request_runtime",
        {"req": req},
        (),
        {},
        None,
    )

    assert found_path is True, path_record
    assert path_record["token_count"] == 5, path_record
    assert path_record["token_ids"] == [1, 2, 3, 4, 5], path_record
    assert found_span is True, span_record
    assert span_record["token_count"] == 5, span_record
    assert span_record["path_id"] == path_record["token_path_id"], (span_record, path_record)
    assert handled_runtime and found_runtime, runtime
    assert runtime["request_id"] == "req-token", runtime
    assert runtime["effective_commit_len"] == 5, runtime
    assert runtime["cache_protected_len"] == 2, runtime


def run_hicache_prefetch_progress_source_fixture() -> None:
    """验证 prefetch progress source 能采到 ready/late 判定所需证据。"""

    from trace_sim_probe.probes import sglang_hicache_callable

    class Operation:
        def __init__(self) -> None:
            self.hash_value = ["h1", "h2"]
            self.completed_tokens = 64

        def is_terminated(self) -> bool:
            return False

    class Cache:
        def __init__(self) -> None:
            self.page_size = 64
            self.prefetch_stop_policy = "timeout"
            self.ongoing_prefetch = {"req-a": (None, list(range(128)), list(range(128)), Operation())}
            self.prefetch_loaded_tokens_by_reqid = {"req-b": 64}

    cache = Cache()
    found_ongoing, ongoing = sglang_hicache_callable._extract_prefetch_progress(
        "self,arg:req_id",
        {"req_id": "req-a"},
        (cache,),
        {},
        False,
    )
    found_loaded, loaded = sglang_hicache_callable._extract_prefetch_progress(
        "self,arg:req_id",
        {"req_id": "req-b"},
        (cache,),
        {},
        True,
    )

    assert found_ongoing is True, ongoing
    assert ongoing["has_ongoing_prefetch"] is True, ongoing
    assert ongoing["operation_hash_pages"] == ["h1", "h2"], ongoing
    assert ongoing["completed_tokens"] == 64, ongoing
    assert ongoing["ready_pages_estimate"] == 1, ongoing
    assert ongoing["late_tokens_estimate"] == 64, ongoing
    assert found_loaded is True, loaded
    assert loaded["has_ongoing_prefetch"] is False, loaded
    assert loaded["loaded_tokens_evidence"] == 64, loaded
    assert loaded["check_return"] is True, loaded


def run_torch_profiler_lifecycle_fixture() -> None:
    """验证 torch profiler 默认覆盖完整 workload，显式 num_steps 时自动结束。"""

    spec = importlib.util.spec_from_file_location(
        "trace_sim_profile_runner",
        ROOT / "scripts/internal/profile_runner.py",
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[str(spec.name)] = module
    spec.loader.exec_module(module)

    base_cfg = {
        "name": "profiler_lifecycle_fixture",
        "framework": "sglang",
        "profiling": {
            "enabled": True,
            "channels": ["torch"],
            "torch": {"enabled": True, "output_dir": "trace/torch"},
            "ld_preload": {"enabled": False},
        },
        "server": {"command": ["python3", "-c", "import time; time.sleep(1)"]},
        "bench": {"command": ["python3", "-c", "print('bench')"]},
    }
    default_run = module.ProfileRun(base_cfg, dry_run=True)
    default_body = module.build_profile_body(base_cfg["profiling"]["torch"], default_run.layout)
    assert "num_steps" not in default_body, default_body
    assert default_run._should_stop_torch_profiler_after_workload() is True

    stepped_cfg = json.loads(json.dumps(base_cfg))
    stepped_cfg["profiling"]["torch"]["num_steps"] = 1
    stepped_run = module.ProfileRun(stepped_cfg, dry_run=True)
    stepped_body = module.build_profile_body(stepped_cfg["profiling"]["torch"], stepped_run.layout)
    assert stepped_body["num_steps"] == 1, stepped_body
    assert stepped_run._should_stop_torch_profiler_after_workload() is False


def run_state_trace_env_fixture() -> None:
    """验证 state_trace 配置会补充 validation-only state snapshot。"""

    spec = importlib.util.spec_from_file_location(
        "trace_sim_profile_runner_state_trace",
        ROOT / "scripts/internal/profile_runner.py",
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[str(spec.name)] = module
    spec.loader.exec_module(module)

    cfg = {
        "name": "state_trace_env_fixture",
        "framework": "sglang",
        "profiling": {
            "enabled": True,
            "channels": ["python_probe"],
            "python_probe": {
                "probes": ["sglang.hicache"],
                "state_trace": {"enabled": True},
                "targets": [
                    {
                        "id": "hiradix.lookup_path",
                        "module": "sglang.srt.mem_cache.hiradix_cache",
                        "target": "HiRadixCache.match_prefix",
                        "fields": [
                            {"name": "token_dictionary", "source": "token_path:arg:params.key,self"},
                            {"name": "event_role", "source": "const:lookup_path"},
                        ],
                    }
                ],
            },
        },
        "server": {"command": ["python3", "-c", "import time; time.sleep(1)"]},
        "bench": {"command": ["python3", "-c", "print('bench')"]},
    }
    run = module.ProfileRun(cfg, dry_run=True)
    env = {}
    run._apply_python_probe_env(env)
    assert env["TRACE_SIM_HICACHE_STATE_TRACE"] == "1", env
    targets = json.loads(env["TRACE_SIM_PYTHON_PROBE_TARGETS"])
    fields = targets[0]["fields"]
    assert any(field.get("source") == "hicache_state:self" for field in fields), fields
    assert fields[-1]["source"] == "hicache_state:self", fields


def run_env_placeholder_fixture() -> None:
    """验证 env 支持 run layout 占位符，避免实验间共享 HiCache storage。"""

    spec = importlib.util.spec_from_file_location(
        "trace_sim_profile_runner_env_placeholder",
        ROOT / "scripts/internal/profile_runner.py",
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[str(spec.name)] = module
    spec.loader.exec_module(module)

    cfg = {
        "name": "env_placeholder_fixture",
        "framework": "sglang",
        "env": {"SGLANG_HICACHE_FILE_BACKEND_STORAGE_DIR": "{run_dir}/hicache_storage"},
        "profiling": {"enabled": False, "channels": []},
        "server": {"command": ["python3", "-c", "import time; time.sleep(1)"]},
        "bench": {"command": ["python3", "-c", "print('bench')"]},
    }
    run = module.ProfileRun(cfg, dry_run=True)
    env = run._build_server_env()
    assert env["SGLANG_HICACHE_FILE_BACKEND_STORAGE_DIR"] == str(run.layout.run_dir / "hicache_storage"), env


def run_profile_suite_matrix_fixture() -> None:
    """验证 profiling suite 支持 server/input 矩阵、子集选择和归档元数据。"""

    spec = importlib.util.spec_from_file_location(
        "trace_sim_profile_runner_suite_matrix",
        ROOT / "scripts/internal/profile_runner.py",
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[str(spec.name)] = module
    spec.loader.exec_module(module)

    with tempfile.TemporaryDirectory() as raw_tmp:
        tmp = Path(raw_tmp)
        cfg = {
            "name": "suite_matrix_fixture",
            "framework": "sglang",
            "run_root": str(tmp / "runs"),
            "run_id": "suite_matrix_fixture_run",
            "metadata": {"purpose": "fixture"},
            "profiling": {"enabled": False, "channels": []},
            "matrix": {
                "servers": [
                    {
                        "id": "server_a",
                        "server": {
                            "command": ["python3", "-c", "import time; time.sleep(1)"],
                            "ready_url": "http://127.0.0.1:31001/get_model_info",
                        },
                        "env": {"SERVER_VARIANT": "a"},
                        "metadata": {"cache_write_policy": "policy_a"},
                    },
                    {
                        "id": "server_b",
                        "server": {
                            "command": ["python3", "-c", "import time; time.sleep(1)"],
                            "ready_url": "http://127.0.0.1:31002/get_model_info",
                        },
                        "env": {"SERVER_VARIANT": "b"},
                        "metadata": {"cache_write_policy": "policy_b"},
                    },
                ],
                "inputs": [
                    {"id": "manual", "bench": {"command": ["python3", "-c", "print('{metadata.cache_write_policy}')"]}},
                    {"id": "bench", "bench": {"command": ["python3", "-c", "print('bench')"]}},
                ],
            },
        }

        expanded = module.expand_suite(cfg)
        assert [item["id"] for item in expanded] == [
            "server_a_manual",
            "server_a_bench",
            "server_b_manual",
            "server_b_bench",
        ], expanded
        assert all(item["profiling"] == cfg["profiling"] for item in expanded), expanded
        assert expanded[0]["server"]["ready_url"].endswith(":31001/get_model_info"), expanded[0]
        assert expanded[3]["server"]["ready_url"].endswith(":31002/get_model_info"), expanded[3]
        assert expanded[0]["bench"]["command"][-1] == "print('{metadata.cache_write_policy}')", expanded[0]
        assert expanded[3]["bench"]["command"][-1] == "print('bench')", expanded[3]
        assert expanded[3]["metadata"]["suite_server_id"] == "server_b", expanded[3]
        assert expanded[3]["metadata"]["suite_input_id"] == "bench", expanded[3]

        selected = module.parse_experiment_selection(["server_a_manual,server_b_bench"])
        run_dirs = module.run_profile_suite(cfg, dry_run=True, selected_experiments=selected)
        assert [path.name for path in run_dirs] == ["01_server_a_manual", "04_server_b_bench"], run_dirs

        suite_dir = run_dirs[0].parent
        selection = json.loads((suite_dir / "suite_selection.json").read_text(encoding="utf-8"))
        assert selection["selected_selectors"] == ["server_a_manual", "server_b_bench"], selection
        assert [item["index"] for item in selection["planned_experiments"]] == [1, 4], selection
        result = json.loads((suite_dir / "suite_result.json").read_text(encoding="utf-8"))
        assert len(result["runs"]) == 2, result
        assert not result["failures"], result

        first_config = json.loads((run_dirs[0] / "config.json").read_text(encoding="utf-8"))
        assert first_config["metadata"]["suite_experiment_id"] == "server_a_manual", first_config
        assert first_config["env"]["SERVER_VARIANT"] == "a", first_config
        first_bench_cmd = (run_dirs[0] / "bench_cmd.txt").read_text(encoding="utf-8")
        assert "policy_a" in first_bench_cmd, first_bench_cmd
        first_manifest = json.loads((run_dirs[0] / "profile_manifest.json").read_text(encoding="utf-8"))
        assert first_manifest["status"] == "dry_run", first_manifest

        try:
            module.filter_suite_experiments(list(enumerate(expanded, start=1)), {"missing"})
        except ValueError as exc:
            assert "unknown experiment selector" in str(exc), exc
        else:
            raise AssertionError("missing experiment selector should fail")

        bad_cfg = json.loads(json.dumps(cfg))
        bad_cfg["matrix"]["servers"][0]["profiling"] = {"enabled": True}
        try:
            module.expand_suite(bad_cfg)
        except ValueError as exc:
            assert "must not override profiling" in str(exc), exc
        else:
            raise AssertionError("matrix server profiling override should fail")


def run_no_legacy_hicache_page_identity_fixture() -> None:
    """确认当前主配置不再声明 page-identity/radix-removed source。"""

    targets = sglang_hicache_targets()
    serialized = json.dumps(targets, ensure_ascii=False)
    forbidden = (
        "page_hashes:",
        "page_hashes_after_prefix:",
        "page_hashes_concat:",
        "hicache_radix_removed_pages:",
        "target_page_identity",
        "page_identity",
    )
    for item in forbidden:
        assert item not in serialized, item

    source_result_field_names = {
        "matched_span",
        "matched_token_dictionary",
        "device_hit_tokens",
        "host_hit_length",
        "last_host_node",
        "last_host_node_summary",
        "best_match_node",
        "best_match_node_summary",
        "last_node_summary",
        "last_node_chain",
        "delta",
        "evicted_tokens",
        "inserted_node",
        "inserted_span",
        "producer_id",
        "request_runtime",
        "prefetch_state",
        "progress",
        "admission_result",
        "result_request_id",
        "can_run_count",
        "new_chunked_request_id",
        "rem_total_tokens_snapshot",
        "cur_rem_tokens_snapshot",
        "rem_input_tokens_snapshot",
        "rem_chunk_tokens_snapshot",
    }
    for target in targets:
        fields = target.get("fields") if isinstance(target.get("fields"), list) else []
        const_fields = {
            field.get("name"): str(field.get("source") or "").split(":", 1)[1]
            for field in fields
            if isinstance(field, dict) and str(field.get("source") or "").startswith("const:")
        }
        if const_fields.get("fact_class") != "invariant_state":
            continue
        names = {field.get("name") for field in fields if isinstance(field, dict)}
        leaked = sorted(source_result_field_names & names)
        assert not leaked, (target.get("id"), leaked)


def run_sglang_hicache_target_fixture() -> None:
    """验证当前 HiCache target set 能覆盖真实模块路径和字段 source 语法。"""

    with tempfile.TemporaryDirectory() as raw_tmp:
        tmp = Path(raw_tmp)
        _write_fake_sglang_package(tmp)
        output_dir = tmp / "python_probe"

        env = os.environ.copy()
        env["TRACE_SIM_PYTHON_PROBE"] = "1"
        env["TRACE_SIM_PYTHON_PROBES"] = "sglang.hicache"
        env["TRACE_SIM_PYTHON_PROBE_TARGETS"] = json.dumps(sglang_hicache_targets())
        env["TRACE_SIM_PYTHON_PROBE_OUTPUT"] = str(output_dir)
        env["PYTHONPATH"] = os.pathsep.join([str(PROBE_ROOT), str(tmp), env.get("PYTHONPATH", "")])
        subprocess.check_call(
            [
                sys.executable,
                "-c",
                """
from sglang.srt.managers.scheduler import Req, Scheduler
from sglang.srt.managers.schedule_policy import PrefillAdder
from sglang.srt.mem_cache.hiradix_cache import HiRadixCache, Node, Params
from sglang.srt.managers.cache_controller import HiCacheController, Operation

req = Req('req-1')
scheduler = Scheduler()
scheduler._prefetch_kvcache(req)
adder = PrefillAdder(scheduler.tree_cache)
adder.add_one_req(req, has_chunked_req=False, truncation_align_size=None)
adder.add_chunked_req(req)

cache = HiRadixCache()
node = Node(11)
params = Params(req=req, key=[1, 2, 3, 4], value=[5, 6, 7, 8], best_match_node=node, host_hit_length=4, num_tokens=4)
cache.match_prefix(params)
cache.cache_finished_req(req, is_insert=True)
cache.cache_unfinished_req(req, chunked=True)
cache.prefetch_from_storage(req.rid, node, [1, 2, 3, 4], '0' * 64, ['p0'])
cache.check_prefetch_progress(req.rid)
cache.pop_prefetch_loaded_tokens(req.rid)
cache.init_load_back(params)
cache.load_back(node)
cache.insert(params)
cache.write_backup(node, write_back=True)
cache.write_backup_storage(node)
cache.check_hicache_events()
cache.ready_to_load_host_cache()
cache.flush_write_through_acks()
cache.inc_lock_ref(node)
cache.dec_lock_ref(node)
cache.evict(params)

controller = HiCacheController()
controller.load([1, 2, 3, 4], node_id=11)
controller.start_loading()
controller.write([5, 6, 7, 8], node_id=11)
controller.start_writing()
operation = controller.prefetch(req.rid, [1, 2, 3, 4], [5, 6, 7, 8])
controller._storage_hit_query(operation)
controller._page_transfer(operation)
controller.write_storage([1, 2, 3, 4], [5, 6, 7, 8], ['h1'])
controller._page_backup(Operation(request_id=req.rid))
""",
            ],
            env=env,
        )

        files = sorted(output_dir.glob("python_probe_trace.*.json"))
        assert len(files) == 1, files
        payload = json.loads(files[0].read_text(encoding="utf-8"))
        rows = [event["args"] for event in payload["traceEvents"] if event.get("cat") == "python_probe"]
        target_ids = {row.get("target_id") for row in rows}
        expected_ids = {str(target["id"]) for target in sglang_hicache_targets()}
        missing = expected_ids - target_ids
        assert not missing, missing
        assert any(
            row.get("target_id") == "hiradix.lookup_path"
            and row.get("request_id") == "req-1"
            and row.get("event_role") == "lookup_path"
            and row.get("fact_class") == "invariant_state"
            and row.get("state_model_input") == "true"
            and row.get("token_count") == 4
            and (row.get("token_dictionary") or {}).get("token_count") == 4
            for row in rows
        )
        assert any(
            row.get("target_id") == "hiradix.lookup_result_observed"
            and row.get("fact_class") == "source_actual"
            and row.get("state_model_input") == "false"
            and row.get("host_hit_length") == 4
            for row in rows
        )
        assert any(
            row.get("target_id") == "scheduler.prefetch_decision"
            and row.get("event_role") == "prefetch_decision"
            and row.get("fact_class") == "invariant_state"
            and "host_hit_length" not in row
            for row in rows
        )
        assert any(
            row.get("target_id") == "scheduler.prefetch_decision_observed"
            and row.get("event_role") == "prefetch_decision_observed"
            and row.get("fact_class") == "source_actual"
            and isinstance(row.get("prefetch_state"), dict)
            for row in rows
        )
        assert any(
            row.get("target_id") == "schedule_policy.prefill_admission"
            and row.get("event_role") == "request_admission"
            and row.get("fact_class") == "invariant_state"
            and row.get("state_model_input") == "true"
            and row.get("admission_kind") == "prefill_add_one_req"
            and (row.get("token_dictionary") or {}).get("token_count") == 6
            and "request_runtime" not in row
            and "admission_result" not in row
            for row in rows
        )
        assert any(
            row.get("target_id") == "schedule_policy.prefill_admission_observed"
            and row.get("event_role") == "request_admission_observed"
            and row.get("fact_class") == "source_actual"
            and row.get("admission_result") == "CONTINUE"
            and isinstance(row.get("request_runtime"), dict)
            for row in rows
        )
        assert any(
            row.get("target_id") == "schedule_policy.chunked_admission"
            and row.get("event_role") == "request_admission"
            and row.get("fact_class") == "invariant_state"
            and row.get("admission_kind") == "chunked_continuation"
            for row in rows
        )
        assert any(
            row.get("target_id") == "hiradix.lock_scope_inc"
            and row.get("event_role") == "lock_scope_delta"
            and "delta" not in row
            for row in rows
        )
        assert any(
            row.get("target_id") == "hiradix.lock_scope_inc_observed"
            and row.get("event_role") == "lock_scope_result_observed"
            and row.get("delta") == -4
            for row in rows
        )
        assert any(
            row.get("target_id") == "hicache_internal.write_enqueue"
            and row.get("event_role") == "write_enqueue_observed"
            and row.get("fact_class") == "source_actual"
            for row in rows
        )
        assert all("page_identity" not in row for row in rows), rows
        run_profile_quality_fixture(tmp, files[0])


def run_hicache_state_snapshot_fixture() -> None:
    """验证 HiCache state snapshot 被拆成非执行事件，不污染真实 probe 事件。"""

    with tempfile.TemporaryDirectory() as raw_tmp:
        tmp = Path(raw_tmp)
        _write_fake_sglang_package(tmp)
        output_dir = tmp / "python_probe"
        targets = [
            {
                "id": "hiradix.match_prefix",
                "module": "sglang.srt.mem_cache.hiradix_cache",
                "target": "HiRadixCache.match_prefix",
                "events": ["hicache_lookup_start", "hicache_lookup_end"],
                "fields": [
                    {"name": "request_id", "source": "arg:params.req.rid", "required": False},
                    {"name": "token_dictionary", "source": "token_path:arg:params.key,self"},
                    {"name": "full_path_span", "source": "token_span:arg:params.key"},
                    {"name": "event_role", "source": "const:lookup_path"},
                    {"name": "state_snapshot", "source": "hicache_state:self", "required": False},
                ],
            }
        ]

        env = os.environ.copy()
        env["TRACE_SIM_PYTHON_PROBE"] = "1"
        env["TRACE_SIM_PYTHON_PROBES"] = "sglang.hicache"
        env["TRACE_SIM_PYTHON_PROBE_TARGETS"] = json.dumps(targets)
        env["TRACE_SIM_PYTHON_PROBE_OUTPUT"] = str(output_dir)
        env["TRACE_SIM_HICACHE_STATE_TRACE"] = "1"
        env["PYTHONPATH"] = os.pathsep.join([str(PROBE_ROOT), str(tmp), env.get("PYTHONPATH", "")])
        subprocess.check_call(
            [
                sys.executable,
                "-c",
                """
from sglang.srt.managers.scheduler import Req
from sglang.srt.mem_cache.hiradix_cache import HiRadixCache, Node, Params

req = Req('req-state')
cache = HiRadixCache()
node = Node(11)
params = Params(req=req, key=[1, 2, 3, 4], value=[5, 6, 7, 8], best_match_node=node, host_hit_length=4, num_tokens=4)
cache.match_prefix(params)
""",
            ],
            env=env,
        )

        files = sorted(output_dir.glob("python_probe_trace.*.json"))
        assert len(files) == 1, files
        payload = json.loads(files[0].read_text(encoding="utf-8"))
        events = [event for event in payload["traceEvents"] if event.get("cat") == "python_probe"]
        real_events = [event for event in events if event["args"].get("model_input") is True]
        snapshot_events = [event for event in events if event["args"].get("event_kind") == "state_snapshot"]
        assert len(real_events) == 2, events
        assert len(snapshot_events) == 2, events
        assert all(event["args"].get("model_input") is False for event in snapshot_events), snapshot_events
        snapshot = snapshot_events[-1]["args"]["state_snapshot"]
        assert snapshot["enabled"] is True, snapshot
        assert snapshot["object_type"] == "HiRadixCache", snapshot
        assert snapshot["object_id"].startswith("HiRadixCache:"), snapshot
        assert snapshot["derived"]["l1_resident_pages"] == ["h1", "h2"], snapshot
        assert snapshot["derived"]["l2_resident_pages"] == ["h1", "h2"], snapshot
        assert snapshot["capacity"]["write_policy"] == "write_through", snapshot
        assert snapshot["capacity"]["prefetch_policy"] == "best_effort", snapshot
        assert snapshot["capacity"]["l1_capacity_pages"] == 4, snapshot
        assert snapshot["capacity"]["l1_available_pages"] == 2, snapshot
        assert snapshot["capacity"]["l2_capacity_pages"] == 8, snapshot
        assert snapshot["capacity"]["l2_available_pages"] == 6, snapshot
        assert snapshot["capacity"]["prefetch_capacity_limit_pages"] == 4, snapshot


def run_profile_quality_fixture(tmp: Path, python_probe_file: Path) -> None:
    """验证 profile_quality 能审计 profile manifest 和 Python probe target 命中。"""

    manifest = tmp / "profile_manifest.json"
    manifest.write_text(
        json.dumps(
            {
                "run_dir": str(tmp),
                "status": "completed",
                "profiling_ready": True,
                "profiling": {
                    "python_targets": sglang_hicache_targets(),
                },
                "trace": {
                    "torch_trace_files": [],
                    "ld_preload_trace_files": [],
                },
                "sidecar": {
                    "python_probe_files": [
                        {
                            "path": str(python_probe_file),
                            "exists": True,
                        }
                    ]
                },
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    subprocess.check_call(
        [
            sys.executable,
            "scripts/internal/profile_quality.py",
            "--manifest",
            str(manifest),
        ],
        cwd=ROOT,
    )
    quality = json.loads((tmp / "profile_quality.json").read_text(encoding="utf-8"))
    assert quality["quality_ready"] is True, quality
    assert quality["configured_target_count"] == len(sglang_hicache_targets()), quality
    assert quality["observed_target_count"] == len(sglang_hicache_targets()), quality
    assert not quality["missing_targets"], quality
    assert quality["observed_cache_mechanisms"]["lock_ref"] > 0, quality
    invariant = quality["hicache_invariant_coverage"]
    assert invariant["ready"] is True, quality
    assert invariant["missing_required_fact_events"] == 0, quality
    assert invariant["missing_token_dictionary_refs"] == [], quality
    assert invariant["dictionary_ids_without_tokens"] == [], quality


def run_profile_quality_capacity_fixture() -> None:
    """验证 profile_quality 会汇总 validation-only capacity 快照。"""

    with tempfile.TemporaryDirectory() as raw_tmp:
        tmp = Path(raw_tmp)
        sidecar = tmp / "python_probe_trace.rank0.pid123.json"
        sidecar.write_text(
            json.dumps(
                {
                    "traceEvents": [
                        {
                            "name": "hicache_lookup_end:state_snapshot",
                            "cat": "python_probe",
                            "ph": "X",
                            "ts": 1,
                            "dur": 0,
                            "pid": 123,
                            "tid": 1,
                            "args": {
                                "domain": "python_probe",
                                "event_kind": "state_snapshot",
                                "model_input": False,
                                "state_snapshot": {
                                    "enabled": True,
                                    "object_type": "HiRadixCache",
                                    "capacity": {
                                        "page_size": 128,
                                        "write_policy": "write_back",
                                        "prefetch_policy": "timeout",
                                        "l1_capacity_pages": 46,
                                        "l1_available_pages": 8,
                                        "l2_capacity_pages": 88,
                                        "l2_available_pages": 16,
                                        "prefetch_capacity_limit_pages": 40,
                                        "l1_pool": {
                                            "pool_type": "DevicePool",
                                            "capacity_pages": 46,
                                        },
                                    },
                                },
                            },
                        }
                    ]
                },
                ensure_ascii=False,
            ),
            encoding="utf-8",
        )
        manifest = tmp / "profile_manifest.json"
        manifest.write_text(
            json.dumps(
                {
                    "run_dir": str(tmp),
                    "status": "completed",
                    "profiling_ready": True,
                    "profiling": {"channels_enabled": ["python"], "python_state_trace_enabled": True},
                    "trace": {"torch_trace_files": [], "ld_preload_trace_files": []},
                    "sidecar": {"python_probe_files": [{"path": str(sidecar), "exists": True}]},
                },
                ensure_ascii=False,
            ),
            encoding="utf-8",
        )
        subprocess.check_call(
            [
                sys.executable,
                "scripts/internal/profile_quality.py",
                "--manifest",
                str(manifest),
            ],
            cwd=ROOT,
        )
        quality = json.loads((tmp / "profile_quality.json").read_text(encoding="utf-8"))
        assert quality["quality_ready"] is True, quality
        assert quality["hicache_state_trace_enabled"] is True, quality
        assert quality["hicache_capacity_observed"] is True, quality
        capacity = quality["hicache_capacity"]
        assert capacity["ready"] is True, capacity
        assert capacity["object_type_counts"]["HiRadixCache"] == 1, capacity
        assert capacity["unique_values"]["l1_capacity_pages"] == [46], capacity
        assert capacity["unique_values"]["l2_capacity_pages"] == [88], capacity
        assert capacity["unique_values"]["write_policy"] == ["write_back"], capacity


def run_profile_quality_capacity_missing_fixture() -> None:
    """验证 state trace 开启时缺少 capacity snapshot 会被质量审计拦住。"""

    with tempfile.TemporaryDirectory() as raw_tmp:
        tmp = Path(raw_tmp)
        sidecar = tmp / "python_probe_trace.rank0.pid123.json"
        sidecar.write_text(
            json.dumps(
                {
                    "traceEvents": [
                        {
                            "name": "hicache_lookup_end:state_snapshot",
                            "cat": "python_probe",
                            "ph": "X",
                            "ts": 1,
                            "dur": 0,
                            "pid": 123,
                            "tid": 1,
                            "args": {
                                "domain": "python_probe",
                                "event_kind": "state_snapshot",
                                "model_input": False,
                                "state_snapshot": {
                                    "enabled": True,
                                    "object_type": "HiRadixCache",
                                    "derived": {},
                                },
                            },
                        }
                    ]
                },
                ensure_ascii=False,
            ),
            encoding="utf-8",
        )
        manifest = tmp / "profile_manifest.json"
        manifest.write_text(
            json.dumps(
                {
                    "run_dir": str(tmp),
                    "status": "completed",
                    "profiling_ready": True,
                    "profiling": {"channels_enabled": ["python"], "python_state_trace_enabled": True},
                    "trace": {"torch_trace_files": [], "ld_preload_trace_files": []},
                    "sidecar": {"python_probe_files": [{"path": str(sidecar), "exists": True}]},
                },
                ensure_ascii=False,
            ),
            encoding="utf-8",
        )
        completed = subprocess.run(
            [
                sys.executable,
                "scripts/internal/profile_quality.py",
                "--manifest",
                str(manifest),
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        assert completed.returncode == 1, completed
        quality = json.loads((tmp / "profile_quality.json").read_text(encoding="utf-8"))
        assert quality["quality_ready"] is False, quality
        assert "hicache_capacity_snapshot_missing" in quality["quality_errors"], quality


def run_trace_merger_sidecar_only_fixture() -> None:
    """验证没有 torch trace 时，manifest 仍能生成 state-only merged trace。"""

    with tempfile.TemporaryDirectory() as raw_tmp:
        tmp = Path(raw_tmp)
        sidecar = tmp / "python_probe_trace.rank0.pid123.json"
        sidecar.write_text(
            json.dumps(
                {
                    "traceEvents": [
                        {
                            "name": "hicache_lookup_end",
                            "cat": "python_probe",
                            "ph": "X",
                            "ts": 1,
                            "dur": 1,
                            "pid": 123,
                            "tid": 1,
                            "args": {"domain": "python_probe", "event_kind": "hicache", "model_input": True},
                        }
                    ]
                }
            ),
            encoding="utf-8",
        )
        manifest = tmp / "profile_manifest.json"
        manifest.write_text(
            json.dumps(
                {
                    "trace": {"torch_trace_files": [], "ld_preload_trace_files": []},
                    "sidecar": {"python_probe_files": [{"path": str(sidecar), "exists": True}]},
                }
            ),
            encoding="utf-8",
        )
        out_dir = tmp / "merged"
        subprocess.check_call(
            [
                sys.executable,
                "scripts/trace/trace_merger.py",
                "--manifest",
                str(manifest),
                "--out-dir",
                str(out_dir),
            ],
            cwd=ROOT,
        )
        summary = json.loads((out_dir / "merge_manifest_summary.json").read_text(encoding="utf-8"))
        assert len(summary["merged_trace_files"]) == 1, summary
        merged = json.loads(Path(summary["merged_trace_files"][0]).read_text(encoding="utf-8"))
        assert merged["traceEvents"][0]["name"] == "hicache_lookup_end", merged
        assert summary["reports"][0]["mode"] == "sidecar_only", summary


def _write_fake_sglang_package(tmp: Path) -> None:
    """写入最小假 SGLang 包，专门验证 probe target，不模拟真实业务逻辑。"""

    for package in (
        "sglang",
        "sglang/srt",
        "sglang/srt/managers",
        "sglang/srt/mem_cache",
    ):
        path = tmp / package
        path.mkdir(parents=True, exist_ok=True)
        (path / "__init__.py").write_text("", encoding="utf-8")

    (tmp / "sglang/srt/managers/scheduler.py").write_text(
        """
class SamplingParams:
    def __init__(self):
        self.ignore_eos = False
        self.max_new_tokens = 16

class Req:
    def __init__(self, rid):
        self.rid = rid
        self.sampling_params = SamplingParams()
        self.origin_input_ids = [1, 2, 3, 4]
        self.output_ids = [5, 6]
        self.fill_ids = [1, 2, 3, 4, 5, 6]
        self.kv_committed_len = 5
        self.kv_committed_freed = False
        self.cache_protected_len = 2
        self.extend_input_len = 4
        self.prefix_indices = [10, 11]
        self.host_hit_length = 2
        self.storage_hit_length = 0
        self.priority = 7
        self.last_node = None
        self.last_host_node = None
        self.best_match_node = None

    def _cache_commit_len(self):
        return 5

class TreeCache:
    def __init__(self):
        self.page_size = 2
        self.prefetch_stop_policy = 'best_effort'
        self.hicache_storage_pass_prefix_keys = False
        self.root_node = None
        self.cache_controller = self
        self.write_policy = 'write_through'
        self.prefetch_threshold = 4
        self.prefetch_capacity_limit = 8
        self.prefetch_tokens_occupied = 0
        self.enable_storage = True
        self.storage_batch_size = 128

class Scheduler:
    def __init__(self):
        self.tree_cache = TreeCache()
        self.enable_hicache_storage = True

    def _prefetch_kvcache(self, req):
        if req.last_host_node is None:
            from sglang.srt.mem_cache.hiradix_cache import Node
            node = Node(31)
            req.last_host_node = node
            req.best_match_node = node
            req.last_node = node
            self.tree_cache.root_node = node.parent
        return req.rid
""",
        encoding="utf-8",
    )
    (tmp / "sglang/srt/managers/schedule_policy.py").write_text(
        """
from enum import Enum, auto

class AddReqResult(Enum):
    CONTINUE = auto()
    NO_TOKEN = auto()
    OTHER = auto()

class PrefillAdder:
    def __init__(self, tree_cache, page_size=2):
        self.page_size = page_size
        self.tree_cache = tree_cache
        self.can_run_list = []
        self.new_chunked_req = None
        self.rem_input_tokens = 64
        self.rem_chunk_tokens = 8

    @property
    def rem_total_tokens(self):
        return 64 - len(self.can_run_list) * self.page_size

    @property
    def cur_rem_tokens(self):
        return 32 - len(self.can_run_list) * self.page_size

    def add_one_req(self, req, has_chunked_req, truncation_align_size):
        self.can_run_list.append(req)
        return AddReqResult.CONTINUE

    def add_chunked_req(self, req):
        self.can_run_list.append(req)
        return None
""",
        encoding="utf-8",
    )
    (tmp / "sglang/srt/mem_cache/hiradix_cache.py").write_text(
        """
class Node:
    def __init__(self, node_id, key=None, parent=None):
        self.id = node_id
        self.parent = parent
        self.key = [1, 2] if key is None else key
        self.host_value = [1, 2, 3, 4]
        self.value = [5, 6, 7, 8]
        self.hash_value = ['h1', 'h2']
        self.children = {}
        self.lock_ref = 0
        self.host_ref_counter = 0
        self.hit_count = 0
        self.priority = 7
        self.evicted = False
        self.backuped = True
        if parent is None and node_id != 0:
            root = Node(0, key=[], parent=None)
            self.parent = root
            root.children['child'] = self

    def get_last_hash_value(self):
        return self.hash_value[-1] if self.hash_value else None

    def get_prefix_hash_values(self, node):
        return ['prefix-h1']

class Result:
    def __init__(self, node):
        self.device_indices = [1, 2, 3, 4]
        self.last_device_node = node
        self.last_host_node = node
        self.best_match_node = node
        self.host_hit_length = 4
        self.prefix_len = 4
        self.inserted_host_node = node
        self.num_tokens_evicted = 4

class LockResult:
    def __init__(self, delta):
        self.delta = delta

class Pool:
    def __init__(self, size, page_size, available):
        self.size = size
        self.page_size = page_size
        self.page_num = size // page_size
        self.free_slots = list(range(available))

    def available_size(self):
        return len(self.free_slots)

class ControllerSnapshot:
    def __init__(self):
        self.page_size = 2
        self.mem_pool_device = Pool(8, 2, 4)
        self.mem_pool_host = Pool(16, 2, 12)
        self.write_policy = 'write_through'
        self.prefetch_threshold = 4
        self.prefetch_capacity_limit = 8
        self.prefetch_tokens_occupied = 2
        self.enable_storage = True
        self.storage_batch_size = 128
        self.load_queue = [1]
        self.write_queue = [1]
        self.ack_write_queue = []
        self.ack_load_queue = []

class Params:
    def __init__(self, req, key, value, best_match_node, host_hit_length, num_tokens):
        self.req = req
        self.key = key
        self.value = value
        self.best_match_node = best_match_node
        self.host_hit_length = host_hit_length
        self.num_tokens = num_tokens
        self.chunked = False
        self.priority = 7
        self.prev_prefix_len = 0

class InitLoadBackParams:
    def __init__(self, best_match_node, host_hit_length, req=None, mem_quota=None):
        self.best_match_node = best_match_node
        self.host_hit_length = host_hit_length
        self.req = req
        self.mem_quota = mem_quota

class HiRadixCache:
    def __init__(self):
        self.page_size = 2
        self.ongoing_prefetch = {}
        self.prefetch_loaded_tokens_by_reqid = {'req-1': 4}
        self.prefetch_stop_policy = 'best_effort'
        self.write_through_threshold = 1
        self.load_back_threshold = 10
        self.cache_controller = ControllerSnapshot()
        self.kv_cache = self.cache_controller.mem_pool_device
        self.token_to_kv_pool_host = self.cache_controller.mem_pool_host
        self.node = Node(21)
        self.root_node = self.node.parent
        self.evictable_leaves = {self.node}
        self.evictable_host_leaves = {self.node}
        self.evictable_size_ = 4
        self.protected_size_ = 0

    def match_prefix(self, params):
        params.req.last_node = params.best_match_node
        params.req.last_host_node = params.best_match_node
        params.req.best_match_node = params.best_match_node
        return Result(params.best_match_node)

    def cache_finished_req(self, req, is_insert=True):
        return None

    def cache_unfinished_req(self, req, chunked=False):
        return None

    def prefetch_from_storage(self, req_id, last_host_node, new_input_tokens, last_hash=None, prefix_keys=None):
        op = type('Operation', (), {'id': 3, 'hash_value': ['h1', 'h2'], 'completed_tokens': 2, 'is_terminated': lambda self: False})()
        self.ongoing_prefetch[req_id] = (last_host_node, new_input_tokens, [1, 2, 3, 4], op)

    def check_prefetch_progress(self, req_id):
        return True

    def pop_prefetch_loaded_tokens(self, req_id):
        return 4

    def init_load_back(self, params):
        return [1, 2, 3, 4], params.best_match_node

    def load_back(self, node, mem_quota=None):
        return [5, 6, 7, 8]

    def insert(self, params):
        return Result(params.best_match_node)

    def write_backup(self, node, write_back=False):
        return len(node.value)

    def write_backup_storage(self, node):
        return 3

    def inc_lock_ref(self, node):
        node.lock_ref += 1
        return LockResult(-len(node.hash_value) * self.page_size)

    def dec_lock_ref(self, node):
        node.lock_ref = max(0, node.lock_ref - 1)
        return LockResult(len(node.hash_value) * self.page_size)

    def evict(self, params):
        return Result(self.node)

    def check_hicache_events(self):
        return None

    def ready_to_load_host_cache(self):
        return 17

    def flush_write_through_acks(self):
        return None
""",
        encoding="utf-8",
    )
    (tmp / "sglang/srt/managers/cache_controller.py").write_text(
        """
class Operation:
    def __init__(self, request_id='req-1'):
        self.id = 9
        self.request_id = request_id
        self.token_ids = [1, 2, 3, 4]
        self.host_indices = [1, 2, 3, 4]
        self.device_indices = [5, 6, 7, 8]
        self.hash_value = ['h1', 'h2']
        self.prefix_keys = ['p0']
        self.completed_tokens = 64
        self.bytes = 1024

    def is_terminated(self):
        return False

    def mark_terminate(self):
        self.terminated = True

class HiCacheController:
    def __init__(self):
        self.page_size = 2
        self.load_queue = [1]
        self.write_queue = [1]
        self.prefetch_queue = []
        self.backup_queue = []
        self.prefetch_revoke_queue = []
        self.ack_backup_queue = []
        self.host_mem_release_queue = []
        self.prefetch_tokens_occupied = 0
        self.prefetch_capacity_limit = 8

    def load(self, host_indices, priority=None, node_id=-1):
        return [10, 11, 12, 13]

    def start_loading(self):
        self.load_queue = []
        return 7

    def write(self, device_indices, priority=None, node_id=-1):
        return [20, 21, 22, 23]

    def start_writing(self):
        self.write_queue = []

    def prefetch(self, request_id, host_indices, new_input_tokens, last_hash=None, prefix_keys=None):
        op = Operation(request_id)
        self.prefetch_queue.append(op)
        return op

    def prefetch_rate_limited(self):
        return False

    def _storage_hit_query(self, operation):
        return ['h1', 'h2'], 128

    def terminate_prefetch(self, operation):
        operation.mark_terminate()
        return operation.completed_tokens, operation.hash_value

    def append_host_mem_release(self, host_indices):
        self.host_mem_release_queue.append(host_indices)

    def _page_transfer(self, operation):
        operation.completed_tokens += 64

    def write_storage(self, host_indices, token_ids, hash_value=None, prefix_keys=None):
        return 12

    def _page_backup(self, operation):
        operation.completed_tokens += 64
""",
        encoding="utf-8",
    )


def main() -> int:
    run_probe_fixture()
    run_sglang_hicache_target_fixture()
    run_hicache_state_snapshot_fixture()
    run_hicache_token_source_fixture()
    run_hicache_prefetch_progress_source_fixture()
    run_profile_quality_capacity_fixture()
    run_profile_quality_capacity_missing_fixture()
    run_config_fixture()
    run_torch_profiler_lifecycle_fixture()
    run_state_trace_env_fixture()
    run_env_placeholder_fixture()
    run_profile_suite_matrix_fixture()
    run_no_legacy_hicache_page_identity_fixture()
    run_trace_merger_sidecar_only_fixture()
    print("profiling fixtures passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
