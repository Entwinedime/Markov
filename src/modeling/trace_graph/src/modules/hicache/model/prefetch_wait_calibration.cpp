#include "markov/trace_graph/modules/hicache/model/prefetch_wait_calibration.hpp"
#include <fstream>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::model {
namespace {
using Json = nlohmann::json;
const std::pair<const char *, std::optional<uint64_t> PrefetchCheckCpuTiming::*> cpu_fields[] = {
    {"check_to_max", &PrefetchCheckCpuTiming::check_to_max},
    {"max_to_false_return", &PrefetchCheckCpuTiming::max_to_false_return},
    {"max_to_stop", &PrefetchCheckCpuTiming::max_to_stop},
    {"best_effort_to_stop", &PrefetchCheckCpuTiming::best_effort_to_stop},
    {"stop_to_min", &PrefetchCheckCpuTiming::stop_to_min},
    {"min_to_visible", &PrefetchCheckCpuTiming::min_to_visible},
    {"no_operation_return", &PrefetchCheckCpuTiming::no_operation_return}
};
uint64_t duration(const Json & value) {
    if (!value.is_number_unsigned() && !(value.is_number_integer() && value.get<int64_t>() >= 0))
        throw std::invalid_argument("Wait calibration durations must be nonnegative integer microseconds");
    return value.get<uint64_t>();
}
Json collective_json(const CpuCollectiveTiming & timing) {
    if (!timing.issue.empty()) throw std::invalid_argument("Cannot export incomplete collective timing");
    Json rows = Json::array();
    for (const auto & [rank, call] : timing.calls) {
        Json row{{"rank", rank}, {"entry_ranks", call.entry_ranks}, {"before_submit", call.before_submit},
                 {"submission", call.submission}, {"dispatch", call.dispatch}, {"worker_remainder", call.worker_remainder},
                 {"after_join", call.after_join}};
        row["envelope_remainder_us"] = call.envelope_remainder_us ? Json(*call.envelope_remainder_us) : Json(nullptr);
        rows.push_back(std::move(row));
    }
    return rows;
}
CpuCollectiveTiming collective(const Json & rows) {
    CpuCollectiveTiming result;
    for (const auto & row : rows) {
        CpuCollectiveCallTiming call;
        call.entry_ranks = row.at("entry_ranks").get<std::vector<int>>();
        call.before_submit = duration(row.at("before_submit"));
        call.submission = duration(row.at("submission"));
        call.dispatch = duration(row.at("dispatch"));
        call.worker_remainder = duration(row.at("worker_remainder"));
        call.after_join = duration(row.at("after_join"));
        if (!row.at("envelope_remainder_us").is_null()) call.envelope_remainder_us = duration(row.at("envelope_remainder_us"));
        const int rank = row.at("rank").get<int>();
        if (rank < 0 || !result.calls.emplace(rank, std::move(call)).second) throw std::invalid_argument("Invalid collective calibration rank");
    }
    return result;
}
Json times_json(const PrefetchSchedulerLocalTimes & times) {
    Json result = Json::object();
    for (const auto & [rank, time] : times)
        result[std::to_string(rank)] = {{"cpu_us", time.cpu_us}, {"gap_us", time.gap_us}};
    return result;
}
PrefetchSchedulerLocalTimes times(const Json & values) {
    PrefetchSchedulerLocalTimes result;
    for (const auto & [key, value] : values.items()) {
        const int rank = std::stoi(key);
        if (rank < 0 || key != std::to_string(rank)) throw std::invalid_argument("Invalid wait calibration rank key");
        if (!value.is_object()) throw std::invalid_argument("Re-extract wait calibration from its trace: local timing needs cpu_us and gap_us");
        result.emplace(rank, PrefetchSchedulerLocalTiming{duration(value.at("cpu_us")), duration(value.at("gap_us"))});
    }
    return result;
}
const std::vector<std::string> actions{"receive", "write_completion", "load_completion", "storage_drain"};
} // namespace

Json encode_prefetch_wait_calibration(const PrefetchWaitCalibration & c) {
    Json cpu = Json::array(), steps = Json::array();
    for (const auto & [rank, timing] : c.timing.check.cpu) {
        Json row{{"rank", rank}, {"entry_to_check", timing.entry_to_check}};
        for (const auto & [name, field] : cpu_fields) row[name] = timing.*field ? Json(*(timing.*field)) : Json(nullptr);
        cpu.push_back(std::move(row));
    }
    for (const auto & step : c.timing.scheduler.steps)
        steps.push_back({{"action", actions.at(static_cast<size_t>(step.action))}, {"communication", collective_json(step.communication)},
                         {"before", times_json(step.before)}, {"after", times_json(step.after)}});
    return {{"role", "fixed_calibration"}, {"source_manifest", c.source_manifest}, {"request_id", c.request_id},
            {"source_policy", c.source_policy}, {"cpu", cpu}, {"state_max", collective_json(c.timing.check.state_max)},
            {"completion_min", collective_json(c.timing.check.completion_min)}, {"steps", steps}, {"tail", times_json(c.timing.scheduler.tail)}};
}

Json encode_prefetch_query_calibration(const PrefetchQueryCalibration & c) {
    Json cpu = Json::array();
    for (const auto & [rank, timing] : c.timing.cpu)
        cpu.push_back({{"rank", rank}, {"before_sample", timing.before_sample},
                       {"sample_to_min", timing.sample_to_min}, {"min_to_apply", timing.min_to_apply}});
    return {{"role", "fixed_calibration"}, {"operation", "prefetch_query"},
            {"source_manifest", c.source_manifest}, {"cpu", cpu}, {"agreement", collective_json(c.timing.agreement)}};
}

PrefetchQueryCalibration decode_prefetch_query_calibration(const Json & document) {
    PrefetchQueryCalibration result;
    result.source_manifest = document.at("source_manifest").get<std::string>();
    if (document.at("role") != "fixed_calibration" || document.at("operation") != "prefetch_query" || result.source_manifest.empty())
        throw std::invalid_argument("Query calibration needs an independent operation-specific source");
    for (const auto & row : document.at("cpu")) {
        const int rank = row.at("rank").get<int>();
        if (rank < 0 || !result.timing.cpu.emplace(rank, PrefetchQueueCpuTiming{
                duration(row.at("before_sample")), duration(row.at("sample_to_min")), duration(row.at("min_to_apply"))}).second)
            throw std::invalid_argument("Invalid query calibration CPU rank");
    }
    result.timing.agreement = collective(document.at("agreement"));
    const auto & calls = result.timing.agreement.calls;
    if (calls.empty() || calls.size() != result.timing.cpu.size())
        throw std::invalid_argument("Query calibration needs a complete rank group");
    for (const auto & [rank, call] : calls) {
        const std::set<int> members(call.entry_ranks.begin(), call.entry_ranks.end());
        if (!result.timing.cpu.contains(rank) || members.size() != calls.size()
            || call.entry_ranks.size() != members.size()
            || std::ranges::any_of(members, [&](int peer) { return !calls.contains(peer); }))
            throw std::invalid_argument("Query calibration has inconsistent collective membership");
    }
    return result;
}

PrefetchQueryCalibration read_prefetch_query_calibration(const std::string & path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Cannot open independent query calibration: " + path);
    Json document;
    file >> document;
    return decode_prefetch_query_calibration(document);
}

PrefetchWaitCalibration decode_prefetch_wait_calibration(const Json & document) {
    PrefetchWaitCalibration c;
    c.source_manifest = document.at("source_manifest").get<std::string>();
    c.request_id = document.at("request_id").get<std::string>();
    c.source_policy = document.at("source_policy").get<std::string>();
    if (document.at("role") != "fixed_calibration" || c.source_manifest.empty() || c.request_id.empty()
        || (c.source_policy != "timeout" && c.source_policy != "wait_complete" && c.source_policy != "best_effort"))
        throw std::invalid_argument("Prefetch calibration needs an explicit independent source and supported policy");
    for (const auto & row : document.at("cpu")) {
        PrefetchCheckCpuTiming timing;
        timing.entry_to_check = duration(row.at("entry_to_check"));
        for (const auto & [name, field] : cpu_fields)
            if (!row.at(name).is_null()) timing.*field = duration(row.at(name));
        const int rank = row.at("rank").get<int>();
        if (rank < 0 || !c.timing.check.cpu.emplace(rank, timing).second) throw std::invalid_argument("Invalid wait calibration CPU rank");
    }
    c.timing.check.state_max = collective(document.at("state_max"));
    c.timing.check.completion_min = collective(document.at("completion_min"));
    c.timing.scheduler.tail = times(document.at("tail"));
    for (const auto & row : document.at("steps")) {
        const auto name = row.at("action").get<std::string>();
        const auto found = std::find(actions.begin(), actions.end(), name);
        if (found == actions.end()) throw std::invalid_argument("Unknown calibrated scheduler action: " + name);
        c.timing.scheduler.steps.push_back({static_cast<PrefetchSchedulerAction>(found - actions.begin()),
            collective(row.at("communication")), times(row.at("before")), times(row.at("after"))});
    }
    if (c.timing.check.cpu.empty()) throw std::invalid_argument("Empty prefetch calibration");
    const auto ranks_match = [&](const auto & values) {
        return values.size() == c.timing.check.cpu.size()
            && std::ranges::all_of(values, [&](const auto & item) { return c.timing.check.cpu.contains(item.first); });
    };
    const auto valid_collective = [&](const CpuCollectiveTiming & value) {
        return ranks_match(value.calls) && std::ranges::all_of(value.calls, [&](const auto & item) {
            return !item.second.entry_ranks.empty() && std::ranges::all_of(item.second.entry_ranks,
                [&](int rank) { return c.timing.check.cpu.contains(rank); });
        });
    };
    if (c.source_policy == "best_effort") {
        if (!c.timing.check.state_max.calls.empty() || !c.timing.scheduler.steps.empty() || !c.timing.scheduler.tail.empty()
            || !valid_collective(c.timing.check.completion_min))
            throw std::invalid_argument("Immediate-stop calibration needs a MIN without MAX or retry work");
        for (const auto & [rank, cpu] : c.timing.check.cpu)
            if (!cpu.best_effort_to_stop || !cpu.stop_to_min || !cpu.min_to_visible
                || cpu.check_to_max || cpu.max_to_false_return || cpu.max_to_stop)
                throw std::invalid_argument("Immediate-stop calibration has missing or foreign branch costs");
        return c;
    }
    if (c.timing.scheduler.steps.empty()) throw std::invalid_argument("Empty wait calibration");
    if (!ranks_match(c.timing.scheduler.tail) || !valid_collective(c.timing.check.state_max) || !valid_collective(c.timing.check.completion_min))
        throw std::invalid_argument("Wait calibration has inconsistent rank membership");
    for (const auto & [rank, cpu] : c.timing.check.cpu)
        if (!cpu.check_to_max || !cpu.max_to_false_return || !cpu.max_to_stop || !cpu.stop_to_min || !cpu.min_to_visible)
            throw std::invalid_argument("Wait calibration lacks an active branch cost");
    for (const auto & step : c.timing.scheduler.steps)
        if (!ranks_match(step.before) || !ranks_match(step.after) || !valid_collective(step.communication))
            throw std::invalid_argument("Wait calibration step has inconsistent rank membership");
    return c;
}

PrefetchWaitCalibration read_prefetch_wait_calibration(const std::string & path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open wait calibration: " + path);
    Json document;
    input >> document;
    return decode_prefetch_wait_calibration(document);
}
} // namespace markov::trace_graph::modules::hicache::model
