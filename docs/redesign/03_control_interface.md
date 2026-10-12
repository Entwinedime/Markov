# 统一控制接口与执行合同

状态：S2 的 C++23 类型与 plan 已历史收口；[轻量控制器](../../src/markov/include/markov/controller/lightweight_controller.hpp) 已接入主线限定模块与执行核。2026-10-09 补充流程边界复核与给定耗时要求，完整动作合同、统一插桩及节点级动态展开尚待验收，不代表现有代码已支持新增能力。KTransformers 预测适配后续开展。源码见 [control.hpp](../../src/markov/include/markov/contracts/control.hpp)，输入值对象见 [project.hpp](../../src/markov/include/markov/contracts/project.hpp)。

## 探索集合与分层语义约束

最终入口为固定模型、配置集及 request workload 集。工作流为每个组合构造独立控制器、KV/资源状态和事件队列；共享只读模板与性能资产，不共享可变前沿或模拟时钟，不要求用户指定 base。

Schedule/Prepare/Prefill/Decode/Publish 是顶层合同。attention、KV、通信等内部区域用于模板变换与中途端口，不自动成为控制动作或新继承层。接口消费动态完成事件，不将一次原生采集的调用顺序固定为目标控制路径。

换框架的默认扩展点是模板生产端，控制接口和共同轻量策略保持统一。标准动作、状态和端口通过模板合同连接原生观测，核心不能用框架名称、原生对象或回调决定执行行为。Controller 的可替换接口保留给明确的控制能力扩展，不要求每接入一个框架就实现一个控制器。共同策略无法表达的原生行为需报告边界，不能隐含声称原生调度等价。

## 1. 交互原则

控制器依据请求与完成状态选择标准 serving 步骤；运行时实例化模板、调度资源、维护唯一模拟时钟并产生完成事件。接口不暴露框架对象、控制器队列、可写 DAG 或宿主时间。

共同依据是[流程动作合同](01_system_framework.md)：输入、工作职责、完成端口、状态效果、参数和适用条件。插桩、模板和控制动作对齐这份合同。先在给定节点耗时下验证展开与时间反馈，再接 cost model；两种时延来源不改变控制接口、模板语义或执行核。

同一进程内使用类成员方法与值对象，不使用 RPC、插件注册中心或跨语言仿真。控制器的替换需求通过一个浅层类接口表达；不为模板、配置或每个数据对象另建空壳基类。

```cpp
class Controller {
public:
    virtual ~Controller() = default;
    virtual void initialize(std::span<const Request> requests, ServingLimits limits) = 0;
    virtual std::vector<ControlAction> handle(std::span<const ControlEvent> events,
                                            const RuntimeView& runtime) = 0;
    virtual ControllerSummary finish() const = 0;
};
```

initialize 只建立私有状态，不能把全部请求提前变成可见请求。请求存储由工作流持有，生命周期覆盖控制器运行；每次 handle 的 span/view 只在调用期间有效，延迟工作保存身份和值，不保存这些视图。finish 汇报控制器的业务完成计数，不从宿主时间产生目标耗时。

ServingLimits 是已解释的控制容量值：request_capacity（并发请求数）、kv_capacity_tokens（KV token 容量）、prefill_capacity_tokens（一次 prefill token 上限）。SGLang 生产绑定先检查已有 cold/non-overlap 路径相关配置，再映射这些值；不把 SGLangConfiguration 交给 Controller。页大小仍来自模板资产，由工作流显式构造轻量控制器，不能猜测。运行时以同一 request_capacity 实例化请求池布局，避免控制与图变换各自解释框架配置。

此项只移除了控制器与运行时对原生配置字段的读取。既有具体调度顺序、KV 页释放策略、TemplateBank 的框架布局依赖仍待按共同策略合同收敛；标准容量参数本身不证明这些策略跨框架等价。

## 2. 标准步骤、参数与端口

步骤定义同时约束控制调用、Python wrapper 和 C++ 子图提取。核心维护唯一 BlockDefinition 合同，框架绑定将其关联到原生锚点和提取规则；公共 wrapper/extractor 以浅层继承复用固定流程。具体语言职责、身份与交接规则见[统一 block 抽象](04_collection_templates.md#23-block-定义wrapper-与子图获取的统一抽象)。该设计待实现，不表示现有 StepKind 与阶段字符串已经完成统一装配。

当前代码的五个 StepKind 为 Schedule、Prepare、Prefill、Decode、Publish，作为主线候选划分复核。名称和类型存在不等于真实边界已经成立；S3 需核对原生位置、工作关联及状态效果，确需调整时同步唯一正式合同与消费者，不保留并行版本。到达、等待和终止是控制交互，不伪造有成本的模板。

| 步骤 | 输入与入口 | 出口 | 效果与计时边界 |
| --- | --- | --- | --- |
| schedule | 当前可见集合、已发布进度、准入/KV 预算；控制器形成 BatchInput 并预约成员 | selected | 显式 CPU 调度服务结束，预约变为已选 batch；不完成 token 或释放 KV |
| prepare | selected；每请求 token 数、已有 context、KV 位置与目标配置 | prepared | 输入规格与 KV/attention 元数据准备完成；执行中的 H2D/gather 仍保留真实依赖 |
| prefill | prepared；phase=Prefill，各请求尚待处理的 prompt 范围 | submitted、device_done | 子图包含 CPU launch、NPU/通信/传输和设备采样；前者仅表示提交结束，后者表示本步骤所属设备工作完成，publish 内发起的物化/拷贝仍归 publish |
| decode | prepared 和该请求上一次 published；phase=Decode，每请求一个输入 token | submitted、device_done | 计算下一 token 的结果；不在 device_done 时直接发布业务进度 |
| publish | submitted 约束入口，device_done 约束 published 出口 | published | 本阶段必要结果物化、等待和 CPU 处理完成，token 进度/可见性生效；达到长度条件后释放逻辑 KV 并允许终止 |

SGLang 的 prepared 当前映射到父 schedule 返回，表示主机侧输入规格完成；resolve_forward_inputs 在 run_batch 内才发起/物化实际输入，不能将 prepared 当作全部设备输入已就绪。model_forward 的张量 shape 观测也不证明对应 DMA 已完成。当前通过 InvokeStep.dependencies 表达 submitted 入口门控，通过 port_dependencies 表达 device_done → published 出口门控。publish 的 CPU 前段可提前运行，结果等待仍由内部关系决定；完整内部因果关系仍需资格复核。

RequestSlice 的 request_id 是 Project::requests 的索引；input_tokens 为本次实际输入数量，context_tokens 为输入前已有 KV token 数。generated_tokens 是本批次形成时已经发布的输出 token 数，由控制器现有 published_tokens 生成只读快照；Prefill 为 0，Decode 为 g。同一批次的 Schedule/Prepare/计算/Publish 共用该快照，它不在 DeviceDone 或 Publish 入口提前加一，也不是第二份可变进度状态。首轮未分块 prefill 为 input_tokens=L、context_tokens=0；第 g 个生成 token 已发布后，下一次 decode 为 input_tokens=1、context_tokens=L+g−1。prefill 发布首个生成 token，输出 N 次对应 prefill 一次、decode N−1 次。finishes 表示本次发布是否应达到声明的输出长度，由控制器根据当前已发布进度计算；该动作参数用于匹配原生结束/释放分支，并不提前提交完成状态。请求完成仍只在 Published 事件生效。

BatchInput 描述一次实际工作，不保存第二份可变请求状态。WorkId 由控制器每次调用唯一分配，仅用于调用/端口关联；RequestId、WorkId 不作为成本特征或配置特例。端口引用为 PortRef{work_id, port}，每个端口在一次调用中只完成一次。

submitted 与 device_done 之间**没有默认的全图完成顺序**：设备可在 CPU 提交尚未返回时开始甚至完成。每个设备节点依赖其对应 launch，而非所有 CPU launch 的末尾。publish 入口依赖 submitted，发布状态依赖 device_done，避免提交未返回就进入结果处理或设备工作未完成就发布；若真实部署有其他线程处理，需新增有证据的资源规则，而非删除这项依赖。

一个阶段可以有多个端口。内部事件若使模板外工作就绪或改变后续决策，必须保留为端口；其他工作保留在内部图。原生 scope 的 begin/end 只界定相应 CPU 观测范围，端口完成需实际提交/设备/发布依据。边界切分不增加同步，也不强制所有步骤全局串行。

原生函数嵌套不决定计费粒度。SGLang 的 get_new_batch_prefill 内部调用 prepare_for_extend；schedule/prepare 应按工作所有权分开，父计时不能再次包含子成本。实际 mapping 与插桩边界见[框架路径](framework_paths.md)，叶节点与性能单元在 S3/S5 落实。

适用条件：首条 SGLang 为 FCFS、固定 TP、分离未分块 prefill/decode、无 prefix/HiCache、无调度 overlap 或 graph replay；正常容量内不重撤。KTransformers 同一合同可表达 CPU/NPU 子图与多端口，但其路由、mini batch 和 graph 规则仍需取证，不能因类型相同而声称已支持。

## 3. 事件与动作

ControlEvent 包含 time_ns 和以下三种 payload：

| 事件 | 内容 | 控制器行为 |
| --- | --- | --- |
| RequestArrived | RequestId | 输入来自已校验 Project；请求加入可见集合 |
| WorkPortCompleted | PortRef | 根据端口消费结果；只有对应 published 改变 token/缓存可见进度 |
| TimerExpired | TimerId | 处理已登记的逻辑 deadline/唤醒；首条正常生成流程不依赖 timer |

同刻事件作为一个 span 交付一次 handle，先消费整组事实，再形成下一次调度决定。控制器可在服务尚未完成时接收到达事件并更新待处理集合，但预约中的 batch 不吸收这些新成员。由已有端口完成触发下一步，不能用宿主线程轮询。

| 动作 | 内容 | 运行时合同 |
| --- | --- | --- |
| InvokeStep | WorkId、StepKind、BatchInput、前置 PortRef 数组 | 先合法实例化/变换，再取得节点时延并调度；接受表示登记，不表示完成 |
| RegisterTimer | TimerId、绝对 deadline_ns | 使用同一模拟时钟；deadline 不早于当前时刻 |
| CancelTimer | TimerId | 取消仍有效 timer；不撤销已发布事件 |
| CompleteRequest | RequestId、最后一次 published 引用 | 在长度终止条件与该端口满足后记录逻辑完成；不是另一段服务成本 |
| UnsupportedControl | 明确控制缺口 | 在页/槽位提交前停止该候选并报告 unsupported，不伪造完成或零成本 |
| 空动作数组 | 无 | 等待下一事件，既不忙轮询也不额外收费 |

首轮不提供 cancel_work：正在执行工作是否可取消需要真实资源语义，不能预建永远不可用分支。资源忙表达排队；缺模板/变换/成本是工作流结果（unsupported / needs_data），不是伪完成事件。错误动作按一次接口边界处理为失败，不重复校验所有内部返回值。

S4 的节点时延来自显式给定输入，不能因尚无模型样本而启动性能补采；给定值缺失时停止并定位节点，不默认零。S5 之后由成本提供者取得时延，模型缺口才进入性能数据流程。纯逻辑 marker 可以零服务，真实服务节点仍须有明确给定值或适用成本。

## 4. 单一状态来源

| 事实 | 唯一维护者 | 另一侧如何使用 |
| --- | --- | --- |
| 可见/等待请求、预约 batch、生成 token 数、逻辑 KV 分配与缓存可见性 | 控制器 | InvokeStep 提供明确工作参数；公开输入仍不可变 |
| 叶节点就绪、资源队列、CPU lane、NPU stream、传输、在途调用及端口完成 | 运行时 | RuntimeView 仅提供 time_ns 和在途 WorkId 的只读视图，事件提供完成事实 |
| 模拟时间与 timer 队列 | 运行时 | 控制器只读 time_ns；不再维护平行 clock |
| 真实 base/template/独立成本 | 模板与成本提供者 | 每次实例化与绑定使用明确适用域，不从控制器私有字段猜输入 |
| 逻辑完成的指标时间 | 运行时报告记录 | 控制器拥有终止业务状态；指标是事件观测，不另维护可调度请求副本 |
| 任务状态、采集预算、资产和恢复 | 工作流 | 不混入模拟目标时间；纯预测重算不会重置真实采集消耗 |

首轮控制器没有需要读取的空闲 lane 数，因此不预置巨大资源字典；后续确有决策消费者时再添加只读字段。逻辑 KV 与运行时带宽是不同事实，不混用一个双向状态对象。

## 5. 唯一时钟与同刻规则

所有模拟时间为整数 int64 ns，相对于逻辑工作窗口起点；输入 arrival_offset_us 在边界转换一次。采集的设备/宿主原始坐标、墙钟与线程 CPU 时间另记，S3 统一对齐后才能建立源 DAG，不把源绝对坐标直接当预测时间。

运行时每次推进到最早可发生的时间，处理顺序固定：

1. 满足因果依赖的端口/服务完成。已有依赖优先；同刻无依赖关系的完成按工作登记顺序及出口声明顺序稳定排列，零时长语义 marker 的因果链先闭合。
2. 该时刻的请求到达，按 workload 声明顺序。
3. 该时刻到期的 timer，按登记顺序；同刻完成已先可见，避免 deadline 抢在已完成结果前触发。
4. 将整组事件传入一次 handle，消费状态后做一次新的调度决定；处理动作产生的新因果工作。

这些同刻规则是已声明的离散事件近似，尚未证明与 SGLang 原生调度稳定等价。若真实结果对该顺序敏感，S3/S4 需取证并限制适用域；不根据容器迭代顺序或实验成绩决定。KT 后续单独核对规则适用性。仅有语义 marker 可零成本，真实 CPU/NPU 服务缺测量不能设零。

## 6. CPU 预约、提交与发布序列

CPU 决策采用入口快照：在 t0 根据已可见请求选择并预约 R1，提交 schedule 子图；selected 完成于 ts 后正式接受 batch。ts 前到达的 R2 保留在等待队列，下一轮才考虑。prepare 完成后调用 prefill/decode 子图；最后的 published 才修改逻辑进度。这是明确的阶段边界近似，不声称逐条重现 scheduler 内部读写。

```text
t0: 到达 R1 → 预约 R1 → CPU schedule
          R2 在 schedule 期间到达，进入等待集合
ts: selected → CPU/数据 prepare
tp: prepared → 调用 prefill
          CPU launch A ──→ NPU A
          CPU launch B ──→ NPU B / 必要通信 / 结果拷贝
          两条路径保留各自依赖，允许 CPU 与 NPU 重叠
tc: submitted（所有提交返回）
td: device_done（结果依赖满足，td 不强制晚于 tc）
max(tc, td): CPU publish 开始
te: published → 生成数从 0 变 1；R1 等下一次 decode 或结束
    整组同刻事件消费后，根据当前可见请求再次 schedule
```

t0/ts/tp/tc/td/te 只是符号时间，不是硬编码时延或拟合常数。等待到 max(tc,td) 是依赖等待；publish 的实际 CPU 服务另计，不再给等待区间收费。结构阶段为这些执行节点提供明确给定耗时，成本阶段再由适用模型估计；handle 的宿主执行耗时永远属于模拟器开销。

下一轮若 R3 的到达晚于 te 则暂不可见，若成本使 te 推迟到 R3 到达后则可能参加下一 batch。这就是时间影响后续动态展开的位置，既不改成本来选择结构，也不复制 base 的 batch 顺序。长 CPU 工作若有必须在中途发布的状态，需增加真实端口/拆分单元；不能一次性补总时长假装正确。

## 7. 技术选择与旧实现审查

- 单进程、类导向 C++23；当前正式 target 为 markov_core 与 markov，头文件在 src/markov/include/markov，实现在 src/markov/src。
- 使用标准库的 vector、span、variant、optional、filesystem、RAII 与显式循环；nlohmann-json 3.11 是当前镜像已有的边界 JSON 依赖。无额外图框架、协程运行时、RPC 或模板元编程。
- ProjectReader 将外部输入校验为值对象，Planner 只读解释输入，ServingContract 提供步骤/端口。Controller 定义交互，已有 LightweightController 是实际实现，NativeControllerAdapter 尚未实现；不增加占位后端。
- Python 3.10 承担已有原生 wrapper/采集编排，统一语义记录仍待补齐；核心控制、变换、成本和模拟不复制到 Python。C++ 正式程序通过明确 manifest 获取采集产物。
- Shell 入口只调度 modeling 容器及 CMake/可执行程序。旧 scripts/lib 已归档，本轮不为了复用封装而导入 deprecated 或重建无消费者 common 层。

审查 deprecated 的 DagGraph、DagMutation、TopologicalSimulator、CpuTaskQueues：稳定身份、显式依赖、原子修改和 lane/FIFO 的原则可复用；现有存储同时含 trace 坐标、HiCache/CPU gap、黑盒排除和成本，旧模拟还有 std::function 扩展 hook、源图原地时间写回及多种 scope。整体搬回会违反当前单时钟、类交互和 scope 合同，**本阶段不复用旧实现**。S5 若确有消费者，只提取独立算法并使用新类型，不保留旧接口或适配层。

构建/运行/格式检查均在 modeling Docker，实际入口见[运行说明](development.md)。Release 构建证明合同与 plan 可编译，不能证明真正的异步仿真、原生控制等价或性能精度。

## 8. S2 完成边界

已固定每步输入、端口、效果、适用条件、事件/动作、所有权、单位、同刻规则与 CPU 生效时机；正式 plan 使用同一类型输出步骤和差异。CPU→NPU 异步例子及完整 prefill/publish/decode/publish/终止路径可逐步解释。

以上是当时 S2 的合同收口，不替代 S3 真实边界复核。运行真实模板、给定节点耗时、完成回传、等待和终止的行为验证优先在 S4 完成；S5 再估计耗时，S6 贯通性能产品。给定耗时是明确假设下的执行输入，结果标明条件，不作为预测精度或生产拟合参数。复用同一执行核，不另造自测控制器、mock 或伪产品入口。


### 目标实际 KV 页状态

冷缓存、关闭 radix/HiCache/overlap 的当前路径中，BatchInput.free_kv_pages 是控制器空闲页队列在本批预留前的数量。Progress 持有各请求的逻辑页身份；Prepare 动作只预留本步新增页，Finished 在 Published 后归还。不得另用累计 token 公式维护第二份空闲计数；准入预算只作估计，不代表实际页所有权；旧的完整输出预约已删除。具体生命周期见[控制与语义](05_controller_semantics.md)。


### 批次成员、过滤条件与空调度

轻量控制器分别保存 running batch 的成员顺序，以及尚待合并的 prefill 成员；请求完成状态仍只保存在原进度对象中。调度先过滤并合并上一 prefill，随后按尚未过滤的 running batch 大小检查准入；不能准入时才过滤 running batch 并选择 decode。原 compaction_pending 布尔特例已删除。

BatchInput.filters 按顺序保存每次过滤的输入槽数与保留槽位。全部保留与全部移除也属于条件，不只记录部分压缩。Schedule 模板必须匹配完整过滤序列、当前请求形状和发布条件；未观测条件明确停止。

如果一次真实清理没有选出批次，动作 phase=idle，仅产生 Schedule，Selected 表示本次空决定完成。控制器等待该端口后才能继续，CPU 工作通过原模板和给定节点耗时执行，不填零。原生有批次时 Selected 对应子 prepare 入口，无批次时对应 schedule 返回。最后一个请求 Published 后结束本次 serving 窗口，不附加尾部轮询。

### 准入策略交接与状态生效

以下合同已接入 C++ 的策略交接与控制路径；缺参数的已有资产返回观测缺口，真实新策略的正向执行尚未验收。

生产端将有效初始比例、最小比例、每次 Decode 衰减量、输出估计上限和每请求保留量编译为不可变控制参数，来源入口比例单独用于初始状态核对。参数在 rank 间须一致；每个 rank 的首入口由原始 monotonic 开始时间选择，记录索引不代表执行次序。该时间只用于来源核对，不交给目标执行时钟。缺失返回观测缺口，无法表达的原生行为返回实现缺口。不回调原生函数、不复制每个来源步骤的决策或比例。

主线冷启动控制状态使用策略初始比例、空运行批次和开放准入。原生观测用于验证这一状态时，必须证明测量窗口的起点相同；warmup 后 entry_ratio 不等于 initial_ratio 时，不把前者默默作为目标初值。观测缺失不允许沿用完整输出预约作为兼容后端。

| 时点 | 控制效果 |
| --- | --- |
| Schedule 转换上一 Prefill | 过滤、接管或合并；按成员变化更新准入关闭状态 |
| Schedule 准入 | 使用当前比例、实际空闲页与请求已发布进度计算局部预算；接纳后才扣 Prefill 输入预算 |
| Schedule 进入非空 Decode | 过滤后检查下一步实际新页需求；无回退时更新一次比例，随后形成 Prepare 动作 |
| Publish 完成 | 更新输出进度、完成状态和实际页归还；不提前执行下一次 Schedule 的准入恢复 |
| 空选择且系统完全空闲 | 将比例恢复到初始值；有 Waiting 请求但无法准入不属于完全空闲 |

每个选择周期的预算和完成次序列表都是临时计算，不能成为第二份持久请求/KV 状态。比例与准入关闭状态由控制器各维护一份；它们是调度状态，模拟宿主计算不计入节点成本。对应 CPU 工作仍由 Schedule 模板提供，不能用这些状态计算替代 CPU 子图。

原生公式、插入顺序差异及边界证据见[准入策略与内存压力](sglang_template_relations.md#准入策略与内存压力)。代码已共同交接参数、初始化、Decode 更新、完全空闲重置与缺口结果；缺观测路径可检查，但仍需真实策略资产和独立目标证明正向行为。
