# Profiling 开发文档

维护方式：这是 profiling 主线设计文档。更新时直接删改本文件内容，不在这里写流水账、实验结果或阶段分析。
真实 run、验证结果和历史结论维护在 `docs/work_progress.md` 或 `docs/validation/`。

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

`profiling.channels` 只声明启用哪些采集渠道。`python_probe` 和 `ld_preload` 的细节分别由
`profiling.python_probe`、`profiling.ld_preload` 控制。
LD_PRELOAD wrapper 是 C++ 中硬编码的符号拦截点，不支持从 JSON 动态声明任意 native symbol。

## 运行入口

真实 SGLang / KTransformers profiling 必须使用外层容器入口：

```bash
scripts/profile.sh <config.json> --experiment <id>
```

dry-run 和配置展开也优先使用同一入口：

```bash
scripts/profile.sh <config.json> --list-experiments
scripts/profile.sh <config.json> --experiment <id> --dry-run
```

`scripts/internal/profile_runner.py` 是容器内执行器，只允许在下列场景直接调用：

- 已经位于 `scripts/profile.sh` 启动的 framework 容器内；
- dry-run；
- 不启动真实 server 的配置展开检查。

不要在宿主机直接启动真实 SGLang profiling；宿主机 Python 不保证安装 SGLang、torch_npu、Ascend runtime。

## Experiment Suite

suite config 用于在一套采集契约下展开多个 server/input 组合：

```bash
scripts/profile.sh <suite-config.json> --list-experiments
scripts/profile.sh <suite-config.json> --experiments <id-a>,<id-b>
```

suite 的设计语义：

- 顶层 `profiling` 固定一套采集契约；
- `matrix.servers[]` 定义 server 配置维度；
- `matrix.inputs[]` 定义 workload 维度；
- `experiments[]` 可以显式选择 server/input 组合；
- suite 内不允许 server/input/experiment 覆盖或 unset `profiling`；
- suite 只能用于组合和复现采集配置，不能把实验结果写回设计文档。

suite 输出目录保留：

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

HiCache Python probe target 必须显式写 `module` 和 target-level `fact`：

| 字段 | 语义 |
| --- | --- |
| `fact.class` | `invariant_state`、`timing_observation`、`source_actual`、`oracle_state`、`debug_quality` |
| `fact.role` | atomic role，供后端二级分发 |
| `fact.model_input` | 是否进入 modeling 输入集合 |
| `fact.dag_input` | 是否允许进入默认性能 DAG |
| `fact.granularity` | HiCache state 主线要求为 `atomic` |

后端第一层分流只看：

```text
model_input == true
&& fact_class == "invariant_state"
&& fact_granularity == "atomic"
&& event_role is a known atomic invariant
```

其它事件即使出现在 trace 中，也必须写成 `model_input=false`，只能作为 timing、source actual、oracle 或 debug 证据。

| `fact_class` | 用途 |
| --- | --- |
| `invariant_state` | HiCache state model 唯一主输入；必须是 atomic role |
| `timing_observation` | latency/bandwidth 样本，不能直接决定 target state |
| `source_actual` | source run 实际 movement/policy 结果，不能作为 target answer |
| `oracle_state` | validation-only state snapshot / transition oracle |
| `debug_quality` | probe 内部质量审计和排查 |

## HiCache State Target Contract

当前正常 state model input 只允许下列 `invariant_state` role：

| role | 语义 |
| --- | --- |
| `request_bound_match_anchor` | request-scoped match-prefix token anchor |
| `request_lifecycle_anchor` | finished/unfinished lifecycle 边界 anchor |
| `request_admission` | admission boundary 的 request token path、admission kind 和 policy |
| `prefetch_decision` | scheduler prefetch decision checkpoint 的 request token path 和策略参数 |
| `prefetch_check_point` | request 时间线上的 prefetch check/wait 边界 |

source evidence 可以与 invariant target 采自同一个 Python callable，但必须拆成独立 target：

| evidence role family | fact class | 设计边界 |
| --- | --- | --- |
| cache-stage concrete path / lookup result | `source_actual` | 只描述 source run 已发生结果，不更新 target state |
| lifecycle path/runtime | `source_actual` | 只作为 provenance，不混入 lifecycle anchor |
| insert / capacity / lock / maintenance / storage/controller event | `source_actual` 或 `timing_observation` | 只能用于质量审计、oracle/debug 或后续 target-derived 机制设计 |

`sglang.hicache` probe 可以自动 patch SGLang 内部方法并输出 `source_actual` 事件，例如 radix split/delete、
device/host evictable delta、host ref delta、KV node store/remove、load-back、write-back enqueue/start、
write/load ack checkpoint、storage control checkpoint、controller prefetch enqueue、rate-limit、storage hit query、
prefetch terminate、abort cleanup 和 host memory release enqueue。这些事件默认不是 normal state input。

注意：

- match-prefix path 不再以 `request_tokens` / `lookup_path` 混合 role 出现；
- request-bound anchor 只在 request id 存在时发出；
- concrete cache-stage path 另作为 evidence 保留；
- raw `request_id` 只用于单 run 内关联 request-scoped fact，不能作为跨配置 invariant value；
- 跨配置签名必须归一化到 token path / request fingerprint。

validation-only state snapshot 由 `profiling.python_probe.state_trace.enabled=true` 打开。它写成
`fact_class=oracle_state`、`model_input=false`，只能给 `profile_quality.py` 和 `model_runner.py` 的 validation 路径使用。

## Token / Range 主事实

invariant profile 以 token dictionary + span 引用为核心：

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

质量审计只输出采集质量和合同缺口，不判断 state model 是否正确。

## LD_PRELOAD

LD_PRELOAD 目录为 `src/profiling/ld_preload`，是独立 C++ hook 框架。framework profile 可以复用
AscendCL runtime wrapper 补充 sync/event anchor：

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

state-only profiling suite 不能被当作性能 DAG 证据。需要 faithful replay 或 cache patch 时，应新建/补充完整执行 trace suite。

## HiCache Phased Workload

`scripts/bench/hicache_phased_workload.py` 用于 deterministic HiCache 机制覆盖。phase 语义：

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
