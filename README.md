# Markov Trace Simulation

本项目从真实推理运行中采集 trace，构建可仿真的 DAG，并对配置变化进行图变换和性能预测。

当前建模主线是 SGLang HiCache 的 I/O/control 与 Prefill/Decode 变化：

```text
用户提供平台、model、base config、workload 和 target config 集合
  ├─ 采集 base → 原始 DAG → 目标工作与依赖规则 ─┐
  └─ 独立性能采集 → 性能数据库 → 成本模型 ──────┴─ 填成本、仿真 → HTTP E2E
```

base 主线解释执行结构，并迁移明确未变化的操作成本；性能主线长期积累独立测量，
分别维护计算、I/O 和控制等成本模型。模型消费目标 DAG 的工作描述，供整组 target 及后续预测复用，
不依附于某次 base 采集，也不要求采集每个 target。预测本身不读取 target trace。
两条主线是职责分工，不是并行占用设备。超时等条件由同一执行时钟展开，不另做一套时间推算。

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

首次使用时，在普通采集配置中声明模型、base 启动参数及 workload，用组配置的 `profile_suite` 引用它，
再提供 `target_configs`、成本依据、输出目录及采集预算。只有一个 base 的配置不必重复填写 base/workload 选择，
不要求构造矩阵，也不要求先手工生成 manifest。完整例子见[首次准备](docs/modeling_development.md#首次准备)。

已有采集可从 [复用组配置](configs/modeling/hicache_group_example.json) 开始：替换 `base_manifests` 和 `cost_model`
路径，声明实际资源环境，修改 target 及输出目录。两种方式使用同一入口：

```bash
scripts/model.sh prepare-hicache --group <group_request.json> --dry-run
```

复用示例没有补采预算，不启动额外设备采集；检查可能从 base trace 构图并写出报告。
缺少依据时，按[组准备说明](docs/modeling_development.md#5-一个-base-组)声明已有校准或补采预算。
输入齐备后执行：

```bash
scripts/model.sh prepare-hicache --group <group_request.json>
```

prepare 使用独立成本包组织组预测；也可用 `cost_database` 和 `cost_training` 代替 `cost_model`，
由入口先从已有独立数据离线生成组成本包。已有成本包优先，不自动重训；恢复产生的新包留在组输出，不覆盖共享输入包。
base 提供原始图和工作确实未变化的对应成本，不自动入库或在线拟合；变化或新增操作查询独立模型。
历史测量通过 `cost-db import` 显式入库，模型定义和适用范围仍须明确声明。
平台性能维护使用 `scripts/model.sh prepare-costs --request <performance_request.json>`：
声明独立采样配置或已有测量，入口负责串行采集、同源 CPU 校正、去重入库和离线训练，不需要 target 组。
具体输入见[独立性能准备](docs/modeling_development.md#独立性能准备)。已有适用包可直接交给应用预测。
物理采集只发布测量与来源，成本库不保存其旧拟合参数；服务响应在离线训练时生成。
新平台没有适用成本数据时，须声明独立采样域和模型定义；入口不凭空推断未知框架、操作或所有变量组合。
共享补采默认关闭，显式开启并提供累计预算后才执行；合法分支缺少成本时报告缺口，不填零或删掉操作。
新流程不保证任意组只靠 base 或现有数据库就能完成预测，部署适用性与数据覆盖须分别检查。
当前实验证据限定于已声明的框架、模型、TP、后端和请求条件，不宣称任意配置均受支持。
最新精度、结构覆盖及这轮精简的验证状态统一见[当前进展](docs/work_progress.md)。

输出目录的 `group_summary.json` 直接给出准备状态、停止原因及 `prediction.cells` 中的各目标完整 HTTP 时间和成本局限；
需要查询来源或单格复现时，再打开其中引用的 `workflow_summary.json` 和运行配置。
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
