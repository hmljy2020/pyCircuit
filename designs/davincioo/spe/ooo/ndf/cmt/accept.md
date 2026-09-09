---
doc_id: DOC-DAV-SPE-OOO-CMT-ACCEPT
status: draft
authority: normative
owner: spe-ooo
---

# CMT `accept`：接收 ROB 交接

NDF format 0.2，L2 微架构。条款为本地 draft，尚未完成项目 ID 登记和 L0/L1 对应评审。
[模块契约](../../cmt.md) · [实现](../../cmt.py) · [测试](../../../../tests/spe/ooo/test_cmt.py)

## 规则契约 {#DAV-SPE-OOO-CMT-ACCEPT-0001}
<!-- ndf: kind=requirement modality=must refinement=L2 domain=spe.ooo status=draft -->

涉及状态：busy、retained、发送/确认标志、histories_acked；诊断状态。实现 MUST 遵循以下行为。

合法输入必须 valid、done、handoff_pending，kind 为 MICROCOMMIT，四个身份中的 FlowKey 均匹配结构参数。未要求 MPQ 时历史数必须为零。容量由 u4 历史数限定为 0–15；若要求 MPQ 且数量为零，仍发送通知，历史确认义务为空。

空闲时消费输入、完整保存 RobEvent、置 busy、清除两个发送标志和 BROB 确认标志、将 histories_acked 置零并清除 16 项历史行的 valid，构成一次原子提交。忙时合法输入保持等待。非法输入被消费并进入诊断，不替换当前事务。

历史清理使用 `for i in range(16)` 静态循环及 `histories[i].valid = False` 字段赋值，编译时展开。16 项 valid 的清除 MUST 属于同一次接收事务，不分摊到多个 tick；各行其他字段保持不变。

实现使用外层 `if not busy or bad` 决定是否消费输入；内层 `if bad/else` 分别更新诊断或业务状态。各分支的写入条件 MUST 包含外层条件，未选中分支 MUST 不提交写入。该结构对应 FW-0002 修复后的条件提交能力。

CMT 不维护当前 recovery epoch，已发布的旧 epoch 事务仍可受理；仅检查内部 flow 一致性。ROB 必须恰好发布一次合法事务，不能在交接完成后重发同一完整身份。CMT 不承担永久重放检测。

## 验证 {#DAV-SPE-OOO-CMT-ACCEPT-VER-0001}
<!-- ndf: kind=verification modality=must refinement=L2 domain=spe.ooo status=draft verifies=DAV-SPE-OOO-CMT-ACCEPT-0001 -->

单模块测试 MUST 覆盖 normal、backpressure、invalid、recovery 场景中对应分支，比较独立期望结果、scan/activation 状态与提交，以及录制前后结果；生成离线回放。测试端模拟外部模块，不能据此宣称实际集成已验证。

## Python 实现示例

以下为 [cmt.py](../../cmt.py) 中的完整 `accept` 规则片段，位于 `cmt` 模块内部；
模块状态声明、共享类型及其他规则见实现文件，片段不作为独立模块运行。
该功能逻辑已通过 [CMT 单模块验收](../../../../../../docs/gates/logs/20260909-fw-0005/summary.md)；源码中的 NDF 注释标记对应本文件的 `doc_id`。

```python
# NDF: DOC-DAV-SPE-OOO-CMT-ACCEPT
@ac.rule
def accept(request, core_id, pe_id, stid, launch_generation):
    nonlocal histories
    nonlocal busy, retained, mpq_sent, brob_sent, brob_done, histories_acked
    nonlocal diagnostics, diagnostic_overflow, diagnostic_dropped
    flow = FlowKey(
        core_id=core_id, pe_id=pe_id, stid=stid, launch_generation=launch_generation
    )
    identity_mismatch = (
        request.epoch.flow != flow
        or request.inst.flow != flow
        or request.block.flow != flow
        or request.rob.flow != flow
    )
    kind_mismatch = request.kind != RobEventKind.MICROCOMMIT
    invalid_response = (
        not request.valid or not request.done or not request.handoff_pending
    )
    need_mpq = (request.handoff_required_mask & HANDOFF_OWNER_MPQ) != 0
    capacity_error = not need_mpq and request.mpq_history_record_count != 0
    bad = identity_mismatch or kind_mismatch or invalid_response or capacity_error
    if not busy or bad:
        if bad:
            old = diagnostics[0]
            amount = (
                old.coalesced_count + 1
                if old.valid and old.coalesced_count != 65535
                else 1 if not old.valid else 65535
            )
            diagnostic_overflow[0] = diagnostic_overflow[0] or old.valid
            diagnostic_dropped[0] = (
                diagnostic_dropped[0] + 1
                if old.valid and diagnostic_dropped[0] != 65535
                else diagnostic_dropped[0]
            )
            diagnostics[0] = HandoffDiagnostic(
                owner_mask=0,
                epoch=request.epoch,
                inst=request.inst,
                block=request.block,
                rob=request.rob,
                history_sequence=0,
                coalesced_count=amount,
                valid=True,
                duplicate=False,
                unsolicited=False,
                identity_mismatch=identity_mismatch,
                kind_mismatch=kind_mismatch,
                invalid_response=invalid_response,
                durability_missing=False,
                capacity_error=capacity_error,
            )
        else:
            retained = request
            busy = True
            mpq_sent = False
            brob_sent = False
            brob_done = False
            histories_acked = 0
            for i in range(16):
                histories[i].valid = False
```
