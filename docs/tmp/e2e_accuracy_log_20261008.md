# 完整 E2E 精度改进记录

- 2026-10-08：开始。基线 `three_stage_refactor_20261003/stream_order_evaluation`：60 格完成，WAPE 5.520%，最大 APE 16.211%。优先分析目标 C3/W1、C3/W3、C2/W3 的共同偏慢，沿用现有流程和正常真值。

- 2026-10-08：扩大独立 scheduler 取样至 95 个完整间隔，仅有毫秒级变化；CPU 提交迁移尝试没有改善，已撤回。发现预取倍率的 8 次测量全为 profiled 墙钟，却标注 paired light。已限制预取倍率只接纳匹配轻量测量，缺少时使用现有独立分阶段公式，保留争用不确定性。六个代表格最高 8.023%；该批含已撤回的 CPU 迁移尝试，不能代替最终代码成绩。38 个 scheduler 响应和预取公式已通过正式 cost-db 导入/训练复现。开始最终代码的完整 60-cross。

- 2026-10-08：最终代码 60/60 完成并独立评分，真值及样本路径逐格不变、无排除项。WAPE 3.351%、P90 7.740%、最大 8.483%，60/60 小于 10%、44/60 小于 6%；本阶段目标完成。原严格门槛未通过，评价状态 MODEL_LIMITATION、返回 2，非预测失败。Release/validation 正式程序及导出器构建、对应 Python/C++ 静态与格式检查、diff 检查通过；未重跑完整 target 结构比较。最终资产为 `e2e_accuracy_20261008/full_cross`、`full_cross_evaluation`、`normal_io_cost_package.json`，来源与适用限制已同步主文档。
