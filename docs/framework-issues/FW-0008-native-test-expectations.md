# FW-0008：原生单元测试的操作清单和诊断期望未同步

- 状态：fixed locally, uncommitted（本地已修复，未提交）。
- 用户审阅：待审阅。
- 主线状态：未提交 issue/PR。

## 发现版本与复现

- 日期：2026-09-09；任务：FW-0007 扩展验证。
- 基线：`a44611e0ed70765019e914d63d37e91c0600cd25`，分支 `feat/davincioo-rob`。
- 含未提交的 FW-0007 条件类型诊断修复；工具和 ACIROpsTests 从当前 checkout 重建。

```bash
/home/lc/.codex/skills/pyc6/scripts/run.sh cmake --build \
  .pycircuit_out/local-clang22/build --target ACIROpsTests -j 4
/home/lc/.codex/skills/pyc6/scripts/run.sh \
  .pycircuit_out/local-clang22/build/bin/ACIROpsTests
```

## 原因与影响

- 两项 registry 测试仍期望 140 个操作，漏掉已有的 `ac.var.invariant` 和
  `ac.var.invariant.yield`；当前注册 142 个操作，规范已描述这两个操作。
- `TableEntryTypeRejectsNonStructRecordKinds` 仍匹配不含 `nominal enum` 的旧诊断。
  非法类型实际仍被拒绝，新的诊断已包含支持的 nominal enum。

`ac.var.invariant` 两项操作由 `9582e4e4` 引入，nominal enum Table entry 支持由
`924daa28` 引入；这两个提交更新了实现和语义测试，但没有同步这里的两个精确清单和
一个诊断断言。失败路径不经过 FW-0007 新增的 Rule/Firing 条件类型检查。

源码：[OpsTest.cpp](../../tests/cpp/agentic-circuit/Dialect/ACIR/OpsTest.cpp)。
证据：[修复验收](../gates/logs/20260909-fw-0008/summary.md)。

## 解决方案与验证

将两个 invariant 操作加入两处精确清单，将注册总数更新为 142，并将 Table 类型断言
同步为包含 nominal enum 的完整诊断。精确清单与非法类型拒绝断言均保留。

重新构建后，`ACIROpsTests` **1844/1844 通过**；FW-0007 相关 lit **28/28 通过**。
完整 ACIR lit 为 **194 通过、2 unsupported、1 失败**，唯一失败是已记录的 FW-0004
生成程序链接依赖问题，不是本次修复引入的新失败。修复基线为上述 commit 的当前分支
本地补丁，尚无修复 commit 或主线提交。
