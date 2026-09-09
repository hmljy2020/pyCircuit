# FW-0003：无法连续录制跨复位回放

- 状态：现有录制边界；未扩展。
- 用户审阅：待审阅。
- 主线状态：未提交 issue/PR，未确认主线合入。

## 发现版本

- 日期：2026-09-09；任务：CMT 单模块开发。
- 基线 commit：`0e11f0e93bde4668f8a5b43c48b644e0864fbc7f`，不作为正式发布版本声明。
- 分支：`feat/davincioo-rob`；CMT 源码与测试为未提交开发内容。
- 工具：当前 checkout 的 `.pycircuit_out/local-clang22/build/bin/`，通过 `$pyc6` 固定环境运行。

## 现象与影响

希望同一段动画展示复位前状态、复位和复位后行为。
当前对已注册录制对象执行 reset 会报 `replay: finish recording before reset`。
这是录制器主动限制，不是已确认的 viewer 渲染缺陷。

## 触发形式与证据

开始 ReplaySession 并注册模型状态后，调用模型 dispatch rows 的 reset 即触发。
独立可运行的负向复现尚待补齐；[开发记录](../gates/logs/20260909-cmt/development-notes.md)保留实际报错。

## 当前处理

在独立、未录制的模型中验证复位，再录制正常运行。
[CMT 驱动](../../designs/davincioo/tests/spe/ooo/cmt_driver.cpp)的 reset 场景通过，动画只展示干净初始状态后的操作。
未改变框架录制契约，没有修复 commit；若需要跨复位动画，应作为能力增强请求。
