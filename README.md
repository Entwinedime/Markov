# Markov Trace Simulation

本项目从真实推理运行中采集 trace，构建可仿真的 DAG，并对配置变化进行图变换和性能预测。

当前建模主线是 SGLang HiCache 的直接 I/O 与 control 变化：

```text
profile manifest
  -> source DAG
  -> target HiCache effect plan
  -> I/O cost plan
  -> DAG patch
  -> topological simulation
```

`gap`、`prefill` 和 `decode` 不属于当前 HiCache direct 模型，结果和评分必须单独列出这些 component。

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

主要动作是：

```text
build-dag
calibrate-hicache physical
calibrate-hicache runtime-dma
build-hicache-model
predict-hicache
evaluate-hicache
```

所有 modeling 动作都在同一个 modeling 容器中完成；批量预测不会为每个 cell 再启动一个嵌套容器。

SGLang 与 KTransformers 共用 framework-neutral DAG 入口：

```bash
scripts/model.sh build-dag \
  --profile-manifest <profile_manifest.json> \
  --output-dir <dag-output>
```

它只构图和仿真，不要求 HiCache。KTransformers manifest 显式记录 framework，当前提供 LD_PRELOAD CPU trace；
HiCache prediction 只接受 SGLang source，并会对其他 framework 给出 capability 错误。

正式预测只需要 source manifest、显式 target HiCache 配置和 one-base I/O model：

```bash
scripts/model.sh predict-hicache \
  --source-manifest <source/profile_manifest.json> \
  --target-config configs/modeling/hicache_target_example.json \
  --hicache-io-model <one-base-model.json> \
  --output-dir <prediction-output>
```

真实 target profile 不属于该命令。5×3/12/60-cell 和 target oracle 只通过 `evaluate-hicache` 进入评分流程。
oracle-cost 诊断在 `evaluate-hicache --diagnostics full` 后显式增加
`--oracle-scores <manifest.json>`。manifest 按 base 列出 base observation 和 target score-only 输入，
同一次 matrix 评分可覆盖多个 base；这些输入不会参与参数估计。

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
- `docs/hicache_io_cost_model.md`：I/O cost 的变量、参数和预测方式；
- `docs/validation/`：当前验证结果和已知限制。

`data/` 中的大部分内容是可再生运行资产。删除前必须核对 retention manifest，不能清理最终回归仍需使用的数据。
