#include "markov/graph/source_graph.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace markov {
namespace {

constexpr std::array transfer_directions{ "host_to_host", "host_to_device", "device_to_host", "device_to_device" };

std::string_view domain_name(SourceDomain domain) {
    switch (domain) {
    case SourceDomain::CpuThread:
        return "cpu_thread";
    case SourceDomain::DeviceStream:
        return "device_stream";
    case SourceDomain::CommunicationObservation:
        return "communication_observation";
    case SourceDomain::LogicalCompletion:
        return "logical_completion";
    case SourceDomain::DeviceTaskCompletion:
        return "device_task_completion";
    }

    std::unreachable();
}

std::string_view control_part_name(SourceCpuPart part) {
    switch (part) {
    case SourceCpuPart::SchedulePrefix:
        return "schedule_prefix";
    case SourceCpuPart::PrepareBody:
        return "prepare_body";
    case SourceCpuPart::PrepareSuffix:
        return "prepare_suffix";
    case SourceCpuPart::Execute:
        return "execute";
    case SourceCpuPart::Publish:
        return "publish";
    case SourceCpuPart::Dispatch:
        return "dispatch";
    }

    std::unreachable();
}

} // namespace

nlohmann::json SourceTransfer::describe() const {
    return {
        {      "bytes",                                                    bytes },
        {  "direction", transfer_directions[static_cast<std::size_t>(direction)] },
        { "source_api",                                               source_api }
    };
}

SourceTransfer SourceTransfer::read(const nlohmann::json & value) {
    const auto name = value.at("direction").get<std::string>();
    const auto found = std::find(transfer_directions.begin(), transfer_directions.end(), name);
    if (found == transfer_directions.end()) throw std::invalid_argument("unknown transfer direction: " + name);

    return { value.at("bytes").get<std::uint64_t>(),
             static_cast<TransferDirection>(found - transfer_directions.begin()),
             value.at("source_api").get<std::size_t>() };
}

std::string_view SourceGraph::span_name(SourceSpanKind kind) {
    switch (kind) {
    case SourceSpanKind::CpuObservation:
        return "cpu_observation";
    case SourceSpanKind::CpuControl:
        return "cpu_control";
    case SourceSpanKind::HostWait:
        return "host_wait";
    case SourceSpanKind::Compute:
        return "compute";
    case SourceSpanKind::Dma:
        return "dma";
    case SourceSpanKind::DeviceWait:
        return "device_wait";
    case SourceSpanKind::DeviceMarker:
        return "device_marker";
    case SourceSpanKind::CommunicationObservation:
        return "communication_observation";
    }

    std::unreachable();
}

SourceRelation SourceGraph::parse_relation(std::string_view name) {
    if (name == "interval_extent") return SourceRelation::IntervalExtent;
    if (name == "thread_order") return SourceRelation::ThreadOrder;
    if (name == "stream_order") return SourceRelation::StreamOrder;
    if (name == "native_flow") return SourceRelation::NativeFlow;
    if (name == "collective_submission") return SourceRelation::CollectiveSubmission;
    if (name == "native_notify_signal") return SourceRelation::NativeNotifySignal;
    if (name == "native_event_signal") return SourceRelation::NativeEventSignal;
    if (name == "native_host_wait") return SourceRelation::NativeHostWait;
    if (name == "native_task_dependency") return SourceRelation::NativeTaskDependency;
    if (name == "completion_join") return SourceRelation::CompletionJoin;
    throw std::invalid_argument("unknown source relation: " + std::string(name));
}

SourceDomain SourceGraph::parse_domain(std::string_view name) {
    if (name == "cpu_thread") return SourceDomain::CpuThread;
    if (name == "device_stream") return SourceDomain::DeviceStream;
    if (name == "communication_observation") return SourceDomain::CommunicationObservation;
    if (name == "logical_completion") return SourceDomain::LogicalCompletion;
    if (name == "device_task_completion") return SourceDomain::DeviceTaskCompletion;
    throw std::invalid_argument("unknown source lane domain: " + std::string(name));
}

std::string_view SourceGraph::relation_name(SourceRelation relation) {
    switch (relation) {
    case SourceRelation::IntervalExtent:
        return "interval_extent";
    case SourceRelation::ThreadOrder:
        return "thread_order";
    case SourceRelation::StreamOrder:
        return "stream_order";
    case SourceRelation::NativeFlow:
        return "native_flow";
    case SourceRelation::NativeNotifySignal:
        return "native_notify_signal";
    case SourceRelation::NativeEventSignal:
        return "native_event_signal";
    case SourceRelation::NativeHostWait:
        return "native_host_wait";
    case SourceRelation::NativeTaskDependency:
        return "native_task_dependency";
    case SourceRelation::CollectiveSubmission:
        return "collective_submission";
    case SourceRelation::CompletionJoin:
        return "completion_join";
    case SourceRelation::ModelDataDependency:
        return "model_data_dependency";
    }

    std::unreachable();
}

nlohmann::json KvSequence::describe_addresses(std::int64_t page_size, std::int64_t batch_offset, bool include_writes) const {
    const auto prefix_end = prefix_tokens - 1;
    const std::int64_t last_slot = prefix_tokens == 0 ? -1 : std::int64_t(kv_pages[prefix_end / page_size]) * page_size + prefix_end % page_size;
    nlohmann::json result{
        { "request_pool_slot", request_pool_slot },
        {       "kv_page_ids",          kv_pages },
        {  "prefix_last_slot",         last_slot }
    };
    if (!include_writes) return result;

    // Shared by allocation and cache-write: one range per touched target page.
    auto ranges = nlohmann::json::array();
    auto token = prefix_tokens;
    const auto end = token + input_tokens;
    while (token < end) {
        const auto page_offset = token % page_size;
        const auto count = std::min(end - token, page_size - page_offset);
        const auto slot = std::int64_t(kv_pages[token / page_size]) * page_size + page_offset;
        ranges.push_back({
            { "batch_token_begin", batch_offset + token - prefix_tokens },
            {        "slot_begin",                                 slot },
            {       "token_count",                                count }
        });
        token += count;
    }
    result["write_ranges"] = std::move(ranges);
    return result;
}

nlohmann::json KvAllocationWorkload::describe() const {
    auto requests = nlohmann::json::array();
    std::int64_t offset = 0;
    for (const auto & sequence : sequences) {
        auto request = sequence.describe_addresses(page_size_tokens, offset, true);
        const auto previous_pages = (sequence.prefix_tokens + page_size_tokens - 1) / page_size_tokens;
        request["input_tokens"] = sequence.input_tokens;
        request["prefix_tokens"] = sequence.prefix_tokens;
        request["sequence_tokens"] = sequence.prefix_tokens + sequence.input_tokens;
        request["previous_page_count"] = previous_pages;
        request["new_page_count"] = static_cast<std::int64_t>(sequence.kv_pages.size()) - previous_pages;
        requests.push_back(std::move(request));
        offset += sequence.input_tokens;
    }
    return {
        {         "sequences",                   requests },
        {  "page_size_tokens",           page_size_tokens },
        { "free_pages_before",          free_pages_before },
        {  "parameter_source", "target_page_reservations" }
    };
}

nlohmann::json KvReleaseWorkload::describe() const {
    auto requests = nlohmann::json::array();
    std::vector<std::uint32_t> pages;
    for (const auto & sequence : sequences) {
        requests.push_back({
            { "request_pool_slot",                     sequence.request_pool_slot },
            {   "sequence_tokens", sequence.prefix_tokens + sequence.input_tokens },
            { "released_page_ids",                              sequence.kv_pages }
        });
        pages.insert(pages.end(), sequence.kv_pages.begin(), sequence.kv_pages.end());
    }
    // Cold request reservations are disjoint. Report the released identity set
    // in stable order; this does not prescribe the controller free-list order.
    std::sort(pages.begin(), pages.end());
    return {
        {         "sequences",                               requests },
        {  "page_size_tokens",                       page_size_tokens },
        { "free_pages_before",                      free_pages_before },
        {  "free_pages_after",       free_pages_before + pages.size() },
        { "released_page_ids",                                  pages },
        {  "parameter_source", "target_finished_request_reservations" }
    };
}

nlohmann::json AttentionWorkload::describe() const {
    std::string_view role;
    switch (operation) {
    case AttentionOperation::CacheWrite:
        role = "cache_write";
        break;
    case AttentionOperation::Prefill:
        role = "prefill_attention";
        break;
    case AttentionOperation::Decode:
        role = "decode_attention";
        break;
    }
    auto requests = nlohmann::json::array();
    std::int64_t query_tokens = 0;
    std::int64_t causal_pairs = 0;
    for (const auto & sequence : sequences) {
        auto request = sequence.describe_addresses(page_size_tokens, query_tokens, operation == AttentionOperation::CacheWrite);
        request["query_tokens"] = sequence.input_tokens;
        request["prefix_tokens"] = sequence.prefix_tokens;
        request["kv_tokens"] = sequence.prefix_tokens + sequence.input_tokens;
        requests.push_back(std::move(request));
        query_tokens += sequence.input_tokens;
        causal_pairs += sequence.input_tokens * sequence.prefix_tokens + sequence.input_tokens * (sequence.input_tokens + 1) / 2;
    }
    nlohmann::json result{
        {             "role",                                          role },
        {        "sequences",                                      requests },
        {     "query_tokens",                                  query_tokens },
        {      "query_heads",                                   query_heads },
        {         "kv_heads",                                      kv_heads },
        {         "head_dim",                                      head_dim },
        { "page_size_tokens",                              page_size_tokens },
        {      "cache_pages",                                   cache_pages },
        { "parameter_source", "target_batch_input_and_native_module_layout" }
    };
    if (operation != AttentionOperation::CacheWrite) result["logical_causal_pairs_per_head"] = causal_pairs;
    return result;
}

nlohmann::json PageTableWorkload::describe() const {
    auto rows = nlohmann::json::array();
    std::int64_t max_sequence = 0;
    for (const auto & sequence : sequences) {
        const auto tokens = sequence.prefix_tokens + sequence.input_tokens;
        max_sequence = std::max(max_sequence, tokens);
        rows.push_back({
            { "request_pool_slot", sequence.request_pool_slot },
            {   "sequence_tokens",                     tokens },
            {    "valid_page_ids",          sequence.kv_pages }
        });
    }
    return {
        {             "rows",                                                     rows },
        {          "columns", (max_sequence + page_size_tokens - 1) / page_size_tokens },
        { "page_size_tokens",                                         page_size_tokens },
        {          "padding",             "outside_each_request_sequence; unspecified" },
        { "parameter_source",                            "target_request_reservations" }
    };
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
    if (kv_allocation) result["kv_allocation"] = kv_allocation->describe();
    if (kv_release) result["kv_release"] = kv_release->describe();
    if (batch_filter) result["batch_filter"] = batch_filter->describe();
    if (batch_merge) result["batch_merge"] = batch_merge->describe();
    if (page_table) result["page_table"] = page_table->describe();
    if (sequence_lengths)
        result["sequence_lengths"] = {
            {           "tokens",    *sequence_lengths },
            { "parameter_source", "target_batch_input" }
        };
    if (sequence_length_max)
        result["sequence_length_max"] = {
            {           "tokens", *sequence_length_max },
            { "parameter_source", "target_batch_input" }
        };
    if (kv_new_pages)
        result["kv_new_pages"] = {
            {            "pages",                        *kv_new_pages },
            { "parameter_source", "target_batch_lengths_and_page_size" }
        };
    if (request_pool_slots)
        result["request_pool_access"] = {
            {            "slots",           *request_pool_slots },
            { "parameter_source", "target_request_reservations" }
        };
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

SourceCollective SourceCollective::read(const nlohmann::json & value) {
    SourceCollective collective{ value.at("span").get<SourceSpanId>(),
                                 value.at("rank").get<std::uint32_t>(),
                                 value.at("connection").get<std::uint64_t>(),
                                 value.at("name").get<std::string>(),
                                 value.at("count").get<std::uint64_t>(),
                                 value.at("data_type").get<std::string>(),
                                 value.at("algorithm").get<std::string>(),
                                 value.at("group_name").get<std::string>(),
                                 value.at("communicator").is_null() ? std::nullopt : std::optional(SourceCommunicator::read(value.at("communicator"))),
                                 {} };
    for (const auto & task : value.at("tasks")) {
        collective.tasks.push_back({ task.at("span").get<SourceSpanId>(),
                                     task.at("source_detail").get<std::size_t>(),
                                     task.at("stream").get<std::uint64_t>(),
                                     task.at("task").get<std::uint64_t>(),
                                     task.at("context").get<std::uint64_t>(),
                                     task.at("operation").get<std::string>(),
                                     task.at("bytes").get<std::uint64_t>(),
                                     task.at("data_type").get<std::string>(),
                                     task.at("transport").get<std::string>(),
                                     task.at("source_rank").get<std::uint32_t>(),
                                     task.at("destination_rank").get<std::uint32_t>() });
    }
    return collective;
}

void SourceGraph::observe_collective(SourceCollective collective) { collectives_.push_back(std::move(collective)); }

void SourceGraph::observe_kernel(SourceSpanId span, SourceKernel kernel) { kernels_.emplace(span, std::move(kernel)); }

void SourceGraph::observe_transfer(SourceSpanId span, SourceTransfer transfer) { transfers_.emplace(span, transfer); }

void SourceGraph::observe_stream_wait(SourceSpanId span, SourceStreamWait wait) { stream_waits_.emplace(span, std::move(wait)); }

void SourceGraph::observe_operation(SourceOperation operation) { operations_.push_back(std::move(operation)); }

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
                                  std::optional<std::size_t> source_interval, std::optional<SourceCpuPart> control_part, std::optional<std::size_t> source_api,
                                  std::optional<std::size_t> scope_call) {
    const BoundaryId begin = boundary(lane, begin_ns);
    const BoundaryId end = boundary(lane, end_ns);
    const SourceSpanId identity = spans_.size();
    spans_.push_back({ kind, owner_call, source_interval, begin, end, control_part, source_api, scope_call });
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
        const auto domain = lanes_[key.first].domain;
        if (domain != SourceDomain::CpuThread) { continue; }
        if (previous && boundaries_[*previous].lane == key.first) { connect(*previous, identity, SourceRelation::ThreadOrder); }
        previous = identity;
    }

    // Build compact adjacency once; the traversal below checks dependency cycles.
    std::vector<std::size_t> incoming(boundaries_.size());
    std::vector<std::size_t> offsets(boundaries_.size() + 1);
    std::size_t reversals = 0;
    std::size_t notify_signals = 0;
    std::size_t event_signals = 0;
    std::size_t host_wait_dependencies = 0;
    for (const auto & edge : edges_) {
        ++incoming[edge.target];
        ++offsets[edge.source + 1];
        if (edge.relation == SourceRelation::NativeNotifySignal) ++notify_signals;
        if (edge.relation == SourceRelation::NativeEventSignal) ++event_signals;
        if (edge.relation == SourceRelation::NativeHostWait) ++host_wait_dependencies;
        if (edge.relation == SourceRelation::NativeFlow && boundaries_[edge.target].observed_time_ns < boundaries_[edge.source].observed_time_ns) {
            ++reversals;
        }
    }

    std::partial_sum(offsets.begin(), offsets.end(), offsets.begin());
    auto cursor = offsets;
    std::vector<BoundaryId> targets(edges_.size());
    for (const auto & edge : edges_) { targets[cursor[edge.source]++] = edge.target; }

    std::queue<BoundaryId> ready;
    for (BoundaryId identity = 0; identity < incoming.size(); ++identity) {
        if (incoming[identity] == 0) { ready.push(identity); }
    }

    std::size_t visited = 0;
    while (!ready.empty()) {
        const BoundaryId identity = ready.front();
        ready.pop();
        ++visited;

        for (std::size_t position = offsets[identity]; position < offsets[identity + 1]; ++position) {
            const BoundaryId target = targets[position];
            if (--incoming[target] == 0) { ready.push(target); }
        }
    }

    return { boundaries_.size(), spans_.size(), edges_.size(), visited, reversals, notify_signals, event_signals, host_wait_dependencies };
}

const std::vector<SourceSpan> & SourceGraph::spans() const { return spans_; }

const std::vector<SourceCollective> & SourceGraph::collectives() const { return collectives_; }

const std::vector<SourceEdge> & SourceGraph::edges() const { return edges_; }

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
    for (std::size_t index = 0; index < collectives_.size(); ++index) {
        if (index != 0) stream << ',';
        stream << collectives_[index].describe();
    }

    stream << "],\"operations\":[";
    for (std::size_t index = 0; index < operations_.size(); ++index) {
        if (index != 0) stream << ',';
        const auto & operation = operations_[index];
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

    stream << "],\"lanes\":[";
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
        if (span.control_part) record["control_part"] = control_part_name(*span.control_part);
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
