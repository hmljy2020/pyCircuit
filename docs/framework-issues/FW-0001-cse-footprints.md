# FW-0001：重复状态读取优化后 footprint 失效

- 状态：确认的编译器缺陷；fixed locally, uncommitted（本地已修复，未提交）。
- 用户审阅：待审阅。
- 主线状态：未提交 issue/PR，未确认主线合入。

## 发现版本

- 日期：2026-09-09；任务：CMT 单模块开发。
- 基线 commit：`0e11f0e93bde4668f8a5b43c48b644e0864fbc7f`，不作为正式发布版本声明。
- 分支：`feat/davincioo-rob`；CMT 源码与测试为未提交开发内容。
- 工具：当前 checkout 的 `.pycircuit_out/local-clang22/build/bin/`，通过 `$pyc6` 固定环境运行。

## 现象与影响

两次相同的 live Table 读取在后续 CSE 中合并，但之前推导的访问记录仍算两次。
预期合法规则可以编译，实际报 `footprints=4, operations=3`，阻塞 CMT 编译。

## 复现与证据

最小回归：[rule-cse-footprints.mlir](../../tests/mlir/agentic-circuit/Transforms/rule-cse-footprints.mlir)。
按文件中的 RUN 命令执行；未修复版本失败，当前修复版本通过。
证据：[focused lit](../gates/logs/20260909-cmt/lit.log)、[验收报告](../gates/logs/20260909-cmt/summary.md)。

## 解决方案

在 effect/footprint 推导之前先运行 CSE，保留原有严格 verifier。
修改：[LowerRules.cpp](../../compiler/acir/lib/Transforms/LowerRules.cpp)，对应 Decision 0221。
修复后的工具从上述基线加本地修改重新构建；修复 commit 尚无，提交后补记。
验证：focused lit 4 项、CMT 8 项、ROB 9 项通过。
