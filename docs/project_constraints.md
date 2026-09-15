# 项目约束

本文件列出当前实现和实验不可违反的边界。使用方式见 `docs/profiling_development.md` 与
`docs/modeling_development.md`，当前状态见 `docs/work_progress.md`。

## 1. 公共入口

- profiling：`scripts/profile.sh`
- modeling：`scripts/model.sh`
- 一次性容器排查：`scripts/run.sh`

业务流程不能要求用户串联内部 bundle builder、score runner 或多套模型入口。

## 2. 数据边界

- profile manifest 是 profiling 与 modeling 的唯一交接点；
- source DAG 只来自 source profile；
- target config 可以影响 effect plan；
- calibration 与 selected base 可以影响 cost parameter；
- target observation 只能用于 score-only structure/cost/oracle；
- target E2E、target cell ID 和 workload ID 不能进入模型参数。

正式 predictor 与 observed-target evaluator 是两条物理隔离的路径。`predict-hicache` 不打开 target profile/score root，
也不导入 target oracle；`evaluate-hicache` 必须先确认选中的 predictor 全部完成，再读取 target score。
已有预测使用 `--prediction-dir` 独立评分，不为评分启动补采、构模或重复预测。评分结果不能回写模型、改变
readiness 或触发自动 refit。
预测入口不接收矩阵评分、跨base模型映射或oracle参数。评估默认只评分；显式 `--oracle-cost-replay` 才在评分后做成本替换诊断，
需要已有预测的完整操作详情，缺失时不自动重跑补产物。诊断输出放在独立评分目录，不覆盖预测或混入正式精度/采集账本。
诊断与普通评分共用操作级service/control计时规则；不另读旧oracle成本清单。跨trace对应使用各自的逻辑输入身份，不能比较局部节点编号。

## 3. Component 边界

当前正式预测组件是 HiCache I/O/control（内部 scope ID 为 `hicache_direct`）、`prefill` 和 `decode`。以下内容仍独立管理：

- CPU residual gap；
- profiling probe overhead。

HiCache I/O/control 与 phase 必须分项验收，任一 component 的系数不得用于吸收另一 component 或 residual gap 的误差。完整应用 E2E 也不得
被误报为某一个 component 的误差。

已完成阶段以排除 residual gap 和未归属成本后的三组件组合 scope 为主指标（总体和逐 base WAPE ≤ 3%、p90 ≤ 5%，争取 WAPE ≤ 1%）。
分项继续完整报告既有误差与 gate；无法由机制解释的分项偏差不强行拟合。主目标通过不等于所有分项 gate 通过，
evaluator 的 `MODEL_LIMITATION` 必须保留其原有含义。

下一阶段主目标是完整 HTTP 正式窗口 E2E：预测值与 target 实测比较，不与 target DAG 模拟值混淆。
先验证同成本图变换和 CPU 等待依赖，再处理剩余成本误差；不得清除 gap/未归属成本后仍称完整预测。
当前误差、诊断证据与后续顺序见 `docs/work_progress.md`，历史分项成绩不追溯改写。

统一分解为：

```text
predicted DAG = source-normalized DAG
              + delta_hicache_direct
              + delta_prefill
              + delta_decode
              + delta_gap       # deferred
```

component 由 mutation 的语义 ownership 声明。核心层只要求 owner 明确，不按 config、cell、事件名或固定时间窗白名单
硬编码边界。

## 4. HiCache 结构与 cost

- effect plan 先于 cost plan；
- phase work plan 先于 phase cost plan；
- cost model 不决定结构；
- patch 必须是一次原子 mutation；
- concurrency/queue/overlap 通过 resource lane 和 dependency 表达；
- I/O 实际执行量与最终缓存可见量分别保存；0 页可见不能抹掉已经执行的 service；
- 异步状态推进与 DAG duration 使用同一个 service cost 接口；timeout deadline 是配置派生的等待门，不是另一套 I/O 成本；
- 不按 config、workload 或 cell 写业务分支；
- 不用 oracle cost 反向修改预测结构。

oracle-cost replay 只诊断预测 operation/effect 的 cost binding 和 sensitivity，不证明完整 target 时序结构，也不获得泛化成绩。

## 5. Calibration

- 平台 physical calibration 不运行任何 target workload，并显式声明 `physical_capture.page_token_sizes`；该物理域不能从 target 列表反推；
- 固定校准只有一个逻辑 workload，在物理域的最小/最大 page 端点各采集相同请求；允许原样重复，不能按 target 缺口换 workload；
- 同一模型、TP、I/O backend 和资源环境可共享固定校准；换平台环境必须重采；
- selected base 的 profiles 只提供 source DAG、source 工作量和该 base 的 Prefill/Decode 参数，不能修改共享 HiCache I/O/control 参数；
- target 的增删、重排、命名、profile 与 score 均不得改变校准输入、特征、系数或阈值；
- 60-cross 仅在预测全部结束后评分。精度不足报告 `MODEL_LIMITATION`，不触发 refit 或新候选。

固定校准必须实际覆盖 Prefetch、Load、D2H、H2S existing/new 和相应 control。重复仅用于中位数与工作一致性检查；失败、重试和无效 trace
全部计入累计预算。校准数据不足时停止并指出缺失语义，不用其他 base 或 target 真值作为隐藏 fallback。

## 6. Profiling

- 5 config × 3 workload 是当前 SGLang 评分实验资产，不是正式 prediction API；
- 不新增 micro workload 阶段；
- 固定校准的生成输入放在当次 data 目录；它是参数采集资产，不是单独评分阶段，也不能扩成逐 target 实测；
- forced replay 比较真实 request order 与 token arrays；
- Python probe 默认 snapshot-free；
- formal window 是语义边界，不按 cell 硬编码；
- profiling cells 严格串行，避免 NPU/host/storage 状态跨 cell 漂移。

## 7. Framework

- SGLang 与 KTransformers 是同级目标框架；
- 两者共享 profile manifest、source DAG 和 simulation；
- HiCache 只适用于 SGLang；
- KTransformers submodule、installer、image、compose、hook 和 dispatch 必须保留；
- 共享核心不能为取得通用依赖而反向依赖 KTransformers 源码树。

NodeScale 是框架无关的可选 DAG duration 变换，必须保留且默认关闭；它不属于 HiCache，也不能替代未来的
prefill/decode 语义模型。

## 8. 代码与产物

- active product surface 最终不超过 50,000 行；
- 不通过压缩多语句、移出统计目录或删除必要注释达标；
- 删除死代码、重复检查、开发期测试入口和中间 proof output；
- Debug 功能由 C++ build option 或 Python diagnostics 参数隔离；
- 默认只保留 compact summary 和复现所需输入；
- 项目只维护当前实现，不用内部身份字段、文件摘要或冻结副本管理迭代。

当前命令每次执行当前代码，不以旧 artifact 的 schema、digest、runner hash 或 `reused` 状态证明结果有效。

## 9. 验证纪律

- 每个代码块修改后先运行少量语义关键 cell；
- cost 简化前后使用同一组关键 cell；
- clean Release/validation build 必须通过；
- final 60 cross 只在公式固定后运行；
- 总量误差与不抵消的 I/O 分项误差同时验收，不能用不同类别的正负误差抵消过 gate；
- 结构 exact 必须覆盖操作数、执行/完成页、batch、existing/new 与必要依赖；调度敏感差异单列但不计 exact；
- 大型重构必须新建一份短期 plan 与 append-only log；收口时把稳定结论合并回主文档并删除临时副本；
- 旧 artifact 只能作为参考，不能代替当前工作树运行结果。

5×3、单 base 12-cross 和五 base 60-cross 是数据驱动的 evaluation 面板，不是 predictor 的产品合同。业务核心不得固定
C1–C5、C5 base、三个 workload、12 或 60 这些实验身份和计数。

## 10. 数据保护

不能删除后续 60-cell 所需的：

- 5×3 profile traces 与 manifests；
- forced-token capture/replay assets；
- physical/runtime/phase calibration 与其 compact observation summary；
- selected-base observations；
- target score-only structure/cost assets；
- 当前稳定对照结果。

临时 dry-run、失败装配目录、重复 model summary 和开发期 replay output 可以在确认路径后删除。

当前路径、重建关系和可删除范围统一见 `docs/work_progress.md` 的数据资产章节；不再依赖 retention/SHA manifest。
