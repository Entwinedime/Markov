# 当前状态

更新时间：2026-11-11。

当前范围：按用户最新指令，先完成一个 CUAD alternating 场景：16 份合同、512 条正式请求、1,296 个配置及最好/最差两个正常端点。另外两组已生成输入保留，暂不运行。十二小时目标和历史消耗不重新计时。

## 阶段资格

| 阶段 | 当前资格 |
| --- | --- |
| A：数据、声明与资源实现 | 已分层提交推送；本次 base 已验证 TP 配额生效 |
| B：一份 base 与完整枚举 | 全部完成并分层推送，远端确认 aa7a007 |
| C：两个端点正常测量 | 两点均完成并核对，待文档交付 |
| D：结果与交付 | 待做 |

## 已有资产与证据

A 从 ea4ec8e 完成校准、资源能力及例子三层提交，远端确认 b0c3c84。三组 CUAD 输入均已准备，当前仅选择 alternating；settings.scenes 明确限定，其他两组没有启动设备任务。

共享校准完成 8 次启动、102 HTTP 请求，保留 39 类响应、更新 26 类，45 个来源可追溯；Prefill/query 到 7,200、Decode context 到 7,203，load prepare 到 225 页。所有套餐共用独立只读模型，关闭自动补采。失败回载仍只覆盖 1–2 段；独立成本范围不是 E2E 精度证明。

当前 base 的 520 条请求全部完成，其中 512 正式、8 准备，共 2,808,032 输入、16,448 输出 tokens；逐请求输入和定长生成、完成原因、准备排空均已核对。两 rank 资源声明一致，七个准入槽实际使用，496 条正式请求报告文件缓存命中。正式带采集 HTTP 约 3,970.03 秒，不是正常真值。

采集在完整 HTTP 结束后的日志检查阶段触及原容器时间额度，退出码 124 及 4,217.56 秒用量保留。随后在无设备、1 GiB modeling 容器中复用正式 publish_manifest 完整检查并离线导出，耗时约 111.24 秒；首次宿主 Python 3.9 导入不兼容的失败及耗时也保留。来源累计额度由 4,200 调整为 4,800 秒，只用于导出恢复，没有新增启动或请求。私有 KV 已在成功导出后清除，当前来源保留约 5.43 GiB。

证据：数据根目录 data/prefix_cache_search_20261110；preflight/base_qualification.json、source_export_recovery.log、scenes/alternating/run/base/capture_state.json 和 captures/1/profile_manifest.json。代表 11 格、原生文件配额及构建证据仍在 preflight；它们不作为本轮全量或精度证明。

## 当前结果与恢复起点

两个分区均已完成并经正式 SearchPolicy 合并：1,296/1,296 全部 predicted、全部 ranked，无结构或成本缺口，七项计费齐全；同预算候选 1,242 个。全部 24 个串行代表的预测内容（除运行计时）与分区结果完全一致。端点清单在正常测量之前保存，当时目标真值读取为零；C 只按清单观察，不重新选点。

预测最好为 load_bandwidth_756（回载带宽优先、page128、write_through、wait_complete、threshold128），3,076.465 秒、426.921 费用单位/小时；预测最差为 backup_bandwidth_905（备份带宽优先、page64、write_back、timeout2秒、threshold128），4,181.655 秒、420.317 单位/小时。二者均低于基准 457.394 的小时预算。最好有 6 个同耗时/同费用并列项，最差有 3 个；按预先声明的名字顺序选点，confirmation_plan.json 及两个 normal 请求在正常观察前保存，后续未改模型或重选。

逐目标仿真平均 16.352 秒、中位 13.005 秒，合计 CPU 21,192.244 秒；两次来源准备合计 482.243 秒，最长 worker workflow 为 11,339.030 秒。完整汇总在 15:06:24 UTC 已观察正常退出，约 3 小时 12 分钟含部分观察余量，原两小时目标未达到。核心报告 RSS 最大约 7.65 GiB（容器各限 12 GiB），汇总报告约 585.72 MiB，外围精确结束时间未单独埋点；生命周期及逐会话计时保留，不把两次准备称为两份不同 base。

证据：preflight/full_scan_qualification.json、preflight/prediction_lifecycle.json、scenes/alternating/run/prediction/report.json、run/search/report.json 和两个分区的 workflow.log。短时 CPU 采样只记录遍历与重复检查线索，没有为此重启。最终审查把完整 JSON 的校验集中到汇总所有者，删除进程等待阶段的重复解析；用实际两份 648 格重新汇总，完整排名保持一致。

B 已按建模（5510a9d）、采集（8857f45）、例子（08e55d0）和文档（aa7a007）四层提交推送，远端 OID 已核对。C 已按保存清单串行测量最好、最差各一次，句柄 89061 正常退出 0；其他两个 CUAD 场景未启动。

C 两点完整正式 HTTP 为 3,300.630 / 4,242.311 秒，APE 为 6.792% / 1.430%，WAPE 3.776%；实际改善 22.197%，加速比 1.285，排序一致。每点只有一次观察，不能认证波动、显著性或真实全局最优。两个正常任务分别运行 3,470.166 / 4,409.856 秒，均在原额度内；各 520 请求、2,808,032 输入和 16,448 输出 tokens，私有 KV 均已清除。

三次主运行的实际 workload 完全一致。两点原生启动的设备4/5、CPU16–23、8 CPU 和 128 GiB 均核对；双 rank 配额与声明一致。实际 profiler/calibration/region/semantic 均关闭，探针和 LD_PRELOAD 标志为空。逐请求顺序、定长输出、完成原因、准备排空及完整 HTTP 均通过；响应分别报告 496 / 31 次文件命中、2,650,624 / 128,960 缓存 tokens，与固定预测的重算工作量吻合。

证据为 confirmation/results.json、两个原始 normal 目录、preflight/normal_qualification.json、normal_launches.json 及 endpoint_predictions.json。累计设备 11 次启动、1,662 请求、9,028,431 输入和 49,536 输出 tokens，失败及恢复用量保留。运行中观测约 39.8 GiB，两个正常任务清理后本轮目录约 7.84 GiB；空间观测在 preflight/storage_observations.json，是峰值下界而非连续高水位。

下一步完成 C 的文档检查和提交推送，再进入 D，补齐完整空间图表、两点七项费用/配置、工作量解释与全流程开销报告。D 未完成，当前不能关闭窗口。
