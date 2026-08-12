# graphflow-python

GraphFlow `GraphEngine` 的 Python SDK。Agent Engine 的 Python SDK 位于独立的
`agent-engine` 仓库中。

原生依赖固定在 `vendor/graphflow-cpp` Git submodule 中。

```powershell
git clone --recurse-submodules https://github.com/soundex-dele/graphflow-python.git
cmake -S . -B build `
  -Dpybind11_DIR="$(python -m pybind11 --cmakedir)"
cmake --build build --config Debug
python -m pytest
```
