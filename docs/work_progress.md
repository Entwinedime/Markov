# 当前工作进展

更新时间：2026-09-15

## 最新重点：完整 E2E 尚未达到要求

保留 residual gap 和未归属成本后，将已有预测的 `simulated_e2e_us` 与对应 target 的 HTTP 正式窗口实测比较：

| 已有预测面板 | 完整 E2E WAPE | P90 误差 | 最大误差 |
| --- | ---: | ---: | ---: |
| 09-12 旧 60 格 | 21.98% | 46.46% | 67.39% |
| 09-14 新 workload 12 格 | 5.50% | 14.62% | 15.19% |

这不是当前代码重新预测全部 72 格的成绩。直接使用 source 实测耗时作为 target 预测时，两组 WAPE 分别为
19.75% 和 5.07%；当前完整预测的整体误差尚未优于这个简单参照。对照仍在开启采集的环境下，不代表关闭 profiler 后的部署性能。

09-15 使用当前 C++ 库，对五份已有 profile 做了原始 DAG、同成本阶段变换和诊断性消融。确认两个问题：

- C4/W2 使用同一份实测成本，原始 DAG 为 10.482 秒，仅做 Prefill/Decode 图变换后变成 14.480 秒；
  C2/W2 同样增加约 3.528 秒。保留的 CPU 耗时/等待与新计算节点的衔接需要修复，不能只靠调整成本参数。
- C4/W2 原始 DAG 将已识别的 Prefill/Decode 设备成本全部清零后，总时间仍不变：CPU lane 的实测间隔仍撑起原耗时。
  长 scalar CPU 调用与设备执行高度重叠，但其真实 CPU 工作和阻塞等待尚未完成拆分；不能按事件名直接清零。

此外，HiCache 状态处理使用扣除 residual gap 后的 source 事件时间，完整回放却保留尚未改写的 CPU 时间；
target 计算变化对后续请求和缓存判断的反馈还需核对。以上是问题定位，不是已经完成的模型修复。

下一步先用 C4/W2 等少量样本修复同成本图变换与等待依赖，再统一请求和缓存的时间推进，最后处理剩余成本误差并回归。
不以删除所有 gap、未归属成本或使用 target 成本作为正式预测修复。
数值依据分别在 `data/modeling_runs/hicache_full_e2e_audit_20260914/summary.json` 和
`data/modeling_runs/hicache_full_e2e_root_cause_20260915/summary.json`；详细计划保留在本地
`docs/tmp/hicache_full_e2e_root_cause_and_plan_20260915.md`。这些目录按仓库现有规则不入 Git，关键结论在本节保留。

09-15 随后的实施已修复一个具体原因：CANN 显示进程与框架实际进程不同，使同一线程的长 CPU 调用和内部同步被分到不同 lane。
现在只按明确元数据和唯一线程归属统一运行时 CPU 身份，不合并设备/未知线程，也不调整成本公式。
C4/W2 同成本回放从 14.480 秒降到 10.995 秒，C2/W2 从 17.320 秒降到 14.238 秒；分别仍高于实测约 4.90% 和 3.23%。
一格 C2/W2→C4 正式预测在原参数下从 17.545 秒降到 14.469 秒，完整误差仍为 38.04%，不代表全矩阵通过。
Release/诊断构建、小型 trace 时序检查、既有 I/O 检查和 15 项 Python 测试通过；后续继续查任务线程间隔和阶段提交依赖。
主计划为本地 `docs/tmp/hicache_full_e2e_master_plan_20260915.md`，每轮记录在 `docs/tmp/hicache_full_e2e_development_log_20260915.md`。
新结果在 `data/modeling_runs/hicache_full_e2e_development_20260915/r1_summary.json`。完整 E2E 尚未完成，原历史 60/12 格成绩不追溯修改。

R2 继续修复任务队列等待：已有 enqueue/dequeue 关联时，按线程空闲与任务提交的较晚时间启动任务，
保留实测的就绪后剩余间隔，不把整个等待固定成 gap。原节点/边数量不增加，成本参数不变。
同时修正 HiCache 等待投影，避免对已由依赖表达的等待再扣一次；相关小测试先失败再通过，没有降低边界检查。

| 同一份 profile 的检查 | 原始回放秒 | R2 同成本变换秒 | 相对 HTTP 实测误差 |
| --- | ---: | ---: | ---: |
| C4/W2 | 10.481638 | 10.519666 | 0.363% |
| C2/W2 | 13.791912 | 13.864067 | 0.520% |
| C1/W3 | 9.248454 | 9.267322 | 0.275% |
| TP4 C5/W4 | 30.244560 | 30.357343 | 0.372% |

这些不是四格跨配置预测成绩；相对原图的同成本变换偏差仍都超过 0.1% 目标。旧三份 scope 与 R1 一致；
新 W4 相对 R1 之前的调查由 3.209626 秒变成 3.223095 秒，不能将两轮合并变化归因于单个修复。
正式 C2/W2→C4 已成功应用 patch，预测 14.088068 秒、target 实测 10.481627 秒，完整误差 **34.41%**；
虽较 R1 的 38.04% 改善，仍未优于直接使用 base 实测，配置收益方向也未正确。首次 patch 拒绝的结果已单独保留。
结果汇总为 `data/modeling_runs/hicache_full_e2e_development_20260915/r2_summary.json`。

四份原图清零 Prefill/Decode 设备成本后，完整耗时仍不变。下一轮核对 Gloo 通信、运行时周期查询与业务完成的关系，
以及新阶段节点应接在哪些真实提交/同步点；不能按线程名字删掉它们，也不能把余下问题全称为不可预测的随机 gap。

R3 补上消费线程的 CANN/框架身份合并，并去掉按首次出现 correlation ID 识别提交的旧启发式。
底层 CPU 叶子继承任务关联，保留设备连接；小测试证明提交时间变化能传到设备完成时间，未改成本系数。
C2 同成本回放变为 13.791912 秒，与原图一致；C4/W3 分别为 10.520885/9.268831 秒，较 R2 增加 1.219/1.509 ms，仍未达到同成本目标。
TP4 C5/W4 为 30.358314 秒，较 R2 增加 0.971 ms；四份同成本组合 scope 均不变，汇总见 `data/modeling_runs/hicache_full_e2e_development_20260915/r3_summary.json`。
正式 C2/W2→C4 预测为 13.974329 秒，完整误差约 **33.32%**，仍未优于 base 原值参照，不能宣布完成。

新的诊断已定位到周期查询线程通过事件销毁任务进入共享消费队列，并影响后续业务关键路径。
下一步核对哪些顺序是真正的资源依赖，哪些只是 source 中恰好出现的执行顺序；不按线程名删成本，也不把秒级等待硬拟合进计算或 I/O。
C2 同成本总时间一致仍可能被原始 Gloo 时间线维持，不能作为因果正确的充分证明。

R4 进一步验证了共享 CPU 队列的顺序问题。C4 两个消费线程的全部 CPU 叶子都可对应唯一提交任务；
小反例证明，计算提前后照搬 source 次序会额外等待。局部 FIFO 诊断保留所有任务，原始总时间不变，
清零计算后的主线程结束从固定次序的 9.277440 秒降至 6.521685 秒；完整时间仍是 10.481638 秒，由 Gloo 尾部维持。
这些是诊断，不是新的跨配置预测成绩；正式结果仍为上面的 33.32%。摘要分别为
`r4_queue_coverage.json`、`r4_fifo_critical_path.json`、`r4_raw_flow_inventory.json`，均在当前 E2E 开发产物目录下。
下一步先验证任务重排、补清 Gloo 与业务完成的关联，再决定简洁的正式实现。周期监测线程的源码线索和历史版本限制见本地执行日志，
不能用按名称过滤后台线程替代完整 E2E。

R5 使用已有 scheduler 外层区间核对 CPU 广播与规约的提交/完成关系，发现不能只补广播依赖：
同队列中未关联的规约仍会保留 source 的等待时间。进一步确认部分 `hicache.control.prefetch_progress.self`
包含跨线程通信等待，例如 225 µs 的 self 片段内有 178 µs Gloo 工作，并非全是 CPU 执行成本。
下一步先统一这类等待在归一化、控制成本与校准中的表达，避免重复收费；不通过调系数吸收。
本轮仅有静态分析、机制小测试和局部诊断，正式误差仍为 33.32%。证据为开发目录下
`r5_c2_collective_wait_overlap.json`，具体推进顺序见本地主计划的 R6；完整 E2E 仍未完成。

R6 进一步澄清：现有预取控制校准已经排除 `.self`，没有证据说明上述等待被拿去拟合系数。
实际缺口是跨线程变换不完整：C2→C4 中 442 个 CPU 规约提交被替换为零，对应 Gloo 工作却未变；
418 个重叠等待片段也被清零。等待拆分小测试通过，但只补依赖仍不能恢复目标配置收益，暂不并入正式路径。
下一步将提交、通信工作、等待和返回统一归属，并同时处理目标仍需要的通信；不按名字删除规约。
证据为 `r6_c2_to_c4_collective_ownership.json`，计划已推进到 R7；正式完整误差仍为 33.32%。

R7 的限定通信清理诊断只改善约 42 ms，不能解释秒级误差。R8 因此补了可选的运行时准备标记，
并重新采集一格 C2/W2：39 个请求全部成功、输出 token 全部匹配。在第一个正式请求中，两个 rank 的
2.732/2.880 秒分配空白约 99.97% 被 Triton 准备/装载区间覆盖。预热虽执行过同样的 64-token 内核，
正式请求的空闲页指针对齐不同，仍触发了另一个变体；这类空白不是纯随机噪声。

标记默认关闭，不采 snapshot、不改缓存策略，不作为额外 DAG 工作重复计时。新的原始回放为 13.964155 秒，
HTTP 实测 13.965125 秒；旧 C2→C4 正式回归与 R3 完全相同，仍为 33.32% 误差。
下一步沿现有分配器状态检查：HiCache 变化怎样改变准备的发生条件，哪些代码已在正式窗口前准备好；
不能直接扣掉 2.7 秒或用 target 残差拟合。证据见开发目录的 `r8_base_preparation_summary.json`、
`r8_base_gap_matches.json`，本地主计划已推进到 R9；本次为诊断采集，不是新的完整 E2E 精度成绩。

R9 已将分配器账本独立为运行时组件，保留容量算法，并追踪空闲页索引数组的切片起点：分配推进起点，
非空释放或合并重建数组时归零，仅靠容量对账无法确定时标为未知。目标分配记录按 batch 保存，包含预热
和正式请求；它与逐请求 Prefill 计算记录分开，目前不改变 duration，也尚未用于预测编译/装载成本。
首轮状态诊断中，C2 前六个正式请求的起点为 17，C4 为 0；这还不足以证明目标需要准备几次，
仍需验证目标自己的预热过程。新 base 的完整 self patch 未通过资源依赖检查（`lane_dependencies_exact=0`），
关闭 patch 的状态诊断不能算作 self 通过；该失败尚未解决，不能据此宣布完整 E2E 修复。
提交前复验通过 5 项探针测试、15 项 Python I/O 测试及 C++ I/O、trace 时序检查。

## 09-14 泛化结果

新 workload / 新 HiCache 配置 / TP=4 初步泛化实验已完成：C5 base × W4/W5 × G1/G2/G3，
TP=2→TP=2 与 TP=4→TP=4 各 6 格，合计 12/12 预测、target 采集和独立评分齐全。
不是跨 TP 扩图；新 workload 的 base profile 提供 source DAG/工作量，但不参与参数拟合。

| 当前新面板 | TP=2 六格 | TP=4 六格 | 总体十二格 |
| --- | ---: | ---: | ---: |
| 组合 scope WAPE | 0.276% | 0.389% | 0.317% |
| Scope p90 / 最大 APE | 0.935% / 0.935% | 1.453% / 1.453% | 0.935% / 1.453% |
| HiCache I/O/控制 WAPE | 12.950% | 16.209% | 14.772% |
| 不抵消 I/O 分项 WAPE | 17.463% | 20.486% | 19.153% |
| HiCache / phase 结构严格一致 | 4/6；5/6 | 4/6；5/6 | 8/12；10/12 |

组合 scope 主目标通过，但排除了 residual gap 和未归属成本，不是完整墙钟 E2E；最终仍为 `MODEL_LIMITATION`。
Prefill compute 总体 WAPE/p90 为 1.662%/8.634%，Decode 为 0.151%/0.476%；Prefill 与 I/O 分项仍未达标。
G2/W4 在两种 TP 均暴露完整前缀匹配边界缺陷（预测 Prefill 0，实际每 rank 128 tokens）；
G3 四格的等待/可见性投影尚未对齐，不能把严格结构失败直接说成预测 DAG 漏了等待。
这两项新问题尚未修复；未因新 target 成绩调参或追加校准，保留原始外推成绩。

本轮完成首次正式分配写回的归属修复、可选 NPU profiler 串行导出、TP4/W5 source 原始数据重导出恢复，
并通过机制测试、代表格与十二格正式原生回归。全部十九份普通 profile 请求成功，最终十二份 target 的
独立 phase/I/O 观测 ready，无无效事实、归属冲突或 token 范围错误。早期失败记录仍保留。
正式结果：`data/modeling_runs/hicache_generalization_20260914/generalization_summary.json`。
详细配置、逐 workload/单格结果、参数边界及后续问题见 [主验证文档第 10 节](validation/hicache_validation.md#10-09-14-新-workload--新配置--tp4-泛化结果)，
过程见 [泛化计划](tmp/hicache_generalization_plan_20260914.md) 与 [泛化日志](tmp/hicache_generalization_log_20260914.md)。

以下为 09-12 历史对照结果；没有在本轮首次分配归属修复后重跑旧 60-cross，不与新面板混算。

本阶段完成验证合同与控制边界修复、新 60-cross、独立评分与五格真实成本回放。
普通预测没有调参、补采或缩小评分范围；主要变化是消除旧验证工具造成的错误归因。
见 [本轮计划](tmp/hicache_gap_excluded_causal_plan_20260912.md) 与 [执行日志](tmp/hicache_gap_excluded_causal_log_20260912.md)。

## 1. 09-12 对照状态

“一个固定小型校准 + 一个 base 的 profiles，预测全部 HiCache target”主流程已经完成逻辑修复并执行新的
5-base/60-cross 验证。软件流程和信息边界可用，但严格模型验收没有全部通过：

- 60/60 预测 READY，60/60 位于当前固定校准记录的 I/O 域；
- 60/60 effect shape 与 phase 结构严格一致；原 40 格差异是 target 漏字段造成；
- 排除 residual CPU gap 的组合 scope：WAPE 0.921%、p90 1.981%，总体及五个 base 均通过；最大单格 APE 2.712%；
- Prefill+Decode combined：WAPE 0.694%、p90 2.316%，通过；phase delta 1.383%，54 个大变化方向全对；
- Prefill 单项 p90 3.436%，略高于 3% 门槛；
- HiCache I/O/控制：WAPE 5.186%、p90 32.518%、delta 8.121%；不抵消分项 WAPE 5.995%，失败；
- 最终状态为 `MODEL_LIMITATION`，不是工作流错误，也不是全 gate PASS。

09-12 对照正式结果：

```text
data/modeling_runs/hicache_gap_excluded_causal_20260912/final_60_evaluation/gate_summary.json
```

## 2. 本轮逻辑修复

### 本次修复

- 成本回放不再清零正载荷 Prefetch 的 terminal control；删除过时的 outcome-only 分类，保留真实 CPU 工作。
- 控制节点由操作是否存在决定，不因 duration 为零而消失；避免后台 service 意外成为前台前驱。
- target shape 从同一 target trace 的 Prefetch I/O 完成记录传播 visibility 页数，不用默认 0 冒充观测。
- oracle 先检查相同成本回放恒等性，再比较 target 成本；异常保留具体失败字段，不仅输出 READY。

### 保持的模型边界

- 已执行 I/O 与缓存可见收益分开；状态推进与 DAG 共用 service cost，timeout 等待与后台传输分开。
- DMA 使用设备时钟，存储使用函数墙钟代理，后者可能含内部调度等待。
- 三组件组合 scope 保留必要资源/消费者依赖，排除 residual gap 和未归入组件的 wrapper/probe 成本；不是完整墙钟只减 CPU 空白。
- 分项总量、非抵消误差、适用域和环境信息完整保留，不能通过跨组件抵消或隐式倍率过关。

## 3. 当前正式流程

```text
一个 base 的 3 个 profiles
        +
共享平台物理校准
        +
共享固定小型校准（page 32/128，各 2 次）
        ↓
观测提取 → 简洁成本模型 → source DAG 状态/工作量变换
        ↓
Prefill/Decode 变换 → 原子 DAG patch → 模拟
        ↓
全部预测完成后，独立打开 target profiles 评分
```

target config 在预测时决定预取、写回、容量等行为；target trace 和 target cost 只在最后评分/显式 oracle 中使用，
不会回写模型。当前五个 model build summary 的 `target_inputs`、`target_score_inputs` 都为空。

## 4. 验证证据

- Release 和 validation 全量 C++ 构建通过；`hicache_io_logic_check` 覆盖零/正控制耗时、best-effort/timeout 与 oracle 控制保留。
- 15 项 Python 语义回归和 ruff 通过。
- 固定校准两端点各 2 次成功重复，四类 service 的相对范围均小于 4.3%。
- 5 个 base 各 12 个 cross 全部 READY；60 个 cell 都在固定校准记录的域内。
- 每个 base 选择 1 个 cell，执行 1 次相同成本与 5 次真实成本组合；30 次全部通过，五格恒等性全部成立。
- evaluator 新提取 15 个 target DAG，只在全部预测完成后打开，`parameters_or_capture_plan_changed=false`。
- `current_result_audit.json` 逐值核对 60 格 source manifest 和完整 C++ 模型输入不变；36 格只增加零成本控制边界，耗时不变。

旧 Oracle 的 READY 不能证明成本替换有效：它没有检查相同成本回放恒等性，且会清零 Prefetch 控制成本并改变依赖。
旧约 13% 偏差因此撤回为真实调度误差证据。修复后 C5/W1→C4 完整成本回放误差为 0.0059%；
五格中四格低于 0.01%，C4/W2→C3 仍为 1.272%，保留为尚未单独归因的时序投影残差。
effect shape 与工作量比较正确，仍不等于完整 target 时序同构。

当前 5×3 矩阵没有触发 required/partial capacity gate；这一路径只有机制级证据，尚无本矩阵数据级覆盖。

## 5. 数据资产

当前修复运行根目录：

```text
data/modeling_runs/hicache_gap_excluded_causal_20260912/
```

其中包含：

- 五个 `C*/predictions/`：当前 60 个预测及复现输入；
- `representative/`、`representative_oracle/`：机制代表格和显式成本回放；
- `final_60_evaluation/`：15 个新 target DAG 观测、60 个评分 cell 与正式汇总；
- `identity*/`、`shape*/`、`current_result_audit.json`：根因反例与修复对照。

模型没有重拟合：`data/modeling_runs/hicache_io_logic_repair_20260911/` 中的物理校准、五个 base 的模型、
model build summary、base observations、targets 及其引用的原始 5×3 profiles/固定校准仍是当前必需输入，不能删除。
09-11 预测和更早结果保留为对照，不代替当前验收。

## 6. 结果应如何理解

现在可以正式陈述：

1. 一个固定小型校准加一个 base 的 profiles 能构建并运行所有当前 target 预测；
2. 实际执行 I/O、缓存可见收益、前台等待和后台资源占用已经从概念与实现上分开；
3. 配置决定的 HiCache 工作量/关系在当前矩阵没有不变量差异；
4. 排除 residual gap 的组合 scope 达到本阶段精度要求，但 HiCache I/O/控制分项未达到旧严格门槛；旧调度差异归因已撤回；
5. 当前数字不能称为完整请求 E2E，也不能声称“DAG 完全正确”。

## 7. 09-12 停止边界与当时的后续方向

本轮在看过最终 60-cell 结果后没有修改公式、系数或采集计划，也没有恢复逐 target 补采和 residual correction。
总体及逐 base 组合 scope 目标已通过；HiCache I/O/控制及部分 phase 分项限制保留，不因分项偏差追加经验修正。

当时提出单独研究 target-independent、可复现的调度/资源机制，重点是 best-effort 后台 I/O 如何影响 source
骨架中的资源时序，以及存储函数墙钟能否取得更明确的分解；当时 residual CPU gap 仍 deferred。09-15 的进一步诊断和当前优先级见文首。KTransformers 和 NodeScale
均保留：KTransformers 作为另一目标推理框架，NodeScale 作为默认关闭的可选 what-if，不进入默认 SGLang HiCache 流程。
