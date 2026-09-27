# AGENTS.md

本文件约束在本仓库中工作的代码代理，适用于根目录及其子目录；子目录存在更具体的 `AGENTS.md` 时，按其适用范围执行。用户明确指令优先。本文中的“必须”“不得”为硬性约束，“优先”为默认选择，偏离时应说明理由。

## 1. 开始工作前

- 先阅读 `README.md`、`docs/project_constraints.md`，再根据任务阅读 `docs/profiling_development.md` 或 `docs/modeling_development.md`。成本模型、验收和数据资产分别查阅 `docs/hicache_io_cost_model.md`、`docs/validation/hicache_validation.md`、`docs/work_progress.md`。
- 先检查 `git status --short` 和相关 diff。不得覆盖、回滚或清理用户已有改动；已有脏工作区不构成停止工作的理由。
- 搜索默认限定在自有代码目录；除非任务涉及上游实现，不递归搜索庞大的 `third_party/`、`data/` 和构建产物。
- 格式参数以 `pyproject.toml`、`.clang-format` 为准，业务边界以 `docs/project_constraints.md` 为准。发现文档与实现冲突，应指出并核实，不凭猜测扩展业务合同。

## 2. 仓库结构与职责

| 路径 | 职责与修改边界 |
| --- | --- |
| `scripts/{profile,model,run,build}.sh` | 用户入口、容器调度与构建；保持薄封装 |
| `scripts/internal/markov_internal/` | 配置、采集编排、校准、预测和评分工作流 |
| `src/profiling/` | 采集配置、manifest、时钟、Python probe 与 LD_PRELOAD hook |
| `src/modeling/trace_graph/` | C++ DAG、图变换、运行时建模和仿真 |
| `configs/` | 声明式实验、workload、profiling 与 modeling 配置 |
| `docker/` | SGLang、KTransformers、modeling 的环境定义 |
| `third_party/` | 上游依赖与子模块；仅在任务明确涉及依赖时修改，不做顺手格式化或升级 |
| `data/`、`build/` | 运行资产与构建输出；不得提交生成物，不得把现有数据视为可随意清理的临时文件 |

## 3. 通用代码约束

### 3.1 简洁、清晰、有层次

- **默认采用最直接的实现**：以简洁、可读、职责清楚为首要代码风格。主流程应能自上而下读懂业务步骤，同一函数内保持同一抽象层次，不交错编排逻辑与底层细节。
- **按语义分层**：上层组织流程，下层完成具体操作；仅在职责独立、逻辑复杂或确有复用时提取函数。不为每几行代码增加包装函数、转发类、工厂或策略层，不为尚不存在的需求设计扩展点。
- **保持控制流平直**：优先提前返回、清晰的局部变量和简单分支，减少嵌套、跨层跳转及隐式状态。不要以“简短”为由写难读的一行式，也不要为可读性设置机械的函数行数限制。
- **最小且完整的改动**：围绕任务修改必要代码、调用方和文档，不夹带无关重命名、全仓格式化或依赖升级。
- **单一职责**：解析、计划、执行、序列化保持职责清楚，无需机械拆成独立文件或类；CLI 不承载核心算法。优先复用现有模块，不为一次调用创建通用框架，也不把新业务堆进无边界的 `utils` 文件。
- **可读性优先**：使用表达业务含义的命名和显式控制流；不得压缩多语句、堆叠复杂三元表达式或删除必要注释来缩减行数。数值规则使用有单位的命名常量或配置，禁止无依据的魔法数。
- **代码块内按逻辑留白**：所有自有 Python、Shell、C/C++ 源文件及头文件（包括 `.py`、`.sh`、`.c`、`.cpp`、`.h`、`.hpp`）在函数、循环、条件分支等代码块内部，也应在不同逻辑步骤之间适当留一行空行，例如准备输入、执行计算、更新状态与输出结果之间。紧密相关的连续语句保持成组，不机械地每行或每固定行数插空行；长表达式按现有格式配置合理换行。不得为减少代码行数删除有助阅读的空行。随职责链重构落实，不另做无关的全仓格式化。
- **合同明确**：新增或修改接口时明确输入、返回值、可变性、单位、缺失值和错误行为。业务层使用已校验的对象，避免在每层重复解析原始字典或隐式转换类型。
- **缺失不等于零**：不得把缺失测量、无效 trace、失败预测或不可用成本默认记为 `0`；使用明确的缺失值或错误。区分空集合、未配置与不支持。
- **注释解释语义**：为非显然的算法、不变量、单位换算、生命周期和取舍写注释；接口文档说明合同，不复述语句。不保留注释掉的实现、无归属 TODO 或临时调试输出。
- **控制副作用**：导入模块不得启动服务、容器、采集或写入产物；运行副作用集中在显式执行路径。文件、进程、锁和句柄必须有清晰的释放路径。
- **依赖与兼容性**：优先标准库和已有依赖。新增依赖须说明必要性并同步相关环境定义；不为假想旧版本增加兼容分支。项目只维护当前实现，不引入内部版本号、摘要或冻结副本作为有效性证明。
- **规模约束**：遵守 active product surface 最终不超过 50,000 行的项目目标；通过消除死代码和重复逻辑控制规模，不迁移统计范围或牺牲可读性。

### 3.2 最少必要的校验与异常处理

- **默认不增加防御性代码**：不得仅因某种错误“理论上可能发生”，就新增 `raise`、`assert`、`throw`、空值检查、类型检查、存在性检查或兜底分支。每项检查都应对应真实的外部输入风险或明确的业务约束。
- **边界校验一次，内部信任合同**：必要校验集中在外部配置、文件或服务输入进入系统的位置；内部函数直接使用已建立的类型与不变量，不逐层重复检查，不对每次调用的返回值再加断言。
- **沿用自然失败路径**：标准库或现有接口已经能清楚报错时，直接调用；不先检查文件存在再打开，不捕获异常后原样重抛，不包装仅改写错误文本的多层异常。只有能恢复或确需补充定位信息时才处理异常，不吞错或伪造成功。
- **禁止无必要的完整性机制**：不得新增 SHA256、checksum、digest、文件指纹、重复读回比对、schema/版本握手或冻结副本来证明本地中间产物有效；也不得为这些机制增加 manifest 字段、校验脚本或依赖。只有任务明确要求的外部协议或实际安全边界需要时才引入。
- **用设计消除检查**：优先通过清晰的数据流、合适的类型和唯一的状态来源避免非法状态；不为同一事实维护多份标志、校验结果或一致性账本。新增类型本身也应有实际收益，不能成为另一层空壳封装。
- **必要检查保持精简**：保留输入边界、内存与资源安全、数据隔离和关键业务不变量所必需的检查；不以精简为由掩盖已知错误。修改相关代码时可删除已证明冗余的检查，不开展无关的全仓清理。

## 4. Python 规范

- 兼容 Python 3.10；不得使用仅更新版本支持的语法或标准库 API。
- 使用 4 空格缩进、双引号、LF 换行，行宽按 Ruff 的 120 列配置处理。导入放在模块顶部，按标准库、第三方、自有模块分组；可选依赖或循环依赖需要延迟导入时注明理由。
- 模块、函数和变量使用 `snake_case`，类使用 `PascalCase`，常量使用 `UPPER_SNAKE_CASE`，内部辅助函数使用前导 `_`。
- 新增或修改的业务函数应提供参数与返回类型；优先 `list[T]`、`dict[K, V]`、`T | None`。`Any` 限于 JSON、外部库等动态边界，进入核心逻辑后收窄类型。
- 固定结构数据优先沿用现有 `dataclass`、`TypedDict` 或领域类型，避免多层 `dict[str, Any]` 和位置元组跨模块传递。不得使用可变默认参数。
- 路径优先使用 `pathlib.Path`；文本显式使用 UTF-8。内部工作流 JSON 读写优先复用 `markov_internal.common.io`，保持 2 空格缩进和文件末尾换行。
- 子进程优先复用 `markov_internal.common` 的进程封装，使用参数数组，显式处理退出状态；不得将外部输入拼接进 shell 命令。日志沿用所属模块的现有日志设施。
- 仅在可选字段确有默认语义时使用 `.get(..., default)`；必填字段优先直接索引，让缺失自然报错，不额外堆叠 `if key not in ...: raise ...`，也不用连续 `.get()` 隐藏合同破坏。

## 5. C++ 规范

- 使用 C++23、CMake 3.20 及以上，保持关闭编译器语言扩展；新增源文件必须加入所属 CMake target。
- 使用仓库 `.clang-format`：4 空格、禁止 Tab、160 列；不得用个人 LLVM/Google 默认样式覆盖项目配置。
- 类型沿用 `PascalCase`，函数和变量沿用 `snake_case`，私有成员沿用后缀 `_`；修改已有接口时保持兼容的命名风格，不做无关批量更名。
- 公共头文件放在 `include/markov/trace_graph/` 对应层级，实现放在 `src/`；内部辅助头保留在实现目录。头文件使用 `#pragma once`，直接包含所用类型的声明头，不依赖偶然的传递包含。
- 沿用 `markov::trace_graph` 及所属模块命名空间；头文件不得使用 `using namespace`。非公开辅助实现放入匿名或专用 detail 命名空间。
- 使用 RAII 和明确所有权；优先值语义、容器及 `std::unique_ptr`，仅在确有共享所有权时使用 `std::shared_ptr`。裸指针、引用和 `std::string_view` 不得超出被引用对象的生命周期。
- 不修改的对象使用 `const`；只读大对象优先常量引用。只读字符串视图不得保存对临时字符串的引用；注意容器扩容导致的引用和迭代器失效。
- 明确整数符号、范围和单位；通过合适的类型与必要的边界校验防止 timestamp、duration、页数及字节数转换中的溢出和截断，不在内部算术操作周围重复加防御分支。不得以无符号下溢表达负时间或缺失值。
- 持续使用现有 DAG mutation、成本与仿真接口，禁止旁路直接修改内部存储以绕开校验。公共 API 注释说明节点身份、依赖、不变量和时间语义。
- 诊断代码通过 `TRACE_GRAPH_DEBUG` 隔离；不得让 Release 正确性依赖 Debug 分支。不得为内部正常路径批量补断言；移除冗余断言应基于已建立的合同，不能用于掩盖已知错误。

## 6. Shell 与配置规范

- Bash 入口沿用 `#!/usr/bin/env bash`、`set -euo pipefail`，从脚本位置解析仓库根目录，复用 `scripts/lib/common.sh`。
- 路径和变量展开必须正确引用，参数使用数组并通过 `"${args[@]}"` 传递；不得使用 `eval` 或拼接不可信命令。透传命令优先使用 `exec`。
- JSON 配置必须为合法 JSON；新增字段同步解析、校验、示例与相关文档。默认值必须来自明确语义，不硬编码本机绝对路径或实验身份。
- 不在配置、日志、测试夹具中写入密钥；不提交设备 trace、大型导出文件、编译产物或缓存。

## 7. 建模与采集的硬性边界

- profiling 与 modeling 仅通过 `profile_manifest.json` 正式交接；不得扫描目录猜输入。source DAG 只来自 source profile。
- predictor 与 target evaluator 必须保持物理隔离。预测不得打开 target profile/score root、导入 target oracle，或使用 target E2E、cell ID、workload ID 拟合参数。评分结果不得触发 refit、修改 readiness 或回写模型。
- effect/work plan 先于 cost plan；成本不得反向决定结构。patch 必须作为一次原子 mutation，明确 component ownership；并发、排队和重叠通过资源 lane 与依赖表达。
- HiCache I/O/control、Prefill、Decode、CPU residual gap 与 probe overhead 分开管理，不允许互相吸收误差。不得删除 gap 后仍声称完整 HTTP E2E，也不得将 `EXECUTED` 等同于精度验收通过。
- 时间变量和字段明确 `_ns`、`_us`、`_sec` 等单位；区分墙钟、线程 CPU 时间、源观测坐标与预测执行时间，避免嵌套区间重复计费。I/O 实际执行量与最终缓存可见量分别保存。
- 核心算法不得按 C1–C5、base 身份、workload 名称或 5×3/12/60 等实验计数分支；校准不得读取 target 真值来补缺失语义。
- 保留 SGLang 与 KTransformers 的同级框架能力；共享核心不得反向依赖 KTransformers 源码树。HiCache 只适用于 SGLang；框架无关的 NodeScale 保持可选且默认关闭。
- Python probe 默认 snapshot-free；profiling cells 严格串行。默认仅保留 compact summary 和复现输入，额外诊断通过显式开关启用。
- 清理数据前必须查阅 `docs/work_progress.md` 的资产说明；不得删除后续验证所需的 profile、forced-token、校准、target score 或稳定对照资产。

## 8. 验证要求

从仓库根目录执行，与修改范围匹配。先运行小范围语义检查，再进行必要的构建或关键 cell 验证；不得以旧产物代替当前工作树结果。

### 测试代码保持最少

- **默认不新增测试代码**：普通修改优先使用现有测试、静态检查、构建和必要的实际运行验证；不得为每次改动惯例性增加测试文件、测试入口、mock 框架或验证脚本。
- 仅在用户明确要求，或存在现有验证无法覆盖的重要行为风险时，新增最小的语义测试；说明其验证的具体风险。不得为简单封装、字段转发、显然的标准库行为或纯格式改动编写测试。
- 不编写镜像实现、重复断言、只检查源码文本或私有调用顺序的测试；不追求测试数量和覆盖率数字，不构造庞大夹具来验证简单逻辑。
- 临时排查脚本和中间 proof output 不进入正式代码；相关改动使测试失效或重复时一并精简。保留仍有实际价值的行为验证，不做无关的批量删除。
- 验证达到所需证据后停止；没有新改动、失败或未解决风险时，不反复运行或扩大测试范围。

### 静态检查

```bash
# 将示例路径替换为本次修改的实际文件；只格式化本次涉及的文件。
python3 -m ruff check path/to/changed.py
python3 -m ruff format --check path/to/changed.py
clang-format --dry-run --Werror path/to/changed.cpp path/to/changed.hpp
bash -n path/to/changed.sh
jq empty path/to/changed.json
git diff --check
```

Python 全量 lint 的现有入口为 `python3 -m ruff check scripts src/profiling`。不得为通过检查新增无理由的 `noqa`、关闭规则或修改格式配置。工具缺失或版本不兼容时报告原因，不声称检查通过。

### Python 行为检查

优先运行修改模块旁的现有 `unittest`，例如：

```bash
PYTHONPATH=src:scripts/internal:src/profiling/python_probe python3 -m unittest \
  profiling.test_profiler_clock markov_internal.modeling.test_backend
```

确有必要新增测试时，只验证本次涉及的关键外部行为或业务不变量，不扩成穷举式边界检查套件。纯文档改动无需新增测试，也无需运行设备采集或全量业务测试。

### C++ 构建与语义检查

使用 modeling 容器。Release 构建：

```bash
scripts/run.sh modeling -- bash -lc \
  'cmake -S src/modeling/trace_graph -B build/modeling/trace_graph-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DTRACE_GRAPH_DEBUG=OFF && cmake --build build/modeling/trace_graph-release --target trace_graph -j2'
```

修改 DAG、HiCache 或时序语义时，再执行显式 validation 构建及相关检查：

```bash
scripts/run.sh modeling -- bash -lc \
  'cmake -S src/modeling/trace_graph -B build/modeling/trace_graph-validation -G Ninja -DCMAKE_BUILD_TYPE=Debug -DTRACE_GRAPH_DEBUG=ON && cmake --build build/modeling/trace_graph-validation --target trace_graph trace_timing_check hicache_io_logic_check -j2 && build/modeling/trace_graph-validation/trace_timing_check && build/modeling/trace_graph-validation/hicache_io_logic_check'
```

这两个检查目标为 `EXCLUDE_FROM_ALL`，普通默认构建不会执行它们；当前不应将单独运行 `ctest` 视为完成验证。涉及构建配置或大型 C++ 重构时，使用新的构建目录完成 clean Release/validation 验证。hook 改动按目标框架使用现有 `scripts/build.sh` 与 `scripts/internal/hooks/build.sh` 流程验证。

模型或公式修改后按项目约束运行少量语义关键 cell；cost 简化前后使用同一组 cell。最终 60-cross 仅在公式固定后运行，不作为普通改动的默认检查。环境、设备或数据不足时说明未验证的范围。

## 9. 交付要求

- 提交结果前检查 diff，确认无无关文件、临时输出或意外格式化；不得代替用户回滚已有修改。
- 行为、入口、配置或数据合同变化时同步主文档。大型重构按项目约束维护短期 plan 与 append-only log，收口后合并稳定结论并删除临时副本。
- 交付说明应包含：修改内容、关键行为影响、实际执行的检查及结果、未验证范围和必要限制。不得把构建通过、命令完成或历史实验成绩当作本次精度证据。
