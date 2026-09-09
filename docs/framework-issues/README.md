# 框架问题审阅目录

集中记录组件开发中遇到的框架问题，供用户审阅及后续向 `PTO-ISA/pyCircuit` 提交 issue。
每个问题独立成文，保留发现版本、复现、状态和简要解决方案。详细测试日志仍放在 gates 证据目录。

| 编号 | 问题 | 当前状态 | 用户审阅 | 主线状态 |
| --- | --- | --- | --- | --- |
| FW-0001 | [重复状态读取优化后 footprint 失效](FW-0001-cse-footprints.md) | 确认的编译器缺陷；fixed locally, uncommitted（本地已修复，未提交） | 待审阅 | 未提交 |
| FW-0002 | [阻塞条件内的嵌套状态更新受限](FW-0002-nested-blocking-guard.md) | fixed locally, uncommitted（本地已修复，未提交；单输入无输出） | 待审阅 | 未提交 |
| FW-0003 | [无法连续录制跨复位回放](FW-0003-replay-reset.md) | 现有录制边界；未扩展 | 待审阅 | 未提交 |
| FW-0004 | [acir-build 生成程序链接缺少依赖](FW-0004-acir-build-link.md) | 工具链路失败；根因和修复待进一步确认 | 待审阅 | 未提交 |
| FW-0005 | [字段赋值简写](FW-0005-field-assignment-syntax.md) | fixed locally, uncommitted（本地已实现并验证） | 待审阅 | 未提交 |
| FW-0006 | [纯辅助函数与显式内联](FW-0006-pure-helper-functions.md) | 表达能力增强建议；未实现 | 待审阅 | 未提交 |
| FW-0007 | [非法 output presence 的诊断顺序](FW-0007-output-presence-diagnostic.md) | 负例诊断不匹配；待修正 | 待审阅 | 未提交 |

## 维护约定

- 新增问题使用下一个未占用的 FW 编号；不要覆盖旧问题或重新编号。
- 记录保持简明，复现不完整时明确标注，不把推测写成确认结论。
- 修复后更新同一文件和本索引，补充方案、验证结果与修复 commit。
- 临时绕过、本地修复、用户审阅、主线合入分别跟踪。
- 用户审阅后才更新审阅状态；记录完成不表示获准向外部发布。

规则来源：[AGENTS.md / DEV-001](../../AGENTS.md#dev-001-record-framework-issues-for-upstream-reporting)。
