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
  -> scripts/internal/entrypoints/modeling_workflow.py / scripts/model.sh
```

Profiling 应回答：

- 哪些真实事件发生了；
- 事件属于哪个进程、线程、rank、device 或 stream；
- 事件时间、持续时间和关联 id；
- request、operation、token path、cache scope、storage IO 等身份事实；
- trace/artifact 是否足以进入后续审计。

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

## Full-DAG Profiling Contract

DAG analysis 使用独立的 full-DAG profiling contract。它不是 HiCache state workflow 的附属采集模式，也不能复用只有
Python probe sidecar 的旧 run 来判断 DAG 准确性。

full-DAG run 必须同时启用三类 channel：

```json
{
  "profiling": {
    "enabled": true,
    "channels": ["torch", "ld_preload", "python_probe"],
    "torch": {"enabled": true},
    "ld_preload": {"enabled": true},
    "python_probe": {
      "consumers": [
        "hicache_state_model",
        "hicache_input_contract",
        "hicache_final_state_validator",
        "hicache_transition_validator"
      ]
    }
  }
}
```

当前 HiCache 一阶段 DAG analysis 使用：

```text
configs/experiments/dag_analysis/profiling_hicache_dag_analysis_forced_replay.json
```

该 suite 复用 HiCache forced replay 的 5×3 server/input 矩阵，但采集目标不同：它要求 torch profiler、LD_PRELOAD
native/runtime hook 和 Python probe facts 同时存在。HiCache 只是第一批 domain analyzer；后续其他子模块应新增自己的
`<domain>_dag_analysis` consumer / target，而不是新增一套 `<domain>_workflow.py`。

每个 run 的 `profile_manifest.json` 会写出：

```json
{
  "trace_channel_coverage": {
    "torch_trace_files": 0,
    "ld_preload_trace_files": 0,
    "python_probe_trace_files": 0
  }
}
```

通用 `markov_internal.audit.profile_artifacts` 审计会同时输出 `trace_channel_coverage` 和 `missing_trace_channels`。
full-DAG run 中任一启用 channel 缺失时，`artifact_errors` 包含 `trace_channel_missing`；如果只剩 Python sidecar，则额外包含
`sidecar_only_trace`。这两个 blocker 只说明采集产物不能用于 DAG analysis accuracy / operation visibility 结论，不代表
HiCache state-only workflow 的旧 run 本身非法。

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

## 脚本分层

Profiling 相关脚本按“外层 wrapper、容器内入口、可复用包”三层维护：

| 层级 | 路径 | 职责 |
| --- | --- | --- |
| 宿主机 wrapper | `scripts/profile.sh` | 选择 profiling runtime 容器、挂载仓库和转发 CLI。 |
| 容器内入口 | `scripts/internal/entrypoints/profile.py` | 解析 profiling CLI，调用包内 runner；不直接承载 suite 展开、server 生命周期或采集合同逻辑。 |
| 可复用包 | `scripts/internal/markov_internal/profiling/` | suite/matrix 展开、单 run executor、server/bench env、torch profiler API、artifact 写出、forced-token 运行期 workflow 和 probe target catalog 选择。 |
| post-profile audit | `scripts/internal/markov_internal/audit/` | 采集后通用 artifact audit，例如 trace 文件、manifest 和 Python probe target 命中。 |
| 共享合同 | `scripts/internal/markov_internal/contracts/` | profiling、audit 和 validation 共享的 plan/bundle schema、hash 和 report contract helper。 |
| modeling workflow preflight | `scripts/internal/markov_internal/modeling_workflow/validations/` | DAG trace channel、HiCache state-model input readiness、strict diagnostic coverage 和 workflow gate 所需审计。 |
| 公共工具 | `scripts/internal/markov_internal/common/` | JSON I/O、命令 token 化、路径、命名、日志、trace 读取和进程控制。 |

当前 profiling 包的主要职责边界：

- `profiling.runner` 负责 CLI / suite 编排；
- `profiling.suite` 负责 suite/matrix selector 展开；
- `profiling.executor` 负责单次 server、profiler、bench 和退出清理；
- `profiling.environments` 负责 Python probe、LD_PRELOAD 和 channel 环境变量注入；
- `profiling.forced_workflow` 负责 forced capture 聚合和 replay plan 注入；plan/bundle schema、hash 和 workload report audit helper
  位于 `contracts.forced_token`；
- `profiling.probe_targets` 负责读取 `hicache_probe_targets.json`、校验 fact class/role/consumer，并按 requested consumers 裁剪 target；
- `profiling` 包不解释采集后的 HiCache readiness；通用 artifact audit 位于 `audit.profile_artifacts`，HiCache-specific fact
  coverage 和 state-model 输入合同位于 `modeling_workflow/validations/hicache/preflight`。

当前 post-profile audit 和共享合同职责：

| 路径 | 职责 |
| --- | --- |
| `scripts/internal/markov_internal/audit/profile_artifacts.py` | 通用采集产物审计，不判断某个模型的 consumer contract；可由 workflow preflight 或 `python3 -m markov_internal.audit.profile_artifacts` 调用。 |
| `scripts/internal/markov_internal/modeling_workflow/validations/base_dag/preflight.py` | full-DAG validation 所需的 torch / LD_PRELOAD / Python probe 通道审计。 |
| `scripts/internal/markov_internal/modeling_workflow/validations/hicache/preflight/` | HiCache 单 run 审计、state fact required fields、token dictionary/span、sequence、role coverage、forced-token 和 strict diagnostic coverage。 |
| `scripts/internal/markov_internal/contracts/forced_token.py` | forced-token plan/bundle schema、hash、summary 和 workload report contract helper；profiling capture/replay 与后续 audit 共用。 |

命名约束：

- `artifact_ready` 只说明采集产物自身可读、trace 文件存在、target 命中等低层条件；
- `state_model_input_ready` 说明当前 consumer 的 state-model 输入合同满足；
- `strict_diagnostic_coverage_ready` 说明 source/timing 诊断证据覆盖率，不阻塞 state-only modeling gate；
- `workflow_input_ready` 是 HiCache validation 是否可以继续 prediction 的门禁；
- 不再使用 `profile_quality_ready` 这类同时混合 artifact、state input 和 diagnostic coverage 的字段作为 active 语义。

## 实验 Suite

suite config 用于在一套采集契约下展开多个 server/input 组合：

当前 `configs/` 维护两类 suite：cache-state 开发主链和 DAG analysis full-trace 采集。

| 配置 | 用途 |
| --- | --- |
| `profiling_hicache_state_common.json` | 普通生成的 common suite；用于采集诊断和 self prediction。 |
| `profiling_hicache_state_forced_capture.json` | 三个 manual input 的 immutable plan/bundle capture。 |
| `profiling_hicache_state_forced_replay.json` | 显式 bundle 驱动的 5×3 cross-config replay。 |
| `profiling_hicache_dag_analysis_forced_replay.json` | 显式 bundle 驱动的 5×3 full-DAG replay；用于 DAG analysis 一阶段，必须产出 torch / LD_PRELOAD / Python probe 三类 channel。 |

common suite 只用于 quality、采集诊断和 self prediction；`--prediction-scope cross` 必须使用 forced replay suite。
DAG analysis suite 不替代 HiCache state forced replay suite；它只为 full-DAG 构图、faithful replay sanity、anchor
coverage 和 operation visibility 提供输入。

```bash
scripts/profile.sh <suite-config.json> --list-experiments
scripts/profile.sh <suite-config.json> --experiments <id-a>,<id-b>
scripts/profile.sh <suite-config.json> --inputs <input-a>,<input-b>
scripts/profile.sh <suite-config.json> --servers <server-a>,<server-b>
scripts/profile.sh <forced-replay-suite.json> --forced-token-bundle <bundle.json>
```

full-DAG replay 运行示例：

```bash
scripts/build.sh sglang --hook-only

scripts/profile.sh \
  configs/experiments/dag_analysis/profiling_hicache_dag_analysis_forced_replay.json \
  --forced-token-bundle <capture-suite>/forced_token_bundle.json \
  --inputs manual_phased_fast,manual_pressure_prefetch,manual_deeper_pressure_prefetch
```

运行后可对单个 run 做通用 artifact audit：

```bash
scripts/run.sh sglang -- \
  env PYTHONPATH=scripts/internal python3 -m markov_internal.audit.profile_artifacts \
    --manifest <dag-suite>/<experiment>/profile_manifest.json
```

full-DAG run 的 audit 至少应满足：

```text
trace_channel_coverage.torch_trace_files > 0
trace_channel_coverage.ld_preload_trace_files > 0
trace_channel_coverage.python_probe_trace_files > 0
artifact_errors 不包含 trace_channel_missing
artifact_errors 不包含 sidecar_only_trace
```

suite 的设计语义：

- 顶层 `profiling` 固定一套采集契约；
- `matrix.servers[]` 定义 server 配置维度；
- `matrix.inputs[]` 定义 workload 维度；
- `experiments[]` 可以显式选择 server/input 组合；
- suite 展开后的 run-local config 必须保留 `metadata.suite_server_id` 和 `metadata.suite_input_id`；
  unified modeling workflow 的 HiCache validation 只读取这两个字段作为 config/input 身份，不从 `run_id` 反向猜测；
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
  -> trace_sim_probe.probes.hicache.callable
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
| `TRACE_SIM_PYTHON_PROBE_DEBUG=1` | probe debug 日志 |

HiCache Python probe target 由共享 catalog 维护，默认路径为 `configs/profiling/hicache_probe_targets.json`。
该文件本身就是 target 对象数组，不包额外顶层结构。
profile config 的 `profiling.python_probe` 只声明本次需要的 consumer，例如：

```json
{
  "name": "sglang.hicache",
  "consumers": [
    "hicache_state_model",
    "hicache_input_contract",
    "hicache_final_state_validator",
    "hicache_transition_validator"
  ],
  "flush_every": 256
}
```

runner 会读取 catalog，选择 `requested_consumers ∩ target.fact.consumers` 非空的 target，并把输出 target 中的
`fact.consumers` 收紧为本次交集。profile config 不能内嵌 target、class filter 或 state-trace 开关。

Python probe 启用后，probe module 加载、target 安装和 catalog 解析错误必须让本次 profiling 失败；不能只在 debug
日志中吞掉错误后继续运行。这样可以避免配置写错后得到“成功运行但没有关键 trace”的产物。

Python probe writer 使用 streaming Chrome trace 输出；正常退出时补齐 JSON 结尾。当前 HiCache 采集不维护额外 internal hook
子系统；需要的 source/timing/oracle 证据都必须显式落在 catalog callable target 上，并由 consumer 选择。

单个 target 只采集 `events` 中显式列出的 phase，不存在默认 `end` 采集。`events` 必须写成 phase 到 event name 的映射，
例如 `{"end": "..."}`、`{"start": "...", "end": "..."}` 或 `{"instant": "..."}`。zero-duration checkpoint 使用
`"instant"` key，由通用 callable wrapper 在被包装方法成功返回后发出一条 `phase=instant` 事件。

通用 callable source 由 `generic_callable` 提供；HiCache 特化 source 位于
`trace_sim_probe.probes.hicache` package：

| source | 作用 |
| --- | --- |
| `token_path:<source>[,<scope_source>]` | 输出 token dictionary；同一 scope/path 在 state-model fact 与 diagnostic evidence 各自去重域内首次包含完整 `token_ids` |
| `token_span:<source>` | 输出 `{path_id, begin, end, token_count, hash_algo}` |
| `request_token_path:<req>,<mode>[,<scope>]` | 从 SGLang `Req` 输出 request token dictionary；`mode=fill/committed/extend/prefetch/origin_output` |
| `request_token_span:<req>,<mode>` | 从 SGLang `Req` 输出 request token span |
| `request_token_count:<req>,<mode>` | 从 SGLang `Req` 输出 request token 数 |
| `hicache_cache_scope:<source>` | 输出 rank + cache object 作用域 |
| `hicache_seq:<source>` | 在 cache scope 内生成单调逻辑序号 |
| `hicache_state:self` | validation-only state snapshot，写成 `oracle_state/state_snapshot` fact |

旧的 `page_hashes:*` / `target_page_identity_page<page_size>` 不再是当前 HiCache state 主契约。
state backend 从 token dictionary/span 和 target page size 重建 page hash。

request path mode 必须对应当前 SGLang 调用边界：

| mode | 当前读取语义 |
| --- | --- |
| `fill` | `cache_unfinished_req()` 使用的 `Req.get_fill_ids()`；仅在当前 SGLang 对象没有该 API 时回退等价字段。 |
| `committed` | `origin_input_ids + output_ids` 按 `kv_committed_len` / `_cache_commit_len()` 截断。 |
| `extend` | `ScheduleBatch.prepare_for_extend()` 已形成 batch 后的 accepted fill path，优先使用 `Req.get_fill_ids()`。 |
| `prefetch` | `_prefetch_kvcache()` 的 `full_untruncated_fill_ids[:_compute_max_prefix_len(...)]` 候选 path；不能用 fill path 代替。 |
| `origin_output` | 完整 `origin_input_ids + output_ids`，只在明确需要完整 request path 时使用。 |

`RadixKey` 必须按其迭代和长度语义读取，不能直接绕过 `limit` / bigram view 读取底层 `.token_ids`。
配置 target 需要跟随当前 SGLang callable 签名；缺少 request 归属的 `InsertParams` / `EvictParams` 只能输出 optional
`request_id`，不能伪造归属。

## HiCache 事件分类

HiCache Python probe target 必须在共享 catalog 中显式写 `module` 和 target-level `fact`：

| 字段 | 语义 |
| --- | --- |
| `fact.class` | `workload_identity`、`source_actual`、`timing_observation`、`oracle_state` |
| `fact.role` | class 内的事实角色，供 consumer 二级分发 |
| `fact.consumers` | 可消费该事实的模型、input contract 或 validator 列表 |

catalog target 示例：

```json
{
  "id": "hiradix.insert_result_observed",
  "module": "sglang.srt.mem_cache.hiradix_cache",
  "target": "HiRadixCache.insert",
  "events": {
    "end": "hicache_insert_result_observed_end"
  },
  "fact": {
    "class": "source_actual",
    "role": "insert_result_observed",
    "consumers": [
      "hicache_transition_validator"
    ]
  },
  "fields": [
    {
      "name": "cache_scope",
      "source": "hicache_cache_scope:self"
    }
  ]
}
```

后端第一层分流只看 `fact.consumers`、`fact.class/fact.role`，以及该 role 当前合同允许的完成态 phase。`cache_extend_input`
使用 start-phase，其它当前 workload identity fact 使用 end-phase。其它事件即使出现在 trace 中，也只能作为 timing、
source actual、oracle 或 debug 证据。

| `fact.class` | 用途 |
| --- | --- |
| `workload_identity` | 跨配置 workload/request lifecycle 身份事实，可供 state model 和 input contract 消费 |
| `timing_observation` | source run 的异步 I/O 边界与 duration evidence，不能直接决定 target state |
| `source_actual` | source run 实际 control/operation 边界，不能作为 target answer |
| `oracle_state` | validation-only state snapshot / transition oracle |

当前 active consumer 只有 state model、input contract、final-state validator 和 transition validator。临时 debug 采集需要先
在 catalog 中成为明确 target，再声明给对应 validator consumer；不维护独立 debug consumer。

## HiCache 状态采集目标合同

当前正常 state model 只消费下列 fact：

| fact | 语义 |
| --- | --- |
| `workload_identity/cache_lookup_input` | `HiRadixCache.match_prefix` 的 cache lookup key；只描述输入 path，不描述 source lookup 结果。 |
| `workload_identity/cache_extend_input` | `ScheduleBatch.prepare_for_extend` start-phase 的 batch-level cache extend 输入；包含 request id、位置和 accepted fill path 数组。 |
| `workload_identity/cache_lifecycle_commit` | finished/unfinished lifecycle commit 边界；必须携带当前 committed/fill `token_dictionary`、`full_path_span` 和 `token_count`。 |
| `workload_identity/prefetch_candidate_anchor` | `Scheduler._prefetch_kvcache` 的 prefetch candidate path；target policy 由后端按 target config 重算。 |

`drain_storage_control_queues()` 不再作为 profiling runtime checkpoint 采集。它是 source scheduler round 边界，
跨配置不能稳定映射到 target request timeline；transition validator 只消费 `source_actual` /
`timing_observation` evidence 与 oracle snapshot label。

source evidence 可以与 state-model target 采自同一个 Python callable，但必须拆成独立 target：

| evidence role family | fact class | 设计边界 |
| --- | --- | --- |
| scheduler admission | `source_actual` | 只用于解释 target run admission 边界；state model 消费 batch-level `cache_extend_input`。 |
| insert / capacity / lock | `source_actual` | 只作为 transition validator 的 operation evidence，不更新 target state。 |
| prefetch / writeback | `source_actual` 或 `timing_observation` | 只作为 transition validator 的 operation evidence；target 行为仍由模型按 target config 重算。 |

当前 source/timing evidence 字段只保留 operation-level 关联信息：`cache_scope`、必要的 run-local `request_id` 和异步
`operation_id`。page/state label 来自 `oracle_state/state_snapshot`，不从 source/timing target 中扫描 page/hash 字段。

`sglang.hicache` probe 只安装 catalog 中声明的 callable target。当前不维护额外 internal hook 子系统；如果某个 source/timing
证据需要进入验证链路，必须先成为 catalog target 并声明对应 consumer。

注意：

- match-prefix path 不再以 `request_tokens` / `lookup_path` 混合 role 出现；
- `cache_lookup_input` 只在 request id 存在时发出；
- raw `request_id` 只用于单 run 内关联 request-scoped fact，不能作为跨配置 workload identity；
- 跨配置签名必须归一化到 token path / request fingerprint。

validation-only state snapshot 由 `hicache_final_state_validator` 或 `hicache_transition_validator` consumer 请求。它写成
`oracle_state/state_snapshot` fact，只能给 HiCache profile audit 和 modeling validation 路径使用。当前 snapshot payload
只保留 HiRadixCache node state、object/page identity 和 capacity/policy evidence，不再包含 controller queue、ongoing
operation 或重复的 derived state 摘要。

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
| `token_ids` | dictionary 首次出现必需 | 完整 token id 序列；path-bearing state-model fact 与 diagnostic evidence 分开去重，不能由 `source_actual` 补齐 state-model path |
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
profiling 结束后先看 suite result，再进入 workload report、generic artifact audit、HiCache profile audit 和 workflow input quality。

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

## 采集后审计与 workflow gate

profiling 完成后的通用 artifact audit 可以由 unified workflow preflight 自动执行；需要单独检查时使用包内 audit 模块：

```bash
PYTHONPATH=scripts/internal python3 -m markov_internal.audit.profile_artifacts \
  --manifest <run_dir>/profile_manifest.json \
  --output <run_dir>/profile_artifact_audit.json
```

通用 artifact audit 只检查：

- trace 文件是否存在；
- Python probe target 是否命中；
- required fields 是否缺失；
- Python probe 是否产生 exception event。

HiCache profile audit 不再有 standalone entrypoint；它在 unified modeling workflow 的 `hicache_state_inputs`
preflight 中执行。默认路径检查：

- state-model fact 是否误带 source-result 字段；
- state-model fact 是否具备 token dictionary/span、cache scope、seq_no；
- token span 是否都能找到 dictionary；
- `seq_no` 是否在 scope 内有序；
- `cache_lifecycle_commit` 是否携带可由 state-model fact 自身解析的 committed/fill path。

HiCache validation 路径按 selected validation 显式开启后才检查：

- workload 声明的 HiCache 机制是否实际出现；
- forced replay workload 是否使用合法 plan，且 actual `output_ids` 是否全部匹配 plan；
- replay bundle schema/hash/id 是否存在，同 input 下 bundle 是否唯一，bundle entry hash 是否等于实际 plan hash；
- run config 声明 forced capture/replay 时，workload report 必须存在且 mode 必须一致，不能缺失后退化成普通 generate；
- `cache_lifecycle_commit` 是否与 observed path 对照一致；
- source evidence 出现非空 prefetch intent/enqueue 时，`prefetch_candidate_anchor` 是否也有非空 candidate path；
- state trace 开启时是否采到 capacity snapshot。

workflow 每次都基于当前代码重新审计 manifest，不复用旧 audit JSON 作为 gate；profile discovery 要求当前 suite metadata
明确给出 config/input 身份，缺失时直接作为合同错误失败。HiCache validation preflight summary 使用三层 readiness：

| 字段 | 语义 |
| --- | --- |
| `workflow_input_ready` | workflow 是否可继续执行 final-state / transition。 |
| `state_model_input_ready` | 单 run 是否具备 state model fact、token dictionary/span 和 workload identity。 |
| `strict_diagnostic_coverage_ready` | source/timing 诊断覆盖率是否完整；只在 transition/diagnostic coverage 路径执行。 |

forced-token 一致性、oracle/capacity evidence 和 strict diagnostic coverage 是 validation 路径。关闭时不能只是不输出对应
JSON 字段，而是不能执行对应 trace 遍历、workload report 读取和诊断对象构造。两者必须分别报告，不能用 state gate 掩盖 coverage
缺口。

profiling 后的统一 HiCache validation 入口：

```bash
python3 scripts/internal/entrypoints/modeling_workflow.py \
  --profile-run-dir <forced_replay_suite_dir> \
  --output-dir <forced_replay_suite_dir>/modeling/modeling_workflow_hicache_state_manual_3inputs \
  --inputs manual_phased_fast,manual_pressure_prefetch,manual_deeper_pressure_prefetch \
  --validations hicache_final_state,hicache_transition \
  --prediction-scope self,cross \
  --emit-transition-catalog \
  --emit-transition-gates
```

`hicache_transition` 复用同一次 unified workflow 生成的 cache-state model run 和 validation artifact，不单独重跑 C++。
旧 `hicache_state_matrix_validation.py` 和 `hicache_workflow.py` 入口均已删除。

workflow 默认 console 输出是阶段级 start/done summary。TTY 中运行行可以动态刷新；非 TTY/log 中不逐 run 或逐 prediction
打印 `result ok ...`。默认输出目录的用户入口是：

```text
<workflow_output>/workflow_summary.json
<workflow_output>/preflight_summary.json
<workflow_output>/artifacts/model_runs_summary.json
<workflow_output>/artifacts/validations/hicache_final_state/summary.json
<workflow_output>/artifacts/validations/hicache_transition/summary.json
<workflow_output>/artifacts/{model_run_plan.json,preflight,validations}/
<workflow_output>/model_runs/<model_run_id>/
```

`workflow_summary.json`、`preflight_summary.json`、`artifacts/model_runs_summary.json` 和
`artifacts/validations/*/summary.json` 只保留阶段级计数、按 input/config/family 的分组摘要和关键 ready/exact 状态。
per-run audit、per-prediction validation row、transition exactness payload、catalog/gate 和日志只保留在 `artifacts/`
或 `model_runs/` 下。

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
