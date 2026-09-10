# FW-0006：纯辅助函数

- 分类：已确认的前端表达限制；能力增强。
- 当前状态：结构化函数体与固定多结果已在本地修复并提交。
- 用户审阅：待审阅。
- 主线状态：未提交 issue/PR。

## 版本

- 首次记录基线：`0e11f0e93bde4668f8a5b43c48b644e0864fbc7f`。
- 本次修复基线：`c533f3858ce2c68132e05a8fb66ed24c3dbf9fbc`，最近标签描述
  `agentic-circuit/import-0.3-388-gc533f385`，分支 `feat/davincioo-rob`。
- 工具从同一 checkout 的 `.pycircuit_out/local-clang22/build` 构建；修复 commit：
  `b5cc964a313d63cca66ba2a7fd2f92d3c97ae888`。

## 现状与期望

Decision 0229 的基础 pure helper 只接受一个纯 `return expression` 和一个结果。带局部
计算、`if/else` 或一组相关结果的组合逻辑必须在 rule 中重复展开。期望普通 Python
`def` 能描述这类纯组合计算，并保持零状态、零额外周期和零提交边界。

```python
def next_state(valid: bool, count: ac.u16) -> tuple[ac.u16, bool]:
    overflow = False
    if valid:
        count = count + 1
        overflow = True
    else:
        count = 1
    return count, overflow

count, overflow = next_state(old.valid, old.count)
```

## 实现影响与方案

原实现把 helper 的函数体和返回值硬编码为单表达式、单结果；ACIR inline、QueueGraph
plan、C++ 与 PYC 后端也沿用了单结果模型。

本地修复允许 typed 局部赋值和重绑定、有限嵌套 `if/elif/else`、一个最终 `return`，
以及固定 `tuple[T0, ...]` 的精确直接解构。分支合并为 SSA `ac.var.select`；多结果使用
多个 `func.call` SSA result。普通 helper 在 C++ 中仍只调用一次并用 `std::tuple`
承载结果，PYC 在调用点只展开一次并映射全部 yield。ACIR verifier 和 QueueGraph plan
同时校验结果数量与类型。循环、early return、持久状态及 Queue/Table effect 仍被拒绝。

DavinciOO CMT 已用一个三结果 helper 合并诊断计数、overflow 与 dropped 更新；对应 NDF
片段同步更新。

## 复现与证据

```bash
/home/lc/.codex/skills/pyc6/scripts/run.sh pytest -q \
  tests/python/agentic-circuit/python_frontend/test_helper_functions.py

/home/lc/.codex/skills/pyc6/scripts/run.sh lit -sv \
  .pycircuit_out/local-clang22/build/compiler/acir/tests/mlir \
  --filter=pure-helpers

/home/lc/.codex/skills/pyc6/scripts/run.sh \
  .pycircuit_out/local-clang22/build/bin/CodeGenTests \
  --gtest_filter=QueueGraphPlanTest.*
```

回归覆盖结构化局部变量、字段更新、分支合并、多结果解构、嵌套 helper、非法路径定义、
ACIR 多结果 inline、伪造 QueueGraph 结果元数据、单次 C++ 调用和单次 PYC 展开。完整结果
见 [20260910-structured-helper-functions](../gates/logs/20260910-structured-helper-functions/summary.md)。

基础 single-expression helper 的实现 commit 为
`c533f3858ce2c68132e05a8fb66ed24c3dbf9fbc`；结构化函数体与固定多结果的修复 commit 为
`b5cc964a313d63cca66ba2a7fd2f92d3c97ae888`。
