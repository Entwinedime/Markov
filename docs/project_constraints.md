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

## 3. Component 边界

当前正式预测组件是 `hicache_direct`。以下内容独立管理：

- CPU residual gap；
- prefill；
- decode；
- profiling probe overhead。

Direct 验收不得依靠调整这些 component，完整 E2E 也不得被误报为 Direct 误差。

## 4. HiCache 结构与 cost

- effect plan 先于 cost plan；
- cost model 不决定结构；
- patch 必须是一次原子 mutation；
- concurrency/queue/overlap 通过 resource lane 和 dependency 表达；
- 不按 config、workload 或 cell 写业务分支；
- 不用 oracle cost 反向修改预测结构。

oracle-cost replay 只验证预测结构和 cost sensitivity，不获得泛化成绩。

## 5. Calibration

- physical/runtime calibration 不运行 target workload；
- selected base 只提供一个 base 的三 workload observation；
- 换 base 时参数估计方法保持相同；
- C1/C3 外推只验证方法，不用于改公式；
- 60 cross target score 不能选择特征、系数、阈值或 correction。

## 6. Profiling

- 5 config × 3 workload 是当前 SGLang 评分实验资产，不是正式 prediction API；
- 不新增 micro workload 阶段；
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

## 8. 代码与产物

- active product surface 最终不超过 50,000 行；
- 不通过压缩多语句、移出统计目录或删除必要注释达标；
- 删除死代码、重复检查、开发期测试入口和中间 proof output；
- Debug 功能由 C++ build option 或 Python diagnostics 参数隔离；
- 默认只保留 compact summary 和复现所需输入；
- 项目只维护当前实现，不用内部身份字段、文件摘要或冻结副本管理迭代。

## 9. 验证纪律

- 每个代码块修改后先运行少量语义关键 cell；
- cost 简化前后使用同一组关键 cell；
- clean Release/validation build 必须通过；
- final 60 cross 只在公式固定后运行；
- 每个完成或回退块追加写入当前重构日志；
- 旧 artifact 只能作为参考，不能代替当前工作树运行结果。

## 10. 数据保护

不能删除后续 60-cell 所需的：

- 5×3 profile traces 与 manifests；
- forced-token capture/replay assets；
- physical/runtime calibration；
- selected-base observations；
- target score-only structure/cost assets；
- 当前稳定对照结果。

临时 dry-run、失败装配目录、重复 model summary 和开发期 replay output 可以在确认路径后删除。
