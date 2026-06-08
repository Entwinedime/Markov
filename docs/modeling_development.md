# Modeling 开发文档

维护方式：这是 modeling 主线文档。更新时直接删改本文件内容，不在这里写流水账。

## 目标

Modeling 的目标是基于 profiling 采集到的 trace 构建 DAG，并在目标配置下通过子模块修改 DAG，得到端到端性能预测。

默认流程：

```text
profile trace / sidecar
  -> scripts/trace/trace_merger.py 合并 torch / ld_preload / python_probe
  -> C++ TraceGraph 构建 base DAG
  -> C++ SimulationModule 按固定顺序修改 DAG 或维护模型状态
  -> DAG 拓扑仿真
  -> prediction.json
```

默认主输出只包含端到端预测时间：

```json
{
  "predicted_e2e_ns": 0
}
```

| 字段 | 作用 |
| --- | --- |
| `predicted_e2e_ns` | DAG 经拓扑重放得到的端到端预测时间，单位是纳秒；默认不使用原始绝对时间戳兜底，因此该值用于暴露 DAG 依赖边是否完整。 |

当前主线入口：

```text
python3 scripts/internal/model_runner.py --config configs/modeling/smoke/modeling_smoke_hicache.json
```

从真实 profiling run 入口：

```text
python3 scripts/internal/model_runner.py \
  --config configs/modeling/hicache/modeling_hicache_from_manifest.json \
  --profile-manifest <run_dir>/profile_manifest.json \
  --output-dir <run_dir>/modeling/faithful_replay \
  --mode faithful_replay \
  --emit-validation \
  --emit-module-summary
```

建模配置最小结构：

| 字段 | 作用 |
| --- | --- |
| `input.profile_manifest` | 可选，读取 profiling manifest 中列出的 torch / LD_PRELOAD / Python probe trace，并先交给 trace merger 合并。 |
| `input.trace_paths` | 显式 trace 文件列表；这些文件必须已经是 C++ TraceGraph 可消费的 merged Chrome trace。 |
| `output_dir` | 建模输出目录。 |
| `mode` | 建模模式，支持 `faithful_replay`、`cache_state`、`cache_patch`。 |
| `modules` | 按顺序执行的 C++ `SimulationModule` 配置。 |
| `outputs` | 控制 Chrome trace、summary、validation、debug 输出。 |
| `validation` | 闭环验证阈值，例如 faithful replay E2E 误差阈值和 cache patch E2E 误差阈值。 |

建模模式：

| mode | 作用 |
| --- | --- |
| `faithful_replay` | 消费完整真实 merged trace 构建 base DAG 并拓扑重放，runner 不生成 C++ model config，不加载任何子模块，用于验证 base DAG 正确性。 |
| `cache_state` | 子模块消费 trace 并维护内部状态，但不修改 DAG，用于验证 HiCache 状态机正确性。 |
| `cache_patch` | 子模块维护状态并通过 mutation API 修改 DAG，用于 what-if 性能预测。 |

从 profiling manifest 读取时，runner 会先调用 `scripts/trace/trace_merger.py` 生成
`output_dir/merged_trace/merged_trace_*.json`，再把 merged trace 传给 C++ 后端。
runner 会自动在 run dir 下查找 `bench/**/workload_report.json`，用于 validation 对比。

### Faithful Replay 与事件消费

`faithful_replay` 关闭的是子模块加载和 DAG patch，不是关闭事件消费。它必须消费完整真实执行 trace，
否则无法证明 base DAG 的重放能力。

完整真实执行 trace 包括：

- torch profiler 采集到的 CPU op、runtime、kernel、copy 和同步事件；
- LD_PRELOAD 采集到的 native runtime、event record/wait、stream/device sync 等事件；
- Python probe 采集到的真实执行路径事件，例如 CPUInfer、HiCache lookup、load、prefetch、insert、write、evict。

这些事件只要代表真实运行路径，就应该进入 merged trace，并被 C++ TraceGraph 作为 base DAG 输入。
即使 HiCacheModule 没有加载，HiCache 真实执行事件也仍然可以成为 base DAG 节点或依赖锚点。

子模块是在 base DAG 之上做 what-if：

```text
complete merged trace
  -> base DAG
  -> faithful replay: no module, no patch
  -> cache_state: module reads DAG/facts, no patch
  -> cache_patch: module reads DAG/facts, emits DAG mutations
```

需要隔离的是非执行类事件，而不是某个领域的真实执行事件。验证用 state snapshot、oracle state、
probe 内部调试、质量审计和模型解释输出如果不是业务执行路径的一部分，不能作为默认性能 DAG 节点。
这类事件应放在独立 debug/state trace 中，或显式标记为 `model_input=false`，只在 validation/debug
开关打开时由对应工具读取。

CLI 覆盖项：

| 参数 | 作用 |
| --- | --- |
| `--profile-manifest` | 覆盖 `config.input.profile_manifest`，用于直接消费某次 profiling run。 |
| `--output-dir` | 覆盖 `config.output_dir`。 |
| `--mode` | 覆盖建模模式。 |
| `--emit-dag-chrome-trace` / `--emit-module-summary` | 只打开本次需要的 debug 输出，不改变配置文件默认输出策略。 |

## Trace Merger

三类 trace 不直接分别喂给后端，而是先由 `scripts/trace/trace_merger.py` 合并。

trace merger 负责两类工作：

- 合并事件参数：用 LD_PRELOAD 采到的 AscendCL 参数补充 torch profiler 事件，例如 stream/event 等 torch trace 缺失字段；
- 添加事件：把 torch profiler 采不到但建模需要的 CPUInfer、HiCache、Python probe 事件追加到 merged trace。

trace merger 不根据 modeling mode 删除真实执行事件。`faithful_replay`、`cache_state` 和 `cache_patch`
都应看到同一份完整 merged trace；三者差异只在是否加载子模块、是否产生 DAG mutation。

manifest 模式会按 PID 或文件顺序配对 torch trace、LD_PRELOAD trace 和 Python probe
sidecar。输出目录固定包含 `merge_manifest_summary.json` 和每个 rank 的
`merged_trace_*.json`。

## TraceGraph

C++ TraceGraph 是唯一 modeling 后端。

它负责：

- 读取 profiling trace；
- 规范化事件；
- 构建 DAG node；
- 建立 stream / thread / correlation / sync / collective 依赖边；
- 支持子模块修改后的 DAG 重新仿真；
- 输出 E2E 和可选 critical path。

当前 C++ TraceGraph 位于 `src/modeling/trace_graph`，由 root CMake 构建
`trace_graph` executable。该目录只维护 C++ 后端实现，不维护独立 README；
TraceGraph 开发说明统一写在本文档。它支持：

- merged Chrome trace JSON 读取；
- `TraceEvent` 规范化表示；
- `DagGraph` 节点、边、属性和仿真时间维护；
- `DagBuilder` 从 trace 构建 base DAG；
- thread order、device stream order、correlation edge、显式 dependency edge；
- C++ `SimulationModule` 基类和继承模块；
- 默认拓扑重放输出 `predicted_e2e_ns`，不依赖原始时间戳空洞补齐缺失边；

Base DAG 验证直接运行 C++ TraceGraph。重点是 CPU launch 到 device kernel 的 correlation、同 stream 顺序、
event record/wait、stream synchronize、拓扑重放和 cycle 诊断。

当前 active 目录层次：

| 层次 | 作用 |
| --- | --- |
| `core` | `TraceEvent`、`DagGraph` 和 DAG 构建所需的基础类型。 |
| `frontend` / `io` / `simulation` | 配置解析、trace 输入输出和拓扑仿真。 |
| `modules` | 所有 what-if 子模块及其内部状态、policy、summary、DAG 修改逻辑。 |

不再保留独立领域实现层；复杂领域逻辑必须收敛到对应子模块内部。

### Base DAG 重构基线

当前 C++ TraceGraph 的 base DAG 构建以老版 TraceGraph 的行为为基线，但老版仓库只作为参考，不作为 active
运行依赖。已吸收的关键设计包括：

| 设计点 | 作用 |
| --- | --- |
| 流式 Chrome trace 解析 | 避免用完整 JSON DOM 解析千万级 trace，降低输入阶段内存和时间开销。 |
| `Physic Stream Id` 事件去重合并 | torch / Ascend 事件存在同一 NPU op 的重复记录，需要合并到一个 device node，否则节点数和边数会膨胀。 |
| CPU leaf 事件筛选 | CPU 嵌套区间只保留真正影响 launch / runtime / sync 的叶子节点，避免把父区间也当作可执行节点。 |
| correlation 链 | 通过 `correlation_id` 建立 CPU launch/runtime 到 device kernel 的因果边。 |
| connection 链过滤 | 对旧 trace 中三段及以上但首段不是 `Node@launch` 的 connection group 不强行连边，避免错误串行化。 |
| stream / thread 顺序边 | 保留同 stream、同 CPU lane 的执行顺序。 |
| event record / wait / stream sync 边 | 显式建模 Ascend event 和 stream synchronize 的阻塞关系。 |
| HCCL 跨 rank 合并 | merge 多 rank DAG 时根据 HCCL kernel name 建立跨 rank collective 依赖，并校正 HCCL duration。 |
| duration event 输入 | 当前只把 `ph=X` 的完整 duration event 当作 DAG 节点；metadata 和 flow event 不能作为 0 时长执行节点。 |

当前真实 merged trace 基线：

| 输入 | consumed duration events | nodes | edges | predicted E2E | trace real E2E | 相对误差 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| rank0 | 5,131,330 | 2,257,498 | 3,415,851 | 89,769,412 ns | 89,056,920 ns | 0.80% |
| rank1 | 5,166,791 | 2,297,269 | 3,481,054 | 89,850,961 ns | 89,058,602 ns | 0.89% |
| rank0+rank1 | 10,298,121 | 4,554,767 | 6,952,229 | 89,850,961 ns | 89,058,614 ns | 0.89% |

老版 rank0 对照结果为 6,145,150 records、2,568,595 nodes、3,912,340 edges、90,740,127 ns predicted、
89,056,920 ns real。老版把 Chrome trace metadata / flow event 也纳入了 DAG；当前 active 后端不再以老版
节点数作为验收目标，而以当前后端可解释的 duration-event DAG 作为主线。

已定位并修复的 6 条边差异：

| 差异 | 原因 | 修复 |
| --- | --- | --- |
| 顺序类边多 4 条 | active device lane 优先使用 `streamId` / `Physic Stream Id`，老版优先使用 trace 顶层 `tid`，导致 8 个 device lane 被合并成 4 个。 | `lane_key` 改为对 device 事件按 `tid -> streamId -> Physic Stream Id` 取 lane。 |
| sync 边多 2 条 | 零 duration `EVENT_WAIT` 使用大时间戳 double 计算 `ts + dur - 0.1` 时精度丢失，错误匹配同 timestamp 的 `EVENT_RECORD`。 | 对 `dur=0` 的 `EVENT_WAIT` 使用整数边界，避免同 timestamp record 被纳入。 |

`validation.json` 中 faithful replay 的 actual 来源必须优先使用 trace 内部 `real_e2e_ns`，不能用
bench serving 的端到端 wall time 替代。bench serving duration 只作为 workload 外层窗口参考。

仍需继续审查的点：

| 点 | 当前判断 |
| --- | --- |
| CPU lane 是否全部合并为 `CPU_MERGED` | 这可能过度串行化多线程 CPU 事件，但直接拆成真实 tid 会改变大量 CPU launch 顺序，需用专门实验验证。 |
| HCCL duration 合并策略 | 当前仍按同名 collective 组取最小 duration，并通过 HCCL 边约束后继；该策略可能剥离等待，也可能低估真实通信，需要后续按 collective 事件检查。 |
| Chrome flow event | 当前不消费 flow event；若后续要利用 torch / CANN flow 表达依赖，应解析成边，而不是恢复成 0 时长节点。 |

## 子模块统一抽象

所有 what-if 都必须规约成 C++ 子模块，不再保留另一套普通图变换机制。

简单变化也是子模块：

- `NodeScaleModule`：节点耗时缩放；
- `EdgeLatencyModule`：边延迟修改；
- `BandwidthModule`：通信耗时按带宽重算；
- `SyncWaitModule`：同步等待重算。

复杂变化也是子模块：

- `HiCacheModule`：当前 active 实现维护 page resident/dirty/backuped 状态，但暂不修改 DAG；
- `ParallelStrategyModule`：修改 rank、sharding、collective 相关 DAG；
- `InterconnectModule`：修改 CPU-NPU / NPU-NPU 通信相关 DAG。

## 功能子模块接口

```cpp
class SimulationModule {
public:
    virtual ~SimulationModule() = default;
    virtual std::string name() const = 0;
    virtual void apply(DagGraph& graph) = 0;
    virtual bool has_summary() const;
    virtual std::string summary_json() const;
};
```

| 方法 / 字段 | 作用 |
| --- | --- |
| `name` | 子模块稳定名称，用于 registry、输出归属和 debug 目录。 |
| `apply(DagGraph& graph)` | 子模块唯一执行入口；可以维护状态、修改 DAG 或重算节点耗时。 |
| `has_summary()` / `summary_json()` | 可选输出子模块状态摘要，不参与默认预测输出。 |

功能子模块是真实仿真路径会调用的 C++ 对象。它负责维护内部状态和修改 DAG，不直接写 debug 文件。

## Debug 子模块接口

每个功能子模块可以有对应 C++ debug 类。Debug 类也是抽象类的实现，并显式关联某个功能子模块类型。

```text
SimulationModuleDebug
  module_name
  module_type
  attach(module)
  emit_state_debug(output_dir)
  emit_dag_mutation_debug(dag_mutations, output_dir)
  emit_validation_debug(validation_result, output_dir)
```

| 方法 / 字段 | 作用 |
| --- | --- |
| `module_name` | debug 输出归属的子模块名称。 |
| `module_type` | debug 类关联的功能子模块类型。 |
| `attach(module)` | 绑定已运行的功能子模块实例，读取其内部状态。 |
| `emit_state_debug(output_dir)` | 输出子模块状态相关 debug。 |
| `emit_dag_mutation_debug(dag_mutations, output_dir)` | 输出 DAG 修改相关 debug。 |
| `emit_validation_debug(validation_result, output_dir)` | 输出 validation 差异和失败归因 debug。 |

最高层参数决定是否进入 debug 模式：

- `debug=true`：开启所有已注册 debug 输出；
- `debug_modules=["hicache"]`：只开启指定模块 debug 输出。

Debug 输出分两类：

- 子模块状态 debug：内部状态机、输入 fact 覆盖、rejected facts、policy 决策、状态转移；
- DAG 修改 debug：新增/删除/修改 node/edge、critical path 影响、未锚定修改、mutation 合并冲突。

## DAG 修改

子模块直接修改 DAG。

直接修改不等于无记录修改。实现上应通过统一 DAG mutation API 记录：

| 字段 | 作用 |
| --- | --- |
| `module_id` | 标识产生这次 DAG 修改的子模块。 |
| `mutation_id` | 标识一次具体 DAG 修改，便于回放、debug 和冲突定位。 |
| `source_anchor` | 记录修改依据的 trace event、request、operation 或原 DAG node。 |
| `reason` | 记录修改原因，例如 policy decision、capacity eviction、bandwidth change。 |
| `before` | 记录修改前对象，用于回滚和 diff。 |
| `after` | 记录修改后对象，用于验证最终 DAG 状态。 |

子模块可以：

- 新增节点；
- 删除或禁用节点；
- 修改节点 duration / resource / metadata；
- 新增、删除或修改边；
- 调整 blocking / background / nonblocking 属性；
- 修改 collective 或 sync 依赖。

子模块不应该：

- 修改原始 profiling trace；
- 绕过 DAG mutation API 裸改 DAG；
- 在缺少因果锚点时强行修改 critical path；
- 把 debug 字段写入默认预测输出。

## 输出参数

默认只输出：

- `prediction.json`

可选输出由参数控制：

```json
{
  "emit_dag_chrome_trace": false,
  "emit_module_summary": false,
  "emit_validation": false,
  "debug": false
}
```

| 参数 | 作用 |
| --- | --- |
| `emit_dag_chrome_trace` | 输出 `dag_chrome_trace.json`，用 Chrome trace 格式可视化修改后的 DAG。 |
| `emit_module_summary` | 输出 `model_summary.json`，记录子模块状态摘要和建模摘要。 |
| `emit_validation` | 输出 validation 结果，用于实验矩阵和 actual 对比。 |
| `debug` | 启用 debug 类输出，生成状态 debug 和 DAG 修改 debug。 |

`emit_dag_chrome_trace` 的统一命名：

```text
config: outputs.emit_dag_chrome_trace
CLI: --emit-dag-chrome-trace
```

开启后输出 `dag_chrome_trace.json`，用 Chrome trace 格式表达修改后的 DAG，方便可视化预测图。

## HiCache 建模

HiCache 是第一个复杂子模块。当前 active C++ `HiCacheModule` 已进入 state-only 阶段：
它可以被 C++ module registry 加载，消费 HiCache profiling facts，维护
`L1/L2/L3 resident`、`dirty`、`backuped`、`evicted`、`prefetch planned/ready`
等 page 状态，并输出状态验证 summary。它仍然不修改 DAG、不输出 replay latency；
这样做是为了先验证 cache 状态维护，不让未完成的 DAG patch 逻辑干扰 base DAG。
截至 2026-06-07 23:41 +0800，base 和 page64 target 的同配置 replay 已通过，多数组合
cross-config prediction 已通过；但 page size 变化下的 strict page64 prefetch / final-state
prediction 仍未闭环。因此 HiCache DAG patch 只能作为设计目标，不能把 page64 state 作为已完成前提。

HiCache 后续继续分两步验证：

1. cache 状态维护正确；
2. cache 状态变化正确反映到 DAG。

当前闭环验证分三层：

| 层级 | 命令意图 | 验收重点 |
| --- | --- | --- |
| faithful replay | `mode=faithful_replay` | `dag_mutation_count=0`，不加载任何子模块，trace 内部 E2E 相对误差目标不超过 5%，真实大 trace 当前已达到约 0.89%。 |
| cache state | `mode=cache_state` | `dag_mutation_count=0`；验证 page identity、tier resident、dirty/backuped、evict 和 prefetch 状态转移。 |
| cache patch | `mode=cache_patch` | DAG mutation 非空，mutation 能映射到 fact / 状态 / policy，E2E 相对误差目标不超过 20%。 |

HiCache state validation 的 modeling 入口：

```bash
python3 scripts/internal/model_runner.py \
  --config configs/modeling/hicache_state/modeling_hicache_state_validation.json \
  --profile-manifest <run_dir>/profile_manifest.json \
  --output-dir <run_dir>/modeling/cache_state_replay \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

当 `validation.hicache_state.enabled=true` 时，runner 会从 `model_summary.json` 读取
C++ HiCache state summary，并从独立 state trace 或 merged trace 中 `model_input=false`
的 `state_snapshot` 事件读取 oracle，生成 `validation.json.hicache_state`。
同时会额外生成 `predicted_target_cache_state_trace.json`，用于保存 request / operation /
page 级 state transition。该文件是 validation/debug 输出，不属于默认主输出；不开启
module summary 或 state validation 时不应依赖它存在。

跨配置 state prediction 可以通过 modeling config 的
`input.target_experiment_config` 引用目标实验配置。runner 会从目标实验配置的
`server.command` 中抽取明确出现的 SGLang HiCache 参数：

- `--page-size` -> `hicache.page_size`；
- `--hicache-write-policy` -> `hicache.write_policy`；
- `--hicache-storage-prefetch-policy` -> `hicache.prefetch_policy`；
- `--hicache-storage-backend-extra-config` 中的 `prefetch_timeout_base`、
  `prefetch_timeout_per_ki_token`、`prefetch_timeout_max` -> C++ timeout 参数。

目标实验配置还可以在 `modeling.hicache` 中显式补充 C++ state model 需要但
SGLang 启动参数无法稳定表达的字段，例如有效 `l1_capacity_pages` /
`l2_capacity_pages`。runner 不会根据 `--max-total-tokens` 或 `--hicache-ratio`
粗算 capacity；这些参数描述资源池和比例，不等价于最终可用于 HiCache state
prediction 的有效 page budget。

state validation 的 oracle 不是单一来源。`state_snapshot` 用于验证 resident、backuped、
dirty、evicted 等树状态；`lock_ref_inc` / `lock_ref_dec` facts 用于修正最终
`locked_pages`，因为 trace 尾部可能存在 lock 释放事实但缺少对应的 end snapshot；
`l3_to_l2_transfer_end` 和 `prefetch_progress_state` 共同用于验证 prefetch ready /
late / suppressed。`state_snapshot.capacity` 用于汇总真实运行暴露的 page size、
write policy、prefetch policy、L1/L2 pool capacity、available size、prefetch threshold
和 prefetch capacity limit；这些字段只进入 `oracle_capacity_summary` 解释输出，不直接参与
final-state diff。

final-state diff 默认以 oracle 暴露的集合字段为准：只要 oracle final state 中存在某个
集合，模型没有输出该集合也会按空集合参与比较。若某个旧 trace 缺少对应 model input
不变量，或本次实验明确不验证该 debug 集合，必须在 modeling config 的
`validation.hicache_state.ignore_state_keys` 中显式列出。被忽略字段不会参与
`final_state_match`，但仍会保留在 `ignored_sets_diff_by_tier`、`model_final_state_counts`
和 `oracle_final_state_counts` 中，不能静默消失。

对于显式 prefetch policy，C++ state model 会在 trace 消费结束时执行一次
prefetch finalization。`wait_complete` 和 `best_effort` 下，planned 但没有 ready
evidence 的 page 会进入 `prefetch_suppressed_pages`；`timeout` 下，只有已经有
timeout/terminal empty progress 证据的 request 才会把未 ready page 归为 suppressed，
单纯 schedule 过但没有 timeout/progress 证据的 page 不会在 finalization 中强行
suppressed。`l3_to_l2_transfer_end` 是已经完成的 transfer 事实，在 `best_effort`
和 `timeout` 下都作为 ready evidence；timeout window 只影响没有完成 transfer
evidence 的 page 如何归类。

page size what-if 下，prefetch 的 `target_page_identity` 必须用目标 page size
对应的完整 token path 重新计算。旧 trace 中用 base run 的 `last_hash` 继续计算
`page_hashes:<new_input_tokens>,64,<last_hash>` 只能作为诊断证据，不能作为严格
page64 prefetch prediction 的充分不变量；因为 `last_hash` 本身属于 base page size
下的 radix prefix 链。新的 profiling 配置应使用
`page_hashes_after_prefix:<prefix_keys>,<new_input_tokens>,<target_page_size>`，让
target prefetch pages 先从目标 page size 的 prefix path 重新得到 parent hash，再只返回
`new_input_tokens` 对应的 suffix pages。`page_hashes_concat` 会返回完整 path pages，
不能直接作为 prefetch planned set。当前旧 page64 run 的 core resident / dirty /
backuped / evicted state 曾在旧口径下对齐；最新 strict base -> page64 prediction 仍出现
L2、backuped、evicted 和 prefetch planned/ready/suppressed 差异，需要先用新的真实 validation
重验并修复。

`validation.json.hicache_state.event_delta_validation` 是 transition 级诊断输出：

- inclusive oracle 保留 start/end snapshot 的包围差分，用于观察真实调用包含了哪些状态变化；
- exclusive oracle 只比较没有嵌套 state snapshot 的调用，避免把内层 HiCache 调用的状态变化误归因给外层函数；
- exact event key 只适合同配置 replay。跨配置 prediction 的 base run 和 target run 时间戳不同，应使用 final-state、policy oracle 和 transition coverage 验证；
- 没有模型 transition 的 oracle 状态集合会进入 `ignored_state_keys_without_predicted_transition`，例如旧 trace 未采 `inc_lock_ref` / `dec_lock_ref` 时的 `locked_pages`。

`validation.json.hicache_state.timeline_delta_validation` 是 object-level 状态时间线诊断输出：

- 它依赖 `state_snapshot.object_id`，按 HiRadixCache 对象排序并对多进程 cache state 做 union；
- `match=true` 表示模型输出的 transition kind/page multiset 全部被 raw snapshot timeline 覆盖；
- `exact_match=true` 才表示模型和 raw snapshot timeline 完全相等；
- timeline 只比较 completed snapshot 实际可见的 state key。若某个模型字段只在调用内部瞬时出现，
  但 completed snapshot 从未暴露，例如 write-through 下的 dirty transient，该字段会进入
  `ignored_unobservable_state_keys`，不作为 timeline coverage mismatch；
- 当 lock facts 证明最终 locked set 已清空，但最后一个 completed snapshot 仍残留 locked page 时，
  `final_lock_timeline_correction` 会为 timeline oracle 补齐尾部缺失的 `clear_locked`；
- 多进程场景下 state snapshot 是稀疏采样，不是完整状态日志。某个 cache object 长时间未被采样时，下一次任意 HiCache 调用的 snapshot 可能暴露之前已经发生的状态变化，因此 `oracle_extra_transition_count` 只作为 oracle-only transient oscillation 诊断；
- `model_extra_transition_count` 必须为 `0`，否则说明模型预测了 raw timeline 中没有证据支持的状态变化。

`validation.json.hicache_state.oracle_capacity_summary` 是 capacity / policy 事实摘要：

- `ready=true` 表示 oracle trace 中至少存在一个带 `capacity` 的 validation-only state snapshot；
- `unique_values` 汇总 page size、write policy、prefetch policy、L1/L2 capacity pages、available pages 等字段；
- `samples` 保留最多 5 条压缩样例，用于快速判断实际采到的是 HiRadixCache 还是 controller 对象；
- 该摘要用于解释为什么某个 target 的有效 budget 不是简单的 `max_total_tokens / page_size`，后续可以作为自动生成或校验 target HiCache config 的输入；
- 当前摘要不覆盖 C++ 模型配置，也不改变 `final_state_match` 的判断。

`validation.json.hicache_state.capacity_config_audit` 是 target config 诊断输出：

- `target_config` 记录 C++ HiCache module 实际收到的 page size、L1/L2 capacity、write policy 和 prefetch policy；
- `comparisons.page_size`、`write_policy`、`prefetch_policy` 与 oracle capacity summary 做精确比较；
- `comparisons.l1_capacity_pages` 和 `l2_capacity_pages` 同时比较 raw pool capacity 与 oracle final resident count；
- `target_below_observed_pool` 只表示 target config 小于 raw pool capacity，可能是有效 budget，进入 `warning_fields`；
- `target_exceeds_observed_pool` 或 `target_below_oracle_final_count` 进入 `likely_error_fields`，通常表示 prediction config 需要修正；
- audit 只提供修复指导，不自动覆盖 C++ target config，也不是当前 `validation_ready` 的硬门槛。

`validation.json.hicache_state.oracle_observed_max_state_counts` 记录 raw snapshot 时间线上每个
state set 达到过的最大规模。它与 final-state count 不同：final count 只说明 run 结束时还剩
哪些 page，observed max count 才能说明运行中实际达到过的容量压力峰值。capacity audit 会用它
判断 target capacity 是否低于真实曾经达到过的 resident set；如果低于该峰值，通常说明 target
config 不足以复现 target trace 的状态变化。

`validation.json.hicache_state.capacity_config_audit.recommended_target_config` 是基于 target
oracle 的 C++ HiCache 配置建议：

- `page_size`、`write_policy`、`prefetch_policy` 使用 oracle capacity summary 中唯一观测值；
- `l1_capacity_pages` / `l2_capacity_pages` 只有在 C++ target config 已经显式设置时才复制到推荐配置；
- 如果 target config 没有显式 capacity，`evidence` 只记录 raw pool、observed max 和 final count，
  并标记为 `not_auto_recommended`；
- 这样做是因为 observed max 和 final count 描述的是运行时占用，不等价于 SGLang 的有效
  capacity 参数；把它们自动写回 C++ config 会让 replay 误进入 capacity what-if 分支；
- 该推荐只用于验证后的配置修复指导，不会在同一次 run 中反向覆盖 C++ 模型输入。无 target oracle
  的纯 what-if 仍必须由用户或实验配置显式给出目标参数。

当推荐配置 `ready=true` 时，`model_runner.py` 会额外写出
`recommended_hicache_cpp_model_config.json`。该文件格式为：

```json
{
  "modules": ["hicache"],
  "hicache": {
    "enabled": true,
    "page_size": 128,
    "write_policy": "write_through",
    "prefetch_policy": "timeout"
  }
}
```

它可以作为下一次 C++ TraceGraph 运行的 `--model-config` 输入，但生成它的本次 run 不会使用它。

HiCache 状态应维护：

| 字段 | 作用 |
| --- | --- |
| `l1_resident_pages` / `l2_resident_pages` / `l3_resident_pages` | 记录每层当前 resident page，用于判断 hit、miss 和移动方向。 |
| `dirty_pages` | 记录已修改但尚未写回的 page，用于 write-back 和 dirty eviction。 |
| `backuped_pages` | 记录已经备份到下层或 storage 的 page，用于避免重复写。 |
| `evicted_pages` | 记录被淘汰的 page，用于验证 capacity policy 和后续 miss。 |
| `locked_pages` | 记录 lock ref 非零、不能被 eviction 选择的 page，用于验证 evictable 状态。 |
| `page_identity` | 定义 page 的稳定身份，用于跨事件匹配同一 page。 |
| `radix_nodes` | 记录 radix tree prefix、node 与 page 映射，用于 prefix match。 |
| `prefetch_ready_pages` | 记录已经完成且可命中的 prefetch page。 |
| `prefetch_late_pages` | 记录未及时完成的 prefetch page，用于解释 foreground load。 |
| `prefetch_suppressed_pages` | 记录被策略或状态抑制的 prefetch page。 |
| `write_policy_state` | 记录 write-through、write-back、selective write 的决策状态。 |
| `capacity_state` | 记录容量、淘汰候选、淘汰结果和 dirty writeback。 |

lock state 的输入来自 `lock_ref_inc` / `lock_ref_dec` facts。SGLang 会把 lock ref 沿 radix
父链更新到 root；root 事件没有 page identity，且 `lock_delta=0` 时视为 no-op，不进入
`missing_page_identity_events`。其它 lock facts 如果缺 page identity，应按严格输入契约失败。

HiCache 修改 DAG 时应能表达：

- cache hit；
- foreground load；
- background prefetch；
- storage write；
- dirty eviction writeback；
- blocking / nonblocking 关系；
- request / operation / state transition anchor。

### HiCache What-If 到 DAG 的映射

HiCache what-if 不直接修改原始 profiling trace，而是根据 profiling facts、目标配置和当前 cache 状态生成 DAG mutation。

输入依据分三类：

| 依据 | 来源 | 用途 |
| --- | --- | --- |
| profiling facts | `profiling_development.md` 中 HiCacheModule 采集的 request、operation、page、tier、storage 事实 | 还原 base 运行中的 cache 操作顺序和 page 身份。 |
| target config | page size、L2/L3 容量、write policy、prefetch policy、storage backend、带宽/延迟参数 | 决定目标配置下哪些 page 命中、加载、预取、写回、淘汰。 |
| base DAG anchors | request 节点、scheduler 节点、原 cache 相关节点、device copy 节点、storage IO 节点 | 决定新增/删除/修改的 DAG 节点挂在哪里。 |

HiCache 子模块先维护目标配置下的 cache 状态，再把状态转移映射到 DAG。不能用 base trace 中观测到的 `load_back` 次数直接驱动 target load 数量；base 事件只用于提供事实、锚点和 validation 对照。

#### DAG 修改对象

| 修改对象 | 何时修改 | 修改方式 |
| --- | --- | --- |
| cache lookup 节点 | page size、radix match 或 resident 状态变化导致 lookup 结果变化 | 更新 metadata，必要时修改 duration；lookup 通常不直接新增长耗时节点。 |
| load 节点 | target 下 page 不在 L1，但在 L2/L3 可加载 | 插入或修改 foreground load 节点，并连接到后续 compute。 |
| prefetch 节点 | target policy 计划后台预取 | 插入 background prefetch 节点，连接到 request/scheduler 锚点，不直接阻塞 compute。 |
| write 节点 | write-through、selective write、write-back eviction 触发写 | 插入或修改 L1->L2、L2->L3 写节点，根据 policy 设置 blocking 属性。 |
| evict 节点 | L1/L2 容量不足 | 插入 eviction 节点；dirty eviction 先插入 writeback 节点再释放 resident。 |
| dependency edge | cache 操作影响后续 forward、scheduler 或 storage completion | 新增、删除或重连边，表达 blocking、async、completion 关系。 |
| node duration | 目标带宽、page 数、bytes 或 backend latency 变化 | 用 `bytes / bandwidth + fixed_latency` 或已校准 backend 模型重算。 |
| node metadata | target 状态和 explain 所需信息变化 | 写入 module id、operation id、page set hash、tier、direction、reason。 |

#### DAG 节点类型

HiCache 只生成少量语义节点，避免把内部状态 dump 成大量 debug 节点。

| 节点类型 | 含义 | 默认 blocking |
| --- | --- | --- |
| `hicache_lookup` | radix/prefix/resident 查询 | 否，除非实际 runtime lookup 节点在 base DAG 中阻塞。 |
| `hicache_load_l2_to_l1` | host cache 到 device cache 加载 | 是，后续使用这些 KV 的 compute 必须等待。 |
| `hicache_load_l3_to_l2` | storage 到 host cache 加载 | 若由 foreground miss 触发则阻塞；若由 prefetch 触发则后台。 |
| `hicache_prefetch_l3_to_l2` | storage 到 host cache 后台预取 | 否，只通过 completion 影响后续 request 是否命中。 |
| `hicache_write_l1_to_l2` | device cache 写回 host cache | write-through 可为后台或短阻塞；write-back eviction 通常阻塞 eviction。 |
| `hicache_write_l2_to_l3` | host cache 写 storage | write-through policy 视实现决定是否阻塞；dirty eviction writeback 阻塞 eviction 完成。 |
| `hicache_evict_l1` / `hicache_evict_l2` | 释放 device 或 host resident | 对触发分配的路径阻塞。 |

#### 操作映射规则

| HiCache 状态转移 | 依据 | DAG 修改 |
| --- | --- | --- |
| lookup hit in L1 | `page_identity` 已在 `l1_resident_pages` | 保留或更新 lookup metadata，不插入 load 节点，后续 compute 直接依赖原有路径。 |
| lookup hit in L2 | page 不在 L1，但在 `l2_resident_pages` | 插入 `hicache_load_l2_to_l1`；从 request/operation anchor 连入 load，再从 load 连到使用 KV 的 compute。 |
| lookup hit in L3 | page 不在 L1/L2，但可由 storage evidence 或 target L3 readable set 读取 | 插入 `hicache_load_l3_to_l2`，再插入 `hicache_load_l2_to_l1`；两者串联并阻塞后续 compute。 |
| miss | page 不在目标 resident set 且无 L3 evidence | 不插入 cache load；后续 compute 走原始 prefill/compute 路径，并在 metadata 标记 miss reason。 |
| prefetch ready | target prefetch policy 计划 page，且完成时间早于使用点 | 插入 background `hicache_prefetch_l3_to_l2`，completion 更新 L2 resident；使用点不再插 foreground L3 load。 |
| prefetch late | prefetch 未在使用点前完成 | 保留 background prefetch 节点，但使用点仍插 foreground load 或 miss 路径；metadata 标记 late。 |
| prefetch suppressed | threshold、capacity、timeout、rate limit 或 policy 判断不预取 | 不插 prefetch 节点；在 DAG mutation metadata 中记录 suppressed reason。 |
| insert | 新 KV page 在 L1 产生 | 更新 `l1_resident_pages`；必要时给生成 KV 的 compute 节点追加 cache metadata，不额外插耗时节点。 |
| write-through | insert 后策略要求写 L1->L2/L3 | 插入 `hicache_write_l1_to_l2`；若有 storage backend，再插 `hicache_write_l2_to_l3`。 |
| selective write-through | hit_count、prefix、backuped 等条件满足 | 满足时按 write-through 插节点；不满足时只记录 policy decision，不插写节点。 |
| write-back clean insert | insert 后只标 dirty | 不立即插 storage write 节点，只更新 `dirty_pages` metadata。 |
| dirty eviction | 被淘汰 page 在 `dirty_pages` | 先插 `hicache_write_l1_to_l2` 或 `hicache_write_l2_to_l3`，完成后再插 eviction；eviction 依赖 writeback completion。 |
| clean eviction | 被淘汰 page 已 backuped 或 clean | 插入 eviction 节点或直接删除 resident metadata，不插 writeback。 |

#### 边的连接规则

| 边 | 连接方式 | 依据 |
| --- | --- | --- |
| request -> cache operation | request anchor 指向 lookup/prefetch/load/write 计划节点 | `request_id`、`operation_id`。 |
| prefetch schedule -> prefetch IO | scheduler/preload anchor 指向 background prefetch 节点 | `Scheduler._prefetch_kvcache`、`prefetch_from_storage` facts。 |
| prefetch IO -> future hit | 不直接阻塞当前 compute；只在完成时间早于使用点时影响后续状态 | `completed_tokens`、prefetch policy、operation timestamp。 |
| foreground load -> compute | load 节点必须在使用 KV 的 compute 前完成 | `operation_id`、request stage、base DAG compute anchor。 |
| writeback -> eviction | dirty page 必须写回后才能释放对应 resident | `dirty_pages`、eviction decision。 |
| eviction -> allocation/load | 容量不足时，load/insert 依赖 eviction 完成 | `capacity_state`、allocator failure 或 target capacity model。 |
| storage read/write -> completion | storage IO 节点连接到 cache state update 节点或直接更新 metadata | `batch_get` / `batch_set` facts、status、bytes。 |

#### Duration 计算

HiCache 节点 duration 按以下优先级确定：

1. 如果 target 与 base backend 相同，且 profiling 中有同类型、同方向、同 page size 的实际 duration，可用实际 duration 按 page/bytes 比例缩放。
2. 如果有目标带宽和固定延迟参数，使用 `bytes / bandwidth + fixed_latency`。
3. 如果只有 page 数，使用 page size 和 KV layout 估算 bytes，并在 mutation metadata 标记 `bytes_estimated=true`。
4. 如果缺少 bytes、page size 或 backend 参数，则该 mutation 不能给出可信 duration，应在 validation/debug 输出中标记缺失依据。

#### Mutation 记录

每个 HiCache DAG mutation 至少记录：

| 字段 | 作用 |
| --- | --- |
| `module_id` | 固定为 `hicache`。 |
| `mutation_id` | 唯一标识一次 DAG 修改。 |
| `operation_id` | 对应 profiling 中的 cache operation。 |
| `request_id` | 对应请求。 |
| `source_anchor` | 修改所依据的 base DAG node 或 profiling fact。 |
| `mutation_kind` | `add_node`、`remove_node`、`update_node`、`add_edge`、`remove_edge`、`update_edge`。 |
| `hicache_action` | `lookup`、`load`、`prefetch`、`insert`、`write`、`evict`。 |
| `page_set_hash` | 被修改操作影响的 page 集合摘要。 |
| `tier_src` / `tier_dst` | 数据移动方向。 |
| `blocking_class` | `foreground`、`background`、`nonblocking`。 |
| `reason` | policy、capacity、hit/miss、dirty writeback 等原因。 |
| `before` / `after` | 修改前后的 DAG 对象摘要。 |

#### 缺失依据处理

- 缺少 `request_id` 或 `operation_id`：不能把 cache mutation 挂回请求路径，应拒绝该 request 的 HiCache DAG 修改。
- 缺少 `page_identity`：不能维护 page resident/dirty 状态，应拒绝对应 operation 的 cache 状态转移。
- 缺少 `source_anchor`：可以维护状态，但不能修改 critical path，只能在 debug/validation 中报告。
- 缺少 `bytes` 但有 `num_pages` 和 `page_size`：允许估算 duration，但必须标记估算。
- 缺少 policy 配置：使用 base config 的 policy；如果 base config 也缺失，则拒绝 policy-dependent mutation。

## 并行与互联建模

并行策略子模块应逐步覆盖：

- TP rank 数与 workload 切分；
- collective 数量和大小；
- cross-rank dependency；
- DP 请求分流；
- pipeline bubble。

互联子模块应逐步覆盖：

- CPU-NPU copy；
- NPU-NPU collective；
- bandwidth + fixed latency；
- sync / barrier 保留。

## 验证

验证不是默认输出。只有 `emit_validation=true` 时生成。

验证分两类：

- 子模块内部状态验证；
- DAG 集成后 E2E 验证。

HiCache 的验证顺序必须先状态、再 DAG 集成。不能用 E2E 对齐掩盖 cache 状态错误。
