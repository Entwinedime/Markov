import json
import argparse
import os
import logging
import bisect
from collections import defaultdict
from typing import Tuple, Dict, Any, List

logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")

DIRECT_MERGED_EVENT_NAMES = {"CPUInfer::submit", "CPUInfer::sync"}

def load_trace(file_path: str, auto_repair: bool = False) -> Tuple[Any, List[Dict]]:
    if not file_path or not os.path.exists(file_path):
        logging.error(f"File '{file_path}' does not exist.")
        return None, []

    logging.info(f"Loading '{file_path}' ...")
    with open(file_path, 'r', encoding='utf-8') as file_object:
        content = file_object.read().strip()

    if auto_repair and not content.endswith(']'):
        logging.info(f"Fixing missing closing bracket in '{file_path}'...")
        content = content[:content.rfind('}') + 1] + '\n]'

    try:
        data = json.loads(content)
        events = data.get("traceEvents", []) if isinstance(data, dict) else data
        return data, events
    except json.JSONDecodeError as error:
        logging.error(f"Failed to parse JSON: {error}")
        return None, []

def get_cann_pid(events: List[Dict]) -> int:
    return next((
        event.get("pid", 0) for event in events 
        if event.get("name") == "process_name" and event.get("args", {}).get("name") == "CANN"
    ), 0)

def get_earliest_timestamp(events: List[Dict]) -> float:
    earliest = float('inf')
    for event in events:
        if 'ts' in event:
            earliest = min(earliest, float(event['ts']))
    return earliest

def build_custom_event_map(custom_events: List[Dict]) -> Dict[Tuple[int, str], Tuple[List[float], List[Dict]]]:
    temp_map = defaultdict(list)
    for event in custom_events:
        if event.get("ph") == "X" and "args" in event:
            temp_map[(event.get("tid"), event.get("name"))].append(
                (float(event.get("ts", 0)), event["args"])
            )

    custom_map = {}
    for key, values in temp_map.items():
        values.sort(key=lambda item: item[0])
        # Separating timestamps and args for fast bisect array search
        custom_map[key] = ([item[0] for item in values], [item[1] for item in values])
        
    return custom_map

def inject_custom_args(profiler_event: Dict, custom_args: Dict):
    profiler_args = profiler_event.setdefault("args", {})
    function_args = custom_args.get("Function-Args", {})

    if "stream" in function_args:
        profiler_args["Raw Stream"] = function_args["stream"]
    if "event" in function_args:
        profiler_args["Event Id"] = function_args["event"]

    profiler_args.update(custom_args)

def execute_sequential_match(profiler_events: List[Dict], custom_map: Dict, cann_pid: int, 
                             earliest_profiler_ts: float, margin_us: float, tolerance_us: float) -> Tuple[int, int]:
    need_to_be_matched = 0
    successfully_matched = 0

    logging.info("Using 'sequential' matching mode.")
    
    # * 1. Group active profiler events by their unique identifier (tid, name)
    profiler_groups = defaultdict(list)
    for event in profiler_events:
        if event.get("ph") != "X" or event.get("pid") != cann_pid or not event.get("name"):
            continue
        key = (event.get("tid"), event["name"])
        if key in custom_map:
            profiler_groups[key].append(event)

    # * 2. Iterate each group and try sequential inject
    for key, profiler_event_list in profiler_groups.items():
        profiler_event_list.sort(key=lambda e: float(e.get("ts", 0)))
        
        custom_timestamps, custom_args_list = custom_map[key]
        
        valid_custom_indices = [
            idx for idx, ts in enumerate(custom_timestamps) 
            if ts >= earliest_profiler_ts - margin_us
        ]
        
        need_to_be_matched += len(profiler_event_list)
        
        if len(profiler_event_list) != len(valid_custom_indices):
            logging.warning(f"Key {key}: Count mismatch. Profiler needs {len(profiler_event_list)}, "
                            f"Custom has {len(valid_custom_indices)}. Skipping sequential match.")
            continue
            
        logging.info(f"Key {key}: Exact count match ({len(profiler_event_list)}). Injecting sequentially.")
        
        for profiler_index, profiler_event in enumerate(profiler_event_list):
            custom_index = valid_custom_indices[profiler_index]
            
            profiler_timestamp = float(profiler_event.get("ts", 0))
            custom_timestamp = custom_timestamps[custom_index]
            time_difference = abs(profiler_timestamp - custom_timestamp)
            
            if time_difference > tolerance_us:
                logging.warning(f"Key {key}: Match rejected due to time diff {time_difference} us > tolerance {tolerance_us} us.")
                continue
                
            if profiler_timestamp < custom_timestamp:
                logging.warning(f"Profiler event '{profiler_event['name']}' (ts {profiler_timestamp}) happens before custom event (ts {custom_timestamp}). Potential mismatch.")

            inject_custom_args(profiler_event, custom_args_list[custom_index])
            successfully_matched += 1

    return need_to_be_matched, successfully_matched

def execute_search_match(profiler_events: List[Dict], custom_map: Dict, cann_pid: int, 
                         search_window: int, tolerance_us: float) -> Tuple[int, int]:
    need_to_be_matched = 0
    successfully_matched = 0
    used_custom_indices = {}

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
                logging.warning(f"Custom event {candidate_key} at {candidate['ts']} is already occupied by earlier profiler event at {used_custom_indices[candidate_key]}.\n"
                                f"Profiler event '{event['name']}' at {profiler_ts} trying next best.")
                continue

            if profiler_ts < candidate['ts']:
                logging.warning(f"Profiler event '{event['name']}' (ts {profiler_ts}) matches custom event at {candidate['ts']} which occurs later.")
            
            best_candidate = candidate
            break

        if best_candidate:
            if fallback_triggered:
                logging.warning(f"Profiler event '{event['name']}' at {profiler_ts} used fallback custom event "
                                f"with {best_candidate['diff']} us difference.")

            used_custom_indices[best_candidate['key']] = profiler_ts
            inject_custom_args(event, best_candidate['args'])
            successfully_matched += 1

    return need_to_be_matched, successfully_matched

def append_unmatched_custom_events(profiler_events: List[Dict], custom_events: List[Dict], cutoff_ts: float):
    appended_count = 0
    for event in custom_events:
        if event.get("name", "") in DIRECT_MERGED_EVENT_NAMES:
            if float(event.get("ts", 0)) >= cutoff_ts:
                appended_count += 1
                profiler_events.append(event)
    logging.info(f"Appended {appended_count} standalone custom events to the merged trace.")

def merge_traces(profiler_path: str, custom_path: str, out_path: str, 
                 tolerance_us: float = 10000.0, search_window: int = 5, margin_us: float = 0.0, mode: str = "search"):
    raw_data, profiler_events = load_trace(profiler_path)
    _, custom_events = load_trace(custom_path, auto_repair=True)

    if not profiler_events or not custom_events:
        logging.error("Failed to load events. Aborting merge.")
        return

    cann_pid = get_cann_pid(profiler_events)
    logging.info(f"Identified CANN PID: {cann_pid}")

    custom_map = build_custom_event_map(custom_events)
    logging.info(f"Built custom map with {len(custom_map)} unique keys.")

    earliest_profiler_ts = get_earliest_timestamp(profiler_events)

    if mode == "sequential":
        need_match, matched = execute_sequential_match(
            profiler_events, custom_map, cann_pid, earliest_profiler_ts, margin_us, tolerance_us
        )
    else:
        need_match, matched = execute_search_match(
            profiler_events, custom_map, cann_pid, search_window, tolerance_us
        )

    if need_match != matched:
        logging.warning(f"Matched {matched} out of {need_match} events. Some events missing/not properly merged.")
    else:
        logging.info(f"Successfully matched and injected args into {matched} events.")
        
    cutoff_ts = earliest_profiler_ts - margin_us
    append_unmatched_custom_events(profiler_events, custom_events, cutoff_ts)

    logging.info(f"Saving merged trace to '{out_path}' ...")
    with open(out_path, 'w', encoding='utf-8') as output_file:
        json.dump(raw_data, output_file)
        
    logging.info("Merge complete! Drag it into https://ui.perfetto.dev to view.")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Merge Prefill, Decode, and Custom CPU Traces.")
    parser.add_argument("--profiler", required=True, help="Path to profiler trace json")
    parser.add_argument("--custom", required=True, help="Path to custom cpu hook trace json")
    parser.add_argument("--out", required=True, help="Path to output merged json")
    parser.add_argument("--tolerance", type=float, default=10000.0, help="Max time difference in microseconds")
    parser.add_argument("--window", type=int, default=5, help="Binary search neighbor window size (used in search mode)")
    parser.add_argument("--margin", type=float, default=100.0, help="Margin in microseconds before earliest profiler event")
    parser.add_argument("--mode", type=str, choices=["search", "sequential"], default="search", help="Matching logic mode")
    
    args = parser.parse_args()
    
    merge_traces(args.profiler, args.custom, args.out, args.tolerance, args.window, args.margin, args.mode)
