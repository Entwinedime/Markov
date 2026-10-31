"""Summarize the declared search and its separate normal confirmations."""

import csv
import json
from pathlib import Path

from markov.artifacts.request import read_json
from markov.artifacts.writing import write_json
from markov.workflow.evaluation import normal_statistics


def confirmation_names(result: dict) -> list[str]:
    return list(dict.fromkeys(name for name in (result["baseline"], result["fastest"], result["selected"]) if name))


def write_report(output: Path, targets: list[dict], repetitions: int) -> dict:
    result = read_json(output / "prediction_run/search/report.json")
    measurements = {entry["name"]: entry for entry in read_json(output / "confirmation.json")["entries"]}
    predicted = {row["name"]: row for row in result["candidates"]}
    normal = {
        name: normal_statistics(measurements.get(name, {"observations": []})["observations"])
        for name in confirmation_names(result)
    }
    baseline = normal[result["baseline"]]["mean_sec"]
    confirmed = []
    for name, actual in normal.items():
        row = {
            **predicted[name],
            **actual,
            "ape_percent": None,
            "normal_relative_time_ratio": None,
            "normal_meets_relative_constraint": None,
            "errors": measurements.get(name, {"errors": []})["errors"],
        }
        if actual["mean_sec"] is not None:
            row["ape_percent"] = 100 * abs(row["predicted_sec"] - actual["mean_sec"]) / actual["mean_sec"]
            if baseline is not None:
                row["normal_relative_time_ratio"] = actual["mean_sec"] / baseline
                row["normal_meets_relative_constraint"] = actual["mean_sec"] <= baseline * result["maximum_time_ratio"]
        confirmed.append(row)
    result["confirmation"] = confirmed
    result["confirmation_complete"] = all(row["n"] == repetitions for row in confirmed)
    write_json(output / "results.json", result)

    configurations = {target["name"]: target["configuration"] for target in targets}
    records = []
    for row in result["candidates"]:
        configuration = configurations[row["name"]]
        extra = json.loads(configuration["hicache_storage_backend_extra_config"])
        records.append(
            {
                "name": row["name"],
                "status": row["status"],
                "resource_plan": row["resource_plan"],
                "hourly_cost_units": row["hourly_cost_units"],
                "predicted_sec": row["predicted_sec"],
                "device_reserved_gib_per_rank": row["device_reserved_gib_per_rank"],
                "host_reserved_gib": row["host_reserved_gib"],
                "max_total_tokens": configuration["max_total_tokens"],
                "hicache_ratio": configuration["hicache_ratio"],
                "page_size": configuration["page_size"],
                "hicache_write_policy": configuration["hicache_write_policy"],
                "hicache_storage_prefetch_policy": configuration["hicache_storage_prefetch_policy"],
                "prefetch_threshold_tokens": extra["prefetch_threshold"],
                "prefetch_timeout_sec": extra.get("prefetch_timeout_base"),
            }
        )
    with (output / "candidates.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(records[0]))
        writer.writeheader()
        writer.writerows(records)

    selection = f"选择 {result['selected']}。" if result["selected"] else "未找到满足时间约束的候选。"
    lines = [
        "# 有限资源搜索结果",
        "",
        f"候选 {result['expected']} 个，已进入排名 {result['ranked']} 个；状态 {result['status']}。",
        f"约束为默认配置预测耗时的 {result['maximum_time_ratio']:.0%}，{selection}",
        "",
        "资源档位使用声明的固定预留与原生 KV 池占用，报价为示例成本单位，不代表真实支出。",
        "",
        "| 资源档位 | 主机配额 GiB | 成本单位/小时 | 该档最快预测秒 | 条件 |",
        "| --- | ---: | ---: | ---: | --- |",
    ]
    for plan in result["resource_plans"]:
        supported = [
            row for row in result["candidates"] if row["status"] == "ranked" and row["resource_plan"] == plan["name"]
        ]
        best = min(supported, key=lambda row: (row["predicted_sec"], row["name"])) if supported else None
        time = f"{best['predicted_sec']:.4f}" if best else "无候选"
        name = best["name"] if best else "—"
        lines.append(f"| {plan['name']} | {plan['host_gib']} | {plan['hourly_cost_units']} | {time} | {name} |")

    records_by_name = {row["name"]: row for row in records}
    lines += [
        "",
        "| 确认角色（条件） | 设备 KV tokens | 主机比例 | 页 tokens | 写策略 | 预取策略 / 门槛 tokens / 超时秒 |",
        "| --- | ---: | ---: | ---: | --- | --- |",
    ]
    for row in confirmed:
        name = row["name"]
        configuration = records_by_name[name]
        roles = "、".join(
            label
            for label, key in (("默认", "baseline"), ("预测最快", "fastest"), ("约束下最低成本", "selected"))
            if result[key] == name
        )
        timeout = configuration["prefetch_timeout_sec"]
        prefetch = f"{configuration['hicache_storage_prefetch_policy']} / {configuration['prefetch_threshold_tokens']} / {timeout if timeout is not None else '—'}"
        lines.append(
            f"| {roles}（{name}） | {configuration['max_total_tokens']} | {configuration['hicache_ratio']} | {configuration['page_size']} | {configuration['hicache_write_policy']} | {prefetch} |"
        )
    lines += [
        "",
        "| 正常确认条件 | 预测秒 | 正常均值秒 | 重复数 | APE (%) | 相对默认耗时 | 实测满足约束 |",
        "| --- | ---: | ---: | ---: | ---: | ---: | --- |",
    ]
    for row in confirmed:
        observed = f"{row['mean_sec']:.4f}" if row["mean_sec"] is not None else "缺失"
        error = f"{row['ape_percent']:.3f}" if row["ape_percent"] is not None else "缺失"
        ratio = f"{row['normal_relative_time_ratio']:.1%}" if row["normal_relative_time_ratio"] is not None else "缺失"
        meets = (
            "缺失"
            if row["normal_meets_relative_constraint"] is None
            else ("是" if row["normal_meets_relative_constraint"] else "否")
        )
        lines.append(
            f"| {row['name']} | {row['predicted_sec']:.4f} | {observed} | {row['n']} | {error} | {ratio} | {meets} |"
        )
    lines += [
        "",
        "仅确认事先规定的默认、预测最快和时间约束下最低成本条件，重合时只采一次；没有全空间正常真值，不声称真实全局最优。",
        "正常结果不用于补参数或重新选点。完整候选、Pareto、失败、配额与全部重复样本保存在 results.json。",
        "",
    ]
    (output / "results.md").write_text("\n".join(lines), encoding="utf-8")
    return result
