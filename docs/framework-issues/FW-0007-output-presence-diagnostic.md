# FW-0007：非法 output presence 的诊断顺序不匹配

- 状态：fixed locally, uncommitted（本地已修复，未提交）。
- 用户审阅：待审阅。
- 主线状态：未提交 issue/PR。

## 发现版本

- 日期：2026-09-09；任务：FW-0005 扩展回归。
- 基线 commit：`0e11f0e93bde4668f8a5b43c48b644e0864fbc7f`。
- 分支：`feat/davincioo-rob`；包含未提交的 FW-0001/FW-0002 和 FW-0005。
- 发现问题时的原生工具来自当前 checkout 的 `.pycircuit_out/local-clang22/build/bin`；
  当时的构建报告无需更新。FW-0005 没有修改原生编译器。

## 现象与影响

`ACIR/rule-invalid.mlir` 的 `output-presence-type.mlir` 将 i32 值作为 output presence。
预期诊断为 `condition must be !ac.var<i1>`，实际先报告
`optional output presence requires one input and a true candidate`。
非法 IR 仍被拒绝，但 FileCheck 不通过；这不构成非法 IR 被接受或 CMT 仿真失败的证据。

原因确认：FW-0002 将 `presenceImpliesCandidate` 改用带布尔类型约束的共享证明，
引入于 `135dd90a078201a29baa807771db95e520a63d96`。父级 Rule/Firing 先执行关系检查，
共享证明因非 i1 输入返回 false，导致子级原有的类型诊断被遮蔽。这是该次改动引入的
诊断回归；归因依据为提交差异、校验路径和修复前复现，未独立构建更早的原生基线。

## 复现与证据

```bash
/home/lc/.codex/skills/pyc6/scripts/run.sh lit -v \
  .pycircuit_out/local-clang22/build/compiler/acir/tests/mlir \
  --filter=ACIR/rule-invalid
```

见 [FW-0005 原生检查日志](../gates/logs/20260909-fw-0005/lit.log)：扩展 rule 检查
24 项通过、1 项失败。源用例：[rule-invalid.mlir](../../tests/mlir/agentic-circuit/ACIR/rule-invalid.mlir)。

## 解决方案与验证

在 Rule/Firing 父级验证中，先复用 `verifyI1VarCondition` 检查 candidate、output
presence 和非空 Table proposal presence，再检查条件蕴含。保留共享证明的严格类型
要求和原测试预期，不放宽非法 IR 检查，也不改变合法 rule 的提交行为。

修复后的原生工具及 dialect 单元测试从当前 checkout 重新构建。
修改：[ACIROps.cpp](../../compiler/acir/lib/Dialect/ACIR/ACIROps.cpp)；新增
[六种类型负例](../../tests/mlir/agentic-circuit/ACIR/predicate-type-invalid.mlir)。
原失败测试及相关 Rule/Firing 检查共 28 项通过；其他回归结果见
[修复验收](../gates/logs/20260909-fw-0007/summary.md)。适用 Decision 0221。

修复基线：`a44611e0ed70765019e914d63d37e91c0600cd25`，当前分支本地补丁；
`fixed locally, uncommitted`，尚无修复 commit。主线提交状态单独跟踪。
