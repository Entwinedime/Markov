"""Optional response observations, not execution costs or client completion.

Record terminal request IDs only. Never inspect serialized bodies, tokens, or
cache state. ASGI send completion is distinct from the bench client's receipt.
"""

import functools

from trace_sim_probe.patching import PATCH_MARKER
from trace_sim_probe.writer import get_writer


def _terminal_ids(output):
    return [rid for rid, reason in zip(getattr(output, "rids", ()) or (), getattr(output, "finished_reasons", ()) or ())
            if reason is not None]


def _record(stage, request_ids, start, end):
    get_writer().duration_event("runtime.response." + stage, start, end, "runtime_diagnostic", {"request_ids": request_ids})


def _sender(original):
    def send(instance, output, *args, **kwargs):
        ids = _terminal_ids(output) if instance.socket is not None else []
        if not ids:
            return original(instance, output, *args, **kwargs)
        start = get_writer().now_us()
        result = original(instance, output, *args, **kwargs)
        _record("scheduler_send", ids, start, get_writer().now_us())
        return result
    return send


def _tokenizer(original):
    async def dispatch(instance, output, *args, **kwargs):
        ids = _terminal_ids(output)
        if not ids:
            return await original(instance, output, *args, **kwargs)
        start = get_writer().now_us()
        result = await original(instance, output, *args, **kwargs)
        _record("tokenizer_dispatch", ids, start, get_writer().now_us())
        return result
    return dispatch


def _response_init(original):
    def initialize(instance, content=None, *args, **kwargs):
        rows = content if isinstance(content, list) else [content]
        ids = []
        for row in rows:
            meta = row.get("meta_info", {}) if isinstance(row, dict) else {}
            if isinstance(meta, dict) and meta.get("id") is not None and meta.get("finish_reason") is not None:
                ids.append(meta["id"])
        start = get_writer().now_us() if ids else 0
        original(instance, content, *args, **kwargs)
        instance._trace_sim_response_ids = ids
        if ids:
            _record("serialize", ids, start, get_writer().now_us())
    return initialize


def _response_send(original):
    async def respond(instance, scope, receive, send):
        ids = getattr(instance, "_trace_sim_response_ids", ())
        if not ids:
            return await original(instance, scope, receive, send)

        async def observed_send(message):
            result = await send(message)
            if message["type"] == "http.response.body" and not message.get("more_body", False):
                now = get_writer().now_us()
                _record("http_body_sent", ids, now, now)
            return result

        return await original(instance, scope, receive, observed_send)
    return respond


_TARGETS = {
    "sglang.srt.managers.scheduler_components.output_sender": (("SenderWrapper", "send_output", _sender),),
    "sglang.srt.managers.tokenizer_manager": (("TokenizerManager", "_handle_batch_output", _tokenizer),),
    "sglang.srt.utils.json_response": (
        ("SGLangORJSONResponse", "__init__", _response_init),
        ("SGLangORJSONResponse", "__call__", _response_send),
    ),
}
TARGET_MODULES = tuple(_TARGETS)


def install(module):
    """Wrap loaded classes, including inherited response methods, exactly once."""
    for class_name, method, wrapper in _TARGETS[module.__name__]:
        owner = getattr(module, class_name, None)
        if owner is None:
            continue
        original = getattr(owner, method, None)
        if original is None or getattr(original, PATCH_MARKER, False):
            continue
        measured = functools.wraps(original)(wrapper(original))
        setattr(measured, PATCH_MARKER, True)
        setattr(owner, method, measured)
