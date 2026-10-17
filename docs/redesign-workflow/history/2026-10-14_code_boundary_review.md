# 2026-10-14：代码层次、职责边界与清理审查

> 历史审查/观测记录：结论限定于文中日期与当时实现。2026-10-16 清空数据后，旧 `data/redesign` 地址不再可复核；本文的“当前”“待完成”不作为现在的任务或授权状态。实时状态见[主计划](../2026-10-08_redesign_execution_plan.md)。

本页保留此前边界整改的历史依据；当前完整审查与新增整改统一见[已完成能力审查](2026-10-14_completed_capabilities_review.md)。本页不维护当前状态。

依据：[工程约束](../../redesign/engineering_constraints.md)、[整体框架](../../redesign/01_system_framework.md)、[采集合同](../../redesign/04_collection_templates.md)、[性能合同](../../redesign/06_performance_execution.md)及[实施计划](../2026-10-08_redesign_execution_plan.md)。本轮由用户单独要求，S3–S6 goal 继续暂停；功能状态不在本报告另建进度表。

## 范围与审查方法

覆盖活动的 `src/markov`、`src/collection`，以及 CMake、公开 Shell 入口、Docker 装配引用。全体 C++/Python 做依赖、符号消费者和格式扫描；人工沿配置输入、框架绑定、模板生产/变换、控制器、执行图、成本查询和采集/恢复主链审阅。扫描命中逐项核对真实用途，没有按文件大小判定坏代码，也没有宣称逐行证明全部原生行为。

既有未提交的采样和 attention 工作纳入当前实现审阅；保留原始数据与来源。验证复用固定资产，预声明新增原生运行 0 次、派生报告上限 32 MiB，关闭大图诊断。构建使用新的 Docker Release 目录，未新增 test 工程。

## 已整改的发现

### 1. 成本查询包含整个控制动作与模板实现类型（P2，职责耦合）

原 `performance/service_work` 持有完整 `InvokeStep`、来源 manifest/span/rank，并依赖模板中的发布参数类型。性能层自行从批次解析准入成员，还负责生成整阶段诊断；更换模板组织方式容易牵动模型。

工作值迁入 [contracts/service_work](../../../src/markov/include/markov/contracts/service_work.hpp)，发布成员数据随值合同维护，参数化算法继续在 `templates`。[ExecutionGraph](../../../src/markov/src/execution/execution_graph.cpp) 解析目标成员量，模型只查询得到的工作描述。来源键单独交给 `NodeCosts` 的给定值路径；整批诊断上下文留在执行缺口中。没有新增控制状态或第二套节点描述，旧路径完整删除，CMake 和全部消费者同步更新。

公共物理/语义值仍复用 `SourceKernel` 等图合同；本轮消除的是性能对完整控制动作及模板参数化实现的依赖，不以复制所有 kernel 字段宣称完全独立。

### 2. 实现路径关系以成本专用接口暴露（P2，边界表达不清）

原 `supports_cost_configuration` 实际只判断原生控制路径和配置关系，并不执行性能预测。新增独立成本适配器会重复已有框架解释。

统一为 [TemplateAdapter::shares_execution_path](../../../src/markov/include/markov/adapters/template_adapter.hpp)，同时用于模板来源选择、代表配置分组和成本候选准入。框架适配器解释原生配置；workflow 持有部署条件及候选；模型匹配目标特征。支持域仍由原有 `SGLangBindings` 与模板规则决定，路径相容不构成成本精度或迁移证明。

### 3. 采集装配和产物生命周期有重复所有者（P2，可维护性）

原 workflow 遇到 performance 就直接构造 SGLang 会话，再由该会话重复检查权限/框架并独立发布 Ascend 产物。workflow 本身还继承原生会话，持有并不使用的失败状态。

[NativeCapture](../../../src/collection/markov_collection/workflow/capture.py) 改为显式组合：在原生进程入口检查权限、选择框架/用途会话、记录日志起点并发布 profiler 产物。SGLang 会话负责执行与自身测量记录，CPU 路径输出线程服务样本。KT 保留结构采集能力；独立通信入口继续拥有自己的采集生命周期。没有引入动态注册器或新 KV/请求状态机。

### 4. 无消费者、重复入口和过期标记（P2，冗余）

- 删除无消费者的 `SourceKernel::has_semantic_work`；将当前成本模型的语义适用条件收回 `KernelCostModel`，删除图对象上的成本专用谓词。
- 删除 runtime normalizer 的另一套 CLI 分发。正式处理入口为 `workflow.processing`，统一拥有处理锁、结果与失败发布。
- 删除 `CollectionWorkflow._save` 的纯转发方法，18 个调用直接使用既有原子 JSON 发布函数；`NativeAttempt._save` 封装自身状态，保留其实际职责。
- 删除逐工作点重复恢复 SIGQUIT 的代码。固定启动预热已处理原生 tokenizer 首次建立事件循环时的信号覆盖；依据为当前原生 `auto_create_handle_loop` 的一次性初始化逻辑。
- 删除 CPU 产物中已经过期的 `target_operation_matching_not_implemented`。未测观测开销和未完成的测量资格继续保留。

### 5. 当前文档与资源交接滞后（P2，事实不一致）

计划仍写“goal 已恢复”，开发说明对自动代表建库存在互相冲突的当前描述。已更新暂停状态，稳定职责回到设计/开发文档，阶段状态集中在计划。

前一轮 attention 特征工作在暂停前生成了 607174546 bytes 派生产物，超出预声明 536870912 bytes，超额 70303634 bytes。大图诊断是主要新增来源。已补记该组和累计账本，额度未事后抬高，原文件保留；这不是本轮新增采集，也不是轻量性通过。具体交接见执行记录。

## 保留的边界与后续风险

- `workflow → adapters → core` 构建方向保持；公共模板消费者、控制器、执行和性能没有新增框架名判断。Python 公共 instrumentation/artifacts 不导入适配器，runtime 不反向导入框架。
- 模板拓扑修改仍先暂存再提交；运行时持有实例和唯一模拟时钟，控制器管理业务/KV 状态，workspace 容量由执行层推进。查询借用的工作数据覆盖当前调度调用生命周期，没有复制第二份状态。
- `templates.cpp` 和 Ascend normalizer 较大。已有规则规划、图变换和事件关联职责确实存在，本轮没有为行数目标拆类；后续触碰具体职责时继续整理重复数据流。
- 测量扰动、CPU/通信成本适用性、目标 attention 成本实际消费及跨 shape 精度仍是功能/证据问题。本轮不通过补偿常数、补采或新拒绝规则处理它们。
- Python 原生会话的发布归属变更完成静态及禁采入口检查；本轮不重新启动模型，实际原生生命周期回归随下一次必要采样验证，不把禁采检查描述成真实采集通过。

## 验证与停止条件

证据目录：`data/redesign/2026-10-14-code-review`。新目录 Docker Release 构建通过；全部 131 个核心 C++ 文件的 clang-format 检查、46 个 Python 文件的 Ruff 检查及格式检查通过。活动 Shell 语法与 diff 检查通过，`.clang-format` 未修改。

四条公开预测路径分别为混合成本、纯给定耗时、缺成本和既有不支持输入。改前/改后业务报告完全相同，只排除明确的宿主模板加载墙钟字段；模型指标、目标工作、缺口与来源字段均参与比较。既有 kernel 数据经公开 `fit-costs` 生成的候选与改前完全一致。这些都是开发回归，不是新的独立精度证据。

SGLang/KT 公开 plan、禁采 profile/status 和原生入口权限边界均通过，实际 attempt 为 0。已检查 121 个本地文件链接和 48 个 Markdown 锚点。派生报告及输入副本共 2871650 bytes，在预声明 32 MiB 内，已记入累计账本；详情见证据目录的 `verification.json` 和 `accounting.json`。

本轮以确认问题完成整改、必要构建及公开入口回归通过为停止条件。具体新增能力和待验证限制继续由实施计划管理；本报告不扩展 S3–S6 验收门槛。
