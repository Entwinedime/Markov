# SGLang + Qwen3-32B 示例

## 先走执行结构

```bash
python3 example/sglang_qwen3/structure.py --model /models/Qwen3-32B --devices 4 --service-ns 1000 --image-prefix YOUR_PREFIX
```

该入口先增量构建 modeling 程序，再通过正式预测流程从空资产采集代表结构、生产模板和工作需求。随后在同一时间运行根的 `inputs/` 发布每个来源的给定耗时和模板资产声明，用公开恢复入口展开两配置 × 两请求集：并发上限 2/1，及同步两请求、错峰且生成长度不同的两请求。模板在组合间固定复用，逻辑 TP2 的主角色与普通角色由框架 adapter 在同一物理卡上顺序采集，共享 base 预算；已完成角色在恢复时保留。设备参数填写实际可见卡。

`--service-ns` 必须显式指定。它给每个实际计费服务相同的纳秒数，包括 CPU、计算、通信、传输和命令；等待仍由因果依赖决定。**这些数值只用于执行结构的条件展开，没有真实性能意义，不是性能数据或精度证明。** 性能采样和独立目标验证关闭，不启动拟合；必要性能需求仍由正式流程保留。生成的 `inputs/structure-given-*.json` 记录假设与来源，`inputs/structure-template-assets.json` 是公开资产修订输入。正式结构与模板留在常规 `captures/` 和 `assets/`，先读 `results/summary.md`。

```bash
python3 example/sglang_qwen3/structure.py --resume data/YOUR_RUN --image-prefix YOUR_PREFIX
```

给定文件已发布后，恢复复用原假设和正式模板；终态重复恢复不新增采集或预测轮次。若首次采集返回前宿主退出，给定参数尚未持久化，恢复必须重新明确提供 `--service-ns`；采集的恢复、剩余额度与后处理仍由原流程负责。已发布后传入不同耗时会报错，需要新建任务或使用公开入口明确修订。缺少结构、资产无法加载、规则不支持或预算不足时如实保留未完成状态并返回非零，不用给定耗时掩盖结构问题。两角色共享的 base 累计预算为 1,000 秒，诊断默认关闭。

## 含性能采样与独立对照的全流程

从空模板和空成本资产开始，自动完成统一预热、代表采集、模板生产、CPU/设备性能需求与有界采样、预测、独立端口对照和结果汇总。主线为 Ascend TP2，比较并发上限 2/1 两配置；两个请求同时到达，每个输入 64 个显式 token、输出 2 个 token。

示例在预测完成后启动独立对照；缺数据或预算不足时保留未完成结果。运行完成不代表精度通过，仅需结构检查时使用 `profile/predict` 入口。

## 运行前

- 在具有两张可用 Ascend NPU 的主机准备 Docker，以及同一前缀的 modeling / SGLang 配套镜像；构建方式与队列 ABI 约束见[开发说明](../../docs/design/development.md)。脚本不下载权重或构建原生框架镜像。
- 准备完整 Qwen3-32B 权重、配置和 tokenizer。路径须同时能从宿主和原生容器读取；仓库 Compose 已挂载 `/models` 与 `/root/models`。
- 在仓库根目录执行。脚本先检查 Docker 和模型配置，再在 modeling 容器增量构建 C++23 程序。

```bash
example/sglang_qwen3/run.sh --model /models/Qwen3-32B --devices 4,5 --image-prefix YOUR_PREFIX
```

镜像前缀也可通过 `MARKOV_IMAGE_PREFIX` 设置；省略两者时沿用仓库默认 `markov-next`。模型和设备参数写入本次规范化输入，不修改示例文件。两配置复用 `configs/redesign/sglang/base.json` 与 `target.json`，避免再保存一套相同配置。

## 输出与预算

每次新执行在 `data/` 下创建 UTC 时间目录（以 `Z` 标识时区），终端打印路径和阶段日志。先读 `results/summary.md`：

```text
data/<开始时间>_sglang-qwen3-32b/
├── run.json
├── inputs/
├── captures/{base,performance,validation}/
├── assets/{templates,costs}/
├── results/
│   ├── prediction.json
│   ├── requests.jsonl
│   ├── comparison.json          # 实际比较后生成
│   ├── comparison-requests.jsonl
│   ├── sampling-plan.json       # 仅有未完成需求时保留摘要
│   └── summary.md
├── logs/<阶段>/
├── work/<阶段>/<轮次>/          # 未消费的完整机器需求与配方
└── diagnostics/<阶段>/<轮次>/   # output.diagnostics 开启时记录
```

默认报告保留指标、覆盖数量和少量缺口示例，摘要逐组合列出计时依据，区分给定耗时与候选/条件输入；逐请求指标独立保存，完整需求与采样配方在 `work/` 交接，不受诊断开关裁剪。`results/sampling-plan.json` 只提供待处理计划的摘要。即使给定耗时使条件执行完成，尚未解决的性能需求仍保留；消费确认后清理对应中间文件。`output.diagnostics` 额外控制目标图、原始导出和模块布局等验证信息。模型权重、环境镜像与共享编译产物不复制到运行目录。

原生累计预算为 base 1,000 秒、performance 2400 秒、validation 两配置合计 600 秒且每配置最多 300 秒；包括全部角色的初始化、统一预热、导出与失败尝试。离线提取/编译/拟合时间另计，因此这些预算不是总运行时长承诺。超限或没有新覆盖时停止，保留结果及原因；不会自动增加预算。运行开销和保留规模由当前执行的日志与产物报告，旧数值不作为当前承诺。需等待模型启动、采集和离线处理多个阶段。

## 失败与继续

```bash
example/sglang_qwen3/run.sh --resume data/YOUR_RUN --image-prefix YOUR_PREFIX
scripts/markov.sh status --run data/YOUR_RUN
```

恢复使用同一配套镜像前缀及原输入、资产和累计预算。活动容器仍在运行时先报告状态；终态恢复不重采、不拟合、不重新预测。预测固定后只继续未完成的验证，已完成验证保持。原生采集收到 Ctrl+C 会停止本次采集并保留当前交接，显式恢复使用剩余预算重试；确定的后处理失败须先修正原因并显式重处理原捕获，不能靠重采掩盖。日志位置由运行状态及终端给出。

如需明确增加验证总预算，使用 `scripts/markov.sh resume --run data/YOUR_RUN --validation-budget-sec NEW_TOTAL`。进入独立验证后不接受模板或成本替换；改变模型、配置、请求或预测资产时新建探索。未完成返回非零退出码，完整运行即使误差很大也不伪报失败或自动调参。

预测覆盖统一 serving 流程内的 CPU 服务、设备计算、通信、传输和必要命令。计时从统一预热后开始：逻辑 KV 清空，符合协议的编译缓存和 workspace 保留；模型加载、初始化、首次编译、预热及 HTTP/tokenization 不计入预测。独立对照从原生请求就绪到实际输出角色的 Published，全部 rank 的观察覆盖与入口偏移单独核对。验证数据不参与模板或拟合；此短示例不证明任意请求、TP、backend 或模型均可泛化。
