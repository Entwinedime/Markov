# Markov Trace Simulation

本项目从真实推理运行中采集 trace，构建可仿真的 DAG，并对配置变化进行图变换和性能预测。

当前建模主线是 SGLang HiCache 的 I/O/control 与 Prefill/Decode 变化：

```text
profile manifest
  -> source DAG
  -> target HiCache effect plan
  -> target phase work plan
  -> HiCache I/O/control + phase cost plan
  -> DAG patch
  -> topological simulation
```

HiCache I/O/control（内部 scope ID 为 `hicache_direct`）、`prefill` 和 `decode` 分别拥有自己的工作量与 cost；`gap` 仍未建模。结果同时报告独立 component 和排除
residual gap 的组合 scope，不能让一个 component 的系数吸收另一个 component 的误差。

## 公开入口

- `scripts/profile.sh`：在目标框架容器中采集 profile；
- `scripts/model.sh`：在 modeling 容器中构建 DAG、校准、建模和预测；
- `scripts/run.sh`：进入 SGLang、KTransformers 或 modeling 容器排查环境；
- `scripts/build.sh`：构建对应 image 和 hook。

内部 Python module 和 C++ binary 不是需要用户串联的第二套公开流程。

## Docker 环境

项目保留三个环境：

| 环境 | 用途 |
| --- | --- |
| `sglang` | SGLang 运行、profiling 和 framework hook |
| `ktransformers` | KTransformers 运行、profiling 和 framework hook |
| `modeling` | C++ TraceGraph、Python modeling workflow 和验证 |

构建或进入环境：

```bash
scripts/build.sh modeling
scripts/build.sh sglang
scripts/build.sh ktransformers

scripts/run.sh modeling
scripts/run.sh sglang
scripts/run.sh ktransformers
```

## Profiling

配置决定 framework、server、workload 和采集 channel：

```bash
scripts/profile.sh <experiment.json> --dry-run
scripts/profile.sh <experiment.json>
```

KTransformers 的共享 DAG smoke 配置位于：

```text
configs/experiments/ktransformers/profiling_dag_smoke.json
```

profiling 与 modeling 的唯一正式交接面是每个 run 的 `profile_manifest.json`。

## Modeling

查看当前正式动作：

```bash
scripts/model.sh --help
```

正式 HiCache 动作是：

```text
build-dag
prepare-hicache
evaluate-hicache
```

`prepare-hicache` 按 `base profiles → target-independent physical calibration → 固定小型 calibration →
唯一 cost model → 全部 target prediction` 单向执行。固定 calibration 是同一 workload 在平台 page 域两端的采集，允许原样重复；
它不查看 target 列表、target trace 或评分。`build-hicache-model`、`predict-hicache` 和
`calibrate-hicache physical/runtime-dma` 是该流程的可独立排查阶段，不是另一套产品流程。

除实际 SGLang/物理采集外，modeling 动作都在同一个 modeling 容器中完成；批量预测不会为每个 cell 再启动一个嵌套容器。

SGLang 与 KTransformers 共用 framework-neutral DAG 入口：

```bash
scripts/model.sh build-dag \
  --profile-manifest <profile_manifest.json> \
  --output-dir <dag-output>
```

它只构图和仿真，不要求 HiCache。KTransformers manifest 显式记录 framework，当前提供 LD_PRELOAD CPU trace；
HiCache prediction 只接受 SGLang source，并会对其他 framework 给出 capability 错误。

一个 base 组的正式入口是：

```bash
scripts/model.sh prepare-hicache --group <group_request.json>
```

它需要该 base 的三个 profile、固定校准 profile 和平台物理校准；缺失且声明了预算时会采集，否则只报告缺项。单独重放已建模型时只需要
source manifest、显式 target HiCache 配置和 one-base HiCache model：

```bash
scripts/model.sh predict-hicache \
  --source-manifest <source/profile_manifest.json> \
  --target-config configs/modeling/hicache_target_example.json \
  --hicache-io-model <one-base-model.json> \
  --output-dir <prediction-output>
```

真实 target profile 不属于该命令。5×3/12/60-cell 和 target oracle 只通过 `evaluate-hicache` 进入评分流程。
已有预测使用只评分模式，不再执行模型：

```bash
scripts/model.sh evaluate-hicache \
  --prediction-dir <prediction-output> \
  --profile-run-dir <target-profile-suite> \
  --output-dir <separate-score-output>
```

可重复传入 `--prediction-dir` 评分多个 base；所有选中预测完成后才读取 target，分组报告误差。此模式不接受模型/补采输入。
旧的“评分时重新预测”矩阵路径已删除。需要 oracle-cost 诊断时，预测阶段使用 `--diagnostics full` 保留操作详情，
再给上述评分命令显式增加 `--oracle-cost-replay`；可用 `--oracle-max-runs 1` 限制每组诊断一个格。
回放直接使用本次评分提取的 target 操作成本和原预测中的 base 观测，不再手工提供另一份成本文件。
回放另记费用与状态，不参与模型估计或覆盖正式预测结果。

## 可选 DAG 变换

NodeScale 是框架无关的可选 DAG 变换。它按顺序匹配节点名称子串，并缩放节点耗时；没有配置时默认关闭：

```json
{
  "node_scale": {
    "enabled": true,
    "rules": [
      {
        "name": "AscendCL@aclrtSynchronizeStream",
        "factor": 1.1
      }
    ]
  }
}
```

HiCache 是 SGLang 专属扩展；KTransformers 共享 profile manifest、source DAG 和 simulation，但不会被伪造为支持 HiCache。

## 常用检查

```bash
python3 -m ruff check scripts src/profiling
find configs -name '*.json' -print0 | xargs -0 -n1 jq empty
git diff --check
```

C++ TraceGraph 使用 modeling 容器构建：

```bash
scripts/run.sh modeling -- bash -lc \
  'cmake -S src/modeling/trace_graph -B build/modeling/trace_graph-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DTRACE_GRAPH_DEBUG=OFF && cmake --build build/modeling/trace_graph-release --target trace_graph -j2'
```

## 文档

- `docs/project_constraints.md`：不可违反的项目边界；
- `docs/profiling_development.md`：profiling 结构和采集合同；
- `docs/modeling_development.md`：DAG、HiCache 模型与 workflow；
- `docs/hicache_io_cost_model.md`：HiCache I/O/control 与 phase cost 的变量、参数和预测方式；
- `docs/validation/hicache_validation.md`：结构、HiCache I/O/control、phase cost、oracle 结果和已知限制；
- `docs/work_progress.md`：重构状态、当前数据资产和下一阶段。

`data/` 中的大部分内容是可再生运行资产。清理前按 `docs/work_progress.md` 的资产表确认正式 base、固定校准、物理校准、
prediction 与 score 资产；项目不使用内部版本号、文件摘要或冻结副本管理当前实现。
