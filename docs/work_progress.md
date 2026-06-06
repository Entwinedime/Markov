# 工作进展

维护方式：本文件只做时间戳增量更新。新进展追加到顶部或底部均可，但每条必须带时间戳。除修正事实错误外，不回写历史条目。

## 2026-06-06 14:36:28 +0800

- 增加代码审查注释标记约定：
  - 普通解释性注释继续使用 `//`；
  - 作者判断确定需要修改的问题使用 `// !`；
  - 需要实验或人工审查确认的假设点使用 `// ?`。
- 对 `src/modeling/trace_graph` 做了一轮标记扫描：
  - `// !` 标出 event id 默认值、device lane fallback、sync wrapper 覆盖、stream id 到 lane 映射、nested args parser、CPU interval 条件等确定问题；
  - `// ?` 标出 ts+dur 去重、CPU leaf 启发式、CPU lane 合并、HCCL name/ordinal 对齐、字符串反转义、NodeScale 与 cpuinterval 交互等待验证假设。

## 2026-06-05 21:53:51 +0800

- 清理 active 子目录的旧仓库痕迹：
  - `src/profiling/ld_preload` 不再维护独立 README、局部 `.gitignore` 和局部 `.clang-format`；
  - `src/modeling/trace_graph` 不再维护独立 README、局部 `.gitignore` 和局部 `.clang-format`；
  - 新增仓库根目录 `.clang-format`，内容采用老版 `TraceGraph/.clang-format`，C/C++ 格式化配置统一在根目录维护；
  - LD_PRELOAD 的输出命名、rank 识别、PAPI 开关、wrapper 宏和参数输出约定已合并到 profiling 主文档；
  - TraceGraph 的目录归属和开发说明已明确收敛到 modeling 主文档；
  - constraints 文档补充 active 源码子目录不得维护嵌套 git 结构或独立 README 的约束。

## 2026-06-05 21:46:48 +0800

- 不再继续以老版 TraceGraph 作为唯一正确性标准，改为按当前 C++ 代码逻辑审查 base DAG：
  - 修正 `real_e2e_ns` 计算，使用真实执行事件的 `max(ts + dur) - min(ts)`；
  - 修正 `real_min == 0` 哨兵问题，避免小 fixture 从 0 开始时窗口计算错误；
  - 去掉同 NPU lane 顺序边上的固定 +1ns 人工延迟，该延迟没有 trace 事实支撑；
  - `EVENT_WAIT` 若只能匹配到同 lane `EVENT_RECORD`，不再额外生成 sync 边；
  - Raw Stream 到 stream sync 的映射改为映射到实际 DAG lane，而不是固定映射到 `Physic Stream Id`；
  - C++ parser 只把 `ph=X` duration event 作为 DAG 节点，metadata (`ph=M`) 和 flow (`ph=s/t`) 不再作为 0 时长节点进入 DAG。
- 本轮最关键修正是过滤 metadata / flow event：
  - merged trace 文件开头包含 `ph=M` metadata，且 rank0 中还有大量 `ph=s` flow event；
  - 当前后端尚未把 flow event 解析成边，把它们当作节点会污染 DAG 和真实窗口；
  - 后续如需利用 flow 表达依赖，应专门解析为 DAG edge。
- 修正后真实 merged trace faithful replay：
  - rank0：5,131,330 consumed duration events，2,257,498 nodes，3,415,851 edges，predicted 89,769,412 ns，trace real 89,056,920 ns；
  - rank1：5,166,791 consumed duration events，2,297,269 nodes，3,481,054 edges，predicted 89,850,961 ns，trace real 89,058,602 ns；
  - rank0+rank1：10,298,121 consumed duration events，4,554,767 nodes，6,952,229 edges，predicted 89,850,961 ns，trace real 89,058,614 ns；
  - `validation_ready=true`，faithful replay 相对误差约 0.89%。
- 仍需后续单独验证：
  - `CPU_MERGED` 是否过度串行化多线程 CPU 事件；
  - HCCL 同名 collective 组取最小 duration 是否总是合理；
  - Chrome flow event 是否应作为依赖边进入 DAG。

## 2026-06-05 21:14:16 +0800

- 定位并修复 active C++ TraceGraph 相比老版 rank0 多出的 6 条 base DAG 边：
  - 4 条顺序类边来自 device lane key 选择不一致，active 原先优先使用 `streamId` / `Physic Stream Id`，老版优先使用 trace 顶层 `tid`；
  - 修复后 rank0 lane 数从 5 回到老版的 9，顺序类边数回到 2,568,586；
  - 2 条 sync 边来自零 duration `EVENT_WAIT` 的大时间戳 double 边界误差，错误匹配了同 timestamp 的 `EVENT_RECORD`；
  - 对 `dur=0` 的 `EVENT_WAIT` 改用整数边界后，sync 边数回到老版的 71,982。
- 修复后真实 merged trace 验证结果：
  - rank0：6,145,150 records，2,568,595 nodes，3,912,340 edges，predicted 90,762,411 ns，trace real 89,056,920 ns；
  - rank1：6,180,611 records，2,616,901 nodes，3,986,960 edges，predicted 90,875,239 ns，trace real 89,058,602 ns；
  - rank0+rank1：12,325,761 records，5,185,496 nodes，7,954,624 edges，predicted 90,875,239 ns，trace real 89,058,602 ns，faithful replay 相对误差约 2.04%；
  - rank0 当前节点数和边数均与老版 TraceGraph 对齐；预测仍差约 22,284 ns，说明剩余差异不再来自边数量。

## 2026-06-05 18:28:14 +0800

- 对 active C++ TraceGraph 做老/新后端对照重构：
  - 保留 active `SimulationModule` 子模块骨架，`HiCacheModule` 继续只做 skeleton，不修改 DAG；
  - 吸收老版 TraceGraph 的 base DAG 关键设计，包括流式 Chrome trace 解析、`Physic Stream Id` 去重合并、CPU leaf 过滤、correlation / connection / stream / sync 边、HCCL 跨 rank merge 和老版拓扑仿真语义；
  - `run_summary.json` 增加 `parsed_record_count`、`real_e2e_ns`、`edge_counts_by_kind` 和 `stage_timings_ms`，用于定位 base DAG 准确性和后端性能。
- 使用真实 merged trace 验证 active 后端：
  - rank0：6,145,150 records，2,568,595 nodes，3,913,346 edges，predicted 90,762,443 ns，trace real 89,056,920 ns；
  - rank1：6,180,611 records，2,616,901 nodes，3,986,970 edges，predicted 90,875,153 ns，trace real 89,058,602 ns；
  - rank0+rank1：12,325,761 records，5,185,496 nodes，7,954,642 edges，predicted 90,879,915 ns，trace real 89,058,602 ns，相对误差约 2.05%；
  - rank0 对老版结果节点数完全一致，边数多 6 条，预测差 22,316 ns，差异约 0.025%。
- 修正 faithful replay validation：
  - `scripts/internal/model_runner.py` 优先用 C++ `run_summary.real_e2e_ns` 作为 actual；
  - bench serving duration 只保留为 workload 外层窗口参考，不再作为 base DAG faithful replay 的 actual；
  - `model_runner` 入口复跑通过，`validation_ready=true`，`dag_mutation_count=0`，`actual_source=trace_real_e2e_ns`。

## 2026-06-05 16:20:57 +0800

- 清空 C++ HiCache 旧 replay 建模逻辑：
  - `HiCacheModule` 保留为 skeleton，只统计输入 HiCache 事件数量；
  - skeleton 不修改 DAG node duration、metadata 或 edge；
  - active HiCache summary 删除旧 movement/page/latency replay 指标。
- 新增 base DAG profiling 验证入口：
  - 新配置 `configs/experiments/profiling_sglang_bench_serving_base_dag.json` 使用 SGLang `bench_serving` random dataset；
  - 该实验只启用 `torch` 和 `ld_preload`，不启用 HiCache server 参数和 Python probe；
  - modeling runner 支持从 bench_serving JSONL 的 `duration` 读取 faithful replay actual E2E。

## 2026-06-05 14:59:49 +0800

- 收敛 C++ TraceGraph 模块层次：
  - 移除 active `domains` 层，将 HiCache 建模实现迁入 `modules/hicache`；
  - C++ model config 从 `domains` 收敛为 `modules`，HiCache 配置入口统一为 `hicache`；
  - `CacheIOModule` / `CacheIOConfig` / `CacheIOSummary` 命名收敛为 `HiCacheModule` / `HiCacheConfig` / `HiCacheSummary`；
  - DAG metadata 和 module summary 统一使用 `hicache` 前缀。

## 2026-06-05 13:12:15 +0800

- 对 active C++ TraceGraph 做结构性重构：
  - 新增 `TraceEvent`、`DagGraph`、`DagBuilder`、`TopologicalSimulator`、`ChromeTraceIO` 核心层次；
  - C++ CLI 收敛为 `--input` / `--run-summary` / `--model-config` / `--graph-output`，删除旧 active `TraceDAG`、`ActivityRecord`、parser/export wrapper 和 `ascend_sync` 残留；
  - `SimulationModule` 接口改为 `apply(DagGraph& graph)`，`NodeScaleModule` 与 `HiCacheModule` 已迁移到新图结构；
  - `scripts/internal/model_runner.py` 改为直接向 C++ 后端传入 merged trace，并仅在需要时输出 DAG Chrome trace。
- 增强 base DAG fixture：
  - 覆盖同 stream 串行、不同 stream 并行、correlation 边、stream synchronize 阻塞；
  - `tests/run_tracegraph_fixtures.py`、`tests/run_hicache_state_fixtures.py`、`tests/run_modeling_smoke_fixtures.py` 已在重构过程中通过。

## 2026-06-05 12:25:17 +0800

- 清理 modeling / profiling 旧入口：
  - C++ TraceGraph 删除 CLI `--scale` / `-s` 入口，节点缩放只允许通过 `node_scale` model config 驱动；
  - 删除旧 `opt_scale` DAG 核心接口和 `scale_transform` 头文件，`NodeScaleModule` 自行维护缩放逻辑和 summary；
  - modeling runner 删除 `--engine`、`--emit-dag-patch` 和向 C++ CLI 生成 `--scale` 的兼容路径；
  - `faithful_replay` 模式不再生成 C++ model config，保证 base DAG 验证不加载任何子模块；
  - profiling runner 和 profiling config 不再读取顶层 `profile` / `hook` 兼容入口，`profiling.channels` 只接受 `torch`、`python_probe`、`ld_preload`；
  - 删除 deprecated Python modeling、deprecated Python probe 和旧 `merge_all_traces.py`。
- 引入 nlohmann/json：
  - C++ `model_config` 改为使用 nlohmann/json 解析，不再用正则处理 JSON；
  - `node_scale` 与 `hicache` 都通过统一 C++ model config 进入子模块。
- 文档同步：
  - profiling / modeling / constraints 文档删除旧兼容描述；
  - 用户可见 CLI help、异常和日志保持英文，代码注释保持中文。

## 2026-06-05 12:35:00 +0800

- 将 modeling 主线改为纯 C++ 后端：
  - `src/modeling/deprecated/trace_graph` 已移回 `src/modeling/trace_graph`；
  - root CMake 直接构建 active `trace_graph` target；
  - 删除当前 Python TraceGraph / Python SimulationModule 后端，Python 侧只保留 `model_runner.py` 编排。
- 建立 C++ 子模块接口：
  - 新增 C++ `SimulationModule` 基类；
  - `NodeScaleModule` 和 `HiCacheModule` 通过继承实现；
  - CLI 中 scale 和 hicache 都经 C++ module list 统一执行。
- 强化 trace merger：
  - `scripts/trace/trace_merger.py` 支持从 profile manifest 合并 torch、LD_PRELOAD、Python probe 三类 trace；
  - 支持用 LD_PRELOAD 参数补充 torch 事件；
  - 支持追加 torch 采不到的 CPUInfer / HiCache / Python probe 事件。
- 已验证：
  - `cmake --build build --target trace_graph -j 8` 通过；
  - `tests/run_modeling_smoke_fixtures.py`、`tests/run_hicache_state_fixtures.py`、`tests/run_profiling_fixtures.py` 通过。

## 2026-06-05 11:58:00 +0800

- 修正 profiling 配置主线：
  - 采集渠道配置统一收敛到 `profiling.torch`、`profiling.python_probe`、`profiling.ld_preload`；
  - 顶层 `profile` / `hook` 仅保留为旧配置兼容入口；
  - HiCache Python probe 改用 `sglang.hicache`，`generic_callable` 不再包含 `page_hashes:` 特化。
- 重新启用 C++ TraceGraph：
  - root CMake 重新构建 `src/modeling/deprecated/trace_graph` 的 `trace_graph` target；
  - modeling runner 新增 `--engine cpp_trace_graph`，faithful replay 可直接调用 C++ base DAG 引擎；
  - Python `SimulationModule` 结构保留，cache_state/cache_patch 后续继续围绕子模块状态和 DAG patch 推进。
- 已验证：
  - `tests/run_profiling_fixtures.py`、`tests/run_modeling_smoke_fixtures.py`、`tests/run_hicache_state_fixtures.py` 通过；
  - 三个 active profiling 配置 dry-run 均能生成 manifest；
  - `cmake --build build --target trace_graph -j 8` 通过。

## 2026-06-05 11:00:40 +0800

- 记录 SGLang HiCache + NPU graph + profiling 渠道隔离结果：
  - 纯 SGLang HiCache、NPU graph 开启、无 profiling：`data/profile_runs/sglang/20260605_023403_profiling_minimal_sglang_hicache`，workload `69/69 ok`；
  - 只开 LD_PRELOAD、NPU graph 开启：`data/profile_runs/sglang/20260605_024104_profiling_minimal_sglang_hicache`，workload `69/69 ok`，生成两个 LD_PRELOAD trace 文件；
  - 只开 torch profiler、NPU graph 开启：`data/profile_runs/sglang/20260605_024651_profiling_minimal_sglang_hicache`，在 `prefetch_reuse_C seq=63 prompt_id=C_2` 失败；
  - 用户关闭 SGLang runtime profiler 的 `experimental_config` 后重跑 torch-only、NPU graph 开启：`data/profile_runs/sglang/20260605_025450_profiling_minimal_sglang_hicache`，仍在 `prefetch_reuse_C seq=63 prompt_id=C_2` 失败；
  - 失败 server 日志均出现 `aclnnFusedInferAttentionScoreV3 failed, error code is 507009`，随后 scheduler 进程触发 `Segmentation fault` / `Bus error`；
  - 因此当前不应把 `torch profiler + NPU graph + HiCache phased workload` 作为稳定默认采集路径。需要进一步缩小 workload 或调整 torch profiler activity 范围来隔离问题。

## 2026-06-05 11:08:10 +0800

- 新增并运行更小的 torch-only + NPU graph 临时实验：
  - 临时配置：`data/profile_runs/tmp_configs/profiling_tiny_prefetch_sglang_hicache_torch_npu_graph.json`；
  - 配置只保留 `warmup=1`、`prefetch_seed_C=4`、`prefetch_reuse_C=8`，去掉 A/B 压力段、load_back 验证段和 dirty eviction 段；
  - run dir：`data/profile_runs/sglang/20260605_030221_profiling_tiny_prefetch_sglang_hicache_torch_npu_graph`；
  - 结果 `status=completed`、`profiling_ready=true`、workload `13/13 ok`；
  - 生成两个 torch `trace_view.json`，大小约 639 MB 和 617 MB，整个 run dir 约 2.3 GB；
  - 未出现 `507009`、`FusedInferAttention`、`Segmentation fault`、`Bus error` 或 `RemoteDisconnected`；
  - 初步判断：之前失败不是 `prefetch_reuse_C` 小段单独立即触发，更可能需要完整 phased workload 的前置 cache 压力、请求数量、运行时状态积累，或 torch profiler 持续采集时间达到某个条件。

## 2026-06-05 02:35:00 +0800

- 建立下一轮闭环验证实现入口：
  - modeling runner 支持 `faithful_replay`、`cache_state`、`cache_patch` 三种模式；
  - `faithful_replay` 跳过所有子模块 patch，`cache_state` 只维护子模块状态，`cache_patch` 才修改 DAG；
  - modeling 从 profile manifest 自动发现 `workload_report.json`，只对 workload 请求窗口建模和验证；
  - 默认 DAG 仿真改为拓扑重放，不再用原始绝对时间戳补齐缺失依赖边。
- 增强 HiCache diagnostic workload：
  - phase 覆盖 seed/reuse/backup wait/pressure/load_back/prefetch/dirty eviction；
  - workload report 输出 `expected_cache_mechanisms`，供 profiling 质量审计使用；
  - 官方配置继续固定 `--hicache-ratio=2.0`，容量压力优先由 workload 构造。
- 增强 profiling quality：
  - 检查 workload 声明的预期 HiCache 机制是否出现；
  - 对真正状态转移事件执行严格 page identity 检查，controller 队列锚点仍允许 count-only。

## 2026-06-05 10:08:00 +0800

- 修正真实 profiling 启动方式约束：
  - 真实 SGLang / KTransformers profiling 必须通过 `scripts/profile.sh` 外层容器入口启动；
  - `scripts/internal/profile_runner.py` 只作为容器内执行器、fixture 或 dry-run 入口，不能在宿主机上直接启动真实 server。
- 新 diagnostic workload 首轮真实运行结果：
  - run dir：`data/profile_runs/sglang/20260605_015514_profiling_minimal_sglang_hicache`；
  - server 在 `pressure_B` 第 39 个压力请求附近触发 NPU `aclnnFusedInferAttentionScoreV3` 507009 错误并 segfault；
  - workload 后半段变成 `Connection refused`，manifest 标记 `status=failed`。
- 降低默认 diagnostic 压力并增加失败保护：
  - `pressure_requests` 从 48 降到 24；
  - `shared_prefix_repeat` 从 160 降到 128；
  - `unique_suffix_repeat` 从 16 降到 8；
  - workload 增加 `--max-errors=1`，首个请求错误后停止，保留准确失败点。

## 2026-06-05 01:26:13 +0800

- 修正 torch profiler lifecycle 默认语义：
  - 默认 profile 配置不设置 `num_steps`，profiling 覆盖完整 workload；
  - workload 结束后由 runner 手动调用 `/stop_profile`；
  - 非默认实验显式设置 `profile.num_steps` 时，runner 不再强制 stop，避免 server log 出现重复 stop 的 500 traceback。
- 清理 `configs/experiments/profiling_minimal_sglang_hicache.json`：
  - 删除显式 `num_steps=1`；
  - 删除多余 `stop_after_workload=true`，使用 runner 默认语义。
- 更新 `docs/profiling_development.md` 并增加 profiling fixture 覆盖默认 stop 与 `num_steps` 自动结束两种路径。

## 2026-06-04 22:22:06 +0800

- 修复真实 profiling -> modeling 闭环中的输入接入问题：
  - modeling runner 支持从 `profile_manifest.json` 读取 `ld_preload_trace_files`；
  - manifest 和 modeling runner 支持递归发现 Ascend profiler 的 `trace_view.json`；
  - trace loader 兼容进程退出时未写入结尾 `]` 的 LD_PRELOAD Chrome trace 数组；
  - fallback event id 增加源文件指纹，避免两个 rank 的同名 `trace_view.json` 事件 id 冲突导致 DAG 成环；
  - `predicted_e2e_ns` 改为相对 trace 起点的耗时，不再输出绝对时间戳。
- 强化 SGLang HiCache Python probe：
  - `generic_callable` 新增 `list:` 和 `page_hashes:` source 语法；
  - `profiling_minimal_sglang_hicache.json` 为可直接定位 page 的 HiCache target 增加 `page_identity`；
  - `profile_quality.py` 增加 page identity 覆盖率，并将 required 字段缺失纳入 `quality_ready=false`。
- 完成一轮新的真实最小实验：
  - run dir：`data/profile_runs/sglang/20260604_141533_profiling_minimal_sglang_hicache`；
  - `quality_ready=true`，torch / LD_PRELOAD / Python probe 文件数分别为 2 / 2 / 2；
  - Python probe 事件 1112 个，20 个配置 target 命中 15 个；
  - 未命中 target 为 `controller.load`、`controller.page_transfer`、`hiradix.evict`、`hiradix.init_load_back`、`hiradix.load_back`，当前 workload 未触发这些路径；
  - HiCache page identity 覆盖：556 个 HiCache end 事件中 270 个带 page identity，236 个 operation end 事件中 120 个带 page identity；
  - modeling 跑通，`predicted_e2e_ns=113636766000`；
  - HiCacheModule 消费 566 个 fact，产生 174 个状态转移、94 个 DAG operation，最终 L1/L2/L3 各记录 12 个 page，backuped 12 个 page。

## 2026-06-04 21:58:19 +0800

- 实现下一轮最小真实实验后的质量审计入口：
  - 新增 `scripts/internal/profile_quality.py`，从 `profile_manifest.json` 统计 Python probe target 命中、缺失字段、异常事件和 trace 文件覆盖；
  - 默认输出 `<run_dir>/profile_quality.json`，`quality_ready=false` 时脚本返回非零。
- 推进真实 Python probe 事件到 HiCache modeling 的映射：
  - `HiCacheModule` 支持通过 `event_role` 识别 lookup、load、prefetch、write、evict、storage transfer 等事件；
  - Python probe 的 start phase 不进入建模，end/exception phase 作为事实输入；
  - 缺 page identity 的事件不会伪造 page key，会记录 `missing_page_identity`，但明确的搬运/写入/淘汰事件可生成 count-only DAG operation。
- 增加真实 run 的建模入口：
  - `scripts/internal/model_runner.py` 支持 `--profile-manifest` 覆盖 `config.input.profile_manifest`；
  - 新增 `configs/modeling/modeling_hicache_from_manifest.json`，用于消费真实 profiling run。

## 2026-06-04 21:27:06 +0800

- 修复 modeling 从 profile manifest 读取 LD_PRELOAD trace 的入口：
  - `src/modeling/trace_model/runner.py` 现在读取 `trace.ld_preload_trace_files`；
  - `native_trace_files` / `native_trace` 仅保留为旧产物兼容。
- 审查 active `src/profiling/ld_preload` 实现并修正边界：
  - LD_PRELOAD 不支持 Python 式动态 target；
  - active `sglang` profile 当前只稳定采集 hardcoded AscendCL runtime wrapper；
  - 撤回通用 POSIX IO wrapper 方向，HiCache storage page 事实改由 Python probe 采；
  - `scripts/internal/hooks/build.sh` 改为从 `src/profiling/ld_preload` 构建，并在旧 CMake cache 指向不同 source 时重建本 profile build dir。
  - 根 `CMakeLists.txt` 移除已删除的 `native_hook` / `trace_graph` 子目录引用，改为只接入 active `src/profiling/ld_preload`。
- 扩展 `generic_callable` 字段表达能力：
  - 支持嵌套参数路径，例如 `arg:params.req.rid`；
  - 支持返回 tuple/list 下标，例如 `return.0`、`return.1.id`；
  - 支持 `len:<source>`，用于采 token/page/tensor 长度。
- 新增 `configs/experiments/profiling_minimal_sglang_hicache.json`：
  - 启用 `torch`、`python_probe`、`ld_preload`；
  - 定义 20 个 SGLang HiCache Python probe target；
  - 使用 file storage backend 和 phased workload 作为下一轮最小真实实验入口。

## 2026-06-04 20:54:00 +0800

- 重构 internal runners：
  - `scripts/internal/profile_runner.py` 拆分运行目录、环境注入、server/bench 生命周期、torch profiler 控制和 suite 展开逻辑；
  - `scripts/internal/model_runner.py` 拆分 CLI 解析、配置加载、输出开关覆盖和建模执行入口；
  - 两个 runner 均补充中文注释，明确脚本层只负责流程编排，不承载建模判断。

## 2026-06-04 20:47:00 +0800

- 修正 profiling 主线方向：
  - active LD_PRELOAD 目录命名为 `src/profiling/ld_preload`，不再使用 `native_hook` 命名；
  - 废弃“Python runner 统一控制 Python probe 和 LD_PRELOAD target”的方案；
  - Python runner 只控制 Python 侧 probe，配置入口为 `profiling.python_probe`；
  - LD_PRELOAD 回到 C++ 硬编码 wrapper 方案，runner 只注入 `LD_PRELOAD` 和 `HOOK_TRACE_OUTPUT`；
  - Python probe 依据 `src/profiling/deprecated/python_probe` 重构，保留 `sitecustomize.py`、import hook 和 probe plugin 结构。

## 2026-06-04 17:46:36 +0800

- 完善 profiling ld_preload 主线：
  - 新增 active `src/profiling/native_hook`，实现文件 IO 相关 LD_PRELOAD wrapper；
  - `profiling.instrumentation.targets` 增加 `options` 字段，并统一控制 python/native targets；
  - runner 为 native hook 注入 `TRACE_SIM_NATIVE_HOOK_TARGETS`、`TRACE_SIM_NATIVE_HOOK_OUTPUT` 和 debug quality 输出；
  - manifest 区分 `trace/native/events.jsonl` 和 native debug 文件；
  - 新增 `profiling_smoke_ld_preload.json` 和 native hook fixture。

## 2026-06-04 17:28:10 +0800

- 打通新 profiling 到 modeling 的最小闭环：
  - Python probe 支持同步和 async callable，并在 debug 模式输出质量报告；
  - 新增 Python TraceGraph、DAG mutation API、拓扑仿真和 modeling runner；
  - 新增 `NodeScaleModule`、`BandwidthModule` 和 `HiCacheModule` v0；
  - HiCache v0 可维护 L1/L2/L3、dirty、backuped、evicted、prefetch ready/late/suppressed 状态，并生成 load/prefetch/write/evict DAG mutation；
  - 新增 profiling、TraceGraph、HiCache state、modeling smoke fixtures；
  - 新增干净 smoke 配置 `profiling_smoke_python_probe.json` 和 `modeling_smoke_hicache.json`。

## 2026-06-04 17:05:45 +0800

- 新增可配置 Python probe 执行层：
  - runner 在启用 `python_probe` 渠道时向 server 进程注入 `src/profiling/python_probe`；
  - `sitecustomize.py` 读取 `TRACE_SIM_INSTRUMENTATION_TARGETS` 并延迟包装 Python callable；
  - probe 支持从参数、kwargs、`self` 属性和返回值属性采集字段；
  - sidecar 默认写入 `trace/python_probe/events.jsonl`；
  - bench client 环境会移除本次 probe 注入，避免误采集 workload driver。

## 2026-06-04 17:01:12 +0800

- 完善 profiling 插桩目标配置的灵活性：
  - `profiling.instrumentation.targets` 支持按目标声明 `module`、`channel`、`events` 和字段对象；
  - 模块支持短名规整，例如 `hicache`、`node_scale`、`parallel`；
  - 渠道支持短名规整，例如 `python`、`native`、`hook`；
  - 更新 profiling 文档，说明短名会在 manifest 中统一输出为正式名称。

## 2026-06-04 16:58:30 +0800

- 初步重构 profiling 主线模块：
  - 新增 `src/profiling/schema.py`，定义采集渠道、子模块采集需求和可配置插桩目标；
  - 新增 `src/profiling/config.py`，规整 `profiling.modules`、`profiling.channels`、`profiling.probes` 和 `profiling.instrumentation.targets`；
  - 新增 `src/profiling/manifest.py`，生成干净的 profiling manifest，不再混入 modeling actual 输出；
  - 更新 `scripts/internal/profile_runner.py`，接入新的 profiling runtime config，并通过环境变量传递模块、渠道、probe 和插桩目标；
  - 更新 `docs/profiling_development.md`，补充灵活定义插桩目标和采集字段的配置格式。

## 2026-06-04 16:43:05 +0800

- 在 `docs/modeling_development.md` 增加 HiCache what-if 到 DAG 的映射设计：
  - 明确输入依据包括 profiling facts、target config 和 base DAG anchors；
  - 细化要修改的 DAG 对象、HiCache DAG 节点类型、操作映射规则和边连接规则；
  - 补充 duration 计算优先级、mutation 记录字段和缺失依据处理规则。

## 2026-06-04 16:35:37 +0800

- 在 `docs/project_constraints.md` 增加语言与注释约束：
  - 文档统一使用中文；
  - 代码注释统一使用中文；
  - 新写代码应为状态机、DAG 修改、profiling hook、字段契约、错误处理和边界条件添加充分注释；
  - 注释应解释设计意图和不变量，避免只复述代码表面行为。

## 2026-06-04 16:27:01 +0800

- 详细展开 `docs/profiling_development.md` 中 HiCacheModule profiling 说明：
  - 增加 SGLang HiCache request/prefetch/load/write/evict 调用链辅助说明；
  - 增加建议 probe 位置表；
  - 将 HiCache 必需字段改为“如何采集 / 从哪采集 / 为什么采集”格式；
  - 增加 HiCache 可选字段，覆盖 node id、host/device indices 摘要、hit_count、backuped、evicted、completed_tokens、prefetch/write policy。

## 2026-06-04 16:22:27 +0800

- 删除 `docs/profiling_development.md` 中多余的“子模块字段说明”通用汇总段落。
- 保留各子模块小节内的采集事件、渠道、必需字段和用途说明。

## 2026-06-04 16:19:32 +0800

- 扩展 `docs/profiling_development.md` 的子模块采集详情：
  - 每个 profiling 子模块占据独立小节；
  - 补充 TraceGraph、NodeScale、EdgeLatency、Bandwidth、ParallelStrategy、Interconnect 的采集目标、事件、渠道和字段；
  - 重点展开 HiCacheModule，细化 request lifecycle、operation lifecycle、page identity、cache tier movement、prefetch evidence、write evidence、storage evidence；
  - 明确 HiCache 不默认采集完整 token 列表、page key 明文、policy 推断结果和 target scenario 预测结果。

## 2026-06-04 16:12:23 +0800

- 调整 `docs/profiling_development.md`：
  - 默认 trace 字段收缩为最小公共字段；
  - 新增 `torch`、`ld_preload`、`python_probe` 三类采集渠道说明；
  - 新增按 modeling 子模块划分的采集矩阵；
  - 新增子模块按需字段作用说明；
  - profiling 配置增加 `profiling.modules` 和 `profiling.channels`。

## 2026-06-04 16:08:39 +0800

- 为 `docs/profiling_development.md` 补充字段作用说明：
  - profiling 实验配置字段；
  - trace 事实契约字段；
  - profile manifest 字段。
- 为 `docs/modeling_development.md` 补充字段和接口作用说明：
  - `prediction.json.predicted_e2e_ns`；
  - `SimulationModule` 和 `SimulationModuleDebug` 接口；
  - DAG mutation 记录字段；
  - 可选输出参数；
  - HiCache 状态字段。

## 2026-06-04 15:56:27 +0800

- 清理方向确认：用户已清理旧实现、旧结果、实验配置、profiling python probe target，并将 modeling 相关内容放入 `src/modeling/deprecated/`。
- 文档目录收敛为四个固定文件：
  - `docs/profiling_development.md`
  - `docs/modeling_development.md`
  - `docs/work_progress.md`
  - `docs/project_constraints.md`
- Modeling 主线确认：
  - 基于 profiling trace 构建 Python TraceGraph DAG；
  - 所有 what-if 都规约为 `SimulationModule`；
  - 子模块直接修改 DAG；
  - 每个子模块可以有对应 `SimulationModuleDebug`；
  - 默认主输出只保留 `prediction.json.predicted_e2e_ns`；
  - `outputs.emit_dag_chrome_trace` / `--emit-dag-chrome-trace` 控制是否输出 Chrome trace 格式 DAG。
- 本次只整理文档结构和开发约束，没有恢复 deprecated 实现，也没有新增实验配置。
