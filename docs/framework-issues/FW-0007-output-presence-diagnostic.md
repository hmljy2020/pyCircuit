# FW-0007：非法 output presence 的诊断顺序不匹配

- 状态：原生负例测试失败；非法输入仍被拒绝，诊断顺序待修正。
- 用户审阅：待审阅。
- 主线状态：未提交 issue/PR。

## 发现版本

- 日期：2026-09-09；任务：FW-0005 扩展回归。
- 基线 commit：`0e11f0e93bde4668f8a5b43c48b644e0864fbc7f`。
- 分支：`feat/davincioo-rob`；包含未提交的 FW-0001/FW-0002 和 FW-0005。
- 原生工具来自当前 checkout 的 `.pycircuit_out/local-clang22/build/bin`；
  本次构建报告无需更新。FW-0005 没有修改原生编译器。

## 现象与影响

`ACIR/rule-invalid.mlir` 的 `output-presence-type.mlir` 将 i32 值作为 output presence。
预期诊断为 `condition must be !ac.var<i1>`，实际先报告
`optional output presence requires one input and a true candidate`。
非法 IR 仍被拒绝，但 FileCheck 不通过；这不构成非法 IR 被接受或 CMT 仿真失败的证据。

代码审查显示，FW-0002 将 `presenceImpliesCandidate` 改用带布尔类型约束的共享证明；
非 i1 presence 因而在后续类型诊断前被拒绝。尚未独立构建干净基线。

## 复现与证据

```bash
/home/lc/.codex/skills/pyc6/scripts/run.sh lit -v \
  .pycircuit_out/local-clang22/build/compiler/acir/tests/mlir \
  --filter=ACIR/rule-invalid
```

见 [FW-0005 原生检查日志](../gates/logs/20260909-fw-0005/lit.log)：扩展 rule 检查
24 项通过、1 项失败。源用例：[rule-invalid.mlir](../../tests/mlir/agentic-circuit/ACIR/rule-invalid.mlir)。

## 建议修复与解决状态

在涉及 presence 的父级验证路径中先报告类型错误，再进行谓词蕴含检查；保留原有
负例要求，不放宽蕴含证明。修复时对照 Rule/Firing 的检查顺序并回归相关负例。
本次仅记录，尚无修复 commit 或修复通过声明。
