# S3/S4 主线结构阶段验收

> 历史审查/观测记录：结论限定于文中日期与当时实现。2026-10-16 清空数据后，旧 `data/redesign` 地址不再可复核；本文的“当前”“待完成”不作为现在的任务或授权状态。实时状态见[主计划](../2026-10-08_redesign_execution_plan.md)。

日期：2026-10-13。依据[实施计划](../2026-10-08_redesign_execution_plan.md)的原完成条件，核对代码职责及已有实际产物。结论：S3/S4在既定主线与模板规则适用域内交付通过，可以进入S5；S5/S6未完成，整个goal不结束。当前任务只在实施计划维护。

## 范围

固定Qwen3-32B、SGLang、Ascend TP2及现有部署，统一预热、cold KV、FCFS、非overlap、非chunk、无radix/graph。配置变化沿既有规则支持请求池保持/有条件缩小及Prefill预算变化；请求成员、长度、页与局部操作须通过模板实际资格。候选长度域不构成任意组合保证。本验收不增加或缩小规则条件，也不宣称TP/后端迁移、任意CPU控制路径、内存回退或完整HTTP预测已实现。

## 完成条件及证据

证据地址以`data/redesign/`为根；报告的比较范围须连同结果一起读取，不能单看complete或节点数。

| 条件 | 当前实现与实际证据 | 判断 |
| --- | --- | --- |
| S3 定义→wrapper→观测→模板追溯 | ServingContract定义五类block；公共BlockWrapper/Recorder消费合同，框架解释在适配端；BlockExtractor保留来源与切边。真实单库manifest、source_facts、source_graph、templates及compiled_templates均存在；`s3-bounded-library`及`s6-representative-handoff`生产链已运行 | 通过；不要求模板外观测全部归属 |
| S3 参数、依赖、资源、状态及身份重绑定 | TemplateBank/TemplateAssembly消费公共绑定，目标图保留source_asset/span及端口；`s3-workspace-order`同库同配置、池4→3和发布依赖证据；`s4-prefill-budget-acceptance`新请求预算轴对照 | 通过现有规则域；未知规则仍明确拒绝 |
| S3 有限模板与复用 | attention/feed_forward共3328次区域引用28份局部定义，来源映射保留；`s3-template-families/inventory.json`与采集合同的族/变体/参数关系清单。不是逐decode长度建模板，未共享serving区域不伪称完全去重 | 通过；不预建图同构或第二套族管理器 |
| S3 代表方案、资产与整体开销 | 四组11请求配方、单组一次采集及导入；`s6-representative-handoff/verification.json`实际宿主中断恢复，已有资产和缺成本均不重采；总原生/后处理/原始/派生字节有累计账本。共享采集路径分段计时在validation中实测 | 通过；“有界”不等于所有开销已最优 |
| S4 请求生命周期与唯一时钟 | LightweightController处理准入、组批、KV及发布；SimulationRuntime/ExecutionGraph推进唯一事件时钟，workspace装配后提交。独立对照包含请求成员、token进度与发布结果 | 通过现有控制路径 |
| S4 配置与新请求变换 | 池4→3：`s4-pool-transfer-acceptance`原生参考及`s3-workspace-order`单库复核。预算256：`s4-prefill-budget-acceptance`未参与修改的3请求、5批/20动作；40个rank调用、10个准入入口、8918个kernel、1300个collective、13506个通信任务及70次复制对齐 | 通过所列比较；不外推为完整内部边一致 |
| S4 给定耗时的动态反馈 | `s3-workspace-order/feedback-and-reference-verification.json`同库同请求，100/1000 ns改变动作数与组批；仍使用同一控制器及执行核。缺耗时显式停止，不填零 | 通过；不是实际时延精度 |
| S4 独立结构与关键连接 | 预算轴规则、模板在验证前固定，validation没有导入生产；`structure-comparison`、`admission-comparison`、`communication-comparison`及`publication-dependencies`均按其明确字段无差异。发布前设备工作和发布后下一模型全部kernel的必要路径成立 | 通过；完整CPU/通信内部边不是原阶段隐含门槛 |

## 阶段交接，不混淆未完成工作

- S5负责目标工作描述、性能单元、独立测量及适用域、服务/等待/探针开销、真实成本和误差验证。未变化的来源工作也不能直接把包络时间当服务成本。
- S6继续承担多个成功建库组、原生/后处理失败隔离、受影响组合重算和真实成本探索闭环。已有单组恢复及配方失败隔离不替代这些交付。
- 成本竞争、CPU缓存服务差异和插桩开销先按性能合同处理；若出现改变现有外部因果的具体反证，再重开对应结构问题，不把所有成本风险转回无限S3补采。
- 后续改动触及绑定、变换或端口时重验相关证据；已用于修改的验证输入转为开发用途。本次预算轴输入尚未用于产品修改。

进入下一阶段前，复查了输入/生产/控制/执行依赖及最近Python编排、恢复与计时代码；专项代码审查见[当日审查](2026-10-13_code_structure_review.md)。没有增加采集、重复运行已通过场景或把这份验收写作整体goal完成总结。
