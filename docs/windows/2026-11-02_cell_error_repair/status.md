# 单格误差修复：当前状态

更新时间：2026-11-02。

## 当前资格

第一阶段实施中，已有源码改动、Release/Debug 构建及代表预测，尚未完成最终验收。起点为 fe91286；计划文档是本窗口开始前已有改动。此前窗口已结束，本窗口不沿用其“未达精度也可带限制收口”的完成标准。

本窗口按正常完整 HTTP 的 self 预测理解“同配置回放”，与原始 DAG 观测成本静态回放区分。当前所说的匹配 token 数量或相同配置，都不是完整结构已证明的结论。

下表是修复前基线，不是当前候选的最终成绩。

| 角色与面板 | 格数 | 已达本窗口门槛 | 未达 | 当前最大误差 |
| --- | ---: | ---: | ---: | ---: |
| 旧 cross | 186 | 175 | 11 | 18.285% |
| 新 cross | 88 | 88 | 0 | 4.365% |
| cross 合计，要求每格 <10% | 274 | 263 | 11 | 18.285% |
| 旧 self | 33 | 21 | 12 | 13.848% |
| 新 self | 8 | 7 | 1 | 3.836% |
| self 合计，要求每格 <3% | 41 | 28 | 13 | 13.848% |

还有 96 个交叉预测无正常真值，不进入本窗口验收分母，也不能据此称全部 411 格达标。两批已有真值均按开发/回归资产使用。

## 优先起点与已有线索

| 代表 | 当前误差和方向 | 调查价值 |
| --- | --- | --- |
| C4 self，w3 / w1 | 高估 13.848% / 12.911% | self 和多个 base→C4 同向偏差，先查共用响应与计费 |
| C3 self，w3 / w1 | 低估 9.716% / 6.382% | 与 C4 配成低估对照，避免整体倍率修正 |
| C5→page32_selective_best_effort，w3 | 高估 18.285% | 当前 cross 最大值，需区分成本、内部等待与策略路径 |
| C5→short_timeout_writeback，w1 / w3 | 高估 13.918% / 13.610% | 检查限时预取、控制与计算的计费和重叠 |
| 新 C5 self，revisit | 低估 3.836% | 新面板也有 self 未达，不能只修旧 186 格 |

上一轮逐请求只读核对中，21 个 cross>6% 的 Prefill query 向量均与全部正常重复一致；请求间隔差最多约 28 ms，总差约 0.38–0.93 sec。主要偏差在请求内部，这只缩小范围，不排除内部 gap、遗漏依赖、成本或重叠问题。11 个 cross≥10% 属于这组，但其余接近阈值的格也需要观察。

独立数据曾显示 64-token 页的冷启动/稳态联合拟合会影响稳态估计；32/128-token 页没有同样的直接改善证据。不能把拆冷/稳态预设成所有尾部的解法，先检查是否解释实际代表。

## 可复用资产

以下路径相对 data/tail_accuracy_generalization_20261102/，均保持原位，不复制或覆盖：

- repair/regression/results.json、cells.csv、predictions.json、measurements.json：旧 186 cross＋33 self 的现行结果和输入/正常观察引用。
- generalization/results.json、analysis.json、normal_contract_checks.json、predictions.json、measurements.json：新 88 cross＋8 self，146 个正常窗口，包含一个条件的五次波动确认。
- repair/initial_outlier_groups.json、initial_request_deltas.json、allocation_headroom_checks.json 及三个代表目录：既有分组、逐请求差异和单独结构修正证据。
- repair/calibration/selected_models.json、maintained.sqlite、maintenance_result.json、initialization_fit_analysis.json：现行 38 类独立响应、有效事实和拟合边界线索。部分历史原始区域缺 iteration，不能直接重导。
- generalization/diagnostics/、diagnostics_summary.json、resource_accounting.json：192 个固定候选执行图、关键链和真实开销；空间采样中断只有部分时段覆盖，不冒充全窗峰值。

原始正常与 base 的更多引用指向本项目 data/r3/、data/r4/、data/r5/；依显式 manifest/observation 使用。不得清理这些后续修复资产。/home/markov 保持原状，/home/markov-next 严格只读。

## 当前实现与恢复起点

第一阶段已完成共因分析和首批修正的限定验证，待本阶段审查提交推送；整体两个逐格门槛仍未通过。

- 新成本形式将首次 phase、新准备规格与稳态分开估计，并为 Prefill 流响应保留一个连续转折。参数仍只来自原 12 次独立采集、8230 条事实；38 类响应沿用同一来源选择。只建立约 5 MiB 工作 SQLite，原资产未修改。
- 第一份候选完成 17 个代表和 41 个 self 开发检查，self 仍有 18 格未达；不能与后续候选拼接成绩。最大旧 cross 代表从 18.285% 降至 3.673%，不代表全部 cross 已通过。
- 补齐新准备规格的分离后，五个代表中 C4/w3 self 为 2.760%、C2/w2 为 0.312%；C4/w1 为 4.648%、C4/w2 为 3.454%、C3/w3 为 10.999%，仍未达。该候选尚未完整运行 315 格。
- Release/Debug 构建完成；两种构建的 20 个实际成本查询与 Python 计算相差不足 1 ns，域外页宽保持缺失。Python Ruff/格式、Docker clang-format 和 diff 检查通过。
- 两次轻量区域诊断分别使用 6/7、原 0/1 设备，完整 HTTP 为 5.273、5.129 sec。随后关闭全部探针、沿用原设备的正常对照为 5.488 sec；原三次正常范围为 5.458–5.884 sec。小 query 的计算区域接近模型，而加载请求的观察方式差异明显，优先检查轮询与文件 worker 的竞争，不能将差异全部解释为 CPU 计算。
- 带探针的两份数据不进入训练或正常真值。无探针对照的 deployment、server、environment、workload、profiling、calibration、record_regions 均与原输入相同，作为第四个有效样本追加到本窗口 measurements；原三份样本及起点分数保留，最终同时说明原均值与追加后的均值。

下一阶段从已推送的本阶段提交出发，先处理文件 worker 在正常轮询下的独立观察，避免用 NPU 事件探针改变调度节奏；另有三份相同生成器的 2048-token 中间工作点请求已准备、尚未启动，补齐 456–1472 区间的成本覆盖。它们是组内共享独立采样，不采用目标请求或残差拟合。

当前没有本窗口运行中的任务。正式证据为 data/cell_error_repair_20261102/：baseline、diagnostics、representatives、self_checks、initialization_checks、calibration、diagnostic_capture、normal_probe_comparison。尚无统一最终候选的完整成绩。

新增设备任务为 3 次成功尝试、261 请求、569424 输入 tokens、261 输出 tokens，累计 573.864 设备组秒；临时 KV 已删除，现有窗口保留量约 0.97 GiB，未连续观测峰值。初步总额度仍为 2 设备组小时、16 GiB；失败和历史用量不清零。
