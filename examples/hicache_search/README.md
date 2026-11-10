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

完整合同的顺序对照使用 `contracts.py`，输入为官方 CUADv1.json 和目标模型的本地 tokenizer：

```bash
PYTHONPATH=python USE_TORCH=0 TORCH_DEVICE_BACKEND_AUTOLOAD=0 \
  python3 examples/hicache_search/contracts.py \
  --dataset /path/to/CUADv1.json --model /path/to/Qwen3-32B --output data/contracts
```

在装有 tokenizer 的环境运行即可，不启动模型或设备。它保留完整合同和不同的原始问题，生成交替、长距离回访、分组切换三组各 512 请求，
另加八条准备请求；后两组问答集合相同，仅顺序不同。输入带来源身份、长度、回访距离及不考虑容量的前缀复用统计。
全文或问题数量不足时明确失败，不裁正文或重复问题凑数。定长 32-token 输出用于有界性能实验，不评价问答质量。

若已有性能数据不足以覆盖这组工作，可先声明一次共享扩展，随后所有 target 使用相同的只读模型：

```bash
PYTHONPATH=python python3 examples/hicache_search/calibration.py \
  --settings /path/to/settings.json --output data/shared_extension \
  --models /path/to/selected_models.json --database /path/to/performance.sqlite --plan
```

settings 提供 deployment、capture、calibration_budget，以及 calibration 的 max_context_tokens、page_tokens、geometry。
`--plan` 只保存固定采样声明和核对额度，去掉后才按共同账本串行采集；重跑先恢复和复用已完成采样，不重置用量。
采样覆盖区域、原生搬运和失败回载控制；旧独立锚点只读复用，仅受影响的模型重新训练到新库，原库不变。
它不读取 base 或 target 耗时，也不根据目标误差追加采样。搬运 completion 包括观察边界，不能当作裸物理带宽。

`space.json` 给出完整声明：设备 KV token 配额、主机池比例、页大小、写策略、预取策略、门槛和超时，共 288 配置。
设备配额为 3,072/8,192/24,576 tokens，覆盖短回访负载的紧张、中间和完整工作集区间；不是只在几乎相同的容量附近比较策略。
非 timeout 策略不重复枚举无效的 timeout 取值。baseline 不在参数积内时单独加入一次。
这些配置使用同一设备、TP、文件后端及 I/O 条件；主机池大小由原生规则随设备配额和比例变化，配额用明确的软件搬运准入节拍实现。默认参考值来自 Ascend 910B3 的独立 copy 数据通路及 buffered 文件采样，其他平台应先重新采集，不将其当作 PCIe 或冷 SSD 规格。
自定义 settings 也可提供 resource_packages，每项用 configuration 展开实际资源；它替代 device_tokens/host_ratios 的参数积，
再与所有策略组合。套餐不会绕过正式配置及资源支持检查，也不能仅用名字表示容量或带宽已经生效。

资源与成本分别计算：

- 预测报告里的原生 KV 池预留量，包含 NPU 的 padding 页和池内预取缓冲。
- 明确声明的固定预留，覆盖模型权重、workspace、运行时和池外缓冲。默认每 rank 48 GiB 设备、整组 64 GiB 主机，是保守场景假设，不是测量结果。
- 可部署的资源档位：示例固定每 rank 64 GiB 设备，主机配额为 66/70/80/96 GiB，仅用于判断能否部署。
- pricing 对三级预留容量按 GiB·小时、四向带宽按 GiB/s·小时计价。示例容量单价为 3/1/0.02，两个 PCIe 方向各为 1、两个文件方向各为 10；共同文件带宽按份额收费一次。这是归一化情景，不代表真实账单，不包含 CPU、算力租价或流量费。

每个配置采用能容纳它的最小主机配额档位；成本按该配置实际声明的固定存储、原生 KV 池、文件配额和四向预留计算。
超过配额、缺成本和不支持都保留状态，不进入零耗时排名。
搜索先预测，再列出时间/成本 Pareto；selected 为默认预测耗时 90% 以内的最低资源成本候选，
recommended 为资源成本不高于默认时耗时最短的候选。费用相同按耗时和稳定名字排序。
hourly_cost_breakdown 列七项费用，window_cost_units 给正式窗口的资源时间成本；启动和预热另报。
也可声明 maximum_time_sec 作为绝对上限，与 maximum_time_ratio 二选一；正常确认使用预测筛选时已经确定的同一秒数上限，相对正常默认耗时的改善另报。
没有满足时间约束的配置也是有效结论，不临时放宽约束。覆盖不完整时选择仅为暂定，不开始正常确认。
性能准备面向声明的整个候选集；应先约束空间，避免为明显不可部署的配置浪费补采预算。

完整命令可从缺 base、空性能库开始准备，`--database` 指定共享库，`--base-manifest` 可显式复用同一 workload 的来源。
已有明确独立成本选择时，改用 `--models /path/to/selected_models.json`，整组只读复用，不执行自动补采；它与 `--database` 二选一。
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

study 使用 space、model、database、scenarios、workloads、sources、reference_reports、baseline_relative_limits、methods、prediction_budgets、seeds、prices 和 cpu_wall_limit_sec；prices 的每个情景是一组与 pricing 同形的七项单价，
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

完整合同场景可用 contracts_search.py 编排，沿用正式预测与 confirm.py，最多两个各自受内存上限约束的 CPU 预测，设备采集严格串行：

```bash
PYTHONPATH=python python3 examples/hicache_search/contracts_search.py \
  --settings /path/to/settings.json --workloads /path/to/contracts \
  --models /path/to/selected_models.json --output data/contract_search --phase plan
```

settings 可用 `"scenes": ["alternating"]` 只执行已准备输入的一部分；省略时使用 summary.json 中的全部场景。
`prediction_processes` 可设为 1 或 2，每个场景据此划分互不重叠的 CPU 分区，最多两份预测同时运行，各受声明的内存上限约束。
每份进程只整理一次相同来源；合并全部配置后才统一计价和选点，逐进程的整理开销和内存分别保留。
plan 保存并核对所选场景的正式请求；predict 为每个所选场景采集一个 base 并扫描全部套餐/策略；confirm 在全部覆盖后保存同预算最好/最差清单，再各测一次。
后两个场景可交替端点顺序；角色重合只测一次，不额外测正常 base。report 单独读取正常评价，不更新成本模型。
成本域和全部候选未闭合时不开始确认。单次观察只能说明所选端点的差距，不能证明波动范围、真实全局最优或问答质量。

## 既有大请求量来源的全量离线扫描

`full_scan.py` 复用已经固定的正式预测请求、base 和独立模型，不采集设备数据或自动补成本：

```bash
PYTHONPATH=python python3 examples/hicache_search/full_scan.py \
  --input /path/to/task.json --phase representatives
PYTHONPATH=python python3 examples/hicache_search/full_scan.py \
  --input /path/to/task.json --phase scan
PYTHONPATH=python python3 examples/hicache_search/full_scan.py \
  --input /path/to/task.json --phase report
```

task 提供 requests、deployment、formal_requests、预先固定的 representatives 名单和 wall_limit_sec；同目录的 settings.json 提供现行 policy，usage.json 记录累计执行用量，初始值为 `{"wall_sec": 0, "invocations": []}`。
requests 指向显式已有 manifest/models 的正式请求。代表点计入完整空间，后续每批最多 48 点、最多两路预测；外围编排需另受 4 GiB 约束。
恢复时先检查保留的容器句柄，活跃任务不重复启动；来源、模型或空间改变时使用新的执行目录。

完成后，在装有 Matplotlib 的 Python 环境运行 `python3 examples/hicache_search/full_scan_report.py --input /path/to/task.json`，
读取结构化结果及窗口复核形成的 full_scan_qualification.json、source_qualification.json，导出候选 CSV、并列项、时间—费用/Pareto 图、工作量及运行开销报告。后处理设 4 GiB 地址空间上限。
本例用于 ShareGPT、BurstGPT 长度回放和程序生成三类已完成面板，机制说明需对照实际工作计数；预测完成不授予正常精度或真实排名资格。
本轮已固定的输入和复核证据位于 `data/workload_full_scan_current_20261111/execution/`；复现本轮应使用其中的冻结源码与模型，不能将当前工作区的新实现或模型混入旧批次。
