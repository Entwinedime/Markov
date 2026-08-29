# HiCache Direct I/O cost 模型

状态：模型实现、planning/cost 分离和 60-cell 验收已完成。结构为 PASS，数值结论为
`MODEL_LIMITATION`；限制来自某些 selected base 没有观测到足够的 positive-payload service family。

## 1. 这部分模型回答什么

HiCache effect planner 已经回答“目标配置需要哪些 I/O/control operation”。cost model 只回答第二个问题：

> 给定一项已经预测出的 operation、传输字节数和页数，它应当在目标 DAG 的资源节点上占用多少微秒？

它不预测完整请求 E2E，也不修改 I/O 结构。一次 cross prediction 仍然是：

```text
source DAG + target config
  -> target effect plan（使用 calibration-only io_planning）
  -> Direct I/O/control cost plan
  -> DAG patch
  -> max-plus simulation
```

这里没有通用机器学习“训练”。model build 只是用一次共享 calibration 与当轮选定 base 的三个 workload observation
估计少量物理系数；target observation 在 prediction 完成后才能用于评分。

## 2. 计时边界

Direct cost 采用可从 trace 中重复提取的业务时钟，不采用 Python wrapper 的整段 wall time。

| operation family | service 标签 | intrinsic control 标签 | 明确排除 |
| --- | --- | --- | --- |
| `prefetch`，storage -> host | 嵌套 storage-read service span 的时间并集 | 去掉已证明 collective arrival wait 和未覆盖区间后的 terminal progress CPU 时间 | foreground wait、Python wrapper residual、CPU gap |
| `load`，host -> device | 对应 device-transfer event 的时间并集 | admission/submit/terminal 的已拥有 CPU node | completion wait、CPU gap、wrapper residual |
| `write_device_to_host` | 对应 device-transfer event 的时间并集 | `0`，当前 DAG 没有可证明必须归它拥有的 host CPU node | submission wrapper residual |
| `write_host_to_storage` | 嵌套 storage-write service span 的时间并集 | `0` | outer wrapper residual、无法单独归属的 scheduler wait |

`service_us + control_us` 是 Direct score 的加法口径；它不是完整应用 E2E。DAG 中节点何时阻塞下游、与其他节点是否重叠，
由 dependency 和 resource lane 决定，不能再把相同等待加进 service 公式。

### 2.1 H2S active 与 completion 的决定

当前 trace 的 H2S 标签是嵌套 storage service 的完整时间并集，当前 DAG patch 也只为一项 H2S effect 生成一个带 duration 的
resource node。没有第二个、可独立观测的 completion cost node。因此本轮采用一个 service clock：

```text
H2S service_us = 该项 storage service 的预测占用时间
```

旧实现把 active projection 和累计 completion-blocking surface 相加后写入同一个节点；这与评分标签不是两个独立时钟，存在
重复计费风险。新模型删除这次额外相加。若未来 profile 能证明 capacity gate 等待是另一个执行对象，应把它建成显式 dependency
或独立节点，再单独校准；不能在现有 service duration 上追加一个历史压力 correction。

### 2.2 effect planning 速率不是 cost 系数

effect replay 需要一个物理速率来判断异步 I/O 在某个 target control boundary 前完成了多少页；这会影响 operation readiness、
partial/complete 状态和最终 I/O 结构，但不直接生成 DAG duration。该输入使用独立的 `io_planning` 合同：

```text
planning_rate[device_host] = device↔host calibration 曲线中的保守速率
planning_rate[host_storage] = host↔storage calibration 曲线中的保守速率
```

两个速率只由共享物理 calibration 派生。它们不乘 selected-base `runtime_scale`，也不读取 source/target observation；因此换 C5、
C1 或 C3 做 base 不会改变同一 target 的 effect planning primitive。模型文件把它们保存为顶层 `planning_rates`，投影给 C++ 时成为
与 `io_cost` 平级的 `io_planning`。`io_cost` 只负责计算已确定 operation 的 service/control duration。

这里采用“相关物理曲线中的最小校准速率”作为共享 lane 的保守 readiness 速率，是统一的 calibration 规则，不是 config 白名单或
target 拟合边界。若未来需要更细的并发资源模型，应增加独立 lane/evidence，而不是把 base cost scale 再带回 effect planner。

## 3. 基本量

### 3.1 特征，也就是预测时已知的变量

| 名称 | 类型/单位 | 来源 | 作用 |
| --- | --- | --- | --- |
| `family` | 离散类别 | effect type/direction | 选择物理传输路径，不作为数字拟合 |
| `operation_count` | 次 | effect plan | 计算每次调用的 setup/control |
| `page_count` | 页 | effect plan | 计算逐页 setup/control |
| `byte_count` | byte | effect plan 的 KV geometry projection | 计算传输时间 |
| `page_bytes` | byte/page | `byte_count / page_count` | 在 calibration anchor 之间插值 |
| `storage_existing_page_count` | 页 | H2S target residency decision | 选择已有 key 的覆写路径 |
| `storage_new_page_count` | 页 | H2S target residency decision | 选择新 key 的写入路径 |
| `existing_pages_per_operation[]` | 页/次 | H2S effect operation shape | 只用于已有 key 的 calibration payload-size 曲线 |
| `zero_payload` | bool | `byte_count == 0` | 选择只执行 terminal control 的 prefetch |

H2S 的 existing/new byte 数由各自 page 数乘 `page_bytes` 得到。所有特征都来自预测的 target effect；target config 只在上游改变
effect/state，不能作为 cost feature。source config、workload 名、cell 名和 target 实测 duration 也不是 feature。

### 3.2 系数，也就是模型需要估计的量

| 名称 | 单位 | family | 来源与含义 |
| --- | --- | --- | --- |
| `bandwidth_cal[family, page_bytes]` | byte/s | prefetch、load、D2H | 独立 storage/runtime-DMA calibration；表示该硬件路径的基准吞吐 |
| `new_bandwidth_cal[page_bytes]` | byte/s | H2S new | 独立真实 server storage-write calibration |
| `existing_bandwidth_cal[page_bytes, operation_pages]` | byte/s | H2S existing | 独立 existing-key calibration；payload size 是物理维度，不是配置特例 |
| `setup_cal[family, page_bytes]` | us/次或 us/页 | prefetch、H2S new | calibration 中观测到的调用/页固定工作 |
| `runtime_scale[family]` | 无量纲 | 四个 service family | 一个 base 的 service 总量与 calibration projection 之比；表示隔离 calibration 到真实 runtime 路径的统一时间尺度 |
| `runtime_scale[h2s_existing/new]` | 无量纲 | H2S 两条 residency path | 一个 base 对已有 key 与新 key 路径分别辨识的时间尺度 |
| `control_fixed[prefetch]` | us/次 | positive prefetch | 一个 base 中去掉 collective/probe nuisance 后的每次 intrinsic progress control |
| `zero_payload_control_fixed` | us/次 | zero-payload prefetch | 共享 snapshot-free control calibration |
| `control_page[load]` | us/页 | load | 共享 control calibration 跨 policy 的稳健逐页中心值 |
| `control_fixed[load]` | us/次 | load | 一个 base 在扣除共享逐页项后的剩余每次 admission/submit control |

`runtime_scale` 是唯一允许由 base service 标签估计的 family 修正。它不是 target residual，也没有 target/config/cell 分支、support taper、
pressure ratio 或 workload-specific surcharge。业务模型输出只保存系数，不保存一套“某配置专用”的公式。
`planning_rates` 是上游 effect primitive，不属于本表的 duration coefficient；其来源和作用见 2.2 节。

## 4. calibration 曲线

共享 calibration 负责给出不同物理路径的曲线形状：

- prefetch：一个 storage-read bandwidth 和逐页 setup；
- load/D2H：4、8、16 MiB page 的 sustained runtime-DMA bandwidth；
- H2S new：4、8、16 MiB page 的每次 setup 和 bandwidth；
- H2S existing：page bytes 与单次 operation pages 对应的有效 bandwidth；
- zero-payload prefetch：snapshot-free terminal control 的每次固定时间；
- load：跨 policy 汇总后的逐页 intrinsic control 中心值。

在相邻 page anchor 之间，bandwidth 在 log(page bytes)-log(bandwidth) 空间线性插值，setup 在 log(page bytes)-setup 空间线性插值。
existing-key 的 operation-pages 维也采用相同的小型插值。当前 5×3 面板位于 calibration 的 4–16 MiB page 支持域内。

域外输入只取最近 anchor 并报告 `outside_calibration_range`，不额外缩放，不沿边界生成 taper/correction，也不把配置名写成边界。
这是一条数据支持边界，不是硬代码配置边界。

## 5. 最小 service 模型

令 `n` 为 operation count，`p` 为 page count，`b` 为 byte count，`s_f` 为 family 的 runtime scale。

### 5.1 Prefetch

```text
q_prefetch = p * setup_page_cal[prefetch]
           + b * 1_000_000 / bandwidth_cal[prefetch]

service_prefetch = runtime_scale[prefetch] * q_prefetch
```

### 5.2 Load 与 D2H

这两个方向使用 sustained runtime-DMA page curve，不再按 payload、前后台状态或对向流量切换 bandwidth：

```text
q_dma(f) = b * 1_000_000 / bandwidth_cal[f, page_bytes]
service(f) = runtime_scale[f] * q_dma(f)
```

资源竞争由 H2D/D2H lane 的先后依赖表达；公式不再包含 `runtime_state`、`base_runtime_correction` 或
`cross_direction_contention`。

### 5.3 H2S existing

已有 key 路径不会创建新的 storage payload。对每个已知 operation shape 计算：

```text
q_existing = sum_i(
    existing_bytes_i * 1_000_000
    / existing_bandwidth_cal[page_bytes, existing_pages_i]
)

service_existing = runtime_scale[h2s_existing] * q_existing
```

若 aggregate effect 没有逐 operation shape，则使用 `existing_page_count / operation_count` 作为一个明确记录的平均 shape；不使用
prelude bytes、host-pool ratio 或累计 pressure surface 猜测。

### 5.4 H2S new

```text
q_new = operation_count * setup_operation_cal[h2s_new, page_bytes]
      + new_byte_count * 1_000_000 / new_bandwidth_cal[page_bytes]

service_new = runtime_scale[h2s_new] * q_new
```

一项 H2S effect 同时含 existing/new pages 时：

```text
service_h2s = service_existing + service_new
```

不再按 queue bytes 选择 burst/sustained，不再附加 active runtime correction 或累计 completion curve。resource lane 会串行化共享
storage writer 上的 operation，因此 cost 公式不重复表达 queueing。

## 6. 最小 control 模型

```text
positive prefetch control_us = operation_count * control_fixed[prefetch]
zero-payload prefetch control_us = operation_count * zero_payload_control_fixed

load control_us = operation_count * control_fixed[load]
                + page_count * control_page[load]

D2H control_us = 0
H2S control_us = 0
```

旧 load control 中的 allocation-retry、clean/dirty victim、dirty metadata exponent、same-epoch D2H surcharge 和 source-duration transfer
均删除。它们在现有三个 base workload 中与 policy、page geometry 和 operation shape 高度相关，不能稳定辨识为多个独立系数。
若简化后局部 control tail 变差，应作为模型限制报告；不能自动恢复 surcharge 网络。

## 7. 一张参数估计表

模型构造先把共享 calibration 和选定 base 的三个 workload 归一成下列简单表。默认按
`workload × family` 聚合；只有 existing H2S 的 operation shape 保留数组。

| 列 | 角色 | 是否进入公式 | 说明 |
| --- | --- | --- | --- |
| `family` | 分组 | 是 | 选择唯一 family 公式 |
| `operation_count` | feature | 是 | 正整数或 zero-payload control 次数 |
| `page_count` | feature | 是 | target-independent base effect 数量 |
| `byte_count` | feature | 是 | base effect 的 KV bytes |
| `page_bytes` | derived feature | 是 | `byte_count/page_count` |
| `existing_page_count` / `new_page_count` | feature | H2S only | residency split |
| `existing_pages_per_operation` | feature | H2S existing only | 小型 payload curve坐标 |
| `calibration_service_us` | fixed projection | 是 | 不使用 base label时先由 calibration 算出的 `q` |
| `observed_service_us` | label | 仅参数估计 | 第 2 节定义的 service clock |
| `observed_control_us` | label | 仅参数估计 | 第 2 节定义的 intrinsic control |
workload/config/cell identity 不进入表，也不进入系数；build summary 只报告 observation 数量、family coverage 与 H2S 可辨识秩。

当前 C5 base 的 12 个聚合行覆盖四个 family：W1/W3 覆盖 existing H2S，W2 覆盖 new H2S；C1/C3 的三个 workload 也覆盖
四个 family，并至少提供纯 existing 与含 new 的 H2S 行。C2/C4 可能完全不触发某些 service family；这不是零耗时标签，
而是该 base 对该系数没有辨识证据。换 base 时仍使用同一列、同一公式和同一估计器，不合并多个 base。

## 8. 唯一参数估计方法

### 8.1 Service runtime scale

prefetch、load 和 D2H 各自只估计一个正时间尺度：

```text
runtime_scale[f] = sum(observed_service_us[f])
                 / sum(calibration_service_us[f])
```

这相当于让一个 base 告诉模型“真实 runtime 路径整体比隔离 calibration 快或慢多少”，而不是让每个 workload 产生 correction。
求和只包含 `byte_count > 0` 的 base 行。若一个 family 在该 base 的三个 workload 中完全未出现，则没有合法比值可估计；模型保持
共享 calibration 的 `runtime_scale=1.0`，并在 model-build summary 中以零 coverage/rank 表达“未辨识”。它不能读取其他 target
observation 来补齐，这也是 C2/C4 作为 base 时必须保留的泛化规则。

H2S 将 calibration projection 拆成 `q_existing` 与 `q_new`，用所有 base H2S 行求唯一的非负二系数最小二乘：

```text
observed_h2s ~= s_existing * q_existing + s_new * q_new
```

只有参数估计表在这两列上的秩为 2 时才同时估计两个 scale。若只能辨识一个路径，已辨识路径使用总量比，另一条路径保持 calibration
scale `1.0`；若 H2S 完全未出现，则两者均为 `1.0`、design rank 为 0。任何一种情况都不能借 target observation 补齐。

### 8.2 Control

- `zero_payload_control_fixed` 直接取共享 snapshot-free calibration 的稳健值；
- `control_fixed[prefetch] = sum(base positive prefetch control) / sum(operation_count)`；
- `control_page[load]` 取共享 control calibration 所有支持 policy/page 点的稳健中位数，不按 target policy选择；
- `control_fixed[load]` 是 base load control 扣掉逐页项后，按 operation count 归一的非负剩余。

上述估计器在实现前已经固定。不能尝试多种公式后用 cross target 分数选择最好的一种。

## 9. 可辨识性审查

| 旧机制/候选系数 | 决定 | 原因 |
| --- | --- | --- |
| family runtime scale | 保留，每 family 一个 | calibration projection 与 base service 总量可独立观察 |
| setup 与 bandwidth 同时从 base 重拟合 | 删除 | 一个 base 的 page/payload变化不足；曲线形状已由 calibration 给出 |
| H2S existing/new 两个 scale | 有秩条件地保留 | residency split 是 effect feature，三个 workload 提供不同组合 |
| DMA payload surface | 删除 | payload、page count 与 workload shape 强相关；sustained page curve 已表达主要硬件差异 |
| burst/sustained runtime state switch | 删除 | 状态选择依赖前后台/arrival heuristic，resource DAG 已表达执行次序 |
| cross-direction contention scale | 删除 | 与同 epoch demand 同源且会和 lane overlap 重复建模 |
| H2S queue/pressure/host-pool surface | 删除 | 维度多于一个 base 能独立变化的维度，且容易把配置状态变成 cost correction |
| completion-blocking marginal addition | 删除 | 当前没有独立 label/node，和 H2S service clock可能重复 |
| load retry/dirty/same-epoch surcharge 网络 | 删除 | 多个特征共同由 policy/state触发，样本中高度相关，跨 base tail 也不稳定 |
| config/workload/cell coefficient | 禁止 | 不能泛化到未见配置 |

## 10. 预测与 DAG 的职责分工

预测时只执行：

1. effect plan 提供 family、operation/page/byte 与 H2S residency split；
2. effect replay 只用 calibration-only `io_planning` 解析异步完成状态，不读取 base runtime scale；
3. cost model 查 calibration curve 并代入当轮唯一 coefficients；
4. 每项输出 `service_us`、`control_us` 和紧凑 breakdown；
5. patcher 把 service/control materialize 到各自节点；
6. resource lane 串行化同一物理资源，其他 dependency 表达 consumer readiness；
7. simulator 用 max-plus 计算重叠与关键路径。

允许的 service breakdown 只有：

```text
calibration_setup_us
calibration_transfer_us
runtime_scale
predicted_service_us
```

H2S 再分 existing/new 两组。control breakdown 只有 fixed/page/zero-payload。diagnostics 不再输出 pressure、taper、support transfer、
contention 或 residual correction 明细。

## 11. 手算示例：一次 C5 base prefetch operation

这个示例只演示参数估计和预测路径，不把 C5 名写入模型。共享 calibration 给出：

```text
setup_page = 573.0925 us/page
bandwidth = 2,674,816,317.857 byte/s
```

选定 base 的 positive-prefetch 聚合标签为 `2,944,180 us`，共 256 pages、4,294,967,296 bytes。因此：

```text
base calibration projection
  = 256 * 573.0925
  + 4,294,967,296 * 1,000,000 / 2,674,816,317.857
  = 1,752,417.189 us

runtime_scale[prefetch]
  = 2,944,180 / 1,752,417.189
  = 1.680067977
```

对一个预测出的 8-page、134,217,728-byte prefetch operation：

```text
calibration setup    = 8 * 573.0925                 = 4,584.740 us
calibration transfer = 134,217,728 * 1e6 / bandwidth = 50,178.297 us
predicted service    = (4,584.740 + 50,178.297) * 1.680067977
                     = 92,005.625 us
materialized duration = ceil(predicted service) = 92,006 us
```

这里 `pages/bytes` 是 feature，`setup/bandwidth/runtime_scale` 是 coefficient。target 的实际 cost 没有参与任何一步。

## 12. 评分与明确留待后续的误差

新模型先用与 P8 相同的 9 个语义关键 cell 检查，再按 C5、C1、C3 各 12 cross 验证换 base 外推，最后才运行 60 cross。
所有 cross target observation 只用于 score。Direct 总体目标是 WAPE 不高于 3%、p90 APE 不高于 5%；局部 family/control tail
必须单独报告，但不反向授权特例拟合。

P10-A 的 9-cell 结果为 Direct WAPE 2.125239%、p90 4.304800%，同骨架 normalized WAPE 0.613990%、p90 2.310819%。
随后 P10-B 用完全相同 builder/公式分别以 C5、C1、C3 为 base 完成三个 12-cross：Direct WAPE 分别为
2.239447%、1.828869%、2.001908%，p90 均不高于 4.488048%；delta weighted L1 分别为 3.122594%、2.094476%、
2.199851%，大变化方向均为 100%；同骨架 WAPE/p90 均通过 3%/5%。局部 load control、D2H、部分 H2S 和 C3 prefetch
仍失败。这些是简化模型舍弃旧 correction 后保留的局部限制，当前实现不为这些 residual 增加 target-driven 分支。

P11 进一步以 C1–C5 各作为当轮唯一 base，完成 60 个 non-self cross。C1/C3/C5 的 Direct
WAPE 为 1.829%/2.002%/2.239%，均达标；C2 为 6.474%，C4 为 23.944%。C2 没有 D2H/H2S
positive service observation，C4 四个 service family 均没有 positive-payload observation，因此相应 scale
无法从该 base 辨识。总体 Direct WAPE/p90 为 8.487%/36.857%，按预定规则报告 `MODEL_LIMITATION`，
没有使用 target score 恢复 correction。

该结果同时界定了单一 base 泛化的必要条件：base workload 必须对之后可能预测的每个 service family
提供至少一个 positive-payload observation，H2S existing/new 分开辨识则需要参数估计表具有相应的列秩。
这是数据信息完备性条件，不是 config 名称白名单或硬代码边界。

以下不属于本文模型：

- CPU 大 gap 与调度空白；
- prefill compute 数量/耗时变化；
- decode iteration/长度变化；
- Python probe/snapshot overhead；
- 没有显式 resource/dependency 证据的竞争 residual；
- 完整应用 E2E。

这些部分以后通过独立 `GapModel`、`PrefillModel`、`DecodeModel` 或新的 resource evidence 接口进入，不能混入 Direct I/O
bandwidth/control 系数。
