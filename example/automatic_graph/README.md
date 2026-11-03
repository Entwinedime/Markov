# 自动取图与条件 Serving 示例

输入一个固定模型、配置集合和请求集合，流程自动选择代表来源、观察必要逻辑角色、建立参数化模板，再按目标状态动态展开。模型使用完整结构的 dummy 权重，节点耗时来自显式假设；输出用于检查执行结构，不构成真实性能预测或精度结论。

## 准备与一键运行

从仓库根执行。先准备 `/models` 下的模型文件，并修改 [exploration.json](exploration.json) 中的模型路径、物理设备、目标配置、请求和预算。

默认镜像为 `markov-next-sglang-env:ubuntu22.04`、`markov-next-sglang:ubuntu22.04` 和 `markov-next-modeling:ubuntu24.04`，名称不包含日期。基础镜像提供依赖，最终推理镜像安装当前 SGLang 与采集代码；已有基础镜像或更改标签不表示最终镜像已经构建和验证。

需要制作镜像时使用：

```bash
docker build -t markov-next-sglang-env:ubuntu22.04 docker/images/base/sglang
docker compose -p markov-next-redesign -f docker/compose/inference.yml build sglang-profile modeling
```

镜像就绪后，一条命令完成结构探索：

```bash
example/automatic_graph/run.sh --input example/automatic_graph/exploration.json
```

`MARKOV_IMAGE_PREFIX` 可选择其他已准备好的前缀，默认 `markov-next`。脚本在 modeling 容器内组织任务，通过宿主 Docker 创建本任务的原生容器。采集使用镜像内已安装的观察包和SDK源码，构建任务私有观察库，不逐任务安装可变工作区；C++核心在建库/预测前单独构建。修改观察代码后须重建原生镜像并明确新来源依据。物理设备只选择采集位置，TP 本地分片不要求现场拥有目标数量的卡；PP 采集使用实际 TP×PP 参与进程。

解释在匹配SDK的原生环境中加载当前工作区消费端代码，采集仍使用固定镜像内的包。阶段交付时重建`sglang-profile`，使镜像快照与源码一致。使用本机代理且构建网络不可达时，可通过已有的`MARKOV_BUILD_NETWORK=host`选择构建网络；运行镜像和依赖仍由同一Dockerfile制作。

配置可用 `"placement": {"devices": [4, 6]}` 指定 TP2 的两个目标设备，省略时按 rank 绑定 `0..TP×PP-1`；当前要求单机、设备互异，纯预测不检查现场卡数。它与采集用的 `deployment.physical_devices` 独立，结果的 topology 会列出实际绑定和参与组；设备标签不自行产生带宽或争用成本。

PP 配置可增加 `"pp": 3, "microbatch_requests": 2`；部署的 `physical_devices` 需要容纳该来源组的 TP×PP 参与者，placement 按 stage 内 TP rank 排列。当前 Ascend 实现不支持 PP2。每域的一次 PP 会话启动全部必要参与者，代表发布观察，其余角色使用独立绑定；目标使用同一控制器维护多微批次和唯一 KV 状态。

一级缓存入口示例为 [prefix_cache.json](prefix_cache.json)：`example/automatic_graph/run.sh --input example/automatic_graph/prefix_cache.json`。输入固定Qwen3-8B、TP1，选择原生控制及`prefix_cache=true`，两种容量/并发与两份共享内容请求集共四个目标。采集适配器在一份代表会话中分组观察缓存机制，用户不指定组批。结果的`resources`允许缓存页留存；终态守恒不代替中途引用或设备内容就绪验证。MoE还需提供对应EP拓扑的路由条件，缓存键同时比较token与逐位置专家条件；路由分歧只保留此前的共同前缀，模型输入token不变。

P/D 分离使用 [disaggregated.json](disaggregated.json)：`example/automatic_graph/run.sh --input example/automatic_graph/disaggregated.json`。确认其中两张采集卡可用；目标在两池内分别声明 `tp` 与容量，示例仍为双方 TP1、PP1。P 每次处理一个完整 prompt，D 预约后接收 KV，再发布首 token 和继续 Decode；placement 先列 P 的全部 rank，再列 D 的全部 rank。源页在传输确认后释放，待合并页在下一次分配需要时通过原生模板整理；结果 `resources` 分别报告各池容量、空闲页/槽位和缓存持有量。对称/非对称 TP、双方 PP4 和 MoE 双方 TP=EP2 已有有限开发核对；原生要求 Decode PP 等于 Prefill PP 或1，来源限制和独立验证边界继续保留。

MoE/EP 使用 [expert.json](expert.json)：`example/automatic_graph/run.sh --input example/automatic_graph/expert.json`。示例为完整 Qwen1.5 MoE、TP2/EP2，需两张实际采集卡；请求路由按层和逻辑 token 区间给出，padding 路由单独声明。它是给定路由的条件结构，仍执行 gate/top-k 等工作，不能从长度推断真实内容路由；缺少原型或来源覆盖为 partial 时如实报告。

示例预算是有限上限，不保证任意模型或配置都能在其中完成。初始化、统一预热、观察、导出、处理和失败均计入累计额度；额度不足时保留状态并停止，不隐式扩大预算或循环补采。

同一正式任务可以分阶段执行，状态和费用沿原目录保存：

```bash
example/automatic_graph/run.sh --input example/automatic_graph/exploration.json --stop-after sources
example/automatic_graph/run.sh --resume data/YOUR_RUN --stop-after templates
example/automatic_graph/run.sh --resume data/YOUR_RUN
```

`sources`只取得计划内代表来源，不构建C++核心；`templates`在来源就绪后解释并原子发布模板；默认继续预测全部组合。缺来源时恢复先补齐来源，模板生产方法本身只消费已发布事实，不启动Engine。`sources_complete`与`templates_complete`只表示对应准备阶段完成，不能作为预测或独立对照通过。

## 来源复用与结果含义

模型词表和专家参数由SGLang的配置对象提供，完整配置同时保留用于恢复时核对模型身份；字段别名在框架适配边界处理。

每次任务固定一个模型。dtype、TP、PP、EP、attention backend、page_size及chunk、mixed、prefix cache机制相同的配置共享来源；每个域保留实际逻辑 rank 的本地分片。可隔离 TP 先取得必要根角色，由实际模块和原生词表分片 API 规划其余代表，再为等价 peer 生成独立绑定。原生组仍启动协议所需的参与者，模型加载后比较模块布局和角色职责，仅选代表观察与导出；PP4、EP4 复用、TP2×PP3×EP2 及限定多 rank PD 组合已有开发核对。`inputs/role-plans` 和结果的 `role_source` 区分观察与派生。不同模型或域分别建立来源，不以相似 shape 自动替代所有职责，也不声明同库跨 TP 或跨页大小变换。

同域内根据请求、并发、KV 容量与 Prefill 预算重新组批、求值几何、重复成员子图和连接端口。原生实现变化由已声明的条件和局部原型选择；超过模型上下文、参数范围或原型覆盖时保留明确失败。正式入口提供 SGLang/Ascend，单卡辅助通信只提供结构接口，不提供真实跨卡时延。

随附探索输入均显式选择 `"control": "native"`。先用下文命令构建control镜像，再运行一键探索；已有模板也可通过原生控制入口运行。原生代码决定组批及PP/PD推进，核心管理KV资源和模拟完成反馈。当前开发与验收只推进原生后端，轻量实现保留为待开发/待复验；未指定control的通用输入仍沿用既有缺省，不属于本轮示例选择。

共同实例配置可加 `"prefill_chunk_tokens": 128`，PD放在 `pd.prefill` 内，要求原生控制、正整数且页对齐。chunk机制单独建来源域，目标chunk长度在域内变换；中间块保存KV索引并继续下一块，最终块才生成输出。单worker、dense TP2、MoE TP2/EP2、dense TP1×PP3、P1/D1、P TP2/D TP4、P/D各TP2/EP2及P/D各TP1×PP3已有分块开发核对。

`"mixed_batches": true`用于共同实例，要求原生chunk控制及对应的mixed来源。当前官方FA环境下，Qwen3-8B的TP1、TP2和PP3已完成包含一级缓存的开发闭环；有限到达案例核对了就绪Decode的预算保留及后续Prefill完成。Qwen1.5 MoE的一个TP2/EP2补齐mixed批次被原生attention拒绝，该组合没有合格mixed来源；非mixed chunk/cache已有开发证据。以上不授予任意负载公平性或独立结构资格，overlap尚未开放。

请求输入使用可复现的合成内容：可用workload的`prefixes`（`id`、`parent`、`tokens`）及请求的`prefix_id`声明共享部分，其余prompt和生成后缀互不共享。采集保留原生采样工作，再注入与预测相同的输出token；这是条件执行结构，不是真实文本生成。内容字段本身不启用缓存；共同实例配置或`pd.prefill`显式设置`"prefix_cache": true`，并选择原生控制及同机制来源。一级缓存已接通限定开发范围；PD仅Prefill侧缓存，Decode仍通过KV交接。单worker可与chunk组合，其他机制组合仍须各自取得证据。

统一预热使用与目标序列分开的固定五请求，并按原生 ATB 上界预留 workspace；预热不计入目标服务时间，但计入采集费用。当前 runtime 规则针对 CANN 9.0/Ascend910B 的 dense 路径，workspace 预留限制为相等且不大于 128 的 head 维度。新的并行chunk/缓存组合、overlap、speculative 或其他内容相关路径需要各自的实现和证据。

原生采集在资源清空和观察准备完成后，等待完整循环的空闲边界，再阻塞入口等待真实输入；Profiler启动后也等待相同边界。清空请求沿原生延后flush唤醒并等待发送/设备排空，workspace保留预热，已释放的PD元数据槽位恢复统一顺序。仿真先以空请求推进同一原生循环，所有组都到过空闲后停驻入口，目标请求到达才放行；下游stage继续排空控制环，准备不计入目标图或时间。旁路要求观察域声明`prewarmed_idle_ingress`，旧记录不能补写声明后使用；具名开发案例已贯通这一交接，泛化范围仍需独立验证。

给定耗时改变后，完成事件会改变后续组批。一次运行完成、条件结构可展开、独立原生对照相符和性能精度通过分别判断，不能相互代替。

开发中的真实时间旁路由`markov_application.native --completion-times <观察文件>`交给核心，使用相同原生控制、构图和资源主循环。独立观察文件包含来源、模型/环境域、对应输入及带语义身份的发布/完成事件，各事件携带原生rank/region引用`observations`；不修改workload，也不输入目标图或强制批次。明细输出的`observed_work`给出已匹配的目标任务与原生区域，只供诊断，不影响控制或构图。输出以`completion_source: observed_boundaries`标识，不产生内部节点时间线；结构/资源检查继续运行，内部时间检查为`not_measured`。

普通serving、PP及PD共用转换开发入口`python3 -m markov_collection.adapters.frameworks.sglang.completion_times --capture <采集目录> --output <观察文件>`，在Docker内运行。普通目录包含本次完整请求序列的`session.json`、`request-delivery.json`及所有真实TP/PP参与者的`rank-N.json`；PD传入两池共同的采集根，消费根下的投递记录和prefill/decode各自的会话、rank观察及接收端`transport-rank-N.json`。单卡隔离分片不能提供真实组完成时间，输出位于原始采集目录之外。

PP按stage合并TP完成，从原生发送句柄取得等待时刻；工作引用按原发送批次及wire字段关联延后等待，并涵盖同批次的结果准备和同步。PD接收准入同时引用原生BatchAccepted准备与PrebuiltResult，不以最终时间marker代替全部工作。PD沿实际分片和传输会话合并数据/末片元数据完成，并分别消费目的广告、源端握手、两端成功发布及状态查询事实。握手与发布按请求及端点控制组合并，不按每条路由复制；复制返回不能同时充作两端成功，源端接收广告也不必等待目的发送调用返回。查询只交接读取边界，状态、归约、metadata gate及请求选择仍由核心资源和原生代码决定。

转换直接消费不可变rank观察，共享调度、结果和缓存服务解释，辅助primitive原型不进入控制动作，不要求预先解析完整物理trace。发送以原生入队服务前的KvSubmit标记反馈，后续CPU尾部不阻挡后台复制。具名多阶段PD开发案例已完整消费真实边界，内部结构及工作范围仍有未验证项；旧失败记录保留，缺必要事实的旧派生输入不能用于当前合同。末轮清理沿控制动作交接，不另填idle时间表。

独立原生比较入口`validation.py`先固定目标原生观察，自动将边界时间写入`work/validation/<实际采集名>/observed/completion-times.json`，再经任务的`simulate(..., completion_times=...)`执行精简与明细预测。它不复用给定耗时的探索结果，缺少必要边界时保留错误。默认/明细比较排除图、逐步历史及`observed_work`等诊断字段，正式请求、资源和完成结果仍须相同；恢复到新采集尝试时，预测及评价也归属新尝试。常规探索继续使用输入给定的节点耗时，不自动运行目标ground truth。

核心CLI的`record-batches`和`simulate --batches`可固定原生成员、token范围及PP lane作诊断，当前限完整Prefill、无前缀缓存。它沿用轻量控制器，未记录原生预分配槽位或跨实例预约历史；同一批次仍可能具有不同池状态和缓冲区形状。回放不能代替原生控制预测，局部比较须同时核对输入条件。

## 输出与恢复

每次运行创建一个 UTC 时间目录并打印位置：

```text
data/<UTC时间>_automatic-graph/
  inputs/                       原任务、固定model.json、角色规划及逐组合输入
  captures/<角色-尝试>/         原始会话、语义记录及按策略保留的SDK导出
  assets/<实现域>/templates.json  共享定义/布局/执行模式与独立角色绑定
  results/summary.md             人读入口、失败与累计费用
  results/<配置>/<请求集>.json   默认的精简组合结果及逐请求结果
  diagnostics/                  --details 时的完整组合结果
  logs/run.log                  子进程命令与失败原因
  state.json                    任务状态、资产位置及费用
  work/sources/<域>/<采集名>/   建库消费的语义解释、物理事实及派生trace
  work/                         SDK、观察库与尚未发布的草稿，按任务生命周期清理
```

原始语义记录与模板保留，解释阶段不回写采集文件。建库使用work中的语义声明和物理事实，资产发布后按任务策略释放；PD原始来源另保留每rank的`transport-rank-N.json`注册端点、地址及buffer布局，以及`rank-N.json`中实际复制的地址片段。框架适配将其解释为layer/head、逻辑页槽区间和元数据行，example再对照目标声明与完成关系；进程地址不进入模板或目标身份，页尾填充不冒充有效模型token。首次运行加 `--details` 才保留完整图、逐批/逐token记录和原始profiler/runtime导出；完整组合结果改写到diagnostics。已发布域仅清理自己的`work/sources/<域>`，全部所需域发布后再释放SDK构建与编译缓存；验证和调查尚未消费的work保留。独立调查保留原始SDK材料供重解释，数据和构建产物不提交Git。

显式重新执行`acquisition interpret --capture <原始目录> --output <工作目录>`按当前规则生成语义声明和物理事实，输出必须在原始目录之外。解释先建立通用接口及框架局部作用域，再统一派生运行时几何和条件；通用池声明不能覆盖原生循环实际使用的局部池。SDK使用临时可写副本处理其日志和表，trace及分析日志写入工作目录；解释进程不加载采集用preload。开始后工作目录的旧物理结果失效，失败时不能复用；原生会话已完成而解释失败时，只恢复后处理。带profile的重解释需要原始native/runtime导出，默认精简任务删除它们后仍可使用已发布模板，不能重建物理事实；恢复也不因源码变化自动更新旧库。

恢复沿用原目录：

```bash
example/automatic_graph/run.sh --resume data/<UTC时间>_automatic-graph
```

模型、配置、请求和部署固定；通过 `--input 修改后的任务.json --resume 原目录` 仅可调整总预算或替换完整给定耗时。累计费用不清零，耗时变化从初态重算结果，模板继续复用。模板是否已发布以`assets/<域>/templates.json`实际存在为准，空目录不能跳过建库或触发来源清理。已完成任务的摘要直接由保存状态重新生成，摘要缺失不触发构建或计算。已有事实足够时只重做解释或建库，不重复启动 Engine；恢复不自动升级旧资产，源码合同变化须明确建立新基线。

更换耗时先使全部旧结果登记失效，再发布新输入，避免中断后混用新旧结果。命令失败或中断时先回收本地进程，再清理本任务容器；清理无法确认则保留pending及预留费用，失败原因仍写入状态和摘要。原生预测的结果和两份日志都不得覆盖输入，包括日志通过链接指向输入的情况。

运行根锁保护唯一写者，共享核心构建锁协调编译和消费。预算使用单调时钟；临时峰值按运行目录采样，不包含容器层、共享构建缓存和系统临时空间。`state.json` 的 complete 仅表示组合产物已经发布，结构资格仍由结果单独说明。

## 单步原生观察

以下命令在已经安装当前采集包的 SGLang 推理容器内执行，工作目录为仓库根。为每次观察选择新的输出目录；SDK 必须在该原生环境内构建。

```bash
MARKOV_OBSERVER_BUILD="$PWD/data/YOUR_RUN/work/observer-build"
cmake -S src/collection/native/runtimes/ascend \
  -B "$MARKOV_OBSERVER_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DMARKOV_ASCEND_INCLUDE_DIR=/usr/local/Ascend/ascend-toolkit/latest/include
cmake --build "$MARKOV_OBSERVER_BUILD" -j2
mkdir -p data/YOUR_RUN/capture/runtime
LD_PRELOAD="$MARKOV_OBSERVER_BUILD/libmarkov_ascend_observer.so" \
MARKOV_NATIVE_RUNTIME_DIR="$PWD/data/YOUR_RUN/capture/runtime" \
python example/automatic_graph/serving.py \
  --model /models/Qwen3-8B --profile \
  --matrix-selector "$MARKOV_OBSERVER_BUILD/markov_ascend_matmul_selection" \
  --output data/YOUR_RUN
```

`--tp-size N --rank R` 在单张物理卡上执行指定逻辑角色；实际请求、KV、槽位和 worker 仍由原生框架维护。物理卡由容器的 `ASCEND_RT_VISIBLE_DEVICES` 选择，worker 使用映射后的逻辑卡号。`--dtype float16` 可用于对应 FP16 模型，必须与目标资产一致。

`--input`可读取serving配置与请求；原生观察按`arrival_ns`的单调时钟期限异步提交，同刻请求按声明顺序提交，后到请求不等待前一请求完成。实际组批归原生框架，不提供输入时使用固定代表配方，统一预热独立于目标请求。输出根下`capture/`保存原始rank记录、会话、SDK材料和`request-delivery.json`，后者记录投递轮次、起点、计划到达及实际偏移；它不等于scheduler接收时刻或精确纳秒保证。`source/`保存解释后的rank记录及启用profile时的物理事实/trace，解释由清除采集preload的独立进程完成。预加载目录须与capture/runtime一致。

## 建库与目标执行

在宿主通过 modeling 容器消费来源：

```bash
scripts/markov.sh inspect --source data/YOUR_RUN/source/rank-0.json
scripts/markov.sh produce \
  --source data/YOUR_RUN/source/rank-0.json \
  --output data/YOUR_RUN/diagnostics/source-graph.json
scripts/markov.sh templates \
  --source data/YOUR_RUN/source/rank-0.json \
  --physical data/YOUR_RUN/source/physical.json \
  --output data/YOUR_RUN/assets/templates.json
scripts/markov.sh simulate \
  --templates data/YOUR_RUN/assets/templates.json \
  --input example/automatic_graph/serving.json \
  --output data/YOUR_RUN/results/serving.json
```

直接调用脚本前须已有当前核心构建，可用 `scripts/build.sh` 制作。逻辑 TP 在一次 `templates` 中重复 `--source`，每项后接对应的 `--physical`；随后向 `simulate` 传入这一份来源域模板。共享 catalog 只保存一份相同结构，roles 保留各 rank 的独立入口和资源绑定；旧内部资产需从来源重建。只有语义来源时可省略 `--physical` 建立逻辑诊断库，不能据此执行完整物理 serving。

切换为无设备原生控制时，先从当前推理镜像构建控制镜像，再消费同样的模板与目标输入：

```bash
docker compose -p markov-next-redesign -f docker/compose/inference.yml build control
bash scripts/markov-control.sh \
  --templates data/YOUR_RUN/assets/templates.json \
  --input example/automatic_graph/serving.json \
  --output data/YOUR_RUN/results/native-serving.json
```

启动端需要Python≥3.10和Docker；它连接独立的原生控制与C++执行容器，不挂载设备、权重或目标观察。原生模型及拓扑配置取自模板；结果发布前等待全部参与进程成功退出。`--details`保留图和动作明细，`--timeout-seconds`限制本次运行时间。该入口检查已采模板下的闭环，不自动补采，也不代表独立结构对照通过。

`inspect` 报告声明的形状、条件和状态缺口；`produce` 发布公开图。模板分开自身工作定义、子调用组合布局、具体调用绑定与物理执行变体；层号、动态成员和目标实例不自动形成新定义。`simulate` 依据目标状态和完成事件持续展开，CPU 服务、设备工作、等待及输出端口各有边界。

`expand --templates ... --target ... --output ...` 用于单区域诊断，`compose` 的 `invocations` 数组用于多个区域；二者均通过正式 `TargetGraph` 绑定、选择和发布，输出同一种图。目标可用 `source_rank` 选择来源角色，默认 0。动态参数、子调用和必需输入均须满足模板合同，不能仅凭标量参数跳过数据输入。

目标的 `values` 数组声明初始操作数，每项给出非负 `storage`（相同编号表示共享存储）和 `kind`；`tensor` 另给 `dtype`、`device`、非负整数 `shape`。调用的 `inputs` 按端口名使用 `{"value": 0}` 引用初始值，或在 `compose` 中用 `{"call": 0, "port": "输出端口名"}` 引用前一调用输出。初始值没有图内 producer，输出及状态变化由模板计算；不输入预期输出或来源 layout 编号来选择候选。由所有者初始化的池状态可以省略，显式提供时仍检查几何与别名。

局部诊断核对给定输入条件下的模板行为，派生输出由模板计算；完整serving还需核对控制器怎样产生这些输入，以及跨调用的资源与完成关系。

## 辅助入口

已有探索任务可以继续对指定目标做原生结构对照：

```bash
example/automatic_graph/run.sh validate \
  --run data/YOUR_RUN --configuration larger --workload together \
  --capture captures/target-larger-together
```

配置和请求集ID来自该任务的输入，当前选择原生控制。入口先观察或复用全部真实worker的固定案例，自动转换本例真实边界时间，再经正式任务入口计算默认/明细结果并逐rank比较；目标观察不参与建库。摘要比较去掉明细独有的图、轨迹、工作引用及资源所有权，资源计数等正式结果仍须完全相同。请求按workload到达约定投递，现有固定开发案例为同时到达，错峰资格仍须单独验证。物理卡由原任务deployment提供，来源、目标运行及后处理共用任务累计额度；模型、拓扑、请求、池参数及路由一致时才复用capture，失败记录保留，显式恢复到新尝试时不沿用旧预测和评价。

案例汇总保存在`results/validation/<配置>/<请求集>/<control>/<原始capture名>.json`，入口保持稳定；逐rank明细及执行/路由证据放在旁边的实际采集名目录，原生事实保存在指定capture目录。原生证据逐行核对workload专家ID与实际通信split、专家计数；批次人数取实际Prefill/Decode执行区域。预测证据检查节点完成、依赖就绪、资源独占、动作排空、交接边及输出序列。普通失败保留错误并继续其他rank；预算耗尽与中断单独记录，旧尝试保留为history。matched后清理已消费的派生工作区，其余状态保留必要材料；结构差异或partial均不代表性能精度结论。

`matrix.py`维护基础边界及调度/缓存组合的调查配方。已有模型来源元数据的准备目录可用`run.sh matrix --run data/YOUR_MATRIX`生成任务与边界输入；它只写输入，未运行模型，且拒绝改写已冻结清单。每个来源域的正式输入位于`runs/<域名>/inputs/task.json`，后续通过同一正式workflow启动，批量调查编排和汇总继续留在example。负例通过`error_terms`预先声明必要错误内容，只有原因匹配且未发布结果才算预期拒绝；其他失败记为error，进程崩溃不能充当输入拒绝。

矩阵额度区分初始清单和`adaptation_allowance`备用重建余量，二者合计为整轮上限。每套预定备用输入按一个最大任务预留，不能据此无限重跑；MoE/PD保留较大的原始材料存储额度。更换适配任务路径后，历史结果所引用的旧任务仍计入时间和尝试次数，不退还费用；当前和历史引用同一状态文件时只计一次。

`request_seed`同时影响通用请求组合和固定边界案例的输出长度；人数、prompt边界和单步结束保持，32/64步输出案例改用种子生成的prompt。专家路由在请求长度确定后生成。备用种子沿同一配方预先生成并核对，不能仅改ID或在看到结果后选输入；历史触发案例保留为回归。请求几何未重复只是输入检查，不证明独立验证或泛化通过。Decode优先标签只用于启用mixed的共同实例；EP非mixed及P/D分离仍检查自身chunk/cache机制。

调查配方仅生成原生控制后端的任务、条件和生命周期案例，取消轻量重复项及双后端比较。同一预测的多rank比较通过一次工作交接读取预测并建立共用索引，逐rank原子发布报告；中断或额度停止后保留已完成rank，不重新比较。实际分配动作推进请求驻留范围，准备输入只观察已有槽位、token及页身份，最终prepare不重复记账。生成配方不等于矩阵已经合格：冻结前仍须核对七模型及A–L组的实际机制触发、原生资格、留出、预算和完整覆盖。

正式workflow承担全部配置/request组合的给定耗时预测；指定真实对照另用本例真实边界时间生成结构预测，不复用给定耗时结果。不再为原先另一后端补生成同一正向预测条件。具名负例及机制条件保留，重复的中断/输入保护副本合并成一套原生生命周期检查。清单变更后重新统计次数与额度，旧双后端报告不改写；对应任务受阻时仍保持未覆盖。

原生案例的`minimum_coverage`分别约束`native`和`execution`证据。`coverage.by_actor`从实际模型区域记录总成员、Prefill/Decode成员峰值、未完成prompt的分块批次、混合批次及首次Prefill已有的前缀token；`observed`取各rank最大值，不把TP/PP重复工作累加。预测另从资源历史统计`cache_evictions`，原生未观察的淘汰不补成零。未达到门槛或缺少事实时，原本通过的案例降为partial，已有error/differences保留。覆盖只说明机制发生，不能替代结构、状态或Decode优先策略的正确性检查。

缓存案例要求原生和预测实际复用至少一页前缀。MoE的shared请求使用一致专家条件，arrivals保留路由变化；相同文本但专家条件不同不应共享KV。输入合同可先确认能否形成相同缓存键，实际命中仍以运行证据为准。

审查完成后在清单中记录`frozen_revision`、`adapter_revision`及sglang/control/modeling三个执行镜像的ID，并将`status`设为`frozen`，再执行：

```bash
example/automatic_graph/run.sh investigate --run data/YOUR_MATRIX
```

批量入口核对冻结代码与镜像，每个来源工作流完成后接着比较该域的预选原生目标，再处理边界输入和生命周期案例；不等待全部来源，也不根据目标结果补模板。整轮与各任务分别保留时间、尝试及存储上限，原生启动前检查声明卡的实际进程，已有占用或无法确认空闲时不启动Engine。独立案例出错后继续无依赖案例，缺来源的案例记blocked；预算停止/中断保留历史与费用，原命令可继续未完成案例，完成的结果不覆盖。

代码冻结覆盖src、example/automatic_graph、构建/安装及启动脚本、Docker、third_party和稳定设计约束；其他独立example及窗口文档不被本调查加载，不属于它的冻结资产。适配器仍按登记的adapter_revision单独核对，缩小无关目录范围不会放行核心、原生控制或评价修改。

真实目标也可提前采集，入口为：

```bash
example/automatic_graph/run.sh investigate --run data/YOUR_MATRIX --collect-targets
```

采集冻结在清单中使用`status=capture_frozen`及`capture_image`，先锁定输入与实际推理镜像；此状态禁止重生成矩阵及正式比较。功能/旧问题回归收口后，再登记正式比较的代码、评价和全部执行镜像，将状态改为`frozen`，保留既有事实、历史及费用。

此模式只使用固定输入及登记的原生镜像，不要求模板或编译仿真器，也不解释目标图或执行比较。观察包和SDK源码从镜像加载，允许CPU主线在其他开发任务中修改核心；已完成会话记为`captured`，不算独立验证通过，后续正式调查继续消费同一原始记录。单个正式任务可用`validate ... --capture-only`执行相同采集阶段。固定模型元数据在`inputs/model.json`，每次原生实际使用的镜像ID写入其`capture.json/native_image`；恢复不覆盖旧采集依据，正式比较镜像由整轮冻结记录确定。

需要单独取得清单中某个来源域时，使用`investigate --run data/YOUR_MATRIX --collect-sources JOB`，JOB取自矩阵清单。它调用正式任务的来源阶段，复用原案例编号、尝试上限及累计费用；来源完成不算工作流预测或独立比较通过。来源可隔离代表角色，真实目标采集必须有全部rank所需的物理卡；已有目标事实仍可离线复用。

一轮仍只有一个调查进程和一个任务状态写者：提前采集期间不能同时对同一调查根运行完整investigate、collect-sources或修改其任务。当前入口支持NPU队列与其他开发任务的CPU工作重叠，尚不在同一调查内同时派发采集和比较；它们共用同一份累计费用、锁及原始会话发布规则，不复制预算或启动多项NPU采集。来源角色集合另由任务状态的`source_image`绑定同一生产镜像；换镜像不能沿旧集合继续补角色，须按受影响来源域重新取得资格。

`results/summary.md`为整轮入口，`results/outcomes.json`保留逐次结果，`runs/<域>/`仍采用正式目录结构，额外边界及生命周期证据在`cases/`。调查保留原始SDK/runtime在capture原位置，覆盖普通任务的导出释放策略，不移动或改写原始目录。目标解释结果在`work/validation/<capture>/observed/source`，公开投影和完成时间在其父目录共用，预测明细按后端保存；matched后释放已消费的后端明细，全部计划后端matched再释放共用观察。partial、差异及停止的明细保留供归因，不把重新运行预测当作旧明细。整轮结束不自动判为结构全部通过，也不自动生成窗口审查结论。

| 文件 | 用途 |
| --- | --- |
| [probe.py](probe.py)、[state.py](state.py)、[allocator.py](allocator.py) | 观察公共操作、页池与分配机制 |
| [collectives.py](collectives.py) | 观察原生 collective 及本地接口 |
| [physical.py](physical.py) | 从保留的 trace/runtime 事实形成物理归属清单 |
| [structure.py](structure.py) | 比较独立观察与预测的批次、工作、几何、状态及端口 |
| [operations.py](operations.py) | 共用操作、端口及数据依赖核对，独立于serving配对和调度 |
| [execution.py](execution.py) | 核对 API、事件、队列排空与 stream 完成前沿；共用依赖路径查询，保持完整图中的跨rank/flow路径 |
| [evidence.py](evidence.py) | 检查预测时间线、输出及原生投递、路由与组批证据 |
| [request_state.py](request_state.py) | 核对共同动作顺序、请求输入范围和输出进度；另核对终态资源及逐rank索引绑定，不重演独占页池 |
| [resource_history.py](resource_history.py) | 核对所有者实际分配、共享绑定、传输、释放与终态，以及页归还时已提交模型/传输的设备消费者是否结束；不选择缓存策略 |
| [operands.py](operands.py) | 按当时资源绑定核对SGLang模型入口的KV位置、槽位和长度；检查同rank/layer的attention状态存储及跨动作设备路径 |
| [validation.py](validation.py)、[matrix.py](matrix.py)、[investigation.py](investigation.py) | 单案例对照、具名输入和冻结批量调查；复用正式workflow并与核心业务分开 |

`allocator.py --output <目录> --device npu --page-size 128 --need-sort`直接观察实际分配器，不启动模型或serving；省略`--need-sort`观察普通即时池，`--device cpu`只使用大分配的原生Python路径。原始公共操作保存在observations.json，summary.json只给成员数和三段分支覆盖；该诊断不自动取得物理归属或完整serving资格。

`evidence.py --source ... --input <目标输入> --delivery <同次capture/request-delivery.json> --output <报告>`读取一次连续workload的发送记录，检查请求分组覆盖、未提前发送和发送次序，并报告发送延迟；发现/重复轮次不能当作单次目标输入。验证编排在观察目录共用`native-evidence-<minimum-batch-members>.json`。投递检查只说明发送端行为，不观察调度器接收时刻、不改写arrival_ns，也不单独证明组批或结构正确；`--prediction`只消费预测结果，不接收`--delivery`。

这些入口读取公共结果和原生事实，不回流修改被验预测。物理工作有归属不等于全部依赖已恢复；不完整参数或关联保留 partial，不能按最近时间强配。结构来源 profiler 耗时不直接成为 CPU、设备、通信成本，dummy 权重也不提供数值或内容相关行为的正确性结论。

预测状态核对消费当前程序含`resource_history`的`--details`产物，以实际提交/完成序号处理同时间戳事件。请求检查统一核对到达、连续Prefill输入段、撤回重算及输出进度，首段可从已绑定的缓存前缀开始；中间PP结果不覆盖下一块已预约的范围。资源检查从实际变更核对分配快照、共享前缀、引用、淘汰、传输页对应及图完成后的回收，并与终态身份对照，不重演另一套分配/缓存策略。

启用mixed的原生控制另核对就绪Decode：已提交结果、尚未结束且无需继续Prefill的请求，进入其执行组/lane的就绪集合；下一次准备须选择该集合，释放/撤回则移出。含Prefill的批次还检查新增token与Decode成员合计不超过chunk预算，纯Decode不套用Prefill预算。`request_state.scheduling`和覆盖报告分别记录实际检查的`ready_decode_batches`、`chunk_budget_batches`；缺触发不能满足预定门槛。该检查消费现有结果与动作，不选择批次、不复制等待队列，也不证明任意负载下的公平性或与真实设备完全相同的调度次序。

资源历史只在details下生成，不记录完整token内容；按动作提交时的资源绑定与实际设备区间核对页归还晚于模型/传输消费者完成。消费者范围取模型子图及KV传输，不把轻量后端同一组合动作中的空闲页表维护当作模型工作。另沿消费者所在设备队列的末端工作核对到释放动作完成端口的依赖路径，这项检查不依赖给定耗时，覆盖的前沿数记录为`release_device_frontiers`。报告的matched仅表示已声明工作和资源记录自洽，不证明设备消费者已全部采到、实际KV数值或原生目标等价。旧明细缺少资源历史时须从保留模板和输入重新预测，旧报告保持原结论，不维护绕过资源检查的兼容路径。

PD交接检查覆盖每个生产端完成端口：它必须到达接收动作等待的至少一个完成端口，回调等待全部端口后才能推进。不能仅因某一个生产端有路径就宣布交接完整。同步传输API的成功返回及KV、元数据、接收和源释放顺序，与其内部硬件任务逐项对应分开报告。

`model_operands`进一步核对模型入口的整型内容与当时实际页绑定。attention按接口声明的持久读写存储检查同rank/layer绑定稳定，并检查前次设备完成至后次设备入口存在路径；这项路径检查不依赖某组节点耗时。它不执行backend内部的block table计算，也不验证KV浮点数值或PD搬运的逐字节地址；这些边界不能被入口索引一致或整存储依赖掩盖。

执行核对还沿目标图每个输入值的实际producer，检查生产调用的CPU返回能否到达使用调用的入口，报告在`value_production`。该项保证值描述符先产生再使用，不按原生批次顺序替换目标生产者；设备内容就绪及共享存储读写保护属于另外的关系，不能仅凭描述符检查就授予这些资格。

模型主体比较包含Prefill、Decode和Mixed及其子调用，按请求/token独立核对该范围的操作、设备工作、异步复制和完成关系，结果放在`comparisons[].model_body`。同一项的`checks`核对原生结果scope的槽位、请求效果、回收页身份及池大小；预约和准备的行访问使用实际批次工作信息。配置启用mixed不代表实际出现混合批次，仍须以观察或展开记录确认。

模型主体的完成关系只证明所选操作范围内的边。等待若涉及起点之前或其他工作，分别报告外部完成边及未绑定前沿；完整窗口可证明其中已对应的跨工作路径，状态错误或其他未知不随之消除。范围内缺边、多余边或外部资源连接错误仍判为差异。

整窗口覆盖原生明确记录的source serving批次及请求所属传输，包括无计算决定和终止清理，排除baseline与标为primitive的辅助原型。原始批次标签只用于范围资格及模型/结果定位，不将请求操作重挂到最后一个模型批次，也不再比较“整批所有CPU调用”的人为切片。整窗口统一核对全部公开操作与依赖；`observed_work`提供实际工作引用，同一调用的多个动作和多片段传输据此归并，再沿已知输入和数据生产者建立对应。没有模型批次标签的合格请求传输仍纳入设备/API总量；已知跨工作配对不接受，缺少依据继续保留未验证。

调用序列只能在既有锚点之间补充完整相等区间，输入冲突必须拒绝，新增对应也须重新核对输入。仅比较已对应调用的公开端口、逻辑依赖及CPU返回到消费入口的路径，再核对事件/队列/stream关系；原生调用者总顺序单列，不将独立请求的换序直接视为错误。原始设备及异步复制总量在完整窗口独立核对，未对应工作及其因果仍保留缺口。没有完整对应或工作归属依据时不能判为matched，已证明的差异仍报告differences；核对不改写原图，也不授予原生调度策略等价资格。

没有具名输入的接口按标准化实参树中的Tensor路径核对输入，额外捕获的私有存储没有位置合同。编码字典按实际键识别实参，关键字排列不改变身份。

赋值的目标引用与读取实参分开判断：同一个张量也出现在索引或载荷中时仍有读取，不能因输入ID去重只剩一项就将其视为只写。无法证明读取行范围时保留未知，不移除相应存储依赖。

共享request map与token relay复用同一按行追溯。实际索引必须对应当前批次已声明的request_slots列；request_slot回调优先使用明确请求身份，再使用可对应的批次成员，部分Mixed不能套用最终合批的全部成员。

视图描述符也可由已对应的生产调用证明：先定位同名输出端口或同一输出位置，再核对原生存储、偏移、shape、stride、dtype及device；不能任选同一调用的其他输出。存储版本更新不改变视图几何，写入到读取、CPU返回和设备完成仍独立核对。读取数据按选中行的实际写入者匹配，不把其他行的最后写入者当作当前数据来源。只去除有明确不相交范围证明的整存储约束，未知范围、别名或输入历史保留。`verified_storage_bindings`记录有证据的输入对应；不改写原始来源或目标图。

调用身份使用种类与名称，返回值和公开端口另行检查。`opaque_outputs`沿正式模板生产、展开及图压缩保留，表示存储内容而非张量视图；非具名接口与普通操作的张量结果不计这些内容别名，具名端口仍按其声明核对。原有输出、存储写入及依赖不删除，不根据目标缺shape推断opaque，也不按操作名跳过输出检查。

原生适配器可以在实际控制调用边界声明`control_only`。`controller_scope`单列这些调用及操作数；只有没有跨范围数据依赖、设备/runtime动作和共同通信合同时，比较视图才分开这部分控制工作。声明不成立时保留操作并报告差异；未标记的工作不自动排除。原图、模型通信、准备/释放和等待关系保持原样，状态轮询与不同策略的原生开销不因此获得预测资格。

Stream按每个操作内部的有序工作取得唯一对应，重复API按同一owner内的发生序号绑定真实节点。跨owner的排列由`same_stream_order`单列；必要等待前沿、事件连接和队列排空仍按实际图核对。

局部flow的必要完成关系在完整目标图中检查可达路径，`transitive_completion_links`报告经其他节点完成的关系。范围外前沿必须属于原生实际等待的stream，涉及外部工作时仍为partial；错误stream边界和额外内部完成关系继续报告差异。
