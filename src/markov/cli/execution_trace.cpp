#include "markov/cli/execution_trace.hpp"

#include <vector>

#include "markov/graph/graph.hpp"
#include "markov/resources/kv_pool.hpp"

namespace markov {

ExecutionTrace::ExecutionTrace(const std::filesystem::path & path) {
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    stream_.exceptions(std::ios::badbit | std::ios::failbit);
    stream_.open(path, std::ios::out | std::ios::noreplace);
}

void ExecutionTrace::inserted(const CoarseGraph & graph, std::span<const std::size_t> events, std::span<const std::int64_t> work_ns, std::int64_t time_ns) {

    // Input and template assets own the environment; repeating it per invocation would dominate the trace.
    stream_ << nlohmann::json{
        { "kind", "graph" },
        { "time_ns", time_ns },
        { "graph", graph.document(false) },
        { "event_ids", std::vector<std::size_t>(events.begin(), events.end()) },
        { "work_ns", std::vector<std::int64_t>(work_ns.begin(), work_ns.end()) }
    } << '\n';
}

void ExecutionTrace::connected(std::size_t source, std::size_t target, Relation relation, std::int64_t delay_ns) {
    stream_ << nlohmann::json{
        {     "kind",                  "edge" },
        {   "source",                  source },
        {   "target",                  target },
        { "relation", relation_name(relation) },
        { "delay_ns",                delay_ns }
    } << '\n';
}

void ExecutionTrace::completed(std::size_t event, std::int64_t time_ns) {
    stream_ << nlohmann::json{
        {    "kind", "completion" },
        {   "event",        event },
        { "time_ns",      time_ns }
    } << '\n';
}

void ExecutionTrace::constrained(std::size_t event, std::int64_t begin_ns, std::int64_t end_ns) {
    stream_ << nlohmann::json{
        {    "kind", "boundary_time" },
        {   "event",           event },
        { "begin_ns",       begin_ns },
        { "end_ns",           end_ns }
    } << '\n';
}

void ExecutionTrace::initialized(const nlohmann::json & instances, const nlohmann::json & requests) {

    // Request indices in resource operations resolve through this input order, not through graph event numbers.
    stream_ << nlohmann::json{
        {      "kind", "resources" },
        { "instances",   instances },
        {  "requests",    requests }
    } << '\n';
}

void ExecutionTrace::applied(std::string_view group, const nlohmann::json & operation, const nlohmann::json & result, const ResourceChanges & changes,
                             std::int64_t time_ns) {
    stream_ << nlohmann::json{
        {           "kind",             "resource" },
        {          "group",                  group },
        {        "time_ns",                time_ns },
        {      "operation",              operation },
        {         "result",                 result },
        { "returned_pages", changes.returned_pages },
        { "returned_slots", changes.returned_slots },
        { "returned_fields", changes.returned_fields },
        { "capacity_increased", changes.capacity_increased }
    } << '\n';
}

void ExecutionTrace::observed_values(std::string_view group, std::size_t boundary, const nlohmann::json & values, std::int64_t time_ns) {
    stream_ << nlohmann::json{
        { "kind", "action_values" }, { "group", group }, { "boundary", boundary }, { "values", values }, { "time_ns", time_ns }
    } << '\n';
}

void ExecutionTrace::native_protocol(std::string_view group, const nlohmann::json & record, std::int64_t time_ns) {
    stream_ << nlohmann::json{
        { "kind", "native_protocol" }, { "group", group }, { "record", record }, { "time_ns", time_ns }
    } << '\n';
}

void ExecutionTrace::queried(std::string_view group, std::size_t boundary, const nlohmann::json & result, std::int64_t time_ns) {
    stream_ << nlohmann::json{
        { "kind", "state_query" }, { "group", group }, { "boundary", boundary }, { "result", result }, { "time_ns", time_ns },
        { "completion_reads", completion_reads_[boundary] }
    } << '\n';
    completion_reads_.erase(boundary);
}

void ExecutionTrace::completion_read(std::string_view group, std::size_t boundary, std::string_view signal, std::size_t event,
                                    bool result, std::int64_t time_ns) {

    // Emit at the read itself: a later query result cannot reconstruct completion order at equal timestamps.
    stream_ << nlohmann::json{
        { "kind", "completion_read" }, { "group", group }, { "boundary", boundary }, { "signal", signal },
        { "event", event }, { "result", result }, { "time_ns", time_ns }
    } << '\n';
    ++completion_reads_[boundary];
}

void ExecutionTrace::notification_declared(const nlohmann::json & identity, std::size_t event) {
    stream_ << nlohmann::json{
        { "kind", "notification_declared" }, { "identity", identity }, { "event", event }
    } << '\n';
}

void ExecutionTrace::notification_ready(const nlohmann::json & identity, std::size_t event,
                                       std::span<const std::size_t> prerequisites, std::int64_t time_ns) {
    stream_ << nlohmann::json{
        { "kind", "notification_ready" }, { "identity", identity }, { "event", event }, { "time_ns", time_ns },
        { "prerequisites", std::vector<std::size_t>(prerequisites.begin(), prerequisites.end()) }
    } << '\n';
}

void ExecutionTrace::input_read(const nlohmann::json & identity, std::span<const std::size_t> requests, bool in_flow, std::int64_t time_ns) {
    stream_ << nlohmann::json{
        { "kind", "input_read" }, { "identity", identity }, { "time_ns", time_ns }, { "in_flow", in_flow },
        { "request_indices", std::vector<std::size_t>(requests.begin(), requests.end()) }
    } << '\n';
}

void ExecutionTrace::state_bound(std::size_t invocation, std::string_view pool, std::string_view field, std::string_view request, std::size_t slot) {
    stream_ << nlohmann::json{
        { "kind", "state_binding" }, { "invocation", invocation }, { "pool", pool }, { "field", field }, { "request", request }, { "slot", slot }
    } << '\n';
}

void ExecutionTrace::bound(std::size_t invocation, std::string_view pool, std::string_view request, std::optional<std::size_t> slot, std::int64_t begin,
                           std::span<const KvPageRange> ranges) {
    auto pages = nlohmann::json::array();
    for (const auto & range : ranges) pages.push_back({ range.page, range.begin, range.end });
    stream_ << nlohmann::json{
        {       "kind",                                       "binding" },
        { "invocation",                                      invocation },
        {       "pool",                                            pool },
        {    "request",                                         request },
        {       "slot", slot ? nlohmann::json(*slot) : nlohmann::json() },
        {      "begin",                                           begin },
        {     "ranges",                                           pages }
    } << '\n';
}

void ExecutionTrace::finish() {
    stream_ << nlohmann::json{
        { "kind", "end" }
    } << '\n';
    stream_.close();
}

} // namespace markov
