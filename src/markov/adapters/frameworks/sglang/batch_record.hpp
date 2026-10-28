#pragma once

#include <nlohmann/json.hpp>

namespace markov::sglang {

// Export control choices only; templates and target execution facts are separate inputs.
class BatchRecord {
public:
    explicit BatchRecord(const nlohmann::json & requests);
    void append(const nlohmann::json & source, const nlohmann::json & session);
    const nlohmann::json & document() const;

private:
    nlohmann::json requests_;
    nlohmann::json result_;
};

} // namespace markov::sglang
