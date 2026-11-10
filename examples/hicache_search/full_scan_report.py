"""Export completed full scans to CSV, a readable report, and scientific plots."""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
from pathlib import Path
import resource
from statistics import median, quantiles


def read(path):
    return json.loads(Path(path).read_text())


def write_csv(path, rows):
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def phases(cell):
    formal = [row for row in cell["requests"] if row["formal"]]
    preparation = [row for row in cell["requests"] if not row["formal"]]
    assert formal and preparation and all("prefetch" in row for row in formal)
    spans = [
        (row["prefetch"]["accepted_ns"] - row["prefetch"]["issued_ns"]) / 1e9
        for row in formal
        if row.get("prefetch", {}).get("issued_ns") is not None and row["prefetch"]["accepted_ns"] is not None
    ]
    return {
        "preparation_span_sec": (preparation[-1]["response_ns"] - preparation[0]["start_ns"]) / 1e9,
        "preparation_to_formal_interval_sec": (formal[0]["start_ns"] - preparation[-1]["response_ns"]) / 1e9,
        "formal_inter_request_gap_sec": cell["formal_http_sec"] - sum(row["http_sec"] for row in formal),
        "post_formal_background_sec": cell["background_finish_sec"] - formal[-1]["response_ns"] / 1e9,
        "formal_prefetch_issue_accept_intervals": len(spans),
        "formal_prefetch_issue_accept_span_sum_sec": sum(spans),
        "formal_stopped_early_requests": sum(row.get("prefetch", {}).get("stopped_early", False) for row in formal),
    }


def collect(root, task):
    qualification = read(root / "full_scan_qualification.json")
    source_evidence = read(root / "source_qualification.json")["scenes"]
    assert qualification["status"] == "complete"
    records, scenes = [], []
    for scene, request_path in task["requests"].items():
        request = read(request_path)
        result = read(root / scene / "results.json")
        assert result["status"] == "completed" and result["evaluated"] == result["expected"]
        ranked = {row["name"]: row for row in result["candidates"]}
        assert set(ranked) == {target["name"] for target in request["targets"]}
        baseline = ranked[result["baseline"]]
        budget = baseline["hourly_cost_units"]
        eligible = [row for row in ranked.values() if row["status"] == "ranked" and row["hourly_cost_units"] <= budget]
        endpoints = {
            role: ranked[result[key]]
            for role, key in (("baseline", "baseline"), ("best", "recommended"), ("worst", "worst_within_budget"))
        }
        ties = {
            role: sorted(
                (row["name"] for row in eligible if row["predicted_sec"] == endpoints[role]["predicted_sec"]),
                key=lambda name: (ranked[name]["hourly_cost_units"], name),
            )
            for role in ("best", "worst")
        }
        assert ties["best"][0] == result["recommended"] and ties["worst"][0] == result["worst_within_budget"]
        rows, batches, seen = [], [], set()
        for path in sorted((root / scene).glob("*/result.json")):
            batch = read(path)
            assert batch["status"] == "completed"
            batches.append(batch)
            report = read(batch["report"])
            for cell in report["cells"]:
                name = cell["name"]
                assert name not in seen and cell["status"] == "predicted" and not cell["cost_gaps"]
                seen.add(name)
                row = ranked[name]
                configuration = cell["configuration"]
                page = configuration["page_size"]
                extra = json.loads(configuration["hicache_storage_backend_extra_config"])
                work = result["work"][name]
                record = {
                    "scene": scene,
                    **row,
                    "within_baseline_budget": row["hourly_cost_units"] <= budget,
                    "best_time_tie": name in ties["best"],
                    "worst_time_tie": name in ties["worst"],
                    "pareto": name in result["pareto"],
                    "page_tokens": page,
                    "device_tokens": configuration["max_total_tokens"],
                    "host_ratio": configuration["hicache_ratio"],
                    "write_policy": configuration["hicache_write_policy"],
                    "prefetch_policy": configuration["hicache_storage_prefetch_policy"],
                    "prefetch_threshold_tokens": extra["prefetch_threshold"],
                    "prefetch_timeout_sec": extra.get("prefetch_timeout_base"),
                    **{key: value for key, value in work.items() if key.startswith("formal_")},
                    "formal_loaded_token_equivalents": work["formal_loaded_pages"] * page,
                    "formal_accepted_prefix_tokens": work["formal_accepted_pages"] * page,
                    "all_backup_submissions": work["all_backup_submissions"],
                    "all_load_submissions": work["all_load_submissions"],
                    "all_host_evicted_pages": work["all_host_evicted_pages"],
                    **phases(cell),
                    "coverage": cell["coverage"],
                    "issue": cell["issue"],
                    "configuration": configuration,
                    "prediction_report": batch["report"],
                }
                storage = work["all_storage"]
                for key in (
                    "queries",
                    "prefetches",
                    "rank_read_pages",
                    "rank_copied_pages",
                    "rank_rejected_copies",
                    "rank_new_pages",
                    "rank_existing_pages",
                    "rank_disk_evictions",
                ):
                    record["all_" + key] = storage.get(key)
                for key in ("rank_read_pages", "rank_copied_pages", "rank_new_pages"):
                    record["all_" + key.replace("pages", "token_equivalents")] = storage[key] * page
                assert math.isclose(sum(row["hourly_cost_breakdown"].values()), row["hourly_cost_units"], rel_tol=1e-12)
                assert cell["formal_http_sec"] == row["predicted_sec"]
                rows.append(record)
        assert seen == set(ranked)
        by_name = {row["name"]: row for row in rows}
        values = sorted(row["predicted_sec"] for row in eligible)
        percentiles = quantiles(values, n=100, method="inclusive")
        scenes.append(
            {
                "scene": scene,
                "formal_requests": task["formal_requests"][scene],
                "input_pressure": {
                    "unbounded_reusable_tokens": source_evidence[scene]["prefix"]["unbounded_cache_reusable_tokens"],
                    "paged_reusable_tokens": source_evidence[scene]["prefix"]["unbounded_cache_reusable_paged_tokens"],
                    "window_32_distinct_tokens": source_evidence[scene]["window_32_distinct_input_tokens"],
                    "identity_return_distance": source_evidence[scene]["identity_return_distance"],
                    "shared_family_prefix_tokens": source_evidence[scene]["generated_shared_family_prefix_tokens"],
                },
                "classification": qualification["scenes"][scene],
                "budget_hourly_units": budget,
                "within_budget": len(eligible),
                "endpoints": {role: by_name[row["name"]] for role, row in endpoints.items()},
                "ties": ties,
                "pareto": result["pareto"],
                "within_budget_time_sec": {
                    "min": values[0],
                    "p05": percentiles[4],
                    "median": median(values),
                    "p95": percentiles[94],
                    "max": values[-1],
                },
                "best_from_worst_time_reduction_pct": 100
                * (1 - endpoints["best"]["predicted_sec"] / endpoints["worst"]["predicted_sec"]),
                "best_from_baseline_time_reduction_pct": 100
                * (1 - endpoints["best"]["predicted_sec"] / baseline["predicted_sec"]),
                "best_from_baseline_time_reduction_sec": baseline["predicted_sec"] - endpoints["best"]["predicted_sec"],
                "best_from_worst_time_reduction_sec": endpoints["worst"]["predicted_sec"]
                - endpoints["best"]["predicted_sec"],
                "best_hourly_cost_reduction_units": budget - endpoints["best"]["hourly_cost_units"],
                "best_hourly_cost_reduction_pct": 100 * (1 - endpoints["best"]["hourly_cost_units"] / budget),
                "batch_count": len(batches),
                "batch_wall_sum_sec": sum(batch["wall_sec"] for batch in batches),
                "source_prepare_sum_sec": sum(batch["source_prepare_sec"] for batch in batches),
                "simulation_sum_sec": sum(batch["simulation_sec"] for batch in batches),
                "mean_target_simulation_sec": sum(batch["simulation_sec"] for batch in batches) / len(rows),
                "cpp_peak_rss_bytes": max(batch["process_peak_rss_bytes"] for batch in batches),
                "failed_load_attempts": sum(row["formal_failed_load_attempts"] for row in rows),
                "normal_accuracy": None,
            }
        )
        flattened = []
        for row in rows:
            flat = {
                key: json.dumps(value, ensure_ascii=False, sort_keys=True) if isinstance(value, (dict, list)) else value
                for key, value in row.items()
            }
            flat.update({"cost_" + key: value for key, value in row["hourly_cost_breakdown"].items()})
            flattened.append(flat)
        write_csv(root / scene / "candidates.csv", flattened)
        records.extend(flattened)
    write_csv(root.parent / "candidates.csv", records)
    return {
        "status": "completed",
        "scenes": scenes,
        "runtime": read(root / "usage.json"),
        "qualification": qualification,
    }, records


def plot(root, summary, records):
    os.environ.setdefault("MPLCONFIGDIR", str(root / "report_cache"))
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    figure, axes = plt.subplots(2, len(summary["scenes"]), figsize=(17, 8))
    for column, scene in enumerate(summary["scenes"]):
        rows = [row for row in records if row["scene"] == scene["scene"]]
        axis, cdf = axes[0, column], axes[1, column]
        axis.scatter(
            [row["hourly_cost_units"] for row in rows],
            [row["predicted_sec"] for row in rows],
            s=9,
            alpha=0.25,
            color="#657995",
            label="All candidates",
        )
        frontier = sorted({(row["hourly_cost_units"], row["predicted_sec"]) for row in rows if row["pareto"]})
        axis.scatter(*zip(*frontier), s=24, color="#df9229", label="Pareto points", zorder=3)
        for role, marker, color in (("baseline", "*", "#222222"), ("best", "^", "#16816a"), ("worst", "X", "#c14545")):
            row = scene["endpoints"][role]
            axis.scatter(
                row["hourly_cost_units"],
                row["predicted_sec"],
                marker=marker,
                color=color,
                s=85,
                label=role.title(),
                zorder=4,
            )
        axis.axvline(scene["budget_hourly_units"], color="#777777", linestyle=":", linewidth=1, label="Baseline budget")
        axis.set(
            title=f"{scene['scene']} | {scene['formal_requests']} requests",
            xlabel="Declared cost units / hour",
            ylabel="Predicted formal HTTP (s)",
        )
        values = sorted(row["predicted_sec"] for row in rows if row["within_baseline_budget"])
        cdf.step(
            values, [100 * (index + 1) / len(values) for index in range(len(values))], where="post", color="#426789"
        )
        cdf.axvline(
            scene["endpoints"]["baseline"]["predicted_sec"],
            color="#222222",
            linestyle=":",
            linewidth=1,
            label="Baseline time",
        )
        cdf.set(xlabel="Predicted formal HTTP (s)", ylabel="Within-budget candidates (%)", ylim=(0, 101))
        for current in (axis, cdf):
            current.grid(alpha=0.2)
        if column == 0:
            axis.legend(fontsize=8)
            cdf.legend(fontsize=8)
    figure.suptitle("Complete finite scans | fixed models | best/worst within baseline hourly budget")
    figure.tight_layout()
    figure.savefig(root.parent / "time_cost.png", dpi=180)
    figure.savefig(root.parent / "time_cost.svg")
    plt.close(figure)


def markdown(root, summary):
    lines = [
        "# 三类大请求量负载的全量离线搜索",
        "",
        "三组各 1,296 个唯一配置，合计 3,888 点全部预测成功、可部署，无缺成本、不支持、失败、超时或未运行。采用固定三份语义 base、39 类独立模型、8 个资源套餐与七项报价；请求、顺序、输出及八条准备请求均沿用原实验。ShareGPT/BurstGPT 输出上限为 16 tokens，程序生成固定 8 tokens。",
        "",
        "端点从每小时费用不超过 baseline 的配置中选择；并列时依次按较低费用、稳定名字选择，全部同耗时并列项保存在 analysis.json 和候选 CSV。相同费用上限不表示端点费用相同。以下均为模型预测，没有新增正常实测。",
        "",
        "| 负载 | baseline 秒 | 最好秒 | 最坏秒 | 最好较最坏耗时减少 | 最好较 baseline 耗时减少 |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for scene in summary["scenes"]:
        e = scene["endpoints"]
        lines.append(
            f"| {scene['scene']}（{scene['formal_requests']} 请求） | {e['baseline']['predicted_sec']:.3f} | {e['best']['predicted_sec']:.3f} | {e['worst']['predicted_sec']:.3f} | {scene['best_from_worst_time_reduction_pct']:.2f}% | {scene['best_from_baseline_time_reduction_pct']:.2f}% |"
        )
    lines += [
        "",
        "![全部候选、Pareto 与同预算耗时分布](time_cost.png)",
        "",
        "[全部候选 CSV](candidates.csv) · [端点、并列项和统计 JSON](analysis.json)。原始逐请求报告、固定输入、模型与隔离检查位于 execution/；CSV 保留完整配置、七项费用、工作量、模型覆盖标记及报告路径。",
        "",
    ]
    for scene in summary["scenes"]:
        lines += [
            f"## {scene['scene']}",
            "",
            f"费用上限 {scene['budget_hourly_units']:.3f} 单位/小时，范围内 {scene['within_budget']} 点；最好/最坏同耗时并列 {len(scene['ties']['best'])}/{len(scene['ties']['worst'])} 点，Pareto 候选 {len(scene['pareto'])} 点。",
            "",
            "| 角色 / 条件 | HTTP 秒 | 费用单位/小时 | 正式窗口费用单位 | 设备 tokens / Host 比例 / 页 tokens | 写入 / 预取 / 门槛 / timeout 秒 |",
            "| --- | ---: | ---: | ---: | --- | --- |",
        ]
        for role, row in scene["endpoints"].items():
            lines.append(
                f"| {role} / {row['name']} | {row['predicted_sec']:.3f} | {row['hourly_cost_units']:.3f} | {row['window_cost_units']:.3f} | {row['device_tokens']} / {row['host_ratio']} / {row['page_tokens']} | {row['write_policy']} / {row['prefetch_policy']} / {row['prefetch_threshold_tokens']} / {row['prefetch_timeout_sec'] if row['prefetch_timeout_sec'] is not None else '—'} |"
            )
        lines += [
            "",
            f"最好点较 baseline 减少 {scene['best_from_baseline_time_reduction_sec']:.3f} 秒；较最坏点减少 {scene['best_from_worst_time_reduction_sec']:.3f} 秒。每小时费用较 baseline 减少 {scene['best_hourly_cost_reduction_units']:.3f} 单位（{scene['best_hourly_cost_reduction_pct']:.2f}%）。窗口费用仅按正式 HTTP 时段折算；准备与后台尾部另列，不表示实际账单。",
            "",
            "| 角色 | 正式 Prefill tokens | 可复用前缀 tokens | 逻辑回载 tokens | 文件接受前缀 tokens | 轮询回合 | 全程备份/回载提交 | 全程文件读/新写完成 rank 页 |",
            "| --- | ---: | ---: | ---: | ---: | ---: | --- | --- |",
        ]
        for role, row in scene["endpoints"].items():
            lines.append(
                f"| {role} | {row['formal_prefill_tokens']} | {row['formal_reused_prefix_tokens']} | {row['formal_loaded_token_equivalents']} | {row['formal_accepted_prefix_tokens']} | {row['formal_poll_rounds']} | {row['all_backup_submissions']} / {row['all_load_submissions']} | {row['all_rank_read_pages']} / {row['all_rank_new_pages']} |"
            )
        lines += [
            "",
            "| 角色 | 准备请求跨度秒 | 准备结束至正式开始秒 | 正式请求间隔合计秒 | 正式结束后后台尾部秒 | 发起至接受跨度合计秒 | 提前停止请求数 |",
            "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
        for role, row in scene["endpoints"].items():
            lines.append(
                f"| {role} | {row['preparation_span_sec']:.3f} | {row['preparation_to_formal_interval_sec']:.3f} | {row['formal_inter_request_gap_sec']:.3f} | {row['post_formal_background_sec']:.3f} | {row['formal_prefetch_issue_accept_span_sum_sec']:.3f} | {row['formal_stopped_early_requests']} |"
            )
        distribution = scene["within_budget_time_sec"]
        lines += [
            "",
            f"同预算耗时 P05 / 中位数 / P95 为 {distribution['p05']:.3f} / {distribution['median']:.3f} / {distribution['p95']:.3f} 秒。正式失败回载尝试合计 {scene['failed_load_attempts']} 次；零次表示本轮未触发，不能认证该分支。",
            "",
        ]
    lines += [
        "## 输入复用与容量压力",
        "",
        "| 负载 | 不限容量的可复用 tokens 上限 | 页宽 32 / 64 / 128 的可复用上限 | 32 请求窗口去重 tokens 中位/最大 | 身份回访距离中位/最大（请求） |",
        "| --- | ---: | --- | --- | --- |",
    ]
    for scene in summary["scenes"]:
        pressure = scene["input_pressure"]
        window = pressure["window_32_distinct_tokens"]
        distance = pressure["identity_return_distance"]
        paged = pressure["paged_reusable_tokens"]
        lines.append(
            f"| {scene['scene']} | {pressure['unbounded_reusable_tokens']} | {paged['32']} / {paged['64']} / {paged['128']} | {window['median']} / {window['maximum']} | {distance['median']} / {distance['maximum']} |"
        )
    lines += [
        "",
        "这些是输入统计和复用潜力，不是驻留量或实际命中率。程序生成的共享家族前缀合计 36,864 tokens，超过声明的最大 Device 配额 32,768 tokens；更大的 Host 池提供回访保留空间，具体结果仍由淘汰、页对齐及时间分支决定。窗口和回访统计口径见 execution/source_qualification.json。",
        "",
        "## 工作量与差距",
        "",
        "ShareGPT 的小容量端点通过大量文件回载保持前缀复用，伴随更多轮询和文件工作。较快端点采用更粗的页宽，Prefill tokens 略多，同时减少文件回载和控制工作；不能仅按复用 tokens 判断快慢。",
        "",
        "BurstGPT 的原始前缀复用潜力为零，所有配置保持相同的 Prefill 工作，也没有正式回载。增加缓存资源的性能收益有限，较低套餐可保留接近 baseline 的预测耗时。",
        "",
        "程序生成负载的较快端点使用更大的 Host 比例，保留更多可复用前缀；较慢端点伴随容量淘汰、限时预取和较多重算。端点同时改变容量、页宽和策略，工作计数支持机制解释，不能将全部耗时差独立归给某个组件。",
        "",
        "可复用前缀包含回载后可见的数据，不能称为纯 Device 命中。逻辑回载 tokens 为页数乘页宽，便于不同页宽比较；文件 rank 页对各 rank 求和。全程计数包含准备和后台完成，正式计数只含正式请求；这些是预测程序生成的逻辑工作，未观测的物理流量与独占 I/O 等待不填零。发起至接受跨度包含查询、传输、停止及控制响应，可与其他工作重叠，不能叠加为独占等待。",
        "",
        "## 实际搜索开销",
        "",
        "| 负载 | 调用数 | 调用墙钟合计秒 | 来源准备合计秒 | 仿真合计秒 | 单配置平均仿真秒 | C++ 最大 RSS GiB |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for scene in summary["scenes"]:
        lines.append(
            f"| {scene['scene']} | {scene['batch_count']} | {scene['batch_wall_sum_sec']:.2f} | {scene['source_prepare_sum_sec']:.2f} | {scene['simulation_sum_sec']:.2f} | {scene['mean_target_simulation_sec']:.3f} | {scene['cpp_peak_rss_bytes'] / 1024**3:.3f} |"
        )
    q = summary["qualification"]
    m = q["memory"]
    assets = q["referenced_assets"]
    lines += [
        "",
        "以下是已有资产的只读引用，未复制到本轮新增输出；路径和逐文件体积保存在资格 JSON。",
        "",
        "| 既有来源 | 目录体积 GiB |",
        "| --- | ---: |",
    ]
    for base in assets["bases"]:
        lines.append(f"| {base['scene']} base | {base['bytes'] / 1024**3:.3f} |")
    lines += [
        "",
        f"45 份独立成本来源文件合计 {assets['independent_source_total_bytes'] / 1024**2:.2f} MiB，不含未被引用的采样目录其他文件。冻结模型选择 {assets['frozen_models_bytes'] / 1024**2:.2f} MiB，已计入新增输出。",
    ]
    lines += [
        "",
        f"调用墙钟可重叠，不能相加当作实际搜索历时。完整扫描墙钟为 {read(root / 'commands/scan_0.json')['wall_sec']:.2f} 秒；截至生成本报告前，构建、代表检查、扫描及复核累计执行 {summary['runtime']['wall_sec']:.2f} 秒，后续报告用量继续写入 execution/usage.json。",
        "",
        f"原生预测各限 8 GiB，最多两路；外层限 4 GiB，含页缓存的原始记账峰值 {m['outer_peak_charged_bytes_including_cache'] / 1024**3:.4f} GiB。{m['sample_count']} 次采样的匿名 RSS 最大 {m['outer_anonymous_rss_sample_max_bytes'] / 1024**3:.3f} GiB，采样区间见资格 JSON，不称完整生命周期 RSS 峰值。最终保留量见 delivery_check.json；没有连续采集瞬时磁盘峰值。",
        "",
        "## 适用范围",
        "",
        "成本查询包含允许的外推、新特征组合及来源控制/残余近似；覆盖计数是查询次数，不表示耗时比例或精度。详细适用域、系数和来源保存在 execution/selected_models.json。完整 HTTP 保留 Prefill、Decode、I/O、控制、gap 与未归属成本，没有用删项获得收益。系统文件页缓存等初始状态未完整记录，现有 buffered 文件响应不授予冷 SSD 或实际物理带宽验证资格。",
        "",
        "费用为现行七项归一化假设，按实际预留计价，共同文件服务按份额收费一次，不包括 CPU、算力租价或物理流量费。公开排名、配置及全部并列项保留于 execution/*/results.json 与候选 CSV；90% baseline 耗时上限是否有解另见 selected/selection_status，不临时放宽约束。",
        "",
        "本轮没有对应正常实测，精度留空；旧六个正常条件的 max_prefill_tokens 为 4096，本轮为 8192，声明配置未完全匹配，核对见 execution/historical_normal_compatibility.json。不推广旧六点误差、不认证细微排序或真实全局最优。旧 3×96 抽样的空间、报价与模型不同，不能作为同条件算法收益对照。本轮只覆盖声明部署上的限长、串行固定输出，不推广到完整原始输出、高并发或长期稳态。",
        "",
    ]
    (root.parent / "results.md").write_text("\n".join(lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_AS, (4 * 1024**3, 4 * 1024**3))
    root = args.input.resolve().parent
    summary, records = collect(root, read(args.input))
    plot(root, summary, records)
    markdown(root, summary)
    summary["report_process_peak_rss_bytes"] = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * 1024
    (root.parent / "analysis.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n")
    print(
        json.dumps(
            {"status": summary["status"], "candidates": len(records), "report": str(root.parent / "results.md")},
            ensure_ascii=False,
        )
    )


if __name__ == "__main__":
    main()
