---
doc_id: DOC-DAV-SPE-OOO-CMT-ACKNOWLEDGE-MPQ
status: draft
authority: normative
owner: spe-ooo
---

# CMT `acknowledge_mpq`：接收 MPQ 历史确认

NDF format 0.2，L2 微架构。条款为本地 draft，尚未完成项目 ID 登记和 L0/L1 对应评审。
[模块契约](../../cmt.md) · [实现](../../cmt.py) · [测试](../../../../tests/spe/ooo/test_cmt.py)

## 规则契约 {#DAV-SPE-OOO-CMT-ACKNOWLEDGE-MPQ-0001}
<!-- ndf: kind=requirement modality=must refinement=L2 domain=spe.ooo status=draft -->

涉及状态：busy、retained、mpq_sent、histories_acked、histories；诊断状态。实现 MUST 遵循以下行为。

确认必须对应 busy 事务且 MPQ 已发送、被要求。response.valid、accepted、request.valid、history.valid、history.durable 均真，stale 为假。request 必须与 retained 完全相同；history 的四个完整身份必须匹配。不能只比较槽位或指令序号。

按完整事务身份和 history_sequence 查找已确认历史。重复编号不增加计数，不覆写已接受历史。未重复且 histories_acked 小于所需数量时，在 histories[histories_acked] 保存历史并增加计数，与输入消费原子提交。最多计入 15 条。CMT 不核对输入中没有提供的预期历史 ID 集合；MPQ 对集合正确性负责。

历史表有 16 槽，接收新事务时原子清除所有行的 valid，再从槽零覆盖；查重仍必须匹配完整事务身份。环境必须隔离复位前响应。非法、提前、额外、重复或不可靠确认被消费并诊断，不推进有效计数。

## 验证 {#DAV-SPE-OOO-CMT-ACKNOWLEDGE-MPQ-VER-0001}
<!-- ndf: kind=verification modality=must refinement=L2 domain=spe.ooo status=draft verifies=DAV-SPE-OOO-CMT-ACKNOWLEDGE-MPQ-0001 -->

单模块测试 MUST 覆盖 normal、invalid、capacity、diagnostics、recovery 场景中对应分支，比较独立期望结果、scan/activation 状态与提交，以及录制前后结果；生成离线回放。测试端模拟外部模块，不能据此宣称实际集成已验证。

## Python 实现示例

以下为 [cmt.py](../../cmt.py) 中的完整 `acknowledge_mpq` 规则片段，位于 `cmt` 模块内部；
模块状态声明、共享类型及其他规则见实现文件，片段不作为独立模块运行。
该功能逻辑已通过 [CMT 单模块验收](../../../../../../docs/gates/logs/20260909-cmt-history-loop/summary.md)；源码中的 NDF 注释标记对应本文件的 `doc_id`。

```python
# NDF: DOC-DAV-SPE-OOO-CMT-ACKNOWLEDGE-MPQ
@ac.rule
def acknowledge_mpq(response):
    nonlocal busy, retained, mpq_sent, histories_acked, histories
    nonlocal diagnostics, diagnostic_overflow, diagnostic_dropped
    request = response.request
    history = response.history
    seen = ac.find(
        histories,
        where=lambda row: (
            row.valid
            and row.epoch == retained.epoch
            and row.inst == retained.inst
            and row.block == retained.block
            and row.rob == retained.rob
            and row.history_sequence == history.history_sequence
        ),
    )
    unsolicited = (
        not busy
        or not mpq_sent
        or (retained.handoff_required_mask & HANDOFF_OWNER_MPQ) == 0
    )
    identity_mismatch = (
        request.epoch != retained.epoch
        or request.inst != retained.inst
        or request.block != retained.block
        or request.rob != retained.rob
        or history.epoch != retained.epoch
        or history.inst != retained.inst
        or history.block != retained.block
        or history.rob != retained.rob
    )
    kind_mismatch = request.kind != RobEventKind.MICROCOMMIT
    invalid_response = (
        not response.valid
        or not response.accepted
        or response.stale
        or not request.valid
        or not history.valid
        or request != retained
    )
    durability_missing = not history.durable
    duplicate = busy and not identity_mismatch and seen.valid
    capacity_error = (
        busy
        and histories_acked >= retained.mpq_history_record_count
        and not duplicate
    )
    bad = (
        unsolicited
        or identity_mismatch
        or kind_mismatch
        or invalid_response
        or durability_missing
        or duplicate
        or capacity_error
    )
    if bad:
        old = diagnostics[1]
        amount = (
            old.coalesced_count + 1
            if old.valid and old.coalesced_count != 65535
            else 1 if not old.valid else 65535
        )
        diagnostic_overflow[1] = diagnostic_overflow[1] or old.valid
        diagnostic_dropped[1] = (
            diagnostic_dropped[1] + 1
            if old.valid and diagnostic_dropped[1] != 65535
            else diagnostic_dropped[1]
        )
        diagnostics[1] = HandoffDiagnostic(
            owner_mask=HANDOFF_OWNER_MPQ,
            epoch=request.epoch,
            inst=request.inst,
            block=request.block,
            rob=request.rob,
            history_sequence=history.history_sequence,
            coalesced_count=amount,
            valid=True,
            duplicate=duplicate,
            unsolicited=unsolicited,
            identity_mismatch=identity_mismatch,
            kind_mismatch=kind_mismatch,
            invalid_response=invalid_response,
            durability_missing=durability_missing,
            capacity_error=capacity_error,
        )
    else:
        histories[histories_acked] = history
        histories_acked = histories_acked + 1
```
