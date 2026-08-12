"""Python bindings for the GraphFlow graph runtime."""

from __future__ import annotations

import importlib
import sys
from pathlib import Path


def _load_native():
    try:
        from . import _graphflow_native

        return _graphflow_native
    except ImportError as first_error:
        package_dir = Path(__file__).parent
        for subdirectory in ("Debug", "Release", "."):
            candidate = package_dir / subdirectory
            if not candidate.is_dir():
                continue
            sys.path.insert(0, str(candidate))
            try:
                return importlib.import_module("_graphflow_native")
            except ImportError:
                sys.path.pop(0)
        raise ImportError(
            "Cannot find _graphflow_native. Configure GraphFlow with "
            "-DGRAPHFLOW_BUILD_PYTHON_SDK=ON and build the "
            "_graphflow_native target."
        ) from first_error


_native = _load_native()

GraphEngine = _native.GraphEngine
LifecycleState = _native.LifecycleState
NodeContext = _native.NodeContext
PayloadEvent = _native.PayloadEvent
PYTHON_EVENT_TYPE = _native.PYTHON_EVENT_TYPE

__all__ = [
    "GraphEngine",
    "LifecycleState",
    "NodeContext",
    "PayloadEvent",
    "PYTHON_EVENT_TYPE",
]
