# 执行结构可信性窗口状态

更新：2026-11-16。goal运行中，性能层不属本goal。

分支refactor/modeling-redesign。A5起点36936a1，功能及维护检查已完成；恢复时先核对主运行根results/a5-stage-delivery.json中的最终提交及远端确认。A6和独立B尚未开始。

主运行根为data/20261114T161057Z_execution-structure-development，未另注明的证据路径均相对此根。新标量淘汰边界的取证根为data/20261115T194339.298210Z_a5-resource-boundary。

## 阶段与资格

| 阶段 | 当前资格 |
| --- | --- |
| A0 基线与额度 | 完成，acc8c68 |
| A1 完整闭环与合同 | F01–F18审定范围完成，b1c8f52 |
| A2 状态与结构变换 | 普通Serving9目标32276动作完成，8d02571；PD责任由A4承接 |
| A3 多实现族与自动来源 | F29–F36完成，15目标10094动作，c9da95c |
| A4 多rank协作 | F26/F27的PD部分及F37–F46完成，13目标278093动作，36936a1 |
| A5 调度、缓存与持续运行 | F47–F56开发责任已审定；新256/1024分别13951/57555动作完整通过，维护检查完成，最终Git交付见恢复入口 |
| A6 工程收口与冻结 | 未开始；须全链整理、区分旧局部证据与最终ABI整轮资格、完成整合回归和冻结 |
| B1–B4 独立验证 | 固定库16、新域12、模型留出8、组合12，共48完整目标，未启动 |
| B5 处置与交付 | 未开始 |

[场景表](verification_matrix.md)维护56项责任，责任数不是采集数。A1–A4恢复入口分别为results/a1-values、a2-stage-delivery.json、a3-stage-delivery.json及a4-stage-delivery.json。Meta-Llama-3.1-8B-Instruct和GLM-4-9B-Chat仍为留出候选，冻结前须核实基本原生支持及历史介入。

## A5实现及当前证据

- **有限来源：** 删除冗余完整请求时保留声明机会、到达、路由及共享整页路径；mixed保留驻留窗口外竞争请求。三组主/局部来源请求4/4、9/12、258/4，目标不裁剪，来源不会因目标失败自动增加。
- **局部装配：** Prefill/chunk预算共同容纳完整双prompt；mixed要求实际mix/subset，普通仍merge/subset。同完整worker域可共享已观察的闭合叶动作，保留guard、端口、别名及状态/通信/写入限制；只有已证明的批次数组采用更宽复用域。候选只保存来源索引，拒绝前不复制大图。
- **补齐合同：** 声明表覆盖、attention TP上界与实际消费分开，EP只决定专家分片，EP1也检查；未消费位置不产生模型行、缓存内容或额外接收者机会。
- **资源切口：** 公共淘汰函数只声明eviction_tokens，不继承外层批次张量；查询、原生heap/LRU循环和每次实际页释放保留。安装时在apply_hooks前注册，bind只接真实资源。新主库48定义/45入口，淘汰CPU片段恰1个无张量/子调用/状态的入口，来源资格合格。
- **时间交接：** 条件作为独立只读资产，核心借用actor步骤；准备与执行共用期限，关闭端点后发布并清理临时任务。新1024任务89694字节，独立时间文件3990803000字节，不重复嵌入。

| 完整开发对照 | 动作 | 实际程序及证据范围 |
| --- | ---: | --- |
| 新边界256请求 | 13951 | cc1605；新增根results/a5-resource-boundary/continuous-256/comparison |
| 新边界1024请求 | 57555 | cc1605；新增根results/a5-resource-boundary/continuous-1024/comparison |
| chunk边界 | 620 | ab69；results/a5-closed-actions/boundaries |
| MoE TP1×PP3×EP1，chunk/mixed/prefix | 2888 | ab69；results/a5-mixed-contract/pp-cache |
| EP8共享合同 | 1256 | ab69；results/a5-closed-actions/ep8，旧目标回归 |
| prefix/压力事实重建 | 233/654 | 83ab；results/a5-cache-reuse |

新两目标内容、具名边界依赖、逐次对应、资源历史及最小次序关系均无差异，228/933项在途冲突均有保护路径。时间条件消费完整：256核对169次输入、13951动作及39372边界；1024核对681次输入、57555动作及164966边界，无未消费、未绑定或未完成条件。来源SDK原始作用检查覆盖单host_only片段及合并CPU区域，两个来源和两个目标均合格；1024有16632个CPU区域，必要设备端点缺测/未知时间约束0。

新1024真实发生113次有效淘汰；首次Prefill请求146/归还160 token，含10次页释放，确实触发旧失败分支。时间提取57555动作/252258测量边界、必要工作完成缺测0；47361项仍有校准范围，采用同一共同可行实现，不声称唯一真实轨迹。完整报告在新增根results/complete-development-validation.json及overview.md。

PP旧边界开发中各rank5次真实mixed；同token仅远端层路由不同的foreign命中0，partial命中32，reuse命中64且执行最后1 token。共享32-token页的锁最终解除，三worker资源历史相同，54项在途冲突有序；256私有请求在17–24八种长度下首次命中均0。精简/明细结果相同，成功恢复不重采、不改owner/发布/费用。证据results/a5-dynamics/{observed-scheduling,private-prefix-witness,concise-detail,successful-resume}.json。

## 保留的失败与边界

- 旧1024在ab69下缺Prefill淘汰入口，partial比较14432动作，失败不改判。旧256的13507动作也保留原边界资格；新两目标才提供当前切口的整轮证据。
- 原chunk局部缺subset、主库遗漏辅助闭合片段、PP旧EP1补齐合同及独立merge发布要求均已沿共同链修正，保留原失败和费用。原TP2×PP3×EP2混批FIA来源/目标query4/长度3、query50/长度49为具体原生限制，未patch框架，不推广所有TP2混批不支持。
- A4的PP2输出通知环、32层PP3/PD错误K/V地址资格已分别调查，错误载荷旧资格撤销，合法PP4/PD另有完整见证；普通PP不等分由Mistral32层/PP3承担。旧PD4→2预热超时唯一历史原因仍未知，一次同输入52通知全送达/95644动作成功不等于修复旧交错。相关证据见a4-parallel与work/a4-parallel。
- A4一个owner误写和A5临时报告路径冲突均有显式恢复依据，原始事实未改；A5首轮Ruff只读缓存参数、modeling标签误用及jq metadata探针超时保留。CPU检查强化前后的原报告不改，另存复核且除CPU计数外完全相同；这属于开发评价修正，不是冻结独立结果。

旧成功只证明各自记录的调用边界。A6将复核未变子图并审定最终ABI的MoE/PP及PD完整代表，不能离线补造旧目标缺失的新端点时间。来源代表允许明确未观察peer范围，完整目标必须全参与者正式合格。数值载荷、粗图未保留细节、自主时序、自然路由及性能精度未取得资格。

## 程序、费用及交付

当前程序cc160510d333，sourcecb054118fff5/controla2bd5ea90c28/modelinge066a5196c3b；旧PP/EP8/持续回归按0903/b1edd及ab69/83ab实际身份保留。官方SGLang 4e155ecc31af干净。最后仅恢复宿主workflow返回四元组的准确注解，C++和安装包未变；两CMake target原构建及最终126份Python3.10语法/Ruff均通过，无新测试工程或成本模型。

业务171文件22264行，相对A4净增119行；示例27文件8103行，净增6行，无新业务文件、压行或迁出统计。.clang-format、用户reference/.gitignore及原暂存保持独立。

A5全部18次6326.403原生秒，窗口153次32840.816秒；阶段已测处理28657.535秒，另180秒未测观察/恢复预留。最后固定3项协调14470.691秒减2236.318原生秒，按12234.373处理秒一次结算，不叠加子步骤。两根约116.31GiB，低于原96/新64、合计160GiB上限；原两个依赖未满足的规划任务没有运行或隐式恢复。

代码、文档、数据审查入口为work/a5-dynamics/stage-review.md及新增根results中的核对记录；责任审定results/a5-dynamics/acceptance.json，最终分层提交和远端确认results/a5-stage-delivery.json。下一阶段先按交付基线重新核对A6范围及有限额度，再集中整理、整合回归和冻结，之后才进入48项独立目标。
