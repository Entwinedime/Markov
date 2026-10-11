# Markov Trace Simulation

本项目从真实推理运行中采集 trace，构建可仿真的 DAG，并对配置变化进行图变换和性能预测。

当前建模主线是 SGLang HiCache 的 I/O/control 与 Prefill/Decode 变化：

```text
base profile -> 原始 DAG 与源观测
原始 DAG + target config + workload -> 目标操作、依赖和条件执行规则
目标工作 + 可迁移的 base 成本 + 独立成本模型 -> 执行与完整 HTTP E2E
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
将 `base_manifests` 和 `cost_model` 中的占位路径换成实际输入，声明实际资源环境，按需修改 target 配置及输出目录。然后先运行：

```bash
scripts/model.sh prepare-hicache --group <group_request.json> --dry-run
```

最小配置没有补采预算，不启动额外设备采集；检查可能从 base trace 构图并写出报告。
缺少依据时，按[组准备说明](docs/modeling_development.md#5-一个-base-组)声明已有校准或补采预算。
输入齐备后执行：

```bash
scripts/model.sh prepare-hicache --group <group_request.json>
```

prepare 使用已经离线训练的独立成本包组织组预测。base 提供原始图和工作确实未变化的对应成本，
不自动入库或在线拟合；变化或新增操作查询独立模型。历史测量通过 `cost-db import/train` 显式准备。
共享补采默认关闭，显式开启并提供累计预算后才执行；合法分支缺少成本时报告缺口，不填零或删掉操作。
新流程不保证任意组只靠 base 或现有数据库就能完成预测，部署适用性与数据覆盖须分别检查。
当前实验证据限定于已声明的框架、模型、TP、后端和请求条件，不宣称任意配置均受支持。
最新精度、结构覆盖及这轮精简的验证状态统一见[当前进展](docs/work_progress.md)。

先看输出目录的 `group_summary.json` 了解准备状态与停止原因，再按其中的预测结果路径查看
`workflow_summary.json`：`prediction.cells` 包含各格完整 HTTP 时间、执行状态和成本局限。
失败格时间为 null，不算作零。结果文件、补采预算及失败处理详见[建模使用说明](docs/modeling_development.md)。

有 target 实测时，再对已完成的预测独立评分：

```bash
scripts/model.sh evaluate \
  --prediction-dir <prediction-output> \
  --profile-run-dir <target-profile-suite> \
  --output-dir <separate-score-output>
```

预测不读取 target trace；评分不重新构模、补采或调参。完整 E2E 不排除 gap、未归属成本或计算阶段。
单独构模/预测、详细诊断及保留的历史静态 oracle-cost 回放用法统一见建模使用说明。

## 可选 DAG 变换

NodeScale 是框架无关的可选 DAG 变换。它按顺序匹配节点名称子串，并缩放节点耗时；没有配置时默认关闭。
在三阶段 HiCache 执行路径中，缩放作用于已绑定的最终节点耗时，也包含目标新生成的节点，
不修改原始 DAG 或回归参数。独立构图入口同样先使用校正后的有效成本，再缩放并绑定最终耗时，
不会把已经拆成依赖的同步阻塞墙钟重新放回节点。独立构图入口已用真实输入验证此衔接，完整目标预测仍待验收。
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
