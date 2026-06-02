from __future__ import annotations

import json
import math
from collections import OrderedDict, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


class NoRadixOpsError(RuntimeError):
    """Raised when traces do not contain radix_op inputs."""


class RadixInputError(RuntimeError):
    """Raised when hicache_radix inputs are present but incomplete."""

    def __init__(self, readiness: Dict[str, Any]) -> None:
        self.readiness = readiness
        super().__init__("hicache_radix inputs are incomplete")


PAGE_IDENTITY_KIND = "block_tuple"


def _is_model_input(args: Dict[str, Any], kind: str) -> bool:
    return (
        args.get("model_input") is True
        and args.get("domain") == "cache_io"
        and args.get("event_kind") == kind
    )


def _as_int(value: Any, default: int = 0) -> int:
    if value is None or value == "":
        return default
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _as_float(value: Any, default: float = 0.0) -> float:
    if value is None or value == "":
        return default
    try:
        if str(value).lower() == "inf":
            return math.inf
        return float(value)
    except (TypeError, ValueError):
        return default


def _lower(value: Any) -> str:
    return str(value or "").strip().lower()


def _as_text(value: Any) -> str:
    if value is None or value == "":
        return ""
    return str(value)


def _split_hashes(value: Any) -> List[str]:
    if value is None:
        return []
    if isinstance(value, list):
        return [str(item) for item in value if item is not None]
    text = str(value)
    if not text:
        return []
    return [part for part in text.split("|") if part]


def _split_block_pages(value: Any) -> List[Tuple[str, ...]]:
    pages: List[Tuple[str, ...]] = []
    for page in _split_hashes(value):
        blocks = tuple(part for part in page.split(",") if part)
        if blocks:
            pages.append(blocks)
    return pages


def _flatten_block_pages(pages: List[Tuple[str, ...]]) -> Tuple[str, ...]:
    blocks: List[str] = []
    for page in pages:
        blocks.extend(page)
    return tuple(blocks)


def _page_key(page: Tuple[str, ...]) -> str:
    return ",".join(page)


def _page_keys(pages: List[Tuple[str, ...]]) -> List[str]:
    return [_page_key(page) for page in pages if page]


def _read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _write_json(path: Path, data: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _load_events(paths: Iterable[Path]) -> List[Dict[str, Any]]:
    events: List[Dict[str, Any]] = []
    for path in paths:
        data = _read_json(path)
        if isinstance(data, dict):
            candidates = data.get("traceEvents", [])
        elif isinstance(data, list):
            candidates = data
        else:
            candidates = []
        events.extend(event for event in candidates if isinstance(event, dict))
    events.sort(key=lambda item: (_as_int(item.get("ts")), _as_int((item.get("args") or {}).get("op_seq"))))
    return events


def _cache_config(model_config: Dict[str, Any]) -> Dict[str, Any]:
    return model_config.get("cache_io", model_config)


def _find_tier(config: Dict[str, Any], name: str) -> Dict[str, Any]:
    for tier in config.get("tiers", []):
        if tier.get("name") == name:
            return tier
    return {"name": name}


def _tier_capacity(config: Dict[str, Any], name: str) -> Optional[int]:
    tier = _find_tier(config, name)
    value = tier.get("capacity_pages", "infinite")
    if isinstance(value, str) and value.lower() in ("infinite", "inf"):
        return None
    if isinstance(value, str) and value.lower() == "infer":
        return None
    parsed = _as_int(value, 0)
    return parsed if parsed > 0 else None


def _tier_latency_us(config: Dict[str, Any], name: str) -> float:
    return _as_float(_find_tier(config, name).get("latency_us"), 0.0)


def _tier_bandwidth_gbps(config: Dict[str, Any], name: str) -> float:
    tier = _find_tier(config, name)
    value = tier.get("bandwidth_gbps", "inf")
    if isinstance(value, str) and value.lower() in ("infinite", "inf", "infer"):
        return math.inf
    return _as_float(value, math.inf)


def _page_size_from_config(config: Dict[str, Any], trace_page_size: int) -> int:
    policy = _lower(config.get("page_size_policy", "trace"))
    value = config.get("page_size_tokens", "infer")
    configured = _as_int(value, 0)
    if policy == "scenario" and configured > 0:
        return configured
    if configured > 0 and trace_page_size <= 0:
        return configured
    return trace_page_size if trace_page_size > 0 else max(configured, 1)


def _infer_bytes_per_page(config: Dict[str, Any], page_size: int, trace_bytes_per_page: int = 0) -> int:
    value = config.get("bytes_per_page", "infer")
    configured = _as_int(value, 0)
    if configured > 0:
        return configured
    if trace_bytes_per_page > 0:
        trace_page = _as_int(config.get("_trace_page_size"), 0)
        if trace_page > 0:
            per_token = max(1, trace_bytes_per_page // trace_page)
            return per_token * page_size
        return trace_bytes_per_page

    num_layers = _as_int(config.get("num_layers"))
    num_kv_heads = _as_int(config.get("num_kv_heads"))
    head_dim = _as_int(config.get("head_dim"))
    dtype_bytes = _as_int(config.get("dtype_bytes"))
    tp_size = max(1, _as_int(config.get("tp_size"), 1))
    if num_layers and num_kv_heads and head_dim and dtype_bytes:
        return page_size * num_layers * max(1, num_kv_heads // tp_size) * head_dim * 2 * dtype_bytes
    return 0


def _estimate_transfer_us(config: Dict[str, Any], src: str, dst: str, bytes_moved: int) -> int:
    latency = max(_tier_latency_us(config, src), _tier_latency_us(config, dst))
    bandwidth = min(_tier_bandwidth_gbps(config, src), _tier_bandwidth_gbps(config, dst))
    if bytes_moved <= 0:
        return 0
    if math.isinf(bandwidth) or bandwidth <= 0:
        return int(round(latency))
    transfer_us = bytes_moved / (bandwidth * 1_000_000_000.0) * 1_000_000.0
    return int(round(latency + transfer_us))


@dataclass
class RadixOp:
    ts: int
    dur: int
    pid: str
    name: str
    method: str
    cache_id: str
    operation_id: str
    page_size: int
    block_size: int
    blocks: List[str]
    local_blocks: List[str]
    parent_blocks: List[str]
    trace_pages: List[Tuple[str, ...]]
    raw_token_len: int
    num_tokens: int
    node_id: str = ""
    parent_node_id: str = ""
    request_id: str = ""
    hit_count: int = 0
    backuped: bool = False
    evicted: bool = False
    args: Dict[str, Any] = field(default_factory=dict)

    @classmethod
    def from_event(cls, event: Dict[str, Any]) -> Optional["RadixOp"]:
        args = event.get("args") or {}
        if not _is_model_input(args, "radix_op"):
            return None
        blocks = _split_hashes(args.get("full_path_block_keys_hash"))
        local_blocks = _split_hashes(args.get("node_local_block_keys_hash"))
        parent_blocks = _split_hashes(args.get("parent_full_path_block_keys_hash"))
        trace_pages = _split_block_pages(args.get("trace_page_block_keys_hash"))
        page_size = _as_int(args.get("page_size"))
        block_size = _as_int(args.get("block_size_tokens"), 32)
        raw_token_len = _as_int(args.get("raw_token_len"), _as_int(args.get("num_tokens")))
        return cls(
            ts=_as_int(event.get("ts")),
            dur=_as_int(event.get("dur")),
            pid=_as_text(event.get("pid", "unknown")) or "unknown",
            name=str(event.get("name", "HiCache::radix_op")),
            method=str(args.get("method") or args.get("python_method") or ""),
            cache_id=_as_text(args.get("cache_id") if args.get("cache_id") not in (None, "") else args.get("op_id")) or "default",
            operation_id=_as_text(args.get("operation_id")),
            page_size=page_size,
            block_size=block_size,
            blocks=blocks,
            local_blocks=local_blocks,
            parent_blocks=parent_blocks,
            trace_pages=trace_pages,
            raw_token_len=raw_token_len,
            num_tokens=_as_int(args.get("num_tokens"), raw_token_len),
            node_id=_as_text(args.get("node_id")),
            parent_node_id=_as_text(args.get("parent_node_id")),
            request_id=_as_text(args.get("request_id")),
            hit_count=_as_int(args.get("hit_count")),
            backuped=bool(args.get("backuped", False)),
            evicted=bool(args.get("evicted", False)),
            args=args,
        )


@dataclass
class StorageReadFact:
    ts: int
    dur: int
    pid: str
    cache_id: str
    operation_id: str
    method: str
    page_size: int
    block_pages: List[Tuple[str, ...]]
    runtime_page_keys: List[str]
    success_pages: int
    status: str = "completed"
    source: str = "backend"

    @classmethod
    def from_event(cls, event: Dict[str, Any]) -> Optional["StorageReadFact"]:
        args = event.get("args") or {}
        if not _is_model_input(args, "storage_op"):
            return None
        method = str(args.get("python_method") or "")
        name = str(event.get("name") or "")
        backend_methods = {"batch_get", "batch_get_v1", "batch_get_v2"}
        is_backend_read = method in backend_methods
        if not is_backend_read:
            return None
        if args.get("tier_src") != "L3" or args.get("tier_dst") != "L2":
            return None
        if args.get("page_identity_kind") != PAGE_IDENTITY_KIND:
            return None
        success_pages = _as_int(args.get("success_pages"), _as_int(args.get("storage_success_pages"), _as_int(args.get("num_pages"))))
        if success_pages <= 0:
            return None
        block_pages = _split_block_pages(args.get("trace_page_block_keys_hash"))[:success_pages]
        runtime_page_keys = _split_hashes(args.get("page_keys_hash"))[:success_pages]
        if not block_pages or len(runtime_page_keys) != len(block_pages):
            return None
        return cls(
            ts=_as_int(event.get("ts")),
            dur=_as_int(event.get("dur"), 1),
            pid=_as_text(event.get("pid", "unknown")) or "unknown",
            cache_id=_as_text(args.get("cache_id")) or "default",
            operation_id=_as_text(args.get("operation_id")),
            method=method or "storage_read",
            page_size=_as_int(args.get("page_size")),
            block_pages=block_pages,
            runtime_page_keys=runtime_page_keys,
            success_pages=success_pages,
            status=str(args.get("status") or "completed"),
            source="backend",
        )


@dataclass
class CacheOperationFact:
    ts: int
    pid: str
    cache_id: str
    operation_id: str
    operation_kind: str
    stage: str
    method: str
    node_id: str = ""
    page_keys: List[str] = field(default_factory=list)
    blocks: List[str] = field(default_factory=list)
    block_pages: List[Tuple[str, ...]] = field(default_factory=list)

    @classmethod
    def from_event(cls, event: Dict[str, Any]) -> Optional["CacheOperationFact"]:
        args = event.get("args") or {}
        if not _is_model_input(args, "cache_operation"):
            return None
        raw_operation_id = args.get("operation_id")
        operation_id = "" if raw_operation_id in (None, "") else str(raw_operation_id)
        if not operation_id:
            return None
        return cls(
            ts=_as_int(event.get("ts")),
            pid=_as_text(event.get("pid", "unknown")) or "unknown",
            cache_id=_as_text(args.get("cache_id")) or "default",
            operation_id=operation_id,
            operation_kind=str(args.get("operation_kind") or ""),
            stage=str(args.get("stage") or ""),
            method=str(args.get("method") or args.get("python_method") or ""),
            node_id=_as_text(args.get("node_id")),
            page_keys=_split_hashes(args.get("page_keys_hash")),
            blocks=_split_hashes(args.get("full_path_block_keys_hash")),
            block_pages=_split_block_pages(args.get("trace_page_block_keys_hash")),
        )


@dataclass
class GeneratedEvent:
    name: str
    ts: int
    dur: int
    pid: str
    cache_id: str
    src: str
    dst: str
    direction: str
    event_kind: str
    num_tokens: int
    page_size: int
    bytes_per_page: int
    page_keys: List[str]
    status: str = "ok"
    method: str = "radix_sim"
    node_id: str = ""

    def pages(self) -> int:
        if self.page_size <= 0:
            return 0
        return self.num_tokens // self.page_size

    def bytes(self) -> int:
        return self.pages() * self.bytes_per_page

    def to_trace_event(self, default_pid: int, config: Dict[str, Any]) -> Dict[str, Any]:
        estimated = _estimate_transfer_us(config, self.src, self.dst, self.bytes())
        try:
            pid = int(self.pid)
        except (TypeError, ValueError):
            pid = default_pid
        return {
            "name": self.name,
            "cat": "hicache",
            "ph": "X",
            "ts": self.ts,
            "dur": self.dur,
            "pid": pid,
            "tid": pid,
            "args": {
                "framework": "sglang",
                "producer": "radix_sim",
                "domain": "cache_io",
                "event_kind": self.event_kind,
                "cache_id": self.cache_id,
                "tier_src": self.src,
                "tier_dst": self.dst,
                "direction": self.direction,
                "num_tokens": self.num_tokens,
                "num_pages": self.pages(),
                "page_size": self.page_size,
                "page_identity_kind": PAGE_IDENTITY_KIND,
                "bytes_per_page": self.bytes_per_page,
                "bytes": self.bytes(),
                "page_keys_hash": "|".join(self.page_keys),
                "status": self.status,
                "python_class": "HiRadixCacheSim",
                "python_method": self.method,
                "node_id": self.node_id,
                "cache_io.estimated_time": estimated,
            },
        }


@dataclass
class RadixNodeSim:
    key: Tuple[str, ...] = field(default_factory=tuple)
    value_tokens: int = 0
    parent: Optional["RadixNodeSim"] = None
    children: "OrderedDict[Tuple[str, ...], RadixNodeSim]" = field(default_factory=OrderedDict)
    node_id: int = 0
    hit_count: int = 0
    backuped: bool = False
    evicted: bool = False
    dirty: bool = False
    last_access: int = 0
    insert_order: int = 0


class HiRadixCacheSim:
    def __init__(self, config: Dict[str, Any], trace_page_size: int, trace_bytes_per_page: int, storage_reads: Optional[List[StorageReadFact]] = None) -> None:
        self.config = config
        self.page_size = _page_size_from_config(config, trace_page_size)
        self.block_size = _as_int(config.get("block_size_tokens"), 32)
        self.page_blocks = max(1, self.page_size // self.block_size)
        self.bytes_per_page = _infer_bytes_per_page(config, self.page_size, trace_bytes_per_page)
        self.write_policy = _lower(config.get("write_policy", "write_through"))
        if self.write_policy in ("trace", ""):
            self.write_policy = "write_through"
        self.write_through_threshold = 1 if self.write_policy == "write_through" else 2
        self.prefetch_policy = _lower(config.get("prefetch_policy", "trace_replay"))
        self.prefetch_threshold = max(_as_int(config.get("prefetch_threshold")), self.page_size)
        self.root_by_scope: Dict[str, RadixNodeSim] = {}
        self.nodes_by_scope: Dict[str, List[RadixNodeSim]] = {}
        self.next_node_id_by_scope: Dict[str, int] = {}
        self.clock = 0
        self.events: List[GeneratedEvent] = []
        self.tier_pages_by_scope: Dict[str, Dict[str, OrderedDict[str, bool]]] = {}
        self.storage_reads = storage_reads or []
        self.has_observed_storage_reads = bool(self.storage_reads)
        self.evictions_by_tier: Dict[str, int] = defaultdict(int)
        self.warnings: List[str] = []
        self.missing_pages_by_reason: Dict[str, int] = defaultdict(int)
        self.load_back_pages_requested = 0
        self.load_back_pages_l2_hit = 0
        self.load_back_pages_l3_hit = 0
        self.load_back_pages_missing = 0

    def _scope(self, pid: str, cache_id: str = "default") -> str:
        return f"{pid}:{cache_id or 'default'}"

    def _scope_for_op(self, op: RadixOp) -> str:
        return self._scope(op.pid, op.cache_id)

    def _scope_for_fact(self, fact: StorageReadFact) -> str:
        return self._scope(fact.pid, fact.cache_id)

    def _state(self, scope: str) -> Dict[str, OrderedDict[str, bool]]:
        if scope not in self.tier_pages_by_scope:
            self.tier_pages_by_scope[scope] = {
                "L1": OrderedDict(),
                "L2": OrderedDict(),
                "L3": OrderedDict(),
            }
        return self.tier_pages_by_scope[scope]

    def _root(self, scope: str) -> RadixNodeSim:
        if scope not in self.root_by_scope:
            self.root_by_scope[scope] = RadixNodeSim(node_id=0)
            self.nodes_by_scope[scope] = []
            self.next_node_id_by_scope[scope] = 1
        return self.root_by_scope[scope]

    def _nodes(self, scope: str) -> List[RadixNodeSim]:
        self._root(scope)
        return self.nodes_by_scope[scope]

    def _aligned_blocks(self, op: RadixOp) -> Tuple[str, ...]:
        if not op.blocks:
            return tuple()
        aligned_count = len(op.blocks) // self.page_blocks * self.page_blocks
        return tuple(op.blocks[:aligned_count])

    def _tokens_for_blocks(self, blocks: Tuple[str, ...]) -> int:
        return len(blocks) * self.block_size

    def _block_pages_from_blocks(self, blocks: Tuple[str, ...]) -> List[Tuple[str, ...]]:
        pages: List[Tuple[str, ...]] = []
        for start in range(0, len(blocks), self.page_blocks):
            page = blocks[start : start + self.page_blocks]
            if len(page) == self.page_blocks:
                pages.append(page)
        return pages

    def _block_pages_from_trace_pages(self, trace_pages: List[Tuple[str, ...]]) -> List[Tuple[str, ...]]:
        return self._block_pages_from_blocks(_flatten_block_pages(trace_pages))

    def _page_keys(self, blocks: Tuple[str, ...]) -> List[str]:
        return _page_keys(self._block_pages_from_blocks(blocks))

    def _child_key(self, blocks: Tuple[str, ...]) -> Tuple[str, ...]:
        return tuple(blocks[: self.page_blocks])

    def _match_len(self, left: Tuple[str, ...], right: Tuple[str, ...]) -> int:
        limit = min(len(left), len(right))
        matched = 0
        while matched + self.page_blocks <= limit:
            if left[matched : matched + self.page_blocks] != right[matched : matched + self.page_blocks]:
                break
            matched += self.page_blocks
        return matched

    def _new_node(self, key: Tuple[str, ...], parent: RadixNodeSim, scope: str) -> RadixNodeSim:
        next_node_id = self.next_node_id_by_scope.get(scope, 1)
        node = RadixNodeSim(
            key=key,
            value_tokens=self._tokens_for_blocks(key),
            parent=parent,
            node_id=next_node_id,
            insert_order=self.clock,
        )
        self.next_node_id_by_scope[scope] = next_node_id + 1
        self._nodes(scope).append(node)
        return node

    def _split_node(self, child: RadixNodeSim, split_len: int, scope: str) -> RadixNodeSim:
        parent = child.parent
        assert parent is not None
        prefix = child.key[:split_len]
        suffix = child.key[split_len:]
        new_node = self._new_node(prefix, parent, scope)
        new_node.hit_count = child.hit_count
        new_node.backuped = child.backuped
        new_node.evicted = child.evicted
        new_node.dirty = child.dirty
        old_child_key = self._child_key(child.key)
        parent.children.pop(old_child_key, None)
        parent.children[self._child_key(prefix)] = new_node
        child.key = suffix
        child.value_tokens = self._tokens_for_blocks(suffix)
        child.parent = new_node
        new_node.children[self._child_key(suffix)] = child
        return new_node

    def _emit(self, name: str, op: RadixOp, src: str, dst: str, direction: str, blocks: Tuple[str, ...], node: RadixNodeSim) -> None:
        tokens = self._tokens_for_blocks(blocks)
        if tokens <= 0:
            return
        self.events.append(
            GeneratedEvent(
                name=name,
                ts=op.ts,
                dur=max(1, op.dur),
                pid=op.pid,
                cache_id=op.cache_id,
                src=src,
                dst=dst,
                direction=direction,
                event_kind="movement",
                num_tokens=tokens,
                page_size=self.page_size,
                bytes_per_page=self.bytes_per_page,
                page_keys=self._page_keys(blocks),
                method=op.method,
                node_id=str(node.node_id),
            )
        )

    def _emit_pages(
        self,
        name: str,
        ts: int,
        dur: int,
        scope: str,
        src: str,
        dst: str,
        direction: str,
        page_keys: List[str],
        method: str,
        status: str = "ok",
    ) -> None:
        keys = [key for key in page_keys if key]
        if not keys:
            return
        pid, _, cache_id = scope.partition(":")
        self.events.append(
            GeneratedEvent(
                name=name,
                ts=ts,
                dur=max(1, dur),
                pid=pid or "unknown",
                cache_id=cache_id or "default",
                src=src,
                dst=dst,
                direction=direction,
                event_kind="movement",
                num_tokens=len(keys) * self.page_size,
                page_size=self.page_size,
                bytes_per_page=self.bytes_per_page,
                page_keys=keys,
                method=method,
                status=status,
            )
        )

    def _touch_pages(self, scope: str, tier: str, keys: List[str], dirty: bool = False) -> None:
        pages = self._state(scope).setdefault(tier, OrderedDict())
        for key in keys:
            if key in pages:
                existing_dirty = pages.pop(key)
                pages[key] = bool(existing_dirty or dirty)
            else:
                pages[key] = bool(dirty)
        self._enforce_capacity(scope, tier)

    def _enforce_capacity(self, scope: str, tier: str) -> None:
        capacity = _tier_capacity(self.config, tier)
        if capacity is None:
            return
        pages = self._state(scope).setdefault(tier, OrderedDict())
        while len(pages) > capacity:
            key, dirty = pages.popitem(last=False)
            self.evictions_by_tier[tier] += 1
            if tier == "L2" and dirty and self.write_policy == "write_back":
                bytes_per_page = self.bytes_per_page
                pid, _, cache_id = scope.partition(":")
                self.events.append(
                    GeneratedEvent(
                        name="HiCache::write_l2_to_l3",
                        ts=self.clock,
                        dur=1,
                        pid=pid or "unknown",
                        cache_id=cache_id or "default",
                        src="L2",
                        dst="L3",
                        direction="writeback",
                        event_kind="movement",
                        num_tokens=self.page_size,
                        page_size=self.page_size,
                        bytes_per_page=bytes_per_page,
                        page_keys=[key],
                        method="capacity_evict",
                    )
                )
                self._state(scope).setdefault("L3", OrderedDict())[key] = False

    def _write_backup(self, node: RadixNodeSim, op: RadixOp, write_back: bool = False) -> int:
        scope = self._scope_for_op(op)
        root = self._root(scope)
        if not write_back and node.parent is not root and node.parent is not None and not node.parent.backuped:
            return 0
        blocks = node.key
        keys = self._page_keys(blocks)
        if not keys:
            return 0
        self._emit("HiCache::backup_l1_to_l2", op, "L1", "L2", "backup", blocks, node)
        self._touch_pages(scope, "L2", keys, dirty=self.write_policy == "write_back")
        node.backuped = True
        if self.write_policy != "write_back":
            self._emit("HiCache::write_l2_to_l3", op, "L2", "L3", "write", blocks, node)
            self._touch_pages(scope, "L3", keys)
            node.dirty = False
        else:
            node.dirty = True
        return self._tokens_for_blocks(blocks)

    def _inc_hit_count(self, node: RadixNodeSim, op: RadixOp) -> None:
        if self.write_policy == "write_back":
            return
        node.hit_count += 1
        if not node.backuped and node.hit_count >= self.write_through_threshold:
            self._write_backup(node, op)

    def insert(self, op: RadixOp) -> None:
        self.clock += 1
        scope = self._scope_for_op(op)
        key = self._aligned_blocks(op)
        if not key:
            return
        node = self._root(scope)
        child_key = self._child_key(key)
        while key and child_key in node.children:
            node = node.children[child_key]
            node.last_access = self.clock
            prefix_len = self._match_len(node.key, key)
            if prefix_len == len(node.key):
                if node.evicted:
                    node.evicted = False
                    node.value_tokens = self._tokens_for_blocks(node.key)
                    self._touch_pages(scope, "L1", self._page_keys(node.key))
                else:
                    self._inc_hit_count(node, op)
            else:
                new_node = self._split_node(node, prefix_len, scope)
                if new_node.evicted:
                    new_node.evicted = False
                    self._touch_pages(scope, "L1", self._page_keys(new_node.key))
                else:
                    self._inc_hit_count(new_node, op)
                node = new_node
            key = key[prefix_len:]
            if key:
                child_key = self._child_key(key)
        if key:
            new_node = self._new_node(key, node, scope)
            node.children[self._child_key(key)] = new_node
            self._touch_pages(scope, "L1", self._page_keys(key))
            if self.write_policy != "write_back":
                self._inc_hit_count(new_node, op)
            else:
                new_node.dirty = True

    def prefetch_from_storage(self, op: RadixOp) -> None:
        self.clock += 1
        scope = self._scope_for_op(op)
        if self.prefetch_policy == "none":
            return
        if self.has_observed_storage_reads:
            return
        key = self._aligned_blocks(op)
        tokens = self._tokens_for_blocks(key)
        if tokens < self.prefetch_threshold:
            return
        page_keys = self._page_keys(key)
        hits = [key for key in page_keys if key in self._state(scope).get("L3", {})]
        if len(hits) * self.page_size < self.prefetch_threshold:
            return
        blocks = tuple(block for page in hits for block in page.split(","))
        dummy = RadixNodeSim(key=blocks, node_id=0)
        self._emit("HiCache::prefetch_l3_to_l2", op, "L3", "L2", "prefetch", blocks, dummy)
        self._touch_pages(scope, "L2", hits)

    def apply_storage_read(self, fact: StorageReadFact) -> None:
        self.clock += 1
        scope = self._scope_for_fact(fact)
        block_pages = self._block_pages_from_trace_pages(fact.block_pages[: fact.success_pages])
        keys = _page_keys(block_pages)
        self._touch_pages(scope, "L3", keys)
        if self.prefetch_policy == "none":
            return
        self._emit_pages("HiCache::prefetch_l3_to_l2", fact.ts, fact.dur, scope, "L3", "L2", "prefetch", keys, fact.method, fact.status)
        self._touch_pages(scope, "L2", keys)

    def apply_cache_operation(self, fact: CacheOperationFact) -> None:
        if (
            fact.operation_kind != "prefetch"
            or fact.stage != "completed"
            or fact.method != "_insert_helper_host"
            or self.prefetch_policy == "none"
        ):
            return
        block_pages = self._block_pages_from_trace_pages(fact.block_pages)
        keys = _page_keys(block_pages)
        if not keys:
            return
        self.clock += 1
        scope = self._scope(fact.pid, fact.cache_id)
        self._touch_pages(scope, "L2", keys)

    def load_back(self, op: RadixOp) -> None:
        self.clock += 1
        scope = self._scope_for_op(op)
        if op.trace_pages:
            page_keys = _page_keys(self._block_pages_from_trace_pages(op.trace_pages))
        else:
            key = self._aligned_blocks(op)
            page_keys = self._page_keys(key)
        if not page_keys:
            return
        state = self._state(scope)
        self.load_back_pages_requested += len(page_keys)
        ready_keys = [key for key in page_keys if key in state.get("L2", {})]
        missing_keys = [key for key in page_keys if key not in state.get("L2", {})]
        demand_keys = [key for key in missing_keys if key in state.get("L3", {})]
        self.load_back_pages_l2_hit += len(ready_keys)
        self.load_back_pages_l3_hit += len(demand_keys)
        if demand_keys and self.prefetch_policy in {"none", "wait_complete", "best_effort", "timeout"}:
            self._emit_pages("HiCache::prefetch_l3_to_l2", op.ts, op.dur, scope, "L3", "L2", "load", demand_keys, "demand_load")
            self._touch_pages(scope, "L2", demand_keys)
            ready_keys.extend(demand_keys)
        missing_after_demand = [key for key in missing_keys if key not in set(demand_keys)]
        if missing_after_demand:
            self.load_back_pages_missing += len(missing_after_demand)
            self.missing_pages_by_reason["load_back_missing_l2_l3"] += len(missing_after_demand)
            self.warnings.append("radix_sim_l2_l3_miss_on_load_back")
        if ready_keys:
            self._emit_pages("HiCache::load_l2_to_l1", op.ts, op.dur, scope, "L2", "L1", "load", ready_keys, op.method)
            self._touch_pages(scope, "L1", ready_keys)

    def evict(self, op: RadixOp) -> None:
        self.clock += 1
        scope = self._scope_for_op(op)
        # Minimal write-back pressure model: flush dirty L2 pages when an eviction
        # request arrives. Device-node victim selection is intentionally conservative.
        if self.write_policy == "write_back":
            for node in list(self._nodes(scope)):
                if node.dirty and not node.backuped and not node.evicted:
                    self._write_backup(node, op, write_back=True)
                    node.evicted = True
            dirty_keys = [key for key, dirty in self._state(scope).get("L2", {}).items() if dirty]
            pid, _, cache_id = scope.partition(":")
            for key in dirty_keys:
                self.events.append(
                    GeneratedEvent(
                        name="HiCache::write_l2_to_l3",
                        ts=op.ts,
                        dur=max(1, op.dur),
                        pid=pid or "unknown",
                        cache_id=cache_id or "default",
                        src="L2",
                        dst="L3",
                        direction="writeback",
                        event_kind="movement",
                        num_tokens=self.page_size,
                        page_size=self.page_size,
                        bytes_per_page=self.bytes_per_page,
                        page_keys=[key],
                        method="evict",
                    )
                )
                self._state(scope)["L2"][key] = False
                self._state(scope).setdefault("L3", OrderedDict())[key] = False

    def apply(self, op: RadixOp) -> None:
        if op.method == "insert":
            self.insert(op)
        elif op.method == "prefetch_from_storage":
            self.prefetch_from_storage(op)
        elif op.method == "load_back":
            self.load_back(op)
        elif op.method in ("evict", "evict_host"):
            self.evict(op)


def _summarize(events: List[GeneratedEvent], config: Dict[str, Any], warnings: List[str]) -> Dict[str, Any]:
    summary: Dict[str, Any] = {
        "events": len(events),
        "transfer_events": len(events),
        "eviction_events": 0,
        "writeback_events": 0,
        "events_with_tokens": 0,
        "events_with_pages": 0,
        "events_with_page_size": 0,
        "events_with_bytes": 0,
        "missing_bytes_events": 0,
        "estimated_latency_us": 0,
        "foreground_cache_io_us": 0,
        "background_cache_io_us": 0,
        "movement_events_used": len(events),
        "observed_movements_used": 0,
        "inferred_movements_used": 0,
        "model_generated_movements": len(events),
        "control_events_ignored": 0,
        "hit_tokens_by_tier": {},
        "hit_pages_by_tier": {},
        "miss_pages_by_tier": {},
        "bytes_by_edge": defaultdict(int),
        "pages_by_edge": defaultdict(int),
        "resident_pages_by_tier": {},
        "evictions_by_tier": {},
        "writebacks_by_edge": defaultdict(int),
        "radix_op_events_used": 0,
        "whatif_warnings": sorted(set(warnings)),
    }
    for event in events:
        pages = event.pages()
        bytes_moved = event.bytes()
        edge = f"{event.src}->{event.dst}"
        estimated = _estimate_transfer_us(config, event.src, event.dst, bytes_moved)
        summary["events_with_tokens"] += 1 if event.num_tokens > 0 else 0
        summary["events_with_pages"] += 1 if pages > 0 else 0
        summary["events_with_page_size"] += 1 if event.page_size > 0 else 0
        summary["events_with_bytes"] += 1 if bytes_moved > 0 else 0
        summary["estimated_latency_us"] += estimated
        if event.direction == "prefetch":
            summary["background_cache_io_us"] += estimated
        else:
            summary["foreground_cache_io_us"] += estimated
        summary["bytes_by_edge"][edge] += bytes_moved
        summary["pages_by_edge"][edge] += pages
        if event.direction in ("write", "writeback") and event.src == "L2" and event.dst == "L3":
            summary["writeback_events"] += 1
            summary["writebacks_by_edge"][edge] += 1
    for key in ("bytes_by_edge", "pages_by_edge", "writebacks_by_edge"):
        summary[key] = dict(summary[key])
    return summary


def _first_trace_page_size(ops: List[RadixOp]) -> int:
    for op in ops:
        if op.page_size:
            return op.page_size
    return 0


def _first_trace_bytes_per_page(events: List[Dict[str, Any]]) -> int:
    for event in events:
        args = event.get("args") or {}
        value = _as_int(args.get("bytes_per_page"))
        if value > 0:
            return value
    return 0


def _input_report(events: List[Dict[str, Any]], ops: List[RadixOp], storage_reads: List[StorageReadFact], cache_ops: List[CacheOperationFact]) -> Dict[str, Any]:
    model_input_events = 0
    debug_events = 0
    invalid_model_input_events = 0
    rejected_reasons: Dict[str, int] = defaultdict(int)
    kinds = {"radix_op", "storage_op", "cache_operation"}
    model_events: List[Dict[str, Any]] = []
    storage_model_events = 0
    storage_identity_events = 0
    for event in events:
        args = event.get("args") or {}
        if args.get("model_input") is True:
            model_input_events += 1
            if args.get("domain") == "cache_io" and args.get("event_kind") in kinds:
                model_events.append(event)
                if args.get("event_kind") == "storage_op":
                    storage_model_events += 1
                    runtime_pages = _split_hashes(args.get("page_keys_hash"))
                    block_pages = _split_block_pages(args.get("trace_page_block_keys_hash"))
                    if args.get("direction") == "query":
                        expected_pages = _as_int(args.get("queried_pages"))
                    else:
                        expected_pages = _as_int(args.get("success_pages"), _as_int(args.get("num_pages")))
                    if runtime_pages and block_pages and len(runtime_pages) == len(block_pages) and len(block_pages) == expected_pages:
                        storage_identity_events += 1
        else:
            debug_events += 1
        if (
            args.get("domain") == "cache_io"
            and args.get("event_kind") in kinds
            and args.get("model_input") is not True
        ):
            invalid_model_input_events += 1
            rejected_reasons[str(args.get("rejected_reason") or "missing:model_input")] += 1

    path_ops = [op for op in ops if op.method not in {"evict", "evict_host"}]
    radix_full_path_ready = bool(path_ops) and all(op.blocks for op in path_ops)
    page_identity_map_ready = bool(path_ops) and all(op.trace_pages for op in path_ops)

    node_ids = {op.node_id for op in ops if op.node_id}
    parent_prefix_ready = True
    parent_node_exists_ready = True
    for op in path_ops:
        parent_id = op.parent_node_id
        if parent_id and parent_id != "0" and parent_id not in node_ids:
            parent_node_exists_ready = False
            break
        if parent_id and parent_id != "0" and not op.parent_blocks:
            parent_prefix_ready = False
            break
        if op.parent_blocks and tuple(op.blocks[: len(op.parent_blocks)]) != tuple(op.parent_blocks):
            parent_prefix_ready = False
            break
        if op.local_blocks and (op.parent_blocks or parent_id):
            composed = tuple(op.parent_blocks + op.local_blocks)
            if composed != tuple(op.blocks):
                parent_prefix_ready = False
                break
    parent_chain_ready = parent_prefix_ready and parent_node_exists_ready

    cache_operation_ids = {fact.operation_id for fact in cache_ops if fact.operation_id}
    prefetch_operation_ids = {fact.operation_id for fact in cache_ops if fact.operation_id and fact.operation_kind == "prefetch"}
    load_operation_ids = {fact.operation_id for fact in cache_ops if fact.operation_id and fact.operation_kind == "load"}
    storage_read_operation_ids = set()
    storage_write_operation_ids = set()
    for event in events:
        args = event.get("args") or {}
        if not _is_model_input(args, "storage_op"):
            continue
        raw_operation_id = args.get("operation_id")
        if raw_operation_id not in (None, ""):
            operation_id = str(raw_operation_id)
            if args.get("tier_src") == "L2" and args.get("tier_dst") == "L3":
                storage_write_operation_ids.add(operation_id)
            else:
                storage_read_operation_ids.add(operation_id)
    storage_read_operation_ids.discard("")
    storage_write_operation_ids.discard("")
    storage_op_ready = storage_model_events == storage_identity_events
    cache_operation_ready = bool(cache_ops)
    write_operation_ids = {fact.operation_id for fact in cache_ops if fact.operation_id and fact.operation_kind == "write"}
    storage_link_ready = (
        (storage_read_operation_ids.issubset(prefetch_operation_ids) if storage_read_operation_ids else True)
        and (storage_write_operation_ids.issubset(write_operation_ids) if storage_write_operation_ids else True)
    )
    load_back_ops = [op for op in ops if op.method == "load_back"]
    load_back_operation_ids = {op.operation_id for op in load_back_ops if op.operation_id}
    load_back_link_ready = not load_back_ops or all(
        op.operation_id and op.operation_id in load_operation_ids for op in load_back_ops
    )
    operation_lifecycle_ready = storage_link_ready and load_back_link_ready
    operation_link_ready = operation_lifecycle_ready
    runtime_page_alias_ready = storage_model_events == 0 or storage_identity_events == storage_model_events
    state_scope_ready = all(event.get("pid") not in (None, "") for event in model_events)
    radix_sim_ready = (
        radix_full_path_ready
        and page_identity_map_ready
        and parent_chain_ready
        and parent_prefix_ready
        and storage_op_ready
        and operation_lifecycle_ready
        and runtime_page_alias_ready
        and state_scope_ready
        and invalid_model_input_events == 0
    )
    return {
        "model_input_events": model_input_events,
        "debug_events": debug_events,
        "invalid_model_input_events": invalid_model_input_events,
        "rejected_reasons": dict(rejected_reasons),
        "radix_full_path_ready": radix_full_path_ready,
        "page_identity_map_ready": page_identity_map_ready,
        "runtime_page_alias_ready": runtime_page_alias_ready,
        "state_scope_ready": state_scope_ready,
        "parent_chain_ready": parent_chain_ready,
        "parent_prefix_ready": parent_prefix_ready,
        "parent_node_exists_ready": parent_node_exists_ready,
        "operation_link_ready": operation_link_ready,
        "operation_lifecycle_ready": operation_lifecycle_ready,
        "load_back_link_ready": load_back_link_ready,
        "storage_op_ready": storage_op_ready,
        "cache_operation_ready": cache_operation_ready,
        "radix_sim_ready": radix_sim_ready,
        "radix_op_events": len(ops),
        "storage_read_events": len(storage_reads),
        "cache_operation_events": len(cache_ops),
        "storage_read_operation_ids": sorted(storage_read_operation_ids),
        "storage_write_operation_ids": sorted(storage_write_operation_ids),
        "prefetch_operation_ids": sorted(prefetch_operation_ids),
        "write_operation_ids": sorted(write_operation_ids),
        "load_operation_ids": sorted(load_operation_ids),
        "load_back_operation_ids": sorted(load_back_operation_ids),
    }


def run_hicache_radix_sim(
    traces: Iterable[Path],
    model_config: Dict[str, Any],
    *,
    scenario_name: str,
    output_path: Optional[Path] = None,
    summary_path: Optional[Path] = None,
    run_summary_path: Optional[Path] = None,
) -> Dict[str, Any]:
    raw_events = _load_events(traces)
    ops = [op for event in raw_events if (op := RadixOp.from_event(event)) is not None]
    if not ops:
        raise NoRadixOpsError("trace inputs do not contain hicache_radix radix_op model inputs")
    storage_reads = [fact for event in raw_events if (fact := StorageReadFact.from_event(event)) is not None]
    cache_ops = [fact for event in raw_events if (fact := CacheOperationFact.from_event(event)) is not None]
    input_readiness = _input_report(raw_events, ops, storage_reads, cache_ops)
    if (
        not input_readiness.get("radix_full_path_ready")
        or not input_readiness.get("page_identity_map_ready")
        or not input_readiness.get("storage_op_ready")
        or not input_readiness.get("operation_lifecycle_ready")
        or not input_readiness.get("parent_prefix_ready")
        or not input_readiness.get("state_scope_ready")
        or input_readiness.get("invalid_model_input_events", 0) > 0
    ):
        raise RadixInputError(input_readiness)

    config = _cache_config(model_config)
    trace_page_size = _first_trace_page_size(ops)
    config["_trace_page_size"] = trace_page_size
    trace_bytes_per_page = _first_trace_bytes_per_page(raw_events)
    sim = HiRadixCacheSim(config, trace_page_size, trace_bytes_per_page, storage_reads)
    for event in raw_events:
        if op := RadixOp.from_event(event):
            sim.apply(op)
        elif fact := StorageReadFact.from_event(event):
            sim.apply_storage_read(fact)
        elif cache_fact := CacheOperationFact.from_event(event):
            sim.apply_cache_operation(cache_fact)
    warnings = list(sim.warnings)
    if sim.block_size <= 0 or sim.page_size % sim.block_size != 0:
        warnings.append("radix_sim page_size is not divisible by block_size_tokens")
    summary = _summarize(sim.events, config, warnings)
    input_readiness["no_load_back_replay_miss"] = sim.load_back_pages_missing == 0
    input_readiness["radix_sim_ready"] = bool(input_readiness.get("radix_sim_ready")) and sim.load_back_pages_missing == 0
    summary["radix_op_events_used"] = len(ops)
    summary["storage_read_events_used"] = len(storage_reads)
    resident_pages_by_rank = {
        scope: {tier: len(pages) for tier, pages in tiers.items()}
        for scope, tiers in sim.tier_pages_by_scope.items()
    }
    summary["resident_pages_by_rank"] = resident_pages_by_rank
    summary["resident_pages_by_tier"] = {
        tier: sum(len(tiers.get(tier, {})) for tiers in sim.tier_pages_by_scope.values())
        for tier in ("L1", "L2", "L3")
    }
    summary["evictions_by_tier"] = dict(sim.evictions_by_tier)
    summary["load_back_pages_requested"] = sim.load_back_pages_requested
    summary["load_back_pages_l2_hit"] = sim.load_back_pages_l2_hit
    summary["load_back_pages_l3_hit"] = sim.load_back_pages_l3_hit
    summary["load_back_pages_missing"] = sim.load_back_pages_missing
    summary["missing_pages_by_reason"] = dict(sim.missing_pages_by_reason)
    summary["scenario_name"] = scenario_name
    summary["engine"] = "radix_sim"
    summary["input_readiness"] = input_readiness
    summary["model_input_events"] = input_readiness["model_input_events"]
    summary["debug_events"] = input_readiness["debug_events"]
    summary["invalid_model_input_events"] = input_readiness["invalid_model_input_events"]

    trace_events = [event.to_trace_event(200000, config) for event in sim.events]
    if output_path is not None:
        _write_json(output_path, {"traceEvents": trace_events})
    if summary_path is not None:
        _write_json(summary_path, summary)
    run_summary = {
        "scenario_name": scenario_name,
        "engine": "radix_sim",
        "input_traces": [str(path) for path in traces],
        "cache_io_estimated_latency_us": summary["estimated_latency_us"],
        "simulated_e2e_ns": int(summary["estimated_latency_us"]) * 1000,
        "node_count": len(trace_events),
        "edge_count": 0,
        "warnings": summary["whatif_warnings"],
    }
    if run_summary_path is not None:
        _write_json(run_summary_path, run_summary)
    return {
        "cache_io_summary": summary,
        "run_summary": run_summary,
        "trace_events": trace_events,
    }
