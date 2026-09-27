"""Observe the deployed file-prefetch path, separately from throughput sampling."""

from __future__ import annotations

import time
from types import SimpleNamespace
from typing import Any, Callable


def prefetch_experiments(page_sizes: list[int], batch_pages: int) -> list[tuple[int, int, list[int | None]]]:
    """One shared experiment grid for execution and conservative byte reservation."""
    return [
        (size, pages, [None, 0] + ([batch_pages if pages > batch_pages else pages // 2] if pages > 1 else []))
        for size in page_sizes
        for pages in ([1, 8, batch_pages + 1] if size == min(page_sizes) else [1, 8])
    ]


def prefetch_io_bytes(page_sizes: list[int], batch_pages: int, scopes: int, warmup: int, repeats: int) -> int:
    # Seed write + warm read/copy, then a full read and host copy per case as an
    # upper bound. Cancelled cases are charged even when they transfer less.
    return scopes * sum(
        size * pages * (3 + 2 * (warmup + repeats) * len(cases))
        for size, pages, cases in prefetch_experiments(page_sizes, batch_pages)
    )


def capture_prefetch_timings(process_scopes: Any, page_sizes: list[int], *, warmup: int, repeats: int) -> list[dict]:
    """A fixed warm-file grid shared by all targets, including batch cancellation."""
    limits = process_scopes.worker_storage_batch_pages
    if not limits or len(set(limits)) != 1:
        raise ValueError("prefetch timing requires a common framework batch size")
    samples = []
    for page_bytes, pages, cases in prefetch_experiments(page_sizes, limits[0]):
        keys = [[f"prefetch_timing_scope{scope}_page{page}" for page in range(pages)] for scope in range(len(limits))]
        try:
            process_scopes.write(keys, page_bytes)
            # Warm the same framework path used by the samples. Discard this
            # first observation rather than maintaining a separate read loop.
            process_scopes.observe_prefetch(keys, page_bytes, cancel_after_pages=None)

            for ordinal in range(warmup + repeats):
                for cancel in cases:
                    rows = process_scopes.observe_prefetch(keys, page_bytes, cancel_after_pages=cancel)
                    if ordinal < warmup:
                        continue
                    for scope, row in enumerate(rows):
                        samples.append(
                            {
                                **row,
                                "ordinal": ordinal - warmup,
                                "scope": scope,
                                "page_bytes": page_bytes,
                                "storage_batch_pages": limits[scope],
                                "device": process_scopes.worker_devices[scope],
                                "numa_node": process_scopes.worker_numa_nodes[scope],
                                "torch_num_threads": process_scopes.worker_threads[scope],
                                "resource_state": "warm_file_without_inference",
                            }
                        )
        finally:
            process_scopes.clear()
    return samples


def observe_prefetch_transfer(
    controller: Any,
    operation: Any,
    transfer: Callable,
    *,
    cancel_after_pages: int | None = None,
    clock: Callable[[], int] = time.perf_counter_ns,
) -> dict:
    """Run real framework methods with small, local timing adapters.

    Cancellation is an experimental intervention, not scheduler service time.
    Nested batch envelopes are not additive costs. Unattributed elapsed time
    includes Python observation overhead; it is not a fitted worker coefficient.
    No process-global patching or tensor snapshots are used.
    """
    page_count = len(operation.hash_value)
    if controller.has_draft or not page_count or operation.completed_tokens:
        raise ValueError("prefetch timing requires a fresh, non-draft file operation")
    if cancel_after_pages is not None and not 0 <= cancel_after_pages < page_count:
        raise ValueError("cancellation must precede the final page publication")
    events = []

    def measure(stage, function, *args, **metadata):
        begin = clock()
        value = function(*args)
        end = clock()
        if stage == "publish":
            metadata["accepted"] = bool(value)
            metadata["completed_tokens"] = operation.completed_tokens
        events.append({"stage": stage, "start_ns": begin, "end_ns": end, **metadata})
        return value

    pool = controller.mem_pool_host
    observed = SimpleNamespace(
        page_size=controller.page_size,
        has_draft=False,
        mem_pool_host=SimpleNamespace(
            get_dummy_flat_data_page=lambda: measure("allocate", pool.get_dummy_flat_data_page),
            set_from_flat_data_page=lambda index, page: measure("copy", pool.set_from_flat_data_page, index, page),
        ),
        storage_backend=SimpleNamespace(
            batch_get=lambda keys, destinations: measure(
                "read", controller.storage_backend.batch_get, keys, destinations, page_count=len(keys)
            ),
        ),
    )

    class ObservedOperation:
        def __getattr__(self, name):
            return getattr(operation, name)

        def increment(self, tokens):
            accepted = measure("publish", operation.increment, tokens)
            if (
                accepted
                and cancel_after_pages is not None
                and operation.completed_tokens == cancel_after_pages * observed.page_size
            ):
                measure("injected_cancel", operation.mark_terminate)
            return accepted

    # Rebind the actual generic method to the observed resources, not a copied
    # implementation of its loop or cancellation behavior.
    generic_get = controller.page_get_func.__func__
    observed.page_get_func = lambda op, keys, indices, extra: measure(
        "batch", generic_get, observed, op, keys, indices, extra, page_count=len(keys)
    )
    if cancel_after_pages == 0:
        operation.mark_terminate()  # Outside the measured worker invocation.
    start = clock()
    transfer(observed, ObservedOperation())
    finish = clock()
    for event in events:
        event["start_ns"] -= start
        event["end_ns"] -= start
    events.sort(key=lambda event: (event["start_ns"], -event["end_ns"]))
    leaf_ns = sum(event["end_ns"] - event["start_ns"] for event in events if event["stage"] != "batch")
    completed = operation.completed_tokens // controller.page_size
    expected = page_count if cancel_after_pages is None else cancel_after_pages
    if completed != expected:
        raise IOError(f"prefetch timing published {completed} pages; expected {expected}")
    return {
        "duration_ns": finish - start,
        "unattributed_ns": finish - start - leaf_ns,
        "requested_pages": page_count,
        "read_pages": sum(event["page_count"] for event in events if event["stage"] == "read"),
        "copied_pages": sum(event["stage"] == "copy" for event in events),
        "published_pages": completed,
        "cancel_after_pages": cancel_after_pages,
        "events": events,
        "clock": "perf_counter_ns",
        "measurement": "instrumented_framework_file_prefetch; not a throughput sample or scheduler model",
    }
