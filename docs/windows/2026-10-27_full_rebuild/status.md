# 全量重构当前状态

更新日期：2026-10-30。开发 goal 继续，R1/R2 已完成限定范围交付；精度未通过，整体重构未完成。
工作目录为 /home/markov-rebuild，分支 rebuild。R1 核心/工作流为 6cc01e4、ee1350b，R2 以 3709327 为起点，按成本和用户流程两层收尾。
老项目与 markov-next 未修改；当前 data/r1 均由本项目重新采集，没有复制旧 trace、数据库或成本包。

## 阶段资格

| 阶段 | 当前资格 | 后续交付 |
| --- | --- | --- |
| 既有 0/A 底座 | 已交付 | 输入预览、共同合同和环境保留 |
| R0 复盘整改 | 与 R1 合并交付 | 完整请求优先、大 base、小校准、粗粒度程序 |
| R1 完整 SGLang 预测 | 限定范围交付 | self 与两个 cross 已运行；精度未通过 |
| R2 用户流程与共享准备 | 限定范围交付 | 新 base、空库、整组补采、恢复、复用已运行；仅授予已测区域/备份范围 |
| R3 保留框架与关键分支 | 未交付 | KTransformers 真采集/预测、NodeScale、静态诊断和必要缓存分支 |
| R4 实验与资源搜索 | 未开始 | 共用预测器的矩阵和有实际取舍的有限搜索 |
| R5 完整回归与清理 | 未开始 | 自有 60/186 格、三类报告、正常精度和同口径效率 |

## R1 首份完整结果（历史成本定义）

部署为 Qwen3-32B/BF16/TP2、Ascend 910B3、SGLang eager/FIA；串行 3 请求，每个 448 输入/4 输出，
共享 384-token 前缀。第一个请求连续执行但不在正式窗口内；以下为后两个请求从首发到末响应的完整 HTTP，不扣 gap、未归属控制或计算。

| 条件 | 预测秒 | 正常实测秒 | 绝对误差 | 节点/边 | 备份批次 |
| --- | ---: | ---: | ---: | ---: | ---: |
| self / write_through | 3.173056 | 4.332207 | 26.757% | 528 / 1,135 | 3 |
| write_back | 3.171057 | 4.542070 | 30.185% | 501 / 1,073 | 0 |
| write_through_selective | 3.176016 | 4.259374 | 25.435% | 528 / 1,135 | 3 |

三个条件均完成，成本缺口为零；WAPE 为 27.513%，均低估。正常每条件只有一次运行，不是稳定精度或留出成绩。
该表对应 R1 的 first_phase/new_input_size 成本包；R2 已替换准备变量，不能将表中误差视为现行成本模型成绩。
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

R1 Release 和 Debug 构建通过；三条件公开 predict 在隔离容器实际运行，Debug 与 Release 数值一致。
R1 全活动 Python Ruff/格式、本批 C++ 格式与 diff whitespace 通过；采样桥在推理 SDK 环境构建通过，未重复设备采样。
128-token 页的正式预测报 outside_domain、状态 incomplete，不伪造 E2E；正常配置、工作量及采集关闭项已核对。

源细图回放只验证观测成本下的边界。profiler 警告、时钟逆序和未闭合项仍保留；capture_completeness_proven、
dependency_correctness_proven、normal_runtime_accuracy_proven 均未授予 true。
当前活动源码 150 文件/19,832 行（含空行；不含 CMake、第三方和 deprecated），旧基线为 546 文件/88,814 行。
规模小不表示能力已全部恢复，历史 60/186 格不能充作新结果。

## R2 已验证的前半链

predict 可声明已维护的 performance.database，由准备阶段导出现行模型；显式 models 仍优先。
同一 configuration 任务按部署、运行参数和 workload 复用真实 manifest，不再要求用户改配置。
新文本来源把实际 tokenizer 输入记入声明配置，C++ 核对原文与源 token 关系；预测容器不加载 tokenizer。
容器生命周期与主流程分开，同输出有进程锁；共享成本缺口按实际工作归并，不按目标身份拟合。

同一 Qwen3/TP2 部署新采 3 个文本请求，输入分别为 288/288/289 tokens，输出各 4。
一次公开 predict 自动完成 base 采集与三个写策略预测：流程 205.776 秒，其中采集状态记 202.158 秒、865 输入/12 输出、682,659,550 bytes。
重复同命令约 1.30 秒，采集次数仍为 1、用量不增加；旧 R1 configuration 任务也已直接复用。
三个新文本条件均执行完成，未新增正常评价，不能声称新 workload 精度。证据为 data/r2/new_request.json、new_text_base、workflow_checks.json。

已有 manifest 加数据库的三个预测与 R1 时间/图规模逐项一致；独立数据重复导入为 0 新增、936/96 复用，选定模型仍为 19。
两个 128-token 页目标的 6 个重复缺口归并为 3 项，均保留 HTTP=null；修改已有任务输入会在再次采集前拒绝，
显式 manifest 的文本不匹配由隔离预测器拒绝。数据库、设备和 evaluator 均未进入纯预测容器。
本批 Release、活动 Python Ruff/格式和公开入口已验证；未改 DAG 时序或公式，未重复全量 Debug、正常面板或物理性能采样。

## R2 整组共享准备与恢复

同一公开 predict 请求以新文本 base 和空性能库开始，按实际缺口自动完成两种页大小的区域校准与一次物理备份采样，
维护后完成四个目标。未读取 target trace 或正常评价；模型来源全部为本任务独立采样。

| 目标 | 正式完整 HTTP 预测秒 | 节点/边 | 备份批次 |
| --- | ---: | ---: | ---: |
| self / write_through，page=64 | 4.431842 | 528 / 1,135 | 3 |
| write_back，page=64 | 4.427505 | 501 / 1,073 | 0 |
| selective，page=64 | 4.431842 | 528 / 1,135 | 3 |
| write_through，page=128 | 4.404342 | 528 / 1,135 | 3 |

四目标均无成本缺口，Release/Debug 的 HTTP、逐请求端口、图规模与后台完成一致。
重复同一命令约 1.468 秒，base 和性能用量逐项不变，没有补采；这不是冷启动或完整面板的效率验收。
本批没有正常实测，不能判断新成本模型的 E2E 精度。

性能补采累计 3 次启动、16 请求/2,932 输入/48 输出，设备任务 337.523 秒、维护 0.235 秒；
原始性能任务保留约 483 KB，连同库和选择文件约 1.15 MB。19 项响应中，区域系数共用 64/128-token 页锚点。
base 单列：3 请求/865 输入/12 输出，约 206.193 秒；不把大 trace 计入小型性能补采。
证据为 data/r2/automatic_request.json、automatic_checks.json、automatic/base 和 automatic/performance。

实际两次中断宿主驱动：首次暴露 Docker 纳秒时间解析失败及过早移除容器，已修正；
该 base 按真实 Docker die/exit=0 事件人工补记，没有凭产物猜成功。随后再次中断校准驱动，
重跑自动接管同一活跃容器并正常结束，原尝试仍为一次、用量保留。证据在 recovery_checks.json 和 recovery_exit_events.json。
设备前的不足预算检查返回 incomplete，四目标耗时为 null，未启动容器；物理 I/O 重导入 0 新增/48 复用。

准备变量已改为真实配方规格的新准备数；轻量观察记录融合页分配的长度二次幂上界、页大小与指针对齐。
first_phase 仍聚合其他初始化，不能宣称完整编译状态或冷启动精度已解决。旧 raw-length 模型不会被默默兼容。
自动配方当前只覆盖区域和备份；回载、存储、竞争、KTransformers 与 NodeScale 没有因此取得资格。

## 下一阶段起点

先核对本批交付与 R3 范围，补齐保留框架、必要缓存分支及显式静态诊断；仍按完整职责链联合实现。
R4 的上层实验/资源搜索、R5 的新 60/186 格、正常精度与完整效率验收全部保留。
不恢复被终止的大空间研究，不复制旧资产，不修改另外两个项目；前端/control residual 和完成观察的近似仍需如实报告。
