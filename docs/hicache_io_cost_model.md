# HiCache 数值模型

本文说明当前唯一的 HiCache I/O 与控制开销、以及 Prefill/Decode cost model。执行流程见
`docs/modeling_development.md`，实测结果见 `docs/validation/hicache_validation.md`。

## 1. 模型回答什么

给定一个 base profile 和一个只含 HiCache 配置的 target，C++ 先由 source facts 与 target policy 产生 I/O 机会和需求，
再用同一份 service cost 推进完成、timeout、可见性与资源顺序，最后生成 Prefill/Decode 工作量和 DAG patch：

```text
source DAG + target config
  -> target effect/demand plan
  -> HiCache I/O/control cost + completion/resource schedule
  -> executed/visible work plan
  -> Prefill/Decode cost
  -> atomic DAG patch
  -> gap-excluded simulation
```

cost model 不根据 target 标签选择节点或工作量，也不从 target 残差反推结构；但 service duration 是 deadline、完成批次和发布页数的
真实因果输入，因此会影响时间相关的可见性与依赖。target profile、target E2E 和 target 分项 cost 只在预测全部完成后用于评分。

当前组件为：

- HiCache I/O/control（内部 scope ID 为 `hicache_direct`）：Prefetch、Load、device-to-host（D2H）、host-to-storage（H2S）及其固有 control；
- `prefill` 和 `decode`：由 target cache reuse 改变的计算工作；
- residual CPU gap：未建模，也不进入本模型的参数或验收分母。

## 2. 三类数据

| 数据 | 提供什么 | 不提供什么 |
| --- | --- | --- |
| 平台物理校准 | KV 字节几何、DMA/存储基础曲线、storage batch 上限、resource lane | framework runtime scale、phase cost、target 标签 |
| 固定小型校准 | framework 中四类 I/O 的真实 service 响应和 control；同一 workload 的端点重复 | target workload、target 缺口、target 分数 |
| 一个 base 的 profiles | source DAG、真实请求工作、该 base 的 Prefill/Decode 参数和 source-side I/O holdout | target cost correction |

同一模型、TP、I/O backend、内存布局和资源环境可以共享一份固定校准。换平台环境必须重采；换 base 只重建 phase 部分，不能借此改变
共享 HiCache I/O/control 参数。

## 3. 固定校准

固定校准是一个 `fixed_calibration` 逻辑 workload，不是新的精度评测矩阵。生成器只读取 base 的合法 token 输入和平台物理域：

1. 取平台声明的最小和最大 page 端点；
2. 构造同一组 spill、recover、rewrite 请求；
3. 在两个端点运行完全相同的请求；
4. 每个端点允许原样重复，重复只取中位数并检查实际工作是否一致。

当前物理域为 page 32/64/128，因此运行 page 32 和 128，page 64 由两端插值。当前一次 workload 为 14 个请求、35,854 tokens；
两个端点各保留 2 个成功 profile。target 列表的增删、重排和命名不会改变校准定义。

允许“多次补采”仅指重复这份固定输入。缺少某个 service family、端点或 control 时停止并报告数据限制；不生成另一份按 target 定制的
workload。

## 4. 变量与参数

### 4.1 预测时已知的变量

这些变量来自 target 状态回放，而不是回归标签：

- `page_size`、每 rank 的 `page_bytes`；
- operation、page、byte 数量；
- Prefetch 的 storage service batch 数；
- H2S 每个 batch 的 existing/new page 数；
- Prefill 的 new/context tokens 和 attention token pairs；
- Decode 的 context tokens、iteration 数和 effective pages；
- target policy 决定是否经过一次 Prefetch state check。

### 4.2 需要估计的参数

| 参数 | 来源 | 估计方法 |
| --- | --- | --- |
| Prefetch/Load/D2H runtime scale | 固定校准 | 每个端点、每次 profile 的 observed/physical 总时间比，再取重复中位数 |
| H2S existing runtime scale | 固定校准中的纯 existing batch | 同上 |
| H2S new setup 与 bandwidth | 固定校准中的短/长纯 new operation | 每次 profile 两点直线求解，再取端点中位数 |
| Prefetch commit/state-check control | 固定校准 | operation 样本中位数，再对端点取中位数 |
| Load admission control | 固定校准 | 同上 |
| Prefill/Decode 参数 | base profiles + 折叠后的固定校准点 | 实测 token 曲线或简单非负线性模型 |

不存在 config ID、workload ID、cell ID、target residual、逐格倍率、growth correction 或 target page 枚举参数。

## 5. HiCache I/O service 公式

以下时间单位为微秒，`B` 为 byte 数，`P` 为 page 数，`G` 为每 token 每 rank 的 KV bytes。

### 5.1 Prefetch

```text
page_bytes = target_page_size * G
physical = service_calls * setup_per_operation
         + P * setup_per_page
         + B * 1e6 / physical_bandwidth
predicted = physical * runtime_scale(page_bytes)
```

`service_calls` 来自预测的 storage batches。物理 setup/bandwidth 来自平台校准；runtime scale 来自固定 workload 两端的实测响应。

### 5.2 Load 与 D2H

```text
physical = B * 1e6 / physical_bandwidth(page_bytes)
         + operations * setup_per_operation(page_bytes)
predicted = physical * runtime_scale(page_bytes)
```

Load 和 D2H 分别有自己的物理 bandwidth 曲线和 runtime-scale 曲线，不共享 target 修正项。

### 5.3 H2S existing

已有 storage key 的读取/覆盖响应与单次 batch 深度有关：

```text
predicted_existing = sum_over_batches(
    existing_pages * page_bytes * 1e6
    / physical_existing_bandwidth(page_bytes, batch_pages)
) * existing_runtime_scale(page_bytes)
```

### 5.4 H2S new

创建新 storage operation 有一次 setup，再传输该 batch 的 bytes：

```text
predicted_new = sum_over_new_batches(
    setup_per_new_operation(page_bytes)
    + new_pages * page_bytes * 1e6 / new_bandwidth(page_bytes)
)
```

setup 和 bandwidth 直接由固定 workload 每个端点的短、长 new-write 两点得到。这里没有旧的 `growth_us_per_mib`，也没有按 target
残差再乘 scale。

端点间以 page bytes 的对数坐标插值；超出已声明物理域时使用最近端点并在 coverage 中显式暴露，而不是外推新的经验曲线。

## 6. HiCache control、timeout 与资源

- Prefetch 始终支付一次 terminal commit；`wait_complete` 和 `timeout` 另支付一次 state check，`best_effort` 不支付；
- Load 支付一次 admission 固定成本；
- D2H/H2S 当前没有独立 owner control，固定为 0；
- 零载荷 Prefetch 只落 control，不伪造 service bytes；
- page/operation 计数仍由结构模型决定，control 系数不吸收 service 或 gap。

I/O 并发用 resource lane 和 DAG dependency 表达，不把各 operation wall time直接相加：storage read/write、H2D、D2H 按平台声明决定
是共享 lane 还是每 rank lane。状态回放和最终 DAG 都调用同一个 service 估计；不存在另一套 planning bandwidth。

Prefetch 另外区分三个时间概念：

- 实际执行的 service：决定设备成本和资源占用，即使最终 0 页可见也保留；
- 发布的完整 batch：决定哪些页能进入 host cache；
- 前台等待：`wait_complete` 依赖完整 service，`best_effort` 不等待，未完成的 `timeout` 只依赖从 enqueue 到 deadline 的策略门。

因此 timeout 后未完成的 I/O 可以继续占用后台资源，但不能把完整 service 时长强加给当前请求。策略门没有拟合系数，持续时间直接来自 target
配置的 timeout 规则。

## 7. Prefill/Decode 模型

Phase 与 HiCache I/O/control 分开估计，但保存在同一个模型 JSON 中。

### 7.1 Prefill

```text
common kernel      = measured curve(new_tokens)
collective         = measured curve(new_tokens)
prefix attention   = a + b * new_tokens
                       + c * new_tokens * (context_tokens + new_tokens / 2)
```

曲线在相同 new-token 点先对样本取中位数。测量噪声造成后一个锚点更低时，只向前做单调夹取，不增加自由参数；锚点间线性插值。
`a/b/c` 用非负最小二乘求解，允许不需要的项自然为 0。

### 7.2 Decode

```text
effective_pages = ceil(context_tokens / min(target_page_size, base_kernel_page_tokens))
paged attention per iteration = a + b * context_tokens + c * effective_pages
collective per iteration      = observed median
```

target 的 decode iteration 数沿用 source 请求语义；非 paged kernel cost 和 CPU submit 保留 source 模板。CPU submit 单独诊断，不允许它吸收
HiCache I/O/control 或 residual gap。

固定校准重复在 phase 中先折叠为每端点一个逻辑实验，避免重复次数改变权重。不同 base 的三份 profiles 产生不同 phase 参数；target profile
不参与这些估计。

## 8. “训练”和预测分别做什么

这里的“训练”不是通用机器学习：它是一次确定性的参数估计。

```text
构模：physical + fixed calibration + base profiles
    -> 中位数、两点直线、两条实测曲线、两个小型非负线性模型

预测：source DAG + target config + 已建模型
    -> target 工作量 -> 公式代入 -> DAG patch -> simulation
```

相同输入重复构模必须得到相同参数。重复列出同一个 manifest 会先去重，不能改变权重。`model_build_summary.json` 记录每类参数的公式、
单位、manifest、有效样本、重复数、端点、source holdout 误差，以及物理测量的 backend/NUMA/CPU/计时范围。

模型还保存固定校准实际观测到的 page-byte 与每类 service-call byte 范围。预测超出范围时仍按同一物理公式计算，但明确报告
`outside_observed_domain`；旧模型未保存调用大小范围时报告 `unverified`。该状态只说明证据范围，不触发隐式补采、倍率或按 target 修正。

## 9. 计时与验收边界

HiCache I/O/control 标签计 operation 自身的 service 和明确归属的 control CPU：

- DMA 使用 device-transfer 时钟；storage 当前使用函数墙钟代理，其中可能仍含函数内部调度等待，不能宣称为完全纯净的设备时间；
- 函数外已识别的 residual CPU gap、完整请求 wall time和随机到达空白不进入 HiCache I/O/control 参数；
- Prefill/Decode compute 分项计已归属 device compute/collective；组合 scope 还保留对应 submit CPU；
- gap-excluded scope 组合 HiCache I/O/control + Prefill + Decode 在预测 DAG 上的关键路径，保留必要因果/资源边；
- 未归入上述组件的 wrapper/probe 成本与 residual gap 被排除，不等于完整应用墙钟只减去所有 CPU 空白；
- formal window 由 workload 的语义起止点决定，不按 config/cell 写死时间边界。

默认评分只保存紧凑指标。oracle-cost replay 只有显式开启时才用 target observation 替换预测 cost，用于诊断操作级绑定和 cost
sensitivity；它不是完整 target 时序同构证明，不是预测成绩，也不能回写参数。
回放前先检查相同预测成本注入恒等性；control 输入是明确观测的 CPU 工作，不是轮询等待。
控制操作边界独立于 duration 数值存在，不能因 oracle control 为零而删除依赖节点。

09-12 之前的旧 60-cross 只作修复前参照，其结构检查范围较窄。09-12 严格评分的 HiCache I/O/控制
WAPE/p90/delta 为 5.186%/32.518%/8.121%，严格 gate 未通过。09-14 又完成新 workload、新配置及
TP=4 的十二格泛化实验；两批结果、严格结构口径和不抵消分项统一见
[验证与当前限制](validation/hicache_validation.md)，不能把旧 60 格当作新面板成绩。
新结果暴露完整前缀匹配边界、短超时等待/可见性投影及成本泛化限制；没有根据 target 分数追加参数或改变公式。
