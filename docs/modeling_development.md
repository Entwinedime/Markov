# Modeling 开发与使用

本项目从一个 base 的真实采集构建 source DAG，根据 target 配置推演缓存行为、工作量和耗时。
用户的正式路径是：

~~~text
base profile + 一组 target 配置
  → prepare-hicache：检查成本依据，必要时共享补采，构模并预测
  → evaluate-hicache：有 target 实测时独立评分
~~~

预测不需要 target trace。参数原理见 [HiCache 数值模型](hicache_io_cost_model.md)，
验收口径见 [验证说明](validation/hicache_validation.md)，当前结果及数据资产见 [工作进展](work_progress.md)。

## 1. 当前能做什么

- SGLang：预测 HiCache 配置变化引起的 I/O、控制流程及 Prefill/Decode 变化，输出完整 HTTP 正式窗口时间。
- KTransformers：与 SGLang 共用 profile manifest、DAG 构建和仿真；没有 HiCache，不接受 HiCache 预测。
- NodeScale：框架无关的可选节点耗时缩放，默认关闭，不能替代 HiCache 或计算阶段建模。

完整预测不排除 gap、未归属成本或 Prefill/Decode，但部分残余等待仍沿用 base 近似。
“执行完成”不是“误差达标”，也不证明所有目标的成本都能可靠外推。

部署信息与服务成本依据分别准备；base 无法辨识的服务可能需要独立测量，并非先采完整物理包。
按需补采尚未覆盖所有写入、淘汰和局部返回分支。
已验证的同 TP 预测不意味着支持 TP=2→TP=4 扩图：更换 TP、模型或资源环境需要相应的 base 和适用校准。
采集命令省略 TP 参数时，组环境匹配和控制校准均采用 SGLang 的默认 TP=1，不要求用户重复声明。
详细限制以运行结果和成本来源报告为准。

共享控制补采按实际实验去重：本地提前返回和 best-effort 等待缺口共用一次 best-effort 实验。
若同时缺少预取查询成本，先完成等待实验，再从已准入的完整/轻量配对来源导出查询成本；
没有适用来源才安排新采集。一次 prepare 不为同一个实验反复补采；仍未覆盖的需求保留在缺口报告中。

## 2. 输入与职责

### 代码分工

同一份信息尽量只有一个所有者；观察到的源执行与预测出的目标执行不是两份需要互相同步的状态。

| 信息 | 所有者 | 不负责什么 |
| --- | --- | --- |
| 原始事实、源节点和观测区间 | source DAG、fact、source_dag_index | 不决定 target 采用哪个分支 |
| 源 I/O 操作、工作量及其归属 | io_operation_ledger、attribution | 不把源耗时当成目标完成时刻 |
| 目标缓存状态、操作数量与生命周期 | model 状态机与 runtime | 不按有没有源模板决定合法分支是否存在 |
| 源 host 调用的成本与依赖片段 | runtime/write_host | 不负责目标图落图 |
| 独立控制成本的读取、转换和资源绑定 | runtime/control_calibration、write_resources | 不读取 target trace 或决定目标工作量 |
| 目标提交、等待与传输计划 | runtime/load_execution 等操作生成器 | 不在生成时重新解析校准文件 |
| 共享资源顺序、操作展开和依赖连接 | runtime/write_expansion、FutureDag | 不重新推导缓存策略 |
| 成本需求与共享实验的对应 | Python control_acquisition、物理采样器 | 不看 target 分数，也不维护另一份组状态 |
| 准备、尝试记账、恢复整组预测 | Python prepare | 不实现各实验的测量细节 |

源样本与独立测量仍有不同的读取方式，但得到的 host 操作计划通过同一个落图入口执行。
资源顺序由操作计划统一给出：先是执行的 stream，再是等待的 stream；同一资源重复出现时不能去重，
显式 event completion 则另外绑定。main、worker、设备服务与 residual gap 也不能合并为一个成本。

当前全流程重构正在实施，尚未统一验收：源片段替换结束后，再统一观察保留下来的 worker 队列，
加载、层等待、写完成确认与写入共用这份资源依据。各组件不在删除中间片段后分别认定最终队列。
候选分支的资源不足仍在目标实际需要时报告；已确定执行的调用则立即要求可用资源。
本轮进展与统一检查安排见 [重构计划](tmp/end_to_end_refactor_plan_20260927.md)。

### 输入来源

profiling 与 modeling 只通过 `profile_manifest.json` 交接，不扫描目录猜输入。
运行期间不要修改已选 profile。

一个 base 组可以包含多个 workload，但每个 workload 只选一份 base profile；
这些 profiles 必须属于同一个 base HiCache 配置，模型、TP、后端和资源环境相容。
配置名、workload 名只是身份，不作为拟合变量，也不能代替实际配置与环境核对。

target 文件只含 `{name?, hicache}`，示例见
[hicache_target_example.json](../configs/modeling/hicache_target_example.json)。
target 不得携带成本、观测或评分字段。页大小及策略在输入边界校验。
省略预取策略时，输入解析统一补为 `timeout`，不继承 base 的策略；成本需求检查与执行使用同一个值。

完整 HTTP 评分需要 workload 报告声明正式窗口。只有 bench-serving 总时长、没有 trace 起止坐标时，
不能据此裁剪 trace。普通 build-dag 可保留完整 trace；需要正式窗口的构模或评分会报告缺失。

## 3. 选择命令

所有命令从仓库根目录运行：

| 需求 | 命令 |
| --- | --- |
| 一个 base 预测一组 target | `scripts/model.sh prepare-hicache --group <group.json>` |
| 通用 source DAG 构建、仿真 | `scripts/model.sh build-dag` |
| 已完成预测的独立评分 | `scripts/model.sh evaluate-hicache` |

`build-hicache-model`、`predict-hicache`、`prepare-cpu-service`、
`export-hicache-operation-costs` 和 `calibrate-hicache` 是同一流程的可独立排查阶段，
不是用户必须手工串联的另一套流程。具体参数用对应命令的 `--help` 查询。

`prepare-hicache` 在宿主机协调容器；设备采集进入框架容器，构模与预测进入 modeling 容器。
一次批量预测共用一个 modeling 容器，但整个 prepare 不保证只启动一次容器。

## 4. 构建普通 DAG

SGLang 和 KTransformers 都使用：

~~~bash
scripts/model.sh build-dag \
  --profile-manifest <profile_manifest.json> \
  --output-dir <dag-output>
~~~

该命令不要求 HiCache model。需要应用 NodeScale 等可选变换时，追加
`--model-config <model.json>`；文件直接包含 `node_scale` 等模型配置，不必另写 runner 配置。
省略该参数时只构建并仿真 source DAG。普通 DAG 路径不提取 HiCache 成本观测，也不执行
HiCache control、去 gap 两次分项回放；摘要不输出这些未计算的指标，不能把它们理解为零。
成本构建的专用观测入口、历史静态 HiCache 回放和评分专用 phase carrier 仍按需执行这些步骤。
普通后端与校准操作导出共用 manifest 输入的规范化和 DAG 构建实现；逻辑输入编号、窗口外上下文
及线程预算由同一处处理，不再由两个入口分别维护。
需要复现已生成的运行配置时：

~~~bash
scripts/model.sh build-dag --config <runner_config.json>
~~~

`--config` 原样复现文件中的运行选项，不能同时传直接构图的输出、模型、线程或 DAG 导出参数；
这些参数只用于 `--profile-manifest` 形式。混用会报错，不再静默忽略用户传入的参数。

批量预测直接使用已解析的运行对象，同时保存同一对象的 `runner_config.json` 供上述命令复现。
文件入口负责校验外部配置；内部执行不再先拼原始字典再重新解析。正式窗口保存在执行参数中，
来源仍是 source manifest 指向的 workload 报告，不额外保存一份无人消费的窗口 metadata。
缺少 HiCache 成本模型的任务在规划阶段跳过；执行适配层只处理已具备模型的任务，直接生成 C++ 模型配置，
不再保留无模型执行分支或外包一层随后拆掉的配置字段。

提供配置中启用的模型就会应用相应变换；只回放 source 时应省略或关闭模型。
不要用旧的 mode 字段假定已声明的变换被屏蔽。

## 5. 一个 base 组

### 最小起点

复制 [hicache_group_example.json](../configs/modeling/hicache_group_example.json)，替换实际路径：

~~~json
{
  "base_manifests": ["<base-profile>/profile_manifest.json"],
  "target_configs": ["configs/modeling/hicache_target_example.json"],
  "output_dir": "data/modeling_runs/my_hicache_group"
}
~~~

target 也可以直接写成 `{"name": "...", "hicache": {...}}`。已有 base 时不需要矩阵 suite，
base/workload 身份可从 manifest 推导；没有显式 metadata 名称时使用配置名称或 run 身份。
多个 workload 应显式使用相同 config_id 和不同 workload_id，评分时与 target 身份对应。

先检查输入，再执行：

~~~bash
scripts/model.sh prepare-hicache --group <group.json> --dry-run
scripts/model.sh prepare-hicache --group <group.json>
~~~

最小配置没有额外采集预算。它会报告缺少的依据，不能保证仅靠这份配置就能完成预测。

`--dry-run` 不启动设备采集、CPU 配对准备或 target 预测，但会写摘要，必要时从已有 base 构图提取观测；
它不是完全无写入的语法检查。`--calibration-only` 准备成本依据后停止，不发布模型或预测。
源码或原 trace 实质改变后，可用 `--refresh-observations` 重新提取；没有版本号、摘要或冻结副本替你判断变化。

### 已有校准与预算

成本选择顺序是：base 实测 → 有依据的外推 → 适用的共享独立测量。
不能把其他 base 或 target 的观测改名后当成独立校准。
已有独立物理测量能提供缺少的服务参数时，直接复用，不为缺少运行时倍率强制再采 workload；
构模摘要会注明这项估计尚未验证推理负载下的竞争影响。CPU 控制、计算和执行分支缺口仍独立处理。
预测阶段的普通设备释放缺口也可进入共享补采：使用同一 `budget` 计费，经过完整/轻量配对与 CPU 校正后导出，
供整组使用并恢复预测。预算不足会保留失败格和具体成本需求；不会借用备份释放成本，也不无限重试同一缺口。
补采完成后与首次准备共用观测准入、成本检查和构模路径；不另外拼装模型，已有适用观测继续复用。
已有适用的校正释放文件可直接声明在 `control_calibrations.release_host`；独立导出命令支持 `--operation release_host`。

在组配置中按需加入以下字段：

~~~json
{
  "physical_calibration": {
    "report": "<physical-report.json>",
    "measurement_sources": ["<original-measurement.json>"],
    "measurement_description": "Target-independent platform measurements."
  },
  "fixed_calibration_manifests": ["<independent-profile>/profile_manifest.json"],
  "budget": {
    "wall_seconds": 1200,
    "server_starts": 3,
    "requests": 100,
    "tokens": 100000
  }
}
~~~

这只是字段补充示例，须与 base、targets 和 output_dir 放在同一组配置中；预算值由实际可接受开销决定。
`fixed_calibration_manifests` 可为空，这个沿用的字段名不要求固定端点或固定重复套餐。

各类采集的预算分开声明：

共享 workload 补采生成一个显式实验，不再构造单行单列矩阵；server、bench 和普通配置/workload 标签直接声明。
仍保留 suite 结果清单和 token bundle，用于确认实际产物及完整/轻量回放；不要求用户手工维护这些生成输入。

| 情况 | 组字段 | 必要输入 |
| --- | --- | --- |
| 缺 base profile | `base_capture_budget` | `profile_suite`、base/workload 选择、`forced_token_bundle` |
| 缺 base 的轻量 CPU 回放 | `cpu_service_capture_budget` | 可从已选 base 自动规划；默认复用 base 报告记录的 token plan |
| 缺共享 workload 依据 | `budget` | base 的合法 token 输入 |
| 缺物理服务或 CPU 原语测量 | `physical_capture.budget` | 对应物理采样域、设备与 CPU 绑定 |

前三类使用 wall_seconds、server_starts、requests、tokens；物理采样使用
wall_seconds、container_starts、logical_io_bytes。省略 workload budget 等价于零预算。
启动、失败、重试和无效 trace 都计费；墙钟保留 30 秒用于超时清理，不能换目录绕过累计预算。
普通采集和 base 轻量回放共用产物记账：终态 manifest 明确说明 workload 未启动时，请求和 token
不扣预留量，启动次数与已用墙钟仍计费；缺少该证据时保守计费。失败命令不会因产物存在而变成成功。
每项补采按自己的规模检查累计预算，不因某个大实验超限而跳过后续较小实验或已有测量复用。
例如物理 I/O 与锁定候选 CPU 原语共用物理预算，但 CPU 原语不消耗 I/O 字节配额；控制 workload 使用另一份预算。
成功补采后继续预测，新出现的需求仍按相应账本检查。被拒绝的同一项需求本次运行不重复尝试；
流程不能自行突破预算、清空账本或把未完成测量当作零成本来继续。
经用户授权调整累计上限时，只更新组配置的预算，历史成功、失败和重试用量仍保留。
某项缺少采样声明、适用采样器或物理预算时，保留该项缺口，其他独立需求仍可继续补充；
这类未启动采集的情况不视为进程失败。同一项需求本次运行只尝试一次，不反复生成相同实验。
例如只声明了 `eviction_cpu` 的组遇到 I/O 缺口，会提示补充 `physical_capture.page_token_sizes`，
不会因为缺少 I/O 采样域而抛异常、中断本来独立的 CPU 或控制补采。
进程失败、未完成清理等其他停止原因仍终止后续采集；已取得的成本保留，并在可构模时统一用于重新预测。
base 补采按 profiling 的正式展开规则选择实验，保留 experiment 覆盖与 `$unset`；
每个 base/workload 必须唯一匹配一个已声明实验，不从两个矩阵轴重新拼出被排除的组合。
`profile_suite` 也可指向普通单实验或显式 experiments 文件，不要求 matrix；使用
`metadata.config_id/workload_id` 标明要采集的 base 与 workload。已有显式 token plan 时只回放一次；
使用 `{forced_token_plan}` 时，才从所给 bundle 取计划，或先捕获 tokens 再回放。
target 若用实验配置名简写，读取展开后的实际 server 命令；同名实验的 HiCache 配置不一致时，
改用明确的 target 对象或文件，不任取其中一个。标签仍不进入成本拟合。

例如声明物理采样：

~~~json
{
  "physical_capture": {
    "page_token_sizes": [32, 64, 128],
    "devices": [2, 3],
    "cpu_sets": "<rank-0-cpus>|<rank-1-cpus>",
    "budget": {
      "wall_seconds": 900,
      "container_starts": 2,
      "logical_io_bytes": 100000000000
    }
  }
}
~~~

页域是平台的显式采样域，不从 target 列表反推。设备与绑核须匹配 base 环境。
首次准备会自动从 base 部署读取 KV 几何和存储范围，不要求声明 `physical_capture`；这不启动测量，
也不提供任何测得的成本。宿主机无法读取 base 模型的 config.json 时，报告具体路径及已有校准替代方式。
修改采样次数、页域或采样绑核不会使这份部署信息失效。
实际 I/O 补采仍须声明页域、适用放置和预算；没有页域时不会自动沿用账本中的旧 I/O 测量。
仅补锁定淘汰候选 CPU 原语时声明 `physical_capture.eviction_cpu` 的 heap_sizes、batch、repeats，
不要求 DMA 页域，逻辑 I/O 字节为零。原理及外推限制见[锁定候选成本](hicache_io_cost_model.md#61-淘汰时跳过锁定候选的-cpu-成本)。
另一组已有适用测量时，可在 `physical_capture.eviction_cpu.reuse_ledger` 显式填写其
`physical_capture_ledger.json` 路径。只复用环境、CPU 绑定和采样域相符的成功测量；
来源不相符则报告缺口，不静默换成新采集。旧账本及耗用留在原处，新组引用原始报告，不复制或清零历史用量。

### CPU 采集开销校正

CPU 成本校正比较同一个 base 的完整采集与轻量回放，不拿 target 正常耗时拟合 base。
新采集的 base 轻量回放与共享控制校准的轻量回放使用同一验收规则：manifest/workload 完成、
没有采集错误、forced-token replay 验证通过，且仅含 LD_PRELOAD 通道。命令成功不能替代这些条件；
CPU 配对所需计时文件继续由对应采集路径检查，失败尝试仍计入预算。
两份输入必须有可对应的 token、请求、rank、forward 和层调用；没有配对依据的成本保留原值。
共享控制补采恢复时，轻量回放继承所选完整采集实际使用的 token bundle 和 workload 输入，
不改用后来新捕获的输出 tokens。已有轻量采集也只在 bundle 对应时复用；无关尝试仍计入累计预算。

已有修正文件使用 `cpu_service_costs: ["<cpu_service.json>"]`。
需要自动补轻量回放时，声明 `cpu_service_capture_budget` 并省略 `cpu_service_pairs` 即可：
流程为尚无 CPU 成本的已选 base 自动建立配对，TP 来自各自的实际 server command，未指定 TP 时采用框架的单 rank 默认值。
预算仍须覆盖实际请求和 token，缺少必要计时或可验证的 token 回放依据时会停止，不会改用 target 数据。
CPU 轻量回放的 dry-run 也会核对累计预算，并显示当前这一份 base 回放的启动、请求和 token 需求；
这不是整组所有 base 的预算估计，也不代表已验证实际采集能完成。缺少预算或存在未完成采集时，
入口直接显示停止原因，详细用量仍保存在 `group_summary.json` 的 `cpu_service` 中。
已有 light profile 或希望显式指定配对时使用：

~~~json
{
  "cpu_service_pairs": [
    {
      "profile_manifest": "<full-base>/profile_manifest.json",
      "light_manifest": "<light-base>/profile_manifest.json",
      "tp_size": 2
    }
  ]
}
~~~

同一 source 不可重复绑定或同时声明已有成本和重新准备。
声明 pair 后，pairs 与已有成本文件合计须覆盖所选 sources。同源修正由全部 target 共享。
显式空 `cpu_service_pairs` 表示不自动创建配对；未声明 CPU 预算时也保持不自动采集。
缺 light 时需要 CPU 回放预算，默认直接使用 base 报告记录的 token plan，不要求再制作 bundle。
若显式提供 `forced_token_bundle`，它必须与 base 的原始计划一致；生成的轻量配置仍引用 base 计划，
不再先改回占位符、再由采集入口用 bundle 替换。base 本身缺少必要计时，不能仅补 light 修复。
轻量回放保留 base 的 workload 命令原文，包括 token-plan 路径；每次预算内尝试只允许启动一次服务，
不能在一次记账中隐藏多次启动重试。配对比较忽略输出用的 `run_id` 和启动重试次数，
但仍要求实际推理配置、请求及 token 计划一致。

单独排查：

~~~bash
scripts/model.sh prepare-cpu-service \
  --profile-manifest <full-manifest> --light-manifest <light-manifest> \
  --tp-size <tp> --correct-recorder --output-dir <cpu-output>
~~~

输出 cpu_measurements.json、cpu_service.json 和 cpu_service.retained.json。
retained 报告说明不能安全扣除的 recorder 成本；缺报告不能解释成全部扣除。
Release 默认只保存保留条数、保留 CPU 时间与 recorder 校正汇总；逐条 `rows` 明细仅在
`TRACE_GRAPH_DEBUG=ON` 构建中输出。两种构建生成同一份正式 CPU 成本，Python 准备流程不依赖明细。
正式 cpu_service.json 按区间逐条写出，每条记录一行，避免为序列化再拼一份完整区间表；字段与读取合同不变。
校正只处理已测量区间，不清零 residual gap，也不是逐条指令的开销归因。
host、scheduler、输入处理和层等待配对直接生成统一的 CPU 测量记录；source 在配对入口绑定一次，
合并及追加 recorder 修正时共用线程区间重叠检查，避免重复扣费，不再经过字段转换中间层。
CPU 准备只构建一次 source DAG：先绑定 forward/host 的配对成本，再在同一张图上筛选尚未覆盖的 recorder 写入。
每份采集的同步事件在输入处统一转换为纳秒区间，由 forward、host、输入、scheduler 和 layer 观察共用；
各观察器不再重复解析 Chrome trace 或反复换算时间单位，仍分别检查自己的区间归属和成本扣除。
原始写入保存在 cpu_measurements.json 的 recorder_writes 中；C++ 输出扣除及保留统计，Python 不再读回成本后第二次构图。
forward、HiCache host 和 scheduler 的观测与配对分别放在 `calibration/step_timing.py`、`host_timing.py`、
`scheduler_timing.py`；host、scheduler 和输入处理共用成本记录的生成规则，层等待仍单独保留整数预算。
测量文件保留原始时间坐标、配对成本及修正依据，不再导出无消费者的 forward 墙钟差、同步耗时差和 probe 外包区间差；
这些诊断项不参与成本扣除，删除它们不表示对应开销已被消除。

### 共享控制成本

组级 `control_calibrations` 声明已有操作成本文件：prefetch_wait 按策略、write_host 按页大小和写策略索引，
load_index、load_submission、layer_wait 等按操作名声明。导出用法见[共享 CPU 操作校准](hicache_io_cost_model.md#共享-cpu-操作校准的采集开销)。
base 没有完整预取查询片段且未提供适用校准时，执行器报告 `execution_control/prefetch_query`。
组流程先从已声明、同环境且有配对 CPU 修正的预取等待校准来源重新导出查询成本，不要求等待策略相同。
没有这类来源时使用共享 wait-complete 校准实验，复用匹配的原始采集或在预算内完成三阶段采集，只导出查询成本，
自动加入 `control_calibrations.prefetch_query`。也可用 `--operation prefetch_query` 手动导出并声明。
已有来源的复用/导出已用真实独立采集验证；无已有来源时的新增设备补采尚待验收。查询局部区间仍是观测墙钟包络，
不能把配对文件存在解释成所有查询开销都已剥离；成本文件保留未校正等待等限制。

显式声明与组内已完成导出合并，同一操作以显式声明为准。组输入先核对来源、环境及预取策略族，
需求扫描使用已经准入的集合，不重复读取校准文件；具体操作、资源与依赖语义由 C++ 消费端核对。
导出成功不等于全部 target 分支齐备；未做 CPU 校正的依据仍标记为 profiled 成本。

## 6. 准备流程与失败处理

prepare 按顺序完成：取得 base → 准备 CPU 修正 → 提取观测并选择成本 → 检查和补充共享缺口 → 发布模型 → 组预测。
物理服务、workload 和控制补采共用一个检查循环。一次执行暴露的多类 I/O 缺口按服务串行采样，
每项采样沿用前一项的报告，完成本批后统一重新构模、检查缺口；DMA 双向需求合并为一次实验。
存储采样底层也只执行指定的一种服务，不再保留整包串行采样分支；原始观测在拟合前保存，
拟合失败仍可检查测量结果，采样成功或失败都会关闭本次 worker。
若中途因预算等原因停止，已完成的服务仍保留并进入构模，不把整个批次算作全部完成。
同一次准备不重复尝试同一服务。共享模型更新后整组重新预测，不沿用旧模型下的成功格结果。
已有匹配的物理测量可直接复用，不要求新增采集预算；预算和活动采集检查只约束新采集的启动。
后续服务补采会扩展共享报告；只要测量条件未变、当前报告仍包含原测量，报告路径变化不触发重采。
换成不包含原测量的独立报告时，不能仅凭旧账本记录宣称当前成本已补齐。
物理成本不再只在流程开头检查；后续观测暴露的物理缺口也进入同一条预算受限的补采路径。
成本输入统一检查来源角色及 manifest 唯一性；重复观测报错，真实重复测量使用各次采集自己的 manifest。
数值参数只保存于 hicache_io_model.json，参数来源与范围见 model_build_summary.json。
正常准备与独立构模共用一次成本选择和发布决策，上层只接收就绪报告，不再转交内部成本字典。
观测提取后先保存；base、部署几何、CPU 或 phase 依据不足时，构模摘要记录缺口，不发布模型。
I/O 服务可以缺席：`service_gaps` 列出尚无成本依据的服务，模型只保存可估计的服务，不填零。
这时构模 ready 只表示可开始目标执行，不保证目标可完成；实际传输请求缺失成本时，失败格 HTTP 为 null，
组流程按该服务的缺口安排预算内共享补采、重新构模并恢复预测。未使用的服务不会因缺席而强制采集。
I/O 补采直接采用执行器报告的具体服务需求，不要求它同时出现在构模前的缺口摘要中；
采样器映射只在物理采集模块维护，构模前的覆盖报告用于解释已有依据，不再充当第二道补采门槛。
同轮发现的可补采缺口串行合并处理，再统一构模和预测，不为每一种成本单独重跑整组。
预算不足只拒绝当前实验，后续不同需求仍分别检查剩余限额；采集失败等其他原因停止后续采集。
若本轮已获得新数据，先用这些数据构模并预测，再报告剩余缺口和停止原因。
已有释放校准仍不适用时保留其缺口，不重采相同声明，也不阻断其他 target 的独立校准需求。
最终采集用量从账本汇总，包含预测阶段触发的补采及失败尝试。
group_summary.json 的 captures 按顺序记录本次准备中的共享补采结果，并用 operation 标明 phase、I/O 或控制需求；
命令结束时逐项显示状态和停止原因，不再只保留最后一项。它是本次流程摘要，不是另一套预算账本；
跨次运行的失败、重试和已完成测量仍以原采集账本为准。
dry-run 和 calibration-only 不执行目标，因此不能证明这些服务是否需要；两者仍报告 service_gaps。
构模前仅准备部署信息、同源 CPU 和必要的 phase 数据，不再保留另一套 I/O 服务补采循环。
缺少部署信息时先解决输入声明；不能靠运行传输采样替代模型结构与资源配置。
目录中的旧模型文件仍保留，不能把它的存在当作本次构模成功；以本次退出状态和摘要为准。

当前补采能力有明确边界：

- 仅缺计算参数时，使用常驻前缀和不同长度续写；I/O 缺口按实际服务选择独立测量，
  不再通过通用 workload 规划器生成整套缓存压力实验。控制 CPU 缺口仍使用有针对性的配对 workload。
- 预取等待成本在目标实际进入活跃分支时检查；只发生本地提前返回时，不要求完整等待程序，仍必须有本地返回成本。
  已删除构模前的预取程序预扫描及内部 `--observe-prefetch-policy` 参数，需求不再由两套逻辑判断。
  执行报告缺口后，可安排 token 记录、完整回放、轻量回放、CPU 校正及控制导出。三次运行都计费，不将首次采集与强制回放直接相减。
  token 记录阶段关闭 profiler，仅保留请求执行和 HiCache 状态检查；其结果提供回放 token，不作为成本观测。
  完整回放保留原采集配置，轻量回放只用 LD_PRELOAD；两遍回放才用于同源 CPU 校正。
  正式构模不再估计历史静态路径使用的 commit、state-check、load admission 标量，也不报告其缺口或触发补采。
  历史模型参数的读取及静态回放保留；现行执行式预测继续使用操作级 CPU 成本，缺失时不填零。
  这条流程只导出所缺的 prefetch_wait，不要求同时成功导出 write_host；已有写入校准保持不变，写入成本仍可显式导出和声明。
- 锁定候选 CPU 缺口可安排共享原语测量；获得新测量后重建并重试整组一次，不原样重试已有证据。
- 写入、淘汰、局部返回等需求检查仍不完整。执行失败报告的 missing_costs 可能只是遇到的第一个缺口，并非完整需求清单。
- 物理、workload 或 control 实验完成后重新检查成本。如果缺项清单原样保留，返回 experiment_exhausted，
  不再重复生成或复用同一实验尝试补洞；已有采集和累计用量保留，需检查缺失的变量或执行分支。
  公开入口同时打印各阶段状态、预算限项、停止原因及采样器限制；完整记录仍见 group_summary.json。
- 首次没有物理报告时，先从 base 部署读取 KV 几何、批大小和资源放置，写入 platform_inputs.json。
  该文件不含实测系数；成本检查先辨识 base 参数，再按缺项选择 DMA、预取、覆盖写或新页写入补充。
  读取这些部署信息不要求解析 CPU 采样绑核，也不受采样器暂不支持 membind 的限制；
  只有实际需要独立测量时才检查这些条件，不能把未运行的测量限制变成 base 成本建模的前置要求。
  零采集预算不启动测量进程。I/O 按实际执行需要补齐；存储写入的新旧页成本分别保留和按批次检查，
  纯新页不要求覆盖写成本，纯已有页不要求新页成本，混合批次必须具备两者。
  CPU/phase 仍在构模前检查，真实多缺口补采也未全面验收，因此尚不能宣称任意组都已实现最小补采。
  采样器只处理明确请求的缺口：补 DMA 或写入不要求先有预取参数；没有需求时不启动测量。
  只补 Prefetch 阶段时不再要求 DMA 原始报告；从已有物理报告的 measurement_scope 读取设备和 NUMA 放置，
  与当前采样声明核对后只运行预取实验。缺失或不匹配时报告 existing_storage_placement_missing_or_different。
  独立命令统一使用 `--base-report <已有报告>`，通过 `--service prefetch|existing_write|new_write` 选择服务；
  DMA 和存储采样均从该报告读取 KV 几何、TP 设备及 NUMA 放置，不再重复传入模型配置、TP 或设备参数。
  首次可使用 prepare-hicache 生成的 platform_inputs.json（只有部署信息，没有测量系数）；
  存储命令的 `--model-config`、`--tensor-parallel-size`、`--kv-element-bytes` 已删除，
  DMA 命令的 `--devices`、`--numa-nodes`、`--layer-count`、`--kv-heads-per-rank`、`--head-dim`、`--element-bytes` 已删除。
  CPU 绑核、页大小和测量规模仍是采样参数；组入口在启动前核对报告与 base 的几何和设备放置。
  两个参数均为必填；不再保留默认一次采全的 `--service all`、`--runtime-dma-report` 和 `--skip-new-write` 分支。
  需要多项测量时由 prepare-hicache 按实际需求串行安排并合并报告；首次所需的部署信息也由该入口准备。
  已有完整物理报告仍可作为输入，不需要重新采集或转换资产。
  新页写入补充只运行运行时的 materialize-then-batch-set，不启动 DMA、覆盖写或预取测量；
  覆盖写补充只预填充并测量已有键写入，不运行 DMA、预取或新页队列实验，保留已有新页成本参数。
  DMA 曲线缺失时单独调用 `calibrate-hicache runtime-dma --base-report <已有报告>`，不启动存储测量。
  DMA 补采按缺口选择 H2D、D2H 或两者，采样操作和逻辑 I/O 预算使用同一方向集合；
  独立命令可用 `--directions host_to_device` 或 `--directions device_to_host`，不指定时仍测双向。
  合并报告只填缺失曲线，不覆盖已有有效参数。采集前核对 KV 几何、设备及 NUMA 放置。
  两个操作规模来自明确的 payload 或 base 输入规模，页大小域来自 physical_capture 声明，不读取 target 真值。
  成功后保留其他服务参数与来源，按累计物理预算计费。组准备重新检查成本；同一缺口不变时停止，不重复测量。
  预取补采的工作量只由声明的页大小、批大小和重复次数决定，不读取 token plan 或准备 DMA/新页写入规模。
  已完成原语按采样规模、几何、设备/CPU/NUMA 放置和环境复用，不把生成命令的拼写当成有效性证明；
  平台测量的适用性不依赖 base token 文件路径，调整 CLI 不会单独触发重采。
  调整 CPU 淘汰实验的样本规模或预算不会作废已有 I/O 报告；I/O 页大小域、设备和 CPU 放置仍参与自动复用判断。

补采使用 base 环境和输入 token，不读取 target trace 或误差。普通模板 workload 报告中的实际 token、
显式 token 模板或 base 自身回放计划可以作为输入；旧文本采集未记录 token 时需补充自身输入，
不从其他 cell 借用，也不编造词表编号。

先看 group_summary.json 的 status 和缺口：

| 状态 | 含义与下一步 |
| --- | --- |
| predicted | 组内预测已完成；查看成本覆盖，再独立评分 |
| ready_for_model_build | 检查/准备已够，可以执行正式 prepare |
| needs_calibration / needs_control_calibration | 已有待执行计划，常见于 dry-run；查看所需预算 |
| budget_exhausted | 预算不足，不会自动越预算启动 |
| cpu_capture_budget_exhausted | CPU 轻量回放预算不足；查看 cpu_service.limits 与 capture_usage，调整预算后重新 prepare |
| cpu_capture_failed | CPU 回放命令未完成；查看 cpu_service.attempt 的失败原因与 command_log，失败用量仍计入预算 |
| capture_incomplete | 存在未收尾记录；提示包含账本路径、记录状态和 Docker 当前查询结果，先核对再恢复 |
| experiment_exhausted | 所选实验已做但证据仍不足，重复同一实验没有依据 |
| no_suitable_experiment | 当前没有能解决这些缺口的实验 |
| prediction_incomplete / prediction_failed | 查看 prediction.missing_costs、失败格日志及实际执行结果 |

采集失败保留其原状态。base、CPU 回放、物理和共享 workload 补采统一在对应结果的 `attempt` 中
保留本次采集记录；命令行直接显示已有的失败原因、退出码和 `command_log`，不必再翻多个报告找日志。
物理实验被预算拒绝时，限制项也保存在 `attempt.limits`；尚未进入实验的准备限制仍在计划顶层。
遇到未完成采集记录时先核对进程、容器和日志，不因记录旧就删除账本重启。
入口只查询该记录声明的容器，不把账本中的 running 当作活进程证据。容器不存在或查询失败仍不自动重试：
它们不能证明产物有效、清理完成或用量已结算；当前还需要人工确认未收尾记录，不宣称已实现无人工中断恢复。
命令失败、预算超时或中断后，损坏的结果文件作为 artifact_error 附加记录，不覆盖原始停止原因；
只有命令已完成但产物验证失败时，才使用 invalid_capture_output。没有可读用量证据时仍按预留预算计费。
CPU 补采遇到上述停止状态时保留已准备的 CPU 成本文件，不启动后续构模或预测，正式命令返回非零。
省略 light_manifest 时，先查找同一 base 和回放输入下的已完成轻量采集；正式运行和 dry-run 均可复用，
不要求新增采集预算。只有没有可复用结果时才要求预算；dry-run 不创建采集目录或启动回放。
这与损坏的 trace/manifest 不同：无效采集数据及意外异常仍报错，不当成普通预算不足。
已完成阶段可复用；异常保留失败阶段和本次尝试耗时，强制杀进程或写盘失败可能来不及保存终态。
采集墙钟用量统一从登记尝试开始，覆盖容器运行、清理及产物读取和验证；不是仅计推理运行时间。
结果验证失败也会计入本次用量，后续补采使用更新后的累计预算。历史账本保留原记录，不倒算旧采集耗时。
共享控制成本按实际完整/轻量采集对保存，不按可重复使用的实验点名称覆盖。恢复时复用原始采集，
所需操作成本由当前导出实现重新生成，不因文件存在而跳过。导出完成后原子替换对应成本文件；失败保留原文件，
但本次准备仍报错，不将旧文件当作本次成功结果。任一采集改变后使用另一配对目录，旧配对保留；
成功导出后才更新组内的操作引用。重新导出不启动设备采集，也不增加采集账本用量。
旧位置的已声明成本仍可使用；重新进入自动准备时使用采集对目录，可能重做派生计算，但不因此重采设备数据。
采集命令成功后若结果处理抛出异常，共享账本会将本次标为 `invalid_capture_output`，不能作为成功阶段复用；
命令尚未完成时的异常标为 `interrupted`，已有超时、失败或清理不完整状态不被后续异常覆盖。

prepare_wall_seconds 包含本次准备、采集、构模和预测，不能再加一次分项耗时。
各采集账本是累计用量；旧资产首次采集成本可能未知，不能据此宣称完整冷启动成本。

## 7. 单独构模、预测与结果

已有观测可独立重建：

~~~bash
scripts/model.sh build-hicache-model --group <group.json>
~~~

它与 prepare 共用观测准入、成本选择和发布逻辑，不启动采集。每次按当前声明核对 base、共享校准和 CPU 校正绑定；
适用观测直接复用，缺失或绑定变化时重新解析已有 trace，而不是直接发布旧观测中的成本。
已声明但尚未准备的 CPU 校正会返回缺项，需先运行 prepare；不会悄悄退回未校正成本。
`--output-dir` 仅改变模型输出位置，观测和输入检查结果仍保存在组目录。缺成本时退出码为 2。
手工重建不会清空旧 group_summary；该摘要描述上次 prepare，不证明新模型已执行。

单独预测：

~~~bash
scripts/model.sh predict-hicache \
  --source-manifest <base/profile_manifest.json> \
  --target-config <target.json> \
  --hicache-io-model <hicache_io_model.json> \
  --cpu-service-cost <optional-same-source-cpu_service.json> \
  --output-dir <prediction-output>
~~~

没有 CPU 修正时省略该参数。source、target、CPU 文件参数可重复传入。
默认失败后停止启动后续任务，已经启动的任务允许结束；`--continue-on-error` 可继续其余目标。
任务数由明确选择的 source 与 target 组合决定。验证代表格时只传对应输入，不再支持按顺序截断的 `--max-predictions`。

常用结果：

| 文件/字段 | 用途 |
| --- | --- |
| group_summary.json | 准备阶段、预算用量、停止原因 |
| model_inputs.json | 完整成本输入检查及缺口 |
| model_build_summary.json | 公式、参数来源、观测范围及限制 |
| preflight_summary.json | 每份 source 的 trace/事实错误 |
| workflow_summary.json → prediction.cells | 每格输入身份、执行状态、http_e2e_us、成本覆盖和近似项 |
| model_runs/&lt;cell&gt;/run_summary.json | 详细执行结果与 HTTP 时间 |
| model_runs/&lt;cell&gt;/runner_config.json | 单格复现输入 |

group_summary 的 prediction 只保留组级汇总和成本缺口，逐格结果统一读取 prediction_summary 指向的
workflow_summary.json，不在两个文件中重复保存。各阶段不再输出恒定为空的 target_inputs、target_score_inputs
或恒为 false 的 accuracy_verified；数据隔离依靠输入准入和独立执行路径，精度结论来自单独评分。

预测每完成一格就更新 workflow_summary；中途停止时，已报告完成的结果仍可查看，其余格保留未完成和 null 时间。
这不是预测缓存：再次执行仍运行当前代码，不根据旧报告跳过格子。组级状态在整组返回后汇总，运行中查看逐格报告。

预测结束后，终端逐目标显示预测 HTTP 耗时（秒）；没有结果时显示 `not predicted` 和缺口或停止原因，
不把缺失时间记为零。这是预测值，不是 target 实测值；是否使用外推、保留了哪些 base 等待，以及成本覆盖限制，
仍需结合终端指向的 workflow_summary.json 查看，命令成功不代表精度验收通过。

现行逐格身份统一记录在 `prediction.cells`：`model_run_id`、`source_manifest`、`source_config_id`、
`target_config`、`target_hicache`、`workload_id`。执行前先保存未完成的任务，执行结束后更新同一份摘要；
结果目录由 `model_runs/<model_run_id>` 确定，不另存任务索引，也不向 C++ 的 run_summary.json 回写 prediction 字段。
评分先核对组摘要的完成状态，再读取对应 C++ 执行结果，最后才打开 target 数据。
历史结果原有的任务索引与逐格身份仍可读取，静态 oracle-cost 回放不受影响；不迁移或删除历史数据。

失败或未执行格的预测时间为 null，不按零计。cost_coverage=partial 表示估计仍有局限，不等于没有预测值，
也不是已计算出的逐操作成本覆盖率。
损坏的运行摘要直接报错，不把错误类型的内容替换为空对象后伪装成普通未执行结果。
EXECUTED 表示事实及操作已完成执行，不证明结构与 target 一致，更不证明精度达标。

命令成功与预测完成分开报告：顶层 model_run_error_count 统计命令失败，prediction.completed_count
统计 DAG 完整执行。未执行原因（包括 dry-run）见各格 blockers；不再维护 handled、runnable、usable、
skipped 四份顶层计数，避免把命令成功数误解为可用预测数。

完整 HTTP 使用 http_client.e2e_us；simulated_e2e_us 和 full_graph_e2e_us 可包含请求结束后的后台工作，不能混用。
source_observations_status=not_requested 只表示默认未额外导出观测，不表示没有成本依据。

默认 `--diagnostics off` 使用 Release，仅保留紧凑摘要、失败日志和复现输入。
`--diagnostics full` 使用 validation 构建，保留更多明细和成功日志，不改变模型。
当前执行式预测没有完整组件归属和 target 操作映射，不能据此使用历史 oracle 回放。

## 8. 独立评分与历史回放

~~~bash
scripts/model.sh evaluate-hicache \
  --prediction-dir <completed-predictions> \
  --profile-run-dir <target-profile-suite> \
  --output-dir <separate-score-output>
~~~

可重复传入 prediction-dir 评分多个 base，也可用 target-profile-manifest 精确指定 target。
对执行式预测评估正常运行耗时，追加 `--normal-http`，用重复的 `--target-profile-manifest`
传入关闭 profiling、channels 和 TIMING 探针的正常运行。需同时提供各 base/workload 的正常测量，
作为“不改变 base 耗时”的比较基线；不能用带采集的 base 墙钟代替。
同一配置与 workload 的重复运行按正式窗口耗时取算术平均，结果保留报告路径、样本数和最小/最大值。
评分核对请求顺序、失败请求及已报告的 token 回放失败，不排除 gap、未归属成本或计算阶段。
不加该参数时仍使用单份 target profile，重复身份会报歧义；历史静态 oracle 诊断保持此路径。
所有选中预测完成后才读取 target；输出必须与预测目录分离。
评分不采集、不重预测、不改模型，也不触发 refit。

完整 E2E 对比预测 HTTP 时间与 target 正式 HTTP 窗口，不排除任何成本，
不以 target DAG 回放或预测图后台终点为真值。报告总面板、逐 base、self 和 base-wall 参照；
缺失格不从验收中删除。具体阈值及当前成绩统一见[验证说明](validation/hicache_validation.md)。

普通评分不再生成历史逐组件结构、phase 成本、delta 和去 gap 汇总，也不为这些报告构建 target DAG。
`gate_summary.json` 的 `acceptance_scope=full_http_formal_window`；PASS 只表示完整 HTTP 与 base-wall
门槛通过，不证明成本覆盖完整或各组件准确。成本覆盖限制继续保留在逐格执行结果中。
历史细分报告及其数据不删除；不把停止输出的指标记为零或视为通过。

### 显式静态成本回放

静态成本回放仅用于诊断。在 validation runner 的 `cpp_trace_graph` 中设置
`hicache_static_replay: true`，可以先用当前规则生成不注入 target 成本的静态对照；
Release 不接受该选项，正常 HiCache 预测仍采用执行式路径。随后成本回放共用正式 runner 的
manifest、窗口和源 CPU 校正，不能漏掉 CPU 校正后再声称“只替换了目标成本”。
这不保证旧静态资产相容，也不把静态诊断当作当前执行式预测的精度证据。
静态标量控制成本须显式提供，不能直接拿执行式模型的细粒度控制文件替代。
目前，包含无源坐标的重建 gap 的静态图不能再叠加源 CPU 校正；这种输入会报错。
不带 CPU 校正的静态诊断必须从一开始就使用同一未校正对照，不能中途丢掉校正项以通过恒等性检查，
也不能将其耗时当作正常 HTTP 预测。
显式 `model_summary.json` 中的 `hicache_dag_patch.target_effects` 保留严格配对所需的目标工作量、
操作身份和资源顺序，直接来自模型计划。它不是重新执行一套缓存逻辑，也不恢复已删的细分评分表。
其中操作数量指实际传输服务次数；零载荷终止控制仍在 I/O 成本记录中单独计费。

相容的静态预测资产可显式使用 `--oracle-cost-replay`，前提是已有完整操作映射和诊断数据。
先验证相同预测成本回放，再执行五种真实 target 成本替换；结果只用于诊断，不计入预测精度或回写参数。
`--oracle-max-runs 1` 表示每个 base 选一个 cell，每格包含一次基准和五种成本变体。
只选择结构严格匹配的格子；没有合适输入时报告 `NOT_RUN` 及排除原因，不填零、不影响已完成的独立评分。
现行执行式预测缺少这套完整映射，不能仅开启 diagnostics full 就获得静态回放能力。

诊断摘要保留必要的目标操作计划、patch 成本、source attribution、图规模、ownership 和失败原因。
同成本恒等性比较计时、图规模及归属汇总，不声称比较逐段关键路径或证明完整 target 图同构。
保留去 gap 的诊断时长，不再生成逐家族/逐请求分段、分项 WAPE/P90 或另一套总 gate。
这不改变正式预测对 residual gap 的保留。

09-12 旧聚合 Prefetch 参数不被当前分阶段解析器接受；旧结果只作历史证据，不恢复旧规则或转换参数冒充复现。
09-27 已在相容的 C3→C5/W1 输入完成严格配对及真实成本回放，具体证据与失败范围见
[验证说明](validation/hicache_validation.md#现行静态回放的已验证范围)，不能据此承诺所有静态输入都可用。

## 9. 开发边界与实现位置

C++ 正式入口是 cli/module_pipeline.cpp → runtime::execute_hicache_window。
target 配置与执行规则生成操作，成本估计提供耗时；不得从 target trace 复制结构。
缓存状态、完成事件与 DAG 使用同一 service 时钟，避免用两套成本判断 timeout。
预取检查与重试由执行式 DAG 生成；通信采用同一组观测时序构建入口、worker 与返回依赖，
不再保留测试专用的标量预取循环/通信回放。历史静态 DAG patch 和显式 oracle-cost 诊断仍保留。

TraceGraph 的头文件统一位于 `include/markov/trace_graph/`，与 `src/` 的实现按模块对应；
内部辅助头也遵守这一布局，位置变化不表示它成为公开 API。项目不再维护独立测试文件和测试夹具。
引用统一写为 `markov/trace_graph/...`，不通过相对路径引用 src 内部文件。

trace 读取器与事件参数查询共用 `include/markov/trace_graph/json_scan.hpp` 定位 JSON 片段，保留共享缓冲区和延迟解析，
不为简化实现而构造整份 trace 的 JSON 对象。转义字符串由现有 JSON 库解码，保证按需查询与完整
参数展开对 Unicode 的解释一致；跨采集通道合并参数时保留接收事件的默认 pid/tid。

Prefill 与 Decode 的设备、通信和提交 CPU 观测共用节点成本结构及统计逻辑；
Prefill 的普通计算与注意力仍分别保存。诊断归属与输出复用同一份 phase 观测，
不重复解析；对外观测字段、成本公式和历史静态回放接口不变。

维护时必须保留：

- source facts 身份与可执行 DAG 节点身份分开；FutureDag 不回滚已开始工作。
- 写入、容量分配与回载通过 replay 的统一身份索引查找事实；decode 派生事实入表后先重建索引，再绑定执行边界。
- 物理完成、前台可见和引用回收分开；跨 rank 确认只消费本轮入口采样到的前缀。
- 实际读取、拷贝和发布量分开；取消不能抹掉已经发生的 I/O。
- 线程、worker FIFO、stream/event 与资源 lane 表达顺序和重叠；不能用总耗时替代依赖。
- HiCache 只替换明确归属的 CPU 区间；未测量 gap 与 CPU 服务成本分开，嵌套区间不能重复收费。
- Prefill/Decode 工作量来自目标缓存状态，按 source 算子份额投影成本；缺少算子/层依赖仍报告限制。

gap-excluded 是历史组件组合关键路径，排除 residual gap 和未归属成本，不是完整墙钟仅减 CPU 空白。
component ownership 和 formal window 按执行语义定义，不按配置名或 cell 硬编码。

| 位置 | 职责 |
| --- | --- |
| modeling_workflow/group.py、prepare.py | 组输入、预算和宿主机编排 |
| base_capture.py、cpu_capture.py、capture.py、physical_capture.py | 各类采集及恢复 |
| observations.py、fixed_calibration.py | 观测提取和共享实验选择 |
| io_model_builder.py、phase_calibration.py | 成本选择、缺口报告和模型发布；检查与发布使用同一次估计 |
| prediction/、execution/ | 预测任务与结果 |
| evaluation/、validations/ | 独立评分和历史诊断 |
| trace_graph/src/modules/hicache/model/、runtime/ | 缓存执行规则、目标操作及运行时依赖 |
| trace_graph/src/modules/hicache/patch/、src/simulation/ | 图变换、成本落图与仿真 |

预测执行器只接收本次计划并返回结果，不跨调用保存状态。串行和并行共用有界任务队列；
停止策略在补发任务前检查整批已完成结果，已启动任务仍正常收尾，未启动格显式标记。

完整约束见[项目约束](project_constraints.md)。日志是开发记录，不是当前实现的替代说明。

## 10. 容器与构建

SGLang 推理、profiling 和设备校准使用 sglang；KTransformers 采集使用 ktransformers；
观测提取、DAG、构模、预测和评分使用 modeling。公开脚本负责调度。

~~~bash
scripts/run.sh modeling -- bash -lc \
  'cmake -S src/modeling/trace_graph -B build/modeling/trace_graph-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DTRACE_GRAPH_DEBUG=OFF && cmake --build build/modeling/trace_graph-release --target trace_graph -j2'
~~~

详细诊断和历史静态回放需要 TRACE_GRAPH_DEBUG=ON 的 validation 构建。
历史静态 patch、边界/落图检查和 target observed carrier 只在诊断构建中编译；
Release 与 validation 的正式预测仍共用执行式模型。相关检查命令见仓库 AGENTS.md。
