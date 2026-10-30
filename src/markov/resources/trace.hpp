#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "markov/contracts/serving.hpp"

namespace markov {

// Optional owner facts, stamped by the serving loop; never used to make resource decisions.
class ResourceTrace {
public:
    void context(std::int64_t time_ns, std::size_t order, std::string_view phase, std::optional<TaskIdentity> task = {});
    void append(std::string_view pool, std::string_view kind, nlohmann::json facts);
    nlohmann::json take();

private:
    std::int64_t time_ns_ = 0;
    std::size_t order_ = 0;
    std::string phase_ = "initial";
    std::optional<TaskIdentity> task_;
    nlohmann::json events_ = nlohmann::json::array();
};

} // namespace markov
