#include "markov/adapters/frameworks/sglang/batch_record.hpp"

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace markov::sglang {

BatchRecord::BatchRecord(const nlohmann::json & requests)
    : requests_(requests),
      result_({
          {     "scope",    "native_batch_replay" },
          { "instances", nlohmann::json::object() }
}) {}

void BatchRecord::append(const nlohmann::json & source, const nlohmann::json & session) {
    const auto & environment = source.at("environment");
    if (environment.at("tp_rank") != 0 || environment.at("pp_rank") != 0)
        throw std::invalid_argument("batch records require the first TP/PP rank of each serving instance");
    const auto mode = environment.at("implementation").at("disaggregation_mode").get<std::string>();
    const auto instance = mode == "null" ? std::string("model") : mode;
    if (result_.at("instances").contains(instance)) throw std::invalid_argument("batch records repeat a serving instance");
    const auto & source_requests = session.at("source_requests");
    if (source_requests.size() != requests_.size()) throw std::invalid_argument("native session request count differs from workload");
    std::map<std::string, std::string> identities;
    for (std::size_t index = 0; index < requests_.size(); ++index)
        identities.emplace(source_requests[index].get<std::string>(), requests_[index].at("id").get<std::string>());

    // Only completed model/prebuilt batches belong to the recorded serving window; auxiliary prototypes do not.
    std::map<std::size_t, bool> completed;
    for (const auto & region : source.at("regions")) {
        const auto role = region.at("role").get<std::string>();
        if (role == "ResultProcessed" || role == "PrebuiltResult")
            completed.emplace(region.at("metadata").at("batch").get<std::size_t>(), role == "PrebuiltResult");
    }
    auto batches = nlohmann::json::array();
    for (const auto & batch : source.at("batches")) {
        const auto found = completed.find(batch.at("id").get<std::size_t>());
        if (batch.at("observation") != "source" || found == completed.end()) continue;
        auto members = nlohmann::json::array();
        for (const auto & member : batch.at("members")) {
            const auto & id = identities.at(member.at("id").get<std::string>());
            auto context = member.at("context_tokens").get<std::int64_t>();
            auto tokens = member.at("new_tokens").get<std::int64_t>();
            if (found->second) {

                // Prebuilt seq_lens contains the prompt; observation splits it into context and one input token.
                if (tokens != 1) throw std::invalid_argument("prebuilt batch has an unsupported token transition");
                tokens += context;
                context = 0;
            }
            members.push_back({
                {        "request",      id },
                { "context_tokens", context },
                {     "new_tokens",  tokens }
            });
        }
        if (environment.at("logical_pp") != 1 && !batch.contains("lane"))
            throw std::invalid_argument("native PP observation lacks batch lane identity; record it before replay");
        batches.push_back({
            { "phase", found->second ? nlohmann::json("accept") : batch.at("phase") },
            { "lane", batch.value("lane", std::size_t{ 0 }) },
            { "members", std::move(members) }
        });
    }
    result_["instances"][instance] = std::move(batches);
}

const nlohmann::json & BatchRecord::document() const { return result_; }

} // namespace markov::sglang
