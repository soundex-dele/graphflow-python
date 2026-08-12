from __future__ import annotations

from dataclasses import dataclass

from graphflow import (
    GraphEngine,
    LifecycleState,
    PayloadEvent,
    PYTHON_EVENT_TYPE,
)


class UppercaseNode:
    def __init__(self, context, params):
        self.context = context
        self.suffix = params.get("suffix", "")

    def on_init(self):
        self.context.subscribe_payload(self.on_payload)
        return True

    def on_payload(self, event: PayloadEvent):
        self.context.publish_payload(
            PayloadEvent(event.payload.upper() + self.suffix, event.sequence + 1)
        )


def test_cpp_and_python_nodes_share_a_concrete_event_type():
    engine = GraphEngine()
    engine.register_standard_payload_nodes()
    engine.register_python_node("UppercaseNode", UppercaseNode)

    config = {
        "nodes": [
            {
                "id": "source",
                "class": "NativePayloadSource",
                "params": {"payload": "hello", "sequence": 41},
            },
            {
                "id": "python",
                "class": "UppercaseNode",
                "params": {"suffix": "!"},
            },
            {
                "id": "sink",
                "class": "NativePayloadSink",
                "params": {
                    "payload_key": "result",
                    "sequence_key": "result_sequence",
                },
            },
        ],
        "paths": [
            {
                "name": "mixed",
                "path": [
                    {"id": "source", "edge": "PayloadEvent"},
                    {"id": "python", "edge": "PayloadEvent"},
                    {"id": "sink"},
                ],
            }
        ],
    }

    assert engine.load(config)
    assert engine.path_count == 1
    assert engine.init()
    assert engine.start()
    assert engine.state == LifecycleState.RUNNING
    assert engine.blackboard == {"result": "HELLO!", "result_sequence": 42}

    engine.stop()
    engine.release()
    assert engine.state == LifecycleState.RELEASED


def test_python_factory_must_be_callable():
    engine = GraphEngine()
    try:
        engine.register_python_node("Broken", object())
    except TypeError:
        pass
    else:
        raise AssertionError("non-callable factory was accepted")


@dataclass
class TextEvent:
    text: str


@dataclass
class ResultEvent:
    text: str


class PythonSource:
    published = None

    def __init__(self, context, params):
        self.context = context

    def on_init(self):
        self.context.subscribe_global(TextEvent, self.on_global_event)
        return True

    def on_global_event(self, event):
        assert event is PythonSource.published
        self.context.publish(event)


class PythonTransform:
    def __init__(self, context, params):
        self.context = context

    def on_init(self):
        self.context.subscribe(TextEvent, self.on_text)
        return True

    def on_text(self, event):
        assert event is PythonSource.published
        self.context.publish(ResultEvent(event.text.upper()))


class PythonSink:
    received = []

    def __init__(self, context, params):
        self.context = context

    def on_init(self):
        self.context.subscribe(ResultEvent, self.received.append)
        return True


def test_pure_python_event_types_need_no_native_binding():
    PythonSource.published = None
    PythonSink.received = []

    engine = GraphEngine()
    engine.register_python_node("PythonSource", PythonSource)
    engine.register_python_node("PythonTransform", PythonTransform)
    engine.register_python_node("PythonSink", PythonSink)
    config = {
        "nodes": [
            {"id": "source", "class": "PythonSource"},
            {"id": "transform", "class": "PythonTransform"},
            {"id": "sink", "class": "PythonSink"},
        ],
        "paths": [
            {
                "name": "python-only",
                "path": [
                    {"id": "source", "edge": PYTHON_EVENT_TYPE},
                    {"id": "transform", "edge": PYTHON_EVENT_TYPE},
                    {"id": "sink"},
                ],
            }
        ],
    }

    assert engine.load(config)
    assert engine.init()
    assert engine.start()
    PythonSource.published = TextEvent("hello")
    engine.publish_global(PythonSource.published)
    assert PythonSink.received == [ResultEvent("HELLO")]
    engine.release()
