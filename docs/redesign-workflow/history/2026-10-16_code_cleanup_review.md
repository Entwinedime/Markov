# 结构整理与全量代码审查记录

更新：2026-10-17。对应[专项计划](2026-10-16_structure_cleanup_plan.md)。本次完成全部活动自有源码审阅、确认问题整改和有限主线的真实验收。精度、范围外能力与未证实风险单列，不把源码审阅或流程完成当作精度通过。

## 范围、身份与审阅结论

审阅 `src/markov`、`src/collection`、`scripts`、`example`、`docker`、`configs` 及根构建/配置，共 **237/237 个文件、36,521 行**，含空行和注释。逐文件内容哈希、行数及职责组见[覆盖清单](../evidence/2026-10-16_code_review_coverage.csv)。最终复核清单与源码身份一致；编译和搜索不替代逐文件阅读。官方依赖、参考源码按需只读核对，旧项目不进入重构，原主分支不受影响。

| 已审职责组 | 直接消费者、结论与边界 |
| --- | --- |
| 构建、安装、CLI、示例 | Docker 直接调用适配安装器；Shell 传递参数，CLI 使用标准子解析和摘要；无失效安装转发。现有镜像运行通过不等于重新构建 KT 镜像 |
| Python workflow | RunDirectory 管路径/身份，ContainerEnvironment 管 Docker/路径映射；主 workflow 写父账本，PID 1 写原生结果，离线处理器写交接收据。父任务确认后清理，恢复不从缺文件猜成功 |
| 合同、控制与执行 | 控制器持有逻辑请求/KV，执行层持有时钟/资源，workspace 先暂存再提交；报告及成本查询不推进业务状态，无第二套原生组批/KV 生命周期 |
| 性能与需求规划 | 需求与查询共用工作特征；计算、通信、传输、命令分别筛选，同一设备样本只解码一次。实际成员/工作决定覆盖，不把运行过某批次等同于取得成本；查询不启动采集 |
| 插桩、框架与模型生产 | block、wrapper、region 共用定义身份；原生符号、组批、请求池、Qwen3、KV、HCCL/ATB 绑定归框架/模型适配。KT 只声明已有采集能力，不伪装成预测支持 |
| 原生观察及 Ascend | C ABI、queue/task/event 关联和物理 DMA 描述归 runtime；CPU 因果/等待保留但不计服务耗时，同步 driver I/O 与设备 DMA 分开。跨时钟仍有适用限制 |
| 模板、变换与编解码 | 定义拥有来源，实例拥有目标变更；替换按端口、资源和依赖资格，完整 cohort 通过才发布；反序列化先恢复拥有对象再绑定引用。未新增兼容读取器 |
| 产物、报告与恢复 | 默认结果引用共享来源/目录；诊断开关不改变业务；验证隔离。事实、资产、原生结果、收据各有用途，不以删除原生归属信息压缩输出 |

84 份核心头/源的直接及传递 include 未进入具体适配或 workflow。2524 行 `templates.cpp` 按已有类拆为 `template_definition.cpp` / `template_bank.cpp`：56 个方法原文迁移；拓扑、装配及编解码职责继续独立。拆分净增 17 行包含和命名空间，计作可读性整改，不计作代码缩减。

## 主要产物与消费者

完整目录合同见专项计划，此处记录实际交接。正式运行不靠人工移动或硬链接去重。

| 产物 | 生产者 → 消费者 | 保留与清理 |
| --- | --- | --- |
| inputs/project、配置、请求 | C++ prepare → Python 快照 → 规划/采集/预测 | 一份正式输入；修订先记录 pending input 并使旧结果失效，再发布输入 |
| run.json | 主 workflow → status/resume、摘要 | 阶段、身份、累计预算、资产引用的父账本，不复制原生内部状态 |
| attempt_input/status、launch_result | 宿主 launcher / PID 1 → reconcile | 分别说明启动参数、实际终态、launcher 结果；同一尝试只计费一次 |
| work/prepared_input、processing/fitting/prediction 收据 | 对应生产者 → 父任务确认 | 交接前保留；确认 manifest/结果后由所属 workflow 清理，终态没有 work 目录 |
| captures/.../prepared_input | 原生准备 → worker、normalizer、模板/成本生产 | 本次实际 token 化参数与部署上下文；不是第二份可变 project 状态 |
| source_facts、compiled_templates | normalizer → 提取/编译 → 动态展开 | 每来源各一份；图/实例 JSON 是编译中间物，默认清理，事实可重建同字节 CBOR |
| device_measurements、cpu_worker_units、候选 | 独立性能采集 → 拟合 → 查询 | 样本每来源一份，非空类别才写候选；本次 4 次采样、10 份候选 |
| vendor 导出及结构/设备 journal | 原生观察 → normalizer | 事实/资产交接后释放所属原始包；保留的定位符是来源身份，不再是可读取输入 |
| transfer 原始 queue/scopes、worker 环境 | 原生传输观察 → TransferMeasurements、测量边界复核 | 保留约 17.9 MiB queue 记录；其中完整 worker 窗口/关联未全部转存到紧凑单位，供重规范化和边界审阅，非无消费者大图 |
| validation ports、observation_facts | 原生端口 → 规范化 → ServingComparison | 独立评价及端口审阅依据；不进入模板、候选或预测容器 |
| catalogs、prediction、comparison、summary | C++ 需求 / Python 合并与评价 → 用户/恢复 | 两配置共享一份目录，三份固定结果；完成后没有 sampling-plan 和旧轮次全量结果 |
| logs、diagnostics | 实际任务 / ExecutionDetails → 定位或显式诊断 | 每任务一份必要日志；完整图默认关闭，诊断共享来源定义，不重复追加容器日志 |

最终核对 7 份 manifest、82 条正式引用和 5 份 gzip，引用存在且压缩文件可完整读取。来源定位符、退役开发检查的日期路径不冒充正式可读取资产。

## 确认问题与关闭依据

表中“关闭”限本次当前域；安装/范围外能力的源码结论不升级为原生运行资格。证据组 E1–E8 见下节。

| 编号 | 问题与最终处理 | 关闭依据及限制 |
| --- | --- | --- |
| O1 | 路径各自拼装 → RunDirectory 单根、稳定编号和共享身份 | E1/E2：全部新阶段在同根，恢复沿用原根 |
| O2 | 逐轮全量报告重复 → 最新合并结果、共享 catalogs、消费后删收据 | E1/E2/E4：6 轮只保留一份预测，局部重算保留已完成组合 |
| O3 | 图/实例与 CBOR 重复保留 → 默认中间物清理、事实显式重建 | E3：原始包释放后重建，CBOR 及事实字节保持 |
| O4 | 空 candidate 也落盘 → 只发布有效非空候选 | E5：真实 warmup 记录的部分输入检查，四类均 0 entries、无模型文件；不是新采样或性能资格 |
| O5 | 输入、状态和收据重复职责 → 正式输入、实际原生参数、任务/交接收据分开 | 完整生产/消费审阅及 E1/E4；保留各自唯一信息，删除已消费工作副本 |
| O6 | 四类拟合重复解压同份数据 → 共用 Observation 读取 | 源码审阅与 E1 的四类实际拟合；旧等价结果仅作历史记录 |
| O7 | 样本和诊断重复字段 → 去冗余、共享来源定义、实例只携带变化 | E5：业务字段及目录字节保持；当前检查不声称比较了两份默认物理图 |
| B1 | Python 按框架名决定 base 可提取 → C++ TemplateAdapter 拥有能力判断 | 源码及 E1/E3；KT capture-only 分支经审阅，未新跑 KT |
| B2 | 恢复反复追加 Docker 日志 → ModelingJob 覆盖完整快照 | E2/E4/E5：恢复不新增日志内容/文件，交接复用 |
| B3 | 缺样本被归成特征合同不足 → 区分数据缺口与合同缺口 | E1/E4/E5：有样本时完整、去 kernel 时 needs_data，不靠给定耗时补齐 |
| B4 | 无消费者 thread CPU 计时 → Python/native/规范化整链删除 | 全量审阅、构建及 E1 新观察库；墙钟、因果与真实传输保留 |
| B5 | runtime 解释 SGLang 组批/池 → 框架适配注入 KernelWork | 全量依赖审阅及 E1；runtime 保留物理观察，不以搬目录代替边界核对 |
| B6 | 完成数量回退掩盖成员缺失 → 使用实际 completed members | E1 的实际批次/完成覆盖和正式采样规划 |
| B7 | CPU 分类旧字段残留 → 来源、模板、编解码整链删除 | 源码、E1/E3；保留 CPU 因果，不保留旧格式读取 |
| B8 | 框架绑定解释物理 driver/DMA → AscendSourceExtractor 拥有物理描述 | 依赖审阅及 E1/E3；输入 token 语义仍在框架端，不推定其他 runtime 已通过 |
| B9 | 编译清理可能删除借用 JSON → 只清理所属工作目录 | 源码、E3 和借用资产哈希核对；未删除外部资产 |
| B10 | 原生完成与处理完成混淆、原始包重复保留 → manifest/收据/账本分步确认后释放 | E1/E3/E6：结构/设备原始包释放；端口及传输审阅事实按上表保留 |
| B11 | CLI/Docker/采集状态混放 → CLI、ContainerEnvironment、capture 子记录各司其职 | 全量审阅、E1/E2/E4/E6；不新增任务平台 |
| B12 | 独立验证/汇总有入口但缺一键验收 → 正式 explore + 薄示例 | E1：从空资产一条命令完成；Shell 没有解释 DAG 或搬输出 |
| B13 | 不完整预测可能进入验证/沿用成功状态 → 预测完整才固定并验证 | E4/E5/E6：缺成本不验证；真实验证预算失败保留两份固定预测，无空对照 |
| B14 | 新输入可能配旧成功结果 → pending input 与结果失效同账本交接 | E4：连续发布失败时旧输入保留、旧结果失效，公开恢复后新输入生效 |
| B15 | 登记和启动之间中断可能重复原生尝试 → 同身份恢复，实际终态只计一次 | 源码、历史启动阻断记录及 E6 活动/宿主丢失检查；不声称当前重验全部 pre-launch 分支 |
| B16 | 独立采集的暂存清理依赖预测父任务 → collect 确认 manifest 后清理 | 共同 collect 路径审阅及 E1 实际交接；清理分支不读取 prediction 字段，未为分支覆盖额外采集一套模型 |
| B17 | 三个安装脚本只转发 → Dockerfile 直接调用所属适配安装器 | 路径/消费者、Shell 和 Compose 核对；未重建 KT 原生镜像 |
| B18 | KT 手写 YAML 行解析 → 已有 PyYAML | 源码及官方配置只读核对；旧解析等价检查是历史证据，本次不宣称 KT 运行验证 |
| B19 | 结构/设备原始 journal 与事实重复 → 成功交接后释放，诊断/借用保留 | E1/E3：实际释放记录、事实重编及借用资产保持；原始重解释需要诊断采集 |
| B20 | CLI 跨命令参数判断冗余 → 标准子解析和必要跨字段约束 | 源码、E1/E2/E4/E5 的公开入口；非法参数历史检查不冒充本次新原生验证 |
| B21 | explore 手动 compare 沿用旧整体状态 → 随最新对照同步状态 | E2：部分对照 exit 2，完整对照恢复 complete，原生/预测/预算保持 |
| B22 | 观察重复完成数量、传输单元重复 metadata → 删除重复字段 | 全量消费者核对及 E1 实际采样/覆盖/拟合；配方中的期望 count 仍有独立用途 |
| B23 | templates.cpp 职责混放 → 按已有类分文件 | 56 方法原文迁移、构建、E1/E3；不增加类/转发层，不宣称删行 |
| B24 | 活动 explore 只显示 needs_data → 显示已有活动及原因 | E6：真实活动 status/resume 不重启；终态 E2 |
| B25 | `+0800` 触发 profiler 路径拒绝 → UTC `Z`，启动前调用 SDK 自身检查 | SDK 实际验证器及 E1 全部原生角色；失败开发运行单独记账，无自有正则或兼容分支 |

## 当前证据与实际开销

正式运行：[用户摘要](../../../data/20261016T154651.604935Z_sglang-qwen3-32b/results/summary.md)、[预测](../../../data/20261016T154651.604935Z_sglang-qwen3-32b/results/prediction.json)、[对照](../../../data/20261016T154651.604935Z_sglang-qwen3-32b/results/comparison.json)。开发证据集中在 `data/20261016T231044.871632+0800_structure-review/`，其 `results/` 保存以下证据；全量开发运行已退役，日志、必要输入、观察及哈希保留，不把退役目录当作可恢复任务。

| 组 | 证据文件 | 实际证明 |
| --- | --- | --- |
| E1 | acceptance_monitor.json、native_base_handoff.json、artifact_inventory.json、validation_isolation.json | 公开示例 exit 0；1 base、4 performance、2 validation、6 轮预测、10 候选；验证前后预测及候选字节保持，所有预测/拟合挂载不含验证 |
| E2 | terminal_comparison_recovery.json | 多次终态恢复文件字节、轮次、尝试和预算保持；部分/完整手动对照正确切换状态 |
| E3 | base_rebuild.json、borrowed_assets_baseline.json | 原始包释放后约 216.9 秒重建；CBOR SHA256 相同，事实、manifest 保持，中间 JSON 清理 |
| E4 | input_revision_recovery.json、partial_prediction.json | 两次真实输入发布阻断、旧结果失效、解除故障后恢复、缺 kernel 及恢复成本；无原生采集；正常运行保留未重算组合 |
| E5 | diagnostic_equivalence.json、empty_candidate_publication.json | 诊断与默认业务字段相同、catalog 字节相同；warmup-only 部分输入不落空候选。开发输入首次漏掉 coverage 字段，修正输入后公开恢复，不据此添加产品检查 |
| E6 | active_resume.json、host_loss_recovery.json、validation_failure_recovery.json | 活动恢复；真实宿主 SIGKILL 后同一原生尝试沿预算退出且只计一次；验证预算失败不重建/拟合，额度耗尽后不再尝试 |
| E7 | include_boundaries.json、template_responsibilities.json、profiler_path_validation.json；logs/build.log、format.log | 依赖方向、方法迁移、SDK 路径，以及 C++ Release/148 份格式、56 份 Python、Shell/Compose 检查；静态检查不证明运行正确 |
| E8 | source_scale.json、development_usage.json、development_cleanup.json、documentation_check.json | 同口径规模、开发开销/清理、引用检查；不把开发检查算成用户运行 |

正式示例墙钟约 **1689.0 秒（28.2 分钟）**。原生累计 base **192.056 秒**、performance **687.342 秒**、validation **310.528 秒**；离线所属容器合计 **473.387 秒**。每 10 秒采样的最大观察值：原始文件约 481.5 MB、派生约 678.0 MB、实际磁盘约 789.0 MB；分别发生，不相加，不称连续峰值。全部在专项初始额度内。

开发失败另计：监控启动中断 7.362 秒，修正 UTC 前的 profiler 路径失败 172.008 秒，宿主丢失检查 8.084 秒，验证启动预算检查 3.466 秒，共约 **190.920 秒原生时间**。验证检查的首次显式恢复合法使用剩余额度重试，随后累计额度耗尽；初始“不能增加尝试”的开发断言过严，已按实际合同记录，没有修改产品来迎合断言。开发离线处理见 E8，与正式示例分开；开发磁盘峰值未测。

## 规模、实际精简与剩余边界

物理行含空行/注释，统计后缀一致。开始时已是脏工作区，Git HEAD 与专项开始盘点不能混用；精确数据见 source_scale.json。

| 自有代码范围 | Git HEAD 文件/行 | 专项开始文件/行 | 最终文件/行 |
| --- | ---: | ---: | ---: |
| src/markov（C++） | 131 / 22,545 | 139 / 24,535 | 143 / 24,634 |
| src/collection（C++/Python/Shell） | 50 / 8,392 | 57 / 9,481 | 64 / 10,330 |
| scripts（Shell） | 3 / 65 | 未单独记录 | 2 / 67 |
| example（Shell） | 0 / 0 | 未单独记录 | 1 / 64 |
| docker（Shell） | 4 / 131 | 未单独记录 | 1 / 116 |

前两个目录合计相对专项开始 **增加 948 行**；一键探索、目录/环境职责、独立验证、摘要和可恢复交接增加了必要实现，不能声称总体代码量缩减。实际删除了无消费者 CPU 计时/分类、安装转发、CLI 重复判断、重复样本字段与日志追加、重复解压及已消费全量产物。新增机制对应本次明确交付，没有兼容层、独立 test 工程、通用缓存/任务平台，也未更改 `.clang-format`。

默认产物从旧记录的 **258 路径 / 约 1.13 GiB 实际占用**变为 **128 文件 / 181.16 MiB 实际占用（180.82 MiB 逻辑量）**。旧数据在 23:04 被外部清除，旧数值只有历史记录，不能给出精确复测差值。新流程自动清理，不包含旧人工硬链接收益。大文件及消费者逐项见 artifact_inventory.json：125.5 MB 编译资产、39.6 MB 事实、两份约 1.7 MB 设备样本和四份约 4.7 MB 传输 queue 记录均有保留理由。

诊断例额外生成约 169.2 MB 图及 18.5 MB details，含 18 份共享来源定义、17 次 work；属于显式开发诊断，不计默认输出。检查后删除整个开发运行及两份失败运行、无消费者旧 CPU operator 构建，保留小型证据与必要输入，未删除正式运行、借用资产、镜像或其他任务容器。

当前确认的结构问题已关闭。尚存的是有边界的限制：性能精度未合格（两配置约 −99.26% / −95.39%）；跨时钟关联和观测扰动未获一般资格；累计图扫描/区域可达性有计算量债务，当前域未证实结构错误；KT 预测、其他模型/TP/backend、更复杂请求与原生控制器未扩展。当前运行及故障检查不覆盖全部历史失败分支，不声称精度、一般结构等价或整棵目录跨机器迁移通过。

最终交付及后续方向统一在[里程碑](../2026-10-08_redesign_execution_log.md)和[主计划](../2026-10-08_redesign_execution_plan.md)维护，不再增加并行总结。
