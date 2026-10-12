# 2026-10-12：workspace 生产、变换与执行链审阅

本文件集中维护功能交接现状及剩余缺口，不代替[代码质量整体检查](2026-10-12_code_structure_review.md)。历史修复过程留在执行日志，阶段门槛以[实施计划](2026-10-08_redesign_execution_plan.md)为准。以下开发证据均使用显式假设耗时，不能作为真实成本预测或独立结构验收。

## 当前职责与证据

| 交接 | 当前实现及已验证范围 | 证据（data/redesign 下） |
| --- | --- | --- |
| 原生资源与同步 | Ascend适配器保留初始快照、分配/复用/扩容/释放记录及CPU切口；profiler开关活动明确排除于业务同步域，其他未知工作不按owner为空排除 | s4-workspace-growth/serving_growth_verification.json、device_wait_reextract_verification.json |
| 起点资格 | 固定预热配方由生产端核对，TemplateBank::qualify_initialization限定首选来源；WorkspaceState只从首选来源初始化。后续供体须满足共同配置、资源角色与工作绑定，不导入供体容量 | s4-fixed-warmup/native_workspace_verification.json、donor_only_result.json、idle_cohort_verification.json |
| 需求和实际使用点 | ATB有限需求规则在runtime适配端生成；TemplateInstance在变换后遍历实际保留/插入marker并绑定目标需求。缺需求由WorkspaceState明确拒绝，不当作零 | s4-fixed-warmup/workspace_use_verification.json、target_wait_binding_verification.json |
| 容量和分支 | WorkspaceState按拓扑及同池真实可达顺序推进；allocator与语义区域共用替换器。SimulationRuntime在整批图装配成功后提交暂存容量，图重调度不再推进状态 | s4-fixed-warmup/target_wait_binding_verification.json |
| 扩容等待 | 插入的设备等待按实际worker提交及目标依赖可达性绑定当前动作前沿，并保留跨动作前驱；不搬用供体的来源前驱或按时间选最后节点 | s4-fixed-warmup/submission_frontier_verification.json、target_wait_binding_verification.json |
| 尾部清理及延迟到达 | base在两rank原生空Schedule记录后结束窗口，validation仍到最后Publish。已完成两请求结束→Idle清空→四请求到达→扩容→全部完成，4业务批次/17动作 | s4-native-cleanup/cleanup_observation_verification.json、dynamic_expansion_verification.json、full_graph_verification.json |

需求、初始容量、allocator规则和控制状态是不同事实。只有初始容量来自预热快照；目标分支由当前需求及唯一容量状态计算。KV cold不意味着workspace为空。稳定机制与有限域见[模板关系](../redesign/sglang_template_relations.md#workspace-容量状态与设备级同步待实现)。

## 插入区域再次替换的接口核对

`TemplateBank::plan_workspace_replacement`当前只接受主定义中仍保留的区域；`TemplateOperationRewrite.nodes/dependencies`引用原始定义，`TemplateTopology::apply_operations`仅删除原始节点/边，已插入节点和新增边只追加。边界编号达到source_boundary_count后按本次插入偏移重定位，不能把已存在的目标插入边界当作新边界再次偏移。

`TemplateInsertedNode`保留供体定义及原节点身份，但同一供体可以被重复插入；仅用manifest、source_call或source_span查找，不能唯一定位目标片段。因此不能只删除inserted_workspace_replacement_not_qualified检查。补齐该能力需要同一拓扑接口明确表达：要替换的实际目标片段及节点/边所有权、既存目标端点与新插入端点的区分、保留的外部服务及端口、原子移除并插入后的使用点/等待重绑定。继续复用现有替换器与唯一WorkspaceState，不另建allocator图或用供体时间戳定位。

当前实际调用链为请求级区域、KV、transition/update、五类BatchOperationRole（RelayWrite、RequestTableWrite、LastLocationConcat、PageTableDivide、LogitsReshape）及联合通信替换；尚无整个attention主体替换规则。allocator分支在这些变换后由WorkspaceState一次规划并共同提交。该静态核对没有证明任意插入片段均安全，也不删除该未完成项；在明确可达的模型区域变换及真实取证路径前，不增加无消费者的通用嵌套改写机制。

## 仍须完成

1. **变换的完整支持域**：插入子图中的再次allocator替换仍被明确拒绝；其他提交线程、更多分配生命周期和跨配置初值须建立可证明的规则。当前一次扩容及后续复用不授予这些资格。
2. **来源和错误解释**：workspace需求错误已由适配端经编译资产交给消费者，各拒绝分支的真实输入验证仍待完成。profiler启停归属已通过第72次双请求原生验证：4个完成范围、6条停止活动，其中4条同步明确列为scope；源图保留活动及72个host wait、48条原生等待边。证据s4-profile-lifecycle/lifecycle_verification.json。仍有3465条CPU、4条设备标记未归属及9条时间诊断，不能据此认定完整来源资格。旧来源无启停记录时保留历史缺口，不补造归属。

   第71次时间矛盾的数据库与因果核对集中在s4-group-cleanup/database_clock_audit.json：53条时间精确一致，52条数据库内仍有矛盾；所有受影响任务完成点均可达相应等待完成点。该组问题保留为时间诊断，不据此删除依赖、拟合偏移或新增结构补采，也不授予实测成本资格。后续优先真实结构覆盖及其他未归属工作，不能只以清空时间诊断推进。
   未归属工作与模板端口核对见s4-profile-lifecycle/unassigned_port_audit.json：3465条CPU记录中3107条是事件查询及销毁相关注解，604对入队/出队有唯一rank/correlation关联，但队列注解没有event handle，不能反推业务事件代次。全部26条一端未归属的模板交接均为CPU thread_order（销毁出队及请求收发）；现有TemplateAssembler可投影模板外CPU顺序并通过资源前沿交接，不给外部工作补零成本节点或强加业务归属。该结果支持在已声明modeled范围内继续结构验收，不要求清空全局未归属计数；投影的目标执行仍需独立验收，完整CPU/E2E成本及未知因果不因此获资格。
3. **轻量建库**：代表采集组间与末组均已用全部rank的原生空Schedule确认；现有三组配方的两次组间交接已核对，证据s4-group-cleanup/group_cleanup_verification.json。重复实例归并、完整机制覆盖及累计开销资格尚未收口，不以继续枚举请求组合代替。
4. **独立结构验收**：新增耗时反馈输入已用于修正合并行数规则，转为开发回归；2+2合并后过滤及100ns基线验证通过，证据见实施计划当前结构验收段。修正后新留出序列的首次原生明细对照已通过，证据集中在实施计划当前结构验收段及s4-independent-structure；规则/资产未变，保持独立性。下一步复用已有validation事实核对等待前沿、跨阶段端口及关键内部依赖，CPU与全局拓扑等价性仍待验收；不能以kernel集合相同代替拓扑。
5. **后续阶段**：S5节点成本模型尚未实现；S6代表性自动采集及只重算受影响组合仍待完成。安装适配器隔离构建/加载有证据，完整Dockerfile重建未验收。

本报告删除了“需求未被执行核消费”“所有目标分支尚未执行”等已失效结论，保留未完成项与对应证据。完整S3–S6目标保持进行中。
