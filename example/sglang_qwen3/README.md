# SGLang + Qwen3-32B 全流程示例

从空模板和空成本资产开始，自动完成代表采集、模板生产、性能需求与有界采样、预测、独立端口对照和结果汇总。主线为 Ascend TP2，比较并发上限 2/1 两配置；两个请求同时到达，每个输入 64 个显式 token、输出 2 个 token。

本示例已于 2026-10-17 从空资产通过真实全流程验收：1 次 base、4 次性能采样、2 次独立验证，终态恢复不新增工作。证据和支持边界见[专项审查](../../docs/redesign-workflow/history/2026-10-16_code_cleanup_review.md)。流程完成不代表性能精度合格，偏差会原样显示。

## 运行前

- 在具有两张可用 Ascend NPU 的主机准备 Docker，以及同一前缀的 modeling / SGLang 配套镜像；安装方式与队列 ABI 约束见[开发说明](../../docs/redesign/development.md)。脚本不下载权重或构建原生框架镜像。
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
├── assets/{templates,costs,catalogs}/
├── results/
│   ├── prediction.json
│   ├── comparison.json
│   └── summary.md
├── logs/
└── work/                  # 未完成交接或失败时保留
```

没有诊断开关时不长期保留完整来源图、实例 JSON 和目标图；成功交接后清理可重建中间产物，结构/设备原始导出自动释放；传输原始 queue 和轻量端口保留作边界审阅依据，具体见专项审查。未完成的性能需求另保留 `results/sampling-plan.json`。模型权重、环境镜像与共享编译产物不复制到运行目录。

原生累计预算为 base 600 秒、performance 2400 秒、validation 两配置合计 600 秒且每配置最多 300 秒；包括初始化、统一预热、导出与失败尝试。离线提取/编译/拟合时间另计，因此这些预算不是总运行时长承诺。超限或没有新覆盖时停止，保留结果及原因；不会自动增加预算。本次验收墙钟约 28.2 分钟、默认实际占用约 181 MiB，仅作为该主机和镜像的观察值。需等待模型启动、采集和离线处理多个阶段。

## 失败与继续

```bash
example/sglang_qwen3/run.sh --resume data/YOUR_RUN --image-prefix YOUR_PREFIX
scripts/markov.sh status --run data/YOUR_RUN
```

恢复使用同一配套镜像前缀及原输入、资产和累计预算。活动容器仍在运行时先报告状态；终态恢复不重采、不拟合、不重新预测。预测固定后只继续未完成的验证，已完成验证保持。原生失败可在显式恢复时使用剩余预算重试；确定的后处理失败须先修正原因并显式重处理原捕获，不能靠重采掩盖。日志位置由运行状态及终端给出。

如需明确增加验证总预算，使用 `scripts/markov.sh resume --run data/YOUR_RUN --validation-budget-sec NEW_TOTAL`。进入独立验证后不接受模板或成本替换；改变模型、配置、请求或预测资产时新建探索。未完成返回非零退出码，完整运行即使误差很大也不伪报失败或自动调参。

预测排除 host 控制类 CPU 服务时延及 HTTP/tokenization 等模板外服务；原生 Published 窗口仍包含这些影响。独立验证不参与模板或拟合；此短示例不证明任意请求、TP、backend 或模型均可泛化。
