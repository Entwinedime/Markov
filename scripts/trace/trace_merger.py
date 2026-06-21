"""合并 torch profiler、LD_PRELOAD 和 Python probe 生成的 Chrome trace。

trace_merger 是 profiling 到 C++ modeling 之间的 trace 合流层。它只把 runtime
wrapper 参数注入对应 profiler event，并把 Python sidecar 事实附加到 merged trace；
它不推导 HiCache policy，也不生成 synthetic model_input 事件。
"""

import argparse
import json
import logging
import os
import bisect
import re
import shutil
from collections import defaultdict
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Tuple, Dict, Any, List, Optional

logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")

DIRECT_MERGED_EVENT_NAMES = {"CPUInfer::submit", "CPUInfer::sync"}
DIRECT_MERGED_NAME_PREFIXES = ("HiCache::", "hicache_")


@dataclass
class MergeReport:
    """单次 trace merge 的质量摘要。

    report 字段既供 runner 判断合并是否成功，也供后续人工审查匹配质量；字段名
    保持稳定，避免 downstream validation 需要解析日志文本。
    """

    profiler_path: str
    custom_path: str
    out_path: str
    mode: str
    tolerance_us: float
    search_window: int
    margin_us: float
    success: bool = False
    cann_pid: int = 0
    custom_key_count: int = 0
    need_match: int = 0
    matched: int = 0
    unmatched: int = 0
    rejected_count: int = 0
    count_mismatch_count: int = 0
    occupied_collision_count: int = 0
    fallback_count: int = 0
    later_match_count: int = 0
    max_match_diff_us: float = 0.0
    standalone_custom_appended: int = 0
    sidecar_events_appended: int = 0
    sidecar_paths: List[str] = field(default_factory=list)
    sidecar_details: List[Dict[str, Any]] = field(default_factory=list)
    error: str = ""

    def to_dict(self) -> Dict[str, Any]:
        """转换为 JSON 可序列化字典，保持 report 字段名稳定。"""

        return asdict(self)


def write_report(report: MergeReport, report_path: str) -> None:
    """写入 merge report，保证父目录存在。"""

    os.makedirs(os.path.dirname(os.path.abspath(report_path)), exist_ok=True)
    with open(report_path, "w", encoding="utf-8") as report_file:
        json.dump(report.to_dict(), report_file, indent=2, sort_keys=True)

def load_trace(file_path: str, auto_repair: bool = False) -> Tuple[Any, List[Dict]]:
    """读取 Chrome trace JSON。

    LD_PRELOAD trace 在异常退出时可能缺少最后的 `]`；auto_repair 只修复这一种
    明确可恢复的尾部缺口，不尝试修复任意损坏 JSON。Python probe 流式 trace
    也可能缺少最后的 `]}`，按同一原则补齐。
    """

    if not file_path or not os.path.exists(file_path):
        logging.error(f"File '{file_path}' does not exist.")
        return None, []

    logging.info(f"Loading '{file_path}' ...")
    with open(file_path, 'r', encoding='utf-8') as file_object:
        content = file_object.read().strip()

    if auto_repair:
        repaired = repair_trace_tail(content)
        if repaired != content:
            logging.info(f"Fixing missing Chrome trace tail in '{file_path}'...")
            content = repaired

    try:
        data = json.loads(content)
        events = data.get("traceEvents", []) if isinstance(data, dict) else data
        return data, events
    except json.JSONDecodeError as error:
        logging.error(f"Failed to parse JSON: {error}")
        return None, []

def repair_trace_tail(content: str) -> str:
    """补齐流式 Chrome trace 常见的尾部未闭合。"""

    stripped = content.strip()
    if stripped.startswith('{"traceEvents":[') and not stripped.endswith("]}"):
        end = stripped.rfind("}")
        if end >= 0:
            return stripped[: end + 1].rstrip(",") + "]}"
    if stripped.startswith("[") and not stripped.endswith("]"):
        end = stripped.rfind("}")
        if end >= 0:
            return stripped[: end + 1].rstrip(",") + "\n]"
    return content

def get_cann_pid(events: List[Dict]) -> int:
    """从 metadata event 中查找 CANN 进程 pid。"""

    return next((
        event.get("pid", 0) for event in events 
        if event.get("name") == "process_name" and event.get("args", {}).get("name") == "CANN"
    ), 0)

def get_earliest_timestamp(events: List[Dict]) -> float:
    """读取 trace 中最早 timestamp，用于过滤 profiler 开始前的 sidecar 事件。"""

    earliest = float('inf')
    for event in events:
        if 'ts' in event:
            earliest = min(earliest, float(event['ts']))
    return earliest

def build_custom_event_map(custom_events: List[Dict]) -> Dict[Tuple[int, str], Tuple[List[float], List[Dict]]]:
    """按 `(tid, name)` 建立 LD_PRELOAD event 查找表。"""

    temp_map = defaultdict(list)
    for event in custom_events:
        if event.get("ph") == "X" and "args" in event:
            temp_map[(event.get("tid"), event.get("name"))].append(
                (float(event.get("ts", 0)), event["args"])
            )

    custom_map = {}
    for key, values in temp_map.items():
        values.sort(key=lambda item: item[0])
        # 将 timestamp 与 args 分离，search 模式可以直接对 timestamp 数组做 bisect。
        custom_map[key] = ([item[0] for item in values], [item[1] for item in values])
        
    return custom_map

def inject_custom_args(profiler_event: Dict, custom_args: Dict):
    """把 LD_PRELOAD 采集到的 wrapper 参数注入 profiler event。"""

    profiler_args = profiler_event.setdefault("args", {})
    function_args = custom_args.get("Function-Args", {})

    if "stream" in function_args:
        profiler_args["Raw Stream"] = function_args["stream"]
    if "event" in function_args:
        profiler_args["Event Id"] = function_args["event"]

    profiler_args.update(custom_args)

def execute_sequential_match(profiler_events: List[Dict], custom_map: Dict, cann_pid: int,
                             earliest_profiler_ts: float, margin_us: float, tolerance_us: float) -> Tuple[int, int, Dict[str, Any]]:
    """按同 key 顺序一一匹配 profiler event 和 LD_PRELOAD event。

    sequential 模式要求每个 `(tid, name)` 下 profiler/custom 数量完全一致，适合
    稳定 trace 的严格审计；数量不一致时直接跳过该 key，避免错位注入。
    """

    need_to_be_matched = 0
    successfully_matched = 0
    diagnostics: Dict[str, Any] = {
        "rejected_count": 0,
        "count_mismatch_count": 0,
        "later_match_count": 0,
        "max_match_diff_us": 0.0,
    }

    logging.info("Using 'sequential' matching mode.")
    
    # 第一步：按 `(tid, name)` 分组，只处理 CANN pid 下能和 custom_map 对齐的 event。
    profiler_groups = defaultdict(list)
    for event in profiler_events:
        if event.get("ph") != "X" or event.get("pid") != cann_pid or not event.get("name"):
            continue
        key = (event.get("tid"), event["name"])
        if key in custom_map:
            profiler_groups[key].append(event)

    # 第二步：分组内按 timestamp 顺序注入，超过 tolerance 的样本保留为未匹配。
    for key, profiler_event_list in profiler_groups.items():
        profiler_event_list.sort(key=lambda e: float(e.get("ts", 0)))
        
        custom_timestamps, custom_args_list = custom_map[key]
        
        valid_custom_indices = [
            idx for idx, ts in enumerate(custom_timestamps) 
            if ts >= earliest_profiler_ts - margin_us
        ]
        
        need_to_be_matched += len(profiler_event_list)
        
        if len(profiler_event_list) != len(valid_custom_indices):
            diagnostics["count_mismatch_count"] += 1
            logging.warning(f"Key {key}: Count mismatch. Profiler needs {len(profiler_event_list)}, "
                            f"Custom has {len(valid_custom_indices)}. Skipping sequential match.")
            continue
            
        logging.info(f"Key {key}: Exact count match ({len(profiler_event_list)}). Injecting sequentially.")
        
        for profiler_index, profiler_event in enumerate(profiler_event_list):
            custom_index = valid_custom_indices[profiler_index]
            
            profiler_timestamp = float(profiler_event.get("ts", 0))
            custom_timestamp = custom_timestamps[custom_index]
            time_difference = abs(profiler_timestamp - custom_timestamp)
            diagnostics["max_match_diff_us"] = max(diagnostics["max_match_diff_us"], time_difference)
            
            if time_difference > tolerance_us:
                diagnostics["rejected_count"] += 1
                logging.warning(f"Key {key}: Match rejected due to time diff {time_difference} us > tolerance {tolerance_us} us.")
                continue
                
            if profiler_timestamp < custom_timestamp:
                diagnostics["later_match_count"] += 1

            inject_custom_args(profiler_event, custom_args_list[custom_index])
            successfully_matched += 1

    return need_to_be_matched, successfully_matched, diagnostics

def execute_search_match(profiler_events: List[Dict], custom_map: Dict, cann_pid: int,
                         search_window: int, tolerance_us: float) -> Tuple[int, int, Dict[str, Any]]:
    """用 timestamp 附近窗口搜索匹配 LD_PRELOAD event。

    search 模式允许 profiler/custom 数量不完全一致，但每个 custom event 只能被占用一次；
    collision 会记录到 diagnostics，避免静默把同一 wrapper 参数注入多个 profiler event。
    """

    need_to_be_matched = 0
    successfully_matched = 0
    used_custom_indices = {}
    diagnostics: Dict[str, Any] = {
        "occupied_collision_count": 0,
        "fallback_count": 0,
        "later_match_count": 0,
        "max_match_diff_us": 0.0,
    }

    for event in profiler_events:
        if event.get("ph") != "X" or event.get("pid") != cann_pid or not event.get("name"):
            continue

        key = (event.get("tid"), event["name"])
        if key not in custom_map:
            continue
            
        need_to_be_matched += 1
        profiler_ts = float(event.get("ts", 0))
        custom_timestamps, custom_args_list = custom_map[key]

        insert_index = bisect.bisect_right(custom_timestamps, profiler_ts)
        
        candidates = []
        start_index = max(0, insert_index - search_window)
        end_index = min(len(custom_timestamps), insert_index + search_window)
        
        for custom_index in range(start_index, end_index):
            time_difference = abs(profiler_ts - custom_timestamps[custom_index])
            if time_difference <= tolerance_us:
                candidates.append({
                    'diff': time_difference,
                    'key': (key[0], key[1], custom_index),
                    'args': custom_args_list[custom_index],
                    'ts': custom_timestamps[custom_index]
                })

        candidates.sort(key=lambda item: item['diff'])
        
        best_candidate = None
        fallback_triggered = False
        
        for candidate in candidates:
            candidate_key = candidate['key']
            if candidate_key in used_custom_indices:
                fallback_triggered = True
                diagnostics["occupied_collision_count"] += 1
                logging.warning(f"Custom event {candidate_key} at {candidate['ts']} is already occupied by earlier profiler event at {used_custom_indices[candidate_key]}.\n"
                                f"Profiler event '{event['name']}' at {profiler_ts} trying next best.")
                continue

            if profiler_ts < candidate['ts']:
                diagnostics["later_match_count"] += 1
            
            best_candidate = candidate
            break

        if best_candidate:
            diagnostics["max_match_diff_us"] = max(diagnostics["max_match_diff_us"], best_candidate["diff"])
            if fallback_triggered:
                diagnostics["fallback_count"] += 1
                logging.warning(f"Profiler event '{event['name']}' at {profiler_ts} used fallback custom event "
                                f"with {best_candidate['diff']} us difference.")

            used_custom_indices[best_candidate['key']] = profiler_ts
            inject_custom_args(event, best_candidate['args'])
            successfully_matched += 1

    return need_to_be_matched, successfully_matched, diagnostics

def append_unmatched_custom_events(profiler_events: List[Dict], custom_events: List[Dict], cutoff_ts: float) -> int:
    """把不需要匹配 profiler event 的事实事件直接附加到 merged trace。"""

    appended_count = 0
    for event in custom_events:
        if should_append_standalone_event(event):
            if float(event.get("ts", 0)) >= cutoff_ts:
                appended_count += 1
                profiler_events.append(event)
    logging.info(f"Appended {appended_count} standalone custom events to the merged trace.")
    return appended_count

def should_append_standalone_event(event: Dict[str, Any]) -> bool:
    """判断 custom event 是否应作为独立事实保留。"""

    name = str(event.get("name", ""))
    if name in DIRECT_MERGED_EVENT_NAMES:
        return True
    if any(name.startswith(prefix) for prefix in DIRECT_MERGED_NAME_PREFIXES):
        return True
    args = event.get("args") if isinstance(event.get("args"), dict) else {}
    domain = str(args.get("domain") or event.get("cat") or "").lower()
    return domain in {"hicache", "cache_io", "python_probe"}

def append_sidecar_trace_events(profiler_events: List[Dict], sidecar_paths: List[str]) -> Tuple[int, List[Dict[str, Any]]]:
    """附加 Python probe sidecar trace，并返回可审计的路径明细。"""

    appended_count = 0
    details: List[Dict[str, Any]] = []
    for sidecar_path in sidecar_paths:
        _, sidecar_events = load_trace(sidecar_path, auto_repair=True)
        if not sidecar_events:
            logging.warning("No sidecar events loaded from %s", sidecar_path)
            details.append({"path": sidecar_path, "events": 0, "loaded": False})
            continue
        profiler_events.extend(sidecar_events)
        appended_count += len(sidecar_events)
        details.append({"path": sidecar_path, "events": len(sidecar_events), "loaded": True})
    if sidecar_paths:
        logging.info(f"Appended {appended_count} sidecar trace events.")
    return appended_count, details

def sort_events(events: List[Dict[str, Any]]) -> None:
    """按 Chrome trace 可读性稳定排序 merged event。"""

    events.sort(key=lambda event: (float(event.get("ts", 0) or 0), str(event.get("pid", "")), str(event.get("tid", "")), str(event.get("name", ""))))

def merge_traces(profiler_path: str, custom_path: str, out_path: str,
                 tolerance_us: float = 10000.0, search_window: int = 5, margin_us: float = 0.0, mode: str = "search",
                 sidecar_paths: Optional[List[str]] = None, report_path: Optional[str] = None) -> Dict[str, Any]:
    """合并单组 torch profiler、LD_PRELOAD 和可选 Python sidecar trace。"""

    report = MergeReport(
        profiler_path=profiler_path,
        custom_path=custom_path,
        out_path=out_path,
        mode=mode,
        tolerance_us=tolerance_us,
        search_window=search_window,
        margin_us=margin_us,
        sidecar_paths=list(sidecar_paths or []),
    )

    raw_data, profiler_events = load_trace(profiler_path)
    _, custom_events = load_trace(custom_path, auto_repair=True)

    if not profiler_events or not custom_events:
        logging.error("Failed to load events. Aborting merge.")
        report.error = "failed_to_load_events"
        if report_path:
            write_report(report, report_path)
        return report.to_dict()

    cann_pid = get_cann_pid(profiler_events)
    report.cann_pid = cann_pid
    logging.info(f"Identified CANN PID: {cann_pid}")

    custom_map = build_custom_event_map(custom_events)
    report.custom_key_count = len(custom_map)
    logging.info(f"Built custom map with {len(custom_map)} unique keys.")

    earliest_profiler_ts = get_earliest_timestamp(profiler_events)

    if mode == "sequential":
        need_match, matched, diagnostics = execute_sequential_match(
            profiler_events, custom_map, cann_pid, earliest_profiler_ts, margin_us, tolerance_us
        )
    else:
        need_match, matched, diagnostics = execute_search_match(
            profiler_events, custom_map, cann_pid, search_window, tolerance_us
        )
    report.need_match = need_match
    report.matched = matched
    report.unmatched = max(0, need_match - matched)
    for key, value in diagnostics.items():
        if hasattr(report, key):
            setattr(report, key, value)

    if need_match != matched:
        logging.warning(f"Matched {matched} out of {need_match} events. Some events missing/not properly merged.")
    else:
        logging.info(f"Successfully matched and injected args into {matched} events.")
        
    cutoff_ts = earliest_profiler_ts - margin_us
    report.standalone_custom_appended = append_unmatched_custom_events(profiler_events, custom_events, cutoff_ts)
    report.sidecar_events_appended, report.sidecar_details = append_sidecar_trace_events(profiler_events, sidecar_paths or [])
    sort_events(profiler_events)

    logging.info(f"Saving merged trace to '{out_path}' ...")
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, 'w', encoding='utf-8') as output_file:
        json.dump(raw_data, output_file)

    report.success = True
    if report_path:
        write_report(report, report_path)
        
    logging.info("Merge complete! Drag it into https://ui.perfetto.dev to view.")
    return report.to_dict()

def merge_manifest(manifest_path: str, out_dir: str, tolerance_us: float = 10000.0, search_window: int = 5,
                   margin_us: float = 100.0, mode: str = "search") -> Dict[str, Any]:
    """按 profile_manifest.json 批量合并一次 profiling run 的所有 trace channel。"""

    manifest = json.loads(Path(manifest_path).read_text(encoding="utf-8"))
    trace = manifest.get("trace") if isinstance(manifest.get("trace"), dict) else {}
    sidecar = manifest.get("sidecar") if isinstance(manifest.get("sidecar"), dict) else {}
    torch_paths = existing_paths(trace.get("torch_trace_files", []))
    ld_paths = existing_paths(trace.get("ld_preload_trace_files", []))
    python_paths = existing_paths(sidecar.get("python_probe_files", []))

    out_root = Path(out_dir)
    out_root.mkdir(parents=True, exist_ok=True)
    merged_paths: List[str] = []
    reports: List[Dict[str, Any]] = []
    if not torch_paths and (python_paths or ld_paths):
        out_path = out_root / "merged_trace_00.json"
        report_path = out_root / "merge_report_00.json"
        report = merge_sidecar_only(ld_paths, python_paths, str(out_path), str(report_path))
        merged_paths.append(str(out_path))
        reports.append(report)

    for index, torch_path in enumerate(torch_paths):
        pid = pid_from_path(torch_path)
        custom_path = select_by_pid_or_index(ld_paths, pid, index)
        sidecars = select_sidecars(python_paths, pid)
        out_path = out_root / f"merged_trace_{index:02d}.json"
        report_path = out_root / f"merge_report_{index:02d}.json"
        if custom_path:
            report = merge_traces(
                torch_path,
                custom_path,
                str(out_path),
                tolerance_us=tolerance_us,
                search_window=search_window,
                margin_us=margin_us,
                mode=mode,
                sidecar_paths=sidecars,
                report_path=str(report_path),
            )
        else:
            report = copy_profiler_with_sidecars(torch_path, str(out_path), sidecars, str(report_path))
        merged_paths.append(str(out_path))
        reports.append(report)

    summary = {
        "manifest_path": manifest_path,
        "out_dir": str(out_root),
        "torch_trace_files": torch_paths,
        "ld_preload_trace_files": ld_paths,
        "python_probe_files": python_paths,
        "merged_trace_files": merged_paths,
        "reports": reports,
    }
    summary_path = out_root / "merge_manifest_summary.json"
    summary_path.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return summary

def existing_paths(items: Any) -> List[str]:
    """从 manifest 路径条目中筛出真实存在的文件。"""

    paths: List[str] = []
    if not isinstance(items, list):
        return paths
    for item in items:
        if isinstance(item, dict) and item.get("exists", True) and isinstance(item.get("path"), str):
            path = map_repo_path(item["path"])
            if os.path.isfile(path):
                paths.append(path)
        elif isinstance(item, str):
            path = map_repo_path(item)
            if os.path.isfile(path):
                paths.append(path)
    return paths

def map_repo_path(path: str) -> str:
    """把容器内 trace-sim 路径映射回当前仓库路径。"""

    for prefix in ("/workspace/trace-sim", "/opt/trace-sim"):
        if path == prefix:
            return str(Path(__file__).resolve().parents[2])
        if path.startswith(prefix + "/"):
            return str(Path(__file__).resolve().parents[2] / path[len(prefix) + 1:])
    return path

def pid_from_path(path: str) -> Optional[str]:
    """从 trace 文件名中提取 pid，用于多进程 trace 对齐。"""

    match = re.search(r"pid(\d+)", path)
    if match:
        return match.group(1)
    match = re.search(r"_([0-9]+)_20[0-9]{11,}", path)
    if match:
        return match.group(1)
    return None

def select_by_pid_or_index(paths: List[str], pid: Optional[str], index: int) -> Optional[str]:
    """优先按 pid 选择 trace；缺 pid 时回退到输入顺序。"""

    if pid:
        for path in paths:
            if pid_from_path(path) == pid:
                return path
    if index < len(paths):
        return paths[index]
    return None

def select_sidecars(paths: List[str], pid: Optional[str]) -> List[str]:
    """选择同 pid 的 Python sidecar；无法判定 pid 时保守返回全部 sidecar。"""

    if not pid:
        return paths
    selected = [path for path in paths if pid_from_path(path) == pid]
    return selected or paths

def merge_sidecar_only(ld_paths: List[str], sidecar_paths: List[str], out_path: str, report_path: str) -> Dict[str, Any]:
    """没有 torch trace 时生成 state-only merged trace。

    faithful replay / cache patch 仍应使用完整 torch+ld+python trace；这个 fallback
    只服务 HiCache state-only 快速验证，让 C++ state model 可以消费 Python facts。
    """

    events: List[Dict[str, Any]] = []
    standalone_custom = 0
    for path in ld_paths:
        _, custom_events = load_trace(path, auto_repair=True)
        for event in custom_events:
            if should_append_standalone_event(event):
                standalone_custom += 1
                events.append(event)
    sidecar_appended, details = append_sidecar_trace_events(events, sidecar_paths)
    sort_events(events)
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    Path(out_path).write_text(json.dumps({"traceEvents": events}, ensure_ascii=False), encoding="utf-8")
    report = MergeReport(
        profiler_path="",
        custom_path=",".join(ld_paths),
        out_path=out_path,
        mode="sidecar_only",
        tolerance_us=0.0,
        search_window=0,
        margin_us=0.0,
        success=True,
        standalone_custom_appended=standalone_custom,
        sidecar_events_appended=sidecar_appended,
        sidecar_details=details,
        sidecar_paths=sidecar_paths,
    )
    write_report(report, report_path)
    return report.to_dict()

def copy_profiler_with_sidecars(profiler_path: str, out_path: str, sidecar_paths: List[str], report_path: str) -> Dict[str, Any]:
    """缺 LD_PRELOAD trace 时复制 profiler trace 并附加 Python sidecar。"""

    raw_data, profiler_events = load_trace(profiler_path)
    if raw_data is None:
        shutil.copyfile(profiler_path, out_path)
        profiler_events = []
        raw_data = {"traceEvents": profiler_events}
    appended, details = append_sidecar_trace_events(profiler_events, sidecar_paths)
    sort_events(profiler_events)
    if isinstance(raw_data, dict):
        raw_data["traceEvents"] = profiler_events
    else:
        raw_data = profiler_events
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    Path(out_path).write_text(json.dumps(raw_data, ensure_ascii=False), encoding="utf-8")
    report = MergeReport(
        profiler_path=profiler_path,
        custom_path="",
        out_path=out_path,
        mode="copy_with_sidecar",
        tolerance_us=0.0,
        search_window=0,
        margin_us=0.0,
        success=True,
        sidecar_events_appended=appended,
        sidecar_details=details,
        sidecar_paths=sidecar_paths,
    )
    write_report(report, report_path)
    return report.to_dict()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Merge Prefill, Decode, and Custom CPU Traces.")
    parser.add_argument("--manifest", help="Path to profile_manifest.json; when set, merge all trace channels listed in manifest")
    parser.add_argument("--out-dir", help="Output directory for manifest mode")
    parser.add_argument("--profiler", help="Path to profiler trace json")
    parser.add_argument("--custom", help="Path to custom cpu hook trace json")
    parser.add_argument("--out", help="Path to output merged json")
    parser.add_argument("--tolerance", type=float, default=10000.0, help="Max time difference in microseconds")
    parser.add_argument("--window", type=int, default=5, help="Binary search neighbor window size (used in search mode)")
    parser.add_argument("--margin", type=float, default=100.0, help="Margin in microseconds before earliest profiler event")
    parser.add_argument("--mode", type=str, choices=["search", "sequential"], default="search", help="Matching logic mode")
    parser.add_argument("--sidecar", action="append", default=[], help="Additional Chrome trace JSON to append")
    parser.add_argument("--report", help="Optional JSON merge report output")
    
    args = parser.parse_args()

    if args.manifest:
        if not args.out_dir:
            parser.error("--manifest requires --out-dir")
        merge_manifest(args.manifest, args.out_dir, args.tolerance, args.window, args.margin, args.mode)
    else:
        if not args.profiler or not args.custom or not args.out:
            parser.error("single-file mode requires --profiler, --custom and --out")
        merge_traces(args.profiler, args.custom, args.out, args.tolerance, args.window, args.margin, args.mode, args.sidecar, args.report)
