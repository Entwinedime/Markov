# 外部参考源码与 profiling 调研

此目录用于源码参考，不是 Markov 的运行依赖。Dooly 调研于2026-10-10，LLMServingSim 调研于2026-10-14；均为暂停 redesign 执行期间的独立静态阅读，不代表选定或实现新采集方案。

## Dooly

### 来源与复现

- [论文](https://arxiv.org/html/2605.07985v2)：Dooly: Configuration-Agnostic, Redundancy-Aware Profiling for LLM Inference Simulation，2026-05-21 修订稿。
- [DoolyProf](https://github.com/dooly-project/doolyprof)：子模块 `doolyprof`，提交 `0538d443a314d818e656bc44cf11778a4b7fa203`。
- [DoolySim](https://github.com/dooly-project/doolysim)：子模块 `doolysim`，提交 `a22563082e97e1af78f0f0b643441d8e43b442da`。

两仓库均保留上游源码及 MIT LICENSE。使用 `git submodule update --init reference/doolyprof reference/doolysim` 获取锁定提交。本次仅静态阅读，未安装依赖、启动模型或验证 Ascend 可用性。

### 对采集路线的结论

Dooly 更接近“独立于目标 workload 的参数化资产库”。但是配置无关描述的是提取/采样方法及跨配置复用能力，不表示一份固定配置的完整执行图能直接用于所有配置。论文第 3、6 节为待探索配置提取操作集合，再识别已有测量；新配置可以复用共有操作，新增操作仍需要采样。

其一次 trace 主要用于发现调用结构和参数来源，随后仍要扫描 workload 相关参数进行性能测量。论文附录 D 对 prefill/decode 的处理依靠重新设置有状态模块的 attention metadata，不能据此认定一次 prefill 捕获的细粒度 kernel DAG 覆盖 decode 的所有路径。

### 源码中实际采用的机制

| 机制 | 当前源码依据 | 对 Markov 的意义 |
| --- | --- | --- |
| 小输入发现结构 | [run-tracer.py](doolyprof/doolyprof/run/run-tracer.py) 的默认输入为重复短 prompt，batch=5、max_tokens=1；入口调用 vLLM LLM.generate | 结构发现 workload 可以独立于待预测 workload |
| 参数来源传播 | [tracer/config.py](doolyprof/doolyprof/tracer/config.py)、[propagation.py](doolyprof/doolyprof/tracer/propagation.py)；[input_generator.py](doolyprof/doolyprof/profiler/input_generator.py) 据标签替换 token/request 维度及混合维度 | 学习维度的来源和替换规则，而非逐长度保留不可变实例 |
| 可变性能粒度 | [resolver.py](doolyprof/doolyprof/profiler/resolver.py) 的 ModuleResolver 尝试独立执行，失败时向父模块寻找可运行范围并吸收子项 | 性能单元可以细于语义 block，也可因上下文需要合并；避免父子重复计费 |
| 操作复用 | [module.py](doolyprof/doolyprof/profiler/module.py) 的 _module_signature 与 _get_model_param_signature | 区分固定语义、动态维度和模块状态，不能仅凭名字或 shape 去重 |
| 动态调度与时间估计 | [dooly_sim.py](doolysim/doolysim/core/dooly_sim.py) 调用 scheduler.schedule；[simulation_predictor.py](doolysim/doolysim/core/simulation_predictor.py) 以预测时延乘操作次数并累计通信 | 其主要消费方式是批次时延估计，不是 Markov 当前要求的逐事件依赖图展开 |

论文与当前代码存在差异：论文第 6 节的签名包含 GPU kernel 符号；当前 `_module_signature` 已去掉原始 kernel 符号部分，采用操作名称、模型参数及语义后代结构、模块状态、dtype。代码注释解释，autotuner 的 tile/prefix/format 特化会随 token 数改变，直接比较符号会造成错误拆分。因此不能照抄论文签名，也不能把性能签名相同等同于细粒度执行图完全一致。

论文讨论 vLLM 和 SGLang；当前检出的主入口、上下文重建和模拟器代码使用 vLLM，两个 Python 源码目录检索未发现 SGLang 适配。应区分论文验证范围与公开提交已经提供的实现，不能宣称可以直接接入本项目的 SGLang + Ascend。

当前 DoolySim 的 `_estimate_process_outputs` 在模拟宿主机计时真实输出处理，失败时使用固定开销；代码还包含缺预测时的固定时延回退。这些方式与 Markov 的成本来源和未知值处理约束不同，不作为移植方案。Dooly 的签名哈希也不构成本项目新增摘要机制的理由。

### 已纳入设计的取舍

采用“代表输入发现结构 → 参数化模板与适用条件 → 独立性能采样 → 目标 workload 驱动展开”的组织方式。先在固定模型/框架/后端及明确配置变化范围内证明复用，再扩展配置域。

语义模板继续保存控制端口、依赖和资源语义；性能单元单独选择粒度与测量复用条件。参数化模板需要携带维度来源及有限变换规则，不能只保存具体 token 长度。跨配置时重新确认操作结构，复用已经证实等价的单元；未覆盖分支明确报告。调研当时的270个采集实例只是原始证据，不能据实例数量判断参数化模板库是否完成；当前状态只见实施计划。

后续讨论已确定固定模型 × 配置集 × request workload 集的输入合同，代表采集参考 Dooly，不作为核心研究难点；详见[实施计划](../docs/redesign-workflow/2026-10-08_redesign_execution_plan.md)。仍保持结构先行、成本后置，执行目标保持暂停。

## LLMServingSim 2.0

### 来源与复现

- [官方仓库](https://github.com/casys-kaist/LLMServingSim)：子模块 `llmservingsim`，锁定提交 `b09c7aaab4b072d73136641841cc2688616c49eb`，提交日期2026-10-06，对应上游发布 `v1.2.0`。
- [论文](https://arxiv.org/abs/2602.23036)：LLMServingSim 2.0: A Unified Simulator for Heterogeneous and Disaggregated LLM Serving Infrastructure，ISPASS 2026。
- “2.0”是论文/系统名称；本次读取官方当前实现，不是 `ispass26-artifact` 的论文原始快照。结论限于锁定提交；上游当前 profiler 面向 vLLM 0.28.0 / CUDA。

保留上游源码及MIT LICENSE。获取命令为 `git submodule update --init reference/llmservingsim`；本次不递归初始化其ASTRA-Sim，不安装依赖或运行GPU采样。

### 实际 profiling 路径

```text
模型配置与操作目录 → Category生成工作点Shot
→ 轻量vLLM实例 → collective_rpc("fire")进入worker
→ 构造SchedulerOutput及KV/请求状态 → model_runner.execute_model
→ layerwise_profile与CUDA活动归属 → 按工作特征保存CSV
→ 仿真批次查表/插值 → 按模型层组成执行图与通信任务
```

| 机制 | 源码依据与实际边界 |
| --- | --- |
| 控制采样特征 | [categories.py](llmservingsim/profiler/core/categories.py)与[batch.py](llmservingsim/profiler/core/hooks/batch.py)：Dense按token数、PerSequence按成员数、Attention按查询/KV/Prefill与Decode组合、MoE按token与专家条件组织工作点。Shot直接声明新token与历史token，不通过完整请求序列反推 |
| 减少模型启动和执行开销 | [config.py](llmservingsim/profiler/core/config.py)、[engine.py](llmservingsim/profiler/core/engine.py)、[runner.py](llmservingsim/profiler/core/runner.py)：dummy权重、eager执行、最少覆盖所需模块类型的层数；同实例测多个点。普通TP条件默认在单GPU上按分片调整配置，不能当成真实多rank完整运行 |
| 保留真实模型实现 | [extension.py](llmservingsim/profiler/core/hooks/extension.py)：worker扩展调用原生model_runner；每点先预热，再重复执行并经vLLM layerwise_profile测量。没有逐算子复制模型实现 |
| 对应模型语义 | [qwen3.yaml](llmservingsim/profiler/models/qwen3.yaml)、[stack.py](llmservingsim/profiler/core/stack.py)、[timings.py](llmservingsim/profiler/core/hooks/timings.py)：目录用类/祖先及kernel匹配实际profile节点；共同stack解释层组合。一次测量可取得多个操作成本，需核对覆盖与父子重叠，模块树不自动等于profile树 |
| CPU与GPU计时分开 | [cuda_timing.py](llmservingsim/profiler/core/hooks/cuda_timing.py)：CPU scope和launch关联用于归属；每次调用按设备求CUDA活动区间并集，再做调用级归一化。排除CPU时间与活动间空隙，不能把该CSV当成完整调用墙钟成本 |
| 明确维护原生状态 | [history.py](llmservingsim/profiler/core/hooks/history.py)、[dummy_cache.py](llmservingsim/profiler/core/hooks/dummy_cache.py)：分配block IDs、初始化已用KV页、区分prompt/output、清理请求及排空异步输出。history只建立合成历史状态，不执行完整历史Prefill，也不保证真实KV内容；批次入口的控制力来自这些维护工作 |
| 计算与通信分离 | [trace_generator.py](llmservingsim/serving/core/trace_generator.py)读取层级CSV，按目标批次插值并组装模型层；[hardware.py](llmservingsim/profiler/core/hardware.py)另测硬件与NCCL链路，网络交给ASTRA-Sim。计算CSV不能替代CPU、网络及存储时延模型 |

### 对 Markov 的取舍

值得借鉴的是**按性能单元的工作特征设计采样域、同次运行复用启动、保持实际执行底层、明确成本归属及覆盖**。这与语义block可以较粗、成本单元可以较细的约定一致。缩层/dummy权重及TP模拟只是其采样假设，不能未经资格判断移植到我们真实serving模板的成本来源。

它没有消除直接调用ModelRunner的状态维护：KV页、请求状态、异步输出及版本接口仍由适配代码负责。因此不据此替换Markov现有原生serving采样。若以后探索可控批次入口，须先证明状态由框架拥有或存在足够简洁的原生载体；不能把整套维护逻辑搬进核心。

本轮只完成源码参考注册和上述路径复核，没有验证精度、Ascend迁移、完整CPU成本或Markov接入，也没有改变实现计划及暂停状态。
