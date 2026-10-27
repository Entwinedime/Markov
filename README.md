# Markov

项目正在全量重构。旧实现、配置、环境和文档在本地 deprecated 中保留，并已保存到历史提交。
当前提供输入预览、采集 manifest 检查与 modeling 构建；原始 DAG、正式预测、采集和搜索正在按阶段恢复。

项目继续使用三阶段路线：base trace 构建原始 DAG，配置与执行规则生成目标工作和依赖，
独立成本模型与未变工作的源成本填入后执行，预测完整 HTTP。实验与资源成本搜索在上层共用这一预测器。

## 先运行输入预览

```bash
scripts/markov.sh plan --request configs/prediction.json
```

预览只核对声明输入并列出准备动作，不启动容器或设备，也不创建预测结果。
请求中的相对路径以请求文件目录为起点；示例 base 需要采集且未提供独立模型，输出会明确这些缺口。
源语义、目标结构和成本覆盖必须由后续正式阶段分析，预览成功不表示能够预测。

构建和检查已有采集输入：

```bash
docker build -t markov-modeling:ubuntu24.04 -f docker/modeling/Dockerfile .
scripts/build.sh Release
scripts/run.sh modeling -- build/modeling/Release/markov_core inspect-manifest /path/to/profile_manifest.json
```

manifest 检查核对声明通道文件和访问路径，不加载 trace 内容或构建 DAG。
modeling 开发容器无设备和网络，默认内存上限 4 GiB；通过 `MARKOV_MODELING_MEMORY` 显式调整。

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
| 本地 `deprecated/src/` | 旧采集器、Python probe、C++ hook、DAG 与预测后端 |
| `deprecated/scripts/` | 旧用户入口、准备、成本维护、预测、评分和搜索工作流 |
| `deprecated/examples/` | 旧复现和搜索研究脚本 |
| `deprecated/configs/` | 旧实验、workload、profiling 与 modeling 配置 |
| `deprecated/docker/` | 旧 SGLang、KTransformers 与 modeling 环境定义 |
| `deprecated/CMakeLists.txt`、`deprecated/.dockerignore` | 随旧实现保留的构建入口和 Docker 上下文规则 |
| `deprecated/docs/` | 完整旧文档及临时计划、日志，保留原内容与历史口径 |
| `python/markov/` | 用户输入合同与任务编排 |
| `src/markov/` | C++ 来源输入、后续 DAG/结构/成本/执行核心 |
| `scripts/`、`docker/` | 薄入口及独立建模环境 |
| `configs/` | 可直接预览的最小请求与 workload |
| `docs/design/` | 新实现的静态设计与约束，不记录每轮进度 |
| `docs/workflow.md` | 项目级当前判断与精简里程碑 |
| `docs/windows/` | 同一任务维护一组 plan/status/log，任务、当前状态和历史事实分开 |
| `third_party/` | 原位保留 SGLang、KTransformers 子模块，未归档或删除上游依赖 |
| `data/`、`build/`、`ppts/` | 原位保留本地数据、构建产物与汇报资料；不提交生成物 |

`.clang-format`、`pyproject.toml` 与 `AGENTS.md` 留在根目录，作为后续开发的格式和工作约束。
默认 Ruff 检查跳过 `deprecated/`；显式检查归档代码时需使用相应路径和排除覆盖选项。

## 旧实现与实验

归档前的主仓库提交为 `8668eec`，已按采集、C++ 建模、Python 工作流、实验脚本和文档分层提交并推送。
SGLang 配套改动已推送，子模块提交为 `99e8be732`。完整旧文档的历史归档提交为 `824b458`。

旧使用说明保留在本地 deprecated/README.md。参考目录不再跟踪；完整内容可由归档提交恢复。
归档中的源码、配置和旧说明保留迁移前内容，
其中的命令与相对路径按原仓库布局理解；它们不是根目录当前可运行的入口。
需要运行旧流程时，使用归档前提交的独立 checkout，不将归档目录直接视为已适配的新运行根目录。

当前结论及资产位置见 [`docs/workflow.md`](docs/workflow.md)。旧搜索任务已终止，本次没有重新采集、
预测、评分或启动搜索，也没有移动、删除数据资产。

归档不计作源码瘦身，输入检查不计作预测恢复；新实现的精度和速度仍需后续实际验证。
