"use strict";

const $ = (id) => document.getElementById(id);
const pretty = (value) => JSON.stringify(value, null, 2);
const labels = {
    starting: "启动中",
    running: "运行中",
    stopping: "正在停止并回收资源",
    completed: "已完成",
    incomplete: "有未完成项",
    failed: "失败",
    stopped: "已停止",
    interrupted: "进程已结束，待恢复",
    preparing_base: "准备 base",
    selecting_costs: "选择成本模型",
    predicting: "预测",
    preparing_costs: "准备独立成本",
    capture_slot: "等待串行采集名额",
    idle_devices: "等待空闲设备",
};
let token,
    current,
    targetRows = [],
    selected,
    uploadedWorkload,
    timer,
    detailSignature;

function message(text, error = false) {
    $("message").textContent = text;
    $("message").className = error ? "error" : "";
}

async function api(path, body) {
    const response = await fetch(
        path,
        body === undefined
            ? {}
            : {
                  method: "POST",
                  headers: {
                      "Content-Type": "application/json",
                      "X-Markov-Token": token,
                  },
                  body: pretty(body),
              },
    );
    const value = await response.json();
    if (!response.ok) throw new Error(value.error || response.statusText);
    return value;
}

function button(text, action, secondary = true) {
    const node = document.createElement("button");
    node.textContent = text;
    if (secondary) node.className = "secondary";
    node.onclick = () => safely(action, node);
    return node;
}

async function safely(action, node) {
    if (node) node.disabled = true;
    try {
        await action();
    } catch (error) {
        message(error.message, true);
    } finally {
        if (node) node.disabled = false;
    }
}

function rawDetails(parent, title, value) {
    const detail = document.createElement("details"),
        summary = document.createElement("summary"),
        pre = document.createElement("pre");
    summary.textContent = title;
    pre.textContent = typeof value === "string" ? value : pretty(value);
    detail.append(summary, pre);
    parent.append(detail);
}

function text(parent, tag, value, className = "") {
    const node = document.createElement(tag);
    node.textContent = value;
    node.className = className;
    parent.append(node);
    return node;
}

function showTargets() {
    $("targets").replaceChildren();
    targetRows.forEach((row, index) => {
        const tr = document.createElement("tr");
        const fields = [
            ["name", row.name, (value) => (row.name = value), "text"],
            [
                "TP",
                row.deployment?.tensor_parallel ??
                    current.deployment.tensor_parallel,
                (value) =>
                    (row.deployment = { tensor_parallel: Number(value) }),
                "number",
            ],
            [
                "设备 token 池",
                row.configuration.max_total_tokens ?? "",
                (value) => set("max_total_tokens", value),
                "number",
            ],
            [
                "主存倍数",
                row.configuration.hicache_ratio ?? "",
                (value) => set("hicache_ratio", value),
                "number",
            ],
            [
                "页 tokens",
                row.configuration.page_size ?? "",
                (value) => set("page_size", value),
                "number",
            ],
            [
                "写策略",
                row.configuration.hicache_write_policy ?? "",
                (value) => {
                    if (value) row.configuration.hicache_write_policy = value;
                    else delete row.configuration.hicache_write_policy;
                },
                "text",
            ],
        ];
        function set(name, value) {
            if (value === "") delete row.configuration[name];
            else row.configuration[name] = Number(value);
        }
        fields.forEach(([name, value, update, type]) => {
            const cell = document.createElement("td"),
                input = document.createElement("input");
            input.type = type;
            if (type === "number") input.min = "0.001";
            input.value = value;
            input.setAttribute("aria-label", `目标 ${index + 1} ${name}`);
            input.oninput = () => {
                update(input.value);
                $("target-json").value = pretty(targetRows);
            };
            cell.append(input);
            tr.append(cell);
        });
        const cell = document.createElement("td");
        cell.append(
            button("移除", () => {
                targetRows.splice(index, 1);
                showTargets();
            }),
        );
        tr.append(cell);
        $("targets").append(tr);
    });
    $("target-json").value = pretty(targetRows);
}

function fill(request) {
    current = structuredClone(request);
    ["model", "platform", "framework", "runtime", "dtype"].forEach(
        (key) => ($(key).value = request.deployment[key]),
    );
    $("tp").value = request.deployment.tensor_parallel;
    $("workload-path").value = request.workload;
    $("workload-file").value = "";
    uploadedWorkload = undefined;
    $("request-count").value = "";
    $("output-tokens").value = "";
    $("base-mode").value = request.base.manifest ? "manifest" : "configuration";
    $("manifest").value = request.base.manifest || "";
    $("base-config").value = pretty(request.base.configuration || {});
    $("output").value = request.execution.output;
    $("memory").value = request.execution.memory_limit_gib || 4;
    $("kernel-state").value =
        request.execution.kernel_cache_initial_state || "empty";
    const capture = request.execution.capture || {};
    $("image").value = capture.image || "";
    $("devices").value = (capture.devices || []).join(",");
    $("capture-json").value = pretty(capture);
    $("database").value = request.performance?.database || "";
    $("models").value = request.performance?.models || "";
    $("allow-sampling").checked = !!request.performance?.sampling_budget;
    $("sampling-json").value = pretty(
        request.performance?.sampling_budget || {},
    );
    $("request-json").value = pretty(request);
    targetRows = structuredClone(request.targets);
    showTargets();
}

function form() {
    const request = structuredClone(current);
    ["model", "platform", "framework", "runtime", "dtype"].forEach(
        (key) => (request.deployment[key] = $(key).value.trim()),
    );
    request.deployment.tensor_parallel = Number($("tp").value);
    request.workload = $("workload-path").value.trim();
    request.base =
        $("base-mode").value === "manifest"
            ? { manifest: $("manifest").value.trim() }
            : { configuration: JSON.parse($("base-config").value) };
    request.targets = structuredClone(targetRows);
    request.targets.forEach(
        (row) =>
            (row.deployment = {
                tensor_parallel:
                    row.deployment?.tensor_parallel ??
                    request.deployment.tensor_parallel,
            }),
    );
    request.execution.output = $("output").value.trim();
    request.execution.memory_limit_gib = Number($("memory").value);
    request.execution.kernel_cache_initial_state = $("kernel-state").value;
    if ($("image").value.trim()) {
        request.execution.capture = JSON.parse($("capture-json").value);
        request.execution.capture.image = $("image").value.trim();
        request.execution.capture.devices = $("devices")
            .value.split(",")
            .map((value) => Number(value.trim()));
    } else delete request.execution.capture;
    request.performance = {};
    if ($("database").value.trim())
        request.performance.database = $("database").value.trim();
    if ($("models").value.trim())
        request.performance.models = $("models").value.trim();
    if ($("allow-sampling").checked)
        request.performance.sampling_budget = JSON.parse(
            $("sampling-json").value,
        );
    $("request-json").value = pretty(request);
    const body = { request };
    if (uploadedWorkload) {
        body.workload = structuredClone(uploadedWorkload);
        if ($("request-count").value)
            body.workload.requests = body.workload.requests.slice(
                0,
                Number($("request-count").value),
            );
        if ($("output-tokens").value)
            body.workload.requests.forEach(
                (row) =>
                    (row.max_new_tokens = Number($("output-tokens").value)),
            );
    } else if ($("request-count").value || $("output-tokens").value) {
        throw new Error(
            "修改规模时请先上传 workload JSON；或直接编辑本机 workload 后提供路径。",
        );
    }
    return body;
}

async function preview() {
    const result = await api("/api/preview", form()),
        box = $("preview-result");
    box.hidden = false;
    box.replaceChildren();
    text(box, "h2", "准备预览");
    const size = result.plan.workload_size;
    text(
        box,
        "p",
        `${size.requests} 个请求，其中 ${size.formal_requests} 个正式请求；输出 token 上限合计 ${size.output_token_limit}。`,
    );
    const reuse = result.plan.base.reuse;
    text(
        box,
        "p",
        reuse.status === "matching_capture"
            ? "已找到输入相同的 base，运行会复用。"
            : reuse.status === "declared"
              ? "使用声明的 base，运行时再核对源语义。"
              : "需要准备 base；未完成任务会沿原账本恢复。",
    );
    result.plan.maintained_selections.forEach((item) =>
        text(
            box,
            "p",
            `TP${item.deployment.tensor_parallel} 已有 ${item.response_count} 类成本响应；具体工作是否在适用域内仍需运行查询。`,
        ),
    );
    text(
        box,
        "p",
        "静态预览不保证成本齐备、镜像可启动或精度达标。具体缺项由运行中的真实工作查询确定；未知耗时不作估计。",
        "notice",
    );
    rawDetails(box, "全部准备与预算", result.plan);
    message("预览完成。检查环境和累计预算后运行。");
}

async function refresh() {
    const rows = await api("/api/tasks"),
        list = $("task-list");
    list.replaceChildren();
    if (!rows.length) text(list, "p", "尚未创建任务。", "muted");
    rows.forEach((row) => {
        const item = document.createElement("div");
        item.className = "task";
        text(
            item,
            "span",
            `${row.title} · ${labels[row.status] || row.status} · ${new Date(row.created_at * 1000).toLocaleString()}`,
        );
        item.append(
            button("查看", async () => {
                selected = row.id;
                await detail();
            }),
        );
        list.append(item);
    });
    if (selected) await detail();
    clearTimeout(timer);
    timer = setTimeout(
        () => safely(refresh),
        rows.some((row) => row.alive) ? 3000 : 15000,
    );
}

function results(box, report) {
    const rows = report.candidates || report.cells;
    if (!rows) {
        rawDetails(box, "维护结果", report);
        return;
    }
    const missing = rows.filter((row) => row.status === "missing_costs");
    if (missing.length)
        text(
            box,
            "p",
            `${missing.length} 个配置缺少适用的成本数据，尚无完整耗时。可在共享预算内补采，或先导入同部署的独立数据再继续；缺项没有被填成零。`,
            "notice",
        );
    if (rows.some((row) => row.status === "resource_accounting_unavailable"))
        text(
            box,
            "p",
            "部分配置没有声明完整的存储或带宽配额。耗时预测已保留，但目前无法计算资源费用或完成排序；请补充资源声明。",
            "notice",
        );
    rows.filter((row) => row.issue).forEach((row) =>
        text(box, "p", `${row.name}：${row.issue}`, "notice"),
    );
    const baselineLabel = text(box, "label", "比较基准"),
        select = document.createElement("select");
    rows.forEach((row) => {
        const option = document.createElement("option");
        option.value = row.name;
        option.textContent = row.name;
        select.append(option);
    });
    select.value = report.baseline || rows[0]?.name || "";
    baselineLabel.append(select);
    if (report.selected !== undefined)
        text(
            box,
            "p",
            `时间约束下的成本选择：${report.selected || "无可行结果"}；最快：${report.fastest || "无"}。`,
        );
    const wrap = document.createElement("div");
    wrap.className = "table-scroll";
    box.append(wrap);
    function draw() {
        wrap.replaceChildren();
        const table = document.createElement("table"),
            head = document.createElement("tr");
        [
            "配置",
            "TP",
            "状态",
            "完整 HTTP（秒）",
            "相对基准",
            "每小时资源权重",
            "窗口资源权重",
        ].forEach((name) => text(head, "th", name));
        table.append(head);
        const base = rows.find((row) => row.name === select.value),
            baseTime = base?.predicted_sec ?? base?.formal_http_sec;
        rows.forEach((row) => {
            const tr = document.createElement("tr"),
                seconds = row.predicted_sec ?? row.formal_http_sec;
            const number = (value) =>
                value == null ? "—" : Number(value).toFixed(3);
            [
                row.name,
                row.deployment?.tensor_parallel ?? "—",
                row.status,
                number(seconds),
                baseTime && seconds != null
                    ? `${((seconds / baseTime - 1) * 100).toFixed(2)}%`
                    : "—",
                number(row.hourly_cost_units),
                number(row.window_cost_units),
            ].forEach((value) => text(tr, "td", String(value)));
            table.append(tr);
        });
        wrap.append(table);
    }
    select.onchange = draw;
    draw();
    text(
        box,
        "p",
        "耗时为完整正式 HTTP 预测。缺项或不支持保留其状态；这里没有正常实测，不能据此判断精度。",
        "hint",
    );
    if (report.qualification) text(box, "p", report.qualification, "hint");
    rawDetails(box, "成本缺口、资源与适用范围（完整结果）", report);
}

async function detail() {
    const row = await api(`/api/tasks/${selected}`),
        box = $("task-detail");
    const signature = pretty(row);
    if (signature === detailSignature) return;
    detailSignature = signature;
    box.hidden = false;
    box.replaceChildren();
    text(box, "h2", `${row.title} · ${labels[row.status] || row.status}`);
    const phase = row.progress?.phase;
    if (phase)
        text(
            box,
            "p",
            `当前阶段：${labels[phase] || phase}${row.progress.completed_targets !== undefined ? `，完成 ${row.progress.completed_targets}/${row.progress.targets} 个目标` : ""}`,
        );
    if (row.error) text(box, "p", row.error, "notice");
    const actions = document.createElement("div");
    actions.className = "actions";
    box.append(actions);
    if (row.alive) {
        if (row.status !== "stopping")
            actions.append(
                button("停止本任务", async () => {
                    await api(`/api/tasks/${selected}/stop`, {});
                    message("已发送停止请求，正在回收本任务资源。");
                    await refresh();
                }),
            );
    } else if (row.status !== "completed")
        actions.append(
            button("继续任务", async () => {
                await api(`/api/tasks/${selected}/resume`, {});
                await refresh();
            }),
        );
    if (row.action !== "maintain")
        actions.append(
            button("复用输入", () => {
                fill(row.document);
                if (row.policy) {
                    $("policy").value = pretty(row.policy);
                    $("action").value = "search";
                }
                message(
                    "已载入输入。修改结果目录可另存新结果；原目录会复用采集并更新报告。请先预览。",
                );
                window.scrollTo({ top: 0, behavior: "smooth" });
            }),
        );
    const exports = [
        "request",
        ...(row.action === "search" ? ["policy"] : []),
        ...(row.result ? ["result", "csv"] : []),
    ];
    exports.forEach((kind) => {
        const link = document.createElement("a");
        link.href = `/api/tasks/${selected}/${kind}`;
        link.className = "export";
        link.textContent = {
            request: "导出请求",
            policy: "导出搜索",
            result: "导出结果",
            csv: "导出 CSV",
        }[kind];
        actions.append(link);
    });
    row.budgets.forEach((budget) => {
        const state = budget.state;
        const used = {
            starts: state.attempts.length,
            wall_sec: state.attempts.reduce(
                (sum, attempt) => sum + attempt.wall_sec,
                state.processing_wall_sec || 0,
            ),
        };
        ["requests", "input_tokens", "output_tokens"].forEach(
            (key) =>
                (used[key] = state.attempts.reduce(
                    (sum, attempt) => sum + attempt[key],
                    0,
                )),
        );
        if (state.waiting_for)
            text(
                box,
                "p",
                `${budget.kind}：${labels[state.waiting_for] || state.waiting_for}`,
                "notice",
            );
        text(
            box,
            "p",
            `${budget.kind} 已记账 ${used.starts} 次启动、${used.wall_sec.toFixed(1)} 秒、${used.requests} 个请求。当前运行/等待未结算时间另见进度。`,
            "hint",
        );
        rawDetails(box, `${budget.kind} 累计预算与原始账本`, {
            used,
            limit: budget.limit,
            state,
        });
    });
    if (row.result) results(box, row.result);
    if (row.runtime_log) rawDetails(box, "预测实时进度与日志", row.runtime_log);
    if (row.worker_log) rawDetails(box, "任务日志", row.worker_log);
    rawDetails(box, "有效公共输入", row.document);
}

async function init() {
    const bootstrap = await api("/api/bootstrap");
    token = bootstrap.token;
    bootstrap.request.execution.output = `${bootstrap.directory}/work/default`;
    bootstrap.request.execution.capture.detail = "semantic";
    fill(bootstrap.request);
    $("policy").value = pretty({
        baseline: targetRows[0].name,
        fixed_device_gib_per_rank: 20,
        fixed_host_gib: 8,
        maximum_time_ratio: 1.1,
        resource_plans: [
            { name: "local", device_gib_per_rank: 64, host_gib: 128 },
        ],
        pricing: {
            device_gib_hour: 8,
            host_gib_hour: 1,
            storage_gib_hour: 0.05,
            device_to_host_gibps_hour: 1,
            host_to_device_gibps_hour: 1,
            host_to_storage_gibps_hour: 1,
            storage_to_host_gibps_hour: 1,
        },
    });
    $("preview").onclick = () => safely(preview, $("preview"));
    $("start").onclick = () =>
        safely(async () => {
            const body = form();
            body.action = $("action").value;
            body.title = $("title").value;
            if (body.action === "search")
                body.policy = JSON.parse($("policy").value);
            const task = await api("/api/tasks", body);
            selected = task.id;
            message("任务已提交。运行会保留累计用量，可在下方查看和停止。");
            await refresh();
        }, $("start"));
    $("refresh").onclick = () => safely(refresh);
    $("add-target").onclick = () => {
        targetRows.push({
            name: `target_${targetRows.length + 1}`,
            configuration: {},
        });
        showTargets();
    };
    $("apply-targets").onclick = () =>
        safely(() => {
            const value = JSON.parse($("target-json").value);
            if (!Array.isArray(value)) throw new Error("目标应为数组");
            targetRows = value;
            showTargets();
        });
    $("apply-request").onclick = () =>
        safely(() => fill(JSON.parse($("request-json").value)));
    for (const kind of ["request", "policy"])
        $("import-" + kind).onclick = () =>
            safely(async () => {
                const value = await api("/api/import", {
                    path: $("import-path").value,
                    kind,
                });
                fill(value.request);
                if (value.policy) {
                    $("policy").value = pretty(value.policy);
                    $("action").value = "search";
                }
                message("已载入请求，保留其路径和预算；请先预览。");
            });
    $("workload-file").onchange = () =>
        safely(async () => {
            uploadedWorkload = JSON.parse(
                await $("workload-file").files[0].text(),
            );
            $("workload-summary").textContent =
                `已上传 ${uploadedWorkload.requests.length} 个请求，尚未运行。`;
        });
    $("assets").onclick = () =>
        safely(async () => {
            const assets = await api("/api/assets"),
                box = $("asset-list");
            box.replaceChildren();
            text(
                box,
                "p",
                `已登记 ${assets.bases.length} 份 base、${assets.databases.length} 个数据库。`,
            );
            rawDetails(box, "Base 路径", assets.bases);
            assets.databases.forEach((database) => {
                text(box, "h2", database.path);
                text(
                    box,
                    "p",
                    `${database.captures} 次独立采集，${database.measurements} 条测量。`,
                );
                database.deployments.forEach((item) =>
                    rawDetails(
                        box,
                        `${item.deployment.model} / TP${item.deployment.tensor_parallel}：${item.responses.length} 类响应`,
                        item,
                    ),
                );
            });
        });
    $("maintain").onclick = () =>
        safely(async () => {
            const task = await api("/api/tasks", {
                action: "maintain",
                path: $("maintenance-path").value,
                title: "维护性能数据库",
            });
            selected = task.id;
            await refresh();
        });
    await refresh();
}

safely(init);
