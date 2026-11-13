# 当前状态

更新时间：2026-11-13。

- 资格：阶段 A 限定范围验证完成并交付；阶段 B 实现、预检和关联检查完成，等待本阶段记录提交推送；模型运行复现尚未完成。
- 分支与起点：`rebuild`，阶段 A 起点为计划交付提交 `ac80196fcb1a45ad650cd961129ac7490d481903`；启动时工作区干净。
- 已查范围：阅读工程约束、README、项目现状，静态盘点旧 KT 专属 Python/C++、共享分发、CMake、Docker、子模块和文档；读取上游新教程、环境、setup 与 verify 脚本。
- 环境事实：驱动 `25.3.rc1`，与教程要求 `25.5.1` 不同；`npu-smi info` 显示 8 张 910B3、各 64 GiB HBM。CANN 9 镜像中 torch/torch_npu 2.10.0 在选定 0 号 NPU 上完成小矩阵计算，DeepSeek 专用算子与完整模型尚未验证。
- 资源快照：可见 192 个 CPU、8 NUMA，RAM 总量约 501 GiB；工作区文件系统最近可用约 1.1 TiB，模型目录位于同一文件系统。资源是瞬时观察，运行前须重新确认可用 RAM、cgroup 上限和空闲设备。
- 网络与来源限制：ModelScope 官方 checkpoint 固定 `4e9bb2457e147f6ebfe2a46f3df047dc9869e224`，W8A8 候选固定 `6f4a67e00656c0c8410fcf7ec670bb3f304a0125`；配置结构、tokenizer 和完整 `embed.weight` 张量均一致，完整相容性仍待真实加载。两类分片均已完成 1 字节直连和重定向检查；Hugging Face 直连 DNS 解析失败。固定版本官方权重和 W8A8 均已完成直连下载，日志为 `data/kt_dsv4_20261113/w8a8-download.log` 和 `native-download.log`；实际进程与日志为恢复依据，下载每套各串行一个文件。
- 候选镜像已拉取：ARM64 摘要 `sha256:5ed0acf504181fe092f9badbd8f77611b985ec65df0f93b66ab4b9616231f70b`；SGLang 参考分支当前提交 `ff46d660d16cbf11ef393c80cc6b3ab7f6259780`。Docker daemon 未配置 HTTP/HTTPS 代理，Quay 直连可用。
- 下载快照：W8A8 下载退出码 0，固定清单 65/65 文件均已按大小与 SHA-256 校验，其中 safetensors 46/46 片；官方 checkpoint 已完成，固定清单 74/74 文件均完成大小与 SHA-256 校验，下载进程退出码 0。另有独立窗口，后续设备操作前须重新确认空闲与串行使用。
- 当前改动：已删除旧 KT Python/C++ 专属链路及主要调用方，旧 KT 子模块配置与 gitlink 已移除、本地历史检出保留；实现及紧密相关合同已提交 `e7e5a3a`。新 Dockerfile 已构建，镜像 ID 为 `sha256:78ced147cb41d820dd69458ad1973049b5f2df7cf9e2e889dac57f721d4376de`，KT-Kernel Ascend 和 MXFP4 类型检查通过。modeling Release 已成功构建，SGLang 示例 plan 返回 planned，旧 KT 示例 plan 明确返回不支持；下载、构建均有本窗口日志。
- 配置加载：在另一窗口记录设备采集结束后，持项目设备锁以新镜像、单个 NPU 和禁网容器执行 16×16 矩阵运算，并以 SGLang `ModelConfig` 加载 W8A8 配置；`NPU_SMOKE_OK` 与 `CONFIG_OK deepseek_v4 43 compressed-tensors` 均出现、进程退出 0，日志为 `data/kt_dsv4_20261113/config-load-npu.log`。设备和配置成功不证明完整模型或专用算子可运行。
- 当前转换：第 0–42 层均已转换并通过上游逐层核验，43 个正式 GGUF 已发布，`markov-dsv4-gguf-follow` 退出码 0。无网络、无设备容器运行的上游 `setup.sh gguf` 退出码 0：43 层大小一致，3 个源权重抽样层通过；上游 SHA-256 步骤实际跳过，日志为 `data/kt_dsv4_20261113/gguf-set.log`。另以上游校验器对本窗口 SHA-256 快照显式比对，43/43 一致、退出码 0，日志为 `data/kt_dsv4_20261113/gguf-sha-verify.log`；该自清单只证明文件稳定，不是独立来源认证。其他窗口设备阶段可能继续，后续须重查。
- 上游预检：无网络、只读权重的单 NPU 容器持设备锁执行 `setup.sh probe` 和 `setup.sh check`，两者及额外小矩阵运算均退出码 0；`PREFLIGHT OK`、`npu available: True devices=1` 和 `PREFLIGHT_NPU_OPERATION_OK` 见 `data/kt_dsv4_20261113/preflight.log`。probe 确认算子栈、KT Ascend 后端及 43 个 GGUF。服务的 CPU 配额和线程数须按实际运行资源重新确认，预检限 CPU 0–3 的自动线程数不用于服务。
- 未执行：完整模型设备运行、HTTP 生成、准确性与吞吐量验证。阶段 B 记录提交推送后方可进入阶段 C。
- 恢复起点：阶段 B 起点 `2460476`，上游源码位于 `data/kt_dsv4_20261113/upstream/`；确认阶段 B 记录和代码已推送，再检查其他窗口实际设备空闲状态后进入阶段 C。下载权重仅使用已核实的无代理路线，并在实际执行时持续确认。
