---
doc_id: DOC-DAV-SPE-OOO-CMT-RELEASE
status: draft
authority: normative
owner: spe-ooo
---

# CMT `release`：授权 ROB 释放

NDF format 0.2，L2 微架构。条款为本地 draft，尚未完成项目 ID 登记和 L0/L1 对应评审。
[模块契约](../../cmt.md) · [实现](../../cmt.py) · [测试](../../../../tests/spe/ooo/test_cmt.py)

## 规则契约 {#DAV-SPE-OOO-CMT-RELEASE-0001}
<!-- ndf: kind=requirement modality=must refinement=L2 domain=spe.ooo status=draft -->

涉及状态：busy、retained、mpq_sent、brob_sent、brob_done、histories_acked。实现 MUST 遵循以下行为。

busy 且所有被要求的请求已发送，历史计数等于所需数量，要求 BROB 时其可靠确认已收到，才可输出 RobHandoff。required/received mask 都等于原请求 mask，required/acked 历史数都等于原请求数量，四个完整身份原样保留，valid、durable 为真。

发布授权和清除 busy 必须原子发生。输出背压时保持事务，不能接入下一笔。成功发布后允许接入下一笔，无需等待 ROB 消费授权。未要求任何 owner 且历史为零时可直接授权。CMT 不发布架构状态、不回收物理寄存器。

没有 flush 输入。上游恢复不能取消已发布到 ROB committed Queue 的交接，旧 epoch 的合法确认必须继续处理。复位清除所有状态，仅可在环境隔离/排空旧消息的复位边界执行。

## 验证 {#DAV-SPE-OOO-CMT-RELEASE-VER-0001}
<!-- ndf: kind=verification modality=must refinement=L2 domain=spe.ooo status=draft verifies=DAV-SPE-OOO-CMT-RELEASE-0001 -->

单模块测试 MUST 覆盖 normal、backpressure、recovery、isolation 场景中对应分支，比较独立期望结果、scan/activation 状态与提交，以及录制前后结果；生成离线回放。测试端模拟外部模块，不能据此宣称实际集成已验证。

## Python 实现示例

以下为 [cmt.py](../../cmt.py) 中的完整 `release` 规则片段，位于 `cmt` 模块内部；
模块状态声明、共享类型及其他规则见实现文件，片段不作为独立模块运行。
该功能逻辑已通过 [CMT 单模块验收](../../../../../../docs/gates/logs/20260909-cmt-history-loop/summary.md)；源码中的 NDF 注释标记对应本文件的 `doc_id`。

```python
# NDF: DOC-DAV-SPE-OOO-CMT-RELEASE
@ac.rule
def release():
    nonlocal busy, retained, mpq_sent, brob_sent, brob_done, histories_acked
    need_mpq = (retained.handoff_required_mask & HANDOFF_OWNER_MPQ) != 0
    need_brob = (retained.handoff_required_mask & HANDOFF_OWNER_BROB) != 0
    if (
        busy
        and (not need_mpq or mpq_sent)
        and histories_acked == retained.mpq_history_record_count
        and (not need_brob or (brob_sent and brob_done))
    ):
        result = RobHandoff(
            epoch=retained.epoch,
            inst=retained.inst,
            block=retained.block,
            rob=retained.rob,
            required_owner_mask=retained.handoff_required_mask,
            received_owner_mask=retained.handoff_required_mask,
            mpq_histories_required=retained.mpq_history_record_count,
            mpq_histories_acked=histories_acked,
            valid=True,
            durable=True,
        )
        busy = False
        return result
```
