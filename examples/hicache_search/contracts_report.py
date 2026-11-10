"""Report complete contract searches and their separately measured endpoints."""

from __future__ import annotations

from collections import Counter
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
from statistics import median
import time

from markov.artifacts.request import read_json
from markov.artifacts.writing import write_json

from full_scan_report import phases, write_csv


def package_name(configuration: dict, packages: list[dict]) -> str:
    extra = json.loads(configuration["hicache_storage_backend_extra_config"])
    for package in packages:
        values = package["configuration"]
        if all(
            configuration[key] == value
            for key, value in values.items()
            if key != "hicache_storage_backend_extra_config"
        ) and all(
            extra[key] == value for key, value in json.loads(values["hicache_storage_backend_extra_config"]).items()
        ):
            return package["name"]
    raise ValueError("A reported configuration does not belong to the declared resource packages")


def observed_work(path: Path) -> dict:
    rows = read_json(path.parent / read_json(path)["workload_report"])["requests"]
    formal = [row for row in rows if row.get("measure")]
    preparation = [row for row in rows if row["kind"] == "request" and not row["measure"]]
    cached = sum(row["response"]["meta_info"]["cached_tokens"] for row in formal)
    return {
        "formal_requests": len(formal),
        "input_tokens": sum(row["input_tokens"] for row in formal),
        "reported_cached_tokens": cached,
        "input_minus_reported_cache_tokens": sum(row["input_tokens"] for row in formal) - cached,
        "requests_reporting_cache_hits": sum(row["response"]["meta_info"]["cached_tokens"] > 0 for row in formal),
        "generated_tokens": sum(row["generated_tokens"] for row in formal),
        "preparation_span_sec": (float(preparation[-1]["end_time_ms"]) - float(preparation[0]["start_time_ms"])) / 1000,
        "preparation_to_formal_sec": (float(formal[0]["start_time_ms"]) - float(preparation[-1]["end_time_ms"])) / 1000,
        "qualification": "Server-reported logical cache tokens; not instrumented component timing or physical traffic.",
    }


def figures(output: Path, scene: str, rows: list[dict], search: dict, endpoints: dict) -> dict:
    os.environ.setdefault("MPLCONFIGDIR", str(output / "report_cache"))
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    plt.rcParams.update({"font.family": ["Noto Sans CJK SC", "DejaVu Sans"], "axes.unicode_minus": False})
    budget = next(row["hourly_cost_units"] for row in rows if row["name"] == search["baseline"])
    figure, axis = plt.subplots(figsize=(10, 5.5))
    values = [row["predicted_sec"] / 60 for row in rows]
    _, bins, _ = axis.hist(values, bins=28, color="#cbd2db", label=f"全部声明配置（{len(rows)}）")
    axis.hist(
        [row["predicted_sec"] / 60 for row in rows if row["within_budget"]],
        bins=bins,
        color="#426789",
        alpha=0.85,
        label="其中：费用不超过基准",
    )
    for role, label, color in (("best", "同预算预测最好", "#16816a"), ("worst", "同预算预测最差", "#bd4444")):
        axis.axvline(endpoints[role]["predicted_sec"] / 60, color=color, linestyle="--", label=label)
    axis.set(xlabel="预测完整正式 HTTP 耗时（分钟）", ylabel="配置数量", title="CUAD 长合同交替审阅：完整配置耗时分布")
    axis.legend(fontsize=9)
    axis.grid(axis="y", alpha=0.2)
    figure.tight_layout()
    distribution = f"{scene}_time_distribution"
    for suffix in ("png", "svg"):
        figure.savefig(output / f"{distribution}.{suffix}", dpi=180)
    plt.close(figure)

    figure, axis = plt.subplots(figsize=(11, 7))
    for package in dict.fromkeys(row["package"] for row in rows):
        group = [row for row in rows if row["package"] == package]
        axis.scatter(
            [row["hourly_cost_units"] for row in group],
            [row["predicted_sec"] / 60 for row in group],
            s=15,
            alpha=0.45,
            label=package,
        )
    frontier = sorted(
        {(row["hourly_cost_units"], row["predicted_sec"] / 60) for row in rows if row["name"] in search["pareto"]}
    )
    axis.plot(*zip(*frontier), color="#666666", linewidth=1, label="预测 Pareto 边界", zorder=3)
    for role, label, marker, color in (("best", "预测最好点", "^", "#16816a"), ("worst", "预测最差点", "X", "#bd4444")):
        point = endpoints[role]
        predicted, actual = point["predicted_sec"] / 60, point["normal"]["mean_sec"] / 60
        axis.plot([point["hourly_cost_units"]] * 2, [predicted, actual], linestyle=":", color=color)
        axis.scatter(point["hourly_cost_units"], predicted, marker=marker, s=100, color=color, label=label, zorder=5)
        axis.scatter(
            point["hourly_cost_units"],
            actual,
            marker=marker,
            s=130,
            facecolors="none",
            edgecolors=color,
            linewidths=2,
            label=label + "的实测",
            zorder=5,
        )
    axis.axvline(budget, linestyle="--", color="#333333", linewidth=1, label="基准小时费用上限")
    axis.set(
        xlabel="归一化资源费用（单位/小时）",
        ylabel="完整正式 HTTP 耗时（分钟）",
        title="容量、四向带宽与执行策略的联合搜索",
    )
    axis.grid(alpha=0.15)
    axis.legend(fontsize=8, ncol=3, loc="upper center", bbox_to_anchor=(0.5, -0.13))
    figure.tight_layout()
    cost_time = f"{scene}_cost_time"
    for suffix in ("png", "svg"):
        figure.savefig(output / f"{cost_time}.{suffix}", dpi=180)
    plt.close(figure)
    return {"distribution": f"{distribution}.png", "cost_time": f"{cost_time}.png"}


def scene_markdown(scene: str, result: dict, search: dict, settings: dict, summary: dict) -> list[str]:
    data = result["scenes"][scene]
    endpoints = {role: data[role] for role in ("best", "worst")}
    best, worst = endpoints["best"], endpoints["worst"]
    actual_best, actual_worst = best["normal"]["mean_sec"], worst["normal"]["mean_sec"]
    improvement, speedup, wape = data["actual_improvement_percent"], data["actual_speedup"], data["wape_percent"]
    budget = data["baseline"]["hourly_cost_units"]
    ties, images, packages, runtime = data["ties"], data["images"], data["package_statuses"], data["runtime"]
    baseline = data["baseline"]
    lines = []
    info = summary["scenes"][scene]
    labels = {"best": "预测最好点", "worst": "预测最差点"}
    lines += [
        f"## {scene}：结果",
        "",
        f"{info['contracts']} 份原始合同、{info['requests']} 个不同问答，完整扫描 {search['expected']:,} 个配置。两点各正常测量一次：预测最好点为 {actual_best:.3f} 秒，预测最差点为 {actual_worst:.3f} 秒，实际耗时减少 **{improvement:.2f}%**，加速比 **{speedup:.3f}**；两点 WAPE **{wape:.3f}%**。",
        "完整正式 HTTP 保留 Prefill、Decode、HiCache I/O/control、CPU 事件间空隙（gap）、未归属成本及正式长规格首次准备。启动、八条准备请求和排空另计。",
        "单点绝对百分比误差（APE）为 |预测－实测| / 实测；两点 WAPE 为绝对误差之和 / 正常耗时之和。",
        "",
        "| 预先选定角色 | 条件 | 预测秒 | 正常秒 | APE | 小时费用单位 | 正常正式窗口费用单位 |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for role, point in endpoints.items():
        lines.append(
            f"| {labels[role]} | {point['target']} | {point['predicted_sec']:.3f} | {point['normal']['mean_sec']:.3f} | {point['ape_percent']:.3f}% | {point['hourly_cost_units']:.3f} | {point['normal_window_cost_units']:.3f} |"
        )
    lines += [
        "",
        f"两点 APE 是否均低于 10%：{'是' if result['scenes'][scene]['accuracy_under_ten_percent'] else '否，推荐精度尚不可靠'}；实测排序是否一致：{'是' if actual_best <= actual_worst else '否，保留反序结果'}。预测耗时差为 {result['scenes'][scene]['predicted_improvement_percent']:.2f}%，实际带符号差值为 {actual_worst - actual_best:.3f} 秒。",
        f"共同小时预算为 {budget:.3f} 单位，范围内 {result['scenes'][scene]['within_budget']:,} 点；其余配置超过费用上限，不是部署失败。最好与最差的费用并不相等。基准只做了带采集运行，其预测 {baseline['predicted_sec']:.3f} 秒单独列示，不能据此报告相对正常默认配置的提升。",
        f"原搜索入口保留的 90% 基准耗时门槛为 {search['maximum_time_sec']:.3f} 秒，满足该门槛的候选为 {search['selected'] or '无'}。本轮端点按同预算的最小/最大耗时选择，不通过放宽该门槛制造推荐。",
        "",
        f"![完整配置耗时分布]({images['distribution']})",
        "",
        f"![资源费用与完整耗时]({images['cost_time']})",
        "",
        "### 输入与覆盖",
        "",
        f"来自 CUAD 原始全文和问题；固定种子 {summary['seed']}，不裁正文、不重复问题。顺序与已保存 workload 一致，回访距离中位数为 {info['revisit_distance_median']:g} 个请求。输入 {info['input_tokens_min']:,}–{info['input_tokens_max']:,} tokens，正式合计 {info['input_tokens_total']:,}；正文工作集 {info['body_tokens_working_set']:,}，去重前缀树 {info['distinct_prefix_tree_tokens']:,} tokens。不限容量时可复用 {info['unbounded_cache_reusable_tokens']:,} tokens，仅表示潜力。",
        f"{info['questions_with_answer']} 个问题有答案，共 {info['reference_answers']} 条参考答案，其中 {info['reference_answers_over_32_tokens']} 条超过本轮 32-token 输出上限。串行、ignore_eos、关闭 thinking，固定生成 32 tokens；这不是问答质量或生产并发评价。每次另有八条互不重合的准备输入，各生成 8 tokens，之后排空。",
        "",
        "| 资源套餐 | 候选数 | 分类 |",
        "| --- | ---: | --- |",
    ]
    for package, counts in packages.items():
        lines.append(f"| {package} | {sum(counts.values())} | {counts.get('ranked', 0)} 个已完成预测和排名 |")
    lines += [
        "",
        "全部声明配置均成功；缺成本、不支持、失败、超时及未运行均为 0。八个套餐各与 162 种策略组合；套餐内部七项资源不是任意笛卡尔积。",
        "",
        "### 两个端点的完整配置",
        "",
        "| 项目 | 预测最好点 | 预测最差点 |",
        "| --- | --- | --- |",
    ]
    for label, key in (
        ("资源套餐", "package"),
        ("Device KV tokens", "device_tokens"),
        ("Host/Device 比例", "host_ratio"),
        ("页大小 tokens", "page_tokens"),
        ("写策略", "write_policy"),
        ("预取策略", "prefetch_policy"),
        ("门槛 tokens", "threshold_tokens"),
        ("timeout 秒", "timeout_sec"),
        ("Device KV GiB（TP 合计）", "device_kv_gib"),
        ("Host KV GiB（TP 合计）", "host_kv_gib"),
        ("SSD 配额 GiB（TP 合计）", "storage_gib"),
    ):
        values = [point["work"][key] if point["work"][key] is not None else "—" for point in (best, worst)]
        lines.append(f"| {label} | {values[0]} | {values[1]} |")
    lines += ["", "| 费用分项（单位/小时） | 预测最好点 | 预测最差点 |", "| --- | ---: | ---: |"]
    charges = {
        "device_storage": "Device 容量",
        "host_storage": "Host 容量",
        "storage_capacity": "SSD 容量",
        "device_to_host_bandwidth": "Device → Host 带宽",
        "host_to_device_bandwidth": "Host → Device 带宽",
        "host_to_storage_bandwidth": "Host → SSD 带宽",
        "storage_to_host_bandwidth": "SSD → Host 带宽",
    }
    for key in best["resources"]["hourly_cost_breakdown"]:
        lines.append(
            f"| {charges[key]} | {best['resources']['hourly_cost_breakdown'][key]:.6f} | {worst['resources']['hourly_cost_breakdown'][key]:.6f} |"
        )
    lines += ["", "| 预留带宽（GiB/s，整组） | 预测最好点 | 预测最差点 |", "| --- | ---: | ---: |"]
    directions = {
        "device_to_host": "Device → Host",
        "host_to_device": "Host → Device",
        "host_to_storage": "Host → SSD",
        "storage_to_host": "SSD → Host",
        "storage_shared": "文件共同服务上限",
    }
    for key in best["resources"]["bandwidth_gib_per_sec"]:
        lines.append(
            f"| {directions[key]} | {best['resources']['bandwidth_gib_per_sec'][key]:.6f} | {worst['resources']['bandwidth_gib_per_sec'][key]:.6f} |"
        )
    lines += [
        "",
        f"容量按实际预留、带宽按配置配额计费；Device 固定预留每 rank {search['fixed_device_gib_per_rank']:g} GiB，Host 固定预留整组 {search['fixed_host_gib']:g} GiB，另外加原生 KV 池。七项归一化单价保留在 results.json 的 pricing；共同文件服务按份额收费一次，不计 CPU、算力租价或流量费，不是已验证的市场账单。",
        f"最好同耗时/同费用并列 {len(ties['best'])} 项：{', '.join(ties['best'])}。最差并列 {len(ties['worst'])} 项：{', '.join(ties['worst'])}。按预定的费用、名字顺序选点，全部并列保留，不能称唯一最优。",
        "",
        "### 差距从哪里来",
        "",
        "| 工作量或正常响应记录 | 预测最好点 | 预测最差点 |",
        "| --- | ---: | ---: |",
    ]
    for label, section, key in (
        ("预测正式 Prefill tokens", "work", "formal_prefill_tokens"),
        ("预测回载 tokens", "work", "formal_loaded_tokens"),
        ("预测提前停止预取请求", "work", "formal_stopped_early_requests"),
        ("正常响应报告缓存 tokens", "observed_work", "reported_cached_tokens"),
        ("正常响应报告命中请求", "observed_work", "requests_reporting_cache_hits"),
        ("输入扣除正常报告缓存 tokens", "observed_work", "input_minus_reported_cache_tokens"),
    ):
        lines.append(f"| {label} | {best[section][key]:,} | {worst[section][key]:,} |")
    lines += [
        "",
        "较快端点优先保障回载带宽，并等待回载完成；较慢端点优先备份、回载配额较低，采用限时预取，许多请求停止等待后重算正文。正常响应报告的命中量与预测工作量可对照，但未采正常组件 trace，不能把全部时间差独立归因于 I/O；页宽、写入和等待策略同时改变，仍是联合配置效果。逻辑回载按页乘页宽，包含回载后可见的前缀，不能当纯 Device 初始命中或实测物理流量。",
        f"共享模型选择含 {result['independent_model_classes']} 类独立响应，本次执行报告实际引用 {len(data['model_sources'])} 个来源。下面统计成本查询次数，不是耗时占比；适用域及来源保留在共享模型文件，不能推论未见模型、平台、并发或任意长度也准确。",
        "",
        "| 成本来源与适用范围标记 | 预测最好点查询数 | 预测最差点查询数 |",
        "| --- | ---: | ---: |",
    ]
    for label, key in (
        ("独立数据已有工作点", "measured_work_point"),
        ("允许的特征外推", "extrapolated"),
        ("新特征组合", "new_feature_combination"),
        ("源控制残余近似", "source_control_residual_approximation"),
        ("源其他残余近似", "source_residual_approximation"),
    ):
        lines.append(f"| {label} | {best['coverage'].get(key, 0):,} | {worst['coverage'].get(key, 0):,} |")
    lines += [
        "",
        "### 预测与正常运行的开销",
        "",
        f"{len(runtime['source_sessions'])} 份 CPU 会话共享唯一 base，各整理一次；来源准备合计 {runtime['source_prepare_sec_sum']:.3f} 秒。全部目标仿真合计 {runtime['simulation_sec_sum']:.3f} 秒，平均 {runtime['simulation_mean_sec']:.3f}、中位 {runtime['simulation_median_sec']:.3f} 秒/配置。核心自报最大 RSS 为 {runtime['core_peak_rss_bytes'] / 1024**3:.3f} GiB，各预测容器限 {settings['prediction_memory_limit_gib']} GiB，最多两路；这不是整个宿主的连续内存峰值。正式汇总报告 {runtime['prediction_report_bytes'] / 1024**2:.2f} MiB。",
        "两次来源准备的 CPU 时间不能相加当作并行墙钟，也不能把最终一次扫描当作整轮实验的全部用时。",
        "",
    ]
    if "wall_observed_upper_sec" in runtime:
        lines += [
            f"完整扫描至已观察终态约 {runtime['wall_observed_upper_sec'] / 3600:.3f} 小时，含观察余量；原两小时目标{'达到' if runtime['wall_observed_upper_sec'] <= 7200 else '未达到'}。各进程调用墙钟为 {' / '.join(f'{value:.3f}' for value in runtime['worker_workflow_sec'])} 秒，可相互重叠；精确外围结束时刻未单独埋点。",
            "",
        ]
    lines += [
        "| 正常角色 | 完整运行秒 | 正式 HTTP 秒 | 准备请求跨度秒 | 准备至正式间隔秒 | 其余启动/导出/清理秒 |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for role, point in endpoints.items():
        state = read_json(Path(point["capture_state"]))
        wall = sum(attempt["wall_sec"] for attempt in state["attempts"]) + state.get("processing_wall_sec", 0)
        work = point["observed_work"]
        other = wall - point["normal"]["mean_sec"] - work["preparation_span_sec"] - work["preparation_to_formal_sec"]
        lines.append(
            f"| {labels[role]} | {wall:.3f} | {point['normal']['mean_sec']:.3f} | {work['preparation_span_sec']:.3f} | {work['preparation_to_formal_sec']:.3f} | {other:.3f} |"
        )
    lines += [""]
    return lines


def write_report(
    output: Path, settings: dict, workloads: Path, models: Path, scenes: list[str], targets: list[dict]
) -> dict:
    normal = read_json(output / "confirmation/results.json")
    if normal["status"] != "completed":
        raise ValueError("Complete the fixed normal observations before writing the final contract report")
    summary = read_json(workloads / "summary.json")
    result = {
        "status": "completed",
        "scenes": {},
        "models": str(models),
        "deployment": settings["deployment"],
        "independent_model_classes": len(read_json(models)["models"]),
    }
    records, lines, ledgers = [], ["# CUAD 长合同前缀缓存搜索", ""], []
    for scene in scenes:
        directory = output / "scenes" / scene / "run"
        search = read_json(directory / "search/report.json")
        prediction = read_json(directory / "prediction/report.json")
        result["pricing"] = search["pricing"]
        if prediction["models"] != str(models) or prediction["deployment"] != settings["deployment"]:
            raise ValueError("Report inputs must identify the deployment and models used for prediction")
        if search["status"] != "completed" or {cell["name"] for cell in prediction["cells"]} != {
            target["name"] for target in targets
        }:
            raise ValueError("The final report requires complete declared prediction coverage")
        ranked = {row["name"]: row for row in search["candidates"]}
        baseline = ranked[search["baseline"]]
        budget = baseline["hourly_cost_units"]
        endpoints = {
            role: next(row for row in normal["rows"] if row["scene"] == scene and role in row["roles"])
            for role in ("best", "worst")
        }
        rows, packages = [], {}
        for cell in prediction["cells"]:
            row, configuration = ranked[cell["name"]], cell["configuration"]
            if cell["status"] != "predicted" or cell["cost_gaps"] or cell["formal_http_sec"] != row["predicted_sec"]:
                raise ValueError("Unresolved prediction or mismatched ranking in the declared space")
            if not math.isclose(sum(row["hourly_cost_breakdown"].values()), row["hourly_cost_units"], rel_tol=1e-12):
                raise ValueError("Resource fee components do not sum to the declared hourly fee")
            package = package_name(configuration, settings["resource_packages"])
            packages.setdefault(package, Counter())[row["status"]] += 1
            formal = [request for request in cell["requests"] if request["formal"]]
            extra = json.loads(configuration["hicache_storage_backend_extra_config"])
            record = {
                "scene": scene,
                "name": cell["name"],
                "package": package,
                "status": row["status"],
                "within_budget": row["hourly_cost_units"] <= budget,
                "predicted_sec": row["predicted_sec"],
                "hourly_cost_units": row["hourly_cost_units"],
                "window_cost_units": row["window_cost_units"],
                "page_tokens": configuration["page_size"],
                "device_tokens": configuration["max_total_tokens"],
                "host_ratio": configuration["hicache_ratio"],
                "write_policy": configuration["hicache_write_policy"],
                "prefetch_policy": configuration["hicache_storage_prefetch_policy"],
                "threshold_tokens": extra["prefetch_threshold"],
                "timeout_sec": extra.get("prefetch_timeout_base"),
                "device_kv_gib": row["device_kv_gib"],
                "host_kv_gib": row["host_kv_gib"],
                "storage_gib": row["storage_reserved_gib"],
                **{"cost_" + key: value for key, value in row["hourly_cost_breakdown"].items()},
                **{"bandwidth_" + key + "_gibps": value for key, value in row["bandwidth_gib_per_sec"].items()},
                "formal_prefill_tokens": sum(request["prefill_tokens"] for request in formal),
                "formal_loaded_tokens": sum(request["loaded_pages"] for request in formal) * configuration["page_size"],
                "formal_reused_prefix_tokens": sum(request["device_hit_pages"] for request in formal)
                * configuration["page_size"],
                "formal_stopped_early_requests": sum(request["prefetch"]["stopped_early"] for request in formal),
                "formal_poll_rounds": sum(request["prefetch"]["poll_rounds"] for request in formal),
                "simulation_sec": cell["simulation_sec"],
            }
            rows.append(record)
            for endpoint in endpoints.values():
                if endpoint["target"] != cell["name"]:
                    continue
                if endpoint["normal"]["n"] != 1:
                    raise ValueError("Contract confirmation declares one observation per fixed endpoint")
                endpoint.update(
                    configuration=configuration,
                    resources=row,
                    work=record,
                    phases=phases(cell),
                    coverage=cell["coverage"],
                    storage=cell["storage"],
                )
                endpoint["observed_work"] = observed_work(Path(endpoint["observations"][0]))
                ledgers.append(Path(endpoint["capture_state"]))
        best, worst = endpoints["best"], endpoints["worst"]
        actual_best, actual_worst = best["normal"]["mean_sec"], worst["normal"]["mean_sec"]
        improvement, speedup = 100 * (1 - actual_best / actual_worst), actual_worst / actual_best
        wape = (
            100
            * sum(abs(point["predicted_sec"] - point["normal"]["mean_sec"]) for point in endpoints.values())
            / (actual_best + actual_worst)
        )
        ties = {
            role: sorted(
                row["name"]
                for row in rows
                if row["within_budget"]
                and row["predicted_sec"] == endpoint["predicted_sec"]
                and row["hourly_cost_units"] == endpoint["hourly_cost_units"]
            )
            for role, endpoint in endpoints.items()
        }
        images = figures(output, scene, rows, search, endpoints)
        runtime = {
            "source_prepare_sec_sum": prediction["source_prepare_sec"],
            "source_records_read_sum": prediction["source_records_read"],
            "simulation_sec_sum": sum(row["simulation_sec"] for row in rows),
            "simulation_mean_sec": sum(row["simulation_sec"] for row in rows) / len(rows),
            "simulation_median_sec": median(row["simulation_sec"] for row in rows),
            "core_peak_rss_bytes": prediction["process_peak_rss_bytes"],
            "prediction_report_bytes": (directory / "prediction/report.json").stat().st_size,
            "source_sessions": prediction["source_sessions"],
        }
        lifecycle = output / "preflight/prediction_lifecycle.json"
        if lifecycle.exists():
            lifecycle = read_json(lifecycle)
            first_start = min(
                read_json(Path(path).parent.parent / "cpu_process.json")["started_at_sec"]
                for path in runtime["source_sessions"]
            )
            observed_end = datetime.fromisoformat(lifecycle["terminal_observed_utc"].replace("Z", "+00:00"))
            runtime["wall_observed_upper_sec"] = observed_end.timestamp() - first_start
            runtime["worker_workflow_sec"] = [worker["workflow_sec"] for worker in lifecycle["workers"]]
        result["scenes"][scene] = {
            "input": summary["scenes"][scene],
            "expected": search["expected"],
            "statuses": search["statuses"],
            "package_statuses": packages,
            "within_budget": sum(row["within_budget"] for row in rows),
            "baseline": baseline,
            **endpoints,
            "ties": ties,
            "pareto": search["pareto"],
            "actual_improvement_percent": improvement,
            "actual_speedup": speedup,
            "actual_difference_sec": actual_worst - actual_best,
            "wape_percent": wape,
            "ranking_consistent": actual_best <= actual_worst,
            "predicted_improvement_percent": 100 * (1 - best["predicted_sec"] / worst["predicted_sec"]),
            "accuracy_under_ten_percent": all(point["ape_percent"] < 10 for point in endpoints.values()),
            "selection_status": search["selection_status"],
            "maximum_time_sec": search["maximum_time_sec"],
            "runtime": runtime,
            "images": images,
            "model_sources": prediction["model_sources"],
            "cost_assumptions": prediction["cost_assumptions"],
            "prediction_limitations": prediction["limitations"],
            "prediction_report": str(directory / "prediction/report.json"),
            "search_report": str(directory / "search/report.json"),
        }
        records.extend(rows)
        ledgers.append(directory / "base/capture_state.json")
        lines += scene_markdown(scene, result, search, settings, summary)
        del prediction

    calibration = output / "shared_cost/sampling/capture_state.json"
    if calibration.exists():
        ledgers.insert(0, calibration)
    usage, stages = Counter(), []
    for path in dict.fromkeys(ledgers):
        state = read_json(path)
        for attempt in state["attempts"]:
            usage["starts"] += 1
            for key in ("requests", "input_tokens", "output_tokens"):
                usage[key] += attempt[key]
        stages.append(
            {
                "ledger": str(path),
                "wall_sec": sum(a["wall_sec"] for a in state["attempts"]) + state.get("processing_wall_sec", 0),
                "failed_attempts": sum(a["status"] == "failed" for a in state["attempts"]),
                "attempts": state["attempts"],
            }
        )
    stages.sort(key=lambda stage: stage["attempts"][0]["started_at"])
    result["device_usage"], result["device_stages"] = dict(usage), stages
    lines += ["| 设备阶段账本 | 启动数 | 运行及后处理秒 | 失败尝试数 |", "| --- | ---: | ---: | ---: |"]
    for stage in stages:
        lines.append(
            f"| {Path(stage['ledger']).relative_to(output)} | {len(stage['attempts'])} | {stage['wall_sec']:.3f} | {stage['failed_attempts']} |"
        )
    lines += [
        "",
        f"设备累计 {usage['starts']} 次启动、{usage['requests']:,} 请求、{usage['input_tokens']:,} 输入和 {usage['output_tokens']:,} 输出 tokens。共享独立采样在 base/target 真值前完成，所有候选复用同一选择，关闭逐格补采；没有按端点误差追加校准。",
        "base 的原容器在正式 HTTP 完成后导出超时，退出码 124 和全部用量保留；随后只复用记录进行 CPU 离线导出恢复，没有新增设备请求。之前的 CPU 失败/停止与宿主导入失败记录也保留，其精确耗时未全部单列，整轮历时不重置。",
        "",
    ]
    for name in (
        "base_qualification",
        "calibration_coverage",
        "prediction_lifecycle",
        "storage_observations",
        "normal_qualification",
        "normal_launches",
        "experiment_timing",
    ):
        path = output / "preflight" / f"{name}.json"
        if path.exists():
            result[name] = json.loads(path.read_text())
    if "base_qualification" in result:
        base = result["base_qualification"]
        lines += [
            f"原 base 为语义采集，正式带采集 HTTP 为 {base['formal_http_observed_sec']:.3f} 秒，保留 {base['retained_bytes'] / 1024**3:.3f} GiB；其轻量来源资格不代表完整低层设备因果回放已验证，也不能将该 HTTP 当正常真值。",
            "",
        ]
    if "experiment_timing" in result:
        timing = result["experiment_timing"]
        timing["report_observed_at_unix_sec"] = time.time()
        timing["elapsed_sec"] = timing["report_observed_at_unix_sec"] - timing["started_at_unix_sec"]
        timing["under_twelve_hour_target_at_report"] = timing["elapsed_sec"] <= timing["target_wall_sec"]
        lines += [
            f"整轮自 {datetime.fromtimestamp(timing['started_at_unix_sec'], timezone.utc).isoformat()} 起计；本报告生成时已用 {timing['elapsed_sec'] / 3600:.2f} 小时，包含实现、校准、采集、失败恢复、扫描和报告。最终检查与推送仍需计入，不能把此快照当最终关闭时刻。",
            "",
        ]
    if "storage_observations" in result:
        disk = result["storage_observations"]
        lines += [
            f"本轮目录运行中最大观测 {max(row['allocated_bytes'] for row in disk) / 1024**3:.3f} GiB，包含私有 KV；两个正常任务导出后已清理私有 KV，最近保留量 {disk[-1]['allocated_bytes'] / 1024**3:.3f} GiB。采样时间保留在 JSON；未连续测量磁盘高水位，观测只能作为峰值下界。",
            "",
        ]
    lines += [
        "## 能说明什么，以及仍不能说明什么",
        "",
        "这轮报告原始长合同问答集合与声明空间中，预测预先选定的两个合法配置具有怎样的正常耗时差距；两点均用完整正式 HTTP 评分。没有关闭前缀缓存，也没有正常 base 对照，因此表中的差距不是开缓存相对关缓存、也不是相对默认配置的实测收益。",
        "每个端点只有一次正常观察，未测全空间真值，不提供置信区间、显著性、真实全局最优或长期稳态保证。八条短准备请求不能消除所有长规格首次准备；操作系统文件页缓存和驱动状态没有完全清空。带宽为允许一次原生操作突发的软件传输准入配额，不是更换 PCIe/SSD 硬件；独立 buffered 数据通路参考不代表冷 SSD 规格。",
        f"本次实际运行场景为 {', '.join(scenes)}；未运行的已准备输入不能计为泛化验证，几种访问顺序也不代表多个数据集。成本文件、原始输入、base manifest、全部候选与两点原始正常记录均保留；不读 target trace、不按正常残差补参数。",
        "",
        "[全部候选 CSV](candidates.csv) · [完整统计与证据 JSON](results.json) · [预先确定的两个端点](confirmation_plan.json)。图同时提供 PNG 和 SVG，适合后续汇报使用。",
        "",
    ]
    result["qualification"] = (
        "Complete declared package/policy predictions and fixed single normal endpoints. Full HTTP; no target trace, refitting, repeat selection, significance, normal baseline benefit or real global-optimum claim."
    )
    write_csv(output / "candidates.csv", records)
    write_json(output / "results.json", result)
    (output / "results.md").write_text("\n".join(lines), encoding="utf-8")
    return result
