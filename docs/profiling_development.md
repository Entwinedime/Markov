# Profiling 开发文档

维护方式：这是 profiling 主线文档。更新时直接删改本文件内容，不在这里写流水账。

## 目标

Profiling 只采集真实运行事实，为 trace graph、state model 和后续 what-if 提供输入。采集阶段不判断
target 配置下应该发生什么，也不生成 target 行为答案。

主流程：

```text
experiment config
  -> scripts/profile.sh
  -> container-side profile_runner.py
  -> torch / python_probe / ld_preload 分渠道采集
  -> profile_manifest.json
  -> profile_quality.py / model_runner.py
```

Profiling 应回答：

- 哪些真实事件发生了；
- 事件属于哪个进程、线程、rank、device 或 stream；
- 事件时间、持续时间和关联 id；
- request、operation、token path、cache scope、storage IO 等身份事实；
- 某个建模子模块所需事实是否齐备。

Profiling 不回答：

- target 配置下 cache / prefetch / writeback / eviction 应该如何变化；
- DAG 应如何 patch；
- E2E 预测是多少；
- state mismatch 应由哪个模型规则修复。

## 采集渠道

| 渠道 | 控制方式 | 主要用途 | 输出位置 |
| --- | --- | --- | --- |
| `torch` | runner 调用 framework profiler API | CPU op、device kernel、copy、runtime correlation | `trace/torch/` |
| `python_probe` | runner 注入 `src/profiling/python_probe` 到 server `PYTHONPATH` | request、scheduler、HiCache token/range facts、state oracle | `trace/python_probe/` |
| `ld_preload` | runner 注入 C++ hook so | Python 看不到的 native runtime、AscendCL sync/event anchor | `trace/ld_preload/` |

`python_probe` 和 `ld_preload` 分别由 `profiling.python_probe`、`profiling.ld_preload` 控制。
LD_PRELOAD wrapper 是 C++ 中硬编码的符号拦截点，不支持从 JSON 动态声明任意 native symbol。

## 运行入口

真实 SGLang / KTransformers profiling 必须使用外层容器入口：

```bash
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json --experiment s1a_manual
```

dry-run 和配置展开也优先使用同一入口：

```bash
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json --list-experiments
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json --experiment s1a_manual --dry-run
```

`scripts/internal/profile_runner.py` 是容器内执行器，只允许在下列场景直接调用：

- 已经位于 `scripts/profile.sh` 启动的 framework 容器内；
- fixture / dry-run；
- 不启动真实 server 的配置展开检查。

不要在宿主机直接启动真实 SGLang profiling；宿主机 Python 不保证安装 SGLang、torch_npu、Ascend runtime。

## Experiment Suite

HiCache state 主线使用 suite config：

```bash
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json --list-experiments
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json --experiments s1a_manual,s1b_manual
```

suite 的语义：

- 顶层 `profiling` 固定一套采集契约；
- `matrix.servers[]` 定义 server 配置维度；
- `matrix.inputs[]` 定义 workload 维度；
- `experiments[]` 可以显式选择 server/input 组合；
- suite 内不允许 server/input/experiment 覆盖或 unset `profiling`。

当前 suite 展开为：

| experiment | server | input |
| --- | --- | --- |
| `s1a_manual` | page128、write-through-selective、wait_complete | phased manual workload |
| `s1a_bench` | page128、write-through-selective、wait_complete | bench serving workload |
| `s1b_manual` | page64、write-back、best_effort | phased manual workload |
| `s1b_bench` | page64、write-back、best_effort | bench serving workload |

suite 输出目录会保留：

| 文件 | 作用 |
| --- | --- |
| `suite_config.json` | 本次使用的 suite config 归档 |
| `suite_selection.json` | 全部可选实验与本次选择 |
| `suite_result.json` | 已完成/失败 run 摘要 |
| `<experiment>/profile_manifest.json` | 单个实验的 manifest |

## Python Probe

当前 active Python probe 位于 `src/profiling/python_probe`，采用：

```text
sitecustomize.py
  -> import hook
  -> trace_sim_probe.probes.generic_callable
  -> trace_sim_probe.probes.sglang_hicache_callable
```

关键环境变量：

| 变量 | 作用 |
| --- | --- |
| `TRACE_SIM_PYTHON_PROBE=1` | 打开 Python probe bootstrap |
| `TRACE_SIM_PYTHON_PROBES` | probe 插件列表；HiCache state 使用 `sglang.hicache` |
| `TRACE_SIM_PYTHON_PROBE_TARGETS` | target JSON 数组 |
| `TRACE_SIM_PYTHON_PROBE_OUTPUT` | Chrome trace 输出目录 |
| `TRACE_SIM_PYTHON_PROBE_DEBUG=1` | probe debug 日志 |
| `TRACE_SIM_HICACHE_STATE_TRACE=1` | 允许 `sglang.hicache` 采集 validation-only state snapshot |

通用 callable source 由 `generic_callable` 提供；HiCache 特化 source 只存在于
`sglang_hicache_callable.py`：

| source | 作用 |
| --- | --- |
| `token_path:<source>[,<scope_source>]` | 输出 token dictionary；同一 scope/path 只在首次包含完整 `token_ids` |
| `token_span:<source>` | 输出 `{path_id, begin, end, token_count, hash_algo}` |
| `request_token_path:<req>,<mode>[,<scope>]` | 从 SGLang `Req` 输出 request token dictionary；`mode=fill/committed/origin_output` |
| `request_token_span:<req>,<mode>` | 从 SGLang `Req` 输出 request token span |
| `request_token_count:<req>,<mode>` | 从 SGLang `Req` 输出 request token 数 |
| `token_path_concat:<prefix>,<suffix>[,<scope>]` | 拼接两段 token 后输出 dictionary |
| `token_span_concat:<prefix>,<suffix>` | 拼接两段 token 后输出 span |
| `node_token_path:<node>[,<scope>]` | 从 radix node 的 parent chain 还原完整 token path |
| `node_token_span:<node>` | 从 radix node 输出完整 path span |
| `node_token_count:<node>` | 统计 radix node full key token 数 |
| `hicache_node_summary:<node>` | 输出 node id、parent、token span、hash、device/host/ref/child 摘要 |
| `hicache_node_chain:<node>` | 输出 root 到 node 的链式摘要 |
| `hicache_evictable_snapshot:<cache>` | 输出 device/host evictable 候选数量和样本 |
| `hicache_prefetch_progress:<cache>,<req_id>` | 输出 source prefetch progress 证据；只用于 observed/debug |
| `hicache_request_runtime:<req>` | 输出 source request runtime 摘要；只用于 observed/debug |
| `hicache_scheduler_prefetch_state:<scheduler>,<req>` | 输出 source scheduler prefetch 判定摘要；只用于 observed/debug |
| `hicache_cache_scope:<source>` | 输出 rank + cache object 作用域 |
| `hicache_seq:<source>` | 在 cache scope 内生成单调逻辑序号 |
| `hicache_config:<source>[,<field>]` | 读取 source cache 配置摘要，用于质量审计和解释 |
| `hicache_requested_pages:<tokens>,<cache>` | 按 source page size 计算请求页数摘要 |
| `hicache_state:self` | validation-only state snapshot，`model_input=false` |

旧的 `page_hashes:*` / `target_page_identity_page<page_size>` 不再是当前 HiCache state 主契约。
state backend 从 token dictionary/span 和 target page size 重建 page hash。

## HiCache 事件分类

HiCache Python probe 事件必须显式写入：

| 字段 | 语义 |
| --- | --- |
| `model_input` | 是否进入 modeling 输入集合 |
| `dag_input` | 是否允许进入默认性能 DAG |
| `state_model_input` | 是否允许 HiCache state model 消费 |
| `fact_class` | `invariant_state`、`timing_observation`、`source_actual`、`oracle_state`、`debug_quality` |
| `event_role` | role 级语义，供后端二级分发 |

后端第一层分流只看：

```text
fact_class == "invariant_state" && state_model_input == true
```

其它事件即使出现在 trace 中，也只能作为 timing、source actual、oracle 或 debug 证据。

| `fact_class` | `state_model_input` | `dag_input` | 用途 |
| --- | --- | --- | --- |
| `invariant_state` | true | false | HiCache state model 唯一主输入 |
| `timing_observation` | false | 按实验决定 | latency/bandwidth 样本，不能直接决定 target state |
| `source_actual` | false | 按实验决定 | source run 实际 movement/policy 结果，不能作为 target answer |
| `oracle_state` | false | false | validation-only state snapshot / transition oracle |
| `debug_quality` | false | false | probe 内部质量审计和排查 |

## 当前 HiCache State Targets

`configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json` 当前配置 31 个
HiCache target：16 个 `invariant_state`、13 个 `source_actual`、2 个 `timing_observation`。新的口径把“可重放输入”和“source 已发生结果”分开：同一个 Python callable 可以有一个
`invariant_state` target 和一个并行 `source_actual` target。

| target id | role | fact class | state input | 说明 |
| --- | --- | --- | --- | --- |
| `hiradix.request_tokens` | `request_tokens` | `invariant_state` | true | 记录 request full token path |
| `hiradix.lookup_path` | `lookup_path` | `invariant_state` | true | 只记录 lookup full token path；matched/source hit 已拆出 |
| `hiradix.lookup_result_observed` | `lookup_result_observed` | `source_actual` | false | 记录 source matched node、device/host hit、node chain 和 evictable snapshot |
| `hiradix.cache_config_observed` | `cache_config_observed` | `invariant_state` | true | 记录 source cache 配置摘要；target config 仍由 modeling 显式给出 |
| `hiradix.cache_finished_req` | `request_cache_lifecycle` | `invariant_state` | true | 记录 finished request 的 committed token path、is_insert 和 priority |
| `hiradix.cache_finished_req_observed` | `request_cache_lifecycle_observed` | `source_actual` | false | 记录 source request runtime、cache_protected_len 和 last node 证据 |
| `hiradix.cache_unfinished_req` | `request_cache_lifecycle` | `invariant_state` | true | 记录 unfinished/chunked request 的 fill token path、chunked 和 priority |
| `hiradix.cache_unfinished_req_observed` | `request_cache_lifecycle_observed` | `source_actual` | false | 记录 source unfinished request runtime 和 last node 证据 |
| `scheduler.prefetch_decision` | `prefetch_decision` | `invariant_state` | true | 只记录 scheduler prefetch decision checkpoint 的 request token path 和策略参数 |
| `scheduler.prefetch_decision_observed` | `prefetch_decision_observed` | `source_actual` | false | 记录 source prefix/host hit/new-input/anchor 判定摘要 |
| `schedule_policy.prefill_admission` | `request_admission` | `invariant_state` | true | 记录 `PrefillAdder.add_one_req` 的 request token path、admission kind、chunk/truncation 参数和 policy；不记录 source admission result |
| `schedule_policy.prefill_admission_observed` | `request_admission_observed` | `source_actual` | false | 记录 source admission return、request runtime 和 phase-scoped budget snapshot |
| `schedule_policy.chunked_admission` | `request_admission` | `invariant_state` | true | 记录 `PrefillAdder.add_chunked_req` 的 chunked continuation request token path 和 policy |
| `schedule_policy.chunked_admission_observed` | `request_admission_observed` | `source_actual` | false | 记录 source chunked continuation return、request runtime 和 phase-scoped budget snapshot |
| `hiradix.insert_path` | `insert_path` | `invariant_state` | true | 只记录 insert full path、value token 数、chunked 和 priority；prefix/inserted source result 已拆出 |
| `hiradix.insert_result_observed` | `insert_result_observed` | `source_actual` | false | 记录 source prefix_len、inserted node 和 evictable snapshot |
| `hiradix.prefetch_intent` | `prefetch_intent_observed` | `source_actual` | false | 记录 source 已发起的 prefetch prefix/suffix intent，不驱动 target prefetch |
| `hiradix.prefetch_check_point` | `prefetch_check_point` | `invariant_state` | true | 记录请求时间线上的 prefetch check/wait 边界 |
| `hiradix.prefetch_progress_observed` | `prefetch_progress_observed` | `source_actual` | false | 记录 source prefetch completed/revoked/progress 证据 |
| `hiradix.maintenance_check` | `maintenance_checkpoint` | `invariant_state` | true | 记录 `check_hicache_events` 维护检查点 |
| `hiradix.ready_to_load_host_cache` | `maintenance_checkpoint` | `invariant_state` | true | 记录 host->device load queue 启动检查点 |
| `hiradix.flush_write_through_acks` | `maintenance_checkpoint` | `invariant_state` | true | 记录 write-through ack flush 检查点 |
| `hiradix.capacity_request` | `capacity_request` | `invariant_state` | true | 记录 capacity request 的 token/page 需求，不记录 source victim |
| `hiradix.capacity_result_observed` | `capacity_result_observed` | `source_actual` | false | 记录 source evicted tokens 和 evictable snapshot |
| `hiradix.lock_scope_inc` | `lock_scope_delta` | `invariant_state` | true | 记录 logical path 上的 lock/ref increase，不记录 source delta |
| `hiradix.lock_scope_inc_observed` | `lock_scope_result_observed` | `source_actual` | false | 记录 source inc return delta、node chain 和 evictable snapshot |
| `hiradix.lock_scope_dec` | `lock_scope_delta` | `invariant_state` | true | 记录 logical path 上的 lock/ref decrease，不记录 source delta |
| `hiradix.lock_scope_dec_observed` | `lock_scope_result_observed` | `source_actual` | false | 记录 source dec return delta、node chain 和 evictable snapshot |
| `hicache_controller.prefetch_io_observed` | `prefetch_io_observed` | `timing_observation` | false | 真实 prefetch IO 样本，不驱动 target ready |
| `hicache_controller.writeback_io_observed` | `writeback_io_observed` | `timing_observation` | false | 真实 writeback IO 样本 |
| `hicache_controller.writeback_enqueue_observed` | `writeback_enqueue_observed` | `source_actual` | false | source writeback enqueue 事实，不驱动 target flushed pages |

`sglang.hicache` probe 还会自动 patch 下列内部方法并输出 `source_actual` 事件：radix split/delete、
device/host evictable delta、host ref delta、KV node store/remove、load-back、write-back enqueue/start、write hit counter delta、write/load ack checkpoint、storage control
checkpoint、controller prefetch enqueue、rate-limit、storage hit query、prefetch terminate、abort cleanup 和 host memory release enqueue。这些事件用于
provenance 和 oracle 对照，默认 `state_model_input=false`。

validation-only state snapshot 由 `profiling.python_probe.state_trace.enabled=true` 打开。它写成
`fact_class=oracle_state`、`model_input=false`、`state_model_input=false`，只能给
`profile_quality.py` 和 `model_runner.py` 的 validation 路径使用。

## Token / Range 主事实

新的 invariant profile 以 token dictionary + span 引用为核心：

| 字段 | 必需性 | 说明 |
| --- | --- | --- |
| `token_path_id` | 必需 | 完整 token 序列的内容 hash，当前为 `sha256_u32le:<hex>` |
| `token_ids` | dictionary 首次出现必需 | 完整 token id 序列，只在 dictionary 事件首次携带 |
| `token_span` / `full_path_span` / `prefix_span` / `suffix_span` | 按 role 必需 | 引用 token path 的闭开区间 |
| `hash_algo` | 必需 | 当前为 `sglang_radix_sha256_v1` |
| `cache_scope` | 必需 | rank + cache object 作用域 |
| `seq_no` | 必需 | 同一 cache scope 内单调递增逻辑顺序 |

后端根据 token 序列和 target `page_size` 生成 page identity。新增 target page size 不应要求新增
`target_page_identity_page<page_size>` 字段。

## Profile Quality

profiling 完成后运行：

```bash
python3 scripts/internal/profile_quality.py \
  --manifest <run_dir>/profile_manifest.json \
  --output <run_dir>/profile_quality.json
```

质量审计检查：

- trace 文件是否存在；
- Python probe target 是否命中；
- required fields 是否缺失；
- invariant target 是否误带 source-result 字段；
- workload 声明的 HiCache 机制是否实际出现；
- `invariant_state` 是否具备 token dictionary/span、cache scope、seq_no；
- token span 是否都能找到 dictionary；
- `seq_no` 是否在 scope 内有序；
- state trace 开启时是否采到 capacity snapshot。

旧 `01_s1a_manual` 质量结果如下。它来自 2026-06-10 已完成 profile，不是当前 31-target 契约的重跑结果；
当前契约重跑前只能把它作为历史基线：

| 指标 | 值 |
| --- | --- |
| `quality_ready` | true |
| `profiling_ready` | true |
| invariant events | 6960 |
| required end events | 3480 |
| missing required fact events | 0 |
| token dictionary paths | 172 |
| missing token dictionary refs | 0 |
| seq scope count | 2 |
| seq order errors | 0 |

## LD_PRELOAD

LD_PRELOAD 目录为 `src/profiling/ld_preload`，是独立 C++ hook 框架。当前 `sglang` profile 主要复用
AscendCL runtime wrapper，用于补充 sync/event anchor：

| wrapper | 用途 |
| --- | --- |
| `aclrtSynchronizeStream` / `aclrtSynchronizeStreamWithTimeout` | stream 同步等待 |
| `aclrtSynchronizeEvent` / `aclrtSynchronizeEventWithTimeout` | event 同步等待 |
| `aclrtSynchronizeDevice` / `aclrtSynchronizeDeviceWithTimeout` | device 全局同步 |
| `aclrtRecordEvent` | event record anchor |
| `aclrtStreamWaitEvent` | stream wait event anchor |

构建入口：

```bash
scripts/internal/hooks/build.sh sglang
scripts/internal/hooks/build.sh ktransformers
scripts/internal/hooks/build.sh ascendcl
scripts/internal/hooks/build.sh ld_preload
```

真实 HiCache state suite 目前关闭 torch profiler 和 LD_PRELOAD，只采 Python probe。需要 faithful replay 或
cache patch 时，应新建/补充完整执行 trace suite，不能把 state-only suite 当作性能 DAG 证据。

## HiCache Phased Workload

`scripts/bench/hicache_phased_workload.py` 用于 deterministic HiCache 机制覆盖。当前 phase：

| phase | 作用 |
| --- | --- |
| `seed_A` | 建立 A 前缀和首次插入 |
| `reuse_A` | 复用 A 前缀，验证 prefix hit |
| `backup_wait_A` | 提高 selective write 触发概率 |
| `pressure_B` | 构造 cache pressure 和 eviction |
| `reuse_A_after_pressure` | 压力后复用 A，验证 load/backfill 路径 |
| `prefetch_seed_C` | 建立 C 前缀和 storage backup |
| `prefetch_reuse_C` | 触发 prefetch intent/check point |
| `dirty_eviction` | write-back 场景验证 dirty eviction/writeback |

`--hicache-ratio` 必须大于 `1.0`。容量压力优先通过 workload 和显式 capacity config 构造，不用小于等于
`1.0` 的 ratio 制造异常场景。
