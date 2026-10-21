# 构建、运行与实现入口

本文是稳定操作说明，只描述已有入口，不保存实验结果或滚动待办。命令从仓库根执行；`path/to`、ID 和 `YOUR_*` 均须替换为真实输入。根 README 保持为空。

## 环境与构建

```bash
docker compose -p markov-next-redesign -f docker/compose/inference.yml build modeling
scripts/build.sh
```

modeling 使用 Ubuntu 24.04、GCC 13、C++23、CMake≥3.20、Ninja、nlohmann-json≥3.11 和 Zlib；语言扩展关闭，Release 产物在 `build/markov`。C++ 构建、运行和静态工具均在容器内。大范围核心变动使用新构建目录，切换编译器后重新配置，不能沿用旧缓存。

正式目标为 `markov_core`、`markov_adapters`、`markov_workflow` 和 `markov`，没有独立测试 target。公开脚本固定 Compose project `markov-next-redesign`；镜像前缀通过 `MARKOV_IMAGE_PREFIX` 指定。

真实采集前须准备框架镜像、模型权重及可用设备；已保存模型配置与固定镜像身份的离线预测只需 modeling 环境及声明资产。Dockerfile 组合 `src/collection/adapters` 内的安装适配；官方子模块不保存采集业务修改。框架/环境版本以 gitlink、实际镜像与 manifest 为准，不用历史镜像名称推定兼容。

Ascend 同步传输观察要求 torch_npu 队列头文件与原生库成套编译，入口为 `markov_native_queue_begin(correlation_id, worker)`。只替换 preload 不会重编镜像内 torch_npu；依据 `markov_workspace_build.json` 核对组合，不猜测 ABI 或保留旧入口。

## 代码目录与修改归属

| C++ 目录，声明和实现对应 | 职责 |
| --- | --- |
| `contracts`、`graph` | 公共值、工作和控制合同；图及依赖表达 |
| `production` | 共同来源绑定、调用及子图提取 |
| `adapters/frameworks/<框架>/models` | 框架 hook、端口、配方、模板生产；模型结构与原生布局 |
| `adapters/runtimes/<环境>` | 物理事件、SDK、通信和环境解释 |
| `templates` | 定义、规则、选择、原子变换及实例装配 |
| `controller` | 请求、准入、成员及逻辑 KV 状态 |
| `performance`、`execution` | 工作成本查询；目标图、资源、事件和唯一时钟 |
| `workflow`、`cli` | 项目解析和装配；正式命令入口 |

公共头文件和实现位于 `src/markov/include/markov` 与 `src/markov/src`。依赖为 `markov → workflow → adapters → core`；core 包含公共 production，不反向解释具体框架配置。

Python 位于 `src/collection/markov_collection`：workflow 拥有容器、预算和发布；instrumentation 拥有通用 wrapper/记录生命周期；artifacts 拥有共同文件与样本合同；adapters 解释框架和 runtime。原生 CPU 记录器位于 `src/collection/instrumentation`，安装补丁、PyTorch 和 CANN 观察资源位于 `src/collection/adapters`。

只换 request 通常改输入；换模型改模型绑定；换框架/API 改框架适配；换 profiler/SDK 改 runtime；换并行策略先解释布局、通信和控制差异。原生版本影响适配位置，不能因此创建内部 v1/v2。目录存在不代表对应能力已获资格。

## 常用入口

```bash
scripts/markov.sh plan --project configs/redesign/sglang/project.json
scripts/markov.sh predict --project path/to/project.json
scripts/markov.sh status --run data/YOUR_RUN
scripts/markov.sh resume --run data/YOUR_RUN
```

`plan` 只解析和规划，不采集、不加载编译模板。`predict` 读取显式资产，按项目开关和累计预算组织必要采集。空资产禁采时返回缺口，不存在默认成本或隐式历史目录。

新任务在输出位置下建立 UTC 时间加任务名的运行根，恢复沿用同一根。终端 JSON 是状态与指标摘要；逐请求数据与机器交接以 `run.json` 登记的正式产物为准。采样准备必须接收预测流程交付的完整工作需求，不能把终端重定向的摘要当作完整输入。

全流程示例：

```bash
example/sglang_qwen3/run.sh --model /models/Qwen3-32B --devices 4,5 --image-prefix YOUR_PREFIX
example/sglang_qwen3/run.sh --resume data/YOUR_RUN --image-prefix YOUR_PREFIX
```

示例调用正式 `explore`，先预测，全部组合完成后再固定资产并取得独立端口对照；从 `results/summary.md` 读取结果。它会启动原生工作，预算不足或缺数据时保留未完成状态。只需结构检查时使用对应 profile/predict 入口，不把示例结束等同于精度通过。

模型路径须在宿主和原生容器均可读，现有 Compose 挂载 `/models` 与 `/root/models`。脚本不下载权重或构建推理镜像。预测和拟合容器只挂载声明输入/资产与必要输出，不挂载 validation 真值。

## 结构采集与重建

```bash
scripts/markov.sh profile --project path/to/project.json --configuration CONFIG_ID --workload WORKLOAD_ID
scripts/markov.sh profile --project path/to/project.json --configuration REPRESENTATIVE_ID --representative
scripts/markov.sh extract --base-manifest data/path/to/profile_manifest.json
```

profile 使用 base 授权和预算；显式诊断选择不意味着普通 predict 需要用户指定 base。`--representative` 只接受计划选出的代表配置，不能同时指定 workload。

`extract` 接收 `role=base` manifest，从保留规范事实生成当前模板；`--output-directory` 指定资产位置，`--work-directory` 指定诊断位置，省略时使用 manifest 所在目录。直接 C++ 命令使用容器命名空间，仓库外输入须明确挂载。

模板规则改动后重提取并显式导入新 manifest，核对正式结果实际加载的资产。源码更新不自动重建已有 CBOR；缺观测字段不能从旧事实猜测。诊断 source_graph 不作为生产输入，不保留独立 compile-templates 或旧格式 fallback。

## 独立性能采样与拟合

```bash
scripts/markov.sh sample --project path/to/project.json --configuration CONFIG_ID --workload WORKLOAD_ID --kind device --measurement-requests work-domains.json --repetitions 3
scripts/markov.sh sample --project path/to/project.json --configuration CONFIG_ID --workload WORKLOAD_ID --kind transfer --measurement-requests work-domains.json --repetitions 3
scripts/markov.sh sample --project path/to/project.json --configuration CONFIG_ID --kind communication --repetitions 3
```

transfer/device 要求显式非空请求域；CPU 由 predict/explore 的共同需求计划提供区间与目标特征，不开放缺少这些合同的手工 CPU 入口。准备性能计划读取正式交接的完整工作需求，不从摘要或图文件猜测需求。

SGLang 经原生 Engine 执行短请求，复用统一预热和 idle 清理；原生 scheduler 管理状态。实际成员、特征、槽位和各 rank 工作决定覆盖，客户端批量提交不能保证命中。

| 产物/载体 | 用途 |
| --- | --- |
| `device_measurements.json.gz` | 设备计算、DMA、命令及关联通信 |
| `native_serving_timing` / `transfer_units` | 完整成功的同步 host driver 传输，排除准备与前置等待 |
| `native_cpu_activity` / `cpu_service_units` | 独立 CPU 服务及关联同步传输，分别拟合 |
| `collective_measurements.json.gz` | 独立通信的物理 task 样本，不用父包络计费 |

```bash
scripts/markov.sh fit-costs --performance-manifest path/to/performance_manifest.json --unit kernel > kernel-candidate.json
scripts/markov.sh evaluate-costs --model-candidate kernel-candidate.json --performance-manifest path/to/held-out-performance-manifest.json
```

unit 支持 `all|kernel|communication|transfer|command|cpu`，默认 all。指定单个 unit 时输出一份候选；all 输出包含 `candidates` 数组的集合，不能直接作为单个模型交给 evaluate-costs 或 resume。自动流程会将有效候选分别发布到 `assets/costs/`。拟合消费角色明确的独立样本；固定评价只读候选与独立留出，不回写或补采。重新压缩同一份数据不产生独立样本。

标准化成功且资产交接后可释放原始导出与已消费 journal，保留规范样本、校准/覆盖事实和来源。`raw_exports=released` 时不能宣称从 raw 重放；结构重建使用保留 facts，诊断模式才额外保留原始包。

## 资产修订、恢复和独立对照

```bash
scripts/markov.sh resume --run data/YOUR_RUN --template-assets template-assets.json
scripts/markov.sh resume --run data/YOUR_RUN --performance-manifest path/to/performance_manifest.json
scripts/markov.sh resume --run data/YOUR_RUN --cost-model kernel-candidate.json
scripts/markov.sh resume --run data/YOUR_RUN --base-budget-sec NEW_TOTAL --performance-budget-sec NEW_TOTAL
```

模板列表文件是完整 JSON 数组，项为 manifest 及可选 node_durations，路径相对列表文件。性能 manifest 和成本选项可重复，分别替换对应完整列表。新增资产从初态重算全部组合；活跃预测、拟合或未交接的结构比较期间不接受修订，终态无修订恢复不新增轮次。

结构比较由命名 modeling 容器写入正式处理收据。宿主中断后先用 status 确认原任务，再用 resume 接回；活动期间不另启动比较，传入不同 validation manifest 须先完成当前交接。比较结果发布并确认消费后才释放其终态容器和临时收据，进程日志不承担业务数据恢复。

原生 completed 与处理 succeeded 分开。确定的后处理失败由登记工作目录中的 `processing_failure.json` 表达；修正原因后可在 modeling 容器显式重处理保留捕获：

```bash
PYTHONPATH=src/collection python3 -m markov_collection.workflow.processing \
  --capture path/to/native_capture.json --role base
```

该入口复用输入及处理锁，不是新采集；原始导出已释放时不能恢复缺失字段。已存在规范 facts 的模板重建使用 extract。

```bash
scripts/markov.sh observe-target --project path/to/project.json --configuration CONFIG_ID --workload WORKLOAD_ID --budget-sec 600
scripts/markov.sh compare --run data/YOUR_RUN --validation-manifest path/to/validation_manifest.json
```

observe-target 默认 `--purpose ports`，不启用结构 profiler；显式 `--purpose structure` 用于结构观察，两者都属于独立 validation，不生产模板或成本。compare 可重复传验证 manifest，只读固定结果并核对端口/时钟。

explore 进入验证后不能修订预测资产；只继续验证或用 `--validation-budget-sec NEW_TOTAL` 调整总额。改模型、配置、请求或固定资产须新建任务。宿主中断时依据真实容器状态恢复，不把主命令退出当成容器结束。

## 修改后的必要检查

只检查受影响的职责链，重要 C++ 变化在 Docker 做必要 Release 构建。以下为只读检查示例，不能替代语义和生命周期审阅：

```bash
docker compose -p markov-next-redesign -f docker/compose/inference.yml run --rm --no-deps modeling bash -lc \
  'find src/markov/include/markov src/markov/src -type f \( -name "*.hpp" -o -name "*.cpp" \) -print0 | xargs -0 clang-format --dry-run --Werror'
bash -n scripts/build.sh scripts/markov.sh
git diff --check
```

Python 使用实际环境做语法及已有 lint 检查；格式工具不替代 block 内逻辑分段和重要注释。纯文档变更只核对职责、引用和描述，不构建、不采集，不新增验证脚本到业务仓库。
