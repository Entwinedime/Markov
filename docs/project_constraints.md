# 项目约束规范

维护方式：这是约束规范文档。更新时直接删改本文件内容，不做时间戳流水记录。

## Docs 约束

`docs/` 目录只维护四个文件：

- `profiling_development.md`
- `modeling_development.md`
- `work_progress.md`
- `project_constraints.md`

其中：

- profiling 文档只记录 profiling 开发主线；
- modeling 文档只记录 modeling 开发主线；
- work progress 文档只做时间戳增量更新；
- constraints 文档记录项目长期约束，更新时删改。

不再在 `docs/` 下维护按实验、模块、历史实现拆出来的零散文档。

active 源码子目录也不维护独立开发文档。模块说明、设计说明、使用说明必须合并到上述四个主文档中。

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
- Probe 不做 observed replay。
- Probe 不生成 target 行为。
- Debug 字段不能成为默认模型输入。
- 缺关键事实时应明确暴露缺口，不能用默认值掩盖。
- 实验配置描述采集和运行，不嵌入 modeling 预测逻辑。
- `python_probe` 只由 `profiling.python_probe` 控制。
- `ld_preload` 只由 `profiling.ld_preload` 配置控制是否注入和输出到哪里；不读取旧顶层 `hook` / `native` 兼容入口；具体 wrapper 由 C++ 硬编码实现决定。
- `profiling.channels` 只接受 `torch`、`python_probe`、`ld_preload`，不接受 `python`、`hook`、`native` 等旧短名。
- 普通 LD_PRELOAD 只能启用 hook so 中已实现 wrapper 的 native 符号；不能声称支持任意未知符号动态拦截。
- HiCache diagnostic 实验中 `--hicache-ratio` 不能随意修改；默认固定为当前基线值 `2.0`。容量压力优先通过 workload 触发，必要时只能使用已确认安全的 `--hicache-size`。
- 真实 SGLang / KTransformers profiling 必须通过 `scripts/profile.sh` 外层容器入口启动。`scripts/internal/profile_runner.py` 是容器内执行器，不能在宿主机上直接用于真实 server profiling。

## Modeling 约束

- Modeling 后端必须使用 C++ TraceGraph；Python 侧只保留 runner 和 trace merger 编排脚本。
- 子模块必须通过 C++ `SimulationModule` 基类和继承实现。
- 所有 what-if 都必须规约为 C++ `SimulationModule`。
- 不保留独立的普通图变换机制。
- 子模块直接修改 C++ DAG。
- 子模块修改 DAG 应通过统一 mutation API 记录修改。
- 默认 E2E 预测来自 DAG 拓扑仿真，不来自子模块 latency 求和。
- 默认 DAG 拓扑仿真不使用原始绝对时间戳兜底，faithful replay 必须能暴露缺失依赖边。
- 子模块不能修改原始 profiling trace；三类 trace 的合并只能由 `scripts/trace/trace_merger.py` 生成新的 merged trace。
- 子模块不能把 debug 字段混入默认预测输出。

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
- 文档结构变化后，`docs/` 应保持四文件约束。
- C/C++ 格式化配置只在仓库根目录 `.clang-format` 维护；active 源码子目录不维护局部 `.clang-format`。
- active 源码子目录不维护嵌套 `.git/`、局部 `.gitignore` 或独立 `README.md`。
- `src/profiling/ld_preload`、`src/modeling/trace_graph` 等从旧仓库迁入的目录只能保留实现文件；目录级使用说明必须收敛到 profiling / modeling 主文档。
