# Markov

项目已进入全量重构准备阶段。2026-10-27，旧实现、配置、环境及完整文档已移至
[`deprecated/`](deprecated/)，保留为设计和行为对照。新的实现尚未开始，根目录暂不提供采集、预测或搜索入口。

项目继续使用三阶段路线：base trace 构建原始 DAG，配置与执行规则生成目标工作和依赖，
独立成本模型与未变工作的源成本填入后执行，预测完整 HTTP。实验与资源成本搜索在上层共用这一预测器。

## 新文档从哪里读

- [整体架构](docs/design/architecture.md)：三阶段、共同合同、粒度与建议源码职责。
- [用户合同](docs/design/user_contract.md)：首次使用、性能维护、结果和错误；命令目前均为规划。
- [成本与数据库](docs/design/cost_models.md)：源成本迁移、独立测量、离线模型和补采边界。
- [验证方式](docs/design/validation.md)：三份报告、完整 HTTP、60/186 格和 10× 效率口径。
- [工程与文档约束](docs/design/engineering_constraints.md)：代码表达、维护边界及三层文档规则。
- [项目工作流](docs/workflow.md)：精简里程碑、当前判断和仍保留的资产。
- 本轮重构：[详细计划](docs/windows/2026-10-27_full_rebuild/plan.md)、[当前状态](docs/windows/2026-10-27_full_rebuild/status.md)、[执行日志](docs/windows/2026-10-27_full_rebuild/log.md)。

## 目录

| 路径 | 当前用途 |
| --- | --- |
| `deprecated/src/` | 旧采集器、Python probe、C++ hook、DAG 与预测后端 |
| `deprecated/scripts/` | 旧用户入口、准备、成本维护、预测、评分和搜索工作流 |
| `deprecated/examples/` | 旧复现和搜索研究脚本 |
| `deprecated/configs/` | 旧实验、workload、profiling 与 modeling 配置 |
| `deprecated/docker/` | 旧 SGLang、KTransformers 与 modeling 环境定义 |
| `deprecated/CMakeLists.txt`、`deprecated/.dockerignore` | 随旧实现保留的构建入口和 Docker 上下文规则 |
| `deprecated/docs/` | 完整旧文档及临时计划、日志，保留原内容与历史口径 |
| `docs/design/` | 新实现的静态设计与约束，不记录每轮进度 |
| `docs/workflow.md` | 项目级当前判断与精简里程碑 |
| `docs/windows/` | 同一任务维护一组 plan/status/log，任务、当前状态和历史事实分开 |
| `third_party/` | 原位保留 SGLang、KTransformers 子模块，未归档或删除上游依赖 |
| `data/`、`build/`、`ppts/` | 原位保留本地数据、构建产物与汇报资料；不提交生成物 |

`.clang-format`、`pyproject.toml` 与 `AGENTS.md` 留在根目录，作为后续开发的格式和工作约束。
默认 Ruff 检查跳过 `deprecated/`；显式检查归档代码时需使用相应路径和排除覆盖选项。

## 旧实现与实验

归档前的主仓库提交为 `8668eec`，已按采集、C++ 建模、Python 工作流、实验脚本和文档分层提交并推送。
SGLang 配套改动已推送，子模块提交为 `99e8be732`。

旧使用说明保留在 [`deprecated/README.md`](deprecated/README.md)。归档中的源码、配置和旧说明保留迁移前内容，
其中的命令与相对路径按原仓库布局理解；它们不是根目录当前可运行的入口。
需要运行旧流程时，使用归档前提交的独立 checkout，不将归档目录直接视为已适配的新运行根目录。

当前结论及资产位置见 [`docs/workflow.md`](docs/workflow.md)。旧搜索任务已终止，本次没有重新采集、
预测、评分或启动搜索，也没有移动、删除数据资产。

这次完成归档、文档重建与重构计划，尚未开始代码实现；不计作源码瘦身或预测提速成果。
