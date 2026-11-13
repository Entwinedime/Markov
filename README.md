# Markov

Markov 用一份 base trace 解释执行依据，根据 target 配置生成工作与依赖，再绑定成本预测完整 HTTP 耗时。
独立性能数据由另一条共享流程维护；预测不读取 target 实测。

```text
base trace → 源语义区域 → target 工作、依赖与条件程序 ─┐
独立性能采集 → SQLite → 离线成本模型 ────────────────┴→ 执行 → HTTP 预测
```

当前正式预测入口已运行限定范围的 SGLang 主机/文件回访；原始/扩展矩阵、有限资源搜索及固定模型后的新配置和新请求验证已有独立正常对照。KTransformers 的 DeepSeek-V4-Flash 单 NPU 流程已完成非空生成和同配置复启，其采集和预测尚不支持。实际范围、最新成绩及限制见[项目工作流](docs/workflow.md)。单格达标仍受正常实测波动和成本适用范围限制，不代表限时预取路径、细微配置排序或任意配置均已验证。旧工程严格同成本对照未完成，相对旧实现的 10× 提速尚未证实。

## 怎样运行

先构建不需要设备的 modeling 环境和程序：

```bash
docker build -t markov-modeling:ubuntu24.04 -f docker/modeling/Dockerfile .
scripts/build.sh Release
scripts/markov.sh plan --request configs/prediction.json
```

plan 只预览输入与准备动作，不启动设备。先按实际部署修改示例中的模型位置、镜像、设备和预算；
预览不会核验这些资源能否真正启动。
请求中的相对路径以请求文件目录为起点。

同一个请求可以采集 base 并预测，重复运行会复用相同输入的 base：

```bash
scripts/markov.sh predict --request /path/to/request.json
```

请求提供 deployment、workload、base、targets、performance.database（或显式 performance.models），以及 execution.output 和
execution.kernel_cache_initial_state（empty 或 prepared）。结果在 output/prediction/report.json，
包含完整正式 HTTP、逐请求耗时、后台完成、缺口、成本来源、近似、图规模和运行开销。
同一会话只准备一次 source；每个 target 独立维护缓存和执行状态。
SGLang 普通预测解释语义 probe 与客户端记录；execution.capture.detail="semantic" 可只采这些记录，当前默认 diagnostic 另保留 profiler/native-hook 用于原始图诊断。
语义采集没有完整源图回放资格；两种模式的实际精度与开销分别验证。可选 capture.cpu_set 固定采集进程的 CPU 放置。
预测容器只挂载指定程序、base、模型和输出，不挂载整个仓库、真实设备或正常评分数据。

首次建立性能库时，声明 performance.database、performance.sampling_budget 和 execution.capture。
流程先使用现有模型，按整组缺少的工作生成小型独立采样，维护模型后继续预测；不会采集 target trace 或按误差补参数。
自动准备更新本轮缺口，并复用同次采集补入尚缺的其他响应；其余成本继续使用已有选择。
当前自动采样覆盖 SGLang/Ascend 串行区域响应、批量备份、整批回载及消费控制、失败回载控制、文件预取控制/查询及 buffered 读写 worker。
文件预取保留发起返回、最终检查、停止快照和接受完成的边界；后台可在检查期间继续发布，停止后固定接受页数。
文件 worker 与查询补采只观察后台调用，保留正常前台轮询，不插入计算区域的 NPU 事件探针。
这些闭合响应与等待回合使用独立采样，其他前台控制和竞争仍有近似，不能据此认证策略排名。
采样与 base 分别累计预算；中断后重跑同一命令会接管原容器、保留用量，完成任务直接复用。

平台维护者也可显式维护独立数据：

```bash
scripts/markov.sh capture --request /path/to/base_request.json
scripts/markov.sh calibrate --request /path/to/independent_calibration_request.json
scripts/markov.sh maintain --request /path/to/maintenance.json
```

capture 用 base.configuration、实际推理镜像、设备与累计预算采集，返回 manifest；
calibrate 用另一个小型 workload 记录语义区域，不开 profiler 或 snapshot；maintain 显式导入、选择训练并发布库中的现行模型。
predict 从数据库导出本组的只读选择，用户无需手拼模型文件；未选中的历史记录不会自动参加训练。
正常对照单独运行 measure，关闭 profiler 与 Markov 探针；它只用于独立评价，不用于预测或训练。
批量研究例子在 [HiCache 矩阵](examples/hicache_matrix/README.md)：声明原始 60 和完整 186 cross，复用正式接口，失败与缺成本保留。
这些数字是实验布局，当前实际完成范围以运行结果为准。
有限候选可用 `scripts/markov.sh search --request /path/to/search.json`，在完整预测上按资源配额和声明报价选择。
[HiCache 搜索例子](examples/hicache_search/README.md)负责组合参数与独立正常确认；回访和低复用场景已运行有限实例，完整精度与真实全空间排名仍未认证。

## 已验证范围与限制

当前实际输入包括 Qwen3-32B/BF16/TP2 和 Qwen3-8B/BF16/TP1、TP2，各用自己的 base 和独立成本；平台为 Ascend 910B3、SGLang eager/FIA，串行固定输出。
8B 已跑通同机 TP=1↔2 的 24 个预测；TP1 正常对照与留出验证仍在进行，已有 TP2 对照的最大误差为 12.49%。同 TP 的早期开发结果也有写回/限时预取条件超过 10%。紧主机池下的原生分配失败仍不能可靠预测，执行成功不代表精度已通过。
预测需使用与 base 一致的输入、ignore_eos 和输出数量。文本使用采集时记录的实际 token，不在预测容器加载 tokenizer。
需要在准备结束后清空后台工作时，workload 设置 drain_before_formal=true；SGLang 以轻量状态等待 HiCache 空闲，预测保留同一控制语义。
目标可声明页大小、设备容量和 HiCache 策略等受支持设置；有规则不等于成本覆盖，域外工作明确停止。
CPU 返回、流进度和结果可读分别保留；gap、未归属控制、Prefill/Decode 不从完整 HTTP 中扣除。
首次使用条件、源控制残余与竞争仍有近似，执行成功不表示结构完整或正常精度已验收。

细粒度诊断按需运行，不是普通预测的必经步骤：

```bash
MARKOV_MODELING_MEMORY=8g scripts/run.sh modeling -- \
  build/modeling/Release/markov_core source /path/to/profile_manifest.json /path/to/source_report.json
```

原生页几何/缓存子链用 page-work，独立模型查询用 costs；字段和范围见用户合同。
可选 execution.node_scale 按已绑定工作的名字做成本敏感性分析，默认关闭，不代表真实硬件提速。
显式诊断运行 `scripts/markov.sh replay --request /path/to/replay.json`，可选源图，或打开 execution.record_graph 后保存的目标执行图。
回放可替换明确节点成本、缩放已有工作；它固定已有路径，不重新选择时间分支，也不自动匹配 target 真实成本。
独立目标诊断用 `scripts/markov.sh evaluate --request configs/target_evaluation.json`，需要已记录的预测图和另采的 target manifest。
SGLang target 采集加 `--record-regions`；评价器单独编译，不进入预测/训练流程。
报告列出真实成本替换范围、未对应工作和可观察的 CPU 顺序；带 profiler 的回放不能代替正常精度。
HiCache 只属于 SGLang。KTransformers 的 DeepSeek-V4-Flash 部署使用 SGLang 服务与 KT-Kernel CPU 专家；模型加载、非空生成和同配置复启均已通过；不支持采集、成本建模或预测。

### DeepSeek-V4-Flash 单 NPU 运行

此部署使用 910B3 单卡、CANN 9 和 CPU 专家；按当前权重清单及单层转换大小估算，准备 W8A8、官方 checkpoint 与全部 GGUF 需要约 600 GB（560 GiB）空间，另留构建和临时文件余量。本机 910B3、驱动 25.3.rc1 上模型加载、非空生成和同配置复启已通过。权重下载直接访问 ModelScope，不使用代理；连接失败时保留 `.part` 文件，可用同一命令续传。

先将 `DSV4_HOST_MODELS` 改成有足够空间的绝对路径，获取固定版本的文件清单和权重：

```bash
DSV4_HOST_MODELS=/absolute/path/to/models
mkdir -p "$DSV4_HOST_MODELS"
curl -q --noproxy '*' -fsSL 'https://www.modelscope.cn/api/v1/models/sgl-npu/DeepSeek-V4-Flash-W8A8/repo/files?Revision=6f4a67e00656c0c8410fcf7ec670bb3f304a0125&Recursive=true' -o "$DSV4_HOST_MODELS/w8a8-files.json"
python3 scripts/download_dsv4_weights.py sgl-npu/DeepSeek-V4-Flash-W8A8 6f4a67e00656c0c8410fcf7ec670bb3f304a0125 "$DSV4_HOST_MODELS/w8a8-files.json" "$DSV4_HOST_MODELS/DeepSeek-V4-Flash-W8A8"

# 若已有经核验的 43 层 GGUF，可跳过原始 checkpoint 的下载和后面的 gguf 转换。
curl -q --noproxy '*' -fsSL 'https://www.modelscope.cn/api/v1/models/deepseek-ai/DeepSeek-V4-Flash/repo/files?Revision=4e9bb2457e147f6ebfe2a46f3df047dc9869e224&Recursive=true' -o "$DSV4_HOST_MODELS/native-files.json"
python3 scripts/download_dsv4_weights.py deepseek-ai/DeepSeek-V4-Flash 4e9bb2457e147f6ebfe2a46f3df047dc9869e224 "$DSV4_HOST_MODELS/native-files.json" "$DSV4_HOST_MODELS/DeepSeek-V4-Flash"
```

下载器按清单逐文件校验大小和 SHA-256。以下启动参数对应本次 192 核、8 NUMA、约 501 GiB 内存的机器；其他机器须按可用资源调整 CPU 集、内存和专家线程数。选择空闲物理设备并保证设备任务串行，容器内该设备为逻辑 0：

```bash
docker build -t markov-ktransformers:dsv4 -f docker/ktransformers/Dockerfile .
docker run -d --name markov-dsv4 --runtime ascend --network host --ipc host \
  --cpuset-cpus 0-15,24-191 --memory 384g --memory-swap 384g \
  --ulimit memlock=-1 --ulimit stack=67108864 -e ASCEND_VISIBLE_DEVICES=0 \
  --env-file configs/ktransformers.env.example -e DSV4_HOST=127.0.0.1 \
  -e DSV4_CPUINFER=64 -e DSV4_THREADPOOL_COUNT=8 \
  --mount type=bind,source=/usr/local/Ascend/driver,target=/usr/local/Ascend/driver,readonly \
  --mount "type=bind,source=$DSV4_HOST_MODELS,target=/models" \
  markov-ktransformers:dsv4 sleep infinity
docker exec markov-dsv4 bash /opt/dsv4/ktransformers/kt-kernel/tools/ascend_dsv4/setup.sh gguf
docker exec markov-dsv4 bash /opt/dsv4/ktransformers/kt-kernel/tools/ascend_dsv4/setup.sh check
docker exec markov-dsv4 bash /opt/dsv4/ktransformers/kt-kernel/tools/ascend_dsv4/serve.sh
timeout 3600 bash -c 'until curl -sf --max-time 5 --noproxy "*" http://127.0.0.1:18080/health >/dev/null; do sleep 30; done'
docker exec markov-dsv4 bash /opt/dsv4/ktransformers/kt-kernel/tools/ascend_dsv4/verify.sh
```

`setup.sh check` 须先返回 `PREFLIGHT OK`，服务日志在模型目录的 `dsv4-logs/serve.log`。验证后可通过本机 `http://127.0.0.1:18080/v1/chat/completions` 调用，停止容器使用 `docker stop markov-dsv4`。已有可核验的 43 层 GGUF 时，可将其放在模型目录的 `cache/`，避免重复转换。

## 文档与目录

- [架构](docs/design/architecture.md)、[用户合同](docs/design/user_contract.md)、[成本与数据库](docs/design/cost_models.md)
- [验证口径](docs/design/validation.md)、[工程约束](docs/design/engineering_constraints.md)
- [项目工作流与重建结果](docs/workflow.md)

Python 的 workflow 组织正式任务，artifacts 定义输入，adapters 处理框架/runtime 差异；
C++ 的 trace_graph 维护源依据、目标规则、成本查询与共同执行器。scripts 保持薄入口，examples 承担研究案例。
C++ 头源在同职责目录，src 为 include 根；原生 SDK 桥在推理 Docker 编译，核心在 modeling Docker 编译。

本仓库位于 /home/markov-rebuild，与老项目 /home/markov 独立；/home/markov-next 仅供只读参考。
deprecated 只供历史对照，活动代码不调用。data 和 build 不提交；本项目重新采集数据，不复制或链接老项目资产。
