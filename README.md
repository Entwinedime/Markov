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

预测本身由 `modeling/` 提供；`preparation/` 准备 Base、CPU 校正和独立性能依据。
准确性实验与配置搜索位于 `applications/`，共同调用同一个批量预测接口。
直接预测仍使用 `scripts/model.sh predict`，整组首次使用仍使用 `prepare-hicache`。
矩阵实验使用 `scripts/model.sh experiment --request <experiment.json>`；可用 `--predict-only`
只运行预测，或用 `--score-only` 为已有完整预测独立评分。
配置搜索使用 `scripts/model.sh search --request <search.json>`，与矩阵实验并列，不需要先运行矩阵。

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

### 从一个 base 预测一组配置

首次使用时，在普通采集配置中声明模型、base 启动参数及 workload，用组配置的 `profile_suite` 引用它，
再提供 `target_configs`、成本依据、输出目录及采集预算。只有一个 base 的配置不必重复填写 base/workload 选择，
不要求构造矩阵，也不要求先手工生成 manifest。完整例子见[首次准备](docs/modeling_development.md#首次准备)。
串行模板负载可先记录自然生成的实际 tokens，再让不同配置执行同一份工作量；这不预测尚未生成的内容或 EOS 位置。

已有采集可从 [复用组配置](configs/modeling/hicache_group_example.json) 开始：替换 `base_manifests` 和 `cost_model`
路径，声明实际资源环境，修改 target 及输出目录。两种方式使用同一入口：

```bash
scripts/model.sh prepare-hicache --group <group_request.json> --dry-run
scripts/model.sh prepare-hicache --group <group_request.json>
```

先读 dry-run 的缺口，再决定是否执行第二条命令。dry-run 会写准备摘要，但不启动设备采集、CPU 配对或预测。
未声明成本依据时，准备流程在采集 base 前就停止；显式 `--calibration-only` 仍可单独准备源输入。
复用示例没有补采预算；首次采集、同源 CPU 配对及独立成本补采分别声明预算，不相互借用。
缺数据或中断时，按[准备与恢复说明](docs/modeling_development.md#6-准备流程与失败处理)处理，保留已完成采集及累计用量，
不要删除账本重新开始。仅调大 profiler API 的等待上限不要求重采已完成输入；改变实际工作或采集通道则需重新匹配。

### 看结果，再独立验证

输出目录的 `group_summary.json` 直接给出准备状态、停止原因及 `prediction.cells` 中的各目标完整 HTTP 时间和成本局限；
需要查询来源或单格复现时，再打开其中引用的 `workflow_summary.json` 和运行配置。
失败格时间为 null，不算作零。配置排序是预测结果，不是已验证推荐；相对 base 的收益使用 self 预测作为参照，缺少时不伪造实测收益。

应用预测到这里即可结束。实验时，等全部选中预测完成，再用未开启 profiler 或计时探针的正常测量独立评分：

```bash
scripts/model.sh evaluate \
  --prediction-dir <prediction-output> \
  --normal-http \
  --profile-run-dir <normal-measurement-suite> \
  --output-dir <separate-score-output>
```

正常测量须同时覆盖 base 和 targets；重复样本按正式窗口取算术平均，不挑最好的一次。
预测不读取 target trace；评分不重新构模、补采或调参。完整 E2E 不排除 gap、未归属成本或计算阶段，
不能把命令成功当成精度达标。结构报告、带采集的对照和历史静态回放见[独立评分](docs/modeling_development.md#8-独立评分与历史回放)。

### 平台性能依据单独维护

base 提供原始图和工作确实未变的对应成本，不自动入库或在线拟合；变化或新增操作查询独立模型。
已有适用成本包时直接引用 `cost_model`；只有数据库时，用 `cost_database` 和 `cost_training` 代替，入口先离线训练再预测。
已有包优先、不自动重训，新包不覆盖共享输入包。

平台维护者使用 `scripts/model.sh prepare-costs --request <performance_request.json>` 组织独立采样、同源 CPU 校正、去重入库和训练，
不需要 target 组。历史测量也可显式导入；物理采集提供测量和来源，服务参数在离线训练时生成。
输入与模型声明见[独立性能准备](docs/modeling_development.md#独立性能准备)。
共享补采默认关闭，开启后仍需累计预算；缺成本不填零，也不删操作。
新平台、模型或执行分支需要各自适用的规则与性能依据，不能仅凭 base 或任意数据库保证可预测。
当前支持范围、精度及结构覆盖统一见[当前进展](docs/work_progress.md)。

### 在有限配置空间里搜索

从 [search_example.json](configs/modeling/search_example.json) 开始。`scenario` 引用同一套 Base/workload/成本准备声明，
不要求其中填写 target；`target_template` 固定不搜索的字段，`space` 列出可变的 HiCache 参数。
先看计划，再实际运行：

```bash
scripts/model.sh search --request <search.json> --dry-run
scripts/model.sh search --request <search.json>
```

能全部算完时使用枚举；预算较小时使用带显式种子的 `stratified_random`，先覆盖写入、预取和页大小分支。
搜索会自行调用统一预测模型，保留完整配置、预测时间、并列候选、缺成本与未评估范围。
搜索目标是完整 HTTP 正式窗口，不删除 gap 或计算阶段，也不会把带 profiler 的源墙钟当作正常 Base 时间。

已有结果中断后可用 `--resume` 只补未完成项，累计预算不重置。
前提是输入、代码和成本依据没有变化；改动它们后使用新的输出目录重新比较，不混用旧预测。
搜索期间不更新成本包；缺数据先回到独立性能维护，再开启新比较。
真实正常测量是单独、默认关闭的确认步骤，预测排名本身不是实测推荐。合同与限制见[搜索说明](docs/modeling_development.md#配置搜索)。

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

- [代码阅读与职责](docs/modeling_development.md#代码分工)：主流程、数据归属和源观测、目标执行的阅读顺序；
- `docs/project_constraints.md`：不可违反的项目边界；
- `docs/profiling_development.md`：profiling 结构和采集合同；
- `docs/modeling_development.md`：DAG、HiCache 模型与 workflow；
- `docs/hicache_io_cost_model.md`：HiCache I/O/control 与 phase cost 的变量、参数和预测方式；
- `docs/validation/hicache_validation.md`：结构、HiCache I/O/control、phase cost、oracle 结果和已知限制；
- `docs/work_progress.md`：重构状态、当前数据资产和下一阶段。

`data/` 中的大部分内容是可再生运行资产。清理前按 `docs/work_progress.md` 的资产表确认正式 base、固定校准、物理校准、
prediction 与 score 资产；项目不使用内部版本号、文件摘要或冻结副本管理当前实现。
