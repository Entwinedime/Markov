"""Names from torch_npu QueueParamType and EventParas::EVENT_PARAS_MAP."""

# Scalar queue events carry no operation string. Use the same names as the
# native profiler so their normalized measurements match source CPU work.
# ASYNC_MEMCPY is intentionally absent: its name depends on copy direction,
# which is not present in the current queue measurement record.
EVENT_NAMES = {
    3: "record_event",
    4: "wait_event",
    5: "destroy_event",
    9: "record_event",
    10: "wait_event",
}
