# 项目约束规范

维护方式：这是约束规范文档。更新时直接删改本文件内容，不做时间戳流水记录。

## Docs 约束

`docs/` 顶级主线文档只维护四个文件：

- `profiling_development.md`
- `modeling_development.md`
- `work_progress.md`
- `project_constraints.md`

其中：

- profiling 文档只记录 profiling 开发主线；
- modeling 文档只记录 modeling 开发主线；
- work progress 文档只做时间戳增量更新；
- constraints 文档记录项目长期约束，更新时删改。

`docs/validation/` 用于纳入 git 追踪的专项验证记录；当前维护 HiCache state validation 的三个文件：
`docs/validation/hicache_state_validation.md` 是 active 文档，只记录当前有效规则、约束、主线和后续新结果；
`docs/validation/hicache_state_model_defects.md` 是当前 state model 缺陷清单，只记录仍需要修复或验证的机制缺口；
`docs/validation/hicache_state_validation_legacy.md` 是历史只读结论文档，只保存旧批次和旧口径结果。
专项验证文档不能依赖本地 `data/` 目录长期存在；真实 run 只能作为
临时实验批次，长期证据必须抽取成配置、commit、命令、关键指标和结论。新增专项验证文档前必须先确认它不是
profiling / modeling / work progress / constraints 四个顶级主文档能够承载的内容。

不再在 `docs/` 下维护按实验、模块、历史实现拆出来的零散主线文档。

`docs/` 下不再使用 `tmp` 子目录。短期计划、临时方案和协作草稿如果需要保留，必须直接合并进上述四个顶级主文档或已批准的 `docs/validation/` 专项文档；不需要保留的临时材料应放在仓库外或系统临时目录，不能作为提交、交接或长期引用的依据。

active 源码子目录也不维护独立开发文档。模块说明、设计说明、使用说明必须合并到上述顶级主文档或已批准的专项验证文档中。

## 语言与注释约束

- 项目文档统一使用中文撰写。
- 代码注释统一使用中文撰写。
- 新写代码应添加充分注释，特别是状态机、DAG 修改、profiling hook、字段契约、错误处理和边界条件。
- 注释应解释“为什么这样做”和“该逻辑维护什么不变量”，不只复述代码表面行为。
- 对外部框架调用链、probe 采集点、子模块状态转移和非显然性能假设，应在代码附近写清楚。
- 普通解释性代码注释使用 `//`。
- 如果注释标记的是作者判断确定需要修改的问题，必须以 `// !` 开头。
- 如果注释标记的是作者不确定是否需要修改、需要实验或审查确认的假设点，必须以 `// ?` 开头。

## Profiling 约束

- Profiling 只采事实，不做建模推断。
- Probe 不决定 policy。
- Probe 不做 trace graph replay 或 target 行为生成。
- Probe 不生成 target 行为。
- Debug 字段不能成为默认模型输入。
- 缺关键事实时应明确暴露缺口，不能用默认值掩盖。
- 实验配置描述采集和运行，不嵌入 modeling 预测逻辑。
- 手动连续运行并归档多组 profiling 时，应优先使用 suite config：同一个 suite 固定一套 `profiling`
  采集配置，实验差异只能来自 `matrix.servers[]` / `matrix.inputs[]` 或显式 `experiments[]`
  指向的 server/input 组合。
- suite 内的 `matrix.servers[]`、`matrix.inputs[]` 和 `experiments[]` 不允许覆盖或 unset
  `profiling`；需要改变采集渠道、probe target、torch profiler 或 LD_PRELOAD 行为时，必须新建
  另一个 suite config。
- suite experiment 必须有稳定 `id`，手动运行前应先用 `scripts/profile.sh <config> --list-experiments`
  查看展开结果，再用 `--experiment` 或 `--experiments` 选择本次要跑的实验集合。归档时保留
  `suite_config.json`、`suite_selection.json` 和 `suite_result.json` 的摘要信息。
- HiCache state mainline-one profiling 只保留
  `configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json` 作为
  归档入口；不得恢复 S1A/S1B × manual/bench 的四个单实验配置。
- page-size what-if 的 `target_page_identity_page<page_size>` 是有限目标 page size 矩阵的过渡字段；
  新增任意 page size 的长期方案应优先设计 size-independent token path digest / range hash，而不是无限增加
  预声明 target page size 字段。
- `python_probe` 只由 `profiling.python_probe` 控制。
- `ld_preload` 只由 `profiling.ld_preload` 配置控制是否注入和输出到哪里；不读取旧顶层 `hook` / `native` 兼容入口；具体 wrapper 由 C++ 硬编码实现决定。
- `profiling.channels` 只接受 `torch`、`python_probe`、`ld_preload`，不接受 `python`、`hook`、`native` 等旧短名。
- 普通 LD_PRELOAD 只能启用 hook so 中已实现 wrapper 的 native 符号；不能声称支持任意未知符号动态拦截。
- HiCache diagnostic 实验中 `--hicache-ratio` 可以按实验目标调整，但必须大于 `1.0`；调整时必须在配置说明或验证记录中写明原因。容量压力优先通过 workload 或显式 capacity 配置触发，不能用小于等于 `1.0` 的 ratio 构造实验。
- 真实 SGLang / KTransformers profiling 必须通过 `scripts/profile.sh` 外层容器入口启动。`scripts/internal/profile_runner.py` 是容器内执行器，不能在宿主机上直接用于真实 server profiling。
- Profiling runner 可以在 state_trace 开启时，从同一次调用的 validation-only start/end snapshot materialize 最小 operation-level
  facts，例如 insert 内消失的 radix page identity；完整 state snapshot 仍必须保持 `model_input=false`，不能整体进入 C++ state model。

## Modeling 约束

- Modeling 后端必须使用 C++ TraceGraph；Python 侧只保留 runner 和 trace merger 编排脚本。
- 子模块必须通过 C++ `SimulationModule` 基类和继承实现。
- 所有 what-if 都必须规约为 C++ `SimulationModule`。
- 不保留独立的普通图变换机制。
- `faithful_replay` 关闭的是子模块加载和 DAG patch，不是关闭事件消费。
- `faithful_replay` 必须消费完整真实 merged trace；HiCache、CPUInfer、Python probe 等真实执行事件不能因为对应子模块关闭而被过滤。
- Base DAG 是所有真实执行事件的重放结果；子模块只能在 base DAG 上做 what-if 修改。
- 子模块直接修改 C++ DAG。
- 子模块修改 DAG 应通过统一 mutation API 记录修改。
- 默认 E2E 预测来自 DAG 拓扑仿真，不来自子模块 latency 求和。
- 默认 DAG 拓扑仿真不使用原始绝对时间戳兜底，faithful replay 必须能暴露缺失依赖边。
- `replay` 是保留术语，只能表示 `mode=faithful_replay`：不加载任何子模块、不执行 DAG patch，只用完整真实 merged trace
  构建并拓扑重放 base trace graph。
- HiCache state 不再使用带 source 行为答案的重放口径；文档、配置和输出命名都应使用
  `self-config prediction` 或 `cross-config prediction`。
- HiCache state model 在任何场景下都只能消费不变量事实和显式 target config。真实 target trace、state snapshot、
  source movement、oracle-only transient、debug 字段和 policy 结果只能用于 validation / debug，不能作为模型输入。
- HiCache state 显式 `write_policy=observed`、`prefetch_policy=observed` 或
  `storage_prefetch_policy=observed` 都是非法配置，不能作为旧配置兼容入口。
- HiCache state validation 必须暴露模型实际消费的非法非不变量事实；正常情况下
  `non_invariant_fact_usage` 必须为空。只要它非空，`invariant_coverage_ready` 必须为 `false`，不能仅凭
  `final_state_match=true` 宣称 prediction 通过。`skipped_non_invariant_events` 只是诊断计数，不表示模型消费了这些事件。
- `self-config prediction` 指 base profiling facts 与 target config 来自同一个场景，但仍必须走显式 target page size /
  capacity / write policy / prefetch policy；不得省略 target config，也不得回落到 source 行为答案。
- `cross-config prediction` 指 base profiling facts 加另一个显式 target config；validation oracle 只能用于对比 predicted state。
- 子模块不能修改原始 profiling trace；三类 trace 的合并只能由 `scripts/trace/trace_merger.py` 生成新的 merged trace。
- 子模块不能把 debug 字段混入默认预测输出。
- 非执行类 state snapshot、oracle state、probe 内部 debug 和质量审计事件不能作为默认性能 DAG 节点；必须放在独立 debug/state trace，或显式标记为 `model_input=false` 并只由 validation/debug 路径消费。

## 配置与数据约束

- `configs/experiments/` 下按实验领域分组；HiCache state validation 配置放在 `configs/experiments/hicache_state/`，smoke 配置放在 `configs/experiments/smoke/`，普通 SGLang profiling 配置放在 `configs/experiments/sglang/`。
- `configs/modeling/` 下按建模领域分组；HiCache state 配置放在 `configs/modeling/hicache_state/`，HiCache faithful replay 配置放在 `configs/modeling/hicache/`，smoke 配置放在 `configs/modeling/smoke/`。
- 新增配置时必须放进已有语义分组；只有出现新的稳定领域时才新增子目录。
- modeling 配置引用目标 experiment config 时必须使用仓库内相对路径，且路径应指向上述分组后的真实位置。
- HiCache state validation 主线一的两个场景必须是全新的联合配置：两个场景彼此不同，并且都不能等同于任何此前已经跑过的
  HiCache state profiling 配置组合。判定时按 page size、L1/L2 capacity、write policy、prefetch policy、prefetch timeout、
  `--hicache-ratio` 等核心项组成联合签名；不能只按配置文件名、场景名或 `C0-C8` 编号集合判断。
- 主线一候选配置开跑前必须做历史配置比对：仓库中仍存在的历史配置用脚本化扫描确认 `old_matches=0`，已清理的历史 run、
  临时验证 run 和失败后重跑前的草案配置则依据专项验证文档保留的摘要、config metadata 或专用 fixture 中固化的历史签名黑名单约束。
  比对结论必须写入 config metadata 或 `docs/validation/hicache_state_validation.md`。
- `data/profile_runs/**`、`data/modeling_runs/**` 和 `data/tmp/**` 都是可再生运行产物，不纳入 git 追踪，也不能作为长期事实来源或文档阅读前提。
- 需要长期保留的真实 run 结论必须写入对应主线或专项验证文档；文档应使用稳定验证编号和内联摘要，记录 config、commit、复现命令、workload 结果、质量门槛和 validation 结论。
- 新写验证文档不能要求读者打开某个 `data/` 目录才能理解结论；临时 run id 只用于说明实验批次，不是长期证据载体。
- 提交前应清理不再需要的大体积 profiling/modeling/debug 产物，避免本地状态依赖未追踪数据。

## Debug 约束

- 每个子模块可以有对应 debug 类。
- Debug 类必须显式关联功能子模块类型。
- 真实仿真默认只调用功能子模块。
- 最高层参数决定是否进入 debug 模式。
- Debug 输出分为子模块状态 debug 和 DAG 修改 debug。
- Debug 输出必须与默认主输出分离。

## 输出约束

默认主输出只包含：

```json
{
  "predicted_e2e_ns": 0
}
```

默认输出文件为：

- `prediction.json`

其他输出必须由显式参数打开：

- `emit_dag_chrome_trace`
- `emit_module_summary`
- `emit_validation`
- `debug`

Chrome trace 格式 DAG 输出参数固定为：

```text
config: outputs.emit_dag_chrome_trace
CLI: --emit-dag-chrome-trace
```

建模 CLI 主入口为：

```text
python3 scripts/internal/model_runner.py --config <config>
```

runner 可以读取 profile manifest 或显式 trace 路径；默认不输出 summary、validation 或 debug。

## Deprecated 约束

- active tree 中不保留可被误用的 deprecated 实现。
- 不恢复旧实验配置。
- 不恢复旧实验结果。
- 不恢复旧 profiling probe target。
- 新实现应按当前 profiling / modeling 主线重新设计。

## Git 与清理约束

- 不回滚用户已经删除或移动的文件。
- 不恢复旧配置和旧结果。
- 不把 pycache、临时结果、debug 大文件重新纳入主线。
- 提交信息必须规范、具体，优先使用 Conventional Commits 形式：`type(scope): summary`。
- 提交摘要应说明本次提交的主要行为和对象，例如 `feat(hicache): add state validation pipeline`；不得使用 `update`、`fix stuff`、`wip` 等无法追踪意图的泛化描述。
- 如果提交涉及大范围行为变化、验证状态或已知未闭环项，应在 commit body 中补充关键上下文和验证情况。
- 文档结构变化后，`docs/` 顶级主线文档应保持四文件约束；专项验证记录只能放在 `docs/validation/`，不再新增 `docs/tmp/` 或其他临时文档子目录。
- C/C++ 格式化配置只在仓库根目录 `.clang-format` 维护；active 源码子目录不维护局部 `.clang-format`。
- C/C++ 改动提交前必须能运行 `git ls-files '*.c' '*.cc' '*.cpp' '*.h' '*.hpp' | xargs clang-format --dry-run --Werror`。
- 复杂 C++ 子模块必须采用面向对象结构，至少拆分 fact parser、state、policy/decision、summary 和 DAG mutation 边界。
- 不允许把复杂状态机、fact parser、summary、policy 和 DAG mutation 全部堆在单个匿名 namespace 中。
- active 源码子目录不维护嵌套 `.git/`、局部 `.gitignore` 或独立 `README.md`。
- `src/profiling/ld_preload`、`src/modeling/trace_graph` 等从旧仓库迁入的目录只能保留实现文件；目录级使用说明必须收敛到 profiling / modeling 主文档。
