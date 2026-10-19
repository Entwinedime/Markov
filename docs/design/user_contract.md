# 用户输入、运行与结果合同

本文维护稳定的产品合同，不记录实验成绩或实施进度。正式任务是一个固定模型、非空配置集合和非空请求集合；用户不指定 base，不为普通预测填写节点成本或原生批次状态。

## 项目输入

项目 JSON 的路径相对声明它的文件解析。输入边界拒绝未知字段、错误类型、重复身份、空集合和无效预算，不保留旧格式兼容。可解析不等于该实现组合能够预测。

| 字段 | 含义 |
| --- | --- |
| `model.path` | 全任务固定的模型路径；更换模型新建任务 |
| `model.tokenizer_path` | 默认与模型路径相同；token 输入无需加载 tokenizer |
| `configurations[]` | 每项含唯一 `id`、`framework`、`environment`、完整配置文件 `config`，不继承其他候选 |
| `environment` | Docker runtime、对应 service，以及非空、不重复的非负设备编号 |
| `request_workloads[]` | 每项含唯一 `id` 和请求文件 `path` |
| `collection.base / performance` | 默认禁采；`allow_run` 和累计 `max_wall_time_sec`。启用时预算为正 |
| `assets.templates[]` | 显式结构 manifest，可附该来源的 `node_durations`；默认空，不重复来源 |
| `assets.performance_manifests[]` | 显式独立性能样本，由拟合进程转换为候选 |
| `assets.cost_models[]` | 显式候选成本文件，声明顺序决定重叠候选的查询顺序 |
| `output.directory / diagnostics` | 必填输出位置；完整图诊断默认关闭 |

框架取 `sglang` 或 `ktransformers`，实际支持按框架/模型/后端/环境组合判断。框架配置只含 `parameters` 和可选 `model_assets`；接受的参数全部显式填写，采集还要记录原生最终生效值。

| 框架 | 参数字段 | 额外模型资产 |
| --- | --- | --- |
| SGLang | `device`、`tp_size`、`attention_backend`、`max_running_requests`、`max_total_tokens`、`chunked_prefill_size`、`max_prefill_tokens`、`schedule_policy`、`disable_radix_cache`、`disable_overlap_schedule`、`disable_cuda_graph`、`enable_mixed_chunk`、`enable_hierarchical_cache` | 无；使用固定主模型 |
| KTransformers | `backend_type`、`tp`、`max_batch_size`、`chunk_size`、`cpu_infer`、`cache_lens`、`sched_strategy`、`use_cuda_graph` | `gguf_path`、`optimize_config_path` |

字段保持原生整数、布尔和字符串类型。SGLang 的 `chunked_prefill_size` 为 −1 或正整数，其他计数为正。主模型路径不重复放入 parameters。

预测固定使用轻量控制器、`modeled` 范围和统一预热后的 cold logical KV。没有 `simulation`、`initial_state` 选择项；其他控制器、暖 KV 或 HTTP 范围需有实际能力后再建立合同。

## 请求合同

请求文件包含非空 `requests` 数组。每项有唯一非空 `id`、非负整数微秒 `arrival_offset_us`、正整数 `output_tokens`，以及 `input_tokens` / `text` 二选一。

- token 数组非空且元素为非负整数，真实运行还须满足模型词表。text 非空；分词只执行一次并保存实际 token。预测 plan 对尚未实现的 text 归一化明确返回不支持，原生采集准备可分词。
- 输入不必按到达排序；同刻按声明次序。到达偏移在边界转换成 ns，不以客户端串行请求替代开放到达。
- 输出长度是条件输入，不预测内容或 EOS。Prefill 产生第一个 token，输出数为 1 时不调用 Decode。
- 同刻原生预分词请求可以合为一条批量消息，实际 batch 由原生 scheduler 决定；整组返回不能代替逐请求 Published。
- 内容相关路由、缓存命中和专家选择不能由长度虚构。输入序列改变后新建任务，不修改已有结果的身份。

## 统一稳态起点

结构采集、独立性能采样和目标观察共用有界预热协议：引擎已准备、无在途业务请求、逻辑 KV 清空，保留协议允许的编译/分配缓存和 workspace 状态。

模型加载、运行时初始化、首次编译和预热不进入预测时延，但全部进入采集开销。一次 Prefill/Decode 不证明全部所需特征已热；预热覆盖依据有限实现条件，不能执行完整目标序列预取调度答案。

原生框架负责请求结束和 idle/清理确认。客户端收到响应不能替代确认，也不能仅凭接口名推断缓存保留效果。窗口内合法的分配、扩容和等待仍属业务工作。

记录预热输入、实际配置和初态；不同配置不直接继承同一容量快照。初始化证据缺失保留缺口，不能补造空缓存。计时窗口出现未覆盖的首次编译时，该窗口不能取得稳态精度资格，不以减常数或删除高值修正。

## 结构、成本与条件输入

模板来源须满足模型、实现、部署、初始化和有限变换条件。加载成功只说明资产可读，不表示目标所有阶段、rank、资源和参数均可展开。共同候选及局部供体由实际模板规则资格判断，不能按形状相近选择。

独立性能 manifest 由有锁的拟合进程消费，生成候选置于显式成本文件之后。预测只读候选，不接触原始时延样本；候选命中不自动取得精度资格。

给定耗时文件包含 `base_manifest`、`unit="ns"`、非空 `assumptions` 及 `values`，每项含来源内 `source_span` 与非负整数 `duration_ns`。它仅用于条件结构分析，不能作为训练标签或正式精度依据。耗时输入错误与结构来源缺失分别报告，不能因此重采结构。

范围内未知成本不能填零、借用来源墙钟或父包络。结构支持、样本覆盖、成本可查询与精度通过是不同结果。

## 自动运行与恢复

1. 先解析配置集合并复用显式资产。仅缺少兼容结构来源且获准采集时，才在累计 base 预算内执行代表配方；规则或成本缺口不触发结构采集。
2. 实例化产生统一工作查询，跨组合归并缺失成本。适配器构造有界采样配方，实际命中由原生工作与完整成本查询共同确认。
3. 新模板或成本资产发布后，从初态重算全部组合。没有新资产时复用已有结果，不拼接不同轮次的组合，也不恢复模拟器内部 checkpoint。
4. 不能构造、已经尝试而无新增证据、预算耗尽或合同不足时停止并保留原因，不无限循环补采。
5. `explore` 在全部预测完成后固定资产，再用独立预算取得原生验证端口；预测缺数据时不进入验证。验证只能读取，不能反哺模板、拟合和调参。

`resume` 的预算参数表示新的总额度，已用量不重置。更换模型、配置或请求须新建任务；尚未固定验证的任务可以显式替换完整资产列表。

| 情况 | 处理 |
| --- | --- |
| 预测/拟合宿主中断而容器仍活动 | 接回已有进程；活动期间修订不生效，不启动替代容器 |
| 原生采集被中断 | 停止本次采集、保留消耗和交接；显式恢复按原输入及剩余预算处理 |
| 原生完成、后处理未交接 | 优先消费已有事实或继续处理，不能重复采集 |
| 确定的后处理失败 | 保留失败收据；修正原因后显式重处理，不自动重试确定失败 |
| 部分组合失败 | 保留其他组合结果和共享资产，任务总体反映未完成项 |
| 终态重复恢复 | 读取正式结果并完成必要清理，不新增预测轮次或设备运行 |

输入修订和旧结果失效一起登记，未交接完成不返回旧结果冒充新输入输出。只有正式发布并被消费确认后，才能清理本任务拥有且无活动写入者的临时产物。预测、拟合、采集各自的真实锁和预算职责保留。

## 产物合同

一次全流程使用一个时间相关运行根。任务状态、输入快照、资产 manifest 和处理收据各有唯一写入者，不复制成多套状态。

```text
data/<时间>_<任务>/
├── run.json                 # 生命周期、预算、交接和结果引用
├── inputs/                  # 复现所需的规范输入
├── assets/                  # 模板、成本及共享 CPU 区间目录
├── captures/                # base、performance、validation 各自的原生记录
├── results/                 # prediction.json、summary.md、可选 comparison.json
├── logs/                    # 运行日志
├── work/                    # 尚未消费的中间产物，成功交接后清理
└── diagnostics/             # 显式开启的图与分析，不能依赖已清理的 work
```

图中为职责布局；具体子目录由入口及 manifest 登记，不靠扫描历史 data 隐式加载。`results/sampling-plan.json` 只在有待处理采样交接时保留。

每组合的 `performance_requirements` 保存实际工作查询与覆盖，`cpu_service_catalog` 指向共享命名 CPU 区间目录。目录只提供服务身份，不是全类别来源原型清单或成本数据。

诊断开启时 `structure.graph.artifact` 指向实际目标图，包含调用上下文、节点、依赖、端口、成本及来源；不另做一次假想变换生成第二份详情。源图只用于诊断，正式模板从保留规范化事实重建。

## 状态、指标与比较

组合按配置顺序 × workload 顺序输出，含身份、`status`、`scope`、指标、缺口、假设和来源。累计采集用量在任务根共享，不按组合重复计费。

`complete` 表示条件执行完成；`needs_data` 表示缺必要数据；`unsupported` 表示范围或规则不支持；`failed` 表示执行失败。任务 `finished` 可以包含未完成组合，不能解释成精度通过。结果没有隐含 base/speedup 或综合排名。

| 指标 | 定义 |
| --- | --- |
| `window_time_ns` | 最后 Published − 最早逻辑 arrival |
| `request_latency_ns` | 请求最后 Published − scheduler_ready |
| `ttft_ns` | 请求首次 Published − scheduler_ready |
| `driver_request_latency_ns / driver_ttft_ns` | 从计划 arrival 起算的对应间隔 |
| `tpot_ns` | 首末 Published 间隔 / (输出数 − 1)；单 token 为 null |
| `output_tokens_per_sec` | 总输出 token / 窗口秒数 |

非重叠控制路径可能在上一批返回后才接收已到达请求；逻辑到达、调度可见和执行准入是不同边界。原生比较必须对齐同一请求、端口及可关联时钟，不能从两个未关联时钟直接相减。

独立验收前固定输入、支持域与误差门槛，先核对结构、局部成本和重叠，再评价请求指标。少量样本列出逐项结果，近零值另报绝对误差，缺失或零分母不伪造比例。验证输入用于修正后转为开发证据，须另选独立输入；文件更名或重新压缩不产生独立性。
