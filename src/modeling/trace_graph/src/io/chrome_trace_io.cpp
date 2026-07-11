/**
 * @file
 * @brief Streaming Chrome trace reader and explicit DAG-trace writer implementation.
 */
#include "markov/trace_graph/io/chrome_trace_io.hpp"

#include "markov/trace_graph/core/logger.hpp"
#include "markov/trace_graph/core/numeric.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
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

bool contains_validation_marker(std::string_view text) {
    constexpr std::string_view kStateSnapshot = "state_snapshot";
    constexpr std::string_view kOracleState = "oracle_state";
    constexpr std::string_view kValidationDiff = "validation_diff";
    for (size_t offset = 0; offset < text.size(); ++offset) {
        std::string_view marker;
        switch (static_cast<char>(std::tolower(static_cast<unsigned char>(text[offset])))) {
        case 's':
            marker = kStateSnapshot;
            break;
        case 'o':
            marker = kOracleState;
            break;
        case 'v':
            marker = kValidationDiff;
            break;
        default:
            continue;
        }
        if (marker.size() > text.size() - offset) continue;
        bool matched = true;
        for (size_t index = 1; index < marker.size(); ++index) {
            const auto actual = static_cast<char>(std::tolower(static_cast<unsigned char>(text[offset + index])));
            if (actual != marker[index]) {
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
    // Filtering keeps args lazy and performs one raw scan. Diagnostics parse exact
    // evidence only after an event is intentionally retained for validation.
    return contains_validation_marker(event.args_json_view());
}

class TraceScanner {
public:
    explicit TraceScanner(std::shared_ptr<const std::string> buffer, TraceReadOptions options = {})
        : buffer_(std::move(buffer)),
          p_(buffer_->data()),
          end_(buffer_->data() + buffer_->size()),
          options_(options) {}

    std::vector<TraceEvent> parse_event_sequence(const char * begin, const char * end) {
        p_ = begin;
        end_ = end;
        return parse_array();
    }

    std::vector<TraceEvent> parse() {
        // Accept either a bare event array or the standard object wrapper. Other top-level
        // fields are skipped because they do not participate in DAG construction.
        skip_ws();
        if (p_ >= end_) return {};
        if (*p_ == '[') {
            ++p_;
            return parse_array();
        }

        // Locate the event array in object-style Chrome traces without building a DOM.
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

    std::string_view parse_string_view(std::string & decoded) {
        if (p_ >= end_ || *p_ != '"') return {};
        const char * literal_start = p_;
        ++p_;
        const char * start = p_;
        bool escaped = false;
        while (p_ < end_ && *p_ != '"') {
            if (*p_ == '\\' && p_ + 1 < end_) {
                escaped = true;
                ++p_;
            }
            ++p_;
        }
        const char * content_end = p_;
        if (p_ < end_) ++p_;
        if (escaped) {
            try {
                decoded = Json::parse(std::string_view(literal_start, static_cast<size_t>(p_ - literal_start))).get<std::string>();
                return decoded;
            }
            catch (const Json::exception &) {
                decoded.clear();
            }
        }
        return std::string_view(start, static_cast<size_t>(content_end - start));
    }

    std::string parse_string() {
        std::string decoded;
        const auto value = parse_string_view(decoded);
        return std::string(value);
    }

    void skip_string() {
        if (p_ >= end_ || *p_ != '"') return;
        ++p_;
        bool escaped = false;
        while (p_ < end_) {
            const char value = *p_++;
            if (escaped) {
                escaped = false;
                continue;
            }
            if (value == '\\') {
                escaped = true;
                continue;
            }
            if (value == '"') return;
        }
    }

    std::string_view parse_primitive_view() {
        const char * start = p_;
        while (p_ < end_ && *p_ != ',' && *p_ != '}' && *p_ != ']' && static_cast<unsigned char>(*p_) > ' ') ++p_;
        return std::string_view(start, static_cast<size_t>(p_ - start));
    }

    void skip_value() {
        // Skip an arbitrary nested JSON value while preserving scanner synchronization.
        skip_ws();
        if (p_ >= end_) return;
        if (*p_ == '"') {
            skip_string();
            return;
        }
        if (*p_ == '{') {
            ++p_;
            int depth = 1;
            while (p_ < end_ && depth > 0) {
                if (*p_ == '"') skip_string();
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
                if (*p_ == '"') skip_string();
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
        (void)parse_primitive_view();
    }

    uint64_t parse_u64_value(std::string_view field) {
        // Chrome producers emit timestamps and durations as integers, decimals, or numeric
        // strings. Fractional microseconds are truncated consistently by `parse_u64`.
        const auto raw = p_ < end_ && *p_ != '"' ? parse_primitive_view() : std::string_view{};
        const auto value = raw.empty() ? core::parse_u64(parse_string()) : core::parse_u64(raw);
        if (!value) throw std::runtime_error("Chrome trace field '" + std::string(field) + "' must be a non-negative uint64-compatible value");
        return *value;
    }

    std::string parse_scalar_value() {
        if (p_ < end_ && *p_ == '"') return parse_string();
        return std::string(parse_primitive_view());
    }

    void parse_compound_value_literal(TraceEvent & event) {
        // Retain the complete raw args value as a shared buffer slice. Structured consumers
        // perform key-local lazy lookups and materialize the full map only on explicit demand.
        skip_ws();
        const char * start = p_;
        skip_value();
        const auto offset = static_cast<size_t>(start - buffer_->data());
        event.set_args_json_slice(buffer_,
                                  core::TraceByteRange{
                                      .offset = offset,
                                      .length = static_cast<size_t>(p_ - start),
                                  });
    }

    void parse_phase(TraceEvent & event, bool & saw_phase) {
        const auto phase = parse_scalar_value();
        event.ph = phase.size() == 1 ? phase.front() : '\0';
        saw_phase = true;
    }

    void parse_event_field(std::string_view key, TraceEvent & event, bool & saw_phase) {
        if (key == "name") event.name = parse_scalar_value();
        else if (key == "cat") event.cat = parse_scalar_value();
        else if (key == "ph") parse_phase(event, saw_phase);
        else if (key == "ts") event.ts = parse_u64_value("ts");
        else if (key == "dur") event.dur = parse_u64_value("dur");
        else if (key == "pid") event.pid = parse_scalar_value();
        else if (key == "tid") event.tid = parse_scalar_value();
        else if (key == "event_id") event.event_id = parse_scalar_value();
        else if (key == "args") parse_compound_value_literal(event);
        else skip_value();
    }

    void parse_event_member(TraceEvent & event, bool & saw_phase) {
        if (*p_ != '"') {
            ++p_;
            return;
        }
        std::string decoded_key;
        const auto key = parse_string_view(decoded_key);
        skip_ws();
        if (p_ < end_ && *p_ == ':') ++p_;
        skip_ws();
        parse_event_field(key, event, saw_phase);
    }

    [[nodiscard]] bool consume_event_end() {
        skip_ws();
        if (p_ < end_ && *p_ != '}') return false;
        if (p_ < end_) ++p_;
        return true;
    }

    [[nodiscard]] bool accepts_event(const TraceEvent & event, bool saw_phase) const {
        if (event.name.empty()) return false;
        const bool executable = saw_phase && event.ph == 'X' && (options_.include_validation_only || !validation_only_event(event))
                                && (options_.include_ignored_duration || !ignored_duration_event_name(event.name));
        const bool metadata = options_.include_metadata && event.ph == 'M';
        return executable || metadata;
    }

    TraceEvent parse_event(bool & valid) {
        // Parse only the top-level event identity and timing fields required by the backend.
        TraceEvent event;
        valid = false;
        if (p_ >= end_ || *p_ != '{') return event;
        ++p_;
        bool saw_phase = false;
        while (p_ < end_) {
            if (consume_event_end()) break;
            parse_event_member(event, saw_phase);
            skip_ws();
            if (p_ < end_ && *p_ == ',') ++p_;
        }

        if (event.pid.empty()) event.pid = "-1";
        if (event.tid.empty()) event.tid = "-1";
        // Metadata is retained only when requested for channel association. Flow records are
        // not executable nodes; supporting them requires explicit dependency reconstruction.
        valid = accepts_event(event, saw_phase);
        return event;
    }

    std::vector<TraceEvent> parse_array() {
        // Event indices follow the stable order of retained records, not raw JSON positions.
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

struct EventByteRange {
    size_t begin = 0;
    size_t end = 0;
};

class EventBoundaryScanner {
public:
    EventBoundaryScanner(const std::string & buffer, size_t begin) : buffer_(buffer), pos_(begin) {}

    [[nodiscard]] std::optional<EventByteRange> next() {
        while (pos_ < buffer_.size() && buffer_[pos_] != '{' && buffer_[pos_] != ']') ++pos_;
        if (pos_ >= buffer_.size() || buffer_[pos_] == ']') return std::nullopt;
        const auto begin = pos_;
        const auto end = scan_object_end();
        if (!end) {
            malformed_ = true;
            return std::nullopt;
        }
        return EventByteRange{ .begin = begin, .end = *end };
    }

    [[nodiscard]] bool malformed() const { return malformed_; }

private:
    [[nodiscard]] std::optional<size_t> scan_object_end() {
        int depth = 0;
        bool in_string = false;
        bool escape = false;
        for (; pos_ < buffer_.size(); ++pos_) {
            const char value = buffer_[pos_];
            if (in_string) {
                if (escape) escape = false;
                else if (value == '\\') escape = true;
                else if (value == '"') in_string = false;
                continue;
            }
            if (value == '"') {
                in_string = true;
                continue;
            }
            if (value == '{') ++depth;
            else if (value == '}' && --depth == 0) return ++pos_;
        }
        return std::nullopt;
    }

    const std::string & buffer_;
    size_t pos_ = 0;
    bool malformed_ = false;
};

std::string read_file_buffer(const std::string & filename, size_t threads) {
    std::ifstream ifs(filename, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) { throw std::runtime_error("Failed to open trace file: " + filename); }
    const auto end_pos = ifs.tellg();
    if (end_pos < 0) { throw std::runtime_error("Failed to determine trace file size: " + filename); }
    const auto size = static_cast<size_t>(end_pos);
    std::string buffer(size, '\0');
    if (size == 0) return buffer;
    if (size > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("Trace file is too large for std::ifstream: " + filename);
    }

    constexpr size_t kMinParallelReadBytes = 64ull * 1'024ull * 1'024ull;
    const auto useful_partitions = size / kMinParallelReadBytes + (size % kMinParallelReadBytes == 0 ? 0 : 1);
    const size_t partition_count = threads > 1 && size >= kMinParallelReadBytes ? std::min<size_t>(threads, useful_partitions) : 1;
    if (partition_count <= 1) {
        ifs.seekg(0, std::ios::beg);
        if (!ifs.read(buffer.data(), static_cast<std::streamsize>(size))) { throw std::runtime_error("Failed to read trace file: " + filename); }
        return buffer;
    }

    std::vector<std::future<void>> futures;
    futures.reserve(partition_count);
    const auto chunk = size / partition_count + (size % partition_count == 0 ? 0 : 1);
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

/** @brief Byte geometry used to place deterministic event-partition boundaries. */
struct EventPartitionPlan {
    size_t array_begin = 0;
    size_t payload_bytes = 0;
    size_t partition_count = 1;

    /** @brief Returns the balanced byte target for one partition boundary. */
    [[nodiscard]] size_t target(size_t partition_index) const {
        const auto quotient = payload_bytes / partition_count;
        const auto remainder = payload_bytes % partition_count;
        return array_begin + quotient * partition_index + (remainder * partition_index) / partition_count;
    }
};

void advance_partition_index(size_t event_begin, const EventPartitionPlan & plan, size_t & next_partition) {
    do { ++next_partition; } while (next_partition < plan.partition_count && event_begin >= plan.target(next_partition));
}

std::vector<EventByteRange> partition_event_array(const std::string & buffer, size_t requested_partitions) {
    std::vector<EventByteRange> partitions;
    const auto array_begin = trace_array_begin(buffer);
    if (array_begin == std::string::npos) return partitions;
    requested_partitions = std::max<size_t>(1, requested_partitions);
    if (requested_partitions == 1)
        return {
            EventByteRange{ .begin = array_begin, .end = buffer.size() }
        };

    const auto payload_bytes = buffer.size() - array_begin;
    const auto plan = EventPartitionPlan{
        .array_begin = array_begin,
        .payload_bytes = payload_bytes,
        .partition_count = requested_partitions,
    };
    size_t partition_begin = array_begin;
    size_t next_partition = 1;
    size_t last_event_end = array_begin;
    EventBoundaryScanner scanner(buffer, array_begin);
    while (const auto event = scanner.next()) {
        last_event_end = event->end;
        const bool reached_target = next_partition < requested_partitions && event->begin >= plan.target(next_partition);
        if (reached_target && event->begin > partition_begin) {
            partitions.push_back(EventByteRange{ .begin = partition_begin, .end = event->begin });
            partition_begin = event->begin;
            advance_partition_index(event->begin, plan, next_partition);
        }
    }
    if (scanner.malformed()) return {};
    if (last_event_end == array_begin) return {};
    partitions.push_back(EventByteRange{ .begin = partition_begin, .end = last_event_end });
    return partitions;
}

std::vector<TraceEvent> parse_event_partition(std::shared_ptr<const std::string> buffer, EventByteRange range, TraceReadOptions options) {
    const auto * begin = buffer->data() + range.begin;
    const auto * end = buffer->data() + range.end;
    TraceScanner scanner(std::move(buffer), options);
    return scanner.parse_event_sequence(begin, end);
}

struct CpuOutputIdentity {
    std::string pid;
    std::string tid;
};

using CpuOutputIdentities = std::unordered_map<int, CpuOutputIdentity>;

std::string output_pid(const TraceEvent & event, const DagNode & node, const CpuOutputIdentities & cpu_identities) {
    if (node.is_cpu) {
        if (const auto found = cpu_identities.find(node.gpu_id); found != cpu_identities.end()) return found->second.pid;
    }
    else { return std::to_string(node.gpu_id); }
    if (!event.pid.empty()) return event.pid;
    return "-1";
}

std::string output_tid(const TraceEvent & event, const DagNode & node, const CpuOutputIdentities & cpu_identities) {
    if (node.is_cpu) {
        if (const auto found = cpu_identities.find(node.gpu_id); found != cpu_identities.end()) return found->second.tid;
    }
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
using chrome_trace_io_detail::TraceScanner;

std::vector<TraceEvent> read_chrome_trace(const std::string & filename) { return read_chrome_trace(filename, TraceReadOptions{}); }

std::vector<TraceEvent> read_chrome_trace(const std::string & filename, const TraceReadOptions & options) {
    // One shared file buffer keeps lazy args alive while avoiding the multi-fold memory
    // expansion of a whole-file JSON DOM.
    std::string buffer = chrome_trace_io_detail::read_file_buffer(filename, std::max<size_t>(1, options.threads));
    if (options.auto_repair) buffer = chrome_trace_io_detail::repair_trace_tail(std::move(buffer));
    auto shared_buffer = std::make_shared<const std::string>(std::move(buffer));

    std::vector<TraceEvent> events;
    if (options.threads <= 1) {
        TraceScanner scanner(shared_buffer, options);
        events = scanner.parse();
    }
    else {
        constexpr size_t kMinParallelParseBytes = 64ull * 1'024ull * 1'024ull;
        const auto useful_partitions = shared_buffer->size() / kMinParallelParseBytes + (shared_buffer->size() % kMinParallelParseBytes == 0 ? 0 : 1);
        auto partitions = chrome_trace_io_detail::partition_event_array(*shared_buffer, std::min(options.threads, useful_partitions));
        if (partitions.empty()) {
            TraceScanner scanner(shared_buffer, options);
            events = scanner.parse();
        }
        else if (partitions.size() == 1) { events = chrome_trace_io_detail::parse_event_partition(shared_buffer, partitions.front(), options); }
        else {
            std::vector<std::future<std::vector<TraceEvent>>> futures;
            futures.reserve(partitions.size());
            for (const auto partition : partitions) {
                futures.push_back(std::async(std::launch::async, [shared_buffer, partition, options] {
                    return chrome_trace_io_detail::parse_event_partition(shared_buffer, partition, options);
                }));
            }

            std::vector<std::vector<TraceEvent>> parsed_partitions;
            parsed_partitions.reserve(futures.size());
            size_t event_count = 0;
            for (auto & future : futures) {
                auto part = future.get();
                if (part.size() > std::numeric_limits<size_t>::max() - event_count) { throw std::length_error("Chrome trace event count overflow"); }
                event_count += part.size();
                parsed_partitions.push_back(std::move(part));
            }
            events.reserve(event_count);
            for (auto & part : parsed_partitions) { events.insert(events.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end())); }
        }
    }

    for (size_t index = 0; index < events.size(); ++index) events[index].index = index;
    auto & logger = Logger::instance();
    if (logger.enabled(Logger::Info)) logger.info() << "Read " << events.size() << " Chrome trace events from " << filename;
    return events;
}

void write_chrome_trace_dag(const std::string & filename, const DagGraph & graph) {
    // Graph output is explicit and may be large; it never participates in the default result.
    std::ofstream ofs(filename);
    if (!ofs.is_open()) { throw std::runtime_error("Failed to write Chrome trace DAG: " + filename); }
    ofs << "{\n  \"traceEvents\": [\n";

    // Shift relative simulation time near the observed trace origin for visual comparison.
    uint64_t real_min = 0;
    bool has_real_time = false;
    chrome_trace_io_detail::CpuOutputIdentities cpu_identities;
    std::ranges::for_each(graph.nodes(), [&](const auto & node) {
        if (!node.active) return;
        const auto & event = graph.event_for_node(node.id);
        if (node.is_cpu) cpu_identities.try_emplace(node.gpu_id, chrome_trace_io_detail::CpuOutputIdentity{ .pid = event.pid, .tid = event.tid });
        if (node.kind != core::DagNodeKind::Synthetic) {
            if (!has_real_time || event.ts < real_min) real_min = event.ts;
            has_real_time = true;
        }
    });

    bool first = true;
    for (const auto & node : graph.nodes()) {
        if (!node.active) continue;
        // Emit each active DAG node as one duration event.
        const auto & event = graph.event_for_node(node.id);
        if (!first) ofs << ",\n";
        first = false;

        auto pid = json_scalar_literal(chrome_trace_io_detail::output_pid(event, node, cpu_identities));
        auto tid = json_scalar_literal(chrome_trace_io_detail::output_tid(event, node, cpu_identities));

        ofs << "    {\n"
            << "      \"name\": \"node_" << escape_json(event.name) << "\",\n"
            << "      \"cat\": \"sim_" << escape_json(event.cat) << "\",\n"
            << "      \"ph\": \"X\",\n"
            << "      \"ts\": " << (real_min + node.simulation_start) << ",\n"
            << "      \"dur\": " << node.duration << ",\n"
            << "      \"pid\": " << pid << ",\n"
            << "      \"tid\": " << tid << ",\n"
            << "      \"args\": {\"node_id\":" << node.id << ",\"event_index\":" << node.event_index << ",\"node_kind\":\""
            << (node.kind == core::DagNodeKind::Synthetic ? "synthetic" : "trace_event") << "\"}\n"
            << "    }";
    }

    for (size_t edge_index = 0; edge_index < graph.edges().size(); ++edge_index) {
        const auto & edge = graph.edges()[edge_index];
        if (!edge.active || edge.src >= graph.node_count() || edge.dst >= graph.node_count() || !graph.node(edge.src).active || !graph.node(edge.dst).active)
            continue;
        // Emit each active dependency as a paired Chrome flow event.
        const auto & src_event = graph.event_for_node(edge.src);
        const auto & dst_event = graph.event_for_node(edge.dst);
        const auto & src_node = graph.node(edge.src);
        const auto & dst_node = graph.node(edge.dst);
        auto edge_id = "edge_" + std::to_string(edge_index) + "_" + std::to_string(edge.src) + "_" + std::to_string(edge.dst);
        auto src_pid = json_scalar_literal(chrome_trace_io_detail::output_pid(src_event, src_node, cpu_identities));
        auto src_tid = json_scalar_literal(chrome_trace_io_detail::output_tid(src_event, src_node, cpu_identities));
        auto dst_pid = json_scalar_literal(chrome_trace_io_detail::output_pid(dst_event, dst_node, cpu_identities));
        auto dst_tid = json_scalar_literal(chrome_trace_io_detail::output_tid(dst_event, dst_node, cpu_identities));

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
