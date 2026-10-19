#include "markov/graph/source_graph.hpp"
#include "markov/graph/directed_graph.hpp"

#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace markov {
namespace {

constexpr std::array transfer_directions{ "host_to_host", "host_to_device", "device_to_host", "device_to_device" };

constexpr std::array request_pool_operations{ "prefix_extend_write", "token_read", "token_write", "row_read", "relay_read", "relay_write" };

constexpr std::array domains{ "cpu_thread", "device_stream", "communication_observation", "logical_completion", "device_task_completion" };
constexpr std::array span_kinds{ "cpu_observation", "cpu_control", "host_wait", "compute", "dma", "device_wait", "device_marker", "communication_observation" };
constexpr std::array relations{ "interval_extent",       "thread_order",        "stream_order",         "native_flow",
                                "native_notify_signal",  "native_event_signal", "native_host_wait",     "native_task_dependency",
                                "collective_submission", "completion_join",     "model_data_dependency" };

template <class Enum, std::size_t Size> Enum enum_value(const std::array<const char *, Size> & names, std::string_view name) {
    const auto found = std::ranges::find(names, name);
    if (found == names.end()) throw std::invalid_argument("unknown source graph value: " + std::string(name));
    return static_cast<Enum>(found - names.begin());
}

std::string_view domain_name(SourceDomain domain) { return domains[static_cast<std::size_t>(domain)]; }

} // namespace

nlohmann::json SourceTransfer::describe() const {
    return {
        {      "bytes",                                                              bytes },
        {  "direction",                                          direction_name(direction) },
        { "source_api", source_api ? nlohmann::json(*source_api) : nlohmann::json(nullptr) }
    };
}

SourceTransfer SourceTransfer::read(const nlohmann::json & value) {
    return { value.at("bytes").get<std::uint64_t>(),
             parse_direction(value.at("direction").get<std::string>()),
             value.at("source_api").is_null() ? std::nullopt : std::optional(value.at("source_api").get<std::size_t>()) };
}

std::string_view SourceTransfer::direction_name(TransferDirection direction) { return transfer_directions[static_cast<std::size_t>(direction)]; }

TransferDirection SourceTransfer::parse_direction(std::string_view name) { return enum_value<TransferDirection>(transfer_directions, name); }

std::string_view SourceGraph::span_name(SourceSpanKind kind) { return span_kinds[static_cast<std::size_t>(kind)]; }
std::string_view SourceGraph::relation_name(SourceRelation relation) { return relations[static_cast<std::size_t>(relation)]; }

nlohmann::json KvSequence::describe() const {
    return {
        {      "input_tokens",      input_tokens },
        {     "prefix_tokens",     prefix_tokens },
        { "request_pool_slot", request_pool_slot },
        {       "kv_page_ids",          kv_pages }
    };
}

nlohmann::json AttentionWorkload::describe() const {
    static constexpr std::array roles{ "cache_write", "prefill_attention", "decode_attention" };
    auto members = nlohmann::json::array();
    for (const auto & sequence : sequences) members.push_back(sequence.describe());
    return {
        {             "role", roles[static_cast<std::size_t>(operation)] },
        {        "sequences",                         std::move(members) },
        {      "query_heads",                                query_heads },
        {         "kv_heads",                                   kv_heads },
        {         "head_dim",                                   head_dim },
        { "page_size_tokens",                           page_size_tokens },
        {      "cache_pages",                                cache_pages }
    };
}

std::string_view RequestPoolWork::operation_name(RequestPoolOperation operation) { return request_pool_operations[static_cast<std::size_t>(operation)]; }

RequestPoolWork RequestPoolWork::read(const nlohmann::json & value) {
    const auto role = input::string_value(value.at("role"));
    const auto found = std::ranges::find(request_pool_operations, role);
    if (found == request_pool_operations.end()) throw std::invalid_argument("unknown request pool operation: " + role);

    const auto operation = static_cast<RequestPoolOperation>(found - request_pool_operations.begin());
    constexpr auto maximum_slot = std::numeric_limits<std::uint32_t>::max();
    constexpr auto maximum_token = std::numeric_limits<std::int64_t>::max();
    if (operation == RequestPoolOperation::PrefixExtendWrite) {
        std::vector<RequestPoolRange> ranges;
        for (const auto & member : value.at("members")) {
            ranges.push_back({ static_cast<std::uint32_t>(input::integer(member.at("slot"), 0, maximum_slot)),
                               input::integer(member.at("prefix_tokens"), 0, maximum_token),
                               input::integer(member.at("input_tokens"), 1, maximum_token) });
        }
        return { operation, std::move(ranges) };
    }
    if (operation == RequestPoolOperation::TokenRead || operation == RequestPoolOperation::TokenWrite) {
        std::vector<RequestPoolPoint> points;
        for (const auto & member : value.at("members")) {
            points.push_back(
                { static_cast<std::uint32_t>(input::integer(member.at("slot"), 0, maximum_slot)), input::integer(member.at("column"), 0, maximum_token) });
        }
        return { operation, std::move(points) };
    }

    std::vector<std::uint32_t> slots;
    for (const auto & slot : value.at("slots")) slots.push_back(static_cast<std::uint32_t>(input::integer(slot, 0, maximum_slot)));
    return { operation, std::move(slots) };
}

nlohmann::json RequestPoolWork::describe() const {
    nlohmann::json result{
        { "role", operation_name(operation) }
    };
    if (operation == RequestPoolOperation::PrefixExtendWrite) {
        auto ranges = nlohmann::json::array();
        for (const auto & member : std::get<std::vector<RequestPoolRange>>(members)) {
            ranges.push_back({
                {          "slot",          member.slot },
                { "prefix_tokens", member.prefix_tokens },
                {  "input_tokens",  member.input_tokens }
            });
        }
        result["members"] = std::move(ranges);
    }
    else if (operation == RequestPoolOperation::TokenRead || operation == RequestPoolOperation::TokenWrite) {
        auto points = nlohmann::json::array();
        for (const auto & member : std::get<std::vector<RequestPoolPoint>>(members)) {
            points.push_back({
                {   "slot",   member.slot },
                { "column", member.column }
            });
        }
        result["members"] = std::move(points);
    }
    else result["slots"] = std::get<std::vector<std::uint32_t>>(members);
    return result;
}

nlohmann::json PositionWorkload::describe() const {
    auto ranges = nlohmann::json::array();
    std::int64_t offset = 0;
    for (const auto & sequence : sequences) {
        ranges.push_back({
            {           "extend_start",                                         offset },
            {          "prefix_tokens",                         sequence.prefix_tokens },
            {           "input_tokens",                          sequence.input_tokens },
            {         "position_begin",                         sequence.prefix_tokens },
            { "position_end_exclusive", sequence.prefix_tokens + sequence.input_tokens }
        });
        offset += sequence.input_tokens;
    }
    return {
        {        "sequences",               ranges },
        {     "total_tokens",               offset },
        { "parameter_source", "target_batch_input" }
    };
}

nlohmann::json SourceKernel::describe() const {
    nlohmann::json result{
        {           "name",           name },
        {      "operation",      operation },
        {   "input_shapes",   input_shapes },
        {  "output_shapes",  output_shapes },
        {   "input_dtypes",   input_dtypes },
        {  "output_dtypes",  output_dtypes },
        {  "input_formats",  input_formats },
        { "output_formats", output_formats }
    };
    if (positions) result["position_workload"] = positions->describe();
    if (batch_filter) result["batch_filter"] = batch_filter->describe();
    if (!value_dependencies.empty()) result["value_dependencies"] = value_dependencies;
    if (request_pool) result["request_pool_access"] = request_pool->describe();
    if (attention) result["attention_workload"] = attention->describe();
    if (logits_token_indices)
        result["logits_selection"] = {
            { "last_token_indices",          *logits_token_indices },
            {             "source", "target_request_input_lengths" }
        };
    return result;
}

nlohmann::json SourceCommunicator::describe() const {
    auto links = nlohmann::json::array();
    for (const auto & [operation, peer, transport] : transports)
        links.push_back({
            { "operation", operation },
            {      "peer",      peer },
            { "transport", transport }
        });
    return {
        {      "buffer_bytes",                                                                   buffer_bytes },
        {     "deterministic",                                                                  deterministic },
        { "op_expansion_mode",                                                              op_expansion_mode },
        {        "rank_count",                                                                     rank_count },
        {      "server_count",                                                                   server_count },
        {      "module_count",                                                                   module_count },
        {       "device_type",           device_type ? nlohmann::json(*device_type) : nlohmann::json(nullptr) },
        {     "hcomm_version",       hcomm_version ? nlohmann::json(*hcomm_version) : nlohmann::json(nullptr) },
        {  "env_aicpu_unfold", env_aicpu_unfold ? nlohmann::json(*env_aicpu_unfold) : nlohmann::json(nullptr) },
        {      "env_aiv_mode",         env_aiv_mode ? nlohmann::json(*env_aiv_mode) : nlohmann::json(nullptr) },
        {        "transports",                                                               std::move(links) }
    };
}

SourceCommunicator SourceCommunicator::read(const nlohmann::json & value) {
    SourceCommunicator result{ value.at("buffer_bytes").get<std::uint64_t>(),
                               value.at("deterministic").get<std::uint32_t>(),
                               value.at("op_expansion_mode").get<std::uint32_t>(),
                               value.at("rank_count").get<std::uint32_t>(),
                               value.at("server_count").get<std::uint32_t>(),
                               value.at("module_count").get<std::uint32_t>(),
                               {},
                               {},
                               {},
                               {},
                               {} };
    if (!value.at("device_type").is_null()) result.device_type = value.at("device_type").get<std::string>();
    if (!value.at("hcomm_version").is_null()) result.hcomm_version = value.at("hcomm_version").get<std::string>();
    if (!value.at("env_aicpu_unfold").is_null()) result.env_aicpu_unfold = value.at("env_aicpu_unfold").get<std::uint32_t>();
    if (!value.at("env_aiv_mode").is_null()) result.env_aiv_mode = value.at("env_aiv_mode").get<std::uint32_t>();
    for (const auto & link : value.at("transports"))
        result.transports.push_back({ link.at("operation").get<std::string>(), link.at("peer").get<std::uint32_t>(), link.at("transport").get<std::string>() });
    return result;
}

nlohmann::json SourceCommunicationTask::describe() const {
    return {
        {             "span",             span },
        {    "source_detail",    source_detail },
        {           "stream",           stream },
        {             "task",             task },
        {          "context",          context },
        {        "operation",        operation },
        {            "bytes",            bytes },
        {        "data_type",        data_type },
        {        "transport",        transport },
        {      "source_rank",      source_rank },
        { "destination_rank", destination_rank }
    };
}

nlohmann::json SourceCollective::describe() const {
    auto physical = nlohmann::json::array();
    for (const auto & task : tasks) physical.push_back(task.describe());

    return {
        {         "span",                                                              span },
        {         "rank",                                                              rank },
        {   "connection",                                                        connection },
        {         "name",                                                              name },
        {        "count",                                                             count },
        {    "data_type",                                                         data_type },
        {    "algorithm",                                                         algorithm },
        {   "group_name",                                                        group_name },
        { "communicator", communicator ? communicator->describe() : nlohmann::json(nullptr) },
        {        "tasks",                                               std::move(physical) }
    };
}

void SourceGraph::observe_collective(SourceCollective collective) { collectives_.emplace(collective.span, std::move(collective)); }

void SourceGraph::observe_kernel(SourceSpanId span, SourceKernel kernel) { kernels_.emplace(span, std::move(kernel)); }

void SourceGraph::observe_transfer(SourceSpanId span, SourceTransfer transfer) { transfers_.emplace(span, transfer); }

void SourceGraph::observe_stream_wait(SourceSpanId span, SourceStreamWait wait) { stream_waits_.emplace(span, std::move(wait)); }

void SourceGraph::observe_operation(SourceOperation operation) { operations_.emplace(operation.host_span, std::move(operation)); }

void SourceGraph::observe_cpu_boundary(BoundaryId boundary, CpuBoundary description) { cpu_boundaries_.insert_or_assign(boundary, std::move(description)); }

std::size_t SourceGraph::lane(SourceDomain domain, std::uint32_t rank, std::uint64_t native_id) {
    const auto key = std::tuple(domain, rank, native_id);
    const auto [position, inserted] = lane_ids_.try_emplace(key, lanes_.size());
    if (inserted) { lanes_.push_back({ domain, rank, native_id }); }

    return position->second;
}

BoundaryId SourceGraph::boundary(std::size_t lane, std::int64_t time_ns) {
    if (lanes_[lane].domain != SourceDomain::CpuThread) {
        const auto identity = boundaries_.size();
        boundaries_.push_back({ lane, time_ns, std::nullopt });
        return identity;
    }

    // Only CPU annotations share equal-time endpoints; device tasks keep distinct identities.
    const auto [position, inserted] = boundary_ids_.try_emplace(std::pair(lane, time_ns), boundaries_.size());
    if (inserted) { boundaries_.push_back({ lane, time_ns, std::nullopt }); }

    return position->second;
}

SourceSpanId SourceGraph::observe(SourceSpanKind kind, std::size_t lane, std::int64_t begin_ns, std::int64_t end_ns, std::optional<std::size_t> owner_call,
                                  std::optional<std::size_t> source_interval, std::optional<std::size_t> source_api, std::optional<std::size_t> scope_call) {
    const BoundaryId begin = boundary(lane, begin_ns);
    const BoundaryId end = boundary(lane, end_ns);
    const SourceSpanId identity = spans_.size();
    spans_.push_back({ kind, owner_call, source_interval, begin, end, source_api, scope_call });
    connect(begin, end, SourceRelation::IntervalExtent);

    return identity;
}

void SourceGraph::connect(BoundaryId source, BoundaryId target, SourceRelation relation, std::optional<std::size_t> source_reference) {
    // Same-lane, same-timestamp annotations share a point, so their reflexive
    // relation conveys no additional ordering and must not create a self-cycle.
    if (source == target) { return; }

    edges_.push_back({ source, target, relation, source_reference });
}

BoundaryId SourceGraph::complete(std::span<const SourceSpanId> spans, std::uint32_t rank, std::uint64_t group, SourceDomain domain) {
    std::int64_t observed_time_ns = boundaries_[spans_[spans.front()].end].observed_time_ns;
    for (const auto identity : spans) { observed_time_ns = std::max(observed_time_ns, boundaries_[spans_[identity].end].observed_time_ns); }

    const auto completion_lane = lane(domain, rank, group);
    const auto completion = boundary(completion_lane, observed_time_ns);

    auto owner = spans_[spans.front()].owner_call;
    for (const auto identity : spans) {
        if (owner != spans_[identity].owner_call) owner = std::nullopt;
    }

    boundaries_[completion].owner_call = owner;
    for (const auto identity : spans) { connect(spans_[identity].end, completion, SourceRelation::CompletionJoin); }

    return completion;
}

SourceGraphSummary SourceGraph::seal() {
    std::optional<BoundaryId> previous;
    for (const auto & [key, identity] : boundary_ids_) {
        if (previous && boundaries_[*previous].lane == key.first) { connect(*previous, identity, SourceRelation::ThreadOrder); }
        previous = identity;
    }

    std::size_t reversals = 0;
    std::size_t notify_signals = 0;
    std::size_t event_signals = 0;
    std::size_t host_wait_dependencies = 0;
    for (const auto & edge : edges_) {
        if (edge.relation == SourceRelation::NativeNotifySignal) ++notify_signals;
        if (edge.relation == SourceRelation::NativeEventSignal) ++event_signals;
        if (edge.relation == SourceRelation::NativeHostWait) ++host_wait_dependencies;
        if (edge.relation == SourceRelation::NativeFlow && boundaries_[edge.target].observed_time_ns < boundaries_[edge.source].observed_time_ns) {
            ++reversals;
        }
    }

    const DirectedGraph graph(boundaries_.size(), edges_);
    return { boundaries_.size(), spans_.size(), edges_.size(), graph.order().size(), reversals, notify_signals, event_signals, host_wait_dependencies };
}

const std::vector<SourceSpan> & SourceGraph::spans() const { return spans_; }

const std::map<SourceSpanId, SourceOperation> & SourceGraph::operations() const { return operations_; }

const std::map<SourceSpanId, SourceCollective> & SourceGraph::collectives() const { return collectives_; }

const std::vector<SourceEdge> & SourceGraph::edges() const { return edges_; }

const std::vector<SourceLane> & SourceGraph::lanes() const { return lanes_; }

const std::vector<SourceBoundary> & SourceGraph::boundaries() const { return boundaries_; }

void SourceGraph::write(const std::filesystem::path & path) const {
    // Stream arrays so exporting a large source graph does not build a second
    // JSON object tree in memory. Each row has the same public data contract.
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream stream(temporary);
    stream.exceptions(std::ios::failbit | std::ios::badbit);

    stream << "{\"kernels\":[";
    bool first_kernel = true;
    for (const auto & [span, kernel] : kernels_) {
        if (!first_kernel) stream << ',';
        first_kernel = false;
        auto record = kernel.describe();
        record["source_span"] = span;
        stream << record;
    }

    stream << "],\"transfers\":[";
    bool first_transfer = true;
    for (const auto & [span, transfer] : transfers_) {
        if (!first_transfer) stream << ',';
        first_transfer = false;
        auto record = transfer.describe();
        record["source_span"] = span;
        stream << record;
    }

    stream << "],\"stream_waits\":[";
    bool first_wait = true;
    for (const auto & [span, wait] : stream_waits_) {
        if (!first_wait) stream << ',';
        first_wait = false;
        stream << nlohmann::json({
            {   "source_span",                                                                               span },
            { "native_stream", wait.native_stream ? nlohmann::json(*wait.native_stream) : nlohmann::json(nullptr) },
            {         "lanes",                                                                         wait.lanes },
            {         "scope",                   wait.scope == SynchronizationScope::Device ? "device" : "stream" }
        });
    }

    stream << "],\"collectives\":[";
    bool first_collective = true;
    for (const auto & [span, collective] : collectives_) {
        if (!first_collective) stream << ',';
        first_collective = false;
        stream << collective.describe();
    }

    stream << "],\"operations\":[";
    bool first_operation = true;
    for (const auto & [span, operation] : operations_) {
        if (!first_operation) stream << ',';
        first_operation = false;
        stream << nlohmann::json({
            {             "name",                                                                             operation.name },
            {        "host_span",                                                                        operation.host_span },
            {     "worker_spans",                                                                     operation.worker_spans },
            { "collective_spans",                                                                 operation.collective_spans },
            {     "device_spans",                                                                     operation.device_spans },
            {     "input_shapes", operation.input_shapes ? nlohmann::json(*operation.input_shapes) : nlohmann::json(nullptr) },
            {      "input_types",   operation.input_types ? nlohmann::json(*operation.input_types) : nlohmann::json(nullptr) }
        });
    }

    stream << "],\"cpu_boundaries\":{";
    bool first_cpu_boundary = true;
    for (const auto & [boundary, description] : cpu_boundaries_) {
        if (!first_cpu_boundary) stream << ',';
        first_cpu_boundary = false;
        stream << '"' << boundary << "\":" << description.describe();
    }

    stream << "},\"lanes\":[";
    for (std::size_t index = 0; index < lanes_.size(); ++index) {
        const auto & lane = lanes_[index];
        if (index != 0) stream << ',';
        stream << nlohmann::json({
            {    "domain", domain_name(lane.domain) },
            {      "rank",                lane.rank },
            { "native_id",           lane.native_id }
        });
    }

    stream << "],\"boundaries\":[";
    for (BoundaryId index = 0; index < boundaries_.size(); ++index) {
        const auto & point = boundaries_[index];
        if (index != 0) stream << ',';
        nlohmann::json record({
            {             "lane",             point.lane },
            { "observed_time_ns", point.observed_time_ns }
        });
        if (point.owner_call) record["owner_call"] = *point.owner_call;
        stream << record;
    }

    stream << "],\"spans\":[";
    for (SourceSpanId index = 0; index < spans_.size(); ++index) {
        const auto & span = spans_[index];
        if (index != 0) stream << ',';
        nlohmann::json record({
            {            "kind",                                                                   span_name(span.kind) },
            {      "owner_call",           span.owner_call ? nlohmann::json(*span.owner_call) : nlohmann::json(nullptr) },
            { "source_interval", span.source_interval ? nlohmann::json(*span.source_interval) : nlohmann::json(nullptr) },
            {      "source_api",           span.source_api ? nlohmann::json(*span.source_api) : nlohmann::json(nullptr) },
            {      "scope_call",           span.scope_call ? nlohmann::json(*span.scope_call) : nlohmann::json(nullptr) },
            {           "begin",                                                                             span.begin },
            {             "end",                                                                               span.end }
        });
        stream << record;
    }

    stream << "],\"edges\":[";
    for (std::size_t index = 0; index < edges_.size(); ++index) {
        const auto & edge = edges_[index];
        if (index != 0) stream << ',';
        stream << nlohmann::json({
            {           "source",                                                                              edge.source },
            {           "target",                                                                              edge.target },
            {         "relation",                                                             relation_name(edge.relation) },
            { "source_reference", edge.source_reference ? nlohmann::json(*edge.source_reference) : nlohmann::json(nullptr) }
        });
    }

    stream << "]}\n";
    stream.close();

    std::filesystem::rename(temporary, path);
}

} // namespace markov
