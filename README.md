# Trace Simulation

本仓库维护一条 trace-driven 建模链路：先从真实 SGLang / KTransformers 运行中采集事实，再由 C++ TraceGraph
构建 DAG 或领域状态模型，最后用于 faithful replay、state prediction 和后续 what-if 性能预测。

当前 active 工作重点是 SGLang HiCache state model。旧的 page-identity/observed 行为答案口径已经停止作为主线；
当前采集和后端都按 token/range invariant contract 推进。

## 当前结构

```text
.
├── src/profiling/python_probe/          # sitecustomize + import hook + Python callable probes
│   └── trace_sim_probe/probes/
│       ├── generic_callable.py          # 通用 callable 插桩
│       └── sglang_hicache_callable.py   # HiCache token/span、scope、seq、oracle snapshot source
├── src/profiling/ld_preload/            # C++ LD_PRELOAD hook 框架和硬编码 wrapper
├── src/modeling/trace_graph/            # C++ TraceGraph、DAG 仿真和 SimulationModule 后端
│   ├── include/trace_graph/modules/hicache/
│   └── src/modules/hicache/             # HiCache fact parser、radix tree、state model、summary
├── scripts/profile.sh                   # 宿主机 profiling 入口，进入框架容器运行
├── scripts/model.sh                     # 宿主机 modeling 入口，进入干净 modeling 容器运行
├── scripts/run.sh                       # 打开 framework runtime 或 modeling 容器
├── scripts/build.sh                     # 构建 framework runtime/hook 或 modeling image
├── scripts/internal/profile_runner.py   # 容器内 profiling 执行器
├── scripts/internal/profile_quality.py  # profiling 质量审计
├── scripts/internal/model_runner.py     # 容器内 modeling 执行器
├── scripts/trace/trace_merger.py        # torch / ld_preload / python_probe trace 合并
├── scripts/bench/hicache_phased_workload.py
├── configs/experiments/hicache_state/   # HiCache state profiling suite
├── configs/modeling/hicache_state/      # HiCache state prediction config
├── configs/modeling/hicache/            # faithful replay config
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
| `docs/validation/hicache_state_validation.md` | 当前 HiCache state validation 口径、最新 S1A token backend 结果和复现命令。 |
| `docs/project_constraints.md` | 项目长期约束。 |
| `docs/work_progress.md` | 时间戳流水记录；旧条目只代表当时状态。 |

## Submodules

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
  'cmake -S . -B build/modeling -G Ninja && cmake --build build/modeling --target trace_graph -j2'
```

构建 framework runtime 和对应 hook：

```bash
scripts/build.sh sglang
scripts/build.sh ktransformers
```

真实 profiling 不应直接使用 host build 的 hook so。LD_PRELOAD hook 需要在对应框架容器内构建，保证 ABI、工具链和运行依赖匹配。

## 常用检查

```bash
find configs -name '*.json' -print0 | xargs -0 -n1 jq empty
git diff --check
```

C/C++ 或 modeling runner 改动还需要：

```bash
scripts/run.sh modeling -- bash -lc \
  'python3 -m py_compile scripts/internal/model_runner.py scripts/internal/profile_quality.py'
scripts/run.sh modeling -- bash -lc \
  "git ls-files '*.c' '*.cc' '*.cpp' '*.h' '*.hpp' | xargs clang-format --dry-run --Werror"
```

## Profiling

真实 SGLang / KTransformers profiling 通过宿主机入口启动：

```bash
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json --list-experiments
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json --experiment s1a_manual
```

`scripts/profile.sh` 负责选择 docker compose service、挂载仓库、设置 Ascend 环境，并在容器内调用
`scripts/internal/profile_runner.py`。宿主机上不要直接用 `profile_runner.py` 启动真实 server profiling。

当前 HiCache state suite 只启用 `python_probe`。它采集：

- target-level `fact` 描述的 atomic `invariant_state` 状态事实；
- `timing_observation` / `source_actual` 的异步 IO 或 source 行为观测；
- `oracle_state` 的 validation-only state snapshot。

需要 base DAG faithful replay 或 cache patch 时，应另建完整执行 trace suite，同时启用 torch / LD_PRELOAD / Python
真实执行事件。HiCache state-only suite 不能替代性能 DAG 采集。

profiling 质量审计：

```bash
python3 scripts/internal/profile_quality.py \
  --manifest <run_dir>/profile_manifest.json \
  --output <run_dir>/profile_quality.json
```

## Modeling

faithful replay：

```bash
scripts/model.sh \
  --config configs/modeling/hicache/modeling_hicache_from_manifest.json \
  --profile-manifest <run_dir>/profile_manifest.json \
  --output-dir <run_dir>/modeling/faithful_replay \
  --mode faithful_replay \
  --emit-validation \
  --emit-module-summary
```

S1A HiCache state self-config prediction：

```bash
scripts/model.sh \
  --config configs/modeling/hicache_state/modeling_hicache_state_mainline_one_prediction_s1a.json \
  --profile-manifest <run_dir>/profile_manifest.json \
  --output-dir <run_dir>/modeling/token_backend_s1a \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

HiCacheModule 当前是 state-only backend：它维护 cache state 和 transition trace，不修改 DAG，
`prediction.json.predicted_e2e_ns` 只来自 base DAG 拓扑仿真，不是 HiCache state 准确性的验收指标。

## HiCache 当前进展

截至 2026-06-12，当前主线状态是：

- profiling config 使用 target-level atomic `fact` 契约，不再把 `fact_class` / `event_role` / state gate 写成普通字段；
- 主 HiCache profile config 当前有 33 个 atomic target，其中 normal state model input 是 7 个 target / 5 个 role：
  `request_bound_match_anchor`、`request_lifecycle_anchor`、`request_admission`、`prefetch_decision`、
  `prefetch_check_point`；
- `request_tokens`、`lookup_path` 和 `request_cache_lifecycle` 这类混合 role 已从主配置删除；match-prefix concrete path、
  lifecycle committed/fill path 和 runtime detail 都拆成 `source_actual` evidence；
- C++ HiCache backend 的主门禁是 `model_input=true && fact_class=invariant_state && fact_granularity=atomic`，
  router 只接受已知 atomic invariant role 并做 required-field 检查；
- target page 由后端按 token path 和 target `page_size` 重建，不再消费 `target_page_identity_page64/128`；
- `scripts/internal/hicache_state_cross_input_audit.py` 现在只比较 atomic invariant facts；raw `request_id` 是 run-local
  correlation id，不作为跨配置事实签名，hard gate 检查 count 和 request-normalized canonical fact multiset，sequence mismatch
  只作为诊断输出。

当前契约摘要：

| 项 | 当前口径 |
| --- | --- |
| configured targets | `33` |
| normal state input targets | `7` |
| normal state input roles | `5` |
| source/evidence targets | `24 source_actual` + `2 timing_observation`，均为 `model_input=false` |
| model input gate | `model_input=true && fact_class=invariant_state && fact_granularity=atomic` |
| cross audit hard gate | `model_input_contract_ready` |
| latest atomic S1A/S1B profile | `data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix` |
| latest cross audit result | 双向 `model_input_contract_ready=true`，blocking roles 为空 |
| retained old cross audits | 只作为旧混合输入契约的历史证据 |

当前 atomic profile 的 normal model input 已通过双向 cross audit。下一步应在该 run 上重跑 self/cross modeling validation，
再判断 remaining final diff 是 async boundary、target-derived projection 缺口，还是 C++ state rule bug。

## 数据约束

`data/profile_runs/**`、`data/modeling_runs/**`、`data/traces/**` 都是可再生运行产物，不作为长期事实来源。
需要保留的结论必须抽取到 `docs/validation/` 或主线文档中。
