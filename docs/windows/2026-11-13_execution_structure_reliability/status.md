# 执行结构可信性窗口状态

更新：2026-11-15。

goal运行中。A0–A2已分层提交推送，A3审查完成、待分层交付；尚未开始A4或独立验证。当前分支refactor/modeling-redesign，阶段起点8d02571。运行根data/20261114T161057Z_execution-structure-development；下面的results、inputs、sources及work均相对此根。

## 阶段与资格

| 阶段 | 当前资格 |
| --- | --- |
| A0 基线、遗留归因和额度 | 完成，acc8c68 |
| A1 完整闭环及基础合同 | F01–F18审定开发范围完成，b1c8f52 |
| A2 状态与结构变换 | 普通Serving范围完成，8d02571；9完整目标32276动作，PD动态责任在A4 |
| A3 多实现族和自动来源 | F29–F36审定范围完成；15完整目标10094动作，分层提交推送待确认 |
| A4 多rank、PP/EP/PD及连接反馈 | 待执行，承接PD延迟归还/排序及在途再用 |
| A5 调度、缓存及持续运行 | 待执行 |
| A6 工程收口、整合回归及冻结 | 待前述责任闭合 |
| B1–B4 独立验证 | 固定库16、新域12、模型留出8、组合12，共48个完整目标；未启动 |
| B5 处置、有限复核和交付 | 未开始；性能层不属本goal |

[场景表](verification_matrix.md)维护56项行为/合同责任，责任数不是采集数。Meta-Llama-3.1-8B-Instruct、GLM-4-9B-Chat仍是留出候选，须在冻结前核实原生支持和历史介入。

## A3当前实现与证据

来源规划预先选择有限互补作业，完整预测从正式集合选一份主来源，按声明导入局部实现；不跨采集拼物理流，不由目标失败自动补采。容量互补、路由及padding分组采用共同机会选择；专家数规划时取得并由真实模块核对，纯预测不访问模型目录。缓存输入机会复用ABI guard；默认示例实际两个作业，额度说明同步。

缓存启用时，采集/原生控制/解释显式交接页大小、缓存长度及进入时保护长度，避免空/正拷贝歧义。原始事实不改写，30次调用用原生insert/free独立核对。普通Serving与PD共用已有有限merge/subset来源；一次四请求MoE辅助运行后，旧单/多请求目标172/568动作对应，主来源不变、C++不加特例。

- results/a3-sources/final-opportunity-checks.json：四组规划、10项选择、共享前缀guard及无模型目录的纯选择；规划不是原生覆盖。
- sources/a3-models：八个旧模型/页域按相同原生输入正式重建，14代表库各38–49定义；所有本地层均有实际scope，Qwen8/32分别36/64层，Mistral32层，Mixtral32层/8experts/top2。
- results/a3-models/comparisons.json及results/a3-api-regression/comparisons.json：八模型/页目标共8046动作对应。原矩阵五例直接通过，三例因example漏传环境的工具错误定向复核通过；原结论保留。
- results/a3-cache-regression/comparisons.json：FP16短/非均匀及旧prefix/压力四例1204动作对应；来源三份均发布，无重复采集。
- results/a3-moe-local-regression/comparisons.json：两例740动作对应，包含曾缺merge的多请求到达案例。
- inputs/a3-mixtral-short：沿用旧EP1库，一次新真实短目标完整104动作对应；results/a3-mixtral-short/comparisons.json保留终态。

以上为开发回归，原始旧独立调查不改判。来源检查只有代表rank时，全组通信部分状态不冒充全rank证明；目标对照使用各自实际全rank事实。Mixtral历史EP>1的14次失败发生在原生swiglu_quant的UB编译容量，不patch框架；其EP1成功是另一项事实。七次历史PP watchdog由A4继续分析。

## 程序、镜像和代码

C++本阶段未改。程序build/resource-parameters/markov，SHA256为94a439a70e8aa7d6cc66a18ed8738d362d8ad853cec58f71b7cc9419e209b986。source/control当前ID为ffb5e580539e/13dc6a85682f，modeling为e066a5196c3b；实际完整ID及包校验在work/a3-sources/cache-package-verification.json。17层原生基础相同，官方SGLang 4e155ecc31af未改；旧任务保留当时实际ID。构建首次代理网络失败保留，host构建网络成功，不改变无设备预测的隔离。

当前src171文件/21830行，example27文件/7997行，共29827行，相比A2业务净增158、示例净增4。Ruff F、126文件Python3.10 AST、消费者及空白检查通过，阶段静态审查和依据索引在results/a3-sources/acceptance.json；不反复构建未改的C++。

## 既有阶段恢复入口及边界

A1：results/a1-values/summary.md及results/a1-contracts/summary.md；基础参数/层次、SDK发生实例、多stream、原始载荷/张量用途及父子发布关系已分别核对。TP1/TP2/PP3的233/104/1070动作完整开发见证不代替其他配置资格；零接收和最小输入分别只是所列局部/完整范围。

A2：results/a2-state/summary.md、acceptance.json及results/a2-stage-delivery.json。九例32276动作、6507页表快照、231在途冲突均有保护路径；最大并发配置128、实测最大批70，KV4096到262144由小池规则变换。普通路径归还时在途页0，PD异步归还和排序仍在A4；单请求output2给定耗时和三项局部几何不冒充新真实目标。

历史根data/20261113T111324.301829Z_execution-structure-reliability及data/20261114T084338.925689Z_interactive-structure-validation保留原调查、六例长度回归及原始SDK；只按明确消费者复用，不自动找最新资产。旧调查3通过/3失败及跨域失败不改写，修正后结果归开发。

## 费用与交付

A1原生3次/447.668秒，处理1171.010秒；A2原生3次/434.210秒，处理4252.682秒及120秒未测区间预留。A3原生5次/671.606秒，处理5156.231秒及60秒未测区间预留；阶段上限5次/4500原生秒、16000处理秒、128GiB。总窗口103次/19129.779秒，总上限240次、原生及处理各172800秒；新增任务终态随后由真实owner合计，旧事实引用不重复收费。

当前不授予任意模型、配置、容量/压力或复杂调度组合支持；测量校准范围和粗化可见性保留。未知guard不能任选或填默认值，缺口不挪给性能层。A3审查、数据/文档整理及分层提交推送后才进入A4；用户.gitignore、.gitmodules、reference暂存及.clang-format保持原样。
