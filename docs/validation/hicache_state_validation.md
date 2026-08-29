# HiCache state/effect 验证

本文件说明 effect planner 的验证边界。Direct cost 和 isolated E2E 见
`docs/validation/hicache_direct_effect_validation.md`。

## 1. 目标

HiCache state/effect model 回答：给定 source 中与配置无关的 cache facts 和一个 target config，目标执行需要哪些
I/O/control effects？

它不预测 duration，也不读取 target E2E。

## 2. 输入

允许输入：

- request order 和 formal window；
- token dictionary/path span；
- cache lookup、extend、lifecycle commit；
- source timing observation；
- target page size、capacity、write policy、prefetch policy；
- lifecycle closure context。

不允许输入：

- target-observed duration；
- target final DAG；
- target E2E；
- cell-specific expected answer；
- snapshot 中的完整 cache tree。

## 3. 输出

每个 stable effect 包含：

- effect type 和 transfer direction；
- operation/page/byte count；
- source/target token range；
- resource scope/lane；
- consumer role；
- logical order 和 blocking relation；
- active/inactive state 与原因。

结构随后交给 cost model 和 DAG patcher。

## 4. Target shape oracle

target shape oracle 从 target score-only profile 中提取实际 operation structure，用来比较预测：

- stable effect identity；
- operation presence；
- transfer direction；
- operation/page/byte shape；
- consumer/blocking relation；
- resource lane order。

oracle 不向 predictor 提供参数。比较器的结果不能反向修改 effect plan。

## 5. 结构门禁

硬门禁：

- target oracle 可读且 score-only；
- predicted/actual effect identity 可一一对应；
- invariant shape exact；
- sensitive effect 有独立 alternate evidence；
- source attribution 无歧义；
- patch transaction 完整；
- topology exact；
- prefill/decode 标记为 excluded/deferred。

arrival schedule sensitive 或 formal-window tail background 需要单列，不通过扩大 tolerance 隐藏。

比较按字段而不是整行粗分 ownership。`prefetch_io_operation` 的 existence、pages/bytes 和 direction 仍属于
schedule-invariant 硬门禁；其 `consumer_role` / `blocking_relation` 由相邻的
`prefetch_visibility_dependency` 决定，因此随 cache-extend 到达时刻变化，在 gap/prefill/decode 尚未建模时列为
arrival-schedule-sensitive 诊断。诊断 mismatch 保留原数值，只不计入本阶段 Direct 结构硬门禁。

## 6. 当前稳定面板

正式 profiling 面板为五配置、三 workload。重构前 C5/C1/C3 三个 selected base 共 36 个 cross DAG 已达到：

- 36/36 usable；
- 36/36 structure ready；
- 36/36 cost ready；
- target profile 不参与参数估计。

这批旧结果是回归参考。当前代码变更后先跑关键 cell，再跑 12-cell，不直接复用旧 row。

## 7. Formal window

结构验证以 workload formal window 为边界。窗口外只允许：

- 窗口前 token dictionary context；
- 与窗口内 async operation identity 精确匹配的 ACK/release closure。

这些 context 不创建可执行 DAG node。formal-window tail background effect 必须显式标记，不能混入 foreground Direct score。

## 8. Source invariance

最终 60 cross 中，同一个 target config/workload 会由四个不同 source base 预测。应形成 15 个组并检查：

- stable effect keys 一致；
- operation family/count 一致；
- target byte/page shape 一致；
- resource semantics 一致；
- 差异仅来自 source carrier/context，不来自 target effect answer 漂移。

source invariance 是 effect planner 泛化的重要证据，不能用 target answer 对齐代码特例。

## 9. 失败分类

| 分类 | 含义 | 处理 |
| --- | --- | --- |
| input contract | token/path/lifecycle 不完整 | 修 profile/preflight |
| effect logic | target policy 推导错误 | 修 state/effect model |
| attribution | effect 无唯一 source carrier | 修 DAG index/attribution |
| schedule sensitive | 到达顺序影响状态 | 增加独立语义证据，不加 cell 分支 |
| patch/topology | mutation 破坏依赖 | 修 atomic patch |
| cost only | structure exact、duration 错 | 只进入 cost model |

## 10. 非目标

- 不以 target final state 直接替换预测；
- 不在 effect planner 拟合 duration；
- 不通过扩大 10%/15% gate 接受结构错误；
- 不重新引入 snapshot-heavy probe；
- 不把 KTransformers 纳入 HiCache state matrix。
