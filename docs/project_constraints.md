# 项目约束规范

维护方式：这是长期约束文档。更新时直接删改，不写流水账。

## Docs

`docs/` 顶级主线文档只维护四个文件：

- `profiling_development.md`
- `modeling_development.md`
- `work_progress.md`
- `project_constraints.md`

`docs/validation/` 只放纳入 git 追踪的专项验证记录。当前保留：

- `docs/validation/hicache_state_validation.md`：当前有效验证口径、最新结果和复现入口；
- `docs/validation/hicache_state_model_defects.md`：当前 state model 缺陷清单和处理顺序。

不再保留单独的 legacy validation 文档。旧结果如果还有参考价值，应压缩成 active 文档中的背景说明或
`work_progress.md` 时间线；不得作为当前验收依据。

文档不能依赖本地 `data/` 目录长期存在。真实 run 只能作为临时实验批次，长期证据必须抽取成配置、命令、commit、
关键指标和结论。

active 源码子目录不维护独立 README。模块说明、设计说明和使用说明必须收敛到主线文档或专项验证文档。

## 语言与注释

- 项目文档统一使用中文。
- 新增代码注释优先使用中文。
- 注释解释“为什么这样做”和“维护什么不变量”，不要只复述代码。
- 普通解释性 C/C++ 注释使用 `//`。
- 确定需要修改的问题使用 `// !`。
- 不确定假设或需要实验确认的问题使用 `// ?`。

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

- HiCache state 主线只允许 token/range invariant contract。
- 新采集目标必须显式写入 `fact_class`、`dag_input`、`state_model_input`。
- `fact_class=invariant_state && state_model_input=true` 才能进入 C++ HiCache state model。
- `source_actual`、`timing_observation`、`oracle_state`、`debug_quality` 不得更新 target state。
- `page_identity`、`target_page_identity`、`target_page_identity_page<page_size>` 不再是 state model 主输入。
- target page identity 必须由 token dictionary/span、`hash_algo`、`cache_scope` 和 target `page_size` 推导。
- token dictionary 可作为默认 state input；普通事件应引用 span，避免重复携带完整 token 列表。
- `cache_scope` 和 `seq_no` 是 HiCache invariant state fact 的必需路由字段。
- validation-only `state_snapshot` 必须保持 `state_model_input=false`。

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
- 正常 HiCache prediction 中 `non_invariant_fact_usage` 必须为空；只要非空，不能宣称 invariant-only prediction 通过。
- `self-config prediction` 仍必须显式给出 target page size、capacity、write policy、prefetch policy。
- `cross-config prediction` 只能用 target trace 做 oracle，不得偷读 target actual trace 作为模型事实源。
- 非执行类 state snapshot、oracle state、probe debug、质量审计事件不能作为默认性能 DAG 节点。

## 配置与数据

- `configs/experiments/` 按实验领域分组。
- HiCache state profiling 配置放在 `configs/experiments/hicache_state/`。
- smoke profiling 配置放在 `configs/experiments/smoke/`。
- `configs/modeling/` 按建模领域分组。
- HiCache state prediction 配置放在 `configs/modeling/hicache_state/`。
- faithful replay 配置放在 `configs/modeling/hicache/`。
- smoke modeling 配置放在 `configs/modeling/smoke/`。
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
