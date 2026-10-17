#include "markov/adapters/runtimes/ascend/source_extractor.hpp"

#include <array>
#include <set>

namespace markov {
namespace {

std::optional<SourceSpanId> driver_copy_service(const SourceGraph & graph, const nlohmann::json & intervals, const nlohmann::json & api) {
    const auto & observed = api.at("copy_interval");
    std::optional<SourceSpanId> result;
    for (SourceSpanId index = 0; index < graph.spans().size(); ++index) {
        const auto & span = graph.spans()[index];
        if (span.kind != SourceSpanKind::CpuControl || !span.source_interval) continue;
        const auto & origin = intervals.at(*span.source_interval);
        if (origin.at("rank") != api.at("rank") || origin.at("tid") != api.at("tid")) continue;
        if (graph.boundaries()[span.begin].observed_time_ns != observed.at("begin_ns").get<std::int64_t>()
            || graph.boundaries()[span.end].observed_time_ns != observed.at("end_ns").get<std::int64_t>())
            continue;
        if (result) return std::nullopt;
        result = index;
    }
    return result;
}


} // namespace

std::size_t AscendSourceExtractor::read_transfers(const nlohmann::json & intervals, const nlohmann::json & records) {
    // Ordinary rtMemcpyKind_t directions. Extended/address modes require
    // their own physical-work contract, independent of any serving framework.
    constexpr std::array directions{ TransferDirection::HostToHost,
                                     TransferDirection::HostToDevice,
                                     TransferDirection::DeviceToHost,
                                     TransferDirection::DeviceToDevice };
    std::set<SourceSpanId> communication;
    for (const auto & collective : graph_.collectives())
        for (const auto & task : collective.tasks) communication.insert(task.span);

    std::size_t unresolved = 0;
    for (const auto & interval : intervals) {
        if (interval.at("kind") != "dma" || communication.contains(interval.at("id").get<SourceSpanId>())) continue;
        const auto api = interval.find("native_api");
        if (api == interval.end()) {
            ++unresolved;
            continue;
        }

        const auto index = api->get<std::size_t>();
        const auto & record = records.at(index);
        if (record.at("api") != "rtMemcpyAsync" || record.at("result") != 0 || record.at("transfer_bytes").is_null() || record.at("transfer_kind").is_null()) {
            ++unresolved;
            continue;
        }

        // An API total describes one node only when the hardware member is
        // unique. Communication already owns its workload and is excluded above.
        const auto & members = record.at("hardware_intervals");
        const auto kind = record.at("transfer_kind").get<std::int32_t>();
        if (members.size() != 1 || members.front() != interval.at("id") || kind < 0 || kind >= static_cast<std::int32_t>(directions.size())) {
            ++unresolved;
            continue;
        }
        graph_.observe_transfer(interval.at("id").get<SourceSpanId>(), { record.at("transfer_bytes").get<std::uint64_t>(), directions[kind], index });
    }

    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto & api = records[index];
        if (api.at("copy_execution") != "synchronous_driver" || api.at("owner_call").is_null()) continue;
        const auto kind = api.at("transfer_kind").get<std::int32_t>();
        const auto service = driver_copy_service(graph_, intervals, api);
        if (!service || kind < 0 || kind >= static_cast<std::int32_t>(directions.size())) {
            ++unresolved;
            continue;
        }

        const auto & span = graph_.spans()[*service];
        if (!api.at("scope_call").is_null() && span.scope_call != call_ids_.at(api.at("scope_call").get<std::string>())) {
            ++unresolved;
            continue;
        }

        // The isolated driver leaf owns the bytes. Neither a second overlapping
        // copy nor the enclosing API envelope may charge the same CPU segment.
        const auto begin = graph_.boundaries()[span.begin].observed_time_ns;
        const auto end = graph_.boundaries()[span.end].observed_time_ns;
        std::size_t copies = 0;
        for (const auto & other : records) {
            if (other.at("api") != "rtMemcpyAsync" || other.at("rank") != api.at("rank") || other.at("tid") != api.at("tid")
                || other.at("source_interval").is_null())
                continue;
            const auto & interval = intervals.at(other.at("source_interval").get<std::size_t>());
            if (interval.at("start_ns").get<std::int64_t>() < end && interval.at("end_ns").get<std::int64_t>() > begin) ++copies;
        }
        if (copies != 1) {
            ++unresolved;
            continue;
        }
        graph_.observe_transfer(*service, { api.at("transfer_bytes").get<std::uint64_t>(), directions[kind], index });
    }
    return unresolved;
}

} // namespace markov
