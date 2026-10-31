# HiCache 跨配置实验

这个例子负责批量调用正式采集、性能库、预测和独立评分接口，不实现第二套模型。
`recipe.json` 声明实验配置、预算和面板，`manual.json` 声明三个手工负载；修改实验不需要修改核心算法。

## 运行

先准备项目的 modeling、SGLang 镜像和 Release 程序。模型目录必须能由本机 Docker 读取；
当前配方用于 Qwen3-32B/BF16/TP2、Ascend 910B3、串行 eager/FIA，不代表其他部署已验证。

从仓库根目录先预览，再启动原始面板：

```bash
examples/hicache_matrix/run.sh --model /path/to/Qwen3-32B \
  --output data/hicache_matrix --devices 0 1 --phase plan

examples/hicache_matrix/run.sh --model /path/to/Qwen3-32B \
  --output data/hicache_matrix --devices 0 1
```

默认原始面板为 5 配置 × 3 workload，每个 base 预测其余四配置，共 60 cross；15 self 单列。
只需 15 份 base profile，正常真值也按 15 个配置/workload 条件共享，不逐 cross 重采。
正常测量默认各重复三次；`--repetitions 1` 可以先取得初步全表，单次样本不提供稳定性证据。
`plan` 只列任务数量，不证明镜像、设备、输入或成本可用。

扩展到完整面板，使用同一输出继续运行：

```bash
examples/hicache_matrix/run.sh --model /path/to/Qwen3-32B \
  --output data/hicache_matrix --devices 0 1 --panel all
```

完整声明为原始 60、扩展 72、留出确认 54，共 186 cross，另有 33 self。
需要 33 份 base profile、99 个正常条件；同一已完成采集会复用。
扩展面板结合更多 HiCache 配置和 ShareGPT 开发负载，确认面板使用不同会话。
这是重建后的复现实验：手工负载保留原输入顺序；ShareGPT 重新选样，固定生成数量，不强制生成相同内容，不能直接沿用旧分数。

## 实际顺序与恢复

1. 在无设备的 CPU 容器中编码输入。手工负载核对原 token 数；ShareGPT 先选样再预测。
2. 串行采集各 base，按整组缺失工作准备少量独立成本数据，共用同一性能库。
3. 准备结束后导出共同模型选择，正式预测所有组；预测不挂载数据库或正常真值。
4. 关闭 profiler 和探针做正常测量，最后独立评分；不根据误差重新训练。

中断后重跑同一命令，会先接管已登记且仍在运行的设备容器，再复用已完成任务。
输出目录属于同一模型和输入；改变模型或负载定义应使用新目录。面板可以扩展，正常重复次数可以增加。
修改预算后重跑会保留累计用量和失败，不能通过重跑重置账本。
设备等待计入墙钟预算；上限不足会明确失败，不无限采集。
`retained_mib` 同时约束运行时临时文件峰值；成功导出后仅清理本会话的缓存页文件，保留 trace 和复现输入。
失败会话保留证据，必要时人工确认后清理其无引用临时页。

可用 `--phase prepare|predict|measure|report` 单独续跑对应步骤；输入依赖不足会正常报错。
每次 `predict` 使用当时的共同模型选择，既有正常样本不参与模型维护。
`--database` 可以指定本项目已有性能库，默认使用输出目录自己的 SQLite。
`--reuse` 接受显式的 source、normal 请求路径映射，不扫描目录猜输入；跨项目不复制或链接实验资产。

## 输入与结果

ShareGPT 来自[公开数据集](https://huggingface.co/datasets/anon8231489123/ShareGPT_Vicuna_unfiltered)。
脚本只流式读取声明的前 4096 条再按种子打乱，不下载或载入整个数据集；也可用 `--dataset /path/to/corpus.json`。
保存选中的输入、会话 ID 和选样规则，开发与确认会话不重叠，包含各自 warmup。
这不是全数据集的均匀随机抽样。独立请求不复用历史，多轮请求使用数据集原有回复构成上下文，回访负载重复同一组长前缀。
请求数、上下文范围和输出 token 数都由配方声明，避免无边界地扩大 DAG 和采集。

`progress.json` 显示本次遍历的步骤和处理数量，恢复时会重新遍历并复用；处理过一组不等于预测成功，用量不会重置。
`preparation.json`、`predictions.json`、`measurements.json` 分别保留准备、预测、正常采集状态。
结果写入 `results.md`、`results.json`、`cells.csv`：包含完整 HTTP、逐格误差、总体及逐 base 指标、正常波动和预测开销。
计时从第一个正式 HTTP 发出至最后一个正式响应结束，包含请求间隔、gap、未归属控制和 Prefill/Decode。
缺成本或失败保留原格数，完整矩阵缺项时不公布完整 WAPE；已评分子集另外标注。

`--source-reports` 可额外生成原始 DAG 静态回放；`--structure` 记录目标图并在正式预测后单独做 target 评价。
后者在扩展面板可能需要额外 target profile；这些 profile 不导入性能库。正常应用预测不需要这两项诊断。
既有预测没有记录图时，先用 `--phase predict --structure` 重跑预测，再运行相同开关的 `--phase report`。
三类报告各有用途；回放完成或误差小，都不能单独证明所有结构与依赖正确。
