# FW-0005：用直接字段赋值简化 with_fields 表达

- 状态：本地已修复并提交；尚未向主线提交。
- 用户审阅：待审阅。
- 主线状态：未提交 issue/PR，未确认主线合入。

## 发现版本

- 日期：2026-09-09；任务：CMT 表达简化讨论。
- 基线 commit：`0e11f0e93bde4668f8a5b43c48b644e0864fbc7f`，不作为正式发布版本声明。
- 分支：`feat/davincioo-rob`；含未提交的 CMT 实现、FW-0001 和 FW-0002 修复。

## 目标

支持在 rule 内用直接字段赋值替代“构造新记录再赋回”的写法，减少重复的对象名、索引
以及对 `with_fields` 方法的记忆负担。`with_fields` 保留用于需要返回新值的表达式。

以下语义已在本地实现。

## 情况一：局部变量

局部变量只参与本次 rule 的计算。字段赋值产生新的局部值，后续语句使用新值，不产生
持久状态提交。

```python
# 原写法
local = local.with_fields(x=y)

# 简写
local.x = y
```

编译器将两种写法转换为相同的值更新逻辑。连续赋值保留前面的字段修改：

```python
local.x = y
local.valid = True
result = local.x             # 使用 y
```

从持久状态读取的局部记录仍按值处理：`local = entries[i]` 后修改 `local.x`，只更新
局部值；需要回写时显式执行 `entries[i] = local`。

## 情况二：持久状态

持久状态在多次 rule 执行之间保留。字段赋值产生待提交的新值，随整条 rule 原子提交。
同一 rule 后续表达式可以使用拟更新的值；提交前，已提交的存储内容保持不变。

```python
# 原写法：模块持久记录
retained = retained.with_fields(x=y)

# 简写
retained.x = y
```

持久列表元素同样支持字段赋值，例如 CMT 清除历史有效位：

```python
# 原写法
histories[i] = histories[i].with_fields(valid=False)

# 简写
histories[i].valid = False
```

其他字段保持不变，条件分支、背压和原子提交遵循现有 rule 契约，不额外规定一拍延迟。

## 实现范围与验收

- 首期支持 rule 内的 `a.x = y` 和 `entries[i].x = y`，复用现有记录更新、分支合并及状态提交机制。
- 索引在源码赋值位置捕获并只求值一次；保留字段、类型和索引安全检查。
- 分别验证局部值的后续读取、连续字段更新、无隐式回写，以及持久状态的条件提交和原子性。
- 与等价 `with_fields` 写法比较 gfsim 结果；改写 CMT 时同步更新 NDF，并运行回归和回放。
- 更新规范，将字段赋值定义为值更新或状态提案语法；不引入对 Queue 中 token 的原地修改。

## 证据与解决状态

现有用例：[CMT accept](../../designs/davincioo/spe/ooo/cmt.py) 的历史有效位清理。
当前契约：[Agentic Circuit 规范](../acir/spec/agentic-circuit.md#mutable-channel-immutable-token)。

解决方案：前端将字段赋值规范化为记录更新和赋回，复用 `ac.var.with`。列表写入值
按源顺序保存为带目标类型的 SSA，后续索引读取使用已有提案，避免重复展开表达式。
相同索引别名及连续写入合并提交；混合条件写入保留原生 verifier 所需的条件证据。
局部副本不隐式回写，Queue 输入保持不可变；不支持嵌套字段、切片和字段增量赋值。

修复文件：[前端](../../python/agentic-circuit/src/agentic_circuit/_queue_frontend.py)。
验证：AC G0 372 项通过、4 项跳过；独立 gfsim 新旧写法等价及 FW-0002 回归 3 项通过；
CMT 9 项和 ROB 9 项通过；四个 CMT 回放场景的浏览器检查通过。
完整证据见 [20260909-fw-0005](../gates/logs/20260909-fw-0005/summary.md)。
额外 MLIR 检查 24 项通过、1 项诊断不匹配，单独记录为
[FW-0007](FW-0007-output-presence-diagnostic.md)，不宣称完整原生 gate 全绿。

修复版本：`135dd90a078201a29baa807771db95e520a63d96`；已本地提交，尚未向主线提交。
