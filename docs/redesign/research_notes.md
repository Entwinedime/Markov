# 相关工作与本项目取舍

本页汇总讨论中实际查阅的外部论文与公开实现，用于说明 CPU 时延、原生 scheduler 和 trace 变换的不同方法。资料是设计参考，不是 Markov 的运行或准确性证据。以下结论限于本轮查阅的论文和代码。

## 1. Vidur：按阶段估计 CPU 开销

公开实现对 schedule、sampler、prepare inputs、process outputs 和 Ray 通信分别建立模型，所查实现使用 batch_size 作为这些 CPU 模型的输入。成本组合进入执行时间；配置提供跳过 CPU 建模的开关，所查默认值为开启跳过。[成本代码](https://github.com/microsoft/vidur/blob/main/vidur/execution_time_predictor/sklearn_execution_time_predictor.py)、[配置](https://github.com/microsoft/vidur/blob/main/vidur/config/config.py)、[执行时间](https://github.com/microsoft/vidur/blob/main/vidur/entities/execution_time.py)

官方文档说明 CPU 采集涉及调度、采样与 detokenization，并与具体框架实现相关。论文讨论了小模型 CPU 开销导致的误差。[采集说明](https://github.com/microsoft/vidur/blob/main/docs/profiling.md)、[论文](https://www.microsoft.com/en-us/research/wp-content/uploads/2024/05/vidur_mlsys24.pdf)

对 Markov 的启发是按明确控制阶段测量，而不是模拟每条 CPU 指令。不能据此假定我们所有 CPU 工作仅依赖 batch size，或者关闭 CPU 建模就能覆盖完整 E2E。

## 2. Dooly / DoolySim：操作发现与原生调度复用

Dooly 从执行中追踪输入维度来源，按可独立运行程度选择操作或模块测量，避免父子重复覆盖；DoolySim 使用原生 vLLM 调度逻辑和估计的工作耗时。[论文](https://arxiv.org/html/2605.07985v1)、[项目](https://github.com/dooly-project/doolysim)

所查 DoolySim 代码对请求加入和输出处理使用宿主机 perf_counter 计时，输出处理有固定开销回退。每步累加预测 GPU、输出处理与本轮输入处理时间；同一循环没有把 scheduler.schedule 本身的宿主执行时间显式加进该公式。[执行代码](https://github.com/dooly-project/doolysim/blob/main/doolysim/core/dooly_sim.py)

因此复用原生决策和完整 CPU 时间建模是两项能力。Markov 借鉴性能粒度可调整、原生控制可适配的思路，但首轮不采用宿主计时替代目标 CPU 模型，也不把自动语义发现当成已经具备的能力。性能采样按[本项目四项约束](04_collection_templates.md#采样方法的四项约束)选择载体，不默认移植逐算子/模块独立执行及元数据重建；原生请求中采集多个性能单元也是候选途径。

### 2026-10-10 论文与源码复核

官方 DoolyProf / DoolySim 已加入 reference 子模块，固定提交及代码位置见[调研记录](../../reference/README.md)。代表 request 发现结构、参数来源驱动工作量变化、配置间复用测量，作为 Markov 的基础采集流程；不把最优代表配置选择作为核心研究难题。

当前 DoolyProf 去重签名与论文有差异，已移除原始 kernel 符号参与的两项，避免随 token 数变化的特化导致错误拆分。性能签名不证明 DAG 等价。公开主入口为 vLLM，不能据论文框架范围宣称已有 SGLang + Ascend 适配。

Markov 的潜在贡献是依赖、资源与多种完成事件参与动态展开，需通过独立证据证明价值；语言或硬件支持不单独构成方法优势。

## 3. Daydream：包含 CPU/GPU 的依赖图变换

Daydream 面向 DNN 训练，通过低层 trace 建立 CPU、GPU 与通信依赖图，用图变换表达优化，再模拟运行时间。[论文](https://www.cs.utoronto.ca/~pekhimenko/Papers/Daydream-ATC_20.pdf)

它说明从真实 DAG 进行受控变换是有明确用途的路线。Markov 的 serving 场景还需要处理动态请求、组批和时间相关分支，因此不把单个 base DAG 直接视为完整目标控制器。

## 4. 本轮控制路线的选择

| 路线 | 主要特点 | 当前取舍 |
| --- | --- | --- |
| 完整实现控制器 | 自建完整框架决策与状态管理 | 项目规模过大，不作为目标 |
| 轻量控制器 | 对声明范围实现规则，使用真实模板动态展开 | 首轮正式路线 |
| 完全不实现控制器 | 重放 base 已发生顺序 | 可用于同配置诊断，不作为通用跨配置预测 |
| 原生代码 | 复用原生决策，经适配器替换执行和时间交互 | 接口稳定后的可选扩展 |

最终顺序是先统一接口、再实现轻量控制器、之后有需要再接原生适配。采集、语义和性能的功能划分继续保留，但不由此推导固定的“两层”或“三层”软件目录结构。
