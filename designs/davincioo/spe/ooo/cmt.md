# SPE.OOO.CMT — Commit

- Source candidate: `DAV-SPE-OOO-CMT-0001`
- Hardware hierarchy: **H3**, within H1 `SPE` / H2 `OOO`
- NDF refinement: **L2 microarchitecture**; module-specific L1 behavior links still require review.
- Recommended disposition: **leaf** (proposal, not registry approval)
- Proposed implementation path, if accepted as an independent leaf: `designs/davincioo/spe/ooo/cmt.py`
- Current design-program execution status: **implemented and gfsim behavior verified; NDF traceability, integration and PYC/RTL remain pending**. External source evidence is recorded separately.

A typed NDF Queue contract exists and may have a Python design draft, but the catalog still says planned/deferred; promote only after behavioral and backend gates.

## Inputs

| Name | Payload/type | Meaning | Evidence status |
| --- | --- | --- | --- |
| microcommit | RobEvent | oldest completed ROB row | declared |
| mpq_ack | MpqHandoffAck | durable history handoff | declared |
| brob_ack | BrobHandoffAck | durable block journal handoff | declared |

## Outputs

| Name | Payload/type | Meaning | Evidence status |
| --- | --- | --- | --- |
| mpq_request | RobEvent | retained MPQ handoff request | declared |
| brob_request | RobEvent | retained BROB handoff request | declared |
| rob_handoff | RobHandoff | ROB row release authority | declared |
| diagnostic | HandoffDiagnostic | rejected-response classification | declared |

Payload names in proposed rows are design pseudotypes until fields, widths and nominal identity are frozen. Queue transport is inferred by the compiler, not a requested public Queue wrapper. Clock/reset/time domain are execution context and must not be invented as ordinary payload ports.

## Owned or containing state

- retained microcommit handoff row and owner acknowledgement mask

## Required capabilities to verify

- typed compound Queue payloads
- atomic input/state/output rule semantics

These are requirements, not proof that the current framework is missing each one. First test the current revision; a demonstrated gap becomes a generic framework/primitive issue and regression before a dependent design PR.

## Behavioral acceptance

- All declared/proposed Queue outputs remain stable under backpressure and preserve full identity/generation.
- Stale, duplicate, wrong-flow, and post-recovery responses cause no mutation.
- gfsim and generated C++/Verilog agree at accepted Queue transfers.

gfsim execution is the first implementation gate. PYC/RTL obligations apply to the admitted lowering and remain explicit future work where provisional storage is rejected. Compile-only evidence does not establish behavior.

## Open decisions

- No additional item recorded; exact state/port review remains required.

## ROB seam confirmed 2026-09-08

The [ROB contract](rob.md#reviewed-implementation-contract-2026-09-08) publishes
at most one unacknowledged microcommit per flow. Successful publication to the
`committed` Queue is irreversible, even before CMT consumes it; recovery must
preserve that transaction. CMT must retain its complete identity and payload,
collect distinct durable MPQ histories and required owner acknowledgements,
and return a valid durable `RobHandoff` with matching required counts/mask and
all required acknowledgements. ROB releases exactly once on that confirmation.
CMT must not acknowledge a transaction it has not accepted and durably retained.
This freezes the seam for the ROB testbench; CMT implementation and its own
NDF traceability review remain outstanding.

## Contributor closure

- [ ] Claim the candidate and identify its parent/containing state owner.
- [x] Resolve disposition; aliases and contained state must not duplicate hardware.
- [ ] Link the relevant NDF L0 intent and L1 behavior to this L2 implementation.
- [x] Freeze port payload fields/widths, producer/consumer, parent seam, state/reset and timing profile.
- [x] Define functional branches, all-or-none effects, contention and cancel/recovery lifecycle.
- [ ] Link a minimal failing gate for each actual framework/primitive gap and merge that shared fix first.
- [x] Implement the accepted owner and design-local expected-result tests.
- [x] Prove backpressure, identity/generation, exactly-once effects and isolated instances in gfsim.
- [ ] Integrate into H2/H1 and record admitted PYC/RTL evidence or remaining boundary.

## Source evidence

- `docs/specification/davincioo/ndf-next/interfaces/pycircuit-module-catalog.json:1` — Catalog row: OOO.CMT, source srcs/core/spe/ooo/cmt.py, status planned/deferred.
- `docs/specification/davincioo/ndf-next/scalar/ooo.md:158` — Normative NDF owner/refinement clause.
- `docs/architecture/core/l3/SPE_EXECUTION_PACKETS.md:426` — Detailed proposed module card or disposition evidence.
- `docs/specification/davincioo/ndf-next/modules/spe/ooo/cmt.md:40` — Typed Queue/state contract for existing source draft.

Source paths use the frozen external repository spelling, including legacy `l3/` directories; they are provenance, not the new hierarchy vocabulary. File hashes and the original catalog disposition are in [catalog.json](../../catalog.json). See [architecture](../../ARCHITECTURE.md) for unresolved global decisions.

## Reviewed implementation contract (2026-09-09)

用户确认：每 flow 一个独立 CMT、一次一笔交接；MPQ/BROB 按需独立发送与确认；
MPQ 负责找齐已有重命名历史，CMT 核对身份、去重并计数；收齐才授权 ROB 释放。
诊断不能阻塞正常工作。当前不引入超时、自动重发或架构提交职责。

结构参数与 ROB 一致：core_id u4、pe_id u2、stid u4、launch_generation u16。
本地保留事务、两个发送标志、BROB 确认标志、u4 历史确认数、16 项历史查重表，
以及三个来源的诊断槽、sticky 溢出标志和 u16 饱和覆盖计数。复位全清零。
不新增公共消息类型，不复制 REN、MPQ 或 BROB 的状态所有权。

规则及局部优先级按以下顺序定义；共享状态冲突由编译器处理，规则读取提交前快照：

1. [emit_diagnostic](ndf/cmt/emit_diagnostic.md)
2. [release](ndf/cmt/release.md)
3. [send_mpq](ndf/cmt/send_mpq.md)
4. [send_brob](ndf/cmt/send_brob.md)
5. [acknowledge_mpq](ndf/cmt/acknowledge_mpq.md)
6. [acknowledge_brob](ndf/cmt/acknowledge_brob.md)
7. [accept](ndf/cmt/accept.md)

外部约束：ROB 恰好发布一次合法交接，不在授权释放后重发相同完整身份；
MPQ 确保历史集合完整且编号在事务内唯一，确认原样回传 request；BROB 确认可靠保存；
回复只针对已实际接收的请求生成。CMT 不维护当前 recovery epoch，flush 期间保留
已交接责任；复位和有限身份复用须由环境隔离旧响应。每次接收新事务时清除本地历史有效位。

mask 未要求 MPQ 时历史数必须零；要求 MPQ 且数量零时仍发送通知，历史确认集合为空。
这些边界属于单模块输入契约，真实生产者的遵守情况留待集成验证。

应用 Decisions 0221/0222/0224/0226/0228；验证范围是 G1 单模块 gfsim 和回放。
NDF 项目登记、L0/L1 对应、H2/H1 集成及 provisional Table 的 PYC/RTL 不在本次验收内。

## Reproduction and evidence

Use the `$pyc6` fixed environment from the repository root:

```bash
/home/lc/.codex/skills/pyc6/scripts/run.sh python -m pytest designs/davincioo/tests/spe/ooo/test_cmt.py -q
```

`PYC_DAVINCIOO_CMT_OUT` overrides the default artifact root
`.pycircuit_out/davincioo-cmt/20260909-cmt/`. Each of eight scenarios runs two
independent flow instances in scan and activation modes, compares complete
Queue/Table projections and commit timelines, repeats with recording enabled,
and renders an offline `replay.html`. `index.html` links all scenarios.
Reset is verified on populated unrecorded models; its animation shows clean
post-reset operation because recording across reset is not admitted.

The implementation depends on the Decision 0221 CSE-before-effect-inference
fix in the current checkout. Build current `acir-opt` before reproducing.
[Evidence](../../../../docs/gates/logs/20260909-cmt/summary.md) records the
framework regression, remaining gate gaps and replay checks. This is scoped
module acceptance, not actual ROB/MPQ/BROB composition acceptance.

### FW-0002 表达简化回归

`accept` 已采用“外层阻塞条件 + 内层诊断/业务分支”，与
[NDF 接收条款](ndf/cmt/accept.md) 同步；接口和业务行为保持原契约。
框架条件提交修复与本模块改写的验证见
[20260909-fw-0002](../../../../docs/gates/logs/20260909-fw-0002/summary.md)。
生成模型与回放位于 `.pycircuit_out/davincioo-cmt/20260909-fw-0002/`。

历史有效位清理现使用 `for i in range(16)`，编译时展开为同一次事务中的 16 项更新。
循环与原展开写法的 ACIR 等价检查及 CMT 回归见
[静态循环验收](../../../../docs/gates/logs/20260909-cmt-history-loop/summary.md)。

### FW-0005 字段赋值回归

历史清理使用 `histories[i].valid = False`，保留其他字段并随 `accept` 原子提交。
与展开的 `with_fields` 写法进行 ACIR 等价检查，NDF 示例同步更新。
[CMT、ROB 与框架验收](../../../../docs/gates/logs/20260909-fw-0005/summary.md)
记录功能回归及浏览器检查；最新回放位于
`.pycircuit_out/davincioo-cmt/20260909-field-assignment/index.html`。
