"""Resolve probe source expressions without installing wrappers or emitting events."""

from __future__ import annotations

from typing import Any, Callable


SourceExtractor = Callable[
    [str, str, dict[str, Any], tuple[Any, ...], Any],
    tuple[bool, bool, Any],
]


_SOURCE_EXTRACTOR: SourceExtractor | None = None


def configure_source_extractor(extractor: SourceExtractor) -> None:
    """Bind the selected framework reader at bootstrap, before measured calls."""

    global _SOURCE_EXTRACTOR
    _SOURCE_EXTRACTOR = extractor


def extract_value(
    source: str,
    field_name: str,
    bound: dict[str, Any],
    args: tuple[Any, ...],
    result: Any,
) -> tuple[bool, Any]:
    """Read raw values; missing paths return found=False, never a numeric default.

    len/list expressions compose with ordinary and HiCache sources. List
    summaries retain at most 64 items; token dictionaries use their separate
    complete-token reader. No writer, target catalog or framework is initialized.
    HiCache expressions may add per-event memoized token paths to bound and
    advance observation counters; they do not modify the framework objects.
    """

    kind, separator, spec = source.partition(":")
    if separator and kind in {"len", "list"}:
        found, value = extract_value(spec, field_name, bound, args, result)
        if not found:
            return (False, None)
        return (True, _safe_len(value) if kind == "len" else _safe_list(value))

    if _SOURCE_EXTRACTOR is not None:
        handled, found, value = _SOURCE_EXTRACTOR(source, field_name, bound, args, result)
        if handled:
            return (found, value)

    if not source:
        return (field_name in bound, bound.get(field_name))
    if source.startswith("arg:"):
        return _extract_arg_source(spec, bound, args)
    if source == "self":
        return (bool(args), args[0] if args else None)
    if source.startswith("self."):
        if not args:
            return (False, None)
        return _read_path(args[0], source.split(".", 1)[1])
    if source == "return":
        return (result is not None, result)
    if source.startswith("return."):
        if result is None:
            return (False, None)
        return _read_path(result, source.split(".", 1)[1])
    if source.startswith("const:"):
        return (True, spec)
    return (False, None)


def _extract_arg_source(key: str, bound: dict[str, Any], args: tuple[Any, ...]) -> tuple[bool, Any]:
    """Read arg:name.path or arg:index.path from the already-bound call."""

    head, _, path = key.partition(".")
    if head.isdigit():
        index = int(head)
        if index >= len(args):
            return (False, None)
        value = args[index]
    else:
        if head not in bound:
            return (False, None)
        value = bound[head]
    return _read_path(value, path) if path else (True, value)


def _read_path(obj: Any, path: str) -> tuple[bool, Any]:
    """Read attributes, dictionary keys or list indices without copying objects."""

    cursor = obj
    for part in path.split("."):
        if not part:
            continue
        if cursor is None:
            return (False, None)

        try:
            if isinstance(cursor, dict):
                if part not in cursor:
                    return (False, None)
                cursor = cursor[part]
            elif isinstance(cursor, (list, tuple)) and part.isdigit():
                index = int(part)
                if index >= len(cursor):
                    return (False, None)
                cursor = cursor[index]
            else:
                cursor = getattr(cursor, part)
        except (KeyError, IndexError, TypeError, AttributeError):
            return (False, None)
    return (True, cursor)


def _safe_len(value: Any) -> int | None:
    if value is None:
        return None
    try:
        return len(value)
    except TypeError:
        return None


def _safe_list(value: Any) -> list[Any] | None:
    if value is None:
        return None

    to_list = getattr(value, "tolist", None)
    if callable(to_list):
        value = to_list()
    else:
        value = getattr(value, "token_ids", value)
    try:
        return list(value)[:64]
    except TypeError:
        return None
