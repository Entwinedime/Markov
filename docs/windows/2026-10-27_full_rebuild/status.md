# 全量重构当前状态

更新日期：2026-10-30。开发 goal 继续，R1 已得到首份完整正式 HTTP 与独立正常对照；精度未通过，整体重构未完成。
工作目录为 /home/markov-rebuild，分支 rebuild；本阶段从 1047aaa 起审查全部工作区改动，代码、文档和数据统一检查后分层交付。
核心与工作流分别提交为 6cc01e4、ee1350b，本批文档整理随后提交；推送完成后作为 R2 起点。
老项目与 markov-next 未修改；当前 data/r1 均由本项目重新采集，没有复制旧 trace、数据库或成本包。

## 阶段资格

| 阶段 | 当前资格 | 后续交付 |
| --- | --- | --- |
| 既有 0/A 底座 | 已交付 | 输入预览、共同合同和环境保留 |
| R0 复盘整改 | 与 R1 合并交付 | 完整请求优先、大 base、小校准、粗粒度程序 |
| R1 完整 SGLang 预测 | 限定范围交付 | self 与两个 cross 已运行；精度未通过 |
| R2 用户流程与共享准备 | 待开始，已有采集/维护子链 | 自动复用、整组工作域与有界准备、初始化工作定义及完整链效率 |
| R3 保留框架与关键分支 | 未交付 | KTransformers 真采集/预测、NodeScale、静态诊断和必要缓存分支 |
| R4 实验与资源搜索 | 未开始 | 共用预测器的矩阵和有实际取舍的有限搜索 |
| R5 完整回归与清理 | 未开始 | 自有 60/186 格、三类报告、正常精度和同口径效率 |

## 首份完整结果

部署为 Qwen3-32B/BF16/TP2、Ascend 910B3、SGLang eager/FIA；串行 3 请求，每个 448 输入/4 输出，
共享 384-token 前缀。第一个请求连续执行但不在正式窗口内；以下为后两个请求从首发到末响应的完整 HTTP，不扣 gap、未归属控制或计算。

| 条件 | 预测秒 | 正常实测秒 | 绝对误差 | 节点/边 | 备份批次 |
| --- | ---: | ---: | ---: | ---: | ---: |
| self / write_through | 3.173056 | 4.332207 | 26.757% | 528 / 1,135 | 3 |
| write_back | 3.171057 | 4.542070 | 30.185% | 501 / 1,073 | 0 |
| write_through_selective | 3.176016 | 4.259374 | 25.435% | 528 / 1,135 | 3 |

三个条件均完成，成本缺口为零；WAPE 为 27.513%，均低估。正常每条件只有一次运行，不是稳定精度或留出成绩。
预测与评分物理隔离，评价未 refit、改 readiness 或回写参数。来源、模型、正常配置/输出数量已逐项核对。
证据：data/r1/application/prediction/report.json、data/r1/evaluation.json；Debug 的三个时间、图规模及备份数与 Release 一致。

## 实际闭合的逻辑与规模

- 普通来源准备只读取 manifest 指定的语义观察、必要同步和客户端，不读两份大型设备 trace；Regions 保留区域引用，细图按需建立。
- RegionCompute 分开 CPU、caller 返回与流进度，输出就绪约束结果整理，最终发送与尾部/额外提前计算分开；复用 CacheProgram、caller 调用权和共同执行时钟。
- 后台备份不再绕过 caller 调度；回载和写回淘汰保留准入调用权。R1 只实际验证三种写策略，未触发回载/压力等分支。
- 独立数据导出 16 个区域响应，加 3 个物理 I/O 响应，共 19；结果 CPU 明确匹配 HiRadixCache，不使用早期 RadixCache 样本代替。
- 源控制 residual 保留不明工作与等待，仅扣已识别采集开销；父区域内等待由目标端口表达。未解释成本没有记零或删去。
- 正式 predict 使用只挂载程序、base、模型、准备输入及输出的无设备/无网络容器，不挂载仓库或 evaluator。

本次来源准备 0.829 秒、43,045 条记录，进程峰值约 17.68 MiB；每目标仿真 1.16—1.73 毫秒。
同 base 的全通道读取基线为 12.254 秒、201.56 MiB，来源读取约快 14.78 倍、进程 RSS 约降 11.40 倍。
这仅是来源读取的单次对照，不是完整冷启动、60/186 格或全链 10× 验收。

## 自有资产与用量

| 资产 | 位置 | 实际用途与规模 |
| --- | --- | --- |
| 输入 | data/r1/inputs、data/r1/prediction_request.json | base、独立校准、正常测量及预测的复现输入 |
| Base | data/r1/base/base/captures/1/profile_manifest.json | 3 请求/1,344 输入/12 输出；约 200.56 秒，保留 683,074,024 bytes |
| 区域校准 | data/r1/calibration/calibration/ | 3 次启动，39 请求/13,440 输入/156 输出，累计 464.37 秒；保留约 1.10 MB。第三次匹配 HiCache 分支 |
| 物理备份 | data/r1/cache_io/ | page=32/64、pages=1/2/4/8；一次启动失败与成功累计 64.52 秒，约 19 KB |
| 数据库/成本 | data/r1/performance.sqlite、data/r1/costs/ | 库约 1.17 MB，两个导出约 192 KB；第三次区域采集有 936 个计时量，不是 936 次独立实验 |
| 正常对照 | data/r1/base/normal、data/r1/normal_targets/ | 三个条件分别测量，关闭 profiler/Markov 探针；仅 evaluator 使用 |
| 源细图诊断 | data/r1/source_report.json | 2,299,949 节点/2,589,245 边，正式静态回放 5.962719260 秒，逐请求与 profiled 观察零差 |
| 效率与缺口证据 | data/r1/source_intake_comparison.json、data/r1/application_missing/ | 同源读取对照；128-token 页工作因 3 项域外响应停止，HTTP 为 null |

性能设备采样累计约 528.89 秒，含失败启动；另有少量未单列计时的维护/后处理。
性能原始数据、库及模型合计约 2.49 MB，明显小于 256 MiB 预算；这不是完整峰值测量，也不把 base/正常评价转记为性能采集。
现有原始事实、早期不匹配样本与失败记录均保留，不清零或删证据。

## 检查与资格

Release 和 Debug 构建通过；三条件公开 predict 在隔离容器实际运行，Debug 与 Release 数值一致。
全活动 Python Ruff/格式、本批 C++ 格式与 diff whitespace 通过；采样桥在推理 SDK 环境构建通过，未重复设备采样。
128-token 页的正式预测报 outside_domain、状态 incomplete，不伪造 E2E；正常配置、工作量及采集关闭项已核对。

源细图回放只验证观测成本下的边界。profiler 警告、时钟逆序和未闭合项仍保留；capture_completeness_proven、
dependency_correctness_proven、normal_runtime_accuracy_proven 均未授予 true。
当前活动源码 146 文件/19,116 行（含空行；不含 CMake、第三方和 deprecated），旧基线为 546 文件/88,814 行。
规模小不表示能力已全部恢复，历史 60/186 格不能充作新结果。

## R2 从哪里继续

先沿这条完整预测改善用户准备，而不是补原生诊断的逐项缺口。首次 configuration 能自动复用返回的 base，
文本输入能在准备边界规范成真实 token 工作，已有性能数据按整组需求选择，确需采集才产生有界共享配方。

初始化须先明确实际工作。独立数据中某些“新长度”有约 2.6—2.9 秒准备，另一些只有约 3 毫秒，
现有 first_phase/new_input_size 不能可靠区分。先查真实准备规格/执行分支与初始状态，不能依据这三格残差加补偿或逐长度参数。
同时保留源前端/control residual、竞争和 I/O 完成观察的限制；R1 数值不是精度完成证明。

R3—R5 全部欠项保留；不恢复被终止的大空间研究，不复制旧资产，不修改 markov-next。
