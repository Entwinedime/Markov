"""Post-hoc model search comparisons and figures; never used to choose real search points."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any

import matplotlib

matplotlib.use("Agg")
from matplotlib import pyplot as plt
import numpy as np

from markov_internal.applications.search.algorithms import candidate_indices
from markov_internal.applications.search.request import SearchRequest
from markov_internal.applications.search.results import prediction_rows
from markov_internal.applications.accuracy.http_metrics import configuration_selection
from markov_internal.common.io import load_json, write_json
from markov_internal.common.paths import require_repo_path
from markov_internal.modeling.source_dag.profiles import cpu_service_metadata


WRITE_LABELS = {"write_through": "直写", "write_through_selective": "选择性直写", "write_back": "写回"}
WORKLOAD_LABELS = {"sharegpt_revisit": "ShareGPT 回访", "w3_prefetch_admission_ladder_and_survival": "预取准入负载 W3"}


def target_label(target: dict[str, Any]) -> str:
    fields = target["hicache"]
    policy = fields["prefetch_policy"]
    prefetch = {"wait_complete": "等待完成", "best_effort": "尽力而为", "timeout": "超时"}[policy]
    if policy == "timeout":
        prefetch += f" {fields['prefetch_timeout_base_sec']:g}s"

    return f"{WRITE_LABELS[fields['write_policy']]} / {prefetch} / {fields['prefetch_threshold_pages']}页"


def space_figure(root: Path, protocol: dict[str, Any]) -> None:
    fields = protocol["fixed_hicache"]
    page = fields["page_size"]
    rows = [
        ["固定部署", "SGLang / Qwen3-32B / TP=2 / Ascend / 文件存储"],
        [
            "各候选资源相同",
            f"{page} tokens/页；设备 {fields['l1_capacity_pages'] * page:,} tokens，主机 {fields['l2_capacity_pages'] * page:,} tokens",
        ],
        ["Base 策略", target_label({"hicache": fields})],
        ["写入策略", "直写、选择性直写、写回"],
        ["预取策略", "等待完成、尽力而为、超时等待"],
        ["预取门槛", "、".join(str(value) + "页" for value in protocol["space"]["prefetch_threshold_pages"])],
        [
            "超时基础等待",
            "、".join(f"{value:g}s" for value in protocol["space"]["timeout"]["prefetch_timeout_base_sec"]),
        ],
        ["每个负载的配置数", "3 种写入 × 4 个门槛 × 6 种预取设置 = 72"],
    ]
    for source in protocol["sources"]:
        rows.append([WORKLOAD_LABELS[source["workload"]], f"{source['requests']} 个总请求，含准备；token 序列固定"])

    fig, axis = plt.subplots(figsize=(13, 5.8), constrained_layout=True)
    axis.axis("off")
    table = axis.table(cellText=rows, colWidths=[0.24, 0.76], loc="center", cellLoc="left")
    table.auto_set_font_size(False)
    table.set_fontsize(12)
    table.scale(1, 2.1)
    for (row, column), cell in table.get_celld().items():
        cell.set_edgecolor("#d6e0e6")
        cell.set_facecolor("#eaf1f6" if column == 0 else "white")
    axis.set_title("首轮搜索：固定资源，比较缓存策略", fontsize=17, pad=15)
    fig.savefig(root / "figures/search_space.png", dpi=180)
    fig.savefig(root / "figures/search_space.pdf")
    plt.close(fig)


def search_comparison(root: Path, workload: str, protocol: dict[str, Any]) -> list[dict[str, Any]]:
    """Replay selection order against fixed model scores, not prediction wall time."""

    request = SearchRequest.load(root / "inputs" / f"search_{workload}.json")
    state = load_json(root / "search" / workload / "search_summary.json")
    if state["status"] not in {"completed", "completed_with_gaps"}:
        raise ValueError(f"enumeration is still running or stopped early: {workload}")

    scores = {row["space_index"]: row["score_sec"] for row in state["candidates"]}
    base = request.space.locate(state["base_target"]["hicache"])
    reference = min(value for value in scores.values() if value is not None)
    trials = []
    settings = protocol["efficiency"]
    for algorithm in settings["algorithms"]:
        seeds = [None] if algorithm == "enumerate" else settings["seeds"]
        for seed in seeds:
            stream = candidate_indices(request.space, algorithm, seed)
            order = [base, *(index for index in stream if index != base)]
            for budget in settings["budgets_including_self"]:
                selected = order[:budget]
                available = [scores[index] for index in selected if scores[index] is not None]
                best = min(available) if available else None
                trials.append(
                    {
                        "workload": workload,
                        "algorithm": algorithm,
                        "seed": seed,
                        "predictions_including_self": len(selected),
                        "indices": selected,
                        "best_predicted_sec": best,
                        "loss_vs_enumerated_reference": (best - reference) / reference if best is not None else None,
                        "reference_is_full_space": state["global_model_optimum_proven"],
                        "missing_selected_predictions": len(selected) - len(available),
                        "covered_strategy_branches": len({request.space.branch_index(index) for index in selected}),
                        "total_strategy_branches": len(request.space.branches),
                    }
                )

    return trials


def prediction_figure(root: Path, workload: str, protocol: dict[str, Any]) -> None:
    state = load_json(root / "search" / workload / "search_summary.json")
    settings = [
        ("wait_complete", None),
        ("best_effort", None),
        *(("timeout", t) for t in protocol["space"]["timeout"]["prefetch_timeout_base_sec"]),
    ]
    labels = ["等待完成", "尽力而为", "超时 10ms", "超时 100ms", "超时 500ms", "超时 2s"]
    writes = protocol["space"]["write_policy"]
    times = [row["score_sec"] for row in state["candidates"] if row["score_sec"] is not None]
    low, high = min(times), max(times)

    fig, axes = plt.subplots(2, 2, figsize=(14, 7), constrained_layout=True)
    for axis, threshold in zip(axes.flat, protocol["space"]["prefetch_threshold_pages"]):
        values = np.full((len(writes), len(settings)), np.nan)
        for row in state["candidates"]:
            fields = row["hicache"]
            if fields["prefetch_threshold_pages"] != threshold:
                continue

            setting = (
                fields["prefetch_policy"],
                fields["prefetch_timeout_base_sec"] if fields["prefetch_policy"] == "timeout" else None,
            )
            values[writes.index(fields["write_policy"]), settings.index(setting)] = (
                row["score_sec"] if row["score_sec"] is not None else np.nan
            )
        image = axis.imshow(values, vmin=low, vmax=high, cmap="YlGnBu")
        for y, x in np.ndindex(values.shape):
            axis.text(
                x,
                y,
                f"{values[y, x]:.3f}" if np.isfinite(values[y, x]) else "缺预测",
                ha="center",
                va="center",
                color="white" if values[y, x] > (low + high) / 2 else "black",
                fontsize=10,
            )
        axis.set_xticks(range(len(labels)), labels, fontsize=9)
        axis.set_yticks(range(len(writes)), [WRITE_LABELS[name] for name in writes])
        axis.set_title(f"预取门槛 {threshold} 页（{threshold * protocol['fixed_hicache']['page_size']} tokens）")

    fig.suptitle(f"{WORKLOAD_LABELS[workload]}：72 个策略配置，{len(times)} 个已得到预测", fontsize=16)
    fig.colorbar(image, ax=axes, label="预测完整 HTTP 时间 / 秒", shrink=0.8)
    output = root / "figures"
    output.mkdir(exist_ok=True)
    fig.savefig(output / f"prediction_space_{workload}.png", dpi=180)
    fig.savefig(output / f"prediction_space_{workload}.pdf")
    plt.close(fig)


def efficiency_figure(root: Path, trials: list[dict[str, Any]], workloads: list[str]) -> None:
    colors = {"enumerate": "#777777", "random": "#c58a37", "stratified_random": "#276b9b"}
    labels = {"enumerate": "枚举前缀", "random": "均匀随机", "stratified_random": "分层随机"}
    fig, axes = plt.subplots(1, len(workloads), figsize=(12, 4.5), squeeze=False, constrained_layout=True)
    for axis, workload in zip(axes.flat, workloads):
        for algorithm, color in colors.items():
            rows = [row for row in trials if row["workload"] == workload and row["algorithm"] == algorithm]
            budgets = sorted({row["predictions_including_self"] for row in rows})
            values = [
                [
                    100 * row["loss_vs_enumerated_reference"]
                    for row in rows
                    if row["predictions_including_self"] == budget and row["loss_vs_enumerated_reference"] is not None
                ]
                for budget in budgets
            ]
            axis.plot(budgets, [np.median(v) for v in values], marker="o", label=labels[algorithm], color=color)
            axis.fill_between(budgets, [min(v) for v in values], [max(v) for v in values], color=color, alpha=0.12)
        axis.set(
            title=WORKLOAD_LABELS[workload], xlabel="预测次数（含一次 Base self）", ylabel="相对枚举参照的预测损失 / %"
        )
        axis.grid(axis="y", alpha=0.2)
        axis.legend()
    fig.suptitle("选点方法比较：仅比较已有预测；缺预测点的表现未知", fontsize=13)
    fig.savefig(root / "figures/search_efficiency.png", dpi=180)
    fig.savefig(root / "figures/search_efficiency.pdf")
    plt.close(fig)


def model_report(root: Path, protocol: dict[str, Any]) -> list[str]:
    """Return model findings and save executed mechanism examples without reading normal scores."""

    text = [
        "# HiCache 搜索结果",
        "",
        "同一模型、设备和缓存容量下，分别比较两个负载的 72 个策略组合。模型排序只决定值得实测谁，不能单独证明实际收益。",
        "",
        "参数、资源和负载见[搜索范围表](figures/search_space.png)。图表同时提供 PDF，便于放入汇报。",
        "",
        "ShareGPT 回访观察会话内容复用时的选择；W3 集中考察预取和容量压力。两种负载分别比较，不合成一个掩盖差异的总分。"
        "预测仍沿用部分 Base 残余等待；完整 HTTP 口径不代表这些等待已经能够随配置变化外推。",
        "",
    ]
    examples = {}
    domain_notes = {
        "outside_measured_ranges": "首选有部分操作的工作量超出已有测量范围，成本使用了外推；本轮正常实测用于检验这种估计。",
        "within_ranges_unseen_combination": "首选所需独立操作的变量均在已有测量范围内，但部分变量组合没有直接测过，仍需正常实测检验。",
        "measured_operator_query_points": "首选的独立操作成本查询落在已测工作点上；这不代表整张 DAG 的成本和等待都已经验证。",
        "no_independent_operator_queries": "首选没有独立操作成本查询；不能据此判断其余迁移成本是否准确。",
        "not_recorded": "首选未记录独立操作成本的查询范围，当前不能判断这些测量是否覆盖所需工作。",
    }
    for source in protocol["sources"]:
        workload = source["workload"]
        state = load_json(root / "search" / workload / "search_summary.json")
        ranked = sorted(
            (row for row in state["candidates"] if row["score_sec"] is not None),
            key=lambda row: (row["score_sec"], row["space_index"]),
        )
        base = state["baseline"]
        representatives = {row["name"] for row in [base, *ranked[:2], ranked[-1]]}
        native_paths = {}
        for attempt in state["attempts"]:
            if attempt["report"] is None:
                continue

            for row in load_json(require_repo_path(attempt["report"]))["prediction"]["cells"]:
                if row["status"] == "EXECUTED" and row["target_config"] in representatives:
                    native_paths.setdefault(
                        row["target_config"],
                        require_repo_path(attempt["prediction_dir"])
                        / "model_runs"
                        / row["model_run_id"]
                        / "run_summary.json",
                    )

        rows = []
        for target in ranked:
            if target["name"] not in representatives:
                continue

            native = native_paths[target["name"]]
            execution = load_json(native)["module_results"]["hicache_execution"]
            rows.append(
                dict(
                    target=target["name"],
                    label=target_label(target),
                    predicted_sec=target["score_sec"],
                    time_reduction_vs_predicted_base=1 - target["score_sec"] / base["score_sec"],
                    write_submissions=execution["expanded_write_submissions"],
                    completed_load_batches=execution["completed_load_batches"],
                    blocking_checks=execution["generated_blocking_checks"],
                    active_layer_calls=execution["layer_calls"]["active"],
                    native_summary=str(native.relative_to(Path.cwd())),
                )
            )
        examples[workload] = rows
        text.extend(
            [
                f"## {WORKLOAD_LABELS[workload]}：模型搜索",
                "",
                f"得到 {state['successful_configurations']}/72 个完整预测；其余配置保持未知，不按慢配置处理。"
                f"已知预测范围为 {ranked[0]['score_sec']:.3f}–{ranked[-1]['score_sec']:.3f} 秒，"
                f"Base 为 {base['score_sec']:.3f} 秒。最低预测共有 {len(state['best_candidates'])} 个并列配置。",
                "",
                domain_notes[ranked[0]["workloads"][workload]["operator_query_domain"]],
                "",
                "| 代表配置 | 预测 / 秒 | 相对 Base 缩短 | 写入提交 | 加载完成批数 | 阻塞检查 | 读回逐层检查 |",
                "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
            ]
        )
        for row in rows:
            label = row["label"] + ("（Base）" if row["target"] == protocol["base"] else "")
            text.append(
                f"| {label} | {row['predicted_sec']:.3f} | {100 * row['time_reduction_vs_predicted_base']:.2f}% | "
                f"{row['write_submissions']} | {row['completed_load_batches']} | {row['blocking_checks']} | {row['active_layer_calls']} |"
            )
        text.extend(
            [
                "",
                "结构计数覆盖模型执行的准备和正式请求，各 rank 都计入。末列只统计有读回依赖的逐层检查，"
                "不是 Prefill/Decode 计算层数；为零不表示没有计算。"
                "这些数说明工作有无变化，不是正式窗口的 I/O 字节数，也不能相加推算 HTTP 时间。",
                "",
                "best-effort 不为尚未完成的预取继续等待，模型按当时已经可用的缓存推进计算。"
                "省下的等待与可能增加的重算共同决定预测时间，不能只用 I/O 次数解释收益。",
                "",
                f"全部参数与状态见[本负载搜索结果](search/{workload}/search_summary.json)。",
                "",
                f"[查看预测分布图](figures/prediction_space_{workload}.png)。",
                "",
            ]
        )
        missing = [target for target in state["candidates"] if target["score_sec"] is None]
        if missing:
            text.extend(
                [
                    "以下配置没有参与排名。缺少预测时间不能解释为真实更慢或真实无法运行。",
                    "",
                    "| 配置 | 未完成原因 | 证据 |",
                    "| --- | --- | --- |",
                ]
            )
            labels = {
                "model_command_failed": "预测执行失败",
                "cost_data_missing": "缺少成本数据",
                "structure_gap": "结构支持有缺口",
                "not_started": "尚未得到预测",
            }
            for target in missing:
                detail = target["workloads"][workload]
                reason = "、".join(labels.get(code, code) for code in detail.get("blockers", [])) or "未得到完整预测"
                log = detail.get("failure_log")
                evidence = (
                    require_repo_path(log).relative_to(root)
                    if log
                    else Path("search") / workload / "search_summary.json"
                )
                text.append(f"| {target_label(target)} | {reason} | [执行记录]({evidence}) |")
            text.append("")

    diagnosis = root / "capacity_diagnosis/findings.md"
    if diagnosis.exists():
        text.extend(
            ["四个容量拒绝的现场解释见[容量边界分析](capacity_diagnosis/findings.md)。它不是真实 OOM 的证明。", ""]
        )

    bounded = load_json(root / "efficiency_comparison.json")["actual_budget_search"]
    text.extend(
        [
            "## 搜索效率",
            "",
            f"真实运行的分层随机用了 {bounded['charged_predictions']} 次总预测（含 self），"
            f"得到 {bounded['successful_configurations']} 个有效配置，墙钟为 {bounded['wall_seconds']:.2f} 秒，包含源图准备。",
            "",
            f"它找到的最低预测为 {bounded['best_predicted_sec']:.3f} 秒，"
            f"相对完整枚举中已知最低预测的差距为 {100 * bounded['loss_vs_enumerated_reference']:.2f}%。"
            "这是模型内比较；若枚举仍有未知点，该参照不代表全空间最优。",
            "",
            "图中的多种预算与随机种子复用枚举分数，只比较选点效率；线表示种子均值，阴影表示种子间的最小至最大值。"
            "只有上述一轮真实预算搜索记录了实际预测用时。",
            "",
            "[查看选点效率曲线](figures/search_efficiency.png)。",
            "",
        ]
    )
    write_json(root / "mechanism_examples.json", examples)
    return text


def timing_report(root: Path, protocol: dict[str, Any], normal: dict[str, Any]) -> dict[str, Any]:
    """Separate historical acquisition, current search, confirmation and development work."""

    base_seconds = light_seconds = 0.0
    rows = []
    for source in protocol["sources"]:
        base_seconds += load_json(require_repo_path(source["manifest"]))["duration_sec"]
        cpu = cpu_service_metadata(require_repo_path(source["cpu_service"]))
        light_seconds += load_json(require_repo_path(cpu["light_manifest"]))["duration_sec"]
        state = load_json(root / "search" / source["workload"] / "search_summary.json")
        rows.append(
            dict(
                stage=WORKLOAD_LABELS[source["workload"]] + "完整搜索",
                count=f"{state['charged_predictions']} 次预测，{state['successful_configurations']} 个成功配置",
                wall_seconds=state["total_wall_seconds"],
                source_preparation_seconds_included=state["completed_source_preparation_wall_seconds"],
            )
        )

    bounded = load_json(root / "budget_search/search_summary.json")
    rows.append(
        dict(
            stage="真实分层随机搜索",
            count=f"{bounded['charged_predictions']} 次预测，含 self",
            wall_seconds=bounded["total_wall_seconds"],
            source_preparation_seconds_included=bounded["completed_source_preparation_wall_seconds"],
        )
    )
    for attempt in protocol.get("prior_search_attempts", []):
        rows.append(
            dict(
                stage="规则修正前的已中断搜索",
                count=f"{attempt['completed_predictions']} 个完成预测；不并入本轮排名",
                wall_seconds=attempt["wall_seconds"],
            )
        )

    diagnostics = protocol.get("prior_diagnostic_attempts", [])
    rows.append(
        dict(
            stage="已计时的旧规则 Debug 尝试",
            count=f"{len(diagnostics)} 次，含失败和主动中断；不作为当前预测",
            wall_seconds=sum(attempt["wall_seconds"] for attempt in diagnostics),
        )
    )

    validation = [load_json(require_repo_path(path)) for path in protocol.get("validation_runs", [])]
    rows.append(
        dict(
            stage="当前规则的关键输入验证",
            count=f"{len(validation)} 次公开预测命令；Release 与诊断执行分开保存",
            wall_seconds=sum(attempt["wall_seconds"] for attempt in validation),
        )
    )
    diagnosis = root / "capacity_diagnosis/invocation.json"
    if diagnosis.exists():
        rows.append(
            dict(
                stage="容量拒绝的只读 Debug 检查",
                count="一次现场诊断，不是成功预测",
                wall_seconds=load_json(diagnosis)["wall_seconds"],
            )
        )

    physical = load_json(require_repo_path(protocol["preparation_cpu_budget"]["ledger"]))
    rows.append(
        dict(
            stage="新增共享控制测量",
            count=f"{physical['usage']['container_starts']} 次控制原语容器调用，含失败；无新增推理请求",
            wall_seconds=physical["usage"]["wall_seconds"],
        )
    )
    fitting = [
        entry
        for name in ("cost_invocations.json", "rejection_cost_invocations.json", "shared_control_cost_invocations.json")
        for entry in load_json(require_repo_path(protocol["preparation_cpu_budget"]["ledger"]).parent / name)[
            "invocations"
        ]
    ]
    rows.append(
        dict(
            stage="已计时的导入与构模尝试",
            count=f"{len(fitting)} 次命令，含中断的整库训练",
            wall_seconds=sum(row["wall_seconds"] for row in fitting),
        )
    )
    rows.append(
        dict(
            stage="正常确认",
            count=f"{normal['usage']['server_starts']} 次启动，含失败与重试",
            wall_seconds=normal["usage"]["wall_seconds"],
        )
    )
    execution = root / "normal_execution.json"
    if execution.exists():
        rows.append(
            dict(
                stage="正常确认前的设备等待",
                count="与采集生命周期分开，不计作预测加速",
                wall_seconds=load_json(execution)["resource_wait_seconds"],
            )
        )
    resource_wait = root / "normal/resource_wait.json"
    if resource_wait.exists():
        rows.append(
            dict(
                stage="采集间隔的设备等待",
                count="逐次启动前发现其他 NPU 应用，另记累计等待",
                wall_seconds=load_json(resource_wait)["wall_seconds"],
            )
        )
    rows.extend(
        [
            dict(
                stage="历史 Base 采集生命周期（本轮复用）",
                count=f"{len(protocol['sources'])} 份",
                wall_seconds=base_seconds,
            ),
            dict(
                stage="历史 CPU 轻量配对采集（本轮复用）",
                count=f"{len(protocol['sources'])} 份",
                wall_seconds=light_seconds,
            ),
            dict(stage="原平台性能库初建、构建与其余调试", count="计时不完整，不记为零", wall_seconds=None),
        ]
    )
    return dict(
        rows=rows,
        interpretation="Search includes its source preparation. Historical rows are reused inputs, not new acquisitions. Steps may overlap; summing rows is not total elapsed time. The table is not a cold-start cost estimate.",
    )


def timing_figure(root: Path, rows: list[dict[str, Any]]) -> None:
    values = [
        [
            row["stage"],
            row["count"],
            f"{row['wall_seconds'] / 60:.1f}" if row["wall_seconds"] is not None else "未完整记录",
        ]
        for row in rows
    ]
    fig, axis = plt.subplots(figsize=(15, 0.45 * len(rows) + 1.2), constrained_layout=True)
    axis.axis("off")
    table = axis.table(
        cellText=values,
        colLabels=["阶段", "实际范围", "生命周期 / 分钟"],
        colWidths=[0.3, 0.56, 0.14],
        loc="center",
        cellLoc="left",
    )
    table.auto_set_font_size(False)
    table.set_fontsize(11)
    table.scale(1, 2.0)
    for (row, column), cell in table.get_celld().items():
        cell.set_edgecolor("#d6e0e6")
        cell.set_facecolor("#eaf1f6" if row == 0 or column == 0 else "white")
    axis.set_title("准备、搜索和确认的实际用时", fontsize=17, pad=15)
    fig.text(
        0.025,
        0.025,
        "搜索已包含源图准备；历史采集为复用。开发排查、失败和设备等待均单列。\n"
        "部分步骤曾并行，不能简单相加；缺失计时不记为零。",
        fontsize=11,
    )
    fig.savefig(root / "figures/actual_time.png", dpi=180)
    fig.savefig(root / "figures/actual_time.pdf")
    plt.close(fig)


def normal_figure(root: Path, workload: str, measurements: list[dict[str, Any]], base: str, first: str) -> None:
    fig, axis = plt.subplots(figsize=(13, 0.42 * len(measurements) + 2.4), constrained_layout=True)
    positions = np.arange(len(measurements))
    means = np.array([row["measured_mean_sec"] for row in measurements])
    axis.barh(positions, means, color="#dce7ee", height=0.65, label="正常实测均值")
    for index, row in enumerate(measurements):
        axis.scatter(row["samples_sec"], [index] * len(row["samples_sec"]), s=15, color="#52758b", zorder=3)
    axis.scatter(
        [row["predicted_sec"] for row in measurements],
        positions,
        marker="D",
        s=28,
        color="#c58232",
        label="预测",
        zorder=4,
    )
    labels = []
    for row in measurements:
        marks = []
        if row["target"] == base:
            marks.append("Base")
        if row["target"] == first:
            marks.append("预测首选")
        labels.append(row["label"] + ("（" + "，".join(marks) + "）" if marks else ""))
    axis.set_yticks(positions, labels)
    axis.invert_yaxis()
    axis.set_xlabel("完整 HTTP 正式窗口 / 秒；散点保留每次有效实测")
    axis.set_title(WORKLOAD_LABELS[workload] + "：预测与五次正常实测")
    axis.legend(loc="lower left")
    axis.grid(axis="x", alpha=0.15)
    fig.savefig(root / "figures" / f"normal_{workload}.png", dpi=180)
    fig.savefig(root / "figures" / f"normal_{workload}.pdf")
    plt.close(fig)


def normal_report(root: Path, protocol: dict[str, Any], text: list[str]) -> None:
    """Append independent normal evidence to text in place and retain every valid sample."""

    selection = load_json(root / "selection.json")
    ledger = load_json(root / "normal/confirmation_capture_ledger.json")
    report: dict[str, Any] = {"scope": "完整 HTTP 正式窗口；未排除 gap、未归属成本或 Prefill/Decode", "workloads": {}}
    overview = [
        "本轮已完成预声明的正常对照，每个条件保留五次有效实测。预测首选在两个负载上都比原配置快，"
        "但还不能据此认定它们是整个空间的真实最优。",
        "",
        "| 负载 | Base 实测 / 秒 | 预测首选实测 / 秒 | 时间缩短 | 已测 cross WAPE | P90 |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    text.extend(["## 正常实测", "", report["scope"] + "。正常实测未用于训练或修改预测排名。", ""])
    all_configuration_samples = 0
    for workload, group in selection["workloads"].items():
        targets = {row["name"]: row for row in group["targets"]}
        state = load_json(root / "search" / workload / "search_summary.json")
        all_configuration_samples += len(state["candidates"]) * selection["rule"]["repetitions"]
        predictions = [
            row for (target, name), row in prediction_rows(state).items() if target in targets and name == workload
        ]
        score_root = root / "evaluation" / workload
        cells = [load_json(score_root / "cells" / f"{row['model_run_id']}.json") for row in predictions]
        gate = load_json(score_root / "gate_summary.json")
        base = next(row["full_e2e"] for row in cells if row["is_self"])
        ordered = sorted(cells, key=lambda row: targets[row["target_config"]]["space_index"])
        measurements = []
        for row in ordered:
            metric = row["full_e2e"]
            samples = [
                load_json(require_repo_path(path))["formal_window"]["e2e_ms"] / 1000
                for path in metric["target_reports"]
            ]
            measurements.append(
                dict(
                    target=row["target_config"],
                    label=target_label(targets[row["target_config"]]),
                    predicted_sec=metric["predicted_us"] / 1e6,
                    measured_mean_sec=metric["target_us"] / 1e6,
                    samples_sec=samples,
                    ape=metric["ape"],
                    time_reduction_vs_base=1 - metric["target_us"] / base["target_us"],
                    sample_range_below_base=metric["target_range_us"][1] < base["target_range_us"][0],
                    sample_range_above_base=metric["target_range_us"][0] > base["target_range_us"][1],
                )
            )

        ranked = sorted(targets.values(), key=lambda row: (row["score_sec"], row["space_index"]))
        first = next(row for row in measurements if row["target"] == ranked[0]["name"])
        difference = 100 * first["time_reduction_vs_base"]
        overview.append(
            f"| {WORKLOAD_LABELS[workload]} | {base['target_us'] / 1e6:.3f} | {first['measured_mean_sec']:.3f} | "
            f"{difference:.2f}% | {100 * gate['full_e2e']['wape']:.2f}% | {100 * gate['full_e2e']['p90_ape']:.2f}% |"
        )
        measured_change = (
            f"实测均值比 Base 快 {difference:.2f}%" if difference >= 0 else f"实测均值比 Base 慢 {-difference:.2f}%"
        )
        separation = "两者的样本范围仍有重叠，不能仅凭均值宣称稳定收益。"
        if first["sample_range_below_base"]:
            separation = "它的五次样本均低于 Base 的五次样本。"
        elif first["sample_range_above_base"]:
            separation = "它的五次样本均高于 Base，预测的选择没有得到本轮实测支持。"

        result = dict(
            measurements=measurements,
            first_predicted_representative=first,
            selection_on_measured_set=gate["configuration_selection"],
            cross_accuracy=gate["full_e2e"],
            accuracy_gate=gate["status"],
        )
        if workload == "sharegpt_revisit":
            reference = set(protocol["normal_confirmation"]["sharegpt_reference_indices"])
            result["selection_on_12_point_reference"] = configuration_selection(
                [row for row in cells if targets[row["target_config"]]["space_index"] in reference]
            )
            reference_measurements = [row for row in measurements if targets[row["target"]]["space_index"] in reference]
            reference_choice = min(
                reference_measurements, key=lambda row: (row["predicted_sec"], targets[row["target"]]["space_index"])
            )
            measured_best = min(reference_measurements, key=lambda row: row["measured_mean_sec"])
            result["reference_choice"] = dict(
                target=reference_choice["target"],
                measured_best=measured_best["target"],
                loss_vs_measured_best=reference_choice["measured_mean_sec"] / measured_best["measured_mean_sec"] - 1,
                full_space_representative_reduction=1 - first["measured_mean_sec"] / measured_best["measured_mean_sec"],
            )
        report["workloads"][workload] = result
        result["unpredicted_candidates"] = group["unpredicted_candidates"]
        result["global_model_optimum_proven"] = group["global_model_optimum_proven"]

        text.extend(
            [
                f"## {WORKLOAD_LABELS[workload]}",
                "",
                f"72 点中有 {len(group['unpredicted_candidates'])} 个配置未得到预测；正常实测仅覆盖下表候选，未测配置的真实表现仍然未知。",
                "",
                f"预测第一名代表为 **{first['label']}**，{measured_change}。" + separation,
                "",
                f"除 Base 自身外的已测配置，WAPE 为 {100 * gate['full_e2e']['wape']:.2f}%，P90 为 {100 * gate['full_e2e']['p90_ape']:.2f}%，"
                f"最大误差为 {100 * gate['full_e2e']['max_ape']:.2f}%；严格精度验收为 `{gate['status']}`。",
                "",
                "| 配置 | 预测 / 秒 | 实测均值 / 秒 | APE | 相对 Base 缩短 | 五次有效实测 / 秒 |",
                "| --- | ---: | ---: | ---: | ---: | --- |",
            ]
        )
        for row in measurements:
            samples = "、".join(f"{value:.3f}" for value in row["samples_sec"])
            text.append(
                f"| {row['label']} | {row['predicted_sec']:.3f} | {row['measured_mean_sec']:.3f} | "
                f"{100 * row['ape']:.2f}% | {100 * row['time_reduction_vs_base']:.2f}% | {samples} |"
            )
        text.extend(
            [
                "",
                "单格 APE 是预测与五次实测均值之差的绝对值，除以实测均值；WAPE 按实测时间加权。"
                "P90 将各格 APE 排序后取第 ceil(0.9 × 格数) 项。",
                "",
                "实测结论仅覆盖已测候选；未实测的其他配置不能据此排除。五次样本范围不是置信区间。",
                "",
            ]
        )
        ordering = gate["configuration_selection"][0]
        text.extend(
            [
                f"样本范围不重叠的配置对中，{ordering['correctly_ordered_pairs']} 对排序一致，"
                f"{ordering['inverted_pairs']} 对相反；另有 {ordering['overlapping_sample_ranges']} 对范围重叠，不判断胜负。"
                f"详见[排序记录](evaluation/{workload}/gate_summary.json)。精度通过不等于排序全部正确。",
                "",
            ]
        )
        if workload == "sharegpt_revisit":
            detail = result["reference_choice"]
            text.extend(
                [
                    f"在预声明的 12 点参照里，模型选中 **{reference_choice['label']}**，其均值比这 12 点实测最佳高 **{100 * detail['loss_vs_measured_best']:.2f}%**。72 点搜索的已知首选代表相对 12 点实测最佳的时间缩短为 **{100 * detail['full_space_representative_reduction']:.2f}%**，负值表示更慢。并列预测按原定空间顺序选代表，没有用实测挑选。",
                    "",
                ]
            )
        normal_figure(root, workload, measurements, protocol["base"], first["target"])
        text.extend([f"[查看预测与正常实测对照图](figures/normal_{workload}.png)。", ""])

    mechanisms = root / "normal_mechanism_examples.json"
    if mechanisms.exists():
        report["normal_mechanism_examples"] = load_json(mechanisms)
        text.extend(
            [
                "## 策略改变了哪些工作",
                "",
                "下表取 Base 和预选首选的第一份有效正常运行，按正式请求顺序对齐框架 Prefill 批次日志。"
                "token 计数包含对齐；缓存复用也包含设备缓存，不能直接换算成 HiCache 读回字节数。",
                "",
                "| 负载 | 配置 | 正式请求数 | 新计算 tokens | 缓存复用 tokens |",
                "| --- | --- | ---: | ---: | ---: |",
            ]
        )
        for row in report["normal_mechanism_examples"]["rows"]:
            target = next(
                target
                for target in selection["workloads"][row["workload"]]["targets"]
                if target["name"] == row["target"]
            )
            label = target_label(target) + ("（Base）" if row["role"] == "base" else "（预测首选）")
            text.append(
                f"| {WORKLOAD_LABELS[row['workload']]} | {label} | {row['formal_requests']} | "
                f"{row['framework_new_tokens']:,} | {row['framework_cached_tokens']:,} |"
            )
        text.extend(
            [
                "",
                "这些代表显示，少复用缓存会增加 Prefill 重算。是否值得用更多计算换取更少等待，仍由五次完整 E2E 实测判断。"
                "单次工作计数不替代五次耗时均值，也不证明整张目标 DAG 都正确。",
                "",
                "各项原始 manifest、请求报告和服务日志见[正常运行机制样本](normal_mechanism_examples.json)。",
                "",
            ]
        )

    report["normal_capture_usage"] = ledger["usage"]
    report["search_efficiency"] = load_json(root / "efficiency_comparison.json")
    report["failed_normal_attempts"] = [
        {key: value for key, value in row.items() if key in {"status", "error", "wall_seconds", "profile_manifest"}}
        for row in ledger["attempts"]
        if row["status"] != "completed"
    ]
    failures = sum(row["status"] == "failed" for row in ledger["attempts"])
    interruptions = sum(row["status"] == "interrupted" for row in ledger["attempts"])
    report["timing"] = timing_report(root, protocol, ledger)
    timing_figure(root, report["timing"]["rows"])
    text.extend(["## 实际代价", "", "| 阶段 | 次数与范围 | 生命周期 / 秒 |", "| --- | --- | ---: |"])
    for row in report["timing"]["rows"]:
        seconds = f"{row['wall_seconds']:.2f}" if row["wall_seconds"] is not None else "未完整记录"
        text.append(f"| {row['stage']} | {row['count']} | {seconds} |")
    text.extend(
        [
            "",
            "搜索时间已包含源图准备；历史输入在本轮复用。部分步骤曾并行，不能把各行简单相加，也不能据此声称从空平台开始的总成本。",
            "",
            f"若将两个搜索空间的所有配置都各测 {selection['rule']['repetitions']} 次，采集计划规模为 {all_configuration_samples} 次。"
            f"本轮按预声明集合完成 {sum(row['status'] == 'completed' for row in ledger['attempts'])} 次有效实测，失败启动另记。"
            "这是采集规模的对照，不是实测加速比。",
            "",
            "[查看准备、搜索和确认用时表](figures/actual_time.png)。",
            "",
        ]
    )
    text.extend(
        [
            "## 采集与来源",
            "",
            f"正常确认累计启动 {ledger['usage']['server_starts']} 次，账本计入 {ledger['usage']['requests']:,} 个请求、"
            f"{ledger['usage']['tokens']:,} tokens，采集生命周期 {ledger['usage']['wall_seconds'] / 60:.1f} 分钟。"
            f"其中失败 {failures} 次、主动中断 {interruptions} 次，未作为有效样本；"
            "失败、重试和所有有效慢样本保留在原账本中。",
            "",
            "预测、确认集合、正常采集、独立评分分别见本目录下的 `search/`、`selection.json`、`normal/` 和 `evaluation/`。",
            "",
        ]
    )
    overview.extend(
        [
            "",
            "误差对照包含 gap、未归属成本及 Prefill/Decode，按每个条件的五次实测均值计算，self 单列。"
            "ShareGPT 推荐比 12 点参照的实测最快均值略慢；细小差异和样本重叠见后文，不宣称全部排序正确。",
            "",
        ]
    )
    text[2:2] = overview
    write_json(root / "normal_confirmation.json", report)
    (root / "results.md").write_text("\n".join(text), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--normal", action="store_true", help="Also read completed independent normal evaluations.")
    args = parser.parse_args()
    root = args.root.resolve()
    protocol = load_json(root / "protocol.json")
    plt.rcParams.update({"font.family": "Noto Sans CJK SC", "axes.unicode_minus": False})
    workloads = [source["workload"] for source in protocol["sources"]]
    trials = [row for workload in workloads for row in search_comparison(root, workload, protocol)]
    bounded_request = SearchRequest.load(root / "inputs/budget_search.json")
    bounded = load_json(root / "budget_search/search_summary.json")
    bounded_workload = next(iter(bounded_request.weights))
    enumerated = load_json(root / "search" / bounded_workload / "search_summary.json")
    replay = next(
        row
        for row in trials
        if row["workload"] == bounded_workload
        and row["algorithm"] == bounded_request.algorithm
        and row["seed"] == bounded_request.seed
        and row["predictions_including_self"] == bounded["charged_predictions"]
    )
    actual = dict(
        workload=bounded_workload,
        status=bounded["status"],
        charged_predictions=bounded["charged_predictions"],
        successful_configurations=bounded["successful_configurations"],
        best_predicted_sec=bounded["best_score_sec"],
        loss_vs_enumerated_reference=bounded["best_score_sec"] / enumerated["best_score_sec"] - 1,
        reference_is_full_space=enumerated["global_model_optimum_proven"],
        wall_seconds=bounded["total_wall_seconds"],
        source_preparation_seconds_included=bounded["completed_source_preparation_wall_seconds"],
        selection_matches_posthoc_order=[row["space_index"] for row in bounded["selected_candidates"]]
        == replay["indices"],
        replay_best_predicted_sec=replay["best_predicted_sec"],
    )
    write_json(
        root / "efficiency_comparison.json",
        {
            "scope": "Post-hoc selection efficiency, not new prediction executions or hardware performance.",
            "trials": trials,
            "actual_budget_search": actual,
        },
    )
    for workload in workloads:
        prediction_figure(root, workload, protocol)

    text = model_report(root, protocol)
    space_figure(root, protocol)
    efficiency_figure(root, trials, workloads)
    if args.normal:
        normal_report(root, protocol, text)
    else:
        (root / "results.md").write_text("\n".join(text), encoding="utf-8")


if __name__ == "__main__":
    main()
