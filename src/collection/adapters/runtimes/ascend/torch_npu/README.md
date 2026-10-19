# torch_npu 观测适配

本目录拥有环境相关的采集插桩；Docker 安装层只调用这里的 `install.sh`。这是观察能力改动，会增加观测开销，不是使官方代码能够编译所必需的修正。

| 文件 | 职责 |
| --- | --- |
| `install.sh` | 固定源码/依赖、核对已安装 wheel、编译原生库、安装解析器并记录来源 |
| `workspace.patch` | 在官方共享 workspace 分配/清空处插桩；解析器合并独立事件文件 |
| `queue.patch` / `queue_observation.hpp` | 已跟踪业务范围的入队与worker调用，标量队列ID交接；不包围空队列等待 |
| `record_scope.patch` | 导出原始 RecordScope；区分 FUNCTION 与用户范围，不按 aten/npu/atb/c10d 名称识别操作 |
| `workspace_observation.hpp` | 标量 native journal 回调、原生时钟和 profiler 区间；异常退出记录失败 |

Python 配套位于 `src/collection/markov_collection/adapters/runtimes/ascend/torch_npu`：`workspace.py` 取得窗口前后快照，`profiling.py` 管理观察窗口及 Level2 配置，`allocator.py` 解释固定源码的 cached 分配分支。归一化以安装来源限定该规则，核对请求加 32 字节、2 MiB 对齐及实际前后状态；导出的 `workspace_allocation` 与原始 `workspace` 分开。扩容需要设备同步的标志只是源码要求，不代表同步依赖已经观测完整或在目标中建立。CANN 拦截库位于相邻 `../native`，通过标量 C ABI 交接。

适用条件以 `install.sh` 的实际固定值为准：必须为指定 torch_npu commit 对应的 wheel，并使用 Torch `2.10.0+cpu`；op-plugin、fmt 和 JSON 依赖也固定到对应源码。CANN 构建环境由镜像提供，运行需要对应 Ascend 环境。安装器在重编译前拒绝不匹配的 wheel/torch，不自动选择旧补丁或其他版本。

部署时替换三个原生库及 profiler reader，保留匹配 wheel 的 Python 扩展；包内 `markov_workspace_build.json` 记录真实安装来源。没有观察库回调时不产生 journal；启用观察后记录请求大小、设备/stream、前后容量及地址、成功或失败。独立 `markov.workspace` 文件避免与另一缓冲写入器交错，停止 profiling 后使用原生解码器合并。

SGLang 镜像装配该适配，KT 镜像不装配。cached 分配、扩容、复用和清空可提供观察事实；uncached 仅观察申请，释放未覆盖。目标需求、模板消费及等待关系还须由对应规则与实际证据判断，不能因框架同用 torch_npu 就推定采集或预测资格相同。

升级 torch_npu 时一并核对 allocator 插入点、profiler 格式与写入器、Python 快照 API、ABI 和依赖，修改当前适配并重新验证；不增加按版本并列维护的实现。此目录不改变官方 submodule，所有补丁作用于镜像内构建副本。

同步传输采样由相邻 native 库的 `transfer_sampling` 实现。Python 只读原生 batch 元数据并设置观察 scope；入队将该身份与原生 correlation ID 关联，worker 取回同一身份。队列区间只用于配对、确认成功和检查叶子归属，不产生 CPU 服务成本。同步 `drvMemcpy` 叶子保留完整单调墙钟首尾，不扣除 I/O 等待，也不拿外层 `rtMemcpyAsync` 或整个 worker 的墙钟替代传输时延。

队列入口为 `markov_native_queue_begin(correlation_id, worker)`。部署时须将 native 库与使用同一队列头文件重编译的 torch_npu 一同安装；历史四参数入口不能用于此 transfer 采样。实际安装来源由 `markov_workspace_build.json` 记录，队列配对和完整 driver 记录仍需运行核对，不能由镜像名称推定精度资格。

只有已跟踪的队列 worker、已配对成功的入队和完整观察 scope 能提供同步传输样本。没有队列身份、失败或不完整的记录保留为缺口；不会按时间邻近推断归属。采样不启用 CPU operator profiler，不记录提交计数、CPU 等待/探针扣减或完整 Enqueue 服务时间。现有结构采集的 task、queue、event、wait 和 workspace 因果继续由原生 trace 与 journal 提供。
