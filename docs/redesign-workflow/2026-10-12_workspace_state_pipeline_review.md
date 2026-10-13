# 2026-10-12：workspace 生产、变换与执行链审阅

本文件保留 workspace 专项交接证据和接口限制（2026-10-13 校正），不代替[代码质量整体检查](2026-10-12_code_structure_review.md)。历史修复过程留在执行日志，阶段门槛以[实施计划](2026-10-08_redesign_execution_plan.md)为准。以下开发证据均使用显式假设耗时，不能作为真实成本预测或独立结构验收。

## 当前职责与证据

| 交接 | 当前实现及已验证范围 | 证据（data/redesign 下） |
| --- | --- | --- |
| 原生资源与同步 | Ascend适配器保留初始快照、分配/复用/扩容/释放记录及CPU切口；profiler开关活动明确排除于业务同步域，其他未知工作不按owner为空排除 | s4-workspace-growth/serving_growth_verification.json、device_wait_reextract_verification.json |
| 起点资格 | 固定预热配方由生产端核对，TemplateBank::qualify_initialization限定首选来源；WorkspaceState只从首选来源初始化。后续供体须满足共同配置、资源角色与工作绑定，不导入供体容量 | s4-fixed-warmup/native_workspace_verification.json、donor_only_result.json、idle_cohort_verification.json |
| 需求和实际使用点 | ATB有限需求规则在runtime适配端生成；TemplateInstance在变换后遍历实际保留/插入marker并绑定目标需求。缺需求由WorkspaceState明确拒绝，不当作零 | s4-fixed-warmup/workspace_use_verification.json、target_wait_binding_verification.json |
| 容量和分支 | WorkspaceState按拓扑及同池真实可达顺序推进；allocator与语义区域共用替换器。SimulationRuntime在整批图装配成功后提交暂存容量，图重调度不再推进状态 | s4-fixed-warmup/target_wait_binding_verification.json |
| 扩容等待 | 插入的设备等待按实际worker提交及目标依赖可达性绑定当前动作前沿，并保留跨动作前驱；不搬用供体的来源前驱或按时间选最后节点 | s4-fixed-warmup/submission_frontier_verification.json、target_wait_binding_verification.json |
| 尾部清理及延迟到达 | base在两rank原生空Schedule记录后结束窗口，validation仍到最后Publish。已完成两请求结束→Idle清空→四请求到达→扩容→全部完成，4业务批次/17动作 | s4-native-cleanup/cleanup_observation_verification.json、dynamic_expansion_verification.json、full_graph_verification.json |

需求、初始容量、allocator规则和控制状态是不同事实。只有初始容量来自预热快照；目标分支由当前需求及唯一容量状态计算。KV cold不意味着workspace为空。稳定机制与有限域见[模板关系](../redesign/sglang_template_relations.md#workspace-容量状态与设备级同步)。

## 插入区域再次替换的接口核对

`TemplateBank::plan_workspace_replacement`当前只接受主定义中仍保留的区域；`TemplateOperationRewrite.nodes/dependencies`引用原始定义，`TemplateTopology::apply_operations`仅删除原始节点/边，已插入节点和新增边只追加。边界编号达到source_boundary_count后按本次插入偏移重定位，不能把已存在的目标插入边界当作新边界再次偏移。

`TemplateInsertedNode`保留供体定义及原节点身份，但同一供体可以被重复插入；仅用manifest、source_call或source_span查找，不能唯一定位目标片段。因此不能只删除inserted_workspace_replacement_not_qualified检查。补齐该能力需要同一拓扑接口明确表达：要替换的实际目标片段及节点/边所有权、既存目标端点与新插入端点的区分、保留的外部服务及端口、原子移除并插入后的使用点/等待重绑定。继续复用现有替换器与唯一WorkspaceState，不另建allocator图或用供体时间戳定位。

当前实际调用链为请求级区域、KV、transition/update、五类BatchOperationRole（RelayWrite、RequestTableWrite、LastLocationConcat、PageTableDivide、LogitsReshape）及联合通信替换；尚无整个attention主体替换规则。allocator分支在这些变换后由WorkspaceState一次规划并共同提交。该静态核对没有证明任意插入片段均安全，也不删除该未完成项；在明确可达的模型区域变换及真实取证路径前，不增加无消费者的通用嵌套改写机制。

## 仍须完成

本节区分真实缺口与条件性扩展，当前任务顺序只维护在实施计划。

- **对应配置的真实缺口**：统一预热后跨配置初始容量尚无迁移规则。供体可用不代表它可初始化目标；支持该配置变化需要解决这条交接，不用复制来源容量绕过。
- **条件性扩展**：插入区域再次替换、更多提交线程及其他 allocator 生命周期是已知接口限制。仅当承诺场景实际触发时实施，不预建任意嵌套图替换器，也不作为所有 S4 场景的统一门槛。
- **保留诊断，不新增范围**：第 71/72 次 profiler/time 记录及未归属活动见 s4-group-cleanup/database_clock_audit.json、s4-profile-lifecycle/unassigned_port_audit.json。已有 26 条未归属侧交接属于线程顺序，公共投影保留；模板外后台服务不计费，不要求清空诊断或完整 watchdog 仿真。
- **验证用途纠正**：s4-fixed-assets-acceptance 用于合并规则修正；第 73 次 s4-independent-structure 用于事件归一化修正，两者现均为开发证据。不能继续称其为修改后独立验收。具体已核对内容及限制在执行记录维护，不在本页重新列全图检查任务。

workspace需求、目标容量、扩容分支和必要同步仍属于流程内的真实机制；缩小分析范围不意味着删除它们。采集轻量化、最终独立验收、S5/S6及安装验证由实施计划和相应合同管理，不在专项审查重复维护阶段待办。
