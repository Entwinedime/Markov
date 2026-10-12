# 首轮框架路径与能力条件

当前主线为 SGLang + Qwen3-32B，KT 预测在主线稳定后扩展。SGLang 已有真实采集和限定模块预测，KT 已有原生采集及 source facts；完整资格与阶段状态见[实施计划](../redesign-workflow/2026-10-08_redesign_execution_plan.md)。本页保留源码、资产来源与待补充的事实。

## 适配组合与源码归属

适配器支持的是实际实现组合；下面的目录不能自由排列组合成支持承诺。框架和环境的真实版本由子模块/安装脚本和采集环境记录确定，不维护项目内部 v1/v2。

| 组合/职责 | 当前归属 | 资格边界 |
| --- | --- | --- |
| SGLang + Ascend 安装 | `src/collection/adapters/frameworks/sglang/ascend/install.sh` | 安装镜像内官方子模块副本，使用 NPU 包元数据；观察/模板主线仍限定 Qwen3 及既定 serving 路径 |
| KT archive BalanceServe + Ascend 安装 | `src/collection/adapters/frameworks/ktransformers/ascend/{install.sh,runtime.py}` | 同目录拥有归档依赖、ARM 构建、attention 默认值及既有运行修正；已有 TP=1 采集不代表 KT 预测已实现 |
| torch_npu workspace 观察 | [runtime 适配目录](../../src/collection/adapters/runtimes/ascend/torch_npu/README.md) | 固定 wheel/源码组合的采集插桩；当前由 SGLang Dockerfile 安装，不因 KT 同用 Ascend 而默认适用 |
| CANN SDK 观察 | `src/collection/adapters/runtimes/ascend/native` | 两框架现有采集入口加载标量观察库，SDK 符号/事件解释仍受实际 CANN 环境约束 |

`docker/install/{sglang,ktransformers,torch_npu_workspace}.sh` 仅调用这些实现；镜像选择和 COPY 在 Dockerfile。框架版本改变可能影响模块和方法位置，torch_npu/CANN 改变可能影响分配器、ABI 和 profiler 格式，须在对应适配器调整并重验。单个组件安装成功不能证明组合的模型路径、语义记录或模板复用正确。

## 1. S0/S1 环境核对记录

下表是 2026-10-08 的启动核对，镜像缺失、设备空闲及未加载模型等描述属于当时状态。2026-10-09 已取得两个框架的真实采集；当前运行结果以执行记录与明确资产为准。

| 项目 | 当前事实 | 限制与恢复条件 |
| --- | --- | --- |
| 工作区 | `/home/markov-next`，`refactor/modeling-redesign`；旧项目在 deprecated，README 为 0 字节 | 保留现有归档、Docker 与子模块改动，不提交或清理历史资产 |
| 原仓库 | `/home/markov` 为 main，HEAD `d1d26a6384a93e0b66cac90d76bc0259572ef98e` | 本轮只读确认，未修改 |
| SGLang | 官方 `0460f277b770f8b5a404da2e50e7a9573ef8db40`，工作树干净 | 运行源码位于镜像内 `/opt/markov/sglang`；不是 mounted fork |
| KTransformers | 官方 `f1e2b82c74ce777f636215021047c4dc378f4f64`，工作树干净 | 使用官方 archive Ascend runtime；镜像内保留已授权构建适配 |
| 设备 | npu-smi 可见 8 张健康 910B3，每卡 65,536 MiB；本次没有设备运行进程 | 不证明权重加载、通信拓扑或采集可用；S3 开始前重新检查占用 |
| 宿主 CPU/内存 | aarch64，nproc 可见 192 个逻辑 CPU，物理内存约 501 GiB，核对时 available 约 459 GiB | KTransformers 的 100 个 CPU 推理线程是示例部署参数；NUMA、绑核和真实专家性能仍需记录/测量 |
| 稠密模型 | `/models/Qwen3-32B`，Qwen3ForCausalLM，BF16，64 层，权重约 65.52 GB；config、tokenizer 和 shard index 可读 | 总权重接近单卡容量，首条路线固定 TP=2；实际权重加载与 KV 预算仍需实机验证 |
| 混合模型 | `/models/DeepSeek-R1-w8a8-q4km`，DeepseekV3ForCausalLM，61 层，config/tokenizer/混合 safetensors 可读 | 目录与官方合并权重教程形态一致；仅凭文件名不能证明 W8A8/Q4 布局和优化规则匹配 |
| Docker | Compose 可解析出 sglang-profile、ktransformers-profile、modeling；核对时两个框架镜像均缺失 | 需按现有 Dockerfile 恢复镜像，再验证 import/原生入口/模型推理；历史 import 成功不能替代 |
| modeling | 本次由既有 Dockerfile/cache 构建，并实际执行 clang 18.1.3、CMake 3.28.3、libstdc++ 13.3、nlohmann-json 3.11.3 | Docker events 再次出现镜像 untag/delete，发起者不明；不修改全局 daemon 或其他任务。镜像被清理后重新构建 |
| Compose 网络 | 默认 markov-next_default 同名网络重复，首次 run 被 Docker 拒绝 | 自有入口固定独立 project `markov-next-redesign`，不删除现有网络 |

当前合法历史来源是原仓库 `/home/markov/data`：profile_runs/sglang、calibration、modeling_inputs 等。其用途先按 deprecated/docs/work_progress.md 与 data/README.md 判断；target score 只供评价。KTransformers 旧 framework smoke 是 hook/DAG 输入，不能证明完整模型路径。旧产物不符合新合同，S3 需要显式重采或一次性导入任务，本轮没有打开 target trace/score，也没有清理数据。

## 2. 第一条 SGLang 路线

选择 Qwen3-32B，Ascend attention，TP=2 固定，FCFS，关闭 prefix/HiCache、调度 overlap、graph replay、混合与分块 prefill。冷状态指请求 KV 为空；模型已加载并完成正式窗口外的预热。固定输出长度，开放请求到达。

完整配置见 [base](../../configs/redesign/sglang/base.json) 与 [target](../../configs/redesign/sglang/target.json)。第一项目标变化为 max_running_requests **2→1**。它改变准入、batch 和 shape；权重、后端、TP、线程和量化固定。先在 S3/S4 证明所需模板/变体、shape 映射、通信工作及依赖，再用给定节点耗时验证动态展开；独立成本在 S5 确定，不能因路径相同自动迁移。

| 候选流程动作 | 官方源码入口 | S3 所需事实 |
| --- | --- | --- |
| 到达与调度 | [event_loop_normal / get_next_batch_to_run](../../third_party/sglang/python/sglang/srt/managers/scheduler.py:1506) | 原生接收时间、等待/运行集合、选中请求、预算、分支和 batch 身份 |
| prefill 选择与准备 | [get_new_batch_prefill](../../third_party/sglang/python/sglang/srt/managers/scheduler.py:2735)、[prepare_for_extend](../../third_party/sglang/python/sglang/srt/managers/schedule_batch.py:2008) | selected 在 prepare 调用前的边界；长度、KV 位置、输入 shape、嵌套 CPU 工作所有权 |
| decode 选择与准备 | [update_running_batch](../../third_party/sglang/python/sglang/srt/managers/scheduler.py:3028)、[prepare_for_decode](../../third_party/sglang/python/sglang/srt/managers/schedule_batch.py:2606) | 活跃请求、context 长度、KV 增量、重撤/内存压力分支 |
| 执行与提交 | [run_batch](../../third_party/sglang/python/sglang/srt/managers/scheduler.py:3178)、[forward_batch_generation](../../third_party/sglang/python/sglang/srt/managers/tp_worker.py:482)、[Qwen3 实现](../../third_party/sglang/python/sglang/srt/models/qwen3.py) | CPU launch、NPU/HCCL/拷贝、stream/rank、提交关联、结果拷贝与采样；独立 submitted/device_done |
| 完成处理 | [process_batch_result](../../third_party/sglang/python/sglang/srt/managers/scheduler.py:3400)、[prefill/decode 结果处理](../../third_party/sglang/python/sglang/srt/managers/scheduler_components/batch_result_processor.py:178) | token 发布、first/last token、KV 释放；同步等待与 CPU 服务分开 |

现有候选轻量规则为：空闲时按 FCFS 选择可准入 prefill；否则推进已活跃 decode；不同 phase 不混合，每 replica 同时一个 batch 在途。首轮为每请求预约输入加固定输出，并按实际 KV page size 向上取整，要求所有预约能在声明池内完整保留，避免 retraction/抢占。page size 来自 base 的有效配置。此容量保守规则、入口快照与同刻顺序是明确近似，S3/S4 需核对真实路径，出现影响分支的差异时限制支持关系或实现有依据的规则，不能按成绩补偿。

上述源码位置是观测候选，需按[统一插桩合同](04_collection_templates.md)补齐阶段范围、工作关联和端口事件：selected 可能位于父 schedule 内部，model_forward 属于外层执行，跨 rank 计算与通信要有明确关联。适配器将原生身份映射到同一流程语义，核心不按每个框架新增一套事件格式；必要内部 marker 只用于隔离运行副本。

已有真实 prefill/decode、两个 rank 的观测和模块预测。当前优先完善动作边界、内部依赖、参数化模板、合法变体、有限规则及给定耗时展开，核对首次合批差异；节点成本和测量域后置。plan 的 conditional 是静态范围声明，实际结构和性能资格分别判断，采集成功或局部 complete 不代替完整验收。

## 3. KTransformers 后续扩展路线与差异

2026-10-09 调整开发顺序：先完成 SGLang + Qwen3-32B 的预测闭环与验收，再扩展 KT。本节保留已有源码分析、采集入口和真实资产，KT 模板、控制与成本不作为主线 S3–S7 的完成条件。

选择本地 DeepSeek 混合权重、官方 archive 的 Ascend balance_serve 路径、TP=1 固定。[官方教程](../../third_party/ktransformers/doc/zh/DeepseekR1_V3_tutorial_zh_for_Ascend_NPU.md:155)使用 model_path、gguf_path 和 optimize_config_path。本项目主路径来自 model.path，另外两个通过 model_assets 提供，不复制权重。

| 步骤 | 官方入口 | 与 SGLang 的差异 |
| --- | --- | --- |
| 准入/选择 | [原生 scheduler](../../third_party/ktransformers/archive/csrc/balance_serve/sched/scheduler.cpp:429)、[update_last_batch 的调用](../../third_party/ktransformers/archive/ktransformers/server/backend/interfaces/balance_serve.py:323) | 原生 C++ scheduler、mini batch 与 prefill chunk 规则；max_batch_size 不是 SGLang 的同名准入上限 |
| 准备 | [QueryManager.add_query / update](../../third_party/ktransformers/archive/ktransformers/server/balance_serve/inference/query_manager.py:122)、[ForwardBatchInput](../../third_party/ktransformers/archive/ktransformers/server/balance_serve/inference/forward_batch.py) | page/block table、不同 phase 的 mini batches 与 padding |
| 执行 | [run_split / sync](../../third_party/ktransformers/archive/ktransformers/server/balance_serve/inference/model_runner.py:349) | NPU prefill/decode 分开，decode graph replay 及后台 graph update 是当前原生路径的一部分 |
| CPU 专家 | [Ascend experts](../../third_party/ktransformers/archive/ktransformers/operators/ascend/ascend_experts.py:40) | router → D2H → CPU submit/sync → H2D → 合并；专家分布依赖内容，不能仅凭长度生成 |
| 结果发布 | [loop / sampling](../../third_party/ktransformers/archive/ktransformers/server/backend/interfaces/balance_serve.py:323) | 原生可能在同步和 sampling 前请求 next_batch；需要记录独立的选择、执行完成与更新身份 |

示例 [base](../../configs/redesign/ktransformers/base.json) 与 [target](../../configs/redesign/ktransformers/target.json) 为 max_batch_size 4→2，**当前不在可预测关系内**。源码会改变 mini batch/chunk 组织，且专家路由需要目标适用的来源。保留同一输入与接口能力，不以一个 NPU hook smoke 宣称框架预测已支持。

后续取证任务：基于已有真实 prefill/decode 采集继续确认路由/专家负载、CPU 任务身份、DMA 和 graph update/replay 的依赖。先决定哪些工作可在固定路由条件下回放；变化目标若不能取得合法路由规则，则明确 unsupported。长度合同不足时再增加有实际生产者/消费者的业务资产字段，不复活旧 forced-token 或 checkpoint 接口。

2026-10-09 接入自有 [KTransformersCapture / 原生观察器](../../src/collection/markov_collection/adapters/frameworks/ktransformers/capture.py)，当前限定 Ascend BalanceServe TP=1。采集调用官方参数解析、BalanceServeInterface、Engine.loop 和调度 RPC，按实际 mini batch 保存混合阶段和流水线关系；源码 wrapper 不改生成 token 或调度分支。prefill 专家由优化规则的 KExpertsTorch 在 NPU 执行，decode 使用 KExpertsCPUW8A8 的 CPU 提交/同步及 NPU 图 host callback；不能将两者当成相同 CPU 路径。标准步骤映射、专家路由及目标成本资格仍待完成。

本地模型头部核对为 62 个 safetensors、414.448 GB，首 shard 含 output/token embedding 张量；这只证明资产形态，实际加载和运算结果由执行记录证明。镜像在 Ascend 下不再导入 CUDA 专属模型及 flashinfer.sampling，适配只位于镜像内，未改官方子模块。两个 SDK 的观察库均使用各自实际头文件；失败 attempt 的预算不会因修正镜像/构建而清零。

## 4. 回退官方后需要重建的观测

| 旧依赖 | 官方状态与新处理 |
| --- | --- |
| forced token/replay、checkpoint 与 fork 自定义计时 | 不再视为原生能力；本轮稠密模型按长度条件化，数据依赖模型先声明限制；新插桩独立适配 |
| HiCache 快照、内部 ledger 和自定义事实 | 首轮关闭 HiCache；将来支持时逐项依据官方状态与 API 定义，不复制旧快照协议 |
| 旧 AscendCL hook | 运行库层面的 launch/stream/memcpy 观察思路可复用，但没有请求、batch、步骤和专家语义；不能单独产生模板 |
| wrapper 时间区间 | 必须补唯一工作身份、父子归属、异步提交与完成端口、shape/layout 和跨 rank 关联；不靠区间猜子图 |

插桩优先自有 Python wrapper 与真实 Ascend runtime hook；selected 等内部边界确需源码 marker 时，仅作用于隔离运行副本。S0–S2 不修改两个官方工作树，不复制 deprecated 入口。
