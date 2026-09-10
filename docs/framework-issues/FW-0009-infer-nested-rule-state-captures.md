# FW-0009：自动推导嵌套 rule 的模块状态捕获

- 分类：已确认的前端表达限制；能力增强建议，不是当前语义缺陷。
- 当前状态：已记录，尚未实现。
- 用户审阅：待审阅。
- 主线状态：未提交 issue/PR。

## 发现版本

- 日期：2026-09-10；任务：DavinciOO 前端表达简化审视。
- 最近标签描述：`agentic-circuit/import-0.3-388-gc533f385`。
- 完整 commit：`c533f3858ce2c68132e05a8fb66ed24c3dbf9fbc`。
- 分支：`feat/davincioo-rob`。
- 记录创建前工作区干净；本记录及索引更新是未提交的文档变更。
- 工具从同一 checkout 构建，不涉及不同版本的工具产物。

## 现状与期望

嵌套 `@ac.rule` 只要读取或写入 `@ac.module` 中声明的持久状态，当前前端就要求
rule 直接写出包含全部相关名字的 `nonlocal`。缺少任意名字会报
`ACPY-RULE-015: nested rule module-state reference requires nonlocal declaration`。

前端在报错前已经收集模块 typed state 及 rule 的 `referenced_state`。建议直接从这两份
静态信息推导捕获集合，并按模块声明顺序继续复用现有的隐式 rule 参数和调用改写。
例如：

```python
@ac.module
def counter(command: Command) -> State:
    state: State = 0

    @ac.rule
    def update(command):
        state.value = command.value
        return state

    return update(command)
```

这里的 `state` 应被识别为模块持久状态，不再要求额外写 `nonlocal state`。局部变量、rule
参数和模块状态仍由现有符号集合区分；状态提案、原子提交和后端语义保持不变。

## 实现影响

该限制给每条嵌套 rule 增加一份与函数体重复的状态清单，状态增删时还必须同步维护。
当前 CMT 有 11 条、I2 有 7 条、ROB 有 5 条 `nonlocal` 声明。移除要求预计只涉及
Python source-closure 前端及规范、决策和回归更新，不需要改变 ACIR、QueueGraph、PYC、
gfsim 或 rule 原子提交契约。正式实现前仍需用 decision 明确自动捕获及名字遮蔽规则。

## 最小复现与证据

现有回归明确锁定了当前行为：

```bash
/home/lc/.codex/skills/pyc6/scripts/run.sh python \
  tests/python/agentic-circuit/python_frontend/test_field_assignment.py \
  FieldAssignmentTest.test_missing_nonlocal_remains_an_error
```

源码证据：

- `python/agentic-circuit/src/agentic_circuit/_queue_frontend.py` 中
  `_desugar_nested_rule_captures` 已计算 `state_names` 与 `referenced_state`，随后检查
  `referenced_state - requested` 并产生上述诊断。
- `tests/python/agentic-circuit/python_frontend/test_field_assignment.py` 的
  `test_missing_nonlocal_remains_an_error` 验证缺少声明会失败。
- 实际影响可见于 `designs/davincioo/spe/ooo/cmt.py`、
  `designs/davincioo/spe/ooo/rob.py` 和 `designs/davincioo/spe/iex/i2.py`。

本条仅记录现有行为和建议方向，尚无修复或新 gate 证据。
