# 开发与运行

所有命令从仓库根目录运行，尖括号表示需要替换的路径。公开入口负责容器调度，不要求用户串联内部 Python module 或 C++ binary。输入与结果的含义集中在[用户合同](user_contract.md)，这里维护操作顺序、恢复和开发检查。

## 环境与入口

| 环境 | 职责 |
| --- | --- |
| SGLang | 模型运行、Torch/Python/原生 hook 采集、HiCache 独立采样 |
| KTransformers | 模型运行与专用 hook；共享 manifest、源 DAG 和仿真 |
| modeling | C++ TraceGraph、Python 成本维护、预测及独立评分 |

```bash
scripts/build.sh modeling
scripts/build.sh sglang
scripts/build.sh ktransformers

scripts/run.sh modeling
scripts/model.sh --help
```

只构建需要的环境；真实采集还须有模型权重、驱动、设备和匹配 hook。`scripts/internal/hooks/build.sh <framework>` 在对应推理容器构建 hook，不在 modeling 容器编译原生 SDK 依赖。

| 需求 | 命令 |
| --- | --- |
| 采集普通实验或 suite | `scripts/profile.sh <experiment.json>` |
| 一个 base 预测一组 target | `scripts/model.sh prepare-hicache --group <group.json>` |
| 独立性能准备 | `scripts/model.sh prepare-costs --request <performance.json>` |
| 通用原始 DAG | `scripts/model.sh build-dag` |
| 已准备输入直接预测 | `scripts/model.sh predict` |
| 数据库维护 | `scripts/model.sh cost-db import/train/inspect/migrate` |
| 准确性实验 | `scripts/model.sh experiment --request <experiment.json>` |
| 有限配置搜索 | `scripts/model.sh search --request <search.json>` |
| 已完成结果独立评分 | `scripts/model.sh evaluate` |

## 先采集，再构建原始 DAG

```bash
scripts/profile.sh <experiment.json> --dry-run
scripts/profile.sh <experiment.json>

scripts/model.sh build-dag \
  --profile-manifest <profile_manifest.json> \
  --output-dir <dag-output>
```

dry-run 展开输入和命令，不启动服务；成功也不证明采集或结构正确。`build-dag` 同时用于两个推理框架，不要求 HiCache。KTransformers 配置例子为 [profiling_dag_smoke.json](../../configs/experiments/ktransformers/profiling_dag_smoke.json)。

有生成的运行配置时，可用 `build-dag --config <runner_config.json>` 原样复现；此时不能再混用直接构图的输出、模型或线程选项。普通构图默认不额外提取 HiCache 控制成本或执行分项诊断，未输出的指标不是零。

NodeScale 可选且默认关闭。将下列内容保存为模型配置，并在直接构图命令中添加 `--model-config <model.json>`：

```json
{
  "node_scale": {
    "enabled": true,
    "rules": [{"name": "AscendCL@aclrtSynchronizeStream", "factor": 1.1}]
  }
}
```

规则按节点名子串顺序匹配，在成本绑定后作用于最终耗时；不能用它调节 target 残差并称为独立模型预测。

## 准备一个 base 组

已有输入使用 [hicache_group_example.json](../../configs/modeling/hicache_group_example.json)，修改 manifest、成本包、资源、targets 和输出目录。首次采集改用 `profile_suite` 引用普通采集配置，并给出 base 与 CPU 配对预算，例如：

```json
{
  "profile_suite": "<base-experiment.json>",
  "target_configs": ["configs/modeling/hicache_target_example.json"],
  "cost_model": "<platform-cost-package.json>",
  "base_capture_budget": {
    "wall_seconds": 1200, "server_starts": 2, "requests": 100, "tokens": 100000
  },
  "cpu_service_capture_budget": {
    "wall_seconds": 600, "server_starts": 1, "requests": 100, "tokens": 100000
  },
  "output_dir": "data/modeling_runs/my_hicache_group"
}
```

数字只是示例；实际资源还须按部署补齐并与成本包一致。模型路径在普通采集配置声明，不在组里另存一份。多个 base 时用 `base_config` 选择，可用 `workload_ids` 收窄工作负载。首次准备的模板请求没有 token 计划时，先自然记录，再受控回放采集 base。

```bash
scripts/model.sh prepare-hicache --group <group.json> --dry-run
scripts/model.sh prepare-hicache --group <group.json>
```

先读准备摘要的缺口。dry-run 会写摘要，但不采设备、不做 CPU 配对或预测；`--calibration-only` 只准备源输入，可省略 targets。缺成本依据的普通准备会在采集 base 前停止。

完整采集与轻量 CPU 配对必须来自同一命令、请求和 token 工作量；轻量侧只改变采集方式。自动安排仍受独立 CPU 预算限制。已有配对可单独处理：

```bash
scripts/model.sh prepare-cpu-service \
  --profile-manifest <full/profile_manifest.json> \
  --light-manifest <light/profile_manifest.json> \
  --tp-size <n> --correct-recorder --output-dir <cpu-output>
```

只有已证明的采集 CPU 开销才可校正，不扣设备工作、所有 wall−CPU 差或未解释的 gap。改变实际工作和采集通道后重新匹配；仅调高 profiler API 等待上限不自动使完整输入失效。

### 中断与恢复

检查 `group_summary.json`、准备摘要、预算账本及失败日志，确认实际进程/容器是否已结束。`running` 字段、路径存在或 manifest completed 都不是运行仍在继续或数据完整的证明。

同输入可复用已完成的采集及 CPU 配对，累计用量保留；不要删账本或覆盖原始报告。改变输入、提取含义、规则或成本时，使用新输出目录并重新预测，不把旧结果当缓存。缺结构、缺变量、缺模型、缺测量及资源失败分别处理，单纯补数据不能解决结构问题。

共享独立补采默认关闭。显式开启时，先从已有库刷新适用模型，再为实际测量缺口选择整组共享、有预算的采样；成功后统一重跑该组，不按逐格误差调参。不同预算不得相互借用，失败和重试也计入累计消耗。

## 独立维护性能依据

维护者使用一个请求组织采样、校正、入库和训练，不需要 target 组：

```bash
scripts/model.sh prepare-costs --request <performance.json> --dry-run
scripts/model.sh prepare-costs --request <performance.json>
```

必需字段为 `database`、`training`、`output_dir`。已有数据时只训练；新采样增加 `profile_suite` 与 `capture_budget`。历史采集用 `profile_manifests`，已有测量用 `observations` 或 `evidence`，CPU 依据用 `cpu_service_costs` / `cpu_service_pairs` 及相应预算。

自动 runtime 采样目前限 SGLang 模板负载，独立采集开启形状记录，CPU 服务仍需同源轻量依据。物理 I/O 使用 `physical_calibration` 或显式 `physical_capture`，声明页域、设备、CPU 放置、预算与 `physical_services`；不声明就不启动物理采样。现有物理采样器限 Ascend/page-first/file 路径。

结果在 `performance_summary.json`，导出的 `cost_model.json` 可复用。ready 只对应声明模型；dry-run 不采集、入库或训练。独立模型的变量、公式、时间口径与外推规则见[性能模型](performance.md)。

### 导入、替换、训练

```bash
scripts/model.sh cost-db import \
  --database <measurements.sqlite> \
  --profile-manifest <independent/profile_manifest.json> \
  --cpu-service-cost <same-capture-cpu_service.json> \
  --resource-environment <resources.json> --output-dir <import-output>

scripts/model.sh cost-db train \
  --database <measurements.sqlite> --request <training.json> \
  --output <cost-package.json>

scripts/model.sh cost-db inspect \
  --database <measurements.sqlite> --output <coverage.json>
```

没有 CPU 校正就省略该参数，样本保留 profiled 口径，不能冒充正常服务。物理和控制测量经 `--evidence` 导入；原始声明文件缺失要停止，不从旧系数反造样本。

重复导入按原采集身份与测量位置去重；已知重命名副本通过 `--capture-aliases` 指明身份。修正提取规则时，`--replace-captures` 在事务中**完整替换所选采集的派生观测**，失败回滚；它不是局部追加开关，提交部分数据会移除该采集未提交的旧样本。替换后显式重训，已有包不会自动改变。

旧库需要转换时使用 `cost-db migrate --database <old.sqlite> --output <new.sqlite> --summary <migration.json>`。旧库只读，输出必须不存在；普通导入和训练不暗中转换。物理证据转换仍可能需要原测量文件，缺失则失败，不覆盖旧库。维护和训练顺序进行，不假定有跨进程在线更新协议。

## 直接预测与并发

```bash
scripts/model.sh predict \
  --source-manifest <base/profile_manifest.json> \
  --target-config <target.json> \
  --cost-model <cost-package.json> \
  --resource-environment <resources.json> \
  --cpu-service-cost <same-source-cpu_service.json> \
  --output-dir <prediction-output>
```

CPU 参数可省略，source、target 和 CPU 参数可重复传入。直接预测默认失败后不启动新目标，已启动任务收尾；`--continue-on-error` 继续其他目标。组准备则执行整组以汇总共享缺口，不使用这个开关。

`--trace-threads` 控制源读取/构图，`--trace-file-threads` 控制文件解析，默认各为 1；`--model-run-jobs` 独立控制目标并发。同源目标共享一次源准备，每个目标仍持有大型可变图，不能按源图大小估算整个运行峰值。

modeling 容器默认内存及内存加交换区上限为 320 GiB，可用 `TRACE_SIM_MODELING_MEMORY_LIMIT` 按平台配置。宿主还有其他任务时必须预留余量；硬上限不能替代并发预算及进程树回收。设备采集始终串行，不与重型导出或建模争抢同一资源。

完整 HTTP 读取 `http_client.e2e_us`；`simulated_e2e_us` / `full_graph_e2e_us` 可能含请求后的后台工作，不能混用。默认 `--diagnostics off` 走 Release，保留紧凑摘要、复现输入与失败日志；`--diagnostics full` 走 validation，增加明细和成功日志。实验需要额外源回放与结构报告时使用 `--experiment-reports`。

`performance` 保存真实程序的阶段墙钟、CPU 及内存，不是模拟成本。嵌套阶段不能相加，PSS/私有页是快照，进程 RSS 峰值不能直接相加当整组峰值；整组峰值取独立容器的 cgroup 记录。排查成本查询时可用 `TRACE_GRAPH_QUERY_METRICS=1 scripts/model.sh predict ...`，额外记录查询计时与不同工作点，默认关闭，以免统计本身影响效率比较。

## 实验、搜索与评分

配置例子为 [accuracy_example.json](../../configs/modeling/accuracy_example.json) 和 [search_example.json](../../configs/modeling/search_example.json)。实验支持 `--predict-only` / `--score-only`，搜索支持 `--dry-run` / `--resume`：

```bash
scripts/model.sh experiment --request <experiment.json> --predict-only
scripts/model.sh search --request <search.json> --dry-run
scripts/model.sh search --request <search.json>

scripts/model.sh evaluate \
  --prediction-dir <completed-predictions> --normal-http \
  --profile-run-dir <normal-measurement-suite> --output-dir <separate-score-output>
```

搜索 `scenario` 引用同一套源准备声明，`target_template` 固定不搜索字段，`space` 列出有限取值。`enumerate` 遍历空间，`stratified_random` 先覆盖机制分支，`random` 为均匀随机参照；`local` 在已有预测上探索联合邻域，`coordinate` 只改一个坐标。除枚举外均声明整数种子；这些方法不读取真值或在线拟合成本。资源变化时声明同资源参照，不能把增加容量的收益全归为策略改进。

`--resume` 仅在代码、输入和成本依据不变时补未完成项，累计预算不重置。搜索期间不更新成本包；真实正常确认默认关闭，是独立步骤。特定研究案例的并发数、watchdog 阈值和时限不作为通用推荐。

评分也可用重复的 `--target-profile-manifest` 精确指定正常输入，需同时覆盖 base 与 targets。正式窗口重复样本取算术平均，保留样本数、范围和失败；选中格未完成时不先打开真值。`--retry-prediction-dir` 只替换相同身份的未完成格，不能挑选更好的一次成功预测。

不加 `--normal-http` 时使用 target profile 口径；带采集对照不等于正常精度。结构比较使用匹配操作间的可达关系，稀疏中间节点不是新操作。默认只报告计数，full 才列端点，仍不证明所有未匹配工作正确。

每格 APE 为 `abs(predicted-real)/real`，WAPE 为 `sum(abs(predicted-real))/sum(real)`；P90 将 APE 排序后取第 `ceil(0.9×格数)` 个值。缺格不从总范围里删除。

当前正式 HTTP gate 要求覆盖完整、WAPE ≤3%、P90 APE ≤5%，cross 还须优于直接沿用 base 正常墙钟的参照。额外实验判据可事先声明，但不会自动改变 evaluator 的门槛；`EXECUTED` 不能替代精度判定。完整 E2E 的 gate 也不表示所有组件精度或单格误差都达标。

### 静态成本回放

validation runner 的 `cpp_trace_graph.hicache_static_replay: true` 使用现行规则生成静态对照；Release 不接受该选项。相容资产可在独立评分中显式用 `--oracle-cost-replay`，先做相同成本恒等回放，再做五种 target 成本替换。`--oracle-max-runs 1` 表示每个 base 选一个格，不是仅运行一个变体。

需完整操作映射、静态控制标量及诊断输入；不相容则 `NOT_RUN`，不恢复旧聚合规则。现行执行式预测没有这套完整映射，不能仅开启 full 获得静态回放。含无源坐标重建 gap 的静态输入不能叠加源 CPU 校正；未校正诊断须从头使用同一口径，不能中途删校正绕过检查。回放不改结构或参数，也不是正常 HTTP 成绩。

## 开发检查

按实际修改范围使用 Ruff、`bash -n`、`jq empty` 与 `git diff --check`。不新增独立测试文件，也不为文档改动启动模型。C++ 格式检查和构建均在 modeling Docker 中进行：

```bash
scripts/run.sh modeling -- clang-format --dry-run --Werror <changed.cpp> <changed.hpp>

scripts/run.sh modeling -- bash -lc \
  'cmake -S src/modeling/trace_graph -B build/modeling/trace_graph-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DTRACE_GRAPH_DEBUG=OFF && cmake --build build/modeling/trace_graph-release --target trace_graph -j2'
```

修改结构、时序或诊断时，按风险再配置 `build/modeling/trace_graph-validation`，使用 Debug 与 `TRACE_GRAPH_DEBUG=ON`，构建正式 `trace_graph` target。阶段统一检查整个职责链，先代表输入，固定规则后再完整矩阵；不逐文件构建，也不把所有验证拖到项目结束。
