# 完整 E2E 精度改进

目标：保留当前建模流程，尝试让现有 60-cross 每格完整 HTTP E2E 的绝对百分比误差小于 10%。不排除 gap、未归属成本或 Prefill/Decode。

## 推进顺序

1. 用当前成本包复现 C5→C3/W1、C5→C3/W3、C5→C2/W3，检查实际关键路径和成本绑定，区分服务、等待与采集扰动。
2. 只修复有执行规则或独立测量依据的问题。参数不读取 target 真值，不添加配置专用缩放，不删除未知成本。必要数据优先复用独立历史采集。
3. 在同一组代表格验证修正，并增加低误差对照；公式固定后再运行完整 60 格，独立评分。

## 验收与记录

沿用已选定正常 HTTP 测量，不挑选更接近预测的真值。逐格报告 APE，另报 WAPE、最大误差和未达到目标的格子。无法达标时说明已确认原因与剩余不确定性，不用执行成功替代精度结论。

只在主要发现、实现收口和最终结果时追加简短日志。

## 完成结果（10-08）

最终代码已完成 60/60 cross 和独立正常 HTTP 评分：每格 APE 均小于 10%，WAPE 3.351%、
P90 7.740%、最大 8.483%，44/60 格小于 6%。真值和样本路径逐格与原评分相同，排除项为空。
本阶段目标已完成；没有放宽正式评价器原有的 WAPE/P90 门槛，因此其状态仍为 MODEL_LIMITATION。

保留的改动是预取测量资格与来源标注修正、独立 scheduler 完整重试取样。CPU 迁移尝试已撤回，
未增加 target 专用参数、未补采设备数据。资源争用和成本外推仍有限制，本次没有重跑完整 target 结构比较。
源代码修改后完成 Release/validation 构建、对应 Python/C++ 格式与静态检查；最终评分不训练或改参。

复现输入在 `data/modeling_runs/e2e_accuracy_20261008/c1_full_cross_group.json` 至
`c5_full_cross_group.json`，共用 `normal_io_cost_package.json`。通过公开入口
`scripts/model.sh prepare-hicache --group <组输入> --model-run-jobs 4 --experiment-reports` 预测；
独立 `scripts/model.sh evaluate --normal-http` 汇总五个 `full_cross/<base>/predictions`，
正常测量明确传入原评分中的 30 份 manifest。最终报告为 `full_cross_evaluation/gate_summary.json`，
逐格报告在同级 `cells/`，不混用代表格的实验中间结果。
