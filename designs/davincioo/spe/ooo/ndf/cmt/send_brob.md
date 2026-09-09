---
doc_id: DOC-DAV-SPE-OOO-CMT-SEND-BROB
status: draft
authority: normative
owner: spe-ooo
---

# CMT `send_brob`：通知 BROB

NDF format 0.2，L2 微架构。条款为本地 draft，尚未完成项目 ID 登记和 L0/L1 对应评审。
[模块契约](../../cmt.md) · [实现](../../cmt.py) · [测试](../../../../tests/spe/ooo/test_cmt.py)

## 规则契约 {#DAV-SPE-OOO-CMT-SEND-BROB-0001}
<!-- ndf: kind=requirement modality=must refinement=L2 domain=spe.ooo status=draft -->

涉及状态：busy、retained、brob_sent。实现 MUST 遵循以下行为。

busy 且 required mask 包含 BROB、brob_sent 为假时，原样发布 retained，同时置 brob_sent。发布与置位原子发生；背压保持状态。不等待 MPQ，不重发。

BROB 在可靠保存完整交接责任后返回同一 EpochKey、InstKey、BlockKey、RobKey 的 valid、durable 确认。发布到请求 Queue 是 CMT 的已发送边界，确认必须由实际接收方生成。

## 验证 {#DAV-SPE-OOO-CMT-SEND-BROB-VER-0001}
<!-- ndf: kind=verification modality=must refinement=L2 domain=spe.ooo status=draft verifies=DAV-SPE-OOO-CMT-SEND-BROB-0001 -->

单模块测试 MUST 覆盖 normal、backpressure 场景中对应分支，比较独立期望结果、scan/activation 状态与提交，以及录制前后结果；生成离线回放。测试端模拟外部模块，不能据此宣称实际集成已验证。

## Python 实现示例

以下为 [cmt.py](../../cmt.py) 中的完整 `send_brob` 规则片段，位于 `cmt` 模块内部；
模块状态声明、共享类型及其他规则见实现文件，片段不作为独立模块运行。
该功能逻辑已通过 [CMT 单模块验收](../../../../../../docs/gates/logs/20260909-cmt-history-loop/summary.md)；源码中的 NDF 注释标记对应本文件的 `doc_id`。

```python
# NDF: DOC-DAV-SPE-OOO-CMT-SEND-BROB
@ac.rule
def send_brob():
    nonlocal busy, retained, brob_sent
    if (
        busy
        and not brob_sent
        and (retained.handoff_required_mask & HANDOFF_OWNER_BROB) != 0
    ):
        brob_sent = True
        return retained
```
