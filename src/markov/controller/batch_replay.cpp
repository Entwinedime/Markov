#include "markov/controller/batch_replay.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace markov {

BatchReplay::BatchReplay(const nlohmann::json & document, std::span<const Request> requests, bool disaggregated) {
    if (document.at("scope") != "native_batch_replay") throw std::invalid_argument("expected a native batch replay record");
    std::map<std::string, std::size_t> identities;
    for (std::size_t index = 0; index < requests.size(); ++index) identities.emplace(requests[index].id, index);

    const std::set<std::string> expected = disaggregated ? std::set<std::string>{ "prefill", "decode" } : std::set<std::string>{ "model" };
    std::set<std::string> instances;
    for (const auto & [instance, records] : document.at("instances").items()) {
        instances.insert(instance);
        if (!expected.contains(instance) || !records.is_array() || records.empty()) throw std::invalid_argument("replay has an invalid instance sequence");

        auto & sequence = sequences_[instance];
        std::vector<std::int64_t> generated(requests.size());
        std::vector<std::size_t> request_lanes(requests.size());
        for (const auto & record : records) {
            const auto phase = record.at("phase").get<std::string>();
            if (phase != "prefill" && phase != "decode" && phase != "accept") throw std::invalid_argument("unsupported recorded batch phase");
            const auto & lane = record.at("lane");
            if (!lane.is_number_integer() || lane.get<std::int64_t>() < 0) throw std::invalid_argument("recorded batch lane must be nonnegative");
            RecordedBatch batch{ phase == "decode" ? Phase::Decode : Phase::Prefill, phase == "accept", lane.get<std::size_t>(), {} };

            if (!record.at("members").is_array() || record.at("members").empty()) throw std::invalid_argument("recorded batches must have members");
            std::set<std::size_t> seen;
            for (const auto & member : record.at("members")) {
                const auto id = member.at("request").get<std::string>();
                const auto found = identities.find(id);
                if (found == identities.end()) throw std::invalid_argument("replay refers to an unknown request: " + id);
                const auto index = found->second;
                if (!seen.insert(index).second) throw std::invalid_argument("replay repeats a member in one batch: " + id);

                if (!member.at("context_tokens").is_number_integer() || !member.at("new_tokens").is_number_integer())
                    throw std::invalid_argument("recorded token counts must be integers");
                const auto context = member.at("context_tokens").get<std::int64_t>();
                const auto tokens = member.at("new_tokens").get<std::int64_t>();
                const auto & request = requests[index];

                // This entry preserves the current full-prefill, cold-cache contract; no chunk or cache inference.
                const bool first = generated[index] == 0;
                if (first) request_lanes[index] = batch.lane;
                else if (request_lanes[index] != batch.lane) throw std::invalid_argument("replay moves a request between batch lanes: " + id);
                if ((first && (phase != (instance == "decode" ? "accept" : "prefill") || context != 0 || tokens != request.prompt_tokens))
                    || (!first && (phase != "decode" || instance == "prefill" || tokens != 1 || context != request.prompt_tokens + generated[index] - 1))
                    || generated[index] >= request.output_tokens)
                    throw std::invalid_argument("recorded token progress disagrees with request: " + id);
                ++generated[index];
                batch.members.push_back({ index, context, tokens });
            }
            sequence.batches.push_back(std::move(batch));
        }

        for (std::size_t index = 0; index < requests.size(); ++index) {
            if (generated[index] != (instance == "prefill" ? 1 : requests[index].output_tokens))
                throw std::invalid_argument("replay does not complete request: " + requests[index].id);
        }
    }
    if (instances != expected) throw std::invalid_argument("replay instance set differs from the serving configuration");
}

const RecordedBatch * BatchReplay::next(const std::string & instance) const {
    const auto & sequence = sequences_.at(instance);
    return sequence.position == sequence.batches.size() ? nullptr : &sequence.batches[sequence.position];
}

void BatchReplay::advance(const std::string & instance) { ++sequences_.at(instance).position; }

bool BatchReplay::done() const {
    return std::ranges::all_of(sequences_, [](const auto & entry) { return entry.second.position == entry.second.batches.size(); });
}

std::size_t BatchReplay::lanes(const std::string & instance) const {
    return std::ranges::max_element(sequences_.at(instance).batches, {}, &RecordedBatch::lane)->lane + 1;
}

} // namespace markov
