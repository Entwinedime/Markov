"""Install probes on loaded framework objects without importing the framework."""

from functools import update_wrapper
from typing import Any, Callable

PATCH_MARKER = "__trace_sim_probe_wrapped__"


def install_wrapper(
    owner: Any,
    name: str,
    factory: Callable[[Callable], Callable],
    marker: str = PATCH_MARKER,
) -> None:
    """Wrap an available method once; retry missing or replaced methods on later imports.

    Distinct observations on one method use distinct markers. Copying the
    original wrapper's attributes preserves those markers in either order.
    """
    if owner is None:
        return

    original = getattr(owner, name, None)
    if original is None or getattr(original, marker, False):
        return

    measured = update_wrapper(factory(original), original)
    setattr(measured, marker, True)
    setattr(owner, name, measured)
