# HiCache 数值模型

本文说明当前唯一的 HiCache I/O 与控制开销、以及 Prefill/Decode cost model。执行流程见
`docs/modeling_development.md`，实测结果见 `docs/validation/hicache_validation.md`。

### 共享 CPU 操作校准的采集开销

共享校准也必须区分采集耗时和正常执行成本。先对同一份独立 workload 做完整采集与轻量采集，
再用 `scripts/model.sh prepare-cpu-service --correct-recorder` 生成绑定到完整采集的 CPU 修正数据。
`scripts/model.sh export-hicache-operation-costs` 将该修正应用到校准 DAG，再导出 load index、
load submission 和 layer wait 的共享操作成本；不能将原始 profiler 间隔直接作为正常执行成本。
需要预取等待/停止依据时使用 `--operation prefetch_wait`；策略读取所选源配置，不由 target 指定。
它复用执行器的分支观测，按源时间顺序选择第一个完整的活跃请求，目标重试次数仍由预测状态决定。
同时保留每个 rank 首次观察到的本地提前返回成本；这条分支在策略判断之前执行，不能借活跃等待成本代替。
缺少预取查询依据时，可用 `--operation prefetch_query` 从独立 trace 导出首个完整查询，
供整组复用。查询局部区间仍保留观测墙钟包络；不能把配对修正文件存在等同于全部开销已分离。
只有执行事实中存在预取候选时才要求查询成本；最终命中为空也需要查询，但没有候选时不读取查询校准或触发补采。
缺少 base 查询片段时现已报告可补采的查询成本需求。先从组内已声明、经环境准入且有配对 CPU 修正的
预取等待校准来源重新导出查询成本；无此来源再使用共享三阶段实验。导出失败不发布索引，也不静默启动
新采集绕过错误。成功后才重建组模型，一次准备不会反复重采同一需求。已有来源的真实数据复用已验证，
无来源时的新增设备补采分支尚待验收。
独立操作成本导出与 I/O 观测共用全部受控请求的时间范围，包括准备请求，但不包含服务器启动。
这是校准取数范围，不改变 base 预测或 target 评分的正式窗口；缺少本地返回成本时，执行报告对应补采需求。
可重复 `--operation` 只导出所缺操作；不指定时保持 load index、load submission、layer wait 三类。
多操作导出共用一次 trace 读取、构图及 CPU 修正加载；各操作在独立图副本上提取，不共享边界修改。
后续操作失败时，前面已完整提取并通过整理的成本文件仍保留，失败操作不发布最终文件。
导出器构建目标为 `hicache_calibration_export`，默认读取 Release 构建目录，也可用 `--exporter` 指定路径。
命令参数见 `--help`。

开启 `--diagnostics full` 时，load index 与 load submission 的中间审计文件共用 `nodes`/`edges` 表示。
节点 `id` 是当前数组下标，`service_us` 是 CPU 或设备的执行成本，`residual_us` 是 CPU 残余间隔，
`dispatch_us` 是已观测的 worker 派发延迟；`submission` 与边保留任务关联。
这只是导出器与汇总器之间的诊断格式，最终共享成本文件及其模型接口不变。
历史审计文件保留作排查资料，不要求新汇总器兼容旧中间字段；需要重新汇总时从原 manifest 和 CPU 修正文件重新导出。

这一步只修正有配对测量依据的 CPU 区间。未测量的间隔、工作线程派发和设备成本仍保留，
不是将 residual gap 清零，也不依据 target 的 E2E 误差调整参数。区间内的开销分摊不是逐条指令的归因；
超出校准所覆盖的操作规模仍属于外推，应单独记录不确定性。

CPU 配对先在原始采集输入处检查请求/rank 覆盖、步骤编号和时钟，再比较两次采集的实际工作。
内部 forward、input、layer 和 scheduler 处理复用这份已建立的合同，不逐层重新检查同一身份；
两次采集的同步次数、子操作、层工作或边界不同，仍不能直接配对或用零补齐。

操作示例（两份 manifest 必须来自同一独立校准输入的完整/轻量配对）：

```bash
scripts/run.sh modeling -- cmake --build build/modeling/trace_graph-release --target hicache_calibration_export -j2
scripts/model.sh prepare-cpu-service \
  --profile-manifest <full-manifest> --light-manifest <light-manifest> \
  --tp-size 2 --correct-recorder --output-dir <cpu-output>
scripts/model.sh export-hicache-operation-costs \
  --profile-manifest <full-manifest> --cpu-service-cost <cpu-output>/cpu_service.json \
  --page-size <measured-page-size> --output-dir <operation-output>
```

将输出的 `load_index.json`、`load_submission.json`、`layer_wait.json` 分别接入模型的
`control_calibrations` 同名字段，同一实验组共享这些文件。默认只保留最终成本文件；需要逐项提取明细时，
加 `--diagnostics full` 保留 `*_audit.json`。这些明细不参与构模，不额外复制完整操作程序。
每个操作在临时目录完成提取和成本整理后才发布最终文件；失败不会留下可被恢复流程误认作成功的半成品。
重复导出使用当前实现计算所选操作，并在成功后原子替换其成本文件；失败保留此前完整文件，但命令仍失败。
组恢复复用原始测量，不以已有导出文件作为跳过当前计算的依据；未选操作及原始采集保持不变。
同组内复用按实际 template/config_specs 内容、成功状态和采集阶段匹配，不再额外要求账本中的
校准点名称相同；轻量回放仍须绑定对应完整采集的 token bundle。名称不是成本适用性的证明，
这也不扩大到任意跨环境或跨实验复用。
DMA 原始补采按方向覆盖关系复用：测过 H2D 和 D2H 的同一实验可以满足仅缺其中一个方向的需求，
反过来不行。设备/NUMA、页大小、操作规模、重复设置和报告来源仍须匹配，不把未测方向视为已有成本。
`prefetch_wait.json` 按其中的 `source_policy` 放入组级 `control_calibrations.prefetch_wait`；此操作只保留最终文件，不额外复制一份 audit。
CPU 校正只覆盖有配对依据的区间，调度间隔和 collective 时间仍带来源条件限制。
`--operation write_host` 导出写传输及其淘汰控制片段，保留资源角色、提交/等待依赖、CPU 与残余间隔。
输出 `write_host.json` 按源页大小和写策略放入组级 `control_calibrations.write_host`，同样不复制 audit。
独立淘汰控制片段不会覆盖 base。运行时先检查操作、线程、依赖和规模是否适用，再优先选 base；
纯 CPU 控制沿用同阶段均值或显式的 base 阶段外推，缺少适用 base 才用共享测量。这个估计不能代替尚未测量的“跳过锁定候选”等不同基础操作。
跨页使用仍须通过运行时 FAST2D 几何检查；有适用 base 时优先使用 base。导出成功不等于整组执行分支已经齐全，当前自动需求检查与补采连接尚未完成。
普通设备释放使用 `--operation release_host`，输出放入组级 `control_calibrations.release_host`。
它只提取普通释放，不要求同一校准出现写回操作；保留 CPU、设备、worker、残余间隔及依赖，
拒绝备份完成等待和缺少同源 CPU 校正的输入。不能用备份释放片段代替普通释放。
当前仍按 rank/资源适用性选择邻近规模的整段操作，尚不是分配器与外围元数据处理各自的成本模型；
跨规模复用是不确定的外推，不代表已验证精度。历史未校正的普通释放文件不能用于这条正式路径。
这些文件不能替换设备带宽或 Prefill/Decode 校准，也不能据此声称其他控制路径已完成采集开销校正。

## 1. 模型回答什么

给定一个 base profile 和一个只含 HiCache 配置的 target，C++ 先由 source facts 与 target policy 产生 I/O 机会和需求，
再用同一份 service cost 推进完成、timeout、可见性与资源顺序，在共享执行时钟上生成 Prefill/Decode 工作量和 DAG 变换：

```text
source DAG + target config
  -> target effect/demand plan
  -> HiCache I/O/control cost + completion/resource schedule
  -> executed/visible work plan
  -> Prefill/Decode cost
  -> DAG dependencies and costs
  -> full HTTP request-chain simulation
  -> independent normal-runtime scoring
```

cost model 不根据 target 标签选择节点或工作量，也不从 target 残差反推结构；但 service duration 是 deadline、完成批次和发布页数的
真实因果输入，因此会影响时间相关的可见性与依赖。target profile、target E2E 和 target 分项 cost 只在预测全部完成后用于评分。

当前组件为：

- HiCache I/O/control（内部 scope ID 为 `hicache_direct`）：Prefetch、Load、device-to-host（D2H）、host-to-storage（H2S）及其固有 control；
- `prefill` 和 `decode`：由 target cache reuse 改变的计算工作；
- residual CPU gap：不作为 I/O/计算拟合参数；当前完整预测仍保留部分 source 等待，尚未准确描述它随 target 配置变化的规律。完整 E2E 真值不剔除这些耗时。

## 2. 三类数据

| 数据 | 提供什么 | 不提供什么 |
| --- | --- | --- |
| 平台物理校准 | KV 字节几何、DMA/存储基础曲线、storage batch 上限、resource lane | framework runtime scale、phase cost、target 标签 |
| 独立小型校准 | 补充 base 无法辨识的成本参数、控制路径和计算样本 | target 真值、逐格误差修正 |
| 一个 base 的 profiles | source DAG、真实请求工作、优先采用的 I/O 成本依据和该 base 的计算观测 | target cost correction |

同一模型、TP、I/O backend、内存布局和资源环境可以共享适用的独立校准。不同 base 可以从自己的实测得到不同 I/O 参数，
但不能把另一个 base 的数据伪装成共享独立测量。同一 base 组内的全部 target 共用这些成本依据，不按格子调参。

目前 I/O service 的就绪检查与构模已共用 base 优先的参数选择：base 足以辨识时不用独立样本替换；不足时才补充。
新建存储写入需要至少两个不同的每次调用字节规模来区分启动开销和带宽，但不再强制两个页大小端点。
base 的一个规模可与独立测量的另一个规模组合；有相同规模时优先保留 base，重复样本不靠数量压过 base。
单页参数在域外沿既有物理公式投影并明确标记假设，并不代表已验证所有页大小。

若 base 和已提供的共享 workload 都没有某项服务的可用依据，但已声明的独立物理测量包含该项参数，
直接使用该基础服务公式，不再仅为估计运行时倍率而强制重采 workload。这适用于分阶段 Prefetch、
双向 DMA，以及存储 existing/new 写入；CPU 控制和计算阶段仍分别判断，不能借物理带宽补齐。
此时摘要将来源标为 `independent_physical`，明确未估计负载竞争的影响。
模型中的倍率 1 表示“不追加修正”，不是测得运行时与独立实验完全相同；其曲线参考点也不是新的观测。
独立报告必须有原始测量来源，并明确未使用 target trace 或 E2E；缺阶段、缺参数或来源不明仍报告缺口。

部署几何仍须明确，但首次构模不要求已有完整物理测量报告；base 能辨识的服务不强制独立重测。
CPU control 与 phase 已接入 base 优先选择，但写入、淘汰等执行分支的需求检查尚不完整，
不能把“构模成功”当作全部 target 都有成本依据。

## 3. 按缺口选择共享实验

先检查 base 实测和可推导的参数，再根据剩余缺口选择实验。生成器使用 base 页大小和合法输入 token：
仅缺阶段参数时构造常驻前缀及不同长度的续写；普通释放使用超过设备缓存容量的首次请求；
活跃预取控制使用写出和两次恢复，并选择相应等待或停止策略，不再追加用于旧写入验证的重写尾段。
I/O 服务使用独立原语测量，不为此构造通用压力 workload。
实验由整组共享，不读取 target 的真实耗时或按格子拟合。

这些是有限的参数化实验，不是任意缺口都能自动解决。一个实验成功后仍缺依据时会报告不足，
不靠原样重复无限补采。预取控制校准需要 token 记录、完整回放、轻量回放，再做 CPU 校正和导出；
token 记录不启用 profiler，也不作为成本样本，状态检查仍保留。成本由后续完整/轻量回放提供。
三次运行及失败重试都计入预算。现有 `fixed_calibration` 内部名称不代表必须采两端点或固定重复次数。

`control_models` 中的 commit、state-check 和 load admission 标量用于历史静态图变换，不参与现行执行式预测。
正式 Python 构模和预测不再生成、校验或投影这套标量；带非空 `control_models` 的旧模型不能作为
现行预测输入，需重新构模。已有空字段模型仍可读取，新输出不再保留空占位字段。
历史静态 oracle 回放直接读取原预测的 C++ 配置，保留其中的标量及原有成本缺失检查；
不经过上述 Python 模型投影，也不使用新模型替代历史回放所需的原模型。
现行执行式预测仍使用操作级 CPU 成本与同源校正，不能将上述解耦理解为不再建模控制开销。

服务构模在选定证据后直接生成该服务的参数，不再把倍率、新页写入参数分散保存后重新拼装。
支持域统计与参数来源使用同一次证据选择返回的观测；base 优先、独立测量补缺及缺失不填零的规则不变。

物理报告在组输入处必须明确声明未使用 target workload trace 和 target E2E；标记缺失或不是 false 均拒绝。
该限制在任何物理成本投影之前执行，不仅约束独立参数兜底。部署元数据不冒充独立测量，仍需实际测量来源才能提供独立成本。

新页写入的启动开销与带宽无法由 base 及已有独立参数辨识时，优先在已有物理报告上补充两个操作规模的独立写入，
而不是为这两个系数运行完整推理 workload。报告只更新新页写入参数，保留覆盖写、DMA、预取和原始测量来源。
这一通路尚无本轮真实设备补采结果；首次准备已不要求整包采集，但还不能宣称所有服务都已实现最小补采。

执行预测遇到 `execution_control/release_regular` 时，组流程可选择普通释放的共享实验：沿用 base 的 token
和页大小，以选择性写穿运行互不相同的首次请求，使工作量超过设备缓存容量。不要求恢复或改写存储中的数据，
每请求只输出一个 token，在 prefill 结束时插入缓存一次，避免 unfinished/finished 两次插入
触发选择性备份门槛；种子检查要求设备驻留且 host 无备份。其他需要 Decode 观测的校准仍保留两个输出 token。
也不读取 target trace。实验仍有准备阶段、设备驻留检查与明确正式窗口；随后完成 token 记录、完整/轻量回放、
CPU 校正和 `release_host` 导出，再重建组成本并恢复预测。只有声明预算内的采集才执行。
同一缺口每次组运行只尝试一个共享实验；已提供普通释放依据仍失败时不重复采集。
锁定候选与普通释放缺口可以先后处理，但不能据此宣称写入/淘汰的所有合法分支均可自动补齐。

## 4. 变量与参数

### 4.1 预测时已知的变量

这些变量来自 target 状态回放，而不是回归标签：

- `page_size`、每 rank 的 `page_bytes`；
- operation、page、byte 数量；
- Prefetch 的 storage service batch 数；
- H2S 每个 batch 的 existing/new page 数；
- Prefill 的 new/context tokens 和 attention token pairs；
- Decode 的 context tokens、iteration 数和 effective pages；
- target policy 决定是否经过一次 Prefetch state check。

### 4.2 需要估计的参数

| 参数 | 来源 | 估计方法 |
| --- | --- | --- |
| Load/D2H 启动成本与带宽 | 优先 base，再检查已有共享 workload；仍不能辨识时使用独立 DMA 曲线 | 按调用数归一化，两个规模确定非负启动成本和正带宽 |
| Prefetch runtime scale；未直接辨识的 Load/D2H runtime scale | 优先 base；无法辨识时补独立校准 | 每个页大小、每次 profile 的 observed/physical 总时间比，再取中位数 |
| H2S existing runtime scale | 优先 base 的纯 existing batch | 同上；缺少时才用独立观测 |
| H2S new setup 与 bandwidth | 优先 base，再用已有共享 workload；仍不能辨识时复用已声明的独立物理参数 | 观测按调用数归一化，由两个规模确定直线；物理补充直接使用已测系数，不再按 target 修正 |
| 执行式 CPU 控制操作 | 优先适用的 base 片段，缺少时用同源校正的共享操作测量 | 按操作、资源和规模选取；部分整段复用仍是近似 |
| 静态 Prefetch commit/state-check、Load admission 标量 | 静态诊断显式提供的 base 或独立成本 | 历史估计取观测中位数；不由当前正式构模生成 |
| Prefill/Decode 参数 | 优先 base；缺少独立变量或 token 锚点时补共享观测 | 实测 token 曲线或简单非负线性模型 |

不存在 config ID、workload ID、cell ID、target residual、逐格倍率、growth correction 或 target page 枚举参数。

H2S 的新旧页由 trace 中各批次的实际页数决定，不通过物理模型的预测时间判断。
其中纯新页的启动开销和带宽直接来自观测到的调用次数、字节数与耗时，不需要先有一条独立 new-write 物理曲线。
工作量提取与物理成本投影分开：补采规划直接检查 base 的实测工作量，不构造空物理模型，也不为未估计成本填零。
当前构模允许其他服务缺席，由目标实际执行检查是否需要；缺席不表示零成本。
物理报告缺少某项服务参数时，成本检查分别报告该项缺口，继续识别其他服务的 base 依据。
纯新页写入不依赖覆盖写曲线；混合新旧页不能在缺少覆盖写成本时硬拆分。
模型发布保留已识别的服务，缺少的服务参数单列在 model_inputs.json 和 model_build_summary.json 的 service_gaps。
实际执行请求该服务时，通过统一成本缺口报告触发组级共享补采；缺口未解决的预测保留失败和 null HTTP 时间。
存储写入的新旧页参数分别保留：纯新页、纯已有页只要求各自的参数，混合批次缺一项就报告该项缺口，不计零成本。
整类存储成本缺席时，也先按实际首批次报告新页或已有页需求；后续批次可能暴露另一个缺口，不能把首次报告视为全部需求。
CPU 和 phase 的前置就绪要求仍保留。
当前预取阶段、覆盖写、新页写入和 DMA 支持已有报告上的独立补充，
不会因为预取参数齐全就把整个物理报告视为完整，也不会自动退回整包重采。
DMA 补充按成本检查报告的缺失方向生成采样任务，H2D 与 D2H 可以分别测量；
base 已能辨识的方向不再随另一个方向重测。预算、实际操作序列和报告投影使用同一方向集合，
合并时仅填缺失曲线，保留已有曲线。单方向仍测两个调用规模来区分启动成本与字节成本。
首次没有物理报告时，先生成仅含部署信息的 platform_inputs.json，再辨识 base 中的成本并安排缺项补充。
部署信息不作为实测参数来源，空服务表不代表零成本。当前路径仍限定已支持的设备、存储和声明的页大小域。
局部补充已连接不代表任意组的最小补采已经完成。

组内成本检查统一辨识 base 的纯新页样本，能辨识时不安排独立新页写入实验，也不预留其逻辑 I/O 预算。
采集规划不再重复拟合 base；它只为已报告的缺项生成测量参数。物理采集统一按具体服务执行，
不再维护手动一次采全及 `--skip-new-write` 分支；多项缺口由组流程串行采样并合并报告，已有完整报告仍可复用。
新页参数辨识不足时沿用上述独立参数补充规则，不改写 base 已能确定的系数。

存储采样使用模型的 KV dtype 和 page-first 布局，每个 rank worker 只创建自己的文件后端。
写入复用框架的页提取和批量写入方法，预热读取的目标缓冲也由框架方法分配；不再维护无模型几何的纯字节模拟分支。
采集入口与各 rank 共用同一份采样计划。已有键在每个采样点预填充一次，重复测量后清理；新键在每次测量后清理。
两者共用样本记录规则，尝试传输字节量、各 rank 服务时间之和与整次调用墙钟分别记录，不能互相替代。
这不等于推理负载下的资源竞争，也不改变已有报告中的测量值或自动重建成本参数。

## 5. HiCache I/O service 公式

以下时间单位为微秒，`B` 为 byte 数，`P` 为 page 数，`G` 为每 token 每 rank 的 KV bytes。

### 5.1 Prefetch

```text
page_bytes = target_page_size * G
physical = P * (before_copy_per_page + page_bytes * before_copy_per_byte)
         + copied_pages * (copy_publish_per_page + page_bytes * copy_publish_per_byte)
         + service_calls * return_per_operation + P * return_per_page
predicted = physical * runtime_scale(page_bytes)
```

`P` 是实际执行读取的页数，`copied_pages` 是实际拷贝页数，不能用最终可见或完成的 token 数代替；取消预取也可能已做了这些工作。
`service_calls` 来自 storage batches。各阶段系数来自平台测量，runtime scale 优先由 base 估计。
正式构模要求分阶段测量，不再回退到总耗时公式；旧报告若只有总耗时，会报告阶段证据缺口。
正式模型加载也只接受 `stages` 与运行倍率，不再接受旧总带宽形式或忽略模型中的冗余总成本字段。
Python 构模和 C++ 直接配置入口执行相同规则；带 stages 又附带旧总成本字段的混合输入也会拒绝。
I/O 观测范围统一保存在 `model_build_summary.json` 的 `io_coverage` 中，由所选观测生成，
不由倍率端点补造。它是范围说明，不参与参数选择或预测，因此不再复制进数值模型并作为加载门槛。
只有独立物理参数、没有所选 workload I/O 观测时，`io_coverage.page_bytes` 为 null，调用范围为空；
物理测量来源另列在 `platform_measurement` 中，不把单位倍率当作负载下覆盖证据。
单独使用模型文件不意味着已取得观测范围证明；需要查看来源和限制时，应同时保留构模摘要。
原测量缺少分阶段信息仍须补充证据，不能靠移除报告字段绕过参数缺口。历史静态回放及其数据保留。
物理采集同样只保留分阶段预取测量，已删除会被覆盖的独立整段暖读测速及 `--isolated-repeats` 参数。
分阶段实验自身需要的缓存预热仍保留；旧原始测量和已生成报告不因此删除。
完整存储采集和只补预取阶段共用一个入口，按计划中的非空任务执行；一次采集中每 rank 只建立一个工作进程。
各实验清理自己的存储键，完成的测量在进入下一阶段前保存；失败时关闭整组工作进程，不发布完整报告。
阶段模型只保存六个阶段系数，不再附加另一套总启动开销、总每页开销和总带宽。
只有一个页大小时，测量只能确定该大小下的每页响应，不能分别证明固定项和字节项；字节项为零不是零成本，也不代表其他页大小不影响耗时。
因此跨页使用仍是未验证的外推。原始测量先于参数估计保存，拟合失败不发布完整报告，但保留测量供排查与重建。
物理采集入口在启动测量前检查输出覆盖权限与补采来源；内部发布层使用已准入的几何和观测，
不重复维护覆盖开关或用零代替缺失几何。阶段测量是否足以估计参数仍由估计器判断。

### 5.2 Load 与 D2H

base 在同一页大小下有两个不同的每次调用字节规模时，先直接估计：

```text
predicted = operations * setup_per_operation + B * 1e6 / bandwidth
```

估计方法与纯新页写入共用：按调用数归一化，每份 profile 对相同规模取中位数，再由两端规模确定直线。
负启动成本、非正带宽或只有一种规模都不算辨识成功，不裁剪系数来强行采用 base。
该路径不需要独立 DMA 曲线，写入模型的 runtime_scale 为 1，仅用于沿用统一执行公式。
在未观测页大小或调用规模上的外推仍须说明不确定性。

base 不能直接辨识时，先加入已准入的共享 workload 观测尝试同一公式；同一规模仍优先使用 base。
只有这些已有观测也不足，才使用独立曲线与可观测的运行时倍率：

```text
physical = B * 1e6 / physical_bandwidth(page_bytes)
         + operations * setup_per_operation(page_bytes)
predicted = physical * runtime_scale(page_bytes)
```

Load 和 D2H 分别有自己的物理 bandwidth 曲线和 runtime-scale 曲线，不共享 target 修正项。

### 5.3 H2S existing

已有 storage key 的读取/覆盖响应与单次 batch 深度有关：

```text
predicted_existing = sum_over_batches(
    existing_pages * page_bytes * 1e6
    / physical_existing_bandwidth(page_bytes, batch_pages)
) * existing_runtime_scale(page_bytes)
```

### 5.4 H2S new

创建新 storage operation 有一次 setup，再传输该 batch 的 bytes：

```text
predicted_new = sum_over_new_batches(
    setup_per_new_operation(page_bytes)
    + new_pages * page_bytes * 1e6 / new_bandwidth(page_bytes)
)
```

setup 和 bandwidth 优先由 base 中两个不同的每次调用字节规模确定；不足时才补充适用的独立测量。
同规模优先保留 base，不要求固定 workload 或两个页大小端点，也不按 target 残差再乘倍率。

端点间以 page bytes 的对数坐标插值；超出已声明物理域时使用最近端点并在 coverage 中显式暴露，而不是外推新的经验曲线。

## 6. HiCache control、timeout 与资源

现行执行式路径生成目标 CPU 操作和依赖，再从适用 base 或共享校准取得操作成本。
查询、加载提交、层等待、写入和释放并非统一的一个常数；普通释放的近邻规模片段仍有外推限制。
是否执行由 target 状态和规则决定，缺成本不会改成不执行，也不能计零。

以下标量规则仅用于保留的静态诊断，不是现行执行式 CPU 成本模型：

- Prefetch 始终支付一次 terminal commit；`wait_complete` 和 `timeout` 另支付一次 state check，`best_effort` 不支付；
- Load 支付一次 admission 固定成本；
- 静态 D2H/H2S 没有单列 owner control；该字段为 0 不表示执行式写入/释放没有 CPU 成本；
- 零载荷 Prefetch 只落 control，不伪造 service bytes；
- page/operation 计数仍由结构模型决定，control 系数不吸收 service 或 gap。

两条路径都用 resource lane 和 DAG dependency 表达 I/O 并发，不把各 operation wall time直接相加：storage read/write、H2D、D2H 按平台声明决定
是共享 lane 还是每 rank lane。状态回放和最终 DAG 都调用同一个 service 估计；不存在另一套 planning bandwidth。

Prefetch 另外区分三个时间概念：

- 实际执行的 service：决定设备成本和资源占用，即使最终 0 页可见也保留；
- 发布的完整 batch：决定哪些页能进入 host cache；
- 前台等待：`wait_complete` 依赖完整 service，`best_effort` 不等待，未完成的 `timeout` 只依赖从 enqueue 到 deadline 的策略门。

因此 timeout 后未完成的 I/O 可以继续占用后台资源，但不能把完整 service 时长强加给当前请求。策略门没有拟合系数，持续时间直接来自 target
配置的 timeout 规则。

### 6.1 淘汰时跳过锁定候选的 CPU 成本

淘汰循环可能弹出一个仍被请求占用的节点，检查锁后跳过它，继续找下一个候选。没有传输也仍有 CPU 工作。
当前可选模型为 `cost_us = a + b * log2(H)`：H 是预测状态中弹出前的候选数，不是目标 trace 的观测值；
a 对应固定判断开销，b 对应随堆高度增长的操作开销。共享模型字段分别为
`locked_candidate_us` 和 `locked_candidate_log2_heap_us`，同一组所有目标使用同一对系数，不按页大小或策略缩放。
未声明斜率时显式固定项保持常数语义；缺少整个模型时，实际出现该分支仍报告缺成本，不默认填零。

参数来自独立运行框架原始淘汰循环的 CPU 批量测量，使用真实节点对象。
基线使用相同候选集合但请求释放 0 token，只建堆而不遍历；活跃测量请求释放 1 token，遍历并跳过全部锁定候选。
两者线程 CPU 时间之差用于估计循环增量。单候选确定 a，最大候选集的总增量扣除 `H*a` 后，
除以 `sum(log2(k), k=1..H)` 确定 b；不能把整批平均值当作最大堆规模的一次操作。
其他堆规模及 NUMA 节点只作留出检查，不用 target 耗时调系数。预测时按目标实际跳过顺序插入 CPU 工作，
在线程内累计小数余量，避免逐次取整造成偏差。

这仍是待完整验收的近似：空闲 CPU 原语测量不证明带负载时完全一致，源控制模板也可能已包含部分跳过工作，
需要核查实际选中的模板，避免重复计入。运行通过或单个原语留出误差小，都不能替代整行、其他 base 和完整矩阵验证。

## 7. Prefill/Decode 模型

Phase 与 HiCache I/O/control 分开估计，但保存在同一个模型 JSON 中。

### 7.1 Prefill

```text
common kernel      = measured curve(new_tokens)
collective         = measured curve(new_tokens)
prefix attention   = a + b * new_tokens
                       + c * new_tokens * (context_tokens + new_tokens / 2)
```

曲线在相同 new-token 点先对样本取中位数。测量噪声造成后一个锚点更低时，只向前做单调夹取，不增加自由参数；锚点间线性插值。
`a/b/c` 用非负最小二乘求解，允许不需要的项自然为 0。

### 7.2 Decode

```text
effective_pages = ceil(context_tokens / min(target_page_size, base_kernel_page_tokens))
paged attention per iteration = a + b * context_tokens + c * effective_pages
collective per iteration      = observed median
```

target 的 decode iteration 数沿用 source 请求语义；非 paged kernel cost 和 CPU submit 保留 source 模板。CPU submit 单独诊断，不允许它吸收
HiCache I/O/control 或 residual gap。

若本组选定的 base workload 明确没有 Decode 迭代，不为不存在的工作补采 Decode 参数：
`decode_paged_attention`、`decode_collective` 和 Decode context 覆盖上下限保存为 `null`，不是零成本。
实际预测没有 Decode 时跳过对应成本投影；若执行需要 Decode 而模型没有参数，则报告 `phase/decode` 成本缺口。
有 Decode 迭代却缺少 kernel 测量仍是数据缺失，不能按“未执行”处理。

独立校准中具有相同请求、rank、token 和页大小工作量的重复测量先折叠成逻辑实验，避免重复次数改变权重。
参数优先来自所选 base；一个组有多少份 workload profile 不由矩阵实验计数限定，target profile 不参与估计。

## 8. “训练”和预测分别做什么

这里的“训练”不是通用机器学习：它是一次确定性的参数估计。

```text
构模：physical + fixed calibration + base profiles
    -> 中位数、两点直线、两条实测曲线、两个小型非负线性模型

预测：source DAG + target config + 已建模型
    -> target 工作量 -> 公式代入 -> DAG patch -> simulation
```

相同输入重复构模必须得到相同参数。统一成本入口拒绝重复 manifest 和 target 角色，不由各组件各自去重或覆盖。
真实重复测量使用不同 manifest，按各模型已声明的聚合规则处理。`model_build_summary.json` 记录每类参数的公式、
单位、manifest、有效样本、重复数、端点，以及物理测量的 backend/NUMA/CPU/计时范围。
构模不再对参与估计的服务样本重算 `input_reconstruction_wape`；参数来源和有效测量仍记录在摘要中。
阶段模型也不再生成 `phase.fit`：它只把参数代回参与估计的同一批样本，不是独立泛化验证。
就绪检查和构模保留相同的样本选择、参数求解、特征秩、来源、范围与不确定性；target 误差仍由独立评分计算。
历史报告中的该字段仅是输入重构误差，不是独立验证或 target 泛化精度；真实精度由独立评分流程报告。

构模摘要保存所选 base 与共享补充观测的 page-byte、各类 service-call byte 范围，以及端点钳制等外推行为说明。
阶段参数来源统一放在 `parameter_sources.phase`；`phase` 只记录观测数量与重复样本处理方式，
全部输入清单统一见 `observation_sources`，不再在阶段摘要重复保存。各参数实际选中的来源、特征范围和限制仍完整保留。
当前执行式预测尚未逐操作核对全部调用大小，不能把这些范围解读为 target 已通过覆盖验证；
不再保留无人调用的旧静态账本 domain_status 判断。范围信息不触发按 target 误差补采或调整倍率。
Prefill/Decode 的 coverage 不同：它参与阶段成本的外推判断，仍属于数值模型合同，保留读取与校验。

## 9. 计时与验收边界

**当前完整 E2E** 对比预测 HTTP 请求链和独立正常运行的正式窗口；评分不剔除 gap、未归属成本、Prefill 或 Decode。
同 base 的 profile/light 测量只校正采集开销，不从 target 真值中扣除误差。`EXECUTED` 只说明流程运行完成，
成本覆盖标记和精度验收须另行检查。

以下 gap-excluded scope 是历史组件实验和分项诊断口径，不是当前完整 E2E 的验收分母。

HiCache I/O/control 标签计 operation 自身的 service 和明确归属的 control CPU：

- DMA 使用 device-transfer 时钟；storage 当前使用函数墙钟代理，其中可能仍含函数内部调度等待，不能宣称为完全纯净的设备时间；
- 函数外已识别的 residual CPU gap、完整请求 wall time和随机到达空白不进入 HiCache I/O/control 参数；
- Prefill/Decode compute 分项计已归属 device compute/collective；组合 scope 还保留对应 submit CPU；
- gap-excluded scope 组合 HiCache I/O/control + Prefill + Decode 在预测 DAG 上的关键路径，保留必要因果/资源边；
- 未归入上述组件的 wrapper/probe 成本与 residual gap 被排除，不等于完整应用墙钟只减去所有 CPU 空白；
- formal window 由 workload 的语义起止点决定，不按 config/cell 写死时间边界。

默认评分只保存紧凑指标。oracle-cost replay 只有显式开启时才用 target observation 替换预测 cost，用于诊断操作级绑定和 cost
sensitivity；它不是完整 target 时序同构证明，不是预测成绩，也不能回写参数。
回放前先检查相同预测成本注入恒等性；control 输入是明确观测的 CPU 工作，不是轮询等待。
控制操作边界独立于 duration 数值存在，不能因 oracle control 为零而删除依赖节点。

09-12 之前的旧 60-cross 只作修复前参照，其结构检查范围较窄。09-12 严格评分的 HiCache I/O/控制
WAPE/p90/delta 为 5.186%/32.518%/8.121%，严格 gate 未通过。09-14 又完成新 workload、新配置及
TP=4 的十二格泛化实验；两批结果、严格结构口径和不抵消分项统一见
[验证与当前限制](validation/hicache_validation.md)，不能把旧 60 格当作新面板成绩。
这些历史结果暴露完整前缀匹配边界、短超时等待/可见性投影及成本泛化限制；尚缺当前实现上的关闭证据。
不能把旧缺陷描述直接当作当前代码结论，也不能根据 target 分数追加参数。
