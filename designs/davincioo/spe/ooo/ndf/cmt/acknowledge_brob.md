---
doc_id: DOC-DAV-SPE-OOO-CMT-ACKNOWLEDGE-BROB
status: draft
authority: normative
owner: spe-ooo
---

# CMT `acknowledge_brob`：接收 BROB 确认

NDF format 0.2，L2 微架构。条款为本地 draft，尚未完成项目 ID 登记和 L0/L1 对应评审。
[模块契约](../../cmt.md) · [实现](../../cmt.py) · [测试](../../../../tests/spe/ooo/test_cmt.py)

## 规则契约 {#DAV-SPE-OOO-CMT-ACKNOWLEDGE-BROB-0001}
<!-- ndf: kind=requirement modality=must refinement=L2 domain=spe.ooo status=draft -->

涉及状态：busy、retained、brob_sent、brob_done；诊断状态。实现 MUST 遵循以下行为。

确认必须对应 busy 事务且 BROB 已发送、被要求，四个完整身份匹配，valid、durable 均真，brob_done 为假。消费输入并置 brob_done 原子提交。

重复、无事务、尚未发送、错误身份和不可靠确认均消费并诊断，不改变交接进度。正常协议不引入重发；错误注入用于证明不会提前释放。

## 验证 {#DAV-SPE-OOO-CMT-ACKNOWLEDGE-BROB-VER-0001}
<!-- ndf: kind=verification modality=must refinement=L2 domain=spe.ooo status=draft verifies=DAV-SPE-OOO-CMT-ACKNOWLEDGE-BROB-0001 -->

单模块测试 MUST 覆盖 normal、invalid、diagnostics、recovery 场景中对应分支，比较独立期望结果、scan/activation 状态与提交，以及录制前后结果；生成离线回放。测试端模拟外部模块，不能据此宣称实际集成已验证。

## Python 实现示例

以下为 [cmt.py](../../cmt.py) 中的完整 `acknowledge_brob` 规则片段，位于 `cmt` 模块内部；
模块状态声明、共享类型及其他规则见实现文件，片段不作为独立模块运行。
该功能逻辑已通过 [CMT 单模块验收](../../../../../../docs/gates/logs/20260909-cmt-history-loop/summary.md)；源码中的 NDF 注释标记对应本文件的 `doc_id`。

```python
# NDF: DOC-DAV-SPE-OOO-CMT-ACKNOWLEDGE-BROB
@ac.rule
def acknowledge_brob(response):
    nonlocal busy, retained, brob_sent, brob_done
    nonlocal diagnostics, diagnostic_overflow, diagnostic_dropped
    unsolicited = (
        not busy
        or not brob_sent
        or (retained.handoff_required_mask & HANDOFF_OWNER_BROB) == 0
    )
    identity_mismatch = (
        response.epoch != retained.epoch
        or response.inst != retained.inst
        or response.block != retained.block
        or response.rob != retained.rob
    )
    duplicate = busy and not identity_mismatch and brob_done
    invalid_response = not response.valid
    durability_missing = not response.durable
    if (
        unsolicited
        or identity_mismatch
        or duplicate
        or invalid_response
        or durability_missing
    ):
        old = diagnostics[2]
        amount = (
            old.coalesced_count + 1
            if old.valid and old.coalesced_count != 65535
            else 1 if not old.valid else 65535
        )
        diagnostic_overflow[2] = diagnostic_overflow[2] or old.valid
        diagnostic_dropped[2] = (
            diagnostic_dropped[2] + 1
            if old.valid and diagnostic_dropped[2] != 65535
            else diagnostic_dropped[2]
        )
        diagnostics[2] = HandoffDiagnostic(
            owner_mask=HANDOFF_OWNER_BROB,
            epoch=response.epoch,
            inst=response.inst,
            block=response.block,
            rob=response.rob,
            history_sequence=0,
            coalesced_count=amount,
            valid=True,
            duplicate=duplicate,
            unsolicited=unsolicited,
            identity_mismatch=identity_mismatch,
            kind_mismatch=False,
            invalid_response=invalid_response,
            durability_missing=durability_missing,
            capacity_error=False,
        )
    else:
        brob_done = True
```
