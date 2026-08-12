# graphflow-python

GraphFlow `GraphEngine` 的 Python 绑定。它将 [`graphflow-cpp`](https://github.com/soundex-dele/graphflow-cpp) 作为 Git submodule 编译进 `_graphflow_native`，支持在同一张图中组合 C++ 节点和 Python 节点。

本仓库只封装 GraphEngine，不包含 Agent Engine。Agent 的 Python 与 Flutter SDK 位于独立的 [`agent-engine`](https://github.com/soundex-dele/agent-engine) 仓库中。

## 核心能力

- 从 Python 字典或 JSON 文件加载、重载并运行 GraphFlow 图。
- 注册 Python 节点工厂，并将 `(context, params)` 传给节点实例。
- 在 C++ 和 Python 节点之间传递公共强类型 `PayloadEvent`。
- 在纯 Python 节点之间直接传递普通 Python 对象，无需为每个事件增加原生绑定。
- 提供生命周期状态、路径数量和 Blackboard 快照。
- 支持通过上下文管理器自动 `release()`。

## 仓库关系

原生依赖固定在 `vendor/graphflow-cpp`：

```text
graphflow-python
└── vendor/graphflow-cpp (Git submodule)
    └── utoolkit         (递归 Git submodule)
```

因此必须递归克隆；已有工作副本也应执行 `git submodule update --init --recursive`。

## 环境要求

- Git
- Python 3.8 或更高版本
- CMake 3.16 或更高版本
- 支持 C++17、且与当前 Python 架构兼容的编译器
- pybind11 2.10 或更高版本
- pytest（仅运行测试时需要）

## 构建与验证

以下命令适用于 PowerShell，请在仓库根目录执行：

```powershell
git clone --recurse-submodules https://github.com/soundex-dele/graphflow-python.git
Set-Location graphflow-python

python -m pip install pybind11 pytest
cmake -S . -B build `
  -Dpybind11_DIR="$(python -m pybind11 --cmakedir)"
cmake --build build --config Release --target _graphflow_native

python -m examples.mixed_graph
python -m pytest graphflow/tests -q
```

CMake 会把原生扩展直接生成到 [`graphflow`](graphflow/) 包目录。示例正常运行时会输出：

```text
Hello, GraphFlow
```

> 当前 [`pyproject.toml`](pyproject.toml) 只声明 Python 包元数据，没有接入 CMake 扩展构建。`pip install .` 不会构建 `_graphflow_native`，不能替代上面的 CMake 步骤。

## C++ / Python 混合图

完整示例见 [`examples/mixed_graph.py`](examples/mixed_graph.py)。下例使用原生 source、Python transform 和原生 sink：

```python
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
```

`register_standard_payload_nodes()` 注册的 `NativePayloadSource` 和 `NativePayloadSink` 主要用于示例和冒烟测试。当前 Python API 没有暴露通用的原生 `NodeCreator` 注册入口；业务原生节点应在 C++ 侧注册，并按需增加绑定。

## Python 节点约定

节点工厂必须可调用，并接收两个参数：

```python
def factory(context, params):
    return MyNode(context, params)
```

节点可按需实现以下生命周期方法：

| 方法 | 默认行为 | 用途 |
| --- | --- | --- |
| `on_init()` | 缺省为成功 | 建立订阅、准备资源；返回 `None` 也按成功处理 |
| `on_start()` | 缺省为成功 | 启动任务或发布首个事件；返回 `None` 也按成功处理 |
| `on_stop()` | 无操作 | 可逆停止 |
| `on_release()` | 无操作 | 最终清理 |
| `on_halt()` | 无操作 | 响应节点软中断 |

没有对应动作的节点无需声明这些方法。例如纯消费节点通常只实现 `on_init()`；只有需要主动启动工作的节点才需要 `on_start()`。

`NodeContext` 提供：

| 成员 | 说明 |
| --- | --- |
| `name` | 当前节点名称 |
| `is_halted` | 节点是否已被软中断 |
| `subscribe(event_type, callback)` | 订阅当前路径边上的 Python 事件 |
| `subscribe_global(event_type, callback)` | 订阅引擎级 Python 事件入口 |
| `publish(event, condition="")` | 向匹配的 Python 事件边发布对象 |
| `subscribe_payload(callback)` | 订阅公共原生 `PayloadEvent` |
| `publish_payload(event, condition="")` | 发布公共原生 `PayloadEvent` |

## 纯 Python 事件

如果一条边的生产者和消费者全部是 Python 节点，可以使用普通 Python 类作为事件，不需要新增 pybind11 绑定：

```python
from dataclasses import dataclass
from graphflow import GraphEngine, PYTHON_EVENT_TYPE


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


config = {
    "nodes": [
        {"id": "producer", "class": "Producer"},
        {"id": "consumer", "class": "Consumer"},
    ],
    "paths": [
        {
            "name": "python-only",
            "path": [
                {"id": "producer", "edge": PYTHON_EVENT_TYPE},
                {"id": "consumer"},
            ],
        }
    ],
}

with GraphEngine() as engine:
    engine.register_python_node("Producer", Producer)
    engine.register_python_node("Consumer", Consumer)
    assert engine.load(config)
    assert engine.init()
    assert engine.start()
```

该通道传递原始 Python 对象，并用 `isinstance` 过滤订阅类型，不进行 JSON 序列化。它只适用于纯 Python 边；原生 C++ 消费者需要公共的具体 C++ 事件类型及对应绑定。

## Python API

[`graphflow/__init__.py`](graphflow/__init__.py) 导出以下符号：

| 符号 | 说明 |
| --- | --- |
| `GraphEngine` | 图加载、节点注册、生命周期与全局事件入口 |
| `LifecycleState` | `CREATED`、`INITIALIZED`、`RUNNING`、`STOPPED`、`RELEASED` |
| `NodeContext` | Python 节点的订阅、发布与运行时上下文 |
| `PayloadEvent` | C++ / Python 混合节点共享的示例强类型事件 |
| `PYTHON_EVENT_TYPE` | 纯 Python 事件边在图配置中的原生载体名称 |

`GraphEngine` 的主要成员：

| 成员 | 说明 |
| --- | --- |
| `register_python_node(name, factory)` | 注册 Python 节点工厂 |
| `register_standard_payload_nodes()` | 注册示例原生 payload source / sink |
| `load(config)` / `load_file(path)` | 首次加载图 |
| `reload(config)` / `reload_file(path)` | 重载图 |
| `init()` / `start()` / `stop()` / `release()` | 管理生命周期 |
| `publish_global(event)` | 向引擎级 Python 事件入口发布对象 |
| `state` | 当前 `LifecycleState` |
| `path_count` | 当前路径数量 |
| `blackboard` | Blackboard 的 Python 字典快照 |

## 构建目标

本仓库定义一个原生扩展目标：

| 目标 | 说明 |
| --- | --- |
| `_graphflow_native` | pybind11 模块，链接 `graphflow_engine` 并输出到 `graphflow/` |

配置本仓库时会关闭 submodule 中的 `GRAPHFLOW_BUILD_EXAMPLES` 和 `GRAPHFLOW_BUILD_TESTS`，避免重复构建 C++ 仓库的示例和测试。本仓库当前没有定义独立的 CMake 安装目标。

## 目录结构

```text
examples/               Python 使用示例
graphflow/              Python 包、类型标记与测试
src/                    pybind11 原生绑定
vendor/graphflow-cpp/   GraphFlow C++ Git submodule
CMakeLists.txt          原生扩展构建入口
pyproject.toml          Python 包元数据
```

## 常见问题与边界

### 导入时报 `Cannot find _graphflow_native`

先确认已递归初始化 submodule，并按“构建与验证”一节构建 `_graphflow_native`。还要确保运行示例的 Python 与 CMake 查找到的 Python ABI 和架构一致。

### 为什么 `pip install .` 后仍无法导入？

当前 setuptools 配置没有调用 CMake。请先显式构建 `_graphflow_native`；在加入正式的 wheel/CMake 构建后端之前，不应把本仓库视为可直接发布的纯 pip 安装包。

### Python 事件能直接传给任意 C++ 节点吗？

不能。`PYTHON_EVENT_TYPE` 专用于 Python 节点之间传递对象。跨 C++ / Python 边需要像 `PayloadEvent` 一样的公共 C++ 事件类型和显式绑定。

### 这里包含 Agent SDK 吗？

不包含。Agent Engine 的 C++ 实现以及 Python、Flutter SDK 均在 [`agent-engine`](https://github.com/soundex-dele/agent-engine)。

## 相关仓库

- [graphflow-cpp](https://github.com/soundex-dele/graphflow-cpp)：C++ Core 与 GraphEngine。
- [agent-engine](https://github.com/soundex-dele/agent-engine)：Agent Engine 及其 C++、Python、Flutter SDK。
- [quant-engine](https://github.com/soundex-dele/quant-engine)：量化工作流引擎。
- [behaviortree](https://github.com/soundex-dele/behaviortree)：GraphFlow 行为树扩展。
