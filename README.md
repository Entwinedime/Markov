# Markov Trace Simulation

Markov 从真实推理 trace 构建原始 DAG，推演配置变化后的工作与依赖，再绑定成本、仿真得到完整 HTTP 耗时。

```text
base 配置 + 模型 + workload → 采集 → 原始 DAG → target 工作与依赖 ─┐
独立性能采集 → 性能数据库 → 离线成本包 ──────────────────────────┴→ 执行预测
```

base 提供执行依据和已证明工作不变的成本，变化工作使用独立模型。预测不读取 target 真值；实验结束后才独立评分。完整 HTTP 保留 gap、未归属成本与 Prefill/Decode，未解释的源成本迁移会标明近似。

SGLang 是当前 HiCache 主线；KTransformers 共用 manifest、源 DAG 和仿真，不支持不存在的 HiCache 模块。NodeScale 可选、默认关闭。支持某个入口不等于任意平台、模型或工作量已验证。

## 从哪里开始

公开入口是 `scripts/profile.sh`、`scripts/model.sh`、`scripts/build.sh` 和 `scripts/run.sh`。模型运行和采集使用相应推理 Docker，C++ 建模、格式检查和仿真使用 modeling Docker。

已有适用成本包与 base 输入时，从 [组配置示例](configs/modeling/hicache_group_example.json) 修改路径、实际资源与 targets：

```bash
scripts/model.sh prepare-hicache --group <group.json> --dry-run
scripts/model.sh prepare-hicache --group <group.json>
```

尚未采集 base 时，组请求用 `profile_suite` 引用普通采集配置，并声明采集与同源 CPU 配对预算。平台维护者用 `prepare-costs --request <performance.json>` 单独准备可复用的成本包。dry-run 会写摘要但不采设备；首次准备与恢复步骤见下方操作文档。

只构建源 DAG 时，两个框架共用：

```bash
scripts/model.sh build-dag --profile-manifest <profile_manifest.json> --output-dir <dag-output>
```

准确性实验使用 `experiment --request <experiment.json>`，有限配置搜索使用 `search --request <search.json>`，两者共用统一预测器。特定 186 格复现实例位于 [examples/hicache_reproduction](examples/hicache_reproduction/README.md)，不是普通预测的必经流程。

## 文档入口

| 要了解的内容 | 文档 |
| --- | --- |
| 要提供什么，输出意味着什么 | [用户输入与结果合同](docs/design/user_contract.md) |
| 启动、准备、预测、恢复、评分与构建 | [开发与运行](docs/design/development.md) |
| 三阶段交接及代码阅读路线 | [架构与代码职责](docs/design/architecture.md) |
| trace 通道、探针、时钟与正式窗口 | [采集、观测与交接](docs/design/profiling.md) |
| 数据库、变量、系数、公式与外推范围 | [性能数据库与成本模型](docs/design/performance.md) |
| 代码、验证、文档和交付规则 | [工程与文档维护约束](docs/design/engineering_constraints.md) |
| 当前能力、历史结果与资产 | [项目工作流与进展](docs/workflow.md) |
| 时间与空间优化的恢复起点 | [窗口状态](docs/windows/2026-10-30_time_space_optimization/status.md) |
| 优化任务与执行证据 | [计划](docs/windows/2026-10-30_time_space_optimization/plan.md) · [日志](docs/windows/2026-10-30_time_space_optimization/log.md) |

静态设计、项目进展和窗口执行分开维护；代理开始工作先读 [AGENTS.md](AGENTS.md)。`data/` 与 `build/` 不提交，已有运行资产不能视为可随意删除的临时文件。其他仓库保持独立，`/home/markov-next` 仅供只读参考。
