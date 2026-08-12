# GraphFlow Python 包

本目录是 `graphflow-python` 的 Python 包，导出 GraphFlow `GraphEngine` 的原生绑定。完整的环境要求、构建命令、混合节点示例和 API 说明见[仓库顶层 README](../README.md)。

本包只封装 GraphEngine；Agent Engine 的 Python SDK 位于独立的 `agent-engine` 仓库中。

## 构建

`_graphflow_native` 必须从仓库根目录通过 CMake 构建。以下命令适用于 PowerShell：

```powershell
# 在 graphflow-python 仓库根目录执行
git submodule update --init --recursive
python -m pip install pybind11 pytest

cmake -S . -B build `
  -Dpybind11_DIR="$(python -m pybind11 --cmakedir)"
cmake --build build --config Release --target _graphflow_native

python -m examples.mixed_graph
python -m pytest graphflow/tests -q
```

CMake 会将扩展模块输出到本目录。当前 `pyproject.toml` 尚未接入 CMake 扩展构建，因此 `pip install .` 不会生成 `_graphflow_native`。

## 导出 API

```python
from graphflow import (
    GraphEngine,
    LifecycleState,
    NodeContext,
    PayloadEvent,
    PYTHON_EVENT_TYPE,
)
```

- `GraphEngine`：注册 Python 节点，加载或重载图，并管理生命周期。
- `NodeContext`：在 Python 节点内订阅和发布事件。
- `PayloadEvent`：C++ 与 Python 节点共享的公共示例事件。
- `PYTHON_EVENT_TYPE`：纯 Python 节点之间传递 Python 对象时使用的边类型。

可运行的 C++ / Python 混合图见 [`../examples/mixed_graph.py`](../examples/mixed_graph.py)。
