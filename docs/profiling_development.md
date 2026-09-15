# Profiling 开发与使用

所有真实框架运行从 `scripts/profile.sh` 进入。profiling 与 modeling 的唯一正式交接物是每个 cell 的
`profile_manifest.json`。

## 1. 共同流程

```text
experiment JSON
  -> framework adapter
  -> framework container
  -> server + workload + enabled channels
  -> manifest + traces + formal window
```

常用命令：

```bash
scripts/profile.sh --help
scripts/profile.sh <experiment.json> --dry-run
scripts/profile.sh <experiment.json>
```

`--server/--input/--experiment` 用于选择少量 cell；`--channels` 覆盖采集 channel；SGLang forced replay 使用
`--forced-token-bundle`。profiling cell 严格串行，避免设备、host 和 storage 状态跨 cell 干扰。

Ascend 长 trace 的导出峰值内存可能远高于采集阶段。可在实验 `env` 设置
`SGLANG_NPU_PROFILER_SERIAL_EXPORT=1`，只将采集停止后的 NPU 导出回调按 rank 串行执行；
不改变正式请求、采样窗口或模型参数。该开关适用于当前 scheduler profiler，默认关闭。
长 trace 实验同时设置 `profiling.torch.strict_stop=true`，并按串行导出总时长配置 `stop_timeout_sec`；
本轮 TP=2/4 泛化实验使用 14400 秒上限，不代表预期每次需要四小时。导出不与重型 DAG 建模并发。
默认宽松停止模式可能把 API 超时保存为 `profile_stop_response.json` 的 warning，因此 manifest completed
及 trace 文件存在均不能替代文件完整性和下游设备/phase 语义检查。不完整导出优先从保留的原始数据离线恢复，不能当作成本误差拟合。

## 2. Framework 能力

| 能力 | SGLang | KTransformers |
| --- | --- | --- |
| 独立 image/service | `sglang` | `ktransformers` |
| LD_PRELOAD trace | 支持 | 支持，使用专用 hook |
| torch trace | 支持 | 当前 smoke 默认关闭 |
| Python semantic probe | HiCache 使用 | 当前不使用 |
| framework-neutral DAG | 支持 | 支持 |
| HiCache | 支持 | 不适用 |

两个框架共享 manifest/DAG，不共享不存在的 runtime module。KTransformers submodule、installer、image、compose service、hook 和 dispatch
都属于正式保留能力。

## 3. SGLang 5×3 实验

当前评分面板由 5 个 HiCache config 和 3 个 workload 组成。配置位于：

```text
configs/workloads/hicache_manual/configs.json
configs/workloads/hicache_manual/w1_device_spill_qualification_ring.json
configs/workloads/hicache_manual/w2_writeback_dual_tier_cascade.json
configs/workloads/hicache_manual/w3_prefetch_admission_ladder_and_survival.json
configs/experiments/hicache_manual_workload/profiling_full_dag_replay.json
```

完整采集得到 15 个真实 profile cell；每个 base 到其他 4 个 config 有 12 个 cross prediction，5 个 base 合计 60。这里的 5、3、12、60
只属于当前实验，不是 predictor 的硬编码接口。

开发时应按语义挑关键 cell：

```bash
scripts/profile.sh \
  configs/experiments/hicache_manual_workload/profiling_full_dag_replay.json \
  --server <base-config> \
  --input <workload> \
  --forced-token-bundle <bundle> \
  --dry-run
```

## 4. Forced-token capture 与 replay

forced-token 合同固定真实 request 顺序、输入 tokens 和输出 tokens，使不同配置看到相同 workload。

先捕获：

```bash
scripts/profile.sh configs/experiments/hicache_manual_workload/no_profile_capture.json
```

再 profile：

```bash
scripts/profile.sh \
  configs/experiments/hicache_manual_workload/profiling_full_dag_replay.json \
  --forced-token-bundle <capture-suite>/forced_token_bundle.json
```

capture 的真实输出必须达到请求的 `max_new_tokens`，replay 会重新核对 workload ID、request order 和 token arrays。HTTP 成功但输出被容量
裁短不能作为合法 bundle。

有状态 capture 还要通过 startup gate、barrier 和 checkpoint。token-only capture 不提供建模 trace；随后的 replay 才是 base DAG 数据。

## 5. 固定小型校准的采集语义

`prepare-hicache` 可生成一个 `fixed_calibration` workload。它与旧的按 target 缺口构造候选不同：

- 只读取合法 base tokens 和 target-independent 平台物理 page 域；
- 在物理域最小、最大 page 端点使用同一 request sequence；
- 固定使用 `write_back + wait_complete`，产生 spill、recovery 和 rewrite；
- 允许重复完全相同的输入；首次可 capture token，后续只 replay 已验证 bundle；
- 不读 target profile、score 或某个 cell 的误差；
- 生成 suite 写在当次 `data/` 组目录，不长期增加 `configs/` 文件。

这是一份参数采集资产，不是额外 micro workload 评分阶段。失败、超时、无效 trace、请求和 tokens 都进入累计 ledger；失败后只允许重试
同一端点和输入。

## 6. Trace channels

SGLang HiCache 使用：

- `torch`：device kernel 与 framework execution；
- `ld_preload`：CPU/syscall/I/O timing；
- `python_probe`：cache state、effect 和 phase 语义事实。

Python probe 默认不采集 cache snapshot。早期 snapshot 会遍历并序列化大对象，显著增加 CPU gap；当前正式 target catalog 已删除这条默认
路径。workload checkpoint 只是边界处的轻量状态门禁，不是 snapshot。

可选 `profiling.python_probe.diagnostics: "timing"` 记录 I/O thread CPU/wait 和缺页计数；`full` 再增加有限函数计时。二者只用于诊断，
不能与默认轻量 profile 混作同条件成本样本，也不能把未覆盖时间自动归为可建模 CPU gap。

这两个诊断档位还记录 Triton 的编译准备（`runtime.triton.prepare`）和首次句柄装载（`runtime.triton.load`），默认 off 不安装这些包装。
准备区间包括磁盘缓存查找，异步模式下可能只测到编译提交；装载区间包括 launcher 创建和 binary 装载，均不能当成设备执行时间。
记录保存在 Python probe trace 的 `runtime_diagnostic` 类别中，只供空白归因，不作为 HiCache fact 或额外 DAG 成本重复加入。
不采 tensor 内容、snapshot 或缓存摘要；重复的已装载内核不发事件。不改缓存与预热策略，冷启动和已有缓存的结果必须分开解释。

`timing/full` 还记录终止请求的 `runtime.response.*` 边界：scheduler_send、tokenizer_dispatch、serialize、http_body_sent。
只记录请求 ID 和区间，不读取 token 数组或序列化正文；无 socket 的非发送 rank 不记录发送成功。
http_body_sent 表示非流式 SGLang JSON 响应的最后 ASGI body send 返回，不等于客户端已经收到；
最终 E2E 真值仍来自 bench。它们同属 runtime_diagnostic，不作为额外 DAG 工作重复计时，默认 off 不安装。

probe target 声明位于：

```text
configs/profiling/hicache_probe_targets.json
```

新增 target 时只采集下游实际消费的字段；大对象转换按需执行；事实通过 owner/consumer routing 隔离。token/page 内容身份用于跨 trace 的
同一逻辑输入匹配，不用于 artifact 版本或冻结管理。

## 7. Formal window

每个 workload report 提供语义开始和结束；manifest 将其传给 DAG builder。

- 窗口内执行事件进入 DAG；
- 窗口前的 token dictionary 只解释窗口内路径；
- 与窗口内 async operation 精确匹配的窗口后 ACK/release 只闭合生命周期；
- 前后 context fact 不创建 duration node，也不计入 E2E。

formal window 不能按 config、cell、事件名白名单或固定微秒边界硬编码。固定校准可在 formal window 前使用 barrier/checkpoint 建立状态，正式
测量窗口内只放请求。

## 8. Profile 产物

```text
<cell>/
  profile_manifest.json
  config.json
  server_cmd.txt
  bench_cmd.txt
  bench/<workload>/workload_report.json
  trace/torch/...
  trace/ld_preload/...
  trace/python_probe/...
```

manifest 明确 framework、启用 channel、trace 文件、workload 状态和 formal window；不携带 schema version、image digest、工作树冻结或逐文件
checksum。modeling 不回头扫描 suite 目录猜输入。

成功 profile 至少满足：

- manifest、workload 和启用的 trace channels 完整；
- forced replay 的 request/tokens 精确匹配；
- HiCache lifecycle 能闭合；
- 配置与实际 server command 一致；
- snapshot-free 默认路径没有隐式开启重诊断。

## 9. KTransformers

当前公共 smoke 配置：

```text
configs/experiments/ktransformers/profiling_dag_smoke.json
```

```bash
scripts/profile.sh configs/experiments/ktransformers/profiling_dag_smoke.json --dry-run
scripts/model.sh build-dag --profile-manifest <ktransformers-manifest> --output-dir <dag-output>
```

真实运行要求 image、两张可用 NPU、模型/GGUF 资产和专用 hook 全部存在。当前 fixture 只证明 dispatch、LD_PRELOAD manifest 与共享 DAG 接线；
缺模型资产时不能称为真实性能 profile。

构建：

```bash
scripts/build.sh sglang
scripts/build.sh ktransformers
scripts/internal/hooks/build.sh sglang
scripts/internal/hooks/build.sh ktransformers
```

## 10. 常见问题

`--dry-run` 成功但没有 trace：这是正常的；它只展开配置和命令。

Python probe 出现大 gap：先确认使用当前 snapshot-free catalog，并将 timing/full 诊断关闭；residual gap 当前不建模。

Forced replay 被拒绝：检查 bundle 在 workspace 内，且 workload ID、request order、输入与输出 token arrays 完全一致。

KTransformers 真实运行失败：分别核对 image、NPU、hook、config dispatch 和模型/GGUF，不把 dry-run 当成真实运行。
