# Markov

Markov 用一份 base trace 解释执行依据，根据 target 配置生成工作与依赖，再绑定成本预测完整 HTTP 耗时。
独立性能数据由另一条共享流程维护；预测不读取 target 实测。

```text
base trace → 源语义区域 → target 工作、依赖与条件程序 ─┐
独立性能采集 → SQLite → 离线成本模型 ────────────────┴→ 执行 → HTTP 预测
```

当前正式入口已运行限定范围的 SGLang 主机/文件回访，以及 KTransformers dense 的 self、分块 Prefill 和 SDPA。
这证明两框架的完整请求预测已接通，整体精度与泛化尚未验收；矩阵实验和搜索仍在恢复。

## 怎样运行

先构建不需要设备的 modeling 环境和程序：

```bash
docker build -t markov-modeling:ubuntu24.04 -f docker/modeling/Dockerfile .
scripts/build.sh Release
scripts/markov.sh plan --request configs/prediction.json
```

plan 只预览输入与准备动作，不启动设备。先按实际部署修改示例中的模型位置、镜像、设备和预算；
预览不会核验这些资源能否真正启动。
请求中的相对路径以请求文件目录为起点。

同一个请求可以采集 base 并预测，重复运行会复用相同输入的 base：

```bash
scripts/markov.sh predict --request /path/to/request.json
```

请求提供 deployment、workload、base、targets、performance.database（或显式 performance.models），以及 execution.output 和
execution.kernel_cache_initial_state（empty 或 prepared）。结果在 output/prediction/report.json，
包含完整正式 HTTP、逐请求耗时、后台完成、缺口、成本来源、近似、图规模和运行开销。
同一会话只准备一次 source；每个 target 独立维护缓存和执行状态。
预测容器只挂载指定程序、base、模型和输出，不挂载整个仓库、真实设备或正常评分数据。

首次建立性能库时，声明 performance.database、performance.sampling_budget 和 execution.capture。
流程先使用现有模型，按整组缺少的工作生成小型独立采样，维护模型后继续预测；不会采集 target trace 或按误差补参数。
当前自动采样覆盖 SGLang/Ascend 串行区域响应、批量备份、整批回载、失败回载控制、文件预取控制/查询及 buffered 读写 worker。
文件预取保留发起返回、最终检查、停止快照和接受完成的边界；后台可在检查期间继续发布，停止后固定接受页数。
这些闭合响应与等待回合使用独立采样，其他前台控制和竞争仍有近似，不能据此认证策略排名。
采样与 base 分别累计预算；中断后重跑同一命令会接管原容器、保留用量，完成任务直接复用。

平台维护者也可显式维护独立数据：

```bash
scripts/markov.sh capture --request /path/to/base_request.json
scripts/markov.sh calibrate --request /path/to/independent_calibration_request.json
scripts/markov.sh maintain --request /path/to/maintenance.json
```

capture 用 base.configuration、实际推理镜像、设备与累计预算采集，返回 manifest；
calibrate 用另一个小型 workload 记录语义区域，不开 profiler 或 snapshot；maintain 显式导入、选择训练并发布库中的现行模型。
predict 从数据库导出本组的只读选择，用户无需手拼模型文件；未选中的历史记录不会自动参加训练。
正常对照单独运行 measure，关闭 profiler 与 Markov 探针；它只用于独立评价，不用于预测或训练。

## 已验证范围与限制

当前实际输入为 Qwen3-32B/BF16/TP2、Ascend 910B3、SGLang eager/FIA，串行固定输出。
预测需使用与 base 一致的输入、ignore_eos 和输出数量。文本使用采集时记录的实际 token，不在预测容器加载 tokenizer。
需要在准备结束后清空后台工作时，workload 设置 drain_before_formal=true；SGLang 以轻量状态等待 HiCache 空闲，预测保留同一控制语义。
目标可声明页大小、设备容量和 HiCache 策略等受支持设置；有规则不等于成本覆盖，域外工作明确停止。
CPU 返回、流进度和结果可读分别保留；gap、未归属控制、Prefill/Decode 不从完整 HTTP 中扣除。
首次使用条件、源控制残余与竞争仍有近似，执行成功不表示结构完整或正常精度已验收。

细粒度诊断按需运行，不是普通预测的必经步骤：

```bash
MARKOV_MODELING_MEMORY=8g scripts/run.sh modeling -- \
  build/modeling/Release/markov_core source /path/to/profile_manifest.json /path/to/source_report.json
```

原生页几何/缓存子链用 page-work，独立模型查询用 costs；字段和范围见用户合同。
可选 execution.node_scale 按已绑定工作的名字做成本敏感性分析，默认关闭，不代表真实硬件提速。
显式诊断运行 `scripts/markov.sh replay --request /path/to/replay.json`，可选源图，或打开 execution.record_graph 后保存的目标执行图。
回放可替换明确节点成本、缩放已有工作；它固定已有路径，不重新选择时间分支，也不自动匹配 target 真实成本。
独立目标诊断用 `scripts/markov.sh evaluate --request configs/target_evaluation.json`，需要已记录的预测图和另采的 target manifest。
SGLang target 采集加 `--record-regions`，KTransformers 默认记录同步阶段；评价器单独编译，不进入预测/训练流程。
报告列出真实成本替换范围、未对应工作和可观察的 CPU 顺序；带 profiler 的回放不能代替正常精度。
SGLang 与 KTransformers 共用采集任务、预算、manifest、源图回放与性能库，HiCache 只属于 SGLang。
KTransformers 单 NPU dense/Torch 入口已接通真实来源、独立阶段/控制成本、共享准备及完整 HTTP 预测。
分块规则生成额外阶段，沿用共同执行器；仅支持 empty 初始化，HTTP 外层残余及首次使用仍有近似。
采集示例为 [ktransformers_capture.json](configs/ktransformers_capture.json)，修改模型、设备和预算后可直接 predict，或单独执行 capture、measure、calibrate。

## 文档与目录

- [架构](docs/design/architecture.md)、[用户合同](docs/design/user_contract.md)、[成本与数据库](docs/design/cost_models.md)
- [验证口径](docs/design/validation.md)、[工程约束](docs/design/engineering_constraints.md)
- [项目工作流](docs/workflow.md)、本轮[计划](docs/windows/2026-10-27_full_rebuild/plan.md)、[状态](docs/windows/2026-10-27_full_rebuild/status.md)、[日志](docs/windows/2026-10-27_full_rebuild/log.md)

Python 的 workflow 组织正式任务，artifacts 定义输入，adapters 处理框架/runtime 差异；
C++ 的 trace_graph 维护源依据、目标规则、成本查询与共同执行器。scripts 保持薄入口，examples 承担研究案例。
C++ 头源在同职责目录，src 为 include 根；原生 SDK 桥在推理 Docker 编译，核心在 modeling Docker 编译。

本仓库位于 /home/markov-rebuild，与老项目 /home/markov 独立；/home/markov-next 仅供只读参考。
deprecated 只供历史对照，活动代码不调用。data 和 build 不提交；本项目重新采集数据，不复制或链接老项目资产。
