# SGLang 模板关系与原生机制索引

本页解释维护边界，并指向按需查阅的原生依据；不维护任务状态或逐次实验数字。原 983 行推导笔记已移入[历史机制记录](../redesign-workflow/history/2026-10-16_sglang_mechanism_notes.md)，其中的旧数据地址及“尚未”结论只代表当时状态。当前支持域以[主计划](../redesign-workflow/2026-10-08_redesign_execution_plan.md)为准，公共生产合同见[采集与模板](04_collection_templates.md)。

## 原生机制如何进入模板

框架适配解释函数、模块及请求语义；runtime 适配解释设备任务、传输、时钟和环境补丁；公共生产过程关联子图、参数与端口。目标执行只消费编译后的关系和状态效果，不再识别原生函数或 kernel 名称。

一个关系能计算 shape，并不证明整个子图在目标配置下成立。变换还须满足结构分支、依赖、资源、跨界端口和目标状态条件。语义切口与性能单元分开：一个区域可以含多个收费单元，融合工作不能因多个语义标签重复收费。

| 变更对象 | 维护位置 | 需要共同核对 |
| --- | --- | --- |
| 函数位置、请求参数、模型模块 | SGLang 适配及 `models/` | wrapper、来源身份、参数绑定 |
| CANN / torch_npu / HCCL 行为 | Ascend runtime 与安装资源 | 真实环境、原生记录、时钟与任务关联 |
| 子图参数化及有限变换 | 生产端绑定与公共模板合同 | 源条件、目标分支、边界和资源 |
| 目标请求、KV、workspace 状态 | 对应公共状态所有者 | 动作生效点、完成反馈，不复制原生状态机 |
| 节点性能 | 独立采样与成本合同 | 工作特征一致、计费边界及适用域 |

## 通信工作量与物理分支

逻辑载荷、物理任务和完成依赖分别表达。相同 collective 名称或算法标签不能证明任务拓扑相同；消息大小变化可能改变分块、通知和同步，不能统一按字节比例缩放全部节点。

绑定依据包含部署、communicator 配置、原生分支及逐 rank 任务身份。跨 rank 代次由真实因果顺序关联，不按时间戳、编号大小或“唯一已知组”猜测。完成点覆盖所有物理成员，包络不再额外收费。

详细推导见[通信分支](../redesign-workflow/history/2026-10-16_sglang_mechanism_notes.md#通信工作量与物理分支的已有证据)、[完成点](../redesign-workflow/history/2026-10-16_sglang_mechanism_notes.md#通信完成点与内部依赖)和[跨 rank 替换](../redesign-workflow/history/2026-10-16_sglang_mechanism_notes.md#hccl-分支替换的跨-rank-边界)。历史局部核对不自动授予跨 TP 或 backend 的资格。

## 输入解析与CPU上传的显式区域

输入 token、metadata 与输出复制要同时有语义位置和物理传输身份。框架适配解释数据用途及目标长度；runtime 适配关联 DMA、同步 host 复制和 driver 工作。复制字节数不能由包络时间或同名操作推测。

host 控制类 CPU 服务时间的排除不删除真实同步传输，也不删除提交、等待和发布依赖。计时范围只在[性能合同](06_performance_execution.md)维护。原始切口与身份依据见[输入上传记录](../redesign-workflow/history/2026-10-16_sglang_mechanism_notes.md#输入解析与cpu上传的显式区域)。

## 等待与资源

等待对象及作用范围必须显式保留。来源 stream 恰好为空，不表示目标同一等待没有前驱；目标需按已提交工作连接依赖，不能连到尚未发生的工作。局部替换保留跨界输入、完成点和外部消费者，不凭区域时间包含关系增添全局屏障。

操作子图及切口依据见[完整操作边界](../redesign-workflow/history/2026-10-16_sglang_mechanism_notes.md#完整操作边界与有限替换端口)；公共实例化规则见[控制与语义](05_controller_semantics.md)。

## Workspace 容量状态与设备级同步

workspace 与 KV 页池是不同资源。统一预热提供有来源的初始容量，目标操作提出需求，唯一状态所有者决定复用、分配或扩容及相应同步。不能按“第一次 Decode”、批人数或地址是否改变替代容量判断，也不能无条件继承供体的初值。

需求公式与 allocator 行为来自对应环境的生产适配；只有初态迁移条件成立时才可复用来源。缺观测不填空缓存。统一起点见[用户合同](02_user_contract.md#仿真起点统一预热后开始)，安装边界见[torch_npu 适配说明](../../src/collection/adapters/runtimes/ascend/torch_npu/README.md)，细节见[workspace 历史推导](../redesign-workflow/history/2026-10-16_sglang_mechanism_notes.md#workspace-容量状态与设备级同步)。

## 批人数变换的整体边界

请求池容量变化、实际批人数变化和 token 长度变化是不同关系。跨人数变换须共同覆盖请求级循环、共享输出、过滤/合并、KV 分配释放、模型物化以及全部 rank 的端口，不能只截断请求或修改张量轴。

有限来源可通过同构复用、参数化及有依据的分支选择组织目标；未知分支返回具体缺口。来源中每个机制分别出现，不保证它们可拼成一个合格的目标批次。完整条件与历史验证范围见[人数变换](../redesign-workflow/history/2026-10-16_sglang_mechanism_notes.md#批人数变换的整体边界)和[长度域](../redesign-workflow/history/2026-10-16_sglang_mechanism_notes.md#冷-prefill-的每成员长度域资格)。

## Decode 首批复用的结构分支

过滤、合并及从 Prefill 转入 Decode 以实际成员身份、左右批次来源和原生区域解释。合并不必只出现在下一批 Decode；目标控制器按当前请求状态组织动作，不能复制 base 的调用次序。

首批复用可能涉及位置、KV 与物化分支，局部节点可替换不等于整批已取得资格。观测位置和分支推导见[Decode 与 Schedule 合并记录](../redesign-workflow/history/2026-10-16_sglang_mechanism_notes.md#decode-首批复用的结构分支)，来源字段见[采集合同](04_collection_templates.md#批次过滤事实与调度模板条件)。

## 主线 Qwen3 模型结构与参数化落点

Prefill/Decode 是模型计算主体；内部按真实 embedding、attention、MLP、归一化及 logits 区域解释。重复层复用共有结构，层身份和参数分别绑定。融合节点保持实际工作边界，不能把理论模块分解直接当作设备任务分解。

动态轴来自实际模块、输入/输出槽及请求语义，不能从数值相等猜测。模型和 attention 的原生解释在生产端完成，编译后交给公共参数关系。权重分片、dtype、layout 和后端分支未取得变换依据时保持原适用范围。

具体关系见[Qwen3 结构](../redesign-workflow/history/2026-10-16_sglang_mechanism_notes.md#主线-qwen3-模型结构与参数化落点)、[矩阵与归一化](../redesign-workflow/history/2026-10-16_sglang_mechanism_notes.md#已接入的模型矩阵与归一化形状关系)及[Attention/KV](../redesign-workflow/history/2026-10-16_sglang_mechanism_notes.md#attentionkv-的长度语义与缓存布局)。不完整 vendor 操作数记录继续保留其限制，不能把未观测输出补成事实。

## 准入策略与内存压力

实际 worker 的有效策略参数、入口状态和请求顺序解释来源决策；目标控制器独占请求/KV 与动态准入状态。准入预算与物理页所有权分开，不能由一份静态来源快照代替后续状态推进。

当前范围不包含的内存压力、撤回和缓存行为应给出机制缺口，不通过改变请求或忽略等待使其运行。初始化与状态的公共合同见[控制接口](03_control_interface.md)，公式、原生顺序及历史资格见[准入推导](../redesign-workflow/history/2026-10-16_sglang_mechanism_notes.md#准入策略与内存压力)和[Schedule 前缀](../redesign-workflow/history/2026-10-16_sglang_mechanism_notes.md#联合资产下的-schedule-前缀变换边界)。

维护时在对应合同修改稳定规则，在专项审查记录代码与证据；本页只更新机制导航及边界。历史推导不继续追加“最新进展”，也不作为要求重新采集所有旧案例的任务清单。
