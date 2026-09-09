---
doc_id: DOC-DAV-SPE-OOO-CMT-EMIT-DIAGNOSTIC
status: draft
authority: normative
owner: spe-ooo
---

# CMT `emit_diagnostic`：输出诊断

NDF format 0.2，L2 微架构。条款为本地 draft，尚未完成项目 ID 登记和 L0/L1 对应评审。
[模块契约](../../cmt.md) · [实现](../../cmt.py) · [测试](../../../../tests/spe/ooo/test_cmt.py)

## 规则契约 {#DAV-SPE-OOO-CMT-EMIT-DIAGNOSTIC-0001}
<!-- ndf: kind=requirement modality=must refinement=L2 domain=spe.ooo status=draft -->

涉及状态：diagnostics；错误接收规则另写 diagnostic_overflow、diagnostic_dropped。实现 MUST 遵循以下行为。

三个诊断缓存槽分别属于 ROB 输入、MPQ、BROB。错误接收原子消费错误消息并更新对应槽；槽空时 coalesced_count=1，已有缓存时替换为最新详情并饱和增加计数。覆盖时置对应 sticky overflow，并将 dropped 饱和计数加一，最大 65535。该计数统计被覆盖的详情，不承诺恢复所有原始错误。

本规则选择最低索引的 valid 槽，发布其内容并清除 valid，二者原子提交。诊断输出背压只阻塞本规则，不阻塞正常交接或错误消息消费。诊断之间使用固定优先级，不承诺持续错误洪流下每个槽公平输出；sticky 状态仍在 Table 回放中可见。

标志可同时成立：unsolicited（无事务/未发送/未要求），identity_mismatch，kind_mismatch，invalid_response，durability_missing，duplicate，capacity_error。MPQ 完整 request 内容不一致也置 invalid_response。正常场景不得出现非预期诊断；错误场景检查对应分类与无提前释放。

## 验证 {#DAV-SPE-OOO-CMT-EMIT-DIAGNOSTIC-VER-0001}
<!-- ndf: kind=verification modality=must refinement=L2 domain=spe.ooo status=draft verifies=DAV-SPE-OOO-CMT-EMIT-DIAGNOSTIC-0001 -->

单模块测试 MUST 覆盖 invalid、diagnostics 场景中对应分支，比较独立期望结果、scan/activation 状态与提交，以及录制前后结果；生成离线回放。测试端模拟外部模块，不能据此宣称实际集成已验证。

## Python 实现示例

以下为 [cmt.py](../../cmt.py) 中的完整 `emit_diagnostic` 规则片段，位于 `cmt` 模块内部；
模块状态声明、共享类型及其他规则见实现文件，片段不作为独立模块运行。
该功能逻辑已通过 [CMT 单模块验收](../../../../../../docs/gates/logs/20260909-cmt-history-loop/summary.md)；源码中的 NDF 注释标记对应本文件的 `doc_id`。

```python
# NDF: DOC-DAV-SPE-OOO-CMT-EMIT-DIAGNOSTIC
@ac.rule
def emit_diagnostic():
    nonlocal diagnostics
    found = ac.find(diagnostics, where=lambda row: row.valid)
    if found.valid:
        diagnostics[found.index] = found.value.with_fields(valid=False)
        return found.value
```
