# 单格误差修复：当前状态

更新时间：2026-11-02。

## 当前资格

第一阶段已交付，第二、三阶段按最新统一门槛联合收尾，完整验收尚未完成。起点为 fe91286，第一阶段交付为已推送的 2084077；筹备计划不计作生产代码改动。本窗口不沿用此前“未达精度也可带限制收口”的完成标准。

本窗口按正常完整 HTTP 的 self 预测理解“同配置回放”，与原始 DAG 观测成本静态回放区分。当前所说的匹配 token 数量或相同配置，都不是完整结构已证明的结论。

下表是修复前基线，不是当前候选的最终成绩。

| 角色与面板 | 格数 | 已达本窗口门槛 | 未达 | 当前最大误差 |
| --- | ---: | ---: | ---: | ---: |
| 旧 cross | 186 | 175 | 11 | 18.285% |
| 新 cross | 88 | 88 | 0 | 4.365% |
| cross 合计，要求每格 <10% | 274 | 263 | 11 | 18.285% |
| 旧 self | 33 | 31 | 2 | 13.848% |
| 新 self | 8 | 8 | 0 | 3.836% |
| self 合计，要求每格 <10% | 41 | 39 | 2 | 13.848% |

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

## 当前实现与交付状态

第二、三阶段的统一验收及关联审查已完成，待分层提交推送。用户取消 self 3% 门槛后，全部角色均要求未舍入 APE <10%；最终同一程序、同一 39 类独立响应完成全部 315 格，0 缺项、0 失败、0 成本缺口。

| 角色 | 通过/总数 | WAPE | 最大 APE |
| --- | ---: | ---: | ---: |
| cross | 274/274 | 1.944% | 9.946% |
| self | 41/41 | 2.189% | 9.926% |

最终证据为 data/cell_error_repair_20261102/validation/results.json、results.md、cells.csv 和 predictions.json。原 219 格和新增 96 格统一运行，墙钟 74.574 sec、3 并发、每容器 8 GiB；没有新增设备采集。原参考评分同时保留：若按原三次 C3/w3 均值，仍有 5 格 ≥10%，最大 12.221%，不能把追加正常观察带来的均值变化算作模型修复。

修正涵盖区域初始化/新准备规格、正常轮询下的文件观察、回载消费控制及自动维护范围。补一个成本不再覆盖适配器其他已选响应；公开准备只补入一项消费控制，原 38 项及来源保持。独立大批传输、额外 context 项、前置 CPU 时间的分析没有成为新的逐格参数；完整结构、CPU/NUMA 和层级交错仍有明确限制。

当前源已完成 Release/Debug 构建，14 个 Python 文件 Ruff/格式、4 个 C++ 文件 modeling Docker 格式和 diff 检查通过。公开共享准备、maintain、calibrate、replay/evaluate 已实际运行；本次 KTransformers maintain 及 self/chunked/sdpa 预测通过，三项数值保持，未重采其设备或正常对照。关联检查沿观察/导入/选择、准备与预测调用方及 C++ 工作和依赖完成；数据引用存在、目标正常观察未挂入预测，全部本任务句柄已结束。

全窗累计 33 次设备尝试、32 完成、1 失败，5236.708 设备组秒、1384 请求，保留约 1.79 GiB；全部原始与失败资产保留，连续空间峰值未测。主工作流已写入稳定结论与限制；最终推送并核对远端后撤下本窗口，不启动下一轮采集。
