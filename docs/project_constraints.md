# 项目约束

本文只维护跨阶段稳定的工程边界。具体接口、实现细节和运行方法分别维护在 profiling、modeling 与 validation 文档中；
实验过程记录在 `work_progress.md`。

## 文档与证据

- 顶级主线文档维护 profiling、modeling、项目约束和工作进展；专项验证与长期模型限制维护在 `docs/validation/`。
- 主线开发文档描述架构、职责和输入输出合同，不记录实验流水、临时诊断或具体结果。
- 当前验证口径、有效结论和已知限制维护在 `docs/validation/`。
- 尚未闭环的单一问题可以暂存于 `docs/tmp/`；完成后必须迁移稳定结论并删除临时文档。
- `data/` 下的运行产物是可再生证据，不是长期事实来源。需要长期保留的结论必须写入文档，并附可复现入口。
- 文档、配置和代码必须描述同一条 active workflow；行为变化时应同步更新，不保留相互矛盾的说明。
- active 源码目录不单独维护模块 README；模块职责和使用方法应进入对应主线文档。

## 运行环境

- 真实 SGLang profiling 必须在 `sglang-profile` runtime 容器中执行；framework hook 也必须在相同 ABI 环境中构建。
- C++ TraceGraph 构建、格式检查和 modeling 验证以独立的 `modeling` 容器为准；该环境不依赖 Ascend/CANN runtime。
- 宿主机只负责外层编排和无运行时依赖的静态检查，不作为 framework profiling 或 C++ 构建验收环境。
- Profiling 的宿主机入口是 `scripts/profile.sh`；modeling 的宿主机入口是 `scripts/model.sh`。
- `scripts/internal/entrypoints/profile.py` 和 `scripts/internal/entrypoints/model.py` 是容器内执行器，不作为真实任务的宿主机入口。

## 采集边界

- Profiling 只采集事实，不预测 target 行为，也不执行建模规则。
- 模型输入、诊断证据、时序观测和 validation oracle 必须显式分类，不能相互替代。
- 缺失关键事实时必须报告合同缺口，不得使用默认值、推测值或诊断字段补齐。
- 实验配置只描述 server、workload 和采集行为，不嵌入 modeling prediction 逻辑。
- `profiling.channels` 只允许 `torch`、`python_probe` 和 `ld_preload`；各渠道的细节必须由自身配置段控制。
- Python probe 的 native/runtime 不可见事件不得通过伪造 Python 事实补齐；需要时使用对应 runtime hook。
- suite 共享同一套采集合同；server 和 workload 维度只能改变被测配置与输入，不能私自改变采集语义。
- `matrix.servers[]`、`matrix.inputs[]` 和展开后的 experiment 不得覆盖或删除顶层 `profiling`。
- suite preflight 必须在启动 server 前完成；运行失败时必须留下可审计的 suite result。
- 真实 profiling 统一通过 `scripts/profile.sh` 启动；容器内 runner 不作为宿主机运行入口。

## 缓存状态输入合同

- Cache-state 模型只消费 catalog 显式声明给 `hicache_state_model` 的 completed/end-phase fact。
- 可消费事件必须同时满足：

  ```text
  phase == "end"
  "hicache_state_model" in fact.consumers
  fact.class/fact.role 为 C++ router 已知组合
  ```

- 当前正常输入事实是 `workload_identity/request_bound_match_anchor`、`workload_identity/request_lifecycle_anchor`、
  `workload_identity/request_admission`、`target_policy_input/prefetch_decision` 和
  `runtime_model_checkpoint/prefetch_check_point`。新增事实必须同时更新 target catalog、质量审计和 C++ router。
- 所有正常输入必须具有稳定的 `cache_scope` 与单调 `seq_no`；未知角色、缺字段和非法路由必须进入质量错误。
- source run 已发生的命中、移动、淘汰、异步完成和状态快照只能用于诊断或验证，不能更新 target state。
- `source_actual`、`timing_observation`、`oracle_state` 和 debug/provenance 数据不得声明给 `hicache_state_model` consumer。
- token path 与 range 是跨配置输入身份的基础；page state 必须由 token 事实和 target 配置推导。
- token span 必须能解析到同一输入合同中的 token dictionary，不能依赖诊断事件补齐。
- 单次运行内的临时标识不能直接作为跨配置事实。跨配置比较必须使用规范化后的稳定签名。
- target policy 和资源行为必须由显式 target config 与建模规则决定，不能读取 source 结果作为答案。

## 普通 / Forced 工作流

- Common suite 用于普通生成、采集诊断和 self-config prediction。
- Cross-config prediction 必须使用 forced-token replay 或强度等价的输入合同。
- Forced capture 产物不可覆盖，并由 suite 聚合为可移动、可校验的 bundle。
- Capture plan 使用 `trace_sim.hicache.forced_token_plan.v1`，bundle 使用
  `trace_sim.hicache.forced_token_bundle.v1`；bundle 内 plan path 必须相对 bundle 保存。
- Forced replay 必须显式接收 bundle；禁止固定 plan、自动选择历史 capture 或无 bundle 回退。
- Replay 启动前必须校验 bundle schema/id/hash、selected input 覆盖、plan path/hash、workload id/fingerprint、request count
  和 logical request 顺序。
- Replay 完成后必须逐请求验证实际输出与 plan 一致，并分别报告 plan contract、bundle contract 和总 readiness。
- 同一 input 的 cross-config gate 必须同时满足 plan signature、bundle signature 和规范化 workload signature 一致。
- bundle provenance 只参与输入合同审计，不进入 C++ state model。

## 建模边界

- Modeling 后端使用 C++ TraceGraph；Python 只负责配置生成、运行编排、trace 合并和 validation。
- Cache-state 主流程使用 `mode=cache_state`；HiCacheModule 维护状态和 transition trace，不修改性能 DAG。
- 状态子模块不得修改原始 profiling trace。
- DAG 修改必须通过统一 mutation 接口记录，不能以隐式副作用改变图结构。
- 非执行类 state snapshot、oracle 和质量事件不得成为默认 DAG 节点。
- Cache-state prediction 必须同时具有显式 target config 和满足合同的 profile 输入。
- target config 至少明确 page projection、容量和 policy；不得使用“observed”一类从 source 行为回填 target policy 的配置。
- Target trace 只作为 oracle；cross-config prediction 不得把 target actual 行为作为模型事实源。
- 状态模型、策略推导、验证摘要和 DAG mutation 应保持清晰组件边界，避免重新形成单体状态机。
- 当前 target modeling config 由 workflow 根据 profile suite 动态生成，不作为手工长期配置维护。
- `prediction.json` 中的 E2E 时间来自 TraceGraph 拓扑仿真，不是 cache-state 正确性的验收指标。
- module summary、validation、DAG trace 和 debug 输出必须由显式开关生成。

## 验证门禁

- Validation 必须先检查采集质量和输入合同，再解释模型差异。
- Workflow 每次按当前代码重新审计 profile manifest，不使用旧质量报告绕过新门禁。
- Common workflow 只允许 self prediction；cross prediction 必须证明 forced plan、bundle 和规范化 workload signature 一致。
- 每个参与 prediction 的 run 必须具备可解析的不变量事实、token dictionary/span 和 validation oracle。
- 正常 prediction 中只要消费了未声明给 `hicache_state_model` 的事实，就不能宣称 state-model fact validation 通过。
- 输入合同未通过时，结果只能归类为输入、投影或控制边界问题，不能直接归因于模型规则。
- Transition validation 必须建立在同一次 workflow 的 final-state 门禁上。
- Oracle、debug 和质量事件不能进入默认性能 DAG。

## 配置与产物

`configs/experiments/hicache_state/` 只维护当前 cache-state 开发所需的三套 profiling 配置：

- `profiling_hicache_state_common.json`
- `profiling_hicache_state_forced_capture.json`
- `profiling_hicache_state_forced_replay.json`

Forced-token plan、bundle、动态 modeling config 和 validation 输出均属于运行产物，应写入对应的 `data/` 目录。
配置目录不保存运行结果或由历史运行导出的固定输入。

运行产物至少应保持以下层级：

```text
profile suite
  -> suite selection/result
  -> per-run manifest/trace/workload report
  -> optional forced-token bundle
  -> workflow quality/final-state/transition outputs
```

## 工程质量

- 主线使用 C++23，CMake 最低版本为 3.20。
- 所有项目文档必须使用中文维护；只保留必要的英文协议名、类型名、配置字段、路径、命令和原始事件名。
- 代码和解释性注释以中文为主，保留必要的英文协议名、类型名和配置字段，此外，所有的“输出性内容”，比如 CLI help，需要保持英文。
- C++ 公共接口、状态机边界和不变量说明使用 Doxygen 风格；Python 使用模块、类和函数 docstring 表达同类信息。
- 注释应解释设计原因、不变量和边界，避免复述代码。
- 修改行为后应删除失活代码和旧入口，不为当前主线保留向后兼容分支。
- 不修改或覆盖用户未授权的运行产物和工作区改动。
- 不把 pycache、临时诊断或大体积 debug 产物纳入主线。
- 提交前至少完成与改动范围相符的 Python/Shell 语法、JSON 解析、diff whitespace、C++ 格式/构建和 workflow
  合同检查。
