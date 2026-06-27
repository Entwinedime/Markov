# 采集开发文档

维护方式：这是 profiling 主线设计文档。更新时直接删改本文件内容，不在这里写流水账、实验结果或阶段分析。
真实 run、验证结果和历史结论维护在 `docs/work_progress.md` 或 `docs/validation/`。

## 目标

Profiling 只采集真实运行事实，为 trace graph、state model 和后续 what-if 提供输入。采集阶段不判断
target 配置下应该发生什么，也不生成 target 行为答案。

主流程：

```text
experiment config
  -> scripts/profile.sh
  -> scripts/internal/entrypoints/profile.py
  -> torch / python_probe / ld_preload 分渠道采集
  -> profile_manifest.json
  -> scripts/internal/entrypoints/profile_quality.py / scripts/model.sh
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

Profiling 运行在 framework runtime Docker 容器中：`sglang-profile` 对应 SGLang，`ktransformers-profile` 对应
KTransformers。这两个容器包含 Ascend/CANN、torch_npu、对应框架源码安装层和 LD_PRELOAD hook 的 ABI 上下文。
宿主机不作为真实 profiling 或 C++ hook 编译验收环境。

构建 framework runtime 镜像和 hook：

```bash
scripts/build.sh sglang
scripts/build.sh ktransformers
```

独立的 `modeling` Docker service 只用于 C++ TraceGraph / modeling 编译和运行检查，不用于真实 server profiling，
也不要求安装 Ascend/CANN。

dry-run 和配置展开也优先使用同一入口：

```bash
scripts/profile.sh <config.json> --list-experiments
scripts/profile.sh <config.json> --experiment <id> --dry-run
```

`scripts/internal/entrypoints/profile.py` 是容器内执行器，只允许在下列场景直接调用：

- 已经位于 `scripts/profile.sh` 启动的 framework 容器内；
- dry-run；
- 不启动真实 server 的配置展开检查。

不要在宿主机直接启动真实 SGLang profiling；宿主机 Python 不保证安装 SGLang、torch_npu、Ascend runtime。

## 实验 Suite

suite config 用于在一套采集契约下展开多个 server/input 组合：

当前 `configs/` 只维护 cache-state 开发主链的三套配置：

| 配置 | 用途 |
| --- | --- |
| `profiling_hicache_state_common.json` | 普通生成的 common suite；用于采集诊断和 self prediction。 |
| `profiling_hicache_state_forced_capture.json` | 三个 manual input 的 immutable plan/bundle capture。 |
| `profiling_hicache_state_forced_replay.json` | 显式 bundle 驱动的 5×3 cross-config replay。 |

不维护 S1A/S1B、smoke 或 faithful profiling suite。
common suite 只用于 quality、采集诊断和 self prediction；`--prediction-scope cross` 必须使用 forced replay suite。

```bash
scripts/profile.sh <suite-config.json> --list-experiments
scripts/profile.sh <suite-config.json> --experiments <id-a>,<id-b>
scripts/profile.sh <suite-config.json> --inputs <input-a>,<input-b>
scripts/profile.sh <suite-config.json> --servers <server-a>,<server-b>
scripts/profile.sh <forced-replay-suite.json> --forced-token-bundle <bundle.json>
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
| `suite_selection.json` | suite schema、profile mode、metadata、全部可选实验、本次 selector 和 forced-token preflight 摘要 |
| `suite_result.json` | planned/attempted/completed/failure/aborted count、status、run 列表和 forced-token contract 聚合摘要 |
| `<experiment>/profile_manifest.json` | 单个实验的 manifest |
| `forced_token_bundle.json` | forced capture suite 的稳定输出合同；replay 通过 CLI 显式消费 |
| `forced_token_plans/<input_id>.json` | bundle 内按 input 聚合的 plan；路径相对 bundle 保存 |

## Python Probe 采集

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
| `TRACE_SIM_PYTHON_PROBE_FLUSH_EVERY` | streaming writer 每多少条 event flush 一次，默认 256 |
| `TRACE_SIM_HICACHE_CONSUMERS` | 本次 profile 请求的 HiCache fact consumers |
| `TRACE_SIM_HICACHE_INTERNAL_HOOKS` | 显式开关 HiCache internal source/timing hooks |
| `TRACE_SIM_PYTHON_PROBE_DEBUG=1` | probe debug 日志 |

HiCache Python probe target 由共享 catalog 维护，默认路径为 `configs/profiling/hicache_probe_targets.json`。
该文件本身就是 target 对象数组，不包额外顶层结构。
profile config 的 `profiling.python_probe` 只声明本次需要的 consumer，例如：

```json
{
  "name": "sglang.hicache",
  "consumers": [
    "hicache_state_model",
    "hicache_profile_quality",
    "hicache_input_contract",
    "hicache_final_state_validator",
    "hicache_transition_validator"
  ],
  "flush_every": 256
}
```

runner 会读取 catalog，选择 `requested_consumers ∩ target.fact.consumers` 非空的 target，并把输出 target 中的
`fact.consumers` 收紧为本次交集。profile config 不能内嵌 target、class filter 或 state-trace 开关。

Python probe writer 使用 streaming Chrome trace 输出；正常退出时补齐 JSON 结尾。source/timing internal hooks
由 requested consumers 推导：transition validator 或 profile quality 需要 source/timing evidence 时才安装。

单个 target 默认只发 `end` phase；需要 start/exception 事件时必须在 target 上显式写
`"phases": ["start", "end", "exception"]` 或等价的 `emit_phases`。HiCache state 主线不需要 start phase。

通用 callable source 由 `generic_callable` 提供；HiCache 特化 source 只存在于
`sglang_hicache_callable.py`：

| source | 作用 |
| --- | --- |
| `token_path:<source>[,<scope_source>]` | 输出 token dictionary；同一 scope/path 在 state-model fact 与 diagnostic evidence 各自去重域内首次包含完整 `token_ids` |
| `token_span:<source>` | 输出 `{path_id, begin, end, token_count, hash_algo}` |
| `request_token_path:<req>,<mode>[,<scope>]` | 从 SGLang `Req` 输出 request token dictionary；`mode=fill/committed/admission/prefetch/origin_output` |
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
| `hicache_state:self` | validation-only state snapshot，写成 `oracle_state/state_snapshot` fact |

旧的 `page_hashes:*` / `target_page_identity_page<page_size>` 不再是当前 HiCache state 主契约。
state backend 从 token dictionary/span 和 target page size 重建 page hash。

request path mode 必须对应当前 SGLang 调用边界：

| mode | 当前读取语义 |
| --- | --- |
| `fill` | `cache_unfinished_req()` 使用的 `Req.get_fill_ids()`；仅在 API 不可用时回退旧字段。 |
| `committed` | `origin_input_ids + output_ids` 按 `kv_committed_len` / `_cache_commit_len()` 截断。 |
| `admission` | `PrefillAdder` 接受本轮请求后可见的 fill path，优先使用 `get_fill_ids()`。 |
| `prefetch` | `_prefetch_kvcache()` 的 `full_untruncated_fill_ids[:_compute_max_prefix_len(...)]` 候选 path；不能用 fill path 代替。 |
| `origin_output` | 完整 `origin_input_ids + output_ids`，只在明确需要完整 request path 时使用。 |

`RadixKey` 必须按其迭代和长度语义读取，不能直接绕过 `limit` / bigram view 读取底层 `.token_ids`。
配置 target 与 full/debug internal hook 需要跟随当前 SGLang callable 签名；缺少 request 归属的 `InsertParams` /
`EvictParams` 只能输出 optional `request_id`，不能伪造归属。

## HiCache 事件分类

HiCache Python probe target 必须在共享 catalog 中显式写 `module` 和 target-level `fact`：

| 字段 | 语义 |
| --- | --- |
| `fact.class` | `workload_identity`、`target_policy_input`、`runtime_model_checkpoint`、`source_actual`、`timing_observation`、`oracle_state`、`debug_quality` |
| `fact.role` | class 内的事实角色，供 consumer 二级分发 |
| `fact.consumers` | 可消费该事实的模型、质量审计或 validator 列表 |

catalog target 示例：

```json
{
  "id": "hiradix.cache_finished_req.runtime_observed",
  "module": "sglang.srt.mem_cache.hiradix_cache",
  "target": "HiRadixCache.cache_finished_req",
  "events": [
    "hicache_cache_finished_req_runtime_observed_start",
    "hicache_cache_finished_req_runtime_observed_end"
  ],
  "fields": [],
  "fact": {
    "class": "source_actual",
    "role": "request_lifecycle_runtime_observed",
    "consumers": [
      "hicache_profile_quality",
      "hicache_transition_validator"
    ]
  }
}
```

后端第一层分流只看 completed/end-phase 的 `fact.consumers` 和 `fact.class/fact.role`。其它事件即使出现在
trace 中，也只能作为 timing、source actual、oracle 或 debug 证据。

| `fact.class` | 用途 |
| --- | --- |
| `workload_identity` | 跨配置 workload/request lifecycle 身份事实，可供 state model 和 input contract 消费 |
| `target_policy_input` | target policy 需要重新决策的输入事实 |
| `runtime_model_checkpoint` | source runtime 暴露的模型推进 checkpoint；可随 config 变化 |
| `timing_observation` | latency/bandwidth 样本，不能直接决定 target state |
| `source_actual` | source run 实际 movement/policy 结果，不能作为 target answer |
| `oracle_state` | validation-only state snapshot / transition oracle |
| `debug_quality` | probe 内部质量审计和排查 |

## HiCache 状态采集目标合同

当前正常 state model 只消费下列 fact：

| fact | 语义 |
| --- | --- |
| `workload_identity/request_bound_match_anchor` | request-scoped match-prefix token anchor |
| `workload_identity/request_lifecycle_anchor` | finished/unfinished lifecycle 边界 anchor；必须携带当前 committed/fill `token_dictionary`、`full_path_span` 和 `token_count` |
| `workload_identity/request_admission` | admission boundary 的 request token path、admission kind 和 policy |
| `target_policy_input/prefetch_decision` | scheduler prefetch decision checkpoint 的 request token path 和策略参数 |
| `runtime_model_checkpoint/prefetch_check_point` | request 时间线上的 prefetch check/wait 边界 |

source evidence 可以与 state-model target 采自同一个 Python callable，但必须拆成独立 target：

| evidence role family | fact class | 设计边界 |
| --- | --- | --- |
| cache-stage concrete path / lookup result | `source_actual` | 只描述 source run 已发生结果，不更新 target state |
| lifecycle path/runtime | `source_actual` | 只作为 provenance/quality 对照；state model 只能消费 workload lifecycle anchor 上的 path |
| insert / capacity / lock / maintenance / storage/controller event | `source_actual` 或 `timing_observation` | 只能用于质量审计、oracle/debug 或后续 target-derived 机制设计 |

`sglang.hicache` probe 在 requested consumers 需要 source/timing evidence 时可以 patch SGLang 内部方法并输出 `source_actual` 事件，例如 radix split/delete、
device/host evictable delta、host ref delta、KV node store/remove、load-back、write-back enqueue/start、
write/load ack checkpoint、storage control checkpoint、controller prefetch enqueue、rate-limit、storage hit query、
prefetch terminate、abort cleanup 和 host memory release enqueue。这些事件默认不是 normal state input。

注意：

- match-prefix path 不再以 `request_tokens` / `lookup_path` 混合 role 出现；
- request-bound anchor 只在 request id 存在时发出；
- concrete cache-stage path 另作为 evidence 保留；
- raw `request_id` 只用于单 run 内关联 request-scoped fact，不能作为跨配置 workload identity；
- 跨配置签名必须归一化到 token path / request fingerprint。

validation-only state snapshot 由 `hicache_final_state_validator` 或 `hicache_transition_validator` consumer 请求。它写成
`oracle_state/state_snapshot` fact，只能给 profile quality 和 modeling validation 路径使用。

## HiCache 采集输入层级

HiCache 从 state alignment 推进到 DAG patch / E2E prediction 时，不同阶段需要的 profiling 强度不同。配置和文档必须显式区分
state 输入、物理执行证据和验证标签。

| 阶段 | Profiling 种类 | 高层含义 | 主要用途 |
| --- | --- | --- | --- |
| 概念整理 | 不新增 profiling，只使用 schema / 文档 / 已有样本 | 统一概念和输出结构 | 设计边界、summary 结构、字段契约。 |
| 状态输入 | Python probe state-model facts | target-independent semantic anchors 和 token dictionary/span | target state、transition、target intent。 |
| 状态验证 | Python probe state-model facts + oracle snapshot | 状态输入加 validation label，不进入 state model | final state validation。 |
| 物理证据 | source physical profiling | torch / LD_PRELOAD / physical timing evidence，加 workload anchors 做对齐 | source DAG 中 cache-owned node / edge 归因。 |
| faithful profiling | full faithful profiling | 状态输入 + 物理证据；必要时保留显式标记的 timing/source evidence | cache-neutral DAG、DAG patch、duration calibration、self reconstruction。 |
| target E2E oracle | target E2E oracle profiling | 对真实 target config 跑 full faithful profiling | cross-config E2E prediction 验收标签。 |

当前 HiCache state / transition alignment 仍可使用 full Python probe，因为还需要 `source_actual`、`timing_observation` 和
`oracle_state` 做排查、transition oracle 抽取和验证标签。进入 normal state / target intent 主线时，应优先退回状态输入或状态验证采集，
避免 full probe 自身开销污染 E2E 标签。进入 DAG patch / faithful replay 时，必须补物理证据或 full faithful profiling；只有
Python probe 的 state-fact trace 不能作为完整性能 DAG 证据。

## Token / Range 主事实

state-model fact profile 以 token dictionary + span 引用为核心：

| 字段 | 必需性 | 说明 |
| --- | --- | --- |
| `token_path_id` | 必需 | 完整 token 序列的内容 hash，当前为 `sha256_u32le:<hex>` |
| `token_ids` | dictionary 首次出现必需 | 完整 token id 序列；completed/end-phase state-model fact 与 diagnostic evidence 分开去重，不能由 `source_actual` 补齐 state-model path |
| `token_span` / `full_path_span` / `prefix_span` / `suffix_span` | 按 role 必需 | 引用 token path 的闭开区间 |
| `hash_algo` | 必需 | 当前为 `sglang_radix_sha256_v1` |
| `cache_scope` | 必需 | rank + cache object 作用域 |
| `seq_no` | 必需 | 同一 cache scope 内单调递增逻辑顺序 |

后端根据 token 序列和 target `page_size` 生成 page identity。新增 target page size 不应要求新增
`target_page_identity_page<page_size>` 字段。

## Forced Token 采集

HiCache cross-config prediction 只有在同一 input 的 token timeline 跨配置一致时才可解释。greedy、固定 seed 和
batch-stable kernel 只能降低输出分叉概率，不能作为 hard contract。需要跨配置验证 generated continuation 时，使用
`scripts/bench/hicache_phased_workload.py` 的 forced token 模式。

forced token profiling 分成两步：

```text
capture:
  text prompt -> real /generate
  response.prompt_token_ids + response.output_ids -> forced_token_plan.json

replay:
  plan.origin_input_ids -> real /generate prefill/decode
  plan.forced_output_ids -> SGLang committed output token override
  workload_report 校验 actual output_ids == forced_output_ids
```

关键约束：

- replay 必须提交 `input_ids`，不能提交完整 `prompt + output` 作为长 prefill；
- replay 请求携带 `trace_sim_forced_output_ids`，SGLang 在 prefill/decode append `Req.output_ids` 前覆盖 token；
- forward、sampling、scheduler、allocator、KV 写入、HiCache lifecycle 仍真实执行，只有 committed output token 被替换；
- 第一版只支持普通非 streaming `/generate`、`n=1`、`ignore_eos=true`、无 stop/grammar/speculative/disaggregation 复杂路径；
- C++ state model 不消费 forced token provenance，只消费 Python probe 输出的 state-model token dictionary/span。

真实 profiling 主流程必须通过 `scripts/profile.sh`，capture / replay 使用显式 suite config：

```bash
scripts/profile.sh \
  configs/experiments/hicache_state/profiling_hicache_state_forced_capture.json \
  --inputs manual_phased_fast,manual_pressure_prefetch,manual_deeper_pressure_prefetch

CAPTURE_BUNDLE=data/profile_runs/sglang/<capture_suite>/forced_token_bundle.json

scripts/profile.sh \
  configs/experiments/hicache_state/profiling_hicache_state_forced_replay.json \
  --inputs manual_phased_fast,manual_pressure_prefetch,manual_deeper_pressure_prefetch \
  --forced-token-bundle "$CAPTURE_BUNDLE"
```

capture suite 只负责生成 plan contract：server 使用最小 deterministic 配置，不启用 HiCache，不启用 Python probe、torch profiler
或 LD_PRELOAD。每个 capture experiment 默认在自己的 bench output 下写 `forced_token_plan.json`；suite 成功后 runner 聚合为：

```text
<capture_suite>/
  forced_token_bundle.json
  forced_token_plans/
    <input_id>.json
```

capture artifact 不允许覆盖；需要新 token timeline 时必须生成新的 capture suite。replay config 只保留
`{forced_token_plan}` 占位符，runner 根据显式 bundle 和 selected input 注入具体 plan。不存在固定 plan fallback，也不会自动读取
latest capture。

`forced_token_bundle.json` 使用 `trace_sim.hicache.forced_token_bundle.v1`，至少记录 bundle id、capture suite/config、
model/server provenance，以及每个 input 的相对 plan path、plan hash、workload fingerprint、request count 和 capture report。
相对 path 允许 capture suite 整体移动或归档。

`scripts/bench/hicache_phased_workload.py` 仍是 suite 内部调用的 workload driver，可用于本地调试，但不作为真实 profiling
主入口。replay profiling 开始前，profile entrypoint 会 preflight plan 文件存在、schema、`workload_id`、
`workload_fingerprint` 和 request 顺序。

suite 级 `suite_selection.json` / `suite_result.json` 会记录 `profile_mode`、selector、planned/attempted/completed/failure/aborted
count 和 `forced_token_contracts` 聚合摘要。默认 fail-fast 也会先写失败结果再退出；preflight 失败发生在 suite 目录创建前。
profiling 结束后先看 suite result，再进入 workload report 和 profile quality。

`workload_report.json` 的 `forced_token` 字段是 profiling input contract：

| 字段 | 语义 |
| --- | --- |
| `mode` | `none` / `capture` / `replay`。 |
| `plan_schema` | 当前为 `trace_sim.hicache.forced_token_plan.v1`。 |
| `plan_sha256` | plan 文件内容摘要；同 input 的 replay run 必须一致。 |
| `plan_capture_run_id` / `plan_capture_config_id` | plan 的 capture provenance；只用于审计，不作为 replay oracle。 |
| `bundle_schema` / `bundle_sha256` / `bundle_id` | replay 显式消费的 capture bundle provenance。 |
| `bundle_plan_sha256` | bundle entry 声明的 plan hash；必须等于实际 `plan_sha256`。 |
| `all_actual_outputs_match_plan` | replay run 的实际 `output_ids` 是否逐请求等于 plan。 |
| `unchecked_count` / `mismatch_count` / `prompt_mismatch_count` | forced replay 的硬失败诊断。 |
| `plan_ready` / `bundle_ready` / `ready` | plan/output 合同、bundle provenance 合同及两者合取；matrix 分别汇总 plan 与 bundle signature。 |

## 采集质量审计

profiling 完成后运行：

```bash
python3 scripts/internal/entrypoints/profile_quality.py \
  --manifest <run_dir>/profile_manifest.json \
  --output <run_dir>/profile_quality.json
```

质量审计检查：

- trace 文件是否存在；
- Python probe target 是否命中；
- required fields 是否缺失；
- state-model fact 是否误带 source-result 字段；
- workload 声明的 HiCache 机制是否实际出现；
- state-model fact 是否具备 token dictionary/span、cache scope、seq_no；
- token span 是否都能找到 dictionary；
- `seq_no` 是否在 scope 内有序；
- forced replay workload 是否使用合法 plan，且 actual `output_ids` 是否全部匹配 plan；
- replay bundle schema/hash/id 是否存在，同 input 下 bundle 是否唯一，bundle entry hash 是否等于实际 plan hash；
- run config 声明 forced capture/replay 时，workload report 必须存在且 mode 必须一致，不能缺失后退化成普通 generate；
- `request_lifecycle_anchor` 是否携带可由 state-model fact 自身解析的 committed/fill path，并与 observed path 对照一致；
- source evidence 出现非空 prefetch intent/enqueue 时，`prefetch_decision` 是否也有非空 candidate path；
- state trace 开启时是否采到 capacity snapshot。

质量审计只输出采集质量和合同缺口，不判断 state model 是否正确。workflow 每次都基于当前代码重新审计 manifest，不复用旧
`quality/*.profile_quality.json` 作为 gate。profile quality entrypoint 的 `quality_ready` 是严格采集覆盖率；
HiCache state workflow 另外计算 `state_quality_ready` 和 `input_contract_ready`，用于判断 final-state / transition 是否能进入建模验证。

严格 coverage 可因某个 workload 未触发声明机制而失败，但只要 state-model path、forced-token、oracle 和 state 输入合同完整，
`state_quality_ready` 仍可为真。两者必须分别报告，不能用 state gate 掩盖 coverage 缺口。

profiling 后的统一 HiCache validation 入口：

```bash
python3 scripts/internal/entrypoints/hicache_workflow.py \
  --profile-run-dir <forced_replay_suite_dir> \
  --output-dir <forced_replay_suite_dir>/modeling/hicache_state_workflow_manual_3inputs \
  --inputs manual_phased_fast,manual_pressure_prefetch,manual_deeper_pressure_prefetch \
  --stages quality,final-state,transition \
  --prediction-scope self,cross \
  --emit-transition-catalog \
  --emit-transition-gates
```

`transition` 依赖同一次 workflow 生成或重新门禁后的 final-state rows，因此 `--stages` 包含 `transition` 时必须同时包含
`final-state`。旧 `hicache_state_matrix_validation.py` 入口已删除。

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

## HiCache Phased Workload 驱动

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
