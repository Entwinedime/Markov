# 当前限制与后续组件

本文件只列仍然有效的限制，不保存已经解决的历史缺陷。

## 1. Direct 与完整 E2E

当前模型预测 HiCache 配置直接改变的 I/O/control。完整 source-to-target E2E 还会受到：

- CPU residual gap；
- prefill；
- decode；
- runtime scheduling/noise。

因此 Direct score 与完整 E2E score 必须分开。oracle 同骨架差分用于隔离 Direct cost，但不能证明完整 E2E 已建模。

## 2. CPU gap

trace 中存在无法由已知 execution event 解释的大 gap。可能来源包括 scheduler、Python runtime、OS、I/O wait 或漏采事件。

后续 gap model 必须：

- 只拥有 residual CPU intervals；
- 不吸收已物化的 Direct I/O/control；
- 区分真实 gap 与 profiling overhead；
- 使用 snapshot-free profile 作为输入基线；
- 以 component delta 单独评分。

## 3. Prefill/decode

当前 cross patch 保留 source 的 prefill/decode skeleton，因此 Direct model/oracle replay 差分中它们相同。完整 target 预测仍需要：

- target prefill work amount；
- cache hit/miss 对 prefill compute 的影响；
- decode token count 与 scheduling；
- phase 间 dependency/overlap。

这两部分不得作为 Direct cost correction。

## 4. Probe overhead

早期 snapshot probe 会遍历 cache state 并采集大对象，造成显著 CPU 开销。当前默认 probe 已轻量化，但旧 profile 中可能仍包含
该开销。

验收原则：

- 新 profile 默认不采 snapshot；
- 比较不同 profile 时先核对 probe contract；
- overhead 不作为真实 target runtime 特征；
- 必要时单独估计采集开销，不混入 gap 或 Direct cost。

## 5. Schedule-sensitive effects

prefetch readiness、loadback visibility、capacity release 和 formal-window tail background 可能依赖到达顺序。当前结构比较会把它们
单列，并要求 alternate evidence。

不能用以下方法处理：

- 按 cell 写 expected branch；
- 使用 target E2E 选择状态；
- 放大结构 tolerance；
- 把缺少的结构效果塞入 cost duration。

## 6. Async overlap 与 blocking

target observed control 是结果，不一定等于 foreground blocking。模型通过 resource lane、dependency、join 和 consumer 重新计算
blocking，而不是直接注入 target blocking duration。

oracle 可能显示 cost node 有响应，但 consumer start 被 source context 遮蔽。这种情况应报告 masking，不应直接判定结构断链。

## 7. Calibration 边界

当前 compact calibration 只保留：prefetch storage-read 曲线、H2D/D2H sustained page 曲线、H2S existing/new 曲线、
zero-payload prefetch 固定 control 和 load 逐页 control。queue pressure、completion history、payload/state correction、support taper
已经删除。

one-base builder 只额外估计四个 family runtime scale、H2S existing/new scale，以及 prefetch/load fixed control。换 base 时使用
同一估计方法，不合并多个 base，也不读取 target score。局部短尾仍可能超过 family 诊断线；这种情况必须报告，不能恢复
config/cell-specific correction。

P11 证明了一个更强的边界：C2 没有 D2H/H2S positive service observation，C4 没有任何
positive-payload service observation。当这两个配置被选作唯一 base 时，未见 family 的 runtime scale 在数学上
不可辨识。保持 calibration scale 1.0 是不泄漏 target 数据的 fallback，但不能保证数值达标。这需要
新的信息完备 base workload 或独立 runtime calibration，不能通过 target residual 拟合解决。

## 8. KTransformers

KTransformers 当前只验证 framework-neutral source DAG。它没有 HiCache module，因此不参与上述 state/cost/oracle 限制，也不进入
60-cell HiCache 矩阵。

## 9. 后续顺序

1. 为 single-base 新增 family-coverage/rank 充足的观测或独立 runtime calibration；
2. 保持 target-independent structure planner 不变，重新验证 Direct cost 60 cross；
3. 固定 Direct effect/cost ownership，建立 snapshot-free gap 数据与 residual interval 定义；
4. 分别建立 prefill、decode component；
5. 最后组合完整 E2E，并保留 component attribution。
