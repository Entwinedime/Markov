# 工作进展

维护方式：本文件只做时间戳增量更新。新进展追加到顶部或底部均可，但每条必须带时间戳。除修正事实错误外，不回写历史条目。

## 2026-06-30 00:39:24 +0800

- 收敛 `docs/tmp/` 剩余 inactive internal 文档：
  - 将 `tmp_internal_refactor_guidance_20260629.md` 中仍有效的职责边界迁移到 profiling/modeling 开发文档和 project constraints；
  - 将 `tmp_internal_scripts_audit_20260629.md` 中仍有效的 current script ownership、workflow artifact/progress 和后续维护关注点拆入主线文档；
  - 不迁移旧调用链、旧 `profiling.quality` / `quality_hicache` 表格和已完成任务清单为 active spec；
  - 删除上述两份 inactive 临时文档，`docs/tmp/` 当前不再保留 active 文档。

## 2026-06-29 12:37:24 +0800

- 完成 `scripts/internal` HiCache workflow 职责收紧：
  - `markov_internal/profiling/` 不再持有 post-profile quality/readiness；通用 artifact audit 移到 `audit.profile_artifacts`，
    HiCache readiness 移到 `hicache.quality.profile_audit`；
  - forced-token schema/hash/report helper 移到 `contracts.forced_token`，profiling 只保留 capture/replay 运行期注入与聚合；
  - workflow 使用 `WorkflowRunContext`、`WorkflowArtifactPolicy`、`WorkflowProgressReporter` 和面向对象 stage runner 编排
    quality/final-state/transition；
  - 默认 console 输出改为阶段级 start/done summary，不再逐 run/cell 打印 `result ok ...`；
  - workflow summary / stage summary / runner configs / per-run audit / transition catalog 已分层到
    `workflow_summary.json`、`stages/` 和 `artifacts/`。
- 验证：
  - `python3 -m py_compile $(find scripts/internal/entrypoints scripts/internal/markov_internal -name "*.py" -print)` 通过；
  - 1x1 self smoke 通过，输出目录：
    `data/profile_runs/sglang/20260628_154748_profiling_hicache_state_forced_replay/modeling/internal_refactor_smoke`；
  - 1x1 cross smoke 通过，输出目录：
    `data/profile_runs/sglang/20260628_154748_profiling_hicache_state_forced_replay/modeling/internal_refactor_cross_smoke`。

## 2026-06-29 10:16:00 +0800

- 对齐 HiCache state validation、model limitations、profiling/modeling 开发文档和 project constraints：
  - `HCSV-20260628-forced-bundle-full-matrix` 成为当前 active validation baseline；
  - 最新 forced replay workflow 路径为
    `data/profile_runs/sglang/20260628_154748_profiling_hicache_state_forced_replay/modeling/hicache_state_workflow_manual_3inputs`；
  - input contract `3/3` ready，state quality `15/15` ready，strict profile quality `12/15` ready；
  - final-state self `15/15`、cross `60/60`、full `75/75` exact；
  - transition exactness `75/75` exact，transition-count 和 page-lifecycle multiset 也均为 `75/75`。
- 明确当前合同不采集、不消费 `storage_control_drain_boundary`，也不维护 `check_kind` 字段；Case B 由
  terminal prefetch 后 pending host reservation + 同 request post-admission release drain 的 target-derived 近似关闭。
- 将 `docs/tmp/` 中仍成立的结论拆回主线文档，废弃的 storage-control checkpoint 合同只保留为历史回退说明；已迁移/已废弃的临时文档已删除。
- 将 `scripts/internal` 的当前分层写入主线文档：宿主机 wrapper、`entrypoints/` 容器内 CLI、`markov_internal/` 可复用包、
  不再保留旧平铺脚本或 `deprecated/` 人工对照目录。

## 2026-06-25 21:42:23 +0800

- 在高层重构基础上为 `docs/project_constraints.md` 补回必要的可执行约束：
  - profiling channels、suite 继承规则和运行入口；
  - cache-state 五字段输入 gate、当前原子 role、scope/sequence 与 token dictionary/span 合同；
  - forced plan/bundle schema、preflight、输出匹配和 cross-config 三重签名门禁；
  - target config、validation 前置条件、输出开关和提交前检查。
- 未恢复历史阶段、具体实验参数、失活配置或兼容入口。

## 2026-06-25 21:38:49 +0800

- 重构 `docs/project_constraints.md`：
  - 删除实现清单、具体参数、历史清理记录和已失活实体；
  - 收敛为文档证据、运行环境、profiling、cache-state 输入、common/forced workflow、modeling、validation、配置产物和工程质量的高层边界；
  - 当前配置和 workflow 约束保持对齐。

## 2026-06-25 21:31:57 +0800

- 收敛 `configs/` 为当前 cache-state 开发主链：
  - 只保留 `profiling_hicache_state_common.json`、`profiling_hicache_state_forced_capture.json` 和
    `profiling_hicache_state_forced_replay.json`；
  - 删除 S1A/S1B、mainline-one、smoke、common/forced faithful profiling suite 和静态 faithful/cache-state modeling config；
  - 移除保留配置中的 S1A/S1B 与 faithful pair 描述，common/forced 命名和 `profile_mode` 现在显式区分；
  - cache-state target modeling config 统一由 `hicache_state_workflow.py` 动态生成。
- workflow 同步收紧：common suite 只允许 self prediction，cross-config prediction 必须使用 forced replay suite；
  common run 不再被空 forced-token 集合标记为 `input_contract_ready=true`。
- README、profiling/modeling 开发文档、validation、project constraints 和 `scripts/model.sh` 用法已同步。

## 2026-06-25 21:09:05 +0800

- 完成 forced-token bundle workflow 和相关主线文档的终检：
  - replay quality 改为无条件要求 bundle path/schema/hash/id 与 bundle-plan hash 对齐，旧无 bundle replay 不再能在单 run
    quality 中通过；
  - forced-token quality 拆分 `plan_ready`、`bundle_ready` 和总 `ready`，matrix 的 plan/bundle signature 保持独立诊断；
  - profile quality 从 run config 校验预期 forced mode，workload report 缺失或 mode 不一致时直接失败；
  - workflow quality 每次重新审计 manifest，不再复用旧 quality cache；
  - transition stage 强制与 final-state stage 同次执行，避免复用未经过当前输入合同的旧 prediction rows；
  - suite preflight 改为不留空目录，默认 fail-fast 也会先写 `suite_result.json` 再退出；
  - 删除 deprecated `hicache_state_matrix_validation.py` 兼容入口，统一使用 `hicache_state_workflow.py`；
  - README、profiling、validation、limitations、project constraints 和仍 active 的 transition 临时文档已同步当前
    pre-bundle / active gate 边界。
- 真实 capture/replay 仍未重跑；2026-06-24 5x3 结果继续只作为 pre-bundle 模型回归基线。

## 2026-06-25 16:04:05 +0800

- 完成 forced-token capture/replay bundle workflow：
  - capture experiment 改为写 run-local immutable plan，capture suite 聚合 `forced_token_plans/` 和
    `forced_token_bundle.json`；
  - `scripts/profile.sh` / `profile_runner.py` 新增 `--forced-token-bundle`，forced replay 一律要求显式 bundle；
  - replay config 改为 `{forced_token_plan}` 注入，不再保留仓库固定 plan 或 latest capture fallback；
  - preflight 校验 bundle schema、selected input、plan path/hash、workload id/fingerprint、request count 和 logical request 顺序；
  - suite selection/result、run config、workload report、profile quality、matrix quality 和 workflow summary 均记录并校验
    bundle provenance；
  - 删除 `configs/workloads/hicache/forced/` 下旧固定 plan 和说明。
- 本地验证：
  - Python compile、shell parse、全量 JSON parse 和 `git diff --check` 通过；
  - 临时 plan/bundle replay preflight 通过，无 bundle replay 被拒绝；
  - 模拟 3-input capture aggregation和 portable relative plan resolution 通过；
  - 1x1 replay suite dry-run 证明 suite/run artifacts 记录显式 bundle 依赖；
  - workload report 的 bundle-plan hash mismatch 能被 quality contract 拦截。
- 2026-06-24 的旧 5x3 run 没有 bundle provenance；它保留为 pre-bundle 模型回归基线，不能作为当前 bundle gate 的
  active validation。真实 capture/replay 尚未重跑。

## 2026-06-25 15:37:58 +0800

- 完成 HiCache 临时文档迁移和长期文档对齐：
  - 将 forced-token profiling、profiling workflow 重构、Python probe API 审计和 token directory 重构的稳定内容拆入
    `README.md`、profiling/modeling 主线文档、validation、model limitations 和 project constraints；
  - 删除上述 4 份已完成临时文档；保留尚未实现的 forced-token bundle 设计和仍在处理的 transition exactness 根因文档；
  - 用 2026-06-24 forced-token 5x3 workflow 产物替换 2026-06-18 旧 active baseline。
- 当前有效结果：
  - 15 个 replay run 中 `state_quality_ready=15/15`、strict `profile_quality_ready=12/15`；
  - 3 个 input 的 canonical signature 和 forced-token plan signature 均 ready；
  - final state self `14/15`、cross excluding self `56/60`、full self/cross `70/75`；
  - transition ready/final-state exact `70/75`，transition-count 和 page-lifecycle exact `65/75`；
  - 剩余 failure 为 5 个 `c1/deeper` ACK-stage ordinary lock release 缺口，以及 5 个 `c0/deeper`
    evicted-marker-only mismatch。
- 本轮只修改文档，没有修改 profiling/modeling 实现，也没有重跑真实 profiling。

## 2026-06-22 02:21:06 +0800

- 完成 HiCache 文档收敛：
  - `docs/modeling_development.md` 合并 active state model 数据结构、canonical radix / storage directory / ref ledger /
    capacity index / async operation table / target control clock 等当前 C++ 结构，并补齐 state -> intent -> DAG patch -> E2E
    的长期路线；
  - `docs/profiling_development.md` 合并 HiCache profiling 输入层级 P0-P5，明确 state 输入、物理执行证据和 target
    E2E oracle label 的边界；
  - `docs/validation/hicache_state_validation.md` 提升到当前 5 config x 3 manual input 矩阵口径：final state self
    `15/15`、cross `75/75`，transition exactness T1/T2 `25/75`；
  - `docs/validation/hicache_state_model_limitations.md` 继续作为长期限制文档，补入 host/storage 异步控制边界近似；
  - `docs/project_constraints.md` 对齐新的长期文档集合和 active HiCache internal validation scripts。
- 删除已迁移的临时/短期文档：
  - `docs/hicache_5x4_state_transition_alignment_plan_draft.md`
  - `docs/hicache_e2e_prediction_iteration_plan_draft.md`
  - `docs/hicache_state_data_structure_design_draft.md`
  - `docs/hicache_transition_exactness_iteration_plan_draft.md`
  - `docs/tmp_hicache_l2_host_storage_final_state_diagnosis.md`
  - `docs/validation/hicache_allocator_lifecycle_short_term_plan.md`
- 本轮只整理文档，未运行 modeling/profile 实验。

## 2026-06-13 18:04:12 +0800

- 提交前复跑非 fixture 验证，未使用 `tests/`：
  - `clang-format --dry-run --Werror` 覆盖当前 C++ 改动文件；
  - `cmake --build build --target trace_graph -j2`；
  - `python3 -m py_compile scripts/internal/model_runner.py scripts/internal/hicache_state_cross_input_audit.py scripts/internal/hicache_state_provenance.py scripts/internal/profile_quality.py`；
  - `find configs -name '*.json' -print0 | xargs -0 -n1 jq empty`；
  - `git diff --check`；
  - 四向 `precommit_hicache_audit_20260613_*` HiCache state validation 全部 `validation_ready=true`、
    `validation_errors=[]`、`final_state_match=true`。
- 复核结果：
  - S1A target self/cross：L1 `25/25`、dirty `0/0`、L2/backuped `67/67`、evicted `42/42`、locked `0/0`；
  - S1B target self/cross：L1/dirty `28/28`、L2/backuped/evicted `55/55`、locked `0/0`。

## 2026-06-13 17:54:12 +0800

- 复审 HiCache host release / cleanup 代码后继续收紧：
  - 删除 diagnostic state injection 移除后遗留的未调用 `page_set_size_for_scope()` 和 `page_for_scope()` helper；
  - best-effort prefetch threshold 未显式配置时改为 SGLang 默认 `max(prefetch_threshold=256, page_size)` tokens 的 target page projection；
  - best-effort prefetch capacity limit 未显式配置时改为 SGLang `floor(0.8 * (host_pool_pages - device_pool_pages))` 投影；
  - rate-limit 判断保持 SGLang `occupied >= capacity_limit`，不再把 0 capacity limit 当作无限制；
  - 不再保留 2 pages 或 L2 一半这类经验 fallback。
- 同步更新约束、建模和验证文档，明确 threshold / capacity / cleanup budget 必须来自显式 target config 或 SGLang 源码语义。

## 2026-06-13 17:35:56 +0800

- 收紧 `scripts/internal` 下 HiCache 专项脚本，只保留两个 active 只读入口：
  - `hicache_state_cross_input_audit.py`：跨配置 normal atomic invariant input contract 审计；
  - `hicache_state_provenance.py`：基于 validation / predicted trace / oracle snapshot 的 mismatch 页面证据汇总，不回写模型。
- 删除过期或临时脚本：
  - `hicache_host_residual_audit.py`：S1B host residual 临时审计，结论已经收敛进 active 文档；
  - `hicache_state_async_elision.py`：会生成 synthetic `diagnostic_state_injection` model input；
  - `hicache_state_trace_divergence.py`：仍包含 `--diagnostic-inject-async` oracle replay alignment；
  - `hicache_state_workload_input_audit.py`：front-door workload 对照不再作为 active HiCache state 验收入口。
- `hicache_state_cross_input_audit.py` 移除 `diagnostic_state_injection` 白名单，当前只接受 33-target suite 的 normal role 子集。
- C++ HiCache router/model 同步删除 diagnostic state injection 入口、oracle replace-state handler 和相关 fact 字段，避免 active tree
  继续保留 synthetic model-input 兼容通道。
- 更新约束和验证文档：HiCache internal 脚本不得保留临时 spike、oracle replay alignment 或 synthetic model input 入口。

## 2026-06-13 17:21:31 +0800

- 按用户要求删除 `tests/` 目录；该目录下的 fixture scripts、fixture data 和 `__pycache__` 均不再保留。
- 删除依赖 `tests/fixtures/modeling/tracegraph_basic.json` 的
  `configs/modeling/smoke/modeling_smoke_hicache.json`，避免留下 fixture-backed modeling 入口。
- 更新约束：之后不维护任何 fixture，不保留 `tests/` / `tests/fixtures/` / `run_*_fixtures.py`，不新增
  fixture-backed validation gate；长期验证证据必须收敛为配置、命令、审计脚本、关键指标、结论或真实 run 的复现入口。
- 本轮未运行测试。

## 2026-06-13 17:02:18 +0800

- 完成 HiCache 文档对齐和临时文档收敛：
  - 删除 tracked 临时文档 `docs/tmp_hicache_async_visibility_model_plan.md`、
    `docs/tmp_hicache_target_resource_mechanism_plan.md` 和旧缺陷清单
    `docs/validation/hicache_state_model_defects.md`；
  - `docs/validation/hicache_state_validation.md` 现在是唯一 active validation 文档，合并当前输入契约、final3 四向通过结果、
    S1B host residual 审计结论、历史阶段摘要、剩余风险和复现命令；
  - `docs/modeling_development.md` 已对齐当前 final3 代码状态：SGLang-derived host release / cleanup policy 已实现，S1B
    `70/55` 与 `56/55` 只作为历史中间态保留；
  - `docs/project_constraints.md` 已移除旧 defect 文档入口，并明确不再保留 `docs/tmp_hicache*.md` 临时方案。
- 本轮只做文档整理和一致性检查，未触碰 `tests/`，未运行测试。

## 2026-06-13 04:42:54 +0800

- 按用户要求重新对照 SGLang 源码收紧 host release budget 语义：
  - `reserve_host_pages_for_prefetch()` 的 cleanup budget 仍来自 `prefetch_from_storage()` 中本次 host alloc 请求大小，
    即 alloc 失败后调用 `evict_host(prefetch_length)` 的源实现语义；
  - `PrefetchWorkItem::requested_host_pages` 保留原始 page-aligned prefetch request，用于模拟
    `prefetch_tokens_occupied` / rate limit；`reserved_host_pages` 单独记录 fallback 后实际 host pool reservation；
  - 不再把 fallback 后的 reserved pages 回写成 requested pages，避免把实际分配占用误当成 release/rate-limit budget。
- 复跑非 `tests/` 验证，全部通过：
  - `cmake --build build --target trace_graph -j2`
  - `python3 -m py_compile scripts/internal/model_runner.py scripts/internal/hicache_host_residual_audit.py`
  - `find configs -name '*.json' -print0 | xargs -0 -n1 jq empty`
  - `git diff --check`
  - 四向 state validation 全部 `validation_ready=true`、`validation_errors=[]`、`final_state_match=true`：
    - S1A self：L1 `25/25`、L2/backuped `67/67`、evicted `42/42`、dirty/locked `0/0`；
    - S1B self：L1/dirty `28/28`、L2/backuped/evicted `55/55`、locked `0/0`；
    - S1A -> S1B：同 S1B target final，全 tier match；
    - S1B -> S1A：同 S1A target final，全 tier match。
- 当前最新输出目录：
  - `data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix/01_s1a_manual/modeling/host_release_policy_final3_s1a_self`
  - `data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix/03_s1b_manual/modeling/host_release_policy_final3_s1b_self`
  - `data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix/01_s1a_manual/modeling/host_release_policy_final3_s1a_to_s1b`
  - `data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix/03_s1b_manual/modeling/host_release_policy_final3_s1b_to_s1a`

## 2026-06-13 04:36:14 +0800

- 完成 SGLang-derived target host release / cleanup policy 实现，未触碰 `tests/`，normal path 仍只消费
  `model_input=true && fact_class=invariant_state && fact_granularity=atomic`：
  - 对照 `third_party/sglang/python/sglang/srt/mem_cache/hiradix_cache.py`：
    - `prefetch_from_storage()` 的 host alloc 失败按本次 `prefetch_length` 调用 `evict_host(prefetch_length)`，不是按最终 L2
      count 或 deficit 反推；
    - `evict_host()` 只清理 host radix leaf、`evicted`、无 `host_ref_counter` 的 host nodes；
    - `check_prefetch_progress()` 是 best-effort prefetch reservation 生命周期边界：完成/撤销/未发出都会释放 ongoing host
      reservation，只有 target-derived ready pages 才进入 host-visible projection；
    - write-back 的 host alloc / `writing_check(write_back=True)` / `_evict_backuped()` 是批处理链路，本轮不再保留错误的
      page-level `ensure_host_pages_for_write()` 前置释放路径。
  - C++ `HiCacheState` 调整：
    - 拆出 prefetch `requested_host_pages` 与 `reserved_host_pages`，用 active requested pages 表达
      `prefetch_tokens_occupied`，用 reserved pages 表达 host pool occupancy；
    - `reserve_host_pages_for_prefetch()` 在 alloc 失败时按请求页数调用 host eviction，第二次仍不足则按 available pages
      降级，低于 threshold 则放弃；
    - `prefetch_check_point` 下 best-effort work 在 progress checkpoint 立即 apply 或 suppress，并释放 reservation，不再积累到
      finalize；
    - `apply_host_visibility_for_ready_work()` 把 ready pages 插入 host radix、写 L2/backuped/L3、保持 evicted，并在同一
      transaction 内执行 host capacity cleanup；
    - host loadback 到 L1 时同步恢复 device radix topology，避免后续 request admission 把 host-loaded prefix 当成 device miss；
    - best-effort admission pressure 改为完整 page suffix 数，避免 833-token / 13-page 场景为未成页 token 触发额外 radix
      page eviction；wait-complete 路径保留原 admission reservation 语义。
- 验证通过：
  - `cmake --build build --target trace_graph -j2`
  - `python3 -m py_compile scripts/internal/model_runner.py scripts/internal/hicache_host_residual_audit.py`
  - `find configs -name '*.json' -print0 | xargs -0 -n1 jq empty`
  - `git diff --check`
  - 四向 state validation 全部 `validation_ready=true`、`validation_errors=[]`、`final_state_match=true`：
    - S1A self：L1 `25/25`、L2/backuped `67/67`、evicted `42/42`、dirty/locked `0/0`；
    - S1B self：L1/dirty `28/28`、L2/backuped/evicted `55/55`、locked `0/0`；
    - S1A -> S1B：同 S1B target final，全 tier match；
    - S1B -> S1A：同 S1A target final，全 tier match。
  - 最终输出目录：
    - `data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix/01_s1a_manual/modeling/host_release_policy_final2_s1a_self`
    - `data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix/03_s1b_manual/modeling/host_release_policy_final2_s1b_self`
    - `data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix/01_s1a_manual/modeling/host_release_policy_final2_s1a_to_s1b`
    - `data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix/03_s1b_manual/modeling/host_release_policy_final2_s1b_to_s1a`

## 2026-06-13 03:51:43 +0800

- 按 `docs/tmp_hicache_s1b_host_residual_audit_goal.md` 完成 S1B host/L2 residual 审计产物：
  - 新增只读脚本 `scripts/internal/hicache_host_residual_audit.py`；
  - 生成 S1B self 审计 JSON：
    `data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix/03_s1b_manual/modeling/host_device_state_v2_s1b_self/host_residual_audit.json`；
  - 生成 S1A -> S1B 同形参考审计 JSON：
    `data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix/01_s1a_manual/modeling/host_device_state_v2_s1a_to_s1b/host_residual_audit.json`；
  - 新增报告 `docs/tmp_hicache_s1b_host_residual_audit_report.md`。
- 审计结论：
  - S1B self 的 L2/backuped/evicted diff 三组同形，都是 missing 13、extra 28；
  - missing 13 全部分类为 `missing_ready_not_visible`，都已由 `prefetch_check_point` 标记 ready，但未 host-visible；
  - extra 28 全部分类为 `extra_missing_host_release`，最后 model host-side transition 都停在 `mark_evicted`，
    source evidence 指向 `hicache_node_remove_observed` / `hicache_host_eviction_observed`；
  - 主机制边界判定为 host release / cleanup 缺失为主，`HostVisibilityApply` 不能单独 ready->L2，必须作为
    cleanup-aware transaction 的一部分。

## 2026-06-13 03:42:10 +0800

- 新增 `docs/tmp_hicache_s1b_host_residual_audit_goal.md`，把下一步 S1B host/L2 residual 审计整理成可被 goal 消费的临时文档：
  - goal objective 固定为解释 S1B target 的 L2/backuped/evicted `70/55` residual、missing 13 和 extra 28；
  - 明确 hard constraints：不碰 `tests/`，不恢复 legacy normal role，不消费 source_actual/timing/oracle 更新 target state，
    不写 ready->L2 特化规则；
  - 固定 existing inputs 为 `host_device_state_v2_s1b_self` 和 `host_device_state_v2_s1a_to_s1b` 的现有 modeling 输出；
  - 定义 `host_residual_audit.json` schema、missing 13 / extra 28 的 page-level 分类步骤、机制边界判断和 implementation gate；
  - 下一轮必须先产出 audit report，再决定实现 `HostVisibilityApply` transaction、host release/cleanup、host ref policy，
    或新增 target-independent atomic checkpoint metadata。

## 2026-06-13 03:22:45 +0800

- 按 `docs/tmp_hicache_async_visibility_model_plan.md` 和
  `docs/tmp_hicache_host_device_state_machine_audit.md` 完成一轮 host/device/async 边界重构，不恢复 legacy normal role，
  不消费 source_actual/timing/oracle：
  - 删除未使用的合成 `TargetPathMatch` 路径，避免把 device radix match、host radix match 和 storage-known topology 混成
    一个 `matched_pages`；
  - `add_resident()` / `remove_resident()` 不再对 `L2` 隐式写 host topology、host-visible set 或 backuped；
  - L2/backuped/host-visible 写入收敛到 `add_host_visible_page()`，host-visible 移除收敛到
    `remove_host_visible_page()`；
  - 删除 best-effort prefetch host reservation pressure 的 normal mutation 路径，避免在 ready 之前按 host-buffer pressure
    提前清 L2/backuped/evicted；
  - `prefetch_check_point` 只推进 async work 的 pending/ready/suppressed/late 状态；`HostVisibilityApply` 保留为显式
    host visibility 边界 API，但当前 atomic input 没有可安全触发它的 normal checkpoint。
- 本地验证：
  - `cmake --build build --target trace_graph -j2`
  - 四向 normal prediction 重跑，输出目录为 `host_device_state_v2_*`
- 四向 normal prediction 结果：
  - 四个 run 均为 `invariant_coverage_ready=true`、`missing_invariant_facts=[]`、`non_invariant_fact_usage=[]`，仍只处理
    350 个 normal atomic invariant end events；
  - S1A target self/cross final match：L1 `25/25`、L2/backuped `67/67`、evicted `42/42`、dirty/locked `0/0`；
  - S1B target self/cross 同形：L1/dirty/locked `28/28`、`28/28`、`0/0` 已保持稳定，
    L2/backuped/evicted 为 `70/55`，missing 13、extra 28；
  - best-effort async 在 normalized S1B 上暴露 `prefetch_ready_pages=13` 和 `prefetch_suppressed_pages=143`，
    但 ready pages 保持 ready-but-not-visible，不修改 L2。
- 结论：本轮完成结构边界收紧，避免继续用 host-buffer pressure 或 ready->L2 直接 mutation 污染 device state；S1B final
  仍未通过，剩余问题明确是缺少 target-independent host visibility/apply checkpoint 或完整 host release/cleanup policy，
  不能靠 source completion/host_ref/storage hit result 接回 normal input 解决。

## 2026-06-13 02:00:48 +0800

- 按 `docs/tmp_hicache_async_visibility_model_plan.md` 尝试 async visibility 建模后，修正当前方向判断：
  - v3/v4 把 prefetch ready / host-buffer pressure 过早变成 host-visible mutation，导致后续 request admission /
    device pressure 选错 victim，S1B 的 L1/dirty 从正确的 `28/28` 退化；
  - v5 删除 best-effort host-buffer pressure 对 normal state 的影响，L1/dirty/locked 恢复稳定，但 L2/backuped/evicted
    仍为 `70/55`，不是正确 final 修复；
  - v5 的 `prefetch_ready` set 按集合看对应 oracle 中 model 缺失的 13 个 L2 pages，说明 async completion 候选有用，
    但 ready -> host-visible L2 mutation 和 host victim selection 仍没有 target-side 机制支撑。
- 结论：当前 async 改动只能视为 diagnostic spike / 止血，不应作为最终机制；它可能是在缺少 host-side radix/ref/storage
  数据结构的基础上强行拟合 trace 现象。
- 下一步计划调整为先重构数据结构：
  - 拆清 `DeviceCacheState` 与 `HostCacheState`，不再用单棵 target radix tree + flat L2 page set 混合表达两侧语义；
  - 为 host 侧显式维护 host radix view、host ref/protection、host leaf group、storage-known、ready-but-not-visible
    和 host-visible 状态；
  - `AsyncState` 只维护 pending/ready/suppressed/late，禁止 checkpoint 直接写 L2/backuped/evicted；
  - `HostVisibilityApply` 在明确 target checkpoint 上把 ready pages 插入 host radix，并同一事务内更新 L2/backuped/evicted
    与 host capacity eviction。
- 仍保持 normal path 只消费 `model_input=true && fact_class=invariant_state && fact_granularity=atomic`；不把
  source completed pages、storage hit result、host_ref_delta result 或 wall-clock completion 接回 normal model。

## 2026-06-12 18:31:20 +0800

- 按 `docs/tmp_hicache_target_resource_mechanism_plan.md` 完成一轮 C++ backend target resource mechanism 重构，仍保持
  normal path 只消费 `model_input=true && fact_class=invariant_state && fact_granularity=atomic`：
  - `HiCacheFact` / router 补齐 `request_admission` 的 atomic admission scalar，要求 `admission_kind`；
  - `HiCacheTokenRadixTree` 暴露 page-path match/insert 的 terminal node、ancestor page groups，并新增动态
    device eviction leaf group 查询；
  - `leaf_group_by_page_` 不再把每个 page 映射到整条 projected path，静态索引只记录 page radix leaf node segment；
  - `HiCacheState` 新增 request execution state、device request lock/ref count、admission reservation 和 target-side
    device capacity pressure；
  - `request_admission` 现在从 target radix match 派生 active device lock/ref，并按 target capacity 主动触发 L1 pressure；
  - `request_lifecycle_anchor` 在 unfinished/finished 上执行 insert 后转移或释放 request lock/ref；
  - 未恢复 `capacity_request`、`lock_scope_delta` 或任何 source_actual normal mutation。
- 增强 `scripts/internal/hicache_state_trace_divergence.py` 的 mechanism audit，诊断报告能携带 admission/capacity/lock
  相关 evidence，但仍只作为诊断，不进入 normal prediction。
- 本地验证：
  - `cmake --build build --target trace_graph -j2`
  - 四个 normal prediction 重跑，输出目录为 `target_resource_v2_leaf_groups_*`
  - full / unlocked trace divergence 诊断重跑，unlocked diagnostic replay final 均可对齐
- 四个 normal prediction 结果：
  - 四个 run 均为 `invariant_coverage_ready=true`、`missing_invariant_facts=[]`、
    `non_invariant_fact_usage=[]`，仍只处理 350 个 normal atomic invariant end events；
  - S1A target self/cross 已 final match：L1 `25/25`、L2/backuped `67/67`、evicted `42/42`、dirty/locked `0/0`；
  - S1B target self/cross 同形，L1/dirty/locked 已对齐，L2/backuped/evicted 从 backend-refactor 的 `70/55`
    收敛为 `56/55`，剩余 13 missing / 14 extra；
  - S1B 剩余差异集中在 host/L2/storage/prefetch completion visibility，当前只有 source_actual 的
    `host_ref_delta_observed`、storage hit / prefetch completion evidence；不应把这些 source result 接回 normal model。
- 结论：本轮已闭合 device target-derived lock/ref + capacity pressure + radix victim eligibility 的主缺口；
  后续若要消除 S1B 剩余 L2/backuped/evicted diff，需要 profiling 端新增 target-independent 的 host/prefetch work
  anchor 或 backend 建完整 host_ref/storage async model，不能直接消费 source_actual。

## 2026-06-12 18:20:00 +0800

- 按“后端彻底收窄正常输入、不要残留 source/control-flow 推断入口”的方向完成 C++ HiCache backend refactor：
  - `HiCacheState::apply_fact` 改为按 router enum dispatch，不再在 state model 内重复用 role 字符串分支；
  - 删除不可达的 legacy normal role handler：`apply_maintenance_checkpoint`、`apply_capacity_request`、
    `apply_lock_scope_delta`；
  - 删除旧 cache-stage projection helper `target_cache_stage_tokens`；
  - `HiCacheFact` 只保留当前 atomic invariant 机制需要的字段，不再解析 `requested_pages_source`、
    `lock_direction`、`matched/prefix/suffix/logical/token_span` 等 source observed/control-flow 字段进模型 fact；
  - `lock_state_events` summary 字段删除，lock/ref delta 目前只作为诊断边界，不再暗示 normal model 会消费 lock delta。
- 诊断脚本从“async-only”扩展为 boundary-elision：
  - `hicache_state_trace_divergence.py` 新增 `lock_ref_transient_boundary`、
    `target_capacity_pressure_boundary`、`lock_protected_capacity_boundary`、
    `host_storage_visibility_boundary` 分类；
  - `diagnostic_elision_allowed` 与 `is_async` 分离，lock/capacity 这类非 async 输入边界不再被强行标成 async；
  - `hicache_state_async_elision.py` 生成 `boundary_elision_oracle_injection` 诊断事件，仍通过
    `diagnostic_state_injection` role 进入 C++，不改变 normal prediction 口径。
- 本地验证：
  - `cmake --build build --target trace_graph -j2`
  - `python3 -m py_compile scripts/internal/hicache_state_trace_divergence.py scripts/internal/hicache_state_async_elision.py`
  - `git diff --check`
- 在同一 33-target profile 上重跑四个 normal prediction，输出目录：
  - S1A self：`01_s1a_manual/modeling/atomic_s1a_self_backend_refactor`
  - S1B self：`03_s1b_manual/modeling/atomic_s1b_self_backend_refactor`
  - S1A -> S1B：`01_s1a_manual/modeling/atomic_s1a_to_s1b_backend_refactor`
  - S1B -> S1A：`03_s1b_manual/modeling/atomic_s1b_to_s1a_backend_refactor`
- normal prediction 结果未因删除 legacy 入口而退化：
  - 四个 run 都是 `invariant_coverage_ready=true`、`missing_invariant_facts=[]`、
    `non_invariant_fact_usage=[]`；
  - C++ 只处理 `350` 个 normal atomic invariant end events：
    `request_bound_match_anchor=100`、`request_lifecycle_anchor=100`、`request_admission=50`、
    `prefetch_decision=50`、`prefetch_check_point=50`；
  - S1A target self/cross 同形：L2/backuped `67/67`，L1 `32/25` extra 7，evicted `35/42` missing 7；
  - S1B target self/cross 同形：L1/dirty `28/28`，L2/backuped/evicted `70/55`，missing 13、extra 28。
- boundary-elision 诊断结果：
  - 排除 `locked_pages` 暂态后，S1A self 逐 trace 诊断注入 `14` 个 boundary，分类为
    `target_capacity_pressure_boundary=8`、`lock_protected_capacity_boundary=4`、
    `async_checkpoint_with_source_progress_evidence=2`，Python replay final 与 oracle 对齐；
  - S1B self 逐 trace 诊断注入 `24` 个 boundary，分类为
    `target_capacity_pressure_boundary=12`、`async_checkpoint_with_source_progress_evidence=8`、
    `async_prefetch_storage_completion=2`、`lock_protected_capacity_boundary=2`，Python replay final 与 oracle 对齐；
  - 生成 synthetic trace 后，C++ diagnostic run 也通过：S1A processed `diagnostic_state_injection=14`、
    S1B processed `diagnostic_state_injection=24`，两者 `final_state_match=true`；
  - 该结果只证明 residual diff 来自 lock/capacity/prefetch/storage visibility 边界，不是 normal model 可以直接消费
    oracle/source state 的理由。

## 2026-06-12 16:47:00 +0800

- 对 33-target atomic profile 重跑 prediction 后确认 C++ backend 没有对齐前端重构：
  - 旧结果中 S1A self `state_transition_count=0`，S1B self 也只处理了少量 path role；
  - 根因之一是 `chrome_trace_io.cpp` 把所有 `model_input=false` 事件都过滤掉，导致 source_actual 里的 token
    dictionary/provenance 在 C++ 第一遍扫描时不可见；
  - router 还把“有 `full_path_span` 描述但 token ids 尚未解析”的 path role 拦成
    `token_dictionary_or_full_path_span`；
  - state model 仍停在旧语义：`request_bound_match_anchor` 不执行 lookup，`request_lifecycle_anchor` 不触发 insert。
- 完成 C++ backend alignment：
  - Chrome trace reader 只过滤 `oracle_state` / `debug_quality` / state snapshot 等 validation-only event，不再把所有
    `model_input=false` 当成不可读事件；
  - source_actual/timing 事件仍不进入 state mutation，只用于 token dictionary 水合和 provenance；
  - router 接受有效 `full_path_span` descriptor，把是否能投影 target pages 留给 state model 机制判断；
  - `request_bound_match_anchor` 建立 request token anchor 并执行 target-side lookup；
  - `request_admission` 保存可投影 request path context；
  - `request_lifecycle_anchor` 在 finished/unfinished 边界用 request context 触发 page-aligned insert；
  - token store 不再用更短的 page-aligned projection 覆盖更长的 request anchor。
- 本地检查：
  - `git diff --check`
  - `cmake --build build --target trace_graph -j2`
- 在同一 profile 上重跑四个 prediction，输出目录：
  - S1A self：`01_s1a_manual/modeling/atomic_s1a_self_backend_align`
  - S1B self：`03_s1b_manual/modeling/atomic_s1b_self_backend_align`
  - S1A -> S1B：`01_s1a_manual/modeling/atomic_s1a_to_s1b_backend_align`
  - S1B -> S1A：`03_s1b_manual/modeling/atomic_s1b_to_s1a_backend_align`
- 新结果：
  - 四个 run 都是 `invariant_coverage_ready=true`、`missing_invariant_facts=[]`；
  - source profile side 的 C++ `processed_hicache_events=350`，五个 atomic invariant role 全部处理；
  - S1A target 的 self/cross normalized final：L2/backuped `67/67` 已对齐，L1 `32/25` extra 7，
    evicted `35/42` missing 7；
  - S1B target 的 self/cross normalized final：L1/dirty `28/28` 已对齐，L2/backuped/evicted `70/55`，
    missing 13、extra 28。
- 逐 trace / provenance 结论：
  - first divergence 仍出现在 `lock_scope_inc_end:state_snapshot`，因为 lock/ref delta 仍是 source_actual，不是 normal
    invariant；final locked 仍为 `0/0`，但 trace-level transient 不能声称对齐；
  - S1A final 余量是 capacity/eviction pressure 机制缺口：oracle 后续通过 `capacity_request` 让 7 个 page 离开 L1
    并进入 evicted，当前 normal invariant 没有 cross-safe capacity pressure 输入；
  - S1B final 余量是 host lifecycle / maintenance / prefetch visibility 机制缺口：oracle 中 extra pages 会被
    capacity、maintenance 或 prefetch visibility 后续清掉，当前 normal invariant 不包含这些 target-derived 边界；
  - 因此当前错误不应归因到 final-set 特化规则，而是 backend mechanism/input-boundary 仍未覆盖
    lock/capacity/maintenance/host lifecycle。

## 2026-06-12 15:38:20 +0800

- 新的 33-target atomic S1A/S1B profile 已完成：
  - suite：`data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix`；
  - `suite_result.json` 中 `failures=[]`，选中的 run 是 `s1a_manual` 和 `s1b_manual`；
  - 两侧 `profiling_ready=true`，Python probe trace 各 2 个。
- 修复并确认 profiling crash：
  - 上一轮失败来自 `generic_callable._read_path()` 读取 `arg:params.req.rid` 时遇到 `params.req=None`；
  - `generic_callable` 已改成 path extraction missing-safe，缺中间对象时返回 `(False, None)`，不再抛
    `AttributeError`；
  - 新 run 的 server log 没有旧 `AttributeError`，结尾只有 SIGTERM 后 detokenizer 清理日志。
- profile quality 结论：
  - S1A/S1B 的 atomic invariant coverage 均 ready，completed model-input facts 都是 `350`；
  - 五个 normal role 的 end event count 是 `prefetch_check_point=50`、`prefetch_decision=50`、
    `request_admission=50`、`request_bound_match_anchor=100`、`request_lifecycle_anchor=100`；
  - S1A 整体 `quality_ready=false` 只来自 source evidence `prefetch_transfer` 未观测到，S1B `quality_ready=true`。
- `scripts/internal/hicache_state_cross_input_audit.py` 的 hard gate 口径修正：
  - raw `request_id` 是 run-local correlation id，不再作为跨配置 canonical fact 字段；
  - request-scoped facts 用 path-bearing atomic facts 派生 `request_fingerprint` 做归一化；
  - token dictionary 是否在某个事件上携带完整 `token_ids` 不再影响 path fact 签名；
  - hard gate 比较 count 和 request-normalized canonical fact multiset，sequence mismatch 只作为诊断输出。
- 重新生成双向 cross audit：
  - S1A -> S1B：`source_event_count=350`、`target_event_count=350`、
    `model_input_contract_ready=true`、blocking roles 为空；
  - S1B -> S1A：同样 `350/350`、`model_input_contract_ready=true`、blocking roles 为空；
  - 五个 role 均存在 non-blocking sequence mismatch，说明事件顺序受运行时调度影响，但输入事实 multiset 已跨配置一致。
- 同步更新 `README.md`、`docs/project_constraints.md`、`docs/profiling_development.md`、
  `docs/modeling_development.md`、`docs/validation/hicache_state_validation.md` 和
  `docs/validation/hicache_state_model_defects.md`。下一步应在该 profile run 上重跑 self/cross modeling validation。

## 2026-06-12 12:42:07 +0800

- 按“profiling 端维护精确 atomic fact contract、无 legacy/无向后兼容”的新方向完成主线重构：
  - 主 HiCache profile config 改为 33 个 target-level `fact` target，不再把 `fact_class`、`event_role` 或 state gate
    写在字段表里；
  - 删除旧 `request_tokens`、`lookup_path`、`request_cache_lifecycle` 混合 role；
  - 新增/保留 normal invariant role：`request_bound_match_anchor`、`request_lifecycle_anchor`、
    `request_admission`、`prefetch_decision`、`prefetch_check_point`；
  - match-prefix concrete path、lifecycle committed/fill path/runtime、insert/capacity/lock/maintenance 和 controller/storage
    事件均作为 `source_actual` / `timing_observation` evidence，且显式 `model_input=false`。
- Python probe contract 同步收紧：
  - `profiling.python_probe.targets[]` 必须显式写 `module` 和完整 target-level `fact`；
  - `fact.class`、`fact.role`、`fact.model_input`、`fact.dag_input`、`fact.granularity=atomic` 缺失或非法时直接报错；
  - `generic_callable` 不再从 `target` 猜模块名，也不再把缺失 `fact` 默认当 `model_input=true`。
- C++ HiCache router/fact/model 同步切到 atomic gate：
  - `state_model_input` 从 runtime fact contract 中移除；
  - router 只消费 `model_input=true && fact_class=invariant_state && fact_granularity=atomic` 且 role 属于已知
    atomic invariant 的事件；
  - 旧 source/control-flow role 不再是正常模型入口。
- `profile_quality.py` 和 `hicache_state_cross_input_audit.py` 同步改为 atomic invariant 检查：
  - profile quality 按 atomic role 校验 required fields、token dictionary/span、`dag_input=false` 和
    `fact_granularity=atomic`，并把 HiCache 非 invariant 误标 `model_input=true` / invariant 缺 `model_input=true`
    记为契约错误；
  - cross audit 直接逐 role 比较 count、sequence 和 canonical fact value，不再保留旧 projection/variant pollution gate。
- 已做本地结构检查：
  - `python3 -m py_compile src/profiling/config.py src/profiling/python_probe/trace_sim_probe/probes/generic_callable.py src/profiling/python_probe/trace_sim_probe/probes/sglang_hicache_callable.py scripts/internal/profile_quality.py scripts/internal/hicache_state_cross_input_audit.py scripts/internal/hicache_state_trace_divergence.py scripts/internal/hicache_state_async_elision.py`
  - `find configs -name '*.json' -exec jq empty {} \;`
  - `python3 -c '... normalize_profiling_config(...) ...'` 验证 main HiCache / smoke Python probe config 都能按新契约加载；
  - `git diff --check`
  - `cmake --build build --target trace_graph -j2`
  - `rg -n "state_model_input" src scripts configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json`

## 2026-06-12 12:10:22 +0800

- 按新的 HiCache state 输入契约完成修复：
  - 主 profile config 仍保留 31 个 target，但正常 `state_model_input=true` target 收紧为
    `scheduler.prefetch_decision`、`schedule_policy.prefill_admission`、`schedule_policy.chunked_admission`、
    `hiradix.prefetch_check_point`；
  - `request_tokens`、`lookup_path`、`cache_config_observed`、`request_cache_lifecycle`、`insert_path`、
    `capacity_request`、`lock_scope_delta` 和具体 maintenance checkpoint target 降级为
    `source_actual/state_model_input=false`；
  - C++ router 不作为本轮主修复点，继续承担 schema、role 和 required-field gate。
- 为 `scripts/internal/hicache_state_cross_input_audit.py` 新增 hard `model_input_contract`：
  - 只比较真实会进入模型的 `fact_class=invariant_state && state_model_input=true` 事件；
  - 输出 `model_input_contract_ready`、`model_input_blocking_roles`、`variant_events_marked_as_model_input` 和
    `model_input_contract`；
  - projection/temporal-anchor 诊断仍保留，但不再让 source concrete value 以“projection-ready”为理由进入正常模型输入。
- 补充/更新 fixtures：
  - source_actual `capacity_request` demoted case 会被 hard contract 忽略并通过；
  - 若 `capacity_request`、`insert_path`、`request_cache_lifecycle`、`lock_scope_delta` 或 `maintenance_checkpoint`
    被错误标成 normal model input，audit 会输出对应 blocker；
  - `tests/run_profiling_fixtures.py` 断言当前 4 个正常 input target set。
- 同步主文档并删除临时修复文档：
  - 更新 `README.md`、`docs/profiling_development.md`、`docs/modeling_development.md`、
    `docs/validation/hicache_state_validation.md`、`docs/validation/hicache_state_model_defects.md` 和
    `docs/project_constraints.md`；
  - 删除 `docs/validation/hicache_state_input_contract_tmp.md`；
  - 修复前 retained cross audit 只作为 demotion 的历史证据，下一步需要在新契约下重跑真实 S1A/S1B profile 和 cross audit。
- 本地检查通过：
  - `find configs -name '*.json' -print0 | xargs -0 -n1 jq empty`
  - `python3 -m py_compile scripts/internal/hicache_state_cross_input_audit.py tests/run_hicache_state_fixtures.py tests/run_profiling_fixtures.py tests/run_hicache_mainline_config_fixtures.py`
  - `python3 tests/run_profiling_fixtures.py`
  - `python3 tests/run_hicache_state_fixtures.py`
  - `python3 tests/run_hicache_mainline_config_fixtures.py`
  - `python3 tests/run_modeling_smoke_fixtures.py`
  - `git diff --check`

## 2026-06-12 03:49:18 +0800

- 为 `scripts/internal/hicache_state_cross_input_audit.py` 新增 `normal_model_input_contract`：
  - 将 `projection_gate` 和 `after_projection_blockers` 汇总成正常模型可消费的输入契约；
  - 输出 `contract_status`、`input_contract_ready_for_cross_state_rule_diagnosis`、
    `input_contract_ready_for_non_async_correctness_claim`、`normal_model_safe_roles_after_projection`、
    `roles_requiring_target_derived_projection` 和 `unsafe_roles_after_projection`；
  - 该 contract 仍是 diagnostic-only，不改变 C++ state model，不消费 oracle，也不让 cross 自动通过。
- 补充 fixtures：
  - capacity / lifecycle / lock / maintenance mismatch 都会得到 `blocked_by_input_contract`；
  - temporal anchor 可解释的 unbound/insert case 会得到 `ready_for_cross_state_rule_diagnosis`。
- 重新生成 retained 两向真实 cross audit 后，结果一致：
  - `projection_ready_layers=[insert_paths, unbound_match_prefix_paths]`；
  - `contract_blocking_layers_after_projection=[request_lifecycle_paths, source_control_flow_checkpoints]`；
  - `normal_model_input_contract.contract_status=blocked_by_input_contract`；
  - `unsafe_roles_after_projection=[capacity_request, lock_scope_delta, maintenance_checkpoint, request_cache_lifecycle]`。
- 当前结论：
  - 这轮没有新增 state mutation 规则，也没有围绕 final missing/extra 打补丁；
  - 现在 cross run 的“不能声称排除 async 后无其他问题”已经是机器可检查的输入契约结论；
  - 下一步仍是解决 lifecycle/capacity/lock/maintenance 的 target-derived、高层 invariant 或 evidence/control-flow
    归宿，然后再做四向逐 trace async-elision / deterministic bug 判断。

## 2026-06-12 03:40:40 +0800

- 继续拆 cross projection gate 中的 `lock_scope_delta` 和 `maintenance_checkpoint` blocker：
  - 新增 `lock_scope_analysis`，按 direction、token path/hash、empty/root path、one-sided event 对 lock/ref delta
    做 pair 分类；
  - 新增 `maintenance_checkpoint_analysis`，按 `check_kind` 分布、序列 mismatch 和 one-sided event 对 maintenance
    checkpoint 做 pair 分类；
  - 新增 fixtures 覆盖 lock path mismatch / one-sided delta，以及 maintenance check_kind mismatch / one-sided checkpoint。
- 重新生成最终 retained 两向 cross audit 后，`lock_scope_delta` 的真实结果两向对称：
  - S1A->S1B count `352/308`，S1B->S1A count `308/352`；
  - 各自 inc/dec 都平衡，net delta 都是 `0`；
  - pair 分类包含 `path_mismatch_count=228`、`direction_mismatch_count=166`、`missing_event_count=44`；
  - 跳过 benign empty/root pair 后，首个有效 mismatch 是 index `17`：S1A `inc` 768-token page-aligned prefix vs
    S1B empty-path `dec`。
- `maintenance_checkpoint` 的真实结果两向也对称：
  - S1A->S1B count `1026/1030`，S1B->S1A count `1030/1026`；
  - `flush_write_through_acks=384` 和 `ready_to_load_host_cache=50` 两边对齐；
  - 差异集中在 `maintenance_check` count `592/596` 或反向；
  - pair 分类是 `check_kind_mismatch=348`、`missing_event_count=4`，首个 mismatch index `108`：
    `maintenance_check` vs `ready_to_load_host_cache`。
- 当前结论：
  - lock final `0/0` 对齐不代表 cross lock/ref delta 序列可复用；它仍需要 target radix/request lifecycle 推导、
    高层 invariant，或归入 control-flow boundary；
  - maintenance blocker 更像 schedule/polling/check_kind async 边界，不是围绕 final L1/L2/evicted 打补丁的理由；
  - after-projection blocker 仍是同一组：`request_cache_lifecycle`、`capacity_request`、`lock_scope_delta`、
    `maintenance_checkpoint`。

## 2026-06-12 03:33:27 +0800

- 继续拆 cross projection gate 中的 `capacity_request` blocker，新增 `capacity_pressure_analysis`：
  - 区分“同一 `requested_tokens` 只是 source/target page size 导致页数不同”和“`requested_tokens` 本身已经不同”；
  - 统计 source/target capacity event 数量、page-size-only 解释数量、requested_tokens 分歧数量和 one-sided event；
  - 新增 fixture 覆盖 page-size-only 与 requested_tokens-different 两类 case。
- 重新生成最终 retained 两向 cross audit 后，真实结果两向都不是 page-size-only：
  - S1A->S1B：source/target capacity event `8/12`；
  - S1B->S1A：source/target capacity event `12/8`；
  - 两向 `page_size_only_explains_count=0`、`requested_tokens_differ_count=8`、`missing_event_count=4`。
- 首个 capacity mismatch 是 S1A `requested_tokens=1057, requested_pages=9, page_size=128` vs S1B
  `requested_tokens=993, requested_pages=16, page_size=64`。
- 当前结论：
  - C++ 的 target page-size capacity 修正仍保留，它只解决“同一个 capacity pressure event 在 target page size 下应释放多少页”；
  - cross 的 capacity blocker 是 target/control-flow pressure sequence：source `HiRadixCache.evict(params.num_tokens)`
    已经是 target allocator/radix/lock/memory availability 决策后的结果；
  - 正常 cross model 不能消费 source `params.num_tokens` 或 source evict 调用序列，需要 target-derived pressure、
    高层 invariant，或明确归入 async/control-flow boundary。

## 2026-06-12 03:23:28 +0800

- 继续拆 cross projection gate 中的 `request_cache_lifecycle` blocker，新增 `lifecycle_path_analysis`：
  - 把 lifecycle path 分解成同 request 的 request-bound prompt anchor 和 cache finished/fill 后的 lifecycle suffix；
  - 新增 fixture 覆盖“prompt anchor 一致，但 committed/generated suffix 不同”的场景；
  - 该诊断仍是 diagnostic-only，不改变 normal model 输入，也不让 cross 自动通过。
- 重新生成最终 retained 两向 cross audit 后，真实结果两向一致：
  - `same_request_anchor_different_suffix=50`；
  - `both_missing_lifecycle_token_ids=26`；
  - `one_side_missing_lifecycle_token_ids=24`。
- 首个真实 lifecycle mismatch 在 index `4`：
  - source/target 的 request-bound anchor 都是 5 tokens，hash 都是 `ee7e16ee7db6a14b`；
  - source/target 的 suffix 都是 9 tokens，但 hash 分别是 `0d53b4bb2e405866` 和 `9b84224a91a04844`。
- 当前结论：
  - `request_cache_lifecycle` 的 cross mismatch 不是 page-size projection 没做好；
  - 它记录了输出/生命周期 suffix，因此不是 target-independent normal invariant path；
  - 下一步不能围绕 final set 继续特化补丁，应把 lifecycle path 改成 target-derived、高层 invariant，或降级为 evidence-only。

## 2026-06-12 03:12:24 +0800

- 继续细化 cross projection gate，新增 `after_projection_blockers`：
  - 输出 `blocking_roles`、`blocking_roles_by_layer`、`next_focus_roles`；
  - 每个 role 带 `blocker_class`、`recommended_resolution`、source/target count 和首个 mismatch 摘要；
  - 这仍是 diagnostic-only，不改变 normal model 输入，也不让 cross 自动通过。
- 补充 fixtures：
  - `capacity_request` mismatch 会输出 `target_capacity_pressure_sequence` blocker；
  - temporal anchor 可解释的 unbound/insert fixture 在 after-projection 下没有 blocker。
- 重新生成最终 retained 两向 cross audit 后，剩余 blocker 两向一致：
  - `request_cache_lifecycle`：`100/100`，first mismatch index `4`，field `path`；
  - `capacity_request`：S1A->S1B `8/12`、S1B->S1A `12/8`，first mismatch index `0`，requested tokens/pages；
  - `lock_scope_delta`：S1A->S1B `352/308`、S1B->S1A `308/352`，first mismatch index `17`，direction/path；
  - `maintenance_checkpoint`：S1A->S1B `1026/1030`、S1B->S1A `1030/1026`，first mismatch index `108`，`check_kind`。
- 明确非 blocker：
  - `request_admission` role-level aligned；
  - `prefetch_decision` 和 `prefetch_check_point` role-level aligned；
  - 当前 cross 阻断点不是“所有 prefetch 都不可信”，而是 lifecycle path、capacity/lock/maintenance control-flow
    仍需要 target-derived、高层 invariant 或 async/input-boundary 分类。

## 2026-06-12 03:08:20 +0800

- 为 `scripts/internal/hicache_state_cross_input_audit.py` 新增 `projection_gate`：
  - 保持旧的 `cross_input_contract_ready` 严格语义不变；
  - 新增 `cross_input_contract_after_projection_ready`、`projection_ready_layers`、
    `contract_blocking_layers_after_projection` 和逐 layer `gate_status`；
  - 目的不是让 cross 直接通过，而是把 high-risk layer 拆成“可机制化 target-derived projection”和“仍阻断 cross proof”。
- 补充 fixtures：
  - `run_cross_input_audit_fixture` 现在确认 `capacity_request` 这类 source-control-flow mismatch 会继续阻断
    after-projection proof；
  - `run_cross_input_temporal_anchor_fixture` 现在确认 temporal anchor 可解释的 `unbound_match_prefix_paths` /
    `insert_paths` 不再作为 after-projection blocker。
- 用最终 retained validation 目录重新生成两向真实 cross audit：
  - S1A->S1B：
    `01_s1a_manual/modeling/async_elision_current_s1a_to_s1b_capacity_target_pages_final/cross_input_audit_s1a_to_s1b.json`；
  - S1B->S1A：
    `03_s1b_manual/modeling/async_elision_current_s1b_to_s1a_capacity_target_pages_final/cross_input_audit_s1b_to_s1a.json`。
- 新 gate 的真实结果：
  - 两向 `projection_ready_layers` 都是 `insert_paths`、`unbound_match_prefix_paths`；
  - 两向 `contract_blocking_layers_after_projection` 都是 `request_lifecycle_paths`、`source_control_flow_checkpoints`；
  - 两向 `cross_input_contract_after_projection_ready=false`。
- 结论推进：
  - unbound/insert 不再是围绕 final L1/L2/evicted 打补丁的理由，应作为 normal-model-safe target-derived projection 机制继续验证；
  - cross async-elision proof 仍被 lifecycle/control-flow 输入契约阻断，下一步应优先解决这两类输入边界。

## 2026-06-12 02:59:50 +0800

- 收束本轮 HiCache state model 修正路线，保留两个机制级改动：
  - unbound/insert cache-stage path 使用 request-bound token anchor + target page size 做 target-side projection；
  - `capacity_request` 在存在 `requested_tokens` 时按 target `page_size` 重算 requested pages，再按 modeled
    LRU/lock/radix 选择 L1 victim，不消费 source victim。
- 明确拒绝 headroom/free-space capacity 假设：
  - 曾测试“只有 `resident_pages + requested_pages > capacity` 才淘汰”的语义；
  - 真实 self-config 证伪：S1A self 从 pass 退化为 L1 extra `7` / evicted missing `7`，S1B self
    L2/backuped/evicted diff 也扩大；
  - 因此当前 retained 规则仍是按 target requested page count 触发对应次数的 L1 modeled eviction pressure。
- 用最终保留代码重跑当前 suite 的四向结果并同步文档：
  - S1A self `async_elision_current_s1a_self_capacity_target_pages_final`：final match，L1 `25/25`、
    L2/backuped `67/67`、dirty `0/0`、evicted `42/42`、locked `0/0`；
  - S1B self `async_elision_current_s1b_self_capacity_target_pages_final`：L1/dirty/locked match，normal run
    仍有 L2/backuped/evicted `56/55`、missing `13`、extra `14`；
  - S1B self `async_elision_current_s1b_async_elided_no_lock`：C++ 模型侧 diagnostic async-elision 后 final match；
  - S1A->S1B `async_elision_current_s1a_to_s1b_capacity_target_pages_final`：L1/dirty/locked 已 match，
    L2/backuped/evicted 仍是 `56/55`、missing `13`、extra `14`，形态与 S1B self normal 一致；
  - S1B->S1A `async_elision_current_s1b_to_s1a_capacity_target_pages_final`：L2/backuped/dirty/locked match，
    L1 missing `13`、evicted extra `13`。
- 当前结论不变但更精确：
  - self-config 已基本闭环：S1A normal pass，S1B 排除 async 后 pass；
  - cross-config 尚未证明“排除 async 后无其他问题”，因为 lifecycle/control-flow 输入契约仍未闭环，不能用
    timestamp oracle injection 做假对齐；
  - 下一轮 goal 应先把 lifecycle/control-flow 的 target-independent contract 定义清楚，再做四向逐 trace 分岔归因。

## 2026-06-12 02:08:28 +0800

- 修复并补完整 `scripts/internal/hicache_state_cross_input_audit.py` 中上轮未完成的 `AnchorContext.request_bound_token_events`：
  - 新增 temporal anchor 诊断：对 cache-stage event 先找同一 stream 中最近的 preceding request-bound token event，再按 target page size
    投影；
  - 新增 `temporal_projected_match_count`、`temporal_target_candidate_match_count`、
    `temporal_resolved_projection_count`、`temporal_unresolved_projection_count` 和
    `nearest_request_bound_anchor_status_counts`；
  - 保持诊断只输出 audit 字段，不改变 `cross_input_contract_ready` 判定，也不让正常 C++ 模型消费 target/oracle/source_actual。
- 新增 `run_cross_input_temporal_anchor_fixture`，覆盖“两个 request 共享同一个 source page-aligned prefix，但 target page size
  下投影长度不同”的歧义场景：
  - 全局候选只能说明 target path 在候选集合里；
  - temporal anchor 能在 source->target 方向把该歧义收敛成确定投影；
  - reverse 方向仍可能只能是 candidate，这一点被 fixture 明确锁住，避免把诊断误说成证明。
- 重新生成两向真实 cross audit 后，`unbound_match_prefix_paths` / `insert_paths` 的新结论更强：
  - S1A->S1B：两层各 `100` 个 paired event，global projection `32 exact + 64 candidate + 4 empty`；temporal anchor 后变成
    `96 exact + 0 candidate + 4 empty`，`temporal_unresolved=0`；
  - S1B->S1A：两层各 `100` 个 paired event，temporal anchor 后为 `36 exact + 60 candidate + 4 empty`，
    `temporal_unresolved=0`；
  - 因此 unbound/insert 不像任意 source actual，更像 request-bound token facts 经 page-size / temporal cache-stage anchor
    派生出的 path。
- `request_lifecycle_paths` 仍未解决：
  - 两向都是 `150` 个 paired event，global 与 temporal projection 均 `resolved=0, unresolved=150`；
  - 状态分布是 `no_token_ids` 与 `no_temporal_anchor`，first mismatch 是相同 token_count 但不同 path hash；
  - C++ 当前对 `request_admission` / `request_cache_lifecycle` 只更新 request-scoped token store，不直接产生 resident/dirty/evicted
    transition，所以它是 cross 输入契约阻塞点，不是当前 final set 小修入口。
- 当前路线更新：
  - unbound/insert 的下一步应是机制级 target-derived/page-aligned path 推导，并带 cache-stage/temporal anchor；
  - lifecycle 需要新增高层 invariant、target-side lifecycle derivation，或明确降级为 evidence；
  - 在 lifecycle/control-flow 输入契约未闭环前，cross-only L1/L2/evicted diff 仍不能当 deterministic state rule bug 修。

## 2026-06-12 01:50:00 +0800

- 在 cross input audit 中新增 request-bound anchor 诊断：
  - 每个 role/layer 现在会统计 path 是 `exact_request_bound_path`、`page_aligned_token_prefix`、`token_count_only`、
    `unanchored` 还是 `no_path`；
  - fixture 新增 token ids 级 page-aligned prefix 用例，覆盖 request-bound path 的向下 page-align 推导候选。
- 重新生成两向 cross audit 后得到更细的输入契约结论：
  - request-bound anchor 两边都是 `18` 个唯一 path，token counts 都是 `5, 777, 832, 928`；
  - S1A 按 128 page size 向下对齐后是 `768, 896`，S1B 按 64 page size 向下对齐后是 `768, 832, 896`；
  - S1A->S1B 的 `unbound_match_prefix_paths` / `insert_paths` source 为
    `page_aligned_token_prefix=96, unanchored=4`，target 为
    `exact_request_bound_path=60, page_aligned_token_prefix=36, unanchored=4`；S1B->S1A 方向对称；
  - `request_lifecycle_paths` 两向都是 `unanchored=150`。
- 路线更新：
  - unbound match-prefix / insert path 大多不是任意 source actual，更像 request-bound token facts 按 page size
    派生出的 cache-stage path，下一步可优先考虑 target-derived/page-aligned 推导；
  - lifecycle path 不能靠 front-door prompt path + page size 推出，需要高层 invariant、target lifecycle 推导，
    或明确降级为 evidence，不能混在 workload facts 里消费。

## 2026-06-12 01:40:00 +0800

- 在 `scripts/internal/hicache_state_cross_input_audit.py` 中新增 contract layer 汇总：
  - `request_bound_match_prefix_paths`
  - `unbound_match_prefix_paths`
  - `insert_paths`
  - `request_lifecycle_paths`
  - `source_control_flow_checkpoints`
- 重新生成当前 suite 两向 cross audit 后，blocking 范围进一步缩小：
  - `request_bound_match_prefix_paths` 两向都是 `200/200` aligned，说明 request-scoped `request_tokens` /
    `lookup_path` 当前可作为最接近 front-door token facts 的候选层；
  - `unbound_match_prefix_paths` 两向都是 `100/100` 但首个分岔为 `768` vs `832`；
  - `insert_paths` 两向都是 `100/100` 且全部 unbound，首个分岔同样是 `768` vs `832`；
  - `request_lifecycle_paths` 两向都是 `150/150`，token count 分布一致但 path hash 分岔；
  - `source_control_flow_checkpoints` 仍是 medium risk，数量为 S1A->S1B `1486/1450`、S1B->S1A `1450/1486`。
- 结论更新：
  - cross 的下一步不应再围绕 L1/L2/evicted final diff 打补丁；
  - 应先把 request-scoped front-door token facts 与 unbound cache-stage path、insert mutation path、lifecycle
    committed/fill path 拆开，重新定义哪些事实能作为 cross prediction 输入，哪些必须由 target model 推导或升格为新 invariant。

## 2026-06-12 01:25:00 +0800

- 扩展 cross-config input-contract 审计结果记录：
  - `scripts/internal/hicache_state_cross_input_audit.py` 现在输出 `request_bound` / `unbound` 计数、event shape
    samples 和 token count samples，用于区分“事件形态不一致”和“同形态事件的 key/path value 不一致”；
  - fixture 覆盖 request-bound 与 unbound 的 `request_tokens`，确认增强字段不改变原有 pass/fail 语义。
- 用当前 S1A/S1B suite 重新生成两向 cross audit 后，结论更清楚：
  - `request_tokens` / `lookup_path` 两边都是 `150` 个 completed event，binding shape 都是
    `100 request_bound + 50 unbound`，但第 16 个 unbound event 已经是 S1A `768` tokens vs S1B `832`
    tokens；
  - `insert_path` 两边都是 `100` 个 completed event 且全部 unbound，第 8 个 event 已是 `768` vs `832`；
  - 这说明 cross 的首要问题不是 request id、timestamp 或事件 shape，而是当前 invariant input 里混入了
    cache-stage/control-flow key path，尚未满足 cross prediction 的 target-independent 输入契约。
- 更新 goal plan、validation 记录和 defect 清单：
  - 最好结果被明确写成：修正 cross 输入契约后，四向 prediction 都做逐 trace 对比；如果剩余分岔都能归入
    async/input-boundary，且 C++ 模型侧 async-elision 后 final sets 全对齐，才宣称“排除 async 后模型其他部分没有问题”；
  - 当前状态仍只能说 self-config 已基本闭环，cross-config 还缺输入契约修正与复测。

## 2026-06-12 01:22:09 +0800

- 新增 front-door workload 诊断工具 `scripts/internal/hicache_state_workload_input_audit.py`：
  - 只读取 benchmark `workload_report.json`，比较 request shape args、request sequence identity、cache write policy
    和 response observation；
  - 新增 `run_workload_input_audit_fixture`，验证 prompt/phase 序列一致时 `frontdoor_workload_ready=true`，同时保留
    write policy 和 response bytes 的差异报告；
  - 修复脚本中 response observed key 常量引用错误，并通过 `python3 tests/run_hicache_state_fixtures.py`、
    `python3 -m py_compile ...` 和 `git diff --check`。
- 用当前 S1A/S1B suite 生成两向 workload audit：
  - S1A workload -> S1B workload 输出
    `01_s1a_manual/modeling/async_elision_current_s1a_to_s1b/workload_input_audit_s1a_to_s1b.json`；
  - S1B workload -> S1A workload 输出
    `03_s1b_manual/modeling/async_elision_current_s1b_to_s1a/workload_input_audit_s1b_to_s1a.json`；
  - 两向均为 request count `24/24`、request shape match、request sequence match、`frontdoor_workload_ready=true`；
    cache write policy 和 response bytes 不同符合预期。
- 结论更新：
  - S1A/S1B 的入口 workload prompt/phase 对齐，因此 cross 分岔不是 workload_report 层面的请求不一致；
  - 但 `request_tokens` / `lookup_path` 等 HiCache invariant input 仍不对齐，因为它们记录的是
    `HiRadixCache.match_prefix(params.key)` 的 cache-stage key path，不保证等同 HTTP 原始 prompt；
  - 下一轮 goal 的最好结果应表述为：self-config 已能在排除 async 后闭环；cross-config 要先修正输入契约，之后才能判断
    “各种 prediction 下除 async 外没有其他问题”。

## 2026-06-12 00:48:31 +0800

- 新增 cross-config input-contract 诊断工具 `scripts/internal/hicache_state_cross_input_audit.py`：
  - 只消费 normal invariant input trace，不进入 C++ state model，也不使用 oracle state；
  - 按 completed/end 的 HiCache invariant roles 提取 token path/span 指纹，不按 request id 或 timestamp 对齐；
  - 输出每个 role 的 source/target event count、签名 multiset/sequence 是否匹配、首个分岔和风险分类；
  - 新增 `run_cross_input_audit_fixture`，验证同一 token path 在不同 scope/request_id/source page size 下不会误报，
    而 `capacity_request.requested_pages_source` 不一致会被标成 source-control-flow / async-boundary 风险。
- 用当前 suite 跑两向 cross audit：
  - S1A source -> S1B target 输出
    `01_s1a_manual/modeling/async_elision_current_s1a_to_s1b/cross_input_audit_s1a_to_s1b.json`，
    source/target event count `2036/2000`，`cross_input_contract_ready=false`；
  - S1B source -> S1A target 输出
    `03_s1b_manual/modeling/async_elision_current_s1b_to_s1a/cross_input_audit_s1b_to_s1a.json`，
    source/target event count `2000/2036`，`cross_input_contract_ready=false`；
  - 两向 high-risk roles 都包括 `request_tokens`、`lookup_path`、`insert_path`、`request_cache_lifecycle`；
    第 16 个 `request_tokens` / `lookup_path` completed event 已分岔为 S1A `768` tokens vs S1B `832` tokens。
- 结论更新：
  - self-config 仍保持原结论：S1A self normal pass，S1B self async-elided C++ pass；
  - cross-config 现在有更强证据说明不能直接继续 async oracle injection，也不能把 cross-only final diff 当作 state rule bug 修；
    下一步必须先修正或重新定义 cross 输入契约。

## 2026-06-12 00:35:27 +0800

- 完成 HiCache state model async-elision 诊断阶段收束：
  - 新增/保留 `scripts/internal/hicache_state_trace_divergence.py`、`scripts/internal/hicache_state_async_elision.py`
    和 C++ `diagnostic_state_injection` 路径，用于诊断性排除 async/input-boundary 分岔；
  - 新增 `tests/run_hicache_state_fixtures.py` 中 diagnostic injection fixture，验证正常 trace 不出现 diagnostic role/warning，
    synthetic trace 才能消费 oracle state，并输出明确 warning；
  - 回归通过：`cmake --build build --target trace_graph -j 8`、`python3 tests/run_hicache_state_fixtures.py`、
    `python3 tests/run_modeling_smoke_fixtures.py`、`python3 tests/run_hicache_mainline_config_fixtures.py`、
    `python3 tests/run_profiling_fixtures.py`、`git diff --check`。
- 基于 `20260611_054436_profiling_hicache_state_mainline_one_matrix` 重跑当前 S1A/S1B：
  - S1A self normal prediction 已 `final_state_match=true`，active sets 全对齐；逐 trace 里只看到 locked 暂态可见性差异，
    final locked 仍是 `0/0`；
  - S1B self normal prediction 仍有 L2/backuped/evicted `56/55`，missing `13`、extra `14`；
  - 排除 locked 暂态后，S1B trace divergence 识别 6 个 async 分岔；用 synthetic trace 跑 C++ 模型侧 async-elision 后，
    S1B self `final_state_match=true`，L2/backuped/evicted 全部 `55/55`；
  - 结论：S1B self 的 normal extra/missing 是 async/input-boundary 分岔后的连锁差异，不是当前已发现的 deterministic
    final-set model bug。
- cross-config 尚未关闭：
  - 该条记录的是当时尚未应用 temporal anchor projection 与 target page-size capacity 修正的中间结果；当前最终
    S1A->S1B retained diff 见 2026-06-12 02:59:50 条目；
  - S1B->S1A 的 L2/backuped/dirty 已 match，但 L1 missing `13` 与 evicted extra `13` 配对，表示有 13 页真实 final 在 L1、
    模型 final 留在 evicted；
  - S1A 和 S1B 的 trace 时间窗口不重叠，不能用 target oracle 按 timestamp 注入 source trace；下一步必须先做
    token/workload logical alignment 和 invariant role target-independence 审计，再判断 cross-only diff 是否为模型 bug。
- 文档同步：
  - `docs/validation/hicache_state_model_goal_plan.md` 改成下一轮可消费的 async-elision / cross-alignment goal handoff；
  - `docs/validation/hicache_state_validation.md` 新增 `HCSV-20260612-async-elision-current-self-and-cross`；
  - `docs/validation/hicache_state_model_defects.md` 更新缺陷优先级：S1B self async-elided 已通过，当前 P0 转向
    async 输入边界和 cross logical alignment。

## 2026-06-11 23:30:30 +0800

- 基于 `s1b_self_host_node_projection` 完成 HiCache state model 本轮收束审查：
  - `HiCacheTokenRadixTree` 增加 page-level compressed radix projection 后，host eviction pressure 已能按 host leaf group
    淘汰 parent host leaf；
  - 新增 fixture `run_prefetch_host_pressure_promotes_parent_host_leaf_fixture` 覆盖 parent host leaf 被提升为可淘汰 leaf；
  - 本轮验证继续满足 `invariant_coverage_ready=true`、`missing_invariant_facts=[]`、`non_invariant_fact_usage=[]`；
  - normalized final 从前置 `L1 32/28, dirty 31/28, L2/backuped 81/55, evicted 80/55` 收敛到
    `L1 28/28, dirty 28/28, L2/backuped/evicted 56/55`；
  - final diff 仍是 L2/backuped/evicted missing 13、extra 14，不能宣称 state prediction 通过。
- 逐 trace 对齐显示旧首个 maintenance 分岔已消失：
  - last matched snapshot 是 `hicache_maintenance_check_end:state_snapshot`，order `4200`，counts 为 L1/dirty `28`、
    L2/backuped/evicted `56`；
  - 新首个分岔是 `hicache_prefetch_check_point_end:state_snapshot`，order `4204`，ts `1781155586790332`；
  - oracle 在该 checkpoint 将 L2/backuped/evicted 从 `56` 推到 `69`，model 仍为 `56`，缺 13 个 storage prefetch
    完成插入的 host pages。
- 结论：host projection 是合理的机制级小修；下一步不应继续围绕 leaf group 打补丁，也不应添加
  “best_effort checkpoint 全 pending prefetch 完成”的特化规则。剩余 blocker 应归类为 async prefetch completion /
  storage lifecycle 的模型或 invariant 输入边界问题。

## 2026-06-11 14:46:45 +0800

- 基于 `20260611_050859_profiling_hicache_state_mainline_one_matrix/03_s1b_manual` 的 S1B 31-target profile 修正
  HiCache backend：
  - zero-token span (`begin == end`) 作为合法空路径处理，不再报 `token_dictionary_or_full_path_span` /
    `token_dictionary_or_logical_path_span`；
  - `request_admission` / `request_cache_lifecycle` 改为 request scoped token context 输入，更新 token store 但暂不产生
    resident/dirty mutation；
  - `maintenance_checkpoint` 改为显式 no-op 边界事件，不再计入 `unimplemented_invariant_role.*`；
  - fact replay 排序改为严格全局时间顺序，同 timestamp/scope 下用 `seq_no` 破 ties，修复旧 comparator 非全序导致的
    lock/ref delta 乱序；
  - `capacity_request.requested_pages` 接入 L1 modeled eviction pressure，victim 仍由 modeled LRU/lock 规则选择，不消费 source victim；
  - 新增 `tests/run_hicache_state_fixtures.py::run_context_roles_and_zero_span_fixture`，并同步 capacity fixture 语义。
- 最新 S1B self output：
  `data/profile_runs/sglang/20260611_050859_profiling_hicache_state_mainline_one_matrix/03_s1b_manual/modeling/s1b_self_fast_pressure_ts_order_capacity`。
- 最新验证结果：
  - `validation_errors=["hicache_final_state_mismatch"]`；
  - `invariant_coverage_ready=true`，`missing_invariant_facts=[]`，`non_invariant_fact_usage=[]`；
  - normalized final `locked_pages=0/0` 已对齐；
  - normalized final L1 `32/28`，missing 0、extra 4；dirty `31/28`，missing 0、extra 3；
  - normalized final L2/backuped `81/55`，missing 15、extra 41；evicted `80/55`，missing 16、extra 41。
- 结论：本轮修复了输入 coverage 误报、zero-token path 和 lock/ref replay order；剩余阻塞集中在 write-back async
  flush / host release / L2 eviction 生命周期，不能通过消费 `source_actual` 强行对齐。

## 2026-06-11 13:16:25 +0800

- 查看 `20260611_050859_profiling_hicache_state_mainline_one_matrix/01_s1a_manual/logs/server.log`：
  - S1A 不再是 bench timeout，而是在 server ready 前退出，manifest 记录 `server exited before ready, code=-9`；
  - 根因是 NPU `kernel_ascend` 路径把 HiCache host layout 改写为 `page_first_direct`，但 S1A 的
    `--max-total-tokens=2048`、`page_size=128`、`hicache_ratio=2.25` 只生成 `37` 个 host pages；
  - `MHATokenToKVPoolHost.__init__` 初始化 per-layer refs 时按 64 层索引 host buffer，触发
    `IndexError: index 37 is out of bounds for dimension 0 with size 37`；
  - `continue_on_error=true` 已生效，S1B 继续启动并运行 manual workload；
  - 修正 S1A fast-pressure 配置为 `--max-total-tokens=4096`，建模容量同步为 page128 L1/L2 `32/73`，
    覆盖 13:04 记录中的 S1A `2048` / `16/37` 临时方案；
  - 保持 workload 和 31-target 采集契约不变；S1B 仍为 page64 L1/L2 `32/81`。

## 2026-06-11 13:04:37 +0800

- 处理 31-target HiCache profiling 迭代过慢的问题：
  - 最新 `20260610_172720_profiling_hicache_state_mainline_one_matrix/01_s1a_manual` 的 bench 在
    `reuse_A_after_pressure seq=49 prompt_id=A_0` 触发 `TimeoutError: timed out`，单请求等待约 `600001 ms` 后
    `bench command failed, code=1`，因此 suite 在 S1A 后中断，未继续执行 S1B；
  - 该 run 的 trace 质量审计仍显示 `quality_ready=true`、31 个 configured target 中 observed 29 个，缺失的是未触发的
    `schedule_policy.chunked_admission` / `schedule_policy.chunked_admission_observed`，required invariant coverage ready；
  - 不缩减 31-target 采集契约，改为缩小默认 manual profile 的 workload 和 L1 token budget：
    `L1_manual_phased` 从 83 requests 降到 24 requests，`shared_prefix_repeat` 从 160 降到 96，
    `unique_suffix_repeat` 从 16 降到 8，phase wait 从 2s 降到 0.5s，request timeout 从 600s 降到 300s；
  - server `--max-total-tokens` 从 8192 降到 2048，以小 L1 保证小 workload 仍能触发 pressure/eviction/load-back；
  - S1A modeling capacity 同步为 page128 L1/L2 `16/37`，S1B 同步为 page64 L1/L2 `32/81`；
  - suite 顶层启用 `continue_on_error=true`，单个实验失败不会阻断后续已选择实验；
  - 同步 `configs/modeling/hicache_state/modeling_hicache_state_mainline_one_prediction_s1*.json`、profiling/validation 文档和
    `tests/run_hicache_mainline_config_fixtures.py`；
  - 已通过 JSON 校验、`python3 tests/run_hicache_mainline_config_fixtures.py` 和 `python3 tests/run_profiling_fixtures.py`。

## 2026-06-11 02:09:40 +0800

- 收尾审查 HiCache backend 阶段 2/3/4 最小建模：
  - 确认当前 C++ backend 不再引用旧 `hicache_radix_tree` 源文件，`CMakeLists.txt` 已只编译 router、token store、target pager、
    token radix tree 和 state index；
  - 确认 `prefetch_intent` 没有作为 C++ invariant 分支残留，当前只保留 profiling 契约中的
    `prefetch_intent_observed` source_actual；
  - 修正 `docs/validation/hicache_state_model_defects.md` 中旧 radix 描述措辞，避免被误读为当前结构；
  - 重新通过 `cmake --build build --target trace_graph -j 8`、`python3 tests/run_hicache_state_fixtures.py`、
    `python3 tests/run_modeling_smoke_fixtures.py`、`python3 tests/run_hicache_mainline_config_fixtures.py`、
    `python3 tests/run_profiling_fixtures.py`、`find configs -name '*.json' -print0 | xargs -0 -n1 jq empty`、
    Python compile、C++ `clang-format --dry-run --Werror` 和 `git diff --check`。

## 2026-06-11 02:03:41 +0800

- 在手动 31-target profiling 完成前，先完成 HiCache backend 阶段 2/3/4 的最小建模：
  - 新增 `hicache_token_store.hpp/.cpp`，request token path 由 token store 维护，不再在 state model 中保存 request pages；
  - 新增 `hicache_target_pager.hpp/.cpp`，target page hash 由 token path、target page size 和 cache scope 推导；
  - 新增 `hicache_token_radix_tree.hpp/.cpp`，按 token 维护 radix prefix/split/insert，`prefetch_decision` 和 `insert_path`
    均使用 token radix longest-prefix；
  - 删除旧 `hicache_radix_tree.hpp/.cpp`，不再保留 page-level radix backend 入口；
  - 新增 `hicache_state_index.hpp/.cpp`，resident、dirty、backuped、evicted、locked、prefetch 和 hit count 集合从
    `HiCacheState` 拆到 state index；
  - `HiCacheState` 现在主要做 orchestration：按 role 调用 token store、target pager、token radix 和 state index；
  - 新增最小验证：target page size projection fixture、token radix split projection fixture；
  - 已通过 `cmake --build build --target trace_graph -j 8`、`python3 tests/run_hicache_state_fixtures.py`、
    `python3 tests/run_modeling_smoke_fixtures.py`、`python3 tests/run_hicache_mainline_config_fixtures.py`、
    `python3 tests/run_profiling_fixtures.py`、`find configs -name '*.json' -print0 | xargs -0 -n1 jq empty`、
    Python compile、filesystem C++ `clang-format --dry-run --Werror` 和 `git diff --check`。

## 2026-06-11 01:51:31 +0800

- 开始 HiCache backend 重构阶段 1：
  - 新增 `src/modeling/trace_graph/include/trace_graph/modules/hicache/hicache_router.hpp` 和
    `src/modeling/trace_graph/src/modules/hicache/hicache_router.cpp`；
  - router 现在集中维护 31-target invariant role enum、`fact_class=invariant_state && state_model_input=true` 门禁和 required field 检查；
  - C++ backend 不再把旧 `prefetch_intent` 当 invariant role，`prefetch_decision` 由 target radix longest-prefix 计算 planned suffix；
  - `request_cache_lifecycle`、`request_admission`、`maintenance_checkpoint` 已被识别但暂未实现，会显式计入
    `missing_invariant_facts["unimplemented_invariant_role.<role>"]`，不再静默吞掉；
  - `tests/run_hicache_state_fixtures.py` 已改为 current `prefetch_decision` fixture，并把缺 dictionary 的 invariant 断言改成 router
    拒绝后不计入 processed；
  - 本轮验证通过：`cmake --build build --target trace_graph -j 8`、`python3 tests/run_hicache_state_fixtures.py`、
    `python3 tests/run_modeling_smoke_fixtures.py`、`python3 tests/run_hicache_mainline_config_fixtures.py`、
    `python3 tests/run_profiling_fixtures.py`、`find configs -name '*.json' -print0 | xargs -0 -n1 jq empty`、
    Python compile、clang-format dry-run 和 `git diff --check`。

## 2026-06-11 01:38:04 +0800

- 暂停旧 HiCache backend 的小修小补，明确后端重构最终架构：
  - `docs/modeling_development.md` 已把现有 `HiCacheState` / page-level `HiCacheRadixTree` 标为过渡实现，后续不保留兼容；
  - 新后端以 token-level radix tree 为 source of truth，page set 只作为 target page size projection；
  - 后端边界拆成 `HiCacheFactRouter`、`TokenPathStore`、`TargetPager`、`TokenRadixTree`、`NodeStateIndex`、`RequestState`、
    `PolicyEngine`、`AsyncState`、transition summary 和 validation；
  - 31-target mainline S1A/S1B 采集契约在当前 scope 内冻结，profile quality 通过后的 mismatch 默认归类为 backend model/rule 缺陷；
  - 只有 profile quality 失败、新 scope 或 SGLang upstream hook 语义边界变化，才重新讨论新增采集 target；
  - 同步 `project_constraints.md` 和 validation 缺陷文档，旧 2026-06-10 四向结果只作为 page-level backend 失败证据，
    不再作为当前 31-target 契约的验收结果。

## 2026-06-11 01:29:00 +0800

- 对 27-target HiCache 采集契约做第三轮完整性审计，确认 admission/chunked continuation 是后端重放 load-back、lock/ref 和截断边界时的缺口：
  - 主配置从 27 个 target 扩到 31 个 target；
  - 新增 `schedule_policy.prefill_admission` / `schedule_policy.chunked_admission` 两个 `request_admission` invariant target；
  - 新增并行 `schedule_policy.prefill_admission_observed` / `schedule_policy.chunked_admission_observed`，只记录 source admission return、request runtime 和 phase-scoped budget snapshot；
  - invariant admission target 只携带 request token dictionary/span、`admission_kind`、chunk/truncation 参数、priority/max_new_tokens 和 policy，不携带 `request_runtime`、return result、source prefix/host hit 或 source last node；
  - `profile_quality.py` 已新增 `request_admission` required fields 和 token/span 检查；
  - `tests/run_profiling_fixtures.py` fake SGLang 已覆盖 `PrefillAdder.add_one_req` 和 `add_chunked_req`，并验证 31 个配置 target 全命中。

## 2026-06-11 01:04:01 +0800

- 针对“不要反复重跑 profile”的要求，对 HiCache 采集契约做二次审计并继续收紧：
  - 主配置从 21 个 target 扩到 27 个 target；
  - 从 invariant 中移除 source-result 字段：`scheduler.prefetch_decision` 不再携带 source `prefix_indices/host_hit_length/last_host_node`，
    `lock_scope_delta` 不再携带 source return `delta`，`ready_to_load_host_cache` 不再携带 source `producer_id`；
  - 新增并行 `source_actual` observed targets：request lifecycle observed、scheduler prefetch decision observed、
    capacity result observed、lock/ref result observed；
  - `insert_path` invariant 补充 `chunked`、`priority`、`prev_prefix_len`，避免后端重建 hit-count/eviction priority 时缺输入；
  - Python probe 新增 request runtime / scheduler prefetch state source，并补内部 hook：host ref delta、load-back、write-back
    enqueue/start、host eviction、prefetch rate-limit/terminate/abort/pop 等均以 `source_actual` 采集；
  - `tests/run_profiling_fixtures.py` 已改为加载主配置并验证 27-target 契约，不再用旧 `page_hashes/page_identity/radix_removed` fixture。

## 2026-06-11 00:39:24 +0800

- 为下一轮 HiCache profile 重采收紧采集契约，目标是尽量一次补齐后端 token/node/ref/async 重构需要的事实：
  - `configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json` 从 12 个 configured targets
    第一轮扩展到 21 个 targets；当前契约已在 2026-06-11 01:04:01 继续收紧到 27 个 targets；
  - 新增 invariant roles：`request_cache_lifecycle`、`prefetch_decision`、`maintenance_checkpoint`，用于补齐
    request-level committed/fill token path、scheduler prefetch decision checkpoint 和 async 维护检查点；
  - 将 `lookup` matched result、`insert` prefix/result、`prefetch_from_storage` actual intent 和 prefetch progress
    拆为 `source_actual` observed targets，避免 source actual 混在 `invariant_state` 事件里；
  - `sglang_hicache_callable.py` 新增 request token source、node summary/chain、evictable snapshot、prefetch progress source，
    并自动 patch HiCache 内部 radix split/delete、evictable delta、node store/remove、write/load ack、storage control、
    storage hit query、prefetch terminate 等 source_actual provenance；
  - `profile_quality.py` 已同步新 invariant role 的 required fields，不再要求旧 `lookup_path.matched_span` 或
    `insert_path.prefix_len` 这类 source result 字段；
  - 文档已同步：这一版 21-target 契约尚未真实重跑，旧 S1A/S1B 四向 validation 仍是 2026-06-10 旧 profile 的结果；
    当前需要以后续 27-target 契约为准。

## 2026-06-10 21:37:55 +0800

- 基于四向 validation 和逐 page provenance 做第一轮 HiCache state model 保守修复：
  - 新增 `scripts/internal/hicache_state_provenance.py`，从 `validation.json`、`predicted_target_cache_state_trace.json`
    和 oracle state snapshots 输出 mismatch page 的 model transition、oracle membership changes 和 fixability hint；
  - `capacity_request` 不再把 `requested_pages` 当作可观测 victim 数，也不指定 source victim；
  - `wait_complete` 的 `prefetch_check_point` 不再把 pending planned pages 全量构造成 L2/L3 resident 或 ready，
    finalize 也不再把 wait_complete 未 ready pages 全量 suppressed；
  - 更新 HiCache fixtures，去掉旧的 precise capacity eviction / full prefetch ready 假设；
  - 重跑四向 prediction / validation：四个方向仍 final mismatch，但 S1B self 的 L1 missing 从 46 降到 44、
    L2/backuped/evicted extra 从 64 降到 62，S1B->S1A 的 S1A target L1/L2/backuped/evicted mismatch 明显收窄；
  - 文档已同步：当前能修的是不变量边界内的过度推导；lock/ref chain、write-back flush exact cleanup、
    async prefetch exact ready 和 victim/order 仍需要更强事实或更完整 radix/ref-chain 模型，不能从 oracle 强行补事件。

## 2026-06-10 19:23:32 +0800

- 完成 S1A/S1B mainline-one manual profile 后的四向 HiCache state prediction / oracle validation：
  - S1A self、S1B self、S1A -> S1B、S1B -> S1A 均使用 token-invariant facts，`invariant_coverage_ready=true`、
    `missing_invariant_facts=[]`、`non_invariant_fact_usage=[]`；
  - 四个方向的 final state 均为 mismatch，说明当前阻塞点是 C++ state model，不是采集目标分流或 token dictionary 覆盖；
  - S1A self 仍缺 L1 resident 22、L2/backuped 28，且 L1 missing 页会落到 evicted extra；
  - S1B self 暴露 write-back/best-effort 更明显的问题：L1 missing 46、L2/backuped extra 64、dirty missing 10、locked missing 22；
  - S1A -> S1B 与 S1B -> S1A 分别验证出 target=S1B 的 capacity/write-back over-fill，以及 source=S1B 的 lock/ref 全丢；
  - 修正 `configs/modeling/hicache_state/modeling_hicache_state_mainline_one_prediction_s1b.json`，目标 S1B 的 prediction
    现在要求 `require_oracle_state_trace=true` 并使用 `oracle_page_key_mode=strip_scope`，避免 final mismatch 被误判成 ready；
  - `docs/validation/hicache_state_validation.md` 和 `docs/validation/hicache_state_model_defects.md` 已同步为四向结果和新的缺陷优先级。

## 2026-06-10 18:20:00 +0800

- 同步 HiCache token-invariant 后端后的主线文档：
  - `README.md` 改为当前项目结构入口，删除旧 `merge_all_traces.py`、`inspect_hicache.py`、`cache_io` what-if 等已不匹配说明；
  - `docs/profiling_development.md` 重写为当前 profiling runner、suite、Python probe source、HiCache `fact_class` 分类和 token/span 契约；
  - `docs/modeling_development.md` 重写为当前 C++ TraceGraph / HiCache backend 结构，明确后端只消费
    `fact_class=invariant_state && state_model_input=true`，target page 由 token dictionary/span 和 target page size 重建；
  - `docs/validation/hicache_state_validation.md` 更新为当前有效结果 `HCSV-20260610-token-backend-s1a`：
    profiling quality 和 invariant coverage 通过，但 normalized oracle final state 仍 mismatch；
  - `docs/validation/hicache_state_model_defects.md` 删掉 page-identity 依赖和旧 movement apply 分支这类过期缺陷，改为当前 S1A
    mismatch 驱动的 resident/evicted、selective write、capacity/evictable、radix、prefetch/writeback 缺口；
  - 删除 `docs/validation/hicache_state_validation_legacy.md`，历史只读文档不再作为 active 阅读入口；旧条目只保留在本时间线中作为历史背景。

## 2026-06-09 22:25:56 +0800

- 重构 HiCache state mainline-one profiling 配置归档方式：
  - `configs/experiments/hicache_state/` 现在只保留
    `profiling_hicache_state_mainline_one_matrix.json`；
  - 删除 S1A/S1B × manual/bench 的四个单实验 profiling config，改由一个 suite matrix 展开
    `s1a_manual`、`s1a_bench`、`s1b_manual`、`s1b_bench`；
  - suite 内只有两个 server 维度 `s1a` / `s1b` 和两个 input 维度 `manual` / `bench`，manual input
    通过 `{metadata.hicache_write_policy}` 引用 server 维度的 write policy；
  - shared profiling 同时采集 `target_page_identity_page64` 和 `target_page_identity_page128`，C++ HiCache
    state model 按目标 `page_size` 选择对应 target identity；
  - profiling runner 的 radix removed materialization 同步输出
    `target_radix_removed_page_identity_page<page_size>`；
  - 文档已记录 page-size-specific target identity 只是有限矩阵过渡方案，长期应转向 size-independent
    token path digest / range hash。

## 2026-06-09 22:08:50 +0800

- 完善 profiling suite 配置机制，方便后续手动运行 profiling 和归档配置：
  - `scripts/internal/profile_runner.py` 支持 `matrix.servers[] × matrix.inputs[]` 自动展开实验，也支持
    `experiments[].server_ref/input_ref` 显式选择归档组合；
  - 同一个 suite 内强制共享 `profiling`，矩阵项和 experiment 不能覆盖或 unset 采集配置；
  - runner 和 `scripts/profile.sh` 均支持 `--list-experiments`、重复 `--experiment` 和逗号形式
    `--experiments`，便于先查看稳定实验 id，再选择一组手动运行；
  - suite 输出会写入 `suite_config.json`、`suite_selection.json` 和 `suite_result.json`，并在展开后的
    experiment metadata 中补充 suite/server/input id；
  - `tests/run_profiling_fixtures.py` 新增 dry-run 矩阵 fixture，覆盖展开顺序、子集选择、归档 metadata
    和 suite 内禁止覆盖 `profiling`。

## 2026-06-09 21:31:35 +0800

- 拆分 HiCache state validation 文档职责：
  - `docs/validation/hicache_state_validation.md` 现在只维护 active 内容：当前有效规则、术语约束、建模边界、复现入口、主线和后续新结果模板；
  - 新增 `docs/validation/hicache_state_validation_legacy.md`，只读保存 `HCSV-20260608-state-matrix`、
    `HCSV-20260609-mainline-one-manual-l1` 历史口径摘要、早期 S1A 排查和历史关键修复；
  - `docs/project_constraints.md` 已同步两个专项文档的职责，避免继续把旧结果和新规则堆在同一个入口。

## 2026-06-09 21:00:30 +0800

- 完成 HiCache profiling / state model invariant-only 审计：
  - C++ HiCache summary 新增 `non_invariant_fact_usage_by_role` / `non_invariant_fact_usage`，Python validation
    会在该字段非空时把 `invariant_coverage_ready` 置为 `false`；
  - fixture 覆盖缺省 observed lock/ref、observed `l3_to_l2_transfer`、observed `write_backup` 三类旧兼容路径；
  - 不重新 profiler，仅用现有 S1A/S1B manifest 做 modeling-only 重跑：`S1A` self-config 仍消费 lock/ref observed facts，
    `S1B` self-config 无 non-invariant usage 但 final state 不对齐；
  - `S1A -> S1B` 无 non-invariant usage 但 final state / timeline extra 仍失败，属于 state model 机制缺口；
  - `S1B -> S1A` 无 non-invariant usage，除 final locked page 外对齐，仍需要更强 lock/ref oracle 或 target lock 模型；
  - 因此 17:29 的 self-config 通过结论按当前 invariant-only 口径降级，旧 final/timeline 指标只能作为历史诊断，
    不能单独证明 state model 已闭环。

## 2026-06-09 20:29:08 +0800

- 对齐 HiCache state validation 术语和入口：
  - `replay` 只表示 `mode=faithful_replay` 的 trace graph baseline，不加载任何子模块；
  - HiCache state 不再维护带 observed/default 行为答案的重放入口，启用 HiCacheModule 的建模一律称为
    `self-config prediction` 或 `cross-config prediction`；
  - HiCache state model 在任何场景下都只能消费不变量 facts 和显式 target config，target actual trace、state snapshot、
    observed movement、oracle-only transient 和 debug 字段只能用于 validation / debug；
  - 删除缺省 target config 的长期入口，避免继续运行缺省 target config；
  - 更新 `docs/project_constraints.md`、`docs/validation/hicache_state_validation.md`、`docs/modeling_development.md`、
    `docs/profiling_development.md` 和 prediction config usage，使 oracle trace 统一写成 `<target_oracle_trace.json>`。

## 2026-06-09 17:29:17 +0800

- 完成 HiCache state validation 主线一 `L1_manual_phased` 的 S1A/S1B 逐 trace 对比：
  - 当前有效 run label 为 `20260609_053538_profiling_hicache_state_mainline_one_manual_s1a` 和
    `20260609_073205_profiling_hicache_state_mainline_one_manual_s1b`；
  - 两个 profile quality 均为 `quality_ready=true`，23/23 Python targets observed，stateful page identity 缺口为 `0`；
  - 两个 self-config prediction 均通过 final state validation，timeline 均为 `match=true`、`model_extra_transition_count=0`；
  - `S1A -> S1B` prediction 失败，最终 diff 集中在同一组 `36` 页：模型多 dirty、少 L2、少 backuped；
  - 逐页对齐 `S1B` 正确 self-config prediction 后确认，这 `36/36` 页真实路径都有 `hicache_write_backup_end` 产生
    `add_l2_resident + mark_backuped + clear_dirty`，而跨配置 prediction 缺少等价 target write-back flush 事实；
  - `S1B -> S1A` prediction 只剩一个 final locked page 缺口，属于 page-size what-if 下 lock/ref 非不变量的验证口径问题；
  - 已更新 `docs/validation/hicache_state_validation.md`，明确主线一当前不能宣称完成，后续需补强 target write-back flush oracle
    或调整主线一配置选择后重新验证。

## 2026-06-09 06:20:00 +0800

- 收紧 HiCache state validation 主线一配置新颖性执行口径：
  - 当前候选不再沿用早期主线一 2.0 草案签名；
  - `S1A_baseline_large` 改为 page128、L1/L2 capacity `64/145`、`write_through_selective`、`wait_complete`、ratio `2.25`、prefetch timeout extra config `8s/0/8s`；
  - `S1B_divergent_large` 改为 page64、L1/L2 capacity `128/321`、`write_back`、`best_effort`、ratio `2.5`、prefetch timeout extra config `6s/0/6s`；
  - `tests/run_hicache_mainline_config_fixtures.py` 增加 HCSV 历史签名黑名单，覆盖当前矩阵和早期主线一草案，不只依赖仍存在的配置文件或 `data/` 目录；
  - ratio 均保持 `>1.0`，容量压力仍由 workload 和显式 capacity snapshot 承担；旧 S1A 手工 profile 只保留为排查结论，不能作为新签名的主线一验收结果；
  - 曾短暂尝试的低 ratio `1.5/1.75` 草案导致真实 profile 明显慢速，未完成 run 不纳入主线一证据；
  - `2.25/2.5` 新签名配置已经固化，但本轮未完成的 20260609 profile 产物已清理，不能作为主线一 faithful replay / prediction 证据。

## 2026-06-09 05:58:00 +0800

- 固化 HiCache state validation 主线一配置新颖性为长期项目约束：
  - 两个场景不仅必须彼此不同，也必须不同于任何此前已经跑过的 HiCache state profiling 联合配置；
  - 判定依据为 page size、L1/L2 capacity、write policy、prefetch policy、prefetch timeout、`--hicache-ratio` 等核心项组成的联合签名；
  - 新增 `tests/run_hicache_mainline_config_fixtures.py` 固化该只读检查；
  - 当前 S1A/S1B 配置签名检查通过，四个主线一 profiling 配置按场景收敛为两套签名，且对仓库可见非主线一配置均为 `old_matches=0`。

## 2026-06-09 05:48:03 +0800

- 闭环 HiCache state validation 主线一 `S1A_baseline_large + L1_manual_phased` 预验证：
  - 真实 profile run label 为 `20260608_203828_profiling_hicache_state_mainline_one_manual_s1a`，workload `83/83 ok`、`errors=0`；
  - profile quality 为 `quality_ready=true`、23/23 Python targets observed，stateful required page identity `2130/2130`；
  - live `hicache_radix_removed_pages:self` 在真实 SGLang 中仍未稳定产出非空字段，因此 profiling runner 收尾新增 materialization：
    只从同一次 insert start/end validation snapshot 中派生 `radix_removed_page_identity`，本 run 写入 `2` 个 insert end、合计 `26` 页；
  - C++ HiCache state model 消费该 operation-level fact 后，state validation 达到 `validation_ready=true`、`final_state_match=true`、
    timeline `match=true`、`model_extra_transition_count=0`；
  - final counts 对齐：L1 `54/54`、L2 `106/106`、backuped `106/106`、evicted `52/52`、locked `1/1`、
    prefetch planned/ready/suppressed `356/18/338`；
  - 该批次现在只保留为 radix materialization 排查结果；当前入口必须使用显式 target config 的 self-config prediction。

## 2026-06-09 01:20:04 +0800

- 继续推进 HiCache state validation 主线一 `S1A_baseline_large + L1_manual_phased`：
  - 真实 profile 完成，`quality_ready=true`，23/23 Python targets observed，stateful required page identity `2130/2130`；
  - 初始 state validation 仍为 `hicache_final_state_mismatch`，model 多出 13 个 L2/backuped/evicted pages；
  - 定位原因为 insert 期间 radix leaf 消失，但第一版 `radix_removed_page_identity` 因 state snapshot 注入顺序在其后，672 个 insert start/end 字段均为空；
  - 修改 profiler 注入顺序：含 `hicache_radix_removed_pages:self` 的 target 会先采 validation-only `hicache_state:self`，再导出 model-input `radix_removed_page_identity`；
  - C++ state model 已消费 `radix_removed_page_identity`，同 page size 清理 L1/L2/dirty/backuped/evicted/locked 及辅助索引，page-size what-if 下跳过；
  - 本地 fake radix extractor、C++ HiCache fixture、派生 enriched state validation 均通过；派生结果达到 `validation_ready=true`、`final_state_match=true`、`model_extra_transition_count=0`；
  - 仍需用修复后的注入顺序重新跑真实 S1A manual profile，确认原生 trace 中非空 `radix_removed_page_identity` 能直接让 state prediction 通过。

## 2026-06-08 23:58:49 +0800

- 复核 HiCache state validation 主线一配置新颖性要求：
  - `S1A_baseline_large` 与 `S1B_divergent_large` 的联合配置签名彼此不同；
  - 签名口径包含 page size、L1/L2 capacity、write policy、prefetch policy、`--hicache-ratio`、prefetch extra config；
  - 脚本化扫描当前仓库可查的非主线一 HiCache state 实验配置，两个主线一签名均为 `old_matches=0`；
  - 对已清理且仓库中没有实体配置的历史 run，只依据验证文档保留的历史摘要和 config metadata 继续约束，不让 HCSV 依赖 `data/` 记录。

## 2026-06-08 23:39:51 +0800

- 收紧 HiCache state validation 主线一配置约束：
  - `S1A` / `S1B` 不仅必须彼此不同，还必须不同于任何此前已经跑过的 HiCache state profiling 配置组合；
  - “历史已跑配置”覆盖当前矩阵、已清理历史 run、临时验证 run 和失败后重跑前的草案配置，不能只按 `C0-C8` 判断；
  - 主线一候选配置开跑前需要做历史配置比对，并把比对结果写入 config metadata 或 HCSV 摘要。

## 2026-06-08 21:44:46 +0800

- 完成 HiCache state validation 本轮重验矩阵和主线一可执行部分：
  - `I0`、`I1`、`I2`、`I3` 四类输入下的 self-config / cross-config prediction 结果已内联到
    `docs/validation/hicache_state_validation.md`；
  - 当前矩阵覆盖 `C0-C7`、`write_back + low capacity`、page64、capacity、prefetch wait /
    best_effort / aggressive timeout 等配置；
  - 所有列入矩阵的 validation 均达到 `final_state_match=true`、`timeline match=true`、
    `model_extra_transition_count=0`。
- 修复 `I2 C0 -> C3_page64` prediction 中 prefetch ready `26/40` 的问题：
  - page size what-if 下，transfer completion 只要 source pages 覆盖同 request 的 schedule source pages，
    就把 completion credit 归到 target schedule pages；
  - 新增等长但 target identity 重新映射的 fixture，避免后续 page64 prefetch completion 回退。
- 将后续主线拆分：
  - `主线一`：两个强差异 HiCache 配置场景 + 两个大输入，包括手工 phased workload 和大型 bench serving；
  - `主线二`：用户手动完成大型 profiling 矩阵，Codex 只消费 manifest 做 faithful replay / self-config prediction / cross-config prediction；
  - 原 operation-level ordered transition oracle 主线顺延为 `主线三`。
- 清理可再生生成数据：
  - 清理前 `data` 约 `95G`；
  - 清理后只剩空目录，约 `4.0K`；
  - 当前长期证据以专项验证文档中的 run label、配置、指标和结论为准。
- 本轮提交前检查已通过：
  - `cmake --build build --target trace_graph -j 8`
  - `python3 tests/run_hicache_state_fixtures.py`
  - `python3 tests/run_modeling_smoke_fixtures.py`
  - `python3 -m py_compile scripts/internal/model_runner.py tests/run_modeling_smoke_fixtures.py tests/run_hicache_state_fixtures.py`
  - `find configs -name '*.json' -print0 | xargs -0 -n1 jq empty`
  - `git ls-files '*.c' '*.cc' '*.cpp' '*.h' '*.hpp' | xargs clang-format --dry-run --Werror`
  - `git diff --check`

## 2026-06-08 13:15:22 +0800

- 调整 HiCache profiling / validation 约束：
  - `--hicache-ratio` 不再要求固定为 `2.0`；
  - diagnostic / validation 实验中可以按目标调整，但必须大于 `1.0`；
  - 调整时必须在配置说明或 `HCSV-*` 验证摘要中写明原因；
  - 容量压力仍优先通过 workload 或显式 capacity 配置构造。
- 已同步更新 `docs/project_constraints.md`、`docs/profiling_development.md`、
  `docs/validation/hicache_state_validation.md` 和相关 HiCache/SGLang profiling config 的
  `hicache_ratio_note`。

## 2026-06-08 13:07:45 +0800

- 在 `docs/validation/hicache_state_validation.md` 中新增“下一步工作主线”：
  - 主线一是扩大 HiCache state model 的多配置、多输入验证矩阵；
  - 要求不同配置使用同一输入形态做 self-config prediction 和跨配置 prediction，先做 `C0 -> Cj`，再扩展组合配置；
  - 明确配置集合、输入集合、faithful replay / state prediction 流程、验收门槛和 `HCSV-*` 记录编号方式；
  - 主线二是引入更强 oracle 采集，推进 operation-level ordered transition oracle；
  - 明确 oracle transition 字段、probe-level / mutation-near / source-level guarded emitter 三阶段实现顺序，
    以及真正 `exact_match=true` 的验收门槛。

## 2026-06-08 12:55:21 +0800

- 继续丰富 `docs/validation/hicache_state_validation.md`，补回已清理验证记录中仍有价值但不应依赖
  `data/` 目录的关键信息：
  - 目标与验收边界、验证分层、trace 输入边界；
  - C++ HiCache state model 的输入、状态对象、状态转移和模块边界；
  - profiling 不变量 / oracle 字段边界和 state snapshot 采集契约；
  - 历史 cross-config 能力矩阵、event delta / timeline delta 口径、历史修正规则；
  - phased workload 语义、配置入口、faithful replay / state prediction 验证方法、fixture 覆盖和失败处理。
- 专项文档仍不恢复旧 run 路径：旧结论只保留为历史能力摘要，当前真实证据仍以新的
  `HCSV-*` 记录为准。

## 2026-06-08 12:44:15 +0800

- 重构 `docs/validation/hicache_state_validation.md`：
  - 删除旧版大量 `data/` run 路径索引和历史表格；
  - 改为“记录规则 / 当前结论 / 最新真实验证摘要 / 能力边界 / 复现入口 / 新记录模板”的结构；
  - 明确文档不依赖本地 `data/` 产物长期存在，真实 run 只保留临时 id 和抽取后的关键指标。
- 重新跑了一轮真实 SGLang HiCache state-only validation：
  - 临时 run id：`20260608_041916_profiling_hicache_state_validation`；
  - workload `65/65` 请求成功，`errors=0`；
  - profile quality `quality_ready=true`，`missing_cache_mechanisms=[]`；
  - state validation `final_state_match=true`，`missing_page_identity_events=0`；
  - event delta `match=true`、`mismatch_count=0`；
  - timeline coverage `match=true`、`model_extra_transition_count=0`，仅保留 oracle-only evicted transient 诊断。
- 当前文档不再把旧 write-back / capacity / prefetch / page64 / selective run 目录当作当前凭据；
  这些 cross-config 能力后续需要按新的 `HCSV-*` 记录规则成对重跑并内联摘要。
- 同步收紧 `docs/project_constraints.md`：
  - 专项验证文档以后记录稳定验证编号、内联摘要、复现入口和关键指标；
  - 新写验证文档不能要求读者打开某个 `data/` 目录才能理解结论；
  - 临时 run id 只用于说明实验批次，不作为长期证据载体。
- 说明：`docs/work_progress.md` 旧时间线里保留的 `data/` 路径只是历史流水记录，不是新的验证文档引用规范。

## 2026-06-08 12:04:52 +0800

- 完成项目结构整理和本地生成产物清理：
  - `docs/tmp/hicache_state_validation.md` 已迁移为可追踪专项验证文档
    `docs/validation/hicache_state_validation.md`，不再使用 `docs/tmp/`；
  - `docs/project_constraints.md` 已对齐为顶级四个主文档 + `docs/validation/` 专项验证文档的约束，
    并补充配置目录分组和 `data/` 产物保留规则；
  - `configs/experiments/` 已按 `hicache_state/`、`sglang/`、`smoke/` 分组；
  - `configs/modeling/` 已按 `hicache_state/`、`hicache/`、`smoke/` 分组；
  - 文档和 modeling config 内部引用已改为新的配置路径。
- 清理 `data/` 下未追踪的可再生 profiling/modeling 产物：
  - 清理前 `data/` 约 `302G`；
  - 清理后只剩空目录，约 `4.0K`；
  - `data/profile_runs/...` 相关结论保留在 `docs/validation/hicache_state_validation.md`，路径只作为历史 run 索引和复现入口名称。

## 2026-06-08 02:51:13 +0800

- 重新跑通 strict page64 prediction，并把输出体积压到可重验范围：
  - 新 strict 输出目录：`data/profile_runs/sglang/20260607_170641_profiling_hicache_state_validation/modeling/predict_page64_state_strict_lock_skip_v1`；
  - 因为 HiCache 默认不再写 transition digest，`model_summary.json` / `predicted_target_cache_state_trace.json`
    从此前约 `4.2G` / `2.1G` 缩到约 `77M`，可直接在一轮里完成重验；
  - 该 strict 结果 `validation_ready=true`、`final_state_match=true`；
  - timeline 也已经闭环到 `match=true`、`model_extra_transition_count=0`、`oracle_extra_transition_count=34`；
  - 剩余差异只在 `mark_evicted` / `clear_evicted` 的 oracle-only transient，上面已不再阻塞 timeline 通过。
- 这次闭环的关键修正：
  - `lock_ref_inc` / `lock_ref_dec` 在 page-size mismatch 下被视为 non-invariant observed facts；
  - C++ HiCache summary 默认不输出 transition digest，只在显式 `emit_state_digests=true` 时保留；
  - Python runner 透传了 `emit_state_digests`，并给默认轻量输出加了 fixture 断言。
- 本轮验证：
  - `cmake --build build --target trace_graph -j 8`
  - `python3 tests/run_hicache_state_fixtures.py`
  - `python3 tests/run_modeling_smoke_fixtures.py`
  - `python3 -m py_compile scripts/internal/model_runner.py tests/run_modeling_smoke_fixtures.py tests/run_hicache_state_fixtures.py`
  - `git ls-files '*.c' '*.cc' '*.cpp' '*.h' '*.hpp' | xargs clang-format --dry-run --Werror`
  - `git diff --check`

## 2026-06-08 02:42:37 +0800

- 处理 strict page64 prediction 中 lock/ref timeline 的 predicted extra：
  - 对照 SGLang `HiRadixCache.inc_lock_ref` / `dec_lock_ref` 实现确认，lock/ref 沿当前
    radix tree 父链更新；page size what-if 下 base run 的 lock/ref 次数和节点路径不是
    target timeline 的稳定不变量；
  - C++ HiCache state model 已把 `lock_ref_inc` / `lock_ref_dec` 纳入 page-size mismatch
    下的 non-invariant observed facts；当前 state prediction 入口不再允许消费这类目标行为答案；
  - 新增 fixture 验证 page-size mismatch 下带 `target_page_identity` 的 base lock/ref 会被跳过，
    不再生成 `mark_locked` / `clear_locked` transition；
  - 用现有 `predict_page64_state_strict_transfer_extend_v1` 的 predicted trace 做只读过滤模拟后，
    去掉 lock/ref transitions 时 timeline coverage 可变为 `match=true`、`exact_match=false`，
    `model_extra_transition_count=0`、`oracle_extra_transition_count=34`，只剩
    `mark_evicted` / `clear_evicted` 的 oracle-only transient。
- 当前未重新生成完整 strict validation：
  - 现有 strict 输出的 `model_summary.json` 和 `predicted_target_cache_state_trace.json` 各约 `2.1G`；
  - 本轮修正通过 C++ fixture、构建、格式检查和基于现有 trace 的只读过滤模拟验证；
  - 后续如果要把新字段和 lock/ref skip 反映到真实 `validation.json`，需要增加轻量 state trace /
    summary 输出，或接受一次较重的完整重跑。
- 本轮验证：
  - `cmake --build build --target trace_graph -j 8`
  - `python3 tests/run_hicache_state_fixtures.py`
  - `python3 tests/run_modeling_smoke_fixtures.py`
  - `python3 -m py_compile scripts/internal/model_runner.py tests/run_modeling_smoke_fixtures.py tests/run_hicache_state_fixtures.py`
  - `git ls-files '*.c' '*.cc' '*.cpp' '*.h' '*.hpp' | xargs clang-format --dry-run --Werror`
  - `git diff --check`

## 2026-06-08 02:36:25 +0800

- 继续诊断 strict page64 prediction 的 timeline delta：
  - final-state 仍保持已闭环，当前剩余问题是 timeline diagnostic，不影响 `validation_ready=true`；
  - 手工复算完整 mismatch 后确认，`timeline_delta_validation.match=false` 的硬失败主要来自
    lock/ref 的 predicted extra，而不是 evicted oracle-only transient；
  - 当前完整 mismatch breakdown：`mark_locked` / `clear_locked` 各有 `extra_in_predicted=352`、
    `missing_in_predicted=89`、`mismatch_rows=64`；`mark_evicted` / `clear_evicted` 各有
    `missing_in_predicted=17`、`extra_in_predicted=0`、`mismatch_rows=17`；
  - evicted 差异表现为 17 个 page 在真实 target timeline 中经历第二次 mark/clear transient，
    模型 final state 已对齐但没有生成这组中间震荡；
  - lock/ref 差异来自 page-size mismatch 下 base lock facts 与 target snapshot timeline 的
    transition granularity 不一致，后续需要对照 SGLang `inc_lock_ref` / `dec_lock_ref`
    沿父链更新语义继续判断是模型过扩展还是 timeline 诊断口径过严。
- 本轮代码改动：
  - `model_runner.py` 的 `event_delta_validation` 和 `timeline_delta_validation` 新增
    `mismatch_totals_by_kind`，按 transition kind 汇总 missing / extra / mismatch rows；
  - smoke fixture 增加该字段的空 mismatch 断言，避免后续 schema 回退。
- 本轮验证：
  - `python3 tests/run_modeling_smoke_fixtures.py`
  - `python3 -m py_compile scripts/internal/model_runner.py tests/run_modeling_smoke_fixtures.py`
  - `git diff --check`

## 2026-06-08 02:12:50 +0800

- strict page64 prediction 的 final-state 已闭环：
  - 新的 strict 输出目录：`data/profile_runs/sglang/20260607_170641_profiling_hicache_state_validation/modeling/predict_page64_state_strict_transfer_extend_v1`；
  - `validation_ready=true`、`final_state_match=true`；
  - final state counts 对齐：L1 `111/111`、L2 `250/250`、dirty `0/0`、backuped `250/250`、
    evicted `139/139`、locked `0/0`、prefetch planned `364/364`、ready `17/17`、
    suppressed `347/347`；
  - 这次修复把 page size mismatch 下完整 base transfer 的 target planned 尾页补成 ready，
    解决了此前 `4c64...` 被压到 suppressed 的最后一个差异。
- 仍然保留的诊断差异：
  - `timeline_delta_validation.match=false`，`model_extra_transition_count=704`、`oracle_extra_transition_count=212`；
  - 现阶段这部分更像 transition granularity / observation gap 的诊断，而不是 final-state 闭环障碍；
  - 后续如果继续收敛，应优先对照 prediction state trace 和真实 state trace 的 transition 顺序。
- 本轮验证：
  - `cmake --build build --target trace_graph -j 8`
  - `git ls-files '*.c' '*.cc' '*.cpp' '*.h' '*.hpp' | xargs clang-format --dry-run --Werror`
  - `git diff --check`
  - `python3 tests/run_hicache_state_fixtures.py`
  - `python3 tests/run_profiling_fixtures.py`
  - `python3 tests/run_modeling_smoke_fixtures.py`

## 2026-06-08 01:52:33 +0800

- 继续收敛 base -> page64 strict HiCache state prediction：
  - 新 base profiling run 已完成：`data/profile_runs/sglang/20260607_170641_profiling_hicache_state_validation`；
  - 新 base state validation 已通过，历史输出为 prefix-fix 版本，
    final state L1 `56/56`、L2 `121/121`、backuped `121/121`、evicted `65/65`、
    prefetch planned/ready/suppressed `166/166`、`8/8`、`158/158`；
  - probe 的 `page_hashes_after_prefix` 已改为按 target page size 的 page-aligned prefix
    计算 parent hash，避免把非页对齐 prefix 残余 token 拼入 suffix page hash；
  - C++ state model 在 page size mismatch 下不再把 base `prefetch_progress.operation_hash_pages`
    当作 target ready/L2 证据，只保留 terminal progress 语义；
  - C++ state model 在 page size mismatch 下优先从同 request 的 lookup target path 尾部推导
    `prefetch_schedule` 的 target suffix pages，解决 `prefix_keys` 为空时 no-parent hash 造成的
    planned 集合偏差。
- 当前 strict page64 重验输出：
  - `modeling/predict_page64_state_strict_lookup_suffix_v1` 已把 core final state 对齐：
    L1 `111/111`、L2 `250/250`、dirty `0/0`、backuped `250/250`、evicted `139/139`、
    locked `0/0`；
  - prefetch planned 已对齐为 `364/364`；
  - 仅剩 prefetch debug 集合单页差异：ready `16/17`、suppressed `348/347`，
    page `4c64cf964a9c8cb73db2e12c361f9d2ce4b43f107ae5dcbe52e947acb769bb94` 在模型中
    被 suppress、在 oracle 中 ready；
  - strict validation 仍未闭环，不能进入 state-to-DAG patch。
- 下一步：
  - 检查 page size mismatch 下 `l3_prefetch_enqueue` / `l3_to_l2_transfer` 的 target ready
    evidence 是否应从已记住的 lookup/schedule target path 和目标 page 数补齐尾页；
  - 为 1121 new-input tokens / 64 page size 产生 17 个 target suffix pages、但 base transfer
    只暴露 16 个 target pages 的场景补 fixture，再重跑 strict page64。

## 2026-06-07 23:41:53 +0800

- 对齐 HiCache state validation 文档与当前实际进展：
  - `docs/validation/hicache_state_validation.md` 明确区分已完成、未完成和下一步计划；
  - 当前 base state validation 已通过，历史输出为 prefix 版本；
  - 当前 page64 target state validation 已通过，历史输出为 pagehash-concat 版本；
  - strict base -> page64 prediction 仍未通过：`predict_page64_state_strict_after_prefix_v1` 中 L2 `239/250`、backuped `239/250`、evicted `120/139`、prefetch planned/ready/suppressed `366/364`、`24/17`、`350/347`，`timeline_delta_validation.match=false`；
  - 已记录一次候选 prefetch progress 修复尚未完成真实 strict 重验，临时输出 `/tmp/hicache_page64_strict_recheck` 没有 `validation.json`。
- 下一步不进入 state-to-DAG patch；先完成 strict page64 state prediction 重验和修复。

## 2026-06-07 21:00:32 +0800

- 继续收敛 page size what-if 下的 prefetch state prediction：
  - C++ HiCache state model 已把 page size mismatch 场景中带 `target_page_identity` 的
    `l3_to_l2_transfer` 作为 target prefetch ready evidence 消费；
  - 新增 fixture 覆盖 page size mismatch 下 L3->L2 transfer target identity 能更新 target
    L2 / ready state；
  - 旧 page64 run 的 core state prediction 已通过 v5：L1 `111/111`、L2 `250/250`、
    dirty `0/0`、backuped `250/250`、evicted `139/139`；
  - strict prefetch diagnostic 从 ready `0/17` 推进到 `16/17`，但 planned `366/364`
    和 ready `16/17` 仍未完全对齐，因此官方 page64 配置继续把 prefetch debug 集合作为
    ignored diagnostic。
- Python HiCache probe 新增 `page_hashes_after_prefix:<prefix_tokens>,<tokens>,<page_size>[,<prior_hash>]`：
  - 用于从 `prefix_keys` 按 target page size 重算 prefetch parent hash，并只输出
    `new_input_tokens` 对应的 suffix pages；
  - 避免继续使用 base page size 下的 `last_hash` 链作为 target page size prefetch 的 parent hash，
    也避免把 full path prefix pages 混入 prefetch planned set；
  - 已更新 HiCache state validation 实验配置中的 `prefetch_from_storage.target_page_identity`，
    后续需要用新的真实 profiling run 重新验证 page64 strict prefetch。

## 2026-06-07 20:33:57 +0800

- 用当前严格 final-state diff 复跑主要 HiCache state prediction，并修复暴露的问题：
  - `write_back` target 不能只从 target experiment 启动参数派生 page/policy；还需要显式
    effective capacity。`profiling_hicache_state_write_back_validation.json` 已补
    `modeling.hicache.l1_capacity_pages=56`、`l2_capacity_pages=129`；
  - `wait_complete` 下 planned 但缺少 ready evidence 的 page 需要进入
    `prefetch_suppressed_pages`；
  - `timeout` 下 `l3_to_l2_transfer_end` 是完成事实，不能被 aggressive timeout window
    否掉；terminal empty progress 会 suppress pending pages，schedule-only 则不会尾部强行
    suppress。
- 新增/更新 C++ HiCache fixtures：
  - wait_complete planned-only suppressed；
  - timeout terminal empty progress suppressed；
  - timeout completed transfer credit 即使 timeout window 为 0 也算 ready。
- 已完成严格 prediction 回归：
  - write_back v3：L1 `56/56`、L2 `118/118`、dirty `48/48`、backuped `118/118`、
    evicted `110/110`，legacy `locked_pages=0/8` 显式 ignore；
  - wait_complete v2：L1 `56/56`、L2 `121/121`、prefetch planned `166/166`、
    ready `8/8`、suppressed `158/158`，legacy `locked_pages=0/8` 显式 ignore；
  - aggressive timeout v2：prefetch planned `166/166`、ready `8/8`、suppressed
    `158/158`，legacy `locked_pages=0/8` 显式 ignore；
  - write_back + low capacity v2：L1 `46/46`、L2 `88/88`、dirty `38/38`、
    backuped `88/88`、evicted `80/80`、locked `0/0`、prefetch planned `326/326`、
    ready `8/8`、suppressed `318/318`；
  - write_through_selective v1：L1 `56/56`、L2 `121/121`、backuped `121/121`、
    evicted `65/65`、locked `0/0`、prefetch planned `166/166`、ready `8/8`、
    suppressed `158/158`，timeline coverage `match=true`。

## 2026-06-07 20:06:14 +0800

- 收紧 HiCache final-state validation：
  - oracle final state 中出现的集合字段默认必须参与比较，模型缺字段会按空集合处理；
  - 新增 `validation.hicache_state.ignore_state_keys`，只影响 `final_state_match` 硬门槛；
  - 被忽略字段仍保留在 `ignored_sets_diff_by_tier`、`model_final_state_counts` 和
    `oracle_final_state_counts`，避免静默隐藏问题；
  - 新增 smoke fixture 覆盖 ignored key 与 oracle-only 非 ignored key 两种情况。
- 修正 prefetch finalization 的当前规则：
  - `best_effort` 在 run end 把 planned 但未 ready 的 page 归为 suppressed；
  - `timeout` 只在 request 已有 timeout/terminated 证据时 suppressed，普通 schedule-only page
    不会被 finalization 强行 suppressed；
  - `l3_to_l2_transfer_end` 会作为 best_effort completion credit，timeout 则必须落在目标
    timeout 窗口内才计入 ready。
- 更新旧 prediction 配置的 validation-only 口径：
  - 旧 page64 / capacity / prefetch policy runs 缺少 lock/ref model input，因此在对应
    modeling config 中显式忽略 `locked_pages`；
  - page64 还显式忽略 prefetch debug 集合，因为当前目标是 page-size/radix/capacity state，
    而不是 page-size 与 prefetch completion identity 的组合验证；
  - `write_through_selective` 不忽略 lock，继续作为 lock/ref state 的严格验证路径。
- 已完成真实回归：
  - page64 derived target experiment v4：`validation_ready=true`，L1 `111/111`、L2
    `250/250`、dirty `0/0`、backuped `250/250`、evicted `139/139`；
  - capacity derived target experiment v4：`validation_ready=true`，L1 `46/46`、L2
    `96/96`、dirty `0/0`、backuped `96/96`、evicted `50/50`、prefetch planned
    `326/326`、ready `8/8`；
  - best_effort derived target experiment v3：`validation_ready=true`，L1 `56/56`、
    L2 `121/121`、backuped `121/121`、evicted `65/65`、prefetch planned `166/166`、
    ready `8/8`、suppressed `158/158`。
- 剩余风险：
  - 当前真实 run 的 `model_summary.json` / `predicted_target_cache_state_trace.json`
    可达到 GB 级，state validation 运行慢；后续需要增加 summary-only 或压缩调试输出。

## 2026-06-07 19:52:03 +0800

- 增强 HiCache state prediction 的 target config 来源：
  - `model_runner.py` 支持 `input.target_experiment_config`；
  - runner 会从目标实验配置 `server.command` 抽取 `--page-size`、`--hicache-write-policy`、`--hicache-storage-prefetch-policy` 和 storage prefetch timeout 参数；
  - 目标实验配置可在 `modeling.hicache` 显式补充有效 `l1_capacity_pages` / `l2_capacity_pages`；
  - runner 不根据 `--max-total-tokens` 或 `--hicache-ratio` 粗算 capacity，避免把资源池配置误当有效 page budget。
- 修复 explicit timeout / best_effort prefetch final-state 语义：
  - trace 结束时，planned 但未 ready 的 prefetch pages 会进入 `prefetch_suppressed_pages`；
  - 解决了从 target experiment 派生真实 `timeout` policy 后，suppressed pages 漏建模的问题。
- 固化 `write_back + low capacity` prediction 配置：
  - `configs/experiments/hicache_state/profiling_hicache_state_write_back_capacity_validation.json` 新增 `modeling.hicache` 有效 capacity；
  - `configs/modeling/hicache_state/modeling_hicache_state_prediction_write_back_capacity.json` 改为引用 target experiment config，不再重复手写 page/policy/capacity。
- 已完成真实 cross-config 验证：
  - base run：`20260607_083213_profiling_hicache_state_capacity_base_validation`；
  - target oracle：`20260607_073450_profiling_hicache_state_write_back_capacity_validation`；
  - prediction：`predict_write_back_capacity_state_derived_target_experiment_v2`；
  - `validation_ready=true`、`final_state_match=true`；
  - L1 `46/46`、L2 `88/88`、dirty `38/38`、backuped `88/88`、evicted `80/80`、prefetch planned `326/326`、ready `8/8`、suppressed `318/318`。

## 2026-06-07 19:05:14 +0800

- 修正 HiCache 推荐 C++ config 的 capacity 语义：
  - 真实验证发现旧规则把 `oracle_observed_max_state_counts` 当作 capacity 写入推荐配置后，会让 state prediction 误进入 capacity what-if 分支；
  - 失败表现为模型保留 L1 `62` / L2 `128` 页，而真实 final state 是 L1 `56` / L2 `121` 页；
  - 新规则只自动推荐稳定的 `page_size`、`write_policy`、`prefetch_policy`；
  - `l1_capacity_pages` / `l2_capacity_pages` 只有在 target C++ config 已显式设置时才复制到推荐配置；
  - 未显式设置 capacity 时，推荐结果只在 `evidence` 中记录 raw pool、observed max、final count，并标记为 `not_auto_recommended`。
- 已完成真实 run 回归：
  - 重新生成 capacity-snapshot 版本的 `recommended_hicache_cpp_model_config.json`；
  - 推荐配置内容为 `page_size=128`、`write_policy=write_through`、`prefetch_policy=timeout`，不包含 L1/L2 capacity；
  - 使用该推荐配置重跑 state validation，`validation_ready=true`、`final_state_match=true`；
  - final L1 `56/56`、L2 `121/121`、backuped `121/121`、evicted `65/65`、prefetch planned `166/166`、ready `8/8`、suppressed `158/158`。

## 2026-06-07 18:26:48 +0800

- 将 HiCache 推荐 target config 落盘为可复用 C++ model config：
  - 当 `capacity_config_audit.recommended_target_config.ready=true` 时，`model_runner.py` 会写出 `recommended_hicache_cpp_model_config.json`；
  - 文件格式为 `{"modules":["hicache"],"hicache":{...}}`，可直接传给 C++ TraceGraph `--model-config`；
  - validation 会回写 `hicache_state.recommended_hicache_cpp_model_config_path`。
- 已刷新真实 run `20260607_095024_profiling_hicache_state_validation` 的 state validation：
  - 推荐配置文件位于历史 state-validation 输出目录；
  - 旧版内容曾包含 `l1_capacity_pages=62`、`l2_capacity_pages=128`，该规则已在 19:05 修正为不自动推荐 capacity。

## 2026-06-07 18:21:58 +0800

- 增加基于 target oracle 的 HiCache target config 推荐规则：
  - `capacity_config_audit.recommended_target_config` 会输出建议的 C++ HiCache 配置；
  - page size、write policy、prefetch policy 使用 oracle capacity summary 中唯一观测值；
  - 旧版曾把 L1/L2 `oracle_observed_max_state_counts` 作为推荐 capacity；该规则已在 19:05 证伪并修正；
  - evidence 会记录每个字段来源，capacity 未显式配置时只保留诊断证据。
- 刷新真实 run `20260607_095024_profiling_hicache_state_validation` 的 state validation：
  - 旧版推荐 config 为 `page_size=128`、`write_policy=write_through`、`prefetch_policy=timeout`、`l1_capacity_pages=62`、`l2_capacity_pages=128`，现已改成不自动带 capacity；
  - `validation_ready=true`、`final_state_match=true` 保持不变。
- 当前边界：
  - 推荐规则依赖 target oracle，适合真实 target self-config / cross-config prediction 验证后修复配置；
  - 没有 target oracle 的纯 what-if 仍需要显式给出目标 page size、policy 和 capacity。

## 2026-06-07 18:17:12 +0800

- 完成新 probe capacity 字段的真实 SGLang HiCache run 验证：
  - run：`data/profile_runs/sglang/20260607_095024_profiling_hicache_state_validation`；
  - `profile_quality.quality_ready=true`，`hicache_capacity_observed=true`，capacity snapshot `4824` 个；
  - 真实采到 `write_policy=write_through`、`prefetch_policy=timeout`、L1 raw pool `64` pages、L2 raw pool `129` pages、prefetch capacity limit `52` pages；
  - `missing_cache_mechanisms=[]`，必须有 page identity 的 stateful events 全部覆盖。
- 完成该 run 的同配置 state validation：
  - 输出为历史 capacity-snapshot state-validation 目录；
  - `validation_ready=true`、`final_state_match=true`；
  - final L1 `56/56`、L2 `121/121`、backuped `121/121`、evicted `65/65`、locked `0/0`、prefetch planned `166/166`、ready `8/8`、suppressed `158/158`；
  - event delta match，shared event key `222`；
  - timeline coverage match，`model_extra_transition_count=0`。
- 增加 `oracle_observed_max_state_counts`：
  - 用 state snapshot 时间线统计运行中达到过的 resident/metadata 峰值；
  - 本次真实 run 显示 L1 raw pool `64`、observed max `62`、final `56`，L2 raw pool `129`、observed max `128`、final `121`；
  - capacity audit 现在会同时参考 raw pool、final count 和 observed max count，避免只看 final state 低估容量压力。

## 2026-06-07 17:46:01 +0800

- 补充 HiCache target config 与 oracle capacity / policy 事实的一致性审计：
  - `validation.json.hicache_state.capacity_config_audit` 会输出 C++ target config、oracle observed values 和逐字段比较结果；
  - page size、write policy、prefetch policy 做精确匹配；
  - L1/L2 capacity 同时比较 raw pool capacity 与 oracle final resident count；
  - `target_below_observed_pool` 作为 warning，用于表达有效 budget 可能小于 raw pool；
  - `target_exceeds_observed_pool` / `target_below_oracle_final_count` 作为 likely error，用于指导修正 prediction config。
- 收紧 profiling quality：
  - `profiling.python_probe.state_trace.enabled=true` 时，缺少 `state_snapshot.capacity` 会输出 `hicache_capacity_snapshot_missing`，并使 `quality_ready=false`；
  - 这样新一轮 state validation run 能在 profiling 阶段发现 capacity prediction 缺关键事实。
- 已补 fixture：
  - modeling smoke 中验证 target config 和 capacity oracle 完全匹配；
  - 直接单元 fixture 验证低于 raw pool 与超过 raw pool 的分类语义。
  - profiling fixture 验证 state trace 缺少 capacity 时 quality 失败。

## 2026-06-07 17:40:46 +0800

- 补充 HiCache validation-only capacity / policy 事实采集：
  - `sglang.hicache` state snapshot 新增 `capacity` 字段，采集 page size、write policy、prefetch policy、L1/L2 pool capacity、available size、prefetch threshold 和 prefetch capacity limit；
  - `profile_quality.py` 新增 `hicache_capacity_observed` 和 `hicache_capacity` 摘要，用于审计真实 profiling 是否暴露有效 budget 证据；
  - `model_runner.py` 新增 `validation.json.hicache_state.oracle_capacity_summary`，从 oracle state snapshot 汇总 capacity / policy 事实；
  - 新增 profiling 和 modeling fixture，验证 capacity 事实不进入模型输入，但能被 quality / validation 输出读取。
- 当前限制：
  - 新增 capacity 事实尚未自动覆盖 C++ target config；
  - 下一次真实 profiling run 后，应检查 `profile_quality.hicache_capacity` 和 `oracle_capacity_summary` 是否足以解释 L1/L2 有效 page budget。

## 2026-06-07 17:24:31 +0800

- 完成 `write_back + low capacity` 组合 state 验证闭环：
  - target run：`data/profile_runs/sglang/20260607_073450_profiling_hicache_state_write_back_capacity_validation`；
  - target state validation：历史 v4 输出；
  - fresh base run：`data/profile_runs/sglang/20260607_083213_profiling_hicache_state_capacity_base_validation`；
  - base state validation：历史 v4 输出；
  - cross prediction：`modeling/predict_write_back_capacity_state_20260607_073450_l2cap88`。
- 本轮真实 workload 验证结果：
  - target 和 fresh base 的 `profile_quality.quality_ready=true`，`missing_cache_mechanisms=[]`；
  - target state validation `validation_ready=true`、`final_state_match=true`，L1 `46/46`、L2 `88/88`、dirty `38/38`、backuped `88/88`、evicted `80/80`、locked `0/0`；
  - base state validation `validation_ready=true`、`final_state_match=true`，L1 `56/56`、L2 `126/126`、dirty `0/0`、backuped `126/126`、evicted `70/70`、prefetch ready `8/8`；
  - base -> target prediction `validation_ready=true`、`final_state_match=true`，L1 `46/46`、L2 `88/88`、dirty `38/38`、backuped `88/88`、evicted `80/80`、prefetch ready `8/8`。
- 修正 validation oracle：
  - final `locked_pages` 在有 lock facts 时按 `lock_ref_inc` / `lock_ref_dec` refcount 推导，避免尾部缺失 end snapshot 误判；
  - timeline oracle 增加 `final_lock_timeline_correction`，本轮 target state validation 补齐 8 个 `clear_locked`；
  - prefetch ready oracle 增加 `l3_to_l2_transfer_end` 完成证据，避免把已 transfer 的 8 页误判为 late/suppressed；
  - timeline coverage 只比较 completed snapshot 可见字段，write-through 下不可见 dirty transient 进入 `ignored_unobservable_state_keys`。
- 更新 `configs/modeling/hicache_state/modeling_hicache_state_prediction_write_back_capacity.json`：
  - `l2_capacity_pages` 从 `96` 调整为 `88`；
  - 本轮结果表明 `write_back + low capacity` 的有效 L2 budget 需要从真实配置/运行状态中采集，不能简单复用普通 low-capacity target 的 `96`。

## 2026-06-07 15:27:37 +0800

- 收敛 HiCache state timeline validation 口径：
  - `timeline_delta_validation.match=true` 现在表示模型输出的 transition 全部被 raw snapshot timeline 覆盖；
  - 新增 `exact_match`、`model_transition_covered`、`model_extra_transition_count`、`oracle_extra_transition_count`；
  - 多进程稀疏 state snapshot 造成的 oracle-only transient oscillation 保留为诊断，不再误判为模型凭空缺 transition。
- 完成带 `object_id` 的真实 `write_through_selective` target state validation：
  - target run：`data/profile_runs/sglang/20260607_063721_profiling_hicache_state_write_through_selective_validation`；
  - 输出为历史 timeline-v4 state-validation 目录；
  - `final_state_match=true`，`missing_page_identity_events=0`；
  - `event_delta_validation.match=true`，shared exclusive event key 为 `242`；
  - `timeline_delta_validation.match=true`，`exact_match=false`，`model_extra_transition_count=0`，`oracle_extra_transition_count=348`。
- 完成 base `20260607_053949` -> target `20260607_063721` 的跨配置 state prediction：
  - prediction：`modeling/predict_write_through_selective_state_20260607_063721_timeline_v4`；
  - final L1/L2/dirty/backuped/evicted/locked/prefetch 集合全部对齐；
  - `timeline_delta_validation.match=true`，`model_extra_transition_count=0`；
  - exact event delta 仍因 base/target run 时间戳不同不可比较。

## 2026-06-07 14:30:56 +0800

- 完成 lock-enabled `write_through_selective` target profiling / state validation / prediction 闭环：
  - target run：`data/profile_runs/sglang/20260607_060803_profiling_hicache_state_write_through_selective_validation`；
  - target workload `65/65` 请求成功；
  - target `profile_quality.quality_ready=true`，`observed_cache_mechanisms.lock_ref=872`，`stateful_required_events_missing_page_identity=0`。
- 完成 target 同配置 state validation：
  - 输出为历史 lock state-validation 目录；
  - `validation_ready=true`、`final_state_match=true`；
  - L1 `56/56`、L2 `121/121`、dirty `0/0`、backuped `121/121`、evicted `65/65`、locked `0/0`；
  - `lock_state_events=708`，`mark_locked=918`、`clear_locked=918`；
  - `event_delta_validation.match=true`，shared exclusive event key 为 `242`。
- 完成 lock-enabled base -> `write_through_selective` 跨配置 prediction：
  - base run：`data/profile_runs/sglang/20260607_053949_profiling_hicache_state_validation`；
  - prediction：`modeling/predict_write_through_selective_state_20260607_060803_lock`；
  - `validation_ready=true`、`final_state_match=true`；
  - `locked_pages=0/0`，`mark_locked=918`、`clear_locked=918`，`missing_page_identity_events=0`；
  - 跨配置 exact event delta 仍不可比较，原因是 base/target run 时间戳不同；final-state 和 transition coverage 已覆盖 lock state。

## 2026-06-07 14:04:55 +0800

- 跑完新一轮带 lock probe 的真实 HiCache state profiling：
  - run：`data/profile_runs/sglang/20260607_053949_profiling_hicache_state_validation`；
  - workload `65/65` 请求成功；
  - 手动生成 `profile_quality.json`，`quality_ready=true`；
  - `observed_cache_mechanisms.lock_ref=872`；
  - `stateful_required_events_missing_page_identity=0`。
- 完成该 run 的同配置 state validation：
  - 输出为历史 lock-v2 state-validation 目录；
  - `validation_ready=true`、`final_state_match=true`；
  - L1 `56/56`、L2 `121/121`、dirty `0/0`、backuped `121/121`、evicted `65/65`、locked `0/0`；
  - `lock_state_events=708`，`mark_locked=918`、`clear_locked=918`；
  - `event_delta_validation.match=true`，shared exclusive event key 为 `224`，不再有 ignored lock state key。
- 修复 C++ HiCache parser 和 quality audit 的 no-op lock 语义：
  - `HiRadixCache.inc_lock_ref` / `dec_lock_ref` 沿父链到 root，root 没有 page identity；
  - 当 `lock_delta=0` 且 page 为空时视为 no-op，不计入 `missing_page_identity_events`；
  - 非 root lock events 仍必须携带 page identity。

## 2026-06-07 13:34:04 +0800

- 补强 HiCache state transition 验证输出：
  - `validation.json.hicache_state.event_delta_validation` 现在区分 inclusive 包围差分和 exclusive 可比较差分；
  - 嵌套 state snapshot 使用 snapshot `dur` 和事件顺序识别，避免把内层 HiCache 调用造成的状态变化误归因给外层函数；
  - mismatch 只在 predicted 和 oracle 共享的 exclusive event key 上比较 page delta，跨配置 prediction 仍以 final-state / policy oracle / coverage 为主。
- 真实 selective 同配置 state validation 已用新口径重跑：
  - 输出为历史 event-delta-v4 state-validation 目录；
  - `validation_ready=true`、`final_state_match=true`；
  - `event_delta_validation.match=true`，shared exclusive event key 为 `18`；
  - 识别出 `84` 个嵌套事件和 `248` 条嵌套包围 transition，旧真实 run 中 `locked_pages` 因未采 lock facts 暂列为未比较字段。
- 补齐 lock state 骨架和采集契约：
  - C++ HiCache state 已维护 `locked_pages` 和按 `cache_scope + page` 的 lock ref count；
  - HiCache state fixture 已覆盖 inc/dec lock ref；
  - state validation profiling 配置加入 `HiRadixCache.inc_lock_ref` / `dec_lock_ref` target；
  - 更新 `docs/validation/hicache_state_validation.md`、`docs/profiling_development.md` 和 `docs/modeling_development.md`。

## 2026-06-07 13:15:00 +0800

- 完成 HiCache `write_through_selective` state 建模和真实跨配置验证：
  - C++ `HiCacheStateModel` 增加按 `cache_scope(pid) + page` 维护的 hit_count，避免多 TP 上同一 page 的一次命中被全局相加后误判达到 selective threshold；
  - insert 缺少 request_id 时，改用同一 cache scope 下最近一次 lookup path 配对，避免 full-prefix insert 被 `prefix_len` 剪空后丢失 hit_count；
  - 显式 write policy target 仍消费 CPU_PINNED `remove_page`，把 host/L2 resident 清理视为 state transition，而不是 write movement。
- 新增配置：
  - `configs/experiments/hicache_state/profiling_hicache_state_write_through_selective_validation.json`；
  - `configs/modeling/hicache_state/modeling_hicache_state_prediction_write_through_selective.json`。
- 真实 selective target run 已完成：
  - target profiling run：`data/profile_runs/sglang/20260607_043930_profiling_hicache_state_write_through_selective_validation`；
  - target 同配置 state validation：`validation_ready=true`、`final_state_match=true`；
  - base `20260607_031354` -> selective target prediction：`predict_write_through_selective_state_20260607_043930_final`，`validation_ready=true`、`final_state_match=true`；
  - 对齐计数：L1 `56/56`、L2 `121/121`、dirty `0/0`、backuped `121/121`、evicted `65/65`、prefetch planned `166/166`、ready `8/8`、suppressed `158/158`。
- 更新 `docs/validation/hicache_state_validation.md`，将 write policy final-set model 标记为已完成；剩余重点是逐 transition oracle、evictable lock / 完整 radix node split，以及后续 DAG patch。

## 2026-06-07 12:25:00 +0800

- 完成 HiCache prefetch policy state 建模补强：
  - C++ `HiCacheStateModel` 在同 timestamp 内按 `start -> end` 的逻辑顺序消费 HiCache facts，避免 Python probe start/end 同时刻反序导致 `prefetch_progress_end` 先被误判为 suppressed；
  - C++ model config 增加 `prefetch_timeout_base_sec`、`prefetch_timeout_per_ki_token_sec`、`prefetch_timeout_max_sec`，timeout prediction 可按目标 timeout 判断；
  - `best_effort` / aggressive `timeout` 不再把缺少 `operation_hash_pages` 的 ongoing progress 当成 late page 证据；late 必须来自 operation progress，terminal empty progress 只产生 suppressed；
  - validation oracle 支持从 target progress evidence 派生 `prefetch_late_pages` 和 `prefetch_suppressed_pages`。
- 新增并运行真实 prefetch target：
  - `profiling_hicache_state_prefetch_best_effort_validation`，run `20260607_035602`；
  - `profiling_hicache_state_prefetch_timeout_aggressive_validation`，run `20260607_041431`；
  - 两个 target 的 `profile_quality.quality_ready=true`，同配置 state validation 均 `final_state_match=true`。
- 完成 base `20260607_031354` 到 prefetch target 的真实跨配置 prediction：
  - best_effort prediction `predict_prefetch_best_effort_state_20260607_035602_rerun3` 通过，planned `166/166`、ready `0/0`、late `8/8`、suppressed `166/166`；
  - aggressive timeout prediction `predict_prefetch_timeout_aggressive_state_20260607_041431` 通过，planned `166/166`、ready `0/0`、late `8/8`、suppressed `166/166`。
- 更新 `docs/validation/hicache_state_validation.md`，将 prefetch wait/best_effort/timeout 的 self-config 和跨配置 prediction 结果写入短期计划；下一项转向 `write_through_selective` 的 hit_count threshold 和更严格逐 transition oracle。

## 2026-06-07 11:40:00 +0800

- 完成 HiCache prefetch state 建模收尾：
  - C++ `HiCacheState` 增加 `prefetch_late_pages`、`prefetch_suppressed_pages`，并从
    `prefetch_progress_state` / `prefetch_done` 解析 operation progress evidence；
  - 修正 wait_complete 语义：`prefetch_schedule` 只表示 planned，ready 必须由
    `check_prefetch_progress` 的 operation pages 和 completed tokens 驱动；
  - validation oracle 增加 `prefetch_late_pages`、`prefetch_suppressed_pages`，且只在真实
    progress evidence 暴露 operation pages 时验证 ready/late/suppressed，避免用 schedule
    伪造完整 oracle。
- 跑完新一轮真实 HiCache state 实验：
  - base run：`data/profile_runs/sglang/20260607_031354_profiling_hicache_state_validation`；
  - wait target run：`data/profile_runs/sglang/20260607_032622_profiling_hicache_state_prefetch_wait_validation`；
  - base state validation 和 wait target state validation 均 `validation_ready=true`、`final_state_match=true`；
  - base -> wait_complete prediction 通过，`prefetch_planned_pages=166/166`、
    `prefetch_ready_pages=8/8`，仅 `l3_resident_pages` 因缺 full-set oracle 保留为 unchecked。
- 更新 `docs/validation/hicache_state_validation.md`：
  - 明确当前完整机制 workload 已覆盖 lookup/insert/load_back/evict/prefetch/write；
  - 把 prefetch wait 从“部分完成”更新为已完成；
  - 下一阶段重点收敛到 best_effort / timeout 的 late/suppressed 跨配置 state prediction。

## 2026-06-07 06:15:43 +0800

- 推进 HiCache state 跨配置验证：
  - 完成 capacity target 真实 profiling、同配置 state validation 和 base -> target state prediction；
  - 修复 C++ HiCache state model 中显式 `write_through` target 没有补 L3 readable set 的问题；
  - 修复同 page size capacity 下错误按 leaf group 驱逐、以及 insert pre-allocation 按全量 page 数过驱逐的问题；
  - 确认本次低容量 target 的有效 L1 page budget 是 `46`，并更新 `modeling_hicache_state_prediction_capacity.json`。
- 完成 prefetch wait target 真实 profiling 和 state validation：
  - resident/dirty/backuped/evicted final set 与 oracle 对齐；
  - 当前 workload 下 timeout 与 wait_complete 的 resident 结果相同，不能严格验证 ready/late/suppressed；
  - `model_runner.py` 增加 final state 计数和 `unchecked_model_state_keys`，显式暴露 `prefetch_ready_pages` 这类缺 oracle 字段的集合。
  - validation 已能从 oracle trace 的 prefetch schedule/progress evidence 派生 `prefetch_planned_pages` 和 `prefetch_ready_pages`；旧 run 中 planned 已对齐，ready 需要下一轮带新字段的真实 run。
  - `sglang.hicache` probe 增加 `prefetch_progress:` source，state profiling 配置会采集 per-request prefetch progress evidence，供下一轮 ready/late/suppressed 验证使用。
- 更新 `docs/validation/hicache_state_validation.md`：
  - 区分已完成、部分完成和未完成项；
  - 记录 capacity / prefetch wait 真实 run 结果；
  - 把下一阶段重点收敛到 prefetch policy oracle 字段和 workload 区分度。

## 2026-06-06 18:47:31 +0800

- 按 `docs/validation/hicache_state_validation.md` 完成 HiCache state validation 收尾实现：
  - 安装并验证 `clang-format`，仓库根目录 `.clang-format` 改为当前工具链可识别的格式；
  - `sglang.hicache` Python probe 增加显式 `hicache_state:self` snapshot source，默认关闭，只有 `profiling.python_probe.state_trace.enabled=true` 时由 runner 开启；
  - validation-only `state_snapshot` 事件标记 `model_input=false`，C++ Chrome reader 不把它们放进性能 DAG；
  - C++ `HiCacheModule` 重构为 `HiCacheFactParser`、`HiCacheState`、`HiCacheStateModel` 等面向对象骨架，当前只维护 state，保持 `dag_mutations=0`；
  - `model_runner.py` 增加 `predicted_target_cache_state_trace.json` 输出和 `validation.json.hicache_state` diff；
  - 新增 HiCache state validation 的 profiling / modeling 历史入口；当前 modeling 入口已收敛为显式 target config 的 prediction config。
- 真实 state validation 验证通过：
  - profiling run：`data/profile_runs/sglang/20260606_103159_profiling_hicache_state_validation`，manifest `status=completed`；
  - modeling output：历史 final2 state-validation 输出；
  - Python probe 事件 5536 条，其中 `state_snapshot` 2628 条；
  - `validation_ready=true`，`final_state_match=true`，`invariant_coverage_ready=true`，`missing_page_identity_events=0`；
  - 对齐集合：`l1_resident_pages=24/24`、`l2_resident_pages=24/24`、`dirty_pages=0/0`、`backuped_pages=24/24`、`evicted_pages=0/0`。
- 已通过验证：
  - `cmake --build build --target trace_graph -j 8`；
  - `clang-format --dry-run --Werror`；
  - `python3 -m py_compile` 覆盖 runner、trace merger、profiling config/probe 和相关 tests；
  - `tests/run_tracegraph_fixtures.py`、`tests/run_modeling_smoke_fixtures.py`、`tests/run_hicache_state_fixtures.py`、`tests/run_profiling_fixtures.py`、`tests/run_native_hook_fixtures.py`；
  - `scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_validation.json --dry-run`；
  - `scripts/profile.sh configs/experiments/sglang/profiling_minimal_sglang_hicache.json --dry-run`；
  - `python3 scripts/internal/model_runner.py --config configs/modeling/smoke/modeling_smoke_hicache.json`。

## 2026-06-06 17:58:41 +0800

- 固化 faithful replay 与事件消费边界：
  - `faithful_replay` 关闭的是子模块加载和 DAG patch，不是关闭事件消费；
  - base DAG 必须消费完整真实 merged trace，HiCache、CPUInfer、Python probe 等真实执行事件也应参与重放；
  - 需要隔离的是 state snapshot、oracle state、probe debug 和质量审计这类非执行事件，它们不能作为默认性能 DAG 节点；
  - 子模块后续在完整 base DAG 上做 what-if 修改，不能通过过滤真实事件来构造“干净”重放。

## 2026-06-06 16:02:14 +0800

- 已完成一次轻量基线整理提交并推送到远程：
  - 提交 `a11eb1f chore: consolidate profiling and trace graph baseline`；
  - 外部老版 `TraceGraph/` 仓库只作为本地参考，不进入 active 仓库提交。
- 修复 `src/modeling/trace_graph` 中上一轮 `// !` 标出的确定问题：
  - 缺 `Event Id` 的 record/wait 不再用默认 id 匹配，避免生成错误 sync 边；
  - stream sync 支持 `streamId`、`stream id`、`Physic Stream Id`、`Raw Stream` 到真实 DAG lane 的 alias 映射；
  - 补齐 `aclrtSynchronizeStreamWithTimeout`、`aclrtSynchronizeEvent`、`aclrtSynchronizeDevice` 等 sync wrapper；
  - Chrome trace reader 能完整跳过 args 中的对象/数组，避免嵌套字段打乱后续字段扫描；
  - 拓扑仿真按前驱节点的 `cpuinterval` 计入 CPU gap。
- `HiCacheModule` 从 skeleton 推进到 state-only：
  - 消费 HiCache profiling facts，维护 `L1/L2/L3 resident`、`dirty`、`backuped`、`evicted`、`prefetch planned/ready`；
  - 输出 `final_state`、`transitions_by_kind`、缺失 page identity 和 dirty eviction 计数；
  - 仍保持 `dag_mutations=0`，暂不做 DAG patch。
- 已新增 fixture 覆盖：
  - 缺 event id 不错误建 sync；
  - stream alias sync、WithTimeout、event sync、device sync；
  - nested args 不破坏 streamId 解析；
  - CPU gap 计入；
  - HiCache insert/load/write/prefetch/evict 的状态转移。

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

- 清空 C++ HiCache 旧 movement/latency 建模逻辑：
  - `HiCacheModule` 保留为 skeleton，只统计输入 HiCache 事件数量；
  - skeleton 不修改 DAG node duration、metadata 或 edge；
  - active HiCache summary 删除旧 movement/page/latency 指标。
- 新增 base DAG profiling 验证入口：
  - 新配置 `configs/experiments/sglang/profiling_sglang_bench_serving_base_dag.json` 使用 SGLang `bench_serving` random dataset；
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
  - `--hicache-ratio` 当前约束为可按实验目标调整，但必须大于 `1.0`；容量压力优先由 workload
    或显式 capacity 配置构造。
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
- 清理 `configs/experiments/sglang/profiling_minimal_sglang_hicache.json`：
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
  - 新增 `configs/modeling/hicache/modeling_hicache_from_manifest.json`，用于消费真实 profiling run。

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
- 新增 `configs/experiments/sglang/profiling_minimal_sglang_hicache.json`：
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
