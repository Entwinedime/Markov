# KT + DeepSeek-V4-Flash 运行复现窗口

开始日期：2026-11-13。

目标：按上游新的 Ascend 单 NPU 流程替换原有 KT 链路，让 DeepSeek-V4-Flash 完成加载并通过真实 HTTP 请求返回非空生成；不做准确性、吞吐量或预测精度验收。

## 范围与实现原则

- 新部署使用 SGLang 的 DeepSeek-V4 分支承载服务、KT-Kernel 承载 CPU MoE，NPU 使用 W8A8、CPU 专家使用逐层 MXFP4 GGUF；不能沿用旧 dense/Torch 包装器冒充复现。
- 完整删除旧 KT 专属实现、调用方、注册、配置、构建补丁及失效说明，以新流程覆盖，不保留旧入口、双路径或兼容 fallback。通用基础设施及独立 SGLang/HiCache 能力继续保留。
- 直接复用上游 `dsv4_env.sh`、`setup.sh`、`serve.sh`、`verify.sh`，项目只维护必要的依赖固定、容器装配、环境示例和可重复运行说明，不复制上游服务端或再建部署平台。
- 本窗口只交付运行能力。旧 KT 的 dense 来源、阶段成本和预测路径随替换删除；新的 DeepSeek MoE profiling、calibrate、predict、replay 与成本建模不在本窗口实现，受影响的公开入口须明确报告不支持，不能套用 SGLang dense/HiCache 规则。
- 单 die、TP=1、EP=1、一次一个请求；关闭 prefix cache、shared-expert fusion 和可选 streaming prefill，先使用教程短提示配置。不扩展长上下文、并发、其他模型或硬件。
- 不运行 evalscope、GPQA、吞吐 benchmark、矩阵、性能采样、profiler 或 snapshot。文件完整性与量化格式检查属于加载前置条件，不等同于模型准确性验证。
- 本次仅编制和交付计划；后续执行从阶段 A 开始，不在计划编制期间下载、构建、删除旧路径或启动设备任务。

## 上游依据与固定方法

主要依据为[Ascend 教程](https://github.com/kvcache-ai/ktransformers/blob/main/doc/en/DeepSeek-V4-Flash_tutorial_for_Ascend_NPU.md)及同仓库 `kt-kernel/tools/ascend_dsv4/` 脚本。
规划参考提交为 [`ca198c50de70abb6827ba6f573bb6671e54436e2`](https://github.com/kvcache-ai/ktransformers/commit/ca198c50de70abb6827ba6f573bb6671e54436e2)；执行时固定完整提交，而非持续追随 main。

| 组件 | 计划采用的依据 |
| --- | --- |
| KT | 官方仓库中的 KT-Kernel 及 Ascend DSV4 工具；初始化所需 llama.cpp、pybind11 子模块 |
| SGLang | `Pan-Boyi/sglang` 的 `dsv4-cann9-no-patch` 分支，执行前解析并固定完整提交；与现有独立 SGLang 依赖隔离 |
| 推理镜像 | 教程的 `quay.io/ascend/sglang:main-cann9.0.0-910b` 作为候选，记录并固定实际 digest；按 probe 结果确定已有算子，不能靠移动 tag 判断 |
| 软件栈 | CANN 9.0.0、Python 3.11、匹配的 torch/torch_npu；教程验证组合为 2.9.1/2.9.1 或 2.10.0/2.10.0，GCC 至少 11 |
| 驱动 | 教程要求 25.5.1；现有版本能否运行须由阶段 A 检查，不能仅改版本字符串或忽略运行错误 |
| 自定义算子 | 缺少时按固定版本上游 setup 构建；教程给定 cann-recipes-infer `1c8e6bcc2333d95b3db47d873210f921113d6d11`、ops-transformer `8edcd591e83e536e9ee98a9ce0de3af02ea4f3ea` |

优先使用含完整算子的镜像。依赖安装走 NPU requirements，保留镜像匹配的 torch/torch_npu，不运行会引入 CUDA 依赖的默认 SGLang editable install。上游构建会改动的依赖副本放在容器或忽略的构建区，仓库官方 third_party 工作树不打本地补丁。

## 阶段 A：环境、驱动与下载资格

任务：

1. 核对开发分支基线和已有改动，盘点旧 KT 的生产者、消费者、构建及公开入口，确认替换边界；暂不删除旧实现。
2. 只读确认宿主 driver/firmware、NPU 型号和单 die HBM、空闲设备、CPU 架构/NUMA、可用 RAM、Docker/Ascend runtime、模型盘和构建盘空间。当前会话看不到设备节点时，先确认实际设备运行环境，不能据此宣称硬件不可用。
3. 获取并固定候选镜像，在选定空闲 NPU 上用新 CANN/torch 组合做无权重的初始化、小张量运算和同步；必要时用小 shape 检查所需算子实际可调用。所有设备检查串行、有超时并回收本次进程树。
4. 查清驱动不匹配的实际影响。若仅缺用户态依赖，按上游方法修复后重试；同一兼容性错误只做一次有证据的修正重试。若 runtime/算子仍不能执行，或必须升级宿主驱动、固件或重启，则暂停，不自行升级宿主环境。
5. 下载前确定 W8A8 的真实来源、revision、文件清单、量化配置及与原生模型/GGUF 的对应关系。教程没有指定唯一 W8A8 仓库，不能凭目录名认定可用；不把其他框架的 W8A8 格式直接当作 compressed-tensors。
6. 确认权重直连路线与磁盘峰值，先用元数据和小文件验证最终下载域名、重定向及下载器的代理设置，合格后才允许大文件传输。

交付与通过条件：固定的环境与来源记录、兼容性检查日志、权重获取方案；新 runtime 在选定 NPU 上真实执行成功，权重来源及非付费代理路线明确。无权重检查通过仅允许继续，不证明完整模型一定能启动。

暂停条件：驱动/设备不兼容且不能在用户态解决，权重来源或格式不能确定，无法保证大文件不走付费代理，或空间/RAM 预算不足。暂停记录失败命令、首个有效错误、已用资源和恢复前提；不先下载数百 GB 权重或以旧路径替代。

## 阶段 B：完整替换旧 KT，准备新环境与权重

任务按一条完整调用链联合实施，避免只换 Dockerfile、保留旧内部实现：

| 影响面 | 必须处理的范围 |
| --- | --- |
| Python KT 适配 | `python/markov/adapters/ktransformers/` 中旧 inference、capture、sampling、measurements 及 Ascend build.patch 全链路删除；只按新运行流程需要落入最小装配 |
| 共享 Python 调用方 | `workflow/capture.py`、`preparation.py`、`performance.py` 的旧适配导入、采样选择与 `ktransformers_phases` 注册；联查 CLI、输入校验、产物消费，旧请求不再进入残留分支 |
| C++ KT 路径 | `src/markov/adapters/ktransformers/` 的 observations、work、prediction 及对应头文件；同步 `adapters/source.cpp`、`adapters/prediction.cpp` 和 CMake target，保留通用执行器 |
| 构建与依赖 | 完全替换 `docker/ktransformers/Dockerfile`、配套 dockerignore 和 archive COPY；调整 KT 上游 URL/固定提交及实际需要的子模块，不覆盖现有独立 SGLang 工作树 |
| 配置与包出口 | 删除 `configs/ktransformers_capture.json` 旧 dense 示例，改用新运行环境示例；联查 scripts、包发现、entry points 及依赖，不为新部署保留旧字段别名 |
| 文档 | 更新 README 与受影响的架构、用户合同、成本、验证说明，撤下旧能力声明；项目历史事实保留历史口径，不改写旧结果为新模型验证 |
| 旧资产 | 旧成本数据、模型、trace、镜像和缓存不自动迁入新运行；解除活动引用，不擅自删除历史数据、归档或他人资源，不用删除本地历史文件调整忽略规则 |

新流程准备：

1. 用固定来源和实际路径装配镜像、源码、环境及挂载。只选择需要的设备；教程 privileged/设备挂载按真实 runtime 要求评估，不能把全部 NPU 暴露后称为单卡运行。
2. 执行 `dsv4_env.sh --show` 和 `setup.sh probe`，分步运行缺少的 deps、kt-kernel、sgl-kernel、cann-ops，记录编译日志和实际包版本。KT wheel 使用上游的 `--no-deps` 路径。
3. 按下文直连策略准备 W8A8、官方 checkpoint 和 43 层 MXFP4 GGUF。默认从官方 checkpoint 转换；已有或社区 GGUF 只有来源、固定版本、清单与哈希可核验时才复用。
4. 限制编译及转换并发，按实际可用 RAM 留余量，避免直接采用 192 核机器的无上限并发；中断保留可续传/可复用产物，失败和重试累计记账。
5. 完成上游权重完整性、逐层 GGUF 路径及格式检查，再执行 `setup.sh check`。只有得到 `PREFLIGHT OK`，并额外确认 NPU 确实可用，才交付可启动环境；脚本打印版本或 import 成功不代替设备执行。
6. 统一审查旧链路删除、包导入、源码引用和文档；在 modeling Docker 内构建受 CMake 删除影响的正式核心，用既有代表 SGLang 输入检查共享入口，不重跑历史精度矩阵。不新增独立测试工程、fixture 或内部自测脚本。

交付与通过条件：活动仓库只剩新 KT 运行路径，旧模块及注册无可达消费者；镜像/源码/权重固定，环境预检通过，必要共享构建与公开入口检查通过。此阶段尚不宣称 DeepSeek 已正常运行。

## 权重与流量安排

| 资产 | 教程给出的约量 | 获取原则 |
| --- | --- | --- |
| W8A8 compressed-tensors | 约 275 GB | 来源先核对，再从可直连的同版本仓库或镜像下载 |
| 官方 checkpoint | 约 150 GB | `deepseek-ai/DeepSeek-V4-Flash`，仅用于 GGUF 转换；若合格 GGUF 可复用则不重复下载 |
| MXFP4 CPU 专家 | 约 138 GiB，43 层 | 优先复用核验合格的已有产物，否则由官方 checkpoint 转换 |

教程的三类权重合计约 570 GB 是粗略值，不是峰值上限。执行前按实际文件字节数加下载缓存、转换临时文件、镜像层、构建产物和保留日志计算峰值；空间不足即停，不删除其他数据腾空间。权重与日志进入忽略的数据区，不提交 Git。

下载必须遵循：

- 先盘点当前机器已存在的合格文件，不重复下载、不从其他项目建立隐式数据依赖。优先验证可直连的官方仓库、ModelScope 或同版本镜像；社区 CPU-only GGUF 是可选来源，不替代 W8A8。
- 权重下载进程清除大小写 HTTP/HTTPS/ALL proxy，检查下载器、Git/LFS 与配置文件里的额外代理。显式禁用代理，并验证重定向/CDN/Xet 实际连接；只改 `NO_PROXY` 主站域名不算完成。
- **权重大文件使用付费代理的额度为 0；直连失败时停止，不自动回退代理，不开启全局代理重试。** 小型源码、索引及包的代理需求单独处理，不能让代理配置继承到权重下载、转换或服务进程。
- 使用下载器已有的断点续传和文件校验，限制并发；记录来源 revision、文件清单、实际下载字节及路由证据。不得用 Git clone/LFS 无筛选拉全历史或同时下载多套量化。
- 上游环境脚本会清除代理，但这不替代下载阶段的显式控制；服务及本机 HTTP 检查也禁用代理，避免启动 warmup 误走代理。

## 阶段 C：启动、最小功能验收与交付

1. 使用固定环境执行上游 `serve.sh`，保存实际命令、选定设备、日志与 PID。设置有限启动等待，检查权重加载、CPU 专家路径、NPU 执行和 KV 池；不以进程存活或端口打开作为成功。
2. `/health` 与 `/health_generate` 返回 200；对 `/generate` 发送一条短提示、最多 16 个新 token，对 `/v1/chat/completions` 发送一条短提示、最多 64 个新 token。要求 HTTP 成功、响应结构完整、生成 token 大于 0 且生成文本非空，没有对应请求的运行异常。
3. 执行上游 `verify.sh` 并保存逐项结果，其中非空生成检查只用于确认专家实际加载和生成链路；不比较标准答案、不跨机器比文本、不计算准确率。默认使用教程的 NPU graph 路径；若只有 graph 检查失败但实际生成可用，明确记录降级及该检查未通过，不虚报上游全部通过，也不追加性能调优。
4. 正常停止并确认只回收本窗口的服务及子进程，用相同配置再启动一次、发送一条短请求，确认复现命令可重复；交付启动、检查和停止方法，以及最终服务状态。
5. 沿替换影响范围完成代码、文档、数据检查，确认新旧能力声明一致、没有残留旧 KT 引用或把运行通过写成预测资格。记录业务/示例源码净变化、固定来源、真实证据和未验证边界，按职责提交推送开发分支。

完成条件：新流程完成真实模型加载及非空 HTTP 生成，停止/再次启动可复现，旧 KT 链路全面删除，必要共享检查及分阶段维护交付完成。构建成功、probe、dry-run、历史 KT 结果和单独健康端点都不能代替这项验收。

## 执行额度、停止与恢复

- 设备严格串行。初始安排至多 3 次完整模型启动尝试，包含一次复启；累计模型启动及功能检查最多 4 小时，单次启动等待最多 60 分钟、单请求最多 10 分钟。时间仅作为防挂住边界，不作吞吐判据。
- 编译与转换单独记录，初始累计最多 12 小时；同一阶段连续 30 分钟无日志、文件或进程活动进展时先诊断，不将安静等待直接认作失败。下载记录字节和路由，按实际带宽安排，不承诺总墙钟。
- 额度用尽、同一致命错误经一次有依据修正仍复现、必须改变宿主 driver/firmware、资源不足或代理路线无法控制时暂停；保留失败记录、下载进度及产物，不反复重试或静默增加额度。新增尝试须有新事实及明确预算，不能把预算结束写成复现成功。
- 每阶段开始复核上一阶段已推送提交及现实环境，收尾完成关联审查和代码/文档/数据检查、提交推送后再进入下一阶段；计划编制是独立文档筹备，不启动阶段 A 的设备检查。
- 暂停时将状态写为暂停，注明首个阻塞、最小恢复动作及资产位置；驱动问题未解决时不继续大文件下载，也不为交付而回退旧 KT。恢复先读 status、plan 和相关 log，再依据新环境继续。
