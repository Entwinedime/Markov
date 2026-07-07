/**
 * @file
 * @brief Chrome trace JSON 读取和 DAG debug trace 写出实现。
 */
#include "markov/trace_graph/io/chrome_trace_io.hpp"

#include "markov/trace_graph/core/logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <future>
#include <memory>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace markov::trace_graph::io {

using core::DagGraph;
using core::DagNode;
using core::escape_json;
using core::Logger;
using core::TraceEvent;

namespace chrome_trace_io_detail {

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
    const auto it = object.find(key);
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
    if (const auto it = object.find(key); it != object.end()) return scalar_to_string(*it);
    return fallback;
}

std::string lower_string(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool contains_ascii_case_insensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool matched = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            auto a = static_cast<char>(std::tolower(static_cast<unsigned char>(haystack[i + j])));
            auto b = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[j])));
            if (a != b) {
                matched = false;
                break;
            }
        }
        if (matched) return true;
    }
    return false;
}

bool ignored_duration_event_name(const std::string & name) {
    constexpr std::string_view ignored_names[] = { "Free", "Computing", "Communication", "Communication(Not Overlapped)" };
    return std::ranges::find(ignored_names, name) != std::end(ignored_names);
}

bool validation_only_event(const TraceEvent & event) {
    /**
     * @brief 判断事件是否只服务 validation/debug。
     *
     * 读取阶段保持 args 懒加载，因此这里只做 raw args JSON 的轻量 marker 判断。
     * 需要精确解释 validation evidence 的路径应在 diagnostics/validation 层显式解析。
     */
    auto raw = event.args_json_view();
    return contains_ascii_case_insensitive(raw, "state_snapshot") || contains_ascii_case_insensitive(raw, "oracle_state")
           || contains_ascii_case_insensitive(raw, "validation_diff");
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
    explicit TraceScanner(std::shared_ptr<const std::string> buffer, TraceReadOptions options = {})
        : buffer_(std::move(buffer)),
          p_(buffer_->data()),
          end_(buffer_->data() + buffer_->size()),
          options_(options) {}

    TraceScanner(std::shared_ptr<const std::string> buffer, const char * begin, const char * end, TraceReadOptions options)
        : buffer_(std::move(buffer)), p_(begin), end_(end), options_(options) {}

    TraceEvent parse_single_event(bool & valid) { return parse_event(valid); }

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
            auto marker = buffer_->find("\"traceEvents\"");
            if (marker != std::string::npos) {
                auto bracket = buffer_->find('[', marker);
                if (bracket != std::string::npos) {
                    p_ = buffer_->data() + bracket + 1;
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
         * 字符串 parser。
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
                if (*p_ == '"') parse_string();
                else if (*p_ == '{') {
                    ++depth;
                    ++p_;
                }
                else if (*p_ == '}') {
                    --depth;
                    ++p_;
                }
                else ++p_;
            }
            return;
        }
        if (*p_ == '[') {
            ++p_;
            int depth = 1;
            while (p_ < end_ && depth > 0) {
                if (*p_ == '"') parse_string();
                else if (*p_ == '[') {
                    ++depth;
                    ++p_;
                }
                else if (*p_ == ']') {
                    --depth;
                    ++p_;
                }
                else ++p_;
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
        if (p_ < end_ && *p_ == '"') raw = parse_string();
        else raw = parse_primitive();
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

    void parse_compound_value_literal(TraceEvent & event) {
        /**
         * @brief 保留 args 中对象/数组的原始 JSON 片段。
         *
         * scanner 只保证完整越过该值；需要参与建模的结构化字段由 TraceEvent 懒加载读取。
         */
        skip_ws();
        const char * start = p_;
        skip_value();
        const auto offset = static_cast<size_t>(start - buffer_->data());
        event.set_args_json_slice(buffer_, offset, static_cast<size_t>(p_ - start));
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

                if (key == "name") event.name = parse_scalar_value();
                else if (key == "cat") event.cat = parse_scalar_value();
                else if (key == "ph") {
                    event.ph = parse_scalar_value();
                    saw_phase = true;
                }
                else if (key == "ts") event.ts = parse_u64_value();
                else if (key == "dur") event.dur = parse_u64_value();
                else if (key == "pid") event.pid = parse_scalar_value();
                else if (key == "tid") event.tid = parse_scalar_value();
                else if (key == "event_id") event.event_id = parse_scalar_value();
                else if (key == "args") parse_compound_value_literal(event);
                else skip_value();
            }
            else { ++p_; }
            skip_ws();
            if (p_ < end_ && *p_ == ',') ++p_;
        }

        if (event.pid.empty()) event.pid = "-1";
        if (event.tid.empty()) event.tid = "-1";
        /**
         * @brief 当前后端只支持完整 duration event。
         *
         * metadata 和 flow event 不是可执行节点；如果未来需要使用 flow，需要单独解析为依赖边，
         * 而不是当作 0 时长节点。
         */
        valid = !event.name.empty()
                && ((saw_phase && event.ph == "X" && (options_.include_validation_only || !validation_only_event(event))
                     && (options_.include_ignored_duration || !ignored_duration_event_name(event.name)))
                    || (options_.include_metadata && event.ph == "M"));
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
            else { ++p_; }
            skip_ws();
            if (p_ < end_ && *p_ == ',') ++p_;
        }
        return events;
    }

    std::shared_ptr<const std::string> buffer_;
    const char * p_ = nullptr;
    const char * end_ = nullptr;
    TraceReadOptions options_;
};

struct ObjectRange {
    size_t begin = 0;
    size_t end = 0;
    size_t ordinal = 0;
};

std::string read_file_buffer(const std::string & filename, size_t threads) {
    std::ifstream ifs(filename, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) { throw std::runtime_error("Failed to open trace file: " + filename); }
    const auto end_pos = ifs.tellg();
    if (end_pos < 0) { throw std::runtime_error("Failed to determine trace file size: " + filename); }
    const auto size = static_cast<size_t>(end_pos);
    std::string buffer(size, '\0');
    if (size == 0) return buffer;

    constexpr size_t kMinParallelReadBytes = 64ull * 1024ull * 1024ull;
    const size_t partition_count = threads > 1 && size >= kMinParallelReadBytes ? std::min<size_t>(threads, (size + kMinParallelReadBytes - 1) / kMinParallelReadBytes) : 1;
    if (partition_count <= 1) {
        ifs.seekg(0, std::ios::beg);
        if (!ifs.read(buffer.data(), static_cast<std::streamsize>(size))) { throw std::runtime_error("Failed to read trace file: " + filename); }
        return buffer;
    }

    std::vector<std::future<void>> futures;
    futures.reserve(partition_count);
    const auto chunk = (size + partition_count - 1) / partition_count;
    for (size_t part = 0; part < partition_count; ++part) {
        const size_t begin = part * chunk;
        const size_t length = begin < size ? std::min(chunk, size - begin) : 0;
        if (length == 0) continue;
        futures.push_back(std::async(std::launch::async, [filename, &buffer, begin, length] {
            std::ifstream part_stream(filename, std::ios::binary);
            if (!part_stream.is_open()) { throw std::runtime_error("Failed to open trace file partition: " + filename); }
            part_stream.seekg(static_cast<std::streamoff>(begin), std::ios::beg);
            if (!part_stream.read(buffer.data() + begin, static_cast<std::streamsize>(length))) {
                throw std::runtime_error("Failed to read trace file partition: " + filename);
            }
        }));
    }
    for (auto & future : futures) future.get();
    return buffer;
}

std::string repair_trace_tail(std::string buffer) {
    auto trim_end = [](std::string & value) {
        while (!value.empty() && static_cast<unsigned char>(value.back()) <= ' ') value.pop_back();
    };
    trim_end(buffer);
    if (buffer.starts_with("{\"traceEvents\":[") && !buffer.ends_with("]}")) {
        auto end = buffer.rfind('}');
        if (end != std::string::npos) {
            buffer.resize(end + 1);
            trim_end(buffer);
            while (!buffer.empty() && buffer.back() == ',') {
                buffer.pop_back();
                trim_end(buffer);
            }
            buffer += "]}";
        }
    }
    else if (buffer.starts_with("[") && !buffer.ends_with("]")) {
        auto end = buffer.rfind('}');
        if (end != std::string::npos) {
            buffer.resize(end + 1);
            trim_end(buffer);
            while (!buffer.empty() && buffer.back() == ',') {
                buffer.pop_back();
                trim_end(buffer);
            }
            buffer += "\n]";
        }
    }
    return buffer;
}

size_t trace_array_begin(const std::string & buffer) {
    size_t pos = 0;
    while (pos < buffer.size() && static_cast<unsigned char>(buffer[pos]) <= ' ') ++pos;
    if (pos < buffer.size() && buffer[pos] == '[') return pos + 1;
    if (pos < buffer.size() && buffer[pos] == '{') {
        auto marker = buffer.find("\"traceEvents\"", pos);
        if (marker != std::string::npos) {
            auto bracket = buffer.find('[', marker);
            if (bracket != std::string::npos) return bracket + 1;
        }
    }
    return std::string::npos;
}

std::vector<ObjectRange> scan_event_object_ranges(const std::string & buffer) {
    std::vector<ObjectRange> ranges;
    auto pos = trace_array_begin(buffer);
    if (pos == std::string::npos) return ranges;
    size_t ordinal = 0;
    while (pos < buffer.size()) {
        while (pos < buffer.size() && buffer[pos] != '{' && buffer[pos] != ']') ++pos;
        if (pos >= buffer.size() || buffer[pos] == ']') break;
        const size_t begin = pos;
        int depth = 0;
        bool in_string = false;
        bool escape = false;
        for (; pos < buffer.size(); ++pos) {
            char c = buffer[pos];
            if (in_string) {
                if (escape) escape = false;
                else if (c == '\\') escape = true;
                else if (c == '"') in_string = false;
                continue;
            }
            if (c == '"') {
                in_string = true;
                continue;
            }
            if (c == '{') ++depth;
            else if (c == '}') {
                --depth;
                if (depth == 0) {
                    ++pos;
                    ranges.push_back(ObjectRange{ begin, pos, ordinal++ });
                    break;
                }
            }
        }
    }
    return ranges;
}

std::vector<std::vector<ObjectRange>> partition_ranges(const std::vector<ObjectRange> & ranges, size_t partition_count) {
    partition_count = std::max<size_t>(1, std::min(partition_count, ranges.size()));
    std::vector<std::vector<ObjectRange>> partitions(partition_count);
    const size_t chunk = (ranges.size() + partition_count - 1) / partition_count;
    for (size_t i = 0; i < ranges.size(); ++i) partitions[i / chunk].push_back(ranges[i]);
    return partitions;
}

std::vector<TraceEvent> parse_ranges(std::shared_ptr<const std::string> buffer, const std::vector<ObjectRange> & ranges, TraceReadOptions options) {
    std::vector<TraceEvent> events;
    events.reserve(ranges.size());
    for (const auto & range : ranges) {
        bool valid = false;
        TraceScanner scanner(buffer, buffer->data() + range.begin, buffer->data() + range.end, options);
        auto event = scanner.parse_single_event(valid);
        if (valid) {
            event.index = range.ordinal;
            events.push_back(std::move(event));
        }
    }
    return events;
}

std::string output_pid(const TraceEvent & event, const DagNode & node) {
    if (const auto it = node.attrs.find("sim_pid"); it != node.attrs.end()) return it->second;
    if (const auto it = node.attrs.find("gpuid"); it != node.attrs.end()) return it->second;
    if (!event.pid.empty()) return event.pid;
    return "-1";
}

std::string output_tid(const TraceEvent & event, const DagNode & node) {
    if (const auto it = node.attrs.find("sim_tid"); it != node.attrs.end()) return it->second;
    if (const auto it = node.attrs.find("tid"); it != node.attrs.end()) return it->second;
    if (!event.tid.empty()) return event.tid;
    return "-1";
}

bool is_integer_literal(const std::string & value) {
    if (value.empty()) return false;
    size_t start = value[0] == '-' ? 1 : 0;
    if (start == value.size()) return false;
    auto digits = value | std::views::drop(static_cast<std::ranges::range_difference_t<std::string>>(start));
    return std::ranges::all_of(digits, [](unsigned char c) { return std::isdigit(c); });
}

std::string json_scalar_literal(const std::string & value) {
    if (is_integer_literal(value)) return value;
    return Json(value).dump();
}

} // namespace chrome_trace_io_detail

using chrome_trace_io_detail::json_scalar_literal;
using chrome_trace_io_detail::output_pid;
using chrome_trace_io_detail::output_tid;
using chrome_trace_io_detail::TraceScanner;

std::vector<TraceEvent> read_chrome_trace(const std::string & filename) {
    return read_chrome_trace(filename, TraceReadOptions{});
}

std::vector<TraceEvent> read_chrome_trace(const std::string & filename, const TraceReadOptions & options) {
    /**
     * @brief 一次性读入文件后使用 streaming scanner 扫描。
     *
     * 这样仍需要一份文件大小的内存，但避免了 nlohmann::json DOM 的多倍放大。
     */
    std::string buffer = chrome_trace_io_detail::read_file_buffer(filename, std::max<size_t>(1, options.threads));
    if (options.auto_repair) buffer = chrome_trace_io_detail::repair_trace_tail(std::move(buffer));
    auto shared_buffer = std::make_shared<const std::string>(std::move(buffer));

    std::vector<TraceEvent> events;
    if (options.threads <= 1) {
        TraceScanner scanner(shared_buffer, options);
        events = scanner.parse();
    }
    else {
        auto ranges = chrome_trace_io_detail::scan_event_object_ranges(*shared_buffer);
        if (ranges.empty()) {
            TraceScanner scanner(shared_buffer, options);
            events = scanner.parse();
        }
        else {
            auto partitions = chrome_trace_io_detail::partition_ranges(ranges, options.threads);
            std::vector<std::future<std::vector<TraceEvent>>> futures;
            futures.reserve(partitions.size());
            for (auto & partition : partitions) {
                futures.push_back(std::async(std::launch::async, [shared_buffer, partition = std::move(partition), options] {
                    return chrome_trace_io_detail::parse_ranges(shared_buffer, partition, options);
                }));
            }
            for (auto & future : futures) {
                auto part = future.get();
                events.insert(events.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
            }
            std::ranges::sort(events, [](const TraceEvent & a, const TraceEvent & b) { return a.index < b.index; });
        }
    }

    Logger::instance().info() << "Read " << events.size() << " Chrome trace events from " << filename;
    return events;
}

void write_chrome_trace_dag(const std::string & filename, const DagGraph & graph) {
    /**
     * @brief DAG Chrome trace 是显式 debug 输出，不参与默认 prediction。
     */
    std::ofstream ofs(filename);
    if (!ofs.is_open()) { throw std::runtime_error("Failed to write Chrome trace DAG: " + filename); }
    ofs << "{\n  \"traceEvents\": [\n";

    /**
     * @brief 输出时把 simulation_start 平移到原 trace 的 real_min 附近。
     *
     * 这样便于在 Chrome viewer 中与原 trace 对照。
     */
    uint64_t real_min = 0;
    bool has_real_time = false;
    std::ranges::for_each(graph.events(), [&](const auto & event) {
        if (!has_real_time || event.ts < real_min) real_min = event.ts;
        has_real_time = true;
    });

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

} // namespace markov::trace_graph::io
