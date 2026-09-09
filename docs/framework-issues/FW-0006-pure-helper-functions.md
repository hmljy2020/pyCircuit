# FW-0006：纯辅助函数与显式内联

- 状态：本地已实现并验证。
- 用户审阅：待审阅。
- 主线状态：未提交 issue/PR。

## 发现版本

- 日期：2026-09-09；任务：CMT 前端表达简化讨论。
- 基线 commit：`0e11f0e93bde4668f8a5b43c48b644e0864fbc7f`；不作为正式发布版本声明。
- 分支：`feat/davincioo-rob`；含未提交的 CMT 实现及 FW-0001、FW-0002 修复。

## 当前限制与影响

rule 内尚不支持一般的普通 Python 辅助函数调用。现有 `ac.invariant` 限于单个
nominal struct 参数、布尔返回值和单个纯返回表达式，不能覆盖通用的多参数比较、
计数计算和记录构造。CMT 因此重复展开身份比较、饱和计数等逻辑。

依据：[前端规范](../acir/spec/agentic-circuit.zh-CN.md)、
[CMT 实现](../../designs/davincioo/spe/ooo/cmt.py)。本条记录的是能力增强建议，
不是已确认的语义缺陷。

## 建议行为

普通 `def` 定义纯计算辅助函数；`@ac.inline` 显式要求编译器展开：

| 源码形式 | 编译行为 |
| --- | --- |
| 普通 `def` | 中间表示保留函数及调用，C++ 后端生成辅助函数和调用 |
| `@ac.inline` 的 `def` | 由框架编译器在调用位置展开函数体 |

两种形式具有相同的值语义，不新增持久状态、时钟周期或 rule 提交边界。
普通 C++ 辅助函数仍可能被下游优化器自动内联，不要求最终机器码保留调用；
`@ac.inline` 也不是仅在生成 C++ 时添加 `inline` 关键字。

首期边界：调用目标静态确定，参数和返回值使用已有类型；函数体只包含一个纯返回
表达式，可使用条件表达式并嵌套调用 helper。运行时数据通过参数传入；禁止读取或修改外部
持久状态、操作 Queue、递归、动态调用及外部副作用。编译器检查这些约束并明确报错。

## 最小表达示例

以下语法已由前端、ACIR、QueueGraph C++ 和 PYC 测试覆盖：

```python
def saturating_increment(value: ac.u16) -> ac.u16:
    return value + 1 if value != 65535 else value

@ac.inline
def saturating_increment_inline(value: ac.u16) -> ac.u16:
    return value + 1 if value != 65535 else value
```

rule 分别调用两个函数，应得到相同计算结果；前者生成 C++ 辅助函数调用，后者在
框架编译阶段展开。类型推导或特化的详细规则在实现设计中补齐。

## 实现与验收要求

- 扩展前端函数解析和类型检查；在中间表示及 verifier 中表达纯函数、调用与约束，
  再实现内联和后端生成，不能只在 C++ 后端改变语义。
- 普通函数允许其他后端按目标需要展开，不把软件调用解释成额外硬件周期。
- 补充普通调用、显式内联的生成代码检查，以及边界值、嵌套调用、条件计算的
  gfsim 等价测试；验证非法副作用、递归和动态调用被拒绝。
- 用 CMT 重复计算验证实际简化效果，同步对应 NDF 示例并回归功能与回放。
- 实现前对照 Decision 0221 确定决策更新及 gate 范围；CMT 用例遵循 Decision 0222。
  同步规范，使用 `$pyc6` 固定环境，并将验证证据归档至 `docs/gates/logs/<run-id>/`。

## 证据与解决状态

解决方案已按 Decision 0229 落地：前端生成 `func.func`/`func.call`，ACIR verifier
检查纯度、类型和无环调用图，`@ac.inline` 在框架 pass 中强制展开；普通 helper
由 QueueGraph C++ 保留为 typed 调用，PYC 在生成时展开。CMT 已使用 helper 提取
身份比较和饱和计数逻辑，并同步规则 NDF 示例。

验证证据见
[20260909-pure-helper-functions](../gates/logs/20260909-pure-helper-functions/summary.md)。
修复包含在本记录所在提交中；主线 issue/PR 状态仍为未提交。
