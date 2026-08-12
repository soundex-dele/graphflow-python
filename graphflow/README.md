# GraphFlow Python runtime

The `_graphflow_native` extension exposes `GraphEngine` and supports native
C++ nodes and Python nodes in the same graph. Cross-language edges remain
strongly typed: both sides of the included example use the public C++
`graphflow::core::PayloadEvent` type from `core/standard_events.h`.

## Build

```powershell
cmake -S . -B build -DGRAPHFLOW_BUILD_PYTHON_SDK=ON
cmake --build build --config Release --target _graphflow_native
```

The extension is written into `sdk/python/graphflow`. Run Python from
`sdk/python`, or install the Python package in editable mode.

## Mixed graph

```python
from graphflow import GraphEngine, PayloadEvent

class Transform:
    def __init__(self, context, params):
        self.context = context

    def on_init(self):
        self.context.subscribe_payload(self.on_payload)
        return True

    def on_payload(self, event):
        self.context.publish_payload(
            PayloadEvent(event.payload.upper(), event.sequence + 1)
        )

engine = GraphEngine()
engine.register_standard_payload_nodes()
engine.register_python_node("Transform", Transform)
```

`register_standard_payload_nodes()` supplies `NativePayloadSource` and
`NativePayloadSink`, which are useful for examples and smoke tests. Production
native nodes continue to be registered through the normal C++ `NodeFactory`.

Python node factories receive `(context, params)`. Optional lifecycle methods
are `on_init`, `on_start`, `on_stop`, `on_release`, and `on_halt`.

## Pure Python events

Python-only edges do not require a new native event binding. Use the single
native `PYTHON_EVENT_TYPE` carrier in the graph and ordinary Python classes in
node code:

```python
from dataclasses import dataclass
from graphflow import PYTHON_EVENT_TYPE

@dataclass
class TextEvent:
    text: str

class Producer:
    def __init__(self, context, params):
        self.context = context

    def on_start(self):
        self.context.publish(TextEvent("hello"))
        return True

class Consumer:
    def __init__(self, context, params):
        self.context = context

    def on_init(self):
        self.context.subscribe(TextEvent, self.on_text)
        return True

    def on_text(self, event):
        print(event.text)
```

Configure the edge as `{"edge": PYTHON_EVENT_TYPE}`. The original Python
object is passed by reference and filtered with `isinstance`; it is not
serialized. Use this channel only when every producer and consumer on the edge
is a Python node. Native consumers still require a concrete C++ event binding.

A complete offline agent made of five pure Python nodes is available at
`sdk/python/examples/pure_python_agent.py`. Run it from `sdk/python`:

```powershell
# Interactive multi-turn conversation
python -m examples.pure_python_agent

# One-shot invocation
python -m examples.pure_python_agent "请计算 12 / 3 + 5"
```

The interactive shell keeps one `GraphEngine` running across all turns. Use
`/history` to display conversation history and `/quit` to stop the graph.
`InputNode` subscribes once with
`context.subscribe_global(UserRequest, callback)`; every command-line turn is
injected through `engine.publish_global(UserRequest(...))` and then forwarded
onto the graph's first edge. No new engine or agent graph is created per turn.

## Adding another concrete event

Concrete event types are compile-time C++ types because `EventBus` uses
templated `publish<T>` and `subscribe<T>`. To expose another type:

1. Define it in a public C++ header using `EventT<MyEvent>`.
2. Add a `py::class_<MyEvent>` binding.
3. Add matching context publish/subscribe adapters that instantiate
   `GraphNode::publish<MyEvent>` and `GraphNode::subscribe<MyEvent>`.

This keeps C++ and Python on the same exact event type and avoids JSON
serialization.
