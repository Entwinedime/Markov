# 项目约束

本文只维护跨阶段稳定的工程边界。具体接口、实现细节和运行方法分别维护在 profiling、modeling 与 validation 文档中；
实验过程记录在 `work_progress.md`。

## 文档与证据

- 顶级主线文档维护 profiling、modeling、项目约束和工作进展；专项验证与长期模型限制维护在 `docs/validation/`。
- 主线开发文档描述架构、职责和输入输出合同，不记录实验流水、临时诊断或具体结果。
- 当前验证口径、有效结论和已知限制维护在 `docs/validation/`。
- 尚未闭环的单一问题可以暂存于 `docs/tmp/`；完成后必须迁移稳定结论并删除临时文档。
- `docs/tmp/` 里的内容不能作为 active spec 引用；如果临时方案在实现后被证明错误或已回退，只能把“废弃原因”和“当前替代方案”
  迁移到主线文档，不能迁移旧合同细节。
- `data/` 下的运行产物是可再生证据，不是长期事实来源。需要长期保留的结论必须写入文档，并附可复现入口。
- 文档、配置和代码必须描述同一条 active workflow；行为变化时应同步更新，不保留相互矛盾的说明。
- active 源码目录不单独维护模块 README；模块职责和使用方法应进入对应主线文档。

## 运行环境

- 真实 SGLang profiling 必须在 `sglang-profile` runtime 容器中执行；framework hook 也必须在相同 ABI 环境中构建。
- C++ TraceGraph 构建、格式检查和 modeling 验证以独立的 `modeling` 容器为准；该环境不依赖 Ascend/CANN runtime。
- 宿主机只负责外层编排和无运行时依赖的静态检查，不作为 framework profiling 或 C++ 构建验收环境。
- Profiling 的宿主机入口是 `scripts/profile.sh`；post-profile modeling workflow 的用户入口是
  `python3 scripts/internal/entrypoints/modeling_workflow.py`。`scripts/model.sh` 只作为 workflow/诊断调用单次 runner 的低层
  container wrapper。
- `scripts/internal/entrypoints/profile.py` 和 `scripts/internal/entrypoints/model.py` 是容器内执行器，不作为真实任务的宿主机入口；
  `scripts/internal/entrypoints/modeling_workflow.py` 是 profiling 后 modeling validation / analysis 的直接入口。
- `scripts/internal/entrypoints/` 当前只保留 `profile.py`、`model.py` 和 `modeling_workflow.py` 三个主入口。可复用逻辑必须放在 `scripts/internal/markov_internal/` 下按
  `common`、`profiling`、`audit`、`contracts`、`modeling` 和 `modeling_workflow` 分层。
- CLI 解析可以保留在容器内 entrypoint 或包内 `main()` 边界；可复用业务函数应接收显式 typed option / path / config
  参数，不把 `argparse.Namespace` 继续向深层传递。
- `scripts/internal` 不保留旧平铺脚本、可继续执行旧逻辑的兼容入口或人工对照副本；旧逻辑若仍有长期价值，必须迁移到主线包或文档。
- 不为旧 `scripts/internal/*.py` 平铺入口保留兼容 wrapper；入口迁移后，主线文档和 shell wrapper 必须指向新 entrypoint。
  迁移完成后旧入口必须直接删除，不保留失败提示入口。
- `scripts/internal` 下不应保留 `__pycache__/`、临时 spike 输出或一次性审计脚本；需要长期保留的经验应迁移到主线文档。

## 采集边界

- Profiling 只采集事实，不预测 target 行为，也不执行建模规则。
- `markov_internal/profiling/` 只负责 config 展开、运行期环境、server/bench 生命周期、trace/artifact 写出和 forced-token
  replay/capture 注入；采集后的通用 artifact audit 归属 `audit`，HiCache readiness、workflow gate 和 validation 归属
  `modeling_workflow/validations/hicache`。
- 模型输入、诊断证据、时序观测和 validation oracle 必须显式分类，不能相互替代。
- 单个 probe target 是原子 fact 单元；target 中的所有 fields 必须整体属于同一个 `fact.class`、`fact.role`
  和 `fact.consumers`。不存在字段级 consumer 分流，也不得在同一 target 内混合“部分字段给模型、部分字段只给诊断”的语义。
- HiCache target catalog 保持单一 JSON，target 主字段顺序固定为 `id / module / target / events / fact / fields`；
  `emit_when` 只允许作为尾部可选谓词，用来抑制当前 SGLang 调用中没有 request 身份的噪声事件，不承担采集
  mode / enable 开关语义。
- 同一采集点如果确实需要产出不同语义的 fact，应拆成独立 target 或独立事件，并分别定义字段、consumer 和质量检查。
- 缺失关键事实时必须报告合同缺口，不得使用默认值、推测值或诊断字段补齐。
- 输入合同重构后只维护当前合同的 schema、required fields、consumer routing、phase 合法性和 readiness 检查；不为历史
  role、历史字段或旧 trace 增加专项兼容检测，也不输出新旧双轨 ready 状态。任何输入不满足当前合同，都按普通合同缺口、字段缺失或 route error 处理。
- 实验配置只描述 server、workload 和采集行为，不嵌入 modeling prediction 逻辑。
- `profiling.channels` 只允许 `torch`、`python_probe` 和 `ld_preload`；各渠道的细节必须由自身配置段控制。
- Python probe 的 native/runtime 不可见事件不得通过伪造 Python 事实补齐；需要时使用对应 runtime hook。
- Python probe 启用后，probe module 加载、target 安装和 catalog 解析错误必须让本次 profiling 失败；不能只写 debug 日志后继续运行。
- suite 共享同一套采集合同；server 和 workload 维度只能改变被测配置与输入，不能私自改变采集语义。
- `matrix.servers[]`、`matrix.inputs[]` 和展开后的 experiment 不得覆盖或删除顶层 `profiling`。
- suite preflight 必须在启动 server 前完成；运行失败时必须留下可审计的 suite result。
- 真实 profiling 统一通过 `scripts/profile.sh` 启动；容器内 runner 不作为宿主机运行入口。

## 缓存状态输入合同

- Cache-state 模型只消费 catalog 显式声明给 `hicache_state_model` 的 fact；每个 role 的可消费 phase 必须由当前合同和
  C++ router 明确声明。`cache_extend_input` 使用 start-phase，其它当前 workload identity fact 使用 end-phase。
- `hicache_profile_quality` 不作为采集 consumer；quality 审计只能解释当前 run 已按 input/final/transition consumer
  采到的事实，不能反向拉起额外 target。
- 可消费事件必须同时满足：

  ```text
  "hicache_state_model" in fact.consumers
  fact.class/fact.role 为 C++ router 已知组合
  phase 满足该 role 的当前合同
  ```

- 当前正常输入事实只包含 `workload_identity/cache_lookup_input`、`workload_identity/cache_extend_input`、
  `workload_identity/cache_lifecycle_commit` 和 `workload_identity/prefetch_candidate_anchor`。新增 state-model fact 必须同时更新
  schema、采集入口、质量审计和 C++ router。
- `drain_storage_control_queues()` 这种 source runtime scheduler boundary 不声明 profiling checkpoint；跨配置 release 时机必须由
  target-derived 模型逻辑近似，不能把 source scheduler round 重新作为 quality、transition 或 state-model 输入。
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

- Modeling 后端使用 C++ TraceGraph；Python 只负责配置生成、运行编排和 validation。profile manifest 中的 torch /
  LD_PRELOAD / Python probe trace 由 C++ 后端直接读取，并在进程内完成合流，不物化大型 merged trace。
- Active C++ TraceGraph public include 根为 `include/markov/trace_graph/...`，命名空间为
  `markov::trace_graph::...`；不维护旧 `include/trace_graph/...` 转发层、旧 `namespace TraceGraph` alias 或兼容 target。
- 不保留旧 C++ TraceGraph 对照目录；重构后的 active tree 必须直接位于 `src/modeling/trace_graph`。
- C++ 业务层 target 不依赖 diagnostics / validation target；diagnostics / validation 只消费业务层暴露的结构化结果。
- C++ diagnostics / validation / debug dump 的编译裁剪只使用单一 `DEBUG` 宏，由 CMake build type 或 `TRACE_GRAPH_DEBUG` 统一控制。
- Cache-state 主流程使用 `mode=cache_state`；HiCacheModule 维护业务状态，不修改性能 DAG；transition/policy/ref/capacity
  行级 trace 只在 Debug/validation backend 中保存。
- 状态子模块不得修改原始 profiling trace。
- DAG 修改必须通过统一 mutation 接口记录，不能以隐式副作用改变图结构。
- 非执行类 state snapshot、oracle 和质量事件不得成为默认 DAG 节点。
- Cache-state prediction 必须同时具有显式 target config 和满足合同的 profile 输入。
- target config 至少明确 page size、容量和 policy；byte projection 由 workflow 级模型几何生成，不得使用“observed”一类从 source 行为回填 target policy 的配置。
- HiCache I/O cost 只允许使用 effective bytes、device-host bandwidth 和 host-storage bandwidth；KV bytes 是模型几何，不能作为第三个 cost 参数。
- Workflow 只能通过统一的 `--hicache-io-model` 文件提供模型几何与两个 bandwidth，同一 invocation 不允许 target config 或单个 cell 覆盖这些值。
- HiCache I/O duration 不读取 source/target observed latency、E2E、oracle exactness 或 config/input/cell 专项修正；参数缺失时必须输出明确 blocker。
- Resource model 必须显式区分 host-storage、device-to-host 和 host-to-device 三条 lane；同 lane 顺序不能隐式依赖 simulator 行为。
- Target trace 只作为 oracle；cross-config prediction 不得把 target actual 行为作为模型事实源。
- 状态模型、策略推导、验证摘要和 DAG mutation 应保持清晰组件边界，避免重新形成单体状态机。
- 当前 target modeling config 由 workflow 根据 profile suite 动态生成，不作为手工长期配置维护。
- Unified modeling workflow 维护两层 modeling config：`model_runs/<model_run_id>/runner_config.json` 是 Python runner config，供
  `scripts/model.sh --config` 使用；同一 model run 输出目录下的 `cpp_model_config.json` 是 C++ TraceGraph backend narrow
  config。`runner_config.json` 必须自包含 manifest、output、backend、trace channel、oracle 和 narrow model config；
  `scripts/model.sh` 不再用额外 CLI 参数重复覆盖这些字段。两层 config 不能混称为同一种目标建模配置。
- Unified modeling workflow 必须使用统一 validation object、artifact policy、runner adapter 和 `WorkflowProgressReporter`；
  preflight、plan、model-runs、validations 业务模块不直接拥有终端进度输出。
- Base-DAG analysis、Final-DAG patch、HiCache final-state 和 HiCache transition 都是同一个 modeling workflow 的 validation
  object，不再维护独立 DAG workflow、cache patch workflow 或 HiCache state workflow 主入口。
- workflow 共享进度条、count/value 渲染等公共编排能力维护在
  `scripts/internal/markov_internal/modeling_workflow/progress.py`；具体 validation 只保留自身 stage metrics 和业务 summary。
- Workflow 用户第一入口是 `workflow_summary.json`、`preflight_summary.json`、`artifacts/model_runs_summary.json` 和
  `artifacts/validations/<name>/summary.json`；这些 summary 只保留阶段级计数和分组摘要，
  不嵌入 per-run / per-cell rows。per-run audit、per-cell prediction、transition catalog、gate、model log 和 debug trace
  属于 `artifacts/` 或 `model_runs/` 下的诊断/复现产物。
- `prediction.json` 中的 E2E 时间来自 TraceGraph 拓扑仿真，不是 cache-state 正确性的验收指标。
- module summary、validation、DAG trace 和 debug 输出必须由显式开关生成；Release backend 不链接 diagnostics/validation，
  不暴露 module summary 或 DAG analysis artifacts，也不保存 HiCache transition/policy/ref/capacity/radix/async 的行级 debug history。

## 验证门禁

- Validation 必须先检查采集质量和输入合同，再解释模型差异。
- Workflow 每次按当前代码重新审计 profile manifest，不使用旧质量报告绕过新门禁。
- Workflow quality 输出必须区分 `workflow_input_ready`、`state_model_input_ready` 和
  `strict_diagnostic_coverage_ready`：`state_model_input_ready` 只表示业务 state-model 输入合同，
  `workflow_input_ready` 才包含本次 workflow 显式请求的 validation evidence / forced-token cross-config gate；
  strict diagnostic coverage 是 source/timing 诊断覆盖率，不得混入 state-model gate。
- Validation/debug 开关必须裁剪执行路径；关闭时不能只是不写输出文件，相关 trace 遍历、oracle 构造、summary 填充和 debug
  collector 都不应执行。
- Common workflow 只允许 self prediction；cross prediction 必须证明 forced plan、bundle 和规范化 workload signature 一致。
- 业务 prediction 的 run 必须具备可解析的 state-model fact、token dictionary/span 和 target config；validation workflow 额外要求
  validation oracle。
- 正常 prediction 中只要消费了未声明给 `hicache_state_model` 的事实，就不能宣称 state-model fact validation 通过。
- 输入合同未通过时，结果只能归类为输入、投影或控制边界问题，不能直接归因于模型规则。
- Transition validation 必须复用同一次 workflow 的 cache-state model run、validation artifact 和 target oracle gate；不单独重跑
  C++ prediction。
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
  -> modeling workflow
       -> workflow_summary.json
       -> preflight_summary.json
       -> artifacts/{model_run_plan.json,preflight,model_runs_summary.json,validations}
       -> model_runs/<model_run_id>/{runner_config.json,execution.json,prediction.json,run_summary.json,...}
```

## 工程质量

- 主线使用 C++23，CMake 最低版本为 3.20。
- 所有项目文档必须使用中文维护；只保留必要的英文协议名、类型名、配置字段、路径、命令和原始事件名。
- 所有源码注释和 docstring 必须使用英文；CLI help、日志、错误信息、schema 名称和其它程序输出同样保持英文。项目文档仍按本节上一条约束使用中文维护。
- C++ 公共接口、模块合同、所有权和生命周期、状态机边界及不变量必须使用 Doxygen 风格英文注释。实现内注释只用于解释非显然的算法、顺序约束和失败边界。
- Python 模块、类、公共函数和复杂私有 helper 必须提供英文 docstring；参数、返回值、副作用、异常和跨层合同在无法从类型签名直接判断时必须明确说明。
- 注释应解释设计原因、不变量和边界，避免逐句复述代码、保留失效历史或用注释掩盖可以通过结构表达的复杂度。
- Python 和 C++ 都优先用清晰的对象、接口和小模块表达有生命周期、有共享状态或多阶段的流程；短小纯函数不强行类化。
- 单文件长度是可维护性信号，不设硬性数字门禁；文件持续膨胀时应优先按职责拆分，而不是用长函数、匿名 helper 或文件名前缀掩盖边界。
- 源码结构应体现业务层次：入口只做 CLI glue，编排层只做 plan/execute/summary，业务模型、诊断、validation 和 artifact writer
  必须分层。非业务逻辑代码不能大量穿插在主体业务状态机中。
- Debug、diagnostics、validation、summary 输出和临时审计都应由显式开关或独立模块隔离；业务主路径不应长期携带大段非业务输出逻辑。
- Python 代码统一使用 Ruff formatter，配置只维护在仓库根目录 `pyproject.toml`。格式化命令为
  `python3 -m ruff format .`，提交前检查命令为 `python3 -m ruff format --check .`。
- Ruff formatter 覆盖一方 Python 源码；`build/`、`data/`、`docs/tmp/` 和 `third_party/` 只包含可再生产物、临时文档或
  vendored 代码，不进入项目 Python 格式化范围。
- 每轮重构都必须同步审视删减面：删除失活代码、旧入口、旧字段、旧文档入口和临时对照脚本，不为当前主线保留向后兼容分支。
- `markov_internal/modeling` 保持通用 runner / trace input / C++ config 边界；HiCache-heavy validation、oracle、transition
  和 input contract 逻辑归属 `markov_internal/modeling_workflow/validations/hicache`，避免 generic modeling 包重新膨胀。
- `modeling_workflow/validations` 顶层只保留 validation registry 和公共模板；base DAG、final DAG、HiCache 等领域逻辑必须下沉到
  对应 validation 子包。
- HiCache validation 内部继续按多级子目录拆分职责，`core`、`input_contract`、`oracle`、`preflight`、`transition`
  等大域下必须继续使用 `signature` / `reporting` / `snapshot` / `diff` / `evidence` / `exactness` / `replay` 等语义子目录；
  不能依赖 `workflow_xxx.py`、`transition_xxx.py`、`oracle_xxx.py` 这类文件名前缀模拟分层。
- one-shot subprocess 执行、命令记录、stdout/stderr capture、log path 和失败 payload 应逐步收敛到公共 helper，避免
  profiling、modeling 和 workflow 各自维护不一致的命令执行风格。
- `scripts/internal` 不新增旧代码人工对照目录；重构时应直接删除失活入口，并把仍有效的经验迁移到主线文档。
- 不修改或覆盖用户未授权的运行产物和工作区改动。
- 不把 pycache、临时诊断或大体积 debug 产物纳入主线。
- 提交前至少完成与改动范围相符的 Ruff 格式检查、Python/Shell 语法、JSON 解析、diff whitespace、C++ 格式/构建和 workflow
  合同检查。
