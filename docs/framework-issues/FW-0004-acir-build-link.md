# FW-0004：acir-build 生成程序链接缺少依赖

- 状态：工具链路失败；根因和修复待进一步确认。
- 用户审阅：待审阅。
- 主线状态：未提交 issue/PR，未确认主线合入。

## 发现版本

- 日期：2026-09-09；任务：CMT 单模块开发。
- 基线 commit：`0e11f0e93bde4668f8a5b43c48b644e0864fbc7f`，不作为正式发布版本声明。
- 分支：`feat/davincioo-rob`；CMT 源码与测试为未提交开发内容。
- 工具：当前 checkout 的 `.pycircuit_out/local-clang22/build/bin/`，通过 `$pyc6` 固定环境运行。

## 现象与影响

`check-acir` 中 `CodeGen/emit-cxx-current.mlir` 在 acir-build 链接阶段失败，
报 `acir::bindings::parseIJson` 及 LLVM JSON/support 符号未定义。
预期生成程序可链接；实际使完整门禁无法全绿。

## 复现与证据

在 `$pyc6` 环境执行：

```bash
lit -v .pycircuit_out/local-clang22/build/compiler/acir/tests/mlir --filter='emit-cxx-current'
```

发现时已应用 FW-0001 的本地修复并重建工具；该用例不经过被修改的 rule-lowering pipeline。
尚未在干净基线独立复测，不能据此断言所有平台或版本都受影响。
[完整原始日志](../../.pycircuit_out/cmt-work/acir-gate.log)；[有界验收摘要](../gates/logs/20260909-cmt/summary.md)。

## 当前处理

CMT 驱动显式链接 ACIRBindings 和 LLVM support 相关库，单模块测试通过。
这不是 acir-build 的修复。完整检查结果为 191 passed、2 unsupported、1 failed；
没有修复 commit，主线提交前应确认链接命令和依赖传播责任。
