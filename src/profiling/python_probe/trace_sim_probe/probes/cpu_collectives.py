"""Optional CPU collective identities, not additional DAG execution costs.

Record metadata only: no tensor contents, snapshots or extra synchronization.
An async return is submission, and a coalesced call may not issue work at all.
"""

import functools
import inspect
import sys
from contextvars import ContextVar

from trace_sim_probe.patching import PATCH_MARKER
from trace_sim_probe.writer import get_writer

TARGET_MODULES = ("torch.distributed.distributed_c10d", "torch.distributed")
_CALLS = ContextVar("cpu_collective_calls", default=frozenset())
# Both public namespaces and compatibility wrappers share each group's order.
_GROUPS = {}


def _wrap(original, operation, c10d):
    signature = inspect.signature(original)

    @functools.wraps(original)
    def observed(tensor, *args, **kwargs):
        if getattr(getattr(tensor, "device", None), "type", None) != "cpu":
            return original(tensor, *args, **kwargs)
        arguments = signature.bind(tensor, *args, **kwargs)
        arguments.apply_defaults()
        fields = arguments.arguments
        group = fields.get("group")
        if c10d._rank_not_in_group(group) or c10d.get_backend(group) != "gloo":
            return original(tensor, *args, **kwargs)
        group = c10d._get_default_group() if group is None else group
        identity = (group, operation)
        active = _CALLS.get()
        if identity in active:
            return original(tensor, *args, **kwargs)
        sequence = group._get_sequence_number_for_group()
        if group not in _GROUPS:
            _GROUPS[group] = {"group": c10d._get_process_group_name(group),
                              "members": c10d.get_process_group_ranks(group), "rank": c10d.get_rank(),
                              "observation_start_sequence": sequence, "collective_index": 0}
        record = dict(_GROUPS[group], operation=operation, numel=tensor.numel(), dtype=str(tensor.dtype),
                      async_op=bool(fields["async_op"]), status="raised",
                      sequence_before=sequence)
        # This counts observed calls, including failed/coalesced ones. Gloo's
        # native sequence also counts asymmetric P2P work such as monitored_barrier.
        _GROUPS[group]["collective_index"] += 1
        if operation == "broadcast":
            record.update(src=fields.get("src"), group_src=fields.get("group_src"))
        else:
            record["reduce_op"] = str(fields["op"])
        writer = get_writer()
        start = writer.now_us()
        token = _CALLS.set(active | {identity})
        try:
            result = original(tensor, *args, **kwargs)
            record["status"] = "returned"
            return result
        finally:
            _CALLS.reset(token)
            end = writer.now_us()
            record["sequence_after"] = group._get_sequence_number_for_group()
            writer.duration_event("runtime.cpu_collective", start, end, "runtime_diagnostic", record)

    setattr(observed, PATCH_MARKER, True)
    return observed


def install(module):
    """Wrap loaded distributed functions without importing or initializing torch."""
    c10d = sys.modules.get("torch.distributed.distributed_c10d")
    if c10d is None:
        return
    for name in ("broadcast", "all_reduce"):
        original = getattr(module, name, None)
        if original is not None and not getattr(original, PATCH_MARKER, False):
            setattr(module, name, _wrap(original, name, c10d))
