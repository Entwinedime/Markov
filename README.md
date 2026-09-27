# Markov Trace Simulation

本项目从真实推理运行中采集 trace，构建可仿真的 DAG，并对配置变化进行图变换和性能预测。

当前建模主线是 SGLang HiCache 的 I/O/control 与 Prefill/Decode 变化：

```text
base profile + target config + workload
  -> source DAG 与 base 成本依据
  -> 按执行规则生成 target 操作和依赖，估计成本
  -> DAG 执行与完整 HTTP 时间预测
```

HiCache I/O/control（内部 scope ID 为 `hicache_direct`）、Prefill 和 Decode 分别建模。默认预测不排除 gap 或未归属成本；仍沿用 base 的残余等待等近似，会随结果说明。执行完成不等于精度达标，当前执行式预测的分项归属评分尚不完整。

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

只需构建 source DAG 和仿真时，SGLang 与 KTransformers 共用入口：

```bash
scripts/model.sh build-dag \
  --profile-manifest <profile_manifest.json> \
  --output-dir <dag-output>
```

该入口不要求 HiCache。以下 HiCache 预测流程只适用于 SGLang。

从 [最小组配置](configs/modeling/hicache_group_example.json) 开始：复制为自己的配置文件，
将 `base_manifests` 中的占位路径换成实际 manifest，按需修改 target 配置及输出目录。然后先运行：

```bash
scripts/model.sh prepare-hicache --group <group_request.json> --dry-run
```

最小配置没有补采预算，不启动额外设备采集；检查可能从 base trace 构图并写出报告。
缺少依据时，按[组准备说明](docs/modeling_development.md#5-一个-base-组)声明已有校准或补采预算。
输入齐备后执行：

```bash
scripts/model.sh prepare-hicache --group <group_request.json>
```

prepare 统一完成成本检查、必要的共享补采、构模和组预测，不需要手工串联内部阶段。
成本优先来自 base 实测及有依据的外推，不足才使用共享独立校准。首次准备自动读取 base 部署的模型配置信息，
不必声明物理采样或先采完整参数包；宿主机缺少模型配置时，会提示提供配置文件或适用的已有校准报告。
未具备的 I/O 服务成本单独报告，目标执行确实用到时才触发共享补采。
CPU 控制和计算参数仍有构模前置要求，自动补采尚未覆盖全部控制分支。
不能保证任意组只靠 base 就能预测。

先看输出目录的 `group_summary.json` 了解准备状态与停止原因，再按其中的预测结果路径查看
`workflow_summary.json`：`prediction.cells` 包含各格完整 HTTP 时间、执行状态和成本局限。
失败格时间为 null，不算作零。结果文件、补采预算及失败处理详见[建模使用说明](docs/modeling_development.md)。

有 target 实测时，再对已完成的预测独立评分：

```bash
scripts/model.sh evaluate-hicache \
  --prediction-dir <prediction-output> \
  --profile-run-dir <target-profile-suite> \
  --output-dir <separate-score-output>
```

预测不读取 target trace；评分不重新构模、补采或调参。完整 E2E 不排除 gap、未归属成本或计算阶段。
单独构模/预测、详细诊断及保留的历史静态 oracle-cost 回放用法统一见建模使用说明。

## 可选 DAG 变换

NodeScale 是框架无关的可选 DAG 变换。它按顺序匹配节点名称子串，并缩放节点耗时；没有配置时默认关闭。
将以下内容保存为模型配置，并在 `build-dag` 命令中追加 `--model-config <model.json>`：

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
