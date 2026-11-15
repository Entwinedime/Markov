# 执行结构可信性窗口状态

更新：2026-11-15。

goal运行中；A0–A3已提交推送，A4功能、责任审定及代码/文档/数据检查完成，本次分层交付记录为results/a4-stage-delivery.json。A5尚未启动，独立验证和性能层未开始。分支refactor/modeling-redesign，A4起点c9da95c；运行根data/20261114T161057Z_execution-structure-development，以下数据路径均相对此根。

## 阶段与资格

| 阶段 | 当前资格 |
| --- | --- |
| A0 基线、遗留归因和额度 | 完成，acc8c68 |
| A1 完整闭环及基础合同 | F01–F18审定开发范围完成，b1c8f52 |
| A2 状态与结构变换 | 普通Serving完成，8d02571；9完整目标32276动作，PD责任由A4承接 |
| A3 多实现族和自动来源 | F29–F36审定范围完成，c9da95c；15完整目标10094动作 |
| A4 多rank、PP/EP/PD及连接反馈 | F26/F27的PD部分、F37–F46审定开发范围完成；13完整目标278093动作 |
| A5 调度、缓存及持续运行 | 待执行，先按A4交付核对现状及既有事实，再固定有限作业 |
| A6 工程收口、整合回归及冻结 | 待前述责任闭合 |
| B1–B4 独立验证 | 固定库16、新域12、模型留出8、组合12，共48个完整目标，未启动 |
| B5 处置、有限复核和交付 | 未开始；性能层不属本goal |

[场景表](verification_matrix.md)维护56项行为/合同责任，责任数不是采集数。Meta-Llama-3.1-8B-Instruct、GLM-4-9B-Chat仍为留出候选，冻结前须核实原生支持及历史介入。

## A4完成部分的逻辑

1. **选来源和角色：** 结构等价代表与输入机会分开。EP8观察0/1/7而非全部rank；PD4→2来源只观察PF0/1/2，目标全4个PF均能展开。协议仍有全部实际worker参与，局部实现保留目标rank及资源绑定。
2. **构造和绑定模板：** 共享定义保存工作、规则及端口，调用帧保存池/层/切片。声明的状态输入保留池别名、相对层、相对字节位置和宽度；adapter取得来源端点几何，按本次预约顺序重绑定，不为每种顺序补库。
3. **动态连接与反馈：** EP Dispatch保留全组metadata，Combine按真实双向split及输入推导必要peer。PP保留真实复制并移除观察envelope制造的伪栅栏；PD区分CPU中间循环、设备尾部、载荷可见和原生Success后的源释放。
4. **独立核对：** 真实目标检查全rank内容、内部/跨图依赖及控制/资源历史。代表来源可保留未观察范围，完整目标必须正式检查退出0；实际复制还核对全局层和K/V身份，不能以地址真实代替载荷语义正确。

## A4完整开发目标

本表使用各自报告记录的程序，不代表所有目标均在最终二进制下重跑。收尾针对受影响PP4/PD和EP8以ff9bf38247a5复核；最终83ab6b12998d仅将两处父节点读取改为受检读取，原图编译产物逐字节相等及畸形输入拒绝另有证据。

| 案例 | 动作 | results下的入口 |
| --- | ---: | --- |
| Mistral PD 1→2 | 1735 | a4-mistral-pd-flow/comparisons.json |
| Qwen8 TP1×PP4 | 4618 | a4-qwen8-pp4-copy-owner/comparisons.json |
| Mistral PD 2→1压力 | 38848 | a4-pd-reuse-copy-owner/comparisons.json |
| Qwen30 EP8集中 | 1256 | a4-ep8-local-placement/comparisons.json |
| Mistral TP8 | 1782 | a4-mistral-tp8-local-placement/comparisons.json |
| Qwen30 EP4阶段来源到均衡目标 | 2028 | a4-moe-pipeline-qualified/comparisons.json |
| Mistral PP3不等分 | 2558 | a4-parallel-directions/peer-scope/comparisons.json |
| Mistral TP2×PP4 | 6582 | a4-state-bindings/comparisons.json |
| Qwen30 TP2×PP3×EP2 | 5708 | a4-parallel-coupling/comparisons.json |
| Mistral PD 2→4 | 66292 | a4-state-bindings/comparisons.json |
| Mistral Prefill TP1×PP4→Decode TP2 | 45198 | a4-pd-pp4/comparisons.json |
| Mistral TP2×PP4，实际微批4/4/1 | 5844 | a4-pp4-microbatch/comparisons.json |
| Mistral PD 4→2，只读通知诊断目标 | 95644 | a4-pd-notifications/comparisons.json |

A4的12项责任映射及资格见results/a4-parallel/acceptance.json；关联代码审查在work/a4-parallel/repository-review.md。实际微批与驻留槽、排序及原生split分别在a4-parallel/microbatch-behavior.json、pd-sorting-witness.json及a4-moe-pipeline/native-splits.json。PD压力的42项在途冲突有序，归还时在途KV页0；成功释放受Success保护，不把零计数当作异步释放见证。

## 失败与限制

- **原生PP2：** 两份失败恢复14次HCCL/28通知代次，前五次配对无环，随后输出send→receive形成六指令环。无device/data，不声称恢复实际设备停滞context。另有PP2来源成功，不能一概称全部PP2不支持。见a4-parallel/pp-output-order-diagnosis.md、plain-pp-diagnosis.json及a4-moe-pipeline/native-qualification.md。
- **原生PP3/PD地址：** 32层的10/11/11布局被上游误判draft，V起点30/22而非32，目的V22–V31缺写且部分V落入K。源48/目标54次错误识别，旧整组consistent资格撤销，36688调用partial保留。合法PP4/PD另取独立来源及完整目标，不改判旧输入，不patch上游或补造KV。见a4-parallel/pd-layer-routes/explanation.md。
- **旧PD4→2预热超时：** 旧记录缺少通知链，唯一历史原因仍未知；一次同输入只读诊断52次通知全部送达，8组预热及18组正式计数闭合，但观察可能改变交错，不能称已修复。见a4-parallel/pd-notifications/summary.md。
- **证据范围：** 旧EP8 split由固定注入路由与实测行数恢复，新来源另有2880条真实handle和11520对peer行数；两者不混淆。时间及后台顺序是明确验证输入，目标几何/图体未进入预测；自主网络时序、数值载荷、粗图未观察细节及性能精度均不取得资格。时钟校准不确定性保留。

一次费用脚本误写首个MoE来源execution.json，原始profile/input未改，必要字段已显式恢复，但原序列化字节丢失；owner/recovery.json保留事故，预算采用固定清单和单一原子写入点。旧失败与恢复费用不清零。

## 程序、规模、费用及下一步

source/control/modeling为3e7b3d4db695、bc251fd3aeae、e066a5196c3b，最终程序83ab6b12998d固定在inputs/a4-parallel/stage-review/final。source8份/control6份本阶段模块与工作区逐字节一致，官方SGLang 4e155ecc31af未改。统一C++23构建、126份Python3.10语法及全src/example Ruff F通过。两次完整收尾回归见results/a4-stage-review，字节平移/重叠及父节点边界见a4-parallel/stage-review-contracts、stage-review-boundary-evidence.json；错误诊断文案断言的失败记录保留，不改写成命令成功。

业务171文件22145行、示例27文件8097行，总30242行；A4净增315/100，无新业务文件。数据、模型、构建、deprecated和用户reference工作不提交；受保护文件、原暂存及.clang-format保持原样。阶段提交和远端实际HEAD在results/a4-stage-delivery.json。

A4累计32次7384.633原生秒、27092.249已测处理秒及300秒未测观察预留；窗口135次26514.413秒、余105次。阶段上限32次/12600原生秒/38400处理秒/64GiB，当前运行根约47GiB。本次收尾无新增原生、SDK重导或目标范围扩张，全部本阶段作业终态。

下一步A5先核对chunk、mixed、prefix的原生分支及既有真实事实，明确实际触发和有限来源，再做调度/缓存/压力及256/1024请求持续运行；不能只重复配置开关或先跑全量找缺口。A6之后才冻结并启动48项独立目标。A1、A2、A3恢复入口分别为results/a1-values、a2-stage-delivery.json、a3-stage-delivery.json；历史事实仅显式引用，旧调查不改判。
