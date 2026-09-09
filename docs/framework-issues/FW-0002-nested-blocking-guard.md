# FW-0002：阻塞条件内的嵌套状态更新受限

- 状态：本地已修复并提交；尚未向主线提交。 适用单输入、无输出的阻塞分支。
- 用户审阅：待审阅。
- 主线状态：未提交 issue/PR，未确认主线合入。

## 发现版本

- 日期：2026-09-09；任务：CMT 单模块开发。
- 基线 commit：`0e11f0e93bde4668f8a5b43c48b644e0864fbc7f`，不作为正式发布版本声明。
- 分支：`feat/davincioo-rob`；CMT 源码与测试为未提交开发内容。
- 工具：当前 checkout 的 `.pycircuit_out/local-clang22/build/bin/`，通过 `$pyc6` 固定环境运行。

## 现象与影响

期望一条 rule 在外层条件不满足时保留输入，满足时按内层分支更新业务或诊断状态。
修复前的前端拒绝这种写法：`ACPY-RULE-011: nested conditional state effects inside a blocking guard require explicit CFG implication proof`。

## 触发形式与证据

```python
if 可以处理:
    if 输入错误:
        更新诊断状态
    else:
        更新业务状态
```

结构示意对应的独立复现：[architecture.py](../../tests/integration/agentic-circuit/e2e/fixtures/blocking_branch/architecture.py)。
修复前的原始错误：[before.log](../gates/logs/20260909-fw-0002/before.log)。

复现与回归命令（仓库根目录）：

```bash
/home/lc/.codex/skills/pyc6/scripts/run.sh python -m pytest tests/integration/agentic-circuit/e2e/test_blocking_branch.py -q
```

## 原因与解决方案

前端禁止外层阻塞条件与内部状态分支组合；Rule/Firing 和 QueueGraph 原先也只接受
“写入条件等于执行条件”或“执行条件恒真”，不能证明 `执行条件且分支条件` 安全。

修复保留外层执行条件，在已有同 owner 合并后给分支写入加上它；共享的保守布尔证明
算法分别检查 live SSA 和最终 QueueGraph，支持标识、常量与合取，未知形式继续拒绝。
输入消费和选中的状态写入仍属于一个原子提交，未选中分支不再用自赋值模拟。

- 框架：[条件证明](../../compiler/acir/include/acir/Analysis/PredicateImplication.h)、
  [前端](../../python/agentic-circuit/src/agentic_circuit/_queue_frontend.py)；Decision 0221。
- 应用：[CMT accept](../../designs/davincioo/spe/ooo/cmt.py) 与 NDF 同步改成嵌套分支。
- 边界：不扩展多输入、selected output、early-return 组合或惰性索引读取。
- 验证：通用 gfsim、Rule/Firing/QueueGraph 反例、CMT 8 项、ROB 9 项及共享契约
  4 项通过；回放四个关键场景离线浏览通过。完整命令和其他检查见
  [修复证据](../gates/logs/20260909-fw-0002/summary.md)。
- 修复版本：同上基线 SHA，2026-09-09 当前 checkout 的未提交补丁；依赖已记录的
  FW-0001 本地 CSE/footprint 修复。实际工具从该 checkout 重建，未复用其他分支产物。
- 修复 commit：`135dd90a078201a29baa807771db95e520a63d96`；本地提交不表示已向主线提交或合入。

## 后续纠正：静态参数误计为 Queue 输入

同一基线 SHA 上，首轮修复末尾新增的单输入检查位于参数绑定之前，将 CMT 的
`core_id`、`pe_id`、`stid`、`launch_generation` 等编译期参数也算成 Queue 输入，
导致原来的展开写法和静态循环写法都报 `require exactly one Queue input`。
该问题与 `for` 的展开能力无关。

先前 CMT 8 项通过的结果早于这条检查的加入，不能作为那个最终源码快照的验收结果。
本次将检查移至常量专门化和状态归属解析之后，只统计实际 Queue 输入；保留真实多输入
的拒绝检查。新增通用静态参数反例/正例，以及 CMT 展开写法和静态循环的 ACIR 等价检查。

纠正随 `135dd90a078201a29baa807771db95e520a63d96` 一并提交；最终回归结果和源码哈希见
[补充验收记录](../gates/logs/20260909-fw-0002-input-check/summary.md)。

补充验收：前端/通用 gfsim 155 项、CMT 原有 8 项、ROB 9 项通过；CMT 静态循环与展开
写法在仅排除源码 JIT 指纹后，ACIR 完全一致（新增 1 项通过）。契约清单 2 项及
修改文件检查通过。该次纠正只修改输入数检查并增加回归覆盖；后续 CMT 已改为静态循环，见
[循环改写验收](../gates/logs/20260909-cmt-history-loop/summary.md)。
