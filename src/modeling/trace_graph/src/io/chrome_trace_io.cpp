#include "trace_graph/io/chrome_trace_io.hpp"

#include "trace_graph/core/logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace TraceGraph {

namespace {

using Json = nlohmann::json;

/**
 * @brief 完整 JSON parser 路径使用的标量转换辅助函数。
 *
 * 当前主要服务于未来扩展；真实大 trace 读取路径使用下面的 TraceScanner，避免 DOM 内存开销。
 */
std::string scalar_to_string(const Json & value) {
    if (value.is_null()) return "";
    if (value.is_string()) return value.get<std::string>();
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    if (value.is_number_unsigned()) return std::to_string(value.get<uint64_t>());
    if (value.is_number_integer()) return std::to_string(value.get<int64_t>());
    if (value.is_number_float()) {
        std::ostringstream os;
        os << value.get<double>();
        return os.str();
    }
    return value.dump();
}

uint64_t json_u64(const Json & object, const std::string & key, uint64_t fallback = 0) {
    auto it = object.find(key);
    if (it == object.end() || it->is_null()) return fallback;
    if (it->is_number_unsigned()) return it->get<uint64_t>();
    if (it->is_number_integer()) {
        auto value = it->get<int64_t>();
        return value >= 0 ? static_cast<uint64_t>(value) : fallback;
    }
    if (it->is_number_float()) {
        auto value = it->get<double>();
        return value >= 0.0 ? static_cast<uint64_t>(value) : fallback;
    }
    if (it->is_string()) {
        try {
            double value = std::stod(it->get<std::string>());
            return value >= 0.0 ? static_cast<uint64_t>(value) : fallback;
        }
        catch (...) {
            return fallback;
        }
    }
    return fallback;
}

std::string json_string(const Json & object, const std::string & key, const std::string & fallback = "") {
    auto it = object.find(key);
    return it == object.end() ? fallback : scalar_to_string(*it);
}

std::string lower_string(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool validation_only_event(const TraceEvent & event) {
    /**
     * @brief 判断事件是否只服务 validation/debug。
     *
     * validation-only 事件属于辅助输入，不是业务执行路径。它们可以保留在 merged trace 中，
     * 但不能进入性能 DAG，否则 faithful replay 会被 state snapshot / oracle debug 污染。
     */
    auto fact_class = lower_string(event.arg("fact_class"));
    if (fact_class == "oracle_state" || fact_class == "debug_quality") return true;
    auto kind = lower_string(event.arg("event_kind"));
    return kind == "state_snapshot" || kind == "oracle_state" || kind == "validation_diff" || kind == "profiling_quality";
}

void flatten_args(const Json & value, const std::string & prefix, std::unordered_map<std::string, std::string> & args) {
    /**
     * @brief 递归展开对象字段，形如 Function-Args.stream。
     *
     * 当前 TraceScanner 没有调用这个函数；如果未来切回 DOM parser，应复用这里的展开规则。
     */
    if (!value.is_object()) return;
    for (const auto & item : value.items()) {
        const auto key = prefix.empty() ? item.key() : prefix + "." + item.key();
        args[key] = scalar_to_string(item.value());
        if (item.value().is_object()) flatten_args(item.value(), key, args);
    }
}

Json load_json_file(const std::string & filename) {
    /**
     * @brief 加载完整 JSON DOM。
     *
     * 当前未在 read_chrome_trace 主路径使用，保留是为了调试小 JSON 或后续 schema 检查。
     */
    std::ifstream ifs(filename);
    if (!ifs.is_open()) { throw std::runtime_error("Failed to open trace file: " + filename); }
    try {
        return Json::parse(ifs);
    }
    catch (const std::exception & e) {
        throw std::runtime_error("Failed to parse Chrome trace JSON '" + filename + "': " + e.what());
    }
}

class TraceScanner {
  public:
    explicit TraceScanner(std::string buffer) : buffer_(std::move(buffer)), p_(buffer_.data()), end_(buffer_.data() + buffer_.size()) {}

    std::vector<TraceEvent> parse() {
        /**
         * @brief 支持 traceEvents 数组本身和 {"traceEvents": [...]} 对象两种输入形态。
         *
         * 其他顶层字段会被跳过，因为 C++ 后端只需要 duration events。
         */
        skip_ws();
        if (p_ >= end_) return {};
        if (*p_ == '[') {
            ++p_;
            return parse_array();
        }

        /**
         * @brief 部分 Chrome trace 输入使用 {"traceEvents": [...]}，真实 merged trace 通常是数组。
         */
        if (*p_ == '{') {
            auto marker = buffer_.find("\"traceEvents\"");
            if (marker != std::string::npos) {
                auto bracket = buffer_.find('[', marker);
                if (bracket != std::string::npos) {
                    p_ = buffer_.data() + bracket + 1;
                    return parse_array();
                }
            }
        }
        return {};
    }

  private:
    void skip_ws() {
        while (p_ < end_ && static_cast<unsigned char>(*p_) <= ' ') ++p_;
    }

    std::string parse_string() {
        /**
         * @warning 轻量 scanner 只跳过转义字符，不真正反转义。
         *
         * 这对当前构图字段可能足够；如果 name/args 中需要精确保留转义内容，需要改成完整 JSON
         * string parser。
         */
        if (p_ >= end_ || *p_ != '"') return "";
        ++p_;
        const char * start = p_;
        while (p_ < end_ && *p_ != '"') {
            if (*p_ == '\\' && p_ + 1 < end_) ++p_;
            ++p_;
        }
        std::string value(start, p_ - start);
        if (p_ < end_) ++p_;
        return value;
    }

    std::string parse_primitive() {
        const char * start = p_;
        while (p_ < end_ && *p_ != ',' && *p_ != '}' && *p_ != ']' && static_cast<unsigned char>(*p_) > ' ') ++p_;
        return std::string(start, p_ - start);
    }

    void skip_value() {
        /**
         * @brief 跳过不关心的 JSON value，支持嵌套对象/数组。
         *
         * 该函数用于顶层未知字段；args 解析当前只读取 scalar。
         */
        skip_ws();
        if (p_ >= end_) return;
        if (*p_ == '"') {
            parse_string();
            return;
        }
        if (*p_ == '{') {
            ++p_;
            int depth = 1;
            while (p_ < end_ && depth > 0) {
                if (*p_ == '"')
                    parse_string();
                else if (*p_ == '{') {
                    ++depth;
                    ++p_;
                }
                else if (*p_ == '}') {
                    --depth;
                    ++p_;
                }
                else
                    ++p_;
            }
            return;
        }
        if (*p_ == '[') {
            ++p_;
            int depth = 1;
            while (p_ < end_ && depth > 0) {
                if (*p_ == '"')
                    parse_string();
                else if (*p_ == '[') {
                    ++depth;
                    ++p_;
                }
                else if (*p_ == ']') {
                    --depth;
                    ++p_;
                }
                else
                    ++p_;
            }
            return;
        }
        parse_primitive();
    }

    uint64_t parse_u64_value() {
        /**
         * @brief Chrome trace 的 ts/dur 可能是整数、浮点或字符串；这里统一截断到 uint64。
         */
        std::string raw;
        if (p_ < end_ && *p_ == '"')
            raw = parse_string();
        else
            raw = parse_primitive();
        try {
            double value = std::stod(raw);
            return value >= 0.0 ? static_cast<uint64_t>(value) : 0;
        }
        catch (...) {
            return 0;
        }
    }

    std::string parse_scalar_value() {
        if (p_ < end_ && *p_ == '"') return parse_string();
        return parse_primitive();
    }

    std::string parse_compound_value_literal() {
        /**
         * @brief 保留 args 中对象/数组的原始 JSON 片段。
         *
         * scanner 只保证完整越过该值；需要参与建模的结构化字段应由 trace_merger 展平成稳定标量 key。
         */
        skip_ws();
        const char * start = p_;
        skip_value();
        return std::string(start, p_ - start);
    }

    void parse_args(std::unordered_map<std::string, std::string> & args) {
        /**
         * @brief 当前 args 解析只保存一层 key -> string。
         *
         * 标量直接收敛成字符串，对象/数组保留原始 JSON 片段，避免结构化值打乱后续字段扫描。
         */
        if (p_ >= end_ || *p_ != '{') {
            skip_value();
            return;
        }
        ++p_;
        while (p_ < end_) {
            skip_ws();
            if (p_ >= end_ || *p_ == '}') {
                if (p_ < end_) ++p_;
                break;
            }
            if (*p_ == '"') {
                auto key = parse_string();
                skip_ws();
                if (p_ < end_ && *p_ == ':') ++p_;
                skip_ws();
                if (p_ < end_ && (*p_ == '{' || *p_ == '['))
                    args[key] = parse_compound_value_literal();
                else
                    args[key] = parse_scalar_value();
            }
            else {
                ++p_;
            }
            skip_ws();
            if (p_ < end_ && *p_ == ',') ++p_;
        }
    }

    TraceEvent parse_event(bool & valid) {
        /**
         * @brief 只解析构图必须字段；未知字段会被 skip_value 跳过。
         */
        TraceEvent event;
        valid = false;
        if (p_ >= end_ || *p_ != '{') return event;
        ++p_;
        bool saw_phase = false;
        while (p_ < end_) {
            skip_ws();
            if (p_ >= end_ || *p_ == '}') {
                if (p_ < end_) ++p_;
                break;
            }
            if (*p_ == '"') {
                auto key = parse_string();
                skip_ws();
                if (p_ < end_ && *p_ == ':') ++p_;
                skip_ws();

                if (key == "name")
                    event.name = parse_scalar_value();
                else if (key == "cat")
                    event.cat = parse_scalar_value();
                else if (key == "ph") {
                    event.ph = parse_scalar_value();
                    saw_phase = true;
                }
                else if (key == "ts")
                    event.ts = parse_u64_value();
                else if (key == "dur")
                    event.dur = parse_u64_value();
                else if (key == "pid")
                    event.pid = parse_scalar_value();
                else if (key == "tid")
                    event.tid = parse_scalar_value();
                else if (key == "event_id")
                    event.event_id = parse_scalar_value();
                else if (key == "args")
                    parse_args(event.args);
                else
                    skip_value();
            }
            else {
                ++p_;
            }
            skip_ws();
            if (p_ < end_ && *p_ == ',') ++p_;
        }

        if (event.pid.empty()) event.pid = "-1";
        if (event.tid.empty()) event.tid = "-1";
        /**
         * @brief 把顶层 pid/tid 也写入 args，方便后续统一走 event.arg()。
         *
         * DagBuilder::lane_key 直接读取 event.tid，不依赖这里的 args["tid"] fallback。
         */
        event.args["pid"] = event.pid;
        event.args["tid"] = event.tid;
        /**
         * @brief 当前后端只支持完整 duration event。
         *
         * metadata 和 flow event 不是可执行节点；如果未来需要使用 flow，需要单独解析为依赖边，
         * 而不是当作 0 时长节点。
         */
        valid = saw_phase && event.ph == "X" && !event.name.empty() && !validation_only_event(event) && event.name != "Free" && event.name != "Computing" &&
                event.name != "Communication" && event.name != "Communication(Not Overlapped)";
        return event;
    }

    std::vector<TraceEvent> parse_array() {
        /**
         * @brief 只把 valid 的 duration event 放入结果，event.index 使用过滤后的顺序。
         */
        std::vector<TraceEvent> events;
        while (p_ < end_) {
            skip_ws();
            if (p_ >= end_ || *p_ == ']') break;
            if (*p_ == '{') {
                bool valid = false;
                auto event = parse_event(valid);
                if (valid) {
                    event.index = events.size();
                    events.push_back(std::move(event));
                }
            }
            else {
                ++p_;
            }
            skip_ws();
            if (p_ < end_ && *p_ == ',') ++p_;
        }
        return events;
    }

    std::string buffer_;
    const char * p_ = nullptr;
    const char * end_ = nullptr;
};

std::string output_pid(const TraceEvent & event, const DagNode & node) {
    if (node.attrs.count("sim_pid")) return node.attrs.at("sim_pid");
    if (node.attrs.count("gpuid")) return node.attrs.at("gpuid");
    if (!event.pid.empty()) return event.pid;
    return "-1";
}

std::string output_tid(const TraceEvent & event, const DagNode & node) {
    if (node.attrs.count("sim_tid")) return node.attrs.at("sim_tid");
    if (node.attrs.count("tid")) return node.attrs.at("tid");
    if (!event.tid.empty()) return event.tid;
    return "-1";
}

bool is_integer_literal(const std::string & value) {
    if (value.empty()) return false;
    size_t start = value[0] == '-' ? 1 : 0;
    if (start == value.size()) return false;
    for (size_t i = start; i < value.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(value[i]))) return false;
    }
    return true;
}

std::string json_scalar_literal(const std::string & value) {
    if (is_integer_literal(value)) return value;
    return Json(value).dump();
}

} // namespace

std::vector<TraceEvent> read_chrome_trace(const std::string & filename) {
    /**
     * @brief 一次性读入文件后使用 streaming scanner 扫描。
     *
     * 这样仍需要一份文件大小的内存，但避免了 nlohmann::json DOM 的多倍放大。
     */
    std::ifstream ifs(filename, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) { throw std::runtime_error("Failed to open trace file: " + filename); }
    auto size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    std::string buffer(static_cast<size_t>(size), '\0');
    if (!ifs.read(buffer.data(), size)) { throw std::runtime_error("Failed to read trace file: " + filename); }

    TraceScanner scanner(std::move(buffer));
    auto events = scanner.parse();
    Logger::instance().info() << "Read " << events.size() << " Chrome trace events from " << filename;
    return events;
}

void write_chrome_trace_dag(const std::string & filename, const DagGraph & graph, bool full_output) {
    /**
     * @brief DAG Chrome trace 是显式 debug 输出，不参与默认 prediction。
     */
    std::ofstream ofs(filename);
    if (!ofs.is_open()) { throw std::runtime_error("Failed to write Chrome trace DAG: " + filename); }
    ofs << "{\n  \"traceEvents\": [\n";
    if (!full_output) {
        ofs << "\n  ]\n}\n";
        return;
    }

    /**
     * @brief 输出时把 simulation_start 平移到原 trace 的 real_min 附近。
     *
     * 这样便于在 Chrome viewer 中与原 trace 对照。
     */
    uint64_t real_min = 0;
    bool has_real_time = false;
    for (const auto & event : graph.events()) {
        if (!has_real_time || event.ts < real_min) real_min = event.ts;
        has_real_time = true;
    }

    bool first = true;
    for (const auto & node : graph.nodes()) {
        /**
         * @brief 每个 DAG node 输出为一个 duration event。
         */
        const auto & event = graph.event_for_node(node.id);
        if (!first) ofs << ",\n";
        first = false;

        auto pid = json_scalar_literal(output_pid(event, node));
        auto tid = json_scalar_literal(output_tid(event, node));

        ofs << "    {\n"
            << "      \"name\": \"node_" << escape_json(event.name) << "\",\n"
            << "      \"cat\": \"sim_" << escape_json(event.cat) << "\",\n"
            << "      \"ph\": \"X\",\n"
            << "      \"ts\": " << (real_min + node.simulation_start) << ",\n"
            << "      \"dur\": " << node.duration << ",\n"
            << "      \"pid\": " << pid << ",\n"
            << "      \"tid\": " << tid << ",\n"
            << "      \"args\": {\"node_id\":" << node.id << ",\"event_index\":" << node.event_index << "}\n"
            << "    }";
    }

    for (const auto & edge : graph.edges()) {
        /**
         * @brief 每条 DAG edge 输出为 Chrome trace flow start/end 事件，方便可视化依赖。
         */
        const auto & src_event = graph.event_for_node(edge.src);
        const auto & dst_event = graph.event_for_node(edge.dst);
        const auto & src_node = graph.node(edge.src);
        const auto & dst_node = graph.node(edge.dst);
        auto edge_id = "edge_" + std::to_string(edge.src) + "_" + std::to_string(edge.dst);
        auto src_pid = json_scalar_literal(output_pid(src_event, src_node));
        auto src_tid = json_scalar_literal(output_tid(src_event, src_node));
        auto dst_pid = json_scalar_literal(output_pid(dst_event, dst_node));
        auto dst_tid = json_scalar_literal(output_tid(dst_event, dst_node));

        ofs << ",\n    {\n"
            << "      \"name\": \"edge\",\n"
            << "      \"cat\": \"edge\",\n"
            << "      \"ph\": \"s\",\n"
            << "      \"ts\": " << (real_min + src_node.simulation_start + src_node.duration) << ",\n"
            << "      \"pid\": " << src_pid << ",\n"
            << "      \"tid\": " << src_tid << ",\n"
            << "      \"id\": \"" << edge_id << "\",\n"
            << "      \"args\": {\"type\": \"" << static_cast<int>(edge.kind) << "\"}\n"
            << "    },\n"
            << "    {\n"
            << "      \"name\": \"edge\",\n"
            << "      \"cat\": \"edge\",\n"
            << "      \"ph\": \"t\",\n"
            << "      \"ts\": " << (real_min + dst_node.simulation_start) << ",\n"
            << "      \"pid\": " << dst_pid << ",\n"
            << "      \"tid\": " << dst_tid << ",\n"
            << "      \"id\": \"" << edge_id << "\",\n"
            << "      \"args\": {\"type\": \"" << static_cast<int>(edge.kind) << "\"}\n"
            << "    }";
    }
    ofs << "\n  ]\n}\n";
}

} // namespace TraceGraph
