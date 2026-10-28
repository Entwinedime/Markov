#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/contracts/serving.hpp"

namespace markov {

struct RecordedMember {
    std::size_t request;
    std::int64_t context_tokens;
    std::int64_t new_tokens;
};

struct RecordedBatch {
    Phase phase;
    bool prebuilt;
    std::size_t lane;
    std::vector<RecordedMember> members;
};

// Records select work; the existing controller remains the sole owner of progress and KV state.
class BatchReplay {
public:
    BatchReplay(const nlohmann::json & document, std::span<const Request> requests, bool disaggregated);

    const RecordedBatch * next(const std::string & instance) const;
    void advance(const std::string & instance);
    bool done() const;
    std::size_t lanes(const std::string & instance) const;

private:
    struct Sequence {
        std::vector<RecordedBatch> batches;
        std::size_t position = 0;
    };
    std::map<std::string, Sequence> sequences_;
};

} // namespace markov
