# Profiling 开发与使用

本文件描述当前 SGLang 与 KTransformers profiling 流程。所有真实运行都从 `scripts/profile.sh` 进入。

## 1. 共同流程

```text
experiment JSON
  -> framework adapter
  -> framework container
  -> server + workload + enabled channels
  -> profile_manifest.json + traces + formal window
```

experiment 配置选择 framework、server command、workload、hook 和 channel；runner 负责容器 dispatch、生命周期、artifact 与
manifest。业务代码不在多层重复判断 framework。

查看入口：

```bash
scripts/profile.sh --help
```

常用选项：

- `--dry-run`：展开配置和命令，不启动服务；
- `--list-experiments`：列出 suite 中的 cell；
- `--experiment/--experiments`：按展开后的 ID 选择；
- `--input/--inputs`：按 workload 选择；
- `--server/--servers`：按 server/config 选择；
- `--channels`：覆盖启用的 trace channel；
- `--forced-token-bundle`：为 SGLang forced replay 提供捕获结果。

## 2. Framework 能力

| 能力 | SGLang | KTransformers |
| --- | --- | --- |
| framework image/service | `sglang` | `ktransformers` |
| torch profiler | 可用 | 当前 smoke 不启用 |
| LD_PRELOAD hook | 可用 | 可用，专用 wrapper |
| lightweight Python probe | HiCache 模式可用 | 不启用 |
| HiCache forced workload | 可用 | 不适用 |
| HiCache modeling | 可用 | 不适用 |
| framework-neutral DAG | 可用 | 可用 |

两个框架共享 manifest/DAG 接口，不共享不存在的 runtime module。

## 3. SGLang HiCache 正式面板

正式数据是五个 HiCache 配置和三个 workload：

- `C1_l1_cliff_wait`
- `C2_host_cliff_selective_wait`
- `C3_writeback_wait`
- `C4_selective_best_effort_proxy`
- `C5_writeback_long_gate_timeout`

workload：

- `w1_device_spill_qualification_ring`
- `w2_writeback_dual_tier_cascade`
- `w3_prefetch_admission_ladder_and_survival`

当前配置位于：

```text
configs/workloads/hicache_manual/configs.json
configs/workloads/hicache_manual/w1_device_spill_qualification_ring.json
configs/workloads/hicache_manual/w2_writeback_dual_tier_cascade.json
configs/workloads/hicache_manual/w3_prefetch_admission_ladder_and_survival.json
```

不新增 micro workload 阶段；开发和验证使用现有 5×3 面板及已有 calibration 数据。

## 4. Forced-token capture 与 replay

forced tokens 固定真实 request 顺序、输入 token 和输出 token，使不同 HiCache 配置看到相同 workload。

### 4.1 Capture

```bash
scripts/profile.sh \
  configs/experiments/hicache_manual_workload/no_profile_capture.json
```

capture suite 最终提供：

```text
<capture-suite>/forced_token_bundle.json
```

bundle 保留三个 workload 的真实 token 内容。它不是模型参数，也不包含 target performance label。

### 4.2 Lightweight full-DAG replay

```bash
scripts/profile.sh \
  configs/experiments/hicache_manual_workload/profiling_full_dag_replay.json \
  --forced-token-bundle <capture-suite>/forced_token_bundle.json
```

完整运行应产生 `5 config × 3 workload = 15` 个严格串行 cell。迭代时使用 semantic selector，例如：

```bash
scripts/profile.sh \
  configs/experiments/hicache_manual_workload/profiling_full_dag_replay.json \
  --server C5_writeback_long_gate_timeout \
  --input w1_device_spill_qualification_ring \
  --forced-token-bundle <capture-suite>/forced_token_bundle.json \
  --dry-run
```

## 5. Trace channels

SGLang Direct 面板使用：

- `torch`：device/kernel 和 framework execution；
- `ld_preload`：CPU/syscall/I/O timing；
- `python_probe`：HiCache semantic facts。

Python probe 默认轻量化，不构造 state snapshot。它只采集 state/effect planning、input contract 与 DAG patch 所需事实。
完整 snapshot 曾用于早期 HiCache 验证，会明显增加 CPU gap，不属于当前主线。

## 6. Formal window

每个 workload report 提供正式开始和结束时间。profiling manifest 将它们投影为 modeling 的 trace window。

窗口规则：

- 窗口内执行事件进入 DAG；
- 窗口前 token dictionary 只帮助解释窗口内 path；
- 与窗口内 async operation 精确匹配的窗口后 ACK/release 只证明 lifecycle closure；
- 这些 context facts 不创建 duration node，也不贡献 E2E。

formal window 是语义边界，不在业务代码中用 config/cell 白名单硬编码。

## 7. Python probe target

目标目录为：

```text
configs/profiling/hicache_probe_targets.json
```

每个 target 声明 module、callable、start/end event、fact class/role/consumers 和必要字段。新增 target 时遵守：

1. 只采集下游模型实际消费的字段；
2. 大对象转换必须按需执行；
3. 默认不采集 snapshot 或完整 cache tree；
4. 事实用 consumer routing 隔离，不能被无关模块消费；
5. runtime endpoint 不存在时在 profile audit 中给出明确 blocker。

token/page hash 是 SGLang radix path 的业务身份，不用于 artifact 管理。

## 8. Profile 产物

每个 cell 的主要输出：

```text
<cell>/
  profile_manifest.json
  config.json
  server_cmd.txt
  bench_cmd.txt
  workload/workload_report.json
  trace/torch/...
  trace/ld_preload/...
  trace/python_probe/...
```

suite 根只需要一个 concise result 和 forced-token 相关输入。默认流程不生成完整 startup snapshot、逐事件 proof bundle 或重复
suite selection 副本。

`profile_manifest.json` 是 profiling 与 modeling 的唯一交接点。建模不重新扫描 suite config 来猜测 trace。

## 9. Audit

profile audit 检查：

- manifest completed/ready；
- 请求和 formal window 完整；
- 已启用 channel 有实际文件；
- configured Python targets 的关键字段存在；
- forced replay 的 request order 与 token 内容相同；
- HiCache lifecycle closure 可解释。

同一事实只在 manifest/preflight 边界验证一次。reader 可以忽略旧资产中的多余管理字段，但不会迁移或重新输出它们。

## 10. KTransformers

当前 active smoke：

```text
configs/experiments/ktransformers/profiling_dag_smoke.json
```

先验证配置和 dispatch：

```bash
scripts/profile.sh \
  configs/experiments/ktransformers/profiling_dag_smoke.json \
  --dry-run
```

真实运行要求：

- KTransformers image 与 source installer 已完成；
- 两张 Ascend NPU 可用；
- 配置中声明的 DeepSeek model 和 GGUF 路径实际存在；
- 专用 hook 已由 `scripts/internal/hooks/build.sh ktransformers` 构建。

真实 profile 只启用 LD_PRELOAD channel，生成 framework-neutral manifest。之后用 modeling `build-dag` 运行同一个 TraceGraph DAG
主链，结果不应包含 HiCache module output。

## 11. 容器与 hook

```bash
docker compose -f docker/compose/inference.yml build sglang
docker compose -f docker/compose/inference.yml build ktransformers
scripts/internal/hooks/build.sh sglang
scripts/internal/hooks/build.sh ktransformers
```

KTransformers submodule、installer、compose service、runtime image 和 hook 是保留功能；共享建模核心不反向依赖其源码树。

## 12. 常见问题

### Dry-run 成功但没有 trace

Dry-run 只证明配置展开、选择和命令生成正确，不启动 runtime。

### Python probe 造成大 gap

确认使用当前 lightweight target catalog，且没有旧 snapshot probe。先比较 snapshot-free capture，再讨论 gap model。

### Forced replay 被拒绝

检查 bundle 路径是否在仓库 workspace 内，以及 workload ID、request order 和真实 token arrays 是否匹配。

### KTransformers 无法真实运行

先分别证明 image、NPU、hook、config dispatch 和 LD_PRELOAD-only DAG smoke。缺少兼容模型/GGUF 时不能把 dry-run 报告成真实
inference profile。
