# CMT 单模块实现与验收 — 2026-09-09

工作分支 `feat/davincioo-rob`，起点 HEAD `0e11f0e9`；证据针对当前工作区，
未创建提交或发布。既有 `rob.py` intent-to-add、`env.md` 和此前工作流文档均保留。
用户在本轮确认了 CMT 方案，交付范围是单模块功能、规则 NDF 和回放。

## 实现范围

- `designs/davincioo/spe/ooo/cmt.py`：每 flow 独立、一次一笔事务，按需独立
  发送 MPQ/BROB 请求，完整身份核对、历史编号去重，可靠确认齐全后授权 ROB 释放。
- `spe/ooo/ndf/cmt/` 七份规则级 NDF 与源码同步；模块卡和 MPQ/BROB 对应责任同步。
- 三个来源的诊断缓存、覆盖计数和 sticky overflow，不因诊断背压阻塞正常处理。
- Table/Queue 全状态比较覆盖两个独立实例，scan/activation 提交结果一致，
  录制开关前后投影文件和运行计数一致。测试端模拟真实上下游承诺。
- Decision 0221 通用修复：CSE 在 effect 推导前运行，避免之后删除重复 live
  Table read 时留下过期 footprint。最小复现修复前 footprints=4/operations=3，
  修复后通过逐 pass verifier、QueueGraph 和 gfsim C++ 编译。没有降低 verifier 要求。

## 验证结果

- CMT：8 passed，111.59 秒。normal、capacity、backpressure、invalid、
  diagnostics、recovery、isolation、reset。
- 共享设计 contracts：4 passed，8.36 秒；catalog 检查通过，240 candidates/278 cards。
- focused ACIR lit：4 passed；包括新的重复读取回归、错误 footprint 拒绝、
  multi-state lowering 和 snapshot-set lowering。
- broad `check-acir`：191 passed，2 unsupported，1 failed。
  失败为 `CodeGen/emit-cxx-current.mlir` 中 acir-build 链接缺少
  ACIRBindings/LLVM support 符号；该用例不经过本次修改的 rule-lowering pipeline。
  CMT 驱动显式链接相关库并通过，完整 ACIR 门禁仍不宣称全绿。
- 八个场景的两份录制均由独立 reader 确认为 complete，14 个实例化规则名称完整。
- Chromium：normal（55 commits）、diagnostics（47）、backpressure（53）、
  recovery（19）的全部提交投影与独立重建一致；播放/暂停、前后单步、seek 正常，
  精确 u64 字段保留，全离线，零页面错误。
- scoped pre-commit：merge-conflict、空白、ruff、black、Markdown、API hygiene 通过。

ROB 单/双实例回归：9 passed，75.04 秒，详见 `rob-regression.log`。

## 环境与复现

所有环境相关命令通过 `/home/lc/.codex/skills/pyc6/scripts/run.sh`。
固定激活脚本设置 `ACIR_OPT` 和 `ACIR_QUEUE_CXXGEN` 到当前 checkout 的 build/bin；
不使用 PATH 中可能滞后的安装副本，也不复制其他 checkout 的工具或产物。

```bash
cmake --build .pycircuit_out/local-clang22/build --target acir-opt acir-queue-cxxgen -j 4
python -m pytest designs/davincioo/tests/spe/ooo/test_cmt.py -q --tb=line
python -m pytest designs/davincioo/tests/test_contracts.py -q
python designs/davincioo/tools/check_catalog.py
cmake --build .pycircuit_out/local-clang22/build --target check-acir -j 4
lit -v .pycircuit_out/local-clang22/build/compiler/acir/tests/mlir --filter='rule-cse-footprints|firing-footprint-invalid|rule-multi-state-lowering|rule.*snapshot|rule.*footprint'
```

CMT 默认产物为 `.pycircuit_out/davincioo-cmt/20260909-cmt/`：源码/raw/frozen MLIR、
生成 C++、驱动、编译命令，以及每场景日志、投影、两份 trace、离线 HTML。
`index.html` 是回放入口，`browser-results.json` 记录浏览器结果。
浏览器脚本 `.pycircuit_out/cmt-work/browser.mjs` 使用既有 Playwright、浏览器缓存
和 `browser-deps/usr/lib64`，命令见同目录 `browser-command.txt`。

## 边界与已知问题

[开发问题记录](development-notes.md) 包含接收 guard 修正、尚不支持的嵌套
blocking guard 写法、通用 CSE 修复，以及录制器禁止录制中 reset 的边界。
复位在独立未录制的 populated 模型上验证；reset 动画只展示复位后正常操作。
本轮浏览器检查未发现需要修改 viewer 的渲染问题。

NDF 条款仍为本地 draft，未建立项目登记或未经核实的 L0/L1 关系。
真实 ROB/MPQ/BROB 集成、架构发布、物理资源回收及 Table PYC/RTL 不在验收范围内。
共享框架修复应先于依赖它的设计提交合入；当前未执行提交或合并。
