# Modeling 开发与使用

本文描述当前唯一可执行流程。模型公式见 `docs/hicache_io_cost_model.md`，结果见
`docs/validation/hicache_validation.md`，不可违反的边界见 `docs/project_constraints.md`。

## 1. 总体结构

所有框架共享基础链路：

```text
profile_manifest.json -> normalized source DAG -> topological simulation
```

SGLang 在此基础上增加 HiCache：

```text
source facts + target config
  -> target cache-state/effect plan
  -> Prefill/Decode work plan
  -> HiCache I/O/control + phase cost plan
  -> one atomic DAG patch
  -> simulation
```

KTransformers 是同级目标框架，继续使用 manifest、DAG build 和 simulation，但没有 HiCache module。NodeScale 是默认关闭的框架无关
duration-only 变换，也不属于 HiCache。

基础 CPU 时序先统一有明确线程归属的 CANN 显示进程，避免嵌套同步被拆成两份成本。
对能够关联的任务提交和消费，任务开始取“线程空闲、任务已提交”两者的较晚时刻，再加 base 中实测的剩余间隔；
整段等待不再固定重放。没有关联证据的 gap 保留，剩余间隔不参与 HiCache 或 Prefill/Decode 成本拟合。
这只修复已确认的任务等待，尚未完成所有 CPU 等待、业务响应出口和完整 E2E 建模，当前验证见进展文档。
消费线程的框架事件与底层运行时调用也须合并到同一 CPU lane；去除嵌套外层后，实际保留的底层调用继承任务关联。
提交身份由明确的 enqueue 类别决定，不能由跨线程遍历中“第一次遇到关联号”决定。

Event 等待按主机调用顺序绑定：读取同句柄在等待调用前最近的一次 Record，后续复用不改变已提交的等待。
记录调用重叠或身份不明确时不猜；没有等待依赖时保留原始阻塞耗时，不缩成固定返回开销。
已绑定同步仍使用现有的 10 µs 近似，尚非测准的通用成本模型。设备 stream 排序使用原始纳秒余量，
不人为移动 WAIT 时间；CPU 合成 self 区间及 scope 仍按整微秒划分，维持一致的区间顺序。
设备时钟物理对时尚在离线验证中，没有把一次测得的频率写成正式模型常数。

可选请求入口观测与发送观测共用 CPU gap 分段逻辑。瞬时时刻只生成一个零耗时点，区间仍生成首尾两个点；
线程不明、位置有歧义或 gap 已被改写时不猜。插点本身不增删成本，也不改变业务终点。
正式窗口为串行请求、且 source 收发观测完整时，已将 bench 的客户端完成与下一次请求开始接成动态依赖。
前端提交、响应返回及请求间客户端处理时间取自 base；服务端原有 residual 等待仍保留，不能据此宣称等待成本已正确建模。
`run_summary.http_client.e2e_us` 单独报告显式客户端完成时间，`simulated_e2e_us` 仍是图结束时间；
control/scope 指标在加入客户端链之前计算。缺少观测、请求并发或窗口内存在尚未表达的控制步骤时，
`http_client.status` 明确说明不可用原因，不以图结束时间代替 HTTP 结果。现有组件评分没有改成 HTTP 评分。
接收调用区间只提供开始点，区间末端复用分发就绪点；它覆盖的 CPU 调用及 gap 仍按原图保留。
Gloo 事件目前缺少可用的全局通信序号，不能简单按全 trace 出现次序配对或把较慢 rank 的等待都当作传输成本。

缓存 lookup/commit 事实不表示设备全局同步。核心不再按查询时刻挑选“最近的任意 CPU 节点”，
并将所有设备流强行接到该节点前后；这会把后台轮询的等待错误传给请求。请求串行性由客户端收发链表达，
设备依赖由实际提交、Event/Stream 同步等证据建立。真实共享资源依赖仍需保留，不能因为删除假屏障就删除后台工作。

source 中已证明的预取轮询等待是否需要移除，与 target 是否需要 I/O 完成节点，是两个独立判断。
即使 target 是立即返回或零 payload，也要按 source 的等待合同处理旧 false→true 轮询覆盖；
不能因关闭 target join 就保留旧等待。未知 gap、终止检查之外的工作和 self NoOp 仍保留，
mutation 与前后验证共用 `replaces_source_completion_wait()` 的语义判断，不改变 target 策略。

完整回放现在会重新安排已可靠识别的 CPU 任务队列：只有整条 worker lane 的连续叶子都能通过 correlation
唯一对应提交事件时，才将跨任务的 source 顺序换成预测到达顺序。任务内部执行、生产者顺序及真实同步依赖保留，
实际采用的队列顺序写回 DAG，供后续组件回放使用；身份不完整的线程仍沿用原有依赖。
到达时间暂取提交函数返回，任务在 worker 空闲且依赖满足后，再承担 source 实测的剩余就绪间隔。
异步任务可能在提交函数返回前就开始，因此 HTTP 摘要中的 `cpu_task_queues` 明确报告这一近似的重叠次数与时长，
以及队列数量、任务数和最大深度；缺少客户端观测时不输出这组 HTTP 附属信息。
目前不模拟队列满时的提交阻塞，未识别的 CPU 等待仍保留；不能据此声称所有 CPU gap 已可预测。

Prefill 的可复用范围还受请求的前缀查询上限约束：缓存中存在完整输入，不代表允许全部跳过计算。
模型保留 lookup 输入的长度上限，extend 时按目标页大小限制缓存视图，不照搬 source 实际命中量；
完整输入仍用于提交和生命周期。分配工作按 batch 独立记录，包含预热，并用于推导进程内的内核准备状态。
正式模型保留 source 的 prepare/load 包围区间作为非执行元数据。观测签名符合当前 NPU allocator 时，
按目标页数、batch/extend 上界和 free-page 指针对齐判断是否走 Triton、是否已准备；不按配置名分类。
目标已准备或转为 naive 分配时，只移除对应 source 调用中已观测、且承载于未改写 CPU gap 的准备覆盖；
prepare/load 取并集，未覆盖的时间、原始观测及依赖保持不变。目标仍需相同变体时保留实测准备成本。
新变体成本未覆盖、来源不足或承载冲突时不修改准备成本，在 patch 摘要 `runtime_preparation` 中报告限制。
其 `call_counts` 包含预热，`removed_coverage_us` 是跨 rank 覆盖总量，不是 E2E 收益；`ready` 也只表示这部分已覆盖。
未测量变体的冷编译/磁盘加载成本及其目标位置仍待实现，不能把当前准备处理称为完整运行时成本模型。

`source_io_observations` 与 `source_phase_observations` 均在任何模型 mutation 之前记录。
后者不能从已经清零原设备节点的 target 图重新采样，否则会把 source 成本误报为 0；
source 观测、预测成本和最终图统计分别保留，不互相替代。

可选的 scheduler 响应观测若唯一位于原始 CPU 顺序 gap 内，建图将它拆成 begin/end 两个零耗时连接点。
时间仍属于原 gap，分段后的观测长度用于排除区间计算；旧 CPU/设备节点的完成时刻不应改变。
这些点本身不是 HTTP 完成，也不能独立决定 E2E。跨边、重叠、额外顺序消费者或已有 gap 改写时不猜测绑定。

## 2. 职责边界

| 阶段 | 输入 | 输出 | 不负责 |
| --- | --- | --- | --- |
| Profiling | framework/config/workload | manifest、trace、formal window | 预测 target |
| DAG build | 一个 source manifest | normalized source DAG | 猜 target DAG |
| Effect/work planning | source facts、target HiCache config、统一 service model | target I/O 执行/可见结构、phase 工作量 | 用 target 标签选结构 |
| Cost model | effect demand、固定校准、base phase 参数 | duration；供完成/timeout 与 DAG 共用 | 用 target 残差选结构 |
| DAG patch | source DAG、effect/work/cost plan | target DAG | 拟合系数 |
| Simulation | target DAG | component timing | 回填模型 |
| Evaluation | 已完成预测、target profile | score | 补采、构模、重预测 |

预测只接收 source manifest、target config 和已建模型。target profile、target structure oracle、target cost 与 E2E 只属于 evaluator。

已实际验证 TP=2→TP=2 和 TP=4→TP=4 的新 workload/HiCache 配置预测与独立评分，未实现 TP=2→TP=4 的跨 rank 扩图。
更换 TP 属于更换参数环境，需要该环境的 base profiles 和物理/固定校准；不能直接把 TP2 成本参数用于 TP4。
运行成功不等于全部精度门槛通过，具体覆盖与限制见 [泛化结果](validation/hicache_validation.md#10-09-14-新-workload--新配置--tp4-泛化结果)。

## 3. 唯一公开入口

```bash
scripts/model.sh --help
```

正式用户流程由三个动作组成：

```text
build-dag -> prepare-hicache -> evaluate-hicache
```

以下动作是同一流程的可独立排查阶段，不构成另一套产品路径：

- `calibrate-hicache physical|runtime-dma`
- `build-hicache-model`
- `predict-hicache`

不存在单独的 phase calibration 命令、旧模型版本转换、target-gap candidate loop 或“评分时重新预测”入口。

## 4. 构建普通 DAG

SGLang 和 KTransformers 都使用：

```bash
scripts/model.sh build-dag \
  --profile-manifest <profile_manifest.json> \
  --output-dir <dag-output>
```

该动作不需要 HiCache model。对 KTransformers 调用 HiCache prediction 会返回 capability 错误，而不是伪造空 HiCache 结果。

## 5. 一个 base 组

一个组描述：一个 base config、该 base 的 workload profiles、一份平台物理校准、一份固定校准以及要预测的 target config。最小形态：

```json
{
  "profile_suite": "configs/experiments/hicache_manual_workload/profiling_full_dag_replay.json",
  "base_config": "<base-id>",
  "workload_ids": ["<w1>", "<w2>", "<w3>"],
  "base_manifests": ["<base-w1-manifest>", "<base-w2-manifest>", "<base-w3-manifest>"],
  "target_configs": ["<target-a>", "<target-b>"],
  "physical_calibration": {
    "report": "<physical-report>",
    "measurement_sources": ["<original-measurement>"],
    "measurement_description": "Target-independent platform primitives."
  },
  "fixed_calibration_manifests": ["<page-low-repeat-1>", "<page-high-repeat-1>"],
  "budget": {
    "wall_seconds": 0,
    "server_starts": 0,
    "requests": 0,
    "tokens": 0,
    "repeats": 1
  },
  "output_dir": "data/modeling_runs/<group>"
}
```

`repeats` 是每个物理 page 端点所需的成功 profile 数。显式 manifests 可以满足它；不足时只有在剩余总预算内才原样补采。失败和无效
trace 同样计入 wall/start/request/token 预算。

缺 base profile 时可另外声明 `base_capture_budget` 和 `forced_token_bundle`。缺物理数据时可声明：

```json
{
  "physical_capture": {
    "page_token_sizes": [32, 64, 128],
    "devices": [2, 3],
    "cpu_sets": "<rank-0-cpus>|<rank-1-cpus>",
    "budget": {
      "wall_seconds": 900,
      "container_starts": 2,
      "logical_io_bytes": 100000000000
    }
  }
}
```

`page_token_sizes` 是部署平台的采样域，必须显式给出，不能从 `target_configs` 推导。设备与 CPU 集合应匹配该 base 的 TP/资源环境。

## 6. 准备、构模和预测

```bash
scripts/model.sh prepare-hicache --group <group_request.json>
```

它严格单向执行：

1. 准入或在独立预算内采集缺失 base profiles；
2. 准入或采集 target-independent physical calibration；
3. 生成一个固定 workload，在物理域最小/最大 page 端点原样采集到声明重复数；
4. 提取 base 和固定校准 observations；
5. 构建一个 HiCache I/O/control + phase model；
6. 用该模型和每个显式 target config 预测全部 source workload。

不会执行 `target requirements -> gap -> candidate -> refit` 循环。target 不受支持或结果不够准时，输出 limitation；它不会回到采集器改变
固定 workload。

只检查输入和 readiness：

```bash
scripts/model.sh prepare-hicache --group <group_request.json> --dry-run
```

只准备数据、不构模和预测：

```bash
scripts/model.sh prepare-hicache --group <group_request.json> --calibration-only
```

源码或原 trace 有实质变化时可用 `--refresh-observations`。项目不通过 schema version、文件摘要、runner hash 或冻结副本自动判断代码是否变化。

独立重建模型：

```bash
scripts/model.sh build-hicache-model --group <group_request.json>
```

输出：

```text
<group>/
  observations.json
  calibration_plan.json
  hicache_io_model.json
  phase_calibration.json
  model_build_summary.json
```

`ready` 只表示参数可辨识且合同完整，不表示 target 精度通过。summary 中的 `target_inputs` 和 `target_score_inputs` 必须为空。summary 还应
保留物理测量环境和 I/O 观测域；预测超出固定校准调用大小时标记为超域，而不是隐式改变公式。

## 7. 独立预测

```bash
scripts/model.sh predict-hicache \
  --source-manifest <base/profile_manifest.json> \
  --target-config <target.json> \
  --hicache-io-model <hicache_io_model.json> \
  --output-dir <prediction-output>
```

`target.json` 只含 `{name?, hicache}` 配置。多个 source 或 target 可重复传参。`--max-predictions` 只用于开发期挑少量语义关键 cell，
不能改变模型。

默认 `--diagnostics off` 保留评分必需的 compact summary；`--diagnostics full` 才保留逐操作 row、C++ model summary 和成功日志。
diagnostics 不改变预测语义。

主要产物：

```text
<prediction-output>/
  workflow_summary.json
  preflight_summary.json
  artifacts/model_run_plan.json
  model_runs/<cell>/
    runner_config.json
    cpp_model_config.json
    run_summary.json
```

## 8. 独立评分

只有所有选中的预测完成后，才允许打开 target profiles：

```bash
scripts/model.sh evaluate-hicache \
  --prediction-dir <base-a-predictions> \
  --prediction-dir <base-b-predictions> \
  --profile-run-dir <target-profile-suite> \
  --output-dir <separate-score-output>
```

evaluator 不接收模型或采集预算，不执行普通 prediction，也不回写任何参数。多 base 结果同时给出总面板和 `by_source`，不能用总均值隐藏
失败 base。

显式结构/cost sensitivity 诊断：

```bash
scripts/model.sh evaluate-hicache \
  --prediction-dir <predictions-made-with-full-diagnostics> \
  --profile-run-dir <target-profile-suite> \
  --output-dir <oracle-score-output> \
  --oracle-cost-replay \
  --oracle-max-runs 1
```

oracle replay 保持已经预测出的 operation/effect plan，只替换 target-observed cost。默认评分不生成或保存 oracle cost 数组；它验证
操作级绑定与 cost sensitivity，不证明 source 骨架和 target 到达时序完全相同，诊断结果不计作泛化精度。
每个选中 cell 先执行一次相同预测成本回放，计时、ownership、关键路径及节点/边计数不一致就停止该 cell 诊断。
通过后才执行五种 target 成本变体；`--oracle-max-runs 1` 是每 base 一个 cell、六次回放，不是只运行一次 C++。

## 9. Component 与结果解释

| Component | 当前状态 | 正式评分 |
| --- | --- | --- |
| HiCache I/O/control（内部 `hicache_direct`） | implemented | service/control、delta、按 kind、不抵消分项 |
| `prefill` | implemented | compute、delta |
| `decode` | implemented | compute、delta |
| residual CPU gap | deferred | 排除 |
| Python snapshot overhead | 默认采集已移除 | 排除 |

gap-excluded scope 是 HiCache I/O/control + Prefill + Decode 在 target DAG 上的组合关键路径，不是完整应用 E2E。formal window 是 workload 语义边界，
不按 config/cell 写死。
它保留这些组件的 service、compute/collective、submit/显式 control CPU 与必要资源依赖，
排除 residual gap 和未归入组件的 wrapper/probe 成本；存储 service 仍含函数内部可能的等待。
因此不要把该指标描述为“完整墙钟仅减去所有 CPU 空白”。

逻辑命中、容量策略和 operation 身份先由 source facts 与 target config 决定；但 timeout 前完成了多少 batch 是时间因果问题，因此状态推进和
最终 DAG 必须使用同一个 service cost。这里不是用 cost 反向拟合结构，而是避免用两套互相矛盾的时钟判断同一个 deadline。

## 10. 容器与构建

- SGLang 推理、profiling、physical/runtime-DMA：`sglang` image；
- KTransformers 推理/profiling：`ktransformers` image；
- observation、DAG、构模、预测、评分：`modeling` image。

Release 构建：

```bash
scripts/run.sh modeling -- bash -lc \
  'cmake -S src/modeling/trace_graph -B build/modeling/trace_graph-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DTRACE_GRAPH_DEBUG=OFF && cmake --build build/modeling/trace_graph-release --target trace_graph -j2'
```

需要 oracle/详细诊断时另建 `TRACE_GRAPH_DEBUG=ON` 的 validation binary。Debug 只增加证据，不改变 business model。

## 11. 代码所有权

| 路径 | 职责 |
| --- | --- |
| `scripts/internal/markov_internal/modeling_workflow/group.py` | one-base 输入与预算合同 |
| `base_capture.py`、`physical_capture.py`、`capture.py` | 三类独立、串行、有限预算采集 |
| `fixed_calibration.py` | 唯一固定 workload 生成与重复计划 |
| `observations.py`、`coverage.py`、`stability.py` | source-only 提取和 readiness |
| `io_model_builder.py`、`control_cost.py`、`phase_calibration.py` | 唯一数值模型 |
| `prediction/`、`execution/` | source-to-target DAG prediction |
| `evaluation/`、`validations/` | score-only 与显式 oracle 诊断 |
| `src/modeling/trace_graph/src/modules/hicache/model/` | target effect/phase work planning |
| `src/modeling/trace_graph/src/modules/hicache/patch/` | cost materialization 与 atomic patch |
| `src/modeling/trace_graph/src/simulation/` | topological simulation |

当前实现和数据资产见 `docs/work_progress.md`。
