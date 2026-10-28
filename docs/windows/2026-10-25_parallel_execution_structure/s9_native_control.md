# S9 原生控制接入指导

创建：2026-10-28；更新：2026-10-29。本文补充 [plan.md](plan.md) 的 S9：先解释接入路线，再给出源码依据、实施次序和通过条件。当前资格由 [status.md](status.md) 维护，探索经过由 [log.md](log.md) 记录。以下接口和目录调整是实施建议，不表示已经实现。

## 1. 路线与推进顺序

目标保持：**原生代码负责逻辑控制，核心维护唯一的仿真资源和执行状态，adapter 连接二者。** 轻量控制器使用同一资源和执行接口。

S9按 **单worker → TP/EP → PP/PD → 审查交付** 推进。调度优化和一级前缀缓存独立放入S10，S11再整合并执行完整独立验证。单 worker 指 TP=EP=PP=1、关闭PD的基本请求闭环，不代表完整S9。先让每一层能独立说明输入、原生决定、图执行和完成反馈，再增加下一层的协作。

无设备运行优先参考上游 `tools/sglang-simulator` 的导入hooks、模型/设备入口替换和CPU元数据路径。它与本地 `debug_utils/schedule_simulator` 是两份不同实现；此前对后者的判断不能用于否定前者。具体参考和适用边界集中在第8节。

**PP/PD的组织规则由原生代码继续执行。** TP/EP组内批次一致性、PP微批次轮转、PD接收准入都不重新设计。待做的是把原生资源查询、工作提交、通信及等待接到共同服务和模拟完成事件。每个worker需要控制上下文，不代表每个worker应独立选批；也不因worker数量增加就机械复制自写控制器。

目前没有证据认定PP/PD存在原理性障碍或必然需要庞大改动；其接入工作量尚待实际验证。单worker上游方案也不能直接授予多worker支持。一级缓存、严格Decode优先等要求由S10承接，源码可用性和运行资格分别确认。

Markov分析基于已交付`1b4f329`及工作区；目标SGLang固定为`0460f277b770f8b5a404da2e50e7a9573ef8db40`。上游Simulator仅作参考，不自动升级本项目框架、修改官方工作树或重置已有来源资格。阶段起点核对遗留修正的交付及适用范围，不计入原生控制实现成果。

## 2. 一次预测怎样运行

```text
请求到达 / 模拟事件
        ↓
原生请求队列、选批及进度处理
        ↔ adapter ↔ 核心KV资源服务：查询、分配、引用、释放
        ↓
adapter 交出实际工作成员、token 范围及资源绑定
        ↓
共同模板实例化 → 数据/通信连接 → 执行器推进模拟时间
        ↓
提交 / 数据可用 / 模型结果可读 / 输出 / 回收完成
        ↓
adapter 恢复原生调用 → 原生处理结果、继续选择
```

例如，请求到达后，原生选出Prefill成员，调用核心取得槽位、KV页和映射，再把模型工作交给模板和执行器。结果可读时恢复原生结果处理，生成首个输出并决定后续Decode；达到固定输出长度后释放请求资源。在途工作仍持有的页等相应完成事件后回收。新请求能否加入后续批次由原生决定，已提交批次保持不变；chunk和前缀命中在S10加入这一闭环。

这里真实执行的是控制代码和必要的整数/tensor 元数据操作；不会执行真实模型、目标设备 kernel 或网络传输。仍使用固定输出长度和给定 EP 路由；不预测自然文本。真实原生 serving 则用于采集和独立对照，保留它自己的真实资源管理，两者不能混为一次运行。

## 3. “逻辑控制”和“资源管理”的具体划分

下表说明S9/S10共用的所有权边界。S9实现普通KV的槽位、页、映射、引用与就绪；前缀索引、缓存匹配/发布/LRU和chunk进度在S10实现，不作为S9交付前提，也不预建空接口。

| 所有者 | 唯一维护的内容 | 对外边界 |
| --- | --- | --- |
| 所选控制实现 | 等待/运行队列、请求逻辑阶段、chunk/output 进度、批次成员、PP 微批和 PD 控制上下文 | 提交工作，查询或申请资源，等待并消费对应反馈 |
| 核心资源服务 | 槽位与页归属、请求映射、容量、前缀索引、引用保护、驻留、就绪、淘汰和回收 | 返回有身份的资源结果和依赖，不替原生选择下一批 |
| 模板与执行器 | 工作图、存储版本、数据/通信依赖、在途任务、资源服务顺序和唯一模拟时钟 | 发出具名完成事件，推进对应资源的就绪/回收边界 |
| 框架 adapter | 原生接口形态、必要身份映射、来源约束和返回描述 | 原生资源 facade 调用核心；不另养请求队列、页账或缓存树 |

**资源服务也需要算法。** 原生决定何时匹配、申请或释放；核心服务实际完成匹配、分配、引用及淘汰。共享机制放核心；框架特定的页顺序、对齐、缓存发布边界等由有源码依据的适配绑定提供。优先小量明确的策略参数；确有不同算法且存在真实消费者时才提取策略接口，不建设任意规则引擎。

这意味着部分资源语义需要按原生规则实现，并逐项对照，不是“所有 SGLang 内部代码都原封不动地运行”。直接复用的是调度/请求控制；核心资源实现是需要验证的一部分。保留原生 RadixCache 自己的树、再把页数同步给 C++，不符合本路线。

原生 `Req` 中仍会保留长度、前缀句柄、释放标记等控制字段。它们是控制进度或资源服务返回的视图，不是第二份可独立修改的资源真相：

- `prepare_for_extend/decode` 在模型执行前更新 `kv_committed_len`，这个名字不能解释为设备数据已写好。
- 原生准入时估计未来 token 的预算，不等于已经占用了相应物理页。
- 请求结束解除请求引用；缓存或在途传输仍可持有页，不能把结束等同全量释放。
- 核心记录“已占用但未写好”；只有对应写入完成，消费者才能使用。缓存可见性与内容就绪分别表达，必要时带等待依赖。

跨 TP/PP/P/D 的资源域也要明确。实例和 stage 的物理池彼此独立；TP 对应请求的逻辑身份可相同，但每个分片有自己的容量与就绪。不能让一次逻辑申请在同一池重复扣 N 次，也不能把真实 N 份分片资源合成一份。首期沿原生参与者保留明确的池/分片绑定，控制参与者的合并需要另有等价证据；模板共享本身不提供这种证据。

## 4. SGLang 的实际接管点

### 4.1 初始化和控制循环

| 源码位置 | 当前行为 | 接入方法及限制 |
| --- | --- | --- |
| [Scheduler.__init__](../../../third_party/sglang/python/sglang/srt/managers/scheduler.py#L303) | 建立 watchdog、IPC、worker、内存池、cache、stream 及结果处理器 | 保留可复用的原生初始化，替换明确的硬件入口；不启动真实推理，也不靠 `__new__` 后猜填大量字段 |
| 同文件 `init_model_worker`（860）、`init_memory_pools`（837） | 取得 worker 能力、组、stream、池，原 worker 会加载模型并初始化设备 | 能力来自配置与合格来源；模型执行和物理资源替换为模拟边界。不得通过读真实空闲显存决定预测配置 |
| 同文件 `event_loop_normal`（1506） | 收请求→选批→运行→处理结果；空批调用 `on_idle` | 原生主循环或其实际控制方法直接运行，到工作/等待边界交出控制，不在 C++ 另写原生选批流程 |
| 同文件 `get_next_batch_to_run`（2588） | stash chunk、过滤/合并旧批、尝试 Prefill，再考虑 Decode | 保留实际调用和副作用，不能只调用 `SchedulePolicy` 的排序函数冒充完整控制 |
| [PrefillAdder](../../../third_party/sglang/python/sglang/srt/managers/schedule_policy.py#L425) | 查询可用/可淘汰容量、锁引用、限制 chunk、改变请求切片 | 查询必须在决策过程中回答；不能选完 batch 后才同步资源 |
| [Req.init_next_round_input](../../../third_party/sglang/python/sglang/srt/managers/schedule_batch.py#L1123) | 根据当前 token 和 cache 取得前缀索引、节点及有效输入 | S9接入关闭缓存时的有效索引；S10增加内容身份及缓存句柄，保留末token等原生边界 |
| 同文件 `ScheduleBatch.prepare_for_extend`（2008）、`prepare_for_decode` | 分配、写映射、构造 device tensor，同时推进控制字段 | 保留控制效果，替换物理操作；CPU 元数据需要真实整数，单用 meta tensor 不够 |

初始化先借鉴Simulator的有效硬件替代边界，证明原生正常选择和结果处理能继续。允许最小模型对象、CPU元数据及必要kernel加载替身；不能用无意义的成功返回替代真实控制和资源结果。若开始逐函数复制Scheduler或批量补造无消费依据的私有字段，应返回接管边界分析。环境适配只处理环境，框架资源 facade 只处理协议；不要把所有问题塞进一个初始化兼容文件。

### 4.2 资源访问并非只有 alloc/free

| 原生协议/位置 | 核心必须提供的含义 | 容易漏掉的地方 |
| --- | --- | --- |
| [ReqToTokenPool](../../../third_party/sglang/python/sglang/srt/mem_cache/memory_pool.py#L229) | 请求槽位、token→KV 索引、映射写入和释放 | 调用方会直接切片读取 `req_to_token`，仅替换四个方法不够；提供当前所需的映射视图，不复制全池 tensor 账本 |
| [alloc_for_extend / alloc_for_decode / release_kv_cache](../../../third_party/sglang/python/sglang/srt/mem_cache/common.py#L442) | 按当前前缀和尾页申请、回收、归还槽位 | 原生会写映射、处理过量预留和分组释放，不能最后统一按请求长度计算 |
| [NPU allocator](../../../third_party/sglang/python/sglang/srt/hardware_backend/npu/allocator_npu.py) | 页对齐、尾页延用、空闲/延迟归还页顺序 | 只对上总页数不足以保证实际索引、排序和相应准备子图正确 |
| [BasePrefixCache](../../../third_party/sglang/python/sglang/srt/mem_cache/base_prefix_cache.py) | match/insert/evict/lock/unlock 的输入和返回 | `IncLockRefResult` 等返回 delta 会影响预算，不能全部返回空结果 |
| [RadixCache](../../../third_party/sglang/python/sglang/srt/mem_cache/radix_cache.py#L360) | 压缩前缀、引用、缓存发布、LRU 与容量变化的语义依据 | match 可分裂节点并更新时间；node 句柄、树变化和页就绪不能被一个命中长度取代 |

S9资源服务实现所选普通KV语义，S10再接一级设备前缀缓存；上表的BasePrefixCache/RadixCache条目是后者的接管依据。S9只满足关闭缓存时实际需要的协议，不提前实现树或淘汰算法。本轮不扩到Mamba/SWA、HiCache或所有cache-aware排序策略。轻量和原生都调用它，后端能力仍分别声明。`RadixCache.create_simulated()` 可用于原生算法对照，但直接把它当生产缓存管理器，会把权威缓存状态留在 Python，不能据此宣称已完成共同资源层。

原生 facade 的返回值采用原生参数/结果类型或最小句柄视图。按实际调用补齐必要协议；对树遍历、具体类判断等额外依赖逐项核实，不能通过伪装类型或无限代理 `__getattr__` 隐藏缺口。

### 4.3 结果处理需要第二次进入原生代码

[Scheduler.process_batch_result](../../../third_party/sglang/python/sglang/srt/managers/scheduler.py#L3400) 分派普通、PD、prebuilt、idle 等结果处理；[SchedulerBatchResultProcessor](../../../third_party/sglang/python/sglang/srt/managers/scheduler_components/batch_result_processor.py#L62) 承接普通批次。处理器推进输出、chunk 进度、cache 及分组回收；这些不是一次模型提交时就全部确定的效果。

推荐交接顺序：

1. 原生准备批次，核心给出资源绑定；相应准备和模型工作进入图。
2. 模型结果可读时，adapter 提供有效的 `GenerationBatchResult` 元数据，恢复原生结果处理。
3. 原生结果处理触发输出和回收，S10再加入缓存效果；核心记录实际资源操作，并展开对应工作片段。
4. 输出、可重用资源和下一次控制唤醒，分别按其完成端口生效。

准备或结果处理期间，如果后续控制读取的资源值必须等待某个 CPU/映射工作完成，就在该真实服务边界等待；同一个工作片段可以合并相邻操作，但不能跨越影响决定的观察点。同步申请可先占用资源，内容与异步归还仍等待各自事件。这样不会让“Python 函数先运行完”变成所有模拟 CPU 工作都已经完成。

普通无 overlap 路径可在结果消费边界等待；PP 需要异步提交后继续推进其他微批，不能让每次 `run_batch` 等到整批完全结束。实际 host 控制运行时间只计处理开销；图上的 CPU 工作使用给定成本，不能再把 Python wall time 加一遍。

## 5. 当前 Markov 要调整的接口和职责

以下是接入原生控制及后续优化需要调整的职责。S9先满足基础闭环，逐成员阶段、共享前缀和优化来源按S10实际消费者补齐，不在S9一次预铺；共同状态所有权从S9起保持唯一。

| 当前实现 | 不适合直接承接的情况 | 计划调整 |
| --- | --- | --- |
| [ServingControl::plan/start/finish](../../../src/markov/controller/control.hpp) | `plan const`、构图成功后才 start；原生选批期间已有资源副作用 | 改成有状态地推进至工作/等待边界，返回带身份的工作或等待条件；不克隆整个原生对象来假装无副作用 |
| [Batch / BatchWork / BatchEffects](../../../src/markov/contracts/serving.hpp) | 一个 batch.phase、预先确定整批结束效果 | 成员自带阶段、逻辑 token 范围和有效 context；模型完成和结果处理效果分开，不重复预测原生下一状态 |
| [ServingWorkflow](../../../src/markov/workflow/serving.cpp) | 先扩整批，随后统一 finish；idle 只在特定空闲路径推进 | 共同处理资源调用、提交、等待及完成；无模型工作也能形成片段，workflow 不读私有队列 |
| [KvPool](../../../src/markov/resources/kv_pool.hpp) 与轻量/PD 控制 | 页/槽位已独立为共同对象并由轻量控制借用，原生资源尚未接入 | 原生 facade 访问同一服务，补充实际使用的映射/就绪接口；前缀缓存留S10 |
| [SGLangRankStructure](../../../src/markov/adapters/frameworks/sglang/rank_structure.cpp) | 维护空闲池/请求映射的图值，并基于批次效果推导回收 | 图值和存储版本继续保留；数值分配与状态变化绑定资源服务结果，删除独立推测页账/回收的职责 |
| [Executor](../../../src/markov/execution/executor.hpp) | 已有就绪资源、任务和 Published/Completed | 扩展必要端口和唤醒，不另建时间轴、事件模拟器或全局屏障 |
| 现有采集会话及 hooks | 关闭chunk/cache/overlap时的普通batch来源不授予新机制资格 | S9核对基础闭环来源；S10取得chunk/mixed/cache来源，观察与控制适配分开 |

特别注意：当前 `KvPool::ready_pages_` 表示延迟排序之前可直接分配的空闲页段，不是 KV 内容就绪状态。迁移时按职责命名，不能复用这个字段假装已经有数据依赖管理。

资源操作使用核心内部有类型的接口；仅跨 Python/C++ 边界序列化必要值。每次操作明确请求/任务/池身份和对应效果，复杂转移由业务资源服务一次完成，不铺开任意字段读写接口。构图失败时本次运行明确失败并清理所属资源，不为继续预测而实现原生整状态回滚、重试和隐藏 fallback。

### 5.1 共同接口至少要表达什么

下面是职责合同，不是要求再建一套通用消息平台：

| 交接 | 必要内容 | 由谁决定 |
| --- | --- | --- |
| 资源方法 | 请求/池/前缀身份，逻辑 token 范围；返回槽位、具体 token→KV 索引、引用句柄、容量变化及就绪依赖 | 核心资源服务，原生 facade 转换原生类型 |
| 工作提交 | 任务、实例/组、flow/lane/stage；实际语义调用及成员参数、资源/输入绑定、已有依赖 | 控制端实际调用产生，模板消费；不携带原生对象 |
| 等待 | 等待哪个任务端口、通信或下一次输入；是可继续提交还是阻塞消费 | 原生真实调用边界；核心按事件唤醒 |
| 完成 | 对应身份、端口、模拟时间及该端口允许读取的结果描述 | 执行器；原生处理结果后再产生后续资源操作和工作 |

语义工作从真实调用边界交出：既有框架 hooks/NativeCall 的语义身份可复用，预测时只发必要调用及参数，不启动 profiler 或重新采图。adapter 把调用映射到库中对应的准备、模型、缓存/结果等片段，核心按输入与资源绑定实例化。不能继续让装配器猜一次原生批次内部发生了哪些过滤、合并和释放；也不将每个 Python 函数逐项上报。新 mixed/cache 边界所需片段必须先有来源资格。

S9返回当前调用所需的有效索引，原生需要的CPU tensor由这些索引形成视图；图中的映射写入、排序和拼接仍保留必要工作，不能因为核心已算出整数就删除应模拟的操作。S10接入共享前缀时，单纯页号列表不足以表达已有前缀、尾页及本轮新增token，须补齐实际消费者需要的返回合同。

### 5.2 代码放置、跨语言与依赖方向

下表规定实现位置，新增目录随真实消费者出现，不预建空包/父类。C++头文件与源文件同目录，文件移动同时更新调用方、构建、Docker和正式入口。

| 职责 | 放置位置 | 不应混入的内容 |
| --- | --- | --- |
| 共同工作、资源结果和完成合同 | `src/markov/contracts/` | SGLang类、私有字段、torch对象、原生函数名分支 |
| 控制交接及轻量实现 | `src/markov/controller/` | 设备替身、原生导入hooks、第二套框架策略 |
| 页/槽位、映射、引用与就绪；S10增加前缀缓存 | `src/markov/resources/`，现有KvPool与轻量消费者已迁入 | 框架队列、选择下一批、独立的原生页账 |
| 子图实例化、连接、模拟时间及完成反馈 | 现有`src/markov/templates/`、`graph/`、`execution/` | 私自重写PP轮转、PD准入，真实框架/SDK初始化 |
| C++侧SGLang语义工作与图绑定 | 现有`src/markov/adapters/frameworks/sglang/` | Python进程管理、通用协议传输和验证编排 |
| 原生控制适配 | `src/control/markov_control/adapters/frameworks/sglang/` | 通用图/资源算法、采集器、第二份请求或缓存状态机 |
| 设备/runtime替代 | 同一控制包的`adapters/runtimes/`，仅建立实际需要的实现 | 框架选批规则、按模型名字分支；runtime不反向依赖框架 |
| 正式运行装配 | C++现有`workflow/`与CLI；Python控制包的`workflow/` | 决策算法、适配私有字段、完整验证矩阵 |
| 来源采集与观察 | 现有`src/collection/markov_collection/` | 预测时启动原生控制或执行器；不把控制后端塞进采集会话 |
| 案例与独立比较 | `example/`；Docker目录仅组装运行环境 | 验证规则进入业务、Docker内维护业务hooks或模型特例 |

Python框架adapter负责原生对象构造、选批/结果入口、请求及rank身份转换和原生通信接口形态；PyTorch/Ascend加载、设备event等差异在runtime适配。框架adapter可组合runtime，通用层不反向识别框架。真正共享的身份/值编码只维护一处，通过明确合同复用；不要为了共享几行代码让采集与控制相互导入。

保持 `workflow → adapters → core` 的依赖方向。资源服务提供共同机制；确有来源依据的框架特殊策略由适配提供并注入，核心不按框架名分支。每轮改动同时检查代码位置和实际调用关系，不能只移目录却继续越层访问。

当前没有现成Python绑定。首条实现建议采用**长期存活的本地控制进程+管道消息**，保留原生调用栈，C++持有资源和图执行。跨语言只传必要资源查询/结果、工作快照和完成条件，不逐节点传图、不每批启动CLI，不同时建设多种桥接方案。框架无关的传输和进程生命周期归workflow；原生类型转换归adapter；`ServingControl`只表达共同推进接口。

S9.1只启动一个原生控制worker；到TP/EP及PP/PD再增加原生协议实际需要的参与者。原生进程级配置/组状态按需隔离，不提前建设多进程平台。控制参与者等待时只挂起自己，执行器继续推进其他就绪工作；目标设备编号不等同控制进程编号。

无设备运行环境需要CPU依赖和固定上游代码，实际C++程序仍在Docker内构建/运行并核对依赖。参考无设备方法不意味着把建模镜像编出的二进制直接塞入另一镜像，或把SGLang/Ascend SDK引入核心库。环境安装资源随适配维护，Dockerfile负责选择与调用。

## 6. TP/EP之后再接PP/PD

| 顺序 | 原生继续负责什么 | adapter接管什么 | 共同核心负责什么 |
| --- | --- | --- | --- |
| TP | 请求分发、组内批次与控制协同 | group调用、rank身份、必要通信结果 | 分片资源、模型/collective图和对应完成端口 |
| EP | 已有请求组控制，按给定路由参与模型工作 | 原生角色与给定token/padding路由绑定 | dispatch/expert/combine模板与通信；不另写专家调度器 |
| PP | [event_loop_pp / init_pp_loop_state](../../../third_party/sglang/python/sglang/srt/managers/scheduler_pp_mixin.py)的lane轮转、收发及结果处理 | stream/event、异步P2P和proxy元数据 | 每份工作及消息的依赖、完成和缓冲区生命周期 |
| PD | [Prefill控制](../../../third_party/sglang/python/sglang/srt/disaggregation/prefill.py)与[Decode控制](../../../third_party/sglang/python/sglang/srt/disaggregation/decode.py)的接收准入、预分配、prebuilt及组批 | 地址注册、KV发送/接收、poll及原生所需元数据 | P/D独立资源、KV片段传输、数据可用和回收条件 |

PP/PD的待办是**接通原生等待的完成信号**。例如原生D等待KV接收完成，adapter查询对应模拟传输，图完成后再报告Success；不重写D的准入或批次选择。PP中对应的微批次身份、事件和返回结果同样逐项交接，不能把`run_batch`强制改成整批同步完成，也不能让所有event立即成功。

PD已有[BaseKVManager/Sender/Receiver](../../../third_party/sglang/python/sglang/srt/disaggregation/base/conn.py)接口，优先沿`init/send/send_metadata/poll/abort`适配。[get_kv_class](../../../third_party/sglang/python/sglang/srt/disaggregation/utils.py#L422)仍是固定分派，需要在适配拥有的初始化/工厂边界接入；[FakeKVSender](../../../third_party/sglang/python/sglang/srt/disaggregation/fake/conn.py#L38)假定传输立即完成，不能直接用于有等待关系的正式预测。

控制通知和真实工作分别归属：每项模型通信/KV传输只生成一次图工作，adapter通知不能重复计费；必要元数据和决定可运行性的独立消息仍需保留。CPU环境下的描述符要维持目标dtype、布局及身份含义，不能沿CPU默认值改变Ascend目标协议。

S9.2先完成TP/EP，再由S9.3先接PP、后接PD，最后核对合法组合。这里不预判多worker一定复杂；根据实际接口改动、闭环结果和代码边界判断接入代价。

## 7. 向S10交接的边界

S9交付无设备原生控制、基础KV资源、工作/等待/完成接口，以及单worker到TP/EP、PP/PD的具名闭环。基础连续组批、PP轮转和PD接收准入属于本阶段；不因推迟优化而把请求生命周期改成固定批次。

S10在这条闭环上增加chunked prefill、混合组批、Decode优先和一级prefix caching，实现及验证细节统一维护在[plan.md](plan.md)的S10。S9交付时说明已有来源的适用域、尚待确认的原生能力及接口限制，不预建完整缓存树、多级后端或另一套调度器。新机制的资源状态仍由同一核心服务维护，必要CPU/设备工作仍经同一模板与执行层。

## 8. 无设备运行：优先参考SGLang Simulator

本节参考上游提交`4517a5ff1bbd1f944d6bde1819300ed14af7b698`的`tools/sglang-simulator`。它复用真实Scheduler/请求及缓存生命周期，提供已有的无设备接管方法；本地固定框架尚无这个工具目录，不能把上游支持直接视作本项目已验证支持。

| 上游源码 | 值得复用的方法 | 我们的接入边界 |
| --- | --- | --- |
| [hook_bootstrap.py](https://github.com/sgl-project/sglang/blob/4517a5ff1bbd1f944d6bde1819300ed14af7b698/tools/sglang-simulator/src/sglang_simulator/simulation/sglang/hook_bootstrap.py) | 导入前安装hooks，子进程重复安装必要接管 | 框架hooks放控制adapter，runtime加载替代单独归属；不修改官方工作树 |
| [model_runner.py](https://github.com/sgl-project/sglang/blob/4517a5ff1bbd1f944d6bde1819300ed14af7b698/tools/sglang-simulator/src/sglang_simulator/simulation/sglang/model_runner.py) | 最小模型对象，替换load/forward/sample/attention/graph初始化；缩小KV数据载荷并保留逻辑元数据 | 参考无权重和无设备办法；正式资源状态仍由核心统一管理，forward最终接共同图执行 |
| [sgl_kernel_hook.py](https://github.com/sgl-project/sglang/blob/4517a5ff1bbd1f944d6bde1819300ed14af7b698/tools/sglang-simulator/src/sglang_simulator/simulation/sglang/sgl_kernel_hook.py) | kernel loader stub避免加载真实GPU库 | 可以替代不执行的物理入口；不能把实际需要的计算或同步静默丢掉 |
| [compat.py](https://github.com/sgl-project/sglang/blob/4517a5ff1bbd1f944d6bde1819300ed14af7b698/tools/sglang-simulator/src/sglang_simulator/compat.py) | 配置单worker、CPU attention及关闭overlap/CUDA Graph | 是S9.1参考；它把TP/EP/PP等运行拓扑固定为1，不是多worker已完成的证据 |
| [scheduler.py](https://github.com/sgl-project/sglang/blob/4517a5ff1bbd1f944d6bde1819300ed14af7b698/tools/sglang-simulator/src/sglang_simulator/simulation/sglang/scheduler.py) | 包装真实收请求、选批、run_batch及结果处理 | 保留原生控制切口；共同执行器提供时间和完成，不搬入整批时延推进或host墙钟CPU开销 |

仅复用需要的方法。Simulator自己的预测器、统计平台、HiCache模拟和版本兼容层不随之引入。当前框架与参考代码的池初始化接口已有差异，例如本地没有其挂钩的`KVCacheConfigurator`；先核对固定版本的对应调用，不为安装Simulator而自动升级框架、放宽已有来源资格或维护多个内部兼容版本。

另一份[debug_utils/schedule_simulator](../../../third_party/sglang/python/sglang/srt/debug_utils/schedule_simulator/simulator.py)使用独立GPUState/SimRequest及FIFO，不能作为真实Scheduler接管依据。[手工SchedulePolicy检查](../../../third_party/sglang/test/manual/test_schedule_policy.py)可参考有效参数构造，但只覆盖局部组件。

### 硬件替代的允许范围

允许有明确用途的stub/mock/hook：kernel加载入口、模型载荷、CPU索引元数据、模拟stream/event/通信句柄。它们是正式无设备后端的一部分，不是用测试假对象替代业务。目标是什么、返回哪些有效值、完成条件是什么，必须在对应adapter内清楚表达。

不允许替代原生选批决定、伪造资源容量/命中、把在途通信统一立即完成、用占位token冒充自然内容或把空对象拼凑成“闭环通过”。不按是否出现`sys.modules`或mock字样判定合理性；以是否保留需要的控制、资源和完成语义判断。

### 当前环境证据与实现前提

此前四个无设备探针表明现有Ascend推理镜像在普通导入时受`torch_npu`检测阻碍；CPU开关和关闭Torch自动加载不足以解决Transformers再次导入NPU backend的问题。证据在`data/20261028T144023.928299Z_s9-native-control-feasibility/`，精确费用和状态留在status/log。探针没有安装Simulator hooks，不能用它的失败否定上游无设备方法。

后续隔离探索确认：移除torch-npu插件，向Triton声明期提供显式拒绝实际benchmark的driver，并替换sgl_kernel二进制loader后，上述原生模块可无设备导入。Triton-Ascend当前版本在构造Autotuner时仍访问driver，仅传do_bench参数不能阻止硬件初始化。该边界只承接未执行kernel的声明；实际模型执行仍须交给共同图执行，不能把拒绝函数改成静默成功。

进一步探索已执行真实Scheduler构造和三个请求的原生选批/结果生命周期。原生分页分配仍会调用Triton索引kernel；探索使用上游alloc_extend_naive完成CPU索引计算，容量和页账仍由原生管理。请求参数须沿SamplingParams.normalize/verify形成有效输入。该探索的模型完成结果明确为给定token，未接Markov图或核心资源，不作为原生预测交付。正式资源facade须以核心返回的索引取代这份探索性原生页账，而不是把参考池搬进正式控制包。

先以固定上游和必要CPU依赖准备控制环境，再逐项接入明确硬件替代。影响决策的backend条件、页顺序、dtype和PP/PD约束仍须与目标来源核对；CPU运行只用于执行控制和元数据，目标计算/通信继续由已采模板表达。原生真实模型、设备、网络和目标批次答案不进入纯预测。

### 时间与等待

核心维护唯一模拟时间；到达、工作完成和必要控制计时按对应事件推进。外层预算/watchdog和实际处理开销仍用真实时钟，不全局替换`time.monotonic/perf_counter`。原生代码实际运行多久不能直接当目标CPU成本。

单worker先关闭范围外overlap、超时、抢占和自适应chunk。多worker阶段再接真实等待关系，只挂起相应参与者。资源不足可能进入原生`retract_decode`，不能因关闭priority preemption就假设它不存在；按该步实际范围验证或保留限制，不靠超额容量掩盖。

## 9. 按什么顺序实现

**S9.1单worker → S9.2 TP/EP → S9.3 PP/PD → S9.4维护交付。** 每步先收口实现，再做必要开发复核；通过后进入下一步，不在新场景验证中逐条补规则。每步检查实际修改及关联调用的放置位置、唯一状态和可删除路径；S9结束完成整体关联审查、整理及分层提交推送。完整独立矩阵留给S11。

### S9.1：单worker原生控制闭环

1. 核对阶段起点及S8遗留修正的交付，列出控制、资源、图装配、CLI、采集和回放消费者。
2. 参考Simulator的无设备方法，按固定版本接管导入、模型与设备初始化。先运行真实单worker请求/结果生命周期；局部参考运行若使用原生资源，仅作接管探索，不作为共同资源交付或新增正式后端。
3. 迁移最小KvPool资源服务，轻量和原生经同一服务取得槽位、页、映射及就绪。让实际原生选批/结果处理驱动模板和执行器，变更给定耗时后仍由模拟反馈推进。
4. 仅做TP=EP=PP=1、关闭PD和新增优化的完整请求闭环；基础批次随请求到达/结束发生变化。只实现当前消费者所需的接口，不预铺多worker平台和完整缓存树。

**通过条件：** 正式入口完成到达→原生选批→共同资源/图执行→结果处理→下一批→释放；真实执行原生代码，无设备/网络/目标记录依赖。请求进度、资源身份、等待及完成一致，轻量仍消费同一服务，原生框架/runtime差异留在对应adapter。

导入或单worker生命周期受阻时先修此步，不同时扩写TP/EP/PP/PD。未来优化的原生能力疑点单列，不能因此停止本步已具备条件的基础工作，也不能将疑点算作能力完成。

### S9.2：TP/EP组内协作

1. 在单worker闭环上接TP：沿原生协议保持同组批次决定一致，绑定rank本地资源及collective端口；保留原生所需控制参与者，不让每张卡自行选择不同请求。
2. 再接已具备原生资格的EP配置，使用现有按拓扑给定的token/padding路由，复用模型分片、dispatch/expert/combine模板和角色绑定。不同专家负载不演变成新调度器。
3. 核对代表模板复用与独立参与者的边界：共享结构不等于省掉必要原生控制或实际通信，不修改原生并行算法凑支持。

**通过条件：** 新优化关闭时，选定合法TP与EP案例从同一正式入口完成，组内成员/token一致、分片资源独立、通信及反馈对应、释放正确；单worker保持可用。TP/EP差异集中于原生框架适配和图绑定，共同controller/execution不增加按框架名或特殊rank的分支。完成后进入PP/PD。

### S9.3：PP/PD交接与等待

1. **先PP。** 直接复用原生微批次轮转和结果处理，接管异步send/recv、stream/event及必要proxy元数据；用既有合法多lane案例核对发送、接收、结果消费和缓冲区复用。核心执行依赖，不另写微批次选择策略。
2. **再PD。** 保留原生bootstrap、接收准入、预分配、prebuilt及组批；将传输poll和完成交给图执行器。先P/D各一服务实例、多请求交接，再扩到已有合法异构拓扑。
3. **最后组合。** 在前两项闭环上核对与TP/EP及PP×PD的具名合法组合；沿统一任务、请求、lane和资源身份连接，不为组合复制控制流程。删除被真实控制事实替代的旧装配推测，保留必要图值/存储版本。

**通过条件：** 无提前读取/释放、漏通知或反馈串任务；源等待发送结束、目的等待KV就绪有直接因果证据。PD允许新请求在后续Decode组批时加入，已提交批次不被中途修改；必要无模型清理覆盖。S8案例可作开发回归，不能计入独立验收。

此步处理的是原生通信/等待接口接管，不预判需要重写控制器。若某边界实际需要广泛复制原生流程，记录具体调用链并收紧方案，不通过单worker成功或静默回放宣称此步完成。

### S9.4：审查、维护交付与S10入口

沿Git基线审查合同、轻量/原生、资源、图、执行器、采集、输入/CLI、Docker和验证编排的完整调用链。核对代码放置与依赖方向，删除已替代的状态同步、整批预计算及无消费者分支，保留仍有用途的回放诊断。默认输出简明，明细显式开启。

完成必要Docker构建/公开入口运行、文档与数据整理，按职责分层提交推送；本次文档调整不执行这些未来交付。向S10移交具名基础闭环、来源适用域、S8遗留及未验证范围，由S10实现优化并单独维护交付。S11随后整合再冻结执行完整矩阵，冻结包含共同资源语义和影响决定的原生接管代码；性能建设继续后置。

## 10. 必要验证怎样安排

验证器只消费正式事件、资源快照及原生观察，不选择预测批次，不复制待验分配/缓存算法。开发阶段保留少量能区分错误机制的案例；S11 承接七模型和完整 A–L 矩阵，不能因本表较小而删减原范围。

| 验证主题 | 最小有辨识力的条件 | 检查什么 |
| --- | --- | --- |
| 原生真实性 | 记录实际调用的上游函数及提交/结果恢复顺序 | 直接运行原生控制，无记录回放、函数复制或静默后端切换 |
| 事件驱动 | 改变给定模型/通信耗时，加入执行中到达的请求 | 决定随模拟完成边界变化，host 运行快慢不改变确定结果 |
| 资源一致 | 槽位复用、跨页追加、延迟归还、在途请求结束 | 唯一占用、有效映射、无双重释放；页数守恒和就绪分别正确 |
| TP/EP协作 | 同批成员变化、不同分片/专家负载、给定token/padding路由、组内通信 | 原生决定一致，身份及分片资源不混用，结构复用不省略必要参与者 |
| PP/PD | 多 lane、多成员、交错完成、源先结束而传输未结束 | 正确结果唤醒、资源保护和目的准入；无模型清理也覆盖 |
| 退出/失败 | 所属原生进程失败、无可推进事件、正常结束 | 正式失败、所属资源清理和预算结算，不保留悬挂进程或无限 polling |

独立对照分两类证据：相同条件下原生决定/资源轨迹，以及这些决定对应的目标执行结构。原生控制改善了策略一致性，但模拟时延仍会改变到达窗口与组批，不能要求所有给定耗时运行的批次都等于一次真实 wall time trace；需要固定输入条件的局部对照和完整因果/生命周期核对共同支持结论。对照记录只用于验证，不输入正式预测来获得正确答案。

本项目尚未证明完整无设备闭环、共同资源及各并行接管的运行资格。上游Simulator提供可参考的方法，已有导入失败仅说明原镜像的直接导入受阻；按上述顺序取得实际证据后再升级支持声明。
