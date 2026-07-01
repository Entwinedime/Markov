# Trace Simulation

本仓库维护一条 trace-driven 建模链路：先从真实 SGLang / KTransformers 运行中采集事实，再由 C++ TraceGraph
构建 DAG 或领域状态模型，最后用于 faithful replay、state prediction 和后续 what-if 性能预测。

当前 active 工作重点是 SGLang HiCache state model。旧的 page-identity/observed 行为答案口径已经停止作为主线；
当前采集和后端都按 token/range fact contract 推进。

## 当前结构

```text
.
├── src/profiling/python_probe/          # sitecustomize + import hook + Python callable probes
│   └── trace_sim_probe/probes/
│       ├── generic_callable.py          # 通用 callable 插桩
│       └── hicache/                     # HiCache source、token、snapshot、internal wrapper package
├── src/profiling/ld_preload/            # C++ LD_PRELOAD hook 框架和硬编码 wrapper
├── src/modeling/trace_graph/            # C++ TraceGraph、DAG 仿真和 SimulationModule 后端
│   ├── include/markov/trace_graph/modules/hicache/
│   └── src/modules/hicache/             # HiCache fact parser、radix tree、state model、summary
├── scripts/profile.sh                   # 宿主机 profiling 入口，进入框架容器运行
├── scripts/model.sh                     # 宿主机 modeling 入口，进入干净 modeling 容器运行
├── scripts/run.sh                       # 打开 framework runtime 或 modeling 容器
├── scripts/build.sh                     # 构建 framework runtime/hook 或 modeling image
├── scripts/internal/entrypoints/        # 容器内 CLI：profile/model/audit/HiCache validation
├── scripts/internal/markov_internal/    # 内部 Python 包：profiling、audit、contracts、modeling、HiCache validation
├── scripts/trace/trace_merger.py        # torch / ld_preload / python_probe trace 合并
├── scripts/bench/hicache_phased_workload.py
├── configs/experiments/hicache_state/   # common / forced capture / forced replay
├── docs/                                # 主线文档和专项验证记录
├── third_party/sglang/                  # SGLang fork submodule
├── third_party/ktransformers/           # KTransformers fork submodule
└── data/                                # 可再生 profiling/modeling 产物，不纳入长期证据
```

## 文档入口

| 文档 | 内容 |
| --- | --- |
| `docs/profiling_development.md` | profiling 架构、runner、suite、Python probe 和 HiCache 采集契约。 |
| `docs/modeling_development.md` | C++ TraceGraph、model runner、mode、HiCache state backend 和输出格式。 |
| `docs/validation/hicache_state_validation.md` | 当前 HiCache state validation 口径、forced-token bundle workflow、保留基线和复现命令。 |
| `docs/validation/hicache_state_model_limitations.md` | 当前仍存在的中长期模型限制和收敛方向。 |
| `docs/project_constraints.md` | 项目长期约束。 |
| `docs/work_progress.md` | 时间戳流水记录；旧条目只代表当时状态。 |

## 子模块

```bash
git submodule update --init --recursive
```

框架源码作为可编辑 submodule 保留：

- `third_party/sglang/`
- `third_party/ktransformers/`

容器 runtime image 会把 submodule 源码复制进镜像并安装。框架源码变更后需要重建对应 runtime layer。

## Docker 环境

项目维护三个 Docker 环境：

- `sglang-profile`：SGLang 真实 profiling / runtime 环境，包含 Ascend/CANN 和 hook build 上下文；
- `ktransformers-profile`：KTransformers 真实 profiling / runtime 环境，包含 Ascend/CANN 和 hook build 上下文；
- `modeling`：干净 Ubuntu 24.04 C++23 环境，只用于 C++ TraceGraph 构建、clang-format/clang-tidy 和 modeling run/check。

构建 modeling 环境并检查 TraceGraph：

```bash
scripts/build.sh modeling
scripts/run.sh modeling -- bash -lc \
  'cmake -S src/modeling/trace_graph -B build/modeling/trace_graph-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DTRACE_GRAPH_DEBUG=OFF && cmake --build build/modeling/trace_graph-release --target trace_graph -j2'
```

构建 framework runtime 和对应 hook：

```bash
scripts/build.sh sglang
scripts/build.sh ktransformers
```

真实 profiling 不应直接使用 host build 的 hook so。LD_PRELOAD hook 需要在对应框架容器内构建，保证 ABI、工具链和运行依赖匹配。

## 常用检查

```bash
python3 -m ruff format --check .
find configs -name '*.json' -print0 | xargs -0 -n1 jq empty
git diff --check
```

C/C++ 或 modeling runner 改动还需要：

```bash
scripts/run.sh modeling -- bash -lc \
  'python3 -m py_compile $(find scripts/internal/entrypoints scripts/internal/markov_internal -name "*.py" -print)'
scripts/run.sh modeling -- bash -lc \
  "find src/modeling/trace_graph src/profiling/ld_preload -type f \\( -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \\) -print0 | xargs -0 clang-format --dry-run --Werror"
```

## 采集

真实 SGLang / KTransformers profiling 通过宿主机入口启动：

```bash
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_common.json --list-experiments
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_common.json \
  --inputs manual_phased_fast
```

`scripts/profile.sh` 负责选择 docker compose service、挂载仓库、设置 Ascend 环境，并在容器内调用
`scripts/internal/entrypoints/profile.py`。宿主机上不要直接调用容器内 entrypoint 启动真实 server profiling。

当前 HiCache state validation suite 只启用 `python_probe`。它采集：

- 声明给 `hicache_state_model` 的 `workload_identity` 状态输入事实，包括 cache lookup、cache extend、cache lifecycle commit
  和 prefetch candidate；
- `timing_observation` / `source_actual` 的异步 IO 或 source 行为观测；
- `oracle_state` 的 validation-only state snapshot。

需要 base DAG faithful replay 或 cache patch 时，应另建完整执行 trace suite，同时启用 torch / LD_PRELOAD / Python
真实执行事件。HiCache state-only suite 不能替代性能 DAG 采集。

profiling 后通用 artifact audit：

```bash
python3 scripts/internal/entrypoints/profile_audit.py \
  --manifest <run_dir>/profile_manifest.json \
  --output <run_dir>/profile_artifact_audit.json
```

HiCache workflow readiness audit：

```bash
python3 scripts/internal/entrypoints/hicache_profile_audit.py \
  --manifest <run_dir>/profile_manifest.json \
  --output <run_dir>/hicache_profile_audit.json
```

跨配置验证使用 forced-token capture/replay suite，保证同一 input 的 generated token timeline 一致：

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

capture suite 在自身目录生成 `forced_token_bundle.json` 和 `forced_token_plans/`。forced replay 必须显式传 bundle；
runner 不读取仓库固定 plan，也不自动选择最近一次 capture。

## 建模

当前不维护静态 modeling config。`hicache_workflow.py` 根据 replay suite 中的 target server config 动态生成
`<workflow_output>/artifacts/runner_configs/target_<config_id>.json` 并调用 `scripts/model.sh`；每个 prediction 输出目录下的
`cpp_model_config.json` 是 C++ TraceGraph backend narrow config：

```bash
python3 scripts/internal/entrypoints/hicache_workflow.py \
  --profile-run-dir <profile_suite_dir> \
  --output-dir <profile_suite_dir>/modeling/hicache_state_workflow \
  --stages quality,final-state \
  --prediction-scope self
```

HiCacheModule 当前是 state-only backend：它维护 cache state 和 transition trace，不修改 DAG，
`prediction.json.predicted_e2e_ns` 只来自 base DAG 拓扑仿真，不是 HiCache state 准确性的验收指标。

## HiCache 当前进展

截至 2026-07-01，当前主线状态是：

- Python probe target catalog 统一维护在 `configs/profiling/hicache_probe_targets.json`，当前共 `54` 个 target；
- `fact` 只保留 `class`、`role`、`consumers` 三类语义字段；采集入口按 consumer 选择 target，不维护旧 registry / envelope；
- `hicache_state_model` 只消费 `5` 个 workload identity target，覆盖 `4` 个 role；其余 target 只用于输入合同、
  transition validation 或 final-state oracle；
- `hicache_profile_quality` 不再作为采集 consumer；quality 审计按当前 run 实际请求的 input/final/transition consumer 判断 readiness；
- `oracle_state/state_snapshot` 只保留 `23` 个 `HiRadixCache.*` target，不再让 `HiCacheController.*`、`PrefillAdder.*`
  或 `Scheduler.*` snapshot 定义 HiCache cache-tree final state；
- C++ 使用 `HiCacheTokenDirectory` 和 role-specific resolver，不再用 `request_id -> longest path` 或 admission path 回退；
- forced-token capture bundle、显式 replay bundle 依赖、preflight、workflow input quality gate 和 `hicache_workflow.py` 已落地；
- `scripts/internal/entrypoints/` 只放容器内 CLI，复用逻辑位于 `scripts/internal/markov_internal/`；旧平铺脚本和
  `scripts/internal/deprecated/` 人工对照已删除，不保留兼容入口。

当前详细结果、已关闭诊断问题、已知限制和复现命令以 `docs/validation/hicache_state_validation.md` 与
`docs/validation/hicache_state_model_limitations.md` 为准。

## 数据约束

`data/profile_runs/**`、`data/modeling_runs/**`、`data/traces/**` 都是可再生运行产物，不作为长期事实来源。
需要保留的结论必须抽取到 `docs/validation/` 或主线文档中。
