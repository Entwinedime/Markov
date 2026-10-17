# Serving 子区域联合观测计划

> 历史审查/观测记录：结论限定于文中日期与当时实现。2026-10-16 清空数据后，旧 `data/redesign` 地址不再可复核；本文的“当前”“待完成”不作为现在的任务或授权状态。实时状态见[主计划](../2026-10-08_redesign_execution_plan.md)。

状态：首次联合采集已成功；两次失败及成功记录见文末。以下追加准入工作联合观测额度，整体目标仍是完整 S3–S6。

## 目的与必要性

现有资产没有有效准入策略，也没有新增的过滤/合并、Schedule 前缀、请求发布循环和共享输出区域。正式预测已返回 admission_policy_not_observed；源码检查不能补出真实 CPU/设备切口。本次用一次短原生运行联合取得这些事实，不逐个缺口采集。

合同已接到正式生产链：核心 Schedule 参数声明 admission_policy，公共 wrapper 保留该字段；SourceAdmissionPolicy 解析并输出到 source_state，生产端交接 AdmissionPolicy，目标控制器不使用默认值或旧完整输出预约。公开交接证据见 data/redesign/s3-admission-block-contract/verification.json。

## 输入与预期分支

固定 SGLang + Qwen3-32B、TP=2、NPU 4/5（原计划 2/3 已被其他任务占用，执行前改用空闲同型号设备），复用 configs/redesign/sglang/base.json：max_running_requests=2、KV=16384、page=128、冷缓存、FCFS、非 overlap、关闭 radix、非分块。设备空闲情况必须在执行前重新读取；不停止其他任务。

准备文件位于 data/redesign/s3-joint-serving-capture：project.json、workload.json、prepared_input.json、plan.json。Token IDs 复用已有真实 workload，不生成随机 token 或验证夹具。四个请求均在 0 时刻提交：

| 请求 | 输入 token 数 | 输出 token 数 | 机制用途 |
| --- | ---: | ---: | --- |
| page-edge-a | 127 | 6 | 跨页及页内 Decode；保持运行成员 |
| page-edge-b | 128 | 1 | 首批 Prefill 完成，后续过滤删减 |
| slot-reuse-c | 16 | 1 | 中途单请求 Prefill 完成，后续过滤清空 |
| merge-d | 128 | 2 | 非空 Prefill 并入运行批次，随后完成 |

在预期的前两请求首批合批下，共三个 Prefill、五个 Decode 批次，包含全保留、删减、清空、接管和非空合并。原生接收时序可能改变组批；必须按实际记录判断覆盖，不通过强制 batch、改原生决策或反复重采凑齐表格。

## 一次观测的消费者与结束条件

| 观测事实 | 正式消费者 | 本次核对 |
| --- | --- | --- |
| 有效准入参数和首入口比例 | 策略生产、共同控制器 | 两 rank 参数一致、冷启动初值资格；字段未被 wrapper 过滤 |
| 转换/准入/Decode 更新前缀 | SourceExtractor、区域绑定 | 完整 CPU 范围和嵌套过滤身份；Prepare 保持在区域外 |
| filter/merge，包括空/无设备分支 | BatchLayout、区域替换规划 | 成员、调用位置及真实跨区域资源端口 |
| last_loc 聚合构造 | Prepare 区域规划 | B=1/B=2 的 CPU/设备成员，保留 Cat 外部连接 |
| 请求发布循环及 prefix/free 子区域 | 请求槽位继承、Publish 规划 | 循环成员与请求身份一致，子区域归属完整 |
| 共享输出及周期参数 | Publish 共享区域、PeriodicOutputPolicy | 非完成/完成输出切口；不漏掉无 token payload 的发送 |

已有分配/释放、通信和模型结构事实继续复用。本次不覆盖长输出的周期边界、内存压力回退、跨部署通信或独立目标正确性；这些限制继续报告，不能把短请求观测推广到全部目标。完成本次观测不代表有限变换、S3/S4 或 S5 完成。

## 额度与停止规则

已记录 31 次 base attempt 合计 8,005.114298840 秒；最近一次有界取证已结束。以下是自主安排的新增额度，不重置旧记录：

- 最多一次原生 attempt，累计原生墙钟不超过 300 秒，包含初始化、加载、预热、采集和失败。
- 后处理不超过 120 秒；本任务新增产物不超过 1.5 GiB，包含原始记录和处理产物，不包含复用的既有模型/资产。
- 项目 base allow_run=true，依据用户的持续采集授权执行。performance 保持禁用。
- 原生时间由已有 workflow 额度执行；后处理与新增目录大小需运行时监测，现有程序没有统一的这两项硬限制，不宣称已自动强制。
- 超额、原生失败、初值不符或必要区域缺失即结束此次尝试。保留事实与缺口，不自动换 workload、扩大额度或创建第二次 attempt。
- 若仅后处理失败且额度尚余，只恢复同一原始记录；不重新执行原生模型。缺口不得改为默认参数或伪造区域。

若完整使用本次额度，已记录 base 原生累计上界将为 8,305.114298840 秒；实际使用按 run.json 追加，不记作新的“首次采集”。

## 执行与结果检查

执行前重新检查设备与源码/镜像交接，并启用同一项目文件的 base 权限。使用公开入口：

```bash
scripts/markov.sh profile --project data/redesign/s3-joint-serving-capture/project.json
```

跟踪该入口实际返回的 run 目录和进程，不因观察超时重启。profile 的正常链路负责规范化与提取；失败时使用同一 run 的 status/resume，并遵守上限。

完成后先检查上述记录/切口及策略资格，再将此次 role=base manifest 交给正式预测。给定节点耗时重新明确绑定此次来源，只用于结构展开，不作为成本测量。任何用于修改规则的请求归入开发证据；独立结构验收另用未参与修改的请求序列，不将本次原生结果当作目标 oracle。

实际运行 run-20261011-014456-054138 在初始化日志中发现请求发布 hook 错指 Scheduler；实际方法属于 SchedulerBatchResultProcessor。主动停止唯一 attempt，原生累计 84.951368920 秒，无后处理和有效模板。历史累计 32 次、8090.065667760 秒。本次不自动重试；修正正式绑定后先在已安装框架上解析并构造全部 inline hook，后续采集另行说明必要性和额度，无须用户确认。

## 绑定修正后的执行

首次失败尚未执行请求，不能复用为结构事实。已修正正式绑定并通过 Docker 构建和全部 6 个 inline 原生入口解析/插桩构造，故安排一次修正后运行；配置与 workload 保持不变。新增原生上限 300 秒，后处理上限 120 秒，两次尝试合计新增产物仍不超过 1.5 GiB。历史 32 次原生累计 8090.065667760 秒，新累计上界 8390.065667760 秒。使用新 run 以重新生成修正后的 prepared 合同；保留失败记录，不重置累计用量。失败即停，不在本次任务中自动发起第三次运行。

修正后运行 run-20261011-014840-580211 原生耗时 205.444404670 秒，在 Publish 入口给 slots dataclass SchedulerOutputStreamer 实例替换 stream_output 时失败，已停止；没有完整 manifest 或有效模板。历史 33 次原生累计 8295.510072430 秒，本任务两次共 290.395773590 秒，无后处理。全部 10 个组件绑定已按安装类核对，仅共享输出类没有实例字典；应使用已有类级 hook 路径并复用统一记录合同，不修改原生类布局、不为此新增兼容分支。两 rank 均留下首个 Schedule 的完整 admission_policy，entry_ratio=initial_ratio=0.7；这是部分原始事实，不能冒充模板或正向预测验收。证据见 corrected_capture_outcome.json 和 component_binding_audit.json。

## 类级输出修正后的联合验证

15 个正式 hook 目标已在真实安装框架中注册应用，实例替换路径已删除。前两次运行没有完整 Publish，现有原始记录无法补出后续事实，因此安排一次修正后联合验证，配置与请求不变，不增加模板变体。新增原生上限 300 秒、后处理 120 秒，本目录累计产物仍限 1.5 GiB；历史累计上界 8595.510072430 秒。失败即停止本次执行并检查共同机制，不自动追加尝试。执行前已核对 NPU 4/5 无运行进程。

联合采集 run-20261011-015642-333904 成功，原生 187.978900550 秒，后处理上界 68.081062680 秒；整个联合采集目录约 983 MB，包含两次失败记录，未超限。历史 34 次原生合计 8483.488972980 秒。每 rank 8 个批次、3 Prefill/5 Decode，共 64 个阶段实例；新增调度前缀、过滤、合并、last_locations、请求发布和共享输出均有记录。来源仍 needs_data，同步/时钟关系与成本分离缺口保留；成功采集不代表完整目标展开验收。证据：successful_capture_outcome.json。


## 准入工作联合观测（新增一次额度）

此前 34 次原生累计 8483.488972980 秒。来源工作合同已增加候选/请求计数、队列整理、原生退出事实、请求预算内部 horizon 计数及 Prepare 后统计区域；已有资产没有这些事实，无法通过重提取获得。此次复用上表相同四请求、同配置和合法 token，不新增请求组合或模板规则；目的为一次取得这些区域的实际嵌套、工作量和返回记录，并验证原始 JSONL → source_facts → templates 的保存。

调用项目 data/redesign/s3-admission-work-capture/project.json。最多一个原生 attempt、300 秒，后处理上界 120 秒、新目录上限 1.5 GiB，沿用前述监测与停止方式。累计原生上界 8783.488972980 秒，初始化、失败均计入。本次不以预期批次数强制调度；缺必要观测或失败即结束并诊断，不自动重试。设备使用已检查空闲的 NPU 4/5，执行前再次确认。

验收读取真实候选/请求计数、嵌套预算区域及其 horizon_checks，检查统计区域在 Prepare 后、等待数来自压缩后的原生队列，两个 rank 对照实际事实。NO_TOKEN/OTHER 保留原始退出含义；早退仍按入口和支持域解释，不编造循环。观测成功不等于统一工作编译、有限变换或独立结构验收完成。成本采集禁用。


准入工作联合观测已成功：run-20261011-025519-092008，单次原生 186.304324590 秒，后处理完成观察上界 92.404632640 秒（包含完成后验证等待，低于 120 秒）。累计 35 次、8669.793297570 秒。两 rank 各 2732 条记录、无失败；每 rank 8 次准入、4 个预算区域、3 个统计区域。30 条相关区域的 binding、入口参数和返回值在原始 JSONL、source_facts、templates 三处一致。统计在 Prepare 后，等待数与实际压缩后的队列一致。

本次首批原生实际收到四请求，候选/请求检查为 3/2、horizon 总计 3、容量停止。与此前首批收到三请求的差异来自实际接收时序，不代表三→四的 CPU 工作变换已实现。产出 64 个阶段实例、5400 个区域，readiness=needs_data；未用本次结果放宽目标资格。证据 data/redesign/s3-admission-work-capture/{raw_work_verification,capture_verification}.json。没有第二次 attempt。

## Idle 与 worker 控制事实联合观测

既有 35 次原生累计 8669.793297570 秒。历史库存虽有空决定，但没有当前区域、准入状态和 worker_controls，无法通过重提取得到这些事实。本次同时验证统一 Schedule 参数筛选、worker 内实际控制开关、空 update 早退的 CPU 切口、Selected 返回端口及下一轮准入；不针对每个字段分别采集。

配置与前次一致，使用 data/redesign/s3-idle-worker-capture/project.json。沿用四个请求，再复用已有 token IDs 加入 drain-e（16 输入、4 输出）与 after-idle-f（127 输入、6 输出），六请求均在 0 时刻提交。预期 A 的剩余 Decode 与 E 同批完成，而 F 仍等待，使容量关闭入口之后发生一次清空 running 的 Idle；F 随后提供恢复准入及单请求 Decode/完成释放。预期 15 个计算批次加一次 Idle，但以实际记录判断，不强制原生合批或因顺序差异重采。新增请求用于覆盖有等待者的清空→恢复机制，同时保留先前过滤、merge、跨页和发布机制，不枚举长度库。

最多一次原生 attempt、360 秒，后处理最多 180 秒，新目录合计不超过 3 GiB；依现有监测方式执行，失败、缺事实或超限结束本次尝试，不自动重试。累计原生上界 9029.793297570 秒，最多第 36 次。NPU 4/5 已检查空闲，执行前再核对。成本采集禁用。完成原生采集后沿同一 run 后处理，不重跑模型。

验收按 rank 检查 worker_controls 在原始记录、source_facts、templates 中保持；Idle 有非空 running 输入、空过滤结果、无 Prepare、Selected=NativeReturned，transition 与 update 过滤不混合，下一次准入保留正确比例与容量标志。纯 CPU 空 update 才授予参数域，其节点和依赖保留，成本由后续性能单元处理；不将其时延记零。所有新增字段、区域资格和受限展开在正式路径核对。来源仍可能存在依赖、Publish 泛化等缺口，本次观测不承诺完成 S3/S4 或独立验收；用于开发后不能充当留出目标验证。

本次 run-20261011-043814-242543 成功，原生 209.850832740 秒、后处理完成观察上界 142.108228280 秒，采集目录 1720 MiB，均未超限；累计 36 次、8879.644130310 秒，无第二次 attempt。两 rank 各 5126 条观测全部 completed，各 16 次 Schedule，包含一次有等待请求的 Idle，随后接纳 after-idle-f；Idle 前后的 entry_ratio 均为 0.6949833333333332，符合不衰减、不重置路径。实际 waiting/running timeout 均 -1 秒，TEST_RETRACT 与 require_mlp_sync 均 false。32 条 Schedule worker_controls 已逐条核对 raw → source_facts → templates 一致。

生成 122 个阶段实例、10130 个区域。两个 Idle 实例均为 6 个 CPU observation 加 9 个 CPU control，Selected=NativeReturned，没有 Prepare/设备计算。source readiness 仍 needs_data，不等于整体结构已验收。证据 data/redesign/s3-idle-worker-capture/{raw_idle_verification,capture_verification}.json；固定资产的条件性展开结果另见实施计划及执行记录。

## 首次完成输出的工作事实联合观测

已有 36 次原生累计 8879.644130310 秒。30 个历史 batch_output 区域只有入口模式和范围，没有实际发送 offset、有效解码上下文上限、socket 或可选载荷状态；无法通过重提取得到。现已复用同一 wrapper/区域记录这些事实，并接入统一 PublicationParameterLayout；不是为 2/6 个输出 token 各建一个模板。使用 data/redesign/s3-output-work-capture/project.json，配置与前次六请求 workload 完全复用，不新增请求组合；同时覆盖不输出、首次结束输出、两 rank 发送状态与已有 Idle/释放机制。

自主新增最多一次原生 attempt、360 秒，后处理 180 秒，新目录 3 GiB；成本采集禁用。上限含初始化、预热和失败，累计原生上界 9239.644130310 秒，最多第 37 次。原生由现有预算控制，后处理和目录大小按前述方式监测，不宣称后两项已有程序硬限制。当前 NPU 4/5 无运行进程；执行前复核。失败、超限或关键事实缺失结束本次尝试，不自动重试。

验收逐 rank 核对 output_work 在 raw/source_facts/templates 中保持；用 before/after offset 验证首次输出的生成 token、prompt 尾部及解码列表数量，不记录 token 内容或猜测序列化字节。消费者仅授予已知单请求、首次完成、普通生成、非流式、无可选载荷的输出 CPU 区域，保留原节点/端口。来源参数正确后以显式节点耗时展开，检查目标输出工作与释放 DMA 的实际值；整请求与独立结构验收仍另行进行。此次作为开发资产，不作为留出评价。

本次 run-20261011-053447-612772 成功：原生 206.824225620 秒，后处理完成观察上界 111.955936830 秒，目录 1719 MiB，均在预算内。累计 37 次、9086.468355930 秒。两 rank 各 5126 条原始记录全部 completed，各 15 个输出区域；30 条 output_work 已核对 raw → source_facts → templates 完全一致。产出 122 个阶段实例、10130 个区域，来源仍 needs_data。rank 0 sender_enabled=true、rank 1=false；单请求最终输出的 send_token_offset 为 6、send_decode_id_offset 为 11，解码上下文上限实际为 5、输出周期为 50，其他新增模式均在声明普通范围。

真实 before.finished_output 为 null，表示 Req 尚未输出；已按原生 None/False 的未输出含义修正生产端解释，未重采或改写记录。证据 data/redesign/s3-output-work-capture/{raw_output_work,facts_verification,capture_verification}.json。此处仅确认观测链，目标输出参数和端口另由公开 predict 验证。
