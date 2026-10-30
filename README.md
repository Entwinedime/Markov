# Markov

Markov 用一份 base trace 解释执行依据，根据 target 配置生成工作与依赖，再绑定成本预测完整 HTTP 耗时。
独立性能数据由另一条共享流程维护；预测不读取 target 实测。

```text
base trace → 源语义区域 → target 工作、依赖与条件程序 ─┐
独立性能采集 → SQLite → 离线成本模型 ────────────────┴→ 执行 → HTTP 预测
```

当前可通过正式入口完成限定范围的 SGLang/Ascend 预测，已运行 self 和两种写策略 cross。
这证明调用链已接通，精度仍未达标；其他框架、自动成本准备、矩阵实验和搜索尚在恢复。

## 怎样运行

先构建不需要设备的 modeling 环境和程序：

```bash
docker build -t markov-modeling:ubuntu24.04 -f docker/modeling/Dockerfile .
scripts/build.sh Release
scripts/markov.sh plan --request configs/prediction.json
```

plan 只预览输入与准备动作，不启动设备。示例尚未指定本机模型、镜像、设备和独立模型，不能直接预测。
请求中的相对路径以请求文件目录为起点。

已准备 base manifest 和适用成本包后：

```bash
scripts/markov.sh predict --request /path/to/request.json
```

请求提供 deployment、workload、base.manifest、targets、performance.models，以及 execution.output 和
execution.kernel_cache_initial_state（empty 或 prepared）。结果在 output/prediction/report.json，
包含完整正式 HTTP、逐请求耗时、后台完成、缺口、成本来源、近似、图规模和运行开销。
同一会话只准备一次 source；每个 target 独立维护缓存和执行状态。
预测容器只挂载指定程序、base、模型和输出，不挂载整个仓库、真实设备或正常评分数据。

首次使用目前仍需分步准备，尚非一键自动流程：

```bash
scripts/markov.sh capture --request /path/to/base_request.json
scripts/markov.sh calibrate --request /path/to/independent_calibration_request.json
scripts/markov.sh maintain --request /path/to/maintenance.json
```

capture 用 base.configuration、实际推理镜像、设备与累计预算采集，返回 manifest；
calibrate 用另一个小型 workload 记录语义区域，不开 profiler 或 snapshot；maintain 显式导入、选择训练并导出模型。
HiCache 物理 I/O 另有独立采样，普通用户的整组需求分析和自动组合是后续工作。
正常对照单独运行 measure，关闭 profiler 与 Markov 探针；它只用于独立评价，不用于预测或训练。

## 已验证范围与限制

当前实际输入为 Qwen3-32B/BF16/TP2、Ascend 910B3、SGLang eager/FIA，串行固定输出。
预测需使用与 base 一致的 input_ids、ignore_eos 和输出数量；暂不接受文本 prompt 直接预测。
目标可声明页大小、设备容量和 HiCache 策略等受支持设置；有规则不等于成本覆盖，域外工作明确停止。
CPU 返回、流进度和结果可读分别保留；gap、未归属控制、Prefill/Decode 不从完整 HTTP 中扣除。
首次使用条件、源控制残余与竞争仍有近似，执行成功不表示结构完整或正常精度已验收。

细粒度诊断按需运行，不是普通预测的必经步骤：

```bash
MARKOV_MODELING_MEMORY=8g scripts/run.sh modeling -- \
  build/modeling/Release/markov_core source /path/to/profile_manifest.json /path/to/source_report.json
```

原生页几何/缓存子链用 page-work，独立模型查询用 costs；字段和范围见用户合同。
SGLang 与 KTransformers 保持同级设计，HiCache 只属于 SGLang；KTransformers 真采集/预测和默认关闭的 NodeScale 尚待恢复。

## 文档与目录

- [架构](docs/design/architecture.md)、[用户合同](docs/design/user_contract.md)、[成本与数据库](docs/design/cost_models.md)
- [验证口径](docs/design/validation.md)、[工程约束](docs/design/engineering_constraints.md)
- [项目工作流](docs/workflow.md)、本轮[计划](docs/windows/2026-10-27_full_rebuild/plan.md)、[状态](docs/windows/2026-10-27_full_rebuild/status.md)、[日志](docs/windows/2026-10-27_full_rebuild/log.md)

Python 的 workflow 组织正式任务，artifacts 定义输入，adapters 处理框架/runtime 差异；
C++ 的 trace_graph 维护源依据、目标规则、成本查询与共同执行器。scripts 保持薄入口，examples 承担研究案例。
C++ 头源在同职责目录，src 为 include 根；原生 SDK 桥在推理 Docker 编译，核心在 modeling Docker 编译。

本仓库位于 /home/markov-rebuild，与老项目 /home/markov 独立；/home/markov-next 仅供只读参考。
deprecated 只供历史对照，活动代码不调用。data 和 build 不提交；本项目重新采集数据，不复制或链接老项目资产。
