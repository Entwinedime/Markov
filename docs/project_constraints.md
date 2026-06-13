# 项目约束规范

维护方式：这是长期约束文档。更新时直接删改，不写流水账。

## Docs

`docs/` 顶级主线文档只维护四个文件：

- `profiling_development.md`
- `modeling_development.md`
- `work_progress.md`
- `project_constraints.md`

其中 `profiling_development.md` 和 `modeling_development.md` 只记录设计、接口、组件职责、输入输出契约和长期边界；
不得携带具体实验结果、历史阶段分析、run 路径或数值 diff。实验进展写入 `work_progress.md`，当前有效验证口径和结果写入
`docs/validation/`。

`docs/validation/` 只放纳入 git 追踪的专项验证记录。当前保留：

- `docs/validation/hicache_state_validation.md`：当前有效验证口径、最新结果、剩余风险和复现入口。

不再保留单独的 legacy validation 文档、缺陷清单文档或 `docs/tmp_hicache*.md` 临时方案。旧结果如果还有参考价值，
应压缩成 active 文档中的背景说明或 `work_progress.md` 时间线；不得作为当前验收依据。

不再维护任何 fixture。具体要求：

- 不保留 `tests/`、`tests/fixtures/`、`run_*_fixtures.py` 或依赖 fixture trace 的 smoke modeling config；
- 不新增 fixture-backed validation gate；
- 不把长期验收建立在本地 synthetic fixture 文件上；
- 需要长期保留的验证证据必须抽取成配置、命令、审计脚本、关键指标和结论，或指向真实 profile/modeling run 的可复现入口。

文档不能依赖本地 `data/` 目录长期存在。真实 run 只能作为临时实验批次，长期证据必须抽取成配置、命令、commit、
关键指标和结论。

active 源码子目录不维护独立 README。模块说明、设计说明和使用说明必须收敛到主线文档或专项验证文档。

## 语言与注释

- 项目文档统一使用中文。
- 新增代码注释统一使用中文，保留必要的项目内英文术语、类型名、字段名和配置名。
- 解释性 C/C++ 注释统一使用 Doxygen 风格，优先使用 `/** ... */` 与 `@brief`；文件、类型、函数、状态机边界、字段契约、错误处理和不变量说明都应按这个风格书写。
- 同一组声明需要分区时使用 Doxygen 分组标记，例如 `@name`、`@{` 和 `@}`，不要用普通分隔线注释代替。
- Python 解释性注释优先使用中文模块 docstring、类 docstring 和函数 docstring；只在局部非显然分支、不变量、错误处理或外部协议边界处使用 `#` 行注释。
- shell 脚本使用 shebang 后的中文文件说明块和函数前中文 `#` 注释；保留必要的 `shellcheck` 指令、环境变量名、命令名和英文错误输出。
- 注释解释“为什么这样做”和“维护什么不变量”，不要只复述代码。
- 普通 `//` 只保留给 namespace 结尾标记。
- 确定需要修改的问题必须显式说明影响面和期望修复方向：C/C++ 使用 Doxygen `@todo` 块；Python 和 shell 使用中文
  `TODO:` 注释或 docstring 段落，不再使用 `// !` 标记。
- 不确定假设或需要实验确认的问题必须显式说明假设边界、风险和验证证据：C/C++ 使用 Doxygen `@warning` 或
  `@note` 块；Python 和 shell 使用中文 `NOTE:` / `WARNING:` 注释或 docstring 段落，不再使用 `// ?` 标记。

## Profiling

- Profiling 只采事实，不做建模推断。
- Probe 不决定 target policy，不生成 target 行为。
- Debug / oracle 字段不能成为默认模型输入。
- 缺关键事实时必须暴露缺口，不能用默认值掩盖。
- 实验配置描述采集和运行，不嵌入 modeling prediction 逻辑。
- 真实 SGLang / KTransformers profiling 必须通过 `scripts/profile.sh` 外层容器入口启动。
- `scripts/internal/profile_runner.py` 是容器内执行器，不能在宿主机上直接用于真实 server profiling。
- `profiling.channels` 只接受 `torch`、`python_probe`、`ld_preload`。
- `python_probe` 只由 `profiling.python_probe` 控制。
- `ld_preload` 只由 `profiling.ld_preload` 控制注入和输出；具体 wrapper 由 C++ 硬编码。
- suite config 中 `profiling` 必须共享；`matrix.servers[]`、`matrix.inputs[]`、`experiments[]` 不得覆盖或 unset `profiling`。
- 改采集类别、probe target、torch profiler 或 LD_PRELOAD 行为时，新建 suite。
- HiCache diagnostic 中 `--hicache-ratio` 必须大于 `1.0`；容量压力优先来自 workload 或显式 capacity config。

### HiCache Profiling

- HiCache state 主线只允许 target-level atomic fact contract。
- 新采集目标必须显式写入 `fact.class`、`fact.role`、`fact.model_input`、`fact.dag_input` 和
  `fact.granularity=atomic`。
- `model_input=true && fact_class=invariant_state && fact_granularity=atomic` 才能进入 C++ HiCache state model；router
  只接受已知 atomic invariant role。
- 非 `invariant_state` 的 HiCache target 必须显式写 `model_input=false`。
- `source_actual`、`timing_observation`、`oracle_state`、`debug_quality` 不得更新 target state。
- HiCache mainline 可以保留 source/evidence target，但正常 state model input 必须是显式枚举且通过 cross audit 的子集；
  当前 33-target suite 的正常输入 role 是 `request_bound_match_anchor`、`request_lifecycle_anchor`、
  `request_admission`、`prefetch_decision`、`prefetch_check_point`。
- raw `request_id` 只是单次运行内的 correlation id；cross audit 不得把它当成跨配置 invariant fact，必须先用
  path-bearing atomic facts 归一化 request scope。
- source matched result、admission return、actual victim、actual movement、actual async completion 等 source 已发生结果不得作为
  `invariant_state` 事件字段混入；需要保留时必须拆成并行 `source_actual` / `timing_observation` / `oracle_state` 事件。
- cache-stage concrete match-prefix path、source `insert_path`、request lifecycle generated/committed suffix、
  source `capacity_request`、source `lock_scope_delta` 和 source maintenance polling/check-kind 序列不得作为正常 cross
  model input；需要使用时必须改成 target-derived 机制或新的 target-independent invariant。
- `page_identity`、`target_page_identity`、`target_page_identity_page<page_size>` 不再是 state model 主输入。
- target page identity 必须由 token dictionary/span、`hash_algo`、`cache_scope` 和 target `page_size` 推导。
- 正常 state input 可以引用 token dictionary/span，但能否进入模型由 `fact_class` 和 atomic `event_role` 决定；普通事件应引用
  span，避免重复携带完整 token 列表。
- `cache_scope` 和 `seq_no` 是 HiCache invariant state fact 的必需路由字段。
- validation-only `state_snapshot` 必须保持 `model_input=false` 且 `fact_class=oracle_state`。
- 重跑真实 HiCache profile 前必须先通过本地契约检查：JSON config 校验和 invariant target source-result 字段审计；
  不再使用 fixture 作为 profile gate。
- `scripts/internal` 下 HiCache 专项工具只保留 active、只读、文档化的审计入口。当前允许保留：
  `hicache_state_cross_input_audit.py` 和 `hicache_state_provenance.py`。
- 不保留临时 residual/report spike、front-door workload 对照、async elision、timeline oracle replay alignment 或任何会生成
  synthetic `model_input=true` 事件的 HiCache internal 脚本。

## Modeling

- Modeling 后端必须使用 C++ TraceGraph；Python 侧只保留 runner、trace merger 和 validation 编排。
- 所有 what-if 都必须规约为 C++ `SimulationModule`。
- 子模块不能修改原始 profiling trace。
- 子模块修改 DAG 必须通过统一 mutation API 记录。
- 默认 E2E prediction 来自 DAG 拓扑仿真，不来自子模块 latency 求和。
- `faithful_replay` 关闭的是子模块加载和 DAG patch，不是关闭事件消费。
- `faithful_replay` 必须消费完整真实执行 trace；不能因为某个领域子模块关闭就过滤该领域真实执行事件。
- `replay` 是保留术语，只能表示 `mode=faithful_replay`。
- 启用 HiCacheModule 的 state 建模只能称为 `self-config prediction` 或 `cross-config prediction`。
- HiCache state model 只能消费不变量事实和显式 target config。
- target actual trace、state snapshot、source movement、oracle transient 和 debug 字段只能用于 validation/debug。
- 显式 `write_policy=observed`、`prefetch_policy=observed`、`storage_prefetch_policy=observed` 都是非法配置。
- HiCache best-effort prefetch threshold、capacity limit 和 host release budget 必须来自显式 target config 或 SGLang
  源码语义：默认 threshold 是 `max(prefetch_threshold=256, page_size)` tokens 经 target page size 投影后的页数，capacity limit 是
  `floor(0.8 * (host_pool_pages - device_pool_pages))`，host alloc 失败后的 cleanup budget 是本次 page-aligned
  prefetch request，rate-limit 判断保持 `occupied >= capacity_limit`；不允许使用 L2 一半、deficit、最终 L2 差值、
  超容量拟合预算或把 0 capacity limit 解释成无限制。
- 正常 HiCache prediction 中 `non_invariant_fact_usage` 必须为空；只要非空，不能宣称 invariant-only prediction 通过。
- `self-config prediction` 仍必须显式给出 target page size、capacity、write policy、prefetch policy。
- `cross-config prediction` 只能用 target trace 做 oracle，不得偷读 target actual trace 作为模型事实源。
- 非执行类 state snapshot、oracle state、probe debug、质量审计事件不能作为默认性能 DAG 节点。
- 当前 HiCache mainline S1A/S1B profiling target count 是 33，但 profile quality 通过不代表全部 target 都是正常 state
  input；cross-config state-rule diagnosis 必须先通过 hard `model_input_contract_ready=true`，确认 atomic invariant role
  在 request-normalized canonical fact multiset 上逐项跨配置一致。sequence mismatch 是诊断信号，不是输入事实 hard
  blocker。
- 如果 `model_input_contract_ready=false`，state mismatch 先归类为输入契约、projection 或 async/control-flow boundary
  问题，不能直接当作 backend model/rule bug 修。
- 只有 profile quality 明确失败、进入 DLLM/disaggregation/streaming/abort/preemption 等新 scope，或 SGLang upstream hook
  语义边界变化时，才允许重新讨论新增 HiCache 采集 target。
- HiCache backend 重构不保留 page-level state machine 兼容性；token-level radix tree 是 source of truth，page set 只能是
  target page projection。
- HiCache backend 必须拆分 router/schema、token store、target pager、token radix tree、state index、policy、async state、
  summary/validation 边界；不能继续把复杂状态机堆在 `hicache_model.cpp` 单体里。

## 配置与数据

- `configs/experiments/` 按实验领域分组。
- HiCache state profiling 配置放在 `configs/experiments/hicache_state/`。
- smoke profiling 配置放在 `configs/experiments/smoke/`。
- `configs/modeling/` 按建模领域分组。
- HiCache state prediction 配置放在 `configs/modeling/hicache_state/`。
- faithful replay 配置放在 `configs/modeling/hicache/`。
- 不再维护依赖 fixture trace 的 smoke modeling 配置。
- 新增配置必须放进已有语义分组；只有出现新的稳定领域时才新增子目录。
- modeling 配置引用目标 experiment config 时使用仓库内相对路径。
- `data/profile_runs/**`、`data/modeling_runs/**`、`data/traces/**` 是可再生运行产物，不纳入 git 追踪，不作为长期事实来源。
- 提交前清理不再需要的大体积 profiling/modeling/debug 产物。

## 输出

默认主输出只包含：

```json
{
  "predicted_e2e_ns": 0
}
```

默认输出文件：

- `prediction.json`

其他输出必须显式打开：

- `emit_dag_chrome_trace`
- `emit_module_summary`
- `emit_validation`
- `debug`

建模 CLI 主入口：

```bash
python3 scripts/internal/model_runner.py --config <config>
```

## Deprecated

- active tree 中不保留可被误用的 deprecated 实现。
- 不恢复 `tests/` 目录或任何 fixture suite。
- 不恢复旧实验配置。
- 不恢复旧实验结果。
- 不恢复旧 profiling probe target。
- 不恢复 page-identity state backend。
- 不恢复 observed/default policy 兼容入口。
- 新实现按当前 profiling / modeling 主线重新设计。

## Git 与清理

- 不回滚用户已经删除或移动的文件。
- 不把 pycache、临时结果、debug 大文件纳入主线。
- 提交信息必须规范、具体，优先使用 Conventional Commits，例如 `docs(hicache): sync token invariant state docs`。
- 提交涉及大范围行为变化、验证状态或未闭环项时，commit body 需要写关键上下文和验证情况。
- C/C++ 改动提交前运行 clang-format dry-run。
- 复杂 C++ 子模块必须拆分 fact parser、state、policy/decision、summary 和 DAG mutation 边界。
- 不允许把复杂状态机、fact parser、summary、policy 和 DAG mutation 全部堆在单个匿名 namespace 中。
