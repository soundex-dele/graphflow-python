"""Native source -> Python transform -> native sink using PayloadEvent."""

from graphflow import GraphEngine, PayloadEvent


class PrefixNode:
    def __init__(self, context, params):
        self.context = context
        self.prefix = params.get("prefix", "")

    def on_init(self):
        self.context.subscribe_payload(self.on_payload)
        return True

    def on_payload(self, event):
        self.context.publish_payload(
            PayloadEvent(self.prefix + event.payload, event.sequence + 1)
        )


config = {
    "nodes": [
        {
            "id": "source",
            "class": "NativePayloadSource",
            "params": {"payload": "GraphFlow", "sequence": 1},
        },
        {
            "id": "python",
            "class": "PrefixNode",
            "params": {"prefix": "Hello, "},
        },
        {
            "id": "sink",
            "class": "NativePayloadSink",
            "params": {"payload_key": "result"},
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

with GraphEngine() as engine:
    engine.register_standard_payload_nodes()
    engine.register_python_node("PrefixNode", PrefixNode)
    if not engine.load(config) or not engine.init() or not engine.start():
        raise RuntimeError("failed to start mixed graph")
    print(engine.blackboard["result"])
