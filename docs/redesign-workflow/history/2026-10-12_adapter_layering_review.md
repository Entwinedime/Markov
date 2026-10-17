# 采集与模板生产分层检查

> 历史审查/观测记录：结论限定于文中日期与当时实现。2026-10-16 清空数据后，旧 `data/redesign` 地址不再可复核；本文的“当前”“待完成”不作为现在的任务或授权状态。实时状态见[主计划](../2026-10-08_redesign_execution_plan.md)。

本轮落实用户要求的代码/目录分层，不推进已暂停的 S3–S6 goal，不新增原生采集。当前结构及扩展规则统一维护在[开发说明](../../redesign/development.md#c-目录与职责)，本页只记录检查结果。

## 发现与修正

- Python 平铺目录混合任务编排、共同 wrapper、框架执行和 Ascend 解析。拆为 workflow、instrumentation、artifacts、框架适配与 runtime 适配；同步插件入口、子进程模块及源码根目录定位。
- 公共 capture 同时拥有 SGLang engine、请求执行与 Ascend 产物发布。拆出 CaptureSession、SGLang/KT 实现和 AscendCaptureArtifacts，workflow 显式装配。
- Recorder 直接创建 PyTorch marker；改为接收 ProfilerMarkers，PyTorch 实现在 runtime 层。Ascend profiler Level2 与 workspace 窗口也归回 runtime。
- Ascend normalizer 反向导入 SGLang 采样程序，只为使用 SamplingDomain；共同身份/覆盖合同移入 artifacts。profiler CSV 的 Block Dim/Block Num 根据实际字段解释，不根据框架名猜格式。
- C++ SourceExtractor 含 CANN 硬件常量及事件解释，实际并非通用实现。改为 runtime 层 AscendSourceExtractor；SourceCall 值合同及公共 BlockExtractor 留在 production。HCCL 布局解释也移入 Ascend runtime。
- Qwen3 模块声明与 serving 声明混在一起。独立 Qwen3Regions，模型/attention 布局位于框架 models 目录；两组声明仍形成原有统一绑定输入。
- 原有单 target 无法表达生产适配与消费边界。改为 markov_workflow → markov_adapters → markov_core，检查公共核心无 adapters/workflow include、runtime Python 无 framework import。
- 修正两个已有接口断点：CPU 采样父类缺少 regions 参数；设备/通信产物发布缺少日志偏移。现在显式传合同，并在采样开始前创建统一发布者。移动 native CMake 后使用 build/collection/ascend 下的新构建目录，避免旧缓存记录的源码路径冲突。

## 验证

- modeling Docker 中全新 build/layering-review Release 构建通过；调整涉及的 C++ 文件通过 clang-format 检查，未修改 .clang-format。
- SGLang Docker 中移动后的 native SDK 观察库构建通过；警告来自原有 CANN 头文件的非标准数组/匿名结构。
- Python 语法编译、23 个无需原生 KT 环境的模块导入、插件 callable 与包发现、命令 help、源码根路径及 import 方向检查通过。Ruff 和 shell 语法检查通过。
- 正式 plan/prepare 使用现有 SGLang 示例，输出与原有程序一致，包含原有 block/region 声明。
- 复用已有 workspace serving 来源事实，compile-templates 成功生成 69,882,553 字节资产；原来源文件保持不变。
- 复用 synchronization-scope 的模板与给定节点耗时，predict 返回 task_status=finished、组合 status=complete、gaps=[]、timing_basis=given_node_durations、collection_performed=false。

验证产物位于 data/redesign/layering-review。未重新执行原生 serving/CPU/设备/通信采集，未重建完整 Docker 镜像，也未验证新框架、其他模型或其他原生版本。条件化展开通过不代表成本模型或完整预测资格完成。仍需在各适配器支持域内验证真实原生行为；目录调整不扩大支持域。

## 后续收敛：安装资源也属于适配器

按工作负载、实现配置、适配代码三个维度明确输入语义；框架可以是配置探索变量，适配资格以实际框架/模型/后端/并行/环境组合为单位。现有输入字段不变。

torch_npu 的 workspace patch、观察头文件、固定源码构建脚本及适用说明归入 `src/collection/adapters/runtimes/ascend/torch_npu`；Python workspace/profiling 同步归入运行包的对应子目录。SGLang 与 KT 的 Ascend 安装适配归入 `src/collection/adapters/frameworks/<框架>/ascend`，KT 原有 runtime.py 随安装实现一并归位。`docker/install` 三个入口仅调用适配器，两个 Dockerfile 复制实际组合所需目录。来源元数据使用新路径，历史观测记录保持原样。

本次调整未改变补丁、观察头文件或原生算法。Docker 内原始源码 patch dry-run、头文件格式、Python 编译及 23 个模块导入/插件入口检查通过；shell 语法、Docker COPY 路径、当前设计链接、旧路径残留及 diff 检查通过。未进行完整镜像重建或新设备采集；现有已安装镜像不会因源码移动自动更新。具体适用边界统一见[框架与环境组合](../../redesign/framework_paths.md#适配组合与源码归属)。
