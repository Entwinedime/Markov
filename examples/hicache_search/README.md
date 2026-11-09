# 有限资源搜索

这个例子枚举一组可解释的 HiCache 配置，调用正式 `search`，再单独实测事先规定的几个候选。
它复用同一个预测器、共享成本准备和采集任务，不实现新的仿真器或搜索框架。

```bash
examples/hicache_search/run.sh --model /path/to/Qwen3-32B \
  --workload /path/to/workload.json --output data/hicache_search \
  --devices 0 1 --phase plan

examples/hicache_search/run.sh --model /path/to/Qwen3-32B \
  --workload /path/to/workload.json --output data/hicache_search --devices 0 1
```

模型、workload 是必要输入。workload 沿用正式请求合同，控制请求数和每请求输出 tokens；
当前示例适用于 Qwen3-32B/BF16/TP2、Ascend 910B3、SGLang eager/FIA、串行固定输出。
可使用矩阵例子已经编码的 ShareGPT 回访负载；另用独立请求负载运行同一空间作为低复用对照。
负载在看到搜索结果前确定，不按实测收益挑选。
两类负载尽量匹配输入长度和输出数量；无法匹配时只在各自场景内比较默认与候选，不把跨场景的耗时差归因于复用。

需要生成有界研究输入时，在装有目标模型 tokenizer 的推理镜像中按 CPU 模式运行
python -m examples.hicache_search.workloads --model <模型目录> --sharegpt <公开 JSON 前缀> --burstgpt <公开 CSV> --exclude <前次选择 JSON> --output <输出目录> --output-cap 32。
命令生成 512 条 ShareGPT 多轮、256 条 BurstGPT 长度回放和 1,200 条程序生成请求，各加 8 条准备请求，
同时保存逐请求来源、原始/上限后生成量和不考虑容量的前缀复用潜力；后者不是实际命中率。
BurstGPT 只提供长度和顺序，这里的正文为独立生成，按串行发送；输出上限可在正式输入确定前根据估时改为 16。

`space.json` 给出完整声明：设备 KV token 配额、主机池比例、页大小、写策略、预取策略、门槛和超时，共 288 配置。
设备配额为 3,072/8,192/24,576 tokens，覆盖短回访负载的紧张、中间和完整工作集区间；不是只在几乎相同的容量附近比较策略。
非 timeout 策略不重复枚举无效的 timeout 取值。baseline 不在参数积内时单独加入一次。
这些配置使用同一设备、TP、文件后端及 I/O 条件；主机池大小由原生规则随设备配额和比例变化，不独立设置理想带宽。

资源与成本分别计算：

- 预测报告里的原生 KV 池预留量，包含 NPU 的 padding 页和池内预取缓冲。
- 明确声明的固定预留，覆盖模型权重、workspace、运行时和池外缓冲。默认每 rank 48 GiB 设备、整组 64 GiB 主机，是保守场景假设，不是测量结果。
- 可部署的资源档位：示例固定每 rank 64 GiB 设备，主机配额为 66/70/80/96 GiB，仅用于判断能否部署。
- pricing 对 device/host 存储按 GiB·小时计价，两条共享链路按参考带宽档计价。示例分别为 10/1 单位每 GiB·小时、两条链路各 10 单位每小时；它是归一化比较情景，不代表真实账单，不包含 CPU、整卡算力租价、SSD 容量或流量费。

每个配置采用能容纳它的最小主机配额档位；成本按该配置实际声明的固定存储、原生 KV 池和两条参考链路计算。
超过配额、缺成本和不支持都保留状态，不进入零耗时排名。
搜索先预测，再列出时间/成本 Pareto；selected 为默认预测耗时 90% 以内的最低资源成本候选，
recommended 为资源成本不高于默认时耗时最短的候选。费用相同按耗时和稳定名字排序。
hourly_cost_breakdown 列四项费用，window_cost_units 给正式窗口的资源时间成本；启动和预热另报。
也可声明 maximum_time_sec 作为绝对上限，与 maximum_time_ratio 二选一；正常确认使用预测筛选时已经确定的同一秒数上限，相对正常默认耗时的改善另报。
没有满足时间约束的配置也是有效结论，不临时放宽约束。覆盖不完整时选择仅为暂定，不开始正常确认。
性能准备面向声明的整个候选集；应先约束空间，避免为明显不可部署的配置浪费补采预算。

完整命令可从缺 base、空性能库开始准备，`--database` 指定共享库，`--base-manifest` 可显式复用同一 workload 的来源。
source、共享补采和正常测量分别累计预算；设备任务串行，失败用量不重置。
中断后重跑同一命令，先接管登记中的来源、补采和正常容器。改变模型、workload、base 或参数空间应使用新输出目录。
调整预算后续跑，恢复阶段使用当前上限，已有失败和用量仍累计。
`--settings` 可替换声明，`--phase predict|confirm|report` 可续跑某一步；`plan` 仅预览，不授予资源或成本覆盖资格。
`confirm` 和 `report` 使用已经保存的预测请求及候选，不重写模型、workload、base 或空间；预算仍取当前 settings。

正常确认只测默认和同预算推荐两个角色，重合只测一次，重复数由声明指定。
正常关闭 profiler 和计时探针，并使用所选档位的主机容器上限。
正常结果仅用于评分，不更新模型、重新选点或隐藏失败。

`results.md` 展示各档位最快预测、确认角色的关键配置及正常结果；`candidates.csv` 保存全部配置和资源报价；
`results.json` 保留完整候选、Pareto、正常样本、缺项与实测是否满足固定耗时上限。
缺少某个确认角色或重复时明确未完成，不用其余成功样本冒充完整确认。
没有全空间正常真值，只能评价已确认候选，不能宣称真实全局最优、生产吞吐或 P99 保证。

## 在固定预测预算内选点

默认 `--method enumerate` 评价声明的完整空间。已有共同性能库时，可用下面的入口比较预算搜索：

```bash
examples/hicache_search/run.sh --model /path/to/Qwen3-32B \
  --workload /path/to/workload.json --output data/budget_search \
  --database /path/to/performance.sqlite --base-manifest /path/to/profile_manifest.json \
  --phase predict --method stratified --budgets 48 96 192 --seed 17
```

`random` 均匀随机选择；`stratified` 先覆盖容量、写入和等待策略的分组，随后根据已评价点在邻近配置中细化，并留四分之一的后续名额继续随机探索。
每批最多 48 点，baseline 计入预算；相同的原始来源和成本选择用于所有批次。它只读取自己请求过的预测，不读取完整参照或正常真值。
分组及距离只使用配置参数，不使用候选名称或 workload 身份。两个方法都保存全部选点和 48／96／192 等预算节点的实际耗时。
默认优先使用较简单的 random；预算内找到低成本配置不证明真实全空间最优。

预算模式面向已经维护好的共同成本，缺口明确停止，不为单个算法临时补采。首次空库准备继续使用默认完整流程，维护好整组成本后再做方法比较。
已完成批次可续跑，正在运行或没有完整结果的批次保留状态；恢复得到的未知耗时不会填零。改变方法、种子、预算或业务输入使用新输出目录。
报告中的完成表示本次预算内请求已经评价，不表示枚举了整个空间或证明真实全局最优。

研究对照使用以下入口，最多 3 个进程，每个预测容器沿用声明的内存上限：

```bash
PYTHONPATH=python python3 examples/hicache_search/compare.py \
  --study /path/to/study.json --output data/comparison --jobs 3
```

study 使用 space、model、database、scenarios、workloads、sources、reference_reports、baseline_relative_limits、methods、prediction_budgets、seeds、prices 和 cpu_wall_limit_sec；prices 的每个情景是一组与 pricing 同形的四项单价，
路径相对该文件，workloads/sources/reference_reports 按场景列出明确文件。输入请求、来源与共同模型先确定，参照只在所有运行之后由评价步骤读取，不传给子搜索进程。
`--phase run|report` 可分开执行；runs.json 保留命令、失败和实际墙钟，comparison.json 保存所有种子及条件于找到可行点的成本差距。
三种报价的敏感性使用同一批查询结果重新排序，不能冒充三套独立在线搜索。

研究中的固定条件确认使用 `confirm.py`，沿用原始采集账本，不重新维护失败用量：

```bash
PYTHONPATH=python python3 examples/hicache_search/confirm.py \
  --plan /path/to/confirmation_plan.json --output data/confirmation
```

plan 的 cases 预先列出 scene、target、roles、request、predicted_sec、applicable_limits_sec 和 hourly_cost_units；request 是正式 measure 请求，声明实际资源档位。
initial_repetitions、maximum_repetitions、extra_cv_ratio、near_ceiling_ratio、maximum_extra_conditions 规定重复与追加规则。
正常采集严格串行，轮换场景及条件顺序；追加依据正常波动和固定门槛，不按预测误差挑样。additional_cases.json 保存首次追加决定，续跑不重选。
`--phase report` 只读已有正常观察；results.json 区分采集完整、预测误差与是否满足耗时要求。报告中的实际主机配额从正式请求读取，报价是声明情景。初测未齐或追加未完成时保持 incomplete，保留所有成功观察与失败尝试。
