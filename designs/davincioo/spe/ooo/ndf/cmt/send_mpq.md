---
doc_id: DOC-DAV-SPE-OOO-CMT-SEND-MPQ
status: draft
authority: normative
owner: spe-ooo
---

# CMT `send_mpq`：通知 MPQ

NDF format 0.2，L2 微架构。条款为本地 draft，尚未完成项目 ID 登记和 L0/L1 对应评审。
[模块契约](../../cmt.md) · [实现](../../cmt.py) · [测试](../../../../tests/spe/ooo/test_cmt.py)

## 规则契约 {#DAV-SPE-OOO-CMT-SEND-MPQ-0001}
<!-- ndf: kind=requirement modality=must refinement=L2 domain=spe.ooo status=draft -->

涉及状态：busy、retained、mpq_sent。实现 MUST 遵循以下行为。

busy 且 required mask 包含 MPQ、mpq_sent 为假时，原样发布 retained，同时置 mpq_sent。发布与置位必须原子发生；输出背压不改变标志。不等待 BROB，且每笔事务最多发送一次，不设超时重发。

MPQ 必须查找自己从 REN 接收的全部相关历史，并逐条返回 accepted、非 stale、valid 的确认，携带原样 request 和 valid、durable 的历史。MPQ 负责集合完整性和归属正确性，历史数为零时无需返回历史确认。

## 验证 {#DAV-SPE-OOO-CMT-SEND-MPQ-VER-0001}
<!-- ndf: kind=verification modality=must refinement=L2 domain=spe.ooo status=draft verifies=DAV-SPE-OOO-CMT-SEND-MPQ-0001 -->

单模块测试 MUST 覆盖 normal、backpressure 场景中对应分支，比较独立期望结果、scan/activation 状态与提交，以及录制前后结果；生成离线回放。测试端模拟外部模块，不能据此宣称实际集成已验证。

## Python 实现示例

以下为 [cmt.py](../../cmt.py) 中的完整 `send_mpq` 规则片段，位于 `cmt` 模块内部；
模块状态声明、共享类型及其他规则见实现文件，片段不作为独立模块运行。
该功能逻辑已通过 [CMT 单模块验收](../../../../../../docs/gates/logs/20260909-cmt-history-loop/summary.md)；源码中的 NDF 注释标记对应本文件的 `doc_id`。

```python
# NDF: DOC-DAV-SPE-OOO-CMT-SEND-MPQ
@ac.rule
def send_mpq():
    nonlocal busy, retained, mpq_sent
    if (
        busy
        and not mpq_sent
        and (retained.handoff_required_mask & HANDOFF_OWNER_MPQ) != 0
    ):
        mpq_sent = True
        return retained
```
