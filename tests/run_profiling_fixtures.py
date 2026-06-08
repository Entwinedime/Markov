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
    """返回当前建议的 SGLang HiCache Python probe target set。"""

    return [
        {
            "id": "scheduler.prefetch_kvcache",
            "module": "sglang.srt.managers.scheduler",
            "target": "Scheduler._prefetch_kvcache",
            "events": ["hicache_prefetch_decision_start", "hicache_prefetch_decision_end"],
            "fields": [
                {"name": "request_id", "source": "arg:req.rid"},
                {"name": "host_hit_length", "source": "arg:req.host_hit_length", "required": False},
                {"name": "storage_hit_length", "source": "arg:req.storage_hit_length", "required": False},
                {"name": "fill_tokens", "source": "len:arg:req.fill_ids", "required": False},
                {"name": "page_size", "source": "self.tree_cache.page_size"},
                {"name": "event_role", "source": "const:prefetch_decision"},
            ],
        },
        {
            "id": "hiradix.match_prefix",
            "module": "sglang.srt.mem_cache.hiradix_cache",
            "target": "HiRadixCache.match_prefix",
            "events": ["hicache_lookup_start", "hicache_lookup_end"],
            "fields": [
                {"name": "request_id", "source": "arg:params.req.rid", "required": False},
                {"name": "input_tokens", "source": "len:arg:params.key"},
                {"name": "page_size", "source": "self.page_size"},
                {"name": "page_identity", "source": "page_hashes:arg:params.key,self.page_size"},
                {"name": "device_hit_tokens", "source": "len:return.device_indices", "required": False},
                {"name": "host_hit_length", "source": "return.host_hit_length", "required": False},
                {"name": "last_device_node_id", "source": "return.last_device_node.id", "required": False},
                {"name": "last_host_node_id", "source": "return.last_host_node.id", "required": False},
                {"name": "best_match_node_id", "source": "return.best_match_node.id", "required": False},
                {"name": "event_role", "source": "const:lookup"},
            ],
        },
        {
            "id": "hiradix.prefetch_from_storage",
            "module": "sglang.srt.mem_cache.hiradix_cache",
            "target": "HiRadixCache.prefetch_from_storage",
            "events": ["hicache_prefetch_schedule_start", "hicache_prefetch_schedule_end"],
            "fields": [
                {"name": "request_id", "source": "arg:req_id"},
                {"name": "last_host_node_id", "source": "arg:last_host_node.id"},
                {"name": "new_input_tokens", "source": "len:arg:new_input_tokens"},
                {"name": "page_identity", "source": "page_hashes:arg:new_input_tokens,self.page_size,arg:last_hash"},
                {"name": "prefix_keys", "source": "len:arg:prefix_keys", "required": False},
                {"name": "page_size", "source": "self.page_size"},
                {"name": "event_role", "source": "const:prefetch_schedule"},
            ],
        },
        {
            "id": "hiradix.check_prefetch_progress",
            "module": "sglang.srt.mem_cache.hiradix_cache",
            "target": "HiRadixCache.check_prefetch_progress",
            "events": ["hicache_prefetch_progress_start", "hicache_prefetch_progress_end"],
            "fields": [
                {"name": "request_id", "source": "arg:req_id"},
                {"name": "prefetch_done", "source": "return", "required": False},
                {"name": "ongoing_prefetch_count", "source": "len:self.ongoing_prefetch"},
                {"name": "page_size", "source": "self.page_size"},
                {"name": "event_role", "source": "const:prefetch_progress"},
            ],
        },
        {
            "id": "hiradix.pop_prefetch_loaded_tokens",
            "module": "sglang.srt.mem_cache.hiradix_cache",
            "target": "HiRadixCache.pop_prefetch_loaded_tokens",
            "events": ["hicache_prefetch_loaded_tokens_start", "hicache_prefetch_loaded_tokens_end"],
            "fields": [
                {"name": "request_id", "source": "arg:req_id"},
                {"name": "loaded_tokens", "source": "return", "required": False},
                {"name": "page_size", "source": "self.page_size"},
                {"name": "event_role", "source": "const:prefetch_loaded_tokens"},
            ],
        },
        {
            "id": "hiradix.init_load_back",
            "module": "sglang.srt.mem_cache.hiradix_cache",
            "target": "HiRadixCache.init_load_back",
            "events": ["hicache_init_load_back_start", "hicache_init_load_back_end"],
            "fields": [
                {"name": "request_id", "source": "arg:params.req.rid", "required": False},
                {"name": "host_hit_length", "source": "arg:params.host_hit_length"},
                {"name": "best_match_node_id", "source": "arg:params.best_match_node.id"},
                {"name": "page_identity", "source": "arg:params.best_match_node.hash_value", "required": False},
                {"name": "loaded_tokens", "source": "len:return.0", "required": False},
                {"name": "last_node_id", "source": "return.1.id", "required": False},
                {"name": "page_size", "source": "self.page_size"},
                {"name": "event_role", "source": "const:init_load_back"},
            ],
        },
        {
            "id": "hiradix.load_back",
            "module": "sglang.srt.mem_cache.hiradix_cache",
            "target": "HiRadixCache.load_back",
            "events": ["hicache_load_back_start", "hicache_load_back_end"],
            "fields": [
                {"name": "node_id", "source": "arg:node.id"},
                {"name": "page_identity", "source": "arg:node.hash_value"},
                {"name": "host_tokens", "source": "len:arg:node.host_value", "required": False},
                {"name": "loaded_tokens", "source": "len:return", "required": False},
                {"name": "page_size", "source": "self.page_size"},
                {"name": "event_role", "source": "const:load_back"},
            ],
        },
        {
            "id": "hiradix.insert",
            "module": "sglang.srt.mem_cache.hiradix_cache",
            "target": "HiRadixCache.insert",
            "events": ["hicache_insert_start", "hicache_insert_end"],
            "fields": [
                {"name": "request_id", "source": "arg:params.req.rid", "required": False},
                {"name": "insert_tokens", "source": "len:arg:params.key", "required": False},
                {"name": "value_tokens", "source": "len:arg:params.value", "required": False},
                {"name": "page_identity", "source": "page_hashes:arg:params.key,self.page_size"},
                {"name": "radix_removed_page_identity", "source": "hicache_radix_removed_pages:self", "required": False},
                {"name": "prefix_len", "source": "return.prefix_len", "required": False},
                {"name": "inserted_host_node_id", "source": "return.inserted_host_node.id", "required": False},
                {"name": "page_size", "source": "self.page_size"},
                {"name": "event_role", "source": "const:insert"},
            ],
        },
        {
            "id": "hiradix.write_backup",
            "module": "sglang.srt.mem_cache.hiradix_cache",
            "target": "HiRadixCache.write_backup",
            "events": ["hicache_write_backup_start", "hicache_write_backup_end"],
            "fields": [
                {"name": "node_id", "source": "arg:node.id"},
                {"name": "page_identity", "source": "arg:node.hash_value"},
                {"name": "device_tokens", "source": "len:arg:node.value", "required": False},
                {"name": "write_back", "source": "arg:write_back", "required": False},
                {"name": "written_tokens", "source": "return", "required": False},
                {"name": "page_size", "source": "self.page_size"},
                {"name": "event_role", "source": "const:write_backup"},
            ],
        },
        {
            "id": "hiradix.write_backup_storage",
            "module": "sglang.srt.mem_cache.hiradix_cache",
            "target": "HiRadixCache.write_backup_storage",
            "events": ["hicache_write_storage_schedule_start", "hicache_write_storage_schedule_end"],
            "fields": [
                {"name": "node_id", "source": "arg:node.id"},
                {"name": "host_tokens", "source": "len:arg:node.host_value", "required": False},
                {"name": "page_identity", "source": "arg:node.hash_value"},
                {"name": "hash_pages", "source": "len:arg:node.hash_value", "required": False},
                {"name": "page_size", "source": "self.page_size"},
                {"name": "event_role", "source": "const:write_storage_schedule"},
            ],
        },
        {
            "id": "hiradix.evict",
            "module": "sglang.srt.mem_cache.hiradix_cache",
            "target": "HiRadixCache.evict",
            "events": ["hicache_evict_start", "hicache_evict_end"],
            "fields": [
                {"name": "requested_tokens", "source": "arg:params.num_tokens"},
                {"name": "evicted_tokens", "source": "return.num_tokens_evicted", "required": False},
                {"name": "page_identity", "source": "return.best_match_node.hash_value", "required": False},
                {"name": "page_size", "source": "self.page_size"},
                {"name": "event_role", "source": "const:evict"},
            ],
        },
        {
            "id": "hiradix.inc_lock_ref",
            "module": "sglang.srt.mem_cache.hiradix_cache",
            "target": "HiRadixCache.inc_lock_ref",
            "events": ["hicache_inc_lock_ref_start", "hicache_inc_lock_ref_end"],
            "fields": [
                {"name": "node_id", "source": "arg:node.id", "required": False},
                {"name": "page_identity", "source": "arg:node.hash_value"},
                {"name": "lock_delta", "source": "return.delta", "required": False},
                {"name": "page_size", "source": "self.page_size"},
                {"name": "event_role", "source": "const:lock_ref_inc"},
            ],
        },
        {
            "id": "hiradix.dec_lock_ref",
            "module": "sglang.srt.mem_cache.hiradix_cache",
            "target": "HiRadixCache.dec_lock_ref",
            "events": ["hicache_dec_lock_ref_start", "hicache_dec_lock_ref_end"],
            "fields": [
                {"name": "node_id", "source": "arg:node.id", "required": False},
                {"name": "page_identity", "source": "arg:node.hash_value"},
                {"name": "lock_delta", "source": "return.delta", "required": False},
                {"name": "page_size", "source": "self.page_size"},
                {"name": "event_role", "source": "const:lock_ref_dec"},
            ],
        },
        {
            "id": "controller.load",
            "module": "sglang.srt.managers.cache_controller",
            "target": "HiCacheController.load",
            "events": ["hicache_l2_l1_enqueue_start", "hicache_l2_l1_enqueue_end"],
            "fields": [
                {"name": "node_id", "source": "arg:node_id"},
                {"name": "host_tokens", "source": "len:arg:host_indices"},
                {"name": "device_tokens", "source": "len:return", "required": False},
                {"name": "page_size", "source": "self.page_size"},
                {"name": "event_role", "source": "const:l2_to_l1_enqueue"},
            ],
        },
        {
            "id": "controller.start_loading",
            "module": "sglang.srt.managers.cache_controller",
            "target": "HiCacheController.start_loading",
            "events": ["hicache_l2_l1_start", "hicache_l2_l1_end"],
            "fields": [
                {"name": "producer_id", "source": "return", "required": False},
                {"name": "load_queue_size", "source": "len:self.load_queue", "required": False},
                {"name": "page_size", "source": "self.page_size"},
                {"name": "event_role", "source": "const:l2_to_l1_start"},
            ],
        },
        {
            "id": "controller.write",
            "module": "sglang.srt.managers.cache_controller",
            "target": "HiCacheController.write",
            "events": ["hicache_l1_l2_enqueue_start", "hicache_l1_l2_enqueue_end"],
            "fields": [
                {"name": "node_id", "source": "arg:node_id"},
                {"name": "device_tokens", "source": "len:arg:device_indices"},
                {"name": "host_tokens", "source": "len:return", "required": False},
                {"name": "page_size", "source": "self.page_size"},
                {"name": "event_role", "source": "const:l1_to_l2_enqueue"},
            ],
        },
        {
            "id": "controller.start_writing",
            "module": "sglang.srt.managers.cache_controller",
            "target": "HiCacheController.start_writing",
            "events": ["hicache_l1_l2_start", "hicache_l1_l2_end"],
            "fields": [
                {"name": "write_queue_size", "source": "len:self.write_queue", "required": False},
                {"name": "page_size", "source": "self.page_size"},
                {"name": "event_role", "source": "const:l1_to_l2_start"},
            ],
        },
        {
            "id": "controller.prefetch",
            "module": "sglang.srt.managers.cache_controller",
            "target": "HiCacheController.prefetch",
            "events": ["hicache_l3_prefetch_enqueue_start", "hicache_l3_prefetch_enqueue_end"],
            "fields": [
                {"name": "request_id", "source": "arg:request_id"},
                {"name": "host_tokens", "source": "len:arg:host_indices"},
                {"name": "new_input_tokens", "source": "len:arg:new_input_tokens"},
                {"name": "page_identity", "source": "page_hashes:arg:new_input_tokens,self.page_size,arg:last_hash"},
                {"name": "operation_id", "source": "return.id", "required": False},
                {"name": "page_size", "source": "self.page_size"},
                {"name": "event_role", "source": "const:l3_prefetch_enqueue"},
            ],
        },
        {
            "id": "controller.storage_hit_query",
            "module": "sglang.srt.managers.cache_controller",
            "target": "HiCacheController._storage_hit_query",
            "events": ["hicache_l3_hit_query_start", "hicache_l3_hit_query_end"],
            "fields": [
                {"name": "request_id", "source": "arg:operation.request_id", "required": False},
                {"name": "token_count", "source": "len:arg:operation.token_ids"},
                {"name": "page_identity", "source": "return.0", "required": False},
                {"name": "hit_pages", "source": "len:return.0", "required": False},
                {"name": "hit_tokens", "source": "return.1", "required": False},
                {"name": "page_size", "source": "self.page_size"},
                {"name": "event_role", "source": "const:l3_hit_query"},
            ],
        },
        {
            "id": "controller.page_transfer",
            "module": "sglang.srt.managers.cache_controller",
            "target": "HiCacheController._page_transfer",
            "events": ["hicache_l3_l2_transfer_start", "hicache_l3_l2_transfer_end"],
            "fields": [
                {"name": "request_id", "source": "arg:operation.request_id", "required": False},
                {"name": "page_identity", "source": "arg:operation.hash_value"},
                {"name": "hash_pages", "source": "len:arg:operation.hash_value", "required": False},
                {"name": "completed_tokens", "source": "arg:operation.completed_tokens", "required": False},
                {"name": "page_size", "source": "self.page_size"},
                {"name": "event_role", "source": "const:l3_to_l2_transfer"},
            ],
        },
        {
            "id": "controller.write_storage",
            "module": "sglang.srt.managers.cache_controller",
            "target": "HiCacheController.write_storage",
            "events": ["hicache_l2_l3_enqueue_start", "hicache_l2_l3_enqueue_end"],
            "fields": [
                {"name": "host_tokens", "source": "len:arg:host_indices"},
                {"name": "token_count", "source": "len:arg:token_ids"},
                {"name": "page_identity", "source": "arg:hash_value"},
                {"name": "hash_pages", "source": "len:arg:hash_value", "required": False},
                {"name": "operation_id", "source": "return", "required": False},
                {"name": "page_size", "source": "self.page_size"},
                {"name": "event_role", "source": "const:l2_to_l3_enqueue"},
            ],
        },
        {
            "id": "controller.page_backup",
            "module": "sglang.srt.managers.cache_controller",
            "target": "HiCacheController._page_backup",
            "events": ["hicache_l2_l3_transfer_start", "hicache_l2_l3_transfer_end"],
            "fields": [
                {"name": "operation_id", "source": "arg:operation.id", "required": False},
                {"name": "page_identity", "source": "arg:operation.hash_value"},
                {"name": "hash_pages", "source": "len:arg:operation.hash_value", "required": False},
                {"name": "completed_tokens", "source": "arg:operation.completed_tokens", "required": False},
                {"name": "page_size", "source": "self.page_size"},
                {"name": "event_role", "source": "const:l2_to_l3_transfer"},
            ],
        },
    ]


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


def run_hicache_page_hash_literal_page_size_fixture() -> None:
    """验证 HiCache page hash source 支持字面量 target page size。"""

    from trace_sim_probe.probes import sglang_hicache_callable

    bound = {"tokens": [1, 2, 3, 4, 5], "page_size": 2}
    found_base, base_hashes = sglang_hicache_callable._extract_page_hashes(
        "arg:tokens,arg:page_size",
        bound,
        (),
        {},
        None,
    )
    found_literal, literal_hashes = sglang_hicache_callable._extract_page_hashes(
        "arg:tokens,2",
        bound,
        (),
        {},
        None,
    )
    found_const, const_hashes = sglang_hicache_callable._extract_page_hashes(
        "arg:tokens,const:2",
        bound,
        (),
        {},
        None,
    )
    found_larger, larger_hashes = sglang_hicache_callable._extract_page_hashes(
        "arg:tokens,4",
        bound,
        (),
        {},
        None,
    )

    assert found_base is True, base_hashes
    assert found_literal is True, literal_hashes
    assert found_const is True, const_hashes
    assert found_larger is True, larger_hashes
    assert literal_hashes == base_hashes == const_hashes, (base_hashes, literal_hashes, const_hashes)
    assert len(literal_hashes) == 2, literal_hashes
    assert len(larger_hashes) == 1, larger_hashes

    concat_found, concat_hashes = sglang_hicache_callable._extract_page_hashes_concat(
        "arg:prefix,arg:suffix,2",
        {"prefix": [1, 2], "suffix": [3, 4, 5]},
        (),
        {},
        None,
    )
    full_found, full_hashes = sglang_hicache_callable._extract_page_hashes(
        "arg:tokens,2",
        {"tokens": [1, 2, 3, 4, 5]},
        (),
        {},
        None,
    )
    assert concat_found is True, concat_hashes
    assert full_found is True, full_hashes
    assert concat_hashes == full_hashes, (concat_hashes, full_hashes)

    suffix_found, suffix_hashes = sglang_hicache_callable._extract_page_hashes_after_prefix(
        "arg:prefix,arg:suffix,2",
        {"prefix": [1, 2], "suffix": [3, 4, 5, 6]},
        (),
        {},
        None,
    )
    suffix_full_found, suffix_full_hashes = sglang_hicache_callable._extract_page_hashes(
        "arg:tokens,2",
        {"tokens": [1, 2, 3, 4, 5, 6]},
        (),
        {},
        None,
    )
    assert suffix_found is True, suffix_hashes
    assert suffix_full_found is True, suffix_full_hashes
    assert suffix_hashes == suffix_full_hashes[-2:], (suffix_hashes, suffix_full_hashes)

    nonaligned_found, nonaligned_hashes = sglang_hicache_callable._extract_page_hashes_after_prefix(
        "arg:prefix,arg:suffix,2",
        {"prefix": [1, 2, 3], "suffix": [4, 5, 6]},
        (),
        {},
        None,
    )
    prefix_found, prefix_hashes = sglang_hicache_callable._extract_page_hashes(
        "arg:prefix,2",
        {"prefix": [1, 2, 3]},
        (),
        {},
        None,
    )
    expected_suffix_found, expected_suffix_hashes = sglang_hicache_callable._extract_page_hashes(
        f"arg:suffix,2,const:{prefix_hashes[-1]}",
        {"suffix": [4, 5, 6]},
        (),
        {},
        None,
    )
    assert nonaligned_found is True, nonaligned_hashes
    assert prefix_found is True, prefix_hashes
    assert expected_suffix_found is True, expected_suffix_hashes
    assert nonaligned_hashes == expected_suffix_hashes, (nonaligned_hashes, expected_suffix_hashes)


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
    """验证 state_trace 配置会补充环境变量和 validation-only source。"""

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
                        "id": "hiradix.match_prefix",
                        "module": "sglang.srt.mem_cache.hiradix_cache",
                        "target": "HiRadixCache.match_prefix",
                        "fields": [
                            {"name": "radix_removed_page_identity", "source": "hicache_radix_removed_pages:self", "required": False},
                            {"name": "event_role", "source": "const:lookup"},
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
    state_index = next(index for index, field in enumerate(fields) if field.get("source") == "hicache_state:self")
    radix_index = next(index for index, field in enumerate(fields) if field.get("source") == "hicache_radix_removed_pages:self")
    assert state_index < radix_index, fields


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


def run_hicache_radix_removed_materialization_fixture() -> None:
    """验证 runner 收尾能从同次 insert start/end snapshot 派生 radix removed pages。"""

    spec = importlib.util.spec_from_file_location(
        "trace_sim_profile_runner_radix_materialize",
        ROOT / "scripts/internal/profile_runner.py",
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[str(spec.name)] = module
    spec.loader.exec_module(module)

    with tempfile.TemporaryDirectory() as raw_tmp:
        tmp = Path(raw_tmp)
        trace_dir = tmp / "trace"
        python_probe_dir = trace_dir / "python_probe"
        python_probe_dir.mkdir(parents=True)
        trace_path = python_probe_dir / "python_probe_trace.rank0.pid1.json"

        def snapshot_event(name: str, dur: int, pages: list[str]) -> dict[str, object]:
            return {
                "name": name,
                "cat": "python_probe",
                "pid": 1,
                "tid": 1,
                "ts": 100,
                "dur": dur,
                "args": {
                    "target_id": "hiradix.insert",
                    "model_input": False,
                    "state_snapshot": {
                        "nodes": [
                            {
                                "hash_value": pages,
                            }
                        ]
                    },
                },
            }

        payload = {
            "traceEvents": [
                {
                    "name": "hicache_insert_start",
                    "cat": "python_probe",
                    "pid": 1,
                    "tid": 1,
                    "ts": 100,
                    "dur": 0,
                    "args": {
                        "target_id": "hiradix.insert",
                        "radix_removed_page_identity": [],
                    },
                },
                snapshot_event("hicache_insert_start:state_snapshot", 0, ["old_a", "old_b", "kept"]),
                {
                    "name": "hicache_insert_end",
                    "cat": "python_probe",
                    "pid": 1,
                    "tid": 1,
                    "ts": 100,
                    "dur": 7,
                    "args": {
                        "target_id": "hiradix.insert",
                        "radix_removed_page_identity": [],
                    },
                },
                snapshot_event("hicache_insert_end:state_snapshot", 7, ["kept", "new_a"]),
            ]
        }
        trace_path.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")

        summary = module.materialize_hicache_radix_removed_pages(trace_dir)
        assert summary["files_scanned"] == 1, summary
        assert summary["files_updated"] == 1, summary
        assert summary["insert_end_events"] == 1, summary
        assert summary["materialized_events"] == 1, summary
        assert summary["materialized_pages"] == 2, summary

        updated = json.loads(trace_path.read_text(encoding="utf-8"))
        end_event = next(event for event in updated["traceEvents"] if event["name"] == "hicache_insert_end")
        assert end_event["args"]["radix_removed_page_identity"] == ["old_a", "old_b"], end_event
        snapshots = [event for event in updated["traceEvents"] if event["name"].endswith(":state_snapshot")]
        assert all(event["args"]["model_input"] is False for event in snapshots), snapshots


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
from sglang.srt.mem_cache.hiradix_cache import HiRadixCache, Node, Params
from sglang.srt.managers.cache_controller import HiCacheController, Operation

req = Req('req-1')
scheduler = Scheduler()
scheduler._prefetch_kvcache(req)

cache = HiRadixCache()
node = Node(11)
params = Params(req=req, key=[1, 2, 3, 4], value=[5, 6, 7, 8], best_match_node=node, host_hit_length=4, num_tokens=4)
cache.match_prefix(params)
cache.prefetch_from_storage(req.rid, node, [1, 2, 3, 4], '0' * 64, ['p0'])
cache.check_prefetch_progress(req.rid)
cache.pop_prefetch_loaded_tokens(req.rid)
cache.init_load_back(params)
cache.load_back(node)
cache.insert(params)
cache.write_backup(node, write_back=True)
cache.write_backup_storage(node)
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
            row.get("target_id") == "hiradix.match_prefix"
            and row.get("request_id") == "req-1"
            and row.get("input_tokens") == 4
            and row.get("host_hit_length") == 4
            for row in rows
        )
        assert any(
            row.get("target_id") == "controller.storage_hit_query"
            and row.get("hit_pages") == 2
            and row.get("hit_tokens") == 128
            for row in rows
        )
        assert any(
            row.get("target_id") == "hiradix.match_prefix"
            and len(row.get("page_identity") or []) == 2
            for row in rows
        )
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
                    {"name": "page_identity", "source": "page_hashes:arg:params.key,self.page_size"},
                    {"name": "event_role", "source": "const:lookup"},
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


def run_hicache_radix_removed_pages_phase_fixture() -> None:
    """验证 insert 返回 None 时仍按 phase 提取 radix removed pages。"""

    from trace_sim_probe.probes import sglang_hicache_callable

    with tempfile.TemporaryDirectory() as raw_tmp:
        tmp = Path(raw_tmp)
        package = tmp / "radix_pkg"
        package.mkdir()
        (package / "__init__.py").write_text("", encoding="utf-8")
        (package / "demo.py").write_text(
            """
class Node:
    def __init__(self):
        self.id = 1
        self.hash_value = ['old_a', 'old_b']
        self.value = [1, 2]
        self.host_value = [3, 4]

class Params:
    pass

class Cache:
    def __init__(self):
        self.page_size = 2
        self.node = Node()

    def insert(self, params):
        self.node.hash_value = []
        return None
""",
            encoding="utf-8",
        )
        output_dir = tmp / "python_probe"
        targets = [
            {
                "id": "hiradix.insert",
                "module": "radix_pkg.demo",
                "target": "Cache.insert",
                "events": ["hicache_insert_start", "hicache_insert_end"],
                "fields": [
                    {"name": "state_snapshot", "source": "hicache_state:self", "required": False},
                    {"name": "radix_removed_page_identity", "source": "hicache_radix_removed_pages:self", "required": False},
                    {"name": "event_role", "source": "const:insert"},
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
                "from radix_pkg.demo import Cache, Params; cache = Cache(); assert cache.insert(Params()) is None",
            ],
            env=env,
        )

        files = sorted(output_dir.glob("python_probe_trace.*.json"))
        assert len(files) == 1, files
        payload = json.loads(files[0].read_text(encoding="utf-8"))
        events = [event for event in payload["traceEvents"] if event.get("cat") == "python_probe"]
        real_end = [event for event in events if event["name"] == "hicache_insert_end"]
        snapshots = [event for event in events if event["name"].endswith(":state_snapshot")]
        assert len(real_end) == 1, events
        assert len(snapshots) == 2, events
        assert real_end[0]["args"]["radix_removed_page_identity"] == ["old_a", "old_b"], real_end
        assert all(event["args"].get("model_input") is False for event in snapshots), snapshots

    class DirectNode:
        def __init__(self):
            self.hash_value = ["direct_old"]

    class DirectCache:
        def __init__(self):
            self.node = DirectNode()

    direct_cache = DirectCache()
    sglang_hicache_callable._RADIX_PAGES_BEFORE_CALL_BY_OBJECT.clear()
    handled, found, value = sglang_hicache_callable._hicache_radix_removed_pages_source(
        "hicache_radix_removed_pages:self",
        "radix_removed_page_identity",
        {},
        (direct_cache,),
        {},
        None,
    )
    assert handled and found and value == [], value
    direct_cache.node.hash_value = []
    handled, found, value = sglang_hicache_callable._hicache_radix_removed_pages_source(
        "hicache_radix_removed_pages:self",
        "radix_removed_page_identity",
        {},
        (direct_cache,),
        {},
        None,
    )
    assert handled and found and value == ["direct_old"], value

    class SnapshotNode:
        def __init__(self):
            self.hash_value = ["snapshot_old"]

    class SnapshotCache:
        def __init__(self):
            self.node = SnapshotNode()

    snapshot_cache = SnapshotCache()
    object_id = id(snapshot_cache)
    snapshot_object_id = f"{type(snapshot_cache).__name__}:{object_id}"
    sglang_hicache_callable._RADIX_PAGES_BEFORE_CALL_BY_OBJECT.clear()
    sglang_hicache_callable._RADIX_PAGES_AT_STATE_SNAPSHOT_BY_OBJECT.clear()
    sglang_hicache_callable._RADIX_REMOVED_AT_STATE_SNAPSHOT_BY_OBJECT.clear()
    sglang_hicache_callable._record_radix_state_snapshot_delta(
        sglang_hicache_callable._snapshot_hicache_object(snapshot_cache)
    )
    handled, found, value = sglang_hicache_callable._hicache_radix_removed_pages_source(
        "hicache_radix_removed_pages:self",
        "radix_removed_page_identity",
        {"__trace_sim_phase": "start"},
        (snapshot_cache,),
        {},
        None,
    )
    assert handled and found and value == [], value
    snapshot_cache.node.hash_value = []
    sglang_hicache_callable._record_radix_state_snapshot_delta(
        sglang_hicache_callable._snapshot_hicache_object(snapshot_cache)
    )
    sglang_hicache_callable._RADIX_PAGES_BEFORE_CALL_BY_OBJECT[object_id] = [set()]
    assert sglang_hicache_callable._RADIX_REMOVED_AT_STATE_SNAPSHOT_BY_OBJECT[snapshot_object_id] == ["snapshot_old"]
    handled, found, value = sglang_hicache_callable._hicache_radix_removed_pages_source(
        "hicache_radix_removed_pages:self",
        "radix_removed_page_identity",
        {"__trace_sim_phase": "end"},
        (snapshot_cache,),
        {},
        None,
    )
    assert handled and found and value == ["snapshot_old"], value


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
    assert quality["page_identity_coverage"]["stateful_required_events_missing_page_identity"] == 0, quality


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
class Req:
    def __init__(self, rid):
        self.rid = rid
        self.host_hit_length = 4
        self.storage_hit_length = 0
        self.fill_ids = [1, 2, 3, 4, 5, 6]

class TreeCache:
    page_size = 2

class Scheduler:
    def __init__(self):
        self.tree_cache = TreeCache()

    def _prefetch_kvcache(self, req):
        return req.rid
""",
        encoding="utf-8",
    )
    (tmp / "sglang/srt/mem_cache/hiradix_cache.py").write_text(
        """
class Node:
    def __init__(self, node_id):
        self.id = node_id
        self.host_value = [1, 2, 3, 4]
        self.value = [5, 6, 7, 8]
        self.hash_value = ['h1', 'h2']

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

class Params:
    def __init__(self, req, key, value, best_match_node, host_hit_length, num_tokens):
        self.req = req
        self.key = key
        self.value = value
        self.best_match_node = best_match_node
        self.host_hit_length = host_hit_length
        self.num_tokens = num_tokens

class HiRadixCache:
    def __init__(self):
        self.page_size = 2
        self.ongoing_prefetch = {}
        self.prefetch_stop_policy = 'best_effort'
        self.write_through_threshold = 1
        self.load_back_threshold = 10
        self.cache_controller = ControllerSnapshot()
        self.kv_cache = self.cache_controller.mem_pool_device
        self.token_to_kv_pool_host = self.cache_controller.mem_pool_host
        self.node = Node(21)

    def match_prefix(self, params):
        return Result(params.best_match_node)

    def prefetch_from_storage(self, req_id, last_host_node, new_input_tokens, last_hash=None, prefix_keys=None):
        self.ongoing_prefetch[req_id] = last_host_node

    def check_prefetch_progress(self, req_id):
        return True

    def pop_prefetch_loaded_tokens(self, req_id):
        return 4

    def init_load_back(self, params):
        return [1, 2, 3, 4], params.best_match_node

    def load_back(self, node):
        return [5, 6, 7, 8]

    def insert(self, params):
        return Result(params.best_match_node)

    def write_backup(self, node, write_back=False):
        return len(node.value)

    def write_backup_storage(self, node):
        return 3

    def inc_lock_ref(self, node):
        return LockResult(-len(node.hash_value) * self.page_size)

    def dec_lock_ref(self, node):
        return LockResult(len(node.hash_value) * self.page_size)

    def evict(self, params):
        return Result(self.node)
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
        self.hash_value = ['h1', 'h2']
        self.completed_tokens = 64

class HiCacheController:
    def __init__(self):
        self.page_size = 2
        self.load_queue = [1]
        self.write_queue = [1]

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
        return Operation(request_id)

    def _storage_hit_query(self, operation):
        return ['h1', 'h2'], 128

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
    run_hicache_radix_removed_pages_phase_fixture()
    run_hicache_page_hash_literal_page_size_fixture()
    run_hicache_prefetch_progress_source_fixture()
    run_profile_quality_capacity_fixture()
    run_profile_quality_capacity_missing_fixture()
    run_config_fixture()
    run_torch_profiler_lifecycle_fixture()
    run_state_trace_env_fixture()
    run_env_placeholder_fixture()
    run_hicache_radix_removed_materialization_fixture()
    run_trace_merger_sidecar_only_fixture()
    print("profiling fixtures passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
