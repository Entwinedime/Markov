# torch_npu 观测适配

本目录拥有环境相关的采集插桩；Docker 安装层只调用这里的 `install.sh`。这是观察能力改动，会增加观测开销，不是使官方代码能够编译所必需的修正。

| 文件 | 职责 |
| --- | --- |
| `install.sh` | 固定源码/依赖、核对已安装 wheel、编译原生库、安装解析器并记录来源 |
| `workspace.patch` | 在官方共享 workspace 分配/清空处插桩；解析器合并独立事件文件 |
| `queue.patch` / `queue_observation.hpp` | 已跟踪业务范围的入队与worker调用，标量队列ID交接；不包围空队列等待 |
| `workspace_observation.hpp` | 标量 native journal 回调、原生时钟和 profiler 区间；异常退出记录失败 |

Python 配套位于 `src/collection/markov_collection/adapters/runtimes/ascend/torch_npu`：`workspace.py` 取得窗口前后快照，`profiling.py` 管理观察窗口及 Level2 配置，`allocator.py` 解释固定源码的 cached 分配分支。归一化以安装来源限定该规则，核对请求加 32 字节、2 MiB 对齐及实际前后状态；导出的 `workspace_allocation` 与原始 `workspace` 分开。扩容需要设备同步的标志只是源码要求，不代表同步依赖已经观测完整或在目标中建立。CANN 拦截库位于相邻 `../native`，通过标量 C ABI 交接。

适用条件以 `install.sh` 的实际固定值为准：必须为指定 torch_npu commit 对应的 wheel，并使用 Torch `2.10.0+cpu`；op-plugin、fmt 和 JSON 依赖也固定到对应源码。CANN 构建环境由镜像提供，运行需要对应 Ascend 环境。安装器在重编译前拒绝不匹配的 wheel/torch，不自动选择旧补丁或其他版本。

部署时替换三个原生库及 profiler reader，保留匹配 wheel 的 Python 扩展；包内 `markov_workspace_build.json` 记录真实安装来源。没有观察库回调时不产生 journal；启用观察后记录请求大小、设备/stream、前后容量及地址、成功或失败。独立 `markov.workspace` 文件避免与另一缓冲写入器交错，停止 profiling 后使用原生解码器合并。

当前 SGLang 镜像装配该适配，KT 镜像不装配。已取得的证据覆盖 cached 分配、扩容、复用、清空，以及限定 SGLang serving 窗口；uncached 仅观察申请，释放未覆盖。完整目标需求变换、模板消费和设备等待域仍未完成。不能因框架同用 torch_npu 就推定采集或预测资格相同。

升级 torch_npu 时一并核对 allocator 插入点、profiler 格式与写入器、Python 快照 API、ABI 和依赖，修改当前适配并重新验证；不增加按版本并列维护的实现。此目录不改变官方 submodule，所有补丁作用于镜像内构建副本。

队列CPU观测由相邻native库的worker_sampling实现，Python runtime采样器选择业务范围并刷新产物。入队和worker以原生correlation_id关联，各自保留原生任务名称/类型、线程时钟、已有等待/探针计数及runtime拦截点；跨线程时钟绝对值不能相减。工作线程仅接收已跟踪入队任务，不全局采集模型初始化。内部点已包括按现有trace选定的kernel提交入口，已取得一次prefill/decode真实拦截证据，但不能推导任意分支完整覆盖；没有将墙钟或完整worker调用直接视为服务成本。当前队列补丁已部署到观测镜像，并通过一次双rank入队/worker身份、阶段归属与单元时钟核对；目标成本匹配尚未完成。

提交点记录自身的计数/墙钟读取和向量写入由 native 层用成对线程 CPU 时钟观测，累计到同一 `CpuCounters.probe_cpu_ns`；边界先取线程时钟，再读计数，下一测量段接收该记录点开销，避免扣到前一段。队列入口快照读取也计入已测探针。原始线程时钟和探针计数均保留，由成本读入按段扣除。它不等于全部拦截、时钟或框架观测开销已经消除，不能据此自动提升成本资格。
